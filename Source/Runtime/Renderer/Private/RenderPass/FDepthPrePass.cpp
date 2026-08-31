// ============================================================
// 文件名称：FDepthPrePass.cpp
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：极简深度渲染 — 深度预 Pass 只关注几何遮挡关系，
//          不执行任何材质/光照计算，以最低 GPU 开销将整个场景
//          的深度信息写入共享深度缓冲区，为 FForwardPass 的 Equal
//          深度测试提供精确的遮挡数据。
// 功能描述：FDepthPrePass 完整实现 — 创建 depth-only RenderPass
//          (无颜色附件)，使用 depth_only.vert/frag 着色器和共享深度视图
//          创建单一 Framebuffer，渲染所有场景物体写入深度缓冲区。
//          Execute 结束后插入管线屏障，确保深度写入完成前不进入
//          后续 FForwardPass 的 EarlyFragmentTests 阶段。
// 技术特性：depth-only RenderPass: 无颜色附件, 单深度附件 Clear/Store;
//          单一 Framebuffer (共享深度视图);
//          Pipeline: DepthCompareOp=Less, DepthWrite=true, 无颜色混合;
//          Execute 末尾: LateFragmentTests→EarlyFragmentTests 管线屏障。
//
// ── 函数表 ──────────────────────────────────────────────────
// │ 函数名                       │ 描述                            │
// │─────────────────────────────│────────────────────────────────│
// │ Setup()                     │ 创建深度 RenderPass/FB/Shader/管线 │
// │ Execute()                   │ 录制深度渲染 + 管线屏障            │
// │ OnResize()                  │ 重建深度 Framebuffer              │
// │ Shutdown()                  │ 释放所有 GPU 资源                 │
// │ CreateDepthRenderPass()     │ 创建仅含深度附件的渲染通道          │
// │ CreateDepthFramebuffer()    │ 创建 depth-only 帧缓冲             │
// │ DestroyDepthFramebuffer()   │ 销毁帧缓冲                        │
// │ CreateShaders()             │ 加载 depth_only.vert/frag        │
// │ CreateDepthPipeline()       │ 创建 depth-only 图形管线           │
// │ DestroyPipelineResources()  │ 销毁管线和着色器                   │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M0.5 Early-Z 深度预 Pass) │
// ============================================================

#include "Renderer/RenderPass/FDepthPrePass.h"
#include "RenderCore/Shaders/FShaderManager.h"

namespace Limx
{

LIMX_DECLARE_LOG_CATEGORY(LogRenderer)

// ============================================================================
// Setup — 创建全部 GPU 资源
// ============================================================================

ERHIResult FDepthPrePass::Setup(const FPassSetupDesc& desc)
{
    LIMX_CHECK(desc.Device != nullptr);

    m_PipelineLayout      = desc.PipelineLayout;
    m_SharedDepthTexture  = desc.SharedDepthTexture;
    m_SwapchainExtent     = desc.SwapchainExtent;

    // 1. 创建 depth-only 渲染通道
    ERHIResult result = CreateDepthRenderPass(desc.Device);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
                 "[DepthPrePass] depth-only RenderPass 创建失败");
        return result;
    }

    // 2. 创建 depth-only Framebuffer (单一，使用共享深度视图)
    result = CreateDepthFramebuffer(desc.Device,
                                     desc.SwapchainExtent,
                                     desc.SharedDepthTextureView);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
                 "[DepthPrePass] depth-only Framebuffer 创建失败");
        return result;
    }

    // 3. 加载 depth_only 着色器模块
    result = CreateShaders(desc.Device);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
                 "[DepthPrePass] 着色器模块创建失败");
        return result;
    }

    // 4. 创建 depth-only 图形管线
    for (SizeType variant = 0;
         variant < kPipelineVariantCount && IsRHISuccess(result); ++variant)
    {
        result = CreateDepthPipeline(desc.Device, variant != 0,
                                     m_DepthPipelines[variant]);
    }
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
                 "[DepthPrePass] depth-only 管线创建失败");
        return result;
    }

    LIMX_LOG(LogRenderer, Log,
             "[DepthPrePass] 初始化完成 — {}x{} (depth-only)",
             desc.SwapchainExtent.Width, desc.SwapchainExtent.Height);

    return ERHIResult::Success;
}

// ============================================================================
// Execute — 录制深度渲染命令 + 插入管线屏障
// ============================================================================

// ============================================================================
// 录制辅助 — 内联路径与并行路径共用
//
// 与前向、阴影两个 Pass 同一模式: 次级命令缓冲区不继承任何绑定状态,
// 公共状态每段各设一次; 绘制代码只有一份, 逐像素比对才检验得了并行本身。
// ============================================================================

void FDepthPrePass::RecordCommonState(IRHICommandBuffer*        commandBuffer,
                                       const FRenderPassContext& context)
{

    // ================================================================
    // 绑定 depth-only 管线 + 设置动态状态
    // ================================================================

    // 管线改为逐物体惰性绑定 —— 单面/双面剔除必须与前向 Pass 对齐

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
    // 绑定描述符集 (set 0 — ViewProj UBO，深度着色器中使用)
    // ================================================================

    commandBuffer->BindDescriptorSet(
        EPipelineBindPoint::Graphics,
        context.PipelineLayout,
        0,
        context.ViewProjDescriptorSet,
        nullptr,
        0
    );

    // set 1 = bindless 材质表 —— Masked 材质的 alpha 测试要读 albedo
    commandBuffer->BindDescriptorSet(
        EPipelineBindPoint::Graphics,
        context.PipelineLayout,
        1,
        context.BindlessDescriptorSet,
        nullptr,
        0);

    // ================================================================
    // 遍历所有渲染物体: BindVBO/IBO → Push Model → DrawIndexed
    // (仅写入深度，不执行片段着色)
    // ================================================================

}

void FDepthPrePass::RecordRange(IRHICommandBuffer*        commandBuffer,
                                 const FRenderPassContext& context,
                                 SizeType                  begin,
                                 SizeType                  end)
{
    if (context.RenderObjects != nullptr)
    {
        // 材质集必须绑定 —— Masked 材质要在这里做与前向 Pass **完全相同**的
        // alpha 测试。不测的话, 深度预 Pass 会为完全透明的纹素写入深度,
        // 把它背后的东西挡掉; 而前向 Pass 又把这些纹素 discard 掉,
        // 结果是植被叶片之间出现挖空的黑洞。
        //
        // 两个 Pass 的裁剪结论必须逐纹素一致, 否则 DepthCompareOp=Equal
        // 的 Early-Z 会在边缘处失配。
        FRHIGraphicsPipelineHandle boundPipeline;
        FRHIDescriptorSetHandle boundMaterial;
        FRHIBufferHandle boundVertexBuffer;
        FRHIBufferHandle boundIndexBuffer;
        EIndexType       boundIndexType = EIndexType::UInt32;

        for (SizeType i = begin; i < end; ++i)
        {
            const FRenderObject& obj = (*context.RenderObjects)[i];

            const FRHIGraphicsPipelineHandle pipeline =
                SelectPipeline(obj.IsDoubleSided);

            if (pipeline.Packed != boundPipeline.Packed)
            {
                commandBuffer->BindGraphicsPipeline(pipeline);
                boundPipeline = pipeline;
            }

            // set 1 已在本段开头绑过一次 (bindless 全局表), 逐 draw 不再绑。
            //
            // 这正是 bindless 的意义: 材质切换从"绑一次描述符集"降级为
            // "push constant 里换一个整数"。GPU 驱动渲染更进一步 ——
            // 间接绘制根本没有逐 draw 绑描述符集这回事。

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
            pushData.Model         = obj.Transform.ToMatrix();
            pushData.MaterialIndex = obj.BindlessMaterialIndex;

            commandBuffer->PushConstants(
                context.PipelineLayout,
                EShaderStage::Vertex | EShaderStage::Fragment,
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

}

void FDepthPrePass::Execute(IRHICommandBuffer*        commandBuffer,
                             const FRenderPassContext& context)
{
    commandBuffer->BeginDebugLabel("DepthPrePass", 1.0f, 0.8f, 0.2f);

    // ================================================================
    // 清除深度值 — 最大深度 1.0
    // ================================================================

    FRHIClearDepthStencilValue clearDepth = {};
    clearDepth.Depth   = 1.0f;
    clearDepth.Stencil = 0;

    // ================================================================
    // 开始深度渲染通道 (仅深度附件，无颜色)
    // ================================================================

    FRHIRenderPassBeginInfo beginInfo = {};
    beginInfo.RenderPass        = m_DepthRenderPass;
    beginInfo.Framebuffer       = m_DepthFramebuffer;
    beginInfo.RenderAreaOffset  = { 0, 0 };
    beginInfo.RenderAreaExtent  = context.SwapchainExtent;
    beginInfo.ClearColors       = nullptr;
    beginInfo.ClearColorCount   = 0;
    beginInfo.ClearDepthStencil = &clearDepth;

    // 并行录制时通道内容必须来自次级缓冲区
    const bool useParallel =
        (m_Recorder != nullptr) && m_Recorder->IsInitialized();

    beginInfo.UseSecondaryCommandBuffers = useParallel;

    commandBuffer->BeginRenderPass(beginInfo);

    const SizeType objectCount =
        (context.RenderObjects != nullptr) ? context.RenderObjects->GetSize()
                                           : 0;

    if (useParallel)
    {
        FRHICommandBufferInheritance inheritance;
        inheritance.RenderPass  = m_DepthRenderPass;
        inheritance.Subpass     = 0;
        inheritance.Framebuffer = m_DepthFramebuffer;

        const FRecorderBatch batch = m_Recorder->RecordSegmented(
            objectCount, inheritance,
            [this, &context](IRHICommandBuffer* segmentBuffer,
                             SizeType begin, SizeType end)
            {
                RecordCommonState(segmentBuffer, context);
                RecordRange(segmentBuffer, context, begin, end);
            });

        m_Recorder->ExecuteInto(commandBuffer, batch);
    }
    else
    {
        RecordCommonState(commandBuffer, context);
        RecordRange(commandBuffer, context, 0, objectCount);
    }

    commandBuffer->EndRenderPass();

    // ================================================================
    // 管线屏障 — 确保深度写入完成后 FForwardPass 才读取深度
    // srcStage: LateFragmentTests (深度写入发生在这里)
    // dstStage: EarlyFragmentTests (ForwardPass 读取深度在这里)
    // ================================================================

    FRHIImageMemoryBarrier depthBarrier = {};
    depthBarrier.SrcAccessMask  = EAccessFlags::DepthStencilAttachmentWrite;
    depthBarrier.DstAccessMask  = EAccessFlags::DepthStencilAttachmentRead;
    depthBarrier.OldLayout      = EImageLayout::DepthStencilAttachment;
    depthBarrier.NewLayout      = EImageLayout::DepthStencilAttachment;
    depthBarrier.Texture        = m_SharedDepthTexture;
    depthBarrier.BaseMipLevel   = 0;
    depthBarrier.MipLevelCount  = 1;
    depthBarrier.BaseArrayLayer = 0;
    depthBarrier.ArrayLayerCount = 1;

    commandBuffer->PipelineBarrier(
        EPipelineStageFlags::LateFragmentTests,
        EPipelineStageFlags::EarlyFragmentTests,
        nullptr, 0,
        nullptr, 0,
        &depthBarrier, 1
    );

    commandBuffer->EndDebugLabel();
}

// ============================================================================
// OnResize — 重建共享深度 Framebuffer
// ============================================================================

ERHIResult FDepthPrePass::OnResize(const FPassResizeDesc& desc)
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

    m_SwapchainExtent    = newExtent;
    m_SharedDepthTexture = newSharedDepth;

    DestroyDepthFramebuffer(device);

    ERHIResult result = CreateDepthFramebuffer(device,
                                                newExtent,
                                                newSharedDepthView);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
                 "[DepthPrePass] Resize: Framebuffer 重建失败");
    }

    return result;
}

// ============================================================================
// ReleaseSwapchainResources — 释放尺寸相关资源
// ============================================================================

void FDepthPrePass::ReleaseSwapchainResources(IRHIDevice* device)
{
    if (device == nullptr)
    {
        return;
    }

    DestroyDepthFramebuffer(device);
}

// ============================================================================
// Shutdown — 释放所有 GPU 资源
// ============================================================================

void FDepthPrePass::Shutdown(IRHIDevice* device)
{
    if (device == nullptr)
    {
        return;
    }

    DestroyPipelineResources(device);
    DestroyDepthFramebuffer(device);
    device->DestroyRenderPass(m_DepthRenderPass);

    LIMX_LOG(LogRenderer, Log, "[DepthPrePass] 已关闭");
}

// ============================================================================
// CreateDepthRenderPass — 仅含深度附件的渲染通道
// ============================================================================

ERHIResult FDepthPrePass::CreateDepthRenderPass(IRHIDevice* device)
{
    // 单一深度附件 — D32_SFLOAT, Clear→Store, Undefined→DepthStencilAttachment
    FRHIAttachmentDesc depthAttachment = {};
    depthAttachment.Format         = EPixelFormat::D32_SFLOAT;
    depthAttachment.Samples        = ESampleCount::Count1;
    depthAttachment.LoadOp         = ELoadOp::Clear;
    depthAttachment.StoreOp        = EStoreOp::Store;
    depthAttachment.StencilLoadOp  = ELoadOp::DontCare;
    depthAttachment.StencilStoreOp = EStoreOp::DontCare;
    depthAttachment.InitialLayout  = EImageLayout::Undefined;
    depthAttachment.FinalLayout    = EImageLayout::DepthStencilAttachment;

    FRHIAttachmentReference depthRef = {};
    depthRef.AttachmentIndex = 0;
    depthRef.Layout          = EImageLayout::DepthStencilAttachment;

    // 子通道: 无颜色附件，仅深度附件
    FRHISubpassDesc subpass = {};
    subpass.ColorAttachments       = nullptr;
    subpass.ColorAttachmentCount   = 0;
    subpass.DepthStencilAttachment = &depthRef;

    // 子通道依赖: 外部 → 子通道 0 (深度附件写入)
    FRHISubpassDependency dependency = {};
    dependency.SrcSubpass    = 0xFFFFFFFF;
    dependency.DstSubpass    = 0;
    dependency.SrcStageMask  = EPipelineStageFlags::TopOfPipe;
    dependency.DstStageMask  = EPipelineStageFlags::EarlyFragmentTests;
    dependency.SrcAccessMask = EAccessFlags::None;
    dependency.DstAccessMask = EAccessFlags::DepthStencilAttachmentWrite;

    FRHIRenderPassDesc renderPassDesc = {};
    renderPassDesc.Attachments     = &depthAttachment;
    renderPassDesc.AttachmentCount = 1;
    renderPassDesc.Subpasses       = &subpass;
    renderPassDesc.SubpassCount    = 1;
    renderPassDesc.Dependencies    = &dependency;
    renderPassDesc.DependencyCount = 1;
    renderPassDesc.DebugName       = "DepthPrePass_RenderPass";

    return device->CreateRenderPass(renderPassDesc, m_DepthRenderPass);
}

// ============================================================================
// CreateDepthFramebuffer — 单一 depth-only Framebuffer
// ============================================================================

ERHIResult FDepthPrePass::CreateDepthFramebuffer(
    IRHIDevice*           device,
    FRHIExtent2D          extent,
    FRHITextureViewHandle sharedDepthView)
{
    FRHIFramebufferDesc fbDesc = {};
    fbDesc.RenderPass      = m_DepthRenderPass;
    fbDesc.Attachments     = &sharedDepthView;
    fbDesc.AttachmentCount = 1;
    fbDesc.Width           = extent.Width;
    fbDesc.Height          = extent.Height;
    fbDesc.Layers          = 1;
    fbDesc.DebugName       = "DepthPrePass_Framebuffer";

    ERHIResult result = device->CreateFramebuffer(fbDesc, m_DepthFramebuffer);

    if (IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Log,
                 "[DepthPrePass] Framebuffer 创建完成 — {}x{} (depth-only)",
                 extent.Width, extent.Height);
    }

    return result;
}

// ============================================================================
// DestroyDepthFramebuffer
// ============================================================================

void FDepthPrePass::DestroyDepthFramebuffer(IRHIDevice* device)
{
    if (m_DepthFramebuffer.IsValid())
    {
        device->DestroyFramebuffer(m_DepthFramebuffer);
    }
}

// ============================================================================
// CreateShaders — 加载 depth_only.vert / depth_only.frag
// ============================================================================

ERHIResult FDepthPrePass::CreateShaders(IRHIDevice* device)
{
    FShaderManager& shaderManager = FShaderManager::Get();
    if (!shaderManager.IsInitialized())
    {
        shaderManager.Initialize();
    }

    ERHIResult result = shaderManager.CreateShaderModule(
        device,
        FString("Builtin/depth_only.vert"),
        EShaderStage::Vertex,
        m_VertShader);
    if (!IsRHISuccess(result))
    {
        return result;
    }

    result = shaderManager.CreateShaderModule(
        device,
        FString("Builtin/depth_only.frag"),
        EShaderStage::Fragment,
        m_FragShader);

    return result;
}

// ============================================================================
// CreateDepthPipeline — depth-only 管线 (Less 深度测试, 深度写入, 无颜色混合)
// ============================================================================

ERHIResult FDepthPrePass::CreateDepthPipeline(
    IRHIDevice* device, bool isDoubleSided,
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

    // ---- 顶点输入 — 与 FMeshVertex 完全兼容 (必须匹配 VBO 步幅) ----
    FRHIVertexInputBinding vertexBinding = {};
    vertexBinding.Binding   = 0;
    vertexBinding.Stride    = sizeof(FMeshVertex);
    vertexBinding.InputRate = EVertexInputRate::PerVertex;

    // 只声明本管线真正读取的属性 —— 声明了却不消费的属性会让校验层报
    // "not consumed by vertex shader"。步幅仍是完整的 FMeshVertex,
    // 属性按偏移量取值, 与缓冲区里其余字段的存在与否无关。
    //
    // 偏移量取自 FMeshVertex 成员而非手写常量: 结构变化时 offsetof 会跟着走,
    // 手写常量只会静默错位。72 字节的布局静态断言在 FAssetTypes.h 中。
    // 位置用于变换, UV 用于 Masked 材质的 alpha 测试。
    // Location 不必连续 —— 保持与 FMeshVertex 在前向管线中的编号一致,
    // 两条管线用同一套 location 号能避免"同一个属性在不同 Pass 里编号不同"
    // 这种极易看漏的错。
    FRHIVertexInputAttribute vertexAttributes[2] = {};

    vertexAttributes[0].Location = 0;
    vertexAttributes[0].Binding  = 0;
    vertexAttributes[0].Format   = EPixelFormat::RGB32_SFLOAT;
    vertexAttributes[0].Offset   = static_cast<UInt32>(
        LIMX_OFFSET_OF(FMeshVertex, Position));

    vertexAttributes[1].Location = 3;
    vertexAttributes[1].Binding  = 0;
    vertexAttributes[1].Format   = EPixelFormat::RG32_SFLOAT;
    vertexAttributes[1].Offset   = static_cast<UInt32>(
        LIMX_OFFSET_OF(FMeshVertex, TexCoord0));

    pipelineDesc.VertexInput.Bindings       = &vertexBinding;
    pipelineDesc.VertexInput.BindingCount   = 1;
    pipelineDesc.VertexInput.Attributes     = vertexAttributes;
    pipelineDesc.VertexInput.AttributeCount = 2;

    // ---- 输入装配 ----
    pipelineDesc.InputAssembly.Topology                  = EPrimitiveTopology::TriangleList;
    pipelineDesc.InputAssembly.IsPrimitiveRestartEnabled = false;

    // ---- 光栅化 ----
    pipelineDesc.Rasterization.PolygonMode                = EPolygonMode::Fill;
    // 必须与 FForwardPass 对同一物体的选择完全一致
    pipelineDesc.Rasterization.CullMode                   =
        isDoubleSided ? ECullMode::None : ECullMode::Back;
    pipelineDesc.Rasterization.FrontFace                  = EFrontFace::CounterClockwise;
    pipelineDesc.Rasterization.LineWidth                  = 1.0f;
    pipelineDesc.Rasterization.IsDepthClampEnabled        = false;
    pipelineDesc.Rasterization.IsRasterizerDiscardEnabled = false;
    pipelineDesc.Rasterization.IsDepthBiasEnabled         = false;

    // ---- 多重采样 ----
    pipelineDesc.Multisample.RasterizationSamples  = ESampleCount::Count1;
    pipelineDesc.Multisample.IsSampleShadingEnabled = false;

    // ---- 深度模板 — Less 测试, 深度写入开启 ----
    pipelineDesc.DepthStencil.IsDepthTestEnabled  = true;
    pipelineDesc.DepthStencil.IsDepthWriteEnabled = true;
    pipelineDesc.DepthStencil.DepthCompareOp      = ECompareOp::Less;

    // ---- 颜色混合 — depth-only pass 无颜色附件，AttachmentCount=0 ----
    pipelineDesc.ColorBlend.Attachments      = nullptr;
    pipelineDesc.ColorBlend.AttachmentCount  = 0;
    pipelineDesc.ColorBlend.IsLogicOpEnabled = false;

    // ---- 动态状态 ----
    pipelineDesc.DynamicState.EnabledStates =
        EDynamicState::Viewport | EDynamicState::Scissor;

    // ---- 管线布局与渲染通道 ----
    pipelineDesc.PipelineLayout = m_PipelineLayout;
    pipelineDesc.RenderPass     = m_DepthRenderPass;
    pipelineDesc.SubpassIndex   = 0;
    pipelineDesc.DebugName      = "DepthPrePass_Pipeline";

    ERHIResult result =
        device->CreateGraphicsPipeline(pipelineDesc, outPipeline);

    if (IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Log,
                 "[DepthPrePass] depth-only 管线创建完成 (DepthCompareOp=Less, DepthWrite=true)");
    }

    return result;
}

// ============================================================================
// DestroyPipelineResources
// ============================================================================

void FDepthPrePass::DestroyPipelineResources(IRHIDevice* device)
{
    for (SizeType variant = 0; variant < kPipelineVariantCount; ++variant)
    {
        device->DestroyGraphicsPipeline(m_DepthPipelines[variant]);
    }
    device->DestroyShader(m_FragShader);
    device->DestroyShader(m_VertShader);
}

} // namespace Limx
