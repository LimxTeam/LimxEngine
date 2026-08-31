/*******************************************************************************
 * 文件: FBloomPass.h
 * 创建时间: 2026-08-31
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   泛光 (Bloom) — 降采样链 + 升采样链 + 合成
 *
 * 设计哲学:
 *   用**独立纹理**而非一张纹理的多级 mip — mip 视图要给每一级建一个视图,
 *     而且渲染到某一级时其余级的布局状态要单独跟踪。独立纹理让每一级的
 *     生命周期完全独立, 代价只是几张小纹理 (总和约全分辨率的 1/3)。
 *
 *   升采样**加回**降采样链的同一组纹理 — 读第 i+1 级、写第 i 级, 两者是
 *     不同的纹理, 所以不存在读写同一张的问题。省掉一整套升采样纹理。
 *
 *   在色调映射**之前**合成 — 泛光加到已映射的值上是错的: 亮部本来就被压平,
 *     泛光叠上去几乎看不出来, 而暗部的泛光又会显得过强。
 *
 *   排在 TAA (800) 之后 — 泛光的输入应当是已经消过锯齿的图像。反过来的话,
 *     锯齿边缘的高频会被泛光放大成一圈爬行的亮边, 而 TAA 在那之后已经无从
 *     区分"真实的泛光"与"被放大的锯齿"。
 *
 * 技术特性:
 *   - 13 抽头降采样 (Jimenez 2014), 权重和恰好为 1
 *   - 3x3 帐篷升采样, 权重和恰好为 1
 *   - 软膝盖阈值, 避免亮度跨过阈值时泛光突然出现
 *
 * 依赖关系:
 *   内部: Renderer/RenderPass/IRenderPass.h, RHI
 *
 ******************************************************************************/

#pragma once

#include "Renderer/RendererMinimal.h"
#include "Renderer/RenderPass/IRenderPass.h"

namespace Limx
{

class FBloomPass final : public IRenderPass
{
public:
    /// 降采样链的级数
    ///
    /// 6 级意味着最小的一级是 1/64 分辨率 (1280x720 下是 20x11)。再多几级
    /// 收益很小 —— 那时一个纹素已经覆盖屏幕的十几分之一, 泛光的形状完全由
    /// 升采样的帐篷核决定, 与源图像无关。
    static constexpr UInt32 kMipCount = 6;

    FBloomPass()           = default;
    ~FBloomPass() override = default;

    LIMX_NODISCARD const AnsiChar* GetName() const override
    {
        return "BloomPass";
    }

    /// 850 — TAA (800) 之后, 色调映射 (900) 之前
    LIMX_NODISCARD UInt32 GetOrder() const override
    {
        return 850;
    }

    ERHIResult Setup(const FPassSetupDesc& desc) override;

    void Execute(IRHICommandBuffer*        commandBuffer,
                 const FRenderPassContext& context) override;

    ERHIResult OnResize(const FPassResizeDesc& desc) override;

    void ReleaseSwapchainResources(IRHIDevice* device) override;

    void Shutdown(IRHIDevice* device) override;

    // ========================================================================
    // 外部接线
    // ========================================================================

    /// 合成结果的视图 —— 色调映射通道采样它
    LIMX_NODISCARD FRHITextureViewHandle GetOutputView() const
    {
        return m_OutputView;
    }

    /// 合成结果的纹理 (自检回读用)
    LIMX_NODISCARD FRHITextureHandle GetOutputTexture() const
    {
        return m_OutputTexture;
    }

    /// 泛光链最大的那一级 (自检回读用) —— 半分辨率
    LIMX_NODISCARD FRHITextureHandle GetBloomTexture() const
    {
        return m_MipTexture[0];
    }

    LIMX_NODISCARD FRHIExtent2D GetBloomExtent() const
    {
        return m_MipExtent[0];
    }

    /// 链上任意一级 (自检回读用)
    ///
    /// 自检要比较第 0 级与第 1 级的能量: 升采样把每一级**加回**上一级,
    /// 所以 sum(mip0) 会明显大于 4*sum(mip1); 若改成覆盖写, 两者相等。
    /// 那是"多尺度累加确实在发生"的直接证据, 而画面上两者都是"一团光"。
    LIMX_NODISCARD FRHITextureHandle GetMipTexture(UInt32 level) const
    {
        return (level < kMipCount) ? m_MipTexture[level] : FRHITextureHandle();
    }

    LIMX_NODISCARD FRHIExtent2D GetMipExtent(UInt32 level) const
    {
        return (level < kMipCount) ? m_MipExtent[level] : FRHIExtent2D{};
    }

    /// 设置输入 (场景颜色)。TAA 开时是 TAA 的解析目标, 否则是 HDR 目标。
    void SetSourceView(FRHITextureViewHandle view);

    void SetEnabled(bool enabled) { m_Enabled = enabled; }

    LIMX_NODISCARD bool IsEnabled() const { return m_Enabled; }

    /// 亮度阈值 —— 超过它的部分才参与泛光
    void SetThreshold(Float32 threshold) { m_Threshold = threshold; }

    LIMX_NODISCARD Float32 GetThreshold() const { return m_Threshold; }

    /// 软膝盖宽度 —— 阈值附近的过渡区间
    void SetKnee(Float32 knee) { m_Knee = knee; }

    LIMX_NODISCARD Float32 GetKnee() const { return m_Knee; }

    /// 合成强度
    void SetIntensity(Float32 intensity) { m_Intensity = intensity; }

    LIMX_NODISCARD Float32 GetIntensity() const { return m_Intensity; }

    /// 升采样的帐篷核半径 (纹素)
    void SetFilterRadius(Float32 radius) { m_FilterRadius = radius; }

private:
    ERHIResult CreateTargets(IRHIDevice* device, FRHIExtent2D extent);
    ERHIResult CreateRenderPasses(IRHIDevice* device);
    ERHIResult CreateFramebuffers(IRHIDevice* device);
    ERHIResult CreateDescriptors(IRHIDevice* device);
    ERHIResult CreatePipelines(IRHIDevice* device);

    void DestroyTargets(IRHIDevice* device);
    void DestroyFramebuffers(IRHIDevice* device);

    void UpdateDescriptors(IRHIDevice* device);

    IRHIDevice*                m_Device = nullptr;

    FRHIExtent2D               m_Extent = {};

    /// 降采样链。[0] 是半分辨率, 逐级减半。
    FRHITextureHandle          m_MipTexture[kMipCount];
    FRHITextureViewHandle      m_MipView[kMipCount];
    FRHIExtent2D               m_MipExtent[kMipCount];

    /// 合成结果 (全分辨率)
    FRHITextureHandle          m_OutputTexture;
    FRHITextureViewHandle      m_OutputView;

    FRHITextureViewHandle      m_SourceView;

    FRHISamplerHandle          m_Sampler;

    /// 降采样与升采样各一个渲染通道。
    ///
    /// 差别只在 LoadOp 与混合: 降采样覆盖写 (DontCare + Opaque), 升采样要
    /// **加回**已有的降采样结果 (Load + 加法混合)。用同一个通道做不到 ——
    /// LoadOp 是渲染通道的属性, 不是逐次绘制的状态。
    ///
    /// Framebuffer 两者共用: Vulkan 的渲染通道兼容性只看附件的格式与采样数,
    /// 不看 LoadOp/StoreOp/布局。
    FRHIRenderPassHandle       m_DownRenderPass;
    FRHIRenderPassHandle       m_UpRenderPass;

    /// 合成用另一个 —— 它有两个输入而不是一个, 描述符布局不同
    FRHIRenderPassHandle       m_CompositeRenderPass;

    FRHIFramebufferHandle      m_MipFramebuffer[kMipCount];
    FRHIFramebufferHandle      m_OutputFramebuffer;

    /// 降采样: [0] 从源图读 (带阈值), [i] 从 mip[i-1] 读
    FRHIDescSetLayoutHandle    m_ChainSetLayout;
    FRHIDescriptorSetHandle    m_DownSet[kMipCount];

    /// 升采样: [i] 从 mip[i+1] 读, 写 mip[i]
    FRHIDescriptorSetHandle    m_UpSet[kMipCount];

    FRHIDescSetLayoutHandle    m_CompositeSetLayout;
    FRHIDescriptorSetHandle    m_CompositeSet;

    FRHIPipelineLayoutHandle   m_ChainPipelineLayout;
    FRHIPipelineLayoutHandle   m_CompositePipelineLayout;

    FRHIGraphicsPipelineHandle m_DownPipeline;
    FRHIGraphicsPipelineHandle m_UpPipeline;
    FRHIGraphicsPipelineHandle m_CompositePipeline;

    FRHIShaderHandle           m_VertShader;
    FRHIShaderHandle           m_DownShader;
    FRHIShaderHandle           m_UpShader;
    FRHIShaderHandle           m_CompositeShader;

    bool                       m_Enabled      = false;

    Float32                    m_Threshold    = 1.0f;
    Float32                    m_Knee         = 0.5f;
    Float32                    m_Intensity    = 0.05f;
    Float32                    m_FilterRadius = 1.0f;
};

} // namespace Limx
