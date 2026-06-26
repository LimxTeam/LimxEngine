/*******************************************************************************
 * 文件: FStringConverter.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   字符串转换 — 数值与字符串之间的双向转换
 *   提供 Int32/Int64/Float32/Float64/Bool 到字符串的格式化输出
 *   以及字符串到数值的解析
 *   用于控制台命令解析、配置文件读写、日志输出等场景
 *
 * 设计哲学:
 *   零 STL — 手写整数/浮点转换，不依赖 sprintf/sscanf
 *   静态方法 — 所有方法为静态工具函数，无状态
 *   FString 输出 — 转换结果为 FString
 *
 * 技术特性:
 *   - ToString: Int32/Int64/Float32/Float64/Bool -> FString
 *   - ToInt32/ToInt64/ToFloat32/ToFloat64/ToBool: FString -> 数值
 *   - 整数转换支持负数和零
 *   - 浮点转换提供可配置精度
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Containers/FString.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Containers/FString.h"

namespace Limx
{

/// 字符串转换工具
struct FStringConverter
{
    // ========================================================================
    // 数值 -> 字符串
    // ========================================================================

    /// Int32 转字符串
    LIMX_NODISCARD static FString ToString(Int32 value)
    {
        if (value == 0) return FString("0");

        AnsiChar buffer[12]; // -2147483648 最多 11 字符 + '\0'
        SizeType writeIndex = 0;
        bool isNegative = false;

        if (value < 0)
        {
            isNegative = true;
            // 处理 INT_MIN 特殊情况
            if (value == -2147483647 - 1)
            {
                return FString("-2147483648");
            }
            value = -value;
        }

        // 逆序写入数字
        while (value > 0)
        {
            buffer[writeIndex++] =
                static_cast<AnsiChar>('0' + (value % 10));
            value /= 10;
        }

        if (isNegative)
        {
            buffer[writeIndex++] = '-';
        }

        // 反转
        for (SizeType reverseIndex = 0;
             reverseIndex < writeIndex / 2; ++reverseIndex)
        {
            AnsiChar temp = buffer[reverseIndex];
            buffer[reverseIndex] =
                buffer[writeIndex - 1 - reverseIndex];
            buffer[writeIndex - 1 - reverseIndex] = temp;
        }

        buffer[writeIndex] = '\0';
        return FString(buffer);
    }

    /// Int64 转字符串
    LIMX_NODISCARD static FString ToString(Int64 value)
    {
        if (value == 0) return FString("0");

        AnsiChar buffer[21]; // -9223372036854775808 最多 20 字符
        SizeType writeIndex = 0;
        bool isNegative = false;

        if (value < 0)
        {
            isNegative = true;
            if (value == static_cast<Int64>(-9223372036854775807ll - 1))
            {
                return FString("-9223372036854775808");
            }
            value = -value;
        }

        while (value > 0)
        {
            buffer[writeIndex++] =
                static_cast<AnsiChar>('0' + (value % 10));
            value /= 10;
        }

        if (isNegative)
        {
            buffer[writeIndex++] = '-';
        }

        for (SizeType reverseIndex = 0;
             reverseIndex < writeIndex / 2; ++reverseIndex)
        {
            AnsiChar temp = buffer[reverseIndex];
            buffer[reverseIndex] =
                buffer[writeIndex - 1 - reverseIndex];
            buffer[writeIndex - 1 - reverseIndex] = temp;
        }

        buffer[writeIndex] = '\0';
        return FString(buffer);
    }

    /// UInt32 转字符串
    LIMX_NODISCARD static FString FromUInt32(UInt32 value)
    {
        if (value == 0) return FString("0");

        AnsiChar buffer[11];
        SizeType writeIndex = 0;

        while (value > 0)
        {
            buffer[writeIndex++] =
                static_cast<AnsiChar>('0' + (value % 10));
            value /= 10;
        }

        for (SizeType reverseIndex = 0;
             reverseIndex < writeIndex / 2; ++reverseIndex)
        {
            AnsiChar temp = buffer[reverseIndex];
            buffer[reverseIndex] =
                buffer[writeIndex - 1 - reverseIndex];
            buffer[writeIndex - 1 - reverseIndex] = temp;
        }

        buffer[writeIndex] = '\0';
        return FString(buffer);
    }

    /// Float32 转字符串
    /// @param precision 小数位数 (默认 6)
    LIMX_NODISCARD static FString FromFloat32(
        Float32 value, Int32 precision = 6)
    {
        return FloatToString(
            static_cast<Float64>(value), precision);
    }

    /// Float64 转字符串
    LIMX_NODISCARD static FString FromFloat64(
        Float64 value, Int32 precision = 6)
    {
        return FloatToString(value, precision);
    }

    /// Bool 转字符串
    LIMX_NODISCARD static FString FromBool(bool value)
    {
        return value ? FString("true") : FString("false");
    }

    // ========================================================================
    // 字符串 -> 数值
    // ========================================================================

    /// 字符串转 Int32
    /// @param outValue 输出值
    /// @return 是否解析成功
    LIMX_NODISCARD static bool ToInt32(
        const FString& str, Int32& outValue)
    {
        const AnsiChar* ptr = str.GetCStr();
        return ParseInt32(ptr, outValue);
    }

    /// 字符串转 Int64
    LIMX_NODISCARD static bool ToInt64(
        const FString& str, Int64& outValue)
    {
        const AnsiChar* ptr = str.GetCStr();
        return ParseInt64(ptr, outValue);
    }

    /// 字符串转 Float32
    LIMX_NODISCARD static bool ToFloat32(
        const FString& str, Float32& outValue)
    {
        Float64 temp = 0.0;
        if (!ParseFloat64(str.GetCStr(), temp)) return false;
        outValue = static_cast<Float32>(temp);
        return true;
    }

    /// 字符串转 Float64
    LIMX_NODISCARD static bool ToFloat64(
        const FString& str, Float64& outValue)
    {
        return ParseFloat64(str.GetCStr(), outValue);
    }

    /// 字符串转 Bool
    /// 接受: "true"/"1"/"yes" -> true, "false"/"0"/"no" -> false
    LIMX_NODISCARD static bool ToBool(
        const FString& str, bool& outValue)
    {
        const AnsiChar* ptr = str.GetCStr();

        if (CompareInsensitive(ptr, "true") ||
            CompareInsensitive(ptr, "1") ||
            CompareInsensitive(ptr, "yes"))
        {
            outValue = true;
            return true;
        }

        if (CompareInsensitive(ptr, "false") ||
            CompareInsensitive(ptr, "0") ||
            CompareInsensitive(ptr, "no"))
        {
            outValue = false;
            return true;
        }

        return false;
    }

private:
    // ========================================================================
    // 内部实现
    // ========================================================================

    /// 浮点转字符串
    static FString FloatToString(Float64 value, Int32 precision)
    {
        AnsiChar buffer[64];
        SizeType writeIndex = 0;

        // 处理负数
        if (value < 0.0)
        {
            buffer[writeIndex++] = '-';
            value = -value;
        }

        // 分离整数和小数部分
        Int64 integerPart = static_cast<Int64>(value);
        Float64 fractionalPart = value - static_cast<Float64>(integerPart);

        // 写入整数部分
        if (integerPart == 0)
        {
            buffer[writeIndex++] = '0';
        }
        else
        {
            AnsiChar intBuf[20];
            SizeType intLen = 0;
            Int64 temp = integerPart;
            while (temp > 0)
            {
                intBuf[intLen++] =
                    static_cast<AnsiChar>('0' + (temp % 10));
                temp /= 10;
            }
            for (SizeType reverseIndex = intLen;
                 reverseIndex > 0; --reverseIndex)
            {
                buffer[writeIndex++] = intBuf[reverseIndex - 1];
            }
        }

        // 写入小数部分
        if (precision > 0)
        {
            buffer[writeIndex++] = '.';

            for (Int32 digitIndex = 0;
                 digitIndex < precision; ++digitIndex)
            {
                fractionalPart *= 10.0;
                Int32 digit = static_cast<Int32>(fractionalPart);
                buffer[writeIndex++] =
                    static_cast<AnsiChar>('0' + digit);
                fractionalPart -= static_cast<Float64>(digit);
            }
        }

        buffer[writeIndex] = '\0';
        return FString(buffer);
    }

    /// 解析 Int32
    static bool ParseInt32(const AnsiChar* str, Int32& outValue)
    {
        if (str == nullptr || *str == '\0') return false;

        bool isNegative = false;
        if (*str == '-') { isNegative = true; ++str; }
        else if (*str == '+') { ++str; }

        if (*str == '\0') return false;

        Int32 result = 0;
        while (*str != '\0')
        {
            if (*str < '0' || *str > '9') return false;
            result = result * 10 + (*str - '0');
            ++str;
        }

        outValue = isNegative ? -result : result;
        return true;
    }

    /// 解析 Int64
    static bool ParseInt64(const AnsiChar* str, Int64& outValue)
    {
        if (str == nullptr || *str == '\0') return false;

        bool isNegative = false;
        if (*str == '-') { isNegative = true; ++str; }
        else if (*str == '+') { ++str; }

        if (*str == '\0') return false;

        Int64 result = 0;
        while (*str != '\0')
        {
            if (*str < '0' || *str > '9') return false;
            result = result * 10 + (*str - '0');
            ++str;
        }

        outValue = isNegative ? -result : result;
        return true;
    }

    /// 解析 Float64
    static bool ParseFloat64(const AnsiChar* str, Float64& outValue)
    {
        if (str == nullptr || *str == '\0') return false;

        bool isNegative = false;
        if (*str == '-') { isNegative = true; ++str; }
        else if (*str == '+') { ++str; }

        if (*str == '\0') return false;

        Float64 result = 0.0;
        bool hasDigits = false;

        // 整数部分
        while (*str >= '0' && *str <= '9')
        {
            result = result * 10.0 + (*str - '0');
            ++str;
            hasDigits = true;
        }

        // 小数部分
        if (*str == '.')
        {
            ++str;
            Float64 factor = 0.1;
            while (*str >= '0' && *str <= '9')
            {
                result += (*str - '0') * factor;
                factor *= 0.1;
                ++str;
                hasDigits = true;
            }
        }

        if (!hasDigits) return false;

        // 指数部分
        if (*str == 'e' || *str == 'E')
        {
            ++str;
            bool expNegative = false;
            if (*str == '-') { expNegative = true; ++str; }
            else if (*str == '+') { ++str; }

            Int32 exponent = 0;
            while (*str >= '0' && *str <= '9')
            {
                exponent = exponent * 10 + (*str - '0');
                ++str;
            }

            Float64 power = 1.0;
            for (Int32 powerIndex = 0;
                 powerIndex < exponent; ++powerIndex)
            {
                power *= 10.0;
            }

            if (expNegative)
                result /= power;
            else
                result *= power;
        }

        outValue = isNegative ? -result : result;
        return true;
    }

    /// 大小写不敏感比较
    static bool CompareInsensitive(
        const AnsiChar* a, const AnsiChar* b)
    {
        while (*a != '\0' && *b != '\0')
        {
            AnsiChar ca = *a;
            AnsiChar cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca += 32;
            if (cb >= 'A' && cb <= 'Z') cb += 32;
            if (ca != cb) return false;
            ++a;
            ++b;
        }
        return *a == '\0' && *b == '\0';
    }
};

} // namespace Limx
