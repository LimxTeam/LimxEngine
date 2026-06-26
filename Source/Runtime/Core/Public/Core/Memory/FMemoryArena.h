/*******************************************************************************
 * 文件: FMemoryArena.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   内存竞技场 — 帧级线性分配器
 *   从预分配的大块内存中线性分配，通过一次性重置释放所有分配
 *   用于帧临时数据、渲染命令缓冲、每帧粒子数据等短生命周期场景
 *
 * 设计哲学:
 *   极速分配 — 仅移动指针，O(1) 分配
 *   批量释放 — Reset 一次性释放所有分配，无逐个 Free
 *   多块链式 — 首块用完后自动分配新块，形成块链
 *
 * 技术特性:
 *   - FMemoryArena: 帧级竞技场分配器
 *   - Allocate: 线性分配 (对齐)
 *   - Reset: 重置所有分配 (不释放内存)
 *   - Release: 释放所有块内存
 *   - GetUsedBytes/GetTotalBytes: 统计查询
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Memory/MemoryOps.h,
 *          Core/Memory/DefaultAllocator.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Memory/MemoryOps.h"
#include "Core/Memory/IAllocator.h"
#include "Core/Memory/DefaultAllocator.h"

namespace Limx
{

/// 内存竞技场 — 帧级线性分配器
class FMemoryArena
{
    /// 内存块
    struct FBlock
    {
        UInt8*   Data;      ///< 块数据
        SizeType Capacity;  ///< 块容量
        SizeType Used;      ///< 已使用字节
        FBlock*  Next;      ///< 下一块
    };

    static constexpr SizeType kDefaultBlockSize = 65536; // 64KB

public:
    explicit FMemoryArena(SizeType blockSize = kDefaultBlockSize)
        : m_BlockSize(blockSize)
        , m_Head(nullptr)
        , m_Current(nullptr)
        , m_TotalAllocated(0)
    {
    }

    ~FMemoryArena()
    {
        Release();
    }

    // 不可拷贝
    FMemoryArena(const FMemoryArena&) = delete;
    FMemoryArena& operator=(const FMemoryArena&) = delete;

    // 可移动
    FMemoryArena(FMemoryArena&& other) noexcept
        : m_BlockSize(other.m_BlockSize)
        , m_Head(other.m_Head)
        , m_Current(other.m_Current)
        , m_TotalAllocated(other.m_TotalAllocated)
    {
        other.m_Head = nullptr;
        other.m_Current = nullptr;
        other.m_TotalAllocated = 0;
    }

    // ========================================================================
    // 分配
    // ========================================================================

    /// 线性分配 (对齐)
    /// @param size 请求字节数
    /// @param alignment 对齐要求 (必须为 2 的幂)
    /// @return 分配的内存指针
    void* Allocate(SizeType size, SizeType alignment = 16)
    {
        LIMX_ASSERT(size > 0);
        LIMX_ASSERT((alignment & (alignment - 1)) == 0);

        // 尝试在当前块分配
        if (m_Current != nullptr)
        {
            void* result = TryAllocateFromBlock(
                m_Current, size, alignment);
            if (result != nullptr) return result;
        }

        // 当前块空间不足 — 尝试后续块 (Reset 后可能有空闲块)
        if (m_Current != nullptr && m_Current->Next != nullptr)
        {
            FBlock* candidate = m_Current->Next;
            while (candidate != nullptr)
            {
                candidate->Used = 0; // 重置此块
                void* result = TryAllocateFromBlock(
                    candidate, size, alignment);
                if (result != nullptr)
                {
                    m_Current = candidate;
                    return result;
                }
                candidate = candidate->Next;
            }
        }

        // 需要新块
        SizeType requiredSize = size + alignment;
        SizeType newBlockCapacity = m_BlockSize;
        if (requiredSize > newBlockCapacity)
        {
            newBlockCapacity = requiredSize;
        }

        FBlock* newBlock = AllocateBlock(newBlockCapacity);
        if (m_Head == nullptr)
        {
            m_Head = newBlock;
        }
        else if (m_Current != nullptr)
        {
            // 插入到当前块之后
            newBlock->Next = m_Current->Next;
            m_Current->Next = newBlock;
        }
        m_Current = newBlock;

        void* result = TryAllocateFromBlock(
            newBlock, size, alignment);
        LIMX_ASSERT(result != nullptr);
        return result;
    }

    /// 分配并零初始化
    void* AllocateZeroed(SizeType size, SizeType alignment = 16)
    {
        void* ptr = Allocate(size, alignment);
        Memory::MemZero(ptr, size);
        return ptr;
    }

    /// 分配类型化对象 (默认构造)
    template<typename T>
    T* AllocateTyped()
    {
        void* memory = Allocate(sizeof(T), alignof(T));
        return new (memory) T();
    }

    // ========================================================================
    // 重置与释放
    // ========================================================================

    /// 重置所有分配 (不释放内存块)
    /// 下次分配从头开始复用已有块
    void Reset()
    {
        FBlock* block = m_Head;
        while (block != nullptr)
        {
            block->Used = 0;
            block = block->Next;
        }
        m_Current = m_Head;
    }

    /// 释放所有块内存
    void Release()
    {
        FBlock* block = m_Head;
        while (block != nullptr)
        {
            FBlock* next = block->Next;
            GetDefaultAllocator().Deallocate(block->Data);
            GetDefaultAllocator().Deallocate(block);
            block = next;
        }
        m_Head = nullptr;
        m_Current = nullptr;
        m_TotalAllocated = 0;
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 当前已使用字节数
    LIMX_NODISCARD SizeType GetUsedBytes() const
    {
        SizeType total = 0;
        FBlock* block = m_Head;
        while (block != nullptr)
        {
            total += block->Used;
            block = block->Next;
        }
        return total;
    }

    /// 总分配字节数 (所有块容量之和)
    LIMX_NODISCARD SizeType GetTotalBytes() const
    {
        return m_TotalAllocated;
    }

    /// 块数量
    LIMX_NODISCARD SizeType GetBlockCount() const
    {
        SizeType count = 0;
        FBlock* block = m_Head;
        while (block != nullptr)
        {
            ++count;
            block = block->Next;
        }
        return count;
    }

    /// 是否为空 (未分配任何内存)
    LIMX_NODISCARD bool IsEmpty() const
    {
        return m_Head == nullptr;
    }

private:
    /// 从指定块尝试分配
    void* TryAllocateFromBlock(
        FBlock* block, SizeType size, SizeType alignment)
    {
        // 对齐当前偏移
        SizeType aligned = (block->Used + alignment - 1) &
                            ~(alignment - 1);
        if (aligned + size <= block->Capacity)
        {
            void* result = block->Data + aligned;
            block->Used = aligned + size;
            return result;
        }
        return nullptr;
    }

    /// 分配新块
    FBlock* AllocateBlock(SizeType capacity)
    {
        FBlock* block = static_cast<FBlock*>(
            GetDefaultAllocator().Allocate(
                sizeof(FBlock), alignof(FBlock)));
        block->Data = static_cast<UInt8*>(
            GetDefaultAllocator().Allocate(capacity, 16));
        block->Capacity = capacity;
        block->Used = 0;
        block->Next = nullptr;
        m_TotalAllocated += capacity;
        return block;
    }

    SizeType m_BlockSize;       ///< 默认块大小
    FBlock*  m_Head;            ///< 块链头
    FBlock*  m_Current;         ///< 当前分配块
    SizeType m_TotalAllocated;  ///< 总分配内存
};

} // namespace Limx
