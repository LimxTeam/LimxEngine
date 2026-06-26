/*******************************************************************************
 * 文件: TDenseMap.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   紧凑哈希表 — 开放寻址 + 线性探测的扁平哈希表
 *   所有键值对存储在连续数组中，缓存友好
 *   用于高频查找的小到中等规模映射场景
 *
 * 设计哲学:
 *   扁平存储 — 键值对紧凑排列在单一数组中
 *   线性探测 — 冲突时依次向后查找空槽
 *   墓碑标记 — 删除使用 Tombstone 标记，不破坏探测链
 *   自动扩容 — 负载因子超过 75% 时 Rehash
 *
 * 技术特性:
 *   - TDenseMap<KeyType, ValueType>: 扁平哈希表
 *   - Insert/Remove/Find: 基础操作
 *   - Contains: 存在性查询
 *   - GetCount/GetCapacity: 统计
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Memory/MemoryOps.h,
 *          Core/Memory/DefaultAllocator.h
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

/// 紧凑哈希表
/// @tparam KeyType 键类型 (需要 GetTypeHash 支持)
/// @tparam ValueType 值类型
template<typename KeyType, typename ValueType>
class TDenseMap
{
    /// 槽位状态
    enum class ESlotState : UInt8
    {
        Empty     = 0, ///< 空槽
        Occupied  = 1, ///< 已占用
        Tombstone = 2  ///< 已删除
    };

    /// 槽位
    struct FSlot
    {
        ESlotState State;
        KeyType    Key;
        ValueType  Value;
    };

    static constexpr SizeType kMinCapacity = 16;
    static constexpr SizeType kMaxLoadPercent = 75;

public:
    TDenseMap()
        : m_Slots(nullptr)
        , m_Capacity(0)
        , m_Count(0)
        , m_TombstoneCount(0)
    {
    }

    explicit TDenseMap(SizeType initialCapacity)
        : m_Slots(nullptr)
        , m_Capacity(0)
        , m_Count(0)
        , m_TombstoneCount(0)
    {
        if (initialCapacity > 0)
        {
            SizeType cap = NextPowerOfTwo(initialCapacity);
            AllocateSlots(cap);
        }
    }

    ~TDenseMap()
    {
        DestroyAllSlots();
        DeallocateSlots();
    }

    // 不可拷贝
    TDenseMap(const TDenseMap&) = delete;
    TDenseMap& operator=(const TDenseMap&) = delete;

    // 可移动
    TDenseMap(TDenseMap&& other) noexcept
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

    TDenseMap& operator=(TDenseMap&& other) noexcept
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
    void Insert(const KeyType& key, const ValueType& value)
    {
        EnsureCapacity();

        SizeType slotIndex = FindSlotForInsert(key);
        FSlot& slot = m_Slots[slotIndex];

        if (slot.State == ESlotState::Occupied)
        {
            // 已存在 — 覆盖
            slot.Value = value;
        }
        else
        {
            if (slot.State == ESlotState::Tombstone)
            {
                --m_TombstoneCount;
            }
            slot.State = ESlotState::Occupied;
            new (&slot.Key) KeyType(key);
            new (&slot.Value) ValueType(value);
            ++m_Count;
        }
    }

    /// 插入键值对 (移动)
    void Insert(const KeyType& key, ValueType&& value)
    {
        EnsureCapacity();

        SizeType slotIndex = FindSlotForInsert(key);
        FSlot& slot = m_Slots[slotIndex];

        if (slot.State == ESlotState::Occupied)
        {
            slot.Value = MoveTemp(value);
        }
        else
        {
            if (slot.State == ESlotState::Tombstone)
            {
                --m_TombstoneCount;
            }
            slot.State = ESlotState::Occupied;
            new (&slot.Key) KeyType(key);
            new (&slot.Value) ValueType(MoveTemp(value));
            ++m_Count;
        }
    }

    // ========================================================================
    // 查找
    // ========================================================================

    /// 查找值 (可写)
    LIMX_NODISCARD ValueType* Find(const KeyType& key)
    {
        if (m_Count == 0) return nullptr;

        SizeType slotIndex = FindSlotForLookup(key);
        if (slotIndex == kInvalidSlot) return nullptr;
        return &m_Slots[slotIndex].Value;
    }

    /// 查找值 (只读)
    LIMX_NODISCARD const ValueType* Find(
        const KeyType& key) const
    {
        if (m_Count == 0) return nullptr;

        SizeType slotIndex = FindSlotForLookup(key);
        if (slotIndex == kInvalidSlot) return nullptr;
        return &m_Slots[slotIndex].Value;
    }

    /// 是否包含键
    LIMX_NODISCARD bool Contains(const KeyType& key) const
    {
        if (m_Count == 0) return false;
        return FindSlotForLookup(key) != kInvalidSlot;
    }

    // ========================================================================
    // 删除
    // ========================================================================

    /// 移除键
    bool Remove(const KeyType& key)
    {
        if (m_Count == 0) return false;

        SizeType slotIndex = FindSlotForLookup(key);
        if (slotIndex == kInvalidSlot) return false;

        FSlot& slot = m_Slots[slotIndex];
        slot.Key.~KeyType();
        slot.Value.~ValueType();
        slot.State = ESlotState::Tombstone;
        --m_Count;
        ++m_TombstoneCount;
        return true;
    }

    /// 清空
    void Clear()
    {
        DestroyAllSlots();
        m_Count = 0;
        m_TombstoneCount = 0;
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 元素数量
    LIMX_NODISCARD SizeType GetCount() const
    {
        return m_Count;
    }

    /// 容量
    LIMX_NODISCARD SizeType GetCapacity() const
    {
        return m_Capacity;
    }

    /// 是否为空
    LIMX_NODISCARD bool IsEmpty() const
    {
        return m_Count == 0;
    }

private:
    static constexpr SizeType kInvalidSlot =
        static_cast<SizeType>(-1);

    /// 哈希函数 — FNV-1a 对键取哈希
    LIMX_NODISCARD SizeType HashKey(const KeyType& key) const
    {
        // 使用字节级 FNV-1a
        const UInt8* bytes =
            reinterpret_cast<const UInt8*>(&key);
        UInt32 hash = 2166136261u;
        for (SizeType byteIndex = 0;
             byteIndex < sizeof(KeyType); ++byteIndex)
        {
            hash ^= static_cast<UInt32>(bytes[byteIndex]);
            hash *= 16777619u;
        }
        return static_cast<SizeType>(hash);
    }

    /// 查找插入位置
    SizeType FindSlotForInsert(const KeyType& key)
    {
        SizeType mask = m_Capacity - 1;
        SizeType startIndex = HashKey(key) & mask;
        SizeType firstTombstone = kInvalidSlot;

        for (SizeType probeOffset = 0;
             probeOffset < m_Capacity; ++probeOffset)
        {
            SizeType probeIndex =
                (startIndex + probeOffset) & mask;
            FSlot& slot = m_Slots[probeIndex];

            if (slot.State == ESlotState::Empty)
            {
                // 空槽 — 使用之前的墓碑或此空槽
                return (firstTombstone != kInvalidSlot)
                    ? firstTombstone : probeIndex;
            }

            if (slot.State == ESlotState::Tombstone)
            {
                if (firstTombstone == kInvalidSlot)
                {
                    firstTombstone = probeIndex;
                }
                continue;
            }

            // Occupied — 检查是否同键
            if (KeysEqual(slot.Key, key))
            {
                return probeIndex;
            }
        }

        // 不应到达此处 (EnsureCapacity 保证有空间)
        return firstTombstone;
    }

    /// 查找查询位置
    SizeType FindSlotForLookup(const KeyType& key) const
    {
        SizeType mask = m_Capacity - 1;
        SizeType startIndex = HashKey(key) & mask;

        for (SizeType probeOffset = 0;
             probeOffset < m_Capacity; ++probeOffset)
        {
            SizeType probeIndex =
                (startIndex + probeOffset) & mask;
            const FSlot& slot = m_Slots[probeIndex];

            if (slot.State == ESlotState::Empty)
            {
                return kInvalidSlot; // 探测链断裂
            }

            if (slot.State == ESlotState::Occupied &&
                KeysEqual(slot.Key, key))
            {
                return probeIndex;
            }
            // Tombstone — 继续探测
        }

        return kInvalidSlot;
    }

    /// 键比较
    static bool KeysEqual(const KeyType& a, const KeyType& b)
    {
        const UInt8* bytesA =
            reinterpret_cast<const UInt8*>(&a);
        const UInt8* bytesB =
            reinterpret_cast<const UInt8*>(&b);
        for (SizeType byteIndex = 0;
             byteIndex < sizeof(KeyType); ++byteIndex)
        {
            if (bytesA[byteIndex] != bytesB[byteIndex])
                return false;
        }
        return true;
    }

    /// 确保容量足够
    void EnsureCapacity()
    {
        if (m_Capacity == 0)
        {
            AllocateSlots(kMinCapacity);
            return;
        }

        // 负载因子检查 (含墓碑)
        SizeType totalOccupied =
            m_Count + m_TombstoneCount;
        if (totalOccupied * 100 >= m_Capacity * kMaxLoadPercent)
        {
            Rehash(m_Capacity * 2);
        }
    }

    /// 重新哈希
    void Rehash(SizeType newCapacity)
    {
        FSlot* oldSlots = m_Slots;
        SizeType oldCapacity = m_Capacity;

        AllocateSlots(newCapacity);
        m_Count = 0;
        m_TombstoneCount = 0;

        for (SizeType oldIndex = 0;
             oldIndex < oldCapacity; ++oldIndex)
        {
            if (oldSlots[oldIndex].State == ESlotState::Occupied)
            {
                Insert(oldSlots[oldIndex].Key,
                       MoveTemp(oldSlots[oldIndex].Value));
                oldSlots[oldIndex].Key.~KeyType();
                oldSlots[oldIndex].Value.~ValueType();
            }
        }

        if (oldSlots != nullptr)
        {
            GetDefaultAllocator().Deallocate(oldSlots);
        }
    }

    /// 分配槽位数组
    void AllocateSlots(SizeType capacity)
    {
        m_Capacity = capacity;
        m_Slots = static_cast<FSlot*>(
            GetDefaultAllocator().Allocate(
                capacity * sizeof(FSlot), alignof(FSlot)));

        for (SizeType slotIndex = 0;
             slotIndex < capacity; ++slotIndex)
        {
            m_Slots[slotIndex].State = ESlotState::Empty;
        }
    }

    /// 释放槽位数组
    void DeallocateSlots()
    {
        if (m_Slots != nullptr)
        {
            GetDefaultAllocator().Deallocate(m_Slots);
            m_Slots = nullptr;
            m_Capacity = 0;
        }
    }

    /// 销毁所有已占用槽位
    void DestroyAllSlots()
    {
        if (m_Slots == nullptr) return;

        for (SizeType slotIndex = 0;
             slotIndex < m_Capacity; ++slotIndex)
        {
            if (m_Slots[slotIndex].State == ESlotState::Occupied)
            {
                m_Slots[slotIndex].Key.~KeyType();
                m_Slots[slotIndex].Value.~ValueType();
                m_Slots[slotIndex].State = ESlotState::Empty;
            }
            else if (m_Slots[slotIndex].State ==
                     ESlotState::Tombstone)
            {
                m_Slots[slotIndex].State = ESlotState::Empty;
            }
        }
    }

    /// 取大于等于 n 的最小 2 的幂
    static SizeType NextPowerOfTwo(SizeType n)
    {
        if (n <= 1) return 1;
        --n;
        n |= n >> 1;
        n |= n >> 2;
        n |= n >> 4;
        n |= n >> 8;
        n |= n >> 16;
        n |= n >> 32;
        return n + 1;
    }

    FSlot*   m_Slots;           ///< 槽位数组
    SizeType m_Capacity;        ///< 容量 (2 的幂)
    SizeType m_Count;           ///< 已占用数
    SizeType m_TombstoneCount;  ///< 墓碑数
};

} // namespace Limx
