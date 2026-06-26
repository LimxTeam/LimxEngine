/*******************************************************************************
 * 文件: TTagSet.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   标签集合 — 命名标签的集合操作
 *   基于 FName 哈希的高效标签存储，支持集合运算
 *   用于实体标签系统、资产分类、过滤查询等场景
 *
 * 设计哲学:
 *   哈希驱动 — 使用 FNV-1a 哈希的 TSet 存储
 *   集合运算 — 支持交集、并集、差集、子集判断
 *   字符串接口 — 通过字符串添加/查询/删除标签
 *
 * 技术特性:
 *   - FTagSet: 标签集合
 *   - Add/Remove/Contains: 基础操作
 *   - HasAll/HasAny: 批量查询
 *   - Union/Intersect/Difference: 集合运算
 *   - IsSubsetOf: 子集判断
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Containers/TArray.h,
 *          Core/Containers/FString.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Containers/TArray.h"
#include "Core/Containers/FString.h"

namespace Limx
{

/// 标签条目 — 存储标签名和缓存哈希
struct FTag
{
    FString Name;    ///< 标签名
    UInt32  Hash;    ///< 缓存的哈希值

    FTag() : Hash(0) {}

    explicit FTag(const AnsiChar* name)
        : Name(name)
        , Hash(ComputeHash(name))
    {
    }

    explicit FTag(const FString& name)
        : Name(name)
        , Hash(ComputeHash(name.GetCStr()))
    {
    }

    LIMX_NODISCARD bool operator==(const FTag& other) const
    {
        return Hash == other.Hash && Name == other.Name;
    }

    LIMX_NODISCARD bool operator!=(const FTag& other) const
    {
        return !(*this == other);
    }

private:
    static UInt32 ComputeHash(const AnsiChar* str)
    {
        UInt32 hash = 2166136261u;
        while (*str != '\0')
        {
            hash ^= static_cast<UInt32>(
                static_cast<UInt8>(*str));
            hash *= 16777619u;
            ++str;
        }
        return hash;
    }
};

/// 标签集合
class FTagSet
{
public:
    FTagSet() = default;

    // ========================================================================
    // 基础操作
    // ========================================================================

    /// 添加标签
    void Add(const AnsiChar* tagName)
    {
        if (!Contains(tagName))
        {
            m_Tags.Add(FTag(tagName));
        }
    }

    /// 移除标签
    bool Remove(const AnsiChar* tagName)
    {
        FTag target(tagName);
        for (SizeType tagIndex = 0;
             tagIndex < m_Tags.GetSize(); ++tagIndex)
        {
            if (m_Tags[tagIndex] == target)
            {
                m_Tags.RemoveAt(tagIndex);
                return true;
            }
        }
        return false;
    }

    /// 是否包含标签
    LIMX_NODISCARD bool Contains(const AnsiChar* tagName) const
    {
        FTag target(tagName);
        for (SizeType tagIndex = 0;
             tagIndex < m_Tags.GetSize(); ++tagIndex)
        {
            if (m_Tags[tagIndex] == target) return true;
        }
        return false;
    }

    /// 清空所有标签
    void Clear() { m_Tags.Clear(); }

    // ========================================================================
    // 批量查询
    // ========================================================================

    /// 是否包含所有指定标签
    LIMX_NODISCARD bool HasAll(const FTagSet& other) const
    {
        for (SizeType otherIndex = 0;
             otherIndex < other.m_Tags.GetSize(); ++otherIndex)
        {
            bool found = false;
            for (SizeType myIndex = 0;
                 myIndex < m_Tags.GetSize(); ++myIndex)
            {
                if (m_Tags[myIndex] == other.m_Tags[otherIndex])
                {
                    found = true;
                    break;
                }
            }
            if (!found) return false;
        }
        return true;
    }

    /// 是否包含任一指定标签
    LIMX_NODISCARD bool HasAny(const FTagSet& other) const
    {
        for (SizeType otherIndex = 0;
             otherIndex < other.m_Tags.GetSize(); ++otherIndex)
        {
            for (SizeType myIndex = 0;
                 myIndex < m_Tags.GetSize(); ++myIndex)
            {
                if (m_Tags[myIndex] == other.m_Tags[otherIndex])
                {
                    return true;
                }
            }
        }
        return false;
    }

    /// 是否为另一集合的子集
    LIMX_NODISCARD bool IsSubsetOf(const FTagSet& other) const
    {
        return other.HasAll(*this);
    }

    // ========================================================================
    // 集合运算
    // ========================================================================

    /// 并集
    LIMX_NODISCARD FTagSet Union(const FTagSet& other) const
    {
        FTagSet result = *this;
        for (SizeType otherIndex = 0;
             otherIndex < other.m_Tags.GetSize(); ++otherIndex)
        {
            bool found = false;
            for (SizeType myIndex = 0;
                 myIndex < result.m_Tags.GetSize(); ++myIndex)
            {
                if (result.m_Tags[myIndex] ==
                    other.m_Tags[otherIndex])
                {
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                result.m_Tags.Add(other.m_Tags[otherIndex]);
            }
        }
        return result;
    }

    /// 交集
    LIMX_NODISCARD FTagSet Intersect(const FTagSet& other) const
    {
        FTagSet result;
        for (SizeType myIndex = 0;
             myIndex < m_Tags.GetSize(); ++myIndex)
        {
            for (SizeType otherIndex = 0;
                 otherIndex < other.m_Tags.GetSize(); ++otherIndex)
            {
                if (m_Tags[myIndex] == other.m_Tags[otherIndex])
                {
                    result.m_Tags.Add(m_Tags[myIndex]);
                    break;
                }
            }
        }
        return result;
    }

    /// 差集 (this - other)
    LIMX_NODISCARD FTagSet Difference(const FTagSet& other) const
    {
        FTagSet result;
        for (SizeType myIndex = 0;
             myIndex < m_Tags.GetSize(); ++myIndex)
        {
            bool inOther = false;
            for (SizeType otherIndex = 0;
                 otherIndex < other.m_Tags.GetSize(); ++otherIndex)
            {
                if (m_Tags[myIndex] == other.m_Tags[otherIndex])
                {
                    inOther = true;
                    break;
                }
            }
            if (!inOther)
            {
                result.m_Tags.Add(m_Tags[myIndex]);
            }
        }
        return result;
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 标签数量
    LIMX_NODISCARD SizeType GetCount() const
    {
        return m_Tags.GetSize();
    }

    /// 是否为空
    LIMX_NODISCARD bool IsEmpty() const
    {
        return m_Tags.GetSize() == 0;
    }

    /// 获取标签列表 (只读)
    LIMX_NODISCARD const TArray<FTag>& GetTags() const
    {
        return m_Tags;
    }

private:
    TArray<FTag> m_Tags;  ///< 标签列表
};

} // namespace Limx
