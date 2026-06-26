/*******************************************************************************
 * 文件: TFlatMap.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   扁平映射 — 基于排序数组的有序键值对容器
 *   键值对在连续内存中按键排序存储，二分查找 O(log N)
 *   用于小到中等规模的有序映射，缓存友好，读多写少场景
 *
 * 设计哲学:
 *   排序数组 — 键值对紧凑存储，利用缓存局部性
 *   二分查找 — 查找复杂度 O(log N)
 *   稳定键序 — 遍历顺序即为键的排序顺序
 *
 * 技术特性:
 *   - TFlatMap<KeyType, ValueType, CompareLess>: 排序数组映射
 *   - Insert: 有序插入 (已存在则覆盖)
 *   - Find: 二分查找
 *   - Remove: 移除键值对
 *   - operator[]: 查找或插入
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Containers/TArray.h,
 *          Core/Templates/TPair.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Containers/TArray.h"
#include "Core/Templates/TPair.h"

namespace Limx
{

/// 扁平映射
/// @tparam KeyType 键类型 (需要 operator<)
/// @tparam ValueType 值类型
template<typename KeyType, typename ValueType>
class TFlatMap
{
    using FPairType = TPair<KeyType, ValueType>;

public:
    TFlatMap() = default;

    // ========================================================================
    // 插入
    // ========================================================================

    /// 插入键值对 (已存在则覆盖)
    void Insert(const KeyType& key, const ValueType& value)
    {
        SizeType idx = LowerBound(key);

        if (idx < m_Data.GetSize() &&
            !KeyLess(key, m_Data[idx].Key) &&
            !KeyLess(m_Data[idx].Key, key))
        {
            // 已存在 — 覆盖值
            m_Data[idx].Value = value;
            return;
        }

        InsertAt(idx, FPairType(key, value));
    }

    /// 插入键值对 (移动值)
    void Insert(const KeyType& key, ValueType&& value)
    {
        SizeType idx = LowerBound(key);

        if (idx < m_Data.GetSize() &&
            !KeyLess(key, m_Data[idx].Key) &&
            !KeyLess(m_Data[idx].Key, key))
        {
            m_Data[idx].Value = MoveTemp(value);
            return;
        }

        InsertAt(idx, FPairType(key, MoveTemp(value)));
    }

    // ========================================================================
    // 查找
    // ========================================================================

    /// 查找值 (可写)
    LIMX_NODISCARD ValueType* Find(const KeyType& key)
    {
        SizeType idx = LowerBound(key);
        if (idx < m_Data.GetSize() &&
            !KeyLess(key, m_Data[idx].Key) &&
            !KeyLess(m_Data[idx].Key, key))
        {
            return &m_Data[idx].Value;
        }
        return nullptr;
    }

    /// 查找值 (只读)
    LIMX_NODISCARD const ValueType* Find(
        const KeyType& key) const
    {
        SizeType idx = LowerBound(key);
        if (idx < m_Data.GetSize() &&
            !KeyLess(key, m_Data[idx].Key) &&
            !KeyLess(m_Data[idx].Key, key))
        {
            return &m_Data[idx].Value;
        }
        return nullptr;
    }

    /// 是否包含键
    LIMX_NODISCARD bool Contains(const KeyType& key) const
    {
        return Find(key) != nullptr;
    }

    /// 下标访问 (不存在则默认构造插入)
    ValueType& operator[](const KeyType& key)
    {
        SizeType idx = LowerBound(key);
        if (idx < m_Data.GetSize() &&
            !KeyLess(key, m_Data[idx].Key) &&
            !KeyLess(m_Data[idx].Key, key))
        {
            return m_Data[idx].Value;
        }
        InsertAt(idx, FPairType(key, ValueType()));
        return m_Data[idx].Value;
    }

    // ========================================================================
    // 删除
    // ========================================================================

    /// 移除键值对
    bool Remove(const KeyType& key)
    {
        SizeType idx = LowerBound(key);
        if (idx < m_Data.GetSize() &&
            !KeyLess(key, m_Data[idx].Key) &&
            !KeyLess(m_Data[idx].Key, key))
        {
            m_Data.RemoveAt(idx);
            return true;
        }
        return false;
    }

    /// 清空
    void Clear() { m_Data.Clear(); }

    // ========================================================================
    // 访问
    // ========================================================================

    /// 按索引访问键值对
    LIMX_NODISCARD const FPairType& GetPairAt(
        SizeType index) const
    {
        return m_Data[index];
    }

    /// 按索引访问键
    LIMX_NODISCARD const KeyType& GetKeyAt(
        SizeType index) const
    {
        return m_Data[index].Key;
    }

    /// 按索引访问值
    LIMX_NODISCARD const ValueType& GetValueAt(
        SizeType index) const
    {
        return m_Data[index].Value;
    }

    /// 按索引访问值 (可写)
    LIMX_NODISCARD ValueType& GetValueAt(SizeType index)
    {
        return m_Data[index].Value;
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
    /// 键比较
    static bool KeyLess(const KeyType& a, const KeyType& b)
    {
        return a < b;
    }

    /// 二分查找 lower_bound
    LIMX_NODISCARD SizeType LowerBound(
        const KeyType& key) const
    {
        SizeType low = 0;
        SizeType high = m_Data.GetSize();

        while (low < high)
        {
            SizeType mid = low + (high - low) / 2;
            if (KeyLess(m_Data[mid].Key, key))
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

    /// 在指定位置插入
    void InsertAt(SizeType index, FPairType&& pair)
    {
        m_Data.Add(FPairType());
        for (SizeType moveIdx = m_Data.GetSize() - 1;
             moveIdx > index; --moveIdx)
        {
            m_Data[moveIdx] = MoveTemp(m_Data[moveIdx - 1]);
        }
        m_Data[index] = MoveTemp(pair);
    }

    TArray<FPairType> m_Data;  ///< 排序键值对数组
};

} // namespace Limx
