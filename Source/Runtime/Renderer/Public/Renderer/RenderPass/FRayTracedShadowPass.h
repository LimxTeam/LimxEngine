// ============================================================
// 文件名称：FRayTracedShadowPass.h
// 创建时间：2026-08-31
// 创建者  ：LimxTeam
// 设计哲学：阴影贴图回答"这个点在光源的深度图里排第几"，光追回答"从这个
//          点到光源的线段上有没有东西"。后者就是问题本身的定义 —— 偏置、
//          漏光、级联接缝这些词只属于前者。所以这个通道刻意**不提供**
//          任何"调一调就好看了"的旋钮：起点的容差是从深度缓冲区的量化
//          误差推出来的，不是试出来的。
// 功能描述：逐像素的光线追踪阴影 — 从深度缓冲区还原世界坐标，向指定光源
//          发一条阴影射线，输出一张全分辨率的可见度掩码。
// 技术特性：TerminateOnFirstHit（阴影只关心有没有，不关心最近的是哪个）；
//          一次只处理一盏光源，掩码是 R8_UNORM 单通道 —— 多光源需要
//          多通道打包，那是后面的事，现在不假装支持。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                          │ 描述                      │
// │────────────────────────────────│─────────────────────────│
// │ SetInputs()                    │ 绑定深度与法线输入        │
// │ SetTlas()                      │ 绑定场景加速结构          │
// │ SetLight()                     │ 指定这一帧照哪盏灯        │
// │ GetShadowMaskView()            │ 取可见度掩码 (供着色采样) │
// │ Execute()                      │ 派发计算                  │
// ============================================================

#pragma once

#include "Renderer/RendererMinimal.h"
#include "Renderer/RenderPass/IRenderPass.h"

#include "RenderCore/Lighting/FLight.h"

namespace Limx
{

// ============================================================================
// FRayTracedShadowPass — 光线追踪阴影
// ============================================================================

class LIMX_RENDERER_API FRayTracedShadowPass final : public IRenderPass
{
public:
    FRayTracedShadowPass() = default;
    ~FRayTracedShadowPass() override = default;

    LIMX_NODISCARD const AnsiChar* GetName() const override
    {
        return "RayTracedShadowPass";
    }

    /// 排在深度预通道 (100) 之后、天空 (150) 与前向 (200) 之前
    ///
    /// 它要读深度预通道写出的深度与法线, 而前向通道要读它写出的掩码 ——
    /// 这个顺序不是偏好, 是数据依赖。
    LIMX_NODISCARD UInt32 GetOrder() const override { return 120; }

    ERHIResult Setup(const FPassSetupDesc& desc) override;

    void Execute(IRHICommandBuffer*        commandBuffer,
                 const FRenderPassContext& context) override;

    ERHIResult OnResize(const FPassResizeDesc& desc) override;

    void ReleaseSwapchainResources(IRHIDevice* device) override;

    void Shutdown(IRHIDevice* device) override;

    /// 绑定深度与法线输入 (由 FRenderer 在通道就绪后调用)
    void SetInputs(FRHITextureHandle depthTexture,
                   FRHITextureViewHandle depthView,
                   FRHITextureViewHandle normalView)
    {
        m_DepthTexture = depthTexture;
        m_DepthView    = depthView;
        m_NormalView   = normalView;
    }

    /// 绑定场景加速结构 —— 无效时本通道整个跳过
    void SetTlas(FRHIAccelStructHandle tlas) { m_Tlas = tlas; }

    /// 指定这一帧为哪盏灯算阴影
    ///
    /// 一次只处理一盏。掩码是单通道的, 多光源要么多通道打包要么多张图,
    /// 两者都是实打实的一段工作 —— 在做之前不假装支持。
    void SetLight(const FLightData& light) { m_Light = light; }

    /// 相机矩阵 (反投影要用) 与近远平面
    void SetCameraParams(const FMatrix& viewProj,
                         Float32 nearPlane,
                         Float32 farPlane)
    {
        m_ViewProj  = viewProj;
        m_NearPlane = nearPlane;
        m_FarPlane  = farPlane;
    }

    void SetEnabled(bool enabled) { m_Enabled = enabled; }

    LIMX_NODISCARD bool IsEnabled() const { return m_Enabled; }

    LIMX_NODISCARD FRHITextureHandle GetShadowMaskTexture() const
    {
        return m_MaskTexture;
    }

    LIMX_NODISCARD FRHITextureViewHandle GetShadowMaskView() const
    {
        return m_MaskView;
    }

    /// 射线起点沿法线的偏移量 (世界单位)
    ///
    /// 判据要读它 —— 偏移会把影子边界推开一点点, 而推开多少是能算出来的。
    /// 判据据此预测边界位置, 而不是留一个"差不多"的容差。
    LIMX_NODISCARD Float32 GetNormalOffset() const { return m_NormalOffset; }

    void SetNormalOffset(Float32 offset) { m_NormalOffset = offset; }

    LIMX_NODISCARD Float32 GetRayTMin() const { return m_RayTMin; }

private:
    ERHIResult CreateMask(IRHIDevice* device, FRHIExtent2D extent);
    ERHIResult CreatePipeline(IRHIDevice* device);
    ERHIResult CreateDescriptors(IRHIDevice* device);

    IRHIDevice* m_Device = nullptr;

    bool m_Enabled = false;

    FRHIExtent2D m_Extent = {};

    FRHITextureHandle     m_MaskTexture;
    FRHITextureViewHandle m_MaskView;

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

    FLightData m_Light;

    FMatrix m_ViewProj  = FMatrix::kIdentity;
    Float32 m_NearPlane = 0.1f;
    Float32 m_FarPlane  = 100.0f;

    /// 射线起点沿法线的偏移
    ///
    /// 1e-3 世界单位。这个数不是调出来的: 世界坐标是从深度缓冲区反投影
    /// 得到的, 它自带量化误差 (实测 near=0.1 的场景上 10 米处约 5.5e-5),
    /// 容差盖住它即可, 1e-3 是近二十倍余量。
    Float32 m_NormalOffset = 1.0e-3f;

    /// 射线的 tMin —— 与法线偏移同理, 防的是掠射角之外的自相交
    Float32 m_RayTMin = 1.0e-3f;

    /// 纹理是否已经离开 Undefined 布局
    ///
    /// 通道禁用时也要做这一次转换 —— 它被绑进了描述符集, 而 Vulkan 检查的是
    /// 描述符声明的布局与图像实际布局是否一致, 与读不读无关。
    bool m_LayoutInitialized = false;
};

} // namespace Limx
