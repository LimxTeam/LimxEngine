/*******************************************************************************
 * 文件: TRangeAllocator.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   范围分配器 — 管理连续范围的分配与回收
 *   维护空闲范围列表，分配时查找合适范围，回收时合并相邻范围
 *   用于 GPU 缓冲区子分配、纹理图集区域管理、虚拟地址空间等场景
 *
 * 设计哲学:
 *   范围管理 — 以 [Offset, Size] 描述连续范围
 *   合并回收 — 释放范围时自动与相邻空闲范围合并，减少碎片
 *   首次适配 — 分配策略为 First-Fit，简单高效
 *
 * 技术特性:
 *   - TRangeAllocator: 范围分配器
 *   - Allocate: 分配指定大小的范围
 *   - Free: 释放范围并自动合并
 *   - GetFreeSize: 总空闲大小
 *   - GetLargestFreeRange: 最大连续空闲范围
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Containers/TArray.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Containers/TArray.h"

namespace Limx
{

/// 范围描述
struct FRange
{
    SizeType Offset;  ///< 起始偏移
    SizeType Size;    ///< 大小

    FRange() : Offset(0), Size(0) {}
    FRange(SizeType offset, SizeType size)
        : Offset(offset), Size(size) {}

    /// 范围结束位置 (不包含)
    LIMX_NODISCARD SizeType GetEnd() const
    {
        return Offset + Size;
    }
};

/// 范围分配器
class TRangeAllocator
{
public:
    /// 构造 — 初始化为一个完整的空闲范围
    explicit TRangeAllocator(SizeType totalSize)
        : m_TotalSize(totalSize)
    {
        m_FreeRanges.Add(FRange(0, totalSize));
    }

    // ========================================================================
    // 分配
    // ========================================================================

    /// 分配指定大小的范围 (First-Fit)
    /// @param size 请求大小
    /// @param outRange 输出分配的范围
    /// @return 是否分配成功
    bool Allocate(SizeType size, FRange& outRange)
    {
        if (size == 0) return false;

        for (SizeType rangeIdx = 0;
             rangeIdx < m_FreeRanges.GetSize(); ++rangeIdx)
        {
            FRange& freeRange = m_FreeRanges[rangeIdx];

            if (freeRange.Size >= size)
            {
                outRange.Offset = freeRange.Offset;
                outRange.Size = size;

                if (freeRange.Size == size)
                {
                    // 完全消耗
                    m_FreeRanges.RemoveAt(rangeIdx);
                }
                else
                {
                    // 缩小空闲范围
                    freeRange.Offset += size;
                    freeRange.Size -= size;
                }

                return true;
            }
        }

        return false; // 无足够空间
    }

    /// 分配对齐的范围
    /// @param size 请求大小
    /// @param alignment 对齐要求 (必须是 2 的幂)
    /// @param outRange 输出分配的范围
    /// @return 是否分配成功
    bool AllocateAligned(SizeType size, SizeType alignment,
                         FRange& outRange)
    {
        if (size == 0 || alignment == 0) return false;

        for (SizeType rangeIdx = 0;
             rangeIdx < m_FreeRanges.GetSize(); ++rangeIdx)
        {
            FRange& freeRange = m_FreeRanges[rangeIdx];

            // 对齐后的起始偏移
            SizeType alignedOffset =
                (freeRange.Offset + alignment - 1) &
                ~(alignment - 1);
            SizeType padding = alignedOffset - freeRange.Offset;
            SizeType totalNeeded = padding + size;

            if (freeRange.Size >= totalNeeded)
            {
                outRange.Offset = alignedOffset;
                outRange.Size = size;

                if (padding > 0)
                {
                    // 前面留有间隙 — 保留为空闲范围
                    FRange gapRange(freeRange.Offset, padding);
                    freeRange.Offset = alignedOffset + size;
                    freeRange.Size -= totalNeeded;

                    if (freeRange.Size == 0)
                    {
                        // 替换当前范围为间隙
                        m_FreeRanges[rangeIdx] = gapRange;
                    }
                    else
                    {
                        // 在当前位置前插入间隙
                        InsertFreeRange(rangeIdx, gapRange);
                    }
                }
                else
                {
                    if (freeRange.Size == size)
                    {
                        m_FreeRanges.RemoveAt(rangeIdx);
                    }
                    else
                    {
                        freeRange.Offset += size;
                        freeRange.Size -= size;
                    }
                }

                return true;
            }
        }

        return false;
    }

    // ========================================================================
    // 释放
    // ========================================================================

    /// 释放范围 (自动合并相邻空闲范围)
    void Free(const FRange& range)
    {
        if (range.Size == 0) return;

        SizeType insertIdx = 0;
        for (; insertIdx < m_FreeRanges.GetSize(); ++insertIdx)
        {
            if (m_FreeRanges[insertIdx].Offset > range.Offset)
            {
                break;
            }
        }

        // 插入到正确位置
        InsertFreeRange(insertIdx, range);

        // 尝试与后一个范围合并
        if (insertIdx + 1 < m_FreeRanges.GetSize())
        {
            FRange& current = m_FreeRanges[insertIdx];
            FRange& next = m_FreeRanges[insertIdx + 1];

            if (current.GetEnd() == next.Offset)
            {
                current.Size += next.Size;
                m_FreeRanges.RemoveAt(insertIdx + 1);
            }
        }

        // 尝试与前一个范围合并
        if (insertIdx > 0)
        {
            FRange& prev = m_FreeRanges[insertIdx - 1];
            FRange& current = m_FreeRanges[insertIdx];

            if (prev.GetEnd() == current.Offset)
            {
                prev.Size += current.Size;
                m_FreeRanges.RemoveAt(insertIdx);
            }
        }
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 总大小
    LIMX_NODISCARD SizeType GetTotalSize() const
    {
        return m_TotalSize;
    }

    /// 总空闲大小
    LIMX_NODISCARD SizeType GetFreeSize() const
    {
        SizeType total = 0;
        for (SizeType rangeIdx = 0;
             rangeIdx < m_FreeRanges.GetSize(); ++rangeIdx)
        {
            total += m_FreeRanges[rangeIdx].Size;
        }
        return total;
    }

    /// 已分配大小
    LIMX_NODISCARD SizeType GetAllocatedSize() const
    {
        return m_TotalSize - GetFreeSize();
    }

    /// 最大连续空闲范围
    LIMX_NODISCARD SizeType GetLargestFreeRange() const
    {
        SizeType largest = 0;
        for (SizeType rangeIdx = 0;
             rangeIdx < m_FreeRanges.GetSize(); ++rangeIdx)
        {
            if (m_FreeRanges[rangeIdx].Size > largest)
            {
                largest = m_FreeRanges[rangeIdx].Size;
            }
        }
        return largest;
    }

    /// 空闲范围数 (碎片指标)
    LIMX_NODISCARD SizeType GetFreeRangeCount() const
    {
        return m_FreeRanges.GetSize();
    }

    /// 是否完全空闲
    LIMX_NODISCARD bool IsEmpty() const
    {
        return m_FreeRanges.GetSize() == 1 &&
               m_FreeRanges[0].Size == m_TotalSize;
    }

    /// 是否完全占满
    LIMX_NODISCARD bool IsFull() const
    {
        return m_FreeRanges.GetSize() == 0;
    }

    /// 重置为完全空闲
    void Reset()
    {
        m_FreeRanges.Clear();
        m_FreeRanges.Add(FRange(0, m_TotalSize));
    }

private:
    /// 在指定位置插入空闲范围
    void InsertFreeRange(SizeType index, const FRange& range)
    {
        if (index >= m_FreeRanges.GetSize())
        {
            m_FreeRanges.Add(range);
            return;
        }

        m_FreeRanges.Add(
            m_FreeRanges[m_FreeRanges.GetSize() - 1]);
        for (SizeType moveIdx = m_FreeRanges.GetSize() - 2;
             moveIdx > index; --moveIdx)
        {
            m_FreeRanges[moveIdx] = m_FreeRanges[moveIdx - 1];
        }
        m_FreeRanges[index] = range;
    }

    TArray<FRange> m_FreeRanges;  ///< 空闲范围列表 (按偏移排序)
    SizeType       m_TotalSize;   ///< 总管理大小
};

} // namespace Limx
