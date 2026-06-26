/*******************************************************************************
 * 文件: FStringHash.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   编译时字符串哈希 — 基于 FNV-1a 的 constexpr 哈希
 *   支持编译时和运行时两种模式，结果完全一致
 *   用于 switch-case 字符串分发、资源标识、消息 ID 等场景
 *
 * 设计哲学:
 *   编译时求值 — constexpr 函数在编译期完成哈希计算
 *   运行时兼容 — 同一算法可用于运行时字符串
 *   零冲突优化 — FNV-1a 在短字符串上具有良好分布特性
 *
 * 技术特性:
 *   - HashString32: 32 位 FNV-1a 哈希
 *   - HashString64: 64 位 FNV-1a 哈希
 *   - operator""_hash: 用户定义字面量 (编译时)
 *   - LIMX_STRING_HASH: 宏版本 (兼容)
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

/// 编译时字符串哈希
struct FStringHash
{
    // ========================================================================
    // FNV-1a 常量
    // ========================================================================

    static constexpr UInt32 kFnv32Offset = 2166136261u;
    static constexpr UInt32 kFnv32Prime  = 16777619u;

    static constexpr UInt64 kFnv64Offset = 14695981039346656037ull;
    static constexpr UInt64 kFnv64Prime  = 1099511628211ull;

    // ========================================================================
    // 32 位哈希
    // ========================================================================

    /// 编译时/运行时 32 位 FNV-1a 哈希
    LIMX_NODISCARD static constexpr UInt32 HashString32(
        const AnsiChar* str)
    {
        UInt32 hash = kFnv32Offset;
        while (*str != '\0')
        {
            hash ^= static_cast<UInt32>(
                static_cast<UInt8>(*str));
            hash *= kFnv32Prime;
            ++str;
        }
        return hash;
    }

    /// 带长度的 32 位 FNV-1a 哈希
    LIMX_NODISCARD static constexpr UInt32 HashString32(
        const AnsiChar* str, SizeType length)
    {
        UInt32 hash = kFnv32Offset;
        for (SizeType index = 0; index < length; ++index)
        {
            hash ^= static_cast<UInt32>(
                static_cast<UInt8>(str[index]));
            hash *= kFnv32Prime;
        }
        return hash;
    }

    /// 字节数组 32 位 FNV-1a 哈希
    LIMX_NODISCARD static constexpr UInt32 HashBytes32(
        const UInt8* data, SizeType length)
    {
        UInt32 hash = kFnv32Offset;
        for (SizeType index = 0; index < length; ++index)
        {
            hash ^= static_cast<UInt32>(data[index]);
            hash *= kFnv32Prime;
        }
        return hash;
    }

    // ========================================================================
    // 64 位哈希
    // ========================================================================

    /// 编译时/运行时 64 位 FNV-1a 哈希
    LIMX_NODISCARD static constexpr UInt64 HashString64(
        const AnsiChar* str)
    {
        UInt64 hash = kFnv64Offset;
        while (*str != '\0')
        {
            hash ^= static_cast<UInt64>(
                static_cast<UInt8>(*str));
            hash *= kFnv64Prime;
            ++str;
        }
        return hash;
    }

    /// 带长度的 64 位 FNV-1a 哈希
    LIMX_NODISCARD static constexpr UInt64 HashString64(
        const AnsiChar* str, SizeType length)
    {
        UInt64 hash = kFnv64Offset;
        for (SizeType index = 0; index < length; ++index)
        {
            hash ^= static_cast<UInt64>(
                static_cast<UInt8>(str[index]));
            hash *= kFnv64Prime;
        }
        return hash;
    }

    /// 字节数组 64 位 FNV-1a 哈希
    LIMX_NODISCARD static constexpr UInt64 HashBytes64(
        const UInt8* data, SizeType length)
    {
        UInt64 hash = kFnv64Offset;
        for (SizeType index = 0; index < length; ++index)
        {
            hash ^= static_cast<UInt64>(data[index]);
            hash *= kFnv64Prime;
        }
        return hash;
    }

    // ========================================================================
    // 组合哈希
    // ========================================================================

    /// 组合两个 32 位哈希值
    LIMX_NODISCARD static constexpr UInt32 Combine32(
        UInt32 hash1, UInt32 hash2)
    {
        return hash1 ^ (hash2 + 0x9E3779B9u +
                         (hash1 << 6) + (hash1 >> 2));
    }

    /// 组合两个 64 位哈希值
    LIMX_NODISCARD static constexpr UInt64 Combine64(
        UInt64 hash1, UInt64 hash2)
    {
        return hash1 ^ (hash2 + 0x9E3779B97F4A7C15ull +
                         (hash1 << 12) + (hash1 >> 4));
    }
};

} // namespace Limx

/// 用户定义字面量 — 编译时字符串哈希
/// 用法: constexpr auto id = "PlayerHealth"_hash;
LIMX_NODISCARD inline constexpr Limx::UInt32 operator""_hash(
    const char* str, Limx::SizeType length)
{
    return Limx::FStringHash::HashString32(str, length);
}

/// 64 位版本
LIMX_NODISCARD inline constexpr Limx::UInt64 operator""_hash64(
    const char* str, Limx::SizeType length)
{
    return Limx::FStringHash::HashString64(str, length);
}
