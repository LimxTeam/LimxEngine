// ============================================================
// 文件名称：FRayTracedAoPass.cpp
// 创建时间：2026-08-31
// 创建者  ：LimxTeam
// 设计哲学：这个通道没有"效果参数"。阴影贴图那一套里的深度偏置、法线偏置、
//          PCF 半径、级联过渡宽度，每一个都是在补离散化的窟窿；光追这里
//          唯一的容差是盖住深度缓冲区自身的量化误差，而那个量算得出来。
//          留下可调旋钮的代价不是多一行代码，是从此没人知道"对"是什么。
// 功能描述：FRayTracedAoPass 的实现 — 掩码纹理、计算管线、描述符与
//          每帧派发。
// 技术特性：掩码是 R8_UNORM (Storage + Sampled)；派发前后各一次布局转换，
//          把深度转成着色器可读、掩码转成着色器可读；加速结构无效时整个
//          通道跳过，而不是画一张全亮的掩码。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                          │ 描述                      │
// │────────────────────────────────│─────────────────────────│
// │ Setup()                        │ 建掩码、管线与描述符       │
// │ CreateTarget()                   │ 建 R8 掩码纹理与视图       │
// │ CreatePipeline()               │ 建计算管线                │
// │ CreateDescriptors()            │ 建描述符集布局与集合       │
// │ Execute()                      │ 每帧派发                  │
// │ OnResize()                     │ 重建掩码                  │
// ============================================================

#include "Renderer/RenderPass/FRayTracedAoPass.h"

#include "Renderer/RayTracing/FRayTracingScene.h"

#include "RenderCore/Shaders/FShaderManager.h"

#include "RHI/RHI/IRHIDevice.h"
#include "RHI/RHI/IRHICommandBuffer.h"

namespace Limx
{

LIMX_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{

/// AO 图的格式
///
/// R8_UNORM: 遮蔽率的取值域是 [0,1], 而 8 位的量化步长 1/255 远小于蒙特
/// 卡洛本身的噪声 (16 个样本时约 0.12)。换 R16F 不会让结果更准, 只会让
/// 显存翻倍。
constexpr EPixelFormat kMaskFormat = EPixelFormat::R8_UNORM;

/// 与 rt_ao.comp 的 push constant 块逐字段一致
struct FRtAoPushConstants
{
    FMatrix InvViewProj;

    UInt32 Width       = 0;
    UInt32 Height      = 0;
    UInt32 SampleCount = 16;
    UInt32 RayMask     = 0xFFu;

    Float32 Radius       = 0.8f;
    Float32 NormalOffset = 1.0e-3f;
    Float32 RayTMin      = 1.0e-3f;

    /// 像素步长: 1 = 全分辨率, 2 = 半分辨率
    Float32 PixelStep    = 1.0f;

    Float32 NearPlane = 0.1f;
    Float32 FarPlane  = 100.0f;
    Float32 Pad1      = 0.0f;
    Float32 Pad2      = 0.0f;

    Float32 CameraX = 0.0f;
    Float32 CameraY = 0.0f;
    Float32 CameraZ = 0.0f;
    Float32 CameraW = 0.0f;
};

/// 与 rt_ao_upsample.comp 的 push constant 块逐字段一致
struct FRtAoUpsamplePushConstants
{
    UInt32 FullWidth  = 0;
    UInt32 FullHeight = 0;
    UInt32 HalfWidth  = 0;
    UInt32 HalfHeight = 0;

    Float32 NearPlane = 0.1f;
    Float32 FarPlane  = 100.0f;
    Float32 Pad0      = 0.0f;
    Float32 Pad1      = 0.0f;
};

static_assert(sizeof(FRtAoUpsamplePushConstants) == 32,
              "FRtAoUpsamplePushConstants 必须是 32 字节 — 与 "
              "rt_ao_upsample.comp 的 push constant 块逐字段一致");

static_assert(sizeof(FRtAoPushConstants) == 128,
              "FRtAoPushConstants 必须是 96 字节 — 与 rt_ao.comp 的 "
              "push constant 块逐字段一致");

} // namespace

// ============================================================================
// Setup
// ============================================================================

ERHIResult FRayTracedAoPass::Setup(const FPassSetupDesc& desc)
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
                 "[光追AO] 设备不支持光线追踪 — 通道不启用");
        m_Enabled = false;
        return ERHIResult::Success;
    }

    ERHIResult result = CreateTarget(m_Device, m_Extent);

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

    result = CreateUpsample(m_Device);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    LIMX_LOG(LogRenderer, Log,
             "[光追AO] 初始化完成 — {}x{} R8_UNORM (半分辨率 {}x{}), "
             "半径 {}, 采样 {}",
             m_Extent.Width, m_Extent.Height,
             HalfExtent().Width, HalfExtent().Height,
             m_Radius, m_SampleCount);

    return ERHIResult::Success;
}

// ============================================================================
// CreateTarget
// ============================================================================

ERHIResult FRayTracedAoPass::CreateTarget(IRHIDevice* device,
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
    texDesc.DebugName   = "RayTracedAo";

    ERHIResult result = device->CreateTexture(texDesc, m_AoTexture);

    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[光追AO] 掩码纹理创建失败");
        return result;
    }

    FRHITextureViewDesc viewDesc = {};
    viewDesc.Texture         = m_AoTexture;
    viewDesc.ViewType        = ETextureType::Texture2D;
    viewDesc.Format          = kMaskFormat;
    viewDesc.BaseMipLevel    = 0;
    viewDesc.MipLevelCount   = 1;
    viewDesc.BaseArrayLayer  = 0;
    viewDesc.ArrayLayerCount = 1;

    ERHIResult viewResult = device->CreateTextureView(viewDesc, m_AoView);

    if (!IsRHISuccess(viewResult))
    {
        return viewResult;
    }

    // 半分辨率的中间结果 —— 无条件建。
    //
    // 按需建的话, 开关一拨就要在录制命令的中途创建资源, 而那要么阻塞
    // 队列要么错过这一帧。一张 R8 的四分之一图是 230 KiB, 不值得为它
    // 引入一条"资源可能还没准备好"的路径。
    const FRHIExtent2D half = HalfExtent();

    texDesc.Extent    = { half.Width, half.Height, 1 };
    texDesc.DebugName = "RayTracedAoHalf";

    ERHIResult halfResult = device->CreateTexture(texDesc, m_HalfAoTexture);

    if (!IsRHISuccess(halfResult))
    {
        return halfResult;
    }

    viewDesc.Texture = m_HalfAoTexture;

    return device->CreateTextureView(viewDesc, m_HalfAoView);
}

// ============================================================================
// CreateUpsample — 双边上采样的管线与描述符
// ============================================================================

ERHIResult FRayTracedAoPass::CreateUpsample(IRHIDevice* device)
{
    FRHIDescriptorBinding bindings[3] = {};

    bindings[0].Binding    = 0;
    bindings[0].Type       = EDescriptorType::CombinedImageSampler;
    bindings[0].Count      = 1;
    bindings[0].StageFlags = EShaderStage::Compute;

    bindings[1].Binding    = 1;
    bindings[1].Type       = EDescriptorType::CombinedImageSampler;
    bindings[1].Count      = 1;
    bindings[1].StageFlags = EShaderStage::Compute;

    bindings[2].Binding    = 2;
    bindings[2].Type       = EDescriptorType::StorageImage;
    bindings[2].Count      = 1;
    bindings[2].StageFlags = EShaderStage::Compute;

    FRHIDescSetLayoutDesc layoutDesc = {};
    layoutDesc.Bindings     = bindings;
    layoutDesc.BindingCount = 3;
    layoutDesc.DebugName    = "RtAoUpsampleSetLayout";

    ERHIResult result =
        device->CreateDescSetLayout(layoutDesc, m_UpsampleSetLayout);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    result = device->AllocateDescriptorSet(m_UpsampleSetLayout, m_UpsampleSet);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    result = FShaderManager::Get().CreateShaderModule(
        device, FString("Builtin/rt_ao_upsample.comp"),
        EShaderStage::Compute, m_UpsampleShader);

    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
                 "[光追AO] rt_ao_upsample.comp 加载失败");
        return result;
    }

    FRHIPushConstantRange pushRange = {};
    pushRange.StageFlags = EShaderStage::Compute;
    pushRange.Offset     = 0;
    pushRange.Size       = sizeof(FRtAoUpsamplePushConstants);

    FRHIPipelineLayoutDesc pipelineLayoutDesc = {};
    pipelineLayoutDesc.SetLayouts             = &m_UpsampleSetLayout;
    pipelineLayoutDesc.SetLayoutCount         = 1;
    pipelineLayoutDesc.PushConstantRanges     = &pushRange;
    pipelineLayoutDesc.PushConstantRangeCount = 1;
    pipelineLayoutDesc.DebugName              = "RtAoUpsampleLayout";

    result = device->CreatePipelineLayout(pipelineLayoutDesc, m_UpsampleLayout);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    FRHIComputePipelineDesc pipelineDesc = {};
    pipelineDesc.ComputeShader.Shader     = m_UpsampleShader;
    pipelineDesc.ComputeShader.Stage      = EShaderStage::Compute;
    pipelineDesc.ComputeShader.EntryPoint = "main";
    pipelineDesc.PipelineLayout           = m_UpsampleLayout;
    pipelineDesc.DebugName                = "RtAoUpsamplePipeline";

    return device->CreateComputePipeline(pipelineDesc, m_UpsamplePipeline);
}

// ============================================================================
// CreateDescriptors
// ============================================================================

ERHIResult FRayTracedAoPass::CreateDescriptors(IRHIDevice* device)
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
    layoutDesc.DebugName    = "RtAoSetLayout";

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

ERHIResult FRayTracedAoPass::CreatePipeline(IRHIDevice* device)
{
    FShaderManager& shaders = FShaderManager::Get();

    if (!shaders.IsInitialized())
    {
        shaders.Initialize();
    }

    ERHIResult result = shaders.CreateShaderModule(
        device, FString("Builtin/rt_ao.comp"), EShaderStage::Compute,
        m_Shader);

    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[光追AO] rt_ao.comp 加载失败");
        return result;
    }

    FRHIPushConstantRange pushRange = {};
    pushRange.StageFlags = EShaderStage::Compute;
    pushRange.Offset     = 0;
    pushRange.Size       = sizeof(FRtAoPushConstants);

    FRHIPipelineLayoutDesc layoutDesc = {};
    layoutDesc.SetLayouts             = &m_SetLayout;
    layoutDesc.SetLayoutCount         = 1;
    layoutDesc.PushConstantRanges     = &pushRange;
    layoutDesc.PushConstantRangeCount = 1;
    layoutDesc.DebugName              = "RtAoPipelineLayout";

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
    pipelineDesc.DebugName                = "RtAoPipeline";

    return device->CreateComputePipeline(pipelineDesc, m_Pipeline);
}

// ============================================================================
// Execute
// ============================================================================

void FRayTracedAoPass::Execute(IRHICommandBuffer*        commandBuffer,
                                    const FRenderPassContext& context)
{
    (void)context;

    if (commandBuffer == nullptr || m_Device == nullptr)
    {
        return;
    }

    // 无论启不启用, 纹理都必须先离开 Undefined 布局。
    //
    // 它被绑进了光照描述符集, 而 Vulkan 在提交时检查**描述符声明的布局**
    // 与图像的实际布局是否一致 —— 与着色器读不读它无关。通道禁用时早退
    // 的话, 这张图一辈子停在 Undefined, 于是每一次提交都报错, 而报的是
    // "某个 VkImage 布局不对", 完全看不出是哪个通道没跑。
    //
    // 内容不用管: 禁用时着色器根本不会去读它 (UBO 里的开关是 -1)。
    if (!m_LayoutInitialized && m_AoTexture.IsValid())
    {
        commandBuffer->TransitionImageLayout(
            m_AoTexture,
            EImageLayout::Undefined,
            EImageLayout::ShaderReadOnly,
            EPipelineStageFlags::TopOfPipe,
            EPipelineStageFlags::FragmentShader,
            EAccessFlags::None,
            EAccessFlags::ShaderRead);

        if (m_HalfAoTexture.IsValid())
        {
            commandBuffer->TransitionImageLayout(
                m_HalfAoTexture,
                EImageLayout::Undefined,
                EImageLayout::ShaderReadOnly,
                EPipelineStageFlags::TopOfPipe,
                EPipelineStageFlags::FragmentShader,
                EAccessFlags::None,
                EAccessFlags::ShaderRead);
        }

        m_LayoutInitialized = true;
    }

    if (!m_Enabled)
    {
        return;
    }

    // 加速结构无效时整个跳过, 而不是画一张全亮的掩码。
    //
    // 全亮看起来"没有阴影", 与"这个场景本来就没有遮挡"分不开 —— 而那正是
    // 一条光追阴影判据最需要区分的两件事。跳过之后掩码保持上一帧的内容,
    // 而调用方能通过 IsEnabled 与 TLAS 是否有效自己判断。
    if (!m_Tlas.IsValid() || !m_AoView.IsValid() || !m_DepthView.IsValid())
    {
        return;
    }

    commandBuffer->BeginDebugLabel("RayTracedAo", 0.6f, 0.4f, 0.9f);

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

    // 半分辨率时求解写进半分辨率图, 再由上采样填满全分辨率图
    const bool half = m_HalfResolution;

    writes[3] = FRHIDescriptorWrite();
    writes[3].DescriptorSet = m_DescriptorSet;
    writes[3].Binding       = 3;
    writes[3].Type          = EDescriptorType::StorageImage;
    writes[3].ImageView     = half ? m_HalfAoView : m_AoView;
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
        half ? m_HalfAoTexture : m_AoTexture,
        EImageLayout::Undefined,
        EImageLayout::General,
        EPipelineStageFlags::FragmentShader,
        EPipelineStageFlags::ComputeShader,
        EAccessFlags::ShaderRead,
        EAccessFlags::ShaderWrite);

    // ---- 派发 ----
    FRtAoPushConstants push;
    push.InvViewProj = m_ViewProj.Inverse();

    // 求解范围与像素步长必须在 PushConstants 之前定下来。
    //
    // 写在派发之前但在推送之后, 着色器收到的就还是上一组值 —— 半分辨率
    // 那次它会按全分辨率的参数在四分之一的网格上跑, 算出的是屏幕左上角
    // 那一块, 再被上采样拉满整屏。而那看起来像"半分辨率精度差", 不像
    // "参数没送到"。
    const FRHIExtent2D solveExtent = half ? HalfExtent() : m_Extent;

    push.Width       = solveExtent.Width;
    push.Height      = solveExtent.Height;
    push.PixelStep   = half ? 2.0f : 1.0f;
    push.SampleCount = m_SampleCount;

    // 只让会写深度的那一类几何体参与遮蔽。
    //
    // 半透明在光栅化里不写深度, 让它遮蔽的话玻璃后面会出现一块与画面完全
    // 对不上的暗区。
    push.RayMask = kRayMaskDepthWriting;

    push.Radius       = m_Radius;
    push.NormalOffset = m_NormalOffset;
    push.RayTMin      = m_RayTMin;
    push.NearPlane    = m_NearPlane;
    push.FarPlane     = m_FarPlane;
    push.CameraX      = m_CameraPos.X;
    push.CameraY      = m_CameraPos.Y;
    push.CameraZ      = m_CameraPos.Z;

    commandBuffer->BindComputePipeline(m_Pipeline);
    commandBuffer->BindDescriptorSet(EPipelineBindPoint::Compute,
                                      m_PipelineLayout, 0, m_DescriptorSet);
    commandBuffer->PushConstants(m_PipelineLayout, EShaderStage::Compute, 0,
                                  sizeof(push), &push);

    constexpr UInt32 kGroup = 8;
    commandBuffer->Dispatch((solveExtent.Width + kGroup - 1) / kGroup,
                             (solveExtent.Height + kGroup - 1) / kGroup, 1);

    // ---- 交还布局 ----
    commandBuffer->TransitionImageLayout(
        half ? m_HalfAoTexture : m_AoTexture,
        EImageLayout::General,
        EImageLayout::ShaderReadOnly,
        EPipelineStageFlags::ComputeShader,
        EPipelineStageFlags::FragmentShader,
        EAccessFlags::ShaderWrite,
        EAccessFlags::ShaderRead);

    // ---- 上采样 ----
    if (half)
    {
        FRHIDescriptorWrite upsampleWrites[3];

        upsampleWrites[0] = FRHIDescriptorWrite();
        upsampleWrites[0].DescriptorSet = m_UpsampleSet;
        upsampleWrites[0].Binding       = 0;
        upsampleWrites[0].Type          = EDescriptorType::CombinedImageSampler;
        upsampleWrites[0].ImageView     = m_HalfAoView;
        upsampleWrites[0].Sampler       = m_PointSampler;
        upsampleWrites[0].ImageLayout   = EImageLayout::ShaderReadOnly;

        upsampleWrites[1] = FRHIDescriptorWrite();
        upsampleWrites[1].DescriptorSet = m_UpsampleSet;
        upsampleWrites[1].Binding       = 1;
        upsampleWrites[1].Type          = EDescriptorType::CombinedImageSampler;
        upsampleWrites[1].ImageView     = m_DepthView;
        upsampleWrites[1].Sampler       = m_PointSampler;
        upsampleWrites[1].ImageLayout   = EImageLayout::ShaderReadOnly;

        upsampleWrites[2] = FRHIDescriptorWrite();
        upsampleWrites[2].DescriptorSet = m_UpsampleSet;
        upsampleWrites[2].Binding       = 2;
        upsampleWrites[2].Type          = EDescriptorType::StorageImage;
        upsampleWrites[2].ImageView     = m_AoView;
        upsampleWrites[2].ImageLayout   = EImageLayout::General;

        m_Device->UpdateDescriptorSets(upsampleWrites, 3);

        commandBuffer->TransitionImageLayout(
            m_AoTexture,
            EImageLayout::Undefined,
            EImageLayout::General,
            EPipelineStageFlags::FragmentShader,
            EPipelineStageFlags::ComputeShader,
            EAccessFlags::ShaderRead,
            EAccessFlags::ShaderWrite);

        FRtAoUpsamplePushConstants upsamplePush;
        upsamplePush.FullWidth  = m_Extent.Width;
        upsamplePush.FullHeight = m_Extent.Height;
        upsamplePush.HalfWidth  = HalfExtent().Width;
        upsamplePush.HalfHeight = HalfExtent().Height;
        upsamplePush.NearPlane  = m_NearPlane;
        upsamplePush.FarPlane   = m_FarPlane;

        commandBuffer->BindComputePipeline(m_UpsamplePipeline);
        commandBuffer->BindDescriptorSet(EPipelineBindPoint::Compute,
                                          m_UpsampleLayout, 0, m_UpsampleSet);
        commandBuffer->PushConstants(m_UpsampleLayout, EShaderStage::Compute,
                                      0, sizeof(upsamplePush), &upsamplePush);

        commandBuffer->Dispatch((m_Extent.Width + kGroup - 1) / kGroup,
                                 (m_Extent.Height + kGroup - 1) / kGroup, 1);

        commandBuffer->TransitionImageLayout(
            m_AoTexture,
            EImageLayout::General,
            EImageLayout::ShaderReadOnly,
            EPipelineStageFlags::ComputeShader,
            EPipelineStageFlags::FragmentShader,
            EAccessFlags::ShaderWrite,
            EAccessFlags::ShaderRead);
    }

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

ERHIResult FRayTracedAoPass::OnResize(const FPassResizeDesc& desc)
{
    if (m_Device == nullptr || !m_Device->IsRayTracingSupported())
    {
        return ERHIResult::Success;
    }

    m_Device->DestroyTextureView(m_HalfAoView);
    m_Device->DestroyTexture(m_HalfAoTexture);
    m_Device->DestroyTextureView(m_AoView);
    m_Device->DestroyTexture(m_AoTexture);

    m_Extent = desc.Extent;

    // 纹理换了 —— 布局要重新初始化
    m_LayoutInitialized = false;

    return CreateTarget(m_Device, m_Extent);
}

void FRayTracedAoPass::ReleaseSwapchainResources(IRHIDevice* device)
{
    if (device == nullptr)
    {
        return;
    }

    device->DestroyTextureView(m_HalfAoView);
    device->DestroyTexture(m_HalfAoTexture);
    device->DestroyTextureView(m_AoView);
    device->DestroyTexture(m_AoTexture);
}

void FRayTracedAoPass::Shutdown(IRHIDevice* device)
{
    if (device == nullptr)
    {
        return;
    }

    device->DestroyComputePipeline(m_UpsamplePipeline);
    device->DestroyPipelineLayout(m_UpsampleLayout);
    device->FreeDescriptorSet(m_UpsampleSet);
    device->DestroyDescSetLayout(m_UpsampleSetLayout);
    device->DestroyShader(m_UpsampleShader);

    device->DestroyTextureView(m_HalfAoView);
    device->DestroyTexture(m_HalfAoTexture);

    device->DestroyComputePipeline(m_Pipeline);
    device->DestroyPipelineLayout(m_PipelineLayout);
    device->FreeDescriptorSet(m_DescriptorSet);
    device->DestroyDescSetLayout(m_SetLayout);
    device->DestroyShader(m_Shader);
    device->DestroySampler(m_PointSampler);

    device->DestroyTextureView(m_AoView);
    device->DestroyTexture(m_AoTexture);

    m_Device = nullptr;
}

} // namespace Limx
