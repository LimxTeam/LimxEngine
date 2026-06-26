/*******************************************************************************
 * 文件: TIntrusiveList.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   侵入式双向链表 — 链表节点嵌入到元素自身中
 *   无额外内存分配，O(1) 插入/删除/移动
 *   用于定时器队列、LRU 缓存、任务链、对象注册表等场景
 *
 * 设计哲学:
 *   零分配 — 链表节点是元素的成员，无额外堆分配
 *   O(1) 操作 — 插入/删除仅修改指针，无搜索开销
 *   非拥有 — 链表不管理元素内存，元素自行管理生命周期
 *
 * 技术特性:
 *   - TIntrusiveListNode: 嵌入到元素中的链表节点
 *   - TIntrusiveList: 管理节点的双向链表
 *   - PushFront/PushBack: O(1) 头部/尾部插入
 *   - Remove: O(1) 从链表中移除节点
 *   - 支持迭代器遍历
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"

namespace Limx
{

/// 侵入式链表节点 — 嵌入到元素类中
class TIntrusiveListNode
{
    template<typename T, TIntrusiveListNode T::*>
    friend class TIntrusiveList;

public:
    TIntrusiveListNode() : m_Next(nullptr), m_Prev(nullptr) {}

    /// 是否已链入某个链表
    LIMX_NODISCARD bool IsLinked() const
    {
        return m_Next != nullptr || m_Prev != nullptr;
    }

private:
    TIntrusiveListNode* m_Next;
    TIntrusiveListNode* m_Prev;
};

/// 侵入式双向链表
/// @tparam T         元素类型
/// @tparam NodePtr   T 中 TIntrusiveListNode 成员的指针
template<typename T, TIntrusiveListNode T::* NodePtr>
class TIntrusiveList
{
    /// 从节点指针获取元素指针
    static T* NodeToElement(TIntrusiveListNode* node)
    {
        if (!node) return nullptr;
        // 反向计算偏移
        // offsetof 等效: (char*)&(((T*)0)->*NodePtr) - (char*)0
        UInt8* nodeAddr = reinterpret_cast<UInt8*>(node);
        T* dummyNull = nullptr;
        UInt8* memberAddr = reinterpret_cast<UInt8*>(
            &(dummyNull->*NodePtr));
        SizeType offset = static_cast<SizeType>(
            memberAddr - reinterpret_cast<UInt8*>(dummyNull));
        return reinterpret_cast<T*>(nodeAddr - offset);
    }

    static TIntrusiveListNode* ElementToNode(T* element)
    {
        return &(element->*NodePtr);
    }

public:
    // ========================================================================
    // 构造
    // ========================================================================

    TIntrusiveList() : m_Size(0)
    {
        // 哨兵节点: head.next → tail, head.prev = nullptr
        //           tail.next = nullptr, tail.prev → head
        m_Head.m_Next = &m_Tail;
        m_Head.m_Prev = nullptr;
        m_Tail.m_Next = nullptr;
        m_Tail.m_Prev = &m_Head;
    }

    ~TIntrusiveList() = default;

    // 不可拷贝
    TIntrusiveList(const TIntrusiveList&) = delete;
    TIntrusiveList& operator=(const TIntrusiveList&) = delete;

    // ========================================================================
    // 插入
    // ========================================================================

    /// 插入到头部
    void PushFront(T* element)
    {
        LIMX_ASSERT(element != nullptr);
        TIntrusiveListNode* node = ElementToNode(element);
        LIMX_ASSERT(!node->IsLinked());

        InsertAfter(&m_Head, node);
        ++m_Size;
    }

    /// 插入到尾部
    void PushBack(T* element)
    {
        LIMX_ASSERT(element != nullptr);
        TIntrusiveListNode* node = ElementToNode(element);
        LIMX_ASSERT(!node->IsLinked());

        InsertBefore(&m_Tail, node);
        ++m_Size;
    }

    /// 在指定元素之前插入
    void InsertBefore(T* target, T* element)
    {
        LIMX_ASSERT(target != nullptr && element != nullptr);
        TIntrusiveListNode* targetNode = ElementToNode(target);
        TIntrusiveListNode* elementNode = ElementToNode(element);
        LIMX_ASSERT(!elementNode->IsLinked());

        InsertBefore(targetNode, elementNode);
        ++m_Size;
    }

    // ========================================================================
    // 删除
    // ========================================================================

    /// 从链表中移除元素
    void Remove(T* element)
    {
        LIMX_ASSERT(element != nullptr);
        TIntrusiveListNode* node = ElementToNode(element);
        LIMX_ASSERT(node->IsLinked());

        Unlink(node);
        --m_Size;
    }

    /// 弹出头部元素
    LIMX_NODISCARD T* PopFront()
    {
        if (IsEmpty()) return nullptr;
        TIntrusiveListNode* node = m_Head.m_Next;
        Unlink(node);
        --m_Size;
        return NodeToElement(node);
    }

    /// 弹出尾部元素
    LIMX_NODISCARD T* PopBack()
    {
        if (IsEmpty()) return nullptr;
        TIntrusiveListNode* node = m_Tail.m_Prev;
        Unlink(node);
        --m_Size;
        return NodeToElement(node);
    }

    // ========================================================================
    // 访问
    // ========================================================================

    /// 头部元素
    LIMX_NODISCARD T* GetFront()
    {
        if (IsEmpty()) return nullptr;
        return NodeToElement(m_Head.m_Next);
    }

    LIMX_NODISCARD const T* GetFront() const
    {
        if (IsEmpty()) return nullptr;
        return NodeToElement(
            const_cast<TIntrusiveListNode*>(m_Head.m_Next));
    }

    /// 尾部元素
    LIMX_NODISCARD T* GetBack()
    {
        if (IsEmpty()) return nullptr;
        return NodeToElement(m_Tail.m_Prev);
    }

    LIMX_NODISCARD const T* GetBack() const
    {
        if (IsEmpty()) return nullptr;
        return NodeToElement(
            const_cast<TIntrusiveListNode*>(m_Tail.m_Prev));
    }

    // ========================================================================
    // 状态
    // ========================================================================

    LIMX_NODISCARD SizeType GetSize() const { return m_Size; }
    LIMX_NODISCARD bool IsEmpty() const { return m_Size == 0; }

    // ========================================================================
    // 迭代器
    // ========================================================================

    class Iterator
    {
    public:
        Iterator(TIntrusiveListNode* node,
                 TIntrusiveListNode* sentinel)
            : m_Current(node), m_Sentinel(sentinel) {}

        T& operator*() { return *NodeToElement(m_Current); }
        T* operator->() { return NodeToElement(m_Current); }

        Iterator& operator++()
        {
            m_Current = m_Current->m_Next;
            return *this;
        }

        bool operator!=(const Iterator& other) const
        {
            return m_Current != other.m_Current;
        }

        bool operator==(const Iterator& other) const
        {
            return m_Current == other.m_Current;
        }

    private:
        TIntrusiveListNode* m_Current;
        TIntrusiveListNode* m_Sentinel;
    };

    Iterator begin() { return Iterator(m_Head.m_Next, &m_Tail); }
    Iterator end() { return Iterator(&m_Tail, &m_Tail); }

    // ========================================================================
    // 遍历辅助
    // ========================================================================

    template<typename Func>
    void ForEach(Func&& func)
    {
        TIntrusiveListNode* current = m_Head.m_Next;
        while (current != &m_Tail)
        {
            TIntrusiveListNode* next = current->m_Next;
            func(*NodeToElement(current));
            current = next;
        }
    }

private:
    /// 在 target 之前插入 node
    static void InsertBefore(TIntrusiveListNode* target,
                              TIntrusiveListNode* node)
    {
        node->m_Prev = target->m_Prev;
        node->m_Next = target;
        target->m_Prev->m_Next = node;
        target->m_Prev = node;
    }

    /// 在 target 之后插入 node
    static void InsertAfter(TIntrusiveListNode* target,
                             TIntrusiveListNode* node)
    {
        node->m_Next = target->m_Next;
        node->m_Prev = target;
        target->m_Next->m_Prev = node;
        target->m_Next = node;
    }

    /// 从链表中解除链接
    static void Unlink(TIntrusiveListNode* node)
    {
        node->m_Prev->m_Next = node->m_Next;
        node->m_Next->m_Prev = node->m_Prev;
        node->m_Next = nullptr;
        node->m_Prev = nullptr;
    }

    TIntrusiveListNode m_Head;  ///< 头部哨兵
    TIntrusiveListNode m_Tail;  ///< 尾部哨兵
    SizeType           m_Size;  ///< 元素数量
};

} // namespace Limx
