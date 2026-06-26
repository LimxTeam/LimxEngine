/*******************************************************************************
 * 文件: TPool.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   对象池 — 固定大小对象的层块式分配器
 *   预分配对象块，O(1) Acquire/Release，内存局部性良好
 *   用于频繁创建/销毁的粒子、组件、消息、节点等小型对象
 *
 * 设计哲学:
 *   层块 — 按 BlockSize 块分配，减少碎片
 *   空闲链表 — Release 将对象返回空闲链表，O(1) 再利用
 *   原始内存 — Acquire 返回未构造内存，使用者负责 Placement New
 *
 * 技术特性:
 *   - TPool<T, BlockSize>: 对象池
 *   - Acquire: 获取原始内存
 *   - Release: 归还对象内存
 *   - GetActiveCount: 活跃对象数
 *   - Reset: 释放所有内存
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Memory/IAllocator.h,
 *          Core/Memory/DefaultAllocator.h, Core/Memory/MemoryOps.h
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

/// 对象池
/// @tparam T 对象类型
/// @tparam BlockSize 每块容纳的对象数 (默认 64)
template<typename T, SizeType BlockSize = 64>
class TPool
{
    static_assert(BlockSize > 0,
        "BlockSize must be > 0");

    /// 空闲链表节点
    struct FFreeNode
    {
        FFreeNode* Next;
    };

    static_assert(sizeof(T) >= sizeof(FFreeNode*),
        "T must be large enough to hold a free list pointer");

    /// 内存块
    struct FBlock
    {
        alignas(T) UInt8 Data[sizeof(T) * BlockSize];
        FBlock* Next;
    };

public:
    TPool()
        : m_FreeList(nullptr)
        , m_BlockHead(nullptr)
        , m_ActiveCount(0)
        , m_TotalCapacity(0)
    {
    }

    ~TPool()
    {
        Reset();
    }

    // 不可拷贝
    TPool(const TPool&) = delete;
    TPool& operator=(const TPool&) = delete;

    // ========================================================================
    // 分配/释放
    // ========================================================================

    /// 获取一块未构造对象内存
    /// @return 对齐内存指针 (调用者须使用 Placement New 构造)
    LIMX_NODISCARD void* Acquire()
    {
        if (m_FreeList == nullptr)
        {
            GrowBlock();
        }

        FFreeNode* freeNode = m_FreeList;
        m_FreeList = freeNode->Next;
        ++m_ActiveCount;
        return static_cast<void*>(freeNode);
    }

    /// 归还对象内存
    /// @param ptr 由 Acquire 返回的指针 (调用者须先手动析构对象)
    void Release(void* ptr)
    {
        LIMX_ASSERT(ptr != nullptr);
        FFreeNode* freeNode =
            static_cast<FFreeNode*>(ptr);
        freeNode->Next = m_FreeList;
        m_FreeList = freeNode;
        --m_ActiveCount;
    }

    /// 构造对象 (便捷封装: Acquire + Placement New)
    template<typename... Args>
    LIMX_NODISCARD T* Create(Args&&... args)
    {
        void* mem = Acquire();
        return new (mem) T(static_cast<Args&&>(args)...);
    }

    /// 析构并归还对象 (便捷封装: 析构 + Release)
    void Destroy(T* obj)
    {
        LIMX_ASSERT(obj != nullptr);
        obj->~T();
        Release(static_cast<void*>(obj));
    }

    // ========================================================================
    // 重置
    // ========================================================================

    /// 释放所有内存块 (不调用析构函数)
    void Reset()
    {
        FBlock* block = m_BlockHead;
        while (block != nullptr)
        {
            FBlock* next = block->Next;
            GetDefaultAllocator().Deallocate(block);
            block = next;
        }
        m_BlockHead     = nullptr;
        m_FreeList      = nullptr;
        m_ActiveCount   = 0;
        m_TotalCapacity = 0;
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 当前活跃对象数
    LIMX_NODISCARD SizeType GetActiveCount() const
    {
        return m_ActiveCount;
    }

    /// 总容量 (已分配的块数 * BlockSize)
    LIMX_NODISCARD SizeType GetTotalCapacity() const
    {
        return m_TotalCapacity;
    }

    /// 空闲槽数
    LIMX_NODISCARD SizeType GetFreeCount() const
    {
        return m_TotalCapacity - m_ActiveCount;
    }

    /// 是否为空
    LIMX_NODISCARD bool IsEmpty() const
    {
        return m_ActiveCount == 0;
    }

private:
    void GrowBlock()
    {
        FBlock* newBlock = static_cast<FBlock*>(
            GetDefaultAllocator().Allocate(
                sizeof(FBlock), alignof(FBlock)));
        newBlock->Next = m_BlockHead;
        m_BlockHead = newBlock;

        // 将新块中所有槽加入空闲链表
        for (SizeType slotIdx = 0;
             slotIdx < BlockSize; ++slotIdx)
        {
            void* slotPtr = static_cast<void*>(
                newBlock->Data + slotIdx * sizeof(T));
            FFreeNode* freeNode =
                static_cast<FFreeNode*>(slotPtr);
            freeNode->Next = m_FreeList;
            m_FreeList = freeNode;
        }

        m_TotalCapacity += BlockSize;
    }

    FFreeNode* m_FreeList;      ///< 空闲链表头
    FBlock*    m_BlockHead;     ///< 内存块链表头
    SizeType   m_ActiveCount;   ///< 活跃对象数
    SizeType   m_TotalCapacity; ///< 总容量
};

} // namespace Limx
