/*******************************************************************************
 * 文件: TObjectPool.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   对象池 — 固定类型对象的批量预分配与快速回收
 *   从预分配的内存块中 O(1) 获取/归还对象，避免频繁堆分配
 *   用于粒子、实体、网络消息、渲染命令等高频创建/销毁场景
 *
 * 设计哲学:
 *   空闲链表 — 空闲对象通过侵入式链表串联，O(1) 获取/归还
 *   分块增长 — 内存不足时分配新的对象块，已分配块在池销毁时统一释放
 *   构造/析构分离 — Acquire 调用构造函数，Release 调用析构函数
 *
 * 技术特性:
 *   - Acquire(): O(1) 获取对象 (从空闲链表取出 + placement new)
 *   - Release(ptr): O(1) 归还对象 (析构 + 加入空闲链表)
 *   - GetActiveCount/GetCapacity: 统计查询
 *   - 自动扩容: 空闲链表耗尽时分配新块
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

/// 对象池 — 固定类型批量分配/回收
/// @tparam T 对象类型
template<typename T>
class TObjectPool
{
    /// 空闲节点 — 侵入式链表
    union FreeNode
    {
        FreeNode*   Next;
        alignas(T) UInt8 Storage[sizeof(T)];
    };

    /// 内存块头 — 追踪已分配的块以便统一释放
    struct Block
    {
        Block*      Next;           ///< 下一个块
        SizeType    ObjectCount;    ///< 块中对象数
    };

    static constexpr SizeType kDefaultBlockSize = 64;

public:
    // ========================================================================
    // 构造与析构
    // ========================================================================

    /// 默认构造 — 指定每块对象数
    explicit TObjectPool(SizeType objectsPerBlock = kDefaultBlockSize)
        : m_FreeHead(nullptr)
        , m_BlockHead(nullptr)
        , m_ObjectsPerBlock(objectsPerBlock > 0 ? objectsPerBlock
                                                  : kDefaultBlockSize)
        , m_Capacity(0)
        , m_ActiveCount(0)
        , m_Allocator(&GetDefaultAllocator())
    {
    }

    ~TObjectPool()
    {
        // 释放所有内存块 (不析构活跃对象 — 用户应先 Release 所有对象)
        Block* block = m_BlockHead;
        while (block)
        {
            Block* next = block->Next;
            m_Allocator->Deallocate(block);
            block = next;
        }
    }

    // 不可拷贝/移动
    TObjectPool(const TObjectPool&) = delete;
    TObjectPool& operator=(const TObjectPool&) = delete;

    // ========================================================================
    // 获取与归还
    // ========================================================================

    /// 获取一个对象 (默认构造)
    LIMX_NODISCARD T* Acquire()
    {
        if (!m_FreeHead)
        {
            AllocateBlock();
        }

        // 从空闲链表取出
        FreeNode* node = m_FreeHead;
        m_FreeHead = node->Next;
        m_ActiveCount++;

        // placement new 调用默认构造
        return new (node->Storage) T();
    }

    /// 归还对象 (调用析构函数)
    void Release(T* object)
    {
        LIMX_ASSERT(object != nullptr);
        LIMX_ASSERT(m_ActiveCount > 0);

        // 调用析构函数
        object->~T();

        // 将内存归还到空闲链表
        FreeNode* node = reinterpret_cast<FreeNode*>(object);
        node->Next = m_FreeHead;
        m_FreeHead = node;
        m_ActiveCount--;
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 当前活跃对象数
    LIMX_NODISCARD SizeType GetActiveCount() const
    {
        return m_ActiveCount;
    }

    /// 总容量 (已分配的对象槽位数)
    LIMX_NODISCARD SizeType GetCapacity() const { return m_Capacity; }

    /// 空闲对象数
    LIMX_NODISCARD SizeType GetFreeCount() const
    {
        return m_Capacity - m_ActiveCount;
    }

    /// 每块对象数
    LIMX_NODISCARD SizeType GetObjectsPerBlock() const
    {
        return m_ObjectsPerBlock;
    }

private:
    /// 分配新的对象块
    void AllocateBlock()
    {
        // 块布局: [Block 头] + [FreeNode × objectsPerBlock]
        SizeType blockHeaderSize = sizeof(Block);
        // 对齐到 FreeNode 对齐要求
        SizeType alignment = alignof(FreeNode);
        blockHeaderSize = (blockHeaderSize + alignment - 1) &
                          ~(alignment - 1);

        SizeType totalSize = blockHeaderSize +
                             sizeof(FreeNode) * m_ObjectsPerBlock;

        UInt8* rawMemory = static_cast<UInt8*>(
            m_Allocator->Allocate(totalSize, alignment));

        // 初始化块头
        Block* block = reinterpret_cast<Block*>(rawMemory);
        block->Next = m_BlockHead;
        block->ObjectCount = m_ObjectsPerBlock;
        m_BlockHead = block;

        // 初始化空闲链表
        FreeNode* nodes = reinterpret_cast<FreeNode*>(
            rawMemory + blockHeaderSize);

        for (SizeType index = 0; index < m_ObjectsPerBlock; ++index)
        {
            nodes[index].Next = m_FreeHead;
            m_FreeHead = &nodes[index];
        }

        m_Capacity += m_ObjectsPerBlock;
    }

    // ========================================================================
    // 成员数据
    // ========================================================================

    FreeNode*   m_FreeHead;         ///< 空闲链表头
    Block*      m_BlockHead;        ///< 已分配块链表头
    SizeType    m_ObjectsPerBlock;  ///< 每块对象数
    SizeType    m_Capacity;         ///< 总容量
    SizeType    m_ActiveCount;      ///< 活跃对象数
    IAllocator* m_Allocator;        ///< 内存分配器
};

} // namespace Limx
