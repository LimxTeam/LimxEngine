/*******************************************************************************
 * 文件: TIntervalTree.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   区间树 — 高效区间重叠查询数据结构
 *   支持插入区间、查询与给定区间重叠的所有区间
 *   用于碰撞检测宽阶段、时间线事件查询、渲染裁剪等场景
 *
 * 设计哲学:
 *   平坦存储 — 所有区间存于 TArray，查询时线性扫描
 *   适度优化 — 按区间起点排序，提前终止扫描
 *   简单接口 — Insert/Query/Remove/Clear
 *
 * 技术特性:
 *   - TInterval<T>: 区间 [Low, High]
 *   - TIntervalTree<T, DataType>: 区间树
 *   - Insert: 插入带数据的区间
 *   - QueryOverlap: 查询与给定区间重叠的所有条目
 *   - QueryPoint: 查询包含给定点的所有条目
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

/// 区间 [Low, High]
/// @tparam T 端点类型 (需要 < 和 <= 操作符)
template<typename T>
struct TInterval
{
    T Low;   ///< 下界
    T High;  ///< 上界

    TInterval() : Low(), High() {}
    TInterval(T low, T high) : Low(low), High(high) {}

    /// 是否与另一区间重叠
    LIMX_NODISCARD bool Overlaps(const TInterval& other) const
    {
        return Low <= other.High && other.Low <= High;
    }

    /// 是否包含点
    LIMX_NODISCARD bool Contains(T point) const
    {
        return Low <= point && point <= High;
    }

    /// 区间宽度
    LIMX_NODISCARD T GetWidth() const
    {
        return High - Low;
    }
};

/// 区间树条目
template<typename T, typename DataType>
struct TIntervalEntry
{
    TInterval<T> Interval;  ///< 区间
    DataType     Data;      ///< 关联数据
};

/// 区间树
/// @tparam T 端点类型
/// @tparam DataType 关联数据类型
template<typename T, typename DataType>
class TIntervalTree
{
public:
    using FEntry = TIntervalEntry<T, DataType>;

    TIntervalTree() : m_IsSorted(false) {}

    // ========================================================================
    // 插入与删除
    // ========================================================================

    /// 插入区间
    void Insert(const TInterval<T>& interval,
                const DataType& data)
    {
        FEntry entry;
        entry.Interval = interval;
        entry.Data = data;
        m_Entries.Add(entry);
        m_IsSorted = false;
    }

    /// 插入区间 (移动数据)
    void Insert(const TInterval<T>& interval,
                DataType&& data)
    {
        FEntry entry;
        entry.Interval = interval;
        entry.Data = MoveTemp(data);
        m_Entries.Add(MoveTemp(entry));
        m_IsSorted = false;
    }

    /// 移除索引处的条目
    void RemoveAt(SizeType index)
    {
        m_Entries.RemoveAt(index);
        // 排序状态保持 (RemoveAt 不破坏相对顺序)
    }

    /// 清空
    void Clear()
    {
        m_Entries.Clear();
        m_IsSorted = false;
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 查询与给定区间重叠的所有条目
    void QueryOverlap(const TInterval<T>& query,
                      TArray<FEntry>& outResults) const
    {
        EnsureSorted();

        for (SizeType entryIndex = 0;
             entryIndex < m_Entries.GetSize(); ++entryIndex)
        {
            const FEntry& entry = m_Entries[entryIndex];

            // 如果已排序且当前起点 > 查询上界，后续不可能重叠
            if (m_IsSorted && entry.Interval.Low > query.High)
            {
                break;
            }

            if (entry.Interval.Overlaps(query))
            {
                outResults.Add(entry);
            }
        }
    }

    /// 查询包含给定点的所有条目
    void QueryPoint(T point,
                    TArray<FEntry>& outResults) const
    {
        EnsureSorted();

        for (SizeType entryIndex = 0;
             entryIndex < m_Entries.GetSize(); ++entryIndex)
        {
            const FEntry& entry = m_Entries[entryIndex];

            if (m_IsSorted && entry.Interval.Low > point)
            {
                break;
            }

            if (entry.Interval.Contains(point))
            {
                outResults.Add(entry);
            }
        }
    }

    /// 条目数
    LIMX_NODISCARD SizeType GetCount() const
    {
        return m_Entries.GetSize();
    }

    /// 是否为空
    LIMX_NODISCARD bool IsEmpty() const
    {
        return m_Entries.GetSize() == 0;
    }

    /// 获取条目 (只读)
    LIMX_NODISCARD const FEntry& GetEntry(
        SizeType index) const
    {
        return m_Entries[index];
    }

    /// 重建排序 (显式调用以优化后续查询)
    void RebuildSort()
    {
        SortEntries();
        m_IsSorted = true;
    }

private:
    /// 确保排序 (惰性排序)
    void EnsureSorted() const
    {
        if (!m_IsSorted && m_Entries.GetSize() > 1)
        {
            // const_cast 仅修改排序状态
            auto* mutableThis = const_cast<TIntervalTree*>(this);
            mutableThis->SortEntries();
            mutableThis->m_IsSorted = true;
        }
    }

    /// 按区间起点排序 (插入排序 — 适合小到中等规模)
    void SortEntries()
    {
        for (SizeType sortIndex = 1;
             sortIndex < m_Entries.GetSize(); ++sortIndex)
        {
            FEntry key = MoveTemp(m_Entries[sortIndex]);
            SizeType insertIndex = sortIndex;

            while (insertIndex > 0 &&
                   m_Entries[insertIndex - 1].Interval.Low >
                       key.Interval.Low)
            {
                m_Entries[insertIndex] =
                    MoveTemp(m_Entries[insertIndex - 1]);
                --insertIndex;
            }

            m_Entries[insertIndex] = MoveTemp(key);
        }
    }

    TArray<FEntry> m_Entries;   ///< 条目列表
    bool           m_IsSorted;  ///< 是否已排序
};

} // namespace Limx
