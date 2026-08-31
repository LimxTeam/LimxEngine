// ============================================================
// 文件名称：FForwardPass.h
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：Pass 封装 — 将现有 FRenderer::RecordCommands() 中的前向渲染
//          逻辑提取为独立 Pass 对象，实现渲染逻辑与帧管理的彻底解耦。
//          FForwardPass 管理自己的 RenderPass/Framebuffer/Pipeline，
//          通过 FRenderPassContext 接收场景数据。
// 功能描述：前向渲染 Pass — 负责最终颜色输出的渲染通道。
//          使用 triangle.vert/frag 着色器，颜色附件写入交换链图像，
//          深度测试使用 Equal 比较 (配合 FDepthPrePass 的 Early-Z 优化),
//          禁止深度写入以避免干扰深度预 Pass 写入的精确深度值。
// 技术特性：每个交换链图像一个 Framebuffer (颜色 + 共享深度);
//          管线布局和 set 0 描述符集由 FRenderPassContext 传入;
//          Execute 等价于现有 FRenderer::RecordCommands() 逻辑;
//          支持交换链重建时的 OnResize 资源重建。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                    │ 描述                           │
// │──────────────────────────│───────────────────────────────│
// │ GetName()                │ 返回 "ForwardPass"             │
// │ GetOrder()               │ 返回 200 (在 DepthPrePass 之后) │
// │ Setup()                  │ 创建 RenderPass/Framebuffer/管线 │
// │ Execute()                │ 录制前向渲染命令                 │
// │ OnResize()               │ 重建 Framebuffer                │
// │ Shutdown()               │ 释放所有 GPU 资源               │
//
// ── 结构体字段表 ──────────────────────────────────────────────
// │ 字段名                    │ 类型                           │ 描述          │
// │──────────────────────────│──────────────────────────────│──────────────│
// │ m_RenderPass             │ FRHIRenderPassHandle          │ 颜色+深度渲染通道│
// │ m_Framebuffers           │ TArray<FRHIFramebufferHandle> │ 每图像一个帧缓冲 │
// │ m_VertShader             │ FRHIShaderHandle              │ 顶点着色器      │
// │ m_FragShader             │ FRHIShaderHandle              │ 片段着色器      │
// │ m_GraphicsPipeline       │ FRHIGraphicsPipelineHandle    │ 图形管线        │
// │ m_PipelineLayout         │ FRHIPipelineLayoutHandle      │ 管线布局 (外部)  │
// │ m_SwapchainFormat        │ EPixelFormat                  │ 颜色附件格式    │
// │ m_SwapchainExtent        │ FRHIExtent2D                  │ 当前交换链尺寸   │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M0.5 Pass 抽象层)     │
// ============================================================

#pragma once

#include "Renderer/RenderPass/IRenderPass.h"
#include "Renderer/Recording/FParallelRecorder.h"

namespace Limx
{

// ============================================================================
// FForwardPass — 前向渲染通道 (颜色输出 + 共享深度 Equal 测试)
// ============================================================================

class FForwardPass final : public IRenderPass
{
public:
    FForwardPass() = default;
    ~FForwardPass() override = default;

    // ====================================================================
    // IRenderPass 接口实现
    // ====================================================================

    LIMX_NODISCARD const AnsiChar* GetName() const override
    {
        return "ForwardPass";
    }

    LIMX_NODISCARD UInt32 GetOrder() const override
    {
        return 200;
    }

    ERHIResult Setup(const FPassSetupDesc& desc) override;

    /// 设置并行录制器 (可空 = 走内联路径)
    ///
    /// 两条路径共用同一份绘制代码, 因此可以逐像素比对。
    void SetRecorder(FParallelRecorder* recorder) { m_Recorder = recorder; }

    void Execute(IRHICommandBuffer*       commandBuffer,
                 const FRenderPassContext& context) override;

    ERHIResult OnResize(const FPassResizeDesc& desc) override;

    void ReleaseSwapchainResources(IRHIDevice* device) override;

    void Shutdown(IRHIDevice* device) override;

private:
    /// 录制公共状态 — 视口、裁剪、set 0、set 2
    ///
    /// 次级命令缓冲区不继承主缓冲区的任何绑定状态, 因此每段都要设一遍。
    void RecordCommonState(IRHICommandBuffer*        commandBuffer,
                           const FRenderPassContext& context);

    /// 录制不透明批次的 [begin, end) 区间
    void RecordOpaqueRange(IRHICommandBuffer*        commandBuffer,
                           const FRenderPassContext& context,
                           SizeType                  begin,
                           SizeType                  end);

    /// 录制半透明批次 (全部, 顺序敏感)
    void RecordTranslucent(IRHICommandBuffer*        commandBuffer,
                           const FRenderPassContext& context);

    /// 并行录制器 — 空则走内联路径
    FParallelRecorder* m_Recorder = nullptr;


    // ====================================================================
    // 内部构建方法
    // ====================================================================

    /// 创建颜色+深度渲染通道
    ERHIResult CreateRenderPass(IRHIDevice* device, EPixelFormat swapchainFormat);

    /// 创建每个交换链图像的 Framebuffer (颜色=交换链视图, 深度=共享深度视图)
    ERHIResult CreateFramebuffers(IRHIDevice*           device,
                                   FRHISwapchainHandle   swapchain,
                                   FRHIExtent2D          extent,
                                   UInt32                imageCount,
                                   FRHITextureViewHandle sharedDepthView);

    /// 销毁所有 Framebuffer
    void DestroyFramebuffers(IRHIDevice* device);

    /// 创建着色器模块 (triangle.vert / triangle.frag)
    ERHIResult CreateShaders(IRHIDevice* device);

    /// 创建图形管线 (DepthCompareOp::Equal, 禁止深度写入)
    /// 创建一条图形管线
    ///
    /// 不透明与半透明只在深度与混合状态上不同，其余（着色器、顶点输入、
    /// 光栅化、渲染通道）完全一致，因此共用同一份描述而非各写一遍 ——
    /// 两份描述一旦漂移，症状是"半透明物体的顶点布局对不上"这类极难定位的错。
    ///
    /// @param isTranslucent 半透明变体：启用 Alpha 混合、深度比较放宽为
    ///                      LessOrEqual、仍然禁止深度写入
    /// @param isDoubleSided 双面变体：关闭背面剔除
    ERHIResult CreateGraphicsPipeline(IRHIDevice* device,
                                      bool isTranslucent,
                                      bool isDoubleSided,
                                      FRHIGraphicsPipelineHandle& outPipeline);

    /// 按混合与剔除取对应管线
    LIMX_NODISCARD FRHIGraphicsPipelineHandle SelectPipeline(
        bool isTranslucent, bool isDoubleSided) const
    {
        const SizeType index = (isTranslucent ? 2u : 0u) +
                               (isDoubleSided ? 1u : 0u);
        return m_Pipelines[index];
    }

    /// 销毁图形管线和着色器
    void DestroyPipelineResources(IRHIDevice* device);

    // ====================================================================
    // 成员
    // ====================================================================

    /// 颜色 + 深度渲染通道
    FRHIRenderPassHandle              m_RenderPass;

    /// 每个交换链图像一个 Framebuffer [颜色, 深度]
    TArray<FRHIFramebufferHandle>     m_Framebuffers;

    /// 顶点/片段着色器模块 (triangle.vert / triangle.frag)
    FRHIShaderHandle                  m_VertShader;
    FRHIShaderHandle                  m_FragShader;

    /// 图形管线 (Equal 深度测试, 无深度写入)
    /// 管线排列 — 下标 = (半透明 ? 2 : 0) + (双面 ? 1 : 0)
    ///
    /// 混合与剔除是两个正交的光栅化开关, 组合出四条管线。用下标而非四个
    /// 具名成员, 是为了让 SelectPipeline 的映射只有一处、且一眼可验。
    static constexpr SizeType kPipelineVariantCount = 4;
    FRHIGraphicsPipelineHandle        m_Pipelines[kPipelineVariantCount];

    /// 管线布局 (由外部 FPassSetupDesc 传入, 不拥有)
    FRHIPipelineLayoutHandle          m_PipelineLayout;

    /// 共享 HDR 颜色目标的视图 — 本 Pass 的渲染目标 (非拥有)
    FRHITextureViewHandle             m_ColorTargetView;

    /// 交换链格式缓存 (OnResize 时重建 RenderPass 需要)
    EPixelFormat                      m_SwapchainFormat = EPixelFormat::Unknown;

    /// 当前交换链尺寸缓存
    FRHIExtent2D                      m_SwapchainExtent = {};
};

} // namespace Limx
