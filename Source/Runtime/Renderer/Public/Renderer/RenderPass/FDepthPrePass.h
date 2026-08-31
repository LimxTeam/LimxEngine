// ============================================================
// 文件名称：FDepthPrePass.h
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：Early-Z 优化 — 在主渲染前单独走一趟深度渲染，
//          将场景深度信息完整写入共享深度缓冲区，使后续 FForwardPass
//          使用 DepthCompareOp::Equal 跳过被遮挡的片段，从而避免
//          大量昂贵的片段着色器无效执行，提升 GPU 利用率。
// 功能描述：深度预 Pass (Early-Z) — 使用 depth_only.vert/frag 着色器，
//          创建 depth-only RenderPass (无颜色附件)，渲染场景所有物体
//          仅写入深度缓冲区，不做任何颜色运算。
//          Execute 结束后插入管线屏障，确保深度写入对后续 FForwardPass 可见。
// 技术特性：单一 Framebuffer (depth-only，共享深度纹理视图);
//          depth-only RenderPass: 无颜色附件, 深度 LoadOp=Clear/StoreOp=Store;
//          Pipeline: DepthCompareOp=Less, DepthWrite=true, 无颜色混合状态;
//          Execute 尾部插入 LateFragmentTests→EarlyFragmentTests 屏障。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                    │ 描述                           │
// │──────────────────────────│───────────────────────────────│
// │ GetName()                │ 返回 "DepthPrePass"            │
// │ GetOrder()               │ 返回 100 (在 ForwardPass 之前)  │
// │ Setup()                  │ 创建深度 RenderPass/Framebuffer/管线 │
// │ Execute()                │ 录制深度渲染命令 + 插入屏障       │
// │ OnResize()               │ 重建共享深度 Framebuffer          │
// │ Shutdown()               │ 释放所有 GPU 资源               │
//
// ── 结构体字段表 ──────────────────────────────────────────────
// │ 字段名                    │ 类型                           │ 描述          │
// │──────────────────────────│──────────────────────────────│──────────────│
// │ m_DepthRenderPass        │ FRHIRenderPassHandle          │ 深度专用渲染通道 │
// │ m_DepthFramebuffer       │ FRHIFramebufferHandle         │ 深度专用帧缓冲   │
// │ m_VertShader             │ FRHIShaderHandle              │ depth_only.vert │
// │ m_FragShader             │ FRHIShaderHandle              │ depth_only.frag │
// │ m_DepthPipeline          │ FRHIGraphicsPipelineHandle    │ 深度专用管线    │
// │ m_PipelineLayout         │ FRHIPipelineLayoutHandle      │ 管线布局 (外部)  │
// │ m_SharedDepthTexture     │ FRHITextureHandle             │ 共享深度纹理    │
// │ m_SwapchainExtent        │ FRHIExtent2D                  │ 当前交换链尺寸   │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M0.5 Early-Z 深度预 Pass) │
// ============================================================

#pragma once

#include "Renderer/RenderPass/IRenderPass.h"
#include "Renderer/Recording/FParallelRecorder.h"

namespace Limx
{

// ============================================================================
// FDepthPrePass — 深度预 Pass (Early-Z，仅写深度，无颜色输出)
// ============================================================================

class FDepthPrePass final : public IRenderPass
{
public:
    FDepthPrePass() = default;
    ~FDepthPrePass() override = default;

    // ====================================================================
    // IRenderPass 接口实现
    // ====================================================================

    LIMX_NODISCARD const AnsiChar* GetName() const override
    {
        return "DepthPrePass";
    }

    LIMX_NODISCARD UInt32 GetOrder() const override
    {
        return 100;
    }

    ERHIResult Setup(const FPassSetupDesc& desc) override;

    /// 设置并行录制器 (可空 = 走内联路径)
    void SetRecorder(FParallelRecorder* recorder) { m_Recorder = recorder; }

    void Execute(IRHICommandBuffer*       commandBuffer,
                 const FRenderPassContext& context) override;

    ERHIResult OnResize(const FPassResizeDesc& desc) override;

    void ReleaseSwapchainResources(IRHIDevice* device) override;

    void Shutdown(IRHIDevice* device) override;

private:
    /// 录制公共状态 — 视口、裁剪、set 0
    void RecordCommonState(IRHICommandBuffer*        commandBuffer,
                           const FRenderPassContext& context);

    /// 录制 [begin, end) 区间的深度绘制
    void RecordRange(IRHICommandBuffer*        commandBuffer,
                     const FRenderPassContext& context,
                     SizeType                  begin,
                     SizeType                  end);

    /// 并行录制器 — 空则走内联路径
    FParallelRecorder* m_Recorder = nullptr;


    // ====================================================================
    // 内部构建方法
    // ====================================================================

    /// 创建 depth-only 渲染通道 (无颜色附件)
    ERHIResult CreateDepthRenderPass(IRHIDevice* device);

    /// 创建 depth-only Framebuffer (仅含共享深度视图)
    ERHIResult CreateDepthFramebuffer(IRHIDevice*           device,
                                       FRHIExtent2D          extent,
                                       FRHITextureViewHandle sharedDepthView);

    /// 销毁 Framebuffer
    void DestroyDepthFramebuffer(IRHIDevice* device);

    /// 创建着色器模块 (depth_only.vert / depth_only.frag)
    ERHIResult CreateShaders(IRHIDevice* device);

    /// 创建 depth-only 图形管线 (DepthCompareOp=Less, DepthWrite=true, 无颜色混合)
    /// 创建一条 depth-only 管线
    ///
    /// @param isDoubleSided 双面变体：关闭背面剔除
    ///
    /// 剔除模式必须与 FForwardPass 逐物体一致。前向 Pass 用
    /// DepthCompareOp=Equal，本 Pass 剔掉的那一面在深度缓冲区里没有值，
    /// 前向再去画就整片失配 —— 表现为双面材质只剩一半，且那一半还闪烁。
    ERHIResult CreateDepthPipeline(IRHIDevice* device, bool isDoubleSided,
                                   FRHIGraphicsPipelineHandle& outPipeline);

    /// 按剔除模式取管线
    LIMX_NODISCARD FRHIGraphicsPipelineHandle SelectPipeline(
        bool isDoubleSided) const
    {
        return m_DepthPipelines[isDoubleSided ? 1u : 0u];
    }

    /// 销毁管线和着色器
    void DestroyPipelineResources(IRHIDevice* device);

    // ====================================================================
    // 成员
    // ====================================================================

    /// depth-only 渲染通道 (仅深度附件)
    FRHIRenderPassHandle       m_DepthRenderPass;

    /// depth-only Framebuffer (共享深度视图)
    FRHIFramebufferHandle      m_DepthFramebuffer;

    /// depth_only.vert / depth_only.frag 着色器模块
    FRHIShaderHandle           m_VertShader;
    FRHIShaderHandle           m_FragShader;

    /// depth-only 图形管线
    /// 管线排列 — 下标 0 = 单面, 1 = 双面
    static constexpr SizeType kPipelineVariantCount = 2;
    FRHIGraphicsPipelineHandle m_DepthPipelines[kPipelineVariantCount];

    /// 管线布局 (由外部 FPassSetupDesc 传入, 不拥有)
    FRHIPipelineLayoutHandle   m_PipelineLayout;

    /// 共享深度纹理句柄 (Execute 中需要引用以插入管线屏障)
    FRHITextureHandle          m_SharedDepthTexture;

    /// 当前交换链尺寸缓存
    FRHIExtent2D               m_SwapchainExtent = {};
};

} // namespace Limx
