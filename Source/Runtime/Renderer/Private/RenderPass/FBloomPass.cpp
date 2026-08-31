/*******************************************************************************
 * 文件: FBloomPass.cpp
 * 创建时间: 2026-08-31
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   泛光通道的实现
 *
 * 依赖关系:
 *   内部: Renderer/RenderPass/FBloomPass.h, RenderCore/Shaders
 *
 ******************************************************************************/

#include "Renderer/RenderPass/FBloomPass.h"

#include "RenderCore/Shaders/FShaderManager.h"
#include "Core/Logging/FLog.h"

namespace Limx
{

LIMX_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{

/// 与 HDR 目标同格式 —— 泛光全程在线性 HDR 空间
///
/// 8 位是不行的: 阈值之上的部分本来就可能远大于 1, 而降采样链每一级都在
/// 平均。8 位下超过 1 的部分直接被钳掉, 泛光会失去它全部的层次 —— 表现是
/// 所有高光的泛光看起来一样亮。
constexpr EPixelFormat kBloomFormat = EPixelFormat::RGBA16_SFLOAT;

/// bloom_downsample.frag 的 push constant
struct FDownPushConstant
{
    Float32 TexelX     = 0.0f;
    Float32 TexelY     = 0.0f;
    Float32 DoThreshold = 0.0f;
    Float32 Threshold  = 1.0f;

    Float32 Knee       = 0.5f;
    Float32 Pad0       = 0.0f;
    Float32 Pad1       = 0.0f;
    Float32 Pad2       = 0.0f;
};

static_assert(sizeof(FDownPushConstant) == 32,
              "FDownPushConstant 必须是 32 字节 (两个 vec4)");

/// bloom_upsample.frag 的 push constant
struct FUpPushConstant
{
    Float32 OffsetX = 0.0f;
    Float32 OffsetY = 0.0f;
    Float32 Radius  = 1.0f;
    Float32 Pad0    = 0.0f;
};

static_assert(sizeof(FUpPushConstant) == 16,
              "FUpPushConstant 必须是 16 字节 (一个 vec4)");

/// bloom_composite.frag 的 push constant
struct FCompositePushConstant
{
    Float32 Intensity = 0.05f;
    Float32 Pad0      = 0.0f;
    Float32 Pad1      = 0.0f;
    Float32 Pad2      = 0.0f;
};

static_assert(sizeof(FCompositePushConstant) == 16,
              "FCompositePushConstant 必须是 16 字节 (一个 vec4)");

/// 全屏三角形管线的共同部分 —— 三条管线只在着色器、渲染通道与混合上不同
void FillFullscreenPipeline(FRHIGraphicsPipelineDesc& desc,
                            FRHIShaderHandle          vertShader,
                            FRHIShaderHandle          fragShader)
{
    desc.ShaderStages[0].Shader     = vertShader;
    desc.ShaderStages[0].Stage      = EShaderStage::Vertex;
    desc.ShaderStages[0].EntryPoint = "main";

    desc.ShaderStages[1].Shader     = fragShader;
    desc.ShaderStages[1].Stage      = EShaderStage::Fragment;
    desc.ShaderStages[1].EntryPoint = "main";

    desc.ShaderStageCount = 2;

    desc.VertexInput.Bindings       = nullptr;
    desc.VertexInput.BindingCount   = 0;
    desc.VertexInput.Attributes     = nullptr;
    desc.VertexInput.AttributeCount = 0;

    desc.InputAssembly.Topology = EPrimitiveTopology::TriangleList;
    desc.InputAssembly.IsPrimitiveRestartEnabled = false;

    desc.Rasterization.IsDepthClampEnabled        = false;
    desc.Rasterization.IsRasterizerDiscardEnabled = false;
    desc.Rasterization.PolygonMode = EPolygonMode::Fill;
    desc.Rasterization.CullMode    = ECullMode::None;
    desc.Rasterization.FrontFace   = EFrontFace::CounterClockwise;
    desc.Rasterization.IsDepthBiasEnabled = false;
    desc.Rasterization.LineWidth   = 1.0f;

    desc.Multisample.RasterizationSamples   = ESampleCount::Count1;
    desc.Multisample.IsSampleShadingEnabled = false;

    desc.DepthStencil.IsDepthTestEnabled  = false;
    desc.DepthStencil.IsDepthWriteEnabled = false;

    desc.DynamicState.EnabledStates =
        EDynamicState::Viewport | EDynamicState::Scissor;
}

/// 单颜色附件的渲染通道
ERHIResult CreateSingleColorPass(IRHIDevice*           device,
                                 EPixelFormat          format,
                                 ELoadOp               loadOp,
                                 const char*           debugName,
                                 FRHIRenderPassHandle& outPass)
{
    FRHIAttachmentDesc attachment = {};
    attachment.Format         = format;
    attachment.Samples        = ESampleCount::Count1;
    attachment.LoadOp         = loadOp;
    attachment.StoreOp        = EStoreOp::Store;
    attachment.StencilLoadOp  = ELoadOp::DontCare;
    attachment.StencilStoreOp = EStoreOp::DontCare;

    // Load 意味着要保留已有内容, 那就不能声明 Undefined 作为初始布局 ——
    // 从 Undefined 转换的语义就是"内容可丢弃"。
    attachment.InitialLayout  = (loadOp == ELoadOp::Load)
                                    ? EImageLayout::ShaderReadOnly
                                    : EImageLayout::Undefined;
    attachment.FinalLayout    = EImageLayout::ShaderReadOnly;

    FRHIAttachmentReference colorRef = {};
    colorRef.AttachmentIndex = 0;
    colorRef.Layout          = EImageLayout::ColorAttachment;

    FRHISubpassDesc subpass = {};
    subpass.ColorAttachments       = &colorRef;
    subpass.ColorAttachmentCount   = 1;
    subpass.DepthStencilAttachment = nullptr;

    FRHISubpassDependency dependency = {};
    dependency.SrcSubpass    = 0xFFFFFFFF;
    dependency.DstSubpass    = 0;
    dependency.SrcStageMask  = EPipelineStageFlags::ColorAttachmentOutput;
    dependency.DstStageMask  = EPipelineStageFlags::FragmentShader;
    dependency.SrcAccessMask = EAccessFlags::ColorAttachmentWrite;
    dependency.DstAccessMask = EAccessFlags::ShaderRead;

    FRHIRenderPassDesc renderPassDesc = {};
    renderPassDesc.Attachments     = &attachment;
    renderPassDesc.AttachmentCount = 1;
    renderPassDesc.Subpasses       = &subpass;
    renderPassDesc.SubpassCount    = 1;
    renderPassDesc.Dependencies    = &dependency;
    renderPassDesc.DependencyCount = 1;
    renderPassDesc.DebugName       = debugName;

    return device->CreateRenderPass(renderPassDesc, outPass);
}

} // namespace

// ============================================================================
// Setup
// ============================================================================

ERHIResult FBloomPass::Setup(const FPassSetupDesc& desc)
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
        LIMX_LOG(LogRenderer, Error, "[BloomPass] 目标纹理创建失败");
        return result;
    }

    result = CreateRenderPasses(m_Device);

    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[BloomPass] 渲染通道创建失败");
        return result;
    }

    result = CreateFramebuffers(m_Device);

    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[BloomPass] Framebuffer 创建失败");
        return result;
    }

    result = CreateDescriptors(m_Device);

    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[BloomPass] 描述符资源创建失败");
        return result;
    }

    result = CreatePipelines(m_Device);

    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[BloomPass] 管线创建失败");
        return result;
    }

    LIMX_LOG(LogRenderer, Log,
             "[BloomPass] 初始化完成 — {} 级降采样链, 最大 {}x{}, 最小 {}x{}",
             kMipCount,
             m_MipExtent[0].Width, m_MipExtent[0].Height,
             m_MipExtent[kMipCount - 1].Width,
             m_MipExtent[kMipCount - 1].Height);

    return ERHIResult::Success;
}

// ============================================================================
// CreateTargets
// ============================================================================

ERHIResult FBloomPass::CreateTargets(IRHIDevice* device, FRHIExtent2D extent)
{
    // 链的第 0 级是半分辨率, 逐级减半。每一级至少 1x1 —— 除到 0 会让
    // framebuffer 创建失败, 而失败信息只说"尺寸非法", 不指向哪一级。
    UInt32 width  = extent.Width;
    UInt32 height = extent.Height;

    for (UInt32 i = 0; i < kMipCount; ++i)
    {
        width  = FMath::Max(width / 2u, 1u);
        height = FMath::Max(height / 2u, 1u);

        m_MipExtent[i] = { width, height };

        FRHITextureDesc texDesc = {};
        texDesc.Type        = ETextureType::Texture2D;
        texDesc.Format      = kBloomFormat;
        texDesc.Extent      = { width, height, 1 };
        texDesc.MipLevels   = 1;
        texDesc.ArrayLayers = 1;
        texDesc.Samples     = ESampleCount::Count1;

        texDesc.Usage       = static_cast<ETextureUsage>(
            static_cast<UInt32>(ETextureUsage::ColorAttachment) |
            static_cast<UInt32>(ETextureUsage::Sampled) |
            static_cast<UInt32>(ETextureUsage::TransferSrc));

        texDesc.MemoryUsage = EMemoryUsage::GpuOnly;
        texDesc.DebugName   = "BloomMip";

        ERHIResult result = device->CreateTexture(texDesc, m_MipTexture[i]);

        if (!IsRHISuccess(result))
        {
            return result;
        }

        FRHITextureViewDesc viewDesc = {};
        viewDesc.Texture         = m_MipTexture[i];
        viewDesc.ViewType        = ETextureType::Texture2D;
        viewDesc.Format          = kBloomFormat;
        viewDesc.BaseMipLevel    = 0;
        viewDesc.MipLevelCount   = 1;
        viewDesc.BaseArrayLayer  = 0;
        viewDesc.ArrayLayerCount = 1;

        result = device->CreateTextureView(viewDesc, m_MipView[i]);

        if (!IsRHISuccess(result))
        {
            return result;
        }
    }

    // 合成结果 —— 全分辨率
    FRHITextureDesc outDesc = {};
    outDesc.Type        = ETextureType::Texture2D;
    outDesc.Format      = kBloomFormat;
    outDesc.Extent      = { extent.Width, extent.Height, 1 };
    outDesc.MipLevels   = 1;
    outDesc.ArrayLayers = 1;
    outDesc.Samples     = ESampleCount::Count1;

    outDesc.Usage       = static_cast<ETextureUsage>(
        static_cast<UInt32>(ETextureUsage::ColorAttachment) |
        static_cast<UInt32>(ETextureUsage::Sampled) |
        static_cast<UInt32>(ETextureUsage::TransferSrc));

    outDesc.MemoryUsage = EMemoryUsage::GpuOnly;
    outDesc.DebugName   = "BloomOutput";

    ERHIResult result = device->CreateTexture(outDesc, m_OutputTexture);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    FRHITextureViewDesc outViewDesc = {};
    outViewDesc.Texture         = m_OutputTexture;
    outViewDesc.ViewType        = ETextureType::Texture2D;
    outViewDesc.Format          = kBloomFormat;
    outViewDesc.BaseMipLevel    = 0;
    outViewDesc.MipLevelCount   = 1;
    outViewDesc.BaseArrayLayer  = 0;
    outViewDesc.ArrayLayerCount = 1;

    return device->CreateTextureView(outViewDesc, m_OutputView);
}

void FBloomPass::DestroyTargets(IRHIDevice* device)
{
    for (UInt32 i = 0; i < kMipCount; ++i)
    {
        if (m_MipView[i].IsValid())
        {
            device->DestroyTextureView(m_MipView[i]);
            m_MipView[i] = {};
        }

        if (m_MipTexture[i].IsValid())
        {
            device->DestroyTexture(m_MipTexture[i]);
            m_MipTexture[i] = {};
        }
    }

    if (m_OutputView.IsValid())
    {
        device->DestroyTextureView(m_OutputView);
        m_OutputView = {};
    }

    if (m_OutputTexture.IsValid())
    {
        device->DestroyTexture(m_OutputTexture);
        m_OutputTexture = {};
    }
}

// ============================================================================
// CreateRenderPasses
// ============================================================================

ERHIResult FBloomPass::CreateRenderPasses(IRHIDevice* device)
{
    ERHIResult result = CreateSingleColorPass(
        device, kBloomFormat, ELoadOp::DontCare,
        "BloomPass_Down", m_DownRenderPass);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    result = CreateSingleColorPass(
        device, kBloomFormat, ELoadOp::Load,
        "BloomPass_Up", m_UpRenderPass);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    return CreateSingleColorPass(
        device, kBloomFormat, ELoadOp::DontCare,
        "BloomPass_Composite", m_CompositeRenderPass);
}

// ============================================================================
// CreateFramebuffers
// ============================================================================

ERHIResult FBloomPass::CreateFramebuffers(IRHIDevice* device)
{
    for (UInt32 i = 0; i < kMipCount; ++i)
    {
        FRHIFramebufferDesc fbDesc = {};
        fbDesc.RenderPass      = m_DownRenderPass;
        fbDesc.Attachments     = &m_MipView[i];
        fbDesc.AttachmentCount = 1;
        fbDesc.Width           = m_MipExtent[i].Width;
        fbDesc.Height          = m_MipExtent[i].Height;
        fbDesc.Layers          = 1;
        fbDesc.DebugName       = "BloomPass_MipFramebuffer";

        const ERHIResult result =
            device->CreateFramebuffer(fbDesc, m_MipFramebuffer[i]);

        if (!IsRHISuccess(result))
        {
            return result;
        }
    }

    FRHIFramebufferDesc outDesc = {};
    outDesc.RenderPass      = m_CompositeRenderPass;
    outDesc.Attachments     = &m_OutputView;
    outDesc.AttachmentCount = 1;
    outDesc.Width           = m_Extent.Width;
    outDesc.Height          = m_Extent.Height;
    outDesc.Layers          = 1;
    outDesc.DebugName       = "BloomPass_OutputFramebuffer";

    return device->CreateFramebuffer(outDesc, m_OutputFramebuffer);
}

void FBloomPass::DestroyFramebuffers(IRHIDevice* device)
{
    for (UInt32 i = 0; i < kMipCount; ++i)
    {
        if (m_MipFramebuffer[i].IsValid())
        {
            device->DestroyFramebuffer(m_MipFramebuffer[i]);
            m_MipFramebuffer[i] = {};
        }
    }

    if (m_OutputFramebuffer.IsValid())
    {
        device->DestroyFramebuffer(m_OutputFramebuffer);
        m_OutputFramebuffer = {};
    }
}

// ============================================================================
// CreateDescriptors
// ============================================================================

ERHIResult FBloomPass::CreateDescriptors(IRHIDevice* device)
{
    // ---- 链: 一个采样器 ----
    {
        FRHIDescriptorBinding binding = {};
        binding.Binding    = 0;
        binding.Type       = EDescriptorType::CombinedImageSampler;
        binding.Count      = 1;
        binding.StageFlags = EShaderStage::Fragment;

        FRHIDescSetLayoutDesc layoutDesc = {};
        layoutDesc.Bindings     = &binding;
        layoutDesc.BindingCount = 1;
        layoutDesc.DebugName    = "BloomChainSetLayout";

        const ERHIResult result =
            device->CreateDescSetLayout(layoutDesc, m_ChainSetLayout);

        if (!IsRHISuccess(result))
        {
            return result;
        }
    }

    // ---- 合成: 两个采样器 ----
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
        layoutDesc.DebugName    = "BloomCompositeSetLayout";

        const ERHIResult result =
            device->CreateDescSetLayout(layoutDesc, m_CompositeSetLayout);

        if (!IsRHISuccess(result))
        {
            return result;
        }
    }

    for (UInt32 i = 0; i < kMipCount; ++i)
    {
        ERHIResult result =
            device->AllocateDescriptorSet(m_ChainSetLayout, m_DownSet[i]);

        if (!IsRHISuccess(result))
        {
            return result;
        }

        result = device->AllocateDescriptorSet(m_ChainSetLayout, m_UpSet[i]);

        if (!IsRHISuccess(result))
        {
            return result;
        }
    }

    ERHIResult result =
        device->AllocateDescriptorSet(m_CompositeSetLayout, m_CompositeSet);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    // 双线性 + Clamp。
    //
    // 降采样链**依赖**双线性: 13 抽头里有 4 个落在半纹素位置, 那正是要靠
    // 硬件插值一次取到 2x2 的平均。用 Nearest 的话那四抽头退化成整纹素采样,
    // 核不再归一化, 能量就不守恒了。
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
    {
        FRHIPushConstantRange pushRange = {};
        pushRange.StageFlags = EShaderStage::Fragment;
        pushRange.Offset     = 0;

        // 降采样与升采样共用一个管线布局, 所以 push constant 的范围取两者
        // 的较大值。Vulkan 允许实际推送的字节数少于声明的范围。
        pushRange.Size = sizeof(FDownPushConstant);

        FRHIPipelineLayoutDesc layoutDesc = {};
        layoutDesc.SetLayouts             = &m_ChainSetLayout;
        layoutDesc.SetLayoutCount         = 1;
        layoutDesc.PushConstantRanges     = &pushRange;
        layoutDesc.PushConstantRangeCount = 1;
        layoutDesc.DebugName              = "BloomChainPipelineLayout";

        result = device->CreatePipelineLayout(layoutDesc,
                                              m_ChainPipelineLayout);

        if (!IsRHISuccess(result))
        {
            return result;
        }
    }

    {
        FRHIPushConstantRange pushRange = {};
        pushRange.StageFlags = EShaderStage::Fragment;
        pushRange.Offset     = 0;
        pushRange.Size       = sizeof(FCompositePushConstant);

        FRHIPipelineLayoutDesc layoutDesc = {};
        layoutDesc.SetLayouts             = &m_CompositeSetLayout;
        layoutDesc.SetLayoutCount         = 1;
        layoutDesc.PushConstantRanges     = &pushRange;
        layoutDesc.PushConstantRangeCount = 1;
        layoutDesc.DebugName              = "BloomCompositePipelineLayout";

        result = device->CreatePipelineLayout(layoutDesc,
                                              m_CompositePipelineLayout);

        if (!IsRHISuccess(result))
        {
            return result;
        }
    }

    UpdateDescriptors(device);

    return ERHIResult::Success;
}

// ============================================================================
// UpdateDescriptors
// ============================================================================

void FBloomPass::UpdateDescriptors(IRHIDevice* device)
{
    if (!m_SourceView.IsValid())
    {
        return;
    }

    for (UInt32 i = 0; i < kMipCount; ++i)
    {
        // 降采样第 i 级: 从源图 (i=0) 或上一级读
        const FRHITextureViewHandle downSource =
            (i == 0) ? m_SourceView : m_MipView[i - 1];

        FRHIDescriptorWrite downWrite =
            FRHIDescriptorWrite::CombinedImageSampler(
                m_DownSet[i], 0, downSource, m_Sampler,
                EImageLayout::ShaderReadOnly);

        device->UpdateDescriptorSets(&downWrite, 1);

        // 升采样第 i 级: 从更小的 i+1 级读, 加回第 i 级。
        // 最后一级没有更小的, 那个描述符不会被用到, 但仍要写一个有效值 ——
        // 未写入的描述符即使不被访问也可能触发验证层。
        const FRHITextureViewHandle upSource =
            (i + 1 < kMipCount) ? m_MipView[i + 1] : m_MipView[i];

        FRHIDescriptorWrite upWrite =
            FRHIDescriptorWrite::CombinedImageSampler(
                m_UpSet[i], 0, upSource, m_Sampler,
                EImageLayout::ShaderReadOnly);

        device->UpdateDescriptorSets(&upWrite, 1);
    }

    FRHIDescriptorWrite compositeWrites[2];

    compositeWrites[0] = FRHIDescriptorWrite::CombinedImageSampler(
        m_CompositeSet, 0, m_SourceView, m_Sampler,
        EImageLayout::ShaderReadOnly);

    compositeWrites[1] = FRHIDescriptorWrite::CombinedImageSampler(
        m_CompositeSet, 1, m_MipView[0], m_Sampler,
        EImageLayout::ShaderReadOnly);

    device->UpdateDescriptorSets(compositeWrites, 2);
}

void FBloomPass::SetSourceView(FRHITextureViewHandle view)
{
    m_SourceView = view;

    if (m_Device != nullptr)
    {
        UpdateDescriptors(m_Device);
    }
}

// ============================================================================
// CreatePipelines
// ============================================================================

ERHIResult FBloomPass::CreatePipelines(IRHIDevice* device)
{
    FShaderManager& shaders = FShaderManager::Get();

    struct FShaderSpec
    {
        const char*       Path;
        EShaderStage      Stage;
        FRHIShaderHandle* Target;
    };

    const FShaderSpec shaderSpecs[] =
    {
        { "Builtin/fullscreen.vert",       EShaderStage::Vertex,
          &m_VertShader },
        { "Builtin/bloom_downsample.frag", EShaderStage::Fragment,
          &m_DownShader },
        { "Builtin/bloom_upsample.frag",   EShaderStage::Fragment,
          &m_UpShader },
        { "Builtin/bloom_composite.frag",  EShaderStage::Fragment,
          &m_CompositeShader },
    };

    for (SizeType i = 0; i < sizeof(shaderSpecs) / sizeof(shaderSpecs[0]); ++i)
    {
        const ERHIResult result = shaders.CreateShaderModule(
            device, FString(shaderSpecs[i].Path), shaderSpecs[i].Stage,
            *shaderSpecs[i].Target);

        if (!IsRHISuccess(result))
        {
            LIMX_LOG(LogRenderer, Error,
                     "[BloomPass] 着色器加载失败: {}", shaderSpecs[i].Path);
            return result;
        }
    }

    // ---- 降采样: 覆盖写 ----
    {
        FRHIGraphicsPipelineDesc desc = {};
        FillFullscreenPipeline(desc, m_VertShader, m_DownShader);

        FRHIColorBlendAttachmentDesc blend =
            FRHIColorBlendAttachmentDesc::Opaque();

        desc.ColorBlend.Attachments      = &blend;
        desc.ColorBlend.AttachmentCount  = 1;
        desc.ColorBlend.IsLogicOpEnabled = false;

        desc.PipelineLayout = m_ChainPipelineLayout;
        desc.RenderPass     = m_DownRenderPass;
        desc.SubpassIndex   = 0;
        desc.DebugName      = "BloomPass_Down";

        const ERHIResult result =
            device->CreateGraphicsPipeline(desc, m_DownPipeline);

        if (!IsRHISuccess(result))
        {
            return result;
        }
    }

    // ---- 升采样: 加法混合 ----
    //
    // 加回已有的降采样结果。用 Opaque 覆盖写的话, 每一级都会丢掉自己那一级
    // 的细节, 泛光就只剩最小那一级放大回来的一团 —— 而那看起来"也像泛光",
    // 只是没有层次。
    {
        FRHIGraphicsPipelineDesc desc = {};
        FillFullscreenPipeline(desc, m_VertShader, m_UpShader);

        FRHIColorBlendAttachmentDesc blend = {};
        blend.IsBlendEnabled       = true;
        blend.SrcColorBlendFactor  = EBlendFactor::One;
        blend.DstColorBlendFactor  = EBlendFactor::One;
        blend.ColorBlendOp         = EBlendOp::Add;
        blend.SrcAlphaBlendFactor  = EBlendFactor::One;
        blend.DstAlphaBlendFactor  = EBlendFactor::One;
        blend.AlphaBlendOp         = EBlendOp::Add;
        blend.ColorWriteMask       = EColorWriteMask::All;

        desc.ColorBlend.Attachments      = &blend;
        desc.ColorBlend.AttachmentCount  = 1;
        desc.ColorBlend.IsLogicOpEnabled = false;

        desc.PipelineLayout = m_ChainPipelineLayout;
        desc.RenderPass     = m_UpRenderPass;
        desc.SubpassIndex   = 0;
        desc.DebugName      = "BloomPass_Up";

        const ERHIResult result =
            device->CreateGraphicsPipeline(desc, m_UpPipeline);

        if (!IsRHISuccess(result))
        {
            return result;
        }
    }

    // ---- 合成 ----
    {
        FRHIGraphicsPipelineDesc desc = {};
        FillFullscreenPipeline(desc, m_VertShader, m_CompositeShader);

        FRHIColorBlendAttachmentDesc blend =
            FRHIColorBlendAttachmentDesc::Opaque();

        desc.ColorBlend.Attachments      = &blend;
        desc.ColorBlend.AttachmentCount  = 1;
        desc.ColorBlend.IsLogicOpEnabled = false;

        desc.PipelineLayout = m_CompositePipelineLayout;
        desc.RenderPass     = m_CompositeRenderPass;
        desc.SubpassIndex   = 0;
        desc.DebugName      = "BloomPass_Composite";

        return device->CreateGraphicsPipeline(desc, m_CompositePipeline);
    }
}

// ============================================================================
// Execute
// ============================================================================

void FBloomPass::Execute(IRHICommandBuffer*        commandBuffer,
                         const FRenderPassContext& context)
{
    if (!m_Enabled || commandBuffer == nullptr)
    {
        return;
    }

    if (!m_SourceView.IsValid())
    {
        LIMX_LOG(LogRenderer, Error, "[BloomPass] 输入未接入 — 本帧跳过");
        return;
    }

    commandBuffer->BeginDebugLabel("BloomPass", 1.0f, 0.9f, 0.4f);

    // 每一趟都是同一套动作, 只有 framebuffer / 管线 / 描述符 / push constant
    // 不同。写成一个 lambda 而不是复制五遍 —— 复制的那几份里总会有一份忘了
    // 改视口, 而视口错了的表现是那一级只画了左上角一小块, 泛光整体偏暗。
    const auto DrawFullscreen =
        [commandBuffer](FRHIRenderPassHandle       renderPass,
                        FRHIFramebufferHandle      framebuffer,
                        FRHIExtent2D               extent,
                        FRHIGraphicsPipelineHandle pipeline,
                        FRHIPipelineLayoutHandle   layout,
                        FRHIDescriptorSetHandle    descriptorSet,
                        const void*                pushData,
                        UInt32                     pushSize)
    {
        FRHIRenderPassBeginInfo beginInfo = {};
        beginInfo.RenderPass        = renderPass;
        beginInfo.Framebuffer       = framebuffer;
        beginInfo.RenderAreaOffset  = { 0, 0 };
        beginInfo.RenderAreaExtent  = extent;
        beginInfo.ClearColors       = nullptr;
        beginInfo.ClearColorCount   = 0;
        beginInfo.ClearDepthStencil = nullptr;

        commandBuffer->BeginRenderPass(beginInfo);

        commandBuffer->BindGraphicsPipeline(pipeline);

        FRHIViewport viewport = {};
        viewport.X        = 0.0f;
        viewport.Y        = 0.0f;
        viewport.Width    = static_cast<Float32>(extent.Width);
        viewport.Height   = static_cast<Float32>(extent.Height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;

        commandBuffer->SetViewport(viewport);

        FRHIScissorRect scissor = {};
        scissor.X      = 0;
        scissor.Y      = 0;
        scissor.Width  = extent.Width;
        scissor.Height = extent.Height;

        commandBuffer->SetScissor(scissor);

        commandBuffer->BindDescriptorSet(EPipelineBindPoint::Graphics,
                                         layout, 0, descriptorSet, nullptr, 0);

        commandBuffer->PushConstants(layout, EShaderStage::Fragment, 0,
                                     pushSize, pushData);

        commandBuffer->Draw(3, 1, 0, 0);

        commandBuffer->EndRenderPass();
    };

    // ---- 降采样链 ----
    for (UInt32 i = 0; i < kMipCount; ++i)
    {
        // 纹素尺寸取的是**源**的, 不是目标的 —— 13 抽头的偏移量是相对源
        // 纹理定义的。取成目标的话偏移量差一倍, 核就跨到不该跨的范围,
        // 表现是泛光比预期糊一倍而且能量不守恒。
        const FRHIExtent2D sourceExtent =
            (i == 0) ? m_Extent : m_MipExtent[i - 1];

        FDownPushConstant pushData;
        pushData.TexelX =
            1.0f / static_cast<Float32>(FMath::Max(sourceExtent.Width, 1u));
        pushData.TexelY =
            1.0f / static_cast<Float32>(FMath::Max(sourceExtent.Height, 1u));

        // 只有第 0 级做阈值提取 —— 后面几级读的已经是阈值之后的内容
        pushData.DoThreshold = (i == 0) ? 1.0f : 0.0f;
        pushData.Threshold   = m_Threshold;
        pushData.Knee        = m_Knee;

        DrawFullscreen(m_DownRenderPass, m_MipFramebuffer[i], m_MipExtent[i],
                       m_DownPipeline, m_ChainPipelineLayout, m_DownSet[i],
                       &pushData, sizeof(pushData));
    }

    // ---- 升采样链 ----
    //
    // 从倒数第二级往回走: 读 i+1 级, 加到 i 级上。最小的那一级没有更小的
    // 可以加, 所以不参与。
    for (UInt32 i = kMipCount - 1; i > 0; --i)
    {
        const UInt32 target = i - 1;

        FUpPushConstant pushData;

        // 偏移取**目标**纹素尺寸乘半径 —— 帐篷核是在目标分辨率上定义的
        pushData.OffsetX = m_FilterRadius /
            static_cast<Float32>(FMath::Max(m_MipExtent[target].Width, 1u));
        pushData.OffsetY = m_FilterRadius /
            static_cast<Float32>(FMath::Max(m_MipExtent[target].Height, 1u));
        pushData.Radius  = m_FilterRadius;

        DrawFullscreen(m_UpRenderPass, m_MipFramebuffer[target],
                       m_MipExtent[target], m_UpPipeline,
                       m_ChainPipelineLayout, m_UpSet[target],
                       &pushData, sizeof(pushData));
    }

    // ---- 合成 ----
    FCompositePushConstant compositeData;
    compositeData.Intensity = m_Intensity;

    DrawFullscreen(m_CompositeRenderPass, m_OutputFramebuffer, m_Extent,
                   m_CompositePipeline, m_CompositePipelineLayout,
                   m_CompositeSet, &compositeData, sizeof(compositeData));

    commandBuffer->EndDebugLabel();
}

// ============================================================================
// OnResize
// ============================================================================

ERHIResult FBloomPass::OnResize(const FPassResizeDesc& desc)
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

    result = CreateFramebuffers(desc.Device);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    UpdateDescriptors(desc.Device);

    return ERHIResult::Success;
}

void FBloomPass::ReleaseSwapchainResources(IRHIDevice* device)
{
    (void)device;
}

// ============================================================================
// Shutdown
// ============================================================================

void FBloomPass::Shutdown(IRHIDevice* device)
{
    if (device == nullptr)
    {
        return;
    }

    DestroyFramebuffers(device);
    DestroyTargets(device);

    FRHIGraphicsPipelineHandle* const pipelines[] =
    {
        &m_DownPipeline, &m_UpPipeline, &m_CompositePipeline,
    };

    for (SizeType i = 0; i < 3; ++i)
    {
        if (pipelines[i]->IsValid())
        {
            device->DestroyGraphicsPipeline(*pipelines[i]);
            *pipelines[i] = {};
        }
    }

    FRHIPipelineLayoutHandle* const layouts[] =
    {
        &m_ChainPipelineLayout, &m_CompositePipelineLayout,
    };

    for (SizeType i = 0; i < 2; ++i)
    {
        if (layouts[i]->IsValid())
        {
            device->DestroyPipelineLayout(*layouts[i]);
            *layouts[i] = {};
        }
    }

    FRHIDescSetLayoutHandle* const setLayouts[] =
    {
        &m_ChainSetLayout, &m_CompositeSetLayout,
    };

    for (SizeType i = 0; i < 2; ++i)
    {
        if (setLayouts[i]->IsValid())
        {
            device->DestroyDescSetLayout(*setLayouts[i]);
            *setLayouts[i] = {};
        }
    }

    FRHIRenderPassHandle* const renderPasses[] =
    {
        &m_DownRenderPass, &m_UpRenderPass, &m_CompositeRenderPass,
    };

    for (SizeType i = 0; i < 3; ++i)
    {
        if (renderPasses[i]->IsValid())
        {
            device->DestroyRenderPass(*renderPasses[i]);
            *renderPasses[i] = {};
        }
    }

    if (m_Sampler.IsValid())
    {
        device->DestroySampler(m_Sampler);
        m_Sampler = {};
    }

    FRHIShaderHandle* const shaderHandles[] =
    {
        &m_VertShader, &m_DownShader, &m_UpShader, &m_CompositeShader,
    };

    for (SizeType i = 0; i < 4; ++i)
    {
        if (shaderHandles[i]->IsValid())
        {
            device->DestroyShader(*shaderHandles[i]);
            *shaderHandles[i] = {};
        }
    }

    m_Device = nullptr;

    LIMX_LOG(LogRenderer, Log, "[BloomPass] 已关闭");
}

} // namespace Limx
