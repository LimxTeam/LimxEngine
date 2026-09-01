// ============================================================
// 文件名称：FRayTracedAoPass.h
// 创建时间：2026-08-31
// 创建者  ：LimxTeam
// 设计哲学：GTAO 只能验"朝 0.5 单调收敛"，光追 AO 有闭式解可以对 ——
//          直角凹角处、搜索半径 R、离墙 d 时遮蔽率是
//              (1-c²)/2 - (2/π)∫_c^1 s·arcsin(c/s) ds,   c = d/R
//          一个有闭式解的量，判据就不该停在"趋势对"。
// 功能描述：逐像素在法线半球内按余弦加权投射若干条射线，输出环境光遮蔽。
// 技术特性：余弦加权采样（估计量就是命中比例，不需要额外权重）；
//          Hammersley + 逐像素旋转，确定性可复现；采样数与半径都是
//          外部指定 —— 判据要拿它们扫参数。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                          │ 描述                      │
// │────────────────────────────────│─────────────────────────│
// │ SetInputs()                    │ 绑定深度与法线输入        │
// │ SetTlas()                      │ 绑定场景加速结构          │
// │ SetRadius() / SetSampleCount() │ 搜索半径与每像素采样数    │
// │ GetAoView()                    │ 取 AO 图 (供着色采样)     │
// │ Execute()                      │ 派发计算                  │
// ============================================================

#pragma once

#include "Renderer/RendererMinimal.h"
#include "Renderer/RenderPass/IRenderPass.h"

namespace Limx
{

// ============================================================================
// FRayTracedAoPass — 光线追踪环境光遮蔽
// ============================================================================

class LIMX_RENDERER_API FRayTracedAoPass final : public IRenderPass
{
public:
    FRayTracedAoPass() = default;
    ~FRayTracedAoPass() override = default;

    LIMX_NODISCARD const AnsiChar* GetName() const override
    {
        return "RayTracedAoPass";
    }

    /// 排在光追阴影 (120) 之后, 天空 (150) 之前
    ///
    /// 两者都读深度预通道的产出, 彼此不相干; 排在一起只是为了让所有
    /// "读深度写屏幕空间图"的通道挨着, 布局转换能省几次。
    LIMX_NODISCARD UInt32 GetOrder() const override { return 130; }

    ERHIResult Setup(const FPassSetupDesc& desc) override;

    void Execute(IRHICommandBuffer*        commandBuffer,
                 const FRenderPassContext& context) override;

    ERHIResult OnResize(const FPassResizeDesc& desc) override;

    void ReleaseSwapchainResources(IRHIDevice* device) override;

    void Shutdown(IRHIDevice* device) override;

    void SetInputs(FRHITextureHandle depthTexture,
                   FRHITextureViewHandle depthView,
                   FRHITextureViewHandle normalView)
    {
        m_DepthTexture = depthTexture;
        m_DepthView    = depthView;
        m_NormalView   = normalView;
    }

    void SetTlas(FRHIAccelStructHandle tlas) { m_Tlas = tlas; }

    void SetCameraParams(const FMatrix& viewProj)
    {
        m_ViewProj = viewProj;
    }

    void SetEnabled(bool enabled) { m_Enabled = enabled; }

    LIMX_NODISCARD bool IsEnabled() const { return m_Enabled; }

    /// 搜索半径 (世界单位)
    ///
    /// 判据要扫它 —— 闭式解是半径的函数, 而"结果随半径怎么变"比"某个
    /// 半径下等于多少"难蒙混得多。
    void SetRadius(Float32 radius) { m_Radius = radius; }

    LIMX_NODISCARD Float32 GetRadius() const { return m_Radius; }

    /// 每像素的采样数
    ///
    /// 蒙特卡洛的误差按 1/sqrt(N) 降 (低差异序列更快)。判据要能调高它,
    /// 否则"实现有系统偏差"与"采样数不够"分不开。
    void SetSampleCount(UInt32 count) { m_SampleCount = count; }

    LIMX_NODISCARD UInt32 GetSampleCount() const { return m_SampleCount; }

    LIMX_NODISCARD FRHITextureHandle GetAoTexture() const
    {
        return m_AoTexture;
    }

    LIMX_NODISCARD FRHITextureViewHandle GetAoView() const
    {
        return m_AoView;
    }

private:
    ERHIResult CreateTarget(IRHIDevice* device, FRHIExtent2D extent);
    ERHIResult CreatePipeline(IRHIDevice* device);
    ERHIResult CreateDescriptors(IRHIDevice* device);

    IRHIDevice* m_Device = nullptr;

    bool m_Enabled = false;

    FRHIExtent2D m_Extent = {};

    FRHITextureHandle     m_AoTexture;
    FRHITextureViewHandle m_AoView;

    FRHITextureHandle     m_DepthTexture;
    FRHITextureViewHandle m_DepthView;
    FRHITextureViewHandle m_NormalView;

    FRHISamplerHandle m_PointSampler;

    FRHIAccelStructHandle m_Tlas;

    FRHIShaderHandle          m_Shader;
    FRHIDescSetLayoutHandle   m_SetLayout;
    FRHIDescriptorSetHandle   m_DescriptorSet;
    FRHIPipelineLayoutHandle  m_PipelineLayout;
    FRHIComputePipelineHandle m_Pipeline;

    FMatrix m_ViewProj = FMatrix::kIdentity;

    /// 与 GTAO 的默认半径取同一个数 —— 两者要能在同一个场景上直接比
    Float32 m_Radius = 0.8f;

    /// 默认采样数。判据会调到更高
    UInt32 m_SampleCount = 16;

    /// 射线起点沿法线的偏移与 tMin —— 与光追阴影同理, 盖住深度的量化误差
    Float32 m_NormalOffset = 1.0e-3f;
    Float32 m_RayTMin      = 1.0e-3f;

    /// 纹理是否已经离开 Undefined 布局
    ///
    /// 通道禁用时也要做这一次转换 —— 它被绑进了描述符集, 而 Vulkan 检查的是
    /// 描述符声明的布局与图像实际布局是否一致, 与读不读无关。
    bool m_LayoutInitialized = false;
};

} // namespace Limx
