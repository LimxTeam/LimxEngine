/*******************************************************************************
 * 文件: FEnvironmentMap.h
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   环境光照资源 — 等距柱状 HDR 图 → 环境立方体贴图 → 辐照度贴图
 *                                                  → 镜面预滤波贴图
 *                                                  → BRDF 查找表
 *
 * 设计哲学:
 *   转换在 GPU 上做而非 CPU — 一张 1024 边长的立方体贴图有六百万个纹素,
 *   每个都要一次 atan/acos 与一次双线性采样; 辐照度卷积每个纹素还要积分
 *   上万个方向, 镜面预滤波每级 mip 每纹素上千个重要性样本。CPU 路线在
 *   第二步就走不下去了。
 *
 *   存 RGBA16F 而非 RGBA32F — 半精度浮点在环境光照的量级上足够: 它有
 *   10 位尾数与 ±65504 的范围, 而 RGBE 源本身每通道只有 8 位尾数。
 *   代价却是一半的显存与一半的带宽, 后者在卷积时是实打实的瓶颈。
 *   (源图超出该范围时会告警 —— 太阳盘常常冲到十万量级。)
 *
 *   辐照度贴图只有 32x32 — 它是原图与余弦瓣的卷积, 而余弦瓣半角 90°,
 *   卷积结果本身就极其平滑。存大了只是浪费显存, 换不来任何可见差别。
 *
 *   BRDF 查找表与场景无关 — 它只依赖 (n·v, 粗糙度) 两个标量, 对任何
 *   HDRI、任何 F0 都成立。这里仍随环境贴图一起建, 是因为它的生存期与
 *   其它 IBL 资源一致, 单独抽一层管理换不来什么。
 *
 * 技术特性:
 *   - 立方体贴图以 2D 数组视图写入 (逐纹素定位), 以 Cube 视图采样
 *   - 预滤波贴图每级 mip 一个存储视图 —— 计算着色器一次只写一级
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
// FCubeResource — 一张立方体贴图及其视图
// ============================================================================

/// 同一张图像的两类视图: Cube 用于采样, 2DArray 用于计算着色器逐纹素写入
///
/// 必须分开而非一个: GLSL 的 imageCube 无法按 (x, y, face) 定位,
/// 而 samplerCube 才有硬件的跨面过滤。
struct FCubeResource
{
    FRHITextureHandle     Texture;

    /// 采样视图 —— 覆盖全部 mip
    FRHITextureViewHandle SampleView;

    /// 存储视图 —— 每级 mip 一个
    ///
    /// 存储图像没有 LOD 概念, 一个视图只能对应一级。预滤波要逐级写入,
    /// 因此需要一整组; 只写 mip 0 的贴图这里就只有一个元素。
    TArray<FRHITextureViewHandle> StorageViews;

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
// FEnvironmentMap — 环境光照资源
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

    /// 镜面预滤波贴图 mip 0 的面边长
    ///
    /// 128 而非与环境贴图同尺寸: mip 0 对应 roughness=0, 也就是完全镜面,
    /// 而完全镜面的反射本可以直接采环境贴图。这张表的价值在中高粗糙度段,
    /// 那里的内容早已模糊, 128 绰绰有余。
    static constexpr UInt32 kPrefilterFaceSize = 128;

    /// 预滤波的 mip 级数 —— 每级对应一档粗糙度
    ///
    /// 6 级把 [0,1] 的粗糙度切成 5 段, 段间靠三线性插值过渡。再多不会更准
    /// (最后几级已经只有几个纹素), 再少则中段的过渡能看出跳变。
    static constexpr UInt32 kPrefilterMipLevels = 6;

    /// 预滤波每纹素的重要性采样数
    static constexpr UInt32 kPrefilterSampleCount = 1024;

    /// BRDF 查找表的边长与采样数
    ///
    /// 表面本身极其平滑, 256 已经远超需要; 采样数则要足够, 否则掠射角
    /// 一列会有可见噪声, 而那一列恰恰是菲涅尔效应最强的地方。
    static constexpr UInt32 kBrdfLutSize        = 256;
    static constexpr UInt32 kBrdfLutSampleCount = 1024;

    FEnvironmentMap() = default;
    ~FEnvironmentMap();

    LIMX_NON_COPYABLE(FEnvironmentMap);
    LIMX_NON_MOVABLE(FEnvironmentMap);

    /// 由等距柱状浮点图构建全套 IBL 资源
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

    /// 预滤波贴图的立方体采样视图 —— 供 PBR 的镜面环境项使用
    LIMX_NODISCARD FRHITextureViewHandle GetPrefilteredView() const
    {
        return m_Prefiltered.SampleView;
    }

    /// BRDF 查找表视图
    LIMX_NODISCARD FRHITextureViewHandle GetBrdfLutView() const
    {
        return m_BrdfLutView;
    }

    /// 立方体贴图共用的采样器 (线性 + 三线性 mip + 钳位)
    LIMX_NODISCARD FRHISamplerHandle GetSampler() const { return m_Sampler; }

    /// BRDF 查找表专用采样器 —— 必须钳位且无 mip
    LIMX_NODISCARD FRHISamplerHandle GetBrdfSampler() const
    {
        return m_BrdfSampler;
    }

    LIMX_NODISCARD UInt32 GetFaceSize() const
    {
        return m_Environment.FaceSize;
    }

    /// 预滤波贴图的最高 mip 下标 —— 着色器要用它把粗糙度映射到 LOD
    LIMX_NODISCARD Float32 GetPrefilteredMaxLod() const
    {
        return (m_Prefiltered.MipLevels > 0)
            ? static_cast<Float32>(m_Prefiltered.MipLevels - 1)
            : 0.0f;
    }

    /// 全部 IBL 资源占用的显存字节数
    LIMX_NODISCARD SizeType GetMemoryBytes() const
    {
        // BRDF 查找表是 RG16F, 每纹素 4 字节
        const SizeType brdfBytes =
            static_cast<SizeType>(kBrdfLutSize) * kBrdfLutSize * 4;

        return m_Environment.GetMemoryBytes() +
               m_Irradiance.GetMemoryBytes() +
               m_Prefiltered.GetMemoryBytes() +
               brdfBytes;
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

    /// 把预滤波贴图的某一级 mip 读回内存 (RGBA32F, 六个面依次排列)
    ///
    /// 预滤波是整条 IBL 链里最难靠肉眼判断的一环: 它的每一级都是模糊的,
    /// 而"该有多模糊"没有直觉上的参照。级间映射错位、某一级没被写入、
    /// 采样 mip 选偏一档, 画面上都只表现为"反射的感觉不太对"。
    ///
    /// @param mipLevel 要读取的级别; 超出范围时返回 false
    LIMX_NODISCARD bool ReadbackPrefiltered(FRenderContext*  context,
                                            UInt32           mipLevel,
                                            TArray<Float32>& outPixels,
                                            UInt32&          outFaceSize) const;

    /// 把 BRDF 查找表读回内存 (RG32F 两通道)
    ///
    /// 这张表有一条硬性质可以自查: 白炉条件下 (F0=1, 环境恒为 1) 应有
    /// A + B ≈ 1, 也就是入射能量既不被凭空放大也不被吞掉。数值一取回来
    /// 就能验, 不必等到画面上看出金属偏暗。
    ///
    /// @param outPixels 输出 —— 尺寸为 size * size * 2 个浮点 (A, B 交错)
    /// @param outSize   输出 —— 表的边长
    LIMX_NODISCARD bool ReadbackBrdfLut(FRenderContext*  context,
                                        TArray<Float32>& outPixels,
                                        UInt32&          outSize) const;

private:
    /// 把一张立方体贴图的指定 mip 读回内存 —— 两个公开读回接口的共同实现
    LIMX_NODISCARD bool ReadbackCubeMip(FRenderContext*      context,
                                        const FCubeResource& resource,
                                        UInt32               mipLevel,
                                        TArray<Float32>&     outPixels,
                                        UInt32&              outFaceSize) const;

    /// 把源图上传为一张可采样的 2D 纹理
    ERHIResult UploadEquirect(FRenderContext* context,
                              const FImageData& equirect);

    /// 创建一张立方体贴图及其采样视图与逐级存储视图
    ERHIResult CreateCubeResource(IRHIDevice*     device,
                                  UInt32          faceSize,
                                  UInt32          mipLevels,
                                  const AnsiChar* debugName,
                                  FCubeResource&  outResource);

    /// 逐级 blit 生成立方体贴图的 mip 链
    ///
    /// 六个面各自独立降采样, 不跨面混合。低级 mip 的面边界因此会有极细的
    /// 接缝, 但后续的卷积与预滤波本身就是大范围平均, 这点误差远在噪声之下。
    ERHIResult GenerateCubeMips(FRenderContext* context,
                                FCubeResource&  resource);

    /// 创建立方体贴图共用的采样器 —— MaxLod 必须覆盖整条 mip 链
    ERHIResult CreateSampler(IRHIDevice* device, UInt32 mipLevels);

    /// 创建 BRDF 查找表的纹理、视图与采样器
    ERHIResult CreateBrdfLutResources(IRHIDevice* device);

    /// 等距柱状 → 立方体贴图
    ERHIResult ConvertEquirectToCube(FRenderContext* context);

    /// 立方体贴图 → 辐照度贴图
    ERHIResult ConvolveIrradiance(FRenderContext* context);

    /// 立方体贴图 → 按粗糙度分级的预滤波贴图
    ERHIResult PrefilterSpecular(FRenderContext* context);

    /// 生成 BRDF 查找表
    ERHIResult IntegrateBrdfLut(FRenderContext* context);

    /// 释放仅转换期间需要的资源
    void ReleaseTransientResources(IRHIDevice* device);

    // ====================================================================
    // 成员
    // ====================================================================

    IRHIDevice*   m_Device = nullptr;

    FCubeResource m_Environment;
    FCubeResource m_Irradiance;
    FCubeResource m_Prefiltered;

    /// 三张立方体贴图共用 —— 采样参数完全相同, 没有分开的理由
    FRHISamplerHandle m_Sampler;

    // ---- BRDF 查找表 ----
    FRHITextureHandle     m_BrdfLutTexture;
    FRHITextureViewHandle m_BrdfLutView;

    /// 查找表专用采样器: 必须 ClampToEdge。Repeat 会让 n·v→1 的一列
    /// 取到 n·v→0 的值, 正面看物体时反射强度突然跳变。
    FRHISamplerHandle     m_BrdfSampler;

    // ---- 仅转换期间存在 ----
    FRHITextureHandle     m_EquirectTexture;
    FRHITextureViewHandle m_EquirectView;
    FRHIBufferHandle      m_StagingBuffer;
    FRHISamplerHandle     m_EquirectSampler;
};

} // namespace Limx
