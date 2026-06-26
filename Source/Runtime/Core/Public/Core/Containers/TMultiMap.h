/*******************************************************************************
 * 文件: TMultiMap.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   一键多值哈希表 — 同一个键可以关联多个值
 *   内部基于 TArray 存储键值对，线性探测查找同键元素
 *   用于事件系统 (一个事件多个监听器)、标签系统、多对多关系等场景
 *
 * 设计哲学:
 *   扁平存储 — 所有键值对存储在单个 TArray<TPair> 中
 *   同键聚合 — 同一个键的多个值通过线性扫描查找
 *   简洁接口 — Add 添加、FindAll 获取所有匹配值、Remove 删除
 *
 * 技术特性:
 *   - Add(key, value): 添加键值对 (不去重)
 *   - FindAll(key, outValues): 获取键对应的所有值
 *   - Contains(key): 检查键是否存在
 *   - Remove(key): 删除所有匹配键的条目
 *   - RemoveOne(key, value): 删除一个匹配的键值对
 *   - GetCount: 总条目数
 *
 * 依赖关系:
 *   内部: Core/Containers/TArray.h, Core/Templates/TPair.h
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

/// 一键多值哈希表
/// @tparam K 键类型 (需要支持 == 比较)
/// @tparam V 值类型
template<typename K, typename V>
class TMultiMap
{
    using PairType = TPair<K, V>;

public:
    // ========================================================================
    // 构造
    // ========================================================================

    TMultiMap() = default;
    ~TMultiMap() = default;

    TMultiMap(const TMultiMap&) = default;
    TMultiMap& operator=(const TMultiMap&) = default;

    TMultiMap(TMultiMap&& other) noexcept
        : m_Entries(MoveTemp(other.m_Entries))
    {
    }

    TMultiMap& operator=(TMultiMap&& other) noexcept
    {
        if (this != &other)
        {
            m_Entries = MoveTemp(other.m_Entries);
        }
        return *this;
    }

    // ========================================================================
    // 添加
    // ========================================================================

    /// 添加键值对 (不去重，同键可多次添加)
    void Add(const K& key, const V& value)
    {
        m_Entries.Add(MakePair(key, value));
    }

    /// 添加键值对 (移动语义)
    void Add(K&& key, V&& value)
    {
        m_Entries.Add(MakePair(MoveTemp(key), MoveTemp(value)));
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 获取键对应的所有值
    /// @return 找到的值数量
    SizeType FindAll(const K& key, TArray<V>& outValues) const
    {
        SizeType count = 0;
        for (SizeType index = 0;
             index < m_Entries.GetSize(); ++index)
        {
            if (m_Entries[index].First == key)
            {
                outValues.Add(m_Entries[index].Second);
                ++count;
            }
        }
        return count;
    }

    /// 获取键对应的第一个值
    LIMX_NODISCARD V* Find(const K& key)
    {
        for (SizeType index = 0;
             index < m_Entries.GetSize(); ++index)
        {
            if (m_Entries[index].First == key)
            {
                return &m_Entries[index].Second;
            }
        }
        return nullptr;
    }

    LIMX_NODISCARD const V* Find(const K& key) const
    {
        for (SizeType index = 0;
             index < m_Entries.GetSize(); ++index)
        {
            if (m_Entries[index].First == key)
            {
                return &m_Entries[index].Second;
            }
        }
        return nullptr;
    }

    /// 检查键是否存在
    LIMX_NODISCARD bool Contains(const K& key) const
    {
        for (SizeType index = 0;
             index < m_Entries.GetSize(); ++index)
        {
            if (m_Entries[index].First == key)
            {
                return true;
            }
        }
        return false;
    }

    /// 统计键出现的次数
    LIMX_NODISCARD SizeType CountKey(const K& key) const
    {
        SizeType count = 0;
        for (SizeType index = 0;
             index < m_Entries.GetSize(); ++index)
        {
            if (m_Entries[index].First == key)
            {
                ++count;
            }
        }
        return count;
    }

    // ========================================================================
    // 删除
    // ========================================================================

    /// 删除所有匹配键的条目
    /// @return 删除的条目数
    SizeType Remove(const K& key)
    {
        SizeType removed = 0;
        SizeType writeIndex = 0;

        for (SizeType readIndex = 0;
             readIndex < m_Entries.GetSize(); ++readIndex)
        {
            if (m_Entries[readIndex].First == key)
            {
                ++removed;
            }
            else
            {
                if (writeIndex != readIndex)
                {
                    m_Entries[writeIndex] =
                        MoveTemp(m_Entries[readIndex]);
                }
                ++writeIndex;
            }
        }

        // 缩减数组大小
        while (m_Entries.GetSize() > writeIndex)
        {
            m_Entries.RemoveLast();
        }

        return removed;
    }

    /// 删除第一个匹配的键值对
    /// @return 是否找到并删除
    bool RemoveOne(const K& key, const V& value)
    {
        for (SizeType index = 0;
             index < m_Entries.GetSize(); ++index)
        {
            if (m_Entries[index].First == key &&
                m_Entries[index].Second == value)
            {
                // 与最后一个交换并删除
                if (index + 1 < m_Entries.GetSize())
                {
                    m_Entries[index] = MoveTemp(
                        m_Entries[m_Entries.GetSize() - 1]);
                }
                m_Entries.RemoveLast();
                return true;
            }
        }
        return false;
    }

    // ========================================================================
    // 状态
    // ========================================================================

    /// 总条目数 (含同键重复)
    LIMX_NODISCARD SizeType GetCount() const
    {
        return m_Entries.GetSize();
    }

    /// 是否为空
    LIMX_NODISCARD bool IsEmpty() const
    {
        return m_Entries.IsEmpty();
    }

    /// 清空所有条目
    void Clear()
    {
        m_Entries.Clear();
    }

    // ========================================================================
    // 遍历
    // ========================================================================

    /// 对所有条目执行回调
    template<typename Func>
    void ForEach(Func&& func) const
    {
        for (SizeType index = 0;
             index < m_Entries.GetSize(); ++index)
        {
            func(m_Entries[index].First,
                 m_Entries[index].Second);
        }
    }

private:
    TArray<PairType> m_Entries;  ///< 键值对列表
};

} // namespace Limx
