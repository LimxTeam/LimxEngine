/*******************************************************************************
 * 文件: TLruCache.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   LRU 缓存 — 最近最少使用淘汰策略
 *   固定容量的键值缓存，满时自动淘汰最久未访问的条目
 *   用于纹理缓存、着色器缓存、资产缓存等有限资源管理场景
 *
 * 设计哲学:
 *   O(1) 访问 — 哈希表索引 + 双向链表维护访问顺序
 *   固定容量 — 构造时确定最大容量，无自动扩容
 *   值语义 — 缓存拥有值的所有权
 *
 * 技术特性:
 *   - TLruCache<KeyType, ValueType>: 参数化 LRU 缓存
 *   - Put: 插入或更新 (满时淘汰最旧)
 *   - Get: 查找并提升为最近使用
 *   - Contains: 是否存在
 *   - Remove: 手动移除
 *   - Clear: 清空所有
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Containers/TMap.h,
 *          Core/Memory/MemoryOps.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Containers/TMap.h"
#include "Core/Memory/MemoryOps.h"
#include "Core/Memory/IAllocator.h"
#include "Core/Memory/DefaultAllocator.h"

namespace Limx
{

/// LRU 缓存
/// @tparam KeyType   键类型 (需要支持哈希和相等比较)
/// @tparam ValueType 值类型
template<typename KeyType, typename ValueType>
class TLruCache
{
    /// 双向链表节点
    struct FNode
    {
        KeyType   Key;
        ValueType Value;
        FNode*    Prev;
        FNode*    Next;
    };

public:
    /// 构造
    /// @param capacity 最大容量
    explicit TLruCache(SizeType capacity)
        : m_Capacity(capacity)
        , m_Size(0)
        , m_Head(nullptr)
        , m_Tail(nullptr)
    {
        LIMX_ASSERT(capacity > 0);
    }

    ~TLruCache()
    {
        Clear();
    }

    // 不可拷贝
    TLruCache(const TLruCache&) = delete;
    TLruCache& operator=(const TLruCache&) = delete;

    // 可移动
    TLruCache(TLruCache&& other) noexcept
        : m_Capacity(other.m_Capacity)
        , m_Size(other.m_Size)
        , m_Head(other.m_Head)
        , m_Tail(other.m_Tail)
        , m_Map(MoveTemp(other.m_Map))
    {
        other.m_Size = 0;
        other.m_Head = nullptr;
        other.m_Tail = nullptr;
    }

    // ========================================================================
    // 操作
    // ========================================================================

    /// 插入或更新键值对
    /// 如果键已存在，更新值并提升为最近使用
    /// 如果缓存已满，淘汰最久未使用的条目
    void Put(const KeyType& key, const ValueType& value)
    {
        ValueType** existingPtr = m_Map.Find(key);
        if (existingPtr != nullptr)
        {
            // 已存在 — 通过值指针反推节点
            // 值存储在 FNode 内，通过偏移量获取节点指针
            FNode* node = GetNodeFromValuePtr(*existingPtr);
            node->Value = value;
            MoveToHead(node);
            return;
        }

        // 容量已满 — 淘汰最旧
        if (m_Size >= m_Capacity)
        {
            EvictTail();
        }

        // 创建新节点
        FNode* newNode = static_cast<FNode*>(
            GetDefaultAllocator().Allocate(
                sizeof(FNode), alignof(FNode)));
        new (&newNode->Key) KeyType(key);
        new (&newNode->Value) ValueType(value);
        newNode->Prev = nullptr;
        newNode->Next = nullptr;

        // 插入链表头部
        InsertAtHead(newNode);

        // 插入哈希表
        m_Map.Add(key, &newNode->Value);
        ++m_Size;
    }

    /// 查找值 — 命中时提升为最近使用
    /// @return 值指针，未命中返回 nullptr
    LIMX_NODISCARD ValueType* Get(const KeyType& key)
    {
        ValueType** valuePtr = m_Map.Find(key);
        if (valuePtr == nullptr) return nullptr;

        FNode* node = GetNodeFromValuePtr(*valuePtr);
        MoveToHead(node);
        return &node->Value;
    }

    /// 只读查找 — 不改变访问顺序
    LIMX_NODISCARD const ValueType* Peek(const KeyType& key) const
    {
        const ValueType* const* valuePtr = m_Map.Find(key);
        if (valuePtr == nullptr) return nullptr;
        return *valuePtr;
    }

    /// 是否包含
    LIMX_NODISCARD bool Contains(const KeyType& key) const
    {
        return m_Map.Contains(key);
    }

    /// 手动移除
    bool Remove(const KeyType& key)
    {
        ValueType** valuePtr = m_Map.Find(key);
        if (valuePtr == nullptr) return false;

        FNode* node = GetNodeFromValuePtr(*valuePtr);
        RemoveNode(node);
        m_Map.Remove(key);
        DestroyNode(node);
        --m_Size;
        return true;
    }

    /// 清空所有
    void Clear()
    {
        FNode* current = m_Head;
        while (current != nullptr)
        {
            FNode* next = current->Next;
            DestroyNode(current);
            current = next;
        }
        m_Head = nullptr;
        m_Tail = nullptr;
        m_Size = 0;
        m_Map.Clear();
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 当前条目数
    LIMX_NODISCARD SizeType GetSize() const { return m_Size; }

    /// 最大容量
    LIMX_NODISCARD SizeType GetCapacity() const { return m_Capacity; }

    /// 是否为空
    LIMX_NODISCARD bool IsEmpty() const { return m_Size == 0; }

    /// 是否已满
    LIMX_NODISCARD bool IsFull() const { return m_Size >= m_Capacity; }

private:
    // ========================================================================
    // 链表操作
    // ========================================================================

    /// 从值指针反推节点指针
    static FNode* GetNodeFromValuePtr(ValueType* valuePtr)
    {
        // 通过空指针偏移量计算 Value 在 FNode 中的偏移
        static const SizeType kValueOffset = reinterpret_cast<SizeType>(
            &reinterpret_cast<const volatile UInt8&>(
                static_cast<FNode*>(nullptr)->Value));
        UInt8* bytePtr = reinterpret_cast<UInt8*>(valuePtr);
        return reinterpret_cast<FNode*>(bytePtr - kValueOffset);
    }

    /// 插入节点到链表头部
    void InsertAtHead(FNode* node)
    {
        node->Prev = nullptr;
        node->Next = m_Head;

        if (m_Head != nullptr)
        {
            m_Head->Prev = node;
        }
        m_Head = node;

        if (m_Tail == nullptr)
        {
            m_Tail = node;
        }
    }

    /// 从链表中移除节点 (不释放)
    void RemoveNode(FNode* node)
    {
        if (node->Prev != nullptr)
        {
            node->Prev->Next = node->Next;
        }
        else
        {
            m_Head = node->Next;
        }

        if (node->Next != nullptr)
        {
            node->Next->Prev = node->Prev;
        }
        else
        {
            m_Tail = node->Prev;
        }

        node->Prev = nullptr;
        node->Next = nullptr;
    }

    /// 移动节点到头部 (最近使用)
    void MoveToHead(FNode* node)
    {
        if (node == m_Head) return;
        RemoveNode(node);
        InsertAtHead(node);
    }

    /// 淘汰尾部节点 (最久未使用)
    void EvictTail()
    {
        if (m_Tail == nullptr) return;

        FNode* victim = m_Tail;
        RemoveNode(victim);
        m_Map.Remove(victim->Key);
        DestroyNode(victim);
        --m_Size;
    }

    /// 销毁节点
    void DestroyNode(FNode* node)
    {
        node->Key.~KeyType();
        node->Value.~ValueType();
        GetDefaultAllocator().Deallocate(node);
    }

    SizeType                m_Capacity;  ///< 最大容量
    SizeType                m_Size;      ///< 当前条目数
    FNode*                  m_Head;      ///< 链表头 (最近使用)
    FNode*                  m_Tail;      ///< 链表尾 (最久未使用)
    TMap<KeyType, ValueType*> m_Map;     ///< 哈希索引
};

} // namespace Limx
