/*******************************************************************************
 * 文件: TSet.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   哈希集合容器 — 替代 std::unordered_set 的零 STL 依赖实现
 *   基于开放寻址 + Robin Hood 哈希实现 O(1) 平均查找/插入/删除
 *   元素唯一，不允许重复
 *
 * 设计哲学:
 *   与 TMap 一致的哈希策略 — Robin Hood 开放寻址
 *   缓存友好 — 连续内存布局，避免链表散列
 *   模板化哈希 — 使用 THash<T> 特化体系
 *   分配器感知 — 所有动态内存通过 IAllocator
 *
 * 技术特性:
 *   - 开放寻址 + Robin Hood 哈希
 *   - 负载因子阈值: 0.75
 *   - 后移删除 (Backshift Deletion)
 *   - 支持: Add, Remove, Contains, Find, Clear, Reserve
 *   - 支持 range-based for 迭代
 *   - THash<T> 特化体系
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h,
 *          Core/TypeTraits/TypeTraits.h, Core/Memory/MemoryOps.h,
 *          Core/Memory/DefaultAllocator.h, Core/Containers/TMap.h (THash)
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/TypeTraits/TypeTraits.h"
#include "Core/Memory/MemoryOps.h"
#include "Core/Memory/DefaultAllocator.h"
#include "Core/Containers/TMap.h"

namespace Limx
{

/// 哈希集合 — 元素唯一，O(1) 平均操作
/// @tparam TElement  元素类型
/// @tparam THashFunc 哈希函数 (默认 THash<TElement>)
template<typename TElement, typename THashFunc = THash<TElement>>
class TSet
{
    // 槽位标记
    static constexpr UInt8 kSlotEmpty   = 0;
    static constexpr UInt8 kSlotUsed    = 1;

    // 负载因子阈值
    static constexpr Float32 kMaxLoadFactor = 0.75f;

    // 最小容量
    static constexpr SizeType kMinCapacity = 16;

    struct Slot
    {
        alignas(TElement) UInt8 ElementStorage[sizeof(TElement)];
        UInt8    State;
        UInt8    ProbeDistance;  // Robin Hood 探测距离

        TElement& GetElement()
        {
            return *reinterpret_cast<TElement*>(ElementStorage);
        }

        const TElement& GetElement() const
        {
            return *reinterpret_cast<const TElement*>(ElementStorage);
        }
    };

public:
    // ========================================================================
    // 构造与析构
    // ========================================================================

    TSet()
        : m_Slots(nullptr)
        , m_Capacity(0)
        , m_Size(0)
        , m_Allocator(&GetDefaultAllocator())
    {
    }

    explicit TSet(IAllocator& allocator)
        : m_Slots(nullptr)
        , m_Capacity(0)
        , m_Size(0)
        , m_Allocator(&allocator)
    {
    }

    TSet(const TSet& other)
        : m_Slots(nullptr)
        , m_Capacity(0)
        , m_Size(0)
        , m_Allocator(other.m_Allocator)
    {
        if (other.m_Size > 0)
        {
            Reserve(other.m_Capacity);
            for (SizeType index = 0; index < other.m_Capacity; ++index)
            {
                if (other.m_Slots[index].State == kSlotUsed)
                {
                    Add(other.m_Slots[index].GetElement());
                }
            }
        }
    }

    TSet(TSet&& other) noexcept
        : m_Slots(other.m_Slots)
        , m_Capacity(other.m_Capacity)
        , m_Size(other.m_Size)
        , m_Allocator(other.m_Allocator)
    {
        other.m_Slots = nullptr;
        other.m_Capacity = 0;
        other.m_Size = 0;
    }

    ~TSet()
    {
        Clear();
        DeallocateSlots();
    }

    TSet& operator=(const TSet& other)
    {
        if (this != &other)
        {
            Clear();
            DeallocateSlots();
            m_Allocator = other.m_Allocator;
            if (other.m_Size > 0)
            {
                Reserve(other.m_Capacity);
                for (SizeType index = 0; index < other.m_Capacity; ++index)
                {
                    if (other.m_Slots[index].State == kSlotUsed)
                    {
                        Add(other.m_Slots[index].GetElement());
                    }
                }
            }
        }
        return *this;
    }

    TSet& operator=(TSet&& other) noexcept
    {
        if (this != &other)
        {
            Clear();
            DeallocateSlots();
            m_Slots = other.m_Slots;
            m_Capacity = other.m_Capacity;
            m_Size = other.m_Size;
            m_Allocator = other.m_Allocator;
            other.m_Slots = nullptr;
            other.m_Capacity = 0;
            other.m_Size = 0;
        }
        return *this;
    }

    // ========================================================================
    // 容量
    // ========================================================================

    LIMX_NODISCARD FORCEINLINE SizeType GetSize() const { return m_Size; }
    LIMX_NODISCARD FORCEINLINE SizeType GetCapacity() const { return m_Capacity; }
    LIMX_NODISCARD FORCEINLINE bool IsEmpty() const { return m_Size == 0; }

    void Reserve(SizeType newCapacity)
    {
        if (newCapacity <= m_Capacity)
        {
            return;
        }

        // 向上取 2 的幂
        SizeType capacity = kMinCapacity;
        while (capacity < newCapacity)
        {
            capacity <<= 1;
        }

        Rehash(capacity);
    }

    // ========================================================================
    // 插入
    // ========================================================================

    /// 添加元素 — 如果已存在则不插入，返回是否成功插入
    bool Add(const TElement& element)
    {
        EnsureCapacity(m_Size + 1);
        return InsertInternal(element);
    }

    bool Add(TElement&& element)
    {
        EnsureCapacity(m_Size + 1);
        return InsertInternal(MoveTemp(element));
    }

    // ========================================================================
    // 查找
    // ========================================================================

    /// 是否包含指定元素
    LIMX_NODISCARD bool Contains(const TElement& element) const
    {
        if (m_Capacity == 0)
        {
            return false;
        }
        return FindSlotIndex(element) != kSizeTypeMax;
    }

    /// 查找元素 — 返回指针，未找到返回 nullptr
    LIMX_NODISCARD const TElement* Find(const TElement& element) const
    {
        if (m_Capacity == 0)
        {
            return nullptr;
        }
        SizeType index = FindSlotIndex(element);
        if (index != kSizeTypeMax)
        {
            return &m_Slots[index].GetElement();
        }
        return nullptr;
    }

    // ========================================================================
    // 删除
    // ========================================================================

    /// 删除元素 — 返回是否成功删除
    bool Remove(const TElement& element)
    {
        if (m_Capacity == 0)
        {
            return false;
        }

        SizeType index = FindSlotIndex(element);
        if (index == kSizeTypeMax)
        {
            return false;
        }

        // 析构元素
        m_Slots[index].GetElement().~TElement();
        m_Slots[index].State = kSlotEmpty;
        m_Slots[index].ProbeDistance = 0;
        m_Size--;

        // 后移修复 Robin Hood 不变式
        SizeType mask = m_Capacity - 1;
        SizeType current = (index + 1) & mask;
        while (m_Slots[current].State == kSlotUsed &&
               m_Slots[current].ProbeDistance > 0)
        {
            SizeType prev = (current - 1 + m_Capacity) & mask;
            // 将当前槽位移到前一个空槽位
            new (&m_Slots[prev].GetElement()) TElement(
                MoveTemp(m_Slots[current].GetElement()));
            m_Slots[prev].State = kSlotUsed;
            m_Slots[prev].ProbeDistance =
                m_Slots[current].ProbeDistance - 1;

            m_Slots[current].GetElement().~TElement();
            m_Slots[current].State = kSlotEmpty;
            m_Slots[current].ProbeDistance = 0;

            current = (current + 1) & mask;
        }

        return true;
    }

    /// 清空所有元素
    void Clear()
    {
        if (m_Slots)
        {
            for (SizeType index = 0; index < m_Capacity; ++index)
            {
                if (m_Slots[index].State == kSlotUsed)
                {
                    m_Slots[index].GetElement().~TElement();
                    m_Slots[index].State = kSlotEmpty;
                }
            }
        }
        m_Size = 0;
    }

    // ========================================================================
    // 迭代器
    // ========================================================================

    class ConstIterator
    {
    public:
        ConstIterator(const Slot* slots, SizeType capacity, SizeType index)
            : m_Slots(slots), m_Capacity(capacity), m_Index(index)
        {
            AdvanceToValid();
        }

        LIMX_NODISCARD const TElement& operator*() const
        {
            return m_Slots[m_Index].GetElement();
        }

        LIMX_NODISCARD const TElement* operator->() const
        {
            return &m_Slots[m_Index].GetElement();
        }

        ConstIterator& operator++()
        {
            ++m_Index;
            AdvanceToValid();
            return *this;
        }

        LIMX_NODISCARD bool operator!=(const ConstIterator& other) const
        {
            return m_Index != other.m_Index;
        }

        LIMX_NODISCARD bool operator==(const ConstIterator& other) const
        {
            return m_Index == other.m_Index;
        }

    private:
        void AdvanceToValid()
        {
            while (m_Index < m_Capacity &&
                   m_Slots[m_Index].State != kSlotUsed)
            {
                ++m_Index;
            }
        }

        const Slot* m_Slots;
        SizeType    m_Capacity;
        SizeType    m_Index;
    };

    LIMX_NODISCARD ConstIterator begin() const
    {
        return ConstIterator(m_Slots, m_Capacity, 0);
    }

    LIMX_NODISCARD ConstIterator end() const
    {
        return ConstIterator(m_Slots, m_Capacity, m_Capacity);
    }

private:
    // ========================================================================
    // 内部实现
    // ========================================================================

    void EnsureCapacity(SizeType requiredSize)
    {
        if (m_Capacity == 0)
        {
            Rehash(kMinCapacity);
            return;
        }

        SizeType threshold = static_cast<SizeType>(
            static_cast<Float32>(m_Capacity) * kMaxLoadFactor);
        if (requiredSize > threshold)
        {
            Rehash(m_Capacity * 2);
        }
    }

    template<typename ElementArg>
    bool InsertInternal(ElementArg&& element)
    {
        THashFunc hasher;
        SizeType hash = hasher(element);
        SizeType mask = m_Capacity - 1;
        SizeType index = hash & mask;
        UInt8 probeDistance = 0;

        // 构造临时元素用于 Robin Hood 交换
        alignas(TElement) UInt8 tempStorage[sizeof(TElement)];
        new (tempStorage) TElement(Forward<ElementArg>(element));
        TElement& tempElement = *reinterpret_cast<TElement*>(tempStorage);
        bool ownsTemp = true;

        while (true)
        {
            if (m_Slots[index].State == kSlotEmpty)
            {
                // 空槽位 — 放置元素
                new (&m_Slots[index].GetElement()) TElement(
                    MoveTemp(tempElement));
                m_Slots[index].State = kSlotUsed;
                m_Slots[index].ProbeDistance = probeDistance;
                tempElement.~TElement();
                m_Size++;
                return true;
            }

            // 检查重复
            if (m_Slots[index].GetElement() == tempElement)
            {
                // 已存在 — 不插入
                tempElement.~TElement();
                return false;
            }

            // Robin Hood: 如果当前元素的探测距离更短，交换
            if (m_Slots[index].ProbeDistance < probeDistance)
            {
                // 交换元素
                TElement swapped(MoveTemp(m_Slots[index].GetElement()));
                m_Slots[index].GetElement().~TElement();
                new (&m_Slots[index].GetElement()) TElement(
                    MoveTemp(tempElement));

                UInt8 swappedProbe = m_Slots[index].ProbeDistance;
                m_Slots[index].ProbeDistance = probeDistance;

                tempElement.~TElement();
                new (&tempElement) TElement(MoveTemp(swapped));
                probeDistance = swappedProbe;
            }

            index = (index + 1) & mask;
            probeDistance++;
        }
    }

    LIMX_NODISCARD SizeType FindSlotIndex(const TElement& element) const
    {
        THashFunc hasher;
        SizeType hash = hasher(element);
        SizeType mask = m_Capacity - 1;
        SizeType index = hash & mask;
        UInt8 probeDistance = 0;

        while (true)
        {
            if (m_Slots[index].State == kSlotEmpty)
            {
                return kSizeTypeMax;
            }

            if (m_Slots[index].ProbeDistance < probeDistance)
            {
                // Robin Hood 保证: 不可能在更远处
                return kSizeTypeMax;
            }

            if (m_Slots[index].GetElement() == element)
            {
                return index;
            }

            index = (index + 1) & mask;
            probeDistance++;
        }
    }

    void Rehash(SizeType newCapacity)
    {
        Slot* oldSlots = m_Slots;
        SizeType oldCapacity = m_Capacity;

        // 分配新槽位
        m_Slots = static_cast<Slot*>(
            m_Allocator->Allocate(
                sizeof(Slot) * newCapacity, alignof(Slot)));
        m_Capacity = newCapacity;
        m_Size = 0;

        // 初始化新槽位
        for (SizeType index = 0; index < newCapacity; ++index)
        {
            m_Slots[index].State = kSlotEmpty;
            m_Slots[index].ProbeDistance = 0;
        }

        // 重新插入旧元素
        if (oldSlots)
        {
            for (SizeType index = 0; index < oldCapacity; ++index)
            {
                if (oldSlots[index].State == kSlotUsed)
                {
                    InsertInternal(MoveTemp(oldSlots[index].GetElement()));
                    oldSlots[index].GetElement().~TElement();
                }
            }
            m_Allocator->Deallocate(oldSlots);
        }
    }

    void DeallocateSlots()
    {
        if (m_Slots)
        {
            m_Allocator->Deallocate(m_Slots);
            m_Slots = nullptr;
            m_Capacity = 0;
        }
    }

    // ========================================================================
    // 成员数据
    // ========================================================================

    Slot*       m_Slots;      ///< 槽位数组
    SizeType    m_Capacity;   ///< 槽位总数 (2 的幂)
    SizeType    m_Size;       ///< 已使用槽位数
    IAllocator* m_Allocator;  ///< 内存分配器
};

} // namespace Limx
