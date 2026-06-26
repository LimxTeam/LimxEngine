/*******************************************************************************
 * 文件: TBitArray.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   动态位数组 — 紧凑存储布尔标志集合
 *   每个元素占 1 位，适用于大规模标志位、可见性掩码、分配位图等场景
 *   内部以 UInt64 字为单位存储，支持按位操作
 *
 * 设计哲学:
 *   紧凑存储 — 1 位/元素，比 TArray<bool> 节省 8 倍内存
 *   字对齐操作 — 批量位操作以 64 位字为单位，利用 CPU 位操作指令
 *   分配器感知 — 底层存储通过 IAllocator 管理
 *
 * 技术特性:
 *   - 存储: UInt64 字数组，每字 64 位
 *   - Set/Clear/Toggle: 设置/清除/翻转单个位
 *   - IsSet: 查询单个位
 *   - SetAll/ClearAll: 批量操作
 *   - CountSetBits: 统计置位数 (popcount)
 *   - FindFirstSet/FindFirstClear: 查找首个置位/清零位
 *   - 按位运算: AND, OR, XOR
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h,
 *          Core/Memory/MemoryOps.h, Core/Memory/DefaultAllocator.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Memory/MemoryOps.h"
#include "Core/Memory/DefaultAllocator.h"

namespace Limx
{

/// 动态位数组
class TBitArray
{
    static constexpr SizeType kBitsPerWord = 64;

public:
    // ========================================================================
    // 构造与析构
    // ========================================================================

    /// 默认构造 — 空位数组
    TBitArray()
        : m_Words(nullptr)
        , m_BitCount(0)
        , m_WordCount(0)
        , m_Allocator(&GetDefaultAllocator())
    {
    }

    /// 构造指定位数的数组 (所有位初始为 0)
    explicit TBitArray(SizeType bitCount)
        : m_Words(nullptr)
        , m_BitCount(0)
        , m_WordCount(0)
        , m_Allocator(&GetDefaultAllocator())
    {
        Resize(bitCount);
    }

    /// 构造指定位数的数组并设置初始值
    TBitArray(SizeType bitCount, bool initialValue)
        : m_Words(nullptr)
        , m_BitCount(0)
        , m_WordCount(0)
        , m_Allocator(&GetDefaultAllocator())
    {
        Resize(bitCount);
        if (initialValue)
        {
            SetAll();
        }
    }

    TBitArray(const TBitArray& other)
        : m_Words(nullptr)
        , m_BitCount(0)
        , m_WordCount(0)
        , m_Allocator(other.m_Allocator)
    {
        if (other.m_WordCount > 0)
        {
            m_WordCount = other.m_WordCount;
            m_BitCount = other.m_BitCount;
            m_Words = AllocateWords(m_WordCount);
            Memory::MemCopy(m_Words, other.m_Words,
                            m_WordCount * sizeof(UInt64));
        }
    }

    TBitArray(TBitArray&& other) noexcept
        : m_Words(other.m_Words)
        , m_BitCount(other.m_BitCount)
        , m_WordCount(other.m_WordCount)
        , m_Allocator(other.m_Allocator)
    {
        other.m_Words = nullptr;
        other.m_BitCount = 0;
        other.m_WordCount = 0;
    }

    ~TBitArray()
    {
        FreeWords();
    }

    TBitArray& operator=(const TBitArray& other)
    {
        if (this != &other)
        {
            FreeWords();
            m_Allocator = other.m_Allocator;
            if (other.m_WordCount > 0)
            {
                m_WordCount = other.m_WordCount;
                m_BitCount = other.m_BitCount;
                m_Words = AllocateWords(m_WordCount);
                Memory::MemCopy(m_Words, other.m_Words,
                                m_WordCount * sizeof(UInt64));
            }
        }
        return *this;
    }

    TBitArray& operator=(TBitArray&& other) noexcept
    {
        if (this != &other)
        {
            FreeWords();
            m_Words = other.m_Words;
            m_BitCount = other.m_BitCount;
            m_WordCount = other.m_WordCount;
            m_Allocator = other.m_Allocator;
            other.m_Words = nullptr;
            other.m_BitCount = 0;
            other.m_WordCount = 0;
        }
        return *this;
    }

    // ========================================================================
    // 大小管理
    // ========================================================================

    LIMX_NODISCARD FORCEINLINE SizeType GetBitCount() const
    {
        return m_BitCount;
    }

    LIMX_NODISCARD FORCEINLINE bool IsEmpty() const
    {
        return m_BitCount == 0;
    }

    /// 调整大小 — 新增位初始为 0
    void Resize(SizeType newBitCount)
    {
        SizeType newWordCount = (newBitCount + kBitsPerWord - 1) /
                                kBitsPerWord;
        if (newWordCount != m_WordCount)
        {
            UInt64* newWords = nullptr;
            if (newWordCount > 0)
            {
                newWords = AllocateWords(newWordCount);
                Memory::MemZero(newWords, newWordCount * sizeof(UInt64));

                if (m_Words && m_WordCount > 0)
                {
                    SizeType copyCount = (newWordCount < m_WordCount)
                        ? newWordCount : m_WordCount;
                    Memory::MemCopy(newWords, m_Words,
                                    copyCount * sizeof(UInt64));
                }
            }
            FreeWords();
            m_Words = newWords;
            m_WordCount = newWordCount;
        }
        m_BitCount = newBitCount;

        // 清除尾部多余位
        ClearTrailingBits();
    }

    // ========================================================================
    // 单位操作
    // ========================================================================

    /// 设置位 (置 1)
    FORCEINLINE void Set(SizeType index)
    {
        LIMX_ASSERT(index < m_BitCount);
        m_Words[index / kBitsPerWord] |=
            (1ULL << (index % kBitsPerWord));
    }

    /// 清除位 (置 0)
    FORCEINLINE void Clear(SizeType index)
    {
        LIMX_ASSERT(index < m_BitCount);
        m_Words[index / kBitsPerWord] &=
            ~(1ULL << (index % kBitsPerWord));
    }

    /// 翻转位
    FORCEINLINE void Toggle(SizeType index)
    {
        LIMX_ASSERT(index < m_BitCount);
        m_Words[index / kBitsPerWord] ^=
            (1ULL << (index % kBitsPerWord));
    }

    /// 设置位为指定值
    FORCEINLINE void SetValue(SizeType index, bool value)
    {
        if (value)
        {
            Set(index);
        }
        else
        {
            Clear(index);
        }
    }

    /// 查询位是否为 1
    LIMX_NODISCARD FORCEINLINE bool IsSet(SizeType index) const
    {
        LIMX_ASSERT(index < m_BitCount);
        return (m_Words[index / kBitsPerWord] &
                (1ULL << (index % kBitsPerWord))) != 0;
    }

    // ========================================================================
    // 批量操作
    // ========================================================================

    /// 全部置 1
    void SetAll()
    {
        if (m_Words && m_WordCount > 0)
        {
            Memory::MemSet(m_Words, 0xFF,
                           m_WordCount * sizeof(UInt64));
            ClearTrailingBits();
        }
    }

    /// 全部置 0
    void ClearAll()
    {
        if (m_Words && m_WordCount > 0)
        {
            Memory::MemZero(m_Words, m_WordCount * sizeof(UInt64));
        }
    }

    // ========================================================================
    // 统计
    // ========================================================================

    /// 统计置位数 (popcount)
    LIMX_NODISCARD SizeType CountSetBits() const
    {
        SizeType count = 0;
        for (SizeType wordIndex = 0; wordIndex < m_WordCount; ++wordIndex)
        {
            count += Popcount64(m_Words[wordIndex]);
        }
        return count;
    }

    /// 统计清零位数
    LIMX_NODISCARD SizeType CountClearBits() const
    {
        return m_BitCount - CountSetBits();
    }

    /// 是否全部为 0
    LIMX_NODISCARD bool IsAllClear() const
    {
        for (SizeType wordIndex = 0; wordIndex < m_WordCount; ++wordIndex)
        {
            if (m_Words[wordIndex] != 0)
            {
                return false;
            }
        }
        return true;
    }

    /// 是否全部为 1
    LIMX_NODISCARD bool IsAllSet() const
    {
        return CountSetBits() == m_BitCount;
    }

    // ========================================================================
    // 查找
    // ========================================================================

    /// 查找第一个置位 — 返回索引，未找到返回 kSizeTypeMax
    LIMX_NODISCARD SizeType FindFirstSet() const
    {
        for (SizeType wordIndex = 0; wordIndex < m_WordCount; ++wordIndex)
        {
            if (m_Words[wordIndex] != 0)
            {
                SizeType bitInWord = CountTrailingZeros64(
                    m_Words[wordIndex]);
                SizeType result = wordIndex * kBitsPerWord + bitInWord;
                return (result < m_BitCount) ? result : kSizeTypeMax;
            }
        }
        return kSizeTypeMax;
    }

    /// 查找第一个清零位
    LIMX_NODISCARD SizeType FindFirstClear() const
    {
        for (SizeType wordIndex = 0; wordIndex < m_WordCount; ++wordIndex)
        {
            UInt64 inverted = ~m_Words[wordIndex];
            if (inverted != 0)
            {
                SizeType bitInWord = CountTrailingZeros64(inverted);
                SizeType result = wordIndex * kBitsPerWord + bitInWord;
                return (result < m_BitCount) ? result : kSizeTypeMax;
            }
        }
        return kSizeTypeMax;
    }

    // ========================================================================
    // 按位运算
    // ========================================================================

    /// 按位与
    LIMX_NODISCARD TBitArray operator&(const TBitArray& other) const
    {
        LIMX_ASSERT(m_BitCount == other.m_BitCount);
        TBitArray result(m_BitCount);
        for (SizeType wordIndex = 0; wordIndex < m_WordCount; ++wordIndex)
        {
            result.m_Words[wordIndex] =
                m_Words[wordIndex] & other.m_Words[wordIndex];
        }
        return result;
    }

    /// 按位或
    LIMX_NODISCARD TBitArray operator|(const TBitArray& other) const
    {
        LIMX_ASSERT(m_BitCount == other.m_BitCount);
        TBitArray result(m_BitCount);
        for (SizeType wordIndex = 0; wordIndex < m_WordCount; ++wordIndex)
        {
            result.m_Words[wordIndex] =
                m_Words[wordIndex] | other.m_Words[wordIndex];
        }
        return result;
    }

    /// 按位异或
    LIMX_NODISCARD TBitArray operator^(const TBitArray& other) const
    {
        LIMX_ASSERT(m_BitCount == other.m_BitCount);
        TBitArray result(m_BitCount);
        for (SizeType wordIndex = 0; wordIndex < m_WordCount; ++wordIndex)
        {
            result.m_Words[wordIndex] =
                m_Words[wordIndex] ^ other.m_Words[wordIndex];
        }
        return result;
    }

    /// 按位取反
    LIMX_NODISCARD TBitArray operator~() const
    {
        TBitArray result(m_BitCount);
        for (SizeType wordIndex = 0; wordIndex < m_WordCount; ++wordIndex)
        {
            result.m_Words[wordIndex] = ~m_Words[wordIndex];
        }
        result.ClearTrailingBits();
        return result;
    }

private:
    // ========================================================================
    // 辅助函数
    // ========================================================================

    UInt64* AllocateWords(SizeType wordCount)
    {
        return static_cast<UInt64*>(
            m_Allocator->Allocate(
                wordCount * sizeof(UInt64), alignof(UInt64)));
    }

    void FreeWords()
    {
        if (m_Words)
        {
            m_Allocator->Deallocate(m_Words);
            m_Words = nullptr;
            m_WordCount = 0;
        }
    }

    /// 清除最后一个字中超出 m_BitCount 的多余位
    void ClearTrailingBits()
    {
        if (m_WordCount > 0 && (m_BitCount % kBitsPerWord) != 0)
        {
            UInt64 mask = (1ULL << (m_BitCount % kBitsPerWord)) - 1;
            m_Words[m_WordCount - 1] &= mask;
        }
    }

    /// 64 位 popcount (位计数)
    static SizeType Popcount64(UInt64 value)
    {
        // Hamming Weight — 分治法
        value = value - ((value >> 1) & 0x5555555555555555ULL);
        value = (value & 0x3333333333333333ULL) +
                ((value >> 2) & 0x3333333333333333ULL);
        value = (value + (value >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
        return static_cast<SizeType>(
            (value * 0x0101010101010101ULL) >> 56);
    }

    /// 64 位 CTZ (Count Trailing Zeros)
    static SizeType CountTrailingZeros64(UInt64 value)
    {
        if (value == 0) return 64;
        SizeType count = 0;
        if ((value & 0xFFFFFFFF) == 0) { count += 32; value >>= 32; }
        if ((value & 0x0000FFFF) == 0) { count += 16; value >>= 16; }
        if ((value & 0x000000FF) == 0) { count += 8;  value >>= 8;  }
        if ((value & 0x0000000F) == 0) { count += 4;  value >>= 4;  }
        if ((value & 0x00000003) == 0) { count += 2;  value >>= 2;  }
        if ((value & 0x00000001) == 0) { count += 1; }
        return count;
    }

    // ========================================================================
    // 成员数据
    // ========================================================================

    UInt64*     m_Words;      ///< 底层字数组
    SizeType    m_BitCount;   ///< 总位数
    SizeType    m_WordCount;  ///< 字数
    IAllocator* m_Allocator;  ///< 内存分配器
};

} // namespace Limx
