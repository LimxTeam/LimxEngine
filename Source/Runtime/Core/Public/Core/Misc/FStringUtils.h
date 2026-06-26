/*******************************************************************************
 * 文件: FStringUtils.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   字符串工具集 — 常用字符串操作的静态方法集合
 *   提供大小写转换、前后缀检测、替换、重复等操作
 *   用于命令行解析、配置文件处理、日志格式化等场景
 *
 * 设计哲学:
 *   静态方法 — 无状态工具函数集
 *   FString 操作 — 基于引擎 FString 类
 *   零 STL — 不依赖任何标准库
 *
 * 技术特性:
 *   - FStringUtils: 字符串工具
 *   - ToUpper/ToLower: 大小写转换
 *   - StartsWith/EndsWith: 前后缀检测
 *   - Replace: 字符替换
 *   - Contains: 子串包含
 *   - Repeat: 重复字符串
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
#include "Core/Memory/DefaultAllocator.h"

namespace Limx
{

/// 字符串工具
struct FStringUtils
{
    // ========================================================================
    // 大小写转换
    // ========================================================================

    /// 转大写
    LIMX_NODISCARD static FString ToUpper(const FString& input)
    {
        const AnsiChar* src = input.GetCStr();
        SizeType length = input.GetLength();
        if (length == 0) return FString();

        AnsiChar* buf = AllocBuffer(length);
        for (SizeType charIdx = 0; charIdx < length; ++charIdx)
        {
            buf[charIdx] = ToUpperChar(src[charIdx]);
        }
        buf[length] = '\0';

        FString result(buf);
        FreeBuffer(buf);
        return result;
    }

    /// 转小写
    LIMX_NODISCARD static FString ToLower(const FString& input)
    {
        const AnsiChar* src = input.GetCStr();
        SizeType length = input.GetLength();
        if (length == 0) return FString();

        AnsiChar* buf = AllocBuffer(length);
        for (SizeType charIdx = 0; charIdx < length; ++charIdx)
        {
            buf[charIdx] = ToLowerChar(src[charIdx]);
        }
        buf[length] = '\0';

        FString result(buf);
        FreeBuffer(buf);
        return result;
    }

    // ========================================================================
    // 前后缀检测
    // ========================================================================

    /// 是否以指定前缀开头
    LIMX_NODISCARD static bool StartsWith(
        const FString& input, const FString& prefix)
    {
        SizeType prefixLen = prefix.GetLength();
        if (prefixLen > input.GetLength()) return false;
        if (prefixLen == 0) return true;

        const AnsiChar* inputStr = input.GetCStr();
        const AnsiChar* prefixStr = prefix.GetCStr();

        for (SizeType charIdx = 0;
             charIdx < prefixLen; ++charIdx)
        {
            if (inputStr[charIdx] != prefixStr[charIdx])
                return false;
        }
        return true;
    }

    /// 是否以指定后缀结尾
    LIMX_NODISCARD static bool EndsWith(
        const FString& input, const FString& suffix)
    {
        SizeType suffixLen = suffix.GetLength();
        SizeType inputLen = input.GetLength();
        if (suffixLen > inputLen) return false;
        if (suffixLen == 0) return true;

        const AnsiChar* inputStr = input.GetCStr();
        const AnsiChar* suffixStr = suffix.GetCStr();
        SizeType offset = inputLen - suffixLen;

        for (SizeType charIdx = 0;
             charIdx < suffixLen; ++charIdx)
        {
            if (inputStr[offset + charIdx] !=
                suffixStr[charIdx])
                return false;
        }
        return true;
    }

    // ========================================================================
    // 包含检测
    // ========================================================================

    /// 是否包含子串
    LIMX_NODISCARD static bool Contains(
        const FString& input, const FString& substring)
    {
        return FindFirst(input, substring) !=
            static_cast<SizeType>(-1);
    }

    /// 查找子串首次出现位置
    /// @return 位置索引，未找到返回 SizeType(-1)
    LIMX_NODISCARD static SizeType FindFirst(
        const FString& input, const FString& substring)
    {
        SizeType inputLen = input.GetLength();
        SizeType subLen = substring.GetLength();
        if (subLen == 0) return 0;
        if (subLen > inputLen)
            return static_cast<SizeType>(-1);

        const AnsiChar* inputStr = input.GetCStr();
        const AnsiChar* subStr = substring.GetCStr();

        for (SizeType startIdx = 0;
             startIdx <= inputLen - subLen; ++startIdx)
        {
            bool match = true;
            for (SizeType cmpIdx = 0;
                 cmpIdx < subLen; ++cmpIdx)
            {
                if (inputStr[startIdx + cmpIdx] !=
                    subStr[cmpIdx])
                {
                    match = false;
                    break;
                }
            }
            if (match) return startIdx;
        }

        return static_cast<SizeType>(-1);
    }

    // ========================================================================
    // 替换
    // ========================================================================

    /// 替换所有指定字符
    LIMX_NODISCARD static FString ReplaceChar(
        const FString& input, AnsiChar oldChar,
        AnsiChar newChar)
    {
        const AnsiChar* src = input.GetCStr();
        SizeType length = input.GetLength();
        if (length == 0) return FString();

        AnsiChar* buf = AllocBuffer(length);
        for (SizeType charIdx = 0; charIdx < length; ++charIdx)
        {
            buf[charIdx] = (src[charIdx] == oldChar)
                ? newChar : src[charIdx];
        }
        buf[length] = '\0';

        FString result(buf);
        FreeBuffer(buf);
        return result;
    }

    // ========================================================================
    // 重复
    // ========================================================================

    /// 重复字符串 N 次
    LIMX_NODISCARD static FString Repeat(
        const FString& input, SizeType count)
    {
        if (count == 0) return FString();
        if (count == 1) return input;

        SizeType unitLen = input.GetLength();
        SizeType totalLen = unitLen * count;

        AnsiChar* buf = AllocBuffer(totalLen);
        const AnsiChar* src = input.GetCStr();

        for (SizeType repeatIdx = 0;
             repeatIdx < count; ++repeatIdx)
        {
            for (SizeType charIdx = 0;
                 charIdx < unitLen; ++charIdx)
            {
                buf[repeatIdx * unitLen + charIdx] =
                    src[charIdx];
            }
        }
        buf[totalLen] = '\0';

        FString result(buf);
        FreeBuffer(buf);
        return result;
    }

    // ========================================================================
    // 比较
    // ========================================================================

    /// 不区分大小写比较
    LIMX_NODISCARD static bool EqualsIgnoreCase(
        const FString& a, const FString& b)
    {
        if (a.GetLength() != b.GetLength()) return false;

        const AnsiChar* strA = a.GetCStr();
        const AnsiChar* strB = b.GetCStr();
        SizeType length = a.GetLength();

        for (SizeType charIdx = 0;
             charIdx < length; ++charIdx)
        {
            if (ToLowerChar(strA[charIdx]) !=
                ToLowerChar(strB[charIdx]))
                return false;
        }
        return true;
    }

    // ========================================================================
    // 字符分类
    // ========================================================================

    LIMX_NODISCARD static bool IsAlpha(AnsiChar ch)
    {
        return (ch >= 'A' && ch <= 'Z') ||
               (ch >= 'a' && ch <= 'z');
    }

    LIMX_NODISCARD static bool IsDigit(AnsiChar ch)
    {
        return ch >= '0' && ch <= '9';
    }

    LIMX_NODISCARD static bool IsAlphaNumeric(AnsiChar ch)
    {
        return IsAlpha(ch) || IsDigit(ch);
    }

    LIMX_NODISCARD static bool IsWhitespace(AnsiChar ch)
    {
        return ch == ' ' || ch == '\t' ||
               ch == '\r' || ch == '\n';
    }

private:
    static AnsiChar ToUpperChar(AnsiChar ch)
    {
        return (ch >= 'a' && ch <= 'z')
            ? static_cast<AnsiChar>(ch - 32) : ch;
    }

    static AnsiChar ToLowerChar(AnsiChar ch)
    {
        return (ch >= 'A' && ch <= 'Z')
            ? static_cast<AnsiChar>(ch + 32) : ch;
    }

    static AnsiChar* AllocBuffer(SizeType length)
    {
        return static_cast<AnsiChar*>(
            GetDefaultAllocator().Allocate(length + 1, 1));
    }

    static void FreeBuffer(AnsiChar* buf)
    {
        GetDefaultAllocator().Deallocate(buf);
    }
};

} // namespace Limx
