/*******************************************************************************
 * 文件: TLockFreeQueue.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   无锁 SPSC 队列 — 单生产者单消费者的无锁环形缓冲区
 *   生产者和消费者各自维护独立的头/尾指针，无需互斥锁
 *   用于音频线程通信、渲染提交队列、日志异步写入等场景
 *
 * 设计哲学:
 *   SPSC 约束 — 严格单生产者单消费者，保证无锁安全
 *   缓存友好 — 头/尾指针分离到不同缓存行避免伪共享
 *   固定容量 — 环形缓冲区容量在构造时确定
 *
 * 技术特性:
 *   - Enqueue: 生产者端 O(1) 入队 (无锁)
 *   - Dequeue: 消费者端 O(1) 出队 (无锁)
 *   - 原子读写仅用 memory_order_acquire/release
 *   - 容量为 2 的幂 (掩码取模)
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h,
 *          Core/Threading/FAtomic.h, Core/Memory/DefaultAllocator.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Threading/FAtomic.h"
#include "Core/Memory/MemoryOps.h"
#include "Core/Memory/DefaultAllocator.h"

namespace Limx
{

/// 无锁 SPSC 队列 — 单生产者单消费者
/// @tparam T 元素类型
template<typename T>
class TLockFreeQueue
{
public:
    // ========================================================================
    // 构造与析构
    // ========================================================================

    /// 构造 — capacity 会向上取整到 2 的幂
    explicit TLockFreeQueue(SizeType capacity)
        : m_Allocator(&GetDefaultAllocator())
    {
        // 向上取整到 2 的幂
        m_Capacity = NextPowerOfTwo(capacity);
        m_Mask = m_Capacity - 1;

        m_Buffer = static_cast<T*>(
            m_Allocator->Allocate(
                m_Capacity * sizeof(T), alignof(T)));

        m_Head.Store(0);
        m_Tail.Store(0);
    }

    ~TLockFreeQueue()
    {
        // 析构残留元素
        SizeType head = m_Head.Load();
        SizeType tail = m_Tail.Load();
        while (head != tail)
        {
            m_Buffer[head & m_Mask].~T();
            ++head;
        }

        if (m_Buffer)
        {
            m_Allocator->Deallocate(m_Buffer);
        }
    }

    // 不可拷贝/移动
    TLockFreeQueue(const TLockFreeQueue&) = delete;
    TLockFreeQueue& operator=(const TLockFreeQueue&) = delete;

    // ========================================================================
    // 生产者接口
    // ========================================================================

    /// 入队 (拷贝) — 仅生产者线程调用
    /// @return 成功返回 true，队列满返回 false
    LIMX_NODISCARD bool Enqueue(const T& value)
    {
        SizeType tail = m_Tail.Load();
        SizeType nextTail = tail + 1;

        // 检查是否已满
        if (nextTail - m_Head.Load() > m_Capacity)
        {
            return false;
        }

        // 放置元素
        new (&m_Buffer[tail & m_Mask]) T(value);

        // 发布尾指针
        m_Tail.Store(nextTail);
        return true;
    }

    /// 入队 (移动) — 仅生产者线程调用
    LIMX_NODISCARD bool Enqueue(T&& value)
    {
        SizeType tail = m_Tail.Load();
        SizeType nextTail = tail + 1;

        if (nextTail - m_Head.Load() > m_Capacity)
        {
            return false;
        }

        new (&m_Buffer[tail & m_Mask]) T(MoveTemp(value));
        m_Tail.Store(nextTail);
        return true;
    }

    // ========================================================================
    // 消费者接口
    // ========================================================================

    /// 出队 — 仅消费者线程调用
    /// @param outValue 接收出队的元素
    /// @return 成功返回 true，队列空返回 false
    LIMX_NODISCARD bool Dequeue(T& outValue)
    {
        SizeType head = m_Head.Load();

        // 检查是否为空
        if (head == m_Tail.Load())
        {
            return false;
        }

        // 取出元素
        T& slot = m_Buffer[head & m_Mask];
        outValue = MoveTemp(slot);
        slot.~T();

        // 发布头指针
        m_Head.Store(head + 1);
        return true;
    }

    // ========================================================================
    // 状态查询 (非精确 — 可能在查询瞬间变化)
    // ========================================================================

    /// 近似元素数量
    LIMX_NODISCARD SizeType GetSizeApprox() const
    {
        SizeType tail = m_Tail.Load();
        SizeType head = m_Head.Load();
        return tail - head;
    }

    /// 是否近似为空
    LIMX_NODISCARD bool IsEmptyApprox() const
    {
        return GetSizeApprox() == 0;
    }

    /// 容量
    LIMX_NODISCARD SizeType GetCapacity() const { return m_Capacity; }

private:
    /// 向上取整到 2 的幂
    static SizeType NextPowerOfTwo(SizeType value)
    {
        if (value == 0) return 1;
        --value;
        value |= value >> 1;
        value |= value >> 2;
        value |= value >> 4;
        value |= value >> 8;
        value |= value >> 16;
        value |= value >> 32;
        return value + 1;
    }

    // ========================================================================
    // 成员数据 (缓存行分离)
    // ========================================================================

    alignas(64) TAtomic<SizeType> m_Head;       ///< 消费者头指针
    alignas(64) TAtomic<SizeType> m_Tail;       ///< 生产者尾指针
    T*          m_Buffer;                        ///< 环形缓冲区
    SizeType    m_Capacity;                      ///< 容量 (2 的幂)
    SizeType    m_Mask;                          ///< 掩码 (capacity - 1)
    IAllocator* m_Allocator;                     ///< 内存分配器
};

} // namespace Limx
