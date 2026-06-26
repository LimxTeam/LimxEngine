/*******************************************************************************
 * 文件: TDeque.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   双端队列 — 头部和尾部均可 O(1) 摊还插入/删除
 *   基于环形缓冲区实现，自动扩容
 *   用于滑动窗口、工作窃取队列、BFS 遍历、撤销/重做栈等场景
 *
 * 设计哲学:
 *   环形缓冲 — 连续数组 + 头尾指针，双端操作均摊 O(1)
 *   自动扩容 — 容量不足时倍增并重新排列
 *   随机访问 — 通过逻辑索引映射到物理位置
 *
 * 技术特性:
 *   - TDeque<T>: 双端队列
 *   - PushFront/PushBack: 头/尾插入
 *   - PopFront/PopBack: 头/尾删除
 *   - operator[]: 随机访问
 *   - GetFront/GetBack: 查看头/尾元素
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

/// 双端队列
/// @tparam T 元素类型
template<typename T>
class TDeque
{
    static constexpr SizeType kMinCapacity = 8;

public:
    TDeque()
        : m_Data(nullptr)
        , m_Capacity(0)
        , m_Head(0)
        , m_Count(0)
    {
    }

    explicit TDeque(SizeType initialCapacity)
        : m_Data(nullptr)
        , m_Capacity(0)
        , m_Head(0)
        , m_Count(0)
    {
        if (initialCapacity > 0)
        {
            SizeType cap = NextPowerOfTwo(initialCapacity);
            AllocateBuffer(cap);
        }
    }

    ~TDeque()
    {
        DestroyAll();
        DeallocateBuffer();
    }

    // 不可拷贝
    TDeque(const TDeque&) = delete;
    TDeque& operator=(const TDeque&) = delete;

    // 可移动
    TDeque(TDeque&& other) noexcept
        : m_Data(other.m_Data)
        , m_Capacity(other.m_Capacity)
        , m_Head(other.m_Head)
        , m_Count(other.m_Count)
    {
        other.m_Data = nullptr;
        other.m_Capacity = 0;
        other.m_Head = 0;
        other.m_Count = 0;
    }

    TDeque& operator=(TDeque&& other) noexcept
    {
        if (this != &other)
        {
            DestroyAll();
            DeallocateBuffer();
            m_Data = other.m_Data;
            m_Capacity = other.m_Capacity;
            m_Head = other.m_Head;
            m_Count = other.m_Count;
            other.m_Data = nullptr;
            other.m_Capacity = 0;
            other.m_Head = 0;
            other.m_Count = 0;
        }
        return *this;
    }

    // ========================================================================
    // 插入
    // ========================================================================

    /// 尾部插入 (拷贝)
    void PushBack(const T& element)
    {
        EnsureCapacity();
        SizeType tailIdx = PhysicalIndex(m_Count);
        new (&m_Data[tailIdx]) T(element);
        ++m_Count;
    }

    /// 尾部插入 (移动)
    void PushBack(T&& element)
    {
        EnsureCapacity();
        SizeType tailIdx = PhysicalIndex(m_Count);
        new (&m_Data[tailIdx]) T(MoveTemp(element));
        ++m_Count;
    }

    /// 头部插入 (拷贝)
    void PushFront(const T& element)
    {
        EnsureCapacity();
        m_Head = (m_Head == 0)
            ? m_Capacity - 1 : m_Head - 1;
        new (&m_Data[m_Head]) T(element);
        ++m_Count;
    }

    /// 头部插入 (移动)
    void PushFront(T&& element)
    {
        EnsureCapacity();
        m_Head = (m_Head == 0)
            ? m_Capacity - 1 : m_Head - 1;
        new (&m_Data[m_Head]) T(MoveTemp(element));
        ++m_Count;
    }

    // ========================================================================
    // 删除
    // ========================================================================

    /// 尾部删除
    void PopBack()
    {
        LIMX_ASSERT(m_Count > 0);
        --m_Count;
        SizeType tailIdx = PhysicalIndex(m_Count);
        m_Data[tailIdx].~T();
    }

    /// 头部删除
    void PopFront()
    {
        LIMX_ASSERT(m_Count > 0);
        m_Data[m_Head].~T();
        m_Head = (m_Head + 1) % m_Capacity;
        --m_Count;
    }

    /// 清空
    void Clear()
    {
        DestroyAll();
        m_Head = 0;
        m_Count = 0;
    }

    // ========================================================================
    // 访问
    // ========================================================================

    /// 头部元素 (只读)
    LIMX_NODISCARD const T& GetFront() const
    {
        LIMX_ASSERT(m_Count > 0);
        return m_Data[m_Head];
    }

    /// 头部元素 (可写)
    LIMX_NODISCARD T& GetFront()
    {
        LIMX_ASSERT(m_Count > 0);
        return m_Data[m_Head];
    }

    /// 尾部元素 (只读)
    LIMX_NODISCARD const T& GetBack() const
    {
        LIMX_ASSERT(m_Count > 0);
        return m_Data[PhysicalIndex(m_Count - 1)];
    }

    /// 尾部元素 (可写)
    LIMX_NODISCARD T& GetBack()
    {
        LIMX_ASSERT(m_Count > 0);
        return m_Data[PhysicalIndex(m_Count - 1)];
    }

    /// 随机访问 (只读)
    LIMX_NODISCARD const T& operator[](
        SizeType index) const
    {
        LIMX_ASSERT(index < m_Count);
        return m_Data[PhysicalIndex(index)];
    }

    /// 随机访问 (可写)
    LIMX_NODISCARD T& operator[](SizeType index)
    {
        LIMX_ASSERT(index < m_Count);
        return m_Data[PhysicalIndex(index)];
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
    /// 逻辑索引到物理索引
    LIMX_NODISCARD SizeType PhysicalIndex(
        SizeType logicalIndex) const
    {
        return (m_Head + logicalIndex) % m_Capacity;
    }

    /// 确保容量
    void EnsureCapacity()
    {
        if (m_Count >= m_Capacity)
        {
            SizeType newCap = (m_Capacity == 0)
                ? kMinCapacity : m_Capacity * 2;
            Grow(newCap);
        }
    }

    /// 扩容并重排
    void Grow(SizeType newCapacity)
    {
        T* newData = static_cast<T*>(
            GetDefaultAllocator().Allocate(
                newCapacity * sizeof(T), alignof(T)));

        // 将旧元素按逻辑顺序移动到新缓冲区头部
        for (SizeType elemIdx = 0;
             elemIdx < m_Count; ++elemIdx)
        {
            SizeType oldPhys = PhysicalIndex(elemIdx);
            new (&newData[elemIdx]) T(
                MoveTemp(m_Data[oldPhys]));
            m_Data[oldPhys].~T();
        }

        DeallocateBuffer();
        m_Data = newData;
        m_Capacity = newCapacity;
        m_Head = 0;
    }

    /// 销毁所有元素
    void DestroyAll()
    {
        for (SizeType elemIdx = 0;
             elemIdx < m_Count; ++elemIdx)
        {
            m_Data[PhysicalIndex(elemIdx)].~T();
        }
    }

    /// 释放缓冲区
    void DeallocateBuffer()
    {
        if (m_Data != nullptr)
        {
            GetDefaultAllocator().Deallocate(m_Data);
            m_Data = nullptr;
            m_Capacity = 0;
        }
    }

    /// 取大于等于 n 的最小 2 的幂
    static SizeType NextPowerOfTwo(SizeType n)
    {
        if (n <= 1) return 1;
        --n;
        n |= n >> 1;  n |= n >> 2;
        n |= n >> 4;  n |= n >> 8;
        n |= n >> 16; n |= n >> 32;
        return n + 1;
    }

    T*       m_Data;      ///< 缓冲区
    SizeType m_Capacity;  ///< 容量
    SizeType m_Head;      ///< 头部物理索引
    SizeType m_Count;     ///< 元素数量
};

} // namespace Limx
