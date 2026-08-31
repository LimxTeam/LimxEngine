/*******************************************************************************
 * 文件: FGpuCullPass.cpp
 * 创建时间: 2026-08-31
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   GPU 驱动剔除通道实现 — 上传逐物体数据、分组、计算着色器剔除
 *
 * 设计哲学:
 *   上传与分组**无论开关都做**。图形通道的顶点着色器只有一条路径 (从 set 3
 *   的 storage buffer 取模型矩阵与材质下标), 逐物体绘制那条路径同样把物体
 *   下标写进 firstInstance —— 于是两条路径读的是同一处数据、走的是同一份
 *   着色器代码。开关只决定"命令是 CPU 逐个下的还是 GPU 写好的"。
 *
 *   这样安排是为了让 --gpu-driven-check 的逐像素比对有意义: 比出来的差异
 *   只可能来自剔除与命令下发, 不会混进"两份着色器本来就不一样"这个因素。
 *   分簇光照那一天就是这么设计的, 理由完全相同。
 *
 * 依赖关系:
 *   内部: Renderer/RenderPass/FGpuCullPass.h, RenderCore/Shaders/FShaderManager.h
 *
 ******************************************************************************/

#include "Renderer/RenderPass/FGpuCullPass.h"
#include "Renderer/Renderer/FRenderer.h"

#include "RenderCore/Shaders/FShaderManager.h"

namespace Limx
{

LIMX_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{

/// 计算着色器的 push constant —— 六个视锥平面 + 物体数
///
/// 6 × 16 + 16 = 112 字节, 在 Vulkan 保证的 128 字节之内。再加就要挪进 UBO,
/// 而那多一次绑定与一次每帧写入。
struct FCullPushConstants
{
    Float32 Planes[6][4] = {};

    UInt32 ObjectCount   = 0;
    UInt32 ViewIndex     = 0;
    UInt32 ViewStride    = 0;   // 每个视图占多少条命令
    UInt32 CullBase      = 0;   // 投射体段在逐物体缓冲区里的起点
};

static_assert(sizeof(FCullPushConstants) == 112,
    "FCullPushConstants 必须为 112 字节 — 与 draw_cull.comp 的 push constant "
    "一致");

/// 可见计数器 —— 每个视图一格
struct FVisibleCounter
{
    UInt32 VisibleCount[kMaxCullViews] = {};
};

constexpr UInt32 kCullWorkgroupSize = 64;

} // namespace

// ============================================================================
// Setup
// ============================================================================

ERHIResult FGpuCullPass::Setup(const FPassSetupDesc& desc)
{
    LIMX_CHECK(desc.Device != nullptr);
    LIMX_CHECK(desc.MaxFramesInFlight > 0);

    m_Device     = desc.Device;
    m_FrameCount = desc.MaxFramesInFlight;

    // 设备不支持非零 firstInstance 时整条路径不可用。
    //
    // 这一条必须在建资源之前判: 不支持却照样建, 那些缓冲区会一直占着显存,
    // 而路径永远走不到。
    m_IsSupported = desc.Device->IsDrawIndirectFirstInstanceSupported();

    ERHIResult result = CreateBuffers(desc.Device, desc.MaxFramesInFlight);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[GpuCull] 缓冲区创建失败");
        return result;
    }

    result = CreateDescriptors(desc.Device, desc.MaxFramesInFlight,
                               desc.DrawObjectSetLayout);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[GpuCull] 描述符集创建失败");
        return result;
    }

    result = CreatePipeline(desc.Device);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[GpuCull] 计算管线创建失败");
        return result;
    }

    m_Groups.Reserve(64);

    LIMX_LOG(LogRenderer, Log,
             "[GpuCull] 初始化完成 — 每帧最多 {} 个物体, "
             "drawIndirectFirstInstance {}",
             kMaxGpuDrawObjects,
             m_IsSupported ? "可用" : "**不可用, GPU 驱动路径已禁用**");

    return ERHIResult::Success;
}

// ============================================================================
// CreateBuffers
// ============================================================================

ERHIResult FGpuCullPass::CreateBuffers(IRHIDevice* device, UInt32 frameCount)
{
    m_ObjectBuffers.Reserve(frameCount);
    m_IndirectBuffers.Reserve(frameCount);
    m_Counters.Reserve(frameCount);
    m_CounterReadbacks.Reserve(frameCount);

    for (UInt32 i = 0; i < frameCount; ++i)
    {
        // 逐物体数据 —— CPU 每帧写, 顶点着色器与计算着色器都读
        {
            FRHIBufferDesc bufferDesc = {};
            bufferDesc.Size = static_cast<UInt64>(sizeof(FGpuDrawObject)) *
                              kMaxGpuDrawObjects;
            bufferDesc.Usage       = EBufferUsage::StorageBuffer;
            bufferDesc.MemoryUsage = EMemoryUsage::CpuToGpu;
            bufferDesc.DebugName   = "GpuDrawObjects";

            FRHIBufferHandle buffer;
            const ERHIResult result = device->CreateBuffer(bufferDesc, buffer);
            if (!IsRHISuccess(result))
            {
                return result;
            }

            m_ObjectBuffers.Add(buffer);
        }

        // 间接命令 —— 计算着色器写, 命令处理器读
        //
        // 用途里 IndirectBuffer 与 StorageBuffer 缺一不可: 前者让它能当
        // vkCmdDrawIndexedIndirect 的源, 后者让计算着色器能写它。少写一个
        // 的表现是创建成功、绘制时验证层报"缓冲区用途不含 INDIRECT_BUFFER",
        // 而关掉验证层就是未定义行为。
        {
            FRHIBufferDesc bufferDesc = {};
            // 按**视图数 × 物体数**开。每个视图占固定长度的一段, 段长是
            // 常量而不是本帧的物体数 —— 用物体数的话每段的起点逐帧变化。
            bufferDesc.Size =
                static_cast<UInt64>(sizeof(FDrawIndexedIndirectCommand)) *
                kMaxGpuDrawObjects * kMaxCullViews;
            bufferDesc.Usage = static_cast<EBufferUsage>(
                static_cast<UInt32>(EBufferUsage::StorageBuffer) |
                static_cast<UInt32>(EBufferUsage::IndirectBuffer));
            bufferDesc.MemoryUsage = EMemoryUsage::GpuOnly;
            bufferDesc.DebugName   = "GpuDrawCommands";

            FRHIBufferHandle buffer;
            const ERHIResult result = device->CreateBuffer(bufferDesc, buffer);
            if (!IsRHISuccess(result))
            {
                return result;
            }

            m_IndirectBuffers.Add(buffer);
        }

        // 可见计数器 + 回读
        {
            FRHIBufferDesc bufferDesc = {};
            bufferDesc.Size  = sizeof(FVisibleCounter);
            bufferDesc.Usage = static_cast<EBufferUsage>(
                static_cast<UInt32>(EBufferUsage::StorageBuffer) |
                static_cast<UInt32>(EBufferUsage::TransferDst) |
                static_cast<UInt32>(EBufferUsage::TransferSrc));
            bufferDesc.MemoryUsage = EMemoryUsage::GpuOnly;
            bufferDesc.DebugName   = "GpuCullCounter";

            FRHIBufferHandle buffer;
            ERHIResult result = device->CreateBuffer(bufferDesc, buffer);
            if (!IsRHISuccess(result))
            {
                return result;
            }

            m_Counters.Add(buffer);

            bufferDesc.Usage       = EBufferUsage::TransferDst;
            bufferDesc.MemoryUsage = EMemoryUsage::GpuToCpu;
            bufferDesc.DebugName   = "GpuCullCounterReadback";

            FRHIBufferHandle readback;
            result = device->CreateBuffer(bufferDesc, readback);
            if (!IsRHISuccess(result))
            {
                return result;
            }

            m_CounterReadbacks.Add(readback);
        }
    }

    // 全零源 —— 计数器每帧要归零, 而 RHI 没有 FillBuffer
    {
        FRHIBufferDesc bufferDesc = {};
        bufferDesc.Size        = sizeof(FVisibleCounter);
        bufferDesc.Usage       = EBufferUsage::TransferSrc;
        bufferDesc.MemoryUsage = EMemoryUsage::CpuToGpu;
        bufferDesc.DebugName   = "GpuCullZeroSource";

        const ERHIResult result = device->CreateBuffer(bufferDesc, m_ZeroSource);
        if (!IsRHISuccess(result))
        {
            return result;
        }

        void* mapped = nullptr;
        if (IsRHISuccess(device->MapBuffer(m_ZeroSource, &mapped)) &&
            mapped != nullptr)
        {
            Memory::MemZero(mapped, sizeof(FVisibleCounter));
            device->UnmapBuffer(m_ZeroSource);
        }
    }

    return ERHIResult::Success;
}

// ============================================================================
// CreateDescriptors
// ============================================================================

ERHIResult FGpuCullPass::CreateDescriptors(
    IRHIDevice* device, UInt32 frameCount,
    FRHIDescSetLayoutHandle drawObjectLayout)
{
    // ---- 计算通道自己的 set 0 ----
    FRHIDescriptorBinding bindings[3] = {};

    for (UInt32 i = 0; i < 3; ++i)
    {
        bindings[i].Binding    = i;
        bindings[i].Type       = EDescriptorType::StorageBuffer;
        bindings[i].Count      = 1;
        bindings[i].StageFlags = EShaderStage::Compute;
    }

    FRHIDescSetLayoutDesc layoutDesc = {};
    layoutDesc.Bindings     = bindings;
    layoutDesc.BindingCount = 3;
    layoutDesc.DebugName    = "GpuCullSetLayout";

    ERHIResult result = device->CreateDescSetLayout(layoutDesc, m_CullSetLayout);
    if (!IsRHISuccess(result))
    {
        return result;
    }

    m_CullSets.Reserve(frameCount);
    m_DrawObjectSets.Reserve(frameCount);

    for (UInt32 i = 0; i < frameCount; ++i)
    {
        FRHIDescriptorSetHandle cullSet;
        result = device->AllocateDescriptorSet(m_CullSetLayout, cullSet);
        if (!IsRHISuccess(result))
        {
            return result;
        }

        FRHIDescriptorWrite writes[3];

        writes[0] = FRHIDescriptorWrite::StorageBuffer(
            cullSet, 0, m_ObjectBuffers[i], 0,
            static_cast<UInt64>(sizeof(FGpuDrawObject)) * kMaxGpuDrawObjects);

        writes[1] = FRHIDescriptorWrite::StorageBuffer(
            cullSet, 1, m_IndirectBuffers[i], 0,
            static_cast<UInt64>(sizeof(FDrawIndexedIndirectCommand)) *
                kMaxGpuDrawObjects * kMaxCullViews);

        writes[2] = FRHIDescriptorWrite::StorageBuffer(
            cullSet, 2, m_Counters[i], 0, sizeof(FVisibleCounter));

        device->UpdateDescriptorSets(writes, 3);

        m_CullSets.Add(cullSet);

        // ---- 图形通道的 set 3 ----
        FRHIDescriptorSetHandle drawSet;
        result = device->AllocateDescriptorSet(drawObjectLayout, drawSet);
        if (!IsRHISuccess(result))
        {
            return result;
        }

        FRHIDescriptorWrite drawWrite = FRHIDescriptorWrite::StorageBuffer(
            drawSet, 0, m_ObjectBuffers[i], 0,
            static_cast<UInt64>(sizeof(FGpuDrawObject)) * kMaxGpuDrawObjects);

        device->UpdateDescriptorSets(&drawWrite, 1);

        m_DrawObjectSets.Add(drawSet);
    }

    return ERHIResult::Success;
}

// ============================================================================
// CreatePipeline
// ============================================================================

ERHIResult FGpuCullPass::CreatePipeline(IRHIDevice* device)
{
    FShaderManager& shaders = FShaderManager::Get();

    if (!shaders.IsInitialized())
    {
        shaders.Initialize();
    }

    ERHIResult result = shaders.CreateShaderModule(
        device, FString("Builtin/draw_cull.comp"), EShaderStage::Compute,
        m_CullShader);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    FRHIPushConstantRange pushRange = {};
    pushRange.StageFlags = EShaderStage::Compute;
    pushRange.Offset     = 0;
    pushRange.Size       = sizeof(FCullPushConstants);

    FRHIPipelineLayoutDesc layoutDesc = {};
    layoutDesc.SetLayouts             = &m_CullSetLayout;
    layoutDesc.SetLayoutCount         = 1;
    layoutDesc.PushConstantRanges     = &pushRange;
    layoutDesc.PushConstantRangeCount = 1;
    layoutDesc.DebugName              = "GpuCullPipelineLayout";

    result = device->CreatePipelineLayout(layoutDesc, m_CullPipelineLayout);
    if (!IsRHISuccess(result))
    {
        return result;
    }

    FRHIComputePipelineDesc pipelineDesc = {};
    pipelineDesc.ComputeShader.Shader     = m_CullShader;
    pipelineDesc.ComputeShader.Stage      = EShaderStage::Compute;
    pipelineDesc.ComputeShader.EntryPoint = "main";
    pipelineDesc.PipelineLayout           = m_CullPipelineLayout;
    pipelineDesc.DebugName                = "GpuCullPipeline";

    return device->CreateComputePipeline(pipelineDesc, m_CullPipeline);
}

// ============================================================================
// UploadObjects — 写逐物体数据, 同时算出分组
// ============================================================================

UInt32 FGpuCullPass::WriteSegment(FGpuDrawObject*              destination,
                                  const TArray<FRenderObject>* source,
                                  UInt32                       writeCursor,
                                  bool                         buildGroups)
{
    if (source == nullptr)
    {
        return 0;
    }

    UInt32 written = 0;

    for (SizeType i = 0; i < source->GetSize(); ++i)
    {
        if (writeCursor + written >= kMaxGpuDrawObjects)
        {
            break;
        }

        const FRenderObject& obj = (*source)[i];

        FGpuDrawObject& gpu = destination[writeCursor + written];

        gpu.Model = obj.Transform.ToMatrix();

        const FVector4 sphere = BoundingSphereFromBox(obj.WorldBounds);

        gpu.BoundsCenterX = sphere.X;
        gpu.BoundsCenterY = sphere.Y;
        gpu.BoundsCenterZ = sphere.Z;
        gpu.BoundsRadius  = sphere.W;

        gpu.IndexCount    = obj.IndexCount;
        gpu.FirstIndex    = obj.IndexOffset;
        gpu.VertexOffset  = 0;
        gpu.MaterialIndex = obj.BindlessMaterialIndex;

        if (buildGroups)
        {
            // ---- 分组 ----
            //
            // 分组的依据是"下一条命令能不能与上一条共用同一次绑定": 顶点
            // 缓冲区、索引缓冲区、索引宽度、以及管线变体 (单面/双面)。
            //
            // 列表已按状态聚类过 (FSceneManager 的 FBatchStateLess), 所以组数
            // 远小于物体数 —— 压力场景 576 个物体 24 组。没排序过的话这里会
            // 退化成"每个物体一组", 那时 GPU 驱动一点也不比逐物体绘制快。
            //
            // 四项里有三项在**当前的排序下是冗余的**, 变异验证逐条量过:
            //   只去掉顶点缓冲区那一条 —— 每个网格自带一对顶点/索引缓冲区,
            //     索引那一条照样把组切开, 结果完全一样。
            //   只去掉单双面那一条 —— FBatchStateLess 的首要键就是单双面,
            //     分组最多跨过那一个边界, 而边界两侧的网格恰好不同。
            //   把整个条件拿掉才构造得出缺陷 (--gpu-driven-check 立刻报错)。
            //
            // 四项一个都不能删。它们冗余是**排序器当前键顺序的副产品**, 而
            // 分组不该依赖那个顺序 —— 排序换个写法, 冗余立刻消失。
            const bool startsNewGroup =
                m_Groups.IsEmpty() ||
                m_Groups[m_Groups.GetSize() - 1].VertexBuffer.Packed !=
                    obj.VertexBuffer.Packed ||
                m_Groups[m_Groups.GetSize() - 1].IndexBuffer.Packed !=
                    obj.IndexBuffer.Packed ||
                m_Groups[m_Groups.GetSize() - 1].IndexType != obj.IndexType ||
                m_Groups[m_Groups.GetSize() - 1].IsDoubleSided !=
                    obj.IsDoubleSided;

            if (startsNewGroup)
            {
                FDrawGroup group;
                group.VertexBuffer  = obj.VertexBuffer;
                group.IndexBuffer   = obj.IndexBuffer;
                group.IndexType     = obj.IndexType;
                group.IsDoubleSided = obj.IsDoubleSided;

                // 段内偏移, 不是缓冲区里的绝对下标 —— 间接命令那一段是按
                // 视图独立编号的, 而分组描述的正是那一段。
                group.FirstCommand  = written;
                group.CommandCount  = 0;

                m_Groups.Add(group);
            }

            ++m_Groups[m_Groups.GetSize() - 1].CommandCount;
        }

        ++written;
    }

    return written;
}

// ============================================================================
// UploadObjects — 三段写进同一个缓冲区
//
// 分三段是因为索引它的有三份不同的列表, 而它们的下标毫无对应关系:
// 相机列表经过了相机剔除, 投射体列表没有; 两者还由 FSceneManager 各自排序,
// 而排序不稳定 —— 比较相等的物体在两份列表里的先后可以不同。
//
// 这一点是踩出来的: 先前只上传一份, 阴影通道按投射体列表的下标去索引, 而
// 缓冲区里装的是相机列表 —— 表现是**某一盏灯的阴影整个消失**, 另一盏却完全
// 正确 (那个场景里两份列表恰好只在那一处不同)。不崩、不报错。
// ============================================================================

void FGpuCullPass::UploadObjects(const TArray<FRenderObject>* cameraObjects,
                                 const TArray<FRenderObject>* casters,
                                 const TArray<FRenderObject>* translucent,
                                 UInt32                       frameIndex)
{
    m_Groups.Clear();

    m_CameraCount     = 0;
    m_CullBase        = 0;
    m_ObjectCount     = 0;
    m_TranslucentBase = 0;

    if (frameIndex >= m_ObjectBuffers.GetSize())
    {
        return;
    }

    void* mapped = nullptr;

    if (!IsRHISuccess(m_Device->MapBuffer(m_ObjectBuffers[frameIndex],
                                          &mapped)) ||
        mapped == nullptr)
    {
        LIMX_LOG(LogRenderer, Error,
                 "[GpuCull] 逐物体缓冲区映射失败 — 本帧不走 GPU 驱动路径");
        return;
    }

    FGpuDrawObject* const gpuObjects = static_cast<FGpuDrawObject*>(mapped);

    UInt32 cursor = 0;

    // 段一: 相机列表 (已剔除) —— 相机通道的逐物体路径按它的下标索引
    m_CameraCount = WriteSegment(gpuObjects, cameraObjects, cursor, false);
    cursor += m_CameraCount;

    // 段二: 投射体列表 (未剔除) —— GPU 剔除的输入, 也是阴影通道逐物体路径
    //       的索引依据。分组只在这一段上算。
    m_CullBase    = cursor;
    m_ObjectCount = WriteSegment(gpuObjects, casters, cursor, true);
    cursor += m_ObjectCount;

    // 段三: 半透明 —— 走的是同一个 pbr.vert, 不给它们条目的话
    //       gl_InstanceIndex 会落到别人的数据上, 玻璃会长在别人的位置上。
    //       不参与剔除: 它必须严格由远及近, 而间接命令的顺序表达不了距离。
    m_TranslucentBase = cursor;

    const UInt32 translucentCount =
        WriteSegment(gpuObjects, translucent, cursor, false);

    cursor += translucentCount;

    m_Device->UnmapBuffer(m_ObjectBuffers[frameIndex]);

    const SizeType requested =
        (cameraObjects != nullptr ? cameraObjects->GetSize() : 0) +
        (casters != nullptr ? casters->GetSize() : 0) +
        (translucent != nullptr ? translucent->GetSize() : 0);

    if (requested > cursor)
    {
        // 超上限时明确报出来。静默截断的表现是"场景里少了一部分东西",
        // 而那与资源加载失败长得一样。
        LIMX_LOG(LogRenderer, Warning,
                 "[GpuCull] 三段合计 {} 个物体超过上限 {} — 只写进了 {} 个, "
                 "画面上会少东西",
                 static_cast<UInt64>(requested), kMaxGpuDrawObjects, cursor);
    }

    m_HasUploaded = true;
}

// ============================================================================
// Execute
// ============================================================================

void FGpuCullPass::Execute(IRHICommandBuffer*        commandBuffer,
                           const FRenderPassContext& context)
{
    m_HasUploaded = false;

    // 三份列表都要上传, 与开关无关 —— 索引它们的通道各不相同:
    //   相机通道的逐物体路径读 RenderObjects (已剔除);
    //   阴影通道的逐物体路径与 GPU 剔除都读 ShadowCasterObjects (未剔除);
    //   前向通道的半透明读 TranslucentObjects。
    if (context.ShadowCasterObjects == nullptr)
    {
        return;
    }

    commandBuffer->BeginDebugLabel("GpuCullPass", 0.4f, 0.8f, 0.9f);

    ResolveCounter(m_Device, context.FrameIndex);

    // 上传与分组**无论开关都做** —— 图形通道的顶点着色器只有一条路径, 它
    // 总是从 set 3 取模型矩阵。开关只决定命令是 CPU 逐个下的还是 GPU 写的。
    UploadObjects(context.RenderObjects, context.ShadowCasterObjects,
                  context.TranslucentObjects, context.FrameIndex);

    if (!IsEnabled() || m_ObjectCount == 0)
    {
        commandBuffer->EndDebugLabel();
        return;
    }

    const UInt32 frameIndex = context.FrameIndex;

    // ---- 计数器归零 ----
    {
        FRHIBufferCopyRegion region = {};
        region.SrcOffset = 0;
        region.DstOffset = 0;
        region.Size      = sizeof(FVisibleCounter);

        commandBuffer->CopyBuffer(m_ZeroSource, m_Counters[frameIndex], region);
    }

    {
        FRHIBufferMemoryBarrier barrier = {};
        barrier.SrcAccessMask = EAccessFlags::TransferWrite;
        barrier.DstAccessMask = EAccessFlags::ShaderRead |
                                EAccessFlags::ShaderWrite;
        barrier.Buffer        = m_Counters[frameIndex];

        commandBuffer->PipelineBarrier(
            EPipelineStageFlags::Transfer,
            EPipelineStageFlags::ComputeShader,
            nullptr, 0, &barrier, 1, nullptr, 0);
    }

    // ---- 剔除 ----
    commandBuffer->BindComputePipeline(m_CullPipeline);
    commandBuffer->BindDescriptorSet(EPipelineBindPoint::Compute,
                                     m_CullPipelineLayout, 0,
                                     m_CullSets[frameIndex]);

    // 视锥取自**含抖动**的 viewProj 还是不含? 用不含的。
    //
    // 抖动每帧改变亚像素偏移, 用它算出的视锥边界会逐帧漂移 —— 那本身无害
    // (漂移量远小于一个物体), 但会让"GPU 路径与 CPU 路径一致"这条判据变成
    // 逐帧不同, 无法比对。CPU 侧的剔除用的也是不含抖动的矩阵。
    const UInt32 groupCount =
        (m_ObjectCount + kCullWorkgroupSize - 1u) / kCullWorkgroupSize;

    // 逐视图各分派一次。
    //
    // 在着色器里循环 N 遍也行, 但每个视图的视锥平面不同而平面在 push constant
    // 里 —— 循环的话六个平面要变成 N×6 个, 128 字节装不下第二个视图。改用
    // UBO 又多一次绑定与一次每帧写入, 而整个剔除通道实测只有 0.012 ms。
    // **每个视图都分派, 包括本帧用不到的那些。**
    //
    // 用不到的视图那一段命令若从来没被写过, 就是一整段未初始化的显存 ——
    // 而 vkCmdDrawIndexedIndirect 拿它当命令读是未定义行为: indexCount 可能
    // 是任意大的数, 读到索引缓冲区之外, 轻则画面出现荒谬的几何, 重则设备
    // 复位。而这不会有任何报错。
    //
    // 给用不到的视图一个"全拒"视锥 (法线朝任意方向、常数项极大的负数), 于是
    // 那些段被老老实实写成 instanceCount = 0。多出来的几次分派各约一微秒。
    //
    // 这比"在图形通道那边判一下别用"更靠得住: 判漏一处就回到未定义行为, 而
    // 这里做完之后, **任何一段都可以安全地读**。
    //
    // ── 这一条不受任何画面判据保护 ──
    //
    // 实测过: 把这里与 FShadowPass 那个"本级有没有被剔除过"的判断同时拆掉,
    // --gpu-driven-check 与 --shadow-check 仍然都返回 0。因为那个场景构造
    // 不出来 —— 没有有效方向光时 ShadowEnabled 是 0, 片段着色器压根不采样
    // 级联贴图, 往里画什么都看不见。
    //
    // 但风险是实在的, 只是不在画面上: 未初始化的 indexCount 可能是几十亿,
    // 那是读到索引缓冲区之外。删掉它不会有任何检查变红 —— 所以这段话写在
    // 这里。
    for (UInt32 view = 0; view < kMaxCullViews; ++view)
    {
        const bool isActive = (view < m_ViewCount);

        FCullPushConstants push;

        for (Int32 p = 0; p < FFrustum::kPlaneCount; ++p)
        {
            if (isActive)
            {
                push.Planes[p][0] = m_Frusta[view].Planes[p].Normal.X;
                push.Planes[p][1] = m_Frusta[view].Planes[p].Normal.Y;
                push.Planes[p][2] = m_Frusta[view].Planes[p].Normal.Z;
                push.Planes[p][3] = m_Frusta[view].Planes[p].D;
            }
            else
            {
                // 全拒: 任何包围球都落在这个平面的背面
                push.Planes[p][0] = 0.0f;
                push.Planes[p][1] = 1.0f;
                push.Planes[p][2] = 0.0f;
                push.Planes[p][3] = -1.0e30f;
            }
        }

        push.ObjectCount = m_ObjectCount;
        push.ViewIndex   = view;
        push.ViewStride  = kMaxGpuDrawObjects;
        push.CullBase    = m_CullBase;

        commandBuffer->PushConstants(m_CullPipelineLayout,
                                     EShaderStage::Compute, 0,
                                     sizeof(FCullPushConstants), &push);

        commandBuffer->Dispatch(groupCount, 1, 1);
    }

    // ---- 屏障: 计算写 → 间接命令读 ----
    //
    // 两个目的阶段缺一不可: DrawIndirect 是命令处理器取命令的阶段,
    // VertexShader 是顶点着色器读逐物体数据的阶段。漏掉前者的表现是画面
    // 偶尔闪一下 (命令处理器读到上一帧的命令), 而那在低帧率下根本复现不了。
    {
        FRHIBufferMemoryBarrier barriers[2] = {};

        barriers[0].SrcAccessMask = EAccessFlags::ShaderWrite;
        barriers[0].DstAccessMask = EAccessFlags::IndirectCommandRead;
        barriers[0].Buffer        = m_IndirectBuffers[frameIndex];

        barriers[1].SrcAccessMask = EAccessFlags::ShaderWrite;
        barriers[1].DstAccessMask = EAccessFlags::TransferRead;
        barriers[1].Buffer        = m_Counters[frameIndex];

        commandBuffer->PipelineBarrier(
            EPipelineStageFlags::ComputeShader,
            EPipelineStageFlags::DrawIndirect |
                EPipelineStageFlags::Transfer,
            nullptr, 0, barriers, 2, nullptr, 0);
    }

    // 计数器拷去回读缓冲区 —— 下一次进本函数时读上一帧的值
    {
        FRHIBufferCopyRegion region = {};
        region.SrcOffset = 0;
        region.DstOffset = 0;
        region.Size      = sizeof(FVisibleCounter);

        commandBuffer->CopyBuffer(m_Counters[frameIndex],
                                  m_CounterReadbacks[frameIndex], region);
    }

    commandBuffer->EndDebugLabel();
}

// ============================================================================
// ResolveCounter — 读上一帧写下的可见数
// ============================================================================

void FGpuCullPass::ResolveCounter(IRHIDevice* device, UInt32 frameIndex)
{
    if (device == nullptr || frameIndex >= m_CounterReadbacks.GetSize())
    {
        return;
    }

    void* mapped = nullptr;

    if (!IsRHISuccess(device->MapBuffer(m_CounterReadbacks[frameIndex],
                                        &mapped)) ||
        mapped == nullptr)
    {
        return;
    }

    const FVisibleCounter* const counter =
        static_cast<const FVisibleCounter*>(mapped);

    for (UInt32 view = 0; view < kMaxCullViews; ++view)
    {
        m_LastVisible[view] = counter->VisibleCount[view];
    }

    device->UnmapBuffer(m_CounterReadbacks[frameIndex]);
}

// ============================================================================
// 访问器
// ============================================================================

FRHIBufferHandle FGpuCullPass::GetIndirectBuffer(UInt32 frameIndex) const
{
    if (frameIndex >= m_IndirectBuffers.GetSize())
    {
        return FRHIBufferHandle();
    }

    return m_IndirectBuffers[frameIndex];
}

FRHIDescriptorSetHandle FGpuCullPass::GetDrawObjectSet(UInt32 frameIndex) const
{
    if (frameIndex >= m_DrawObjectSets.GetSize())
    {
        return FRHIDescriptorSetHandle();
    }

    return m_DrawObjectSets[frameIndex];
}

// ============================================================================
// OnResize / Shutdown
// ============================================================================

ERHIResult FGpuCullPass::OnResize(const FPassResizeDesc& desc)
{
    (void)desc;
    return ERHIResult::Success;
}

void FGpuCullPass::ReleaseSwapchainResources(IRHIDevice* device)
{
    (void)device;
}

void FGpuCullPass::Shutdown(IRHIDevice* device)
{
    if (device == nullptr)
    {
        return;
    }

    device->DestroyComputePipeline(m_CullPipeline);
    device->DestroyPipelineLayout(m_CullPipelineLayout);
    device->DestroyShader(m_CullShader);
    device->DestroyDescSetLayout(m_CullSetLayout);

    device->DestroyBuffer(m_ZeroSource);

    for (SizeType i = 0; i < m_ObjectBuffers.GetSize(); ++i)
    {
        device->DestroyBuffer(m_ObjectBuffers[i]);
    }
    m_ObjectBuffers.Clear();

    for (SizeType i = 0; i < m_IndirectBuffers.GetSize(); ++i)
    {
        device->DestroyBuffer(m_IndirectBuffers[i]);
    }
    m_IndirectBuffers.Clear();

    for (SizeType i = 0; i < m_Counters.GetSize(); ++i)
    {
        device->DestroyBuffer(m_Counters[i]);
    }
    m_Counters.Clear();

    for (SizeType i = 0; i < m_CounterReadbacks.GetSize(); ++i)
    {
        device->DestroyBuffer(m_CounterReadbacks[i]);
    }
    m_CounterReadbacks.Clear();

    m_CullSets.Clear();
    m_DrawObjectSets.Clear();
    m_Groups.Clear();

    LIMX_LOG(LogRenderer, Log, "[GpuCull] 已关闭");
}

} // namespace Limx
