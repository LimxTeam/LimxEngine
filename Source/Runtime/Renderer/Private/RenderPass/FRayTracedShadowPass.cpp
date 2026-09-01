// ============================================================
// 文件名称：FRayTracedShadowPass.cpp
// 创建时间：2026-08-31
// 创建者  ：LimxTeam
// 设计哲学：这个通道没有"效果参数"。阴影贴图那一套里的深度偏置、法线偏置、
//          PCF 半径、级联过渡宽度，每一个都是在补离散化的窟窿；光追这里
//          唯一的容差是盖住深度缓冲区自身的量化误差，而那个量算得出来。
//          留下可调旋钮的代价不是多一行代码，是从此没人知道"对"是什么。
// 功能描述：FRayTracedShadowPass 的实现 — 掩码纹理、计算管线、描述符与
//          每帧派发。
// 技术特性：掩码是 R8_UNORM (Storage + Sampled)；派发前后各一次布局转换，
//          把深度转成着色器可读、掩码转成着色器可读；加速结构无效时整个
//          通道跳过，而不是画一张全亮的掩码。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                          │ 描述                      │
// │────────────────────────────────│─────────────────────────│
// │ Setup()                        │ 建掩码、管线与描述符       │
// │ CreateMask()                   │ 建 R8 掩码纹理与视图       │
// │ CreatePipeline()               │ 建计算管线                │
// │ CreateDescriptors()            │ 建描述符集布局与集合       │
// │ Execute()                      │ 每帧派发                  │
// │ OnResize()                     │ 重建掩码                  │
// ============================================================

#include "Renderer/RenderPass/FRayTracedShadowPass.h"

#include "Renderer/RayTracing/FRayTracingScene.h"

#include "RenderCore/Shaders/FShaderManager.h"

#include "RHI/RHI/IRHIDevice.h"
#include "RHI/RHI/IRHICommandBuffer.h"

namespace Limx
{

LIMX_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{

/// 掩码格式 —— 可见度只需要一个通道
///
/// R8_UNORM 而不是 R16F: 现在的输出是二值的 (0 或 1), 8 位绰绰有余。
/// 将来做软阴影 (面光源多采样) 时才需要更多位, 那时再换。
constexpr EPixelFormat kMaskFormat = EPixelFormat::R8_UNORM;

/// 与 rt_shadow.comp 的 push constant 块逐字段一致
struct FRtShadowPushConstants
{
    FMatrix InvViewProj;

    Float32 LightPosX = 0.0f;
    Float32 LightPosY = 0.0f;
    Float32 LightPosZ = 0.0f;
    Float32 LightType = 0.0f;

    Float32 LightDirX = 0.0f;
    Float32 LightDirY = -1.0f;
    Float32 LightDirZ = 0.0f;
    Float32 LightRange = 100.0f;

    UInt32 Width    = 0;
    UInt32 Height   = 0;
    UInt32 RayMask  = 0xFFu;
    UInt32 Pad0     = 0;

    Float32 NearPlane    = 0.1f;
    Float32 FarPlane     = 100.0f;
    Float32 NormalOffset = 1.0e-3f;
    Float32 RayTMin      = 1.0e-3f;
};

static_assert(sizeof(FRtShadowPushConstants) == 128,
              "FRtShadowPushConstants 必须是 128 字节 — 与 rt_shadow.comp 的 "
              "push constant 块逐字段一致");

} // namespace

// ============================================================================
// Setup
// ============================================================================

ERHIResult FRayTracedShadowPass::Setup(const FPassSetupDesc& desc)
{
    m_Device = desc.Device;
    m_Extent = desc.SwapchainExtent;

    if (m_Device == nullptr)
    {
        return ERHIResult::ErrorInvalidParameter;
    }

    // 设备不支持光追时不建任何东西, 并且**保持禁用**。
    //
    // 建了却不用会白占一张全分辨率纹理; 而"建了并且默认启用"更糟 ——
    // 掩码里是未初始化的内容, 乘进直接光照就是一片随机的明暗。
    if (!m_Device->IsRayTracingSupported())
    {
        LIMX_LOG(LogRenderer, Display,
                 "[光追阴影] 设备不支持光线追踪 — 通道不启用");
        m_Enabled = false;
        return ERHIResult::Success;
    }

    ERHIResult result = CreateMask(m_Device, m_Extent);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    {
        FRHISamplerDesc samplerDesc = {};
        samplerDesc.MinFilter    = EFilter::Nearest;
        samplerDesc.MagFilter    = EFilter::Nearest;
        samplerDesc.MipmapMode   = ESamplerMipmapMode::Nearest;
        samplerDesc.AddressModeU = ESamplerAddressMode::ClampToEdge;
        samplerDesc.AddressModeV = ESamplerAddressMode::ClampToEdge;
        samplerDesc.AddressModeW = ESamplerAddressMode::ClampToEdge;
        samplerDesc.IsAnisotropyEnabled = false;

        result = m_Device->CreateSampler(samplerDesc, m_PointSampler);

        if (!IsRHISuccess(result))
        {
            return result;
        }
    }

    result = CreateDescriptors(m_Device);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    result = CreatePipeline(m_Device);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    LIMX_LOG(LogRenderer, Log,
             "[光追阴影] 初始化完成 — {}x{} R8_UNORM, 法线偏移 {}, tMin {}",
             m_Extent.Width, m_Extent.Height, m_NormalOffset, m_RayTMin);

    return ERHIResult::Success;
}

// ============================================================================
// CreateMask
// ============================================================================

ERHIResult FRayTracedShadowPass::CreateMask(IRHIDevice* device,
                                             FRHIExtent2D extent)
{
    FRHITextureDesc texDesc = {};
    texDesc.Type        = ETextureType::Texture2D;
    texDesc.Format      = kMaskFormat;
    texDesc.Extent      = { extent.Width, extent.Height, 1 };
    texDesc.MipLevels   = 1;
    texDesc.ArrayLayers = 1;
    texDesc.Samples     = ESampleCount::Count1;

    // Storage 是计算着色器写它要的, Sampled 是着色阶段读它要的,
    // TransferSrc 是自检回读要的。
    texDesc.Usage = static_cast<ETextureUsage>(
        static_cast<UInt32>(ETextureUsage::Storage) |
        static_cast<UInt32>(ETextureUsage::Sampled) |
        static_cast<UInt32>(ETextureUsage::TransferSrc));

    texDesc.MemoryUsage = EMemoryUsage::GpuOnly;
    texDesc.DebugName   = "RayTracedShadowMask";

    ERHIResult result = device->CreateTexture(texDesc, m_MaskTexture);

    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[光追阴影] 掩码纹理创建失败");
        return result;
    }

    FRHITextureViewDesc viewDesc = {};
    viewDesc.Texture         = m_MaskTexture;
    viewDesc.ViewType        = ETextureType::Texture2D;
    viewDesc.Format          = kMaskFormat;
    viewDesc.BaseMipLevel    = 0;
    viewDesc.MipLevelCount   = 1;
    viewDesc.BaseArrayLayer  = 0;
    viewDesc.ArrayLayerCount = 1;

    return device->CreateTextureView(viewDesc, m_MaskView);
}

// ============================================================================
// CreateDescriptors
// ============================================================================

ERHIResult FRayTracedShadowPass::CreateDescriptors(IRHIDevice* device)
{
    FRHIDescriptorBinding bindings[4] = {};

    bindings[0].Binding    = 0;
    bindings[0].Type       = EDescriptorType::AccelerationStructure;
    bindings[0].Count      = 1;
    bindings[0].StageFlags = EShaderStage::Compute;

    bindings[1].Binding    = 1;
    bindings[1].Type       = EDescriptorType::CombinedImageSampler;
    bindings[1].Count      = 1;
    bindings[1].StageFlags = EShaderStage::Compute;

    bindings[2].Binding    = 2;
    bindings[2].Type       = EDescriptorType::CombinedImageSampler;
    bindings[2].Count      = 1;
    bindings[2].StageFlags = EShaderStage::Compute;

    bindings[3].Binding    = 3;
    bindings[3].Type       = EDescriptorType::StorageImage;
    bindings[3].Count      = 1;
    bindings[3].StageFlags = EShaderStage::Compute;

    FRHIDescSetLayoutDesc layoutDesc = {};
    layoutDesc.Bindings     = bindings;
    layoutDesc.BindingCount = 4;
    layoutDesc.DebugName    = "RtShadowSetLayout";

    ERHIResult result = device->CreateDescSetLayout(layoutDesc, m_SetLayout);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    return device->AllocateDescriptorSet(m_SetLayout, m_DescriptorSet);
}

// ============================================================================
// CreatePipeline
// ============================================================================

ERHIResult FRayTracedShadowPass::CreatePipeline(IRHIDevice* device)
{
    FShaderManager& shaders = FShaderManager::Get();

    if (!shaders.IsInitialized())
    {
        shaders.Initialize();
    }

    ERHIResult result = shaders.CreateShaderModule(
        device, FString("Builtin/rt_shadow.comp"), EShaderStage::Compute,
        m_Shader);

    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[光追阴影] rt_shadow.comp 加载失败");
        return result;
    }

    FRHIPushConstantRange pushRange = {};
    pushRange.StageFlags = EShaderStage::Compute;
    pushRange.Offset     = 0;
    pushRange.Size       = sizeof(FRtShadowPushConstants);

    FRHIPipelineLayoutDesc layoutDesc = {};
    layoutDesc.SetLayouts             = &m_SetLayout;
    layoutDesc.SetLayoutCount         = 1;
    layoutDesc.PushConstantRanges     = &pushRange;
    layoutDesc.PushConstantRangeCount = 1;
    layoutDesc.DebugName              = "RtShadowPipelineLayout";

    result = device->CreatePipelineLayout(layoutDesc, m_PipelineLayout);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    FRHIComputePipelineDesc pipelineDesc = {};
    pipelineDesc.ComputeShader.Shader     = m_Shader;
    pipelineDesc.ComputeShader.Stage      = EShaderStage::Compute;
    pipelineDesc.ComputeShader.EntryPoint = "main";
    pipelineDesc.PipelineLayout           = m_PipelineLayout;
    pipelineDesc.DebugName                = "RtShadowPipeline";

    return device->CreateComputePipeline(pipelineDesc, m_Pipeline);
}

// ============================================================================
// Execute
// ============================================================================

void FRayTracedShadowPass::Execute(IRHICommandBuffer*        commandBuffer,
                                    const FRenderPassContext& context)
{
    (void)context;

    if (!m_Enabled || commandBuffer == nullptr || m_Device == nullptr)
    {
        return;
    }

    // 加速结构无效时整个跳过, 而不是画一张全亮的掩码。
    //
    // 全亮看起来"没有阴影", 与"这个场景本来就没有遮挡"分不开 —— 而那正是
    // 一条光追阴影判据最需要区分的两件事。跳过之后掩码保持上一帧的内容,
    // 而调用方能通过 IsEnabled 与 TLAS 是否有效自己判断。
    if (!m_Tlas.IsValid() || !m_MaskView.IsValid() || !m_DepthView.IsValid())
    {
        return;
    }

    commandBuffer->BeginDebugLabel("RayTracedShadow", 0.2f, 0.6f, 1.0f);

    // ---- 描述符 ----
    FRHIDescriptorWrite writes[4];

    writes[0] = FRHIDescriptorWrite();
    writes[0].DescriptorSet = m_DescriptorSet;
    writes[0].Binding       = 0;
    writes[0].Type          = EDescriptorType::AccelerationStructure;
    writes[0].AccelStruct   = m_Tlas;

    writes[1] = FRHIDescriptorWrite();
    writes[1].DescriptorSet = m_DescriptorSet;
    writes[1].Binding       = 1;
    writes[1].Type          = EDescriptorType::CombinedImageSampler;
    writes[1].ImageView     = m_DepthView;
    writes[1].Sampler       = m_PointSampler;
    writes[1].ImageLayout   = EImageLayout::ShaderReadOnly;

    writes[2] = FRHIDescriptorWrite();
    writes[2].DescriptorSet = m_DescriptorSet;
    writes[2].Binding       = 2;
    writes[2].Type          = EDescriptorType::CombinedImageSampler;
    writes[2].ImageView     = m_NormalView;
    writes[2].Sampler       = m_PointSampler;
    writes[2].ImageLayout   = EImageLayout::ShaderReadOnly;

    writes[3] = FRHIDescriptorWrite();
    writes[3].DescriptorSet = m_DescriptorSet;
    writes[3].Binding       = 3;
    writes[3].Type          = EDescriptorType::StorageImage;
    writes[3].ImageView     = m_MaskView;
    writes[3].ImageLayout   = EImageLayout::General;

    m_Device->UpdateDescriptorSets(writes, 4);

    // ---- 布局 ----
    //
    // 深度此刻停在 DepthStencilAttachment (深度预通道的 FinalLayout)。
    commandBuffer->TransitionImageLayout(
        m_DepthTexture,
        EImageLayout::DepthStencilAttachment,
        EImageLayout::ShaderReadOnly,
        EPipelineStageFlags::LateFragmentTests,
        EPipelineStageFlags::ComputeShader,
        EAccessFlags::DepthStencilAttachmentWrite,
        EAccessFlags::ShaderRead);

    // 掩码上一帧停在 ShaderReadOnly; 第一帧是 Undefined。
    // 两种情形都要能转过去, 而 Undefined 会丢内容 —— 无所谓, 这一帧
    // 每个像素都会被写。
    commandBuffer->TransitionImageLayout(
        m_MaskTexture,
        EImageLayout::Undefined,
        EImageLayout::General,
        EPipelineStageFlags::FragmentShader,
        EPipelineStageFlags::ComputeShader,
        EAccessFlags::ShaderRead,
        EAccessFlags::ShaderWrite);

    // ---- 派发 ----
    FRtShadowPushConstants push;
    push.InvViewProj = m_ViewProj.Inverse();

    push.LightPosX = m_Light.PositionX;
    push.LightPosY = m_Light.PositionY;
    push.LightPosZ = m_Light.PositionZ;
    push.LightType = m_Light.Type;

    push.LightDirX  = m_Light.DirectionX;
    push.LightDirY  = m_Light.DirectionY;
    push.LightDirZ  = m_Light.DirectionZ;
    push.LightRange = m_Light.Range;

    push.Width   = m_Extent.Width;
    push.Height  = m_Extent.Height;

    // 只让会写深度的那一类几何体投影。
    //
    // 半透明在光栅化里不写深度, 让它当不透明遮挡的话, 玻璃后面会出现一块
    // 与画面完全对不上的黑影。
    push.RayMask = kRayMaskDepthWriting;

    push.NearPlane    = m_NearPlane;
    push.FarPlane     = m_FarPlane;
    push.NormalOffset = m_NormalOffset;
    push.RayTMin      = m_RayTMin;

    commandBuffer->BindComputePipeline(m_Pipeline);
    commandBuffer->BindDescriptorSet(EPipelineBindPoint::Compute,
                                      m_PipelineLayout, 0, m_DescriptorSet);
    commandBuffer->PushConstants(m_PipelineLayout, EShaderStage::Compute, 0,
                                  sizeof(push), &push);

    constexpr UInt32 kGroup = 8;
    commandBuffer->Dispatch((m_Extent.Width + kGroup - 1) / kGroup,
                             (m_Extent.Height + kGroup - 1) / kGroup, 1);

    // ---- 交还布局 ----
    commandBuffer->TransitionImageLayout(
        m_MaskTexture,
        EImageLayout::General,
        EImageLayout::ShaderReadOnly,
        EPipelineStageFlags::ComputeShader,
        EPipelineStageFlags::FragmentShader,
        EAccessFlags::ShaderWrite,
        EAccessFlags::ShaderRead);

    commandBuffer->TransitionImageLayout(
        m_DepthTexture,
        EImageLayout::ShaderReadOnly,
        EImageLayout::DepthStencilAttachment,
        EPipelineStageFlags::ComputeShader,
        EPipelineStageFlags::EarlyFragmentTests,
        EAccessFlags::ShaderRead,
        EAccessFlags::DepthStencilAttachmentWrite);

    commandBuffer->EndDebugLabel();
}

// ============================================================================
// 尺寸变化与销毁
// ============================================================================

ERHIResult FRayTracedShadowPass::OnResize(const FPassResizeDesc& desc)
{
    if (m_Device == nullptr || !m_Device->IsRayTracingSupported())
    {
        return ERHIResult::Success;
    }

    m_Device->DestroyTextureView(m_MaskView);
    m_Device->DestroyTexture(m_MaskTexture);

    m_Extent = desc.Extent;

    return CreateMask(m_Device, m_Extent);
}

void FRayTracedShadowPass::ReleaseSwapchainResources(IRHIDevice* device)
{
    if (device == nullptr)
    {
        return;
    }

    device->DestroyTextureView(m_MaskView);
    device->DestroyTexture(m_MaskTexture);
}

void FRayTracedShadowPass::Shutdown(IRHIDevice* device)
{
    if (device == nullptr)
    {
        return;
    }

    device->DestroyComputePipeline(m_Pipeline);
    device->DestroyPipelineLayout(m_PipelineLayout);
    device->FreeDescriptorSet(m_DescriptorSet);
    device->DestroyDescSetLayout(m_SetLayout);
    device->DestroyShader(m_Shader);
    device->DestroySampler(m_PointSampler);

    device->DestroyTextureView(m_MaskView);
    device->DestroyTexture(m_MaskTexture);

    m_Device = nullptr;
}

} // namespace Limx
