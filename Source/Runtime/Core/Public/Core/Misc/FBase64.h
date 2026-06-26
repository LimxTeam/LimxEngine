/*******************************************************************************
 * 文件: FBase64.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   Base64 编解码 — RFC 4648 标准实现
 *   提供二进制数据与 Base64 字符串之间的转换
 *   用于文本协议嵌入二进制、JSON 序列化、网络传输等场景
 *
 * 设计哲学:
 *   查表法 — 编码/解码均使用预计算查找表
 *   零 STL — 输出到 TArray<AnsiChar> 或 TArray<UInt8>
 *   标准兼容 — 使用标准 Base64 字母表 (A-Z a-z 0-9 + / =)
 *
 * 技术特性:
 *   - Encode: 二进制 → Base64 字符串
 *   - Decode: Base64 字符串 → 二进制
 *   - GetEncodedLength: 预计算编码后长度
 *   - GetMaxDecodedLength: 预计算解码后最大长度
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Containers/TArray.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Containers/TArray.h"

namespace Limx
{

/// Base64 编解码
struct FBase64
{
    // ========================================================================
    // 编码
    // ========================================================================

    /// 编码二进制数据为 Base64 字符串
    /// @param data   输入二进制数据
    /// @param length 数据长度
    /// @param output 输出字符数组 (含 null 终止)
    static void Encode(const UInt8* data, SizeType length,
                        TArray<AnsiChar>& output)
    {
        static constexpr AnsiChar kTable[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrst"
            "uvwxyz0123456789+/";

        SizeType encodedLength = GetEncodedLength(length);
        output.Clear();
        output.Reserve(encodedLength + 1);

        SizeType fullTriples = length / 3;
        SizeType remainder = length % 3;
        SizeType sourceIndex = 0;

        // 处理完整的 3 字节组
        for (SizeType triple = 0; triple < fullTriples; ++triple)
        {
            UInt32 block =
                (static_cast<UInt32>(data[sourceIndex + 0]) << 16) |
                (static_cast<UInt32>(data[sourceIndex + 1]) << 8) |
                 static_cast<UInt32>(data[sourceIndex + 2]);

            output.Add(kTable[(block >> 18) & 0x3F]);
            output.Add(kTable[(block >> 12) & 0x3F]);
            output.Add(kTable[(block >>  6) & 0x3F]);
            output.Add(kTable[(block >>  0) & 0x3F]);

            sourceIndex += 3;
        }

        // 处理尾部
        if (remainder == 2)
        {
            UInt32 block =
                (static_cast<UInt32>(data[sourceIndex + 0]) << 16) |
                (static_cast<UInt32>(data[sourceIndex + 1]) << 8);

            output.Add(kTable[(block >> 18) & 0x3F]);
            output.Add(kTable[(block >> 12) & 0x3F]);
            output.Add(kTable[(block >>  6) & 0x3F]);
            output.Add('=');
        }
        else if (remainder == 1)
        {
            UInt32 block =
                static_cast<UInt32>(data[sourceIndex + 0]) << 16;

            output.Add(kTable[(block >> 18) & 0x3F]);
            output.Add(kTable[(block >> 12) & 0x3F]);
            output.Add('=');
            output.Add('=');
        }

        // null 终止
        output.Add('\0');
    }

    // ========================================================================
    // 解码
    // ========================================================================

    /// 解码 Base64 字符串为二进制数据
    /// @param base64 输入 Base64 字符串 (null 终止)
    /// @param output 输出二进制数据
    /// @return 是否解码成功
    LIMX_NODISCARD static bool Decode(const AnsiChar* base64,
                                        TArray<UInt8>& output)
    {
        if (!base64) return false;

        SizeType inputLength = 0;
        while (base64[inputLength] != '\0') { ++inputLength; }

        // 跳过尾部空白
        while (inputLength > 0 &&
               (base64[inputLength - 1] == '\n' ||
                base64[inputLength - 1] == '\r' ||
                base64[inputLength - 1] == ' '))
        {
            --inputLength;
        }

        if (inputLength == 0)
        {
            output.Clear();
            return true;
        }

        // Base64 长度必须是 4 的倍数
        if (inputLength % 4 != 0)
        {
            return false;
        }

        SizeType maxOutput = GetMaxDecodedLength(inputLength);
        output.Clear();
        output.Reserve(maxOutput);

        for (SizeType charIndex = 0;
             charIndex < inputLength; charIndex += 4)
        {
            UInt8 a = DecodeChar(base64[charIndex + 0]);
            UInt8 b = DecodeChar(base64[charIndex + 1]);
            UInt8 c = DecodeChar(base64[charIndex + 2]);
            UInt8 d = DecodeChar(base64[charIndex + 3]);

            // 无效字符检查
            if (a == 0xFF || b == 0xFF)
            {
                return false;
            }

            UInt32 block =
                (static_cast<UInt32>(a) << 18) |
                (static_cast<UInt32>(b) << 12);

            output.Add(static_cast<UInt8>((block >> 16) & 0xFF));

            if (base64[charIndex + 2] != '=')
            {
                if (c == 0xFF) return false;
                block |= static_cast<UInt32>(c) << 6;
                output.Add(static_cast<UInt8>((block >> 8) & 0xFF));
            }

            if (base64[charIndex + 3] != '=')
            {
                if (d == 0xFF) return false;
                block |= static_cast<UInt32>(d);
                output.Add(static_cast<UInt8>(block & 0xFF));
            }
        }

        return true;
    }

    // ========================================================================
    // 长度计算
    // ========================================================================

    /// 预计算编码后的字符数 (不含 null 终止)
    LIMX_NODISCARD static constexpr SizeType GetEncodedLength(
        SizeType inputLength)
    {
        return ((inputLength + 2) / 3) * 4;
    }

    /// 预计算解码后的最大字节数
    LIMX_NODISCARD static constexpr SizeType GetMaxDecodedLength(
        SizeType encodedLength)
    {
        return (encodedLength / 4) * 3;
    }

private:
    /// 解码单个 Base64 字符
    LIMX_NODISCARD static UInt8 DecodeChar(AnsiChar ch)
    {
        if (ch >= 'A' && ch <= 'Z') return static_cast<UInt8>(ch - 'A');
        if (ch >= 'a' && ch <= 'z') return static_cast<UInt8>(ch - 'a' + 26);
        if (ch >= '0' && ch <= '9') return static_cast<UInt8>(ch - '0' + 52);
        if (ch == '+') return 62;
        if (ch == '/') return 63;
        if (ch == '=') return 0; // padding
        return 0xFF; // 无效字符
    }
};

} // namespace Limx
