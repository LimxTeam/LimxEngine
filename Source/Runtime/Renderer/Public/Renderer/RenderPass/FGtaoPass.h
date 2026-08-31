/*******************************************************************************
 * 文件: FGtaoPass.h
 * 创建时间: 2026-08-31
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   屏幕空间环境光遮蔽 (GTAO) 通道
 *
 * 设计哲学:
 *   排在深度预通道 (100) 与前向通道 (200) 之间 — 这个位置不是随便挑的:
 *     前向通道对深度的 StoreOp 是 DontCare, 之后深度内容就是未定义的, 所以
 *     任何要读深度的东西都必须在它之前。而 AO 的结果又要被前向通道消费,
 *     于是只剩下 (100, 200) 这个区间。
 *
 *   法线取自 G-Buffer 而不是从深度差分重建 — 差分在深度不连续处会给出完全
 *     错误的方向, 而那正是 AO 最需要准确的地方 (物体边缘)。Day 4 那张法线
 *     缓冲的正式用途就是这个。
 *
 *   关闭时输出恒为 1 而不是让前向通道分支 — 少一个 uniform、少一个分支,
 *     而代价只是一次清除。更要紧的是: 有分支的话"AO 通道没跑"与"AO 恰好
 *     全是 1"在画面上无法区分, 而前者是缺陷。
 *
 * 技术特性:
 *   - 4 个方向 × 8 步进, 逐像素旋转打散成噪点 (噪点能被 TAA 消掉, 条纹不能)
 *   - 余弦加权的弧积分, 而非计数式的遮挡比例
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

class FGtaoPass final : public IRenderPass
{
public:
    FGtaoPass()           = default;
    ~FGtaoPass() override = default;

    LIMX_NODISCARD const AnsiChar* GetName() const override
    {
        return "GtaoPass";
    }

    /// 130 — 深度预通道 (100) 与分簇剔除 (120) 之后, 天空 (150) 之前
    LIMX_NODISCARD UInt32 GetOrder() const override
    {
        return 130;
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

    /// AO 结果的视图 —— 前向通道采样它
    LIMX_NODISCARD FRHITextureViewHandle GetAoView() const { return m_AoView; }

    /// AO 结果的纹理 (自检回读用)
    LIMX_NODISCARD FRHITextureHandle GetAoTexture() const
    {
        return m_AoTexture;
    }

    /// 接入深度与法线 (由 FRenderer 在 SetupAll 之后调用)
    void SetInputs(FRHITextureHandle     depthTexture,
                   FRHITextureViewHandle depthView,
                   FRHITextureViewHandle normalView);

    /// 每帧的相机参数
    ///
    /// 近远平面显式传入而不是从投影矩阵反解。反解在数学上可行
    /// (near = M[2][3]/M[2][2]), 但那是两个接近的数相除, 远平面较大时相对
    /// 误差会放大到千分之几 —— 而线性深度的公式里 far 出现在分母上。
    void SetCameraParams(const FMatrix& view,
                         const FMatrix& projectionNoJitter,
                         Float32        nearPlane,
                         Float32        farPlane);

    void SetEnabled(bool enabled) { m_Enabled = enabled; }

    LIMX_NODISCARD bool IsEnabled() const { return m_Enabled; }

    /// 采样半径 (世界单位)
    void SetRadius(Float32 radius) { m_Radius = radius; }

    /// 强度指数。1 = 物理值, 大于 1 则加深。
    void SetIntensity(Float32 intensity) { m_Intensity = intensity; }

private:
    ERHIResult CreateTarget(IRHIDevice* device, FRHIExtent2D extent);
    ERHIResult CreateRenderPassAndFramebuffer(IRHIDevice* device,
                                              FRHIExtent2D extent);
    ERHIResult CreateDescriptors(IRHIDevice* device);
    ERHIResult CreatePipeline(IRHIDevice* device);

    void UpdateDescriptors(IRHIDevice* device);

    IRHIDevice*                m_Device = nullptr;

    FRHIExtent2D               m_Extent = {};

    FRHITextureHandle          m_AoTexture;
    FRHITextureViewHandle      m_AoView;

    /// 深度纹理句柄 —— 采样前后要做布局转换, 光有视图不够
    FRHITextureHandle          m_DepthTexture;
    FRHITextureViewHandle      m_DepthView;
    FRHITextureViewHandle      m_NormalView;

    FRHISamplerHandle          m_Sampler;

    FRHIRenderPassHandle       m_RenderPass;
    FRHIFramebufferHandle      m_Framebuffer;

    FRHIDescSetLayoutHandle    m_DescSetLayout;
    FRHIDescriptorSetHandle    m_DescriptorSet;

    FRHIPipelineLayoutHandle   m_PipelineLayout;
    FRHIGraphicsPipelineHandle m_Pipeline;

    FRHIShaderHandle           m_VertShader;
    FRHIShaderHandle           m_FragShader;

    FMatrix                    m_View              = FMatrix::kIdentity;
    FMatrix                    m_InverseProjection = FMatrix::kIdentity;

    bool                       m_Enabled   = false;
    Float32                    m_Radius    = 0.8f;
    Float32                    m_Intensity = 1.0f;

    /// 近远平面。着色器目前不用它们 (视空间位置走逆投影矩阵), 但接口留着
    /// —— 半分辨率 AO 的双边上采样需要按深度加权, 那时会用到。
    Float32                    m_NearPlane   = 0.1f;
    Float32                    m_FarPlane    = 100.0f;

    /// 关闭状态下是否已经把 AO 清成 1
    ///
    /// 只清一次而不是每帧清 —— 内容不会变, 而每帧清是一次全屏写。
    bool                       m_ClearedToOne = false;
};

} // namespace Limx
