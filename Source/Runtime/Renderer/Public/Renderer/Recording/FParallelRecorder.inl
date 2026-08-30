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
    // 先清空再判早退。
    //
    // 反过来的话, 不透明批次为 0 的那一帧会带着上一帧的段列表进入
    // RecordTail, 而 ExecuteInto 按 m_ActiveSegments 从下标 0 数 ——
    // 于是执行到的是上一帧的缓冲区。这种错误只在"某一帧恰好没有不透明
    // 物体"时出现, 平时完全看不到。
    m_Segments.Clear();
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

    m_Segments.Reserve(segmentCount);

    const SizeType baseSize  = batchCount / segmentCount;
    const SizeType remainder = batchCount % segmentCount;

    SizeType cursor = 0;

    for (SizeType s = 0; s < segmentCount; ++s)
    {
        // 余数摊到前几段, 而不是全压给最后一段 —— 后者会让最后一段多出
        // segmentCount-1 个批次, 正好抵消掉切段的意义。
        const SizeType size = baseSize + ((s < remainder) ? 1 : 0);

        // 第 s 段用第 s 个槽位 —— 一段一个命令池。
        //
        // 不能按线程分池: 静态绑定的是段与池而不是段与线程, 共享同一个
        // 池的多个段完全可能被调度器同时派给不同线程。验证层抓到过。
        FRecorderSegment segment;
        segment.Begin         = cursor;
        segment.End           = cursor + size;
        segment.CommandBuffer = m_Slots[s].Buffers[m_FrameIndex].Get();
        segment.Handle        = m_Slots[s].Handles[m_FrameIndex];

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

template<typename BodyType>
void FParallelRecorder::RecordTail(
    const FRHICommandBufferInheritance& inheritance,
    BodyType&&                          body)
{
    if (m_Device == nullptr)
    {
        return;
    }

    IRHICommandBuffer* commandBuffer = m_TailBuffers[m_FrameIndex].Get();

    if (commandBuffer == nullptr)
    {
        return;
    }

    if (commandBuffer->BeginSecondary(inheritance) != ERHIResult::Success)
    {
        return;
    }

    body(commandBuffer);

    commandBuffer->End();

    FRecorderSegment segment;
    segment.Begin         = 0;
    segment.End           = 0;
    segment.CommandBuffer = commandBuffer;
    segment.Handle        = m_TailHandles[m_FrameIndex];

    m_Segments.Add(segment);

    ++m_ActiveSegments;
}

} // namespace Limx
