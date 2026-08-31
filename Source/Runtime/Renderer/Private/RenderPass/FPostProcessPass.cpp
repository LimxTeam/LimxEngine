/*******************************************************************************
 * 文件: FPostProcessPass.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   后处理 Pass 实现 — 全屏三角形、曝光与 ACES 色调映射
 *
 * 设计哲学:
 *   本 Pass 自带描述符集布局与管线布局，不复用场景的那一套。场景的布局是
 *   为 "set0 视图矩阵 + set1 材质 + set2 光照" 设计的，后处理只需要一张
 *   纹理；硬套那套布局意味着要为三个用不到的 set 提供有效的描述符集，
 *   而它们与后处理毫无关系。
 *
 *   描述符集只有一份、不按帧复制：它绑定的 HDR 纹理在整个尺寸周期内不变，
 *   而写入只发生在 Setup 与 OnResize。逐帧复制一份内容恒定的描述符集，
 *   除了让"哪一份是当前的"变复杂之外没有任何好处。
 *
 * 技术特性:
 *   - vkCmdDraw(3,1,0,0)，无顶点缓冲区、无顶点输入状态
 *   - 深度测试关闭：全屏三角形不参与深度
 *   - 曝光经 Push Constant 传入，无需 UBO 与帧同步
 *
 * 依赖关系:
 *   内部: Renderer/RenderPass/FPostProcessPass.h,
 *          RenderCore/Shaders/FShaderManager.h
 *
 ******************************************************************************/

#include "Renderer/RenderPass/FPostProcessPass.h"
#include "RenderCore/Shaders/FShaderManager.h"

namespace Limx
{

LIMX_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{

/// 与 tonemap.frag 的 Push Constant 块一一对应
struct FTonemapPushConstant
{
    Float32 Exposure = 1.0f;

    /// 是否由着色器做 sRGB 编码 (1.0) 还是交给硬件 (0.0)
    ///
    /// 交换链拿到 B8G8R8A8_SRGB 时, 硬件在写入时已经做了同一条编码 ——
    /// 着色器再编一次就是两遍 gamma。而两遍 gamma 不产生任何瑕疵, 只是
    /// 把整幅图往亮处推, 看着像"曝光高了点", 靠肉眼几乎不可能发现。
    Float32 EncodeSrgb = 0.0f;

    Float32 Pad1     = 0.0f;
    Float32 Pad2     = 0.0f;
};

static_assert(sizeof(FTonemapPushConstant) == 16,
              "FTonemapPushConstant 必须为 16 字节以匹配着色器布局");

} // namespace

// ============================================================================
// Setup
// ============================================================================

ERHIResult FPostProcessPass::Setup(const FPassSetupDesc& desc)
{
    LIMX_CHECK(desc.Device != nullptr);

    m_SwapchainFormat = desc.SwapchainFormat;
    m_SwapchainExtent = desc.SwapchainExtent;

    ERHIResult result = CreateRenderPass(desc.Device, desc.SwapchainFormat);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[PostProcessPass] RenderPass 创建失败");
        return result;
    }

    result = CreateFramebuffers(desc.Device, desc.Swapchain,
                                desc.SwapchainExtent,
                                desc.SwapchainImageCount);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[PostProcessPass] Framebuffer 创建失败");
        return result;
    }

    result = CreateDescriptorResources(desc.Device);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[PostProcessPass] 描述符资源创建失败");
        return result;
    }

    UpdateSourceDescriptor(desc.Device, desc.SharedColorTextureView);

    result = CreateShaders(desc.Device);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[PostProcessPass] 着色器创建失败");
        return result;
    }

    result = CreatePipeline(desc.Device);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[PostProcessPass] 管线创建失败");
        return result;
    }

    LIMX_LOG(LogRenderer, Log,
             "[PostProcessPass] 初始化完成 — ACES 色调映射, {}x{}, "
             "sRGB 编码由{}完成",
             desc.SwapchainExtent.Width, desc.SwapchainExtent.Height,
             IsSRGBFormat(m_SwapchainFormat) ? "硬件" : "着色器");

    return ERHIResult::Success;
}

// ============================================================================
// CreateRenderPass — 单颜色附件, 最终布局 PresentSrc
// ============================================================================

ERHIResult FPostProcessPass::CreateRenderPass(IRHIDevice* device,
                                               EPixelFormat swapchainFormat)
{
    FRHIAttachmentDesc colorAttachment = {};
    colorAttachment.Format         = swapchainFormat;
    colorAttachment.Samples        = ESampleCount::Count1;

    // LoadOp=DontCare 而非 Clear: 全屏三角形覆盖每一个像素, 清除是纯粹的
    // 浪费 —— 在移动端 tile 架构上这是一次完整的 tile 填充带宽。
    colorAttachment.LoadOp         = ELoadOp::DontCare;
    colorAttachment.StoreOp        = EStoreOp::Store;
    colorAttachment.StencilLoadOp  = ELoadOp::DontCare;
    colorAttachment.StencilStoreOp = EStoreOp::DontCare;
    colorAttachment.InitialLayout  = EImageLayout::Undefined;
    colorAttachment.FinalLayout    = EImageLayout::PresentSrc;

    FRHIAttachmentReference colorRef = {};
    colorRef.AttachmentIndex = 0;
    colorRef.Layout          = EImageLayout::ColorAttachment;

    FRHISubpassDesc subpass = {};
    subpass.ColorAttachments       = &colorRef;
    subpass.ColorAttachmentCount   = 1;
    subpass.DepthStencilAttachment = nullptr;

    // 进入本通道前, 前向 Pass 对 HDR 纹理的写入必须已完成
    FRHISubpassDependency dependency = {};
    dependency.SrcSubpass    = 0xFFFFFFFF;
    dependency.DstSubpass    = 0;
    dependency.SrcStageMask  = EPipelineStageFlags::ColorAttachmentOutput;
    dependency.DstStageMask  = EPipelineStageFlags::FragmentShader;
    dependency.SrcAccessMask = EAccessFlags::ColorAttachmentWrite;
    dependency.DstAccessMask = EAccessFlags::ShaderRead;

    FRHIRenderPassDesc renderPassDesc = {};
    renderPassDesc.Attachments     = &colorAttachment;
    renderPassDesc.AttachmentCount = 1;
    renderPassDesc.Subpasses       = &subpass;
    renderPassDesc.SubpassCount    = 1;
    renderPassDesc.Dependencies    = &dependency;
    renderPassDesc.DependencyCount = 1;
    renderPassDesc.DebugName       = "PostProcessPass_RenderPass";

    return device->CreateRenderPass(renderPassDesc, m_RenderPass);
}

// ============================================================================
// CreateFramebuffers — 每交换链图像一个
// ============================================================================

ERHIResult FPostProcessPass::CreateFramebuffers(IRHIDevice* device,
                                                 FRHISwapchainHandle swapchain,
                                                 FRHIExtent2D extent,
                                                 UInt32 imageCount)
{
    m_Framebuffers.Reserve(imageCount);

    for (UInt32 i = 0; i < imageCount; ++i)
    {
        FRHITextureViewHandle colorView =
            device->GetSwapchainImageView(swapchain, i);

        FRHIFramebufferDesc fbDesc = {};
        fbDesc.RenderPass      = m_RenderPass;
        fbDesc.Attachments     = &colorView;
        fbDesc.AttachmentCount = 1;
        fbDesc.Width           = extent.Width;
        fbDesc.Height          = extent.Height;
        fbDesc.Layers          = 1;
        fbDesc.DebugName       = "PostProcessPass_Framebuffer";

        FRHIFramebufferHandle framebuffer;
        const ERHIResult result =
            device->CreateFramebuffer(fbDesc, framebuffer);

        if (!IsRHISuccess(result))
        {
            return result;
        }

        m_Framebuffers.Add(framebuffer);
    }

    return ERHIResult::Success;
}

void FPostProcessPass::DestroyFramebuffers(IRHIDevice* device)
{
    for (SizeType i = 0; i < m_Framebuffers.GetSize(); ++i)
    {
        device->DestroyFramebuffer(m_Framebuffers[i]);
    }

    m_Framebuffers.Clear();
}

// ============================================================================
// CreateDescriptorResources — 一张纹理的布局与描述符集
// ============================================================================

ERHIResult FPostProcessPass::CreateDescriptorResources(IRHIDevice* device)
{
    FRHIDescriptorBinding binding = {};
    binding.Binding    = 0;
    binding.Type       = EDescriptorType::CombinedImageSampler;
    binding.Count      = 1;
    binding.StageFlags = EShaderStage::Fragment;

    FRHIDescSetLayoutDesc layoutDesc = {};
    layoutDesc.Bindings     = &binding;
    layoutDesc.BindingCount = 1;
    layoutDesc.DebugName    = "PostProcessDescSetLayout";

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

    // 采样器: 全屏 1:1 采样其实用不到过滤, 但降采样 (Bloom) 时需要。
    // Clamp 而非 Repeat —— 边缘处采到对侧像素会在画面四周留下一圈错误颜色。
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

    // ---- 管线布局 ----
    FRHIPushConstantRange pushRange = {};
    pushRange.StageFlags = EShaderStage::Fragment;
    pushRange.Offset     = 0;
    pushRange.Size       = sizeof(FTonemapPushConstant);

    FRHIPipelineLayoutDesc pipelineLayoutDesc = {};
    pipelineLayoutDesc.SetLayouts             = &m_DescSetLayout;
    pipelineLayoutDesc.SetLayoutCount         = 1;
    pipelineLayoutDesc.PushConstantRanges     = &pushRange;
    pipelineLayoutDesc.PushConstantRangeCount = 1;
    pipelineLayoutDesc.DebugName              = "PostProcessPipelineLayout";

    return device->CreatePipelineLayout(pipelineLayoutDesc, m_PipelineLayout);
}

void FPostProcessPass::UpdateSourceDescriptor(IRHIDevice* device,
                                               FRHITextureViewHandle hdrView)
{
    if (!hdrView.IsValid() || !m_DescriptorSet.IsValid())
    {
        return;
    }

    FRHIDescriptorWrite write = FRHIDescriptorWrite::CombinedImageSampler(
        m_DescriptorSet, 0, hdrView, m_Sampler,
        EImageLayout::ShaderReadOnly);

    device->UpdateDescriptorSets(&write, 1);
}

// ============================================================================
// CreateShaders
// ============================================================================

ERHIResult FPostProcessPass::CreateShaders(IRHIDevice* device)
{
    FShaderManager& shaderManager = FShaderManager::Get();

    if (!shaderManager.IsInitialized())
    {
        shaderManager.Initialize();
    }

    ERHIResult result = shaderManager.CreateShaderModule(
        device, FString("Builtin/fullscreen.vert"),
        EShaderStage::Vertex, m_VertShader);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    return shaderManager.CreateShaderModule(
        device, FString("Builtin/tonemap.frag"),
        EShaderStage::Fragment, m_FragShader);
}

// ============================================================================
// CreatePipeline
// ============================================================================

ERHIResult FPostProcessPass::CreatePipeline(IRHIDevice* device)
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

    // 不剔除 —— 全屏三角形的绕序取决于 gl_VertexIndex 的推导方式,
    // 与场景几何无关; 开剔除只会让"改一下 UV 公式就整屏黑"成为可能。
    pipelineDesc.Rasterization.CullMode  = ECullMode::None;
    pipelineDesc.Rasterization.FrontFace = EFrontFace::CounterClockwise;
    pipelineDesc.Rasterization.IsDepthBiasEnabled = false;
    pipelineDesc.Rasterization.LineWidth = 1.0f;

    pipelineDesc.Multisample.RasterizationSamples   = ESampleCount::Count1;
    pipelineDesc.Multisample.IsSampleShadingEnabled = false;

    // 全屏三角形不参与深度
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
    pipelineDesc.DebugName      = "PostProcessPass_Pipeline";

    return device->CreateGraphicsPipeline(pipelineDesc, m_Pipeline);
}

// ============================================================================
// Execute
// ============================================================================

void FPostProcessPass::Execute(IRHICommandBuffer*        commandBuffer,
                                const FRenderPassContext& context)
{
    if (context.ImageIndex >= m_Framebuffers.GetSize())
    {
        return;
    }

    commandBuffer->BeginDebugLabel("PostProcessPass", 1.0f, 0.6f, 0.2f);

    FRHIRenderPassBeginInfo beginInfo = {};
    beginInfo.RenderPass        = m_RenderPass;
    beginInfo.Framebuffer       = m_Framebuffers[context.ImageIndex];
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

    commandBuffer->BindDescriptorSet(
        EPipelineBindPoint::Graphics,
        m_PipelineLayout,
        0,
        m_DescriptorSet,
        nullptr,
        0);

    FTonemapPushConstant pushData;
    pushData.Exposure = m_Exposure;

    // sRGB 格式的目标由硬件编码, 着色器就不能再编一次
    pushData.EncodeSrgb = IsSRGBFormat(m_SwapchainFormat) ? 0.0f : 1.0f;

    commandBuffer->PushConstants(
        m_PipelineLayout,
        EShaderStage::Fragment,
        0,
        sizeof(FTonemapPushConstant),
        &pushData);

    // 三个顶点, 无顶点缓冲区
    commandBuffer->Draw(3, 1, 0, 0);

    commandBuffer->EndRenderPass();
    commandBuffer->EndDebugLabel();
}

// ============================================================================
// OnResize
// ============================================================================

ERHIResult FPostProcessPass::OnResize(const FPassResizeDesc& desc)
{
    // 解出局部别名 —— 下面的函数体沿用原来的名字。
    //
    // 这样改动只落在签名上, 函数体一行不动, 便于确认这次重构确实没有
    // 改变行为。
    IRHIDevice* const           device               = desc.Device;
    const FRHISwapchainHandle   swapchain            = desc.Swapchain;
    const FRHIExtent2D          newExtent            = desc.Extent;
    const UInt32                swapchainImageCount  = desc.SwapchainImageCount;
    const FRHITextureHandle     newSharedDepth       = desc.SharedDepth;
    const FRHITextureViewHandle newSharedDepthView   = desc.SharedDepthView;
    const FRHITextureHandle     newSharedColor       = desc.SharedColor;
    const FRHITextureViewHandle newSharedColorView   = desc.SharedColorView;

    (void)device;
    (void)swapchain;
    (void)newExtent;
    (void)swapchainImageCount;
    (void)newSharedDepth;
    (void)newSharedDepthView;
    (void)newSharedColor;
    (void)newSharedColorView;

    (void)newSharedDepth;
    (void)newSharedDepthView;
    (void)newSharedColor;

    m_SwapchainExtent = newExtent;

    DestroyFramebuffers(device);

    const ERHIResult result = CreateFramebuffers(device, swapchain, newExtent,
                                                 swapchainImageCount);

    // HDR 目标随交换链重建, 描述符必须指向新的视图 —— 不更新的话采样的是
    // 已销毁的旧纹理。
    UpdateSourceDescriptor(device, newSharedColorView);

    return result;
}

void FPostProcessPass::ReleaseSwapchainResources(IRHIDevice* device)
{
    DestroyFramebuffers(device);
}

// ============================================================================
// Shutdown
// ============================================================================

void FPostProcessPass::Shutdown(IRHIDevice* device)
{
    if (device == nullptr)
    {
        return;
    }

    device->DestroyGraphicsPipeline(m_Pipeline);
    device->DestroyPipelineLayout(m_PipelineLayout);

    device->DestroySampler(m_Sampler);
    device->FreeDescriptorSet(m_DescriptorSet);
    device->DestroyDescSetLayout(m_DescSetLayout);

    device->DestroyShader(m_FragShader);
    device->DestroyShader(m_VertShader);

    DestroyFramebuffers(device);
    device->DestroyRenderPass(m_RenderPass);

    LIMX_LOG(LogRenderer, Log, "[PostProcessPass] 已关闭");
}

} // namespace Limx
