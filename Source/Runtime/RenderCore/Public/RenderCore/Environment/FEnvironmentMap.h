/*******************************************************************************
 * 文件: FEnvironmentMap.h
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   环境立方体贴图 — 等距柱状 HDR 图 → 环境立方体贴图 → 漫反射辐照度贴图
 *
 * 设计哲学:
 *   转换在 GPU 上做而非 CPU — 一张 1024 边长的立方体贴图有六百万个纹素,
 *   每个都要一次 atan/acos 与一次双线性采样; 而辐照度卷积每个纹素还要积分
 *   上万个方向。CPU 路线在第二步就走不下去了。
 *
 *   存 RGBA16F 而非 RGBA32F — 半精度浮点在环境光照的量级上足够: 它有
 *   10 位尾数与 ±65504 的范围, 而 RGBE 源本身每通道只有 8 位尾数。
 *   代价却是一半的显存与一半的带宽, 后者在卷积时是实打实的瓶颈。
 *
 *   辐照度贴图只有 32x32 — 它是原图与余弦瓣的卷积, 而余弦瓣半角 90°,
 *   卷积结果本身就极其平滑。存大了只是浪费显存, 换不来任何可见差别。
 *
 * 技术特性:
 *   - 立方体贴图以 2D 数组视图写入 (逐纹素定位), 以 Cube 视图采样
 *   - 面边长可配置, 默认由源图宽度推出
 *   - 资源自持有: 析构即释放, 无需外部记账
 *
 * 依赖关系:
 *   内部: RenderCore/Renderer/FRenderContext.h, AssetPipeline/FImageTypes.h
 *
 * 注意事项:
 *   转换使用一次性命令缓冲区并等待完成 —— 只在关卡加载时调用
 *
 ******************************************************************************/

#pragma once

#include "RenderCore/RenderCoreMinimal.h"
#include "AssetPipeline/FImageTypes.h"

namespace Limx
{

class FRenderContext;

// ============================================================================
// FCubeResource — 一张立方体贴图及其两个视图
// ============================================================================

/// 同一张图像的两个视图: Cube 用于采样, 2DArray 用于计算着色器逐纹素写入
///
/// 必须是两个视图而非一个: GLSL 的 imageCube 无法按 (x, y, face) 定位,
/// 而 samplerCube 才有硬件的跨面过滤。
struct FCubeResource
{
    FRHITextureHandle     Texture;
    FRHITextureViewHandle SampleView;

    /// 存储视图 —— 只覆盖 mip 0, 计算着色器只写最高一级
    FRHITextureViewHandle StorageView;

    UInt32                FaceSize  = 0;
    UInt32                MipLevels = 1;

    LIMX_NODISCARD bool IsValid() const { return SampleView.IsValid(); }

    /// 六个面的字节数 (RGBA16F, 含完整 mip 链)
    ///
    /// mip 链的总量是基础级的 4/3 (等比级数 1 + 1/4 + 1/16 + ...),
    /// 这里按上界算, 与实际分配的差别在 1% 以内。
    LIMX_NODISCARD SizeType GetMemoryBytes() const
    {
        const SizeType base = static_cast<SizeType>(FaceSize) * FaceSize * 6 * 8;

        return (MipLevels > 1) ? (base * 4 / 3) : base;
    }

    void Release(IRHIDevice* device);
};

// ============================================================================
// FEnvironmentMap — 环境立方体贴图
// ============================================================================

class LIMX_RENDERCORE_API FEnvironmentMap
{
public:
    /// 辐照度贴图的面边长
    ///
    /// 32 已远超余弦卷积的有效频率。改大不会更准, 只会更慢更占显存。
    static constexpr UInt32 kIrradianceFaceSize = 32;

    /// 辐照度积分的角步长 (弧度)
    ///
    /// 0.025 对应每纹素约 1.6 万个样本。再密下去误差已被 RGBA16F 的
    /// 量化淹没, 再稀则平坦区域开始出现可见的条带。
    static constexpr Float32 kIrradianceSampleDelta = 0.025f;

    FEnvironmentMap() = default;
    ~FEnvironmentMap();

    LIMX_NON_COPYABLE(FEnvironmentMap);
    LIMX_NON_MOVABLE(FEnvironmentMap);

    /// 由等距柱状浮点图构建环境贴图与辐照度贴图
    ///
    /// @param context   渲染上下文 (提供设备与一次性命令缓冲区)
    /// @param equirect  源图 —— 必须是浮点格式, 宽高比应为 2:1
    /// @param faceSize  环境贴图的面边长, 0 表示按源图宽度的 1/4 自动选取
    ///
    /// 自动面边长取源图宽度的 1/4: 等距柱状图的赤道一圈占满整个宽度,
    /// 而立方体贴图的赤道一圈跨四个面, 因此 1/4 恰好保持赤道处的角分辨率
    /// 不变 —— 再大只是插值放大, 再小则丢失细节。
    LIMX_NODISCARD ERHIResult BuildFromEquirect(FRenderContext* context,
                                                const FImageData& equirect,
                                                UInt32 faceSize = 0);

    /// 释放全部 GPU 资源
    void Release();

    LIMX_NODISCARD bool IsValid() const { return m_Environment.IsValid(); }

    /// 环境贴图的立方体采样视图 —— 供天空盒使用
    LIMX_NODISCARD FRHITextureViewHandle GetCubeView() const
    {
        return m_Environment.SampleView;
    }

    /// 辐照度贴图的立方体采样视图 —— 供 PBR 的漫反射环境项使用
    LIMX_NODISCARD FRHITextureViewHandle GetIrradianceView() const
    {
        return m_Irradiance.SampleView;
    }

    LIMX_NODISCARD FRHISamplerHandle GetSampler() const { return m_Sampler; }

    LIMX_NODISCARD UInt32 GetFaceSize() const
    {
        return m_Environment.FaceSize;
    }

    /// 全部立方体贴图占用的显存字节数
    LIMX_NODISCARD SizeType GetMemoryBytes() const
    {
        return m_Environment.GetMemoryBytes() + m_Irradiance.GetMemoryBytes();
    }

    /// 把辐照度贴图读回内存 (RGBA32F, 六个面依次排列)
    ///
    /// 卷积是否算对, 只看画面是判断不了的 —— 辐照度本身极其平滑, 系数差
    /// 一倍、漏掉 sinθ 权重、面朝向搞反, 这几种错误在渲染结果里都只表现为
    /// "环境光好像有点不对", 而它们的数值特征截然不同。把数据取回来逐面
    /// 比对才是唯一可靠的判据。
    ///
    /// @param context   渲染上下文
    /// @param outPixels 输出 —— 尺寸为 6 * faceSize * faceSize * 4 个浮点
    /// @param outFaceSize 输出 —— 面边长
    LIMX_NODISCARD bool ReadbackIrradiance(FRenderContext*  context,
                                           TArray<Float32>& outPixels,
                                           UInt32&          outFaceSize) const;

private:
    /// 把源图上传为一张可采样的 2D 纹理
    ERHIResult UploadEquirect(FRenderContext* context,
                              const FImageData& equirect);

    /// 创建一张立方体贴图及其采样/存储两个视图
    ERHIResult CreateCubeResource(IRHIDevice*     device,
                                  UInt32          faceSize,
                                  UInt32          mipLevels,
                                  const AnsiChar* debugName,
                                  FCubeResource&  outResource);

    /// 逐级 blit 生成立方体贴图的 mip 链
    ///
    /// 六个面各自独立降采样, 不跨面混合。低级 mip 的面边界因此会有极细的
    /// 接缝, 但辐照度卷积本身就是大范围平均, 这点误差远在噪声之下。
    ERHIResult GenerateCubeMips(FRenderContext* context,
                                FCubeResource&  resource);

    /// 创建共用的立方体采样器 —— MaxLod 必须覆盖整条 mip 链
    ERHIResult CreateSampler(IRHIDevice* device, UInt32 mipLevels);

    /// 等距柱状 → 立方体贴图
    ERHIResult ConvertEquirectToCube(FRenderContext* context);

    /// 立方体贴图 → 辐照度贴图
    ERHIResult ConvolveIrradiance(FRenderContext* context);

    /// 释放仅转换期间需要的资源
    void ReleaseTransientResources(IRHIDevice* device);

    // ====================================================================
    // 成员
    // ====================================================================

    IRHIDevice*   m_Device = nullptr;

    FCubeResource m_Environment;
    FCubeResource m_Irradiance;

    /// 两张立方体贴图共用 —— 采样参数完全相同, 没有分开的理由
    FRHISamplerHandle m_Sampler;

    // ---- 仅转换期间存在 ----
    FRHITextureHandle     m_EquirectTexture;
    FRHITextureViewHandle m_EquirectView;
    FRHIBufferHandle      m_StagingBuffer;
    FRHISamplerHandle     m_EquirectSampler;
};

} // namespace Limx
