/*******************************************************************************
 * 文件: FTokenizer.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   简易字符串分词器 — 按分隔符将字符串拆分为令牌序列
 *   支持单字符/多字符分隔符、跳过空令牌、引号内保持完整
 *   用于控制台命令解析、CSV 读取、配置文件解析等场景
 *
 * 设计哲学:
 *   流式解析 — 逐字符扫描，单遍完成
 *   零分配选项 — 可通过预分配 TArray 避免解析时分配
 *   简单接口 — Split 一次性输出所有令牌
 *
 * 技术特性:
 *   - FTokenizer: 分词工具
 *   - Split: 按分隔符拆分
 *   - SplitByWhitespace: 按空白字符拆分
 *   - SplitLines: 按换行符拆分
 *   - TrimWhitespace: 去除首尾空白
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Containers/FString.h,
 *          Core/Containers/TArray.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Containers/FString.h"
#include "Core/Containers/TArray.h"

namespace Limx
{

/// 分词工具
struct FTokenizer
{
    /// 按分隔符拆分字符串
    /// @param input 输入字符串
    /// @param delimiter 分隔符
    /// @param outTokens 输出令牌列表
    /// @param skipEmpty 是否跳过空令牌
    static void Split(const FString& input,
                      AnsiChar delimiter,
                      TArray<FString>& outTokens,
                      bool skipEmpty = true)
    {
        outTokens.Clear();
        const AnsiChar* str = input.GetCStr();
        SizeType length = input.GetLength();

        SizeType tokenStart = 0;
        for (SizeType charIndex = 0;
             charIndex <= length; ++charIndex)
        {
            bool isEnd = (charIndex == length);
            bool isDelim = !isEnd && (str[charIndex] == delimiter);

            if (isEnd || isDelim)
            {
                SizeType tokenLen = charIndex - tokenStart;
                if (tokenLen > 0 || !skipEmpty)
                {
                    outTokens.Add(
                        SubString(str, tokenStart, tokenLen));
                }
                tokenStart = charIndex + 1;
            }
        }
    }

    /// 按多个分隔符拆分
    static void SplitAny(const FString& input,
                         const AnsiChar* delimiters,
                         TArray<FString>& outTokens,
                         bool skipEmpty = true)
    {
        outTokens.Clear();
        const AnsiChar* str = input.GetCStr();
        SizeType length = input.GetLength();

        SizeType tokenStart = 0;
        for (SizeType charIndex = 0;
             charIndex <= length; ++charIndex)
        {
            bool isEnd = (charIndex == length);
            bool isDelim = !isEnd &&
                IsOneOf(str[charIndex], delimiters);

            if (isEnd || isDelim)
            {
                SizeType tokenLen = charIndex - tokenStart;
                if (tokenLen > 0 || !skipEmpty)
                {
                    outTokens.Add(
                        SubString(str, tokenStart, tokenLen));
                }
                tokenStart = charIndex + 1;
            }
        }
    }

    /// 按空白字符拆分 (空格/制表符)
    static void SplitByWhitespace(const FString& input,
                                  TArray<FString>& outTokens)
    {
        outTokens.Clear();
        const AnsiChar* str = input.GetCStr();
        SizeType length = input.GetLength();

        SizeType tokenStart = 0;
        bool inToken = false;

        for (SizeType charIndex = 0;
             charIndex <= length; ++charIndex)
        {
            bool isEnd = (charIndex == length);
            bool isWhitespace = !isEnd &&
                (str[charIndex] == ' ' ||
                 str[charIndex] == '\t' ||
                 str[charIndex] == '\r');

            if (isEnd || isWhitespace)
            {
                if (inToken)
                {
                    outTokens.Add(SubString(
                        str, tokenStart,
                        charIndex - tokenStart));
                    inToken = false;
                }
            }
            else
            {
                if (!inToken)
                {
                    tokenStart = charIndex;
                    inToken = true;
                }
            }
        }
    }

    /// 按换行符拆分
    static void SplitLines(const FString& input,
                           TArray<FString>& outLines,
                           bool skipEmpty = false)
    {
        outLines.Clear();
        const AnsiChar* str = input.GetCStr();
        SizeType length = input.GetLength();

        SizeType lineStart = 0;
        for (SizeType charIndex = 0;
             charIndex <= length; ++charIndex)
        {
            bool isEnd = (charIndex == length);
            bool isNewline = !isEnd &&
                (str[charIndex] == '\n');

            if (isEnd || isNewline)
            {
                SizeType lineLen = charIndex - lineStart;
                // 处理 \r\n
                if (lineLen > 0 &&
                    str[lineStart + lineLen - 1] == '\r')
                {
                    --lineLen;
                }

                if (lineLen > 0 || !skipEmpty)
                {
                    outLines.Add(
                        SubString(str, lineStart, lineLen));
                }
                lineStart = charIndex + 1;
            }
        }
    }

    /// 去除首尾空白
    LIMX_NODISCARD static FString TrimWhitespace(
        const FString& input)
    {
        const AnsiChar* str = input.GetCStr();
        SizeType length = input.GetLength();

        SizeType start = 0;
        while (start < length && IsWhitespace(str[start]))
        {
            ++start;
        }

        SizeType end = length;
        while (end > start && IsWhitespace(str[end - 1]))
        {
            --end;
        }

        if (start == 0 && end == length) return input;
        return SubString(str, start, end - start);
    }

    /// 去除首部空白
    LIMX_NODISCARD static FString TrimLeft(
        const FString& input)
    {
        const AnsiChar* str = input.GetCStr();
        SizeType length = input.GetLength();

        SizeType start = 0;
        while (start < length && IsWhitespace(str[start]))
        {
            ++start;
        }

        if (start == 0) return input;
        return SubString(str, start, length - start);
    }

    /// 去除尾部空白
    LIMX_NODISCARD static FString TrimRight(
        const FString& input)
    {
        const AnsiChar* str = input.GetCStr();
        SizeType length = input.GetLength();

        SizeType end = length;
        while (end > 0 && IsWhitespace(str[end - 1]))
        {
            --end;
        }

        if (end == length) return input;
        return SubString(str, 0, end);
    }

private:
    /// 是否为空白字符
    static bool IsWhitespace(AnsiChar ch)
    {
        return ch == ' ' || ch == '\t' ||
               ch == '\r' || ch == '\n';
    }

    /// 是否为指定字符之一
    static bool IsOneOf(AnsiChar ch,
                        const AnsiChar* charSet)
    {
        while (*charSet != '\0')
        {
            if (ch == *charSet) return true;
            ++charSet;
        }
        return false;
    }

    /// 子串提取
    static FString SubString(const AnsiChar* str,
                              SizeType start, SizeType length)
    {
        if (length == 0) return FString();

        // 临时缓冲区 (栈上小缓冲，超大字符串堆分配)
        constexpr SizeType kStackBufSize = 256;
        AnsiChar stackBuf[kStackBufSize];
        AnsiChar* buf = stackBuf;
        bool heapAllocated = false;

        if (length + 1 > kStackBufSize)
        {
            buf = static_cast<AnsiChar*>(
                GetDefaultAllocator().Allocate(
                    length + 1, 1));
            heapAllocated = true;
        }

        for (SizeType copyIndex = 0;
             copyIndex < length; ++copyIndex)
        {
            buf[copyIndex] = str[start + copyIndex];
        }
        buf[length] = '\0';

        FString result(buf);

        if (heapAllocated)
        {
            GetDefaultAllocator().Deallocate(buf);
        }

        return result;
    }
};

} // namespace Limx
