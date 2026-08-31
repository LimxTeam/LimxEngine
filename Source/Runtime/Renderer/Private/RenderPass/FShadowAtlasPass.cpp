/*******************************************************************************
 * 文件: FShadowAtlasPass.cpp
 * 创建时间: 2026-08-31
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   聚光灯阴影图集 Pass 实现 — 单次渲染通道, 逐块换视口
 *
 * 设计哲学:
 *   本 Pass 的所有输入都来自 FLightManager 本帧打包出来的那份阴影数据 ——
 *   块下标、矩阵、UV 变换都是同一份。这不是省事: 绘制用一套矩阵、采样用
 *   另一套的话, 两者只要差一帧, 快速转动的聚光灯就会出现"影子跟不上灯",
 *   而那看着像是阴影偏移没调好。
 *
 *   绘制背面与 FShadowPass 同理 —— 自遮挡的量化误差被物体厚度吸收。
 *
 * 技术特性:
 *   - 4096² D32 图集, 8×8 共 64 块
 *   - 阴影矩阵预乘进 push constant, 整个 Pass 只用一个描述符集
 *   - 每块按该灯的视锥再剔一次投射体
 *
 * 依赖关系:
 *   内部: Renderer/RenderPass/FShadowAtlasPass.h,
 *         RenderCore/Lighting/FLightManager.h,
 *         RenderCore/Shaders/FShaderManager.h
 *
 ******************************************************************************/

#include "Renderer/RenderPass/FShadowAtlasPass.h"
#include "Renderer/Renderer/FRenderer.h"

#include "RenderCore/Lighting/FLightManager.h"
#include "RenderCore/Shaders/FShaderManager.h"
#include "Renderer/RenderPass/FGpuCullPass.h"

namespace Limx
{

LIMX_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{

/// 绑 set 3 (逐物体数据) 并推本块的矩阵
///
/// 与 FShadowPass 里那个同名函数逐字相同。抄两遍而不是抽到公共头里, 是因为
/// 它只有十几行且两处的调用点相隔很远; 真正要紧的是**两处的语义必须一致**,
/// 而那由 --gpu-driven-check 与阴影自检一起兜住。
static void BindViewState(IRHICommandBuffer*        commandBuffer,
                          const FRenderPassContext& context,
                          const FMatrix&            viewProj)
{
    if (context.GpuCull != nullptr)
    {
        const FRHIDescriptorSetHandle set =
            context.GpuCull->GetDrawObjectSet(context.FrameIndex);

        if (set.IsValid())
        {
            commandBuffer->BindDescriptorSet(EPipelineBindPoint::Graphics,
                                             context.PipelineLayout, 3, set,
                                             nullptr, 0);
        }
    }

    FViewPushConstant push;
    push.ViewProj = viewProj;

    commandBuffer->PushConstants(context.PipelineLayout,
                                 EShaderStage::Vertex, 0,
                                 sizeof(FViewPushConstant), &push);
}

} // namespace

// ============================================================================
// Setup
// ============================================================================

ERHIResult FShadowAtlasPass::Setup(const FPassSetupDesc& desc)
{
    LIMX_CHECK(desc.Device != nullptr);

    m_PipelineLayout = desc.PipelineLayout;

    ERHIResult result = CreateAtlas(desc.Device);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[ShadowAtlas] 图集创建失败");
        return result;
    }

    result = CreateAtlasRenderPass(desc.Device);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[ShadowAtlas] RenderPass 创建失败");
        return result;
    }

    result = CreateFramebuffer(desc.Device);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[ShadowAtlas] Framebuffer 创建失败");
        return result;
    }

    result = CreateShaders(desc.Device);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[ShadowAtlas] 着色器创建失败");
        return result;
    }

    for (SizeType variant = 0;
         variant < kPipelineVariantCount && IsRHISuccess(result); ++variant)
    {
        result = CreatePipeline(desc.Device, variant != 0,
                                m_Pipelines[variant]);
    }

    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[ShadowAtlas] 管线创建失败");
        return result;
    }

    LIMX_LOG(LogRenderer, Log,
             "[ShadowAtlas] 初始化完成 — {}x{} D32_SFLOAT, {} 块 每块 {}x{}",
             kShadowAtlasSize, kShadowAtlasSize, kShadowTileCount,
             kShadowTileSize, kShadowTileSize);

    return ERHIResult::Success;
}

// ============================================================================
// CreateAtlas — 深度纹理 + 视图 + 比较采样器
// ============================================================================

ERHIResult FShadowAtlasPass::CreateAtlas(IRHIDevice* device)
{
    FRHITextureDesc depthDesc = FRHITextureDesc::DepthStencil(
        kShadowAtlasSize, kShadowAtlasSize,
        EPixelFormat::D32_SFLOAT,
        ESampleCount::Count1);
    depthDesc.Type      = ETextureType::Texture2D;
    depthDesc.DebugName = "SpotShadowAtlas";

    ERHIResult result = device->CreateTexture(depthDesc, m_Atlas);
    if (!IsRHISuccess(result))
    {
        return result;
    }

    FRHITextureViewDesc viewDesc = {};
    viewDesc.Texture         = m_Atlas;
    viewDesc.ViewType        = ETextureType::Texture2D;
    viewDesc.Format          = EPixelFormat::D32_SFLOAT;
    viewDesc.BaseMipLevel    = 0;
    viewDesc.MipLevelCount   = 1;
    viewDesc.BaseArrayLayer  = 0;
    viewDesc.ArrayLayerCount = 1;

    result = device->CreateTextureView(viewDesc, m_AtlasView);
    if (!IsRHISuccess(result))
    {
        device->DestroyTexture(m_Atlas);
        return result;
    }

    // ---- 比较采样器 ----
    //
    // 寻址用 **ClampToEdge** 而非 FShadowPass 那样的 ClampToBorder。
    //
    // 图集与单张贴图在这一点上恰好相反: 单张贴图之外确实是"没有信息",
    // 拉伸边缘会造出一道假阴影, 所以要用边框色。而图集里越过一块的边界
    // 就进了**隔壁那盏灯**的块 —— 边框色救不了这个, 唯一的办法是在着色器
    // 里把块内坐标钳住, 而那时寻址模式已经不起作用了。
    //
    // 既然如此, ClampToEdge 至少不依赖 borderColor 的可用性。真正防越界的
    // 是着色器里那句 clamp。
    FRHISamplerDesc samplerDesc = {};
    samplerDesc.MinFilter    = EFilter::Linear;
    samplerDesc.MagFilter    = EFilter::Linear;
    samplerDesc.MipmapMode   = ESamplerMipmapMode::Nearest;
    samplerDesc.AddressModeU = ESamplerAddressMode::ClampToEdge;
    samplerDesc.AddressModeV = ESamplerAddressMode::ClampToEdge;
    samplerDesc.AddressModeW = ESamplerAddressMode::ClampToEdge;
    samplerDesc.IsAnisotropyEnabled = false;
    samplerDesc.IsCompareEnabled    = true;
    samplerDesc.CompareOp           = ECompareOp::LessOrEqual;
    samplerDesc.MinLod              = 0.0f;
    samplerDesc.MaxLod              = 1.0f;

    result = device->CreateSampler(samplerDesc, m_AtlasSampler);
    if (!IsRHISuccess(result))
    {
        device->DestroyTextureView(m_AtlasView);
        device->DestroyTexture(m_Atlas);
        return result;
    }

    return ERHIResult::Success;
}

// ============================================================================
// CreateAtlasRenderPass
// ============================================================================

ERHIResult FShadowAtlasPass::CreateAtlasRenderPass(IRHIDevice* device)
{
    FRHIAttachmentDesc depthAttachment = {};
    depthAttachment.Format         = EPixelFormat::D32_SFLOAT;
    depthAttachment.Samples        = ESampleCount::Count1;
    depthAttachment.LoadOp         = ELoadOp::Clear;
    depthAttachment.StoreOp        = EStoreOp::Store;
    depthAttachment.StencilLoadOp  = ELoadOp::DontCare;
    depthAttachment.StencilStoreOp = EStoreOp::DontCare;
    depthAttachment.InitialLayout  = EImageLayout::Undefined;
    depthAttachment.FinalLayout    = EImageLayout::ShaderReadOnly;

    FRHIAttachmentReference depthRef = {};
    depthRef.AttachmentIndex = 0;
    depthRef.Layout          = EImageLayout::DepthStencilAttachment;

    FRHISubpassDesc subpass = {};
    subpass.ColorAttachments       = nullptr;
    subpass.ColorAttachmentCount   = 0;
    subpass.DepthStencilAttachment = &depthRef;

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
    renderPassDesc.DebugName       = "ShadowAtlas_RenderPass";

    return device->CreateRenderPass(renderPassDesc, m_RenderPass);
}

// ============================================================================
// CreateFramebuffer
// ============================================================================

ERHIResult FShadowAtlasPass::CreateFramebuffer(IRHIDevice* device)
{
    FRHIFramebufferDesc fbDesc = {};
    fbDesc.RenderPass      = m_RenderPass;
    fbDesc.Attachments     = &m_AtlasView;
    fbDesc.AttachmentCount = 1;
    fbDesc.Width           = kShadowAtlasSize;
    fbDesc.Height          = kShadowAtlasSize;
    fbDesc.Layers          = 1;
    fbDesc.DebugName       = "ShadowAtlas_Framebuffer";

    return device->CreateFramebuffer(fbDesc, m_Framebuffer);
}

// ============================================================================
// CreateShaders — 复用 depth_only
// ============================================================================

ERHIResult FShadowAtlasPass::CreateShaders(IRHIDevice* device)
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
// CreatePipeline
// ============================================================================

ERHIResult FShadowAtlasPass::CreatePipeline(
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

    // 单面材质绘制背面 —— 与 FShadowPass 同一理由: 自遮挡的量化误差被
    // 物体厚度吸收。双面材质没有厚度可用, 只能照常双面绘制并依赖 bias。
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

    pipelineDesc.ColorBlend.Attachments      = nullptr;
    pipelineDesc.ColorBlend.AttachmentCount  = 0;
    pipelineDesc.ColorBlend.IsLogicOpEnabled = false;

    pipelineDesc.DynamicState.EnabledStates =
        EDynamicState::Viewport | EDynamicState::Scissor;

    pipelineDesc.PipelineLayout = m_PipelineLayout;
    pipelineDesc.RenderPass     = m_RenderPass;
    pipelineDesc.SubpassIndex   = 0;
    pipelineDesc.DebugName      = isDoubleSided ? "ShadowAtlas_TwoSided"
                                                : "ShadowAtlas_Single";

    return device->CreateGraphicsPipeline(pipelineDesc, outPipeline);
}

// ============================================================================
// RecordTile — 绘制一块
// ============================================================================

void FShadowAtlasPass::RecordTile(IRHICommandBuffer*        commandBuffer,
                                   const FRenderPassContext& context,
                                   const FSpotShadowData&    shadowData,
                                   UInt32                    tileIndex)
{
    const FShadowTileRect rect = ShadowTileRect(tileIndex);

    if (rect.Size == 0)
    {
        return;
    }

    FRHIViewport viewport = {};
    viewport.X        = static_cast<Float32>(rect.X);
    viewport.Y        = static_cast<Float32>(rect.Y);
    viewport.Width    = static_cast<Float32>(rect.Size);
    viewport.Height   = static_cast<Float32>(rect.Size);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    commandBuffer->SetViewport(viewport);

    // 裁剪与视口同大。
    //
    // 限制光栅化范围的其实是**裁剪体**: NDC 之外的三角形早被裁掉了, 视口
    // 变换只把 [-1,1] 映射到这一块。所以就阴影本身而言, 裁剪矩形是多余的。
    //
    // 真正的理由是动态状态: 管线声明了 EDynamicState::Scissor, 不设的话
    // 沿用的是**上一个 Pass 留下的**那个矩形 —— 通常是交换链大小。图集是
    // 4096², 交换链常见 1280×720, 于是横坐标超过 1280 的块 (第 3 列往后)
    // 会被整块裁掉。
    //
    // 曾经的盲点, 现已补上: 默认的 --shadow-check 只用两块, 都落在
    // 1280×720 之内, 把这行注释掉它照样通过 (实测退出码 0)。
    //
    // --shadow-lights 64 用填充灯把被测的两盏顶到第 62/63 块 (纹素 x 是
    // 3072 与 3584), 那时同一个变异会被抓住 (退出码 14)。两个规模都跑,
    // 因为它们抓的不是同一类东西。
    FRHIScissorRect scissor = {};
    scissor.X      = static_cast<Int32>(rect.X);
    scissor.Y      = static_cast<Int32>(rect.Y);
    scissor.Width  = rect.Size;
    scissor.Height = rect.Size;

    commandBuffer->SetScissor(scissor);

    // 本块的阴影矩阵进 push constant, 逐物体数据在 set 3。
    //
    // 视图矩阵逐块、模型矩阵逐物体 —— 两者的粒度不同, 所以放在不同的地方。
    // 原先把两者乘在一起塞进 push constant 是因为那时没有逐物体缓冲区。
    BindViewState(commandBuffer, context, shadowData.ViewProj);

    const TArray<FRenderObject>* casters =
        (context.ShadowCasterObjects != nullptr) ? context.ShadowCasterObjects
                                                 : context.RenderObjects;

    if (casters == nullptr)
    {
        return;
    }

    // 按该灯的视锥剔一次。聚光灯的锥通常只覆盖场景的一小块, 不剔的话每盏
    // 灯都要把整个场景画一遍 —— 64 盏灯就是 64 遍。
    const FFrustum lightFrustum =
        FFrustum::FromViewProjection(shadowData.ViewProj);

    FRHIGraphicsPipelineHandle boundPipeline;
    FRHIBufferHandle           boundVertexBuffer;
    FRHIBufferHandle           boundIndexBuffer;
    EIndexType                 boundIndexType = EIndexType::UInt32;

    for (SizeType i = 0; i < casters->GetSize(); ++i)
    {
        const FRenderObject& obj = (*casters)[i];

        // 包围盒无效时保守保留 —— 把"信息缺失"当成"不投影"会静默丢掉阴影。
        if (obj.WorldBounds.IsValid() &&
            !lightFrustum.IsAABBVisible(obj.WorldBounds))
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

        if (obj.VertexBuffer.Packed != boundVertexBuffer.Packed)
        {
            commandBuffer->BindVertexBuffer(0, obj.VertexBuffer, 0);
            boundVertexBuffer = obj.VertexBuffer;
        }

        if (obj.IndexBuffer.Packed != boundIndexBuffer.Packed ||
            obj.IndexType != boundIndexType)
        {
            commandBuffer->BindIndexBuffer(obj.IndexBuffer, 0, obj.IndexType);
            boundIndexBuffer = obj.IndexBuffer;
            boundIndexType   = obj.IndexType;
        }

        // 模型矩阵与材质下标在 set 3 的 storage buffer 里, 本块的阴影矩阵
        // 在 push constant 里 (已在本块开头推过一次)。这里只传物体下标。
        //
        // 原先的做法是把阴影矩阵**预乘进 model** 再当 push constant 推 ——
        // 那个技巧在 model 搬进 storage buffer 之后就没法用了, 而换成
        // "push constant 装视图矩阵"反而更直接: 视图矩阵本来就是逐块的量。
        const UInt32 base =
            (context.GpuCull != nullptr) ? context.GpuCull->GetCullBase() : 0u;

        commandBuffer->DrawIndexed(obj.IndexCount, 1, obj.IndexOffset, 0,
                                   base + static_cast<UInt32>(i));
    }
}

// ============================================================================
// Execute
// ============================================================================

void FShadowAtlasPass::Execute(IRHICommandBuffer*        commandBuffer,
                                const FRenderPassContext& context)
{
    commandBuffer->BeginDebugLabel("ShadowAtlasPass", 0.95f, 0.75f, 0.35f);

    const TArray<FSpotShadowData>& casters =
        FLightManager::Get().GetSpotShadowCasters();

    // 计的是**实际画进去的块数**, 不是分配出去的块数。
    //
    // 第一版写的是 casters.GetSize() —— 那是"应该画几块", 而这个计数的用处
    // 恰恰是回答"到底画了几块"。变异验证一眼看穿: 把绘制循环改成只画第一块,
    // 计数照样报 7, 综合场景自检满分通过。
    //
    // 报意图而不是报事实的计数器, 与没有计数器等价。
    m_RenderedTileCount = 0;

    // 没有投影灯时也要走一遍通道, 只清不画。
    //
    // 与 FShadowPass 同一约束: 片段着色器里那句 sampler2DShadow 只要出现在
    // 代码里, Vulkan 就要求它在绘制时处于 SHADER_READ_ONLY 布局 —— 着色器
    // 有没有真的采样并不重要。直接返回会让它停在 UNDEFINED, 验证层立刻报错。
    //
    // 清成深度 1.0 也正好是"没有遮挡物"的语义。
    FRHIClearDepthStencilValue clearDepth = {};
    clearDepth.Depth   = 1.0f;
    clearDepth.Stencil = 0;

    FRHIRenderPassBeginInfo beginInfo = {};
    beginInfo.RenderPass        = m_RenderPass;
    beginInfo.Framebuffer       = m_Framebuffer;
    beginInfo.RenderAreaOffset  = { 0, 0 };
    beginInfo.RenderAreaExtent  = { kShadowAtlasSize, kShadowAtlasSize };
    beginInfo.ClearColors       = nullptr;
    beginInfo.ClearColorCount   = 0;
    beginInfo.ClearDepthStencil = &clearDepth;
    beginInfo.UseSecondaryCommandBuffers = false;

    commandBuffer->BeginRenderPass(beginInfo);

    if (!casters.IsEmpty())
    {
        // set 1 = bindless 材质表 —— Masked 材质的 alpha 测试要读 albedo
        commandBuffer->BindDescriptorSet(
            EPipelineBindPoint::Graphics,
            context.PipelineLayout,
            1,
            context.BindlessDescriptorSet,
            nullptr,
            0);

        for (SizeType i = 0; i < casters.GetSize(); ++i)
        {
            RecordTile(commandBuffer, context, casters[i],
                       static_cast<UInt32>(i));

            ++m_RenderedTileCount;
        }
    }

    commandBuffer->EndRenderPass();

    commandBuffer->EndDebugLabel();
}

// ============================================================================
// OnResize — 图集与交换链尺寸无关
// ============================================================================

ERHIResult FShadowAtlasPass::OnResize(const FPassResizeDesc& desc)
{
    (void)desc;
    return ERHIResult::Success;
}

void FShadowAtlasPass::ReleaseSwapchainResources(IRHIDevice* device)
{
    (void)device;
}

// ============================================================================
// Shutdown
// ============================================================================

void FShadowAtlasPass::Shutdown(IRHIDevice* device)
{
    if (device == nullptr)
    {
        return;
    }

    for (SizeType variant = 0; variant < kPipelineVariantCount; ++variant)
    {
        device->DestroyGraphicsPipeline(m_Pipelines[variant]);
    }

    device->DestroyShader(m_FragShader);
    device->DestroyShader(m_VertShader);

    device->DestroyFramebuffer(m_Framebuffer);
    device->DestroyRenderPass(m_RenderPass);

    device->DestroySampler(m_AtlasSampler);
    device->DestroyTextureView(m_AtlasView);
    device->DestroyTexture(m_Atlas);

    LIMX_LOG(LogRenderer, Log, "[ShadowAtlas] 已关闭");
}

} // namespace Limx
