/*******************************************************************************
 * 文件: TFixedString.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   固定长度字符串 — 编译时容量的栈存储字符串
 *   无堆分配，适合高频创建/销毁的短字符串场景
 *   用于标签名、组件名、调试标注、日志前缀等固定长度场景
 *
 * 设计哲学:
 *   栈存储 — 编译时确定最大容量，内嵌字符数组
 *   零分配 — 不使用堆，构造/拷贝均为值语义
 *   FString 兼容 — 提供 GetCStr() 接口，可隐式转为 FString
 *
 * 技术特性:
 *   - TFixedString<Capacity>: 固定容量字符串
 *   - Append: 追加字符/字符串
 *   - GetCStr: 获取 C 字符串
 *   - GetLength/GetCapacity: 查询
 *   - operator==: 比较
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

/// 固定长度字符串
/// @tparam Capacity 最大字符数 (不含终止符)
template<SizeType Capacity>
class TFixedString
{
    static_assert(Capacity > 0,
        "TFixedString capacity must be > 0");

public:
    /// 默认构造 — 空字符串
    TFixedString()
        : m_Length(0)
    {
        m_Data[0] = '\0';
    }

    /// 从 C 字符串构造
    TFixedString(const AnsiChar* str)
        : m_Length(0)
    {
        if (str != nullptr)
        {
            while (m_Length < Capacity && str[m_Length] != '\0')
            {
                m_Data[m_Length] = str[m_Length];
                ++m_Length;
            }
        }
        m_Data[m_Length] = '\0';
    }

    /// 从指定长度的字符数组构造
    TFixedString(const AnsiChar* str, SizeType length)
        : m_Length(0)
    {
        if (str != nullptr)
        {
            SizeType copyLen = (length < Capacity)
                ? length : Capacity;
            for (SizeType charIdx = 0;
                 charIdx < copyLen; ++charIdx)
            {
                m_Data[charIdx] = str[charIdx];
            }
            m_Length = copyLen;
        }
        m_Data[m_Length] = '\0';
    }

    // ========================================================================
    // 修改
    // ========================================================================

    /// 追加单个字符
    /// @return 是否成功 (false = 容量不足)
    bool AppendChar(AnsiChar ch)
    {
        if (m_Length >= Capacity) return false;
        m_Data[m_Length] = ch;
        ++m_Length;
        m_Data[m_Length] = '\0';
        return true;
    }

    /// 追加 C 字符串
    /// @return 实际追加的字符数
    SizeType Append(const AnsiChar* str)
    {
        if (str == nullptr) return 0;
        SizeType appendCount = 0;
        while (m_Length < Capacity && str[appendCount] != '\0')
        {
            m_Data[m_Length] = str[appendCount];
            ++m_Length;
            ++appendCount;
        }
        m_Data[m_Length] = '\0';
        return appendCount;
    }

    /// 追加另一个 TFixedString
    template<SizeType OtherCapacity>
    SizeType Append(const TFixedString<OtherCapacity>& other)
    {
        return Append(other.GetCStr());
    }

    /// 清空
    void Clear()
    {
        m_Length = 0;
        m_Data[0] = '\0';
    }

    // ========================================================================
    // 访问
    // ========================================================================

    /// 获取 C 字符串
    LIMX_NODISCARD const AnsiChar* GetCStr() const
    {
        return m_Data;
    }

    /// 按索引访问字符
    LIMX_NODISCARD AnsiChar operator[](
        SizeType index) const
    {
        LIMX_ASSERT(index < m_Length);
        return m_Data[index];
    }

    /// 按索引访问字符 (可写)
    LIMX_NODISCARD AnsiChar& operator[](SizeType index)
    {
        LIMX_ASSERT(index < m_Length);
        return m_Data[index];
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 当前长度
    LIMX_NODISCARD SizeType GetLength() const
    {
        return m_Length;
    }

    /// 最大容量
    LIMX_NODISCARD static constexpr SizeType GetCapacity()
    {
        return Capacity;
    }

    /// 剩余可用空间
    LIMX_NODISCARD SizeType GetRemaining() const
    {
        return Capacity - m_Length;
    }

    /// 是否为空
    LIMX_NODISCARD bool IsEmpty() const
    {
        return m_Length == 0;
    }

    /// 是否已满
    LIMX_NODISCARD bool IsFull() const
    {
        return m_Length >= Capacity;
    }

    // ========================================================================
    // 比较
    // ========================================================================

    template<SizeType OtherCapacity>
    LIMX_NODISCARD bool operator==(
        const TFixedString<OtherCapacity>& other) const
    {
        if (m_Length != other.GetLength()) return false;
        const AnsiChar* otherStr = other.GetCStr();
        for (SizeType charIdx = 0;
             charIdx < m_Length; ++charIdx)
        {
            if (m_Data[charIdx] != otherStr[charIdx])
                return false;
        }
        return true;
    }

    template<SizeType OtherCapacity>
    LIMX_NODISCARD bool operator!=(
        const TFixedString<OtherCapacity>& other) const
    {
        return !(*this == other);
    }

    LIMX_NODISCARD bool operator==(
        const AnsiChar* str) const
    {
        if (str == nullptr) return m_Length == 0;
        SizeType cmpIdx = 0;
        while (cmpIdx < m_Length && str[cmpIdx] != '\0')
        {
            if (m_Data[cmpIdx] != str[cmpIdx]) return false;
            ++cmpIdx;
        }
        return cmpIdx == m_Length && str[cmpIdx] == '\0';
    }

    LIMX_NODISCARD bool operator!=(
        const AnsiChar* str) const
    {
        return !(*this == str);
    }

private:
    AnsiChar m_Data[Capacity + 1];  ///< 字符数组 (+1 终止符)
    SizeType m_Length;               ///< 当前长度
};

/// 常用别名
using FFixedString32 = TFixedString<32>;
using FFixedString64 = TFixedString<64>;
using FFixedString128 = TFixedString<128>;
using FFixedString256 = TFixedString<256>;

} // namespace Limx
