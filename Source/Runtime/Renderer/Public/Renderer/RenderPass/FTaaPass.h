/*******************************************************************************
 * 文件: FTaaPass.h
 * 创建时间: 2026-08-31
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   时域抗锯齿 (TAA) 的解析通道
 *
 * 设计哲学:
 *   MRT 同时写解析目标与历史 — 解析目标是**固定的一张**纹理, 后处理通道的
 *     描述符一次写定; 历史是乒乓的两张之一。若让后处理直接采样乒乓的历史,
 *     它的描述符就要逐帧改 —— 而改一个正在被上一帧使用的描述符集是验证层
 *     错误。用一次拷贝也能解决, 但那是一次全屏读写; MRT 的第二次写入几乎
 *     免费 (同一个片元, 数据已在寄存器里)。
 *
 *   TAA 最危险的失效方式是**看起来正常但什么都没做**。裁剪范围取小了, 历史
 *     每帧都被拉回当前值, 结果与不开 TAA 几乎一样 —— 而画面上没有任何异常,
 *     只是锯齿还在。所以验收判据不能是"看着不糊", 必须是数值的:
 *     --taa-check 断言 TAA 的输出比任何单帧都更接近多帧平均。
 *
 *   默认关闭, 与 TAA 抖动的开关联动。抖动开而 TAA 关 = 画面纯粹多一层
 *     每帧变化的亚像素噪声; TAA 开而抖动关 = 每帧采样位置相同, 累积不出
 *     任何新信息, 只剩下运动时的拖影。两者必须同开同关。
 *
 * 技术特性:
 *   - 方差裁剪 (均值 ± γ·标准差) 而非 3x3 min/max 包围盒
 *   - 按亮度加权混合, 压制高光闪烁
 *   - 重投影用不含抖动的速度缓冲
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

class FTaaPass final : public IRenderPass
{
public:
    FTaaPass()           = default;
    ~FTaaPass() override = default;

    LIMX_NODISCARD const AnsiChar* GetName() const override
    {
        return "TaaPass";
    }

    /// 800 — 前向 (200) 与天空 (150) 之后, 后处理 (900) 之前
    ///
    /// 必须在色调映射之前: TAA 要在线性 HDR 空间里做累积。在色调映射之后
    /// 做的话, 混合的是已经压缩过的值, 亮部的权重被非线性地改变 —— 表现是
    /// 高光区域收敛得比暗部慢, 看起来像"亮的地方有残影"。
    LIMX_NODISCARD UInt32 GetOrder() const override
    {
        return 800;
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

    /// 解析结果的视图 —— 后处理通道采样它
    ///
    /// 固定的一张纹理, 交换链重建之前不会变。
    LIMX_NODISCARD FRHITextureViewHandle GetResolveView() const
    {
        return m_ResolveView;
    }

    /// 设置速度缓冲的视图 (由 FRenderer 从深度预通道取)
    void SetVelocityView(FRHITextureViewHandle view);

    /// 开关。关闭时 Execute 直接返回, 历史也会被标记为失效。
    void SetEnabled(bool enabled);

    LIMX_NODISCARD bool IsEnabled() const { return m_Enabled; }

    /// 当前帧的权重 (0~1)。越小则历史累积越久、越平滑, 但运动时拖影越长。
    void SetBlendFactor(Float32 blend) { m_BlendFactor = blend; }

    LIMX_NODISCARD Float32 GetBlendFactor() const { return m_BlendFactor; }

    /// 方差裁剪的 γ。越大越宽容 (保留更多历史), 越小越保守。
    void SetClipGamma(Float32 gamma) { m_ClipGamma = gamma; }

    /// 丢弃历史 —— 下一帧直接输出当前帧
    ///
    /// 交换链重建、相机瞬移、场景切换之后必须调用。不丢的话会与一张内容
    /// 完全不相干的历史混合, 表现是切换后几帧的鬼影。
    void InvalidateHistory() { m_HasHistory = false; }

private:
    ERHIResult CreateTargets(IRHIDevice* device, FRHIExtent2D extent);
    ERHIResult CreateRenderPassAndFramebuffers(IRHIDevice* device,
                                               FRHIExtent2D extent);
    ERHIResult CreateDescriptors(IRHIDevice* device);
    ERHIResult CreatePipeline(IRHIDevice* device);

    void DestroyTargets(IRHIDevice* device);
    void DestroyFramebuffers(IRHIDevice* device);

    /// 重写采样用的描述符 (历史与速度)
    void UpdateDescriptors(IRHIDevice* device);

    IRHIDevice*              m_Device = nullptr;

    FRHIExtent2D             m_Extent = {};

    /// 解析目标 — 固定的一张, 后处理采样它
    FRHITextureHandle        m_ResolveTexture;
    FRHITextureViewHandle    m_ResolveView;

    /// 历史 — 乒乓的两张
    FRHITextureHandle        m_HistoryTexture[2];
    FRHITextureViewHandle    m_HistoryView[2];

    FRHITextureViewHandle    m_SourceView;     // 前向通道的 HDR 输出
    FRHITextureViewHandle    m_VelocityView;

    FRHISamplerHandle        m_Sampler;

    FRHIRenderPassHandle     m_RenderPass;
    FRHIFramebufferHandle    m_Framebuffer[2];

    FRHIDescSetLayoutHandle  m_DescSetLayout;
    FRHIDescriptorSetHandle  m_DescriptorSet[2];

    FRHIPipelineLayoutHandle m_PipelineLayout;
    FRHIGraphicsPipelineHandle m_Pipeline;

    FRHIShaderHandle         m_VertShader;
    FRHIShaderHandle         m_FragShader;

    /// 本帧写哪一张历史 (0/1)
    ///
    /// 单调递增的帧号取模, 而不是 FrameIndex —— 后者在 MaxFramesInFlight
    /// 不等于 2 时与历史的乒乓节奏对不上。历史的节奏由"上一次渲染的那一帧"
    /// 决定, 与并行帧数无关。
    UInt32                   m_HistoryParity = 0;

    bool                     m_Enabled     = false;
    bool                     m_HasHistory  = false;

    Float32                  m_BlendFactor = 0.1f;
    Float32                  m_ClipGamma   = 1.0f;
};

} // namespace Limx
