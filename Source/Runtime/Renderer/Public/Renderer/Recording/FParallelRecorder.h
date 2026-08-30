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
 *   段与线程静态绑定 — 第 s 段固定用第 (s % 线程数) 个线程的命令池。
 *     不按运行时哪个线程领到这一段来选池: 那样两个线程可能同时碰到同一
 *     个池, 正是要避免的事。
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

    /// 每线程每帧预分配几个次级缓冲区
    ///
    /// 段数上限 = 线程数 x 这个值。取 4 是权衡: 太小则负载不均, 太大则
    /// 每段的固定开销 (Begin/End 各一次系统调用) 开始显著。
    static constexpr UInt32 kSegmentsPerThread = 4;

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
    /// @return 实际产生的段数
    template<typename BodyType>
    SizeType RecordSegmented(SizeType                            batchCount,
                             const FRHICommandBufferInheritance& inheritance,
                             BodyType&&                          body);

    /// 把本帧录好的各段按段号顺序执行进主缓冲区
    void ExecuteInto(IRHICommandBuffer* primary);

    // ========================================================================
    // 统计
    // ========================================================================

    /// 上一帧的录制墙钟耗时 (毫秒)
    LIMX_NODISCARD Float64 GetRecordMilliseconds() const
    {
        return m_RecordMilliseconds;
    }

    /// 上一帧实际使用的段数
    LIMX_NODISCARD SizeType GetSegmentCount() const { return m_ActiveSegments; }

private:
    /// 每个线程一套资源
    ///
    /// 命令池必须逐线程独立 —— 这是 Vulkan 的外部同步要求, 不是性能优化。
    /// 再按帧分开是为了避免重置正在被 GPU 读取的那一帧的池。
    struct FThreadResources
    {
        FRHICommandPoolHandle Pools[kMaxFramesInFlight];

        FRHICommandBufferHandle
            Handles[kMaxFramesInFlight][kSegmentsPerThread];

        TUniquePtr<IRHICommandBuffer>
            Buffers[kMaxFramesInFlight][kSegmentsPerThread];
    };

    IRHIDevice* m_Device         = nullptr;
    UInt32      m_ThreadCount    = 0;
    UInt32      m_FramesInFlight = 0;
    UInt32      m_FrameIndex     = 0;

    TArray<FThreadResources> m_Threads;
    TArray<FRecorderSegment> m_Segments;

    SizeType m_ActiveSegments     = 0;
    Float64  m_RecordMilliseconds = 0.0;

    /// 常驻任务图
    ///
    /// 与资产导入那边不同, 这张图必须常驻 —— 逐帧创建十几个线程是
    /// 不可接受的, 那点开销正是这整件事要省掉的东西。
    FTaskGraph m_Graph;
};

} // namespace Limx

#include "Renderer/Recording/FParallelRecorder.inl"
