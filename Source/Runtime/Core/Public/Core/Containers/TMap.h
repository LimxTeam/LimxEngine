/*******************************************************************************
 * 文件: TMap.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   哈希表容器 — 替代 std::unordered_map 的零 STL 依赖实现
 *   开放寻址 + Robin Hood 哈希策略，缓存友好，低碰撞
 *   支持键值对存储、O(1) 平均查找/插入/删除
 *
 * 设计哲学:
 *   数据导向 — 开放寻址法将所有数据放在连续内存中，缓存友好
 *   Robin Hood — 探测时将"穷"元素（探测距离长）优先安置，均匀化碰撞
 *   POD 优化 — 对 POD 类型键值使用 memcpy/memset 代替逐元素操作
 *   分离哈希 — 键的哈希值独立存储，快速跳过空/删除槽
 *
 * 技术特性:
 *   - 开放寻址 + Robin Hood 哈希
 *   - 负载因子阈值: 0.75 (自动扩容)
 *   - 容量始终为 2 的幂 (位运算取模)
 *   - 支持: Add, Find, Remove, Contains, operator[]
 *   - 范围 for: 迭代所有有效键值对
 *
 * 依赖关系:
 *   内部: Core/CoreTypes.h (完整类型系统)
 *
 * 注意事项:
 *   TMap 不是线程安全的
 *   键类型必须支持 GetTypeHash() 或特化 THash<K>
 *   键类型必须支持 operator==
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

// ============================================================================
// 哈希函数
// ============================================================================

/// 哈希特征 — 为自定义类型特化此模板
template<typename T>
struct THash;

/// 整数哈希 — 基于位混合 (splitmix64 变体)
template<>
struct THash<Int32>
{
    LIMX_NODISCARD FORCEINLINE SizeType operator()(Int32 key) const
    {
        UInt64 hash = static_cast<UInt64>(static_cast<UInt32>(key));
        hash = (hash ^ (hash >> 16)) * 0x45D9F3B;
        hash = (hash ^ (hash >> 16)) * 0x45D9F3B;
        hash = hash ^ (hash >> 16);
        return static_cast<SizeType>(hash);
    }
};

template<>
struct THash<UInt32>
{
    LIMX_NODISCARD FORCEINLINE SizeType operator()(UInt32 key) const
    {
        UInt64 hash = static_cast<UInt64>(key);
        hash = (hash ^ (hash >> 16)) * 0x45D9F3B;
        hash = (hash ^ (hash >> 16)) * 0x45D9F3B;
        hash = hash ^ (hash >> 16);
        return static_cast<SizeType>(hash);
    }
};

template<>
struct THash<Int64>
{
    LIMX_NODISCARD FORCEINLINE SizeType operator()(Int64 key) const
    {
        UInt64 hash = static_cast<UInt64>(key);
        hash = (hash ^ (hash >> 30)) * 0xBF58476D1CE4E5B9ULL;
        hash = (hash ^ (hash >> 27)) * 0x94D049BB133111EBULL;
        hash = hash ^ (hash >> 31);
        return static_cast<SizeType>(hash);
    }
};

template<>
struct THash<UInt64>
{
    LIMX_NODISCARD FORCEINLINE SizeType operator()(UInt64 key) const
    {
        UInt64 hash = key;
        hash = (hash ^ (hash >> 30)) * 0xBF58476D1CE4E5B9ULL;
        hash = (hash ^ (hash >> 27)) * 0x94D049BB133111EBULL;
        hash = hash ^ (hash >> 31);
        return static_cast<SizeType>(hash);
    }
};

/// 指针哈希
template<typename T>
struct THash<T*>
{
    LIMX_NODISCARD FORCEINLINE SizeType operator()(T* key) const
    {
        return THash<UInt64>()(reinterpret_cast<UInt64>(key));
    }
};

/// FString 哈希 — FNV-1a 64-bit
/// 注: FString 在包含顺序中位于 TMap 之前，此处特化安全
template<>
struct THash<FString>
{
    LIMX_NODISCARD FORCEINLINE SizeType operator()(const FString& str) const
    {
        const AnsiChar* data = str.GetCStr();
        SizeType length = str.GetLength();

        constexpr UInt64 kFnvOffset = 14695981039346656037ULL;
        constexpr UInt64 kFnvPrime  = 1099511628211ULL;

        UInt64 hash = kFnvOffset;
        for (SizeType i = 0; i < length; ++i)
        {
            hash ^= static_cast<UInt64>(static_cast<UInt8>(data[i]));
            hash *= kFnvPrime;
        }

        return static_cast<SizeType>(hash);
    }
};

// ============================================================================
// 键值对
// ============================================================================

/// 键值对存储
template<typename K, typename V>
struct TKeyValuePair
{
    K Key;
    V Value;

    TKeyValuePair() = default;

    TKeyValuePair(const K& inKey, const V& inValue)
        : Key(inKey), Value(inValue)
    {
    }

    TKeyValuePair(const K& inKey, V&& inValue)
        : Key(inKey), Value(MoveTemp(inValue))
    {
    }

    TKeyValuePair(K&& inKey, V&& inValue)
        : Key(MoveTemp(inKey)), Value(MoveTemp(inValue))
    {
    }
};

// ============================================================================
// TMap
// ============================================================================

/// 哈希表 — 开放寻址 + Robin Hood 探测
/// @tparam K     键类型 (需要 THash<K> 和 operator==)
/// @tparam V     值类型
/// @tparam Hash  哈希函数 (默认 THash<K>)
template<typename K, typename V, typename Hash = THash<K>>
class TMap
{
public:
    using KeyType     = K;
    using ValueType   = V;
    using PairType    = TKeyValuePair<K, V>;
    using HashType    = Hash;

    // 槽状态标记
    static constexpr UInt8 kSlotEmpty    = 0;
    static constexpr UInt8 kSlotOccupied = 1;
    static constexpr UInt8 kSlotDeleted  = 2;

    // ========================================================================
    // 构造与析构
    // ========================================================================

    TMap()
        : m_Pairs(nullptr)
        , m_States(nullptr)
        , m_Size(0)
        , m_Capacity(0)
        , m_Allocator(&GetDefaultAllocator())
    {
    }

    explicit TMap(IAllocator& allocator)
        : m_Pairs(nullptr)
        , m_States(nullptr)
        , m_Size(0)
        , m_Capacity(0)
        , m_Allocator(&allocator)
    {
    }

    explicit TMap(SizeType initialCapacity,
                  IAllocator& allocator = GetDefaultAllocator())
        : m_Pairs(nullptr)
        , m_States(nullptr)
        , m_Size(0)
        , m_Capacity(0)
        , m_Allocator(&allocator)
    {
        Reserve(initialCapacity);
    }

    TMap(const TMap& other)
        : m_Pairs(nullptr)
        , m_States(nullptr)
        , m_Size(0)
        , m_Capacity(0)
        , m_Allocator(other.m_Allocator)
    {
        if (other.m_Size > 0)
        {
            AllocateTable(other.m_Capacity);
            for (SizeType index = 0; index < other.m_Capacity; ++index)
            {
                if (other.m_States[index] == kSlotOccupied)
                {
                    new (m_Pairs + index) PairType(other.m_Pairs[index]);
                    m_States[index] = kSlotOccupied;
                }
            }
            m_Size = other.m_Size;
        }
    }

    TMap(TMap&& other) noexcept
        : m_Pairs(other.m_Pairs)
        , m_States(other.m_States)
        , m_Size(other.m_Size)
        , m_Capacity(other.m_Capacity)
        , m_Allocator(other.m_Allocator)
    {
        other.m_Pairs = nullptr;
        other.m_States = nullptr;
        other.m_Size = 0;
        other.m_Capacity = 0;
    }

    ~TMap()
    {
        Clear();
        DeallocateTable();
    }

    // ========================================================================
    // 赋值
    // ========================================================================

    TMap& operator=(const TMap& other)
    {
        if (this != &other)
        {
            Clear();
            DeallocateTable();
            m_Allocator = other.m_Allocator;
            if (other.m_Size > 0)
            {
                AllocateTable(other.m_Capacity);
                for (SizeType index = 0; index < other.m_Capacity; ++index)
                {
                    if (other.m_States[index] == kSlotOccupied)
                    {
                        new (m_Pairs + index) PairType(other.m_Pairs[index]);
                        m_States[index] = kSlotOccupied;
                    }
                }
                m_Size = other.m_Size;
            }
        }
        return *this;
    }

    TMap& operator=(TMap&& other) noexcept
    {
        if (this != &other)
        {
            Clear();
            DeallocateTable();
            m_Pairs = other.m_Pairs;
            m_States = other.m_States;
            m_Size = other.m_Size;
            m_Capacity = other.m_Capacity;
            m_Allocator = other.m_Allocator;
            other.m_Pairs = nullptr;
            other.m_States = nullptr;
            other.m_Size = 0;
            other.m_Capacity = 0;
        }
        return *this;
    }

    // ========================================================================
    // 大小与容量
    // ========================================================================

    LIMX_NODISCARD FORCEINLINE SizeType GetSize() const { return m_Size; }
    LIMX_NODISCARD FORCEINLINE SizeType GetCapacity() const { return m_Capacity; }
    LIMX_NODISCARD FORCEINLINE bool IsEmpty() const { return m_Size == 0; }

    // ========================================================================
    // 插入
    // ========================================================================

    /// 添加或更新键值对 — 如果键已存在则更新值
    /// @return 值的引用
    V& Add(const K& key, const V& value)
    {
        EnsureCapacity(m_Size + 1);
        return InsertInternal(key, value);
    }

    V& Add(const K& key, V&& value)
    {
        EnsureCapacity(m_Size + 1);
        return InsertInternalMove(key, MoveTemp(value));
    }

    /// operator[] — 查找或默认构造
    V& operator[](const K& key)
    {
        V* existing = Find(key);
        if (existing)
        {
            return *existing;
        }
        EnsureCapacity(m_Size + 1);
        return InsertInternalMove(key, V());
    }

    // ========================================================================
    // 查找
    // ========================================================================

    /// 查找键对应的值 — 找到返回指针，未找到返回 nullptr
    LIMX_NODISCARD V* Find(const K& key)
    {
        if (m_Capacity == 0)
        {
            return nullptr;
        }

        SizeType hash = m_Hasher(key);
        SizeType index = hash & (m_Capacity - 1);
        SizeType probeDistance = 0;

        while (true)
        {
            UInt8 state = m_States[index];
            if (state == kSlotEmpty)
            {
                return nullptr;
            }

            if (state == kSlotOccupied)
            {
                SizeType existingDistance = ProbeDistance(
                    m_Hasher(m_Pairs[index].Key), index);
                if (probeDistance > existingDistance)
                {
                    // Robin Hood: 如果当前探测距离超过该槽元素的探测距离
                    // 则目标键不存在
                    return nullptr;
                }
                if (m_Pairs[index].Key == key)
                {
                    return &m_Pairs[index].Value;
                }
            }

            ++probeDistance;
            index = (index + 1) & (m_Capacity - 1);
        }
    }

    LIMX_NODISCARD const V* Find(const K& key) const
    {
        return const_cast<TMap*>(this)->Find(key);
    }

    /// 是否包含指定键
    LIMX_NODISCARD bool Contains(const K& key) const
    {
        return Find(key) != nullptr;
    }

    // ========================================================================
    // 删除
    // ========================================================================

    /// 移除指定键 — 返回是否成功移除
    bool Remove(const K& key)
    {
        if (m_Capacity == 0)
        {
            return false;
        }

        SizeType hash = m_Hasher(key);
        SizeType index = hash & (m_Capacity - 1);
        SizeType probeDistance = 0;

        while (true)
        {
            UInt8 state = m_States[index];
            if (state == kSlotEmpty)
            {
                return false;
            }

            if (state == kSlotOccupied)
            {
                SizeType existingDistance = ProbeDistance(
                    m_Hasher(m_Pairs[index].Key), index);
                if (probeDistance > existingDistance)
                {
                    return false;
                }
                if (m_Pairs[index].Key == key)
                {
                    // 找到 — 析构并标记为已删除
                    m_Pairs[index].~PairType();
                    m_States[index] = kSlotDeleted;
                    --m_Size;

                    // 回移后续元素填补空洞 (Robin Hood 删除优化)
                    BackshiftDelete(index);
                    return true;
                }
            }

            ++probeDistance;
            index = (index + 1) & (m_Capacity - 1);
        }
    }

    /// 清空所有键值对
    void Clear()
    {
        if (m_Size > 0 && m_Pairs && m_States)
        {
            for (SizeType index = 0; index < m_Capacity; ++index)
            {
                if (m_States[index] == kSlotOccupied)
                {
                    m_Pairs[index].~PairType();
                }
                m_States[index] = kSlotEmpty;
            }
            m_Size = 0;
        }
    }

    /// 重置 — 清空并释放内存
    void Reset()
    {
        Clear();
        DeallocateTable();
    }

    // ========================================================================
    // 容量管理
    // ========================================================================

    /// 预分配至少容纳 expectedSize 个元素的空间
    void Reserve(SizeType expectedSize)
    {
        // 按 0.75 负载因子计算需要的容量
        SizeType requiredCapacity = (expectedSize * 4 + 2) / 3;
        SizeType newCapacity = NextPowerOfTwo(requiredCapacity);
        if (newCapacity > m_Capacity)
        {
            Rehash(newCapacity);
        }
    }

    // ========================================================================
    // 迭代器
    // ========================================================================

    /// 简易前向迭代器 — 跳过空/删除槽
    class Iterator
    {
    public:
        Iterator(TMap* map, SizeType index)
            : m_Map(map), m_Index(index)
        {
            AdvanceToValid();
        }

        PairType& operator*() const { return m_Map->m_Pairs[m_Index]; }
        PairType* operator->() const { return &m_Map->m_Pairs[m_Index]; }

        Iterator& operator++()
        {
            ++m_Index;
            AdvanceToValid();
            return *this;
        }

        LIMX_NODISCARD bool operator!=(const Iterator& other) const
        {
            return m_Index != other.m_Index;
        }

        LIMX_NODISCARD bool operator==(const Iterator& other) const
        {
            return m_Index == other.m_Index;
        }

    private:
        void AdvanceToValid()
        {
            while (m_Index < m_Map->m_Capacity &&
                   m_Map->m_States[m_Index] != kSlotOccupied)
            {
                ++m_Index;
            }
        }

        TMap*    m_Map;
        SizeType m_Index;
    };

    class ConstIterator
    {
    public:
        ConstIterator(const TMap* map, SizeType index)
            : m_Map(map), m_Index(index)
        {
            AdvanceToValid();
        }

        const PairType& operator*() const { return m_Map->m_Pairs[m_Index]; }
        const PairType* operator->() const { return &m_Map->m_Pairs[m_Index]; }

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

    private:
        void AdvanceToValid()
        {
            while (m_Index < m_Map->m_Capacity &&
                   m_Map->m_States[m_Index] != kSlotOccupied)
            {
                ++m_Index;
            }
        }

        const TMap* m_Map;
        SizeType    m_Index;
    };

    LIMX_NODISCARD Iterator begin() { return Iterator(this, 0); }
    LIMX_NODISCARD Iterator end() { return Iterator(this, m_Capacity); }
    LIMX_NODISCARD ConstIterator begin() const { return ConstIterator(this, 0); }
    LIMX_NODISCARD ConstIterator end() const { return ConstIterator(this, m_Capacity); }

private:
    // ========================================================================
    // 内部辅助
    // ========================================================================

    /// 计算探测距离: 当前索引与理想索引之间的距离
    FORCEINLINE SizeType ProbeDistance(SizeType hash, SizeType currentIndex) const
    {
        return (currentIndex - (hash & (m_Capacity - 1))) & (m_Capacity - 1);
    }

    /// 内部插入 (拷贝)
    V& InsertInternal(const K& key, const V& value)
    {
        SizeType hash = m_Hasher(key);
        SizeType index = hash & (m_Capacity - 1);
        SizeType probeDistance = 0;

        K swapKey = key;
        V swapValue = value;
        V* resultPtr = nullptr;

        while (true)
        {
            if (m_States[index] == kSlotEmpty || m_States[index] == kSlotDeleted)
            {
                // 空槽 — 放入
                new (m_Pairs + index) PairType(MoveTemp(swapKey), MoveTemp(swapValue));
                m_States[index] = kSlotOccupied;
                ++m_Size;
                return resultPtr ? *resultPtr : m_Pairs[index].Value;
            }

            if (m_States[index] == kSlotOccupied)
            {
                // 键已存在 — 更新值
                if (m_Pairs[index].Key == swapKey)
                {
                    m_Pairs[index].Value = MoveTemp(swapValue);
                    return m_Pairs[index].Value;
                }

                // Robin Hood: 如果当前元素比已有元素"更穷" (探测距离更远)
                // 则交换，让"富"元素继续探测
                SizeType existingDistance = ProbeDistance(
                    m_Hasher(m_Pairs[index].Key), index);
                if (probeDistance > existingDistance)
                {
                    // 记录第一次插入位置作为返回值
                    if (!resultPtr)
                    {
                        // 先交换，再记录
                    }

                    // 交换
                    K tempKey = MoveTemp(m_Pairs[index].Key);
                    V tempValue = MoveTemp(m_Pairs[index].Value);
                    m_Pairs[index].~PairType();
                    new (m_Pairs + index) PairType(MoveTemp(swapKey), MoveTemp(swapValue));

                    if (!resultPtr)
                    {
                        resultPtr = &m_Pairs[index].Value;
                    }

                    swapKey = MoveTemp(tempKey);
                    swapValue = MoveTemp(tempValue);
                    probeDistance = existingDistance;
                }
            }

            ++probeDistance;
            index = (index + 1) & (m_Capacity - 1);
        }
    }

    /// 内部插入 (移动)
    V& InsertInternalMove(const K& key, V&& value)
    {
        SizeType hash = m_Hasher(key);
        SizeType index = hash & (m_Capacity - 1);
        SizeType probeDistance = 0;

        K swapKey = key;
        V swapValue = MoveTemp(value);
        V* resultPtr = nullptr;

        while (true)
        {
            if (m_States[index] == kSlotEmpty || m_States[index] == kSlotDeleted)
            {
                new (m_Pairs + index) PairType(MoveTemp(swapKey), MoveTemp(swapValue));
                m_States[index] = kSlotOccupied;
                ++m_Size;
                return resultPtr ? *resultPtr : m_Pairs[index].Value;
            }

            if (m_States[index] == kSlotOccupied)
            {
                if (m_Pairs[index].Key == swapKey)
                {
                    m_Pairs[index].Value = MoveTemp(swapValue);
                    return m_Pairs[index].Value;
                }

                SizeType existingDistance = ProbeDistance(
                    m_Hasher(m_Pairs[index].Key), index);
                if (probeDistance > existingDistance)
                {
                    K tempKey = MoveTemp(m_Pairs[index].Key);
                    V tempValue = MoveTemp(m_Pairs[index].Value);
                    m_Pairs[index].~PairType();
                    new (m_Pairs + index) PairType(MoveTemp(swapKey), MoveTemp(swapValue));

                    if (!resultPtr)
                    {
                        resultPtr = &m_Pairs[index].Value;
                    }

                    swapKey = MoveTemp(tempKey);
                    swapValue = MoveTemp(tempValue);
                    probeDistance = existingDistance;
                }
            }

            ++probeDistance;
            index = (index + 1) & (m_Capacity - 1);
        }
    }

    /// Robin Hood 删除后回移 — 填补空洞，保持探测连续性
    void BackshiftDelete(SizeType emptySlot)
    {
        SizeType index = (emptySlot + 1) & (m_Capacity - 1);

        while (true)
        {
            if (m_States[index] != kSlotOccupied)
            {
                break;
            }

            SizeType idealIndex = m_Hasher(m_Pairs[index].Key) & (m_Capacity - 1);
            if (idealIndex == index)
            {
                // 元素在理想位置，无需移动
                break;
            }

            // 判断该元素是否需要回移: 其理想位置是否"经过"空洞
            SizeType distToEmpty = (emptySlot - idealIndex) & (m_Capacity - 1);
            SizeType distToCurrent = (index - idealIndex) & (m_Capacity - 1);
            if (distToEmpty < distToCurrent)
            {
                // 回移到空洞位置
                new (m_Pairs + emptySlot) PairType(MoveTemp(m_Pairs[index]));
                m_States[emptySlot] = kSlotOccupied;
                m_Pairs[index].~PairType();
                m_States[index] = kSlotEmpty;
                emptySlot = index;
            }

            index = (index + 1) & (m_Capacity - 1);
        }

        m_States[emptySlot] = kSlotEmpty;
    }

    /// 重哈希 — 扩容并重新插入所有元素
    void Rehash(SizeType newCapacity)
    {
        PairType* oldPairs = m_Pairs;
        UInt8* oldStates = m_States;
        SizeType oldCapacity = m_Capacity;

        AllocateTable(newCapacity);
        m_Size = 0;

        if (oldPairs && oldStates)
        {
            for (SizeType index = 0; index < oldCapacity; ++index)
            {
                if (oldStates[index] == kSlotOccupied)
                {
                    InsertInternalMove(oldPairs[index].Key,
                                       MoveTemp(oldPairs[index].Value));
                    oldPairs[index].~PairType();
                }
            }
            m_Allocator->Deallocate(oldPairs);
            m_Allocator->Deallocate(oldStates);
        }
    }

    /// 确保容量足够 — 负载因子 > 0.75 时扩容
    void EnsureCapacity(SizeType requiredSize)
    {
        if (m_Capacity == 0)
        {
            Rehash(16);
            return;
        }

        // 负载因子检查: size * 4 > capacity * 3 等价于 load > 0.75
        if (requiredSize * 4 > m_Capacity * 3)
        {
            Rehash(m_Capacity * 2);
        }
    }

    /// 分配哈希表 (Pairs + States)
    void AllocateTable(SizeType capacity)
    {
        SizeType pairAlignment = alignof(PairType) > kDefaultAlignment
            ? alignof(PairType) : kDefaultAlignment;

        m_Pairs = static_cast<PairType*>(
            m_Allocator->Allocate(capacity * sizeof(PairType), pairAlignment));
        m_States = static_cast<UInt8*>(
            m_Allocator->Allocate(capacity * sizeof(UInt8), kDefaultAlignment));
        Memory::MemZero(m_States, capacity * sizeof(UInt8));
        m_Capacity = capacity;
    }

    /// 释放哈希表内存
    void DeallocateTable()
    {
        if (m_Pairs)
        {
            m_Allocator->Deallocate(m_Pairs);
            m_Pairs = nullptr;
        }
        if (m_States)
        {
            m_Allocator->Deallocate(m_States);
            m_States = nullptr;
        }
        m_Capacity = 0;
    }

    /// 向上取整到 2 的幂
    static SizeType NextPowerOfTwo(SizeType value)
    {
        if (value == 0)
        {
            return 1;
        }
        --value;
        value |= value >> 1;
        value |= value >> 2;
        value |= value >> 4;
        value |= value >> 8;
        value |= value >> 16;
        if constexpr (sizeof(SizeType) > 4)
        {
            value |= value >> 32;
        }
        return value + 1;
    }

    // ========================================================================
    // 成员数据
    // ========================================================================

    PairType*   m_Pairs;       ///< 键值对数组
    UInt8*      m_States;      ///< 槽状态数组 (Empty/Occupied/Deleted)
    SizeType    m_Size;        ///< 当前元素数量
    SizeType    m_Capacity;    ///< 表容量 (始终为 2 的幂)
    Hash        m_Hasher;      ///< 哈希函数实例
    IAllocator* m_Allocator;   ///< 内存分配器
};

} // namespace Limx
