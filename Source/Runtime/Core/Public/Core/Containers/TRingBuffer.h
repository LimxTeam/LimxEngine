/*******************************************************************************
 * 文件: TRingBuffer.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   固定大小环形缓冲区 — 连续内存的 FIFO 容器
 *   写满后自动覆盖最旧元素，适用于日志缓冲、帧历史、性能采样等场景
 *   不触发动态重分配，容量在构造时确定
 *
 * 设计哲学:
 *   固定容量 — 构造后容量不变，写满自动覆盖最旧数据
 *   连续内存 — 缓存友好，无链表开销
 *   O(1) 操作 — Push/Pop/Front/Back 均为常数时间
 *   分配器感知 — 通过 IAllocator 分配底层存储
 *
 * 技术特性:
 *   - Push: 写入尾部，满时覆盖头部
 *   - Pop: 移除头部
 *   - Front/Back: 访问头尾元素
 *   - operator[]: 按逻辑索引访问 (0 = 最旧)
 *   - 连续内存布局，2 的幂容量 + 位掩码取模
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

/// 固定大小环形缓冲区
/// @tparam TElement 元素类型
template<typename TElement>
class TRingBuffer
{
public:
    // ========================================================================
    // 构造与析构
    // ========================================================================

    /// 构造指定容量的环形缓冲区 (会向上取到 2 的幂)
    explicit TRingBuffer(SizeType capacity)
        : m_Buffer(nullptr)
        , m_Capacity(0)
        , m_Head(0)
        , m_Size(0)
        , m_Allocator(&GetDefaultAllocator())
    {
        LIMX_ASSERT(capacity > 0);
        // 向上取 2 的幂
        m_Capacity = 1;
        while (m_Capacity < capacity)
        {
            m_Capacity <<= 1;
        }
        m_Buffer = static_cast<TElement*>(
            m_Allocator->Allocate(
                sizeof(TElement) * m_Capacity, alignof(TElement)));
    }

    TRingBuffer(SizeType capacity, IAllocator& allocator)
        : m_Buffer(nullptr)
        , m_Capacity(0)
        , m_Head(0)
        , m_Size(0)
        , m_Allocator(&allocator)
    {
        LIMX_ASSERT(capacity > 0);
        m_Capacity = 1;
        while (m_Capacity < capacity)
        {
            m_Capacity <<= 1;
        }
        m_Buffer = static_cast<TElement*>(
            m_Allocator->Allocate(
                sizeof(TElement) * m_Capacity, alignof(TElement)));
    }

    TRingBuffer(const TRingBuffer& other)
        : m_Buffer(nullptr)
        , m_Capacity(other.m_Capacity)
        , m_Head(0)
        , m_Size(0)
        , m_Allocator(other.m_Allocator)
    {
        m_Buffer = static_cast<TElement*>(
            m_Allocator->Allocate(
                sizeof(TElement) * m_Capacity, alignof(TElement)));
        for (SizeType index = 0; index < other.m_Size; ++index)
        {
            Push(other[index]);
        }
    }

    TRingBuffer(TRingBuffer&& other) noexcept
        : m_Buffer(other.m_Buffer)
        , m_Capacity(other.m_Capacity)
        , m_Head(other.m_Head)
        , m_Size(other.m_Size)
        , m_Allocator(other.m_Allocator)
    {
        other.m_Buffer = nullptr;
        other.m_Capacity = 0;
        other.m_Head = 0;
        other.m_Size = 0;
    }

    ~TRingBuffer()
    {
        Clear();
        if (m_Buffer)
        {
            m_Allocator->Deallocate(m_Buffer);
        }
    }

    TRingBuffer& operator=(const TRingBuffer& other)
    {
        if (this != &other)
        {
            Clear();
            if (m_Capacity != other.m_Capacity)
            {
                if (m_Buffer)
                {
                    m_Allocator->Deallocate(m_Buffer);
                }
                m_Capacity = other.m_Capacity;
                m_Allocator = other.m_Allocator;
                m_Buffer = static_cast<TElement*>(
                    m_Allocator->Allocate(
                        sizeof(TElement) * m_Capacity, alignof(TElement)));
            }
            m_Head = 0;
            m_Size = 0;
            for (SizeType index = 0; index < other.m_Size; ++index)
            {
                Push(other[index]);
            }
        }
        return *this;
    }

    TRingBuffer& operator=(TRingBuffer&& other) noexcept
    {
        if (this != &other)
        {
            Clear();
            if (m_Buffer)
            {
                m_Allocator->Deallocate(m_Buffer);
            }
            m_Buffer = other.m_Buffer;
            m_Capacity = other.m_Capacity;
            m_Head = other.m_Head;
            m_Size = other.m_Size;
            m_Allocator = other.m_Allocator;
            other.m_Buffer = nullptr;
            other.m_Capacity = 0;
            other.m_Head = 0;
            other.m_Size = 0;
        }
        return *this;
    }

    // ========================================================================
    // 写入
    // ========================================================================

    /// 写入元素到尾部 — 满时覆盖最旧元素
    void Push(const TElement& element)
    {
        SizeType mask = m_Capacity - 1;
        SizeType writeIndex = (m_Head + m_Size) & mask;

        if (m_Size == m_Capacity)
        {
            // 满 — 覆盖最旧元素
            m_Buffer[writeIndex].~TElement();
            new (&m_Buffer[writeIndex]) TElement(element);
            m_Head = (m_Head + 1) & mask;
        }
        else
        {
            new (&m_Buffer[writeIndex]) TElement(element);
            m_Size++;
        }
    }

    void Push(TElement&& element)
    {
        SizeType mask = m_Capacity - 1;
        SizeType writeIndex = (m_Head + m_Size) & mask;

        if (m_Size == m_Capacity)
        {
            m_Buffer[writeIndex].~TElement();
            new (&m_Buffer[writeIndex]) TElement(MoveTemp(element));
            m_Head = (m_Head + 1) & mask;
        }
        else
        {
            new (&m_Buffer[writeIndex]) TElement(MoveTemp(element));
            m_Size++;
        }
    }

    /// 原地构造写入
    template<typename... Args>
    void EmplacePush(Args&&... args)
    {
        SizeType mask = m_Capacity - 1;
        SizeType writeIndex = (m_Head + m_Size) & mask;

        if (m_Size == m_Capacity)
        {
            m_Buffer[writeIndex].~TElement();
            new (&m_Buffer[writeIndex]) TElement(Forward<Args>(args)...);
            m_Head = (m_Head + 1) & mask;
        }
        else
        {
            new (&m_Buffer[writeIndex]) TElement(Forward<Args>(args)...);
            m_Size++;
        }
    }

    // ========================================================================
    // 读取与移除
    // ========================================================================

    /// 移除最旧元素 (头部)
    void Pop()
    {
        LIMX_ASSERT(m_Size > 0);
        m_Buffer[m_Head].~TElement();
        m_Head = (m_Head + 1) & (m_Capacity - 1);
        m_Size--;
    }

    /// 安全弹出 — 如果非空则弹出到 outElement
    bool Pop(TElement& outElement)
    {
        if (m_Size == 0)
        {
            return false;
        }
        outElement = MoveTemp(m_Buffer[m_Head]);
        m_Buffer[m_Head].~TElement();
        m_Head = (m_Head + 1) & (m_Capacity - 1);
        m_Size--;
        return true;
    }

    /// 访问最旧元素 (头部)
    LIMX_NODISCARD const TElement& Front() const
    {
        LIMX_ASSERT(m_Size > 0);
        return m_Buffer[m_Head];
    }

    LIMX_NODISCARD TElement& Front()
    {
        LIMX_ASSERT(m_Size > 0);
        return m_Buffer[m_Head];
    }

    /// 访问最新元素 (尾部)
    LIMX_NODISCARD const TElement& Back() const
    {
        LIMX_ASSERT(m_Size > 0);
        SizeType backIndex = (m_Head + m_Size - 1) & (m_Capacity - 1);
        return m_Buffer[backIndex];
    }

    LIMX_NODISCARD TElement& Back()
    {
        LIMX_ASSERT(m_Size > 0);
        SizeType backIndex = (m_Head + m_Size - 1) & (m_Capacity - 1);
        return m_Buffer[backIndex];
    }

    /// 按逻辑索引访问 (0 = 最旧元素)
    LIMX_NODISCARD const TElement& operator[](SizeType index) const
    {
        LIMX_ASSERT(index < m_Size);
        SizeType physicalIndex = (m_Head + index) & (m_Capacity - 1);
        return m_Buffer[physicalIndex];
    }

    LIMX_NODISCARD TElement& operator[](SizeType index)
    {
        LIMX_ASSERT(index < m_Size);
        SizeType physicalIndex = (m_Head + index) & (m_Capacity - 1);
        return m_Buffer[physicalIndex];
    }

    // ========================================================================
    // 状态查询
    // ========================================================================

    LIMX_NODISCARD FORCEINLINE SizeType GetSize() const { return m_Size; }
    LIMX_NODISCARD FORCEINLINE SizeType GetCapacity() const { return m_Capacity; }
    LIMX_NODISCARD FORCEINLINE bool IsEmpty() const { return m_Size == 0; }
    LIMX_NODISCARD FORCEINLINE bool IsFull() const { return m_Size == m_Capacity; }

    // ========================================================================
    // 清空
    // ========================================================================

    void Clear()
    {
        SizeType mask = m_Capacity - 1;
        for (SizeType index = 0; index < m_Size; ++index)
        {
            SizeType physicalIndex = (m_Head + index) & mask;
            m_Buffer[physicalIndex].~TElement();
        }
        m_Head = 0;
        m_Size = 0;
    }

private:
    // ========================================================================
    // 成员数据
    // ========================================================================

    TElement*   m_Buffer;     ///< 底层存储
    SizeType    m_Capacity;   ///< 容量 (2 的幂)
    SizeType    m_Head;       ///< 头部物理索引
    SizeType    m_Size;       ///< 当前元素数量
    IAllocator* m_Allocator;  ///< 内存分配器
};

} // namespace Limx
