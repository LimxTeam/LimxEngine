/*******************************************************************************
 * 文件: FHash.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   通用哈希工具 — 多种哈希算法的静态工具集合
 *   提供 CRC32、FNV-1a、CityHash 等常用哈希算法
 *   用于容器哈希、资产标识、数据校验、字符串哈希等场景
 *
 * 设计哲学:
 *   静态工具类 — 所有方法为 constexpr/static，无需实例化
 *   编译时可计算 — 字面量字符串可在编译时完成哈希
 *   确定性 — 相同输入始终产生相同输出
 *
 * 技术特性:
 *   - CRC32: 标准 CRC-32/ISO-HDLC (查表法)
 *   - FNV1a32/FNV1a64: Fowler-Noll-Vo 1a 哈希
 *   - Combine: 哈希值组合 (boost::hash_combine 方法)
 *   - HashBytes: 通用字节流哈希
 *   - HashString: 字符串哈希
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

/// 通用哈希工具
struct FHash
{
    // ========================================================================
    // FNV-1a 哈希
    // ========================================================================

    /// FNV-1a 32 位 — 字节流哈希
    LIMX_NODISCARD static constexpr UInt32 Fnv1a32(
        const UInt8* data, SizeType length)
    {
        UInt32 hash = 0x811C9DC5u;  // FNV offset basis
        for (SizeType index = 0; index < length; ++index)
        {
            hash ^= static_cast<UInt32>(data[index]);
            hash *= 0x01000193u;     // FNV prime
        }
        return hash;
    }

    /// FNV-1a 32 位 — 字符串哈希
    LIMX_NODISCARD static constexpr UInt32 Fnv1a32(const AnsiChar* str)
    {
        UInt32 hash = 0x811C9DC5u;
        while (*str)
        {
            hash ^= static_cast<UInt32>(
                static_cast<UInt8>(*str));
            hash *= 0x01000193u;
            ++str;
        }
        return hash;
    }

    /// FNV-1a 64 位 — 字节流哈希
    LIMX_NODISCARD static constexpr UInt64 Fnv1a64(
        const UInt8* data, SizeType length)
    {
        UInt64 hash = 0xCBF29CE484222325ULL;  // FNV offset basis
        for (SizeType index = 0; index < length; ++index)
        {
            hash ^= static_cast<UInt64>(data[index]);
            hash *= 0x00000100000001B3ULL;     // FNV prime
        }
        return hash;
    }

    /// FNV-1a 64 位 — 字符串哈希
    LIMX_NODISCARD static constexpr UInt64 Fnv1a64(const AnsiChar* str)
    {
        UInt64 hash = 0xCBF29CE484222325ULL;
        while (*str)
        {
            hash ^= static_cast<UInt64>(
                static_cast<UInt8>(*str));
            hash *= 0x00000100000001B3ULL;
            ++str;
        }
        return hash;
    }

    // ========================================================================
    // CRC32 (查表法)
    // ========================================================================

    /// CRC32 — 字节流校验
    LIMX_NODISCARD static UInt32 Crc32(const UInt8* data, SizeType length)
    {
        UInt32 crc = 0xFFFFFFFF;
        for (SizeType index = 0; index < length; ++index)
        {
            UInt8 byte = data[index];
            crc = (crc >> 8) ^ GetCrc32Table()[(crc ^ byte) & 0xFF];
        }
        return crc ^ 0xFFFFFFFF;
    }

    /// CRC32 — 字符串校验
    LIMX_NODISCARD static UInt32 Crc32(const AnsiChar* str)
    {
        UInt32 crc = 0xFFFFFFFF;
        while (*str)
        {
            UInt8 byte = static_cast<UInt8>(*str);
            crc = (crc >> 8) ^ GetCrc32Table()[(crc ^ byte) & 0xFF];
            ++str;
        }
        return crc ^ 0xFFFFFFFF;
    }

    // ========================================================================
    // 哈希组合
    // ========================================================================

    /// 组合两个 32 位哈希值 (boost::hash_combine 方法)
    LIMX_NODISCARD static constexpr UInt32 Combine32(UInt32 seed,
                                                       UInt32 value)
    {
        seed ^= value + 0x9E3779B9u + (seed << 6) + (seed >> 2);
        return seed;
    }

    /// 组合两个 64 位哈希值
    LIMX_NODISCARD static constexpr UInt64 Combine64(UInt64 seed,
                                                       UInt64 value)
    {
        seed ^= value + 0x9E3779B97F4A7C15ULL +
                (seed << 12) + (seed >> 4);
        return seed;
    }

    // ========================================================================
    // 通用哈希
    // ========================================================================

    /// 对任意 POD 类型取哈希 (FNV-1a 64)
    template<typename T>
    LIMX_NODISCARD static UInt64 HashOf(const T& value)
    {
        return Fnv1a64(
            reinterpret_cast<const UInt8*>(&value), sizeof(T));
    }

    /// 整数哈希 — 混洗比特 (splitmix64 变体)
    LIMX_NODISCARD static constexpr UInt64 HashInt64(UInt64 value)
    {
        value ^= value >> 30;
        value *= 0xBF58476D1CE4E5B9ULL;
        value ^= value >> 27;
        value *= 0x94D049BB133111EBULL;
        value ^= value >> 31;
        return value;
    }

    /// 32 位整数哈希
    LIMX_NODISCARD static constexpr UInt32 HashInt32(UInt32 value)
    {
        value ^= value >> 16;
        value *= 0x45D9F3Bu;
        value ^= value >> 16;
        value *= 0x45D9F3Bu;
        value ^= value >> 16;
        return value;
    }

    /// 指针哈希
    LIMX_NODISCARD static UInt64 HashPointer(const void* pointer)
    {
        return HashInt64(reinterpret_cast<UInt64>(pointer));
    }

private:
    // ========================================================================
    // CRC32 查找表 (编译时生成)
    // ========================================================================

    struct Crc32Table
    {
        UInt32 Entries[256];

        constexpr Crc32Table() : Entries{}
        {
            for (UInt32 index = 0; index < 256; ++index)
            {
                UInt32 crc = index;
                for (UInt32 bit = 0; bit < 8; ++bit)
                {
                    if (crc & 1)
                    {
                        crc = (crc >> 1) ^ 0xEDB88320u;
                    }
                    else
                    {
                        crc >>= 1;
                    }
                }
                Entries[index] = crc;
            }
        }
    };

    static const UInt32* GetCrc32Table()
    {
        static constexpr Crc32Table s_Table{};
        return s_Table.Entries;
    }
};

/// 编译时字符串哈希字面量运算符
LIMX_NODISCARD constexpr UInt32 operator""_hash32(
    const AnsiChar* str, SizeType /*length*/)
{
    return FHash::Fnv1a32(str);
}

LIMX_NODISCARD constexpr UInt64 operator""_hash64(
    const AnsiChar* str, SizeType /*length*/)
{
    return FHash::Fnv1a64(str);
}

} // namespace Limx
