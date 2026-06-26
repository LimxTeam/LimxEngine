/*******************************************************************************
 * 文件: FStringPool.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   字符串池 — 去重字符串存储
 *   相同内容的字符串只存储一份，返回唯一句柄
 *   用于资产路径去重、标识符内部化、FName 底层存储等场景
 *
 * 设计哲学:
 *   去重存储 — 哈希表索引，相同字符串返回相同指针
 *   只增不删 — 池内字符串生命周期与池一致，不支持单独释放
 *   连续分配 — 使用大块内存连续存储，减少碎片
 *
 * 技术特性:
 *   - FStringPool: 字符串去重池
 *   - Intern: 内部化字符串，返回池内指针
 *   - Contains: 是否已内部化
 *   - GetEntryCount: 条目数
 *   - Clear: 释放所有存储
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Containers/TArray.h,
 *          Core/Memory/MemoryOps.h, Core/Memory/DefaultAllocator.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Containers/TArray.h"
#include "Core/Memory/MemoryOps.h"
#include "Core/Memory/IAllocator.h"
#include "Core/Memory/DefaultAllocator.h"

namespace Limx
{

/// 字符串去重池
class FStringPool
{
    /// 哈希桶条目
    struct FEntry
    {
        const AnsiChar* String;  ///< 池内字符串指针
        SizeType        Length;  ///< 字符串长度 (不含 '\0')
        UInt32          Hash;    ///< 缓存的哈希值
        FEntry*         Next;    ///< 链表下一个 (开链法)
    };

    /// 内存块 — 连续分配字符串数据
    struct FBlock
    {
        UInt8*   Data;      ///< 块内存
        SizeType Capacity;  ///< 块容量
        SizeType Used;      ///< 已用字节
    };

    static constexpr SizeType kDefaultBlockSize = 4096;
    static constexpr SizeType kDefaultBucketCount = 256;

public:
    explicit FStringPool(
        SizeType blockSize = kDefaultBlockSize,
        SizeType bucketCount = kDefaultBucketCount)
        : m_BlockSize(blockSize)
        , m_BucketCount(bucketCount)
        , m_EntryCount(0)
        , m_Buckets(nullptr)
    {
        // 分配桶数组
        m_Buckets = static_cast<FEntry**>(
            GetDefaultAllocator().Allocate(
                m_BucketCount * sizeof(FEntry*), alignof(FEntry*)));
        Memory::MemZero(m_Buckets, m_BucketCount * sizeof(FEntry*));
    }

    ~FStringPool()
    {
        Clear();
        if (m_Buckets)
        {
            GetDefaultAllocator().Deallocate(m_Buckets);
            m_Buckets = nullptr;
        }
    }

    // 不可拷贝
    FStringPool(const FStringPool&) = delete;
    FStringPool& operator=(const FStringPool&) = delete;

    // ========================================================================
    // 操作
    // ========================================================================

    /// 内部化字符串 — 返回池内指针
    /// 如果已存在相同字符串，返回已有指针
    const AnsiChar* Intern(const AnsiChar* str)
    {
        SizeType length = StringLength(str);
        return InternWithLength(str, length);
    }

    /// 内部化字符串 (带长度)
    const AnsiChar* InternWithLength(
        const AnsiChar* str, SizeType length)
    {
        UInt32 hash = HashString(str, length);
        SizeType bucketIndex = hash % m_BucketCount;

        // 查找已有条目
        FEntry* entry = m_Buckets[bucketIndex];
        while (entry != nullptr)
        {
            if (entry->Hash == hash &&
                entry->Length == length &&
                StringEqual(entry->String, str, length))
            {
                return entry->String;
            }
            entry = entry->Next;
        }

        // 不存在 — 分配新条目
        AnsiChar* pooledString = AllocateString(str, length);

        FEntry* newEntry = static_cast<FEntry*>(
            GetDefaultAllocator().Allocate(
                sizeof(FEntry), alignof(FEntry)));
        newEntry->String = pooledString;
        newEntry->Length = length;
        newEntry->Hash = hash;
        newEntry->Next = m_Buckets[bucketIndex];
        m_Buckets[bucketIndex] = newEntry;
        ++m_EntryCount;

        return pooledString;
    }

    /// 是否包含指定字符串
    LIMX_NODISCARD bool Contains(const AnsiChar* str) const
    {
        SizeType length = StringLength(str);
        UInt32 hash = HashString(str, length);
        SizeType bucketIndex = hash % m_BucketCount;

        FEntry* entry = m_Buckets[bucketIndex];
        while (entry != nullptr)
        {
            if (entry->Hash == hash &&
                entry->Length == length &&
                StringEqual(entry->String, str, length))
            {
                return true;
            }
            entry = entry->Next;
        }
        return false;
    }

    /// 清空所有存储
    void Clear()
    {
        // 释放所有条目
        for (SizeType bucketIndex = 0;
             bucketIndex < m_BucketCount; ++bucketIndex)
        {
            FEntry* entry = m_Buckets[bucketIndex];
            while (entry != nullptr)
            {
                FEntry* next = entry->Next;
                GetDefaultAllocator().Deallocate(entry);
                entry = next;
            }
            m_Buckets[bucketIndex] = nullptr;
        }
        m_EntryCount = 0;

        // 释放所有内存块
        for (SizeType blockIndex = 0;
             blockIndex < m_Blocks.GetSize(); ++blockIndex)
        {
            GetDefaultAllocator().Deallocate(
                m_Blocks[blockIndex].Data);
        }
        m_Blocks.Clear();
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 条目数 (唯一字符串数)
    LIMX_NODISCARD SizeType GetEntryCount() const
    {
        return m_EntryCount;
    }

    /// 已分配的内存块数
    LIMX_NODISCARD SizeType GetBlockCount() const
    {
        return m_Blocks.GetSize();
    }

    /// 总分配内存 (字节)
    LIMX_NODISCARD SizeType GetTotalAllocatedBytes() const
    {
        SizeType total = 0;
        for (SizeType blockIndex = 0;
             blockIndex < m_Blocks.GetSize(); ++blockIndex)
        {
            total += m_Blocks[blockIndex].Capacity;
        }
        return total;
    }

private:
    // ========================================================================
    // 内部辅助
    // ========================================================================

    /// 在池内分配字符串副本
    AnsiChar* AllocateString(const AnsiChar* str, SizeType length)
    {
        SizeType requiredBytes = length + 1; // +1 for '\0'

        // 寻找有足够空间的块
        for (SizeType blockIndex = 0;
             blockIndex < m_Blocks.GetSize(); ++blockIndex)
        {
            FBlock& block = m_Blocks[blockIndex];
            if (block.Used + requiredBytes <= block.Capacity)
            {
                AnsiChar* dest = reinterpret_cast<AnsiChar*>(
                    block.Data + block.Used);
                Memory::MemCopy(dest, str, length);
                dest[length] = '\0';
                block.Used += requiredBytes;
                return dest;
            }
        }

        // 分配新块
        SizeType blockCapacity = m_BlockSize;
        if (requiredBytes > blockCapacity)
        {
            blockCapacity = requiredBytes;
        }

        FBlock newBlock;
        newBlock.Data = static_cast<UInt8*>(
            GetDefaultAllocator().Allocate(blockCapacity, 16));
        newBlock.Capacity = blockCapacity;
        newBlock.Used = requiredBytes;

        AnsiChar* dest = reinterpret_cast<AnsiChar*>(newBlock.Data);
        Memory::MemCopy(dest, str, length);
        dest[length] = '\0';

        m_Blocks.Add(newBlock);
        return dest;
    }

    /// FNV-1a 哈希
    static UInt32 HashString(const AnsiChar* str, SizeType length)
    {
        UInt32 hash = 2166136261u;
        for (SizeType charIndex = 0;
             charIndex < length; ++charIndex)
        {
            hash ^= static_cast<UInt32>(
                static_cast<UInt8>(str[charIndex]));
            hash *= 16777619u;
        }
        return hash;
    }

    /// 字符串长度
    static SizeType StringLength(const AnsiChar* str)
    {
        SizeType length = 0;
        while (str[length] != '\0') ++length;
        return length;
    }

    /// 字符串比较
    static bool StringEqual(const AnsiChar* a, const AnsiChar* b,
                             SizeType length)
    {
        for (SizeType charIndex = 0;
             charIndex < length; ++charIndex)
        {
            if (a[charIndex] != b[charIndex]) return false;
        }
        return true;
    }

    SizeType     m_BlockSize;    ///< 内存块大小
    SizeType     m_BucketCount;  ///< 哈希桶数
    SizeType     m_EntryCount;   ///< 条目数
    FEntry**     m_Buckets;      ///< 哈希桶数组
    TArray<FBlock> m_Blocks;     ///< 内存块列表
};

} // namespace Limx
