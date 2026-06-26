/*******************************************************************************
 * 文件: FEndian.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   字节序工具 — 大小端检测与转换
 *   提供整数类型的字节序交换函数和网络/主机字节序转换
 *   用于二进制序列化、网络协议解析、跨平台数据交换等场景
 *
 * 设计哲学:
 *   编译时检测 — 平台字节序在编译时确定
 *   零开销 — 内联函数，编译器可优化为 bswap 指令
 *   统一接口 — HostToNetwork/NetworkToHost 屏蔽平台差异
 *
 * 技术特性:
 *   - IsLittleEndian/IsBigEndian: 编译时字节序判断
 *   - SwapBytes16/32/64: 字节序交换
 *   - HostToNetwork/NetworkToHost: 网络字节序转换
 *   - HostToLittle/HostToBig: 指定端转换
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"

// MSVC 字节交换内建
#if LIMX_COMPILER_MSVC
extern "C"
{
    unsigned short _byteswap_ushort(unsigned short);
    unsigned long  _byteswap_ulong(unsigned long);
    unsigned long long _byteswap_uint64(unsigned long long);
}
#pragma intrinsic(_byteswap_ushort)
#pragma intrinsic(_byteswap_ulong)
#pragma intrinsic(_byteswap_uint64)
#endif

namespace Limx
{

/// 字节序工具
struct FEndian
{
    // ========================================================================
    // 字节序检测
    // ========================================================================

    /// 当前平台是否为小端 (x86/x64 = 小端)
    LIMX_NODISCARD static constexpr bool IsLittleEndian()
    {
#if LIMX_PLATFORM_WINDOWS
        return true; // x64 Windows 始终为小端
#else
        return true; // 默认假设小端
#endif
    }

    /// 当前平台是否为大端
    LIMX_NODISCARD static constexpr bool IsBigEndian()
    {
        return !IsLittleEndian();
    }

    // ========================================================================
    // 字节交换
    // ========================================================================

    /// 16 位字节交换
    LIMX_NODISCARD static FORCEINLINE UInt16 SwapBytes16(UInt16 value)
    {
#if LIMX_COMPILER_MSVC
        return _byteswap_ushort(value);
#else
        return static_cast<UInt16>(
            (value >> 8) | (value << 8));
#endif
    }

    /// 32 位字节交换
    LIMX_NODISCARD static FORCEINLINE UInt32 SwapBytes32(UInt32 value)
    {
#if LIMX_COMPILER_MSVC
        return _byteswap_ulong(value);
#else
        return ((value >> 24) & 0x000000FFu) |
               ((value >>  8) & 0x0000FF00u) |
               ((value <<  8) & 0x00FF0000u) |
               ((value << 24) & 0xFF000000u);
#endif
    }

    /// 64 位字节交换
    LIMX_NODISCARD static FORCEINLINE UInt64 SwapBytes64(UInt64 value)
    {
#if LIMX_COMPILER_MSVC
        return _byteswap_uint64(value);
#else
        return ((value >> 56) & 0x00000000000000FFull) |
               ((value >> 40) & 0x000000000000FF00ull) |
               ((value >> 24) & 0x0000000000FF0000ull) |
               ((value >>  8) & 0x00000000FF000000ull) |
               ((value <<  8) & 0x000000FF00000000ull) |
               ((value << 24) & 0x0000FF0000000000ull) |
               ((value << 40) & 0x00FF000000000000ull) |
               ((value << 56) & 0xFF00000000000000ull);
#endif
    }

    // ========================================================================
    // 网络字节序 (大端) 转换
    // ========================================================================

    /// 主机 → 网络 (16 位)
    LIMX_NODISCARD static FORCEINLINE UInt16 HostToNetwork16(
        UInt16 value)
    {
        return IsLittleEndian() ? SwapBytes16(value) : value;
    }

    /// 网络 → 主机 (16 位)
    LIMX_NODISCARD static FORCEINLINE UInt16 NetworkToHost16(
        UInt16 value)
    {
        return IsLittleEndian() ? SwapBytes16(value) : value;
    }

    /// 主机 → 网络 (32 位)
    LIMX_NODISCARD static FORCEINLINE UInt32 HostToNetwork32(
        UInt32 value)
    {
        return IsLittleEndian() ? SwapBytes32(value) : value;
    }

    /// 网络 → 主机 (32 位)
    LIMX_NODISCARD static FORCEINLINE UInt32 NetworkToHost32(
        UInt32 value)
    {
        return IsLittleEndian() ? SwapBytes32(value) : value;
    }

    /// 主机 → 网络 (64 位)
    LIMX_NODISCARD static FORCEINLINE UInt64 HostToNetwork64(
        UInt64 value)
    {
        return IsLittleEndian() ? SwapBytes64(value) : value;
    }

    /// 网络 → 主机 (64 位)
    LIMX_NODISCARD static FORCEINLINE UInt64 NetworkToHost64(
        UInt64 value)
    {
        return IsLittleEndian() ? SwapBytes64(value) : value;
    }

    // ========================================================================
    // 显式端序转换
    // ========================================================================

    /// 主机 → 小端 (在小端平台上为无操作)
    LIMX_NODISCARD static FORCEINLINE UInt16 HostToLittle16(
        UInt16 value)
    {
        return IsLittleEndian() ? value : SwapBytes16(value);
    }

    LIMX_NODISCARD static FORCEINLINE UInt32 HostToLittle32(
        UInt32 value)
    {
        return IsLittleEndian() ? value : SwapBytes32(value);
    }

    LIMX_NODISCARD static FORCEINLINE UInt64 HostToLittle64(
        UInt64 value)
    {
        return IsLittleEndian() ? value : SwapBytes64(value);
    }

    /// 小端 → 主机
    LIMX_NODISCARD static FORCEINLINE UInt16 LittleToHost16(
        UInt16 value)
    {
        return HostToLittle16(value);
    }

    LIMX_NODISCARD static FORCEINLINE UInt32 LittleToHost32(
        UInt32 value)
    {
        return HostToLittle32(value);
    }

    LIMX_NODISCARD static FORCEINLINE UInt64 LittleToHost64(
        UInt64 value)
    {
        return HostToLittle64(value);
    }

    /// 主机 → 大端 (等同于 HostToNetwork)
    LIMX_NODISCARD static FORCEINLINE UInt32 HostToBig32(
        UInt32 value)
    {
        return HostToNetwork32(value);
    }

    /// 大端 → 主机 (等同于 NetworkToHost)
    LIMX_NODISCARD static FORCEINLINE UInt32 BigToHost32(
        UInt32 value)
    {
        return NetworkToHost32(value);
    }
};

} // namespace Limx
