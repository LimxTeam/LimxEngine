/*******************************************************************************
 * 文件: TSparseArray.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   稀疏数组 — 支持稳定索引的动态数组，空洞可复用
 *   元素通过索引 (ID) 访问，删除元素后索引位置标记为空洞
 *   新元素优先填入空洞位置，避免频繁重分配
 *   用于 ECS 组件存储、Handle 系统、对象池等需要稳定 ID 的场景
 *
 * 设计哲学:
 *   稳定索引 — 元素删除后索引不失效 (其他元素不移动)
 *   空洞复用 — 维护空闲索引链表，O(1) 分配新槽位
 *   代际验证 — 每个槽位携带代 (Generation)，防止悬挂引用
 *
 * 技术特性:
 *   - Add: O(1) 添加 (复用空洞或追加)，返回索引
 *   - RemoveAt: O(1) 删除 (标记空洞，加入空闲链表)
 *   - IsValidIndex: 检查索引是否有效
 *   - operator[]: 按索引访问 (断言保护)
 *   - GetSize: 实际有效元素数
 *   - GetCapacity: 数组总槽位数 (含空洞)
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h,
 *          Core/Memory/MemoryOps.h, Core/Memory/DefaultAllocator.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Memory/MemoryOps.h"
#include "Core/Memory/DefaultAllocator.h"

namespace Limx
{

/// 稀疏数组 — 支持稳定索引和空洞复用
/// @tparam T 元素类型
template<typename T>
class TSparseArray
{
    /// 槽位状态
    struct Slot
    {
        alignas(T) UInt8 Storage[sizeof(T)];  ///< 元素存储
        UInt32 Generation;                     ///< 代标记 (奇数=有效, 偶数=空洞)
        Int32  NextFree;                       ///< 空洞链表: 下一个空闲索引 (-1=链尾)

        /// 是否有效 (代为奇数)
        LIMX_NODISCARD bool IsValid() const
        {
            return (Generation & 1u) != 0;
        }

        /// 获取元素指针
        LIMX_NODISCARD T* GetElement()
        {
            return reinterpret_cast<T*>(Storage);
        }

        LIMX_NODISCARD const T* GetElement() const
        {
            return reinterpret_cast<const T*>(Storage);
        }
    };

    static constexpr Int32 kInvalidIndex = -1;

public:
    // ========================================================================
    // 构造与析构
    // ========================================================================

    TSparseArray()
        : m_Slots(nullptr)
        , m_Capacity(0)
        , m_Count(0)
        , m_FreeHead(kInvalidIndex)
        , m_Allocator(&GetDefaultAllocator())
    {
    }

    ~TSparseArray()
    {
        Clear();
        if (m_Slots)
        {
            m_Allocator->Deallocate(m_Slots);
        }
    }

    // 不可拷贝
    TSparseArray(const TSparseArray&) = delete;
    TSparseArray& operator=(const TSparseArray&) = delete;

    // 可移动
    TSparseArray(TSparseArray&& other) noexcept
        : m_Slots(other.m_Slots)
        , m_Capacity(other.m_Capacity)
        , m_Count(other.m_Count)
        , m_FreeHead(other.m_FreeHead)
        , m_Allocator(other.m_Allocator)
    {
        other.m_Slots = nullptr;
        other.m_Capacity = 0;
        other.m_Count = 0;
        other.m_FreeHead = kInvalidIndex;
    }

    // ========================================================================
    // 添加
    // ========================================================================

    /// 添加元素 (拷贝) — 返回分配的索引
    LIMX_NODISCARD SizeType Add(const T& value)
    {
        SizeType index = AllocateSlot();
        new (m_Slots[index].Storage) T(value);
        return index;
    }

    /// 添加元素 (移动) — 返回分配的索引
    LIMX_NODISCARD SizeType Add(T&& value)
    {
        SizeType index = AllocateSlot();
        new (m_Slots[index].Storage) T(MoveTemp(value));
        return index;
    }

    // ========================================================================
    // 删除
    // ========================================================================

    /// 删除指定索引的元素
    void RemoveAt(SizeType index)
    {
        LIMX_ASSERT(index < m_Capacity);
        LIMX_ASSERT(m_Slots[index].IsValid());

        // 析构元素
        m_Slots[index].GetElement()->~T();

        // 递增代 (奇数→偶数 = 无效)
        m_Slots[index].Generation++;

        // 加入空闲链表头部
        m_Slots[index].NextFree = m_FreeHead;
        m_FreeHead = static_cast<Int32>(index);

        m_Count--;
    }

    /// 清空所有元素
    void Clear()
    {
        for (SizeType index = 0; index < m_Capacity; ++index)
        {
            if (m_Slots[index].IsValid())
            {
                m_Slots[index].GetElement()->~T();
                m_Slots[index].Generation++;
            }
        }
        m_Count = 0;
        m_FreeHead = kInvalidIndex;

        // 重建空闲链表
        for (SizeType index = 0; index < m_Capacity; ++index)
        {
            m_Slots[index].NextFree = m_FreeHead;
            m_FreeHead = static_cast<Int32>(index);
        }
    }

    // ========================================================================
    // 访问
    // ========================================================================

    /// 按索引访问 (断言保护)
    LIMX_NODISCARD T& operator[](SizeType index)
    {
        LIMX_ASSERT(index < m_Capacity);
        LIMX_ASSERT(m_Slots[index].IsValid());
        return *m_Slots[index].GetElement();
    }

    LIMX_NODISCARD const T& operator[](SizeType index) const
    {
        LIMX_ASSERT(index < m_Capacity);
        LIMX_ASSERT(m_Slots[index].IsValid());
        return *m_Slots[index].GetElement();
    }

    /// 检查索引是否指向有效元素
    LIMX_NODISCARD bool IsValidIndex(SizeType index) const
    {
        return index < m_Capacity && m_Slots[index].IsValid();
    }

    /// 获取索引处的代标记
    LIMX_NODISCARD UInt32 GetGeneration(SizeType index) const
    {
        LIMX_ASSERT(index < m_Capacity);
        return m_Slots[index].Generation;
    }

    // ========================================================================
    // 状态查询
    // ========================================================================

    /// 有效元素数量
    LIMX_NODISCARD SizeType GetCount() const { return m_Count; }

    /// 总槽位数 (含空洞)
    LIMX_NODISCARD SizeType GetCapacity() const { return m_Capacity; }

    /// 是否为空
    LIMX_NODISCARD bool IsEmpty() const { return m_Count == 0; }

    // ========================================================================
    // 遍历辅助
    // ========================================================================

    /// 对所有有效元素执行回调
    template<typename Func>
    void ForEach(Func&& func)
    {
        for (SizeType index = 0; index < m_Capacity; ++index)
        {
            if (m_Slots[index].IsValid())
            {
                func(index, *m_Slots[index].GetElement());
            }
        }
    }

    template<typename Func>
    void ForEach(Func&& func) const
    {
        for (SizeType index = 0; index < m_Capacity; ++index)
        {
            if (m_Slots[index].IsValid())
            {
                func(index, *m_Slots[index].GetElement());
            }
        }
    }

private:
    // ========================================================================
    // 内部管理
    // ========================================================================

    /// 分配一个槽位 — 返回索引
    SizeType AllocateSlot()
    {
        SizeType index;

        if (m_FreeHead != kInvalidIndex)
        {
            // 从空闲链表取出
            index = static_cast<SizeType>(m_FreeHead);
            m_FreeHead = m_Slots[index].NextFree;
        }
        else
        {
            // 追加新槽位
            if (m_Capacity == 0)
            {
                Grow(8);
            }
            else
            {
                Grow(m_Capacity * 2);
            }
            index = static_cast<SizeType>(m_FreeHead);
            m_FreeHead = m_Slots[index].NextFree;
        }

        // 递增代 (偶数→奇数 = 有效)
        m_Slots[index].Generation++;
        m_Slots[index].NextFree = kInvalidIndex;
        m_Count++;

        return index;
    }

    /// 扩容到指定容量
    void Grow(SizeType newCapacity)
    {
        LIMX_ASSERT(newCapacity > m_Capacity);

        Slot* newSlots = static_cast<Slot*>(
            m_Allocator->Allocate(
                newCapacity * sizeof(Slot), alignof(Slot)));

        // 拷贝旧数据
        if (m_Slots && m_Capacity > 0)
        {
            Memory::MemCopy(newSlots, m_Slots,
                            m_Capacity * sizeof(Slot));
        }

        // 初始化新槽位并加入空闲链表
        for (SizeType index = newCapacity; index > m_Capacity; --index)
        {
            SizeType slotIndex = index - 1;
            newSlots[slotIndex].Generation = 0;  // 偶数 = 无效
            newSlots[slotIndex].NextFree = m_FreeHead;
            m_FreeHead = static_cast<Int32>(slotIndex);
        }

        if (m_Slots)
        {
            m_Allocator->Deallocate(m_Slots);
        }
        m_Slots = newSlots;
        m_Capacity = newCapacity;
    }

    // ========================================================================
    // 成员数据
    // ========================================================================

    Slot*       m_Slots;       ///< 槽位数组
    SizeType    m_Capacity;    ///< 总槽位数
    SizeType    m_Count;       ///< 有效元素数
    Int32       m_FreeHead;    ///< 空闲链表头索引
    IAllocator* m_Allocator;   ///< 内存分配器
};

} // namespace Limx
