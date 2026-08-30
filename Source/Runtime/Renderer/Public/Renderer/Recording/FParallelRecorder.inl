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
FRecorderBatch FParallelRecorder::RecordSegmented(
    SizeType                            batchCount,
    const FRHICommandBufferInheritance& inheritance,
    BodyType&&                          body)
{
    FRecorderBatch result;
    result.First = m_Segments.GetSize();
    result.Count = 0;

    if (m_Device == nullptr || batchCount == 0)
    {
        return result;
    }

    const Float64 begin = FPlatformTime::Seconds();

    // ---- 切段 ----
    //
    // 两个约束取更严的那个:
    //   上限   线程数 x kSegmentsPerThread —— 再多也没有线程去跑
    //   下限   每段至少 kMinBatchesPerSegment 个批次 —— 段太小时每段的
    //          固定开销 (Begin/End + 重录公共状态) 会压过并行收益
    //
    // 后者是实测逼出来的: 不加它时默认线程数 (硬件并发数, 常见 16) 会
    // 切出每段十几个批次的碎片, 比不并行还慢 40%。
    SizeType segmentCount =
        static_cast<SizeType>(m_ThreadCount) * kSegmentsPerThread;

    const SizeType workLimited = batchCount / kMinBatchesPerSegment;

    if (segmentCount > workLimited)
    {
        segmentCount = workLimited;
    }

    // 至少一段 —— 批次数少于 kMinBatchesPerSegment 时整批放一段里,
    // 那时并行本来也没有意义。
    if (segmentCount == 0)
    {
        segmentCount = 1;
    }

    if (segmentCount > batchCount)
    {
        segmentCount = batchCount;
    }

    if (segmentCount == 0)
    {
        return result;
    }

    // 本批占用 [m_NextSlot, m_NextSlot + segmentCount) 这些槽位。
    //
    // 不复用上一批的槽位: 那些次级缓冲区已经被 vkCmdExecuteCommands 引用,
    // 在主缓冲区执行完之前重写是未定义行为。
    if (EnsureSlots(m_NextSlot + segmentCount) != ERHIResult::Success)
    {
        return result;
    }

    const SizeType slotBase = m_NextSlot;
    m_NextSlot += segmentCount;

    m_Segments.Reserve(m_Segments.GetSize() + segmentCount);

    const SizeType baseSize  = batchCount / segmentCount;
    const SizeType remainder = batchCount % segmentCount;

    SizeType cursor = 0;

    for (SizeType s = 0; s < segmentCount; ++s)
    {
        // 余数摊到前几段, 而不是全压给最后一段 —— 后者会让最后一段多出
        // segmentCount-1 个批次, 正好抵消掉切段的意义。
        const SizeType size = baseSize + ((s < remainder) ? 1 : 0);

        // 一段一个命令池。
        //
        // 不能按线程分池: 静态绑定的是段与池而不是段与线程, 共享同一个
        // 池的多个段完全可能被调度器同时派给不同线程。验证层抓到过。
        const SizeType slot = slotBase + s;

        FRecorderSegment segment;
        segment.Begin         = cursor;
        segment.End           = cursor + size;
        segment.CommandBuffer = m_Slots[slot].Buffers[m_FrameIndex].Get();
        segment.Handle        = m_Slots[slot].Handles[m_FrameIndex];

        m_Segments.Add(segment);

        cursor += size;
    }

    LIMX_ASSERT(cursor == batchCount);

    // ---- 并行录制 ----
    //
    // 批大小取 1: 每段自己就是一个工作单元。
    {
        FRecorderSegment* segments = m_Segments.GetData() + result.First;

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

    result.Count = segmentCount;

    m_RecordMilliseconds += (FPlatformTime::Seconds() - begin) * 1000.0;

    return result;
}

template<typename BodyType>
void FParallelRecorder::RecordTail(
    const FRHICommandBufferInheritance& inheritance,
    BodyType&&                          body,
    FRecorderBatch&                     batch)
{
    if (m_Device == nullptr)
    {
        return;
    }

    // 尾段必须紧接在本批之后 —— ExecuteInto 按 [First, First+Count) 执行
    LIMX_ASSERT(batch.First + batch.Count == m_Segments.GetSize());

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

    ++batch.Count;
}

} // namespace Limx
