/*******************************************************************************
 * 文件: FParallelRecorder.cpp
 * 创建时间: 2026-08-31
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   FParallelRecorder 的非模板部分 — 资源创建、逐帧重置、顺序执行
 *
 ******************************************************************************/

#include "Renderer/Recording/FParallelRecorder.h"

#include "Core/Logging/FLog.h"
#include "Core/Threading/FThread.h"
#include "RHI/RHI/RHIFactory.h"

namespace Limx
{

LIMX_DECLARE_LOG_CATEGORY(LogParallelRecorder)
LIMX_DEFINE_LOG_CATEGORY(LogParallelRecorder)

// ============================================================================
// 生命周期
// ============================================================================

ERHIResult FParallelRecorder::Initialize(IRHIDevice* device,
                                          UInt32      framesInFlight,
                                          UInt32      threadCount)
{
    if (device == nullptr || framesInFlight == 0)
    {
        return ERHIResult::ErrorInvalidParameter;
    }

    if (framesInFlight > kMaxFramesInFlight)
    {
        LIMX_LOG(LogParallelRecorder, Error,
                 "[并行录制] 在飞帧数 {} 超过上限 {}",
                 framesInFlight, kMaxFramesInFlight);
        return ERHIResult::ErrorInvalidParameter;
    }

    // 线程数为 0 表示自动选。
    //
    // **不是**取硬件并发数。实测 (60x60 网格, 918 可见批次, 三个 Pass):
    //
    //     内联       14.66 ms
    //      4 线程    10.31 ms   最优
    //      8 线程    12.33 ms
    //     16 线程    19.15 ms   比不并行还慢 30%
    //
    // 段数在这三档下分别是 16/19/19 (受 kMinBatchesPerSegment 约束),
    // 也就是说差别不在段数, 在线程数本身。
    //
    // 原因在当前任务图的实现: 就绪队列由单个互斥量保护, 十几个线程抢
    // 十几个短任务时, 时间花在锁上而不是录制上; 而每帧三个 Pass 各要
    // 一轮唤醒与休眠, 60 fps 下就是每秒 180 轮。
    //
    // 所以 4 是**当前实现**的性质, 不是工作负载的性质。任务图换成工作
    // 窃取队列之后这个上限应当重新测量。--record-threads 保留就是为了
    // 在别的硬件与负载上重测。
    constexpr UInt32 kAutoThreadCap = 4;

    if (threadCount == 0)
    {
        const UInt32 hardware = FThread::HardwareConcurrency();

        threadCount = (hardware > 1) ? hardware : 1;

        if (threadCount > kAutoThreadCap)
        {
            threadCount = kAutoThreadCap;
        }
    }

    if (threadCount > kMaxThreads)
    {
        threadCount = kMaxThreads;
    }

    m_Device         = device;
    m_ThreadCount    = threadCount;
    m_FramesInFlight = framesInFlight;
    m_FrameIndex     = 0;

    m_Slots.Clear();

    // 先建一批 —— 够前向 Pass 用。阴影 Pass 的三个级联会让本帧的槽位需求
    // 变成四倍, 那时 EnsureSlots 按需补。
    //
    // 不一次性建足最坏情况的量: 槽位数取决于本帧有多少个通道要并行录制,
    // 而那是运行时才知道的。按需扩容之后, 槽位只增不减, 稳态下不再分配。
    const ERHIResult slotResult =
        EnsureSlots(static_cast<SizeType>(threadCount) * kSegmentsPerThread);

    if (slotResult != ERHIResult::Success)
    {
        return slotResult;
    }

    // 串行尾段的资源 —— 独立的池, 只由主线程使用
    for (UInt32 f = 0; f < framesInFlight; ++f)
    {
        const ERHIResult poolResult = device->CreateCommandPool(
            EQueueType::Graphics, m_TailPools[f]);

        if (poolResult != ERHIResult::Success)
        {
            return poolResult;
        }

        const ERHIResult bufferResult = device->AllocateCommandBuffer(
            m_TailPools[f], ECommandBufferLevel::Secondary,
            m_TailHandles[f]);

        if (bufferResult != ERHIResult::Success)
        {
            return bufferResult;
        }

        m_TailBuffers[f] = CreateRHICommandBuffer(device, m_TailHandles[f]);

        if (!m_TailBuffers[f])
        {
            return ERHIResult::ErrorUnknown;
        }
    }

    // 任务图: 线程数减 1 个工作线程 —— 调用线程也参与, 见上面的说明。
    m_Graph.Initialize((threadCount > 1) ? (threadCount - 1) : 1);

    LIMX_LOG(LogParallelRecorder, Display,
             "[并行录制] 已就绪 — {} 线程 / {} 段槽位 / {} 帧 (一段一池)",
             threadCount, m_Slots.GetSize(), framesInFlight);

    return ERHIResult::Success;
}

ERHIResult FParallelRecorder::EnsureSlots(SizeType count)
{
    if (m_Device == nullptr)
    {
        return ERHIResult::ErrorInvalidParameter;
    }

    if (count <= m_Slots.GetSize())
    {
        return ERHIResult::Success;
    }

    const SizeType oldSize = m_Slots.GetSize();

    m_Slots.SetSize(count);

    for (SizeType s = oldSize; s < count; ++s)
    {
        FSegmentResources& res = m_Slots[s];

        for (UInt32 f = 0; f < m_FramesInFlight; ++f)
        {
            const ERHIResult poolResult = m_Device->CreateCommandPool(
                EQueueType::Graphics, res.Pools[f]);

            if (poolResult != ERHIResult::Success)
            {
                LIMX_LOG(LogParallelRecorder, Error,
                         "[并行录制] 槽位 {} 帧 {} 的命令池创建失败", s, f);
                return poolResult;
            }

            const ERHIResult bufferResult = m_Device->AllocateCommandBuffer(
                res.Pools[f], ECommandBufferLevel::Secondary,
                res.Handles[f]);

            if (bufferResult != ERHIResult::Success)
            {
                LIMX_LOG(LogParallelRecorder, Error,
                         "[并行录制] 槽位 {} 帧 {} 的次级缓冲区分配失败", s, f);
                return bufferResult;
            }

            res.Buffers[f] = CreateRHICommandBuffer(m_Device, res.Handles[f]);

            if (!res.Buffers[f])
            {
                return ERHIResult::ErrorUnknown;
            }
        }
    }

    LIMX_LOG(LogParallelRecorder, Display,
             "[并行录制] 槽位扩容 {} → {} (每槽 {} 帧各一个命令池)",
             oldSize, count, m_FramesInFlight);

    return ERHIResult::Success;
}

void FParallelRecorder::Shutdown(IRHIDevice* device)
{
    // 先停任务图再放资源 —— 反过来的话正在录制的线程会写已销毁的缓冲区
    m_Graph.Shutdown();

    if (device == nullptr)
    {
        m_Device = nullptr;
        m_Slots.Clear();
        return;
    }

    for (SizeType s = 0; s < m_Slots.GetSize(); ++s)
    {
        FSegmentResources& res = m_Slots[s];

        for (UInt32 f = 0; f < m_FramesInFlight; ++f)
        {
            res.Buffers[f].Reset();

            if (res.Handles[f].IsValid())
            {
                device->FreeCommandBuffer(res.Handles[f]);
            }

            if (res.Pools[f].IsValid())
            {
                device->DestroyCommandPool(res.Pools[f]);
            }
        }
    }

    for (UInt32 f = 0; f < m_FramesInFlight; ++f)
    {
        m_TailBuffers[f].Reset();

        if (m_TailHandles[f].IsValid())
        {
            device->FreeCommandBuffer(m_TailHandles[f]);
        }

        if (m_TailPools[f].IsValid())
        {
            device->DestroyCommandPool(m_TailPools[f]);
        }
    }

    m_Slots.Clear();
    m_Segments.Clear();
    m_Device = nullptr;
}

// ============================================================================
// 逐帧
// ============================================================================

void FParallelRecorder::BeginFrame(UInt32 frameIndex)
{
    if (m_Device == nullptr)
    {
        return;
    }

    m_FrameIndex = frameIndex % m_FramesInFlight;

    // 重置本帧要用的全部命令池。
    //
    // 重置池而非逐个重置缓冲区: 前者一次调用回收整池, 后者每个缓冲区
    // 一次。按帧分池的意义正在于此 —— 本帧的池此刻一定不再被 GPU 读取
    // (在飞帧数保证了这一点), 可以整体回收。
    for (SizeType s = 0; s < m_Slots.GetSize(); ++s)
    {
        m_Device->ResetCommandPool(m_Slots[s].Pools[m_FrameIndex]);
    }

    m_Device->ResetCommandPool(m_TailPools[m_FrameIndex]);

    m_NextSlot           = 0;
    m_RecordMilliseconds = 0.0;
    m_Segments.Clear();
}

void FParallelRecorder::ExecuteInto(IRHICommandBuffer*    primary,
                                    const FRecorderBatch& batch)
{
    if (primary == nullptr || batch.IsEmpty())
    {
        return;
    }

    LIMX_ASSERT(batch.First + batch.Count <= m_Segments.GetSize());

    // 按段号顺序执行。
    //
    // 各线程的完成先后是随机的, 但这里永远按 0,1,2,... 的顺序交给主
    // 缓冲区。这是多线程录制与单线程逐像素相同的前提: 深度相等的表面、
    // 半透明的混合顺序都依赖绘制顺序, 按完成顺序执行会让结果每帧不同。
    constexpr SizeType kMaxInline = 128;

    FRHICommandBufferHandle handles[kMaxInline];

    SizeType written = 0;

    for (SizeType i = 0; i < batch.Count; ++i)
    {
        if (written >= kMaxInline)
        {
            primary->ExecuteCommands(handles, static_cast<UInt32>(written));
            written = 0;
        }

        handles[written] = m_Segments[batch.First + i].Handle;
        ++written;
    }

    if (written > 0)
    {
        primary->ExecuteCommands(handles, static_cast<UInt32>(written));
    }
}

} // namespace Limx
