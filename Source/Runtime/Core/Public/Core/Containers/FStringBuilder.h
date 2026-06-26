/*******************************************************************************
 * 文件: FStringBuilder.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   高性能字符串拼接器 — 预分配缓冲区的可变字符串构建器
 *   避免频繁分配，支持链式追加多种类型
 *   用于日志格式化、路径拼接、序列化输出、调试信息构建等场景
 *
 * 设计哲学:
 *   预分配缓冲区 — 构造时指定初始容量，自动扩容但减少分配次数
 *   链式 API — 所有 Append 方法返回 *this，支持流式调用
 *   最终输出 — ToString() 生成 FString，MoveToString() 零拷贝转移
 *
 * 技术特性:
 *   - Append(const AnsiChar*): 追加 C 字符串
 *   - Append(const FString&): 追加引擎字符串
 *   - Append(AnsiChar): 追加单字符
 *   - AppendInt/AppendFloat: 追加数值
 *   - AppendLine: 追加并换行
 *   - ToString: 生成 FString 副本
 *   - Reset: 清空缓冲区 (不释放内存)
 *   - operator<<: 流式追加
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h,
 *          Core/Memory/MemoryOps.h, Core/Memory/DefaultAllocator.h,
 *          Core/Containers/FString.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Memory/MemoryOps.h"
#include "Core/Memory/DefaultAllocator.h"
#include "Core/Containers/FString.h"

namespace Limx
{

/// 高性能字符串拼接器
class FStringBuilder
{
    static constexpr SizeType kDefaultCapacity = 256;

public:
    // ========================================================================
    // 构造与析构
    // ========================================================================

    /// 默认构造 — 预分配默认容量
    FStringBuilder()
        : m_Buffer(nullptr)
        , m_Length(0)
        , m_Capacity(0)
        , m_Allocator(&GetDefaultAllocator())
    {
        Reserve(kDefaultCapacity);
    }

    /// 指定初始容量构造
    explicit FStringBuilder(SizeType initialCapacity)
        : m_Buffer(nullptr)
        , m_Length(0)
        , m_Capacity(0)
        , m_Allocator(&GetDefaultAllocator())
    {
        Reserve(initialCapacity > 0 ? initialCapacity : kDefaultCapacity);
    }

    ~FStringBuilder()
    {
        if (m_Buffer)
        {
            m_Allocator->Deallocate(m_Buffer);
        }
    }

    // 不可拷贝
    FStringBuilder(const FStringBuilder&) = delete;
    FStringBuilder& operator=(const FStringBuilder&) = delete;

    // 可移动
    FStringBuilder(FStringBuilder&& other) noexcept
        : m_Buffer(other.m_Buffer)
        , m_Length(other.m_Length)
        , m_Capacity(other.m_Capacity)
        , m_Allocator(other.m_Allocator)
    {
        other.m_Buffer = nullptr;
        other.m_Length = 0;
        other.m_Capacity = 0;
    }

    // ========================================================================
    // 追加操作 — 链式 API
    // ========================================================================

    /// 追加 C 字符串
    FStringBuilder& Append(const AnsiChar* str)
    {
        if (str)
        {
            SizeType length = StringLength(str);
            EnsureCapacity(m_Length + length);
            Memory::MemCopy(m_Buffer + m_Length, str, length);
            m_Length += length;
        }
        return *this;
    }

    /// 追加 C 字符串 (指定长度)
    FStringBuilder& Append(const AnsiChar* str, SizeType length)
    {
        if (str && length > 0)
        {
            EnsureCapacity(m_Length + length);
            Memory::MemCopy(m_Buffer + m_Length, str, length);
            m_Length += length;
        }
        return *this;
    }

    /// 追加 FString
    FStringBuilder& Append(const FString& str)
    {
        return Append(str.GetCStr(), str.GetLength());
    }

    /// 追加单字符
    FStringBuilder& Append(AnsiChar ch)
    {
        EnsureCapacity(m_Length + 1);
        m_Buffer[m_Length++] = ch;
        return *this;
    }

    /// 追加并换行
    FStringBuilder& AppendLine(const AnsiChar* str = nullptr)
    {
        if (str)
        {
            Append(str);
        }
        return Append('\n');
    }

    /// 追加 FString 并换行
    FStringBuilder& AppendLine(const FString& str)
    {
        Append(str);
        return Append('\n');
    }

    // ========================================================================
    // 数值追加
    // ========================================================================

    /// 追加有符号整数
    FStringBuilder& AppendInt(Int64 value)
    {
        // 最大 Int64 需要 20 位 + 符号 + 终止符
        AnsiChar digits[22];
        Int32 digitCount = 0;
        bool isNegative = false;

        if (value < 0)
        {
            isNegative = true;
            // 避免 -INT64_MIN 溢出
            UInt64 absValue = static_cast<UInt64>(-(value + 1)) + 1;
            do
            {
                digits[digitCount++] =
                    static_cast<AnsiChar>('0' + absValue % 10);
                absValue /= 10;
            } while (absValue > 0);
        }
        else
        {
            UInt64 uValue = static_cast<UInt64>(value);
            do
            {
                digits[digitCount++] =
                    static_cast<AnsiChar>('0' + uValue % 10);
                uValue /= 10;
            } while (uValue > 0);
        }

        EnsureCapacity(m_Length +
                        static_cast<SizeType>(digitCount) +
                        (isNegative ? 1 : 0));

        if (isNegative)
        {
            m_Buffer[m_Length++] = '-';
        }

        // 反向写入数字
        for (Int32 index = digitCount - 1; index >= 0; --index)
        {
            m_Buffer[m_Length++] = digits[index];
        }

        return *this;
    }

    /// 追加无符号整数
    FStringBuilder& AppendUInt(UInt64 value)
    {
        AnsiChar digits[22];
        Int32 digitCount = 0;

        do
        {
            digits[digitCount++] =
                static_cast<AnsiChar>('0' + value % 10);
            value /= 10;
        } while (value > 0);

        EnsureCapacity(m_Length + static_cast<SizeType>(digitCount));

        for (Int32 index = digitCount - 1; index >= 0; --index)
        {
            m_Buffer[m_Length++] = digits[index];
        }

        return *this;
    }

    /// 追加浮点数 (固定精度)
    FStringBuilder& AppendFloat(Float64 value, Int32 precision = 6)
    {
        // 处理特殊值
        if (value != value) // NaN
        {
            return Append("NaN");
        }

        // 负数处理
        if (value < 0.0)
        {
            Append('-');
            value = -value;
        }

        // 极大值简化处理
        if (value > 1.0e18)
        {
            return Append("Inf");
        }

        // 整数部分
        UInt64 intPart = static_cast<UInt64>(value);
        AppendUInt(intPart);

        // 小数部分
        if (precision > 0)
        {
            Append('.');

            Float64 fracPart = value - static_cast<Float64>(intPart);
            for (Int32 digit = 0; digit < precision; ++digit)
            {
                fracPart *= 10.0;
                Int32 digitValue = static_cast<Int32>(fracPart);
                Append(static_cast<AnsiChar>('0' + digitValue));
                fracPart -= static_cast<Float64>(digitValue);
            }
        }

        return *this;
    }

    // ========================================================================
    // 流式追加运算符
    // ========================================================================

    FStringBuilder& operator<<(const AnsiChar* str) { return Append(str); }
    FStringBuilder& operator<<(const FString& str) { return Append(str); }
    FStringBuilder& operator<<(AnsiChar ch) { return Append(ch); }
    FStringBuilder& operator<<(Int32 value) { return AppendInt(value); }
    FStringBuilder& operator<<(Int64 value) { return AppendInt(value); }
    FStringBuilder& operator<<(UInt32 value) { return AppendUInt(value); }
    FStringBuilder& operator<<(UInt64 value) { return AppendUInt(value); }
    FStringBuilder& operator<<(Float32 value)
    {
        return AppendFloat(static_cast<Float64>(value), 4);
    }
    FStringBuilder& operator<<(Float64 value)
    {
        return AppendFloat(value, 6);
    }
    FStringBuilder& operator<<(bool value)
    {
        return Append(value ? "true" : "false");
    }

    // ========================================================================
    // 输出
    // ========================================================================

    /// 生成 FString 副本
    LIMX_NODISCARD FString ToString() const
    {
        if (m_Length == 0)
        {
            return FString();
        }
        return FString(m_Buffer, m_Length);
    }

    /// 获取 C 字符串 (以 null 终止)
    LIMX_NODISCARD const AnsiChar* GetCStr()
    {
        EnsureCapacity(m_Length + 1);
        m_Buffer[m_Length] = '\0';
        return m_Buffer;
    }

    // ========================================================================
    // 状态管理
    // ========================================================================

    /// 当前长度
    LIMX_NODISCARD SizeType GetLength() const { return m_Length; }

    /// 当前容量
    LIMX_NODISCARD SizeType GetCapacity() const { return m_Capacity; }

    /// 是否为空
    LIMX_NODISCARD bool IsEmpty() const { return m_Length == 0; }

    /// 清空缓冲区 (不释放内存)
    void Reset()
    {
        m_Length = 0;
    }

    /// 预分配容量
    void Reserve(SizeType newCapacity)
    {
        if (newCapacity > m_Capacity)
        {
            AnsiChar* newBuffer = static_cast<AnsiChar*>(
                m_Allocator->Allocate(newCapacity, 1));
            if (m_Buffer && m_Length > 0)
            {
                Memory::MemCopy(newBuffer, m_Buffer, m_Length);
            }
            if (m_Buffer)
            {
                m_Allocator->Deallocate(m_Buffer);
            }
            m_Buffer = newBuffer;
            m_Capacity = newCapacity;
        }
    }

private:
    /// 确保容量足够
    void EnsureCapacity(SizeType required)
    {
        if (required > m_Capacity)
        {
            // 增长策略: 至少 2 倍，或满足需求
            SizeType newCapacity = m_Capacity * 2;
            if (newCapacity < required)
            {
                newCapacity = required;
            }
            Reserve(newCapacity);
        }
    }

    /// C 字符串长度
    static SizeType StringLength(const AnsiChar* str)
    {
        SizeType length = 0;
        while (str[length] != '\0')
        {
            ++length;
        }
        return length;
    }

    // ========================================================================
    // 成员数据
    // ========================================================================

    AnsiChar*   m_Buffer;     ///< 字符缓冲区
    SizeType    m_Length;      ///< 当前长度
    SizeType    m_Capacity;   ///< 缓冲区容量
    IAllocator* m_Allocator;  ///< 内存分配器
};

} // namespace Limx
