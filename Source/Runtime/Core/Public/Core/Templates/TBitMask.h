/*******************************************************************************
 * 文件: TBitMask.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   位掩码工具 — 编译时大小的位数组，按位操作的强类型容器
 *   存储任意数量的布尔标志，支持 Set/Clear/Test/And/Or/Xor 等操作
 *   用于渲染层掩码、碰撞过滤、可见性标志、功能开关等场景
 *
 * 设计哲学:
 *   编译时大小 — BitCount 为模板参数，内嵌 UInt64 数组
 *   类型安全 — 枚举类型可作为位索引
 *   位级操作 — And/Or/Xor/Not/PopCount 等完整集合
 *
 * 技术特性:
 *   - TBitMask<BitCount>: 固定位数的位掩码
 *   - Set/Clear/Toggle/Test: 单位操作
 *   - operator&/|/^/~: 位逻辑运算
 *   - PopCount: 置位位数
 *   - None/All/Any: 批量查询
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"

namespace Limx
{

/// 固定位数的位掩码
/// @tparam BitCount 位数 (>0)
template<SizeType BitCount>
class TBitMask
{
    static_assert(BitCount > 0,
        "BitCount must be > 0");

    static constexpr SizeType kWordBits = 64;
    static constexpr SizeType kWordCount =
        (BitCount + kWordBits - 1) / kWordBits;

public:
    /// 默认构造 — 全清零
    TBitMask()
    {
        for (SizeType wordIdx = 0;
             wordIdx < kWordCount; ++wordIdx)
        {
            m_Words[wordIdx] = 0ULL;
        }
    }

    // ========================================================================
    // 单位操作
    // ========================================================================

    /// 置位
    void Set(SizeType bitIndex)
    {
        LIMX_ASSERT(bitIndex < BitCount);
        m_Words[bitIndex / kWordBits] |=
            (1ULL << (bitIndex % kWordBits));
    }

    /// 清零
    void Clear(SizeType bitIndex)
    {
        LIMX_ASSERT(bitIndex < BitCount);
        m_Words[bitIndex / kWordBits] &=
            ~(1ULL << (bitIndex % kWordBits));
    }

    /// 翻转
    void Toggle(SizeType bitIndex)
    {
        LIMX_ASSERT(bitIndex < BitCount);
        m_Words[bitIndex / kWordBits] ^=
            (1ULL << (bitIndex % kWordBits));
    }

    /// 按值设置
    void SetValue(SizeType bitIndex, bool value)
    {
        if (value) Set(bitIndex);
        else Clear(bitIndex);
    }

    /// 测试单位
    LIMX_NODISCARD bool Test(SizeType bitIndex) const
    {
        LIMX_ASSERT(bitIndex < BitCount);
        return (m_Words[bitIndex / kWordBits] &
                (1ULL << (bitIndex % kWordBits))) != 0;
    }

    /// operator[] (只读)
    LIMX_NODISCARD bool operator[](
        SizeType bitIndex) const
    {
        return Test(bitIndex);
    }

    // ========================================================================
    // 全量操作
    // ========================================================================

    /// 全部置位
    void SetAll()
    {
        for (SizeType wordIdx = 0;
             wordIdx < kWordCount; ++wordIdx)
        {
            m_Words[wordIdx] = ~0ULL;
        }
        MaskHighBits();
    }

    /// 全部清零
    void ClearAll()
    {
        for (SizeType wordIdx = 0;
             wordIdx < kWordCount; ++wordIdx)
        {
            m_Words[wordIdx] = 0ULL;
        }
    }

    /// 全部翻转
    void ToggleAll()
    {
        for (SizeType wordIdx = 0;
             wordIdx < kWordCount; ++wordIdx)
        {
            m_Words[wordIdx] = ~m_Words[wordIdx];
        }
        MaskHighBits();
    }

    // ========================================================================
    // 批量查询
    // ========================================================================

    /// 是否全为 0
    LIMX_NODISCARD bool None() const
    {
        for (SizeType wordIdx = 0;
             wordIdx < kWordCount; ++wordIdx)
        {
            if (m_Words[wordIdx] != 0ULL) return false;
        }
        return true;
    }

    /// 是否全为 1
    LIMX_NODISCARD bool All() const
    {
        for (SizeType wordIdx = 0;
             wordIdx < kWordCount - 1; ++wordIdx)
        {
            if (m_Words[wordIdx] != ~0ULL) return false;
        }
        // 最后一个 word 单独处理
        UInt64 lastMask = GetLastWordMask();
        return (m_Words[kWordCount - 1] & lastMask)
            == lastMask;
    }

    /// 是否有至少一位为 1
    LIMX_NODISCARD bool Any() const
    {
        return !None();
    }

    /// 置位位数
    LIMX_NODISCARD SizeType PopCount() const
    {
        SizeType count = 0;
        for (SizeType wordIdx = 0;
             wordIdx < kWordCount; ++wordIdx)
        {
            count += CountBitsInWord(m_Words[wordIdx]);
        }
        return count;
    }

    /// 总位数
    LIMX_NODISCARD static constexpr SizeType Size()
    {
        return BitCount;
    }

    // ========================================================================
    // 位逻辑运算
    // ========================================================================

    LIMX_NODISCARD TBitMask operator&(
        const TBitMask& other) const
    {
        TBitMask result;
        for (SizeType wordIdx = 0;
             wordIdx < kWordCount; ++wordIdx)
        {
            result.m_Words[wordIdx] =
                m_Words[wordIdx] & other.m_Words[wordIdx];
        }
        return result;
    }

    LIMX_NODISCARD TBitMask operator|(
        const TBitMask& other) const
    {
        TBitMask result;
        for (SizeType wordIdx = 0;
             wordIdx < kWordCount; ++wordIdx)
        {
            result.m_Words[wordIdx] =
                m_Words[wordIdx] | other.m_Words[wordIdx];
        }
        return result;
    }

    LIMX_NODISCARD TBitMask operator^(
        const TBitMask& other) const
    {
        TBitMask result;
        for (SizeType wordIdx = 0;
             wordIdx < kWordCount; ++wordIdx)
        {
            result.m_Words[wordIdx] =
                m_Words[wordIdx] ^ other.m_Words[wordIdx];
        }
        return result;
    }

    LIMX_NODISCARD TBitMask operator~() const
    {
        TBitMask result;
        for (SizeType wordIdx = 0;
             wordIdx < kWordCount; ++wordIdx)
        {
            result.m_Words[wordIdx] = ~m_Words[wordIdx];
        }
        result.MaskHighBits();
        return result;
    }

    TBitMask& operator&=(const TBitMask& other)
    {
        for (SizeType wordIdx = 0;
             wordIdx < kWordCount; ++wordIdx)
        {
            m_Words[wordIdx] &= other.m_Words[wordIdx];
        }
        return *this;
    }

    TBitMask& operator|=(const TBitMask& other)
    {
        for (SizeType wordIdx = 0;
             wordIdx < kWordCount; ++wordIdx)
        {
            m_Words[wordIdx] |= other.m_Words[wordIdx];
        }
        return *this;
    }

    TBitMask& operator^=(const TBitMask& other)
    {
        for (SizeType wordIdx = 0;
             wordIdx < kWordCount; ++wordIdx)
        {
            m_Words[wordIdx] ^= other.m_Words[wordIdx];
        }
        return *this;
    }

    // ========================================================================
    // 比较
    // ========================================================================

    LIMX_NODISCARD bool operator==(
        const TBitMask& other) const
    {
        for (SizeType wordIdx = 0;
             wordIdx < kWordCount; ++wordIdx)
        {
            if (m_Words[wordIdx] != other.m_Words[wordIdx])
                return false;
        }
        return true;
    }

    LIMX_NODISCARD bool operator!=(
        const TBitMask& other) const
    {
        return !(*this == other);
    }

private:
    void MaskHighBits()
    {
        constexpr SizeType remainBits =
            BitCount % kWordBits;
        if constexpr (remainBits != 0)
        {
            m_Words[kWordCount - 1] &=
                (1ULL << remainBits) - 1ULL;
        }
    }

    LIMX_NODISCARD static UInt64 GetLastWordMask()
    {
        constexpr SizeType remainBits =
            BitCount % kWordBits;
        if constexpr (remainBits == 0)
        {
            return ~0ULL;
        }
        else
        {
            return (1ULL << remainBits) - 1ULL;
        }
    }

    LIMX_NODISCARD static SizeType CountBitsInWord(
        UInt64 word)
    {
        SizeType count = 0;
        while (word != 0)
        {
            word &= word - 1;
            ++count;
        }
        return count;
    }

    UInt64 m_Words[kWordCount];  ///< 位存储
};

/// 常用别名
using FBitMask8   = TBitMask<8>;
using FBitMask16  = TBitMask<16>;
using FBitMask32  = TBitMask<32>;
using FBitMask64  = TBitMask<64>;
using FBitMask128 = TBitMask<128>;

} // namespace Limx
