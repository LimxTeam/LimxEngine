/*******************************************************************************
 * 文件: FEnvironmentMap.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   环境立方体贴图实现 — 上传等距柱状图、两趟计算着色器、资源生命周期
 *
 * 设计哲学:
 *   转换用的资源在转换完成后立刻释放 — 暂存缓冲区、源纹理、计算管线、
 *   描述符集只在那两次 Dispatch 里有意义, 而源纹理是整套资源里最大的一块
 *   (2K HDRI 的 RGBA32F 形态是 32 MB)。留着它就是白占显存。
 *
 *   六个面一次 Dispatch 而非六次 — 立方体贴图的层在 Vulkan 里就是数组层,
 *   分派网格的 z 维直接对应层索引。分六次不会更快, 只会多五次提交开销,
 *   还要额外考虑层间的同步。
 *
 *   两趟转换共用一个"建计算管线 → 分派 → 拆管线"的骨架 (FComputePass)。
 *   它们的差别只有着色器、绑定的两张贴图与推送常量; 抄一遍只会让两处的
 *   屏障逐渐走样 —— 而屏障写错的表现是间歇性的花屏, 极难复现。
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

/// 计算着色器的线程组边长 —— 必须与两个 .comp 的 local_size 一致
constexpr UInt32 kThreadGroupSize = 8;

/// 立方体贴图的面数
constexpr UInt32 kCubeFaceCount = 6;

/// 面边长的下限与上限
///
/// 下限 16: 再小的话每个面不足两个线程组, 天空会糊成几块色斑;
/// 上限 4096: 六个面的 RGBA16F 合计 768 MB, 已经超出任何合理预算。
constexpr UInt32 kMinFaceSize = 16;
constexpr UInt32 kMaxFaceSize = 4096;

/// 半精度浮点能表示的最大有限值
constexpr Float32 kHalfMaxValue = 65504.0f;

/// 推送常量 —— 四个着色器共用同一块布局
///
/// 每个着色器只用其中几个字段, 其余原样忽略。共用一块布局意味着只需一份
/// 管线布局定义, 也省掉了"这一趟该填哪个结构"的判断 —— 而那正是最容易
/// 出错的地方: 填错了不会报错, 只会算出一张看着差不多的贴图。
struct FConvertPushConstant
{
    UInt32  FaceSize       = 0;
    Float32 SampleDelta    = 0.0f;
    Float32 SourceMipLevel = 0.0f;
    Float32 Roughness      = 0.0f;
    UInt32  SourceFaceSize = 0;
    UInt32  SampleCount    = 0;
    UInt32  Pad0           = 0;
    UInt32  Pad1           = 0;
};

static_assert(sizeof(FConvertPushConstant) == 32,
              "FConvertPushConstant 必须为 32 字节以匹配着色器布局");

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


/// 半精度浮点转单精度
///
/// 自己写而非调库: 引擎不用 CRT, 而这个转换本身只是位域重排 —— 指数偏置
/// 从 15 换到 127, 尾数左移 13 位。非规格化数要单独处理: 它们的隐含位是 0
/// 而非 1, 按常规公式解会得到一个大得离谱的值。
Float32 HalfToFloat(UInt16 half)
{
    const UInt32 sign     = static_cast<UInt32>(half >> 15) & 0x1u;
    const UInt32 exponent = static_cast<UInt32>(half >> 10) & 0x1Fu;
    const UInt32 mantissa = static_cast<UInt32>(half) & 0x3FFu;

    UInt32 bits = sign << 31;

    if (exponent == 0)
    {
        if (mantissa != 0)
        {
            // 非规格化: 归一化到单精度的规格化区间。半精度最小非规格化数
            // 是 2^-24, 单精度完全表示得下, 因此不会再次退化。
            UInt32 shifted = mantissa;
            UInt32 e       = 127 - 15 + 1;

            while ((shifted & 0x400u) == 0)
            {
                shifted <<= 1;
                --e;
            }

            shifted &= 0x3FFu;
            bits |= (e << 23) | (shifted << 13);
        }
        // mantissa 为 0 时就是 ±0, bits 保持只有符号位
    }
    else if (exponent == 31)
    {
        // Inf / NaN —— 指数全 1 原样搬过去
        bits |= (0xFFu << 23) | (mantissa << 13);
    }
    else
    {
        bits |= ((exponent - 15 + 127) << 23) | (mantissa << 13);
    }

    Float32 result = 0.0f;
    Memory::MemCopy(&result, &bits, sizeof(Float32));

    return result;
}

// ============================================================================
// FComputePass — "一张采样贴图 + 一张存储贴图 + 一个推送常量"的计算通道
// ============================================================================

/// 两趟转换的共同骨架。构造即建管线, 析构即拆管线。
class FComputePass
{
public:
    FComputePass() = default;

    ~FComputePass()
    {
        Release();
    }

    LIMX_NON_COPYABLE(FComputePass);
    LIMX_NON_MOVABLE(FComputePass);

    /// @param sourceView   源贴图视图; 无效表示本趟不需要输入 (BRDF 查找表)
    /// @param storageViews 输出存储视图数组 —— 每个视图分配一个描述符集
    ///
    /// 逐视图一个描述符集而非逐视图重建管线: 预滤波要逐级 mip 写入, 六级
    /// 之间只有绑定的存储视图与粗糙度不同, 管线与布局完全一样。
    ERHIResult Create(IRHIDevice*                  device,
                      const AnsiChar*              shaderPath,
                      const AnsiChar*              debugName,
                      FRHITextureViewHandle        sourceView,
                      FRHISamplerHandle            sourceSampler,
                      const FRHITextureViewHandle* storageViews,
                      UInt32                       storageViewCount)
    {
        m_Device    = device;
        m_HasSource = sourceView.IsValid();

        FShaderManager& shaderManager = FShaderManager::Get();

        if (!shaderManager.IsInitialized())
        {
            shaderManager.Initialize();
        }

        ERHIResult result = shaderManager.CreateShaderModule(
            device, FString(shaderPath), EShaderStage::Compute, m_Shader);

        if (!IsRHISuccess(result))
        {
            return result;
        }

        // 没有输入贴图时 binding 0 整个不出现。绑定号不必连续, 而声明一个
        // 着色器根本没用到的描述符会让验证层抱怨"布局与着色器不匹配"。
        FRHIDescriptorBinding bindings[2] = {};
        UInt32                bindingCount = 0;

        if (m_HasSource)
        {
            bindings[bindingCount].Binding    = 0;
            bindings[bindingCount].Type       =
                EDescriptorType::CombinedImageSampler;
            bindings[bindingCount].Count      = 1;
            bindings[bindingCount].StageFlags = EShaderStage::Compute;
            ++bindingCount;
        }

        bindings[bindingCount].Binding    = 1;
        bindings[bindingCount].Type       = EDescriptorType::StorageImage;
        bindings[bindingCount].Count      = 1;
        bindings[bindingCount].StageFlags = EShaderStage::Compute;
        ++bindingCount;

        FRHIDescSetLayoutDesc layoutDesc = {};
        layoutDesc.Bindings     = bindings;
        layoutDesc.BindingCount = bindingCount;
        layoutDesc.DebugName    = debugName;

        result = device->CreateDescSetLayout(layoutDesc, m_DescSetLayout);

        if (!IsRHISuccess(result))
        {
            return result;
        }

        FRHIPushConstantRange pushRange = {};
        pushRange.StageFlags = EShaderStage::Compute;
        pushRange.Offset     = 0;
        pushRange.Size       = sizeof(FConvertPushConstant);

        FRHIPipelineLayoutDesc pipelineLayoutDesc = {};
        pipelineLayoutDesc.SetLayouts             = &m_DescSetLayout;
        pipelineLayoutDesc.SetLayoutCount         = 1;
        pipelineLayoutDesc.PushConstantRanges     = &pushRange;
        pipelineLayoutDesc.PushConstantRangeCount = 1;
        pipelineLayoutDesc.DebugName              = debugName;

        result = device->CreatePipelineLayout(pipelineLayoutDesc,
                                              m_PipelineLayout);

        if (!IsRHISuccess(result))
        {
            return result;
        }

        m_DescriptorSets.Reserve(storageViewCount);

        for (UInt32 i = 0; i < storageViewCount; ++i)
        {
            FRHIDescriptorSetHandle descriptorSet;

            result = device->AllocateDescriptorSet(m_DescSetLayout,
                                                   descriptorSet);

            if (!IsRHISuccess(result))
            {
                return result;
            }

            FRHIDescriptorWrite writes[2] = {};
            UInt32              writeCount = 0;

            if (m_HasSource)
            {
                writes[writeCount] =
                    FRHIDescriptorWrite::CombinedImageSampler(
                        descriptorSet, 0, sourceView, sourceSampler,
                        EImageLayout::ShaderReadOnly);
                ++writeCount;
            }

            // 存储图像必须以 General 布局绑定 —— 它是唯一允许着色器写入的
            writes[writeCount].DescriptorSet = descriptorSet;
            writes[writeCount].Binding       = 1;
            writes[writeCount].Type          = EDescriptorType::StorageImage;
            writes[writeCount].ImageView     = storageViews[i];
            writes[writeCount].ImageLayout   = EImageLayout::General;
            ++writeCount;

            device->UpdateDescriptorSets(writes, writeCount);

            m_DescriptorSets.Add(descriptorSet);
        }

        FRHIComputePipelineDesc pipelineDesc = {};
        pipelineDesc.ComputeShader.Shader     = m_Shader;
        pipelineDesc.ComputeShader.Stage      = EShaderStage::Compute;
        pipelineDesc.ComputeShader.EntryPoint = "main";
        pipelineDesc.PipelineLayout           = m_PipelineLayout;
        pipelineDesc.DebugName                = debugName;

        return device->CreateComputePipeline(pipelineDesc, m_Pipeline);
    }

    /// 录制一次分派 —— **不含**任何布局转换
    ///
    /// 转换留给调用方: 一次写一级 mip 与一次写整张贴图, 需要的屏障范围
    /// 完全不同, 塞进这里只会变成一堆参数和分支。
    void Dispatch(IRHICommandBuffer*          commandBuffer,
                  UInt32                      setIndex,
                  UInt32                      groupCountX,
                  UInt32                      groupCountY,
                  UInt32                      groupCountZ,
                  const FConvertPushConstant& pushConstant) const
    {
        if (setIndex >= m_DescriptorSets.GetSize())
        {
            return;
        }

        commandBuffer->BindComputePipeline(m_Pipeline);

        commandBuffer->BindDescriptorSet(EPipelineBindPoint::Compute,
                                         m_PipelineLayout, 0,
                                         m_DescriptorSets[setIndex]);

        commandBuffer->PushConstants(m_PipelineLayout, EShaderStage::Compute,
                                     0, sizeof(FConvertPushConstant),
                                     &pushConstant);

        commandBuffer->Dispatch(groupCountX, groupCountY, groupCountZ);
    }

    void Release()
    {
        if (m_Device == nullptr)
        {
            return;
        }

        if (m_Pipeline.IsValid())
        {
            m_Device->DestroyComputePipeline(m_Pipeline);
        }

        for (SizeType i = 0; i < m_DescriptorSets.GetSize(); ++i)
        {
            m_Device->FreeDescriptorSet(m_DescriptorSets[i]);
        }

        m_DescriptorSets.Clear();

        if (m_PipelineLayout.IsValid())
        {
            m_Device->DestroyPipelineLayout(m_PipelineLayout);
        }

        if (m_DescSetLayout.IsValid())
        {
            m_Device->DestroyDescSetLayout(m_DescSetLayout);
        }

        if (m_Shader.IsValid())
        {
            m_Device->DestroyShader(m_Shader);
        }

        m_Device = nullptr;
    }

private:
    IRHIDevice*                     m_Device    = nullptr;
    bool                            m_HasSource = false;
    FRHIShaderHandle                m_Shader;
    FRHIDescSetLayoutHandle         m_DescSetLayout;
    TArray<FRHIDescriptorSetHandle> m_DescriptorSets;
    FRHIPipelineLayoutHandle        m_PipelineLayout;
    FRHIComputePipelineHandle       m_Pipeline;
};

/// 按线程组边长算分派数, 向上取整
///
/// 向上取整而非整除: 预滤波最后几级 mip 的面只有 4x4, 整除会得到 0 个
/// 线程组 —— 那一级根本不会被写, 留下的是未初始化的显存, 表现为最粗糙的
/// 反射变成一片噪声。着色器里的边界判断负责丢弃多出来的线程。
UInt32 ComputeGroupCount(UInt32 extent)
{
    return (extent + kThreadGroupSize - 1) / kThreadGroupSize;
}

} // namespace

// ============================================================================
// FCubeResource
// ============================================================================

void FCubeResource::Release(IRHIDevice* device)
{
    if (device == nullptr)
    {
        return;
    }

    if (SampleView.IsValid())
    {
        device->DestroyTextureView(SampleView);
    }

    for (SizeType i = 0; i < StorageViews.GetSize(); ++i)
    {
        device->DestroyTextureView(StorageViews[i]);
    }

    StorageViews.Clear();

    if (Texture.IsValid())
    {
        device->DestroyTexture(Texture);
    }

    FaceSize = 0;
}

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

    ReleaseTransientResources(m_Device);

    if (m_Sampler.IsValid())
    {
        m_Device->DestroySampler(m_Sampler);
    }

    if (m_BrdfSampler.IsValid())
    {
        m_Device->DestroySampler(m_BrdfSampler);
    }

    if (m_BrdfLutView.IsValid())
    {
        m_Device->DestroyTextureView(m_BrdfLutView);
    }

    if (m_BrdfLutTexture.IsValid())
    {
        m_Device->DestroyTexture(m_BrdfLutTexture);
    }

    m_Prefiltered.Release(m_Device);
    m_Irradiance.Release(m_Device);
    m_Environment.Release(m_Device);

    m_Device = nullptr;
}

void FEnvironmentMap::ReleaseTransientResources(IRHIDevice* device)
{
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
}

// ============================================================================
// BuildFromEquirect
// ============================================================================

ERHIResult FEnvironmentMap::BuildFromEquirect(FRenderContext*   context,
                                              const FImageData& equirect,
                                              UInt32            faceSize)
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

    const Float64 beginTime = FPlatformTime::Seconds();

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

    // 环境贴图带完整 mip 链: 辐照度卷积要从其中一级采样 (见 ConvolveIrradiance),
    // 镜面预滤波将来也要用到它
    const UInt32 environmentMipLevels =
        ComputeMipLevelCount(resolvedFaceSize, resolvedFaceSize);

    // ---- 建资源并逐趟转换 ----
    ERHIResult result = CreateSampler(m_Device, environmentMipLevels);

    if (IsRHISuccess(result))
    {
        result = CreateCubeResource(m_Device, resolvedFaceSize,
                                    environmentMipLevels,
                                    "EnvironmentMap.Cube", m_Environment);
    }

    if (IsRHISuccess(result))
    {
        // 辐照度贴图不需要 mip —— 它只按法线查一次, 没有缩小采样
        result = CreateCubeResource(m_Device, kIrradianceFaceSize, 1,
                                    "EnvironmentMap.Irradiance", m_Irradiance);
    }

    if (IsRHISuccess(result))
    {
        // 预滤波贴图的 mip 不是分辨率层级而是**粗糙度层级** —— 因此级数
        // 由粗糙度的分档决定, 而不是由面边长推出来
        result = CreateCubeResource(m_Device, kPrefilterFaceSize,
                                    kPrefilterMipLevels,
                                    "EnvironmentMap.Prefiltered",
                                    m_Prefiltered);
    }

    if (IsRHISuccess(result))
    {
        result = CreateBrdfLutResources(m_Device);
    }

    if (IsRHISuccess(result))
    {
        result = UploadEquirect(context, equirect);
    }

    if (IsRHISuccess(result))
    {
        result = ConvertEquirectToCube(context);
    }

    if (IsRHISuccess(result))
    {
        result = GenerateCubeMips(context, m_Environment);
    }

    if (IsRHISuccess(result))
    {
        result = ConvolveIrradiance(context);
    }

    if (IsRHISuccess(result))
    {
        result = PrefilterSpecular(context);
    }

    if (IsRHISuccess(result))
    {
        result = IntegrateBrdfLut(context);
    }

    if (!IsRHISuccess(result))
    {
        Release();
        return result;
    }

    // 转换资源到此为止 —— 源纹理是整套里最大的一块, 留着就是白占显存
    ReleaseTransientResources(m_Device);

    m_EquirectTexture = FRHITextureHandle();
    m_EquirectView    = FRHITextureViewHandle();
    m_StagingBuffer   = FRHIBufferHandle();
    m_EquirectSampler = FRHISamplerHandle();

    LIMX_LOG(LogEnvironment, Display,
             "[EnvironmentMap] 生成完成: 环境 {}x{}({}级) + 辐照度 {}x{} + "
             "预滤波 {}x{}({}级) + BRDF {}x{}, 合计 {} MB "
             "(源图 {}x{}), 耗时 {} ms",
             m_Environment.FaceSize, m_Environment.FaceSize,
             m_Environment.MipLevels,
             m_Irradiance.FaceSize, m_Irradiance.FaceSize,
             m_Prefiltered.FaceSize, m_Prefiltered.FaceSize,
             m_Prefiltered.MipLevels,
             kBrdfLutSize, kBrdfLutSize,
             GetMemoryBytes() / (1024 * 1024),
             equirect.Width, equirect.Height,
             static_cast<Int32>(
                 (FPlatformTime::Seconds() - beginTime) * 1000.0));

    return ERHIResult::Success;
}

// ============================================================================
// UploadEquirect
// ============================================================================

ERHIResult FEnvironmentMap::UploadEquirect(FRenderContext*   context,
                                           const FImageData& equirect)
{
    IRHIDevice* device = context->GetDevice();

    const SizeType byteSize = equirect.Pixels.GetSize();

    FRHIBufferDesc stagingDesc = {};
    stagingDesc.Size        = byteSize;
    stagingDesc.Usage       = EBufferUsage::TransferSrc;
    stagingDesc.MemoryUsage = EMemoryUsage::CpuToGpu;
    stagingDesc.DebugName   = "EnvironmentMap.Staging";

    ERHIResult result = device->CreateBuffer(stagingDesc, m_StagingBuffer);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    void* mapped = nullptr;
    result       = device->MapBuffer(m_StagingBuffer, &mapped);

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

    device->UnmapBuffer(m_StagingBuffer);

    // 立方体贴图是 RGBA16F, 超出半精度上限的值会被钳掉。
    //
    // 必须报出来: 真实 HDRI 的太阳盘常常冲到十万量级, 而它虽然只占几个
    // 像素, 携带的能量却可能是全图的大半 —— 实测这类图上区区七个像素被钳,
    // 朝阳方向的辐照度就少了三成。而这个损失在画面上只表现为"环境光有点暗",
    // 静默发生时几乎不可能被发现。
    const Float32* texels =
        reinterpret_cast<const Float32*>(equirect.Pixels.GetData());

    const SizeType componentCount = byteSize / sizeof(Float32);

    SizeType clampedCount = 0;
    Float32  maxValue     = 0.0f;

    for (SizeType i = 0; i < componentCount; i += 4)
    {
        for (SizeType channel = 0; channel < 3; ++channel)
        {
            const Float32 value = texels[i + channel];

            if (value > maxValue)
            {
                maxValue = value;
            }

            if (value > kHalfMaxValue)
            {
                ++clampedCount;
                break;
            }
        }
    }

    if (clampedCount > 0)
    {
        LIMX_LOG(LogEnvironment, Warning,
                 "[EnvironmentMap] {} 个像素超出半精度上限 ({}), 峰值 {} —— "
                 "这部分能量会被钳掉, 朝该方向的辐照度将偏低",
                 clampedCount, static_cast<Int32>(kHalfMaxValue),
                 static_cast<Int32>(maxValue));
    }

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

    result = device->CreateTexture(textureDesc, m_EquirectTexture);

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
        m_EquirectTexture,
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

    commandBuffer->CopyBufferToTexture(m_StagingBuffer, m_EquirectTexture,
                                       EImageLayout::TransferDst, region);

    // 目标是计算着色器而非片段着色器 —— 转换发生在 Dispatch 里
    commandBuffer->TransitionImageLayout(
        m_EquirectTexture,
        EImageLayout::TransferDst,
        EImageLayout::ShaderReadOnly,
        EPipelineStageFlags::Transfer,
        EPipelineStageFlags::ComputeShader,
        EAccessFlags::TransferWrite,
        EAccessFlags::ShaderRead);

    context->EndSingleTimeCommands(commandBuffer);

    FRHITextureViewDesc viewDesc = {};
    viewDesc.Texture         = m_EquirectTexture;
    viewDesc.ViewType        = ETextureType::Texture2D;
    viewDesc.Format          = EPixelFormat::RGBA32_SFLOAT;
    viewDesc.BaseMipLevel    = 0;
    viewDesc.MipLevelCount   = 1;
    viewDesc.BaseArrayLayer  = 0;
    viewDesc.ArrayLayerCount = 1;

    result = device->CreateTextureView(viewDesc, m_EquirectView);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    // 源图采样器: 线性 + u 方向重复
    //
    // u 必须 Repeat 而非 Clamp: 等距柱状图在 ±180° 处首尾相接,
    // 钳位会让接缝处的双线性插值取到边缘像素的复制, 表现为天空中
    // 一条竖直的亮暗不连续线。v 方向则必须钳位 —— 天顶之上没有内容。
    FRHISamplerDesc samplerDesc;
    samplerDesc.MinFilter           = EFilter::Linear;
    samplerDesc.MagFilter           = EFilter::Linear;
    samplerDesc.MipmapMode          = ESamplerMipmapMode::Nearest;
    samplerDesc.AddressModeU        = ESamplerAddressMode::Repeat;
    samplerDesc.AddressModeV        = ESamplerAddressMode::ClampToEdge;
    samplerDesc.AddressModeW        = ESamplerAddressMode::ClampToEdge;
    samplerDesc.IsAnisotropyEnabled = false;
    samplerDesc.MaxLod              = 1.0f;

    return device->CreateSampler(samplerDesc, m_EquirectSampler);
}

// ============================================================================
// 资源创建
// ============================================================================

ERHIResult FEnvironmentMap::CreateCubeResource(IRHIDevice*     device,
                                               UInt32          faceSize,
                                               UInt32          mipLevels,
                                               const AnsiChar* debugName,
                                               FCubeResource&  outResource)
{
    outResource.FaceSize  = faceSize;
    outResource.MipLevels = (mipLevels == 0) ? 1u : mipLevels;

    FRHITextureDesc cubeDesc = {};
    cubeDesc.Type          = ETextureType::TextureCube;
    cubeDesc.Format        = EPixelFormat::RGBA16_SFLOAT;
    cubeDesc.Extent.Width  = faceSize;
    cubeDesc.Extent.Height = faceSize;
    cubeDesc.Extent.Depth  = 1;
    cubeDesc.MipLevels     = outResource.MipLevels;
    cubeDesc.ArrayLayers   = kCubeFaceCount;
    // TransferSrc/Dst 有两个用途: 逐级 blit 生成 mip 链, 以及把贴图读回
    // 内存做数值校验 —— 卷积算错在画面上只表现为"环境光好像有点不对",
    // 唯一可靠的判据是取回数据逐面比对
    cubeDesc.Usage         = static_cast<ETextureUsage>(
        static_cast<UInt32>(ETextureUsage::Sampled) |
        static_cast<UInt32>(ETextureUsage::Storage) |
        static_cast<UInt32>(ETextureUsage::TransferSrc) |
        static_cast<UInt32>(ETextureUsage::TransferDst));
    cubeDesc.MemoryUsage   = EMemoryUsage::GpuOnly;
    cubeDesc.DebugName     = debugName;

    ERHIResult result = device->CreateTexture(cubeDesc, outResource.Texture);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    // 采样视图 —— 立方体类型, 六层一并覆盖
    FRHITextureViewDesc cubeViewDesc = {};
    cubeViewDesc.Texture         = outResource.Texture;
    cubeViewDesc.ViewType        = ETextureType::TextureCube;
    cubeViewDesc.Format          = EPixelFormat::RGBA16_SFLOAT;
    cubeViewDesc.BaseMipLevel    = 0;
    cubeViewDesc.MipLevelCount   = outResource.MipLevels;
    cubeViewDesc.BaseArrayLayer  = 0;
    cubeViewDesc.ArrayLayerCount = kCubeFaceCount;

    result = device->CreateTextureView(cubeViewDesc, outResource.SampleView);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    // 存储视图 —— 2D 数组类型, 每级 mip 一个。
    // 存储图像不能是多级视图: 着色器里的 image2DArray 没有 LOD 概念,
    // 一个视图只对应一级。
    outResource.StorageViews.Reserve(outResource.MipLevels);

    for (UInt32 level = 0; level < outResource.MipLevels; ++level)
    {
        FRHITextureViewDesc storageViewDesc = cubeViewDesc;
        storageViewDesc.ViewType            = ETextureType::Texture2DArray;
        storageViewDesc.BaseMipLevel        = level;
        storageViewDesc.MipLevelCount       = 1;

        FRHITextureViewHandle storageView;

        result = device->CreateTextureView(storageViewDesc, storageView);

        if (!IsRHISuccess(result))
        {
            return result;
        }

        outResource.StorageViews.Add(storageView);
    }

    return ERHIResult::Success;
}

ERHIResult FEnvironmentMap::CreateSampler(IRHIDevice* device, UInt32 mipLevels)
{
    // 线性 + 钳位。立方体贴图的跨面过滤由硬件负责, 寻址模式只在极少数
    // 实现上影响接缝, 钳位是最保守的选择。
    FRHISamplerDesc samplerDesc     = FRHISamplerDesc::LinearClamp();
    samplerDesc.IsAnisotropyEnabled = false;

    // MaxLod 必须覆盖整条 mip 链 —— 卷积要显式采样中间某一级, 而采样器
    // 会把请求的 LOD 钳到 [MinLod, MaxLod]。留在 1.0 的话请求 mip 3 会
    // 被悄悄钳成 mip 1, 结果依旧偏低而且完全看不出原因。
    samplerDesc.MaxLod = static_cast<Float32>(mipLevels);

    return device->CreateSampler(samplerDesc, m_Sampler);
}

// ============================================================================
// CreateBrdfLutResources — RG16F 查找表 + 钳位采样器
// ============================================================================

ERHIResult FEnvironmentMap::CreateBrdfLutResources(IRHIDevice* device)
{
    // RG16F 而非 RGBA16F: 表只有 A、B 两个通道, 多存两个恒为零的通道
    // 只是白占一半带宽。这张表每帧每片段都要采一次, 带宽是实打实的。
    constexpr EPixelFormat kBrdfFormat = EPixelFormat::RG16_SFLOAT;

    FRHITextureDesc lutDesc = {};
    lutDesc.Type          = ETextureType::Texture2D;
    lutDesc.Format        = kBrdfFormat;
    lutDesc.Extent.Width  = kBrdfLutSize;
    lutDesc.Extent.Height = kBrdfLutSize;
    lutDesc.Extent.Depth  = 1;
    lutDesc.MipLevels     = 1;
    lutDesc.ArrayLayers   = 1;
    lutDesc.Usage         = static_cast<ETextureUsage>(
        static_cast<UInt32>(ETextureUsage::Sampled) |
        static_cast<UInt32>(ETextureUsage::Storage) |
        static_cast<UInt32>(ETextureUsage::TransferSrc));
    lutDesc.MemoryUsage   = EMemoryUsage::GpuOnly;
    lutDesc.DebugName     = "EnvironmentMap.BrdfLut";

    ERHIResult result = device->CreateTexture(lutDesc, m_BrdfLutTexture);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    FRHITextureViewDesc viewDesc = {};
    viewDesc.Texture         = m_BrdfLutTexture;
    viewDesc.ViewType        = ETextureType::Texture2D;
    viewDesc.Format          = kBrdfFormat;
    viewDesc.BaseMipLevel    = 0;
    viewDesc.MipLevelCount   = 1;
    viewDesc.BaseArrayLayer  = 0;
    viewDesc.ArrayLayerCount = 1;

    result = device->CreateTextureView(viewDesc, m_BrdfLutView);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    // 必须 ClampToEdge。Repeat 会让 n·v→1 的一列取到 n·v→0 的值 ——
    // 正面看物体时反射强度突然跳变, 而那正是最容易被注意到的角度。
    FRHISamplerDesc samplerDesc     = FRHISamplerDesc::LinearClamp();
    samplerDesc.IsAnisotropyEnabled = false;
    samplerDesc.MipmapMode          = ESamplerMipmapMode::Nearest;
    samplerDesc.MaxLod              = 1.0f;

    return device->CreateSampler(samplerDesc, m_BrdfSampler);
}

// ============================================================================
// GenerateCubeMips — 逐级 blit
// ============================================================================

ERHIResult FEnvironmentMap::GenerateCubeMips(FRenderContext* context,
                                             FCubeResource&  resource)
{
    if (resource.MipLevels <= 1)
    {
        return ERHIResult::Success;
    }

    IRHICommandBuffer* commandBuffer = context->BeginSingleTimeCommands();

    if (commandBuffer == nullptr)
    {
        return ERHIResult::ErrorUnknown;
    }

    // 进来时整张图是 ShaderReadOnly (上一趟 Dispatch 留下的)。
    // mip 0 转为 blit 源, 其余各级转为 blit 目标。
    commandBuffer->TransitionImageLayout(
        resource.Texture,
        EImageLayout::ShaderReadOnly,
        EImageLayout::TransferSrc,
        EPipelineStageFlags::ComputeShader,
        EPipelineStageFlags::Transfer,
        EAccessFlags::ShaderRead,
        EAccessFlags::TransferRead,
        0, 1, 0, kCubeFaceCount);

    commandBuffer->TransitionImageLayout(
        resource.Texture,
        EImageLayout::Undefined,
        EImageLayout::TransferDst,
        EPipelineStageFlags::TopOfPipe,
        EPipelineStageFlags::Transfer,
        EAccessFlags::None,
        EAccessFlags::TransferWrite,
        1, resource.MipLevels - 1, 0, kCubeFaceCount);

    Int32 mipSize = static_cast<Int32>(resource.FaceSize);

    for (UInt32 level = 1; level < resource.MipLevels; ++level)
    {
        const Int32 nextSize = (mipSize > 1) ? (mipSize / 2) : 1;

        // 六个面一次 blit —— 它们是同一图像的六个数组层
        FRHITextureBlitRegion blit = {};
        blit.SrcMipLevel   = level - 1;
        blit.SrcBaseLayer  = 0;
        blit.SrcLayerCount = kCubeFaceCount;
        blit.SrcOffsetMin  = { 0, 0, 0 };
        blit.SrcOffsetMax  = { mipSize, mipSize, 1 };
        blit.DstMipLevel   = level;
        blit.DstBaseLayer  = 0;
        blit.DstLayerCount = kCubeFaceCount;
        blit.DstOffsetMin  = { 0, 0, 0 };
        blit.DstOffsetMax  = { nextSize, nextSize, 1 };

        commandBuffer->BlitTexture(resource.Texture, EImageLayout::TransferSrc,
                                   resource.Texture, EImageLayout::TransferDst,
                                   blit, EFilter::Linear);

        // 本级写完后转为下一级的源。逐级转换而非整体转换, 是因为同一时刻
        // 不同 mip 处于不同布局。
        commandBuffer->TransitionImageLayout(
            resource.Texture,
            EImageLayout::TransferDst,
            EImageLayout::TransferSrc,
            EPipelineStageFlags::Transfer,
            EPipelineStageFlags::Transfer,
            EAccessFlags::TransferWrite,
            EAccessFlags::TransferRead,
            level, 1, 0, kCubeFaceCount);

        mipSize = nextSize;
    }

    // 全部各级此刻都是 TransferSrc, 一并转回着色器只读
    commandBuffer->TransitionImageLayout(
        resource.Texture,
        EImageLayout::TransferSrc,
        EImageLayout::ShaderReadOnly,
        EPipelineStageFlags::Transfer,
        EPipelineStageFlags::ComputeShader |
            EPipelineStageFlags::FragmentShader,
        EAccessFlags::TransferRead,
        EAccessFlags::ShaderRead,
        0, resource.MipLevels, 0, kCubeFaceCount);

    context->EndSingleTimeCommands(commandBuffer);

    return ERHIResult::Success;
}

// ============================================================================
// 两趟转换
// ============================================================================

ERHIResult FEnvironmentMap::ConvertEquirectToCube(FRenderContext* context)
{
    FComputePass pass;

    ERHIResult result = pass.Create(
        m_Device, "Builtin/equirect_to_cube.comp",
        "EnvironmentMap.EquirectToCube",
        m_EquirectView, m_EquirectSampler,
        m_Environment.StorageViews.GetData(), 1);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    IRHICommandBuffer* commandBuffer = context->BeginSingleTimeCommands();

    if (commandBuffer == nullptr)
    {
        return ERHIResult::ErrorUnknown;
    }

    // 只写 mip 0 —— 其余各级随后由 GenerateCubeMips 逐级 blit 出来
    commandBuffer->TransitionImageLayout(
        m_Environment.Texture,
        EImageLayout::Undefined,
        EImageLayout::General,
        EPipelineStageFlags::TopOfPipe,
        EPipelineStageFlags::ComputeShader,
        EAccessFlags::None,
        EAccessFlags::ShaderWrite,
        0, 1, 0, kCubeFaceCount);

    FConvertPushConstant pushConstant;
    pushConstant.FaceSize = m_Environment.FaceSize;

    const UInt32 groupCount = ComputeGroupCount(m_Environment.FaceSize);

    pass.Dispatch(commandBuffer, 0, groupCount, groupCount, kCubeFaceCount,
                  pushConstant);

    commandBuffer->TransitionImageLayout(
        m_Environment.Texture,
        EImageLayout::General,
        EImageLayout::ShaderReadOnly,
        EPipelineStageFlags::ComputeShader,
        EPipelineStageFlags::ComputeShader |
            EPipelineStageFlags::FragmentShader,
        EAccessFlags::ShaderWrite,
        EAccessFlags::ShaderRead,
        0, 1, 0, kCubeFaceCount);

    context->EndSingleTimeCommands(commandBuffer);

    return ERHIResult::Success;
}

ERHIResult FEnvironmentMap::ConvolveIrradiance(FRenderContext* context)
{
    FComputePass pass;

    // 源是刚生成的环境贴图 —— 它此刻已是 ShaderReadOnly, 且上一次提交
    // 已由 EndSingleTimeCommands 等待完成, 因此不需要额外的跨提交同步
    ERHIResult result = pass.Create(
        m_Device, "Builtin/irradiance_convolve.comp",
        "EnvironmentMap.IrradianceConvolve",
        m_Environment.SampleView, m_Sampler,
        m_Irradiance.StorageViews.GetData(), 1);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    IRHICommandBuffer* commandBuffer = context->BeginSingleTimeCommands();

    if (commandBuffer == nullptr)
    {
        return ERHIResult::ErrorUnknown;
    }

    FConvertPushConstant pushConstant;
    pushConstant.FaceSize    = m_Irradiance.FaceSize;
    pushConstant.SampleDelta = kIrradianceSampleDelta;

    // 选一级 mip, 使其单个纹素的张角约等于求积步长。
    //
    // 一个面覆盖 90 度, 边长 S 的面每纹素张角约 (π/2)/S。令它等于步长,
    // 得目标边长 S* = (π/2)/delta。
    //
    // 取**离 S* 最近**的一级, 而不是第一个不超过 S* 的: mip 边长按 2 的
    // 幂跳变, "第一个不超过"会在 S* 略小于某一级时一路跳到它的一半, 平白
    // 多模糊一倍。相邻两级的几何中点是 S*·√2, 以它为界即可取到最近的一级。
    //
    // 过高的 mip 会在样本之间留下空隙 (太阳被整个跳过), 过低的 mip 则把
    // 本该有方向性的天空压平 —— 后者的表现是"环境光偏灰、朝向差异变小"。
    const Float32 targetFaceSize = FMath::kHalfPi / kIrradianceSampleDelta;

    // √2 —— 相邻两级 mip 边长的几何中点
    constexpr Float32 kMipMidpointRatio = 1.41421356f;

    const Float32 mipThreshold = targetFaceSize * kMipMidpointRatio;

    UInt32 sourceMip = 0;

    while (sourceMip + 1 < m_Environment.MipLevels &&
           static_cast<Float32>(m_Environment.FaceSize >> sourceMip) >
               mipThreshold)
    {
        ++sourceMip;
    }

    pushConstant.SourceMipLevel = static_cast<Float32>(sourceMip);

    LIMX_LOG(LogEnvironment, Log,
             "[EnvironmentMap] 辐照度卷积从 mip {} 采样 (边长 {}, 目标 {})",
             sourceMip, m_Environment.FaceSize >> sourceMip,
             static_cast<Int32>(targetFaceSize));

    commandBuffer->TransitionImageLayout(
        m_Irradiance.Texture,
        EImageLayout::Undefined,
        EImageLayout::General,
        EPipelineStageFlags::TopOfPipe,
        EPipelineStageFlags::ComputeShader,
        EAccessFlags::None,
        EAccessFlags::ShaderWrite,
        0, 1, 0, kCubeFaceCount);

    const UInt32 groupCount = ComputeGroupCount(m_Irradiance.FaceSize);

    pass.Dispatch(commandBuffer, 0, groupCount, groupCount, kCubeFaceCount,
                  pushConstant);

    commandBuffer->TransitionImageLayout(
        m_Irradiance.Texture,
        EImageLayout::General,
        EImageLayout::ShaderReadOnly,
        EPipelineStageFlags::ComputeShader,
        EPipelineStageFlags::FragmentShader,
        EAccessFlags::ShaderWrite,
        EAccessFlags::ShaderRead,
        0, 1, 0, kCubeFaceCount);

    context->EndSingleTimeCommands(commandBuffer);

    return ERHIResult::Success;
}

// ============================================================================
// PrefilterSpecular — 按粗糙度分级的镜面反射贴图
// ============================================================================

ERHIResult FEnvironmentMap::PrefilterSpecular(FRenderContext* context)
{
    FComputePass pass;

    // 每级 mip 一个描述符集 —— 管线、布局、着色器全部共用
    ERHIResult result = pass.Create(
        m_Device, "Builtin/prefilter_env.comp",
        "EnvironmentMap.PrefilterSpecular",
        m_Environment.SampleView, m_Sampler,
        m_Prefiltered.StorageViews.GetData(),
        static_cast<UInt32>(m_Prefiltered.StorageViews.GetSize()));

    if (!IsRHISuccess(result))
    {
        return result;
    }

    IRHICommandBuffer* commandBuffer = context->BeginSingleTimeCommands();

    if (commandBuffer == nullptr)
    {
        return ERHIResult::ErrorUnknown;
    }

    // 整条 mip 链一次转入 General —— 各级之间没有依赖, 可以并行写
    commandBuffer->TransitionImageLayout(
        m_Prefiltered.Texture,
        EImageLayout::Undefined,
        EImageLayout::General,
        EPipelineStageFlags::TopOfPipe,
        EPipelineStageFlags::ComputeShader,
        EAccessFlags::None,
        EAccessFlags::ShaderWrite,
        0, m_Prefiltered.MipLevels, 0, kCubeFaceCount);

    const UInt32 lastMip = (m_Prefiltered.MipLevels > 1)
                         ? (m_Prefiltered.MipLevels - 1)
                         : 1;

    for (UInt32 level = 0; level < m_Prefiltered.MipLevels; ++level)
    {
        const UInt32 mipFaceSize = (m_Prefiltered.FaceSize >> level) > 0
                                 ? (m_Prefiltered.FaceSize >> level)
                                 : 1u;

        FConvertPushConstant pushConstant;
        pushConstant.FaceSize = mipFaceSize;

        // 粗糙度在各级之间线性铺开: mip 0 = 完全光滑, 末级 = 完全粗糙。
        // 着色器采样时用 roughness * maxLod 反查, 两边必须是同一个映射。
        pushConstant.Roughness =
            static_cast<Float32>(level) / static_cast<Float32>(lastMip);

        // 源贴图 mip 0 的边长 —— 着色器据此算单个源纹素的立体角, 进而
        // 为每个样本挑一级合适的 mip
        pushConstant.SourceFaceSize = m_Environment.FaceSize;
        pushConstant.SampleCount    = kPrefilterSampleCount;

        const UInt32 groupCount = ComputeGroupCount(mipFaceSize);

        pass.Dispatch(commandBuffer, level, groupCount, groupCount,
                      kCubeFaceCount, pushConstant);
    }

    commandBuffer->TransitionImageLayout(
        m_Prefiltered.Texture,
        EImageLayout::General,
        EImageLayout::ShaderReadOnly,
        EPipelineStageFlags::ComputeShader,
        EPipelineStageFlags::FragmentShader,
        EAccessFlags::ShaderWrite,
        EAccessFlags::ShaderRead,
        0, m_Prefiltered.MipLevels, 0, kCubeFaceCount);

    context->EndSingleTimeCommands(commandBuffer);

    return ERHIResult::Success;
}

// ============================================================================
// IntegrateBrdfLut — 与场景无关的环境 BRDF 表
// ============================================================================

ERHIResult FEnvironmentMap::IntegrateBrdfLut(FRenderContext* context)
{
    FComputePass pass;

    // 无输入贴图 —— 这张表只依赖 (n·v, 粗糙度), 与环境无关
    ERHIResult result = pass.Create(
        m_Device, "Builtin/brdf_lut.comp",
        "EnvironmentMap.BrdfLut",
        FRHITextureViewHandle(), FRHISamplerHandle(),
        &m_BrdfLutView, 1);

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
        m_BrdfLutTexture,
        EImageLayout::Undefined,
        EImageLayout::General,
        EPipelineStageFlags::TopOfPipe,
        EPipelineStageFlags::ComputeShader,
        EAccessFlags::None,
        EAccessFlags::ShaderWrite);

    FConvertPushConstant pushConstant;
    pushConstant.FaceSize    = kBrdfLutSize;
    pushConstant.SampleCount = kBrdfLutSampleCount;

    const UInt32 groupCount = ComputeGroupCount(kBrdfLutSize);

    pass.Dispatch(commandBuffer, 0, groupCount, groupCount, 1, pushConstant);

    commandBuffer->TransitionImageLayout(
        m_BrdfLutTexture,
        EImageLayout::General,
        EImageLayout::ShaderReadOnly,
        EPipelineStageFlags::ComputeShader,
        EPipelineStageFlags::FragmentShader,
        EAccessFlags::ShaderWrite,
        EAccessFlags::ShaderRead);

    context->EndSingleTimeCommands(commandBuffer);

    return ERHIResult::Success;
}

// ============================================================================
// ReadbackIrradiance
// ============================================================================

bool FEnvironmentMap::ReadbackIrradiance(FRenderContext*  context,
                                        TArray<Float32>& outPixels,
                                        UInt32&          outFaceSize) const
{
    return ReadbackCubeMip(context, m_Irradiance, 0, outPixels, outFaceSize);
}

bool FEnvironmentMap::ReadbackPrefiltered(FRenderContext*  context,
                                          UInt32           mipLevel,
                                          TArray<Float32>& outPixels,
                                          UInt32&          outFaceSize) const
{
    return ReadbackCubeMip(context, m_Prefiltered, mipLevel,
                           outPixels, outFaceSize);
}

// ============================================================================
// ReadbackCubeMip — 两个公开读回接口的共同实现
// ============================================================================

bool FEnvironmentMap::ReadbackCubeMip(FRenderContext*      context,
                                      const FCubeResource& resource,
                                      UInt32               mipLevel,
                                      TArray<Float32>&     outPixels,
                                      UInt32&              outFaceSize) const
{
    outPixels.Clear();
    outFaceSize = 0;

    if (context == nullptr || m_Device == nullptr || !resource.IsValid())
    {
        return false;
    }

    if (mipLevel >= resource.MipLevels)
    {
        return false;
    }

    IRHIDevice* device = context->GetDevice();

    const UInt32 faceSize = (resource.FaceSize >> mipLevel) > 0
                          ? (resource.FaceSize >> mipLevel)
                          : 1u;

    const SizeType texelCount =
        static_cast<SizeType>(faceSize) * faceSize * kCubeFaceCount;

    // RGBA16F = 8 字节/纹素
    const SizeType byteSize = texelCount * 8;

    FRHIBufferDesc readbackDesc = {};
    readbackDesc.Size        = byteSize;
    readbackDesc.Usage       = EBufferUsage::TransferDst;
    readbackDesc.MemoryUsage = EMemoryUsage::GpuToCpu;
    readbackDesc.DebugName   = "EnvironmentMap.CubeReadback";

    FRHIBufferHandle readback;

    if (!IsRHISuccess(device->CreateBuffer(readbackDesc, readback)))
    {
        return false;
    }

    IRHICommandBuffer* commandBuffer = context->BeginSingleTimeCommands();

    if (commandBuffer == nullptr)
    {
        device->DestroyBuffer(readback);
        return false;
    }

    commandBuffer->TransitionImageLayout(
        resource.Texture,
        EImageLayout::ShaderReadOnly,
        EImageLayout::TransferSrc,
        EPipelineStageFlags::FragmentShader,
        EPipelineStageFlags::Transfer,
        EAccessFlags::ShaderRead,
        EAccessFlags::TransferRead,
        mipLevel, 1, 0, kCubeFaceCount);

    // 六个面一次拷完 —— 它们在内存里就是连续的数组层
    FRHIBufferTextureCopyRegion region = {};
    region.BufferOffset      = 0;
    region.BufferRowLength   = 0;
    region.BufferImageHeight = 0;
    region.MipLevel          = mipLevel;
    region.BaseLayer         = 0;
    region.LayerCount        = kCubeFaceCount;
    region.TextureOffset     = { 0, 0, 0 };
    region.TextureExtent     = { faceSize, faceSize, 1 };

    commandBuffer->CopyTextureToBuffer(resource.Texture,
                                       EImageLayout::TransferSrc,
                                       readback, region);

    commandBuffer->TransitionImageLayout(
        resource.Texture,
        EImageLayout::TransferSrc,
        EImageLayout::ShaderReadOnly,
        EPipelineStageFlags::Transfer,
        EPipelineStageFlags::FragmentShader,
        EAccessFlags::TransferRead,
        EAccessFlags::ShaderRead,
        mipLevel, 1, 0, kCubeFaceCount);

    context->EndSingleTimeCommands(commandBuffer);

    void* mapped = nullptr;

    if (!IsRHISuccess(device->MapBuffer(readback, &mapped)) ||
        mapped == nullptr)
    {
        device->DestroyBuffer(readback);
        return false;
    }

    const UInt16* source = static_cast<const UInt16*>(mapped);

    outPixels.SetSize(texelCount * 4);

    for (SizeType i = 0; i < texelCount * 4; ++i)
    {
        outPixels[i] = HalfToFloat(source[i]);
    }

    device->UnmapBuffer(readback);
    device->DestroyBuffer(readback);

    outFaceSize = faceSize;

    return true;
}

// ============================================================================
// ReadbackBrdfLut
// ============================================================================

bool FEnvironmentMap::ReadbackBrdfLut(FRenderContext*  context,
                                      TArray<Float32>& outPixels,
                                      UInt32&          outSize) const
{
    outPixels.Clear();
    outSize = 0;

    if (context == nullptr || m_Device == nullptr || !m_BrdfLutView.IsValid())
    {
        return false;
    }

    IRHIDevice* device = context->GetDevice();

    const SizeType texelCount =
        static_cast<SizeType>(kBrdfLutSize) * kBrdfLutSize;

    // RG16F = 4 字节/纹素
    const SizeType byteSize = texelCount * 4;

    FRHIBufferDesc readbackDesc = {};
    readbackDesc.Size        = byteSize;
    readbackDesc.Usage       = EBufferUsage::TransferDst;
    readbackDesc.MemoryUsage = EMemoryUsage::GpuToCpu;
    readbackDesc.DebugName   = "EnvironmentMap.BrdfReadback";

    FRHIBufferHandle readback;

    if (!IsRHISuccess(device->CreateBuffer(readbackDesc, readback)))
    {
        return false;
    }

    IRHICommandBuffer* commandBuffer = context->BeginSingleTimeCommands();

    if (commandBuffer == nullptr)
    {
        device->DestroyBuffer(readback);
        return false;
    }

    commandBuffer->TransitionImageLayout(
        m_BrdfLutTexture,
        EImageLayout::ShaderReadOnly,
        EImageLayout::TransferSrc,
        EPipelineStageFlags::FragmentShader,
        EPipelineStageFlags::Transfer,
        EAccessFlags::ShaderRead,
        EAccessFlags::TransferRead);

    FRHIBufferTextureCopyRegion region = {};
    region.BufferOffset      = 0;
    region.BufferRowLength   = 0;
    region.BufferImageHeight = 0;
    region.MipLevel          = 0;
    region.BaseLayer         = 0;
    region.LayerCount        = 1;
    region.TextureOffset     = { 0, 0, 0 };
    region.TextureExtent     = { kBrdfLutSize, kBrdfLutSize, 1 };

    commandBuffer->CopyTextureToBuffer(m_BrdfLutTexture,
                                       EImageLayout::TransferSrc,
                                       readback, region);

    commandBuffer->TransitionImageLayout(
        m_BrdfLutTexture,
        EImageLayout::TransferSrc,
        EImageLayout::ShaderReadOnly,
        EPipelineStageFlags::Transfer,
        EPipelineStageFlags::FragmentShader,
        EAccessFlags::TransferRead,
        EAccessFlags::ShaderRead);

    context->EndSingleTimeCommands(commandBuffer);

    void* mapped = nullptr;

    if (!IsRHISuccess(device->MapBuffer(readback, &mapped)) ||
        mapped == nullptr)
    {
        device->DestroyBuffer(readback);
        return false;
    }

    const UInt16* source = static_cast<const UInt16*>(mapped);

    outPixels.SetSize(texelCount * 2);

    for (SizeType i = 0; i < texelCount * 2; ++i)
    {
        outPixels[i] = HalfToFloat(source[i]);
    }

    device->UnmapBuffer(readback);
    device->DestroyBuffer(readback);

    outSize = kBrdfLutSize;

    return true;
}

} // namespace Limx
