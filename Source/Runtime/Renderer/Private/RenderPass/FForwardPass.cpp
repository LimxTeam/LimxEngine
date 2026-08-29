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
    m_ColorTargetView = desc.SharedColorTextureView;

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
    for (SizeType variant = 0;
         variant < kPipelineVariantCount && IsRHISuccess(result); ++variant)
    {
        const bool isTranslucent = (variant & 2u) != 0u;
        const bool isDoubleSided = (variant & 1u) != 0u;

        result = CreateGraphicsPipeline(desc.Device, isTranslucent,
                                        isDoubleSided, m_Pipelines[variant]);
    }
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
    // HDR 目标只有一张, 因此只有一个 Framebuffer —— 与交换链图像下标无关
    beginInfo.Framebuffer       = m_Framebuffers[0];
    beginInfo.RenderAreaOffset  = { 0, 0 };
    beginInfo.RenderAreaExtent  = context.SwapchainExtent;
    beginInfo.ClearColors       = &clearColor;
    beginInfo.ClearColorCount   = 1;
    beginInfo.ClearDepthStencil = &clearDepth;

    commandBuffer->BeginRenderPass(beginInfo);

    // ================================================================
    // 设置动态 Viewport/Scissor
    //
    // 管线改为逐物体惰性绑定 —— 单面/双面是两条不同的管线, 在这里固定绑
    // 一条会让另一类物体用错剔除模式。动态状态在管线切换后依然保持,
    // 因此 Viewport/Scissor 仍然只设一次。
    // ================================================================

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
        FRHIGraphicsPipelineHandle boundPipeline;
        FRHIDescriptorSetHandle boundMaterial;
        FRHIBufferHandle        boundVertexBuffer;
        FRHIBufferHandle        boundIndexBuffer;
        EIndexType              boundIndexType = EIndexType::UInt32;

        for (SizeType i = 0; i < context.RenderObjects->GetSize(); ++i)
        {
            const FRenderObject& obj = (*context.RenderObjects)[i];

            // 管线按剔除模式选择 —— 列表已按 IsDoubleSided 聚类,
            // 因此这里最多切换一次。
            const FRHIGraphicsPipelineHandle pipeline =
                SelectPipeline(false, obj.IsDoubleSided);

            if (pipeline.Packed != boundPipeline.Packed)
            {
                commandBuffer->BindGraphicsPipeline(pipeline);
                boundPipeline = pipeline;
            }

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

    // ================================================================
    // 半透明批次 — 同一渲染通道内, 换管线继续绘制
    // ================================================================
    //
    // 放在同一个 RenderPass 里而非另起一个: 半透明要读已经画好的不透明像素
    // 做混合, 另起通道意味着颜色附件要 Store 再 Load 一遍, 在移动端 tile
    // 架构上这是实打实的带宽开销, 在桌面端也白白多两次布局转换。
    //
    // 列表已由 FSceneManager 按到相机的距离由远及近排序 —— 这里只负责按序
    // 提交, 不再做任何重排。
    if (context.TranslucentObjects != nullptr &&
        context.TranslucentObjects->GetSize() > 0)
    {
        // 绑定状态跟踪重新开始 —— 换到半透明是个天然边界, 沿用上一段的
        // 跟踪值只能省下极少几次绑定, 却让"这里到底绑没绑过"变得难以推理。
        FRHIGraphicsPipelineHandle boundPipeline;
        FRHIDescriptorSetHandle boundMaterial;
        FRHIBufferHandle        boundVertexBuffer;
        FRHIBufferHandle        boundIndexBuffer;
        EIndexType              boundIndexType = EIndexType::UInt32;

        for (SizeType i = 0; i < context.TranslucentObjects->GetSize(); ++i)
        {
            const FRenderObject& obj = (*context.TranslucentObjects)[i];

            // 半透明按距离排序, 双面与单面会交替出现, 管线切换次数无法
            // 像不透明那样压到一次 —— 正确的混合顺序优先于状态聚类。
            const FRHIGraphicsPipelineHandle pipeline =
                SelectPipeline(true, obj.IsDoubleSided);

            if (pipeline.Packed != boundPipeline.Packed)
            {
                commandBuffer->BindGraphicsPipeline(pipeline);
                boundPipeline = pipeline;
            }

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

            commandBuffer->DrawIndexed(obj.IndexCount, 1, obj.IndexOffset,
                                       0, 0);
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
                                   FRHITextureViewHandle newSharedDepthView,
                        FRHITextureHandle     newSharedColor,
                        FRHITextureViewHandle newSharedColorView)
{
    m_SwapchainExtent = newExtent;

    // HDR 目标随交换链一同重建, 视图句柄因此换了新的 —— 不更新的话
    // Framebuffer 会挂在已销毁的旧视图上。
    (void)newSharedColor;
    m_ColorTargetView = newSharedColorView;

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
    // 附件 0: HDR 颜色附件 — RGBA16_SFLOAT, 清除→存储
    //
    // 不再直接画进交换链: 光照结果的动态范围远超 [0,1], 写进 8 位归一化的
    // 交换链意味着在色调映射之前就把亮部截断了 —— 那样再好的映射曲线也
    // 无从发挥。最终布局转为着色器只读, 供后处理 Pass 采样。
    //
    // swapchainFormat 因此不再参与本 Pass 的附件格式。
    (void)swapchainFormat;

    FRHIAttachmentDesc attachments[2] = {};

    attachments[0].Format         = EPixelFormat::RGBA16_SFLOAT;
    attachments[0].Samples        = ESampleCount::Count1;
    attachments[0].LoadOp         = ELoadOp::Clear;
    attachments[0].StoreOp        = EStoreOp::Store;
    attachments[0].StencilLoadOp  = ELoadOp::DontCare;
    attachments[0].StencilStoreOp = EStoreOp::DontCare;
    attachments[0].InitialLayout  = EImageLayout::Undefined;
    attachments[0].FinalLayout    = EImageLayout::ShaderReadOnly;

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
    // 只需一个 Framebuffer —— 渲染目标是那一张共享 HDR 纹理, 与交换链
    // 的多缓冲无关。此前每个交换链图像各建一个, 是因为直接画进交换链。
    //
    // 这也意味着相邻帧会写同一张 HDR 纹理: 由帧栅栏保证上一帧已呈现完毕,
    // 与深度缓冲区一直以来的做法相同。
    (void)swapchain;
    (void)imageCount;

    m_Framebuffers.Reserve(1);

    {
        FRHITextureViewHandle fbAttachments[2] =
        {
            m_ColorTargetView,
            sharedDepthView
        };

        FRHIFramebufferDesc fbDesc = {};
        fbDesc.RenderPass      = m_RenderPass;
        fbDesc.Attachments     = fbAttachments;
        fbDesc.AttachmentCount = 2;
        fbDesc.Width           = extent.Width;
        fbDesc.Height          = extent.Height;
        fbDesc.Layers          = 1;
        fbDesc.DebugName       = "ForwardPass_HDRFramebuffer";

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

ERHIResult FForwardPass::CreateGraphicsPipeline(
    IRHIDevice* device, bool isTranslucent, bool isDoubleSided,
    FRHIGraphicsPipelineHandle& outPipeline)
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
    // 双面材质关闭剔除 —— 植被与薄片几何只有一层三角形, 剔掉背面等于
    // 让每片叶子只剩朝向相机的那半边。
    pipelineDesc.Rasterization.CullMode                 =
        isDoubleSided ? ECullMode::None : ECullMode::Back;
    pipelineDesc.Rasterization.FrontFace                = EFrontFace::CounterClockwise;
    pipelineDesc.Rasterization.LineWidth                = 1.0f;
    pipelineDesc.Rasterization.IsDepthClampEnabled      = false;
    pipelineDesc.Rasterization.IsRasterizerDiscardEnabled = false;
    pipelineDesc.Rasterization.IsDepthBiasEnabled       = false;

    // ---- 多重采样 ----
    pipelineDesc.Multisample.RasterizationSamples  = ESampleCount::Count1;
    pipelineDesc.Multisample.IsSampleShadingEnabled = false;

    // ---- 深度模板 ----
    //
    // 不透明: Equal —— 深度已由 FDepthPrePass 精确写入, 相等即为可见表面,
    //         这正是 Early-Z 生效的方式。
    // 半透明: LessOrEqual —— 半透明批次**不参与**深度预 Pass (它们不该写深度,
    //         否则会挡住自己身后的同类), 因此深度缓冲区里没有它们的值,
    //         用 Equal 会把它们全部剔除, 表现为半透明物体彻底消失。
    pipelineDesc.DepthStencil.IsDepthTestEnabled  = true;
    pipelineDesc.DepthStencil.IsDepthWriteEnabled = false;
    pipelineDesc.DepthStencil.DepthCompareOp      =
        isTranslucent ? ECompareOp::LessOrEqual : ECompareOp::Equal;

    // ---- 颜色混合 ----
    FRHIColorBlendAttachmentDesc colorBlendAttachment =
        isTranslucent ? FRHIColorBlendAttachmentDesc::AlphaBlend()
                      : FRHIColorBlendAttachmentDesc::Opaque();

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
    pipelineDesc.DebugName =
        isTranslucent ? (isDoubleSided ? "ForwardPass_Translucent_TwoSided"
                                       : "ForwardPass_Translucent")
                      : (isDoubleSided ? "ForwardPass_Opaque_TwoSided"
                                       : "ForwardPass_Opaque");

    ERHIResult result =
        device->CreateGraphicsPipeline(pipelineDesc, outPipeline);

    if (IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Log,
                 "[ForwardPass] {}{} 管线创建完成 (DepthCompareOp={}, 混合={})",
                 isTranslucent ? "半透明" : "不透明",
                 isDoubleSided ? "/双面" : "/单面",
                 isTranslucent ? "LessOrEqual" : "Equal",
                 isTranslucent ? "AlphaBlend" : "关");
    }

    return result;
}

// ============================================================================
// DestroyPipelineResources
// ============================================================================

void FForwardPass::DestroyPipelineResources(IRHIDevice* device)
{
    for (SizeType variant = 0; variant < kPipelineVariantCount; ++variant)
    {
        device->DestroyGraphicsPipeline(m_Pipelines[variant]);
    }
    device->DestroyShader(m_FragShader);
    device->DestroyShader(m_VertShader);
}

} // namespace Limx
