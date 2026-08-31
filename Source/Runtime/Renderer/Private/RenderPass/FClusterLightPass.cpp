/*******************************************************************************
 * 文件: FClusterLightPass.cpp
 * 创建时间: 2026-08-31
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   分簇光照剔除通道的实现
 *
 * 依赖关系:
 *   内部: Renderer/RenderPass/FClusterLightPass.h, RenderCore/Shaders
 *
 ******************************************************************************/

#include "Renderer/RenderPass/FClusterLightPass.h"

#include "RenderCore/Shaders/FShaderManager.h"
#include "Core/Logging/FLog.h"

namespace Limx
{

LIMX_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{

/// 与两个 .comp 里的 local_size_x 一致
constexpr UInt32 kComputeGroupSize = 64;

/// cluster_build.comp 的 push constant 布局
struct FBuildParams
{
    FMatrix InverseProjection;
    Float32 NearPlane = 0.1f;
    Float32 FarPlane  = 100.0f;
    Float32 Pad0      = 0.0f;
    Float32 Pad1      = 0.0f;
};

static_assert(sizeof(FBuildParams) == 80,
              "FBuildParams 必须是 80 字节 — 与 cluster_build.comp 的 "
              "push constant 块一致 (mat4 + vec4)");

/// light_cull.comp 的 push constant 布局
///
/// 字段顺序与着色器里**逐字段一致**。uvec4 在前是因为 std430 下 mat4 要求
/// 16 字节对齐 —— 把标量放在 mat4 后面会引入填充, 而那个填充 C++ 侧不会
/// 自动跟上。
struct FCullParams
{
    UInt32  LightCount = 0;
    UInt32  Capacity   = 0;
    UInt32  Pad0       = 0;
    UInt32  Pad1       = 0;
    FMatrix View;
};

static_assert(sizeof(FCullParams) == 80,
              "FCullParams 必须是 80 字节 — 与 light_cull.comp 的 "
              "push constant 块一致 (uvec4 + mat4)");

/// 计数器缓冲区的内容 —— 与 light_cull.comp 的 ClusterCounter 一致
struct FClusterCounter
{
    UInt32 Allocated  = 0;
    UInt32 Overflowed = 0;
};

static_assert(sizeof(FClusterCounter) == 8,
              "FClusterCounter 必须是 8 字节");

UInt32 GroupCountFor(UInt32 threadCount)
{
    return (threadCount + kComputeGroupSize - 1u) / kComputeGroupSize;
}

} // namespace

// ============================================================================
// Setup
// ============================================================================

ERHIResult FClusterLightPass::Setup(const FPassSetupDesc& desc)
{
    m_Device     = desc.Device;
    m_FrameCount = desc.MaxFramesInFlight;

    if (m_Device == nullptr || m_FrameCount == 0)
    {
        LIMX_LOG(LogRenderer, Error,
                 "[ClusterLightPass] Setup 参数无效");
        return ERHIResult::ErrorInvalidParameter;
    }

    ERHIResult result = CreateBuffers(m_Device, m_FrameCount);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    result = CreateDescriptors(m_Device, m_FrameCount);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    result = CreatePipelines(m_Device);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    LIMX_LOG(LogRenderer, Display,
             "[ClusterLightPass] 初始化完成 — {}×{}×{} = {} 簇, "
             "索引表容量 {} 条 ({} KiB/帧)",
             kClusterGridX, kClusterGridY, kClusterGridZ, kClusterCount,
             kClusterLightIndexCapacity,
             (kClusterCount * 32u + kClusterCount * 8u +
              kClusterLightIndexCapacity * 4u) / 1024u);

    return ERHIResult::Success;
}

// ============================================================================
// CreateBuffers
// ============================================================================

ERHIResult FClusterLightPass::CreateBuffers(IRHIDevice* device,
                                            UInt32      frameCount)
{
    // 每并行帧一套。共用一套的话, 帧 N+1 的计算通道会覆写帧 N 的前向通道
    // 正在读的簇表 —— 那是只在高帧率下偶发的光照闪烁, 验证层不报。
    struct FBufferSpec
    {
        UInt64                    Size;
        EMemoryUsage              Memory;
        bool                      NeedsTransferDst;
        bool                      NeedsTransferSrc;
        TArray<FRHIBufferHandle>* Target;
        const char*               DebugName;
    };

    const FBufferSpec specs[] =
    {
        { static_cast<UInt64>(kClusterCount) * 2u * 16u,
          EMemoryUsage::GpuOnly, false, true,
          &m_ClusterBounds, "ClusterBounds" },

        { static_cast<UInt64>(kClusterCount) * 8u,
          EMemoryUsage::GpuOnly, false, true,
          &m_ClusterGrid, "ClusterGrid" },

        { static_cast<UInt64>(kClusterLightIndexCapacity) * 4u,
          EMemoryUsage::GpuOnly, false, true,
          &m_LightIndices, "ClusterLightIndices" },

        { sizeof(FClusterCounter),
          EMemoryUsage::GpuOnly, true, true,
          &m_Counters, "ClusterCounter" },

        { sizeof(FClusterCounter),
          EMemoryUsage::GpuToCpu, true, false,
          &m_CounterReadbacks, "ClusterCounterReadback" },
    };

    for (SizeType s = 0; s < sizeof(specs) / sizeof(specs[0]); ++s)
    {
        const FBufferSpec& spec = specs[s];

        spec.Target->Reserve(frameCount);

        for (UInt32 i = 0; i < frameCount; ++i)
        {
            UInt32 usage = 0;

            // 回读缓冲区不参与着色器访问, 只是拷贝的目的地
            if (spec.Memory != EMemoryUsage::GpuToCpu)
            {
                usage |= static_cast<UInt32>(EBufferUsage::StorageBuffer);
            }

            if (spec.NeedsTransferDst)
            {
                usage |= static_cast<UInt32>(EBufferUsage::TransferDst);
            }

            if (spec.NeedsTransferSrc)
            {
                usage |= static_cast<UInt32>(EBufferUsage::TransferSrc);
            }

            FRHIBufferDesc bufferDesc = {};
            bufferDesc.Size        = spec.Size;
            bufferDesc.Usage       = static_cast<EBufferUsage>(usage);
            bufferDesc.MemoryUsage = spec.Memory;
            bufferDesc.DebugName   = spec.DebugName;

            FRHIBufferHandle buffer;

            const ERHIResult result = device->CreateBuffer(bufferDesc, buffer);

            if (!IsRHISuccess(result))
            {
                LIMX_LOG(LogRenderer, Error,
                         "[ClusterLightPass] {} [{}] 创建失败",
                         spec.DebugName, i);
                return result;
            }

            spec.Target->Add(buffer);
        }
    }

    // 全零源 —— 每帧拷进计数器做清零。
    //
    // RHI 没有 FillBuffer, 而计数器必须每帧归零 (原子加是累积的)。用一个
    // 常驻的全零缓冲区做拷贝源比给 RHI 加一条命令便宜。
    {
        FRHIBufferDesc zeroDesc = {};
        zeroDesc.Size        = sizeof(FClusterCounter);
        zeroDesc.Usage       = EBufferUsage::TransferSrc;
        zeroDesc.MemoryUsage = EMemoryUsage::CpuToGpu;
        zeroDesc.DebugName   = "ClusterCounterZero";

        const ERHIResult result = device->CreateBuffer(zeroDesc, m_ZeroSource);

        if (!IsRHISuccess(result))
        {
            LIMX_LOG(LogRenderer, Error,
                     "[ClusterLightPass] 全零源缓冲区创建失败");
            return result;
        }

        void* mapped = nullptr;

        if (IsRHISuccess(device->MapBuffer(m_ZeroSource, &mapped)) &&
            mapped != nullptr)
        {
            Memory::MemZero(mapped, sizeof(FClusterCounter));
            device->UnmapBuffer(m_ZeroSource);
        }
        else
        {
            LIMX_LOG(LogRenderer, Error,
                     "[ClusterLightPass] 全零源映射失败 — "
                     "计数器将无法清零");
            return ERHIResult::ErrorUnknown;
        }
    }

    return ERHIResult::Success;
}

// ============================================================================
// CreateDescriptors
// ============================================================================

ERHIResult FClusterLightPass::CreateDescriptors(IRHIDevice* device,
                                                UInt32      frameCount)
{
    // ---- cluster_build.comp: 只写簇包围盒 ----
    {
        FRHIDescriptorBinding binding = {};
        binding.Binding    = 0;
        binding.Type       = EDescriptorType::StorageBuffer;
        binding.Count      = 1;
        binding.StageFlags = EShaderStage::Compute;

        FRHIDescSetLayoutDesc layoutDesc = {};
        layoutDesc.Bindings     = &binding;
        layoutDesc.BindingCount = 1;
        layoutDesc.DebugName    = "ClusterBuildSetLayout";

        const ERHIResult result =
            device->CreateDescSetLayout(layoutDesc, m_BuildSetLayout);

        if (!IsRHISuccess(result))
        {
            LIMX_LOG(LogRenderer, Error,
                     "[ClusterLightPass] build 描述符布局创建失败");
            return result;
        }
    }

    // ---- light_cull.comp: 五个 storage buffer ----
    {
        FRHIDescriptorBinding bindings[5] = {};

        for (UInt32 i = 0; i < 5; ++i)
        {
            bindings[i].Binding    = i;
            bindings[i].Type       = EDescriptorType::StorageBuffer;
            bindings[i].Count      = 1;
            bindings[i].StageFlags = EShaderStage::Compute;
        }

        FRHIDescSetLayoutDesc layoutDesc = {};
        layoutDesc.Bindings     = bindings;
        layoutDesc.BindingCount = 5;
        layoutDesc.DebugName    = "ClusterCullSetLayout";

        const ERHIResult result =
            device->CreateDescSetLayout(layoutDesc, m_CullSetLayout);

        if (!IsRHISuccess(result))
        {
            LIMX_LOG(LogRenderer, Error,
                     "[ClusterLightPass] cull 描述符布局创建失败");
            return result;
        }
    }

    // ---- 每帧一份描述符集 ----
    m_BuildSets.Reserve(frameCount);
    m_CullSets.Reserve(frameCount);

    for (UInt32 i = 0; i < frameCount; ++i)
    {
        FRHIDescriptorSetHandle buildSet;

        ERHIResult result =
            device->AllocateDescriptorSet(m_BuildSetLayout, buildSet);

        if (!IsRHISuccess(result))
        {
            LIMX_LOG(LogRenderer, Error,
                     "[ClusterLightPass] build 描述符集 [{}] 分配失败", i);
            return result;
        }

        FRHIDescriptorWrite buildWrite = FRHIDescriptorWrite::StorageBuffer(
            buildSet, 0, m_ClusterBounds[i], 0,
            static_cast<UInt64>(kClusterCount) * 2u * 16u);

        device->UpdateDescriptorSets(&buildWrite, 1);

        m_BuildSets.Add(buildSet);

        FRHIDescriptorSetHandle cullSet;

        result = device->AllocateDescriptorSet(m_CullSetLayout, cullSet);

        if (!IsRHISuccess(result))
        {
            LIMX_LOG(LogRenderer, Error,
                     "[ClusterLightPass] cull 描述符集 [{}] 分配失败", i);
            return result;
        }

        m_CullSets.Add(cullSet);
    }

    return ERHIResult::Success;
}

// ============================================================================
// CreatePipelines
// ============================================================================

ERHIResult FClusterLightPass::CreatePipelines(IRHIDevice* device)
{
    FShaderManager& shaders = FShaderManager::Get();

    struct FPipelineSpec
    {
        const char*                Shader;
        FRHIDescSetLayoutHandle*   SetLayout;
        UInt32                     PushSize;
        FRHIPipelineLayoutHandle*  Layout;
        FRHIComputePipelineHandle* Pipeline;
        FRHIShaderHandle*          Shader2;
        const char*                DebugName;
    };

    const FPipelineSpec specs[] =
    {
        { "Builtin/cluster_build.comp", &m_BuildSetLayout,
          sizeof(FBuildParams), &m_BuildPipelineLayout, &m_BuildPipeline,
          &m_BuildShader, "ClusterBuildPipeline" },

        { "Builtin/light_cull.comp", &m_CullSetLayout,
          sizeof(FCullParams), &m_CullPipelineLayout, &m_CullPipeline,
          &m_CullShader, "LightCullPipeline" },
    };

    for (SizeType s = 0; s < sizeof(specs) / sizeof(specs[0]); ++s)
    {
        const FPipelineSpec& spec = specs[s];

        ERHIResult shaderResult = shaders.CreateShaderModule(
            device, FString(spec.Shader), EShaderStage::Compute,
            *spec.Shader2);

        if (!IsRHISuccess(shaderResult))
        {
            LIMX_LOG(LogRenderer, Error,
                     "[ClusterLightPass] 着色器加载失败: {}", spec.Shader);
            return shaderResult;
        }

        FRHIPushConstantRange pushRange = {};
        pushRange.StageFlags = EShaderStage::Compute;
        pushRange.Offset     = 0;
        pushRange.Size       = spec.PushSize;

        FRHIPipelineLayoutDesc layoutDesc = {};
        layoutDesc.SetLayouts             = spec.SetLayout;
        layoutDesc.SetLayoutCount         = 1;
        layoutDesc.PushConstantRanges     = &pushRange;
        layoutDesc.PushConstantRangeCount = 1;
        layoutDesc.DebugName              = spec.DebugName;

        ERHIResult result =
            device->CreatePipelineLayout(layoutDesc, *spec.Layout);

        if (!IsRHISuccess(result))
        {
            LIMX_LOG(LogRenderer, Error,
                     "[ClusterLightPass] {} 管线布局创建失败",
                     spec.DebugName);
            return result;
        }

        FRHIComputePipelineDesc pipelineDesc = {};
        pipelineDesc.ComputeShader.Shader     = *spec.Shader2;
        pipelineDesc.ComputeShader.Stage      = EShaderStage::Compute;
        pipelineDesc.ComputeShader.EntryPoint = "main";
        pipelineDesc.PipelineLayout           = *spec.Layout;
        pipelineDesc.DebugName                = spec.DebugName;

        result = device->CreateComputePipeline(pipelineDesc, *spec.Pipeline);

        if (!IsRHISuccess(result))
        {
            LIMX_LOG(LogRenderer, Error,
                     "[ClusterLightPass] {} 管线创建失败", spec.DebugName);
            return result;
        }
    }

    return ERHIResult::Success;
}

// ============================================================================
// SetCameraParams / SetLightSource
// ============================================================================

void FClusterLightPass::SetCameraParams(const FMatrix& view,
                                        const FMatrix& projectionNoJitter,
                                        Float32        nearPlane,
                                        Float32        farPlane)
{
    m_View              = view;
    m_InverseProjection = projectionNoJitter.Inverse();
    m_NearPlane         = nearPlane;
    m_FarPlane          = farPlane;
}

void FClusterLightPass::SetLightSource(FRHIBufferHandle lightBuffer,
                                       UInt32           lightCount)
{
    m_LightBuffer = lightBuffer;
    m_LightCount  = lightCount;
}

// ============================================================================
// Execute
// ============================================================================

void FClusterLightPass::Execute(IRHICommandBuffer*        commandBuffer,
                                const FRenderPassContext& context)
{
    if (commandBuffer == nullptr || m_Device == nullptr)
    {
        return;
    }

    const UInt32 frameIndex = context.FrameIndex;

    if (frameIndex >= m_FrameCount)
    {
        LIMX_LOG(LogRenderer, Error,
                 "[ClusterLightPass] 帧索引 {} 超出范围 {}",
                 frameIndex, m_FrameCount);
        return;
    }

    if (!m_Enabled)
    {
        // 分簇关闭时不分派。片段着色器那边走的是暴力法, 簇表没人读 ——
        // 跑了就是白付 0.05~0.2 ms。
        //
        // 开关与着色路径共用同一个布尔值 (见 FRenderer::SetClusteredLighting)。
        // 两者不一致的话, 要么白付开销, 要么读到过期的簇表。
        return;
    }

    if (!m_LightBuffer.IsValid())
    {
        LIMX_LOG(LogRenderer, Error,
                 "[ClusterLightPass] 光源缓冲区未设置 — 本帧不做剔除");
        return;
    }

    // 上一帧的计数器已经被 GPU 写完并拷进回读缓冲区, 现在读出来
    ResolveCounter(m_Device, frameIndex);

    const UInt32 groupCount = GroupCountFor(kClusterCount);

    // ---- 1. 计数器清零 ----
    //
    // 必须在两次分派之前。原子加是累积的, 不清零的话第二帧起分配的偏移
    // 会一路涨到容量之外 —— 表现是"跑几帧之后光照突然全没了", 而那个
    // 时机看起来毫无规律。
    {
        FRHIBufferCopyRegion region = {};
        region.SrcOffset = 0;
        region.DstOffset = 0;
        region.Size      = sizeof(FClusterCounter);

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

    // ---- 2. 算簇包围盒 ----
    commandBuffer->BindComputePipeline(m_BuildPipeline);
    commandBuffer->BindDescriptorSet(EPipelineBindPoint::Compute,
                                     m_BuildPipelineLayout, 0,
                                     m_BuildSets[frameIndex]);

    FBuildParams buildParams;
    buildParams.InverseProjection = m_InverseProjection;
    buildParams.NearPlane         = m_NearPlane;
    buildParams.FarPlane          = m_FarPlane;

    commandBuffer->PushConstants(m_BuildPipelineLayout,
                                 EShaderStage::Compute, 0,
                                 sizeof(FBuildParams), &buildParams);

    commandBuffer->Dispatch(groupCount, 1, 1);

    // ---- 3. 屏障: 包围盒写完才能读 ----
    {
        FRHIBufferMemoryBarrier barrier = {};
        barrier.SrcAccessMask = EAccessFlags::ShaderWrite;
        barrier.DstAccessMask = EAccessFlags::ShaderRead;
        barrier.Buffer        = m_ClusterBounds[frameIndex];

        commandBuffer->PipelineBarrier(
            EPipelineStageFlags::ComputeShader,
            EPipelineStageFlags::ComputeShader,
            nullptr, 0, &barrier, 1, nullptr, 0);
    }

    // ---- 4. 分配光源 ----
    //
    // 描述符集每帧重写。光源缓冲区句柄由 FLightManager 按帧给出, 而它在
    // 交换链重建之后可能变 —— 每帧重写比跟踪"它变了没有"可靠。
    {
        FRHIDescriptorWrite writes[5];

        writes[0] = FRHIDescriptorWrite::StorageBuffer(
            m_CullSets[frameIndex], 0, m_ClusterBounds[frameIndex], 0,
            static_cast<UInt64>(kClusterCount) * 2u * 16u);

        writes[1] = FRHIDescriptorWrite::StorageBuffer(
            m_CullSets[frameIndex], 1, m_LightBuffer, 0, 0);

        writes[2] = FRHIDescriptorWrite::StorageBuffer(
            m_CullSets[frameIndex], 2, m_ClusterGrid[frameIndex], 0,
            static_cast<UInt64>(kClusterCount) * 8u);

        writes[3] = FRHIDescriptorWrite::StorageBuffer(
            m_CullSets[frameIndex], 3, m_LightIndices[frameIndex], 0,
            static_cast<UInt64>(kClusterLightIndexCapacity) * 4u);

        writes[4] = FRHIDescriptorWrite::StorageBuffer(
            m_CullSets[frameIndex], 4, m_Counters[frameIndex], 0,
            sizeof(FClusterCounter));

        m_Device->UpdateDescriptorSets(writes, 5);
    }

    commandBuffer->BindComputePipeline(m_CullPipeline);
    commandBuffer->BindDescriptorSet(EPipelineBindPoint::Compute,
                                     m_CullPipelineLayout, 0,
                                     m_CullSets[frameIndex]);

    FCullParams cullParams;
    cullParams.LightCount = m_LightCount;
    cullParams.Capacity   = kClusterLightIndexCapacity;
    cullParams.View       = m_View;

    commandBuffer->PushConstants(m_CullPipelineLayout,
                                 EShaderStage::Compute, 0,
                                 sizeof(FCullParams), &cullParams);

    commandBuffer->Dispatch(groupCount, 1, 1);

    // ---- 5. 屏障: 簇表写完才能给片段着色器读 ----
    //
    // 两个缓冲区各一条。合并成一条全局内存屏障也行, 但那会把范围扩大到
    // 整个设备内存 —— 精确的缓冲区屏障让驱动能做更细的调度。
    {
        FRHIBufferMemoryBarrier barriers[2] = {};

        barriers[0].SrcAccessMask = EAccessFlags::ShaderWrite;
        barriers[0].DstAccessMask = EAccessFlags::ShaderRead;
        barriers[0].Buffer        = m_ClusterGrid[frameIndex];

        barriers[1].SrcAccessMask = EAccessFlags::ShaderWrite;
        barriers[1].DstAccessMask = EAccessFlags::ShaderRead;
        barriers[1].Buffer        = m_LightIndices[frameIndex];

        commandBuffer->PipelineBarrier(
            EPipelineStageFlags::ComputeShader,
            EPipelineStageFlags::FragmentShader,
            nullptr, 0, barriers, 2, nullptr, 0);
    }

    // ---- 6. 计数器拷进回读缓冲区 ----
    //
    // 下一帧读。当帧读需要 WaitIdle, 而那正是这个通道要避免的东西。
    {
        FRHIBufferMemoryBarrier barrier = {};
        barrier.SrcAccessMask = EAccessFlags::ShaderWrite;
        barrier.DstAccessMask = EAccessFlags::TransferRead;
        barrier.Buffer        = m_Counters[frameIndex];

        commandBuffer->PipelineBarrier(
            EPipelineStageFlags::ComputeShader,
            EPipelineStageFlags::Transfer,
            nullptr, 0, &barrier, 1, nullptr, 0);
    }

    {
        FRHIBufferCopyRegion region = {};
        region.SrcOffset = 0;
        region.DstOffset = 0;
        region.Size      = sizeof(FClusterCounter);

        commandBuffer->CopyBuffer(m_Counters[frameIndex],
                                  m_CounterReadbacks[frameIndex], region);
    }
}

// ============================================================================
// ResolveCounter
// ============================================================================

void FClusterLightPass::ResolveCounter(IRHIDevice* device, UInt32 frameIndex)
{
    void* mapped = nullptr;

    if (!IsRHISuccess(device->MapBuffer(m_CounterReadbacks[frameIndex],
                                        &mapped)) ||
        mapped == nullptr)
    {
        return;
    }

    FClusterCounter counter;
    Memory::MemCopy(&counter, mapped, sizeof(FClusterCounter));

    device->UnmapBuffer(m_CounterReadbacks[frameIndex]);

    m_LastAllocated = counter.Allocated;

    if (counter.Overflowed != 0u)
    {
        m_HasOverflowed = true;

        // 只报一次 —— 它每帧都会重复, 刷满日志之后反而没人看
        if (!m_HasReportedOverflow)
        {
            LIMX_LOG(LogRenderer, Error,
                     "[ClusterLightPass] 光源索引表溢出 — 容量 {} 条不够, "
                     "已有光源被丢弃。画面上表现为某些区域少几盏光。"
                     "调大 kClusterLightIndexCapacity 或减少光源。",
                     kClusterLightIndexCapacity);

            m_HasReportedOverflow = true;
        }
    }
}

// ============================================================================
// OnResize
// ============================================================================

ERHIResult FClusterLightPass::OnResize(const FPassResizeDesc& desc)
{
    // 网格尺寸固定 (见 FClusterGrid.h), 缓冲区大小与分辨率无关 —— 这里
    // 什么都不用做, 描述符也永远不必重写。
    //
    // 这正是选固定网格的理由: Pass 的 OnResize 拿不到描述符句柄, 而
    // "描述符仍指向已销毁的缓冲区"是一个只在交换链重建时触发、平时完全
    // 看不见的坑。
    (void)desc;

    return ERHIResult::Success;
}

void FClusterLightPass::ReleaseSwapchainResources(IRHIDevice* device)
{
    // 本通道不持有任何引用交换链图像的资源
    (void)device;
}

// ============================================================================
// Shutdown
// ============================================================================

void FClusterLightPass::Shutdown(IRHIDevice* device)
{
    if (device == nullptr)
    {
        return;
    }

    TArray<FRHIBufferHandle>* const bufferArrays[] =
    {
        &m_ClusterBounds, &m_ClusterGrid, &m_LightIndices,
        &m_Counters, &m_CounterReadbacks,
    };

    for (SizeType a = 0; a < sizeof(bufferArrays) / sizeof(bufferArrays[0]);
         ++a)
    {
        for (SizeType i = 0; i < bufferArrays[a]->GetSize(); ++i)
        {
            device->DestroyBuffer((*bufferArrays[a])[i]);
        }

        bufferArrays[a]->Clear();
    }

    if (m_ZeroSource.IsValid())
    {
        device->DestroyBuffer(m_ZeroSource);
        m_ZeroSource = {};
    }

    if (m_BuildShader.IsValid())
    {
        device->DestroyShader(m_BuildShader);
        m_BuildShader = {};
    }

    if (m_CullShader.IsValid())
    {
        device->DestroyShader(m_CullShader);
        m_CullShader = {};
    }

    if (m_BuildPipeline.IsValid())
    {
        device->DestroyComputePipeline(m_BuildPipeline);
        m_BuildPipeline = {};
    }

    if (m_CullPipeline.IsValid())
    {
        device->DestroyComputePipeline(m_CullPipeline);
        m_CullPipeline = {};
    }

    if (m_BuildPipelineLayout.IsValid())
    {
        device->DestroyPipelineLayout(m_BuildPipelineLayout);
        m_BuildPipelineLayout = {};
    }

    if (m_CullPipelineLayout.IsValid())
    {
        device->DestroyPipelineLayout(m_CullPipelineLayout);
        m_CullPipelineLayout = {};
    }

    if (m_BuildSetLayout.IsValid())
    {
        device->DestroyDescSetLayout(m_BuildSetLayout);
        m_BuildSetLayout = {};
    }

    if (m_CullSetLayout.IsValid())
    {
        device->DestroyDescSetLayout(m_CullSetLayout);
        m_CullSetLayout = {};
    }

    m_BuildSets.Clear();
    m_CullSets.Clear();

    m_Device = nullptr;

    LIMX_LOG(LogRenderer, Log, "[ClusterLightPass] 已关闭");
}

// ============================================================================
// 访问器
// ============================================================================

FRHIBufferHandle FClusterLightPass::GetClusterGridBuffer(
    UInt32 frameIndex) const
{
    return (frameIndex < m_ClusterGrid.GetSize())
               ? m_ClusterGrid[frameIndex]
               : FRHIBufferHandle();
}

FRHIBufferHandle FClusterLightPass::GetLightIndexBuffer(
    UInt32 frameIndex) const
{
    return (frameIndex < m_LightIndices.GetSize())
               ? m_LightIndices[frameIndex]
               : FRHIBufferHandle();
}

FRHIBufferHandle FClusterLightPass::GetClusterBoundsBuffer(
    UInt32 frameIndex) const
{
    return (frameIndex < m_ClusterBounds.GetSize())
               ? m_ClusterBounds[frameIndex]
               : FRHIBufferHandle();
}

} // namespace Limx
