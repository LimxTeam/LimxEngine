/*******************************************************************************
 * 文件: FParallelRecorder.inl
 * 创建时间: 2026-08-31
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   FParallelRecorder::RecordSegmented 的模板实现
 *
 ******************************************************************************/

#pragma once

#include "Core/Threading/FJobExecutor.h"
#include "Core/HAL/FPlatformTime.h"

namespace Limx
{

template<typename BodyType>
SizeType FParallelRecorder::RecordSegmented(
    SizeType                            batchCount,
    const FRHICommandBufferInheritance& inheritance,
    BodyType&&                          body)
{
    m_ActiveSegments     = 0;
    m_RecordMilliseconds = 0.0;

    if (m_Device == nullptr || batchCount == 0)
    {
        return 0;
    }

    const Float64 begin = FPlatformTime::Seconds();

    // ---- 切段 ----
    //
    // 段数取 线程数 x kSegmentsPerThread, 但不超过批次数 —— 空段没有意义,
    // 而每个空段仍要走一遍 Begin/End 与一次 vkCmdExecuteCommands。
    SizeType segmentCount =
        static_cast<SizeType>(m_ThreadCount) * kSegmentsPerThread;

    if (segmentCount > batchCount)
    {
        segmentCount = batchCount;
    }

    if (segmentCount == 0)
    {
        return 0;
    }

    m_Segments.Clear();
    m_Segments.Reserve(segmentCount);

    const SizeType baseSize  = batchCount / segmentCount;
    const SizeType remainder = batchCount % segmentCount;

    SizeType cursor = 0;

    for (SizeType s = 0; s < segmentCount; ++s)
    {
        // 余数摊到前几段, 而不是全压给最后一段 —— 后者会让最后一段多出
        // segmentCount-1 个批次, 正好抵消掉切段的意义。
        const SizeType size = baseSize + ((s < remainder) ? 1 : 0);

        // 段与线程静态绑定: 第 s 段固定用第 (s % 线程数) 个线程的池。
        //
        // 不按"运行时哪个线程领到这一段"来选池 —— 那样两个线程可能同时
        // 碰到同一个命令池, 而 Vulkan 要求命令池外部同步。
        const UInt32 thread = static_cast<UInt32>(s % m_ThreadCount);
        const UInt32 slot   = static_cast<UInt32>(s / m_ThreadCount);

        FRecorderSegment segment;
        segment.Begin         = cursor;
        segment.End           = cursor + size;
        segment.CommandBuffer =
            m_Threads[thread].Buffers[m_FrameIndex][slot].Get();
        segment.Handle        = m_Threads[thread].Handles[m_FrameIndex][slot];

        m_Segments.Add(segment);

        cursor += size;
    }

    LIMX_ASSERT(cursor == batchCount);

    // ---- 并行录制 ----
    //
    // 批大小取 1: 每段自己就是一个工作单元。
    {
        FRecorderSegment* segments = m_Segments.GetData();

        const FRHICommandBufferInheritance inherit = inheritance;

        FJobExecutor::ParallelFor(
            m_Graph, segmentCount, 1,
            [segments, inherit, &body](SizeType first, SizeType last)
            {
                for (SizeType i = first; i < last; ++i)
                {
                    FRecorderSegment& segment = segments[i];

                    if (segment.CommandBuffer == nullptr)
                    {
                        continue;
                    }

                    if (segment.CommandBuffer->BeginSecondary(inherit)
                        != ERHIResult::Success)
                    {
                        continue;
                    }

                    body(segment.CommandBuffer, segment.Begin, segment.End);

                    segment.CommandBuffer->End();
                }
            });
    }

    m_ActiveSegments     = segmentCount;
    m_RecordMilliseconds = (FPlatformTime::Seconds() - begin) * 1000.0;

    return segmentCount;
}

} // namespace Limx
