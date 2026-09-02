// ============================================================
// 文件名称：FPathTracer.cpp
// 创建时间：2026-09-02
// 创建者  ：LimxTeam
// 设计哲学：几千 spp 不能一次交完。Windows 的 GPU 超时是两秒, 超过就整个
//          设备重置 —— 而重置之后的表现不是"报错", 是"这一帧的数据是上一
//          次的残留", 于是判据拿到一个看起来正常的数。所以样本数按"每次
//          派发的路径条数"分块, 块与块之间靠累加缓冲区接力。
//          接力要求随机数种子与"这是第几块"无关 —— 种子是
//          (像素, 全局样本号, 试验号) 的哈希, 分成一块还是十块, 走过的
//          路径逐条相同。这一点是可判定的: 同样的参数换一个分块大小,
//          结果必须逐位相同。
// 功能描述：离线参考路径追踪器的 GPU 侧实现 —— 上传三角形汤、建加速结构、
//          分块派发、把逐像素统计量回读到 CPU。
// 技术特性：累加缓冲区是 GpuOnly (逐样本读改写落在显存上), 清零靠从一块
//          全零的 CpuToGpu 缓冲区拷贝, 回读靠拷到 GpuToCpu 缓冲区 ——
//          三块分工明确, 没有一块同时承担两种访问模式。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                          │ 描述                      │
// │────────────────────────────────│─────────────────────────│
// │ Initialize()                   │ 建管线与描述符             │
// │ Shutdown()                     │ 释放全部 GPU 资源          │
// │ SetScene()                     │ 上传三角形汤并建加速结构   │
// │ PrepareAccumulation()          │ 按分辨率扩容并清零累加区   │
// │ Render()                       │ 分块派发 + 回读            │
// ============================================================

// 预编译头必须排在最前 —— MSVC 的 /Yu 会丢弃它之前的一切内容。
#include "Renderer/RendererMinimal.h"

#include "Renderer/RayTracing/FPathTracer.h"

#include "Core/Math/FMath.h"
#include "RHI/RHI/IRHICommandBuffer.h"
#include "RHI/RHI/IRHIDevice.h"
#include "RenderCore/Renderer/FRenderContext.h"
#include "RenderCore/Shaders/FShaderManager.h"

namespace Limx
{

LIMX_DEFINE_LOG_CATEGORY(LogPathTracer)

namespace
{

// ============================================================================
// 与着色器逐字段对应的推送常量 (112 字节)
// ============================================================================

struct FPathTracePushConstants
{
    // xyz = 相机世界位置, w = tan(垂直半视角)
    Float32 CameraPosition[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    // xyz = 前向, w = 宽高比
    Float32 CameraForward[4] = { 0.0f, 0.0f, -1.0f, 1.0f };

    Float32 CameraRight[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
    Float32 CameraUp[4]    = { 0.0f, 1.0f, 0.0f, 0.0f };

    // x = 宽, y = 高, z = 本次分派的样本数, w = 样本号起点
    UInt32 Image[4] = { 0u, 0u, 0u, 0u };

    // x = 最大弹射次数, y = 轮盘起始散射序号, z = 试验号, w = 保留
    UInt32 Sampling[4] = { 0u, 0u, 0u, 0u };

    // L_env(ω) = x + y·ω.y
    Float32 Environment[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    // x = tMin, y = 法线偏移, z = tMax, w = 保留
    Float32 Trace[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
};

static_assert(sizeof(FPathTracePushConstants) == 128,
              "FPathTracePushConstants 必须是 128 字节 — 与 path_trace.comp 的"
              " Params 逐字段一致。128 也正好是 Vulkan 保证的推送常量下限, "
              "再多一个 vec4 就有设备装不下");

/// 一次派发允许的最大路径条数
///
/// 3060 上一条 8 次弹射的路径约 0.2 微秒, 四百万条约 0.8 秒 —— 留在两秒
/// 的 GPU 超时之内, 又不至于把派发次数拉到几百次 (每次派发都要交一次
/// 命令缓冲区并等一次栅栏)。
constexpr UInt64 kMaxPathsPerDispatch = 4u * 1024u * 1024u;

constexpr UInt32 kGroupSize = 8;

/// 创建一块缓冲区, 可选地填入初值
bool CreateBuffer(IRHIDevice* device, UInt64 size, EBufferUsage usage,
                  EMemoryUsage memory, const char* name,
                  const void* initialData, FRHIBufferHandle& outHandle)
{
    FRHIBufferDesc desc;
    desc.Size        = size;
    desc.Usage       = usage;
    desc.MemoryUsage = memory;
    desc.DebugName   = name;

    if (!IsRHISuccess(device->CreateBuffer(desc, outHandle)))
    {
        LIMX_LOG(LogPathTracer, Error,
                 "[路径追踪] 缓冲区 {} 创建失败 ({} 字节)", name, size);
        return false;
    }

    if (initialData == nullptr)
    {
        return true;
    }

    void* mapped = nullptr;

    if (!IsRHISuccess(device->MapBuffer(outHandle, &mapped)) ||
        mapped == nullptr)
    {
        LIMX_LOG(LogPathTracer, Error, "[路径追踪] 缓冲区 {} 映射失败", name);
        return false;
    }

    Memory::MemCopy(mapped, initialData, static_cast<SizeType>(size));
    device->UnmapBuffer(outHandle);

    return true;
}

} // namespace

// ============================================================================
// Initialize
// ============================================================================

ERHIResult FPathTracer::Initialize(IRHIDevice* device, FRenderContext* context)
{
    if (device == nullptr || context == nullptr)
    {
        LIMX_LOG(LogPathTracer, Error, "[路径追踪] 设备或上下文为空");
        return ERHIResult::ErrorInvalidParameter;
    }

    // "不支持"必须判失败而不是静默降级。参考实现降级之后交出的数依然会被
    // 当成参考答案用, 而那比没有参考答案糟得多。
    if (!device->IsRayTracingSupported())
    {
        LIMX_LOG(LogPathTracer, Error,
                 "[路径追踪] 设备不支持光线追踪 — 参考路径追踪器无法工作");
        return ERHIResult::ErrorIncompatibleDriver;
    }

    m_Device  = device;
    m_Context = context;

    // ---- 描述符布局 ----
    FRHIDescriptorBinding bindings[6] = {};

    bindings[0].Binding    = 0;
    bindings[0].Type       = EDescriptorType::AccelerationStructure;
    bindings[0].Count      = 1;
    bindings[0].StageFlags = EShaderStage::Compute;

    for (UInt32 i = 1; i < 6; ++i)
    {
        bindings[i].Binding    = i;
        bindings[i].Type       = EDescriptorType::StorageBuffer;
        bindings[i].Count      = 1;
        bindings[i].StageFlags = EShaderStage::Compute;
    }

    FRHIDescSetLayoutDesc layoutDesc = {};
    layoutDesc.Bindings     = bindings;
    layoutDesc.BindingCount = 6;
    layoutDesc.DebugName    = "PathTracer.SetLayout";

    if (!IsRHISuccess(device->CreateDescSetLayout(layoutDesc, m_SetLayout)) ||
        !IsRHISuccess(device->AllocateDescriptorSet(m_SetLayout,
                                                    m_DescriptorSet)))
    {
        LIMX_LOG(LogPathTracer, Error, "[路径追踪] 描述符集创建失败");
        Shutdown();
        return ERHIResult::ErrorUnknown;
    }

    // ---- 着色器 ----
    FShaderManager& shaders = FShaderManager::Get();

    if (!shaders.IsInitialized())
    {
        shaders.Initialize();
    }

    if (!IsRHISuccess(shaders.CreateShaderModule(
            device, FString("Builtin/path_trace.comp"),
            EShaderStage::Compute, m_Shader)))
    {
        LIMX_LOG(LogPathTracer, Error,
                 "[路径追踪] path_trace.comp 加载失败");
        Shutdown();
        return ERHIResult::ErrorShaderCompilation;
    }

    // ---- 管线 ----
    FRHIPushConstantRange pushRange = {};
    pushRange.StageFlags = EShaderStage::Compute;
    pushRange.Offset     = 0;
    pushRange.Size       = sizeof(FPathTracePushConstants);

    if (device->GetMaxPushConstantSize() < sizeof(FPathTracePushConstants))
    {
        LIMX_LOG(LogPathTracer, Error,
                 "[路径追踪] 推送常量上限 {} 字节, 需要 {} 字节",
                 device->GetMaxPushConstantSize(),
                 static_cast<UInt32>(sizeof(FPathTracePushConstants)));
        Shutdown();
        return ERHIResult::ErrorIncompatibleDriver;
    }

    FRHIPipelineLayoutDesc pipelineLayoutDesc = {};
    pipelineLayoutDesc.SetLayouts             = &m_SetLayout;
    pipelineLayoutDesc.SetLayoutCount         = 1;
    pipelineLayoutDesc.PushConstantRanges     = &pushRange;
    pipelineLayoutDesc.PushConstantRangeCount = 1;
    pipelineLayoutDesc.DebugName              = "PathTracer.PipelineLayout";

    if (!IsRHISuccess(device->CreatePipelineLayout(pipelineLayoutDesc,
                                                   m_PipelineLayout)))
    {
        LIMX_LOG(LogPathTracer, Error, "[路径追踪] 管线布局创建失败");
        Shutdown();
        return ERHIResult::ErrorUnknown;
    }

    FRHIComputePipelineDesc pipelineDesc = {};
    pipelineDesc.ComputeShader.Shader     = m_Shader;
    pipelineDesc.ComputeShader.Stage      = EShaderStage::Compute;
    pipelineDesc.ComputeShader.EntryPoint = "main";
    pipelineDesc.PipelineLayout           = m_PipelineLayout;
    pipelineDesc.DebugName                = "PathTracer.Pipeline";

    if (!IsRHISuccess(device->CreateComputePipeline(pipelineDesc, m_Pipeline)))
    {
        LIMX_LOG(LogPathTracer, Error, "[路径追踪] 计算管线创建失败");
        Shutdown();
        return ERHIResult::ErrorUnknown;
    }

    return ERHIResult::Success;
}

// ============================================================================
// Shutdown
// ============================================================================

void FPathTracer::Shutdown()
{
    if (m_Device == nullptr)
    {
        return;
    }

    ReleaseScene();
    ReleaseAccumulation();

    m_Device->DestroyComputePipeline(m_Pipeline);
    m_Device->DestroyPipelineLayout(m_PipelineLayout);
    m_Device->FreeDescriptorSet(m_DescriptorSet);
    m_Device->DestroyDescSetLayout(m_SetLayout);
    m_Device->DestroyShader(m_Shader);

    m_Device  = nullptr;
    m_Context = nullptr;
}

void FPathTracer::ReleaseScene()
{
    if (m_Device == nullptr)
    {
        return;
    }

    // 先毁加速结构 —— 它们内部还引用着自己的存储缓冲区。
    m_Device->DestroyAccelStruct(m_Tlas);
    m_Device->DestroyAccelStruct(m_Blas);

    m_Device->DestroyBuffer(m_MaterialBuffer);
    m_Device->DestroyBuffer(m_TriangleMaterialBuffer);
    m_Device->DestroyBuffer(m_IndexBuffer);
    m_Device->DestroyBuffer(m_PositionBuffer);

    m_TriangleCount = 0;
    m_VertexCount   = 0;
    m_MaterialCount = 0;
}

void FPathTracer::ReleaseAccumulation()
{
    if (m_Device == nullptr)
    {
        return;
    }

    m_Device->DestroyBuffer(m_ReadbackBuffer);
    m_Device->DestroyBuffer(m_ZeroBuffer);
    m_Device->DestroyBuffer(m_AccumBuffer);

    m_AccumPixelCapacity = 0;
}

// ============================================================================
// SetScene
// ============================================================================

ERHIResult FPathTracer::SetScene(const FPathTraceScene& scene)
{
    if (m_Device == nullptr)
    {
        return ERHIResult::ErrorInvalidHandle;
    }

    if (scene.Positions == nullptr || scene.Indices == nullptr ||
        scene.TriangleMaterials == nullptr || scene.Materials == nullptr ||
        scene.VertexCount == 0 || scene.IndexCount == 0 ||
        (scene.IndexCount % 3u) != 0u || scene.MaterialCount == 0)
    {
        LIMX_LOG(LogPathTracer, Error,
                 "[路径追踪] 场景描述不合法 — 顶点 {} 索引 {} 材质 {}",
                 scene.VertexCount, scene.IndexCount, scene.MaterialCount);
        return ERHIResult::ErrorInvalidParameter;
    }

    ReleaseScene();

    m_TriangleCount = scene.IndexCount / 3u;
    m_VertexCount   = scene.VertexCount;
    m_MaterialCount = scene.MaterialCount;

    // 顶点与索引同时是加速结构的输入与着色器的存储缓冲区。
    //
    // 让着色器从**建 BLAS 用的同一块内存**里取三角形, 而不是另传一份
    // 预算好的法线: 另传一份的话, 两份数据不一致时判据一无所知 ——
    // 而 BLAS 里装的到底是不是这些三角形, 正是要验的东西之一。
    const EBufferUsage geometryUsage =
        EBufferUsage::AccelStructBuild | EBufferUsage::ShaderDeviceAddress |
        EBufferUsage::StorageBuffer | EBufferUsage::TransferDst;

    const UInt64 positionBytes =
        static_cast<UInt64>(scene.VertexCount) * 3u * sizeof(Float32);
    const UInt64 indexBytes =
        static_cast<UInt64>(scene.IndexCount) * sizeof(UInt32);
    const UInt64 triangleMaterialBytes =
        static_cast<UInt64>(m_TriangleCount) * sizeof(UInt32);
    const UInt64 materialBytes =
        static_cast<UInt64>(scene.MaterialCount) * sizeof(FPathTraceMaterial);

    if (!CreateBuffer(m_Device, positionBytes,
                      geometryUsage | EBufferUsage::VertexBuffer,
                      EMemoryUsage::CpuToGpu, "PathTracer.Positions",
                      scene.Positions, m_PositionBuffer) ||
        !CreateBuffer(m_Device, indexBytes,
                      geometryUsage | EBufferUsage::IndexBuffer,
                      EMemoryUsage::CpuToGpu, "PathTracer.Indices",
                      scene.Indices, m_IndexBuffer) ||
        !CreateBuffer(m_Device, triangleMaterialBytes,
                      EBufferUsage::StorageBuffer, EMemoryUsage::CpuToGpu,
                      "PathTracer.TriangleMaterials",
                      scene.TriangleMaterials, m_TriangleMaterialBuffer) ||
        !CreateBuffer(m_Device, materialBytes, EBufferUsage::StorageBuffer,
                      EMemoryUsage::CpuToGpu, "PathTracer.Materials",
                      scene.Materials, m_MaterialBuffer))
    {
        ReleaseScene();
        return ERHIResult::ErrorOutOfDeviceMemory;
    }

    // ---- BLAS: 整个场景一棵, 图元下标直接就是全局三角形序号 ----
    FRHIAccelStructGeometry geometry;
    geometry.VertexBuffer = m_PositionBuffer;
    geometry.VertexOffset = 0;
    geometry.VertexCount  = scene.VertexCount;
    geometry.VertexStride = sizeof(Float32) * 3;
    geometry.VertexFormat = EPixelFormat::RGB32_SFLOAT;
    geometry.IndexBuffer  = m_IndexBuffer;
    geometry.IndexOffset  = 0;
    geometry.IndexCount   = scene.IndexCount;
    geometry.IndexType    = EIndexType::UInt32;
    geometry.Opaque       = true;

    FRHIBlasDesc blasDesc;
    blasDesc.Geometries    = &geometry;
    blasDesc.GeometryCount = 1;
    blasDesc.DebugName     = "PathTracer.Blas";

    if (!IsRHISuccess(m_Device->CreateBottomLevelAS(blasDesc, m_Blas)))
    {
        LIMX_LOG(LogPathTracer, Error, "[路径追踪] BLAS 创建失败");
        ReleaseScene();
        return ERHIResult::ErrorUnknown;
    }

    FRHITlasDesc tlasDesc;
    tlasDesc.MaxInstanceCount = 1;
    tlasDesc.DebugName        = "PathTracer.Tlas";

    if (!IsRHISuccess(m_Device->CreateTopLevelAS(tlasDesc, m_Tlas)))
    {
        LIMX_LOG(LogPathTracer, Error, "[路径追踪] TLAS 创建失败");
        ReleaseScene();
        return ERHIResult::ErrorUnknown;
    }

    FRHIAccelStructInstance instance;
    instance.CustomIndex = 0;
    instance.Mask        = 0xFF;
    instance.Blas        = m_Blas;

    if (!IsRHISuccess(m_Device->UpdateTlasInstances(m_Tlas, &instance, 1)))
    {
        LIMX_LOG(LogPathTracer, Error, "[路径追踪] TLAS 实例上传失败");
        ReleaseScene();
        return ERHIResult::ErrorUnknown;
    }

    IRHICommandBuffer* cmd = m_Context->BeginSingleTimeCommands();

    if (cmd == nullptr)
    {
        LIMX_LOG(LogPathTracer, Error, "[路径追踪] 命令缓冲区分配失败");
        ReleaseScene();
        return ERHIResult::ErrorUnknown;
    }

    cmd->BuildAccelStruct(m_Blas, 0);

    // BLAS 必须先于 TLAS 建完 —— TLAS 的构建要读 BLAS 的内容。
    cmd->AccelStructBarrier();

    cmd->BuildAccelStruct(m_Tlas, 1);
    cmd->AccelStructBarrier();

    m_Context->EndSingleTimeCommands(cmd);

    return ERHIResult::Success;
}

// ============================================================================
// PrepareAccumulation
// ============================================================================

ERHIResult FPathTracer::PrepareAccumulation(UInt32 pixelCount)
{
    const UInt64 bytes =
        static_cast<UInt64>(pixelCount) * sizeof(FPathTracePixel);

    if (pixelCount > m_AccumPixelCapacity)
    {
        ReleaseAccumulation();

        // 累加区放显存: 每个样本都要读改写一次, 走主机内存的话这条路
        // 会成为瓶颈, 而"慢"在参考渲染上意味着判据跑不完。
        if (!CreateBuffer(m_Device, bytes,
                          EBufferUsage::StorageBuffer |
                              EBufferUsage::TransferSrc |
                              EBufferUsage::TransferDst,
                          EMemoryUsage::GpuOnly, "PathTracer.Accum",
                          nullptr, m_AccumBuffer) ||
            !CreateBuffer(m_Device, bytes, EBufferUsage::TransferSrc,
                          EMemoryUsage::CpuToGpu, "PathTracer.Zero",
                          nullptr, m_ZeroBuffer) ||
            !CreateBuffer(m_Device, bytes, EBufferUsage::TransferDst,
                          EMemoryUsage::GpuToCpu, "PathTracer.Readback",
                          nullptr, m_ReadbackBuffer))
        {
            ReleaseAccumulation();
            return ERHIResult::ErrorOutOfDeviceMemory;
        }

        void* mapped = nullptr;

        if (!IsRHISuccess(m_Device->MapBuffer(m_ZeroBuffer, &mapped)) ||
            mapped == nullptr)
        {
            ReleaseAccumulation();
            return ERHIResult::ErrorUnknown;
        }

        Memory::MemSet(mapped, 0, static_cast<SizeType>(bytes));
        m_Device->UnmapBuffer(m_ZeroBuffer);

        m_AccumPixelCapacity = pixelCount;
    }

    return ERHIResult::Success;
}

// ============================================================================
// Render
// ============================================================================

ERHIResult FPathTracer::Render(const FPathTraceCamera& camera,
                               const FPathTraceSettings& settings,
                               TArray<FPathTracePixel>& outPixels)
{
    m_LastDispatchCount = 0;

    if (m_Device == nullptr || !m_Tlas.IsValid())
    {
        LIMX_LOG(LogPathTracer, Error, "[路径追踪] 尚未设置场景");
        return ERHIResult::ErrorInvalidHandle;
    }

    if (settings.Width == 0 || settings.Height == 0 ||
        settings.SamplesPerPixel == 0)
    {
        LIMX_LOG(LogPathTracer, Error,
                 "[路径追踪] 分辨率或 spp 为零 — {}x{} spp {}",
                 settings.Width, settings.Height, settings.SamplesPerPixel);
        return ERHIResult::ErrorInvalidParameter;
    }

    const UInt32 pixelCount = settings.Width * settings.Height;

    const ERHIResult prepared = PrepareAccumulation(pixelCount);

    if (prepared != ERHIResult::Success)
    {
        return prepared;
    }

    const UInt64 accumBytes =
        static_cast<UInt64>(pixelCount) * sizeof(FPathTracePixel);

    // ---- 描述符 ----
    FRHIDescriptorWrite writes[6];

    writes[0] = FRHIDescriptorWrite();
    writes[0].DescriptorSet = m_DescriptorSet;
    writes[0].Binding       = 0;
    writes[0].Type          = EDescriptorType::AccelerationStructure;
    writes[0].AccelStruct   = m_Tlas;

    writes[1] = FRHIDescriptorWrite::StorageBuffer(
        m_DescriptorSet, 1, m_PositionBuffer, 0,
        static_cast<UInt64>(m_VertexCount) * 3u * sizeof(Float32));

    writes[2] = FRHIDescriptorWrite::StorageBuffer(
        m_DescriptorSet, 2, m_IndexBuffer, 0,
        static_cast<UInt64>(m_TriangleCount) * 3u * sizeof(UInt32));

    writes[3] = FRHIDescriptorWrite::StorageBuffer(
        m_DescriptorSet, 3, m_TriangleMaterialBuffer, 0,
        static_cast<UInt64>(m_TriangleCount) * sizeof(UInt32));

    writes[4] = FRHIDescriptorWrite::StorageBuffer(
        m_DescriptorSet, 4, m_MaterialBuffer, 0,
        static_cast<UInt64>(m_MaterialCount) * sizeof(FPathTraceMaterial));

    writes[5] = FRHIDescriptorWrite::StorageBuffer(
        m_DescriptorSet, 5, m_AccumBuffer, 0, accumBytes);

    m_Device->UpdateDescriptorSets(writes, 6);

    // ---- 推送常量的固定部分 ----
    FPathTracePushConstants push;

    const Float32 tanHalfY = FMath::Tan(camera.FovY * 0.5f);

    const Float32 aspect = static_cast<Float32>(settings.Width) /
                           static_cast<Float32>(settings.Height);

    push.CameraPosition[0] = camera.Position.X;
    push.CameraPosition[1] = camera.Position.Y;
    push.CameraPosition[2] = camera.Position.Z;
    push.CameraPosition[3] = tanHalfY;

    push.CameraForward[0] = camera.Forward.X;
    push.CameraForward[1] = camera.Forward.Y;
    push.CameraForward[2] = camera.Forward.Z;
    push.CameraForward[3] = aspect;

    push.CameraRight[0] = camera.Right.X;
    push.CameraRight[1] = camera.Right.Y;
    push.CameraRight[2] = camera.Right.Z;
    push.CameraRight[3] = 0.0f;

    push.CameraUp[0] = camera.Up.X;
    push.CameraUp[1] = camera.Up.Y;
    push.CameraUp[2] = camera.Up.Z;
    push.CameraUp[3] = 0.0f;

    push.Image[0] = settings.Width;
    push.Image[1] = settings.Height;

    push.Sampling[0] = settings.MaxBounce;
    push.Sampling[1] = settings.RussianRouletteStartDepth;
    push.Sampling[2] = settings.TrialIndex;
    push.Sampling[3] = 0u;

    push.Environment[0] = settings.EnvironmentRadiance;
    push.Environment[1] = settings.EnvironmentGradientY;
    push.Environment[2] = 0.0f;
    push.Environment[3] = 0.0f;

    push.Trace[0] = settings.RayTMin;
    push.Trace[1] = settings.NormalOffset;
    push.Trace[2] = settings.MaxRayDistance;
    push.Trace[3] = 0.0f;

    // ---- 分块 ----
    UInt32 samplesPerDispatch = static_cast<UInt32>(
        FMath::Max<UInt64>(1u, kMaxPathsPerDispatch /
                                   FMath::Max<UInt64>(1u, pixelCount)));

    samplesPerDispatch =
        FMath::Min(samplesPerDispatch, settings.SamplesPerPixel);

    const UInt32 groupX = (settings.Width + kGroupSize - 1) / kGroupSize;
    const UInt32 groupY = (settings.Height + kGroupSize - 1) / kGroupSize;

    // ---- 清零 ----
    {
        IRHICommandBuffer* cmd = m_Context->BeginSingleTimeCommands();

        if (cmd == nullptr)
        {
            return ERHIResult::ErrorUnknown;
        }

        FRHIBufferCopyRegion clearRegion = {};
        clearRegion.SrcOffset = 0;
        clearRegion.DstOffset = 0;
        clearRegion.Size      = accumBytes;

        cmd->CopyBuffer(m_ZeroBuffer, m_AccumBuffer, clearRegion);

        FRHIMemoryBarrier clearBarrier = {};
        clearBarrier.SrcAccessMask = EAccessFlags::TransferWrite;
        clearBarrier.DstAccessMask =
            EAccessFlags::ShaderRead | EAccessFlags::ShaderWrite;

        cmd->PipelineBarrier(EPipelineStageFlags::Transfer,
                             EPipelineStageFlags::ComputeShader,
                             &clearBarrier, 1, nullptr, 0, nullptr, 0);

        m_Context->EndSingleTimeCommands(cmd);
    }

    // ---- 逐块派发 ----
    for (UInt32 offset = 0; offset < settings.SamplesPerPixel;
         offset += samplesPerDispatch)
    {
        const UInt32 count =
            FMath::Min(samplesPerDispatch, settings.SamplesPerPixel - offset);

        IRHICommandBuffer* cmd = m_Context->BeginSingleTimeCommands();

        if (cmd == nullptr)
        {
            LIMX_LOG(LogPathTracer, Error,
                     "[路径追踪] 第 {} 块的命令缓冲区分配失败", offset);
            return ERHIResult::ErrorUnknown;
        }

        push.Image[2] = count;
        push.Image[3] = offset;

        cmd->BindComputePipeline(m_Pipeline);
        cmd->BindDescriptorSet(EPipelineBindPoint::Compute, m_PipelineLayout,
                               0, m_DescriptorSet);
        cmd->PushConstants(m_PipelineLayout, EShaderStage::Compute, 0,
                           sizeof(push), &push);

        cmd->Dispatch(groupX, groupY, 1);

        // 下一块要读这一块写下的累加值 —— 少了这道屏障, 相邻两块可能
        // 同时读到同一个旧值, 于是丢掉一整块的样本。丢掉的表现是"结果
        // 偏低一点", 而那与能量不守恒长得完全一样。
        FRHIMemoryBarrier chainBarrier = {};
        chainBarrier.SrcAccessMask = EAccessFlags::ShaderWrite;
        chainBarrier.DstAccessMask =
            EAccessFlags::ShaderRead | EAccessFlags::ShaderWrite;

        cmd->PipelineBarrier(EPipelineStageFlags::ComputeShader,
                             EPipelineStageFlags::ComputeShader,
                             &chainBarrier, 1, nullptr, 0, nullptr, 0);

        m_Context->EndSingleTimeCommands(cmd);

        ++m_LastDispatchCount;
    }

    // ---- 回读 ----
    {
        IRHICommandBuffer* cmd = m_Context->BeginSingleTimeCommands();

        if (cmd == nullptr)
        {
            return ERHIResult::ErrorUnknown;
        }

        FRHIMemoryBarrier readBarrier = {};
        readBarrier.SrcAccessMask = EAccessFlags::ShaderWrite;
        readBarrier.DstAccessMask = EAccessFlags::TransferRead;

        cmd->PipelineBarrier(EPipelineStageFlags::ComputeShader,
                             EPipelineStageFlags::Transfer,
                             &readBarrier, 1, nullptr, 0, nullptr, 0);

        FRHIBufferCopyRegion readRegion = {};
        readRegion.SrcOffset = 0;
        readRegion.DstOffset = 0;
        readRegion.Size      = accumBytes;

        cmd->CopyBuffer(m_AccumBuffer, m_ReadbackBuffer, readRegion);

        FRHIMemoryBarrier hostBarrier = {};
        hostBarrier.SrcAccessMask = EAccessFlags::TransferWrite;
        hostBarrier.DstAccessMask = EAccessFlags::HostRead;

        cmd->PipelineBarrier(EPipelineStageFlags::Transfer,
                             EPipelineStageFlags::Host,
                             &hostBarrier, 1, nullptr, 0, nullptr, 0);

        m_Context->EndSingleTimeCommands(cmd);
    }

    void* mapped = nullptr;

    if (!IsRHISuccess(m_Device->MapBuffer(m_ReadbackBuffer, &mapped)) ||
        mapped == nullptr)
    {
        LIMX_LOG(LogPathTracer, Error, "[路径追踪] 回读缓冲区映射失败");
        return ERHIResult::ErrorUnknown;
    }

    outPixels.SetSize(static_cast<SizeType>(pixelCount));

    Memory::MemCopy(outPixels.GetData(), mapped,
                    static_cast<SizeType>(accumBytes));

    m_Device->UnmapBuffer(m_ReadbackBuffer);

    return ERHIResult::Success;
}

} // namespace Limx
