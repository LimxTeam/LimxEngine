/*******************************************************************************
 * 文件: FShadowPass.h
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   方向光阴影 Pass — 从光源视角渲染场景深度，产出供前向 Pass 采样的阴影贴图
 *
 * 设计哲学:
 *   阴影贴图自带尺寸，与交换链无关 — 它是光源空间的一张固定分辨率深度图，
 *   窗口缩放不该让阴影质量跟着抖动。因此本 Pass 不参与 OnResize 的尺寸重建。
 *
 *   光源矩阵由本 Pass 自己算并回写给 FLightManager — 绘制阴影贴图用的矩阵
 *   与片段着色器采样用的矩阵必须是同一个。让两处各自计算，快速转向时会
 *   相差一帧，表现为阴影"拖尾"。
 *
 *   投影体积按场景包围盒拟合 — 方向光没有位置，只有方向；要把它变成可渲染的
 *   视锥，必须先决定"覆盖多大范围"。用场景包围盒是单张阴影贴图能做的最好选择：
 *   保证不漏，代价是远处精度被近处稀释。级联阴影正是为解决这一点而存在。
 *
 * 技术特性:
 *   - 深度专用 RenderPass，无颜色附件
 *   - 复用 depth_only 着色器，Masked 材质在阴影中同样镂空
 *   - 单面/双面两条管线，与前向 Pass 的剔除选择保持一致
 *   - 正面剔除可选：绘制背面能把自遮挡推到物体内部
 *
 * 依赖关系:
 *   内部: Renderer/RenderPass/IRenderPass.h
 *
 * 注意事项:
 *   本 Pass 必须最先执行 (Order 最小) —— 前向 Pass 要采样它的产物
 *
 ******************************************************************************/

#pragma once

#include "Renderer/RenderPass/IRenderPass.h"

namespace Limx
{

// ============================================================================
// FShadowPass — 方向光阴影贴图 Pass
// ============================================================================

class FShadowPass final : public IRenderPass
{
public:
    /// 阴影贴图边长 — 正方形
    ///
    /// 2048 是质量与显存的折中: 单张 D32 深度图占 16 MiB。再高一档
    /// (4096) 要 64 MiB，而在没有级联的情况下，把整个场景塞进一张图时
    /// 精度的瓶颈是投影体积而非分辨率 —— 加分辨率的收益远小于加级联。
    static constexpr UInt32 kShadowMapSize = 2048;

    FShadowPass()           = default;
    ~FShadowPass() override = default;

    // ====================================================================
    // IRenderPass 接口实现
    // ====================================================================

    LIMX_NODISCARD const AnsiChar* GetName() const override
    {
        return "ShadowPass";
    }

    /// 必须早于 DepthPrePass(100) 与 ForwardPass —— 后者要采样本 Pass 的产物
    LIMX_NODISCARD UInt32 GetOrder() const override
    {
        return 50;
    }

    ERHIResult Setup(const FPassSetupDesc& desc) override;

    void Execute(IRHICommandBuffer*        commandBuffer,
                 const FRenderPassContext& context) override;

    ERHIResult OnResize(IRHIDevice*           device,
                        FRHISwapchainHandle   swapchain,
                        FRHIExtent2D          newExtent,
                        UInt32                swapchainImageCount,
                        FRHITextureHandle     newSharedDepth,
                        FRHITextureViewHandle newSharedDepthView) override;

    /// 释放与交换链尺寸相关的资源
    ///
    /// 阴影贴图是光源空间的固定分辨率资源, 与交换链无关, 因此这里无事可做。
    void ReleaseSwapchainResources(IRHIDevice* device) override;

    void Shutdown(IRHIDevice* device) override;

    // ====================================================================
    // 阴影贴图访问
    // ====================================================================

    /// 阴影贴图的纹理视图 — 供前向 Pass 写入 set 2 binding 1
    LIMX_NODISCARD FRHITextureViewHandle GetShadowMapView() const
    {
        return m_ShadowMapView;
    }

    /// 阴影贴图采样器 — 启用了深度比较，采样即得到 0/1 的遮挡结果
    LIMX_NODISCARD FRHISamplerHandle GetShadowSampler() const
    {
        return m_ShadowSampler;
    }

    /// 本帧使用的光源视图投影矩阵
    LIMX_NODISCARD const FMatrix& GetShadowViewProj() const
    {
        return m_ShadowViewProj;
    }

    // ====================================================================
    // 光源与场景范围
    // ====================================================================

    /// 设置方向光方向与场景包围盒 — 由渲染器每帧在 Execute 之前调用
    ///
    /// 包围盒决定正交投影体积。给得过大会浪费阴影贴图精度，
    /// 过小则场景边缘直接落在阴影贴图之外 —— 那里会被判为"完全不在阴影中"，
    /// 表现为远处物体突然失去阴影。
    void SetLightAndBounds(const FVector3& lightDirection,
                           const FBoundingBox& sceneBounds);

    /// 是否已具备可用的光源信息
    LIMX_NODISCARD bool HasValidLight() const { return m_HasValidLight; }

    /// 每帧的光源矩阵 UBO 与描述符集 (与 set 0 布局兼容)
    ///
    /// 阴影 Pass 复用 depth_only.vert，后者从 set 0 binding 0 读取
    /// view/proj。因此这里需要一份自己的 set 0 描述符集，内容是光源矩阵
    /// 而非相机矩阵。
    ERHIResult CreateLightUniforms(IRHIDevice* device,
                                   FRHIDescSetLayoutHandle viewProjLayout,
                                   FRHITextureViewHandle fillerTextureView,
                                   FRHISamplerHandle fillerSampler,
                                   UInt32 frameCount);

    /// 把本帧的光源矩阵写入对应帧的 UBO
    void UpdateLightUniform(IRHIDevice* device, UInt32 frameIndex);

private:
    ERHIResult CreateShadowMap(IRHIDevice* device);
    ERHIResult CreateShadowRenderPass(IRHIDevice* device);
    ERHIResult CreateFramebuffer(IRHIDevice* device);
    ERHIResult CreateShaders(IRHIDevice* device);
    ERHIResult CreateShadowPipeline(IRHIDevice* device, bool isDoubleSided,
                                    FRHIGraphicsPipelineHandle& outPipeline);

    /// 按剔除模式取管线
    LIMX_NODISCARD FRHIGraphicsPipelineHandle SelectPipeline(
        bool isDoubleSided) const
    {
        return m_Pipelines[isDoubleSided ? 1u : 0u];
    }

    // ====================================================================
    // 成员
    // ====================================================================

    FRHITextureHandle          m_ShadowMap;
    FRHITextureViewHandle      m_ShadowMapView;
    FRHISamplerHandle          m_ShadowSampler;

    FRHIRenderPassHandle       m_RenderPass;
    FRHIFramebufferHandle      m_Framebuffer;

    FRHIShaderHandle           m_VertShader;
    FRHIShaderHandle           m_FragShader;

    static constexpr SizeType  kPipelineVariantCount = 2;
    FRHIGraphicsPipelineHandle m_Pipelines[kPipelineVariantCount];

    FRHIPipelineLayoutHandle   m_PipelineLayout;

    /// 每帧的光源矩阵 UBO 与描述符集
    TArray<FRHIBufferHandle>        m_LightUniformBuffers;
    TArray<FRHIDescriptorSetHandle> m_LightDescriptorSets;

    FMatrix  m_ShadowViewProj;
    FVector3 m_LightDirection = FVector3(0.0f, -1.0f, 0.0f);
    bool     m_HasValidLight  = false;
};

} // namespace Limx
