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

    UInt32 ObjectCount = 0;
    UInt32 Pad0        = 0;
    UInt32 Pad1        = 0;
    UInt32 Pad2        = 0;
};

static_assert(sizeof(FCullPushConstants) == 112,
    "FCullPushConstants 必须为 112 字节 — 与 draw_cull.comp 的 push constant "
    "一致");

/// 可见计数器
struct FVisibleCounter
{
    UInt32 VisibleCount = 0;
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
            bufferDesc.Size =
                static_cast<UInt64>(sizeof(FDrawIndexedIndirectCommand)) *
                kMaxGpuDrawObjects;
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
                kMaxGpuDrawObjects);

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

void FGpuCullPass::UploadObjects(const TArray<FRenderObject>* opaque,
                                 const TArray<FRenderObject>* translucent,
                                 UInt32                       frameIndex)
{
    m_Groups.Clear();
    m_ObjectCount     = 0;
    m_TranslucentBase = 0;

    if (frameIndex >= m_ObjectBuffers.GetSize() || opaque == nullptr)
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

    const SizeType total = opaque->GetSize();

    bool truncated = false;

    for (SizeType i = 0; i < total; ++i)
    {
        if (m_ObjectCount >= kMaxGpuDrawObjects)
        {
            // 超上限时明确报出来。静默截断的表现是"场景里少了一部分东西",
            // 而那与资源加载失败长得一样。
            truncated = true;
            break;
        }

        const FRenderObject& obj = (*opaque)[i];

        FGpuDrawObject& gpu = gpuObjects[m_ObjectCount];

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

        // ---- 分组 ----
        //
        // 分组的依据是"下一条命令能不能与上一条共用同一次绑定": 顶点缓冲区、
        // 索引缓冲区、索引宽度、以及管线变体 (单面/双面)。任何一项变了就得
        // 换一组。
        //
        // 列表已按状态聚类过 (FSceneManager 的 FBatchStateLess), 所以组数远
        // 小于物体数 —— 压力场景 576 个物体 24 组。没排序过的话这里会退化成
        // "每个物体一组", 那时 GPU 驱动一点也不比逐物体绘制快。
        //
        // 四项里有三项在**当前的排序下是冗余的**, 变异验证逐条量过:
        //   - 只去掉顶点缓冲区那一条: 每个网格自带一对顶点/索引缓冲区, 索引
        //     那一条照样把组切开, 结果完全一样。
        //   - 只去掉单双面那一条: FBatchStateLess 的首要键就是单双面, 分组
        //     最多跨过那一个边界, 而边界两侧的网格恰好不同。
        //   把整个条件拿掉才构造得出缺陷 (--gpu-driven-check 立刻报错)。
        //
        // 四项一个都不能删。它们冗余是**排序器当前键顺序的副产品**, 而分组
        // 不该依赖那个顺序 —— 排序换个写法, 冗余立刻消失。
        const bool startsNewGroup =
            m_Groups.IsEmpty() ||
            m_Groups[m_Groups.GetSize() - 1].VertexBuffer.Packed !=
                obj.VertexBuffer.Packed ||
            m_Groups[m_Groups.GetSize() - 1].IndexBuffer.Packed !=
                obj.IndexBuffer.Packed ||
            m_Groups[m_Groups.GetSize() - 1].IndexType != obj.IndexType ||
            m_Groups[m_Groups.GetSize() - 1].IsDoubleSided != obj.IsDoubleSided;

        if (startsNewGroup)
        {
            FDrawGroup group;
            group.VertexBuffer  = obj.VertexBuffer;
            group.IndexBuffer   = obj.IndexBuffer;
            group.IndexType     = obj.IndexType;
            group.IsDoubleSided = obj.IsDoubleSided;
            group.FirstCommand  = m_ObjectCount;
            group.CommandCount  = 0;

            m_Groups.Add(group);
        }

        ++m_Groups[m_Groups.GetSize() - 1].CommandCount;
        ++m_ObjectCount;
    }

    // ---- 半透明 ----
    //
    // 接在不透明后面, 不参与分组也不参与剔除。它们走的是同一个 pbr.vert,
    // 而那个着色器只有一条路径 —— 不给它们条目的话, gl_InstanceIndex 会落到
    // 不透明物体的数据上, 玻璃会长在别人的位置上。
    //
    // 不参与剔除是因为半透明必须严格由远及近绘制, 而那个顺序是 CPU 排出来
    // 的; 间接命令的顺序表达不了"按距离"这件事。
    m_TranslucentBase = m_ObjectCount;

    UInt32 written = m_ObjectCount;

    if (translucent != nullptr)
    {
        for (SizeType i = 0; i < translucent->GetSize(); ++i)
        {
            if (written >= kMaxGpuDrawObjects)
            {
                truncated = true;
                break;
            }

            const FRenderObject& obj = (*translucent)[i];

            FGpuDrawObject& gpu = gpuObjects[written];

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

            ++written;
        }
    }

    m_Device->UnmapBuffer(m_ObjectBuffers[frameIndex]);

    if (truncated)
    {
        LIMX_LOG(LogRenderer, Warning,
                 "[GpuCull] 物体数超过上限 {} — 其余被忽略, 画面上会少东西",
                 kMaxGpuDrawObjects);
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

    // 上传哪份列表取决于开关, 而**图形通道迭代的是同一份** —— 两者的下标
    // 必须对得上, 因为顶点着色器靠 gl_InstanceIndex 去这个缓冲区取数据。
    //
    //   开: 取剔除前的 ShadowCasterObjects ("未经相机视锥剔除的不透明与蒙版
    //       批次", 排序规则与主列表相同)。用剔除后的列表的话, GPU 剔除永远
    //       剔不掉任何东西 —— 一个什么都不做的剔除实现会得到完全正确的画面,
    //       判据也就无从判定。
    //   关: 取 CPU 已经剔过的 RenderObjects, 图形通道逐个绘制时传的
    //       firstInstance 就是它在这份列表里的下标。
    const TArray<FRenderObject>* source =
        IsEnabled() ? context.ShadowCasterObjects : context.RenderObjects;

    if (source == nullptr)
    {
        return;
    }

    commandBuffer->BeginDebugLabel("GpuCullPass", 0.4f, 0.8f, 0.9f);

    ResolveCounter(m_Device, context.FrameIndex);

    // 上传与分组**无论开关都做** —— 图形通道的顶点着色器只有一条路径, 它
    // 总是从 set 3 取模型矩阵。开关只决定命令是 CPU 逐个下的还是 GPU 写的。
    UploadObjects(source, context.TranslucentObjects, context.FrameIndex);

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
    FCullPushConstants push;

    for (Int32 p = 0; p < FFrustum::kPlaneCount; ++p)
    {
        push.Planes[p][0] = m_Frustum.Planes[p].Normal.X;
        push.Planes[p][1] = m_Frustum.Planes[p].Normal.Y;
        push.Planes[p][2] = m_Frustum.Planes[p].Normal.Z;
        push.Planes[p][3] = m_Frustum.Planes[p].D;
    }

    push.ObjectCount = m_ObjectCount;

    commandBuffer->PushConstants(m_CullPipelineLayout, EShaderStage::Compute,
                                 0, sizeof(FCullPushConstants), &push);

    const UInt32 groupCount =
        (m_ObjectCount + kCullWorkgroupSize - 1u) / kCullWorkgroupSize;

    commandBuffer->Dispatch(groupCount, 1, 1);

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

    m_LastVisible = counter->VisibleCount;

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
