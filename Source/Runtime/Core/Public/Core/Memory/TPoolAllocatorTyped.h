/*******************************************************************************
 * 文件: TPoolAllocatorTyped.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   类型化池分配器 — 针对特定类型优化的固定大小块分配器
 *   预分配固定数量的同类型对象槽位，通过空闲链表 O(1) 分配/回收
 *   用于频繁创建/销毁的同类型小对象，如粒子、节点、组件等
 *
 * 设计哲学:
 *   类型安全 — 模板参数绑定具体类型，编译时确定对齐和大小
 *   空闲链表 — 空闲槽位通过嵌入式链表连接，无额外内存开销
 *   批量预分配 — 一次分配整个块，块用完后分配新块
 *
 * 技术特性:
 *   - TPoolAllocatorTyped<T, BlockSize>: 类型化池分配器
 *   - Allocate: O(1) 分配一个 T 大小的内存块
 *   - Deallocate: O(1) 回收
 *   - Construct/Destroy: 分配+构造 / 析构+回收
 *   - GetAllocatedCount: 当前分配数
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

/// 类型化池分配器
/// @tparam T 目标类型
/// @tparam BlockSize 每个块中的对象数 (默认 64)
template<typename T, SizeType BlockSize = 64>
class TPoolAllocatorTyped
{
    /// 空闲节点 — 嵌入在未使用的槽位中
    struct FFreeNode
    {
        FFreeNode* Next;
    };

    /// 确保槽位大小至少能容纳空闲链表指针
    static constexpr SizeType kSlotSize =
        sizeof(T) > sizeof(FFreeNode) ? sizeof(T)
                                       : sizeof(FFreeNode);
    static constexpr SizeType kSlotAlignment =
        alignof(T) > alignof(FFreeNode) ? alignof(T)
                                         : alignof(FFreeNode);

    /// 内存块头
    struct FBlockHeader
    {
        FBlockHeader* Next;  ///< 下一个块
    };

public:
    TPoolAllocatorTyped()
        : m_FreeList(nullptr)
        , m_BlockList(nullptr)
        , m_AllocatedCount(0)
        , m_TotalSlots(0)
    {
    }

    ~TPoolAllocatorTyped()
    {
        ReleaseAllBlocks();
    }

    // 不可拷贝/移动
    TPoolAllocatorTyped(const TPoolAllocatorTyped&) = delete;
    TPoolAllocatorTyped& operator=(
        const TPoolAllocatorTyped&) = delete;
    TPoolAllocatorTyped(TPoolAllocatorTyped&&) = delete;
    TPoolAllocatorTyped& operator=(
        TPoolAllocatorTyped&&) = delete;

    // ========================================================================
    // 分配/回收 (原始内存)
    // ========================================================================

    /// 分配一个 T 大小的内存块
    LIMX_NODISCARD void* Allocate()
    {
        if (m_FreeList == nullptr)
        {
            AllocateNewBlock();
        }

        FFreeNode* node = m_FreeList;
        m_FreeList = node->Next;
        ++m_AllocatedCount;
        return static_cast<void*>(node);
    }

    /// 回收内存块
    void Deallocate(void* ptr)
    {
        LIMX_ASSERT(ptr != nullptr);
        FFreeNode* node = static_cast<FFreeNode*>(ptr);
        node->Next = m_FreeList;
        m_FreeList = node;
        --m_AllocatedCount;
    }

    // ========================================================================
    // 构造/析构 (类型安全)
    // ========================================================================

    /// 分配并默认构造
    LIMX_NODISCARD T* Construct()
    {
        void* memory = Allocate();
        return new (memory) T();
    }

    /// 分配并拷贝构造
    LIMX_NODISCARD T* Construct(const T& value)
    {
        void* memory = Allocate();
        return new (memory) T(value);
    }

    /// 分配并移动构造
    LIMX_NODISCARD T* Construct(T&& value)
    {
        void* memory = Allocate();
        return new (memory) T(MoveTemp(value));
    }

    /// 析构并回收
    void Destroy(T* ptr)
    {
        LIMX_ASSERT(ptr != nullptr);
        ptr->~T();
        Deallocate(static_cast<void*>(ptr));
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 当前已分配数
    LIMX_NODISCARD SizeType GetAllocatedCount() const
    {
        return m_AllocatedCount;
    }

    /// 总槽位数 (所有块)
    LIMX_NODISCARD SizeType GetTotalSlots() const
    {
        return m_TotalSlots;
    }

    /// 空闲槽位数
    LIMX_NODISCARD SizeType GetFreeCount() const
    {
        return m_TotalSlots - m_AllocatedCount;
    }

    /// 块数量
    LIMX_NODISCARD SizeType GetBlockCount() const
    {
        SizeType count = 0;
        FBlockHeader* block = m_BlockList;
        while (block != nullptr)
        {
            ++count;
            block = block->Next;
        }
        return count;
    }

    /// 释放所有块 (不调用对象析构函数)
    void ReleaseAllBlocks()
    {
        FBlockHeader* block = m_BlockList;
        while (block != nullptr)
        {
            FBlockHeader* next = block->Next;
            GetDefaultAllocator().Deallocate(block);
            block = next;
        }
        m_BlockList = nullptr;
        m_FreeList = nullptr;
        m_AllocatedCount = 0;
        m_TotalSlots = 0;
    }

private:
    /// 分配新块并将所有槽位加入空闲链表
    void AllocateNewBlock()
    {
        // 块布局: [FBlockHeader][Slot0][Slot1]...[SlotN-1]
        SizeType headerSize =
            (sizeof(FBlockHeader) + kSlotAlignment - 1) &
            ~(kSlotAlignment - 1);
        SizeType blockBytes =
            headerSize + BlockSize * kSlotSize;

        UInt8* raw = static_cast<UInt8*>(
            GetDefaultAllocator().Allocate(
                blockBytes, kSlotAlignment));

        // 初始化块头
        FBlockHeader* header =
            reinterpret_cast<FBlockHeader*>(raw);
        header->Next = m_BlockList;
        m_BlockList = header;

        // 将所有槽位加入空闲链表
        UInt8* slotsStart = raw + headerSize;
        for (SizeType slotIndex = 0;
             slotIndex < BlockSize; ++slotIndex)
        {
            FFreeNode* node = reinterpret_cast<FFreeNode*>(
                slotsStart + slotIndex * kSlotSize);
            node->Next = m_FreeList;
            m_FreeList = node;
        }

        m_TotalSlots += BlockSize;
    }

    FFreeNode*    m_FreeList;       ///< 空闲链表头
    FBlockHeader* m_BlockList;      ///< 块链表头
    SizeType      m_AllocatedCount; ///< 已分配数
    SizeType      m_TotalSlots;     ///< 总槽位数
};

} // namespace Limx
