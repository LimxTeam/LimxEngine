/*******************************************************************************
 * 文件: FSkyPass.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   天空盒 Pass 实现 — 全屏三角形、立方体贴图采样、HDR 目标清屏
 *
 * 设计哲学:
 *   管线布局与场景共享 set 0 而非自建一份 view/proj。这不只是省一个 UBO:
 *   天空的视线方向必须与几何体的投影**逐比特一致**, 否则地平线处天空与
 *   地面会错开半个像素。共用同一份矩阵数据是保证这一点最直接的方式。
 *
 *   没有环境贴图时仍然开渲染通道并清屏。看似多余 —— 但清屏正是本 Pass
 *   从前向 Pass 接过来的职责, 跳过它前向 Pass 就会 Load 到未定义内容。
 *   "没贴图就不做事"是这里最诱人也最错的简化。
 *
 * 依赖关系:
 *   内部: Renderer/RenderPass/FSkyPass.h, RenderCore/Shaders/FShaderManager.h
 *
 ******************************************************************************/

#include "Renderer/RenderPass/FSkyPass.h"
#include "RenderCore/Shaders/FShaderManager.h"

namespace Limx
{

LIMX_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{

/// 与 sky.frag 的 Push Constant 块一一对应
struct FSkyPushConstant
{
    Float32 Intensity = 1.0f;
    Float32 Pad0      = 0.0f;
    Float32 Pad1      = 0.0f;
    Float32 Pad2      = 0.0f;
};

static_assert(sizeof(FSkyPushConstant) == 16,
              "FSkyPushConstant 必须为 16 字节以匹配着色器布局");

} // namespace

// ============================================================================
// Setup
// ============================================================================

ERHIResult FSkyPass::Setup(const FPassSetupDesc& desc)
{
    LIMX_CHECK(desc.Device != nullptr);

    m_ColorFormat = desc.SharedColorFormat;
    m_DepthFormat = desc.SharedDepthFormat;
    m_Extent      = desc.SwapchainExtent;

    ERHIResult result = CreateRenderPass(desc.Device);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[SkyPass] RenderPass 创建失败");
        return result;
    }

    result = CreateFramebuffer(desc.Device, desc.SwapchainExtent,
                               desc.SharedColorTextureView,
                               desc.SharedDepthTextureView);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[SkyPass] Framebuffer 创建失败");
        return result;
    }

    result = CreateDescriptorResources(desc.Device, desc.ViewProjSetLayout);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[SkyPass] 描述符资源创建失败");
        return result;
    }

    result = CreateShaders(desc.Device);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[SkyPass] 着色器创建失败");
        return result;
    }

    result = CreatePipeline(desc.Device);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[SkyPass] 管线创建失败");
        return result;
    }

    LIMX_LOG(LogRenderer, Log,
             "[SkyPass] 初始化完成 — {}x{} (环境贴图未绑定)",
             desc.SwapchainExtent.Width, desc.SwapchainExtent.Height);

    return ERHIResult::Success;
}

// ============================================================================
// CreateRenderPass — HDR 颜色 (Clear) + 共享深度 (Load, 只读)
// ============================================================================

ERHIResult FSkyPass::CreateRenderPass(IRHIDevice* device)
{
    FRHIAttachmentDesc attachments[2] = {};

    // 附件 0: HDR 颜色 —— 本 Pass 是第一个触碰它的, 由它负责清除
    //
    // 最终布局停在 ColorAttachment 而非 ShaderReadOnly: 紧随其后的前向
    // Pass 还要继续往里画。转成只读再转回来是一次纯粹多余的布局转换。
    attachments[0].Format         = m_ColorFormat;
    attachments[0].Samples        = ESampleCount::Count1;
    attachments[0].LoadOp         = ELoadOp::Clear;
    attachments[0].StoreOp        = EStoreOp::Store;
    attachments[0].StencilLoadOp  = ELoadOp::DontCare;
    attachments[0].StencilStoreOp = EStoreOp::DontCare;
    attachments[0].InitialLayout  = EImageLayout::Undefined;
    attachments[0].FinalLayout    = EImageLayout::ColorAttachment;

    // 附件 1: 共享深度 —— 只测试不写入, 但仍须 Store: 前向 Pass 要用它
    attachments[1].Format         = m_DepthFormat;
    attachments[1].Samples        = ESampleCount::Count1;
    attachments[1].LoadOp         = ELoadOp::Load;
    attachments[1].StoreOp        = EStoreOp::Store;
    attachments[1].StencilLoadOp  = ELoadOp::DontCare;
    attachments[1].StencilStoreOp = EStoreOp::DontCare;
    attachments[1].InitialLayout  = EImageLayout::DepthStencilAttachment;
    attachments[1].FinalLayout    = EImageLayout::DepthStencilAttachment;

    FRHIAttachmentReference colorRef = {};
    colorRef.AttachmentIndex = 0;
    colorRef.Layout          = EImageLayout::ColorAttachment;

    FRHIAttachmentReference depthRef = {};
    depthRef.AttachmentIndex = 1;
    depthRef.Layout          = EImageLayout::DepthStencilAttachment;

    FRHISubpassDesc subpass = {};
    subpass.ColorAttachments       = &colorRef;
    subpass.ColorAttachmentCount   = 1;
    subpass.DepthStencilAttachment = &depthRef;

    // 确保深度预通道的写入对本 Pass 的深度测试可见
    FRHISubpassDependency dependency = {};
    dependency.SrcSubpass    = 0xFFFFFFFF;
    dependency.DstSubpass    = 0;
    dependency.SrcStageMask  = EPipelineStageFlags::EarlyFragmentTests
                             | EPipelineStageFlags::LateFragmentTests;
    dependency.DstStageMask  = EPipelineStageFlags::EarlyFragmentTests
                             | EPipelineStageFlags::ColorAttachmentOutput;
    dependency.SrcAccessMask = EAccessFlags::DepthStencilAttachmentWrite;
    dependency.DstAccessMask = EAccessFlags::DepthStencilAttachmentRead
                             | EAccessFlags::ColorAttachmentWrite;

    FRHIRenderPassDesc renderPassDesc = {};
    renderPassDesc.Attachments     = attachments;
    renderPassDesc.AttachmentCount = 2;
    renderPassDesc.Subpasses       = &subpass;
    renderPassDesc.SubpassCount    = 1;
    renderPassDesc.Dependencies    = &dependency;
    renderPassDesc.DependencyCount = 1;
    renderPassDesc.DebugName       = "SkyPass_RenderPass";

    return device->CreateRenderPass(renderPassDesc, m_RenderPass);
}

// ============================================================================
// CreateFramebuffer
// ============================================================================

ERHIResult FSkyPass::CreateFramebuffer(IRHIDevice*           device,
                                       FRHIExtent2D          extent,
                                       FRHITextureViewHandle colorView,
                                       FRHITextureViewHandle depthView)
{
    FRHITextureViewHandle fbAttachments[2] = { colorView, depthView };

    FRHIFramebufferDesc fbDesc = {};
    fbDesc.RenderPass      = m_RenderPass;
    fbDesc.Attachments     = fbAttachments;
    fbDesc.AttachmentCount = 2;
    fbDesc.Width           = extent.Width;
    fbDesc.Height          = extent.Height;
    fbDesc.Layers          = 1;
    fbDesc.DebugName       = "SkyPass_Framebuffer";

    return device->CreateFramebuffer(fbDesc, m_Framebuffer);
}

void FSkyPass::DestroyFramebuffer(IRHIDevice* device)
{
    if (m_Framebuffer.IsValid())
    {
        device->DestroyFramebuffer(m_Framebuffer);
    }
}

// ============================================================================
// CreateDescriptorResources — set 1 的立方体贴图 + 管线布局
// ============================================================================

ERHIResult FSkyPass::CreateDescriptorResources(
    IRHIDevice*             device,
    FRHIDescSetLayoutHandle viewProjLayout)
{
    if (!viewProjLayout.IsValid())
    {
        LIMX_LOG(LogRenderer, Error,
                 "[SkyPass] set 0 布局无效 —— 天空需要场景的 view/proj");
        return ERHIResult::ErrorInvalidHandle;
    }

    FRHIDescriptorBinding binding = {};
    binding.Binding    = 0;
    binding.Type       = EDescriptorType::CombinedImageSampler;
    binding.Count      = 1;
    binding.StageFlags = EShaderStage::Fragment;

    FRHIDescSetLayoutDesc layoutDesc = {};
    layoutDesc.Bindings     = &binding;
    layoutDesc.BindingCount = 1;
    layoutDesc.DebugName    = "SkyCubeDescSetLayout";

    ERHIResult result = device->CreateDescSetLayout(layoutDesc,
                                                    m_CubeSetLayout);
    if (!IsRHISuccess(result))
    {
        return result;
    }

    result = device->AllocateDescriptorSet(m_CubeSetLayout,
                                           m_CubeDescriptorSet);
    if (!IsRHISuccess(result))
    {
        return result;
    }

    // set 0 用场景那一份布局对象本身, 而非另建一个"结构相同"的:
    // Vulkan 的描述符集兼容性按布局对象判定, 结构相同但对象不同的两个布局
    // 在验证层看来并不兼容, 绑定场景的 set 0 会直接报错。
    const FRHIDescSetLayoutHandle setLayouts[2] =
    {
        viewProjLayout,
        m_CubeSetLayout
    };

    FRHIPushConstantRange pushRange = {};
    pushRange.StageFlags = EShaderStage::Fragment;
    pushRange.Offset     = 0;
    pushRange.Size       = sizeof(FSkyPushConstant);

    FRHIPipelineLayoutDesc pipelineLayoutDesc = {};
    pipelineLayoutDesc.SetLayouts             = setLayouts;
    pipelineLayoutDesc.SetLayoutCount         = 2;
    pipelineLayoutDesc.PushConstantRanges     = &pushRange;
    pipelineLayoutDesc.PushConstantRangeCount = 1;
    pipelineLayoutDesc.DebugName              = "SkyPipelineLayout";

    return device->CreatePipelineLayout(pipelineLayoutDesc, m_PipelineLayout);
}

// ============================================================================
// SetEnvironmentMap
// ============================================================================

void FSkyPass::SetEnvironmentMap(IRHIDevice*           device,
                                 FRHITextureViewHandle cubeView,
                                 FRHISamplerHandle     sampler)
{
    if (device == nullptr || !m_CubeDescriptorSet.IsValid())
    {
        return;
    }

    if (!cubeView.IsValid() || !sampler.IsValid())
    {
        // 解绑: 不去改描述符集 —— 往里写无效句柄是未定义行为, 而只要
        // 不再绘制, 集里留着的旧内容就永远不会被读到。
        m_HasEnvironmentMap = false;

        LIMX_LOG(LogRenderer, Log, "[SkyPass] 环境贴图已解绑");
        return;
    }

    FRHIDescriptorWrite write = FRHIDescriptorWrite::CombinedImageSampler(
        m_CubeDescriptorSet, 0, cubeView, sampler,
        EImageLayout::ShaderReadOnly);

    device->UpdateDescriptorSets(&write, 1);

    m_HasEnvironmentMap = true;

    LIMX_LOG(LogRenderer, Log, "[SkyPass] 环境贴图已绑定");
}

// ============================================================================
// CreateShaders
// ============================================================================

ERHIResult FSkyPass::CreateShaders(IRHIDevice* device)
{
    FShaderManager& shaderManager = FShaderManager::Get();

    if (!shaderManager.IsInitialized())
    {
        shaderManager.Initialize();
    }

    ERHIResult result = shaderManager.CreateShaderModule(
        device, FString("Builtin/sky.vert"),
        EShaderStage::Vertex, m_VertShader);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    return shaderManager.CreateShaderModule(
        device, FString("Builtin/sky.frag"),
        EShaderStage::Fragment, m_FragShader);
}

// ============================================================================
// CreatePipeline
// ============================================================================

ERHIResult FSkyPass::CreatePipeline(IRHIDevice* device)
{
    FRHIGraphicsPipelineDesc pipelineDesc = {};

    pipelineDesc.ShaderStages[0].Shader     = m_VertShader;
    pipelineDesc.ShaderStages[0].Stage      = EShaderStage::Vertex;
    pipelineDesc.ShaderStages[0].EntryPoint = "main";

    pipelineDesc.ShaderStages[1].Shader     = m_FragShader;
    pipelineDesc.ShaderStages[1].Stage      = EShaderStage::Fragment;
    pipelineDesc.ShaderStages[1].EntryPoint = "main";

    pipelineDesc.ShaderStageCount = 2;

    // 无顶点输入 —— 顶点由 gl_VertexIndex 算出
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

    // 深度测试开、深度写入关。
    //
    // 天空在顶点着色器里输出深度 1.0 (远平面), 配合 LessOrEqual 只能通过
    // 深度仍为 1.0 的像素 —— 也就是深度预通道没有写过的地方。用 Equal 也
    // 能达到同样效果, 但 LessOrEqual 对"深度预通道被跳过"这种情形更宽容:
    // 那时深度是清屏值 1.0, 天空照样正确铺满。
    //
    // 深度写入必须关: 开着的话半透明物体在深度测试时会被天空挡掉。
    pipelineDesc.DepthStencil.IsDepthTestEnabled  = true;
    pipelineDesc.DepthStencil.IsDepthWriteEnabled = false;
    pipelineDesc.DepthStencil.DepthCompareOp      = ECompareOp::LessOrEqual;

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
    pipelineDesc.DebugName      = "SkyPass_Pipeline";

    return device->CreateGraphicsPipeline(pipelineDesc, m_Pipeline);
}

// ============================================================================
// Execute
// ============================================================================

void FSkyPass::Execute(IRHICommandBuffer*        commandBuffer,
                       const FRenderPassContext& context)
{
    if (!m_Framebuffer.IsValid())
    {
        return;
    }

    commandBuffer->BeginDebugLabel("SkyPass", 0.4f, 0.7f, 1.0f);

    // 清除值 — 与从前的前向 Pass 一致 (深蓝灰)。有环境贴图时它会被
    // 天空整片覆盖; 没有时它就是背景色。
    FRHIClearColorValue clearColor = {};
    clearColor.R = 0.01f;
    clearColor.G = 0.01f;
    clearColor.B = 0.02f;
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

    // 没有环境贴图时只清屏 —— 清屏本身就是本 Pass 的职责之一,
    // 跳过整个通道会让前向 Pass Load 到未定义内容
    if (m_HasEnvironmentMap && context.ViewProjDescriptorSet.IsValid())
    {
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
                                         context.ViewProjDescriptorSet);

        commandBuffer->BindDescriptorSet(EPipelineBindPoint::Graphics,
                                         m_PipelineLayout, 1,
                                         m_CubeDescriptorSet);

        FSkyPushConstant pushData;
        pushData.Intensity = m_Intensity;

        commandBuffer->PushConstants(m_PipelineLayout, EShaderStage::Fragment,
                                     0, sizeof(FSkyPushConstant), &pushData);

        commandBuffer->Draw(3, 1, 0, 0);
    }

    commandBuffer->EndRenderPass();
    commandBuffer->EndDebugLabel();
}

// ============================================================================
// OnResize
// ============================================================================

ERHIResult FSkyPass::OnResize(IRHIDevice*           device,
                              FRHISwapchainHandle   swapchain,
                              FRHIExtent2D          newExtent,
                              UInt32                swapchainImageCount,
                              FRHITextureHandle     newSharedDepth,
                              FRHITextureViewHandle newSharedDepthView,
                              FRHITextureHandle     newSharedColor,
                              FRHITextureViewHandle newSharedColorView)
{
    (void)swapchain;
    (void)swapchainImageCount;
    (void)newSharedDepth;
    (void)newSharedColor;

    m_Extent = newExtent;

    DestroyFramebuffer(device);

    return CreateFramebuffer(device, newExtent, newSharedColorView,
                             newSharedDepthView);
}

void FSkyPass::ReleaseSwapchainResources(IRHIDevice* device)
{
    DestroyFramebuffer(device);
}

// ============================================================================
// Shutdown
// ============================================================================

void FSkyPass::Shutdown(IRHIDevice* device)
{
    if (device == nullptr)
    {
        return;
    }

    device->DestroyGraphicsPipeline(m_Pipeline);
    device->DestroyPipelineLayout(m_PipelineLayout);

    device->FreeDescriptorSet(m_CubeDescriptorSet);
    device->DestroyDescSetLayout(m_CubeSetLayout);

    device->DestroyShader(m_FragShader);
    device->DestroyShader(m_VertShader);

    DestroyFramebuffer(device);
    device->DestroyRenderPass(m_RenderPass);

    m_HasEnvironmentMap = false;

    LIMX_LOG(LogRenderer, Log, "[SkyPass] 已关闭");
}

} // namespace Limx
