/*******************************************************************************
 * 文件: FPostProcessPass.h
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   后处理 Pass — 采样 HDR 目标，应用曝光与 ACES 色调映射，输出到交换链
 *
 * 设计哲学:
 *   色调映射从 PBR 着色器中独立出来，理由不是复用而是正确性 —— 它按定义
 *   是对**最终图像**的操作，而非逐材质的操作。写在 PBR 里还有两个直接后果：
 *   曝光这类全局参数无处安放；Bloom、TAA 之类需要线性 HDR 输入的效果
 *   拿到的将是已被压缩过的颜色，亮部信息已经丢失。
 *
 *   本 Pass 是后续一切屏幕空间效果的挂载点：它已经建立了"读一张全屏纹理、
 *   写另一张"的骨架，加 Bloom 只是在中间多几趟。
 *
 * 技术特性:
 *   - 全屏三角形，无顶点缓冲区（顶点由 gl_VertexIndex 算出）
 *   - 独立的描述符集布局与管线布局（只需一张纹理 + 曝光 Push Constant）
 *   - 每交换链图像一个 Framebuffer，最终布局 PresentSrc
 *
 * 依赖关系:
 *   内部: Renderer/RenderPass/IRenderPass.h
 *
 * 注意事项:
 *   必须最后执行 (Order 最大) —— 它消费前面所有 Pass 的产物
 *
 ******************************************************************************/

#pragma once

#include "Renderer/RenderPass/IRenderPass.h"

namespace Limx
{

// ============================================================================
// FPostProcessPass — HDR 解析与色调映射
// ============================================================================

class FPostProcessPass final : public IRenderPass
{
public:
    FPostProcessPass()           = default;
    ~FPostProcessPass() override = default;

    LIMX_NODISCARD const AnsiChar* GetName() const override
    {
        return "PostProcessPass";
    }

    /// 最后执行 —— 消费前面所有 Pass 的产物
    LIMX_NODISCARD UInt32 GetOrder() const override
    {
        return 900;
    }

    ERHIResult Setup(const FPassSetupDesc& desc) override;

    void Execute(IRHICommandBuffer*        commandBuffer,
                 const FRenderPassContext& context) override;

    ERHIResult OnResize(const FPassResizeDesc& desc) override;

    void ReleaseSwapchainResources(IRHIDevice* device) override;

    void Shutdown(IRHIDevice* device) override;

    // ====================================================================
    // 曝光
    // ====================================================================

    /// 设置线性曝光倍数
    ///
    /// 1.0 表示不调整。这是一个**线性**倍数而非 EV 档位 —— 档位换算
    /// (2^EV) 属于相机模型的范畴，放在这一层只会让"这个数字到底该填多少"
    /// 变得更难判断。
    void SetExposure(Float32 exposure) { m_Exposure = exposure; }

    LIMX_NODISCARD Float32 GetExposure() const { return m_Exposure; }

private:
    ERHIResult CreateRenderPass(IRHIDevice* device,
                                EPixelFormat swapchainFormat);
    ERHIResult CreateFramebuffers(IRHIDevice* device,
                                  FRHISwapchainHandle swapchain,
                                  FRHIExtent2D extent,
                                  UInt32 imageCount);
    void       DestroyFramebuffers(IRHIDevice* device);
    ERHIResult CreateDescriptorResources(IRHIDevice* device);
    ERHIResult CreateShaders(IRHIDevice* device);
    ERHIResult CreatePipeline(IRHIDevice* device);

    /// 把 HDR 视图写入描述符集 —— 尺寸变化后视图换了新的, 需要重写
    void UpdateSourceDescriptor(IRHIDevice* device,
                                FRHITextureViewHandle hdrView);

    // ====================================================================
    // 成员
    // ====================================================================

    FRHIRenderPassHandle             m_RenderPass;
    TArray<FRHIFramebufferHandle>    m_Framebuffers;

    FRHIShaderHandle                 m_VertShader;
    FRHIShaderHandle                 m_FragShader;

    FRHIDescSetLayoutHandle          m_DescSetLayout;
    FRHIDescriptorSetHandle          m_DescriptorSet;
    FRHIPipelineLayoutHandle         m_PipelineLayout;
    FRHIGraphicsPipelineHandle       m_Pipeline;

    /// HDR 采样器 — 线性过滤、Clamp, 全屏 1:1 采样其实用不到过滤,
    /// 但将来做降采样 (Bloom) 时需要
    FRHISamplerHandle                m_Sampler;

    EPixelFormat                     m_SwapchainFormat = EPixelFormat::Unknown;
    FRHIExtent2D                     m_SwapchainExtent = {};

    Float32                          m_Exposure = 1.0f;
};

} // namespace Limx
