// ============================================================
// 文件名称：FForwardPass.cpp
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：渐进式封装 — 将现有 FRenderer::RecordCommands() 的单体逻辑
//          迁移为可独立测试的 Pass 对象，配合 PBR 着色器实现
//          Cook-Torrance BRDF 多光源前向渲染。
// 功能描述：FForwardPass 完整实现 — 颜色+深度渲染通道，使用 pbr.vert/frag
//          着色器，配合 FDepthPrePass 的 Early-Z 优化：DepthCompareOp=Equal，
//          禁止深度写入，确保只渲染可见片段。
//          描述符集绑定: set 0 (ViewProj+纹理), set 1 (材质), set 2 (光照)。
// 技术特性：每图像一个 Framebuffer (颜色=交换链视图, 深度=共享深度视图);
//          depth attachment LoadOp=Load (使用 FDepthPrePass 写入的深度值);
//          depth 初始布局=DepthStencilAttachment (prepass 写完后的布局);
//          管线使用 Equal 深度测试, 不写入深度 (避免干扰精确深度值)。
//
// ── 函数表 ──────────────────────────────────────────────────
// │ 函数名                        │ 描述                           │
// │──────────────────────────────│───────────────────────────────│
// │ Setup()                      │ 创建 RenderPass/FB/Shader/管线  │
// │ Execute()                    │ 录制前向渲染完整命令序列          │
// │ OnResize()                   │ 重建 Framebuffer               │
// │ Shutdown()                   │ 释放所有 GPU 资源               │
// │ CreateRenderPass()           │ 创建颜色+深度渲染通道            │
// │ CreateFramebuffers()         │ 为每个交换链图像创建帧缓冲        │
// │ DestroyFramebuffers()        │ 销毁所有帧缓冲                  │
// │ CreateShaders()              │ 加载 pbr.vert/frag              │
// │ CreateGraphicsPipeline()     │ 创建 Equal 深度测试管线          │
// │ DestroyPipelineResources()   │ 销毁管线和着色器                │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M0.5 Pass 抽象层)     │
// │ 2026-04-07  │ LimxTeam  │ M0.5 集成: PBR 着色器+多描述符集  │
// ============================================================

#include "Renderer/RenderPass/FForwardPass.h"
#include "RenderCore/Shaders/FShaderManager.h"

namespace Limx
{

LIMX_DECLARE_LOG_CATEGORY(LogRenderer)

// ============================================================================
// Setup — 创建全部 GPU 资源
// ============================================================================

ERHIResult FForwardPass::Setup(const FPassSetupDesc& desc)
{
    LIMX_CHECK(desc.Device != nullptr);

    m_PipelineLayout  = desc.PipelineLayout;
    m_SwapchainFormat = desc.SwapchainFormat;
    m_SwapchainExtent = desc.SwapchainExtent;

    // 1. 创建颜色+深度渲染通道
    ERHIResult result = CreateRenderPass(desc.Device, desc.SwapchainFormat);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
                 "[ForwardPass] RenderPass 创建失败");
        return result;
    }

    // 2. 为每个交换链图像创建 Framebuffer
    result = CreateFramebuffers(desc.Device,
                                 desc.Swapchain,
                                 desc.SwapchainExtent,
                                 desc.SwapchainImageCount,
                                 desc.SharedDepthTextureView);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
                 "[ForwardPass] Framebuffer 创建失败");
        return result;
    }

    // 3. 加载着色器模块
    result = CreateShaders(desc.Device);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
                 "[ForwardPass] 着色器模块创建失败");
        return result;
    }

    // 4. 创建图形管线
    result = CreateGraphicsPipeline(desc.Device);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
                 "[ForwardPass] 图形管线创建失败");
        return result;
    }

    LIMX_LOG(LogRenderer, Log,
             "[ForwardPass] 初始化完成 — {}x{}",
             desc.SwapchainExtent.Width, desc.SwapchainExtent.Height);

    return ERHIResult::Success;
}

// ============================================================================
// Execute — 录制前向渲染命令 (等价于 FRenderer::RecordCommands)
// ============================================================================

void FForwardPass::Execute(IRHICommandBuffer*        commandBuffer,
                            const FRenderPassContext& context)
{
    commandBuffer->BeginDebugLabel("ForwardPass", 0.2f, 0.8f, 1.0f);

    // ================================================================
    // 清除值 — 颜色 (深蓝灰 Limx 品牌色) + 深度 (最大值 1.0)
    // ================================================================

    FRHIClearColorValue clearColor = {};
    clearColor.R = 0.01f;
    clearColor.G = 0.01f;
    clearColor.B = 0.02f;
    clearColor.A = 1.0f;

    FRHIClearDepthStencilValue clearDepth = {};
    clearDepth.Depth   = 1.0f;
    clearDepth.Stencil = 0;

    // ================================================================
    // 开始渲染通道
    // ================================================================

    FRHIRenderPassBeginInfo beginInfo = {};
    beginInfo.RenderPass        = m_RenderPass;
    beginInfo.Framebuffer       = m_Framebuffers[context.ImageIndex];
    beginInfo.RenderAreaOffset  = { 0, 0 };
    beginInfo.RenderAreaExtent  = context.SwapchainExtent;
    beginInfo.ClearColors       = &clearColor;
    beginInfo.ClearColorCount   = 1;
    beginInfo.ClearDepthStencil = &clearDepth;

    commandBuffer->BeginRenderPass(beginInfo);

    // ================================================================
    // 绑定管线 + 设置动态 Viewport/Scissor
    // ================================================================

    commandBuffer->BindGraphicsPipeline(m_GraphicsPipeline);

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

    // ================================================================
    // 绑定描述符集 (set 0 — ViewProj UBO + 纹理)
    // ================================================================

    commandBuffer->BindDescriptorSet(
        EPipelineBindPoint::Graphics,
        context.PipelineLayout,
        0,
        context.ViewProjDescriptorSet,
        nullptr,
        0
    );

    // ================================================================
    // 绑定描述符集 (set 2 — 光照 UBO, 全物体共享)
    // ================================================================

    commandBuffer->BindDescriptorSet(
        EPipelineBindPoint::Graphics,
        context.PipelineLayout,
        2,
        context.LightingDescriptorSet,
        nullptr,
        0
    );

    // ================================================================
    // 遍历所有渲染物体: 绑定材质 → BindVBO/IBO → Push Model → DrawIndexed
    // ================================================================

    if (context.RenderObjects != nullptr)
    {
        // 记录上一次绑定的状态 —— 批次列表已按材质/网格排序, 相邻批次
        // 大多共享同一套绑定, 逐个重绑等于把排序的收益原地丢掉。
        // 用无效句柄作为初值, 保证第一个批次一定会真正绑定一次。
        FRHIDescriptorSetHandle boundMaterial;
        FRHIBufferHandle        boundVertexBuffer;
        FRHIBufferHandle        boundIndexBuffer;
        EIndexType              boundIndexType = EIndexType::UInt32;

        for (SizeType i = 0; i < context.RenderObjects->GetSize(); ++i)
        {
            const FRenderObject& obj = (*context.RenderObjects)[i];

            // 绑定 set 1 — 材质描述符集 (逐材质)
            if (obj.MaterialDescriptorSet.Packed != boundMaterial.Packed)
            {
                commandBuffer->BindDescriptorSet(
                    EPipelineBindPoint::Graphics,
                    context.PipelineLayout,
                    1,
                    obj.MaterialDescriptorSet,
                    nullptr,
                    0
                );

                boundMaterial = obj.MaterialDescriptorSet;
            }

            if (obj.VertexBuffer.Packed != boundVertexBuffer.Packed)
            {
                commandBuffer->BindVertexBuffer(0, obj.VertexBuffer, 0);
                boundVertexBuffer = obj.VertexBuffer;
            }

            // 索引宽度由网格顶点数决定 —— 写死 UInt16 会让顶点数超过
            // 65535 的网格读出错位的索引, 表现为随机穿插的三角形。
            // 同一缓冲区换了宽度也必须重绑, 因此宽度参与比较。
            if (obj.IndexBuffer.Packed != boundIndexBuffer.Packed ||
                obj.IndexType != boundIndexType)
            {
                commandBuffer->BindIndexBuffer(obj.IndexBuffer, 0,
                                               obj.IndexType);
                boundIndexBuffer = obj.IndexBuffer;
                boundIndexType   = obj.IndexType;
            }

            FModelPushConstant pushData;
            pushData.Model = obj.Transform.ToMatrix();

            commandBuffer->PushConstants(
                context.PipelineLayout,
                EShaderStage::Vertex,
                0,
                sizeof(FModelPushConstant),
                &pushData
            );

            commandBuffer->DrawIndexed(
                obj.IndexCount,
                1,
                obj.IndexOffset,
                0,
                0
            );
        }
    }

    commandBuffer->EndRenderPass();
    commandBuffer->EndDebugLabel();
}

// ============================================================================
// OnResize — 重建 Framebuffer
// ============================================================================

ERHIResult FForwardPass::OnResize(IRHIDevice*           device,
                                   FRHISwapchainHandle   swapchain,
                                   FRHIExtent2D          newExtent,
                                   UInt32                swapchainImageCount,
                                   FRHITextureHandle     newSharedDepth,
                                   FRHITextureViewHandle newSharedDepthView)
{
    m_SwapchainExtent = newExtent;

    DestroyFramebuffers(device);

    ERHIResult result = CreateFramebuffers(device,
                                            swapchain,
                                            newExtent,
                                            swapchainImageCount,
                                            newSharedDepthView);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
                 "[ForwardPass] Resize: Framebuffer 重建失败");
    }

    return result;
}

// ============================================================================
// ReleaseSwapchainResources — 释放尺寸相关资源
// ============================================================================

void FForwardPass::ReleaseSwapchainResources(IRHIDevice* device)
{
    if (device == nullptr)
    {
        return;
    }

    DestroyFramebuffers(device);
}

// ============================================================================
// Shutdown — 释放所有 GPU 资源
// ============================================================================

void FForwardPass::Shutdown(IRHIDevice* device)
{
    if (device == nullptr)
    {
        return;
    }

    DestroyPipelineResources(device);
    DestroyFramebuffers(device);
    device->DestroyRenderPass(m_RenderPass);

    LIMX_LOG(LogRenderer, Log, "[ForwardPass] 已关闭");
}

// ============================================================================
// CreateRenderPass — 颜色附件 + 深度附件 (LoadOp=Load for depth)
// ============================================================================

ERHIResult FForwardPass::CreateRenderPass(IRHIDevice*  device,
                                           EPixelFormat swapchainFormat)
{
    // 附件 0: 颜色附件 — 交换链格式，清除→存储，最终布局 PresentSrc
    FRHIAttachmentDesc attachments[2] = {};

    attachments[0].Format         = swapchainFormat;
    attachments[0].Samples        = ESampleCount::Count1;
    attachments[0].LoadOp         = ELoadOp::Clear;
    attachments[0].StoreOp        = EStoreOp::Store;
    attachments[0].StencilLoadOp  = ELoadOp::DontCare;
    attachments[0].StencilStoreOp = EStoreOp::DontCare;
    attachments[0].InitialLayout  = EImageLayout::Undefined;
    attachments[0].FinalLayout    = EImageLayout::PresentSrc;

    // 附件 1: 深度附件 — 使用 FDepthPrePass 写入的深度数据 (LoadOp=Load)
    // InitialLayout=DepthStencilAttachment (prepass 后深度缓冲的布局)
    // FinalLayout=DepthStencilAttachment (保持)
    attachments[1].Format         = EPixelFormat::D32_SFLOAT;
    attachments[1].Samples        = ESampleCount::Count1;
    attachments[1].LoadOp         = ELoadOp::Load;
    attachments[1].StoreOp        = EStoreOp::DontCare;
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

    // 子通道依赖 — 确保 FDepthPrePass 的深度写入对本 Pass 可见
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
    renderPassDesc.DebugName       = "ForwardPass_RenderPass";

    return device->CreateRenderPass(renderPassDesc, m_RenderPass);
}

// ============================================================================
// CreateFramebuffers — 每图像一个 Framebuffer [颜色, 共享深度]
// ============================================================================

ERHIResult FForwardPass::CreateFramebuffers(IRHIDevice*           device,
                                              FRHISwapchainHandle   swapchain,
                                              FRHIExtent2D          extent,
                                              UInt32                imageCount,
                                              FRHITextureViewHandle sharedDepthView)
{
    m_Framebuffers.Reserve(imageCount);

    for (UInt32 i = 0; i < imageCount; ++i)
    {
        FRHITextureViewHandle colorView =
            device->GetSwapchainImageView(swapchain, i);

        FRHITextureViewHandle fbAttachments[2] =
        {
            colorView,
            sharedDepthView
        };

        FRHIFramebufferDesc fbDesc = {};
        fbDesc.RenderPass      = m_RenderPass;
        fbDesc.Attachments     = fbAttachments;
        fbDesc.AttachmentCount = 2;
        fbDesc.Width           = extent.Width;
        fbDesc.Height          = extent.Height;
        fbDesc.Layers          = 1;
        fbDesc.DebugName       = "ForwardPass_Framebuffer";

        FRHIFramebufferHandle framebuffer;
        ERHIResult result = device->CreateFramebuffer(fbDesc, framebuffer);
        if (!IsRHISuccess(result))
        {
            return result;
        }

        m_Framebuffers.Add(framebuffer);
    }

    LIMX_LOG(LogRenderer, Log,
             "[ForwardPass] Framebuffer 创建完成 — {} 个, {}x{}",
             imageCount, extent.Width, extent.Height);

    return ERHIResult::Success;
}

// ============================================================================
// DestroyFramebuffers
// ============================================================================

void FForwardPass::DestroyFramebuffers(IRHIDevice* device)
{
    for (SizeType i = 0; i < m_Framebuffers.GetSize(); ++i)
    {
        if (m_Framebuffers[i].IsValid())
        {
            device->DestroyFramebuffer(m_Framebuffers[i]);
        }
    }
    m_Framebuffers.Clear();
}

// ============================================================================
// CreateShaders — 加载 triangle.vert / triangle.frag
// ============================================================================

ERHIResult FForwardPass::CreateShaders(IRHIDevice* device)
{
    FShaderManager& shaderManager = FShaderManager::Get();
    if (!shaderManager.IsInitialized())
    {
        shaderManager.Initialize();
    }

    ERHIResult result = shaderManager.CreateShaderModule(
        device,
        FString("Builtin/pbr.vert"),
        EShaderStage::Vertex,
        m_VertShader);
    if (!IsRHISuccess(result))
    {
        return result;
    }

    result = shaderManager.CreateShaderModule(
        device,
        FString("Builtin/pbr.frag"),
        EShaderStage::Fragment,
        m_FragShader);

    return result;
}

// ============================================================================
// CreateGraphicsPipeline — Equal 深度测试, 禁止深度写入
// ============================================================================

ERHIResult FForwardPass::CreateGraphicsPipeline(IRHIDevice* device)
{
    FRHIGraphicsPipelineDesc pipelineDesc = {};

    // ---- 着色器阶段 ----
    pipelineDesc.ShaderStages[0].Stage      = EShaderStage::Vertex;
    pipelineDesc.ShaderStages[0].Shader     = m_VertShader;
    pipelineDesc.ShaderStages[0].EntryPoint = "main";

    pipelineDesc.ShaderStages[1].Stage      = EShaderStage::Fragment;
    pipelineDesc.ShaderStages[1].Shader     = m_FragShader;
    pipelineDesc.ShaderStages[1].EntryPoint = "main";

    pipelineDesc.ShaderStageCount = 2;

    // ---- 顶点输入 — FMeshVertex (44 bytes 交错布局) ----
    FRHIVertexInputBinding vertexBinding = {};
    vertexBinding.Binding   = 0;
    vertexBinding.Stride    = sizeof(FMeshVertex);
    vertexBinding.InputRate = EVertexInputRate::PerVertex;

    // 只声明本管线真正读取的属性 —— 声明了却不消费的属性会让校验层报
    // "not consumed by vertex shader"。TexCoord1 目前无人使用 (光照贴图
    // 尚未实现), 因此不出现在这里; 它仍占据顶点结构中的 8 字节, 步幅不变。
    //
    // 偏移量取自 FMeshVertex 成员而非手写常量: 结构变化时 offsetof 会跟着走,
    // 手写常量只会静默错位。72 字节的布局静态断言在 FAssetTypes.h 中。
    FRHIVertexInputAttribute vertexAttributes[5] = {};

    vertexAttributes[0].Location = 0;
    vertexAttributes[0].Binding  = 0;
    vertexAttributes[0].Format   = EPixelFormat::RGB32_SFLOAT;
    vertexAttributes[0].Offset   = static_cast<UInt32>(
        LIMX_OFFSET_OF(FMeshVertex, Position));

    vertexAttributes[1].Location = 1;
    vertexAttributes[1].Binding  = 0;
    vertexAttributes[1].Format   = EPixelFormat::RGB32_SFLOAT;
    vertexAttributes[1].Offset   = static_cast<UInt32>(
        LIMX_OFFSET_OF(FMeshVertex, Normal));

    vertexAttributes[2].Location = 2;
    vertexAttributes[2].Binding  = 0;
    vertexAttributes[2].Format   = EPixelFormat::RGBA32_SFLOAT;
    vertexAttributes[2].Offset   = static_cast<UInt32>(
        LIMX_OFFSET_OF(FMeshVertex, Tangent));

    vertexAttributes[3].Location = 3;
    vertexAttributes[3].Binding  = 0;
    vertexAttributes[3].Format   = EPixelFormat::RG32_SFLOAT;
    vertexAttributes[3].Offset   = static_cast<UInt32>(
        LIMX_OFFSET_OF(FMeshVertex, TexCoord0));

    vertexAttributes[4].Location = 5;
    vertexAttributes[4].Binding  = 0;
    vertexAttributes[4].Format   = EPixelFormat::RGBA32_SFLOAT;
    vertexAttributes[4].Offset   = static_cast<UInt32>(
        LIMX_OFFSET_OF(FMeshVertex, Color));

    pipelineDesc.VertexInput.Bindings       = &vertexBinding;
    pipelineDesc.VertexInput.BindingCount   = 1;
    pipelineDesc.VertexInput.Attributes     = vertexAttributes;
    pipelineDesc.VertexInput.AttributeCount = 5;

    // ---- 输入装配 ----
    pipelineDesc.InputAssembly.Topology                  = EPrimitiveTopology::TriangleList;
    pipelineDesc.InputAssembly.IsPrimitiveRestartEnabled = false;

    // ---- 光栅化 ----
    pipelineDesc.Rasterization.PolygonMode              = EPolygonMode::Fill;
    pipelineDesc.Rasterization.CullMode                 = ECullMode::Back;
    pipelineDesc.Rasterization.FrontFace                = EFrontFace::CounterClockwise;
    pipelineDesc.Rasterization.LineWidth                = 1.0f;
    pipelineDesc.Rasterization.IsDepthClampEnabled      = false;
    pipelineDesc.Rasterization.IsRasterizerDiscardEnabled = false;
    pipelineDesc.Rasterization.IsDepthBiasEnabled       = false;

    // ---- 多重采样 ----
    pipelineDesc.Multisample.RasterizationSamples  = ESampleCount::Count1;
    pipelineDesc.Multisample.IsSampleShadingEnabled = false;

    // ---- 深度模板 — Equal 测试, 禁止深度写入 (Early-Z 配合) ----
    pipelineDesc.DepthStencil.IsDepthTestEnabled  = true;
    pipelineDesc.DepthStencil.IsDepthWriteEnabled = false;
    pipelineDesc.DepthStencil.DepthCompareOp      = ECompareOp::Equal;

    // ---- 颜色混合 — 不透明直接覆盖 ----
    FRHIColorBlendAttachmentDesc colorBlendAttachment =
        FRHIColorBlendAttachmentDesc::Opaque();

    pipelineDesc.ColorBlend.Attachments      = &colorBlendAttachment;
    pipelineDesc.ColorBlend.AttachmentCount  = 1;
    pipelineDesc.ColorBlend.IsLogicOpEnabled = false;

    // ---- 动态状态 ----
    pipelineDesc.DynamicState.EnabledStates =
        EDynamicState::Viewport | EDynamicState::Scissor;

    // ---- 管线布局与渲染通道 ----
    pipelineDesc.PipelineLayout = m_PipelineLayout;
    pipelineDesc.RenderPass     = m_RenderPass;
    pipelineDesc.SubpassIndex   = 0;
    pipelineDesc.DebugName      = "ForwardPass_Pipeline";

    ERHIResult result =
        device->CreateGraphicsPipeline(pipelineDesc, m_GraphicsPipeline);

    if (IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Log,
                 "[ForwardPass] 图形管线创建完成 (DepthCompareOp=Equal, DepthWrite=false)");
    }

    return result;
}

// ============================================================================
// DestroyPipelineResources
// ============================================================================

void FForwardPass::DestroyPipelineResources(IRHIDevice* device)
{
    device->DestroyGraphicsPipeline(m_GraphicsPipeline);
    device->DestroyShader(m_FragShader);
    device->DestroyShader(m_VertShader);
}

} // namespace Limx
