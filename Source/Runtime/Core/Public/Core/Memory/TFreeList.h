/*******************************************************************************
 * 文件: TFreeList.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   泛型空闲链表 — 固定大小内存块的回收池
 *   维护已释放内存块的链表，O(1) 获取和归还
 *   用于自定义分配器内部、Slab 分配、节点分配器等场景
 *
 * 设计哲学:
 *   侵入式 — 空闲块的前 8 字节存储 next 指针，无额外开销
 *   分块扩容 — 内存不足时从底层分配器获取整块，切分为节点
 *   无类型 — 操作 void 指针，由调用者负责类型转换
 *
 * 技术特性:
 *   - Allocate(): O(1) 从空闲链表取块
 *   - Deallocate(ptr): O(1) 归还到空闲链表
 *   - GetBlockSize: 每块大小
 *   - GetFreeCount: 空闲块数
 *   - 自动扩容: 空闲链表耗尽时分配新的 Chunk
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h,
 *          Core/Memory/IAllocator.h, Core/Memory/DefaultAllocator.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Memory/IAllocator.h"
#include "Core/Memory/DefaultAllocator.h"

namespace Limx
{

/// 泛型空闲链表 — 固定大小内存块回收池
class TFreeList
{
    /// 空闲节点 — 侵入式链表
    struct FreeNode
    {
        FreeNode* Next;
    };

    /// 已分配的大块内存
    struct Chunk
    {
        Chunk* Next;
    };

public:
    // ========================================================================
    // 构造与析构
    // ========================================================================

    /// 构造
    /// @param blockSize     每个块的大小 (字节，至少 sizeof(void*))
    /// @param blocksPerChunk 每次扩容分配的块数
    /// @param alignment     对齐要求
    TFreeList(SizeType blockSize,
              SizeType blocksPerChunk = 64,
              SizeType alignment = 8)
        : m_FreeHead(nullptr)
        , m_ChunkHead(nullptr)
        , m_BlockSize(blockSize < sizeof(FreeNode) ? sizeof(FreeNode)
                                                     : blockSize)
        , m_BlocksPerChunk(blocksPerChunk)
        , m_Alignment(alignment)
        , m_FreeCount(0)
        , m_TotalBlocks(0)
        , m_Allocator(&GetDefaultAllocator())
    {
        // 对齐块大小
        m_BlockSize = (m_BlockSize + m_Alignment - 1) &
                      ~(m_Alignment - 1);
    }

    ~TFreeList()
    {
        // 释放所有 Chunk
        Chunk* chunk = m_ChunkHead;
        while (chunk)
        {
            Chunk* next = chunk->Next;
            m_Allocator->Deallocate(chunk);
            chunk = next;
        }
    }

    // 不可拷贝/移动
    TFreeList(const TFreeList&) = delete;
    TFreeList& operator=(const TFreeList&) = delete;

    // ========================================================================
    // 分配与释放
    // ========================================================================

    /// 从空闲链表获取一个块 — O(1)
    LIMX_NODISCARD void* Allocate()
    {
        if (!m_FreeHead)
        {
            GrowChunk();
        }

        FreeNode* node = m_FreeHead;
        m_FreeHead = node->Next;
        --m_FreeCount;
        return node;
    }

    /// 归还块到空闲链表 — O(1)
    void Deallocate(void* ptr)
    {
        LIMX_ASSERT(ptr != nullptr);

        FreeNode* node = static_cast<FreeNode*>(ptr);
        node->Next = m_FreeHead;
        m_FreeHead = node;
        ++m_FreeCount;
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 每块大小 (对齐后)
    LIMX_NODISCARD SizeType GetBlockSize() const { return m_BlockSize; }

    /// 空闲块数
    LIMX_NODISCARD SizeType GetFreeCount() const { return m_FreeCount; }

    /// 已分配总块数
    LIMX_NODISCARD SizeType GetTotalBlocks() const
    {
        return m_TotalBlocks;
    }

    /// 活跃块数 (已分配 - 空闲)
    /// 判断指针是否由本自由列表分出
    ///
    /// 遍历 chunk 链表做地址区间与块边界双重校验。上层分配器在只持有指针、
    /// 不知道原始尺寸时靠它定位归属, 从而避免把本列表的内存错误地交给
    /// 其他分配器释放 (那会直接损坏堆)。
    ///
    /// 复杂度 O(chunk 数); chunk 数 = 总块数 / 每 chunk 块数, 实际很小。
    LIMX_NODISCARD bool Owns(const void* ptr) const
    {
        if (ptr == nullptr)
        {
            return false;
        }

        const UInt8* target = static_cast<const UInt8*>(ptr);
        const SizeType headerSize = GetChunkHeaderSize();
        const SizeType dataSize   = m_BlockSize * m_BlocksPerChunk;

        for (const Chunk* chunk = m_ChunkHead;
             chunk != nullptr;
             chunk = chunk->Next)
        {
            const UInt8* dataBegin =
                reinterpret_cast<const UInt8*>(chunk) + headerSize;
            const UInt8* dataEnd = dataBegin + dataSize;

            if (target < dataBegin || target >= dataEnd)
            {
                continue;
            }

            // 落在 chunk 内还需对齐到块边界 — 块中间的地址不是合法起点
            const SizeType offset = static_cast<SizeType>(target - dataBegin);
            return (offset % m_BlockSize) == 0;
        }

        return false;
    }

    LIMX_NODISCARD SizeType GetActiveCount() const
    {
        return m_TotalBlocks - m_FreeCount;
    }

private:
    /// Chunk 头的对齐后尺寸 — GrowChunk 与 Owns 必须使用同一算法,
    /// 否则 Owns 计算出的数据区起点会与实际布局错位
    LIMX_NODISCARD SizeType GetChunkHeaderSize() const
    {
        return (sizeof(Chunk) + m_Alignment - 1) & ~(m_Alignment - 1);
    }

    /// 分配新的 Chunk 并切分为空闲块
    void GrowChunk()
    {
        // Chunk 布局: [Chunk 头] + [Block × blocksPerChunk]
        SizeType headerSize = GetChunkHeaderSize();

        SizeType totalSize = headerSize +
                             m_BlockSize * m_BlocksPerChunk;

        UInt8* raw = static_cast<UInt8*>(
            m_Allocator->Allocate(totalSize, m_Alignment));

        // 初始化 Chunk 头
        Chunk* chunk = reinterpret_cast<Chunk*>(raw);
        chunk->Next = m_ChunkHead;
        m_ChunkHead = chunk;

        // 切分为空闲块，加入链表
        UInt8* blockStart = raw + headerSize;
        for (SizeType index = 0;
             index < m_BlocksPerChunk; ++index)
        {
            FreeNode* node = reinterpret_cast<FreeNode*>(
                blockStart + index * m_BlockSize);
            node->Next = m_FreeHead;
            m_FreeHead = node;
        }

        m_FreeCount += m_BlocksPerChunk;
        m_TotalBlocks += m_BlocksPerChunk;
    }

    // ========================================================================
    // 成员数据
    // ========================================================================

    FreeNode*   m_FreeHead;        ///< 空闲链表头
    Chunk*      m_ChunkHead;       ///< 已分配 Chunk 链表头
    SizeType    m_BlockSize;       ///< 每块大小 (对齐后)
    SizeType    m_BlocksPerChunk;  ///< 每次扩容块数
    SizeType    m_Alignment;       ///< 对齐要求
    SizeType    m_FreeCount;       ///< 空闲块数
    SizeType    m_TotalBlocks;     ///< 已分配总块数
    IAllocator* m_Allocator;       ///< 底层分配器
};

} // namespace Limx
