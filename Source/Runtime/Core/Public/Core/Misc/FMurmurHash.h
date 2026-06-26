/*******************************************************************************
 * 文件: FMurmurHash.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   MurmurHash3 — 高质量非加密哈希函数
 *   提供 32 位和 128 位两种变体
 *   用于哈希表、数据去重、内容寻址、布隆过滤器等场景
 *
 * 设计哲学:
 *   极速分布 — 在短键和长键上均有极佳的分布特性
 *   确定性 — 相同输入 + 种子产生相同输出
 *   零依赖 — 纯算术运算，无外部依赖
 *
 * 技术特性:
 *   - Hash32: MurmurHash3_x86_32 (32 位输出)
 *   - Hash128: MurmurHash3_x64_128 (128 位输出, 两个 UInt64)
 *   - 支持自定义种子
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"

namespace Limx
{

/// MurmurHash3 哈希函数
struct FMurmurHash
{
    // ========================================================================
    // MurmurHash3_x86_32
    // ========================================================================

    /// 32 位 MurmurHash3
    /// @param data 输入数据
    /// @param length 数据长度 (字节)
    /// @param seed 哈希种子
    /// @return 32 位哈希值
    LIMX_NODISCARD static UInt32 Hash32(
        const void* data, SizeType length, UInt32 seed = 0)
    {
        const UInt8* bytes = static_cast<const UInt8*>(data);
        SizeType blockCount = length / 4;

        UInt32 h1 = seed;

        constexpr UInt32 c1 = 0xCC9E2D51u;
        constexpr UInt32 c2 = 0x1B873593u;

        // 主体 — 每次处理 4 字节
        for (SizeType blockIndex = 0;
             blockIndex < blockCount; ++blockIndex)
        {
            UInt32 k1 = ReadUInt32LE(
                bytes + blockIndex * 4);

            k1 *= c1;
            k1 = RotateLeft32(k1, 15);
            k1 *= c2;

            h1 ^= k1;
            h1 = RotateLeft32(h1, 13);
            h1 = h1 * 5 + 0xE6546B64u;
        }

        // 尾部
        const UInt8* tail = bytes + blockCount * 4;
        UInt32 k1 = 0;

        SizeType remainder = length & 3;
        if (remainder >= 3) k1 ^= static_cast<UInt32>(tail[2]) << 16;
        if (remainder >= 2) k1 ^= static_cast<UInt32>(tail[1]) << 8;
        if (remainder >= 1)
        {
            k1 ^= static_cast<UInt32>(tail[0]);
            k1 *= c1;
            k1 = RotateLeft32(k1, 15);
            k1 *= c2;
            h1 ^= k1;
        }

        // 终结混合
        h1 ^= static_cast<UInt32>(length);
        h1 = FinalMix32(h1);

        return h1;
    }

    /// 字符串便捷版本
    LIMX_NODISCARD static UInt32 HashString32(
        const AnsiChar* str, UInt32 seed = 0)
    {
        SizeType length = 0;
        const AnsiChar* ptr = str;
        while (*ptr != '\0') { ++ptr; ++length; }
        return Hash32(str, length, seed);
    }

    // ========================================================================
    // MurmurHash3_x64_128
    // ========================================================================

    /// 128 位 MurmurHash3 结果
    struct FHash128
    {
        UInt64 Low;
        UInt64 High;
    };

    /// 128 位 MurmurHash3
    LIMX_NODISCARD static FHash128 Hash128(
        const void* data, SizeType length, UInt64 seed = 0)
    {
        const UInt8* bytes = static_cast<const UInt8*>(data);
        SizeType blockCount = length / 16;

        UInt64 h1 = seed;
        UInt64 h2 = seed;

        constexpr UInt64 c1 = 0x87C37B91114253D5ull;
        constexpr UInt64 c2 = 0x4CF5AD432745937Full;

        // 主体 — 每次处理 16 字节
        for (SizeType blockIndex = 0;
             blockIndex < blockCount; ++blockIndex)
        {
            UInt64 k1 = ReadUInt64LE(
                bytes + blockIndex * 16);
            UInt64 k2 = ReadUInt64LE(
                bytes + blockIndex * 16 + 8);

            k1 *= c1;
            k1 = RotateLeft64(k1, 31);
            k1 *= c2;
            h1 ^= k1;

            h1 = RotateLeft64(h1, 27);
            h1 += h2;
            h1 = h1 * 5 + 0x52DCE729ull;

            k2 *= c2;
            k2 = RotateLeft64(k2, 33);
            k2 *= c1;
            h2 ^= k2;

            h2 = RotateLeft64(h2, 31);
            h2 += h1;
            h2 = h2 * 5 + 0x38495AB5ull;
        }

        // 尾部
        const UInt8* tail = bytes + blockCount * 16;
        UInt64 k1 = 0;
        UInt64 k2 = 0;

        SizeType remainder = length & 15;
        if (remainder >= 15) k2 ^= static_cast<UInt64>(tail[14]) << 48;
        if (remainder >= 14) k2 ^= static_cast<UInt64>(tail[13]) << 40;
        if (remainder >= 13) k2 ^= static_cast<UInt64>(tail[12]) << 32;
        if (remainder >= 12) k2 ^= static_cast<UInt64>(tail[11]) << 24;
        if (remainder >= 11) k2 ^= static_cast<UInt64>(tail[10]) << 16;
        if (remainder >= 10) k2 ^= static_cast<UInt64>(tail[9]) << 8;
        if (remainder >= 9)
        {
            k2 ^= static_cast<UInt64>(tail[8]);
            k2 *= c2;
            k2 = RotateLeft64(k2, 33);
            k2 *= c1;
            h2 ^= k2;
        }

        if (remainder >= 8) k1 ^= static_cast<UInt64>(tail[7]) << 56;
        if (remainder >= 7) k1 ^= static_cast<UInt64>(tail[6]) << 48;
        if (remainder >= 6) k1 ^= static_cast<UInt64>(tail[5]) << 40;
        if (remainder >= 5) k1 ^= static_cast<UInt64>(tail[4]) << 32;
        if (remainder >= 4) k1 ^= static_cast<UInt64>(tail[3]) << 24;
        if (remainder >= 3) k1 ^= static_cast<UInt64>(tail[2]) << 16;
        if (remainder >= 2) k1 ^= static_cast<UInt64>(tail[1]) << 8;
        if (remainder >= 1)
        {
            k1 ^= static_cast<UInt64>(tail[0]);
            k1 *= c1;
            k1 = RotateLeft64(k1, 31);
            k1 *= c2;
            h1 ^= k1;
        }

        // 终结混合
        h1 ^= static_cast<UInt64>(length);
        h2 ^= static_cast<UInt64>(length);

        h1 += h2;
        h2 += h1;

        h1 = FinalMix64(h1);
        h2 = FinalMix64(h2);

        h1 += h2;
        h2 += h1;

        FHash128 result;
        result.Low = h1;
        result.High = h2;
        return result;
    }

private:
    // ========================================================================
    // 内部辅助
    // ========================================================================

    LIMX_NODISCARD static constexpr UInt32 RotateLeft32(
        UInt32 value, Int32 shift)
    {
        return (value << shift) | (value >> (32 - shift));
    }

    LIMX_NODISCARD static constexpr UInt64 RotateLeft64(
        UInt64 value, Int32 shift)
    {
        return (value << shift) | (value >> (64 - shift));
    }

    LIMX_NODISCARD static constexpr UInt32 FinalMix32(UInt32 h)
    {
        h ^= h >> 16;
        h *= 0x85EBCA6Bu;
        h ^= h >> 13;
        h *= 0xC2B2AE35u;
        h ^= h >> 16;
        return h;
    }

    LIMX_NODISCARD static constexpr UInt64 FinalMix64(UInt64 h)
    {
        h ^= h >> 33;
        h *= 0xFF51AFD7ED558CCDull;
        h ^= h >> 33;
        h *= 0xC4CEB9FE1A85EC53ull;
        h ^= h >> 33;
        return h;
    }

    LIMX_NODISCARD static UInt32 ReadUInt32LE(const UInt8* ptr)
    {
        return static_cast<UInt32>(ptr[0]) |
               (static_cast<UInt32>(ptr[1]) << 8) |
               (static_cast<UInt32>(ptr[2]) << 16) |
               (static_cast<UInt32>(ptr[3]) << 24);
    }

    LIMX_NODISCARD static UInt64 ReadUInt64LE(const UInt8* ptr)
    {
        return static_cast<UInt64>(ptr[0]) |
               (static_cast<UInt64>(ptr[1]) << 8) |
               (static_cast<UInt64>(ptr[2]) << 16) |
               (static_cast<UInt64>(ptr[3]) << 24) |
               (static_cast<UInt64>(ptr[4]) << 32) |
               (static_cast<UInt64>(ptr[5]) << 40) |
               (static_cast<UInt64>(ptr[6]) << 48) |
               (static_cast<UInt64>(ptr[7]) << 56);
    }
};

} // namespace Limx
