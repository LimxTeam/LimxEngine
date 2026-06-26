/*******************************************************************************
 * 文件: FStringView.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   非拥有字符串视图 — 对已有字符数据的零拷贝引用
 *   不管理内存，仅持有指针和长度
 *   用于函数参数传递、字符串切片、解析器 Token 等避免拷贝的场景
 *
 * 设计哲学:
 *   零拷贝 — 不分配内存，不拷贝字符数据
 *   轻量值类型 — 16 字节 (指针 + 长度)，按值传递
 *   只读 — 不提供修改底层数据的方法
 *
 * 技术特性:
 *   - 从 const AnsiChar* 和 FString 隐式构造
 *   - Substr: 子串视图
 *   - StartsWith/EndsWith: 前缀/后缀匹配
 *   - Find/Contains: 子串查找
 *   - Trim: 去除首尾空白
 *   - operator==: 内容比较
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"

namespace Limx
{

// 前向声明
class FString;

/// 非拥有字符串视图
class FStringView
{
public:
    /// 无效索引
    static constexpr SizeType kNPos = static_cast<SizeType>(-1);

    // ========================================================================
    // 构造
    // ========================================================================

    /// 默认构造 — 空视图
    constexpr FStringView() : m_Data(nullptr), m_Length(0) {}

    /// 从 C 字符串构造 (自动计算长度)
    FStringView(const AnsiChar* str)
        : m_Data(str)
        , m_Length(str ? StringLength(str) : 0)
    {
    }

    /// 从指针和长度构造
    constexpr FStringView(const AnsiChar* str, SizeType length)
        : m_Data(str)
        , m_Length(length)
    {
    }

    // ========================================================================
    // 访问
    // ========================================================================

    /// 获取数据指针 (不保证 null 终止)
    LIMX_NODISCARD constexpr const AnsiChar* GetData() const
    {
        return m_Data;
    }

    /// 获取长度
    LIMX_NODISCARD constexpr SizeType GetLength() const
    {
        return m_Length;
    }

    /// 是否为空
    LIMX_NODISCARD constexpr bool IsEmpty() const
    {
        return m_Length == 0;
    }

    /// 索引访问
    LIMX_NODISCARD constexpr AnsiChar operator[](SizeType index) const
    {
        return m_Data[index];
    }

    /// 首字符
    LIMX_NODISCARD constexpr AnsiChar Front() const
    {
        return m_Data[0];
    }

    /// 尾字符
    LIMX_NODISCARD constexpr AnsiChar Back() const
    {
        return m_Data[m_Length - 1];
    }

    // ========================================================================
    // 子串
    // ========================================================================

    /// 从 offset 开始的子串视图
    LIMX_NODISCARD FStringView Substr(SizeType offset,
                                        SizeType count = kNPos) const
    {
        if (offset >= m_Length)
        {
            return FStringView();
        }
        SizeType remaining = m_Length - offset;
        SizeType actualCount = count < remaining ? count : remaining;
        return FStringView(m_Data + offset, actualCount);
    }

    /// 左侧 N 个字符
    LIMX_NODISCARD FStringView Left(SizeType count) const
    {
        SizeType actual = count < m_Length ? count : m_Length;
        return FStringView(m_Data, actual);
    }

    /// 右侧 N 个字符
    LIMX_NODISCARD FStringView Right(SizeType count) const
    {
        SizeType actual = count < m_Length ? count : m_Length;
        return FStringView(m_Data + m_Length - actual, actual);
    }

    // ========================================================================
    // 查找
    // ========================================================================

    /// 查找字符首次出现的位置
    LIMX_NODISCARD SizeType Find(AnsiChar ch,
                                   SizeType startPos = 0) const
    {
        for (SizeType index = startPos; index < m_Length; ++index)
        {
            if (m_Data[index] == ch)
            {
                return index;
            }
        }
        return kNPos;
    }

    /// 查找子串首次出现的位置
    LIMX_NODISCARD SizeType Find(FStringView other,
                                   SizeType startPos = 0) const
    {
        if (other.m_Length == 0) return startPos;
        if (other.m_Length > m_Length) return kNPos;

        SizeType searchEnd = m_Length - other.m_Length + 1;
        for (SizeType index = startPos; index < searchEnd; ++index)
        {
            bool matched = true;
            for (SizeType inner = 0; inner < other.m_Length; ++inner)
            {
                if (m_Data[index + inner] != other.m_Data[inner])
                {
                    matched = false;
                    break;
                }
            }
            if (matched) return index;
        }
        return kNPos;
    }

    /// 是否包含字符
    LIMX_NODISCARD bool Contains(AnsiChar ch) const
    {
        return Find(ch) != kNPos;
    }

    /// 是否包含子串
    LIMX_NODISCARD bool Contains(FStringView other) const
    {
        return Find(other) != kNPos;
    }

    // ========================================================================
    // 前缀/后缀
    // ========================================================================

    /// 是否以指定前缀开头
    LIMX_NODISCARD bool StartsWith(FStringView prefix) const
    {
        if (prefix.m_Length > m_Length) return false;
        for (SizeType index = 0; index < prefix.m_Length; ++index)
        {
            if (m_Data[index] != prefix.m_Data[index])
            {
                return false;
            }
        }
        return true;
    }

    /// 是否以指定后缀结尾
    LIMX_NODISCARD bool EndsWith(FStringView suffix) const
    {
        if (suffix.m_Length > m_Length) return false;
        SizeType offset = m_Length - suffix.m_Length;
        for (SizeType index = 0; index < suffix.m_Length; ++index)
        {
            if (m_Data[offset + index] != suffix.m_Data[index])
            {
                return false;
            }
        }
        return true;
    }

    // ========================================================================
    // 修剪
    // ========================================================================

    /// 去除首尾空白
    LIMX_NODISCARD FStringView Trim() const
    {
        SizeType start = 0;
        SizeType end = m_Length;
        while (start < end && IsWhitespace(m_Data[start]))
        {
            ++start;
        }
        while (end > start && IsWhitespace(m_Data[end - 1]))
        {
            --end;
        }
        return FStringView(m_Data + start, end - start);
    }

    /// 去除左侧空白
    LIMX_NODISCARD FStringView TrimLeft() const
    {
        SizeType start = 0;
        while (start < m_Length && IsWhitespace(m_Data[start]))
        {
            ++start;
        }
        return FStringView(m_Data + start, m_Length - start);
    }

    /// 去除右侧空白
    LIMX_NODISCARD FStringView TrimRight() const
    {
        SizeType end = m_Length;
        while (end > 0 && IsWhitespace(m_Data[end - 1]))
        {
            --end;
        }
        return FStringView(m_Data, end);
    }

    // ========================================================================
    // 比较
    // ========================================================================

    LIMX_NODISCARD bool operator==(FStringView other) const
    {
        if (m_Length != other.m_Length) return false;
        for (SizeType index = 0; index < m_Length; ++index)
        {
            if (m_Data[index] != other.m_Data[index])
            {
                return false;
            }
        }
        return true;
    }

    LIMX_NODISCARD bool operator!=(FStringView other) const
    {
        return !(*this == other);
    }

    // ========================================================================
    // 迭代器
    // ========================================================================

    LIMX_NODISCARD const AnsiChar* begin() const { return m_Data; }
    LIMX_NODISCARD const AnsiChar* end() const
    {
        return m_Data + m_Length;
    }

private:
    /// 空白字符判断
    static bool IsWhitespace(AnsiChar ch)
    {
        return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
    }

    /// C 字符串长度
    static SizeType StringLength(const AnsiChar* str)
    {
        SizeType length = 0;
        while (str[length] != '\0') { ++length; }
        return length;
    }

    const AnsiChar* m_Data;    ///< 字符数据指针
    SizeType        m_Length;  ///< 长度
};

} // namespace Limx
