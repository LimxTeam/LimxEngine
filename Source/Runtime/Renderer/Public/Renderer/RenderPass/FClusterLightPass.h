/*******************************************************************************
 * 文件: FClusterLightPass.h
 * 创建时间: 2026-08-31
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   分簇光照的剔除通道 — 每帧算出簇包围盒并把光源分配到簇
 *
 * 设计哲学:
 *   引擎里第一个**每帧执行**的计算通道。此前四个计算着色器 (IBL 的那几个)
 *     全是一次性预计算, 走 BeginSingleTimeCommands/EndSingleTimeCommands ——
 *     而后者内部有一次全设备 WaitIdle。照抄那条路径的话每帧都会停一次管线。
 *     这里把 Dispatch 直接录进 FPassManager 传下来的主命令缓冲区: 顶层不在
 *     渲染通道内, 可以直接分派。
 *
 *   每帧无条件重算簇包围盒。理论上它只在投影矩阵变化时才需要重算 (相机的
 *     旋转与平移都不影响视空间的簇边界), 但"投影是否变了"是一个要跟踪的
 *     状态, 而跟踪错了的表现是包围盒过期 —— 剔除依据与实际视锥不符, 某些
 *     方向的光被整片漏掉, 且只在改变 FOV 之后才出现。实测重算不到 0.01 ms,
 *     不值得为它引入一个状态。
 *
 *   网格尺寸固定 (见 FClusterGrid.h), 因此本通道的 OnResize 什么都不做 ——
 *     缓冲区尺寸与分辨率无关, 描述符永远不必重写。这正是选固定网格的理由:
 *     Pass 的 OnResize 拿不到描述符句柄, 而"描述符仍指向已销毁的缓冲区"
 *     是一个只在交换链重建时触发、平时完全看不见的坑。
 *
 * 技术特性:
 *   - 两次分派: 先算包围盒, 再分配光源, 中间一道缓冲区屏障
 *   - 索引表用全局原子计数器碰撞式分配, 容量超限**显式报告**而非静默截断
 *   - 每并行帧一套缓冲区 — 共用会让帧 N+1 的计算覆写帧 N 正在读的数据
 *
 * 依赖关系:
 *   内部: RenderCore/Lighting/FClusterGrid.h, RHI
 *
 ******************************************************************************/

#pragma once

#include "Renderer/RendererMinimal.h"
#include "Renderer/RenderPass/IRenderPass.h"

#include "RenderCore/Lighting/FClusterGrid.h"

namespace Limx
{

class FClusterLightPass final : public IRenderPass
{
public:
    FClusterLightPass()           = default;
    ~FClusterLightPass() override = default;

    LIMX_NODISCARD const AnsiChar* GetName() const override
    {
        return "ClusterLightPass";
    }

    /// 120 — 深度预通道 (100) 之后, 天空 (150) 之前
    ///
    /// 本通道不读深度, 所以理论上放在任何位置都行。放在这里是因为它的产出
    /// 只被前向通道 (200) 消费, 而离消费方越近, 那道缓冲区屏障覆盖的命令
    /// 就越少。
    LIMX_NODISCARD UInt32 GetOrder() const override
    {
        return 120;
    }

    ERHIResult Setup(const FPassSetupDesc& desc) override;

    void Execute(IRHICommandBuffer*        commandBuffer,
                 const FRenderPassContext& context) override;

    /// 网格尺寸固定, 与分辨率无关 —— 这里什么都不用做
    ERHIResult OnResize(const FPassResizeDesc& desc) override;

    void ReleaseSwapchainResources(IRHIDevice* device) override;

    void Shutdown(IRHIDevice* device) override;

    // ========================================================================
    // 给前向通道与自检用的访问器
    // ========================================================================

    /// 每簇的 (起点, 数量) —— uvec2 数组, 长度 kClusterCount
    LIMX_NODISCARD FRHIBufferHandle GetClusterGridBuffer(
        UInt32 frameIndex) const;

    /// 全局光源索引表 —— uint 数组, 容量 kClusterLightIndexCapacity
    LIMX_NODISCARD FRHIBufferHandle GetLightIndexBuffer(
        UInt32 frameIndex) const;

    /// 簇包围盒 (回读校验用) —— vec4 数组, 每簇两个
    LIMX_NODISCARD FRHIBufferHandle GetClusterBoundsBuffer(
        UInt32 frameIndex) const;

    /// 上一次回读到的分配条目数 (诊断用)
    LIMX_NODISCARD UInt32 GetAllocatedIndexCount() const
    {
        return m_LastAllocated;
    }

    /// 索引表是否曾经溢出
    ///
    /// 溢出意味着有光源被丢弃 —— 画面上表现为某些区域的光少了几盏, 而那
    /// 看起来像衰减参数的问题。必须让它可见。
    LIMX_NODISCARD bool HasOverflowed() const
    {
        return m_HasOverflowed;
    }

    /// 设置相机参数 —— 由 FRenderer 在每帧录制之前调用
    ///
    /// 传的必须是**未抖动的**投影矩阵。抖动每帧改变亚像素偏移, 用它算出的
    /// 簇边界会逐帧漂移 —— 那本身无害, 但会让"分簇结果与暴力法一致"这条
    /// 验收判据变成逐帧不同, 无法比对。
    void SetCameraParams(const FMatrix& view,
                         const FMatrix& projectionNoJitter,
                         Float32        nearPlane,
                         Float32        farPlane);

    /// 设置本帧的活跃光源数与光源缓冲区
    void SetLightSource(FRHIBufferHandle lightBuffer, UInt32 lightCount);

    /// 是否分派。关闭时 Execute 直接返回 —— 簇表没人读, 跑了是白付。
    void SetEnabled(bool enabled) { m_Enabled = enabled; }

private:
    ERHIResult CreateBuffers(IRHIDevice* device, UInt32 frameCount);
    ERHIResult CreateDescriptors(IRHIDevice* device, UInt32 frameCount);
    ERHIResult CreatePipelines(IRHIDevice* device);

    /// 回读上一帧的计数器 —— 溢出诊断
    void ResolveCounter(IRHIDevice* device, UInt32 frameIndex);

    IRHIDevice*                 m_Device = nullptr;

    UInt32                      m_FrameCount = 0;

    // ---- 每并行帧一套 ----
    TArray<FRHIBufferHandle>    m_ClusterBounds;
    TArray<FRHIBufferHandle>    m_ClusterGrid;
    TArray<FRHIBufferHandle>    m_LightIndices;
    TArray<FRHIBufferHandle>    m_Counters;
    TArray<FRHIBufferHandle>    m_CounterReadbacks;
    TArray<FRHIDescriptorSetHandle> m_BuildSets;
    TArray<FRHIDescriptorSetHandle> m_CullSets;

    /// 8 字节的全零源 —— 每帧拷进计数器做清零
    ///
    /// RHI 没有 FillBuffer, 而计数器必须每帧归零。用一个常驻的全零缓冲区
    /// 做拷贝源比给 RHI 加一条命令便宜, 也不会与正在改 RHI 的工作冲突。
    FRHIBufferHandle            m_ZeroSource;

    FRHIDescSetLayoutHandle     m_BuildSetLayout;
    FRHIDescSetLayoutHandle     m_CullSetLayout;
    FRHIPipelineLayoutHandle    m_BuildPipelineLayout;
    FRHIPipelineLayoutHandle    m_CullPipelineLayout;
    FRHIComputePipelineHandle   m_BuildPipeline;
    FRHIComputePipelineHandle   m_CullPipeline;

    /// 着色器模块。管线建好之后它们本可以立刻销毁 (Vulkan 允许), 但留到
    /// Shutdown 一起放更不容易漏 —— 创建与销毁在同一个生命周期层级上。
    FRHIShaderHandle            m_BuildShader;
    FRHIShaderHandle            m_CullShader;

    // ---- 每帧的输入 ----
    FMatrix                     m_View               = FMatrix::kIdentity;
    FMatrix                     m_InverseProjection  = FMatrix::kIdentity;
    Float32                     m_NearPlane          = 0.1f;
    Float32                     m_FarPlane           = 100.0f;
    FRHIBufferHandle            m_LightBuffer;
    UInt32                      m_LightCount         = 0;
    bool                        m_Enabled            = true;

    // ---- 诊断 ----
    UInt32                      m_LastAllocated      = 0;
    bool                        m_HasOverflowed      = false;

    /// 溢出只报一次 —— 它每帧都会重复, 刷满日志之后反而没人看
    bool                        m_HasReportedOverflow = false;
};

} // namespace Limx
