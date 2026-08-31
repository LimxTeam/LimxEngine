/*******************************************************************************
 * 文件: FTaaPass.cpp
 * 创建时间: 2026-08-31
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   时域抗锯齿解析通道的实现
 *
 * 依赖关系:
 *   内部: Renderer/RenderPass/FTaaPass.h, RenderCore/Shaders
 *
 ******************************************************************************/

#include "Renderer/RenderPass/FTaaPass.h"

#include "RenderCore/Shaders/FShaderManager.h"
#include "Core/Logging/FLog.h"

namespace Limx
{

LIMX_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{

/// 与前向通道的 HDR 目标同格式 —— TAA 在线性 HDR 空间里累积
///
/// 8 位是不行的: 混合系数 0.1 意味着单帧的贡献只有 1/10, 在 8 位上那是
/// 25.5 个量化档里的 2.55 —— 累积几帧之后增量小于一个量化档, 历史就再也
/// 不动了。表现是 TAA "收敛"到一个略微偏暗的值然后卡住。
constexpr EPixelFormat kTaaFormat = EPixelFormat::RGBA16_SFLOAT;

/// taa.frag 的 push constant 布局
struct FTaaPushConstant
{
    Float32 BlendFactor = 0.1f;
    Float32 ClipGamma   = 1.0f;
    Float32 HasHistory  = 0.0f;
    Float32 BlendPad    = 0.0f;

    Float32 ScreenW     = 0.0f;
    Float32 ScreenH     = 0.0f;
    Float32 InvScreenW  = 0.0f;
    Float32 InvScreenH  = 0.0f;
};

static_assert(sizeof(FTaaPushConstant) == 32,
              "FTaaPushConstant 必须是 32 字节 — 与 taa.frag 的 "
              "push constant 块一致 (两个 vec4)");

} // namespace

// ============================================================================
// Setup
// ============================================================================

ERHIResult FTaaPass::Setup(const FPassSetupDesc& desc)
{
    m_Device     = desc.Device;
    m_Extent     = desc.SwapchainExtent;
    m_SourceView = desc.SharedColorTextureView;

    if (m_Device == nullptr)
    {
        return ERHIResult::ErrorInvalidParameter;
    }

    ERHIResult result = CreateTargets(m_Device, m_Extent);

    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[TaaPass] 目标纹理创建失败");
        return result;
    }

    result = CreateRenderPassAndFramebuffers(m_Device, m_Extent);

    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[TaaPass] 渲染通道创建失败");
        return result;
    }

    result = CreateDescriptors(m_Device);

    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[TaaPass] 描述符资源创建失败");
        return result;
    }

    result = CreatePipeline(m_Device);

    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[TaaPass] 管线创建失败");
        return result;
    }

    LIMX_LOG(LogRenderer, Log,
             "[TaaPass] 初始化完成 — {}x{} RGBA16_SFLOAT, "
             "解析目标 1 张 + 历史 2 张",
             m_Extent.Width, m_Extent.Height);

    return ERHIResult::Success;
}

// ============================================================================
// CreateTargets
// ============================================================================

ERHIResult FTaaPass::CreateTargets(IRHIDevice* device, FRHIExtent2D extent)
{
    struct FTargetSpec
    {
        FRHITextureHandle*     Texture;
        FRHITextureViewHandle* View;
        const char*            DebugName;
    };

    const FTargetSpec specs[] =
    {
        { &m_ResolveTexture,    &m_ResolveView,    "TaaResolve"   },
        { &m_HistoryTexture[0], &m_HistoryView[0], "TaaHistory0"  },
        { &m_HistoryTexture[1], &m_HistoryView[1], "TaaHistory1"  },
    };

    for (SizeType i = 0; i < sizeof(specs) / sizeof(specs[0]); ++i)
    {
        FRHITextureDesc texDesc = {};
        texDesc.Type        = ETextureType::Texture2D;
        texDesc.Format      = kTaaFormat;
        texDesc.Extent      = { extent.Width, extent.Height, 1 };
        texDesc.MipLevels   = 1;
        texDesc.ArrayLayers = 1;
        texDesc.Samples     = ESampleCount::Count1;

        // 三张都既是颜色附件又要被采样: 解析目标给后处理采, 历史给下一帧采。
        // TransferSrc 是给 --taa-check 回读用的, 与 G-Buffer 同理常开 ——
        // 只在自检模式下加标志等于验证了一份与发布配置不同的资源。
        texDesc.Usage       = static_cast<ETextureUsage>(
            static_cast<UInt32>(ETextureUsage::ColorAttachment) |
            static_cast<UInt32>(ETextureUsage::Sampled) |
            static_cast<UInt32>(ETextureUsage::TransferSrc));

        texDesc.MemoryUsage = EMemoryUsage::GpuOnly;
        texDesc.DebugName   = specs[i].DebugName;

        ERHIResult result = device->CreateTexture(texDesc, *specs[i].Texture);

        if (!IsRHISuccess(result))
        {
            return result;
        }

        FRHITextureViewDesc viewDesc = {};
        viewDesc.Texture         = *specs[i].Texture;
        viewDesc.ViewType        = ETextureType::Texture2D;
        viewDesc.Format          = kTaaFormat;
        viewDesc.BaseMipLevel    = 0;
        viewDesc.MipLevelCount   = 1;
        viewDesc.BaseArrayLayer  = 0;
        viewDesc.ArrayLayerCount = 1;

        result = device->CreateTextureView(viewDesc, *specs[i].View);

        if (!IsRHISuccess(result))
        {
            return result;
        }
    }

    return ERHIResult::Success;
}

void FTaaPass::DestroyTargets(IRHIDevice* device)
{
    FRHITextureViewHandle* const views[] =
    {
        &m_ResolveView, &m_HistoryView[0], &m_HistoryView[1],
    };

    FRHITextureHandle* const textures[] =
    {
        &m_ResolveTexture, &m_HistoryTexture[0], &m_HistoryTexture[1],
    };

    for (SizeType i = 0; i < 3; ++i)
    {
        if (views[i]->IsValid())
        {
            device->DestroyTextureView(*views[i]);
            *views[i] = {};
        }

        if (textures[i]->IsValid())
        {
            device->DestroyTexture(*textures[i]);
            *textures[i] = {};
        }
    }
}

// ============================================================================
// CreateRenderPassAndFramebuffers
// ============================================================================

ERHIResult FTaaPass::CreateRenderPassAndFramebuffers(IRHIDevice* device,
                                                     FRHIExtent2D extent)
{
    // 两个颜色附件: [0] 解析目标, [1] 本帧的历史。
    //
    // 两者内容相同, 但用途与生命周期不同: 解析目标是固定的一张 (后处理的
    // 描述符指着它), 历史是乒乓的两张之一 (下一帧读另一张)。
    FRHIAttachmentDesc attachments[2] = {};

    for (UInt32 i = 0; i < 2; ++i)
    {
        attachments[i].Format         = kTaaFormat;
        attachments[i].Samples        = ESampleCount::Count1;

        // 全屏三角形覆盖每一个像素, 清除是纯粹的浪费
        attachments[i].LoadOp         = ELoadOp::DontCare;
        attachments[i].StoreOp        = EStoreOp::Store;
        attachments[i].StencilLoadOp  = ELoadOp::DontCare;
        attachments[i].StencilStoreOp = EStoreOp::DontCare;
        attachments[i].InitialLayout  = EImageLayout::Undefined;
        attachments[i].FinalLayout    = EImageLayout::ShaderReadOnly;
    }

    FRHIAttachmentReference colorRefs[2] = {};

    for (UInt32 i = 0; i < 2; ++i)
    {
        colorRefs[i].AttachmentIndex = i;
        colorRefs[i].Layout          = EImageLayout::ColorAttachment;
    }

    FRHISubpassDesc subpass = {};
    subpass.ColorAttachments       = colorRefs;
    subpass.ColorAttachmentCount   = 2;
    subpass.DepthStencilAttachment = nullptr;

    // 进入本通道前, 前向通道对 HDR 纹理与深度预通道对速度缓冲的写入都必须
    // 已完成。两者都是颜色附件写入, 一条依赖覆盖。
    FRHISubpassDependency dependency = {};
    dependency.SrcSubpass    = 0xFFFFFFFF;
    dependency.DstSubpass    = 0;
    dependency.SrcStageMask  = EPipelineStageFlags::ColorAttachmentOutput;
    dependency.DstStageMask  = EPipelineStageFlags::FragmentShader;
    dependency.SrcAccessMask = EAccessFlags::ColorAttachmentWrite;
    dependency.DstAccessMask = EAccessFlags::ShaderRead;

    FRHIRenderPassDesc renderPassDesc = {};
    renderPassDesc.Attachments     = attachments;
    renderPassDesc.AttachmentCount = 2;
    renderPassDesc.Subpasses       = &subpass;
    renderPassDesc.SubpassCount    = 1;
    renderPassDesc.Dependencies    = &dependency;
    renderPassDesc.DependencyCount = 1;
    renderPassDesc.DebugName       = "TaaPass_RenderPass";

    const ERHIResult result =
        device->CreateRenderPass(renderPassDesc, m_RenderPass);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    // 两个 framebuffer, 按历史的奇偶。附件 [1] 交替指向两张历史之一。
    for (UInt32 parity = 0; parity < 2; ++parity)
    {
        const FRHITextureViewHandle views[2] =
        {
            m_ResolveView,
            m_HistoryView[parity],
        };

        FRHIFramebufferDesc fbDesc = {};
        fbDesc.RenderPass      = m_RenderPass;
        fbDesc.Attachments     = views;
        fbDesc.AttachmentCount = 2;
        fbDesc.Width           = extent.Width;
        fbDesc.Height          = extent.Height;
        fbDesc.Layers          = 1;
        fbDesc.DebugName       = "TaaPass_Framebuffer";

        const ERHIResult fbResult =
            device->CreateFramebuffer(fbDesc, m_Framebuffer[parity]);

        if (!IsRHISuccess(fbResult))
        {
            return fbResult;
        }
    }

    return ERHIResult::Success;
}

void FTaaPass::DestroyFramebuffers(IRHIDevice* device)
{
    for (UInt32 i = 0; i < 2; ++i)
    {
        if (m_Framebuffer[i].IsValid())
        {
            device->DestroyFramebuffer(m_Framebuffer[i]);
            m_Framebuffer[i] = {};
        }
    }
}

// ============================================================================
// CreateDescriptors
// ============================================================================

ERHIResult FTaaPass::CreateDescriptors(IRHIDevice* device)
{
    FRHIDescriptorBinding bindings[3] = {};

    for (UInt32 i = 0; i < 3; ++i)
    {
        bindings[i].Binding    = i;
        bindings[i].Type       = EDescriptorType::CombinedImageSampler;
        bindings[i].Count      = 1;
        bindings[i].StageFlags = EShaderStage::Fragment;
    }

    FRHIDescSetLayoutDesc layoutDesc = {};
    layoutDesc.Bindings     = bindings;
    layoutDesc.BindingCount = 3;
    layoutDesc.DebugName    = "TaaDescSetLayout";

    ERHIResult result = device->CreateDescSetLayout(layoutDesc,
                                                    m_DescSetLayout);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    for (UInt32 i = 0; i < 2; ++i)
    {
        result = device->AllocateDescriptorSet(m_DescSetLayout,
                                               m_DescriptorSet[i]);

        if (!IsRHISuccess(result))
        {
            return result;
        }
    }

    // Clamp 而非 Repeat: 重投影到边缘外的采样若绕到对侧, 画面四周会留下一圈
    // 完全不相干的颜色 —— 而那看起来像是后处理的边缘问题。
    FRHISamplerDesc samplerDesc = {};
    samplerDesc.MinFilter    = EFilter::Linear;
    samplerDesc.MagFilter    = EFilter::Linear;
    samplerDesc.MipmapMode   = ESamplerMipmapMode::Nearest;
    samplerDesc.AddressModeU = ESamplerAddressMode::ClampToEdge;
    samplerDesc.AddressModeV = ESamplerAddressMode::ClampToEdge;
    samplerDesc.AddressModeW = ESamplerAddressMode::ClampToEdge;
    samplerDesc.IsAnisotropyEnabled = false;
    samplerDesc.MinLod = 0.0f;
    samplerDesc.MaxLod = 1.0f;

    result = device->CreateSampler(samplerDesc, m_Sampler);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    FRHIPushConstantRange pushRange = {};
    pushRange.StageFlags = EShaderStage::Fragment;
    pushRange.Offset     = 0;
    pushRange.Size       = sizeof(FTaaPushConstant);

    FRHIPipelineLayoutDesc pipelineLayoutDesc = {};
    pipelineLayoutDesc.SetLayouts             = &m_DescSetLayout;
    pipelineLayoutDesc.SetLayoutCount         = 1;
    pipelineLayoutDesc.PushConstantRanges     = &pushRange;
    pipelineLayoutDesc.PushConstantRangeCount = 1;
    pipelineLayoutDesc.DebugName              = "TaaPipelineLayout";

    return device->CreatePipelineLayout(pipelineLayoutDesc, m_PipelineLayout);
}

// ============================================================================
// UpdateDescriptors
// ============================================================================

void FTaaPass::UpdateDescriptors(IRHIDevice* device)
{
    if (!m_SourceView.IsValid() || !m_VelocityView.IsValid())
    {
        return;
    }

    for (UInt32 parity = 0; parity < 2; ++parity)
    {
        // 写历史 parity 的那一帧, 读的是另一张
        const UInt32 readParity = 1u - parity;

        FRHIDescriptorWrite writes[3];

        writes[0] = FRHIDescriptorWrite::CombinedImageSampler(
            m_DescriptorSet[parity], 0, m_SourceView, m_Sampler,
            EImageLayout::ShaderReadOnly);

        writes[1] = FRHIDescriptorWrite::CombinedImageSampler(
            m_DescriptorSet[parity], 1, m_HistoryView[readParity], m_Sampler,
            EImageLayout::ShaderReadOnly);

        writes[2] = FRHIDescriptorWrite::CombinedImageSampler(
            m_DescriptorSet[parity], 2, m_VelocityView, m_Sampler,
            EImageLayout::ShaderReadOnly);

        device->UpdateDescriptorSets(writes, 3);
    }
}

void FTaaPass::SetVelocityView(FRHITextureViewHandle view)
{
    m_VelocityView = view;

    if (m_Device != nullptr)
    {
        UpdateDescriptors(m_Device);
    }
}

void FTaaPass::SetEnabled(bool enabled)
{
    if (m_Enabled != enabled)
    {
        // 关掉再开时历史已经过期若干帧 —— 与它混合会产生一次可见的鬼影。
        m_HasHistory = false;
    }

    m_Enabled = enabled;
}

// ============================================================================
// CreatePipeline
// ============================================================================

ERHIResult FTaaPass::CreatePipeline(IRHIDevice* device)
{
    FShaderManager& shaders = FShaderManager::Get();

    ERHIResult result = shaders.CreateShaderModule(
        device, FString("Builtin/fullscreen.vert"), EShaderStage::Vertex,
        m_VertShader);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    result = shaders.CreateShaderModule(
        device, FString("Builtin/taa.frag"), EShaderStage::Fragment,
        m_FragShader);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    FRHIGraphicsPipelineDesc pipelineDesc = {};

    pipelineDesc.ShaderStages[0].Shader     = m_VertShader;
    pipelineDesc.ShaderStages[0].Stage      = EShaderStage::Vertex;
    pipelineDesc.ShaderStages[0].EntryPoint = "main";

    pipelineDesc.ShaderStages[1].Shader     = m_FragShader;
    pipelineDesc.ShaderStages[1].Stage      = EShaderStage::Fragment;
    pipelineDesc.ShaderStages[1].EntryPoint = "main";

    pipelineDesc.ShaderStageCount = 2;

    pipelineDesc.VertexInput.Bindings       = nullptr;
    pipelineDesc.VertexInput.BindingCount   = 0;
    pipelineDesc.VertexInput.Attributes     = nullptr;
    pipelineDesc.VertexInput.AttributeCount = 0;

    pipelineDesc.InputAssembly.Topology = EPrimitiveTopology::TriangleList;
    pipelineDesc.InputAssembly.IsPrimitiveRestartEnabled = false;

    pipelineDesc.Rasterization.IsDepthClampEnabled        = false;
    pipelineDesc.Rasterization.IsRasterizerDiscardEnabled = false;
    pipelineDesc.Rasterization.PolygonMode = EPolygonMode::Fill;
    pipelineDesc.Rasterization.CullMode    = ECullMode::None;
    pipelineDesc.Rasterization.FrontFace   = EFrontFace::CounterClockwise;
    pipelineDesc.Rasterization.IsDepthBiasEnabled = false;
    pipelineDesc.Rasterization.LineWidth   = 1.0f;

    pipelineDesc.Multisample.RasterizationSamples   = ESampleCount::Count1;
    pipelineDesc.Multisample.IsSampleShadingEnabled = false;

    pipelineDesc.DepthStencil.IsDepthTestEnabled  = false;
    pipelineDesc.DepthStencil.IsDepthWriteEnabled = false;

    // 两个附件各要一份混合状态。数量与子通道的颜色附件数不一致时,
    // vkCreateGraphicsPipelines 直接失败 —— 这一条不会静默。
    FRHIColorBlendAttachmentDesc colorBlend[2] =
    {
        FRHIColorBlendAttachmentDesc::Opaque(),
        FRHIColorBlendAttachmentDesc::Opaque(),
    };

    pipelineDesc.ColorBlend.Attachments      = colorBlend;
    pipelineDesc.ColorBlend.AttachmentCount  = 2;
    pipelineDesc.ColorBlend.IsLogicOpEnabled = false;

    pipelineDesc.DynamicState.EnabledStates =
        EDynamicState::Viewport | EDynamicState::Scissor;

    pipelineDesc.PipelineLayout = m_PipelineLayout;
    pipelineDesc.RenderPass     = m_RenderPass;
    pipelineDesc.SubpassIndex   = 0;
    pipelineDesc.DebugName      = "TaaPass_Pipeline";

    return device->CreateGraphicsPipeline(pipelineDesc, m_Pipeline);
}

// ============================================================================
// Execute
// ============================================================================

void FTaaPass::Execute(IRHICommandBuffer*        commandBuffer,
                       const FRenderPassContext& context)
{
    if (!m_Enabled || commandBuffer == nullptr)
    {
        return;
    }

    if (!m_VelocityView.IsValid())
    {
        LIMX_LOG(LogRenderer, Error,
                 "[TaaPass] 速度缓冲未接入 — 本帧跳过");
        return;
    }

    commandBuffer->BeginDebugLabel("TaaPass", 0.3f, 0.7f, 1.0f);

    // 没有历史时, 两张历史纹理可能还停在 Undefined 布局 (刚创建, 或刚
    // resize 重建)。着色器虽然会跳过对它们的采样, 但 Vulkan 要求**描述符
    // 指向的图像**在绘制时处于描述符写入时声明的布局 —— 验证层无法证明那个
    // 动态分支不会走到, 于是照报。
    //
    // 从 Undefined 转换会丢弃内容, 而这里正需要丢弃。
    if (!m_HasHistory)
    {
        for (UInt32 i = 0; i < 2; ++i)
        {
            commandBuffer->TransitionImageLayout(
                m_HistoryTexture[i],
                EImageLayout::Undefined,
                EImageLayout::ShaderReadOnly,
                EPipelineStageFlags::TopOfPipe,
                EPipelineStageFlags::FragmentShader,
                EAccessFlags::None,
                EAccessFlags::ShaderRead);
        }
    }

    const UInt32 parity = m_HistoryParity;

    FRHIRenderPassBeginInfo beginInfo = {};
    beginInfo.RenderPass        = m_RenderPass;
    beginInfo.Framebuffer       = m_Framebuffer[parity];
    beginInfo.RenderAreaOffset  = { 0, 0 };
    beginInfo.RenderAreaExtent  = context.SwapchainExtent;
    beginInfo.ClearColors       = nullptr;
    beginInfo.ClearColorCount   = 0;
    beginInfo.ClearDepthStencil = nullptr;

    commandBuffer->BeginRenderPass(beginInfo);

    commandBuffer->BindGraphicsPipeline(m_Pipeline);

    FRHIViewport viewport = {};
    viewport.X        = 0.0f;
    viewport.Y        = 0.0f;
    viewport.Width    = static_cast<Float32>(context.SwapchainExtent.Width);
    viewport.Height   = static_cast<Float32>(context.SwapchainExtent.Height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    commandBuffer->SetViewport(viewport);

    FRHIScissorRect scissor = {};
    scissor.X      = 0;
    scissor.Y      = 0;
    scissor.Width  = context.SwapchainExtent.Width;
    scissor.Height = context.SwapchainExtent.Height;

    commandBuffer->SetScissor(scissor);

    commandBuffer->BindDescriptorSet(EPipelineBindPoint::Graphics,
                                     m_PipelineLayout, 0,
                                     m_DescriptorSet[parity], nullptr, 0);

    FTaaPushConstant pushData;
    pushData.BlendFactor = m_BlendFactor;
    pushData.ClipGamma   = m_ClipGamma;
    pushData.HasHistory  = m_HasHistory ? 1.0f : 0.0f;

    pushData.ScreenW = static_cast<Float32>(context.SwapchainExtent.Width);
    pushData.ScreenH = static_cast<Float32>(context.SwapchainExtent.Height);
    pushData.InvScreenW = 1.0f / FMath::Max(pushData.ScreenW, 1.0f);
    pushData.InvScreenH = 1.0f / FMath::Max(pushData.ScreenH, 1.0f);

    commandBuffer->PushConstants(m_PipelineLayout, EShaderStage::Fragment, 0,
                                 sizeof(FTaaPushConstant), &pushData);

    commandBuffer->Draw(3, 1, 0, 0);

    commandBuffer->EndRenderPass();

    commandBuffer->EndDebugLabel();

    // 下一帧写另一张历史, 读这一张
    m_HistoryParity = 1u - parity;
    m_HasHistory    = true;
}

// ============================================================================
// OnResize
// ============================================================================

ERHIResult FTaaPass::OnResize(const FPassResizeDesc& desc)
{
    m_Extent     = desc.Extent;
    m_SourceView = desc.SharedColorView;

    DestroyFramebuffers(desc.Device);
    DestroyTargets(desc.Device);

    ERHIResult result = CreateTargets(desc.Device, m_Extent);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    // 渲染通道本身与尺寸无关, 只需重建 framebuffer
    for (UInt32 parity = 0; parity < 2; ++parity)
    {
        const FRHITextureViewHandle views[2] =
        {
            m_ResolveView,
            m_HistoryView[parity],
        };

        FRHIFramebufferDesc fbDesc = {};
        fbDesc.RenderPass      = m_RenderPass;
        fbDesc.Attachments     = views;
        fbDesc.AttachmentCount = 2;
        fbDesc.Width           = m_Extent.Width;
        fbDesc.Height          = m_Extent.Height;
        fbDesc.Layers          = 1;
        fbDesc.DebugName       = "TaaPass_Framebuffer";

        result = desc.Device->CreateFramebuffer(fbDesc,
                                               m_Framebuffer[parity]);

        if (!IsRHISuccess(result))
        {
            return result;
        }
    }

    UpdateDescriptors(desc.Device);

    // 尺寸变了, 历史里的内容与新的像素网格毫无对应关系
    m_HasHistory = false;

    return ERHIResult::Success;
}

void FTaaPass::ReleaseSwapchainResources(IRHIDevice* device)
{
    // 本通道的目标都是自有纹理, 不引用交换链图像
    (void)device;
}

// ============================================================================
// Shutdown
// ============================================================================

void FTaaPass::Shutdown(IRHIDevice* device)
{
    if (device == nullptr)
    {
        return;
    }

    DestroyFramebuffers(device);
    DestroyTargets(device);

    if (m_Pipeline.IsValid())
    {
        device->DestroyGraphicsPipeline(m_Pipeline);
        m_Pipeline = {};
    }

    if (m_PipelineLayout.IsValid())
    {
        device->DestroyPipelineLayout(m_PipelineLayout);
        m_PipelineLayout = {};
    }

    if (m_DescSetLayout.IsValid())
    {
        device->DestroyDescSetLayout(m_DescSetLayout);
        m_DescSetLayout = {};
    }

    if (m_Sampler.IsValid())
    {
        device->DestroySampler(m_Sampler);
        m_Sampler = {};
    }

    if (m_RenderPass.IsValid())
    {
        device->DestroyRenderPass(m_RenderPass);
        m_RenderPass = {};
    }

    if (m_VertShader.IsValid())
    {
        device->DestroyShader(m_VertShader);
        m_VertShader = {};
    }

    if (m_FragShader.IsValid())
    {
        device->DestroyShader(m_FragShader);
        m_FragShader = {};
    }

    m_Device = nullptr;

    LIMX_LOG(LogRenderer, Log, "[TaaPass] 已关闭");
}

} // namespace Limx
