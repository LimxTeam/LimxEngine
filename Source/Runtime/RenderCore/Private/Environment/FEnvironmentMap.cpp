/*******************************************************************************
 * 文件: FEnvironmentMap.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   环境立方体贴图实现 — 上传等距柱状图、计算着色器转换、资源生命周期
 *
 * 设计哲学:
 *   转换用的资源在转换完成后立刻释放 — 暂存缓冲区、源纹理、计算管线、
 *   描述符集只在这一次 Dispatch 里有意义, 而源纹理是整套资源里最大的一块
 *   (2K HDRI 的 RGBA32F 形态是 32 MB)。留着它就是白占显存。
 *
 *   六个面一次 Dispatch 而非六次 — 立方体贴图的层在 Vulkan 里就是数组层,
 *   分派网格的 z 维直接对应层索引。分六次不会更快, 只会多五次提交开销,
 *   还要额外考虑层间的同步。
 *
 * 依赖关系:
 *   内部: RenderCore/Environment/FEnvironmentMap.h,
 *         RenderCore/Renderer/FRenderContext.h,
 *         RenderCore/Shaders/FShaderManager.h
 *
 ******************************************************************************/

#include "RenderCore/Environment/FEnvironmentMap.h"
#include "RenderCore/Renderer/FRenderContext.h"
#include "RenderCore/Shaders/FShaderManager.h"

namespace Limx
{

LIMX_DECLARE_LOG_CATEGORY(LogEnvironment)
LIMX_DEFINE_LOG_CATEGORY(LogEnvironment)

namespace
{

/// 计算着色器的线程组边长 —— 必须与 equirect_to_cube.comp 的 local_size 一致
constexpr UInt32 kThreadGroupSize = 8;

/// 立方体贴图的面数
constexpr UInt32 kCubeFaceCount = 6;

/// 面边长的下限与上限
///
/// 下限 16: 再小的话每个面不足两个线程组, 天空会糊成几块色斑;
/// 上限 4096: 六个面的 RGBA16F 合计 768 MB, 已经超出任何合理预算。
constexpr UInt32 kMinFaceSize = 16;
constexpr UInt32 kMaxFaceSize = 4096;

/// 面边长向上取到线程组边长的整数倍
UInt32 AlignFaceSize(UInt32 faceSize)
{
    const UInt32 remainder = faceSize % kThreadGroupSize;

    if (remainder == 0)
    {
        return faceSize;
    }

    return faceSize + (kThreadGroupSize - remainder);
}

struct FConversionPushConstant
{
    UInt32 FaceSize = 0;
    UInt32 Pad0     = 0;
    UInt32 Pad1     = 0;
    UInt32 Pad2     = 0;
};

} // namespace

// ============================================================================
// 生命周期
// ============================================================================

FEnvironmentMap::~FEnvironmentMap()
{
    Release();
}

void FEnvironmentMap::Release()
{
    if (m_Device == nullptr)
    {
        return;
    }

    ReleaseConversionResources(m_Device);

    if (m_Sampler.IsValid())
    {
        m_Device->DestroySampler(m_Sampler);
    }

    if (m_CubeView.IsValid())
    {
        m_Device->DestroyTextureView(m_CubeView);
    }

    if (m_StorageView.IsValid())
    {
        m_Device->DestroyTextureView(m_StorageView);
    }

    if (m_CubeTexture.IsValid())
    {
        m_Device->DestroyTexture(m_CubeTexture);
    }

    m_FaceSize = 0;
    m_Device   = nullptr;
}

void FEnvironmentMap::ReleaseConversionResources(IRHIDevice* device)
{
    if (m_Pipeline.IsValid())
    {
        device->DestroyComputePipeline(m_Pipeline);
    }

    if (m_PipelineLayout.IsValid())
    {
        device->DestroyPipelineLayout(m_PipelineLayout);
    }

    if (m_DescSetLayout.IsValid())
    {
        device->DestroyDescSetLayout(m_DescSetLayout);
    }

    if (m_ComputeShader.IsValid())
    {
        device->DestroyShader(m_ComputeShader);
    }

    if (m_EquirectSampler.IsValid())
    {
        device->DestroySampler(m_EquirectSampler);
    }

    if (m_EquirectView.IsValid())
    {
        device->DestroyTextureView(m_EquirectView);
    }

    if (m_EquirectTexture.IsValid())
    {
        device->DestroyTexture(m_EquirectTexture);
    }

    if (m_StagingBuffer.IsValid())
    {
        device->DestroyBuffer(m_StagingBuffer);
    }

    // 描述符集必须显式归还池 —— 池是全局的, 每次换环境贴图都占一个而不还,
    // 反复切换关卡后会耗尽。销毁布局并不会连带回收从池里分配出去的集。
    if (m_DescriptorSet.IsValid())
    {
        device->FreeDescriptorSet(m_DescriptorSet);
    }
}

SizeType FEnvironmentMap::GetMemoryBytes() const
{
    // RGBA16F = 8 字节/纹素
    return static_cast<SizeType>(m_FaceSize) * m_FaceSize * kCubeFaceCount * 8;
}

// ============================================================================
// BuildFromEquirect
// ============================================================================

ERHIResult FEnvironmentMap::BuildFromEquirect(FRenderContext* context,
                                              const FImageData& equirect,
                                              UInt32 faceSize)
{
    if (context == nullptr || context->GetDevice() == nullptr)
    {
        LIMX_LOG(LogEnvironment, Error, "[EnvironmentMap] 渲染上下文无效");
        return ERHIResult::ErrorInvalidParameter;
    }

    if (!equirect.IsValid())
    {
        LIMX_LOG(LogEnvironment, Error, "[EnvironmentMap] 源图无效");
        return ERHIResult::ErrorInvalidParameter;
    }

    // 只接受浮点源。整数格式意味着亮度已被截断到 [0,1] ——
    // 那样转出来的立方体贴图在名义上是 HDR, 实际动态范围与一张 PNG 无异。
    if (!IsImageFormatFloat(equirect.Format))
    {
        LIMX_LOG(LogEnvironment, Error,
                 "[EnvironmentMap] 源图不是浮点格式, 无法作为 HDR 环境图");
        return ERHIResult::ErrorInvalidParameter;
    }

    if (equirect.GetChannelCount() != 4)
    {
        LIMX_LOG(LogEnvironment, Error,
                 "[EnvironmentMap] 源图需为四通道 (当前 {} 通道)",
                 equirect.GetChannelCount());
        return ERHIResult::ErrorInvalidParameter;
    }

    Release();

    m_Device = context->GetDevice();

    // ---- 决定面边长 ----
    UInt32 resolvedFaceSize = faceSize;

    if (resolvedFaceSize == 0)
    {
        resolvedFaceSize = equirect.Width / 4;
    }

    if (resolvedFaceSize < kMinFaceSize)
    {
        resolvedFaceSize = kMinFaceSize;
    }

    if (resolvedFaceSize > kMaxFaceSize)
    {
        resolvedFaceSize = kMaxFaceSize;
    }

    resolvedFaceSize = AlignFaceSize(resolvedFaceSize);

    m_FaceSize = resolvedFaceSize;

    // ---- 建资源 ----
    ERHIResult result = UploadEquirect(context, equirect, m_EquirectTexture,
                                       m_EquirectView, m_StagingBuffer);

    if (!IsRHISuccess(result))
    {
        Release();
        return result;
    }

    result = CreateCubeResources(m_Device, m_FaceSize);

    if (!IsRHISuccess(result))
    {
        Release();
        return result;
    }

    result = CreateConversionPipeline(m_Device);

    if (!IsRHISuccess(result))
    {
        Release();
        return result;
    }

    // ---- 分派 ----
    IRHICommandBuffer* commandBuffer = context->BeginSingleTimeCommands();

    if (commandBuffer == nullptr)
    {
        Release();
        return ERHIResult::ErrorUnknown;
    }

    // 立方体贴图转入 General —— 存储图像的写入必须在这个布局下进行
    commandBuffer->TransitionImageLayout(
        m_CubeTexture,
        EImageLayout::Undefined,
        EImageLayout::General,
        EPipelineStageFlags::TopOfPipe,
        EPipelineStageFlags::ComputeShader,
        EAccessFlags::None,
        EAccessFlags::ShaderWrite,
        0, 1, 0, kCubeFaceCount);

    commandBuffer->BindComputePipeline(m_Pipeline);

    commandBuffer->BindDescriptorSet(EPipelineBindPoint::Compute,
                                     m_PipelineLayout, 0, m_DescriptorSet);

    FConversionPushConstant pushConstant;
    pushConstant.FaceSize = m_FaceSize;

    commandBuffer->PushConstants(m_PipelineLayout, EShaderStage::Compute,
                                 0, sizeof(FConversionPushConstant),
                                 &pushConstant);

    const UInt32 groupCount = m_FaceSize / kThreadGroupSize;

    commandBuffer->Dispatch(groupCount, groupCount, kCubeFaceCount);

    // 写完转为着色器只读 —— 这次转换同时充当"计算写"到"片段读"的屏障
    commandBuffer->TransitionImageLayout(
        m_CubeTexture,
        EImageLayout::General,
        EImageLayout::ShaderReadOnly,
        EPipelineStageFlags::ComputeShader,
        EPipelineStageFlags::FragmentShader,
        EAccessFlags::ShaderWrite,
        EAccessFlags::ShaderRead,
        0, 1, 0, kCubeFaceCount);

    context->EndSingleTimeCommands(commandBuffer);

    // 转换资源到此为止 —— 源纹理是整套里最大的一块, 留着就是白占显存
    ReleaseConversionResources(m_Device);

    LIMX_LOG(LogEnvironment, Display,
             "[EnvironmentMap] 立方体贴图生成完成: {}x{} x6, {} MB "
             "(源图 {}x{})",
             m_FaceSize, m_FaceSize,
             GetMemoryBytes() / (1024 * 1024),
             equirect.Width, equirect.Height);

    return ERHIResult::Success;
}

// ============================================================================
// UploadEquirect
// ============================================================================

ERHIResult FEnvironmentMap::UploadEquirect(FRenderContext* context,
                                           const FImageData& equirect,
                                           FRHITextureHandle& outTexture,
                                           FRHITextureViewHandle& outView,
                                           FRHIBufferHandle& outStaging)
{
    IRHIDevice* device = context->GetDevice();

    const SizeType byteSize = equirect.Pixels.GetSize();

    FRHIBufferDesc stagingDesc = {};
    stagingDesc.Size        = byteSize;
    stagingDesc.Usage       = EBufferUsage::TransferSrc;
    stagingDesc.MemoryUsage = EMemoryUsage::CpuToGpu;
    stagingDesc.DebugName   = "EnvironmentMap.Staging";

    ERHIResult result = device->CreateBuffer(stagingDesc, outStaging);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    void* mapped = nullptr;
    result       = device->MapBuffer(outStaging, &mapped);

    if (!IsRHISuccess(result) || mapped == nullptr)
    {
        return IsRHISuccess(result) ? ERHIResult::ErrorUnknown : result;
    }

    UInt8*       destination = static_cast<UInt8*>(mapped);
    const UInt8* source      = equirect.Pixels.GetData();

    for (SizeType i = 0; i < byteSize; ++i)
    {
        destination[i] = source[i];
    }

    device->UnmapBuffer(outStaging);

    FRHITextureDesc textureDesc = {};
    textureDesc.Type          = ETextureType::Texture2D;
    textureDesc.Format        = EPixelFormat::RGBA32_SFLOAT;
    textureDesc.Extent.Width  = equirect.Width;
    textureDesc.Extent.Height = equirect.Height;
    textureDesc.Extent.Depth  = 1;
    textureDesc.MipLevels     = 1;
    textureDesc.ArrayLayers   = 1;
    textureDesc.Usage         = static_cast<ETextureUsage>(
        static_cast<UInt32>(ETextureUsage::Sampled) |
        static_cast<UInt32>(ETextureUsage::TransferDst));
    textureDesc.MemoryUsage   = EMemoryUsage::GpuOnly;
    textureDesc.DebugName     = "EnvironmentMap.Equirect";

    result = device->CreateTexture(textureDesc, outTexture);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    IRHICommandBuffer* commandBuffer = context->BeginSingleTimeCommands();

    if (commandBuffer == nullptr)
    {
        return ERHIResult::ErrorUnknown;
    }

    commandBuffer->TransitionImageLayout(
        outTexture,
        EImageLayout::Undefined,
        EImageLayout::TransferDst,
        EPipelineStageFlags::TopOfPipe,
        EPipelineStageFlags::Transfer,
        EAccessFlags::None,
        EAccessFlags::TransferWrite);

    FRHIBufferTextureCopyRegion region = {};
    region.BufferOffset      = 0;
    region.BufferRowLength   = 0;
    region.BufferImageHeight = 0;
    region.MipLevel          = 0;
    region.BaseLayer         = 0;
    region.LayerCount        = 1;
    region.TextureOffset     = { 0, 0, 0 };
    region.TextureExtent     = { equirect.Width, equirect.Height, 1 };

    commandBuffer->CopyBufferToTexture(outStaging, outTexture,
                                       EImageLayout::TransferDst, region);

    // 目标是计算着色器而非片段着色器 —— 转换发生在 Dispatch 里
    commandBuffer->TransitionImageLayout(
        outTexture,
        EImageLayout::TransferDst,
        EImageLayout::ShaderReadOnly,
        EPipelineStageFlags::Transfer,
        EPipelineStageFlags::ComputeShader,
        EAccessFlags::TransferWrite,
        EAccessFlags::ShaderRead);

    context->EndSingleTimeCommands(commandBuffer);

    FRHITextureViewDesc viewDesc = {};
    viewDesc.Texture         = outTexture;
    viewDesc.ViewType        = ETextureType::Texture2D;
    viewDesc.Format          = EPixelFormat::RGBA32_SFLOAT;
    viewDesc.BaseMipLevel    = 0;
    viewDesc.MipLevelCount   = 1;
    viewDesc.BaseArrayLayer  = 0;
    viewDesc.ArrayLayerCount = 1;

    return device->CreateTextureView(viewDesc, outView);
}

// ============================================================================
// CreateCubeResources
// ============================================================================

ERHIResult FEnvironmentMap::CreateCubeResources(IRHIDevice* device,
                                                UInt32 faceSize)
{
    FRHITextureDesc cubeDesc = {};
    cubeDesc.Type          = ETextureType::TextureCube;
    cubeDesc.Format        = EPixelFormat::RGBA16_SFLOAT;
    cubeDesc.Extent.Width  = faceSize;
    cubeDesc.Extent.Height = faceSize;
    cubeDesc.Extent.Depth  = 1;
    cubeDesc.MipLevels     = 1;
    cubeDesc.ArrayLayers   = kCubeFaceCount;
    cubeDesc.Usage         = static_cast<ETextureUsage>(
        static_cast<UInt32>(ETextureUsage::Sampled) |
        static_cast<UInt32>(ETextureUsage::Storage));
    cubeDesc.MemoryUsage   = EMemoryUsage::GpuOnly;
    cubeDesc.DebugName     = "EnvironmentMap.Cube";

    ERHIResult result = device->CreateTexture(cubeDesc, m_CubeTexture);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    // 采样视图 —— 立方体类型, 六层一并覆盖
    FRHITextureViewDesc cubeViewDesc = {};
    cubeViewDesc.Texture         = m_CubeTexture;
    cubeViewDesc.ViewType        = ETextureType::TextureCube;
    cubeViewDesc.Format          = EPixelFormat::RGBA16_SFLOAT;
    cubeViewDesc.BaseMipLevel    = 0;
    cubeViewDesc.MipLevelCount   = 1;
    cubeViewDesc.BaseArrayLayer  = 0;
    cubeViewDesc.ArrayLayerCount = kCubeFaceCount;

    result = device->CreateTextureView(cubeViewDesc, m_CubeView);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    // 存储视图 —— 2D 数组类型, 供计算着色器逐纹素写入
    FRHITextureViewDesc storageViewDesc = cubeViewDesc;
    storageViewDesc.ViewType            = ETextureType::Texture2DArray;

    result = device->CreateTextureView(storageViewDesc, m_StorageView);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    // 采样器: 线性 + 钳位。立方体贴图的跨面过滤由硬件负责,
    // 寻址模式只在极少数实现上影响接缝, 钳位是最保守的选择。
    FRHISamplerDesc samplerDesc  = FRHISamplerDesc::LinearClamp();
    samplerDesc.IsAnisotropyEnabled = false;
    samplerDesc.MaxLod              = 1.0f;

    return device->CreateSampler(samplerDesc, m_Sampler);
}

// ============================================================================
// CreateConversionPipeline
// ============================================================================

ERHIResult FEnvironmentMap::CreateConversionPipeline(IRHIDevice* device)
{
    FShaderManager& shaderManager = FShaderManager::Get();

    if (!shaderManager.IsInitialized())
    {
        shaderManager.Initialize();
    }

    ERHIResult result = shaderManager.CreateShaderModule(
        device, FString("Builtin/equirect_to_cube.comp"),
        EShaderStage::Compute, m_ComputeShader);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    // 源图采样器: 线性 + u 方向重复
    //
    // u 必须 Repeat 而非 Clamp: 等距柱状图在 ±180° 处首尾相接,
    // 钳位会让接缝处的双线性插值取到边缘像素的复制, 表现为天空中
    // 一条竖直的亮暗不连续线。v 方向则必须钳位 —— 天顶之上没有内容。
    FRHISamplerDesc equirectSamplerDesc;
    equirectSamplerDesc.MinFilter           = EFilter::Linear;
    equirectSamplerDesc.MagFilter           = EFilter::Linear;
    equirectSamplerDesc.MipmapMode          = ESamplerMipmapMode::Nearest;
    equirectSamplerDesc.AddressModeU        = ESamplerAddressMode::Repeat;
    equirectSamplerDesc.AddressModeV        = ESamplerAddressMode::ClampToEdge;
    equirectSamplerDesc.AddressModeW        = ESamplerAddressMode::ClampToEdge;
    equirectSamplerDesc.IsAnisotropyEnabled = false;
    equirectSamplerDesc.MaxLod              = 1.0f;

    result = device->CreateSampler(equirectSamplerDesc, m_EquirectSampler);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    FRHIDescriptorBinding bindings[2] = {};
    bindings[0].Binding    = 0;
    bindings[0].Type       = EDescriptorType::CombinedImageSampler;
    bindings[0].Count      = 1;
    bindings[0].StageFlags = EShaderStage::Compute;

    bindings[1].Binding    = 1;
    bindings[1].Type       = EDescriptorType::StorageImage;
    bindings[1].Count      = 1;
    bindings[1].StageFlags = EShaderStage::Compute;

    FRHIDescSetLayoutDesc layoutDesc = {};
    layoutDesc.Bindings     = bindings;
    layoutDesc.BindingCount = 2;
    layoutDesc.DebugName    = "EnvironmentMap.Conversion";

    result = device->CreateDescSetLayout(layoutDesc, m_DescSetLayout);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    FRHIPushConstantRange pushRange = {};
    pushRange.StageFlags = EShaderStage::Compute;
    pushRange.Offset     = 0;
    pushRange.Size       = sizeof(FConversionPushConstant);

    FRHIPipelineLayoutDesc pipelineLayoutDesc = {};
    pipelineLayoutDesc.SetLayouts             = &m_DescSetLayout;
    pipelineLayoutDesc.SetLayoutCount         = 1;
    pipelineLayoutDesc.PushConstantRanges     = &pushRange;
    pipelineLayoutDesc.PushConstantRangeCount = 1;
    pipelineLayoutDesc.DebugName              = "EnvironmentMap.Conversion";

    result = device->CreatePipelineLayout(pipelineLayoutDesc, m_PipelineLayout);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    result = device->AllocateDescriptorSet(m_DescSetLayout, m_DescriptorSet);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    FRHIDescriptorWrite writes[2] = {};
    writes[0] = FRHIDescriptorWrite::CombinedImageSampler(
        m_DescriptorSet, 0, m_EquirectView, m_EquirectSampler,
        EImageLayout::ShaderReadOnly);

    // 存储图像必须以 General 布局绑定 —— 它是唯一允许着色器写入的布局
    writes[1].DescriptorSet = m_DescriptorSet;
    writes[1].Binding       = 1;
    writes[1].Type          = EDescriptorType::StorageImage;
    writes[1].ImageView     = m_StorageView;
    writes[1].ImageLayout   = EImageLayout::General;

    device->UpdateDescriptorSets(writes, 2);

    FRHIComputePipelineDesc pipelineDesc = {};
    pipelineDesc.ComputeShader.Shader     = m_ComputeShader;
    pipelineDesc.ComputeShader.Stage      = EShaderStage::Compute;
    pipelineDesc.ComputeShader.EntryPoint = "main";
    pipelineDesc.PipelineLayout           = m_PipelineLayout;
    pipelineDesc.DebugName                = "EnvironmentMap.EquirectToCube";

    return device->CreateComputePipeline(pipelineDesc, m_Pipeline);
}

} // namespace Limx
