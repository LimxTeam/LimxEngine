/*******************************************************************************
 * 文件: FGpuCullPass.h
 * 创建时间: 2026-08-31
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   GPU 驱动绘制的剔除通道 — 逐物体数据上传 + 计算着色器剔除 + 间接命令
 *
 * 设计哲学:
 *   Day 1 的逐 Pass 计时给出的结论一直没变: **整条渲染是 CPU 受限的**。
 *     压力场景实测 CPU 3.41 ms 对 GPU 0.275 ms, 而其中 2.57 ms 花在命令录制
 *     上 —— 内容是每个物体一次句柄比较、一次 68 字节的 push constant 上传、
 *     一次 DrawIndexed。
 *
 *   本通道把这一整段搬到显卡: CPU 每帧只写一次逐物体数据, 计算着色器做视锥
 *     剔除并写出间接命令, 图形通道按"同一对顶点/索引缓冲区"分组, 每组一次
 *     DrawIndexedIndirect。
 *
 *   分组在 CPU 上算而不是 GPU — 分组的依据是缓冲区**句柄**, 而句柄是 CPU
 *     侧的概念: 绑定顶点缓冲区这件事本来就只能在 CPU 上做。GPU 能省掉的是
 *     逐物体的那部分, 不是逐组的那部分。压力场景 576 个物体分成十几组。
 *
 *   命令不压实 — 计算着色器为**每个**物体都写一条命令, 不可见的
 *     instanceCount 写 0。压实 (只写可见的) 会把不同组的命令混在一起, 而
 *     分组要知道自己那段从第几条开始。零实例命令在命令处理器里就被跳过,
 *     实测量不出开销。
 *
 *   输入取**剔除前**的列表 — 用 FRenderPassContext::ShadowCasterObjects,
 *     那是"未经相机视锥剔除的不透明与蒙版批次", 排序规则与主列表相同。
 *     用剔除后的列表 (RenderObjects) 的话, GPU 剔除永远剔不掉任何东西 ——
 *     一个什么都不做的剔除实现会得到完全正确的画面, 判据也就无从判定。
 *
 * 技术特性:
 *   - Order 90: 深度预通道 (100) 之前, 阴影图集 (60) 之后
 *   - 每并行帧一套物体/命令/计数器缓冲区
 *   - 包围球剔除, 外接而非内切 (保证是 CPU 剔除结果的超集)
 *   - firstInstance 携带物体下标, 顶点着色器靠 gl_InstanceIndex 定位
 *
 * 依赖关系:
 *   内部: Renderer/RenderPass/IRenderPass.h, RenderCore/Culling/FGpuDraw.h
 *
 * 注意事项:
 *   设备不支持 drawIndirectFirstInstance 时本通道自动禁用 —— 那时
 *   firstInstance 恒为 0, 整个场景会挤在 0 号物体的变换上。
 *
 ******************************************************************************/

#pragma once

#include "Renderer/RenderPass/IRenderPass.h"

#include "RenderCore/Culling/FGpuDraw.h"

namespace Limx
{

// ============================================================================
// FDrawGroup — 一段共享顶点/索引缓冲区与管线变体的连续命令
// ============================================================================

struct FDrawGroup
{
    FRHIBufferHandle VertexBuffer;
    FRHIBufferHandle IndexBuffer;
    EIndexType       IndexType     = EIndexType::UInt32;
    bool             IsDoubleSided = false;

    /// 本组在间接命令缓冲区里的起始条目与条目数
    UInt32           FirstCommand  = 0;
    UInt32           CommandCount  = 0;
};

// ============================================================================
// FGpuCullPass — GPU 驱动绘制的剔除通道
// ============================================================================

class FGpuCullPass final : public IRenderPass
{
public:
    FGpuCullPass()           = default;
    ~FGpuCullPass() override = default;

    LIMX_NODISCARD const AnsiChar* GetName() const override
    {
        return "GpuCullPass";
    }

    /// 90 — 深度预通道 (100) 之前
    ///
    /// 必须早于任何消费间接命令的图形通道。放在 90 而不是更早, 是因为它要读
    /// 本帧的物体列表, 而那份列表在 ExecuteAll 之前就已经填好了 —— 早晚无关,
    /// 只要在消费方之前。
    LIMX_NODISCARD UInt32 GetOrder() const override
    {
        return 90;
    }

    ERHIResult Setup(const FPassSetupDesc& desc) override;

    void Execute(IRHICommandBuffer*        commandBuffer,
                 const FRenderPassContext& context) override;

    /// 物体数据与命令数量都与分辨率无关 —— 这里什么都不用做
    ERHIResult OnResize(const FPassResizeDesc& desc) override;

    void ReleaseSwapchainResources(IRHIDevice* device) override;

    void Shutdown(IRHIDevice* device) override;

    // ====================================================================
    // 给图形通道用的访问器
    // ====================================================================

    /// 本帧的间接命令缓冲区
    LIMX_NODISCARD FRHIBufferHandle GetIndirectBuffer(UInt32 frameIndex) const;

    /// set 3 的逐物体描述符集
    LIMX_NODISCARD FRHIDescriptorSetHandle GetDrawObjectSet(
        UInt32 frameIndex) const;

    /// 本帧的绘制分组 —— 图形通道照着它逐组下 DrawIndexedIndirect
    LIMX_NODISCARD const TArray<FDrawGroup>& GetGroups() const
    {
        return m_Groups;
    }

    /// 本帧上传的不透明物体数 (也是间接命令的条数)
    LIMX_NODISCARD UInt32 GetObjectCount() const { return m_ObjectCount; }

    /// 半透明批次在逐物体缓冲区里的起始下标
    ///
    /// 半透明走的是**同一个** pbr.vert, 而那个着色器只有一条路径: 从 set 3
    /// 取模型矩阵。所以它们也必须在缓冲区里有条目, 否则 gl_InstanceIndex 会
    /// 落到不透明物体的数据上 —— 玻璃会长在别人的位置上。
    ///
    /// 它们不参与 GPU 剔除: 半透明要严格由远及近绘制, 而那个顺序是 CPU 排
    /// 出来的, 间接命令的顺序无法表达"按距离"这件事。
    LIMX_NODISCARD UInt32 GetTranslucentBase() const
    {
        return m_TranslucentBase;
    }

    /// 上一次回读到的可见物体数 (诊断与自检用)
    ///
    /// 没有它的话, 一个把所有 instanceCount 都写成 1 的实现 (即完全没剔除)
    /// 在画面上与正确实现完全一样, 只是慢。
    LIMX_NODISCARD UInt32 GetVisibleCount() const { return m_LastVisible; }

    // ====================================================================
    // 开关
    // ====================================================================

    /// 是否启用 GPU 驱动路径
    ///
    /// 关闭时本通道直接返回, 图形通道退回逐物体绘制。两条路径读的是**同一份**
    /// 逐物体数据、走的是同一份着色器代码 —— 逐像素比对时比出来的差异只可能
    /// 来自剔除与命令下发。
    void SetEnabled(bool enabled) { m_Enabled = enabled; }

    LIMX_NODISCARD bool IsEnabled() const
    {
        return m_Enabled && m_IsSupported;
    }

    /// 设备是否支持这条路径 (drawIndirectFirstInstance)
    LIMX_NODISCARD bool IsSupported() const { return m_IsSupported; }

    /// 设置本帧的剔除视锥 —— 由 FRenderer 在录制之前调用
    ///
    /// 必须来自**不含抖动**的投影矩阵。抖动每帧改变亚像素偏移, 用它算出的
    /// 视锥边界会逐帧漂移 —— 漂移本身无害 (远小于一个物体), 但会让"GPU 路径
    /// 与 CPU 路径一致"这条判据变成逐帧不同, 无法比对。CPU 侧的剔除用的也是
    /// 不含抖动的矩阵。
    void SetFrustum(const FFrustum& frustum) { m_Frustum = frustum; }

    /// 逐物体绘制路径也要用同一份物体数据 —— 上传与分组在这里做一次
    ///
    /// 图形通道无论走哪条路径都读 set 3 的物体数据, 所以即便本通道被关掉,
    /// 上传也必须发生。关掉的只是剔除与间接命令。
    LIMX_NODISCARD bool HasUploadedThisFrame() const
    {
        return m_HasUploaded;
    }

private:
    ERHIResult CreateBuffers(IRHIDevice* device, UInt32 frameCount);
    ERHIResult CreateDescriptors(IRHIDevice* device, UInt32 frameCount,
                                 FRHIDescSetLayoutHandle drawObjectLayout);
    ERHIResult CreatePipeline(IRHIDevice* device);

    /// 把两份列表写进本帧的缓冲区, 同时算出不透明那部分的分组
    void UploadObjects(const TArray<FRenderObject>*  opaque,
                       const TArray<FRenderObject>*  translucent,
                       UInt32                        frameIndex);

    /// 回读上一帧的可见数
    void ResolveCounter(IRHIDevice* device, UInt32 frameIndex);

    IRHIDevice* m_Device     = nullptr;
    UInt32      m_FrameCount = 0;

    // ---- 每并行帧一套 ----
    TArray<FRHIBufferHandle>        m_ObjectBuffers;
    TArray<FRHIBufferHandle>        m_IndirectBuffers;
    TArray<FRHIBufferHandle>        m_Counters;
    TArray<FRHIBufferHandle>        m_CounterReadbacks;

    /// 计算通道自己的 set 0
    TArray<FRHIDescriptorSetHandle> m_CullSets;

    /// 图形通道的 set 3
    TArray<FRHIDescriptorSetHandle> m_DrawObjectSets;

    /// 4 字节的全零源 —— 每帧拷进计数器做清零
    FRHIBufferHandle                m_ZeroSource;

    FRHIDescSetLayoutHandle         m_CullSetLayout;
    FRHIPipelineLayoutHandle        m_CullPipelineLayout;
    FRHIComputePipelineHandle       m_CullPipeline;
    FRHIShaderHandle                m_CullShader;

    /// 本帧的分组
    TArray<FDrawGroup>              m_Groups;

    /// 本帧的剔除视锥
    FFrustum m_Frustum;

    UInt32 m_ObjectCount     = 0;
    UInt32 m_TranslucentBase = 0;
    UInt32 m_LastVisible  = 0;
    bool   m_Enabled      = false;
    bool   m_IsSupported  = false;
    bool   m_HasUploaded  = false;
};

} // namespace Limx
