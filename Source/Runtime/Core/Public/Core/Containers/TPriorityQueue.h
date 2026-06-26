/*******************************************************************************
 * 文件: TPriorityQueue.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   优先队列 — 基于二叉堆的最小/最大优先队列
 *   支持插入、弹出顶部、查看顶部元素
 *   用于任务调度、路径搜索 (A*)、事件排序等场景
 *
 * 设计哲学:
 *   数组堆 — 使用 TArray 存储完全二叉堆
 *   可定制比较 — 默认最小堆 (TLess)，可传入自定义比较器
 *   零额外分配 — 仅 TArray 的堆存储
 *
 * 技术特性:
 *   - TPriorityQueue<T, Pred>: 参数化优先队列
 *   - Push: 插入元素 O(log n)
 *   - Pop: 弹出顶部 O(log n)
 *   - Top: 查看顶部 O(1)
 *   - GetSize/IsEmpty: 查询
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Containers/TArray.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Containers/TArray.h"

namespace Limx
{

/// 默认最小堆比较器
template<typename T>
struct TPriorityLess
{
    LIMX_NODISCARD constexpr bool operator()(
        const T& a, const T& b) const
    {
        return a < b;
    }
};

/// 优先队列 (二叉堆)
/// @tparam T    元素类型
/// @tparam Pred 比较器 (默认最小堆: 最小元素在顶部)
template<typename T, typename Pred = TPriorityLess<T>>
class TPriorityQueue
{
public:
    TPriorityQueue() = default;

    explicit TPriorityQueue(Pred predicate)
        : m_Predicate(predicate)
    {
    }

    // ========================================================================
    // 操作
    // ========================================================================

    /// 插入元素
    void Push(const T& element)
    {
        m_Heap.Add(element);
        SiftUp(m_Heap.GetSize() - 1);
    }

    /// 插入元素 (移动)
    void Push(T&& element)
    {
        m_Heap.Add(MoveTemp(element));
        SiftUp(m_Heap.GetSize() - 1);
    }

    /// 弹出顶部元素
    void Pop()
    {
        LIMX_ASSERT(m_Heap.GetSize() > 0);

        SizeType lastIndex = m_Heap.GetSize() - 1;
        if (lastIndex > 0)
        {
            SwapElements(0, lastIndex);
        }
        m_Heap.RemoveAt(lastIndex);

        if (m_Heap.GetSize() > 1)
        {
            SiftDown(0);
        }
    }

    /// 查看顶部元素
    LIMX_NODISCARD const T& Top() const
    {
        LIMX_ASSERT(m_Heap.GetSize() > 0);
        return m_Heap[0];
    }

    LIMX_NODISCARD T& Top()
    {
        LIMX_ASSERT(m_Heap.GetSize() > 0);
        return m_Heap[0];
    }

    /// 弹出并返回顶部元素
    T PopAndGet()
    {
        LIMX_ASSERT(m_Heap.GetSize() > 0);
        T result = MoveTemp(m_Heap[0]);

        SizeType lastIndex = m_Heap.GetSize() - 1;
        if (lastIndex > 0)
        {
            m_Heap[0] = MoveTemp(m_Heap[lastIndex]);
        }
        m_Heap.RemoveAt(lastIndex);

        if (m_Heap.GetSize() > 1)
        {
            SiftDown(0);
        }

        return result;
    }

    /// 清空
    void Clear()
    {
        m_Heap.Clear();
    }

    // ========================================================================
    // 查询
    // ========================================================================

    LIMX_NODISCARD SizeType GetSize() const
    {
        return m_Heap.GetSize();
    }

    LIMX_NODISCARD bool IsEmpty() const
    {
        return m_Heap.GetSize() == 0;
    }

    /// 预分配容量
    void Reserve(SizeType capacity)
    {
        m_Heap.Reserve(capacity);
    }

private:
    // ========================================================================
    // 堆操作
    // ========================================================================

    /// 上浮 — 维护堆性质
    void SiftUp(SizeType index)
    {
        while (index > 0)
        {
            SizeType parentIndex = (index - 1) / 2;
            if (m_Predicate(m_Heap[index], m_Heap[parentIndex]))
            {
                SwapElements(index, parentIndex);
                index = parentIndex;
            }
            else
            {
                break;
            }
        }
    }

    /// 下沉 — 维护堆性质
    void SiftDown(SizeType index)
    {
        SizeType heapSize = m_Heap.GetSize();
        while (true)
        {
            SizeType smallest = index;
            SizeType left = 2 * index + 1;
            SizeType right = 2 * index + 2;

            if (left < heapSize &&
                m_Predicate(m_Heap[left], m_Heap[smallest]))
            {
                smallest = left;
            }

            if (right < heapSize &&
                m_Predicate(m_Heap[right], m_Heap[smallest]))
            {
                smallest = right;
            }

            if (smallest == index) break;

            SwapElements(index, smallest);
            index = smallest;
        }
    }

    /// 交换两个元素
    void SwapElements(SizeType indexA, SizeType indexB)
    {
        T temp = MoveTemp(m_Heap[indexA]);
        m_Heap[indexA] = MoveTemp(m_Heap[indexB]);
        m_Heap[indexB] = MoveTemp(temp);
    }

    TArray<T> m_Heap;       ///< 堆存储
    Pred      m_Predicate;  ///< 比较器
};

} // namespace Limx
