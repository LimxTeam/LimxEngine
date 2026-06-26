/*******************************************************************************
 * 文件: TStringMap.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   字符串键映射 — 以 FString 为键的哈希表特化
 *   内部使用 FNV-1a 对字符串内容哈希，开放寻址线性探测
 *   用于配置项查找、属性注册、资源路径索引等字符串键场景
 *
 * 设计哲学:
 *   字符串哈希 — 对字符串内容而非指针进行 FNV-1a 哈希
 *   开放寻址 — 线性探测，缓存友好
 *   自动扩容 — 负载因子 75% 时 Rehash
 *
 * 技术特性:
 *   - TStringMap<ValueType>: 字符串键哈希表
 *   - Insert/Find/Remove/Contains: 基础操作
 *   - 键为 FString，值为任意类型
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Containers/FString.h,
 *          Core/Memory/DefaultAllocator.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Containers/FString.h"
#include "Core/Memory/MemoryOps.h"
#include "Core/Memory/IAllocator.h"
#include "Core/Memory/DefaultAllocator.h"

namespace Limx
{

/// 字符串键映射
/// @tparam ValueType 值类型
template<typename ValueType>
class TStringMap
{
    enum class ESlotState : UInt8
    {
        Empty     = 0,
        Occupied  = 1,
        Tombstone = 2
    };

    struct FSlot
    {
        ESlotState State;
        FString    Key;
        ValueType  Value;
    };

    static constexpr SizeType kMinCapacity = 16;
    static constexpr SizeType kMaxLoadPercent = 75;

public:
    TStringMap()
        : m_Slots(nullptr)
        , m_Capacity(0)
        , m_Count(0)
        , m_TombstoneCount(0)
    {
    }

    explicit TStringMap(SizeType initialCapacity)
        : m_Slots(nullptr)
        , m_Capacity(0)
        , m_Count(0)
        , m_TombstoneCount(0)
    {
        if (initialCapacity > 0)
        {
            AllocateSlots(NextPowerOfTwo(initialCapacity));
        }
    }

    ~TStringMap()
    {
        DestroyAllSlots();
        DeallocateSlots();
    }

    // 不可拷贝
    TStringMap(const TStringMap&) = delete;
    TStringMap& operator=(const TStringMap&) = delete;

    // 可移动
    TStringMap(TStringMap&& other) noexcept
        : m_Slots(other.m_Slots)
        , m_Capacity(other.m_Capacity)
        , m_Count(other.m_Count)
        , m_TombstoneCount(other.m_TombstoneCount)
    {
        other.m_Slots = nullptr;
        other.m_Capacity = 0;
        other.m_Count = 0;
        other.m_TombstoneCount = 0;
    }

    TStringMap& operator=(TStringMap&& other) noexcept
    {
        if (this != &other)
        {
            DestroyAllSlots();
            DeallocateSlots();
            m_Slots = other.m_Slots;
            m_Capacity = other.m_Capacity;
            m_Count = other.m_Count;
            m_TombstoneCount = other.m_TombstoneCount;
            other.m_Slots = nullptr;
            other.m_Capacity = 0;
            other.m_Count = 0;
            other.m_TombstoneCount = 0;
        }
        return *this;
    }

    // ========================================================================
    // 插入
    // ========================================================================

    /// 插入键值对 (已存在则覆盖)
    void Insert(const FString& key, const ValueType& value)
    {
        EnsureCapacity();
        SizeType idx = FindSlotForInsert(key);
        FSlot& slot = m_Slots[idx];

        if (slot.State == ESlotState::Occupied)
        {
            slot.Value = value;
        }
        else
        {
            if (slot.State == ESlotState::Tombstone)
                --m_TombstoneCount;
            slot.State = ESlotState::Occupied;
            new (&slot.Key) FString(key);
            new (&slot.Value) ValueType(value);
            ++m_Count;
        }
    }

    /// 插入 (移动值)
    void Insert(const FString& key, ValueType&& value)
    {
        EnsureCapacity();
        SizeType idx = FindSlotForInsert(key);
        FSlot& slot = m_Slots[idx];

        if (slot.State == ESlotState::Occupied)
        {
            slot.Value = MoveTemp(value);
        }
        else
        {
            if (slot.State == ESlotState::Tombstone)
                --m_TombstoneCount;
            slot.State = ESlotState::Occupied;
            new (&slot.Key) FString(key);
            new (&slot.Value) ValueType(MoveTemp(value));
            ++m_Count;
        }
    }

    /// 插入 (C 字符串键)
    void Insert(const AnsiChar* key, const ValueType& value)
    {
        Insert(FString(key), value);
    }

    // ========================================================================
    // 查找
    // ========================================================================

    LIMX_NODISCARD ValueType* Find(const FString& key)
    {
        if (m_Count == 0) return nullptr;
        SizeType idx = FindSlotForLookup(key);
        if (idx == kInvalidSlot) return nullptr;
        return &m_Slots[idx].Value;
    }

    LIMX_NODISCARD const ValueType* Find(
        const FString& key) const
    {
        if (m_Count == 0) return nullptr;
        SizeType idx = FindSlotForLookup(key);
        if (idx == kInvalidSlot) return nullptr;
        return &m_Slots[idx].Value;
    }

    LIMX_NODISCARD ValueType* Find(const AnsiChar* key)
    {
        return Find(FString(key));
    }

    LIMX_NODISCARD const ValueType* Find(
        const AnsiChar* key) const
    {
        return Find(FString(key));
    }

    LIMX_NODISCARD bool Contains(const FString& key) const
    {
        if (m_Count == 0) return false;
        return FindSlotForLookup(key) != kInvalidSlot;
    }

    LIMX_NODISCARD bool Contains(const AnsiChar* key) const
    {
        return Contains(FString(key));
    }

    // ========================================================================
    // 删除
    // ========================================================================

    bool Remove(const FString& key)
    {
        if (m_Count == 0) return false;
        SizeType idx = FindSlotForLookup(key);
        if (idx == kInvalidSlot) return false;

        FSlot& slot = m_Slots[idx];
        slot.Key.~FString();
        slot.Value.~ValueType();
        slot.State = ESlotState::Tombstone;
        --m_Count;
        ++m_TombstoneCount;
        return true;
    }

    void Clear()
    {
        DestroyAllSlots();
        m_Count = 0;
        m_TombstoneCount = 0;
    }

    // ========================================================================
    // 查询
    // ========================================================================

    LIMX_NODISCARD SizeType GetCount() const
    {
        return m_Count;
    }

    LIMX_NODISCARD SizeType GetCapacity() const
    {
        return m_Capacity;
    }

    LIMX_NODISCARD bool IsEmpty() const
    {
        return m_Count == 0;
    }

private:
    static constexpr SizeType kInvalidSlot =
        static_cast<SizeType>(-1);

    /// FNV-1a 字符串哈希
    LIMX_NODISCARD static UInt32 HashString(
        const FString& str)
    {
        const AnsiChar* data = str.GetCStr();
        SizeType length = str.GetLength();
        UInt32 hash = 2166136261u;
        for (SizeType charIdx = 0;
             charIdx < length; ++charIdx)
        {
            hash ^= static_cast<UInt32>(
                static_cast<UInt8>(data[charIdx]));
            hash *= 16777619u;
        }
        return hash;
    }

    SizeType FindSlotForInsert(const FString& key)
    {
        SizeType mask = m_Capacity - 1;
        SizeType start = HashString(key) & mask;
        SizeType firstTombstone = kInvalidSlot;

        for (SizeType probe = 0;
             probe < m_Capacity; ++probe)
        {
            SizeType idx = (start + probe) & mask;
            FSlot& slot = m_Slots[idx];

            if (slot.State == ESlotState::Empty)
            {
                return (firstTombstone != kInvalidSlot)
                    ? firstTombstone : idx;
            }
            if (slot.State == ESlotState::Tombstone)
            {
                if (firstTombstone == kInvalidSlot)
                    firstTombstone = idx;
                continue;
            }
            if (slot.Key == key)
                return idx;
        }
        return firstTombstone;
    }

    SizeType FindSlotForLookup(const FString& key) const
    {
        SizeType mask = m_Capacity - 1;
        SizeType start = HashString(key) & mask;

        for (SizeType probe = 0;
             probe < m_Capacity; ++probe)
        {
            SizeType idx = (start + probe) & mask;
            const FSlot& slot = m_Slots[idx];

            if (slot.State == ESlotState::Empty)
                return kInvalidSlot;
            if (slot.State == ESlotState::Occupied &&
                slot.Key == key)
                return idx;
        }
        return kInvalidSlot;
    }

    void EnsureCapacity()
    {
        if (m_Capacity == 0)
        {
            AllocateSlots(kMinCapacity);
            return;
        }
        SizeType total = m_Count + m_TombstoneCount;
        if (total * 100 >= m_Capacity * kMaxLoadPercent)
        {
            Rehash(m_Capacity * 2);
        }
    }

    void Rehash(SizeType newCapacity)
    {
        FSlot* oldSlots = m_Slots;
        SizeType oldCapacity = m_Capacity;

        AllocateSlots(newCapacity);
        m_Count = 0;
        m_TombstoneCount = 0;

        for (SizeType oldIdx = 0;
             oldIdx < oldCapacity; ++oldIdx)
        {
            if (oldSlots[oldIdx].State == ESlotState::Occupied)
            {
                Insert(oldSlots[oldIdx].Key,
                       MoveTemp(oldSlots[oldIdx].Value));
                oldSlots[oldIdx].Key.~FString();
                oldSlots[oldIdx].Value.~ValueType();
            }
        }

        if (oldSlots != nullptr)
        {
            GetDefaultAllocator().Deallocate(oldSlots);
        }
    }

    void AllocateSlots(SizeType capacity)
    {
        m_Capacity = capacity;
        m_Slots = static_cast<FSlot*>(
            GetDefaultAllocator().Allocate(
                capacity * sizeof(FSlot), alignof(FSlot)));
        for (SizeType idx = 0; idx < capacity; ++idx)
        {
            m_Slots[idx].State = ESlotState::Empty;
        }
    }

    void DeallocateSlots()
    {
        if (m_Slots != nullptr)
        {
            GetDefaultAllocator().Deallocate(m_Slots);
            m_Slots = nullptr;
            m_Capacity = 0;
        }
    }

    void DestroyAllSlots()
    {
        if (m_Slots == nullptr) return;
        for (SizeType idx = 0; idx < m_Capacity; ++idx)
        {
            if (m_Slots[idx].State == ESlotState::Occupied)
            {
                m_Slots[idx].Key.~FString();
                m_Slots[idx].Value.~ValueType();
                m_Slots[idx].State = ESlotState::Empty;
            }
            else if (m_Slots[idx].State ==
                     ESlotState::Tombstone)
            {
                m_Slots[idx].State = ESlotState::Empty;
            }
        }
    }

    static SizeType NextPowerOfTwo(SizeType n)
    {
        if (n <= 1) return 1;
        --n;
        n |= n >> 1;  n |= n >> 2;
        n |= n >> 4;  n |= n >> 8;
        n |= n >> 16; n |= n >> 32;
        return n + 1;
    }

    FSlot*   m_Slots;
    SizeType m_Capacity;
    SizeType m_Count;
    SizeType m_TombstoneCount;
};

} // namespace Limx
