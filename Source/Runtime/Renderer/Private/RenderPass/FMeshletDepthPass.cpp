/*******************************************************************************
 * 文件: FMeshletDepthPass.cpp
 * 创建时间: 2026-09-02
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   meshlet 深度光栅化 — 网格着色器路径与计算展开回退路径。
 *
 ******************************************************************************/

#include "Renderer/RendererMinimal.h"

#include "Renderer/RenderPass/FMeshletDepthPass.h"
#include "Renderer/RenderPass/FMeshletCullPass.h"
#include "Renderer/Renderer/FRenderer.h"

#include "RenderCore/Geometry/FMeshletBuilder.h"
#include "RenderCore/Shaders/FShaderManager.h"

namespace Limx
{

LIMX_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{

/// 与 meshlet_depth.mesh / meshlet_depth_fallback.vert 的 push constant 一致
struct FMeshletRasterPushConstants
{
    Float32 ViewProj[16] = {};

    /// x = 可见 meshlet 数, y/z/w = 保留
    UInt32 Params[4] = { 0, 0, 0, 0 };
};

static_assert(sizeof(FMeshletRasterPushConstants) == 80,
              "FMeshletRasterPushConstants 必须是 80 字节 — 与两个光栅化"
              "着色器的 push constant 块逐字段一致");

/// 与 meshlet_expand.comp 的 push constant 一致
struct FMeshletExpandPushConstants
{
    /// x = 可见 meshlet 数, y = 顶点流容量, z/w = 保留
    UInt32 Params[4] = { 0, 0, 0, 0 };
};

/// 与 VkDrawIndirectCommand 逐字段一致
struct FDrawIndirectCommand
{
    UInt32 VertexCount = 0;
    UInt32 InstanceCount = 1;
    UInt32 FirstVertex = 0;
    UInt32 FirstInstance = 0;
};

constexpr UInt64 kExpandedBytes =
    static_cast<UInt64>(sizeof(UInt32)) * 2 * kMaxExpandedVertices;

constexpr UInt64 kDrawArgsBytes = sizeof(FDrawIndirectCommand);

} // namespace

// ============================================================================
// Setup
// ============================================================================

ERHIResult FMeshletDepthPass::Setup(const FPassSetupDesc& desc)
{
    LIMX_CHECK(desc.Device != nullptr);

    m_Device     = desc.Device;
    m_FrameCount = desc.MaxFramesInFlight;
    m_Extent     = desc.SwapchainExtent;

    m_MeshShaderAvailable = desc.Device->IsMeshShaderSupported();

    // 设备不支持时**默认就是回退路径**, 而不是"默认网格着色器然后运行时
    // 悄悄退回"。后者会让日志里写着"网格着色器路径"而实际跑的是别的东西。
    if (!m_MeshShaderAvailable)
    {
        m_Mode = EMode::Fallback;
    }

    ERHIResult result = CreateDepthTarget(desc.Device, desc.SwapchainExtent);

    if (IsRHISuccess(result))
    {
        result = CreateRenderPass(desc.Device);
    }

    if (IsRHISuccess(result))
    {
        // 展开顶点流与间接绘制参数 —— 逐并行帧一套
        for (UInt32 i = 0; i < m_FrameCount; ++i)
        {
            FRHIBufferDesc bufferDesc = {};
            bufferDesc.Size        = kExpandedBytes;
            bufferDesc.Usage       = EBufferUsage::StorageBuffer;
            bufferDesc.MemoryUsage = EMemoryUsage::GpuOnly;
            bufferDesc.DebugName   = "MeshletExpandedVertices";

            FRHIBufferHandle expanded;

            result = desc.Device->CreateBuffer(bufferDesc, expanded);

            if (!IsRHISuccess(result))
            {
                return result;
            }

            m_ExpandedBuffers.Add(expanded);

            bufferDesc.Size  = kDrawArgsBytes;
            bufferDesc.Usage = static_cast<EBufferUsage>(
                static_cast<UInt32>(EBufferUsage::StorageBuffer) |
                static_cast<UInt32>(EBufferUsage::IndirectBuffer) |
                static_cast<UInt32>(EBufferUsage::TransferDst));
            bufferDesc.DebugName = "MeshletExpandDrawArgs";

            FRHIBufferHandle drawArgs;

            result = desc.Device->CreateBuffer(bufferDesc, drawArgs);

            if (!IsRHISuccess(result))
            {
                return result;
            }

            m_DrawArgsBuffers.Add(drawArgs);
        }
    }

    // 间接绘制参数的归零源
    //
    // vertexCount 归零 (计算着色器原子累加), instanceCount 必须是 1 ——
    // 全零的话一个实例都不画, 而那与"什么都不可见"分不开。
    if (IsRHISuccess(result))
    {
        FRHIBufferDesc bufferDesc = {};
        bufferDesc.Size        = kDrawArgsBytes;
        bufferDesc.Usage       = EBufferUsage::TransferSrc;
        bufferDesc.MemoryUsage = EMemoryUsage::CpuToGpu;
        bufferDesc.DebugName   = "MeshletExpandReset";

        result = desc.Device->CreateBuffer(bufferDesc, m_ResetSource);

        if (IsRHISuccess(result))
        {
            void* mapped = nullptr;

            if (IsRHISuccess(desc.Device->MapBuffer(m_ResetSource, &mapped)) &&
                mapped != nullptr)
            {
                auto* command = static_cast<FDrawIndirectCommand*>(mapped);

                command->VertexCount   = 0;
                command->InstanceCount = 1;
                command->FirstVertex   = 0;
                command->FirstInstance = 0;

                desc.Device->UnmapBuffer(m_ResetSource);
            }
        }
    }

    if (IsRHISuccess(result))
    {
        result = CreateDescriptors(desc.Device, m_FrameCount);
    }

    if (IsRHISuccess(result))
    {
        result = CreatePipelines(desc.Device);
    }

    if (!IsRHISuccess(result))
    {
        return result;
    }

    LIMX_LOG(LogRenderer, Log,
             "[MeshletDepth] 初始化完成 — 网格着色器 {}, 默认路径 {}",
             m_MeshShaderAvailable ? "可用" : "**不可用**",
             (m_Mode == EMode::MeshShader) ? "网格着色器" : "计算展开回退");

    return ERHIResult::Success;
}

// ============================================================================
// SetMode
// ============================================================================

bool FMeshletDepthPass::SetMode(EMode mode)
{
    if (mode == EMode::MeshShader && !m_MeshShaderAvailable)
    {
        LIMX_LOG(LogRenderer, Error,
                 "[MeshletDepth] 请求网格着色器路径, 但设备不支持 — 保持原样");
        return false;
    }

    m_Mode = mode;

    return true;
}

// ============================================================================
// 深度目标
// ============================================================================

ERHIResult FMeshletDepthPass::CreateDepthTarget(IRHIDevice* device,
                                                FRHIExtent2D extent)
{
    FRHITextureDesc texDesc = {};
    texDesc.Type        = ETextureType::Texture2D;
    texDesc.Format      = EPixelFormat::D32_SFLOAT;
    texDesc.Extent      = { extent.Width, extent.Height, 1 };
    texDesc.MipLevels   = 1;
    texDesc.ArrayLayers = 1;
    texDesc.Samples     = ESampleCount::Count1;
    texDesc.Usage       = static_cast<ETextureUsage>(
        static_cast<UInt32>(ETextureUsage::DepthStencilAttachment) |
        static_cast<UInt32>(ETextureUsage::Sampled) |
        static_cast<UInt32>(ETextureUsage::TransferSrc));
    texDesc.MemoryUsage = EMemoryUsage::GpuOnly;
    texDesc.DebugName   = "MeshletDepth";

    ERHIResult result = device->CreateTexture(texDesc, m_DepthTexture);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    FRHITextureViewDesc viewDesc = {};
    viewDesc.Texture         = m_DepthTexture;
    viewDesc.ViewType        = ETextureType::Texture2D;
    viewDesc.Format          = EPixelFormat::D32_SFLOAT;
    viewDesc.BaseMipLevel    = 0;
    viewDesc.MipLevelCount   = 1;
    viewDesc.BaseArrayLayer  = 0;
    viewDesc.ArrayLayerCount = 1;

    m_Extent = extent;

    return device->CreateTextureView(viewDesc, m_DepthView);
}

void FMeshletDepthPass::DestroyDepthTarget(IRHIDevice* device)
{
    if (m_Framebuffer.IsValid())
    {
        device->DestroyFramebuffer(m_Framebuffer);
        m_Framebuffer = FRHIFramebufferHandle();
    }

    if (m_DepthView.IsValid())
    {
        device->DestroyTextureView(m_DepthView);
        m_DepthView = FRHITextureViewHandle();
    }

    if (m_DepthTexture.IsValid())
    {
        device->DestroyTexture(m_DepthTexture);
        m_DepthTexture = FRHITextureHandle();
    }
}

// ============================================================================
// 渲染通道 + 帧缓冲
// ============================================================================

ERHIResult FMeshletDepthPass::CreateRenderPass(IRHIDevice* device)
{
    FRHIAttachmentDesc attachment = {};
    attachment.Format         = EPixelFormat::D32_SFLOAT;
    attachment.Samples        = ESampleCount::Count1;
    attachment.LoadOp         = ELoadOp::Clear;
    attachment.StoreOp        = EStoreOp::Store;
    attachment.StencilLoadOp  = ELoadOp::DontCare;
    attachment.StencilStoreOp = EStoreOp::DontCare;
    attachment.InitialLayout  = EImageLayout::Undefined;
    attachment.FinalLayout    = EImageLayout::DepthStencilAttachment;

    FRHIAttachmentReference depthRef = {};
    depthRef.AttachmentIndex = 0;
    depthRef.Layout          = EImageLayout::DepthStencilAttachment;

    FRHISubpassDesc subpass = {};
    subpass.ColorAttachmentCount   = 0;
    subpass.DepthStencilAttachment = &depthRef;

    FRHISubpassDependency dependency = {};
    dependency.SrcSubpass    = 0xFFFFFFFF;
    dependency.DstSubpass    = 0;
    dependency.SrcStageMask  = EPipelineStageFlags::TopOfPipe;
    dependency.DstStageMask  = EPipelineStageFlags::EarlyFragmentTests;
    dependency.SrcAccessMask = EAccessFlags::None;
    dependency.DstAccessMask = EAccessFlags::DepthStencilAttachmentWrite;

    FRHIRenderPassDesc renderPassDesc = {};
    renderPassDesc.Attachments     = &attachment;
    renderPassDesc.AttachmentCount = 1;
    renderPassDesc.Subpasses       = &subpass;
    renderPassDesc.SubpassCount    = 1;
    renderPassDesc.Dependencies    = &dependency;
    renderPassDesc.DependencyCount = 1;
    renderPassDesc.DebugName       = "MeshletDepth_RenderPass";

    ERHIResult result = device->CreateRenderPass(renderPassDesc, m_RenderPass);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    FRHIFramebufferDesc fbDesc = {};
    fbDesc.RenderPass      = m_RenderPass;
    fbDesc.Attachments     = &m_DepthView;
    fbDesc.AttachmentCount = 1;
    fbDesc.Width           = m_Extent.Width;
    fbDesc.Height          = m_Extent.Height;
    fbDesc.Layers          = 1;
    fbDesc.DebugName       = "MeshletDepth_Framebuffer";

    return device->CreateFramebuffer(fbDesc, m_Framebuffer);
}

// ============================================================================
// 描述符
// ============================================================================

ERHIResult FMeshletDepthPass::CreateDescriptors(IRHIDevice* device,
                                                UInt32 frameCount)
{
    const auto MakeLayout = [device](UInt32 count, EShaderStage stages,
                                     const AnsiChar* name,
                                     FRHIDescSetLayoutHandle& out)
    {
        FRHIDescriptorBinding bindings[7] = {};

        for (UInt32 i = 0; i < count; ++i)
        {
            bindings[i].Binding    = i;
            bindings[i].Type       = EDescriptorType::StorageBuffer;
            bindings[i].Count      = 1;
            bindings[i].StageFlags = stages;
        }

        FRHIDescSetLayoutDesc layoutDesc = {};
        layoutDesc.Bindings     = bindings;
        layoutDesc.BindingCount = count;
        layoutDesc.DebugName    = name;

        return device->CreateDescSetLayout(layoutDesc, out);
    };

    ERHIResult result = MakeLayout(6, EShaderStage::Mesh,
                                   "MeshletDepthMeshSetLayout",
                                   m_MeshSetLayout);

    if (IsRHISuccess(result))
    {
        result = MakeLayout(7, EShaderStage::Compute,
                            "MeshletExpandSetLayout", m_ExpandSetLayout);
    }

    if (IsRHISuccess(result))
    {
        result = MakeLayout(3, EShaderStage::Vertex,
                            "MeshletFallbackSetLayout", m_FallbackSetLayout);
    }

    if (!IsRHISuccess(result))
    {
        return result;
    }

    for (UInt32 i = 0; i < frameCount; ++i)
    {
        FRHIDescriptorSetHandle meshSet;
        FRHIDescriptorSetHandle expandSet;
        FRHIDescriptorSetHandle fallbackSet;

        result = device->AllocateDescriptorSet(m_MeshSetLayout, meshSet);

        if (IsRHISuccess(result))
        {
            result =
                device->AllocateDescriptorSet(m_ExpandSetLayout, expandSet);
        }

        if (IsRHISuccess(result))
        {
            result = device->AllocateDescriptorSet(m_FallbackSetLayout,
                                                   fallbackSet);
        }

        if (!IsRHISuccess(result))
        {
            return result;
        }

        m_MeshSets.Add(meshSet);
        m_ExpandSets.Add(expandSet);
        m_FallbackSets.Add(fallbackSet);
    }

    return ERHIResult::Success;
}

// ============================================================================
// 管线
// ============================================================================

ERHIResult FMeshletDepthPass::CreatePipelines(IRHIDevice* device)
{
    FShaderManager& shaders = FShaderManager::Get();

    if (!shaders.IsInitialized())
    {
        shaders.Initialize();
    }

    ERHIResult result = shaders.CreateShaderModule(
        device, FString("Builtin/meshlet_depth.frag"), EShaderStage::Fragment,
        m_FragmentShader);

    if (IsRHISuccess(result))
    {
        result = shaders.CreateShaderModule(
            device, FString("Builtin/meshlet_expand.comp"),
            EShaderStage::Compute, m_ExpandShader);
    }

    if (IsRHISuccess(result))
    {
        result = shaders.CreateShaderModule(
            device, FString("Builtin/meshlet_depth_fallback.vert"),
            EShaderStage::Vertex, m_FallbackVertexShader);
    }

    // 网格着色器模块只在设备支持时创建 —— 不支持时创建会被驱动拒绝,
    // 而那条错误看起来像"着色器文件坏了"。
    if (IsRHISuccess(result) && m_MeshShaderAvailable)
    {
        result = shaders.CreateShaderModule(
            device, FString("Builtin/meshlet_depth.mesh"), EShaderStage::Mesh,
            m_MeshShader);
    }

    if (!IsRHISuccess(result))
    {
        return result;
    }

    // ---- 管线布局 ----
    const auto MakeLayout = [device](FRHIDescSetLayoutHandle setLayout,
                                     EShaderStage stages, UInt32 pushSize,
                                     const AnsiChar* name,
                                     FRHIPipelineLayoutHandle& out)
    {
        FRHIPushConstantRange pushRange = {};
        pushRange.StageFlags = stages;
        pushRange.Offset     = 0;
        pushRange.Size       = pushSize;

        FRHIPipelineLayoutDesc layoutDesc = {};
        layoutDesc.SetLayouts             = &setLayout;
        layoutDesc.SetLayoutCount         = 1;
        layoutDesc.PushConstantRanges     = &pushRange;
        layoutDesc.PushConstantRangeCount = 1;
        layoutDesc.DebugName              = name;

        return device->CreatePipelineLayout(layoutDesc, out);
    };

    result = MakeLayout(m_MeshSetLayout, EShaderStage::Mesh,
                        sizeof(FMeshletRasterPushConstants),
                        "MeshletDepthMeshLayout", m_MeshPipelineLayout);

    if (IsRHISuccess(result))
    {
        result = MakeLayout(m_ExpandSetLayout, EShaderStage::Compute,
                            sizeof(FMeshletExpandPushConstants),
                            "MeshletExpandLayout", m_ExpandPipelineLayout);
    }

    if (IsRHISuccess(result))
    {
        result = MakeLayout(m_FallbackSetLayout, EShaderStage::Vertex,
                            sizeof(FMeshletRasterPushConstants),
                            "MeshletFallbackLayout", m_FallbackPipelineLayout);
    }

    if (!IsRHISuccess(result))
    {
        return result;
    }

    // ---- 展开的计算管线 ----
    {
        FRHIComputePipelineDesc pipelineDesc = {};
        pipelineDesc.ComputeShader.Shader     = m_ExpandShader;
        pipelineDesc.ComputeShader.Stage      = EShaderStage::Compute;
        pipelineDesc.ComputeShader.EntryPoint = "main";
        pipelineDesc.PipelineLayout           = m_ExpandPipelineLayout;
        pipelineDesc.DebugName                = "MeshletExpandPipeline";

        result = device->CreateComputePipeline(pipelineDesc, m_ExpandPipeline);

        if (!IsRHISuccess(result))
        {
            return result;
        }
    }

    // ---- 图形管线 (两条路径共用同一份状态, 只有着色器阶段不同) ----
    //
    // 状态必须逐字相同 —— 剔除模式、深度比较、视口约定, 任何一处不同都会
    // 让两条路径画出不同的深度, 而判据要求它们逐位相同。
    const auto MakeGraphics = [&](FRHIShaderHandle firstStage,
                                  EShaderStage firstStageKind,
                                  FRHIPipelineLayoutHandle layout,
                                  const AnsiChar* name,
                                  FRHIGraphicsPipelineHandle& out)
    {
        FRHIGraphicsPipelineDesc pipelineDesc = {};

        pipelineDesc.ShaderStages[0].Shader     = firstStage;
        pipelineDesc.ShaderStages[0].Stage      = firstStageKind;
        pipelineDesc.ShaderStages[0].EntryPoint = "main";

        pipelineDesc.ShaderStages[1].Shader     = m_FragmentShader;
        pipelineDesc.ShaderStages[1].Stage      = EShaderStage::Fragment;
        pipelineDesc.ShaderStages[1].EntryPoint = "main";

        pipelineDesc.ShaderStageCount = 2;

        // 顶点输入为空。网格着色器路径下 Vulkan 直接忽略这一段; 回退路径
        // 的顶点数据来自 storage buffer, 也不经过顶点输入。
        pipelineDesc.VertexInput.BindingCount   = 0;
        pipelineDesc.VertexInput.AttributeCount = 0;

        pipelineDesc.InputAssembly.Topology = EPrimitiveTopology::TriangleList;

        pipelineDesc.Rasterization.PolygonMode = EPolygonMode::Fill;
        pipelineDesc.Rasterization.CullMode    = ECullMode::Back;
        pipelineDesc.Rasterization.FrontFace   = EFrontFace::CounterClockwise;
        pipelineDesc.Rasterization.LineWidth   = 1.0f;

        pipelineDesc.Multisample.RasterizationSamples = ESampleCount::Count1;

        pipelineDesc.DepthStencil.IsDepthTestEnabled  = true;
        pipelineDesc.DepthStencil.IsDepthWriteEnabled = true;
        pipelineDesc.DepthStencil.DepthCompareOp      = ECompareOp::Less;

        pipelineDesc.ColorBlend.AttachmentCount = 0;

        pipelineDesc.DynamicState.EnabledStates =
            EDynamicState::Viewport | EDynamicState::Scissor;

        pipelineDesc.PipelineLayout = layout;
        pipelineDesc.RenderPass     = m_RenderPass;
        pipelineDesc.SubpassIndex   = 0;
        pipelineDesc.DebugName      = name;

        return device->CreateGraphicsPipeline(pipelineDesc, out);
    };

    if (m_MeshShaderAvailable)
    {
        result = MakeGraphics(m_MeshShader, EShaderStage::Mesh,
                              m_MeshPipelineLayout, "MeshletDepthMeshPipeline",
                              m_MeshPipeline);

        if (!IsRHISuccess(result))
        {
            return result;
        }
    }

    return MakeGraphics(m_FallbackVertexShader, EShaderStage::Vertex,
                        m_FallbackPipelineLayout,
                        "MeshletDepthFallbackPipeline", m_FallbackPipeline);
}

// ============================================================================
// Execute
// ============================================================================

void FMeshletDepthPass::Execute(IRHICommandBuffer*        commandBuffer,
                                const FRenderPassContext& context)
{
    if (!m_Enabled || m_Device == nullptr)
    {
        return;
    }

    FMeshletCullPass* const cull = context.MeshletCull;

    if (cull == nullptr || !cull->IsEnabled())
    {
        return;
    }

    const UInt32 frameIndex = context.FrameIndex;

    const UInt32 instanceCount =
        static_cast<UInt32>(cull->GetInstances().GetSize());

    if (instanceCount == 0 || cull->GetSceneMeshletCount() == 0)
    {
        return;
    }

    commandBuffer->BeginDebugLabel("MeshletDepthPass", 0.5f, 0.9f, 0.5f);

    // ---- 描述符指向本帧的场景缓冲区 ----
    //
    // 场景缓冲区只在几何签名变化时重建, 而描述符要跟着走。每帧无条件重写
    // 也行 (几个 write 而已), 但那会掩盖"缓冲区换了而描述符没换"这类错误 ——
    // 现在只在真的换了时才写, 于是那件事有痕迹。
    if (m_BoundVertexBuffer != cull->GetSceneVertexBuffer())
    {
        for (UInt32 i = 0; i < m_FrameCount; ++i)
        {
            FRHIDescriptorWrite meshWrites[6];

            meshWrites[0] = FRHIDescriptorWrite::StorageBuffer(
                m_MeshSets[i], 0, cull->GetInstanceBuffer(i), 0,
                cull->GetInstanceBufferBytes());
            meshWrites[1] = FRHIDescriptorWrite::StorageBuffer(
                m_MeshSets[i], 1, cull->GetSceneMeshletBuffer(), 0,
                cull->GetSceneMeshletBytes());
            meshWrites[2] = FRHIDescriptorWrite::StorageBuffer(
                m_MeshSets[i], 2, cull->GetVisibleMeshletBuffer(i), 0,
                cull->GetVisibleMeshletBytes());
            meshWrites[3] = FRHIDescriptorWrite::StorageBuffer(
                m_MeshSets[i], 3, cull->GetSceneVertexBuffer(), 0,
                cull->GetSceneVertexBytes());
            meshWrites[4] = FRHIDescriptorWrite::StorageBuffer(
                m_MeshSets[i], 4, cull->GetSceneMeshletVertexBuffer(), 0,
                cull->GetSceneMeshletVertexBytes());
            meshWrites[5] = FRHIDescriptorWrite::StorageBuffer(
                m_MeshSets[i], 5, cull->GetSceneMeshletTriangleBuffer(), 0,
                cull->GetSceneMeshletTriangleBytes());

            m_Device->UpdateDescriptorSets(meshWrites, 6);

            FRHIDescriptorWrite expandWrites[7];

            expandWrites[0] = FRHIDescriptorWrite::StorageBuffer(
                m_ExpandSets[i], 0, cull->GetSceneMeshletBuffer(), 0,
                cull->GetSceneMeshletBytes());
            expandWrites[1] = FRHIDescriptorWrite::StorageBuffer(
                m_ExpandSets[i], 1, cull->GetVisibleMeshletBuffer(i), 0,
                cull->GetVisibleMeshletBytes());
            expandWrites[2] = FRHIDescriptorWrite::StorageBuffer(
                m_ExpandSets[i], 2, cull->GetSceneMeshletVertexBuffer(), 0,
                cull->GetSceneMeshletVertexBytes());
            expandWrites[3] = FRHIDescriptorWrite::StorageBuffer(
                m_ExpandSets[i], 3, cull->GetSceneMeshletTriangleBuffer(), 0,
                cull->GetSceneMeshletTriangleBytes());
            expandWrites[4] = FRHIDescriptorWrite::StorageBuffer(
                m_ExpandSets[i], 4, m_ExpandedBuffers[i], 0, kExpandedBytes);
            expandWrites[5] = FRHIDescriptorWrite::StorageBuffer(
                m_ExpandSets[i], 5, m_DrawArgsBuffers[i], 0, kDrawArgsBytes);
            expandWrites[6] = FRHIDescriptorWrite::StorageBuffer(
                m_ExpandSets[i], 6, cull->GetInstanceBuffer(i), 0,
                cull->GetInstanceBufferBytes());

            m_Device->UpdateDescriptorSets(expandWrites, 7);

            FRHIDescriptorWrite fallbackWrites[3];

            fallbackWrites[0] = FRHIDescriptorWrite::StorageBuffer(
                m_FallbackSets[i], 0, cull->GetInstanceBuffer(i), 0,
                cull->GetInstanceBufferBytes());
            fallbackWrites[1] = FRHIDescriptorWrite::StorageBuffer(
                m_FallbackSets[i], 1, cull->GetSceneVertexBuffer(), 0,
                cull->GetSceneVertexBytes());
            fallbackWrites[2] = FRHIDescriptorWrite::StorageBuffer(
                m_FallbackSets[i], 2, m_ExpandedBuffers[i], 0, kExpandedBytes);

            m_Device->UpdateDescriptorSets(fallbackWrites, 3);
        }

        m_BoundVertexBuffer = cull->GetSceneVertexBuffer();
    }

    m_DrawnMeshlets = cull->GetStats().MeshletsVisible;

    // 工作组数**来自 GPU** —— 剔除通道把可见数拷进了一份间接参数。
    //
    // 第一版拿"场景 meshlet 总数"当工作组数, 那是错的: 可见表里是
    // (实例, meshlet) 对, 同一个 meshlet 会因为多个实例出现多次, 于是
    // 表长远大于场景 meshlet 数。实测综合场景 14 个 meshlet 对应九十多条
    // 可见记录 —— 两条光栅化路径各画了任意的 14 条 (原子追加的顺序每帧
    // 都不同), 判据报出 28334 个像素不同。
    //
    // 用上一帧回读的数也不行: 物体一动那个数就不对, 多了就去读表里上一帧
    // 留下的记录, 那是一块位置完全不对的几何体。
    //
    // 着色器里那条越界判断留着, 参数给的是表的容量 —— 间接参数已经是准确
    // 的了, 那条判断防的是"间接参数本身错了"。
    const UInt32 boundsLimit = kMaxSceneMeshlets;

    const FRHIBufferHandle rasterArgs =
        cull->GetRasterArgsBuffer(frameIndex);

    if (!rasterArgs.IsValid())
    {
        commandBuffer->EndDebugLabel();
        return;
    }

    FMeshletRasterPushConstants rasterPush;

    // 视锥/视图矩阵与剔除用的是同一个 —— 剔除按 A 剔、光栅化按 B 画的话,
    // 画出来的东西会缺一块或多一块, 而两者都在"边界附近"。
    {
        const FMatrix viewProj =
            context.Camera != nullptr
                ? (context.Camera->GetProjectionMatrix() *
                   context.Camera->GetViewMatrix())
                : FMatrix::kIdentity;

        for (UInt32 row = 0; row < 4; ++row)
        {
            for (UInt32 col = 0; col < 4; ++col)
            {
                rasterPush.ViewProj[row * 4 + col] = viewProj.M[row][col];
            }
        }
    }

    rasterPush.Params[0] = boundsLimit;

    // ---- 回退路径: 先展开 ----
    if (m_Mode == EMode::Fallback)
    {
        FRHIBufferCopyRegion region = {};
        region.SrcOffset = 0;
        region.DstOffset = 0;
        region.Size      = kDrawArgsBytes;

        commandBuffer->CopyBuffer(m_ResetSource, m_DrawArgsBuffers[frameIndex],
                                  region);

        FRHIBufferMemoryBarrier barrier = {};
        barrier.SrcAccessMask = EAccessFlags::TransferWrite;
        barrier.DstAccessMask =
            EAccessFlags::ShaderRead | EAccessFlags::ShaderWrite;
        barrier.Buffer = m_DrawArgsBuffers[frameIndex];

        commandBuffer->PipelineBarrier(EPipelineStageFlags::Transfer,
                                       EPipelineStageFlags::ComputeShader,
                                       nullptr, 0, &barrier, 1, nullptr, 0);

        commandBuffer->BindComputePipeline(m_ExpandPipeline);
        commandBuffer->BindDescriptorSet(EPipelineBindPoint::Compute,
                                         m_ExpandPipelineLayout, 0,
                                         m_ExpandSets[frameIndex]);

        FMeshletExpandPushConstants expandPush;
        expandPush.Params[0] = boundsLimit;
        expandPush.Params[1] = kMaxExpandedVertices;

        commandBuffer->PushConstants(m_ExpandPipelineLayout,
                                     EShaderStage::Compute, 0,
                                     sizeof(expandPush), &expandPush);

        commandBuffer->DispatchIndirect(rasterArgs, 0);

        FRHIBufferMemoryBarrier after[2] = {};

        after[0].SrcAccessMask = EAccessFlags::ShaderWrite;
        after[0].DstAccessMask = EAccessFlags::ShaderRead;
        after[0].Buffer        = m_ExpandedBuffers[frameIndex];

        after[1].SrcAccessMask = EAccessFlags::ShaderWrite;
        after[1].DstAccessMask = EAccessFlags::IndirectCommandRead;
        after[1].Buffer        = m_DrawArgsBuffers[frameIndex];

        commandBuffer->PipelineBarrier(
            EPipelineStageFlags::ComputeShader,
            EPipelineStageFlags::VertexShader |
                EPipelineStageFlags::DrawIndirect,
            nullptr, 0, after, 2, nullptr, 0);
    }

    // ---- 光栅化 ----
    FRHIClearDepthStencilValue clearDepth = {};
    clearDepth.Depth   = 1.0f;
    clearDepth.Stencil = 0;

    FRHIRenderPassBeginInfo beginInfo = {};
    beginInfo.RenderPass        = m_RenderPass;
    beginInfo.Framebuffer       = m_Framebuffer;
    beginInfo.RenderAreaOffset  = { 0, 0 };
    beginInfo.RenderAreaExtent  = m_Extent;
    beginInfo.ClearColorCount   = 0;
    beginInfo.ClearDepthStencil = &clearDepth;

    commandBuffer->BeginRenderPass(beginInfo);

    FRHIViewport viewport = {};
    viewport.X        = 0.0f;
    viewport.Y        = 0.0f;
    viewport.Width    = static_cast<Float32>(m_Extent.Width);
    viewport.Height   = static_cast<Float32>(m_Extent.Height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    commandBuffer->SetViewport(viewport);

    FRHIScissorRect scissor = {};
    scissor.X      = 0;
    scissor.Y      = 0;
    scissor.Width  = m_Extent.Width;
    scissor.Height = m_Extent.Height;

    commandBuffer->SetScissor(scissor);

    if (m_Mode == EMode::MeshShader)
    {
        commandBuffer->BindGraphicsPipeline(m_MeshPipeline);
        commandBuffer->BindDescriptorSet(EPipelineBindPoint::Graphics,
                                         m_MeshPipelineLayout, 0,
                                         m_MeshSets[frameIndex]);

        commandBuffer->PushConstants(m_MeshPipelineLayout, EShaderStage::Mesh,
                                     0, sizeof(rasterPush), &rasterPush);

        commandBuffer->DrawMeshTasksIndirect(rasterArgs, 0, 1,
                                             sizeof(UInt32) * 4);
    }
    else
    {
        commandBuffer->BindGraphicsPipeline(m_FallbackPipeline);
        commandBuffer->BindDescriptorSet(EPipelineBindPoint::Graphics,
                                         m_FallbackPipelineLayout, 0,
                                         m_FallbackSets[frameIndex]);

        commandBuffer->PushConstants(m_FallbackPipelineLayout,
                                     EShaderStage::Vertex, 0,
                                     sizeof(rasterPush), &rasterPush);

        commandBuffer->DrawIndirect(m_DrawArgsBuffers[frameIndex], 0, 1,
                                    sizeof(FDrawIndirectCommand));
    }

    commandBuffer->EndRenderPass();

    commandBuffer->EndDebugLabel();
}

// ============================================================================
// 其余接口
// ============================================================================

ERHIResult FMeshletDepthPass::OnResize(const FPassResizeDesc& desc)
{
    if (desc.Device == nullptr)
    {
        return ERHIResult::Success;
    }

    DestroyDepthTarget(desc.Device);

    ERHIResult result = CreateDepthTarget(desc.Device, desc.Extent);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    FRHIFramebufferDesc fbDesc = {};
    fbDesc.RenderPass      = m_RenderPass;
    fbDesc.Attachments     = &m_DepthView;
    fbDesc.AttachmentCount = 1;
    fbDesc.Width           = m_Extent.Width;
    fbDesc.Height          = m_Extent.Height;
    fbDesc.Layers          = 1;
    fbDesc.DebugName       = "MeshletDepth_Framebuffer";

    return desc.Device->CreateFramebuffer(fbDesc, m_Framebuffer);
}

void FMeshletDepthPass::ReleaseSwapchainResources(IRHIDevice* device)
{
    if (device != nullptr)
    {
        DestroyDepthTarget(device);
    }
}

void FMeshletDepthPass::Shutdown(IRHIDevice* device)
{
    if (device == nullptr)
    {
        return;
    }

    DestroyDepthTarget(device);

    if (m_RenderPass.IsValid())
    {
        device->DestroyRenderPass(m_RenderPass);
    }

    if (m_MeshPipeline.IsValid())
    {
        device->DestroyGraphicsPipeline(m_MeshPipeline);
    }

    if (m_FallbackPipeline.IsValid())
    {
        device->DestroyGraphicsPipeline(m_FallbackPipeline);
    }

    if (m_ExpandPipeline.IsValid())
    {
        device->DestroyComputePipeline(m_ExpandPipeline);
    }

    const auto DestroyLayout = [device](FRHIPipelineLayoutHandle& handle)
    {
        if (handle.IsValid())
        {
            device->DestroyPipelineLayout(handle);
        }
    };

    DestroyLayout(m_MeshPipelineLayout);
    DestroyLayout(m_ExpandPipelineLayout);
    DestroyLayout(m_FallbackPipelineLayout);

    const auto DestroySetLayout = [device](FRHIDescSetLayoutHandle& handle)
    {
        if (handle.IsValid())
        {
            device->DestroyDescSetLayout(handle);
        }
    };

    DestroySetLayout(m_MeshSetLayout);
    DestroySetLayout(m_ExpandSetLayout);
    DestroySetLayout(m_FallbackSetLayout);

    const auto DestroyShader = [device](FRHIShaderHandle& handle)
    {
        if (handle.IsValid())
        {
            device->DestroyShader(handle);
        }
    };

    DestroyShader(m_MeshShader);
    DestroyShader(m_FragmentShader);
    DestroyShader(m_ExpandShader);
    DestroyShader(m_FallbackVertexShader);

    const auto DestroyBuffers = [device](TArray<FRHIBufferHandle>& buffers)
    {
        for (SizeType i = 0; i < buffers.GetSize(); ++i)
        {
            if (buffers[i].IsValid())
            {
                device->DestroyBuffer(buffers[i]);
            }
        }

        buffers.Clear();
    };

    DestroyBuffers(m_ExpandedBuffers);
    DestroyBuffers(m_DrawArgsBuffers);

    if (m_ResetSource.IsValid())
    {
        device->DestroyBuffer(m_ResetSource);
    }

    m_MeshSets.Clear();
    m_ExpandSets.Clear();
    m_FallbackSets.Clear();

    m_Device = nullptr;
}

} // namespace Limx
