/*******************************************************************************
 * 文件: FParallelRecorder.h
 * 创建时间: 2026-08-31
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   多线程命令录制 — 每个工作线程一套命令池与次级命令缓冲区
 *   把一批绘制切成若干段, 经 FTaskGraph 并行录制, 再按固定顺序执行
 *
 * 设计哲学:
 *   每线程一个命令池 — Vulkan 要求命令池由调用方外部同步。共用一个池而
 *     从多个线程分配或录制是未定义行为, 而它的表现不是立刻崩溃, 而是
 *     偶发的画面错乱与驱动崩溃, 极难定位。验证层能抓到, 所以录制路径的
 *     每一次改动都必须在开着验证层的情况下跑过。
 *
 *   执行顺序固定 — 各线程的完成先后是随机的, 但主缓冲区永远按段号顺序
 *     执行。这使多线程的输出与单线程逐像素相同, 也使"并行有没有改变
 *     画面"成为一个可以逐像素回答的问题, 而不是靠眼看。
 *
 *   一段一个命令池 — 不是"一线程一个"。这一点是被验证层纠正过来的:
 *     最初按线程分池, 理由是"第 s 段固定用第 (s % 线程数) 个线程的池,
 *     所以每个池只被一个线程碰"。这个推理错在静态绑定的是**段与池**,
 *     而不是段与线程 —— 16 线程 4 段/线程共 64 段时, 段 0/16/32/48 共用
 *     同一个池, 而调度器完全可能把它们同时派给四个不同的工作线程。
 *     验证层的原话是 "VkCommandPool is simultaneously used in current
 *     thread A and thread B"。
 *
 *     一段一个池之后, 两个段不可能共享池, 正确性不再依赖对调度行为的
 *     任何假设。池本身很便宜, 这个代价买的是"不需要推理"。
 *
 *   段数多于线程数 — 绘制批次的开销差异很大 (一个 30 万三角形的网格与
 *     一个 200 三角形的网格可能在同一批里), 段多一些才能让先做完的线程
 *     接着领下一段。
 *
 * 技术特性:
 *   - 次级命令缓冲区 (VK_COMMAND_BUFFER_LEVEL_SECONDARY)
 *   - 每帧每线程各一组, 按在飞帧数轮转
 *   - 录制墙钟耗时统计, 供基准脚本对照线程数
 *
 * 依赖关系:
 *   内部: RHI/RHI/IRHIDevice.h, RHI/RHI/RHIFactory.h,
 *          Core/Threading/FTaskGraph.h, Core/Threading/FJobExecutor.h
 *
 ******************************************************************************/

#pragma once

#include "Renderer/RendererMinimal.h"

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Containers/TArray.h"
#include "Core/Templates/TUniquePtr.h"
#include "Core/Threading/FTaskGraph.h"
#include "RHI/RHI/IRHIDevice.h"
#include "RHI/RHI/IRHICommandBuffer.h"

namespace Limx
{

// ============================================================================
// FRecorderSegment — 一段绘制的范围
// ============================================================================

struct FRecorderSegment
{
    /// 起始批次下标 (含)
    SizeType Begin = 0;

    /// 结束批次下标 (不含)
    SizeType End = 0;

    /// 本段要录制到的次级命令缓冲区
    IRHICommandBuffer* CommandBuffer = nullptr;

    /// 对应的句柄 — ExecuteInto 要用
    FRHICommandBufferHandle Handle;
};

// ============================================================================
// FRecorderBatch — 一次 RecordSegmented 产生的段范围
// ============================================================================

/// 指向录制器内部段列表的一个区间
///
/// 一帧里可以有多批 —— 阴影 Pass 的每个级联各是一个独立渲染通道, 各自
/// 录制、各自执行。批与批之间的槽位不能复用: 被 vkCmdExecuteCommands
/// 引用过的次级缓冲区, 在主缓冲区执行完之前重写是未定义行为。
struct FRecorderBatch
{
    /// 在段列表中的起始下标
    SizeType First = 0;

    /// 段数 (0 表示这一批什么都没录)
    SizeType Count = 0;

    LIMX_NODISCARD bool IsEmpty() const { return Count == 0; }
};

// ============================================================================
// FParallelRecorder
// ============================================================================

/// 并行命令录制器
///
/// 用法 (每帧):
///   recorder.BeginFrame(device, frameIndex);
///   recorder.RecordSegmented(batchCount, inheritance,
///       [&](IRHICommandBuffer* cmd, SizeType begin, SizeType end) { ... });
///   recorder.ExecuteInto(primaryCommandBuffer);
///
/// 主缓冲区上的渲染通道必须以 UseSecondaryCommandBuffers = true 开始。
class FParallelRecorder
{
public:
    /// 最多支持的录制线程数
    static constexpr UInt32 kMaxThreads = 32;

    /// 支持的最大在飞帧数
    static constexpr UInt32 kMaxFramesInFlight = 4;

    /// 段数上限 = 线程数 x 这个值
    ///
    /// 只是上限 —— 实际段数还要受 kMinBatchesPerSegment 约束, 见下。
    static constexpr UInt32 kSegmentsPerThread = 4;

    /// 每段至少要有多少个批次
    ///
    /// 段数由工作量决定而不是线程数。每段的固定开销是一次 BeginSecondary、
    /// 一次 End、以及重录一遍公共状态 (视口、裁剪、描述符集) —— 段小到
    /// 十几个批次时这些开销会压过并行收益。
    ///
    /// 实测 (60x60 网格, 918 可见批次, 三个 Pass 各切一次):
    ///
    ///     内联              15.12 ms
    ///     16 段 (每段 57)   10.50 ms   最优
    ///     32 段 (每段 29)   12.33 ms
    ///     64 段 (每段 14)   21.34 ms   比内联还慢 40%
    ///
    /// 取 48 使 918 个批次切成 19 段, 落在最优区间。这条约束同时修掉一个
    /// 更要紧的问题: 默认线程数取硬件并发数 (常见 16), 不加约束时默认
    /// 配置正好落在最差的那一端, 而这种事不会报错, 只会让人以为并行没用。
    static constexpr SizeType kMinBatchesPerSegment = 48;

    FParallelRecorder() = default;
    ~FParallelRecorder() = default;

    LIMX_NON_COPYABLE(FParallelRecorder);

    // ========================================================================
    // 生命周期
    // ========================================================================

    /// 创建每线程的命令池与次级缓冲区
    ///
    /// @param threadCount 录制线程数; 传 0 表示按硬件并发数
    ///
    /// 线程数为 1 时依然走同一条路径 (一段、主线程录制), 这样单线程与
    /// 多线程的差别只有段数, 便于对照。
    ERHIResult Initialize(IRHIDevice* device,
                          UInt32      framesInFlight,
                          UInt32      threadCount);

    void Shutdown(IRHIDevice* device);

    LIMX_NODISCARD UInt32 GetThreadCount() const { return m_ThreadCount; }

    LIMX_NODISCARD bool IsInitialized() const { return m_Device != nullptr; }

    // ========================================================================
    // 逐帧
    // ========================================================================

    /// 开始一帧 — 重置本帧要用的全部命令池
    ///
    /// 重置池而非逐个重置缓冲区: 前者一次系统调用, 后者每个缓冲区一次。
    void BeginFrame(UInt32 frameIndex);

    /// 把 [0, batchCount) 切段并行录制
    ///
    /// @param body 录制函数, 会在多个线程上并发调用, 签名为
    ///             void(IRHICommandBuffer*, SizeType begin, SizeType end)。
    ///             它拿到的是各自独立的次级命令缓冲区, 因此内部不需要
    ///             任何加锁 —— 但它读到的一切必须是只读的。
    ///
    /// @return 本批的段范围
    template<typename BodyType>
    FRecorderBatch RecordSegmented(
        SizeType                            batchCount,
        const FRHICommandBufferInheritance& inheritance,
        BodyType&&                          body);

    /// 在并行各段之后追加一个串行录制的尾段
    ///
    /// 用于顺序敏感、不能切段的部分 —— 典型是半透明: 它按到相机的距离
    /// 由远及近绘制, 切段之后段内顺序虽然还对, 但每段各自重置绑定状态
    /// 并不改变结果, 真正的问题是它本来就不该被拆开推理。
    ///
    /// 尾段仍然是次级缓冲区 —— 通道以 secondary 模式开始之后, 主缓冲区
    /// 里不能再直接录任何绘制命令。
    template<typename BodyType>
    void RecordTail(const FRHICommandBufferInheritance& inheritance,
                    BodyType&&                          body,
                    FRecorderBatch&                     batch);

    /// 把一批段按段号顺序执行进主缓冲区
    void ExecuteInto(IRHICommandBuffer*    primary,
                     const FRecorderBatch& batch);

    // ========================================================================
    // 统计
    // ========================================================================

    /// 上一帧的录制墙钟耗时 (毫秒)
    LIMX_NODISCARD Float64 GetRecordMilliseconds() const
    {
        return m_RecordMilliseconds;
    }

    /// 本帧累计使用的段数 (跨全部批次)
    LIMX_NODISCARD SizeType GetSegmentCount() const { return m_Segments.GetSize(); }

private:
    /// 每个段一套资源
    ///
    /// 命令池必须与命令缓冲区一一对应 —— 这是 Vulkan 的外部同步要求,
    /// 不是性能优化。按帧再分开是为了避免重置正在被 GPU 读取的那一帧。
    struct FSegmentResources
    {
        FRHICommandPoolHandle         Pools[kMaxFramesInFlight];
        FRHICommandBufferHandle       Handles[kMaxFramesInFlight];
        TUniquePtr<IRHICommandBuffer> Buffers[kMaxFramesInFlight];
    };

    IRHIDevice* m_Device         = nullptr;
    UInt32      m_ThreadCount    = 0;
    UInt32      m_FramesInFlight = 0;
    UInt32      m_FrameIndex     = 0;

    /// 段槽位 — 共 线程数 x kSegmentsPerThread 个
    TArray<FSegmentResources> m_Slots;

    TArray<FRecorderSegment> m_Segments;

    /// 串行尾段的资源 — 独立于各槽位的池, 由主线程使用
    ///
    /// 一帧只有一个尾段: 目前只有前向 Pass 的半透明需要它。若将来有第二
    /// 处需要, 尾段也要跟着按批分配。
    FRHICommandPoolHandle         m_TailPools[kMaxFramesInFlight];
    FRHICommandBufferHandle       m_TailHandles[kMaxFramesInFlight];
    TUniquePtr<IRHICommandBuffer> m_TailBuffers[kMaxFramesInFlight];

    /// 本帧已分配到第几个槽位
    ///
    /// 逐帧从 0 开始。同一帧内的多批必须占不同槽位 —— 见 FRecorderBatch
    /// 的说明。
    SizeType m_NextSlot = 0;

    /// 本帧累计录制耗时 (跨全部批次)
    Float64  m_RecordMilliseconds = 0.0;

    /// 按需扩容槽位, 保证至少有 count 个可用
    ERHIResult EnsureSlots(SizeType count);

    /// 常驻任务图
    ///
    /// 与资产导入那边不同, 这张图必须常驻 —— 逐帧创建十几个线程是
    /// 不可接受的, 那点开销正是这整件事要省掉的东西。
    FTaskGraph m_Graph;
};

} // namespace Limx

#include "Renderer/Recording/FParallelRecorder.inl"
