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

    // 线程数为 0 表示按硬件来。
    //
    // 减 1 是把主线程算进去 —— 主线程也参与录制 (FJobExecutor 的
    // ParallelFor 会让调用线程一起干活), 所以工作线程数应比核心数少一。
    if (threadCount == 0)
    {
        const UInt32 hardware = FThread::HardwareConcurrency();
        threadCount = (hardware > 1) ? hardware : 1;
    }

    if (threadCount > kMaxThreads)
    {
        threadCount = kMaxThreads;
    }

    m_Device         = device;
    m_ThreadCount    = threadCount;
    m_FramesInFlight = framesInFlight;
    m_FrameIndex     = 0;

    m_Threads.Clear();
    m_Threads.SetSize(threadCount);

    for (UInt32 t = 0; t < threadCount; ++t)
    {
        FThreadResources& res = m_Threads[t];

        for (UInt32 f = 0; f < framesInFlight; ++f)
        {
            const ERHIResult poolResult = device->CreateCommandPool(
                EQueueType::Graphics, res.Pools[f]);

            if (poolResult != ERHIResult::Success)
            {
                LIMX_LOG(LogParallelRecorder, Error,
                         "[并行录制] 线程 {} 帧 {} 的命令池创建失败", t, f);
                return poolResult;
            }

            for (UInt32 s = 0; s < kSegmentsPerThread; ++s)
            {
                const ERHIResult bufferResult =
                    device->AllocateCommandBuffer(
                        res.Pools[f],
                        ECommandBufferLevel::Secondary,
                        res.Handles[f][s]);

                if (bufferResult != ERHIResult::Success)
                {
                    LIMX_LOG(LogParallelRecorder, Error,
                             "[并行录制] 线程 {} 帧 {} 段 {} 的次级缓冲区分配失败",
                             t, f, s);
                    return bufferResult;
                }

                res.Buffers[f][s] =
                    CreateRHICommandBuffer(device, res.Handles[f][s]);

                if (!res.Buffers[f][s])
                {
                    return ERHIResult::ErrorUnknown;
                }
            }
        }
    }

    // 任务图: 线程数减 1 个工作线程 —— 调用线程也参与, 见上面的说明。
    m_Graph.Initialize((threadCount > 1) ? (threadCount - 1) : 1);

    LIMX_LOG(LogParallelRecorder, Display,
             "[并行录制] 已就绪 — {} 个录制线程 x {} 帧 x {} 段 "
             "= {} 个次级缓冲区",
             threadCount, framesInFlight, kSegmentsPerThread,
             threadCount * framesInFlight * kSegmentsPerThread);

    return ERHIResult::Success;
}

void FParallelRecorder::Shutdown(IRHIDevice* device)
{
    // 先停任务图再放资源 —— 反过来的话正在录制的线程会写已销毁的缓冲区
    m_Graph.Shutdown();

    if (device == nullptr)
    {
        m_Device = nullptr;
        m_Threads.Clear();
        return;
    }

    for (SizeType t = 0; t < m_Threads.GetSize(); ++t)
    {
        FThreadResources& res = m_Threads[t];

        for (UInt32 f = 0; f < m_FramesInFlight; ++f)
        {
            for (UInt32 s = 0; s < kSegmentsPerThread; ++s)
            {
                res.Buffers[f][s].Reset();

                if (res.Handles[f][s].IsValid())
                {
                    device->FreeCommandBuffer(res.Handles[f][s]);
                }
            }

            if (res.Pools[f].IsValid())
            {
                device->DestroyCommandPool(res.Pools[f]);
            }
        }
    }

    m_Threads.Clear();
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
    for (SizeType t = 0; t < m_Threads.GetSize(); ++t)
    {
        m_Device->ResetCommandPool(m_Threads[t].Pools[m_FrameIndex]);
    }

    m_ActiveSegments = 0;
}

void FParallelRecorder::ExecuteInto(IRHICommandBuffer* primary)
{
    if (primary == nullptr || m_ActiveSegments == 0)
    {
        return;
    }

    // 按段号顺序执行。
    //
    // 各线程的完成先后是随机的, 但这里永远按 0,1,2,... 的顺序交给主
    // 缓冲区。这是多线程录制与单线程逐像素相同的前提: 深度相等的表面、
    // 半透明的混合顺序都依赖绘制顺序, 按完成顺序执行会让结果每帧不同。
    constexpr SizeType kMaxInline = 128;

    FRHICommandBufferHandle handles[kMaxInline];

    SizeType written = 0;

    for (SizeType i = 0; i < m_ActiveSegments; ++i)
    {
        if (written >= kMaxInline)
        {
            primary->ExecuteCommands(handles, static_cast<UInt32>(written));
            written = 0;
        }

        handles[written] = m_Segments[i].Handle;
        ++written;
    }

    if (written > 0)
    {
        primary->ExecuteCommands(handles, static_cast<UInt32>(written));
    }
}

} // namespace Limx
