/*******************************************************************************
 * 文件: FShadowPass.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   方向光阴影 Pass 实现 — 光源视锥拟合、深度渲染、比较采样器
 *
 * 设计哲学:
 *   每级拟合到相机视锥切片的包围球 — 球在相机旋转时半径不变，正交体积
 *   因而尺寸恒定；用视锥角点的 AABB 会让体积随视角摆动，阴影边缘随相机
 *   转动闪烁。进一步把正交中心吸附到纹素网格上，消除亚像素级的抖动。
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

    result = CreateFramebuffers(desc.Device);
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
             "[ShadowPass] 初始化完成 — {} 级级联, 每级 {}x{} D32_SFLOAT",
             kCascadeCount, kShadowMapSize, kShadowMapSize);

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
    depthDesc.Type        = ETextureType::Texture2DArray;
    depthDesc.ArrayLayers = kCascadeCount;
    depthDesc.DebugName   = "ShadowMapArray";

    ERHIResult result = device->CreateTexture(depthDesc, m_ShadowMap);
    if (!IsRHISuccess(result))
    {
        return result;
    }

    // 采样视图 —— 覆盖全部层
    FRHITextureViewDesc arrayViewDesc = {};
    arrayViewDesc.Texture         = m_ShadowMap;
    arrayViewDesc.ViewType        = ETextureType::Texture2DArray;
    arrayViewDesc.Format          = EPixelFormat::D32_SFLOAT;
    arrayViewDesc.BaseMipLevel    = 0;
    arrayViewDesc.MipLevelCount   = 1;
    arrayViewDesc.BaseArrayLayer  = 0;
    arrayViewDesc.ArrayLayerCount = kCascadeCount;

    result = device->CreateTextureView(arrayViewDesc, m_ShadowMapView);
    if (!IsRHISuccess(result))
    {
        device->DestroyTexture(m_ShadowMap);
        return result;
    }

    // 逐层视图 —— 渲染目标必须是单层, 数组视图不能直接作附件
    for (UInt32 cascade = 0; cascade < kCascadeCount; ++cascade)
    {
        FRHITextureViewDesc layerViewDesc = {};
        layerViewDesc.Texture         = m_ShadowMap;
        layerViewDesc.ViewType        = ETextureType::Texture2D;
        layerViewDesc.Format          = EPixelFormat::D32_SFLOAT;
        layerViewDesc.BaseMipLevel    = 0;
        layerViewDesc.MipLevelCount   = 1;
        layerViewDesc.BaseArrayLayer  = cascade;
        layerViewDesc.ArrayLayerCount = 1;

        result = device->CreateTextureView(layerViewDesc,
                                           m_CascadeViews[cascade]);
        if (!IsRHISuccess(result))
        {
            device->DestroyTextureView(m_ShadowMapView);
            device->DestroyTexture(m_ShadowMap);
            return result;
        }
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

ERHIResult FShadowPass::CreateFramebuffers(IRHIDevice* device)
{
    for (UInt32 cascade = 0; cascade < kCascadeCount; ++cascade)
    {
        FRHIFramebufferDesc fbDesc = {};
        fbDesc.RenderPass      = m_RenderPass;
        fbDesc.Attachments     = &m_CascadeViews[cascade];
        fbDesc.AttachmentCount = 1;
        fbDesc.Width           = kShadowMapSize;
        fbDesc.Height          = kShadowMapSize;
        fbDesc.Layers          = 1;
        fbDesc.DebugName       = "ShadowPass_CascadeFramebuffer";

        const ERHIResult result = device->CreateFramebuffer(
            fbDesc, m_CascadeFramebuffers[cascade]);

        if (!IsRHISuccess(result))
        {
            return result;
        }
    }

    return ERHIResult::Success;
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
    const UInt32 totalCount = frameCount * kCascadeCount;

    m_LightUniformBuffers.Reserve(totalCount);
    m_LightDescriptorSets.Reserve(totalCount);

    for (UInt32 i = 0; i < totalCount; ++i)
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
// ComputeCascadeSplits — 对数与均匀的加权切分
// ============================================================================

void FShadowPass::ComputeCascadeSplits(Float32 nearPlane,
                                        Float32 shadowDistance,
                                        UInt32 cascadeCount,
                                        Float32 lambda,
                                        Float32* outSplits)
{
    if (outSplits == nullptr || cascadeCount == 0)
    {
        return;
    }

    // 近平面必须为正 —— 对数切分要对 far/near 取幂, 零或负数会得到
    // 非有限值, 而那些值会一路传到正交矩阵里, 表现为阴影整体消失。
    const Float32 safeNear = (nearPlane > 1.0e-4f) ? nearPlane : 1.0e-4f;

    // 覆盖距离至少比近平面远一个单位, 否则整个级联退化为零厚度
    const Float32 farPlane =
        (shadowDistance > safeNear + 1.0f) ? shadowDistance : (safeNear + 1.0f);

    const Float32 range = farPlane - safeNear;
    const Float32 ratio = farPlane / safeNear;

    const Float32 safeLambda = FMath::Clamp(lambda, 0.0f, 1.0f);

    outSplits[0] = safeNear;

    for (UInt32 i = 1; i <= cascadeCount; ++i)
    {
        const Float32 p = static_cast<Float32>(i) /
                          static_cast<Float32>(cascadeCount);

        const Float32 logSplit     = safeNear * FMath::Pow(ratio, p);
        const Float32 uniformSplit = safeNear + range * p;

        outSplits[i] =
            safeLambda * logSplit + (1.0f - safeLambda) * uniformSplit;
    }

    // 最远一级严格等于覆盖距离 —— 浮点插值在 p=1 处可能差出几个 ulp,
    // 而着色器用 "距离 > 最后一级边界则判为无遮挡", 差一点就会在最远处
    // 留下一圈没有阴影的环带。
    outSplits[cascadeCount] = farPlane;
}

// ============================================================================
// SetLightAndBounds — 由场景包围盒拟合光源正交视锥
// ============================================================================

void FShadowPass::SetLightAndBounds(const FVector3& lightDirection,
                                     const FBoundingBox& sceneBounds,
                                     const FCameraFrustumInfo& cameraInfo)
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

    // ---- 1. 切分 ----
    //
    // λ=0.75 偏向对数, 是常见取值。算法本身见 ComputeCascadeSplits。
    constexpr Float32 kSplitLambda = 0.75f;

    Float32 splitDistances[kCascadeCount + 1];

    ComputeCascadeSplits(cameraInfo.NearPlane, cameraInfo.ShadowDistance,
                         kCascadeCount, kSplitLambda, splitDistances);

    // ---- 2. 逐级拟合 ----
    const FVector3 forward = cameraInfo.Forward.GetSafeNormal();
    const FVector3 right   = FVector3::Cross(forward,
                                             cameraInfo.Up).GetSafeNormal();
    const FVector3 up      = FVector3::Cross(right, forward).GetSafeNormal();

    const Float32 tanHalfFovY = FMath::Tan(cameraInfo.FovY * 0.5f);
    const Float32 tanHalfFovX = tanHalfFovY * cameraInfo.AspectRatio;

    // 光源沿光线方向后撤的距离 —— 取场景包围球直径, 保证相机身后的物体
    // 也落在光源近平面之后。漏掉它, 身后的高塔就不会在地面上投影。
    const Float32 sceneRadius =
        (sceneBounds.Max - sceneBounds.GetCenter()).Length();

    for (UInt32 cascade = 0; cascade < kCascadeCount; ++cascade)
    {
        const Float32 sliceNear = splitDistances[cascade];
        const Float32 sliceFar  = splitDistances[cascade + 1];

        m_CascadeSplits[cascade] = sliceFar;

        // ---- 切片的八个角点 ----
        FVector3 corners[8];
        UInt32   cornerIndex = 0;

        for (UInt32 farSide = 0; farSide < 2; ++farSide)
        {
            const Float32 distance = (farSide == 0) ? sliceNear : sliceFar;
            const FVector3 center  = cameraInfo.Position + forward * distance;
            const Float32 halfH    = tanHalfFovY * distance;
            const Float32 halfW    = tanHalfFovX * distance;

            corners[cornerIndex++] = center - right * halfW - up * halfH;
            corners[cornerIndex++] = center + right * halfW - up * halfH;
            corners[cornerIndex++] = center + right * halfW + up * halfH;
            corners[cornerIndex++] = center - right * halfW + up * halfH;
        }

        // ---- 包围球 ----
        //
        // 球心取八角点的平均, 半径取到最远角点的距离。这个球在相机绕自身
        // 旋转时半径恒定 —— 这正是级联稳定性的来源。
        FVector3 sphereCenter(0.0f, 0.0f, 0.0f);

        for (UInt32 i = 0; i < 8; ++i)
        {
            sphereCenter = sphereCenter + corners[i];
        }

        sphereCenter = sphereCenter * (1.0f / 8.0f);

        Float32 sphereRadius = 0.0f;

        for (UInt32 i = 0; i < 8; ++i)
        {
            const Float32 distance = (corners[i] - sphereCenter).Length();
            sphereRadius = FMath::Max(sphereRadius, distance);
        }

        // 略微放大, 避免边界处因浮点误差漏采
        sphereRadius = FMath::Ceil(sphereRadius * 16.0f) / 16.0f;

        // ---- 光源视图 ----
        const FVector3 lightUp = (FMath::Abs(direction.Y) > 0.99f)
                                     ? FVector3(0.0f, 0.0f, 1.0f)
                                     : FVector3(0.0f, 1.0f, 0.0f);

        const FVector3 eye =
            sphereCenter - direction * (sphereRadius + sceneRadius * 2.0f);

        FMatrix view = FMatrix::LookAt(eye, sphereCenter, lightUp);

        // ---- 纹素吸附 ----
        //
        // 把正交体积的中心对齐到阴影贴图的纹素网格上。不做的话, 相机
        // 每移动一点点, 整个投影就平移不到一个纹素的距离, 阴影边缘随之
        // 抖动 —— 静止时看不出, 一走动就满屏爬行。
        const Float32 texelsPerUnit =
            static_cast<Float32>(kShadowMapSize) / (sphereRadius * 2.0f);

        const FVector3 centerLightSpace = view.TransformPosition(sphereCenter);

        const FVector3 snappedLightSpace(
            FMath::Floor(centerLightSpace.X * texelsPerUnit) / texelsPerUnit,
            FMath::Floor(centerLightSpace.Y * texelsPerUnit) / texelsPerUnit,
            centerLightSpace.Z);

        const FVector3 snapOffset = snappedLightSpace - centerLightSpace;

        const Float32 depthExtent = sphereRadius * 2.0f + sceneRadius * 4.0f;

        FMatrix projection = FMatrix::Ortho(
            -sphereRadius + snapOffset.X, sphereRadius + snapOffset.X,
            -sphereRadius + snapOffset.Y, sphereRadius + snapOffset.Y,
            0.0f, depthExtent);

        m_CascadeViewProj[cascade] = projection * view;
    }

    m_HasValidLight = true;
}

// ============================================================================
// UpdateLightUniform
// ============================================================================

void FShadowPass::UpdateLightUniform(IRHIDevice* device, UInt32 frameIndex)
{
    // 每帧每级一份 UBO —— 下标按 (帧 × 级数 + 级) 展开
    for (UInt32 cascade = 0; cascade < kCascadeCount; ++cascade)
    {
        const SizeType index =
            static_cast<SizeType>(frameIndex) * kCascadeCount + cascade;

        if (index >= m_LightUniformBuffers.GetSize())
        {
            return;
        }

        // depth_only.vert 读的是 view 与 proj 两个矩阵并相乘。这里把合成好
        // 的矩阵放进 proj, view 置为单位阵 —— 相乘结果不变, 而阴影贴图与
        // 片段着色器用的是**同一个** m_CascadeViewProj, 不存在两处各算一遍
        // 而出现细微差异的可能。
        FViewProjUBO uboData;
        uboData.View = FMatrix::Identity();
        uboData.Proj = m_CascadeViewProj[cascade];

        void* mapped = nullptr;
        if (IsRHISuccess(device->MapBuffer(m_LightUniformBuffers[index],
                                           &mapped)))
        {
            Memory::MemCopy(mapped, &uboData, sizeof(FViewProjUBO));
            device->UnmapBuffer(m_LightUniformBuffers[index]);
        }
    }
}

// ============================================================================
// Execute — 从光源视角绘制场景深度
// ============================================================================

// ============================================================================
// 录制辅助 — 内联路径与并行路径共用
//
// 与前向 Pass 同理: 次级命令缓冲区不继承主缓冲区的任何绑定状态, 视口与
// 描述符集必须每段各设一次; 共用同一份绘制代码而不是抄两遍, 逐像素比对
// 失败时才分得清是并行的问题还是抄漏了一行。
// ============================================================================

void FShadowPass::RecordCascadeState(IRHICommandBuffer*        commandBuffer,
                                      const FRenderPassContext& context,
                                      UInt32                    cascade)
{
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

    // set 0 = 本级的光源矩阵 (而非相机矩阵)
    const SizeType uniformIndex =
        static_cast<SizeType>(context.FrameIndex) * kCascadeCount + cascade;

    if (uniformIndex < m_LightDescriptorSets.GetSize())
    {
        commandBuffer->BindDescriptorSet(
            EPipelineBindPoint::Graphics,
            context.PipelineLayout,
            0,
            m_LightDescriptorSets[uniformIndex],
            nullptr,
            0);
    }

    // set 1 = bindless 材质表 —— Masked 材质的 alpha 测试要读 albedo
    commandBuffer->BindDescriptorSet(
        EPipelineBindPoint::Graphics,
        context.PipelineLayout,
        1,
        context.BindlessDescriptorSet,
        nullptr,
        0);
}

void FShadowPass::RecordCasterRange(IRHICommandBuffer*           commandBuffer,
                                     const FRenderPassContext&    context,
                                     const FFrustum&              cascadeFrustum,
                                     const TArray<FRenderObject>* casters,
                                     SizeType                     begin,
                                     SizeType                     end)
{
    if (casters == nullptr)
    {
        return;
    }

    {
        FRHIGraphicsPipelineHandle boundPipeline;
        FRHIDescriptorSetHandle    boundMaterial;
        FRHIBufferHandle           boundVertexBuffer;
        FRHIBufferHandle           boundIndexBuffer;
        EIndexType                 boundIndexType = EIndexType::UInt32;

        for (SizeType i = begin; i < end; ++i)
        {
            const FRenderObject& obj = (*casters)[i];

            // 包围盒无效时保守保留 —— 与相机剔除同一原则:
            // 把"信息缺失"当成"不投影"会静默丢掉阴影。
            if (obj.WorldBounds.IsValid() &&
                !cascadeFrustum.IsAABBVisible(obj.WorldBounds))
            {
                continue;
            }

            const FRHIGraphicsPipelineHandle pipeline =
                SelectPipeline(obj.IsDoubleSided);

            if (pipeline.Packed != boundPipeline.Packed)
            {
                commandBuffer->BindGraphicsPipeline(pipeline);
                boundPipeline = pipeline;
            }

            // 材质集用于 Masked 的 alpha 测试 —— 镂空必须在阴影里同样成立,
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
                &pushData);

            commandBuffer->DrawIndexed(obj.IndexCount, 1, obj.IndexOffset,
                                       0, 0);
        }
    }

}

void FShadowPass::Execute(IRHICommandBuffer*        commandBuffer,
                           const FRenderPassContext& context)
{
    commandBuffer->BeginDebugLabel("ShadowPass", 0.9f, 0.9f, 0.3f);

    // 没有有效光源时也要走一遍通道, 只清不画。
    //
    // 直觉上"没有阴影就直接返回"是对的, 但阴影贴图的描述符始终指向这张
    // 深度图, 而片段着色器里那句 sampler2DArrayShadow 只要出现在代码里,
    // Vulkan 就要求它在绘制时处于 SHADER_READ_ONLY 布局 —— 着色器有没有
    // 真的去采样并不重要。直接返回会让它停在 UNDEFINED, 验证层立刻报错。
    //
    // 清成深度 1.0 恰好也是正确的语义: 比较采样用 LessOrEqual, 参考深度
    // 永远 ≤1.0, 于是处处判为无遮挡。
    const bool hasCasters = m_HasValidLight;

    // 逐级各走一遍完整的渲染通道。
    //
    // 没有用几何着色器或 multiview 一次写多层: 前者在移动端支持参差,
    // 后者要求所有层共享同一套绘制调用 —— 而级联的意义恰恰在于每级
    // 可以剔除掉不影响它的物体。三次通道换来的是各级独立可优化。
    for (UInt32 cascade = 0; cascade < kCascadeCount; ++cascade)
    {

    FRHIClearDepthStencilValue clearDepth = {};
    clearDepth.Depth   = 1.0f;
    clearDepth.Stencil = 0;

    FRHIRenderPassBeginInfo beginInfo = {};
    beginInfo.RenderPass        = m_RenderPass;
    beginInfo.Framebuffer       = m_CascadeFramebuffers[cascade];
    beginInfo.RenderAreaOffset  = { 0, 0 };
    beginInfo.RenderAreaExtent  = { kShadowMapSize, kShadowMapSize };
    beginInfo.ClearColors       = nullptr;
    beginInfo.ClearColorCount   = 0;
    beginInfo.ClearDepthStencil = &clearDepth;

    // 并行录制时通道内容必须来自次级缓冲区 —— 与前向 Pass 同一约束
    const bool useParallel =
        (m_Recorder != nullptr) && m_Recorder->IsInitialized();

    beginInfo.UseSecondaryCommandBuffers = useParallel;

    commandBuffer->BeginRenderPass(beginInfo);

    // 用**未经相机剔除**的投射体列表 —— 相机背后的物体照样会把影子投进画面。
    // 半透明不在其中: 把它们画进深度图会让玻璃在地面上留下一块实心黑影。
    //
    // 改按本级的光源视锥再剔一次: 每级只覆盖一小段视锥, 绝大多数投射体
    // 与它无关。不剔的话, 最近那一级也要画完整个场景, 而它实际只影响
    // 脚下几米。
    const FFrustum cascadeFrustum =
        FFrustum::FromViewProjection(m_CascadeViewProj[cascade]);

    const TArray<FRenderObject>* casters =
        (context.ShadowCasterObjects != nullptr) ? context.ShadowCasterObjects
                                                 : context.RenderObjects;

    const SizeType casterCount =
        (hasCasters && casters != nullptr) ? casters->GetSize() : 0;

    if (useParallel)
    {
        FRHICommandBufferInheritance inheritance;
        inheritance.RenderPass  = m_RenderPass;
        inheritance.Subpass     = 0;
        inheritance.Framebuffer = m_CascadeFramebuffers[cascade];

        // 每级各自一批 —— 三级用的是三组不同的槽位, 因为前两级的次级
        // 缓冲区此刻已经被 vkCmdExecuteCommands 引用, 不能重写。
        const FRecorderBatch batch = m_Recorder->RecordSegmented(
            casterCount, inheritance,
            [this, &context, &cascadeFrustum, casters, cascade](
                IRHICommandBuffer* segmentBuffer, SizeType begin, SizeType end)
            {
                RecordCascadeState(segmentBuffer, context, cascade);
                RecordCasterRange(segmentBuffer, context, cascadeFrustum,
                                  casters, begin, end);
            });

        m_Recorder->ExecuteInto(commandBuffer, batch);
    }
    else
    {
        RecordCascadeState(commandBuffer, context, cascade);
        RecordCasterRange(commandBuffer, context, cascadeFrustum,
                          casters, 0, casterCount);
    }

    commandBuffer->EndRenderPass();

    } // 级联循环

    commandBuffer->EndDebugLabel();
}

// ============================================================================
// OnResize — 阴影贴图与交换链尺寸无关, 无需重建
// ============================================================================

ERHIResult FShadowPass::OnResize(const FPassResizeDesc& desc)
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

    for (UInt32 cascade = 0; cascade < kCascadeCount; ++cascade)
    {
        device->DestroyFramebuffer(m_CascadeFramebuffers[cascade]);
        device->DestroyTextureView(m_CascadeViews[cascade]);
    }

    device->DestroyRenderPass(m_RenderPass);

    device->DestroySampler(m_ShadowSampler);
    device->DestroyTextureView(m_ShadowMapView);
    device->DestroyTexture(m_ShadowMap);

    LIMX_LOG(LogRenderer, Log, "[ShadowPass] 已关闭");
}

} // namespace Limx
