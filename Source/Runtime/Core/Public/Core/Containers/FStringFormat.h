/*******************************************************************************
 * 文件: FStringFormat.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   类型安全字符串格式化 — 替代 printf/sprintf 的零 STL 依赖实现
 *   通过 FStringFormatArg 实现编译时类型擦除，运行时安全格式化
 *   格式占位符: {} (按顺序替换)
 *   主要用于日志系统、调试输出、错误消息等场景
 *
 * 设计哲学:
 *   类型安全 — 无格式说明符，不可能类型不匹配
 *   零 STL — 所有转换手工实现，不依赖 <cstdio> 或 <sstream>
 *   可扩展 — 通过 FStringFormatArg 构造函数重载扩展新类型
 *
 * 技术特性:
 *   - FStringFormatArg: 类型擦除的格式化参数 (内联 64 字节缓冲区)
 *   - StringFormat(fmt, args...): 变参模板格式化入口
 *   - 支持类型: Int32, Int64, UInt32, UInt64, Float32, Float64,
 *               bool, const char*, FString, void*
 *   - 占位符: {} 按顺序替换
 *
 * 依赖关系:
 *   内部: Core/Containers/FString.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Memory/MemoryOps.h"
#include "Core/Containers/FString.h"

namespace Limx
{

/// 格式化参数 — 将各种类型转换为字符串片段
/// 内联 64 字节缓冲区，大多数数值转换不触发堆分配
/// 十六进制格式化包装
///
/// StringFormat 的占位符只有 {} 一种形式，不解析 {:X} 之类的格式说明符。
/// 图形编程中格式枚举、内存类型掩码、资源句柄几乎都以十六进制阅读，
/// 因此用一个显式包装类型表达意图: StringFormat("掩码 {}", FHex(mask))。
struct FHex
{
    /// 待格式化的值
    UInt64 Value = 0;

    /// 最少输出的十六进制位数, 不足时前补零; 0 表示不补
    UInt32 MinDigits = 0;

    explicit FHex(UInt64 value, UInt32 minDigits = 0)
        : Value(value)
        , MinDigits(minDigits)
    {
    }

    explicit FHex(UInt32 value, UInt32 minDigits = 0)
        : Value(static_cast<UInt64>(value))
        , MinDigits(minDigits)
    {
    }

    explicit FHex(Int32 value, UInt32 minDigits = 0)
        : Value(static_cast<UInt64>(static_cast<UInt32>(value)))
        , MinDigits(minDigits)
    {
    }
};

class FStringFormatArg
{
    static constexpr SizeType kBufferSize = 64;

public:
    // ========================================================================
    // 从各种类型构造
    // ========================================================================

    FStringFormatArg(Int32 value)
    {
        m_Length = FormatInt64(m_Buffer, kBufferSize,
                              static_cast<Int64>(value));
    }

    FStringFormatArg(UInt32 value)
    {
        m_Length = FormatUInt64(m_Buffer, kBufferSize,
                               static_cast<UInt64>(value));
    }

    FStringFormatArg(Int64 value)
    {
        m_Length = FormatInt64(m_Buffer, kBufferSize, value);
    }

    FStringFormatArg(UInt64 value)
    {
        m_Length = FormatUInt64(m_Buffer, kBufferSize, value);
    }

    FStringFormatArg(Float32 value)
    {
        m_Length = FormatFloat(m_Buffer, kBufferSize,
                              static_cast<Float64>(value), 6);
    }

    FStringFormatArg(Float64 value)
    {
        m_Length = FormatFloat(m_Buffer, kBufferSize, value, 6);
    }

    FStringFormatArg(bool value)
    {
        if (value)
        {
            m_Buffer[0] = 't'; m_Buffer[1] = 'r';
            m_Buffer[2] = 'u'; m_Buffer[3] = 'e';
            m_Buffer[4] = '\0';
            m_Length = 4;
        }
        else
        {
            m_Buffer[0] = 'f'; m_Buffer[1] = 'a';
            m_Buffer[2] = 'l'; m_Buffer[3] = 's';
            m_Buffer[4] = 'e'; m_Buffer[5] = '\0';
            m_Length = 5;
        }
    }

    FStringFormatArg(const AnsiChar* str)
    {
        if (str)
        {
            SizeType len = 0;
            while (str[len] != '\0')
            {
                len++;
            }
            if (len < kBufferSize)
            {
                Memory::MemCopy(m_Buffer, str, len);
                m_Buffer[len] = '\0';
                m_Length = len;
            }
            else
            {
                // 超出内联缓冲区 — 存储外部指针 (生命周期由调用者保证)
                m_ExternalData = str;
                m_Length = len;
            }
        }
        else
        {
            m_Buffer[0] = '('; m_Buffer[1] = 'n';
            m_Buffer[2] = 'u'; m_Buffer[3] = 'l';
            m_Buffer[4] = 'l'; m_Buffer[5] = ')';
            m_Buffer[6] = '\0';
            m_Length = 6;
        }
    }

    FStringFormatArg(const FString& str)
        : FStringFormatArg(str.GetCStr())
    {
    }

    FStringFormatArg(const void* pointer)
    {
        m_Buffer[0] = '0';
        m_Buffer[1] = 'x';
        m_Length = 2 + FormatHex(m_Buffer + 2, kBufferSize - 2,
                                  reinterpret_cast<UInt64>(pointer));
        m_Buffer[m_Length] = '\0';
    }

    FStringFormatArg(const FHex& hex)
    {
        m_Buffer[0] = '0';
        m_Buffer[1] = 'x';

        const SizeType digits = FormatHex(m_Buffer + 2, kBufferSize - 2,
                                          hex.Value);

        // 需要补零时把已写入的数字整体右移, 空出的高位填 '0'
        if (hex.MinDigits > digits && (2 + hex.MinDigits) < kBufferSize)
        {
            const SizeType padding = hex.MinDigits - digits;

            for (SizeType i = digits; i > 0; --i)
            {
                m_Buffer[2 + padding + i - 1] = m_Buffer[2 + i - 1];
            }

            for (SizeType i = 0; i < padding; ++i)
            {
                m_Buffer[2 + i] = '0';
            }

            m_Length = 2 + hex.MinDigits;
        }
        else
        {
            m_Length = 2 + digits;
        }

        m_Buffer[m_Length] = '\0';
    }

    // ========================================================================
    // 访问
    // ========================================================================

    LIMX_NODISCARD const AnsiChar* GetData() const
    {
        return m_ExternalData != nullptr ? m_ExternalData : m_Buffer;
    }
    LIMX_NODISCARD SizeType GetLength() const { return m_Length; }

private:
    // ========================================================================
    // 数值转字符串辅助
    // ========================================================================

    /// 有符号整数转字符串
    static SizeType FormatInt64(AnsiChar* buffer, SizeType bufferSize,
                                 Int64 value)
    {
        if (value < 0)
        {
            buffer[0] = '-';
            SizeType len = FormatUInt64(
                buffer + 1, bufferSize - 1,
                static_cast<UInt64>(-value));
            return len + 1;
        }
        return FormatUInt64(buffer, bufferSize, static_cast<UInt64>(value));
    }

    /// 无符号整数转字符串
    static SizeType FormatUInt64(AnsiChar* buffer, SizeType bufferSize,
                                  UInt64 value)
    {
        if (value == 0)
        {
            buffer[0] = '0';
            buffer[1] = '\0';
            return 1;
        }

        // 反转写入
        AnsiChar temp[20];  // UInt64 最多 20 位
        SizeType len = 0;
        while (value > 0 && len < 20)
        {
            temp[len++] = static_cast<AnsiChar>('0' + (value % 10));
            value /= 10;
        }

        SizeType writeLen = (len < bufferSize - 1) ? len : bufferSize - 1;
        for (SizeType index = 0; index < writeLen; ++index)
        {
            buffer[index] = temp[len - 1 - index];
        }
        buffer[writeLen] = '\0';
        return writeLen;
    }

    /// 浮点数转字符串 (简化实现 — 整数部分 + 小数部分)
    static SizeType FormatFloat(AnsiChar* buffer, SizeType bufferSize,
                                 Float64 value, Int32 precision)
    {
        SizeType pos = 0;

        // 处理特殊值
        // NaN 检测: NaN != NaN
        if (value != value)
        {
            buffer[0] = 'N'; buffer[1] = 'a'; buffer[2] = 'N';
            buffer[3] = '\0';
            return 3;
        }

        // 处理负数
        if (value < 0.0)
        {
            buffer[pos++] = '-';
            value = -value;
        }

        // 无穷大检测
        if (value > 1.0e18)
        {
            buffer[pos++] = 'i'; buffer[pos++] = 'n'; buffer[pos++] = 'f';
            buffer[pos] = '\0';
            return pos;
        }

        // 整数部分
        UInt64 intPart = static_cast<UInt64>(value);
        Float64 fracPart = value - static_cast<Float64>(intPart);

        // 写整数部分
        SizeType intLen = FormatUInt64(buffer + pos, bufferSize - pos, intPart);
        pos += intLen;

        // 小数点
        if (precision > 0 && pos < bufferSize - 1)
        {
            buffer[pos++] = '.';

            // 小数部分
            for (Int32 digit = 0;
                 digit < precision && pos < bufferSize - 1;
                 ++digit)
            {
                fracPart *= 10.0;
                Int32 d = static_cast<Int32>(fracPart);
                buffer[pos++] = static_cast<AnsiChar>('0' + d);
                fracPart -= static_cast<Float64>(d);
            }

            // 去除尾部零
            while (pos > 0 && buffer[pos - 1] == '0')
            {
                pos--;
            }
            if (pos > 0 && buffer[pos - 1] == '.')
            {
                pos--;
            }
        }

        buffer[pos] = '\0';
        return pos;
    }

    /// 十六进制格式化
    static SizeType FormatHex(AnsiChar* buffer, SizeType bufferSize,
                               UInt64 value)
    {
        static constexpr AnsiChar kHexDigits[] = "0123456789abcdef";

        if (value == 0)
        {
            buffer[0] = '0';
            buffer[1] = '\0';
            return 1;
        }

        AnsiChar temp[16];
        SizeType len = 0;
        while (value > 0 && len < 16)
        {
            temp[len++] = kHexDigits[value & 0xF];
            value >>= 4;
        }

        SizeType writeLen = (len < bufferSize - 1) ? len : bufferSize - 1;
        for (SizeType index = 0; index < writeLen; ++index)
        {
            buffer[index] = temp[len - 1 - index];
        }
        buffer[writeLen] = '\0';
        return writeLen;
    }

    // ========================================================================
    // 成员数据
    // ========================================================================

    AnsiChar m_Buffer[kBufferSize];          ///< 内联格式化缓冲区
    const AnsiChar* m_ExternalData = nullptr; ///< 超出内联缓冲区时的外部指针
    SizeType m_Length;                        ///< 格式化后的字节长度
};

// ============================================================================
// StringFormat — 类型安全格式化入口
// ============================================================================

namespace Detail
{

/// 格式化实现 — 扫描 {} 占位符并按顺序替换
inline FString FormatImpl(const AnsiChar* fmt,
                           const FStringFormatArg* args,
                           SizeType argCount)
{
    FString result;
    SizeType argIndex = 0;
    SizeType fmtIndex = 0;

    while (fmt[fmtIndex] != '\0')
    {
        if (fmt[fmtIndex] == '{' && fmt[fmtIndex + 1] == '}')
        {
            // 替换占位符
            if (argIndex < argCount)
            {
                result.Append(args[argIndex].GetData(),
                              args[argIndex].GetLength());
                argIndex++;
            }
            fmtIndex += 2;  // 跳过 {}
        }
        else
        {
            // 普通字符
            result.Append(&fmt[fmtIndex], 1);
            fmtIndex++;
        }
    }

    return result;
}

} // namespace Detail

/// 无参数格式化 — 直接返回格式字符串
LIMX_NODISCARD inline FString StringFormat(const AnsiChar* fmt)
{
    return FString(fmt);
}

/// 变参模板格式化
/// 用法: StringFormat("Player {} has {} HP", playerName, health)
template<typename... Args>
LIMX_NODISCARD FString StringFormat(const AnsiChar* fmt, Args&&... args)
{
    FStringFormatArg argArray[] = { FStringFormatArg(Forward<Args>(args))... };
    return Detail::FormatImpl(fmt, argArray, sizeof...(Args));
}

} // namespace Limx
