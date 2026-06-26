/*******************************************************************************
 * 文件: TFlatSet.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   扁平集合 — 基于排序数组的有序集合
 *   元素在连续内存中排序存储，二分查找 O(log N)
 *   用于小到中等规模的有序唯一元素集合，缓存友好
 *
 * 设计哲学:
 *   排序数组 — 元素紧凑存储，利用缓存局部性
 *   二分查找 — 查找复杂度 O(log N)
 *   插入/删除 O(N) — 需要移动元素，适合读多写少场景
 *
 * 技术特性:
 *   - TFlatSet<T, CompareLess>: 排序数组集合
 *   - Insert: 有序插入 (去重)
 *   - Remove: 移除元素
 *   - Contains: 二分查找
 *   - GetData: 底层数组访问
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

/// 默认比较器 — 要求 T 支持 operator<
template<typename T>
struct TDefaultLess
{
    LIMX_NODISCARD bool operator()(
        const T& a, const T& b) const
    {
        return a < b;
    }
};

/// 扁平集合
/// @tparam T 元素类型 (需要 operator< 和 operator==)
/// @tparam CompareLess 比较器
template<typename T, typename CompareLess = TDefaultLess<T>>
class TFlatSet
{
public:
    TFlatSet() = default;

    // ========================================================================
    // 插入
    // ========================================================================

    /// 有序插入 (去重)
    /// @return 是否插入成功 (false = 已存在)
    bool Insert(const T& element)
    {
        SizeType insertIndex = LowerBound(element);

        // 检查是否已存在
        if (insertIndex < m_Data.GetSize() &&
            !m_Less(element, m_Data[insertIndex]) &&
            !m_Less(m_Data[insertIndex], element))
        {
            return false;
        }

        // 在 insertIndex 处插入
        InsertAt(insertIndex, element);
        return true;
    }

    /// 有序插入 (移动)
    bool Insert(T&& element)
    {
        SizeType insertIndex = LowerBound(element);

        if (insertIndex < m_Data.GetSize() &&
            !m_Less(element, m_Data[insertIndex]) &&
            !m_Less(m_Data[insertIndex], element))
        {
            return false;
        }

        InsertAt(insertIndex, MoveTemp(element));
        return true;
    }

    // ========================================================================
    // 删除
    // ========================================================================

    /// 移除元素
    bool Remove(const T& element)
    {
        SizeType foundIndex = FindIndex(element);
        if (foundIndex == kNotFound) return false;
        m_Data.RemoveAt(foundIndex);
        return true;
    }

    /// 清空
    void Clear() { m_Data.Clear(); }

    // ========================================================================
    // 查找
    // ========================================================================

    /// 是否包含元素
    LIMX_NODISCARD bool Contains(const T& element) const
    {
        return FindIndex(element) != kNotFound;
    }

    /// 查找元素索引
    /// @return 索引，未找到返回 kNotFound
    LIMX_NODISCARD SizeType FindIndex(const T& element) const
    {
        SizeType idx = LowerBound(element);
        if (idx < m_Data.GetSize() &&
            !m_Less(element, m_Data[idx]) &&
            !m_Less(m_Data[idx], element))
        {
            return idx;
        }
        return kNotFound;
    }

    static constexpr SizeType kNotFound =
        static_cast<SizeType>(-1);

    // ========================================================================
    // 访问
    // ========================================================================

    /// 按索引访问
    LIMX_NODISCARD const T& operator[](SizeType index) const
    {
        return m_Data[index];
    }

    /// 底层数组 (只读)
    LIMX_NODISCARD const TArray<T>& GetData() const
    {
        return m_Data;
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 元素数量
    LIMX_NODISCARD SizeType GetCount() const
    {
        return m_Data.GetSize();
    }

    /// 是否为空
    LIMX_NODISCARD bool IsEmpty() const
    {
        return m_Data.GetSize() == 0;
    }

    /// 预分配容量
    void Reserve(SizeType capacity)
    {
        m_Data.Reserve(capacity);
    }

private:
    /// 二分查找 lower_bound — 第一个不小于 element 的位置
    LIMX_NODISCARD SizeType LowerBound(
        const T& element) const
    {
        SizeType low = 0;
        SizeType high = m_Data.GetSize();

        while (low < high)
        {
            SizeType mid = low + (high - low) / 2;
            if (m_Less(m_Data[mid], element))
            {
                low = mid + 1;
            }
            else
            {
                high = mid;
            }
        }
        return low;
    }

    /// 在指定位置插入 (拷贝)
    void InsertAt(SizeType index, const T& element)
    {
        m_Data.Add(T()); // 扩展一个位置
        // 后移
        for (SizeType moveIndex = m_Data.GetSize() - 1;
             moveIndex > index; --moveIndex)
        {
            m_Data[moveIndex] = MoveTemp(m_Data[moveIndex - 1]);
        }
        m_Data[index] = element;
    }

    /// 在指定位置插入 (移动)
    void InsertAt(SizeType index, T&& element)
    {
        m_Data.Add(T());
        for (SizeType moveIndex = m_Data.GetSize() - 1;
             moveIndex > index; --moveIndex)
        {
            m_Data[moveIndex] = MoveTemp(m_Data[moveIndex - 1]);
        }
        m_Data[index] = MoveTemp(element);
    }

    TArray<T>   m_Data;  ///< 排序数组
    CompareLess m_Less;  ///< 比较器
};

} // namespace Limx
