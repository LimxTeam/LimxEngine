/*******************************************************************************
 * 文件: FSkyPass.h
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   天空盒 Pass — 采样环境立方体贴图填充无几何体覆盖的像素
 *
 * 设计哲学:
 *   排在前向 Pass **之前**而非之后。天空在深度上位于最远处, 直觉上应该
 *   最后画, 但前向 Pass 同时画不透明与半透明, 而半透明**不写深度**:
 *   若天空排在后面, 它会依据半透明像素下方仍为 1.0 的深度通过测试, 把
 *   悬空的半透明物体整片抹掉。这是一个只在"半透明物体前方没有任何几何体"
 *   时才出现的错误, 在室内场景里可能几周都碰不到一次。
 *
 *   因此本 Pass 承担了原属前向 Pass 的**清屏**职责: 它是第一个触碰 HDR
 *   目标的 Pass, 由它 Clear, 前向 Pass 改为 Load。没有环境贴图时它只清屏
 *   不绘制 —— 行为与从前完全一致。
 *
 *   不画立方体网格, 而是全屏三角形 + 视线重建。见 sky.vert 的说明。
 *
 * 技术特性:
 *   - 深度测试 LessOrEqual、深度写入关闭, 天空只落在深度仍为 1.0 处
 *   - 复用场景的 set 0 (view/proj), 自带 set 1 存放立方体贴图
 *   - 强度经 Push Constant 传入
 *
 * 依赖关系:
 *   内部: Renderer/RenderPass/IRenderPass.h
 *
 * 注意事项:
 *   环境贴图由外部 (FRenderer) 设置, 本 Pass 不拥有其生命周期
 *
 ******************************************************************************/

#pragma once

#include "Renderer/RenderPass/IRenderPass.h"

namespace Limx
{

// ============================================================================
// FSkyPass — 天空盒
// ============================================================================

class FSkyPass final : public IRenderPass
{
public:
    FSkyPass()           = default;
    ~FSkyPass() override = default;

    LIMX_NODISCARD const AnsiChar* GetName() const override
    {
        return "SkyPass";
    }

    /// 在深度预通道 (100) 之后、前向通道 (200) 之前
    ///
    /// 必须早于前向: 前向的半透明批次不写深度, 天空若排在其后会穿透它们。
    LIMX_NODISCARD UInt32 GetOrder() const override
    {
        return 150;
    }

    ERHIResult Setup(const FPassSetupDesc& desc) override;

    void Execute(IRHICommandBuffer*        commandBuffer,
                 const FRenderPassContext& context) override;

    ERHIResult OnResize(const FPassResizeDesc& desc) override;

    void ReleaseSwapchainResources(IRHIDevice* device) override;

    void Shutdown(IRHIDevice* device) override;

    // ====================================================================
    // 环境贴图
    // ====================================================================

    /// 绑定环境立方体贴图
    ///
    /// 传入无效句柄即解绑 —— 之后本 Pass 只清屏不绘制。切换关卡时必须
    /// 显式解绑: 贴图随旧关卡销毁后, 描述符集里留着的是一个已失效的视图。
    ///
    /// @param device  GPU 设备 (用于更新描述符集)
    /// @param cubeView 立方体采样视图, 无效则解绑
    /// @param sampler  采样器, 无效则解绑
    void SetEnvironmentMap(IRHIDevice*           device,
                           FRHITextureViewHandle cubeView,
                           FRHISamplerHandle     sampler);

    LIMX_NODISCARD bool HasEnvironmentMap() const
    {
        return m_HasEnvironmentMap;
    }

    /// 设置线性强度倍数
    ///
    /// HDRI 的绝对量级取决于拍摄标定, 不同来源可差上百倍。做成运行时参数
    /// 才能在不重新转换立方体贴图的前提下配平天空与直接光。
    void SetIntensity(Float32 intensity) { m_Intensity = intensity; }

    LIMX_NODISCARD Float32 GetIntensity() const { return m_Intensity; }

private:
    ERHIResult CreateRenderPass(IRHIDevice* device);
    ERHIResult CreateFramebuffer(IRHIDevice*           device,
                                 FRHIExtent2D          extent,
                                 FRHITextureViewHandle colorView,
                                 FRHITextureViewHandle depthView);
    void       DestroyFramebuffer(IRHIDevice* device);
    ERHIResult CreateDescriptorResources(IRHIDevice* device,
                                         FRHIDescSetLayoutHandle viewProjLayout);
    ERHIResult CreateShaders(IRHIDevice* device);
    ERHIResult CreatePipeline(IRHIDevice* device);

    // ====================================================================
    // 成员
    // ====================================================================

    FRHIRenderPassHandle       m_RenderPass;
    FRHIFramebufferHandle      m_Framebuffer;

    FRHIShaderHandle           m_VertShader;
    FRHIShaderHandle           m_FragShader;

    /// set 1 的布局 —— 只有一张立方体贴图
    FRHIDescSetLayoutHandle    m_CubeSetLayout;
    FRHIDescriptorSetHandle    m_CubeDescriptorSet;

    FRHIPipelineLayoutHandle   m_PipelineLayout;
    FRHIGraphicsPipelineHandle m_Pipeline;

    EPixelFormat               m_ColorFormat = EPixelFormat::Unknown;
    EPixelFormat               m_DepthFormat = EPixelFormat::Unknown;
    FRHIExtent2D               m_Extent      = {};

    bool                       m_HasEnvironmentMap = false;
    Float32                    m_Intensity         = 1.0f;
};

} // namespace Limx
