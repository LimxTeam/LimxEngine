/*******************************************************************************
 * 文件: TQueue.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   FIFO 队列容器 — 单链表实现的先进先出队列
 *   用于任务系统、消息队列、事件分发等异步场景
 *   支持 Enqueue (入队) 和 Dequeue (出队) 操作
 *
 * 设计哲学:
 *   单链表 — Enqueue/Dequeue 均为 O(1)，无需连续内存
 *   分配器感知 — 所有节点分配通过 IAllocator
 *   值语义 — 拷贝队列时深拷贝所有节点
 *   非线程安全 — 多线程场景需外部同步
 *
 * 技术特性:
 *   - Enqueue/Dequeue: O(1) 常数时间
 *   - Peek: 查看队首元素但不出队
 *   - IsEmpty/GetSize: 状态查询
 *   - Clear: 清空所有元素
 *   - 头/尾指针: m_Head (出队端), m_Tail (入队端)
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h,
 *          Core/TypeTraits/TypeTraits.h, Core/Memory/MemoryOps.h,
 *          Core/Memory/DefaultAllocator.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/TypeTraits/TypeTraits.h"
#include "Core/Memory/MemoryOps.h"
#include "Core/Memory/DefaultAllocator.h"

namespace Limx
{

/// FIFO 单链表队列
/// @tparam TElement 元素类型
template<typename TElement>
class TQueue
{
    struct Node
    {
        TElement Value;
        Node*    Next;

        template<typename... Args>
        explicit Node(Args&&... args)
            : Value(Forward<Args>(args)...)
            , Next(nullptr)
        {
        }
    };

public:
    // ========================================================================
    // 构造与析构
    // ========================================================================

    TQueue()
        : m_Head(nullptr)
        , m_Tail(nullptr)
        , m_Size(0)
        , m_Allocator(&GetDefaultAllocator())
    {
    }

    explicit TQueue(IAllocator& allocator)
        : m_Head(nullptr)
        , m_Tail(nullptr)
        , m_Size(0)
        , m_Allocator(&allocator)
    {
    }

    TQueue(const TQueue& other)
        : m_Head(nullptr)
        , m_Tail(nullptr)
        , m_Size(0)
        , m_Allocator(other.m_Allocator)
    {
        Node* current = other.m_Head;
        while (current)
        {
            Enqueue(current->Value);
            current = current->Next;
        }
    }

    TQueue(TQueue&& other) noexcept
        : m_Head(other.m_Head)
        , m_Tail(other.m_Tail)
        , m_Size(other.m_Size)
        , m_Allocator(other.m_Allocator)
    {
        other.m_Head = nullptr;
        other.m_Tail = nullptr;
        other.m_Size = 0;
    }

    ~TQueue()
    {
        Clear();
    }

    TQueue& operator=(const TQueue& other)
    {
        if (this != &other)
        {
            Clear();
            m_Allocator = other.m_Allocator;
            Node* current = other.m_Head;
            while (current)
            {
                Enqueue(current->Value);
                current = current->Next;
            }
        }
        return *this;
    }

    TQueue& operator=(TQueue&& other) noexcept
    {
        if (this != &other)
        {
            Clear();
            m_Head = other.m_Head;
            m_Tail = other.m_Tail;
            m_Size = other.m_Size;
            m_Allocator = other.m_Allocator;
            other.m_Head = nullptr;
            other.m_Tail = nullptr;
            other.m_Size = 0;
        }
        return *this;
    }

    // ========================================================================
    // 入队
    // ========================================================================

    /// 入队 — 将元素添加到队尾
    void Enqueue(const TElement& element)
    {
        Node* node = AllocateNode(element);
        if (m_Tail)
        {
            m_Tail->Next = node;
            m_Tail = node;
        }
        else
        {
            m_Head = node;
            m_Tail = node;
        }
        m_Size++;
    }

    void Enqueue(TElement&& element)
    {
        Node* node = AllocateNode(MoveTemp(element));
        if (m_Tail)
        {
            m_Tail->Next = node;
            m_Tail = node;
        }
        else
        {
            m_Head = node;
            m_Tail = node;
        }
        m_Size++;
    }

    /// 原地构造入队
    template<typename... Args>
    void EmplaceEnqueue(Args&&... args)
    {
        Node* node = AllocateNode(Forward<Args>(args)...);
        if (m_Tail)
        {
            m_Tail->Next = node;
            m_Tail = node;
        }
        else
        {
            m_Head = node;
            m_Tail = node;
        }
        m_Size++;
    }

    // ========================================================================
    // 出队
    // ========================================================================

    /// 出队 — 移除并返回队首元素
    /// 调用前必须确保队列非空
    TElement Dequeue()
    {
        LIMX_ASSERT(m_Head != nullptr);

        Node* node = m_Head;
        TElement result(MoveTemp(node->Value));

        m_Head = node->Next;
        if (m_Head == nullptr)
        {
            m_Tail = nullptr;
        }

        DeallocateNode(node);
        m_Size--;
        return result;
    }

    /// 安全出队 — 如果队列非空则出队到 outElement，返回是否成功
    bool Dequeue(TElement& outElement)
    {
        if (m_Head == nullptr)
        {
            return false;
        }

        Node* node = m_Head;
        outElement = MoveTemp(node->Value);

        m_Head = node->Next;
        if (m_Head == nullptr)
        {
            m_Tail = nullptr;
        }

        DeallocateNode(node);
        m_Size--;
        return true;
    }

    // ========================================================================
    // 查看
    // ========================================================================

    /// 查看队首元素 (不出队)
    LIMX_NODISCARD const TElement& Peek() const
    {
        LIMX_ASSERT(m_Head != nullptr);
        return m_Head->Value;
    }

    LIMX_NODISCARD TElement& Peek()
    {
        LIMX_ASSERT(m_Head != nullptr);
        return m_Head->Value;
    }

    /// 安全查看 — 返回指向队首元素的指针，空队列返回 nullptr
    LIMX_NODISCARD const TElement* TryPeek() const
    {
        return m_Head ? &m_Head->Value : nullptr;
    }

    // ========================================================================
    // 状态查询
    // ========================================================================

    LIMX_NODISCARD FORCEINLINE SizeType GetSize() const { return m_Size; }
    LIMX_NODISCARD FORCEINLINE bool IsEmpty() const { return m_Size == 0; }

    // ========================================================================
    // 清空
    // ========================================================================

    void Clear()
    {
        while (m_Head)
        {
            Node* next = m_Head->Next;
            DeallocateNode(m_Head);
            m_Head = next;
        }
        m_Tail = nullptr;
        m_Size = 0;
    }

private:
    // ========================================================================
    // 节点分配
    // ========================================================================

    template<typename... Args>
    Node* AllocateNode(Args&&... args)
    {
        void* memory = m_Allocator->Allocate(sizeof(Node), alignof(Node));
        return new (memory) Node(Forward<Args>(args)...);
    }

    void DeallocateNode(Node* node)
    {
        node->~Node();
        m_Allocator->Deallocate(node);
    }

    // ========================================================================
    // 成员数据
    // ========================================================================

    Node*       m_Head;       ///< 队首 (出队端)
    Node*       m_Tail;       ///< 队尾 (入队端)
    SizeType    m_Size;       ///< 元素数量
    IAllocator* m_Allocator;  ///< 内存分配器
};

} // namespace Limx
