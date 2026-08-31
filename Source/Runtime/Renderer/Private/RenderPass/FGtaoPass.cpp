/*******************************************************************************
 * 文件: FGtaoPass.cpp
 * 创建时间: 2026-08-31
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   GTAO 通道的实现
 *
 * 依赖关系:
 *   内部: Renderer/RenderPass/FGtaoPass.h, RenderCore/Shaders
 *
 ******************************************************************************/

#include "Renderer/RenderPass/FGtaoPass.h"

#include "RenderCore/Shaders/FShaderManager.h"
#include "Core/Logging/FLog.h"

namespace Limx
{

LIMX_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{

/// 单通道半精度。
///
/// R8_UNORM 也够用 (AO 只有 256 档人眼分不出), 但半精度让自检能验到更细的
/// 数值 —— 90 度凹角的解析值是 0.5, 而 8 位下 0.5 与 0.502 是同一个数。
constexpr EPixelFormat kAoFormat = EPixelFormat::R16_SFLOAT;

/// gtao.frag 的 push constant 布局
struct FGtaoPushConstant
{
    FMatrix InverseProjection;
    FMatrix View;

    Float32 Radius     = 0.8f;
    Float32 Intensity  = 1.0f;
    Float32 ScreenW    = 0.0f;
    Float32 ScreenH    = 0.0f;
};

static_assert(sizeof(FGtaoPushConstant) == 144,
              "FGtaoPushConstant 必须是 144 字节 — 与 gtao.frag 的 "
              "push constant 块一致 (两个 mat4 + 一个 vec4)");

} // namespace

// ============================================================================
// Setup
// ============================================================================

ERHIResult FGtaoPass::Setup(const FPassSetupDesc& desc)
{
    m_Device = desc.Device;
    m_Extent = desc.SwapchainExtent;

    if (m_Device == nullptr)
    {
        return ERHIResult::ErrorInvalidParameter;
    }

    ERHIResult result = CreateTarget(m_Device, m_Extent);

    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[GtaoPass] AO 目标创建失败");
        return result;
    }

    result = CreateRenderPassAndFramebuffer(m_Device, m_Extent);

    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[GtaoPass] 渲染通道创建失败");
        return result;
    }

    result = CreateDescriptors(m_Device);

    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[GtaoPass] 描述符资源创建失败");
        return result;
    }

    result = CreatePipeline(m_Device);

    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[GtaoPass] 管线创建失败");
        return result;
    }

    LIMX_LOG(LogRenderer, Log,
             "[GtaoPass] 初始化完成 — {}x{} R16_SFLOAT, 4 方向 x 8 步进",
             m_Extent.Width, m_Extent.Height);

    return ERHIResult::Success;
}

// ============================================================================
// CreateTarget
// ============================================================================

ERHIResult FGtaoPass::CreateTarget(IRHIDevice* device, FRHIExtent2D extent)
{
    FRHITextureDesc texDesc = {};
    texDesc.Type        = ETextureType::Texture2D;
    texDesc.Format      = kAoFormat;
    texDesc.Extent      = { extent.Width, extent.Height, 1 };
    texDesc.MipLevels   = 1;
    texDesc.ArrayLayers = 1;
    texDesc.Samples     = ESampleCount::Count1;

    texDesc.Usage       = static_cast<ETextureUsage>(
        static_cast<UInt32>(ETextureUsage::ColorAttachment) |
        static_cast<UInt32>(ETextureUsage::Sampled) |
        static_cast<UInt32>(ETextureUsage::TransferSrc));

    texDesc.MemoryUsage = EMemoryUsage::GpuOnly;
    texDesc.DebugName   = "GtaoAO";

    ERHIResult result = device->CreateTexture(texDesc, m_AoTexture);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    FRHITextureViewDesc viewDesc = {};
    viewDesc.Texture         = m_AoTexture;
    viewDesc.ViewType        = ETextureType::Texture2D;
    viewDesc.Format          = kAoFormat;
    viewDesc.BaseMipLevel    = 0;
    viewDesc.MipLevelCount   = 1;
    viewDesc.BaseArrayLayer  = 0;
    viewDesc.ArrayLayerCount = 1;

    return device->CreateTextureView(viewDesc, m_AoView);
}

// ============================================================================
// CreateRenderPassAndFramebuffer
// ============================================================================

ERHIResult FGtaoPass::CreateRenderPassAndFramebuffer(IRHIDevice* device,
                                                     FRHIExtent2D extent)
{
    FRHIAttachmentDesc attachment = {};
    attachment.Format         = kAoFormat;
    attachment.Samples        = ESampleCount::Count1;

    // 关闭时靠 Clear 把 AO 填成 1, 开启时全屏三角形覆盖每一个像素。
    // 两种情形都不需要读旧内容, 但清除值在关闭那条路径上要用到, 所以
    // 用 Clear 而不是 DontCare —— 多一次清除换掉一整条"关闭时怎么办"的分支。
    attachment.LoadOp         = ELoadOp::Clear;
    attachment.StoreOp        = EStoreOp::Store;
    attachment.StencilLoadOp  = ELoadOp::DontCare;
    attachment.StencilStoreOp = EStoreOp::DontCare;
    attachment.InitialLayout  = EImageLayout::Undefined;
    attachment.FinalLayout    = EImageLayout::ShaderReadOnly;

    FRHIAttachmentReference colorRef = {};
    colorRef.AttachmentIndex = 0;
    colorRef.Layout          = EImageLayout::ColorAttachment;

    FRHISubpassDesc subpass = {};
    subpass.ColorAttachments       = &colorRef;
    subpass.ColorAttachmentCount   = 1;
    subpass.DepthStencilAttachment = nullptr;

    // 进入本通道前, 深度预通道对深度与法线的写入必须已完成。
    // 深度是深度附件写入, 法线是颜色附件写入 —— 两个阶段都要覆盖。
    FRHISubpassDependency dependency = {};
    dependency.SrcSubpass    = 0xFFFFFFFF;
    dependency.DstSubpass    = 0;
    dependency.SrcStageMask  = EPipelineStageFlags::ColorAttachmentOutput |
                               EPipelineStageFlags::LateFragmentTests;
    dependency.DstStageMask  = EPipelineStageFlags::FragmentShader;
    dependency.SrcAccessMask = EAccessFlags::ColorAttachmentWrite |
                               EAccessFlags::DepthStencilAttachmentWrite;
    dependency.DstAccessMask = EAccessFlags::ShaderRead;

    FRHIRenderPassDesc renderPassDesc = {};
    renderPassDesc.Attachments     = &attachment;
    renderPassDesc.AttachmentCount = 1;
    renderPassDesc.Subpasses       = &subpass;
    renderPassDesc.SubpassCount    = 1;
    renderPassDesc.Dependencies    = &dependency;
    renderPassDesc.DependencyCount = 1;
    renderPassDesc.DebugName       = "GtaoPass_RenderPass";

    ERHIResult result =
        device->CreateRenderPass(renderPassDesc, m_RenderPass);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    FRHIFramebufferDesc fbDesc = {};
    fbDesc.RenderPass      = m_RenderPass;
    fbDesc.Attachments     = &m_AoView;
    fbDesc.AttachmentCount = 1;
    fbDesc.Width           = extent.Width;
    fbDesc.Height          = extent.Height;
    fbDesc.Layers          = 1;
    fbDesc.DebugName       = "GtaoPass_Framebuffer";

    return device->CreateFramebuffer(fbDesc, m_Framebuffer);
}

// ============================================================================
// CreateDescriptors
// ============================================================================

ERHIResult FGtaoPass::CreateDescriptors(IRHIDevice* device)
{
    FRHIDescriptorBinding bindings[2] = {};

    for (UInt32 i = 0; i < 2; ++i)
    {
        bindings[i].Binding    = i;
        bindings[i].Type       = EDescriptorType::CombinedImageSampler;
        bindings[i].Count      = 1;
        bindings[i].StageFlags = EShaderStage::Fragment;
    }

    FRHIDescSetLayoutDesc layoutDesc = {};
    layoutDesc.Bindings     = bindings;
    layoutDesc.BindingCount = 2;
    layoutDesc.DebugName    = "GtaoDescSetLayout";

    ERHIResult result = device->CreateDescSetLayout(layoutDesc,
                                                    m_DescSetLayout);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    result = device->AllocateDescriptorSet(m_DescSetLayout, m_DescriptorSet);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    // Nearest 而不是 Linear。
    //
    // 深度与八面体编码的法线**都不能线性插值**: 深度不连续处插值出的是一个
    // 根本不存在的表面; 八面体编码的两个分量在折叠边界两侧不连续, 插值会
    // 给出指向别处的法线。两者的后果都是物体边缘出现一圈错误的 AO。
    FRHISamplerDesc samplerDesc = {};
    samplerDesc.MinFilter    = EFilter::Nearest;
    samplerDesc.MagFilter    = EFilter::Nearest;
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
    pushRange.Size       = sizeof(FGtaoPushConstant);

    FRHIPipelineLayoutDesc pipelineLayoutDesc = {};
    pipelineLayoutDesc.SetLayouts             = &m_DescSetLayout;
    pipelineLayoutDesc.SetLayoutCount         = 1;
    pipelineLayoutDesc.PushConstantRanges     = &pushRange;
    pipelineLayoutDesc.PushConstantRangeCount = 1;
    pipelineLayoutDesc.DebugName              = "GtaoPipelineLayout";

    return device->CreatePipelineLayout(pipelineLayoutDesc, m_PipelineLayout);
}

void FGtaoPass::UpdateDescriptors(IRHIDevice* device)
{
    if (!m_DepthView.IsValid() || !m_NormalView.IsValid())
    {
        return;
    }

    FRHIDescriptorWrite writes[2];

    // 深度以 DepthReadOnly 布局被采样 —— 它同时还是别的通道的深度附件,
    // 用通用的 ShaderReadOnly 会让那些通道的布局转换对不上。
    writes[0] = FRHIDescriptorWrite::CombinedImageSampler(
        m_DescriptorSet, 0, m_DepthView, m_Sampler,
        EImageLayout::DepthStencilReadOnly);

    writes[1] = FRHIDescriptorWrite::CombinedImageSampler(
        m_DescriptorSet, 1, m_NormalView, m_Sampler,
        EImageLayout::ShaderReadOnly);

    device->UpdateDescriptorSets(writes, 2);
}

void FGtaoPass::SetInputs(FRHITextureHandle     depthTexture,
                          FRHITextureViewHandle depthView,
                          FRHITextureViewHandle normalView)
{
    m_DepthTexture = depthTexture;
    m_DepthView    = depthView;
    m_NormalView   = normalView;

    if (m_Device != nullptr)
    {
        UpdateDescriptors(m_Device);
    }
}

void FGtaoPass::SetCameraParams(const FMatrix& view,
                                const FMatrix& projectionNoJitter)
{
    m_View              = view;
    m_InverseProjection = projectionNoJitter.Inverse();
}

// ============================================================================
// CreatePipeline
// ============================================================================

ERHIResult FGtaoPass::CreatePipeline(IRHIDevice* device)
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
        device, FString("Builtin/gtao.frag"), EShaderStage::Fragment,
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

    FRHIColorBlendAttachmentDesc colorBlend =
        FRHIColorBlendAttachmentDesc::Opaque();

    pipelineDesc.ColorBlend.Attachments      = &colorBlend;
    pipelineDesc.ColorBlend.AttachmentCount  = 1;
    pipelineDesc.ColorBlend.IsLogicOpEnabled = false;

    pipelineDesc.DynamicState.EnabledStates =
        EDynamicState::Viewport | EDynamicState::Scissor;

    pipelineDesc.PipelineLayout = m_PipelineLayout;
    pipelineDesc.RenderPass     = m_RenderPass;
    pipelineDesc.SubpassIndex   = 0;
    pipelineDesc.DebugName      = "GtaoPass_Pipeline";

    return device->CreateGraphicsPipeline(pipelineDesc, m_Pipeline);
}

// ============================================================================
// Execute
// ============================================================================

void FGtaoPass::Execute(IRHICommandBuffer*        commandBuffer,
                        const FRenderPassContext& context)
{
    if (commandBuffer == nullptr)
    {
        return;
    }

    // 关闭时把 AO 清成 1, 而且只清一次 —— 内容不会变, 每帧清是一次全屏写。
    //
    // 清而不是让前向通道分支: 有分支的话"AO 通道没跑"与"AO 恰好全是 1"在
    // 画面上无法区分, 而前者是缺陷。
    if (!m_Enabled)
    {
        if (m_ClearedToOne)
        {
            return;
        }
    }
    else if (!m_DepthView.IsValid() || !m_NormalView.IsValid())
    {
        LIMX_LOG(LogRenderer, Error,
                 "[GtaoPass] 深度或法线未接入 — 本帧跳过");
        return;
    }

    commandBuffer->BeginDebugLabel("GtaoPass", 0.6f, 0.6f, 0.6f);

    // 深度此刻停在 DepthStencilAttachment (深度预通道的 FinalLayout)。
    // 采样它要先转成只读布局, 用完转回去 —— 天空通道 (150) 的
    // InitialLayout 声明的还是 DepthStencilAttachment。
    if (m_Enabled && m_DepthTexture.IsValid())
    {
        commandBuffer->TransitionImageLayout(
            m_DepthTexture,
            EImageLayout::DepthStencilAttachment,
            EImageLayout::DepthStencilReadOnly,
            EPipelineStageFlags::LateFragmentTests,
            EPipelineStageFlags::FragmentShader,
            EAccessFlags::DepthStencilAttachmentWrite,
            EAccessFlags::ShaderRead);
    }

    FRHIClearColorValue clearColor = {};
    clearColor.R = 1.0f;
    clearColor.G = 1.0f;
    clearColor.B = 1.0f;
    clearColor.A = 1.0f;

    FRHIRenderPassBeginInfo beginInfo = {};
    beginInfo.RenderPass        = m_RenderPass;
    beginInfo.Framebuffer       = m_Framebuffer;
    beginInfo.RenderAreaOffset  = { 0, 0 };
    beginInfo.RenderAreaExtent  = context.SwapchainExtent;
    beginInfo.ClearColors       = &clearColor;
    beginInfo.ClearColorCount   = 1;
    beginInfo.ClearDepthStencil = nullptr;

    commandBuffer->BeginRenderPass(beginInfo);

    if (m_Enabled)
    {
        commandBuffer->BindGraphicsPipeline(m_Pipeline);

        FRHIViewport viewport = {};
        viewport.X        = 0.0f;
        viewport.Y        = 0.0f;
        viewport.Width    =
            static_cast<Float32>(context.SwapchainExtent.Width);
        viewport.Height   =
            static_cast<Float32>(context.SwapchainExtent.Height);
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
                                         m_DescriptorSet, nullptr, 0);

        FGtaoPushConstant pushData;
        pushData.InverseProjection = m_InverseProjection;
        pushData.View              = m_View;
        pushData.Radius            = m_Radius;
        pushData.Intensity         = m_Intensity;
        pushData.ScreenW =
            static_cast<Float32>(context.SwapchainExtent.Width);
        pushData.ScreenH =
            static_cast<Float32>(context.SwapchainExtent.Height);

        commandBuffer->PushConstants(m_PipelineLayout,
                                     EShaderStage::Fragment, 0,
                                     sizeof(FGtaoPushConstant), &pushData);

        commandBuffer->Draw(3, 1, 0, 0);
    }

    commandBuffer->EndRenderPass();

    // 深度转回去 —— 天空与前向通道声明的 InitialLayout 都是
    // DepthStencilAttachment。不转回去的话那两个通道会从错误的旧布局开始,
    // 表现是深度内容被丢弃 (验证层会报, 但只在开启时)。
    if (m_Enabled && m_DepthTexture.IsValid())
    {
        commandBuffer->TransitionImageLayout(
            m_DepthTexture,
            EImageLayout::DepthStencilReadOnly,
            EImageLayout::DepthStencilAttachment,
            EPipelineStageFlags::FragmentShader,
            EPipelineStageFlags::EarlyFragmentTests,
            EAccessFlags::ShaderRead,
            EAccessFlags::DepthStencilAttachmentRead |
            EAccessFlags::DepthStencilAttachmentWrite);
    }

    commandBuffer->EndDebugLabel();

    if (!m_Enabled)
    {
        m_ClearedToOne = true;
    }
}

// ============================================================================
// OnResize
// ============================================================================

ERHIResult FGtaoPass::OnResize(const FPassResizeDesc& desc)
{
    m_Extent = desc.Extent;

    if (m_Framebuffer.IsValid())
    {
        desc.Device->DestroyFramebuffer(m_Framebuffer);
        m_Framebuffer = {};
    }

    if (m_AoView.IsValid())
    {
        desc.Device->DestroyTextureView(m_AoView);
        m_AoView = {};
    }

    if (m_AoTexture.IsValid())
    {
        desc.Device->DestroyTexture(m_AoTexture);
        m_AoTexture = {};
    }

    ERHIResult result = CreateTarget(desc.Device, m_Extent);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    FRHIFramebufferDesc fbDesc = {};
    fbDesc.RenderPass      = m_RenderPass;
    fbDesc.Attachments     = &m_AoView;
    fbDesc.AttachmentCount = 1;
    fbDesc.Width           = m_Extent.Width;
    fbDesc.Height          = m_Extent.Height;
    fbDesc.Layers          = 1;
    fbDesc.DebugName       = "GtaoPass_Framebuffer";

    result = desc.Device->CreateFramebuffer(fbDesc, m_Framebuffer);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    // 深度与法线的视图换了新的
    m_DepthTexture = desc.SharedDepth;
    m_DepthView    = desc.SharedDepthView;

    UpdateDescriptors(desc.Device);

    m_ClearedToOne = false;

    return ERHIResult::Success;
}

void FGtaoPass::ReleaseSwapchainResources(IRHIDevice* device)
{
    (void)device;
}

// ============================================================================
// Shutdown
// ============================================================================

void FGtaoPass::Shutdown(IRHIDevice* device)
{
    if (device == nullptr)
    {
        return;
    }

    if (m_Framebuffer.IsValid())
    {
        device->DestroyFramebuffer(m_Framebuffer);
        m_Framebuffer = {};
    }

    if (m_AoView.IsValid())
    {
        device->DestroyTextureView(m_AoView);
        m_AoView = {};
    }

    if (m_AoTexture.IsValid())
    {
        device->DestroyTexture(m_AoTexture);
        m_AoTexture = {};
    }

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

    LIMX_LOG(LogRenderer, Log, "[GtaoPass] 已关闭");
}

} // namespace Limx
