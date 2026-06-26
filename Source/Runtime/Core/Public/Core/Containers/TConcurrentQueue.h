/*******************************************************************************
 * 文件: TConcurrentQueue.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   线程安全队列 — 基于互斥锁的 MPMC (多生产者多消费者) 队列
 *   封装 FMutex + FConditionVariable 实现阻塞/非阻塞入队/出队
 *   用于线程间消息传递、任务分发、日志缓冲等场景
 *
 * 设计哲学:
 *   简单正确 — 基于锁的实现，避免无锁队列的 ABA 复杂性
 *   阻塞等待 — Dequeue 可阻塞等待直到有元素可用
 *   批量操作 — DequeueAll 一次取出所有元素减少锁争用
 *
 * 技术特性:
 *   - Enqueue: 线程安全入队 (拷贝/移动)
 *   - TryDequeue: 非阻塞出队 (返回是否成功)
 *   - Dequeue: 阻塞出队 (等待直到有元素)
 *   - DequeueAll: 批量出队到 TArray
 *   - GetSize: 当前元素数 (近似值)
 *
 * 依赖关系:
 *   内部: Core/Threading/FMutex.h, Core/Containers/TQueue.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Threading/FMutex.h"
#include "Core/Containers/TQueue.h"
#include "Core/Containers/TArray.h"

namespace Limx
{

/// 线程安全 MPMC 队列
/// @tparam T 元素类型
template<typename T>
class TConcurrentQueue
{
public:
    // ========================================================================
    // 构造与析构
    // ========================================================================

    TConcurrentQueue() = default;
    ~TConcurrentQueue() = default;

    // 不可拷贝/移动
    TConcurrentQueue(const TConcurrentQueue&) = delete;
    TConcurrentQueue& operator=(const TConcurrentQueue&) = delete;

    // ========================================================================
    // 入队
    // ========================================================================

    /// 线程安全入队 (拷贝)
    void Enqueue(const T& value)
    {
        {
            FScopeLock lock(m_Mutex);
            m_Queue.Enqueue(value);
        }
        m_Condition.NotifyOne();
    }

    /// 线程安全入队 (移动)
    void Enqueue(T&& value)
    {
        {
            FScopeLock lock(m_Mutex);
            m_Queue.Enqueue(MoveTemp(value));
        }
        m_Condition.NotifyOne();
    }

    // ========================================================================
    // 出队
    // ========================================================================

    /// 非阻塞出队 — 返回是否成功
    LIMX_NODISCARD bool TryDequeue(T& outValue)
    {
        FScopeLock lock(m_Mutex);
        if (m_Queue.IsEmpty())
        {
            return false;
        }
        outValue = MoveTemp(m_Queue.Peek());
        m_Queue.Dequeue();
        return true;
    }

    /// 阻塞出队 — 等待直到有元素可用
    void Dequeue(T& outValue)
    {
        FScopeLock lock(m_Mutex);
        m_Condition.Wait(m_Mutex, [this]()
        {
            return !m_Queue.IsEmpty();
        });
        outValue = MoveTemp(m_Queue.Peek());
        m_Queue.Dequeue();
    }

    /// 批量出队 — 一次取出所有元素到数组
    void DequeueAll(TArray<T>& outArray)
    {
        FScopeLock lock(m_Mutex);
        while (!m_Queue.IsEmpty())
        {
            outArray.Add(MoveTemp(m_Queue.Peek()));
            m_Queue.Dequeue();
        }
    }

    // ========================================================================
    // 状态查询
    // ========================================================================

    /// 当前元素数 (近似值 — 调用后可能已变化)
    LIMX_NODISCARD SizeType GetSize() const
    {
        FScopeLock lock(const_cast<FMutex&>(m_Mutex));
        return m_Queue.GetSize();
    }

    /// 是否为空 (近似值)
    LIMX_NODISCARD bool IsEmpty() const
    {
        FScopeLock lock(const_cast<FMutex&>(m_Mutex));
        return m_Queue.IsEmpty();
    }

private:
    TQueue<T>          m_Queue;      ///< 底层无锁队列
    FMutex             m_Mutex;      ///< 保护锁
    FConditionVariable m_Condition;  ///< 非空条件
};

} // namespace Limx
