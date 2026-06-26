/*******************************************************************************
 * 文件: FBitOps.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   位操作工具 — 常用位运算函数集合
 *   提供 PopCount、CTZ、CLZ、向上取整到2的幂等位级操作
 *   用于位掩码处理、分配器对齐、哈希表容量计算等场景
 *
 * 设计哲学:
 *   编译器内建优先 — 使用 MSVC __popcnt/__lzcnt 等内建获取硬件加速
 *   内联零开销 — 所有函数为 FORCEINLINE，编译器可直接内联
 *   32/64 位双版本 — 每个操作均提供 32 位和 64 位变体
 *
 * 技术特性:
 *   - PopCount32/64: 1 的位数统计
 *   - CountLeadingZeros32/64: 前导零计数
 *   - CountTrailingZeros32/64: 尾随零计数
 *   - RoundUpToPowerOfTwo: 向上取整到 2 的幂
 *   - IsPowerOfTwo: 是否为 2 的幂
 *   - FloorLog2/CeilLog2: 以 2 为底的对数
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h
 *   外部: MSVC 内建函数
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"

// MSVC 位操作内建
#if LIMX_COMPILER_MSVC
extern "C"
{
    unsigned char _BitScanForward(unsigned long*, unsigned long);
    unsigned char _BitScanReverse(unsigned long*, unsigned long);
    unsigned char _BitScanForward64(unsigned long*, unsigned long long);
    unsigned char _BitScanReverse64(unsigned long*, unsigned long long);
}
#pragma intrinsic(_BitScanForward)
#pragma intrinsic(_BitScanReverse)
#pragma intrinsic(_BitScanForward64)
#pragma intrinsic(_BitScanReverse64)
#endif

namespace Limx
{

/// 位操作工具
struct FBitOps
{
    // ========================================================================
    // 2 的幂判断与取整
    // ========================================================================

    /// 是否为 2 的幂 (0 不算)
    LIMX_NODISCARD static constexpr bool IsPowerOfTwo(UInt32 value)
    {
        return value > 0 && (value & (value - 1)) == 0;
    }

    LIMX_NODISCARD static constexpr bool IsPowerOfTwo64(UInt64 value)
    {
        return value > 0 && (value & (value - 1)) == 0;
    }

    /// 向上取整到 2 的幂
    LIMX_NODISCARD static constexpr UInt32 RoundUpToPowerOfTwo(
        UInt32 value)
    {
        if (value == 0) return 1;
        --value;
        value |= value >> 1;
        value |= value >> 2;
        value |= value >> 4;
        value |= value >> 8;
        value |= value >> 16;
        return value + 1;
    }

    LIMX_NODISCARD static constexpr UInt64 RoundUpToPowerOfTwo64(
        UInt64 value)
    {
        if (value == 0) return 1;
        --value;
        value |= value >> 1;
        value |= value >> 2;
        value |= value >> 4;
        value |= value >> 8;
        value |= value >> 16;
        value |= value >> 32;
        return value + 1;
    }

    // ========================================================================
    // 前导零/尾随零计数
    // ========================================================================

    /// 前导零计数 — 最高有效位之前的 0 的个数
    /// 输入 0 时返回 32
    LIMX_NODISCARD static FORCEINLINE UInt32 CountLeadingZeros32(
        UInt32 value)
    {
#if LIMX_COMPILER_MSVC
        unsigned long index;
        if (_BitScanReverse(&index, value))
        {
            return 31u - static_cast<UInt32>(index);
        }
        return 32u;
#else
        if (value == 0) return 32;
        UInt32 count = 0;
        if ((value & 0xFFFF0000u) == 0) { count += 16; value <<= 16; }
        if ((value & 0xFF000000u) == 0) { count += 8;  value <<= 8;  }
        if ((value & 0xF0000000u) == 0) { count += 4;  value <<= 4;  }
        if ((value & 0xC0000000u) == 0) { count += 2;  value <<= 2;  }
        if ((value & 0x80000000u) == 0) { count += 1; }
        return count;
#endif
    }

    /// 前导零计数 64 位
    LIMX_NODISCARD static FORCEINLINE UInt32 CountLeadingZeros64(
        UInt64 value)
    {
#if LIMX_COMPILER_MSVC
        unsigned long index;
        if (_BitScanReverse64(&index, value))
        {
            return 63u - static_cast<UInt32>(index);
        }
        return 64u;
#else
        if (value == 0) return 64;
        UInt32 count = 0;
        if ((value >> 32) == 0) { count += 32; value <<= 32; }
        count += CountLeadingZeros32(
            static_cast<UInt32>(value >> 32));
        return count;
#endif
    }

    /// 尾随零计数 — 最低有效位之后的 0 的个数
    /// 输入 0 时返回 32
    LIMX_NODISCARD static FORCEINLINE UInt32 CountTrailingZeros32(
        UInt32 value)
    {
#if LIMX_COMPILER_MSVC
        unsigned long index;
        if (_BitScanForward(&index, value))
        {
            return static_cast<UInt32>(index);
        }
        return 32u;
#else
        if (value == 0) return 32;
        UInt32 count = 0;
        if ((value & 0x0000FFFFu) == 0) { count += 16; value >>= 16; }
        if ((value & 0x000000FFu) == 0) { count += 8;  value >>= 8;  }
        if ((value & 0x0000000Fu) == 0) { count += 4;  value >>= 4;  }
        if ((value & 0x00000003u) == 0) { count += 2;  value >>= 2;  }
        if ((value & 0x00000001u) == 0) { count += 1; }
        return count;
#endif
    }

    /// 尾随零计数 64 位
    LIMX_NODISCARD static FORCEINLINE UInt32 CountTrailingZeros64(
        UInt64 value)
    {
#if LIMX_COMPILER_MSVC
        unsigned long index;
        if (_BitScanForward64(&index, value))
        {
            return static_cast<UInt32>(index);
        }
        return 64u;
#else
        if (value == 0) return 64;
        UInt32 count = 0;
        if ((value & 0xFFFFFFFFull) == 0)
        {
            count += 32;
            value >>= 32;
        }
        count += CountTrailingZeros32(
            static_cast<UInt32>(value));
        return count;
#endif
    }

    // ========================================================================
    // 1 的位数统计 (Population Count)
    // ========================================================================

    /// PopCount — 32 位中 1 的个数
    LIMX_NODISCARD static constexpr UInt32 PopCount32(UInt32 value)
    {
        // Brian Kernighan 方法在小值时快
        // Hamming weight 并行方法在所有情况下 O(1)
        value = value - ((value >> 1) & 0x55555555u);
        value = (value & 0x33333333u) +
                ((value >> 2) & 0x33333333u);
        value = (value + (value >> 4)) & 0x0F0F0F0Fu;
        return (value * 0x01010101u) >> 24;
    }

    /// PopCount — 64 位中 1 的个数
    LIMX_NODISCARD static constexpr UInt32 PopCount64(UInt64 value)
    {
        value = value - ((value >> 1) & 0x5555555555555555ull);
        value = (value & 0x3333333333333333ull) +
                ((value >> 2) & 0x3333333333333333ull);
        value = (value + (value >> 4)) & 0x0F0F0F0F0F0F0F0Full;
        return static_cast<UInt32>(
            (value * 0x0101010101010101ull) >> 56);
    }

    // ========================================================================
    // 以 2 为底的对数
    // ========================================================================

    /// FloorLog2 — 以 2 为底的对数 (向下取整)
    /// 输入 0 时返回 0
    LIMX_NODISCARD static FORCEINLINE UInt32 FloorLog2(UInt32 value)
    {
        if (value == 0) return 0;
        return 31u - CountLeadingZeros32(value);
    }

    LIMX_NODISCARD static FORCEINLINE UInt32 FloorLog2_64(UInt64 value)
    {
        if (value == 0) return 0;
        return 63u - CountLeadingZeros64(value);
    }

    /// CeilLog2 — 以 2 为底的对数 (向上取整)
    LIMX_NODISCARD static FORCEINLINE UInt32 CeilLog2(UInt32 value)
    {
        if (value <= 1) return 0;
        return FloorLog2(value - 1) + 1;
    }

    // ========================================================================
    // 位字段提取
    // ========================================================================

    /// 提取位字段
    LIMX_NODISCARD static constexpr UInt32 ExtractBits(
        UInt32 value, UInt32 offset, UInt32 bitCount)
    {
        UInt32 mask = (1u << bitCount) - 1u;
        return (value >> offset) & mask;
    }

    /// 设置位字段
    LIMX_NODISCARD static constexpr UInt32 SetBits(
        UInt32 base, UInt32 value, UInt32 offset, UInt32 bitCount)
    {
        UInt32 mask = ((1u << bitCount) - 1u) << offset;
        return (base & ~mask) | ((value << offset) & mask);
    }

    // ========================================================================
    // 对齐
    // ========================================================================

    /// 向上对齐到指定对齐边界 (alignment 必须是 2 的幂)
    LIMX_NODISCARD static constexpr SizeType AlignUp(
        SizeType value, SizeType alignment)
    {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    /// 向下对齐到指定对齐边界
    LIMX_NODISCARD static constexpr SizeType AlignDown(
        SizeType value, SizeType alignment)
    {
        return value & ~(alignment - 1);
    }

    /// 是否已对齐
    LIMX_NODISCARD static constexpr bool IsAligned(
        SizeType value, SizeType alignment)
    {
        return (value & (alignment - 1)) == 0;
    }
};

} // namespace Limx
