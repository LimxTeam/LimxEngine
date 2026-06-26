/*******************************************************************************
 * 文件: TBitSet.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   固定大小位集合 — 编译时确定大小的位数组
 *   提供 Set/Clear/Test/Toggle 等位级操作
 *   用于组件掩码、标志集合、布隆过滤器等需要紧凑位存储的场景
 *
 * 设计哲学:
 *   编译时大小 — 模板参数指定位数，内联 UInt64 数组存储
 *   零堆分配 — 所有存储内嵌在对象内
 *   集合操作 — 支持位与/位或/位异或/取反等集合运算
 *
 * 技术特性:
 *   - TBitSet<N>: N 位的固定大小位集合
 *   - Set/Clear/Test/Toggle: 单位操作
 *   - SetAll/ClearAll: 批量操作
 *   - operator&/|/^/~: 集合运算
 *   - PopCount: 置位位数
 *   - FindFirstSet/FindFirstClear: 扫描操作
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h, Core/Misc/FBitOps.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"

namespace Limx
{

/// 固定大小位集合
/// @tparam BitCount 位数 (编译时常量)
template<SizeType BitCount>
class TBitSet
{
    static_assert(BitCount > 0, "TBitSet BitCount must be > 0");

    static constexpr SizeType kBitsPerWord = 64;
    static constexpr SizeType kWordCount =
        (BitCount + kBitsPerWord - 1) / kBitsPerWord;

    /// 最后一个 word 中有效位的掩码
    static constexpr UInt64 kLastWordMask =
        (BitCount % kBitsPerWord == 0)
            ? ~static_cast<UInt64>(0)
            : (static_cast<UInt64>(1) <<
               (BitCount % kBitsPerWord)) - 1;

public:
    // ========================================================================
    // 构造
    // ========================================================================

    /// 默认构造 — 所有位清零
    constexpr TBitSet()
        : m_Words{}
    {
    }

    // ========================================================================
    // 单位操作
    // ========================================================================

    /// 设置指定位
    void Set(SizeType bitIndex)
    {
        LIMX_ASSERT(bitIndex < BitCount);
        SizeType wordIndex = bitIndex / kBitsPerWord;
        SizeType bitOffset = bitIndex % kBitsPerWord;
        m_Words[wordIndex] |= (static_cast<UInt64>(1) << bitOffset);
    }

    /// 清除指定位
    void Clear(SizeType bitIndex)
    {
        LIMX_ASSERT(bitIndex < BitCount);
        SizeType wordIndex = bitIndex / kBitsPerWord;
        SizeType bitOffset = bitIndex % kBitsPerWord;
        m_Words[wordIndex] &=
            ~(static_cast<UInt64>(1) << bitOffset);
    }

    /// 翻转指定位
    void Toggle(SizeType bitIndex)
    {
        LIMX_ASSERT(bitIndex < BitCount);
        SizeType wordIndex = bitIndex / kBitsPerWord;
        SizeType bitOffset = bitIndex % kBitsPerWord;
        m_Words[wordIndex] ^= (static_cast<UInt64>(1) << bitOffset);
    }

    /// 测试指定位
    LIMX_NODISCARD bool Test(SizeType bitIndex) const
    {
        LIMX_ASSERT(bitIndex < BitCount);
        SizeType wordIndex = bitIndex / kBitsPerWord;
        SizeType bitOffset = bitIndex % kBitsPerWord;
        return (m_Words[wordIndex] &
                (static_cast<UInt64>(1) << bitOffset)) != 0;
    }

    /// 设置指定位为给定值
    void SetValue(SizeType bitIndex, bool value)
    {
        if (value)
            Set(bitIndex);
        else
            Clear(bitIndex);
    }

    // ========================================================================
    // 批量操作
    // ========================================================================

    /// 设置所有位
    void SetAll()
    {
        for (SizeType wordIndex = 0;
             wordIndex < kWordCount; ++wordIndex)
        {
            m_Words[wordIndex] = ~static_cast<UInt64>(0);
        }
        // 清除最后一个 word 中的无效位
        m_Words[kWordCount - 1] &= kLastWordMask;
    }

    /// 清除所有位
    void ClearAll()
    {
        for (SizeType wordIndex = 0;
             wordIndex < kWordCount; ++wordIndex)
        {
            m_Words[wordIndex] = 0;
        }
    }

    /// 翻转所有位
    void ToggleAll()
    {
        for (SizeType wordIndex = 0;
             wordIndex < kWordCount; ++wordIndex)
        {
            m_Words[wordIndex] = ~m_Words[wordIndex];
        }
        m_Words[kWordCount - 1] &= kLastWordMask;
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 置位位数 (1 的个数)
    LIMX_NODISCARD SizeType PopCount() const
    {
        SizeType count = 0;
        for (SizeType wordIndex = 0;
             wordIndex < kWordCount; ++wordIndex)
        {
            UInt64 word = m_Words[wordIndex];
            // Hamming weight
            word = word - ((word >> 1) & 0x5555555555555555ull);
            word = (word & 0x3333333333333333ull) +
                   ((word >> 2) & 0x3333333333333333ull);
            word = (word + (word >> 4)) & 0x0F0F0F0F0F0F0F0Full;
            count += static_cast<SizeType>(
                (word * 0x0101010101010101ull) >> 56);
        }
        return count;
    }

    /// 是否所有位都为 0
    LIMX_NODISCARD bool IsEmpty() const
    {
        for (SizeType wordIndex = 0;
             wordIndex < kWordCount; ++wordIndex)
        {
            if (m_Words[wordIndex] != 0) return false;
        }
        return true;
    }

    /// 是否所有位都为 1
    LIMX_NODISCARD bool IsFull() const
    {
        for (SizeType wordIndex = 0;
             wordIndex < kWordCount - 1; ++wordIndex)
        {
            if (m_Words[wordIndex] != ~static_cast<UInt64>(0))
                return false;
        }
        return (m_Words[kWordCount - 1] & kLastWordMask) ==
               kLastWordMask;
    }

    /// 位容量
    LIMX_NODISCARD static constexpr SizeType GetBitCount()
    {
        return BitCount;
    }

    /// 查找第一个置位位 (返回索引，无则返回 BitCount)
    LIMX_NODISCARD SizeType FindFirstSet() const
    {
        for (SizeType wordIndex = 0;
             wordIndex < kWordCount; ++wordIndex)
        {
            if (m_Words[wordIndex] != 0)
            {
                // 尾随零计数
                UInt64 word = m_Words[wordIndex];
                SizeType bitOffset = 0;
                if ((word & 0xFFFFFFFFull) == 0)
                {
                    bitOffset += 32;
                    word >>= 32;
                }
                if ((word & 0xFFFFull) == 0)
                {
                    bitOffset += 16;
                    word >>= 16;
                }
                if ((word & 0xFFull) == 0)
                {
                    bitOffset += 8;
                    word >>= 8;
                }
                if ((word & 0xFull) == 0)
                {
                    bitOffset += 4;
                    word >>= 4;
                }
                if ((word & 0x3ull) == 0)
                {
                    bitOffset += 2;
                    word >>= 2;
                }
                if ((word & 0x1ull) == 0)
                {
                    bitOffset += 1;
                }
                SizeType result =
                    wordIndex * kBitsPerWord + bitOffset;
                return result < BitCount ? result : BitCount;
            }
        }
        return BitCount;
    }

    /// 查找第一个清除位 (返回索引，无则返回 BitCount)
    LIMX_NODISCARD SizeType FindFirstClear() const
    {
        for (SizeType wordIndex = 0;
             wordIndex < kWordCount; ++wordIndex)
        {
            UInt64 inverted = ~m_Words[wordIndex];
            // 最后一个 word 需要掩码
            if (wordIndex == kWordCount - 1)
            {
                inverted &= kLastWordMask;
            }
            if (inverted != 0)
            {
                UInt64 word = inverted;
                SizeType bitOffset = 0;
                if ((word & 0xFFFFFFFFull) == 0)
                {
                    bitOffset += 32;
                    word >>= 32;
                }
                if ((word & 0xFFFFull) == 0)
                {
                    bitOffset += 16;
                    word >>= 16;
                }
                if ((word & 0xFFull) == 0)
                {
                    bitOffset += 8;
                    word >>= 8;
                }
                if ((word & 0xFull) == 0)
                {
                    bitOffset += 4;
                    word >>= 4;
                }
                if ((word & 0x3ull) == 0)
                {
                    bitOffset += 2;
                    word >>= 2;
                }
                if ((word & 0x1ull) == 0)
                {
                    bitOffset += 1;
                }
                SizeType result =
                    wordIndex * kBitsPerWord + bitOffset;
                return result < BitCount ? result : BitCount;
            }
        }
        return BitCount;
    }

    // ========================================================================
    // 集合运算
    // ========================================================================

    LIMX_NODISCARD TBitSet operator&(const TBitSet& other) const
    {
        TBitSet result;
        for (SizeType wordIndex = 0;
             wordIndex < kWordCount; ++wordIndex)
        {
            result.m_Words[wordIndex] =
                m_Words[wordIndex] & other.m_Words[wordIndex];
        }
        return result;
    }

    LIMX_NODISCARD TBitSet operator|(const TBitSet& other) const
    {
        TBitSet result;
        for (SizeType wordIndex = 0;
             wordIndex < kWordCount; ++wordIndex)
        {
            result.m_Words[wordIndex] =
                m_Words[wordIndex] | other.m_Words[wordIndex];
        }
        return result;
    }

    LIMX_NODISCARD TBitSet operator^(const TBitSet& other) const
    {
        TBitSet result;
        for (SizeType wordIndex = 0;
             wordIndex < kWordCount; ++wordIndex)
        {
            result.m_Words[wordIndex] =
                m_Words[wordIndex] ^ other.m_Words[wordIndex];
        }
        return result;
    }

    LIMX_NODISCARD TBitSet operator~() const
    {
        TBitSet result;
        for (SizeType wordIndex = 0;
             wordIndex < kWordCount; ++wordIndex)
        {
            result.m_Words[wordIndex] = ~m_Words[wordIndex];
        }
        result.m_Words[kWordCount - 1] &= kLastWordMask;
        return result;
    }

    TBitSet& operator&=(const TBitSet& other)
    {
        for (SizeType wordIndex = 0;
             wordIndex < kWordCount; ++wordIndex)
        {
            m_Words[wordIndex] &= other.m_Words[wordIndex];
        }
        return *this;
    }

    TBitSet& operator|=(const TBitSet& other)
    {
        for (SizeType wordIndex = 0;
             wordIndex < kWordCount; ++wordIndex)
        {
            m_Words[wordIndex] |= other.m_Words[wordIndex];
        }
        return *this;
    }

    TBitSet& operator^=(const TBitSet& other)
    {
        for (SizeType wordIndex = 0;
             wordIndex < kWordCount; ++wordIndex)
        {
            m_Words[wordIndex] ^= other.m_Words[wordIndex];
        }
        return *this;
    }

    // ========================================================================
    // 比较
    // ========================================================================

    LIMX_NODISCARD bool operator==(const TBitSet& other) const
    {
        for (SizeType wordIndex = 0;
             wordIndex < kWordCount; ++wordIndex)
        {
            if (m_Words[wordIndex] != other.m_Words[wordIndex])
                return false;
        }
        return true;
    }

    LIMX_NODISCARD bool operator!=(const TBitSet& other) const
    {
        return !(*this == other);
    }

    /// 子集测试 — this 是否为 other 的子集
    LIMX_NODISCARD bool IsSubsetOf(const TBitSet& other) const
    {
        for (SizeType wordIndex = 0;
             wordIndex < kWordCount; ++wordIndex)
        {
            if ((m_Words[wordIndex] & ~other.m_Words[wordIndex]) != 0)
                return false;
        }
        return true;
    }

private:
    UInt64 m_Words[kWordCount];  ///< 位存储
};

} // namespace Limx
