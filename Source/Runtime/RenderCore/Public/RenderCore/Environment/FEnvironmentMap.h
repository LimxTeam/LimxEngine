/*******************************************************************************
 * 文件: FEnvironmentMap.h
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   环境立方体贴图 — 由等距柱状 HDR 图经计算着色器转换而来
 *
 * 设计哲学:
 *   转换在 GPU 上做而非 CPU — 一张 1024 边长的立方体贴图有六百万个纹素,
 *   每个都要一次 atan/acos 与一次双线性采样。CPU 单线程要数秒, 而这只是
 *   开始: 后面的辐照度卷积每个纹素要积分上千个方向, 镜面预滤波还要对每个
 *   粗糙度层级重来一遍。CPU 路线在第二步就走不下去了。
 *
 *   存 RGBA16F 而非 RGBA32F — 半精度浮点在环境光照的量级上足够: 它有
 *   10 位尾数与 ±65504 的范围, 而 RGBE 源本身每通道只有 8 位尾数。
 *   代价却是一半的显存与一半的带宽, 后者在卷积时是实打实的瓶颈。
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
// FEnvironmentMap — 环境立方体贴图
// ============================================================================

class LIMX_RENDERCORE_API FEnvironmentMap
{
public:
    FEnvironmentMap() = default;
    ~FEnvironmentMap();

    LIMX_NON_COPYABLE(FEnvironmentMap);
    LIMX_NON_MOVABLE(FEnvironmentMap);

    /// 由等距柱状浮点图构建立方体贴图
    ///
    /// @param context   渲染上下文 (提供设备与一次性命令缓冲区)
    /// @param equirect  源图 —— 必须是浮点格式, 宽高比应为 2:1
    /// @param faceSize  面边长, 0 表示按源图宽度的 1/4 自动选取
    ///
    /// 自动面边长取源图宽度的 1/4: 等距柱状图的赤道一圈占满整个宽度,
    /// 而立方体贴图的赤道一圈跨四个面, 因此 1/4 恰好保持赤道处的角分辨率
    /// 不变 —— 再大只是插值放大, 再小则丢失细节。
    LIMX_NODISCARD ERHIResult BuildFromEquirect(FRenderContext* context,
                                                const FImageData& equirect,
                                                UInt32 faceSize = 0);

    /// 释放全部 GPU 资源
    void Release();

    LIMX_NODISCARD bool IsValid() const { return m_CubeView.IsValid(); }

    /// 立方体采样视图 —— 供天空盒与 IBL 使用
    LIMX_NODISCARD FRHITextureViewHandle GetCubeView() const
    {
        return m_CubeView;
    }

    LIMX_NODISCARD FRHISamplerHandle GetSampler() const { return m_Sampler; }

    LIMX_NODISCARD UInt32 GetFaceSize() const { return m_FaceSize; }

    /// 立方体贴图占用的显存字节数 (六个面, 不含 mip)
    LIMX_NODISCARD SizeType GetMemoryBytes() const;

private:
    /// 把源图上传为一张可采样的 2D 纹理
    ERHIResult UploadEquirect(FRenderContext* context,
                              const FImageData& equirect,
                              FRHITextureHandle& outTexture,
                              FRHITextureViewHandle& outView,
                              FRHIBufferHandle& outStaging);

    /// 创建立方体贴图及其两个视图 (Cube 采样 / 2DArray 存储)
    ERHIResult CreateCubeResources(IRHIDevice* device, UInt32 faceSize);

    /// 创建计算管线与描述符
    ERHIResult CreateConversionPipeline(IRHIDevice* device);

    /// 释放仅转换期间需要的资源
    void ReleaseConversionResources(IRHIDevice* device);

    // ====================================================================
    // 成员
    // ====================================================================

    IRHIDevice*              m_Device   = nullptr;

    FRHITextureHandle        m_CubeTexture;

    /// 采样视图 (TextureCube) 与存储视图 (Texture2DArray)
    ///
    /// 同一张图像两个视图: 计算着色器要逐纹素定位, 只有 2D 数组视图能做到;
    /// 而采样必须走 Cube 视图, 否则拿不到硬件的跨面过滤。
    FRHITextureViewHandle    m_CubeView;
    FRHITextureViewHandle    m_StorageView;

    FRHISamplerHandle        m_Sampler;

    UInt32                   m_FaceSize = 0;

    // ---- 仅转换期间存在 ----
    FRHITextureHandle        m_EquirectTexture;
    FRHITextureViewHandle    m_EquirectView;
    FRHIBufferHandle         m_StagingBuffer;
    FRHISamplerHandle        m_EquirectSampler;
    FRHIShaderHandle         m_ComputeShader;
    FRHIDescSetLayoutHandle  m_DescSetLayout;
    FRHIDescriptorSetHandle  m_DescriptorSet;
    FRHIPipelineLayoutHandle m_PipelineLayout;
    FRHIComputePipelineHandle m_Pipeline;
};

} // namespace Limx
