/*******************************************************************************
 * 文件: FShadowPass.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   方向光阴影 Pass 实现 — 光源视锥拟合、深度渲染、比较采样器
 *
 * 设计哲学:
 *   正交体积按场景包围球而非包围盒拟合 — 包围球在光源旋转时半径不变，
 *   而包围盒在光源方向上的投影长度会随角度变化。用包围盒会让阴影贴图的
 *   有效分辨率随太阳角度抖动，表现为转动光源时阴影边缘"呼吸"。
 *
 *   绘制背面而非正面 — 自遮挡(shadow acne)的根源是深度贴图的量化误差。
 *   只记录背面深度后，正面着色点与记录深度之间隔着整个物体的厚度，
 *   误差远小于这个间隔。代价是薄片几何（叶子、旗帜）没有厚度可用，
 *   因此双面材质仍按双面绘制，靠 bias 兜底。
 *
 * 技术特性:
 *   - 2048² D32 深度贴图，与交换链尺寸无关
 *   - 比较采样器 (CompareOp=LessOrEqual)，硬件 2x2 PCF
 *   - ClampToBorder + 白色边框：贴图之外一律判为无遮挡
 *   - 复用 depth_only 着色器，Masked 材质在阴影中同样镂空
 *
 * 依赖关系:
 *   内部: Renderer/RenderPass/FShadowPass.h, RenderCore/Shaders/FShaderManager.h
 *
 ******************************************************************************/

#include "Renderer/RenderPass/FShadowPass.h"
#include "Renderer/Renderer/FRenderer.h"
#include "RenderCore/Shaders/FShaderManager.h"

namespace Limx
{

LIMX_DECLARE_LOG_CATEGORY(LogRenderer)

// ============================================================================
// Setup — 创建阴影贴图与全部 GPU 资源
// ============================================================================

ERHIResult FShadowPass::Setup(const FPassSetupDesc& desc)
{
    LIMX_CHECK(desc.Device != nullptr);

    m_PipelineLayout = desc.PipelineLayout;

    ERHIResult result = CreateShadowMap(desc.Device);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[ShadowPass] 阴影贴图创建失败");
        return result;
    }

    result = CreateShadowRenderPass(desc.Device);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[ShadowPass] RenderPass 创建失败");
        return result;
    }

    result = CreateFramebuffer(desc.Device);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[ShadowPass] Framebuffer 创建失败");
        return result;
    }

    result = CreateShaders(desc.Device);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[ShadowPass] 着色器创建失败");
        return result;
    }

    for (SizeType variant = 0;
         variant < kPipelineVariantCount && IsRHISuccess(result); ++variant)
    {
        result = CreateShadowPipeline(desc.Device, variant != 0,
                                      m_Pipelines[variant]);
    }

    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[ShadowPass] 管线创建失败");
        return result;
    }

    LIMX_LOG(LogRenderer, Log,
             "[ShadowPass] 初始化完成 — {}x{} D32_SFLOAT",
             kShadowMapSize, kShadowMapSize);

    return ERHIResult::Success;
}

// ============================================================================
// CreateShadowMap — 深度纹理 + 视图 + 比较采样器
// ============================================================================

ERHIResult FShadowPass::CreateShadowMap(IRHIDevice* device)
{
    FRHITextureDesc depthDesc = FRHITextureDesc::DepthStencil(
        kShadowMapSize, kShadowMapSize,
        EPixelFormat::D32_SFLOAT,
        ESampleCount::Count1);
    depthDesc.DebugName = "ShadowMap";

    ERHIResult result = device->CreateTexture(depthDesc, m_ShadowMap);
    if (!IsRHISuccess(result))
    {
        return result;
    }

    FRHITextureViewDesc viewDesc = {};
    viewDesc.Texture         = m_ShadowMap;
    viewDesc.ViewType        = ETextureType::Texture2D;
    viewDesc.Format          = EPixelFormat::D32_SFLOAT;
    viewDesc.BaseMipLevel    = 0;
    viewDesc.MipLevelCount   = 1;
    viewDesc.BaseArrayLayer  = 0;
    viewDesc.ArrayLayerCount = 1;

    result = device->CreateTextureView(viewDesc, m_ShadowMapView);
    if (!IsRHISuccess(result))
    {
        device->DestroyTexture(m_ShadowMap);
        return result;
    }

    // ---- 比较采样器 ----
    //
    // 开启比较后, texture() 返回的不是深度值而是"通过测试的比例"。
    // 硬件会在 2x2 邻域上做一次免费的 PCF, 边缘由此得到一级软化。
    //
    // 寻址用 ClampToBorder + 白色边框: 阴影贴图之外的区域深度视为 1.0
    // (最远), 任何着色点与它比较都会通过, 即判为无遮挡。用 Clamp 的话
    // 边缘那一列纹素会被无限拉伸, 场景边界外会出现一道贯穿画面的假阴影。
    FRHISamplerDesc samplerDesc = {};
    samplerDesc.MinFilter    = EFilter::Linear;
    samplerDesc.MagFilter    = EFilter::Linear;
    samplerDesc.MipmapMode   = ESamplerMipmapMode::Nearest;
    samplerDesc.AddressModeU = ESamplerAddressMode::ClampToBorder;
    samplerDesc.AddressModeV = ESamplerAddressMode::ClampToBorder;
    samplerDesc.AddressModeW = ESamplerAddressMode::ClampToBorder;
    samplerDesc.IsAnisotropyEnabled = false;
    samplerDesc.IsCompareEnabled    = true;
    samplerDesc.CompareOp           = ECompareOp::LessOrEqual;
    samplerDesc.MinLod              = 0.0f;
    samplerDesc.MaxLod              = 1.0f;
    samplerDesc.BorderColor[0]      = 1.0f;
    samplerDesc.BorderColor[1]      = 1.0f;
    samplerDesc.BorderColor[2]      = 1.0f;
    samplerDesc.BorderColor[3]      = 1.0f;

    result = device->CreateSampler(samplerDesc, m_ShadowSampler);
    if (!IsRHISuccess(result))
    {
        device->DestroyTextureView(m_ShadowMapView);
        device->DestroyTexture(m_ShadowMap);
        return result;
    }

    return ERHIResult::Success;
}

// ============================================================================
// CreateShadowRenderPass — 深度专用通道, 结束时转为着色器只读
// ============================================================================

ERHIResult FShadowPass::CreateShadowRenderPass(IRHIDevice* device)
{
    FRHIAttachmentDesc depthAttachment = {};
    depthAttachment.Format         = EPixelFormat::D32_SFLOAT;
    depthAttachment.Samples        = ESampleCount::Count1;
    depthAttachment.LoadOp         = ELoadOp::Clear;
    depthAttachment.StoreOp        = EStoreOp::Store;
    depthAttachment.StencilLoadOp  = ELoadOp::DontCare;
    depthAttachment.StencilStoreOp = EStoreOp::DontCare;
    depthAttachment.InitialLayout  = EImageLayout::Undefined;

    // 通道结束即转为着色器只读 —— 前向 Pass 紧接着要采样它。
    // 由 RenderPass 完成这次转换而非手写屏障: 驱动能把它并入通道的
    // 收尾操作, 且不会漏掉。
    depthAttachment.FinalLayout    = EImageLayout::ShaderReadOnly;

    FRHIAttachmentReference depthRef = {};
    depthRef.AttachmentIndex = 0;
    depthRef.Layout          = EImageLayout::DepthStencilAttachment;

    FRHISubpassDesc subpass = {};
    subpass.ColorAttachments       = nullptr;
    subpass.ColorAttachmentCount   = 0;
    subpass.DepthStencilAttachment = &depthRef;

    // 两条依赖: 进入时等待上一帧的采样读完, 退出时让后续的采样看到写入
    FRHISubpassDependency dependencies[2] = {};

    dependencies[0].SrcSubpass    = 0xFFFFFFFF;
    dependencies[0].DstSubpass    = 0;
    dependencies[0].SrcStageMask  = EPipelineStageFlags::FragmentShader;
    dependencies[0].DstStageMask  = EPipelineStageFlags::EarlyFragmentTests;
    dependencies[0].SrcAccessMask = EAccessFlags::ShaderRead;
    dependencies[0].DstAccessMask = EAccessFlags::DepthStencilAttachmentWrite;

    dependencies[1].SrcSubpass    = 0;
    dependencies[1].DstSubpass    = 0xFFFFFFFF;
    dependencies[1].SrcStageMask  = EPipelineStageFlags::LateFragmentTests;
    dependencies[1].DstStageMask  = EPipelineStageFlags::FragmentShader;
    dependencies[1].SrcAccessMask = EAccessFlags::DepthStencilAttachmentWrite;
    dependencies[1].DstAccessMask = EAccessFlags::ShaderRead;

    FRHIRenderPassDesc renderPassDesc = {};
    renderPassDesc.Attachments     = &depthAttachment;
    renderPassDesc.AttachmentCount = 1;
    renderPassDesc.Subpasses       = &subpass;
    renderPassDesc.SubpassCount    = 1;
    renderPassDesc.Dependencies    = dependencies;
    renderPassDesc.DependencyCount = 2;
    renderPassDesc.DebugName       = "ShadowPass_RenderPass";

    return device->CreateRenderPass(renderPassDesc, m_RenderPass);
}

// ============================================================================
// CreateFramebuffer
// ============================================================================

ERHIResult FShadowPass::CreateFramebuffer(IRHIDevice* device)
{
    FRHIFramebufferDesc fbDesc = {};
    fbDesc.RenderPass      = m_RenderPass;
    fbDesc.Attachments     = &m_ShadowMapView;
    fbDesc.AttachmentCount = 1;
    fbDesc.Width           = kShadowMapSize;
    fbDesc.Height          = kShadowMapSize;
    fbDesc.Layers          = 1;
    fbDesc.DebugName       = "ShadowPass_Framebuffer";

    return device->CreateFramebuffer(fbDesc, m_Framebuffer);
}

// ============================================================================
// CreateShaders — 复用 depth_only
// ============================================================================

ERHIResult FShadowPass::CreateShaders(IRHIDevice* device)
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

    return shaderManager.CreateShaderModule(
        device,
        FString("Builtin/depth_only.frag"),
        EShaderStage::Fragment,
        m_FragShader);
}

// ============================================================================
// CreateShadowPipeline
// ============================================================================

ERHIResult FShadowPass::CreateShadowPipeline(
    IRHIDevice* device, bool isDoubleSided,
    FRHIGraphicsPipelineHandle& outPipeline)
{
    FRHIGraphicsPipelineDesc pipelineDesc = {};

    pipelineDesc.ShaderStages[0].Shader     = m_VertShader;
    pipelineDesc.ShaderStages[0].Stage      = EShaderStage::Vertex;
    pipelineDesc.ShaderStages[0].EntryPoint = "main";

    pipelineDesc.ShaderStages[1].Shader     = m_FragShader;
    pipelineDesc.ShaderStages[1].Stage      = EShaderStage::Fragment;
    pipelineDesc.ShaderStages[1].EntryPoint = "main";

    pipelineDesc.ShaderStageCount = 2;

    // 顶点输入 — 与 depth_only.vert 一致: 位置 + 主 UV
    FRHIVertexInputBinding vertexBinding = {};
    vertexBinding.Binding   = 0;
    vertexBinding.Stride    = sizeof(FMeshVertex);
    vertexBinding.InputRate = EVertexInputRate::PerVertex;

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

    pipelineDesc.InputAssembly.Topology = EPrimitiveTopology::TriangleList;
    pipelineDesc.InputAssembly.IsPrimitiveRestartEnabled = false;

    // ---- 光栅化 ----
    //
    // 单面材质绘制**背面**: 自遮挡的根源是深度贴图的量化误差, 只记录背面
    // 深度后, 正面着色点与记录值之间隔着整个物体的厚度, 误差被这个间隔
    // 完全吸收。双面材质没有厚度可用, 只能照常双面绘制并依赖 bias。
    pipelineDesc.Rasterization.IsDepthClampEnabled  = false;
    pipelineDesc.Rasterization.IsRasterizerDiscardEnabled = false;
    pipelineDesc.Rasterization.PolygonMode          = EPolygonMode::Fill;
    pipelineDesc.Rasterization.CullMode             =
        isDoubleSided ? ECullMode::None : ECullMode::Front;
    pipelineDesc.Rasterization.FrontFace            =
        EFrontFace::CounterClockwise;
    pipelineDesc.Rasterization.IsDepthBiasEnabled   = false;
    pipelineDesc.Rasterization.LineWidth            = 1.0f;

    pipelineDesc.Multisample.RasterizationSamples   = ESampleCount::Count1;
    pipelineDesc.Multisample.IsSampleShadingEnabled = false;

    pipelineDesc.DepthStencil.IsDepthTestEnabled  = true;
    pipelineDesc.DepthStencil.IsDepthWriteEnabled = true;
    pipelineDesc.DepthStencil.DepthCompareOp      = ECompareOp::Less;

    // 无颜色附件
    pipelineDesc.ColorBlend.Attachments      = nullptr;
    pipelineDesc.ColorBlend.AttachmentCount  = 0;
    pipelineDesc.ColorBlend.IsLogicOpEnabled = false;

    pipelineDesc.DynamicState.EnabledStates =
        EDynamicState::Viewport | EDynamicState::Scissor;

    pipelineDesc.PipelineLayout = m_PipelineLayout;
    pipelineDesc.RenderPass     = m_RenderPass;
    pipelineDesc.SubpassIndex   = 0;
    pipelineDesc.DebugName      = isDoubleSided ? "ShadowPass_TwoSided"
                                                : "ShadowPass_Single";

    return device->CreateGraphicsPipeline(pipelineDesc, outPipeline);
}

// ============================================================================
// CreateLightUniforms — 每帧一份光源矩阵 UBO 与 set 0 兼容描述符集
// ============================================================================

ERHIResult FShadowPass::CreateLightUniforms(
    IRHIDevice* device, FRHIDescSetLayoutHandle viewProjLayout,
    FRHITextureViewHandle fillerTextureView, FRHISamplerHandle fillerSampler,
    UInt32 frameCount)
{
    m_LightUniformBuffers.Reserve(frameCount);
    m_LightDescriptorSets.Reserve(frameCount);

    for (UInt32 i = 0; i < frameCount; ++i)
    {
        FRHIBufferDesc bufferDesc =
            FRHIBufferDesc::Uniform(sizeof(FViewProjUBO));
        bufferDesc.DebugName = "ShadowLightViewProjUBO";

        FRHIBufferHandle buffer;
        ERHIResult result = device->CreateBuffer(bufferDesc, buffer);
        if (!IsRHISuccess(result))
        {
            return result;
        }

        FRHIDescriptorSetHandle descSet;
        result = device->AllocateDescriptorSet(viewProjLayout, descSet);
        if (!IsRHISuccess(result))
        {
            return result;
        }

        // binding 0 = 光源矩阵。binding 1 是 set 0 布局里的纹理槽位,
        // depth_only.frag 并不采样它, 但描述符集不该留空 —— 填一张
        // 现成的纹理比依赖"未被静态使用的描述符无需有效"这条规则更稳妥。
        FRHIDescriptorWrite writes[2];

        writes[0] = FRHIDescriptorWrite::UniformBuffer(
            descSet, 0, buffer, 0, sizeof(FViewProjUBO));

        writes[1] = FRHIDescriptorWrite::CombinedImageSampler(
            descSet, 1, fillerTextureView, fillerSampler,
            EImageLayout::ShaderReadOnly);

        device->UpdateDescriptorSets(writes, 2);

        m_LightUniformBuffers.Add(buffer);
        m_LightDescriptorSets.Add(descSet);
    }

    return ERHIResult::Success;
}

// ============================================================================
// SetLightAndBounds — 由场景包围盒拟合光源正交视锥
// ============================================================================

void FShadowPass::SetLightAndBounds(const FVector3& lightDirection,
                                     const FBoundingBox& sceneBounds)
{
    if (!sceneBounds.IsValid())
    {
        m_HasValidLight = false;
        return;
    }

    const FVector3 direction = lightDirection.GetSafeNormal();

    if (direction.LengthSquared() < 0.5f)
    {
        m_HasValidLight = false;
        return;
    }

    m_LightDirection = direction;

    // ---- 用包围球而非包围盒 ----
    //
    // 包围球的半径与光源方向无关, 因此正交体积的尺寸在光源转动时保持恒定,
    // 阴影贴图的有效分辨率也就恒定。改用包围盒的话, 盒在光源方向上的投影
    // 长度随角度变化, 转动太阳时阴影边缘会"呼吸"。
    const FVector3 center = sceneBounds.GetCenter();
    const Float32  radius = (sceneBounds.Max - center).Length();

    // 光源放在包围球外一个半径处, 保证整个球都落在近平面之后
    const FVector3 eye = center - direction * (radius * 2.0f);

    // 上方向避开与光线平行 —— 平行时 LookAt 的叉积退化为零向量
    const FVector3 up = (FMath::Abs(direction.Y) > 0.99f)
                            ? FVector3(0.0f, 0.0f, 1.0f)
                            : FVector3(0.0f, 1.0f, 0.0f);

    const FMatrix view = FMatrix::LookAt(eye, center, up);

    const FMatrix projection = FMatrix::Ortho(
        -radius, radius, -radius, radius, 0.0f, radius * 4.0f);

    m_ShadowViewProj = projection * view;
    m_HasValidLight  = true;
}

// ============================================================================
// UpdateLightUniform
// ============================================================================

void FShadowPass::UpdateLightUniform(IRHIDevice* device, UInt32 frameIndex)
{
    if (frameIndex >= m_LightUniformBuffers.GetSize())
    {
        return;
    }

    // depth_only.vert 读的是 view 与 proj 两个矩阵并相乘。这里把合成好的
    // 矩阵放进 proj, view 置为单位阵 —— 相乘结果不变, 而阴影贴图与
    // 片段着色器用的是**同一个** m_ShadowViewProj, 不存在两处各算一遍
    // 而出现细微差异的可能。
    FViewProjUBO uboData;
    uboData.View = FMatrix::Identity();
    uboData.Proj = m_ShadowViewProj;

    void* mapped = nullptr;
    if (IsRHISuccess(device->MapBuffer(m_LightUniformBuffers[frameIndex],
                                       &mapped)))
    {
        Memory::MemCopy(mapped, &uboData, sizeof(FViewProjUBO));
        device->UnmapBuffer(m_LightUniformBuffers[frameIndex]);
    }
}

// ============================================================================
// Execute — 从光源视角绘制场景深度
// ============================================================================

void FShadowPass::Execute(IRHICommandBuffer*        commandBuffer,
                           const FRenderPassContext& context)
{
    if (!m_HasValidLight)
    {
        return;
    }

    commandBuffer->BeginDebugLabel("ShadowPass", 0.9f, 0.9f, 0.3f);

    FRHIClearDepthStencilValue clearDepth = {};
    clearDepth.Depth   = 1.0f;
    clearDepth.Stencil = 0;

    FRHIRenderPassBeginInfo beginInfo = {};
    beginInfo.RenderPass        = m_RenderPass;
    beginInfo.Framebuffer       = m_Framebuffer;
    beginInfo.RenderAreaOffset  = { 0, 0 };
    beginInfo.RenderAreaExtent  = { kShadowMapSize, kShadowMapSize };
    beginInfo.ClearColors       = nullptr;
    beginInfo.ClearColorCount   = 0;
    beginInfo.ClearDepthStencil = &clearDepth;

    commandBuffer->BeginRenderPass(beginInfo);

    FRHIViewport viewport = {};
    viewport.X        = 0.0f;
    viewport.Y        = 0.0f;
    viewport.Width    = static_cast<Float32>(kShadowMapSize);
    viewport.Height   = static_cast<Float32>(kShadowMapSize);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    commandBuffer->SetViewport(viewport);

    FRHIScissorRect scissor = {};
    scissor.X      = 0;
    scissor.Y      = 0;
    scissor.Width  = kShadowMapSize;
    scissor.Height = kShadowMapSize;

    commandBuffer->SetScissor(scissor);

    // set 0 = 光源矩阵 (而非相机矩阵)
    if (context.FrameIndex < m_LightDescriptorSets.GetSize())
    {
        commandBuffer->BindDescriptorSet(
            EPipelineBindPoint::Graphics,
            context.PipelineLayout,
            0,
            m_LightDescriptorSets[context.FrameIndex],
            nullptr,
            0);
    }

    // 只绘制不透明与蒙版批次 —— 半透明不投射不透明阴影, 把它们画进深度图
    // 会让玻璃在地面上留下一块实心黑影。
    if (context.RenderObjects != nullptr)
    {
        FRHIGraphicsPipelineHandle boundPipeline;
        FRHIDescriptorSetHandle    boundMaterial;
        FRHIBufferHandle           boundVertexBuffer;
        FRHIBufferHandle           boundIndexBuffer;
        EIndexType                 boundIndexType = EIndexType::UInt32;

        for (SizeType i = 0; i < context.RenderObjects->GetSize(); ++i)
        {
            const FRenderObject& obj = (*context.RenderObjects)[i];

            const FRHIGraphicsPipelineHandle pipeline =
                SelectPipeline(obj.IsDoubleSided);

            if (pipeline.Packed != boundPipeline.Packed)
            {
                commandBuffer->BindGraphicsPipeline(pipeline);
                boundPipeline = pipeline;
            }

            // 材质集用于 Masked 的 alpha 测试 —— 镂空必须在阴影里同样成立,
            // 否则叶片会投出实心方块的影子。
            if (obj.MaterialDescriptorSet.Packed != boundMaterial.Packed)
            {
                commandBuffer->BindDescriptorSet(
                    EPipelineBindPoint::Graphics,
                    context.PipelineLayout,
                    1,
                    obj.MaterialDescriptorSet,
                    nullptr,
                    0);

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
                &pushData);

            commandBuffer->DrawIndexed(obj.IndexCount, 1, obj.IndexOffset,
                                       0, 0);
        }
    }

    commandBuffer->EndRenderPass();
    commandBuffer->EndDebugLabel();
}

// ============================================================================
// OnResize — 阴影贴图与交换链尺寸无关, 无需重建
// ============================================================================

ERHIResult FShadowPass::OnResize(IRHIDevice*           device,
                                  FRHISwapchainHandle   swapchain,
                                  FRHIExtent2D          newExtent,
                                  UInt32                swapchainImageCount,
                                  FRHITextureHandle     newSharedDepth,
                                  FRHITextureViewHandle newSharedDepthView)
{
    // 阴影贴图是光源空间的固定分辨率资源 —— 窗口缩放不该让阴影质量抖动
    (void)device;
    (void)swapchain;
    (void)newExtent;
    (void)swapchainImageCount;
    (void)newSharedDepth;
    (void)newSharedDepthView;

    return ERHIResult::Success;
}

// ============================================================================
// ReleaseSwapchainResources — 阴影贴图与交换链无关, 无需释放
// ============================================================================

void FShadowPass::ReleaseSwapchainResources(IRHIDevice* device)
{
    (void)device;
}

// ============================================================================
// Shutdown
// ============================================================================

void FShadowPass::Shutdown(IRHIDevice* device)
{
    if (device == nullptr)
    {
        return;
    }

    for (SizeType i = 0; i < m_LightUniformBuffers.GetSize(); ++i)
    {
        device->DestroyBuffer(m_LightUniformBuffers[i]);
    }
    m_LightUniformBuffers.Clear();
    m_LightDescriptorSets.Clear();

    for (SizeType variant = 0; variant < kPipelineVariantCount; ++variant)
    {
        device->DestroyGraphicsPipeline(m_Pipelines[variant]);
    }

    device->DestroyShader(m_FragShader);
    device->DestroyShader(m_VertShader);

    device->DestroyFramebuffer(m_Framebuffer);
    device->DestroyRenderPass(m_RenderPass);

    device->DestroySampler(m_ShadowSampler);
    device->DestroyTextureView(m_ShadowMapView);
    device->DestroyTexture(m_ShadowMap);

    LIMX_LOG(LogRenderer, Log, "[ShadowPass] 已关闭");
}

} // namespace Limx
