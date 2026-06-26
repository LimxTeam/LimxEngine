/*******************************************************************************
 * 文件: TChunkedList.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   分块链表 — 分页分配、顺序迭代的混合容器
 *   以固定大小的 Chunk 为单位分配，Chunk 间以链表串联
 *   适合频繁追加、很少随机访问、需要稳定指针的大型集合
 *   用于命令缓冲区、事件记录、渲染列表等
 *
 * 设计哲学:
 *   分块分配 — 每次不足时分配新 Chunk，无整体 realloc
 *   指针稳定 — 已插入元素地址永不失效
 *   顺序迭代 — 提供跨 Chunk 的 begin/end 迭代器
 *
 * 技术特性:
 *   - TChunkedList<T, ChunkSize>: 分块链表
 *   - Add: 追加元素 (O(1) 均摊)
 *   - GetSize: 总元素数
 *   - begin/end: 顺序迭代器
 *   - Clear: 清空所有 Chunk
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h,
 *          Core/Memory/DefaultAllocator.h
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

/// 分块链表
/// @tparam T 元素类型
/// @tparam ChunkSize 每块元素数 (默认 32)
template<typename T, SizeType ChunkSize = 32>
class TChunkedList
{
    static_assert(ChunkSize > 0,
        "ChunkSize must be > 0");

    struct FChunk
    {
        alignas(T) UInt8 Data[sizeof(T) * ChunkSize];
        FChunk*  Next;
        SizeType Count;

        FChunk() : Next(nullptr), Count(0) {}

        T* GetPtr()
        {
            return reinterpret_cast<T*>(Data);
        }

        const T* GetPtr() const
        {
            return reinterpret_cast<const T*>(Data);
        }
    };

public:
    TChunkedList()
        : m_Head(nullptr)
        , m_Tail(nullptr)
        , m_Size(0)
    {
    }

    ~TChunkedList()
    {
        Clear();
    }

    // 不可拷贝
    TChunkedList(const TChunkedList&) = delete;
    TChunkedList& operator=(
        const TChunkedList&) = delete;

    // 可移动
    TChunkedList(TChunkedList&& other)
        : m_Head(other.m_Head)
        , m_Tail(other.m_Tail)
        , m_Size(other.m_Size)
    {
        other.m_Head = nullptr;
        other.m_Tail = nullptr;
        other.m_Size = 0;
    }

    // ========================================================================
    // 追加
    // ========================================================================

    /// 追加元素 (拷贝)
    /// @return 稳定指针
    T* Add(const T& element)
    {
        FChunk* chunk = GetOrAllocTailChunk();
        SizeType slotIdx = chunk->Count;
        new (chunk->GetPtr() + slotIdx) T(element);
        ++chunk->Count;
        ++m_Size;
        return chunk->GetPtr() + slotIdx;
    }

    /// 追加元素 (移动)
    T* Add(T&& element)
    {
        FChunk* chunk = GetOrAllocTailChunk();
        SizeType slotIdx = chunk->Count;
        new (chunk->GetPtr() + slotIdx)
            T(MoveTemp(element));
        ++chunk->Count;
        ++m_Size;
        return chunk->GetPtr() + slotIdx;
    }

    // ========================================================================
    // 查询
    // ========================================================================

    LIMX_NODISCARD SizeType GetSize() const
    {
        return m_Size;
    }

    LIMX_NODISCARD bool IsEmpty() const
    {
        return m_Size == 0;
    }

    // ========================================================================
    // 清空
    // ========================================================================

    void Clear()
    {
        FChunk* chunk = m_Head;
        while (chunk != nullptr)
        {
            for (SizeType elemIdx = 0;
                 elemIdx < chunk->Count; ++elemIdx)
            {
                chunk->GetPtr()[elemIdx].~T();
            }
            FChunk* nextChunk = chunk->Next;
            chunk->~FChunk();
            GetDefaultAllocator().Deallocate(chunk);
            chunk = nextChunk;
        }
        m_Head = nullptr;
        m_Tail = nullptr;
        m_Size = 0;
    }

    // ========================================================================
    // 迭代器
    // ========================================================================

    struct FIterator
    {
        FChunk*  m_Chunk;
        SizeType m_LocalIdx;

        FIterator(FChunk* chunk, SizeType localIdx)
            : m_Chunk(chunk), m_LocalIdx(localIdx)
        {}

        T& operator*()
        {
            return m_Chunk->GetPtr()[m_LocalIdx];
        }

        T* operator->()
        {
            return &m_Chunk->GetPtr()[m_LocalIdx];
        }

        FIterator& operator++()
        {
            ++m_LocalIdx;
            if (m_LocalIdx >= m_Chunk->Count)
            {
                m_Chunk = m_Chunk->Next;
                m_LocalIdx = 0;
            }
            return *this;
        }

        bool operator!=(const FIterator& other) const
        {
            return m_Chunk != other.m_Chunk ||
                   m_LocalIdx != other.m_LocalIdx;
        }
    };

    struct FConstIterator
    {
        const FChunk* m_Chunk;
        SizeType      m_LocalIdx;

        FConstIterator(const FChunk* chunk,
                       SizeType localIdx)
            : m_Chunk(chunk), m_LocalIdx(localIdx)
        {}

        const T& operator*() const
        {
            return m_Chunk->GetPtr()[m_LocalIdx];
        }

        FConstIterator& operator++()
        {
            ++m_LocalIdx;
            if (m_LocalIdx >= m_Chunk->Count)
            {
                m_Chunk = m_Chunk->Next;
                m_LocalIdx = 0;
            }
            return *this;
        }

        bool operator!=(
            const FConstIterator& other) const
        {
            return m_Chunk != other.m_Chunk ||
                   m_LocalIdx != other.m_LocalIdx;
        }
    };

    FIterator begin()
    {
        return FIterator(m_Head, 0);
    }

    FIterator end()
    {
        return FIterator(nullptr, 0);
    }

    FConstIterator begin() const
    {
        return FConstIterator(m_Head, 0);
    }

    FConstIterator end() const
    {
        return FConstIterator(nullptr, 0);
    }

private:
    FChunk* GetOrAllocTailChunk()
    {
        if (m_Tail == nullptr ||
            m_Tail->Count >= ChunkSize)
        {
            FChunk* newChunk =
                static_cast<FChunk*>(
                    GetDefaultAllocator().Allocate(
                        sizeof(FChunk),
                        alignof(FChunk)));
            new (newChunk) FChunk();

            if (m_Tail != nullptr)
            {
                m_Tail->Next = newChunk;
            }
            else
            {
                m_Head = newChunk;
            }
            m_Tail = newChunk;
        }
        return m_Tail;
    }

    FChunk*  m_Head;  ///< 首块
    FChunk*  m_Tail;  ///< 尾块
    SizeType m_Size;  ///< 总元素数
};

} // namespace Limx
