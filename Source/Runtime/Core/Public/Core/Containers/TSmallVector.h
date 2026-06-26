/*******************************************************************************
 * 文件: TSmallVector.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   小缓冲区优化动态数组 — 内联存储小量元素，超出时切换到堆分配
 *   类似 LLVM SmallVector，在元素数较少时避免堆分配开销
 *   用于局部临时数组、短列表参数传递、栈上收集结果等场景
 *
 * 设计哲学:
 *   内联优先 — 编译时指定内联容量，元素数 <= N 时零堆分配
 *   透明溢出 — 超出内联容量后自动切换到堆分配，接口不变
 *   TArray 兼容 — API 与 TArray 一致，可互操作
 *
 * 技术特性:
 *   - TSmallVector<T, InlineCapacity>: SBO 动态数组
 *   - Add/RemoveAt: 增删元素
 *   - operator[]: 随机访问
 *   - GetSize/GetCapacity: 查询
 *   - IsUsingInlineStorage: 是否使用内联存储
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

/// 小缓冲区优化动态数组
/// @tparam T 元素类型
/// @tparam InlineCapacity 内联容量 (编译时常量)
template<typename T, SizeType InlineCapacity>
class TSmallVector
{
    static_assert(InlineCapacity > 0,
        "TSmallVector InlineCapacity must be > 0");

public:
    // ========================================================================
    // 构造与析构
    // ========================================================================

    TSmallVector()
        : m_Data(GetInlineBuffer())
        , m_Size(0)
        , m_Capacity(InlineCapacity)
    {
    }

    ~TSmallVector()
    {
        Memory::DestructItems(m_Data, m_Size);
        if (!IsUsingInlineStorage())
        {
            GetDefaultAllocator().Deallocate(m_Data);
        }
    }

    /// 拷贝构造
    TSmallVector(const TSmallVector& other)
        : m_Data(GetInlineBuffer())
        , m_Size(0)
        , m_Capacity(InlineCapacity)
    {
        Reserve(other.m_Size);
        Memory::CopyConstructItems(m_Data, other.m_Data, other.m_Size);
        m_Size = other.m_Size;
    }

    /// 移动构造
    TSmallVector(TSmallVector&& other) noexcept
        : m_Data(GetInlineBuffer())
        , m_Size(0)
        , m_Capacity(InlineCapacity)
    {
        if (other.IsUsingInlineStorage())
        {
            // 内联存储 — 逐元素移动
            Memory::MoveConstructItems(m_Data, other.m_Data, other.m_Size);
            m_Size = other.m_Size;
        }
        else
        {
            // 堆存储 — 窃取指针
            m_Data = other.m_Data;
            m_Size = other.m_Size;
            m_Capacity = other.m_Capacity;
            other.m_Data = other.GetInlineBuffer();
        }
        other.m_Size = 0;
        other.m_Capacity = InlineCapacity;
    }

    /// 拷贝赋值
    TSmallVector& operator=(const TSmallVector& other)
    {
        if (this != &other)
        {
            Clear();
            Reserve(other.m_Size);
            Memory::CopyConstructItems(m_Data, other.m_Data, other.m_Size);
            m_Size = other.m_Size;
        }
        return *this;
    }

    /// 移动赋值
    TSmallVector& operator=(TSmallVector&& other) noexcept
    {
        if (this != &other)
        {
            Memory::DestructItems(m_Data, m_Size);
            if (!IsUsingInlineStorage())
            {
                GetDefaultAllocator().Deallocate(m_Data);
            }

            if (other.IsUsingInlineStorage())
            {
                m_Data = GetInlineBuffer();
                m_Capacity = InlineCapacity;
                Memory::MoveConstructItems(m_Data, other.m_Data, other.m_Size);
                m_Size = other.m_Size;
            }
            else
            {
                m_Data = other.m_Data;
                m_Size = other.m_Size;
                m_Capacity = other.m_Capacity;
                other.m_Data = other.GetInlineBuffer();
            }
            other.m_Size = 0;
            other.m_Capacity = InlineCapacity;
        }
        return *this;
    }

    // ========================================================================
    // 添加元素
    // ========================================================================

    /// 追加元素 (拷贝)
    void Add(const T& element)
    {
        EnsureCapacity(m_Size + 1);
        new (m_Data + m_Size) T(element);
        ++m_Size;
    }

    /// 追加元素 (移动)
    void Add(T&& element)
    {
        EnsureCapacity(m_Size + 1);
        new (m_Data + m_Size) T(MoveTemp(element));
        ++m_Size;
    }

    // ========================================================================
    // 删除元素
    // ========================================================================

    /// 删除指定索引处的元素
    void RemoveAt(SizeType index)
    {
        LIMX_ASSERT(index < m_Size);
        m_Data[index].~T();

        // 移动后续元素
        if (index < m_Size - 1)
        {
            Memory::MoveConstructItems(
                m_Data + index, m_Data + index + 1,
                m_Size - index - 1);
        }
        --m_Size;
    }

    /// 快速删除 (用最后一个元素填充)
    void RemoveAtSwap(SizeType index)
    {
        LIMX_ASSERT(index < m_Size);
        m_Data[index].~T();

        if (index < m_Size - 1)
        {
            new (m_Data + index) T(MoveTemp(m_Data[m_Size - 1]));
            m_Data[m_Size - 1].~T();
        }
        --m_Size;
    }

    /// 清空所有元素
    void Clear()
    {
        Memory::DestructItems(m_Data, m_Size);
        m_Size = 0;
    }

    // ========================================================================
    // 访问
    // ========================================================================

    LIMX_NODISCARD T& operator[](SizeType index)
    {
        LIMX_ASSERT(index < m_Size);
        return m_Data[index];
    }

    LIMX_NODISCARD const T& operator[](SizeType index) const
    {
        LIMX_ASSERT(index < m_Size);
        return m_Data[index];
    }

    LIMX_NODISCARD T* GetData() { return m_Data; }
    LIMX_NODISCARD const T* GetData() const { return m_Data; }

    LIMX_NODISCARD T& Last()
    {
        LIMX_ASSERT(m_Size > 0);
        return m_Data[m_Size - 1];
    }

    LIMX_NODISCARD const T& Last() const
    {
        LIMX_ASSERT(m_Size > 0);
        return m_Data[m_Size - 1];
    }

    // ========================================================================
    // 查询
    // ========================================================================

    LIMX_NODISCARD SizeType GetSize() const { return m_Size; }
    LIMX_NODISCARD SizeType GetCapacity() const { return m_Capacity; }
    LIMX_NODISCARD bool IsEmpty() const { return m_Size == 0; }

    LIMX_NODISCARD bool IsUsingInlineStorage() const
    {
        return m_Data == const_cast<TSmallVector*>(this)->GetInlineBuffer();
    }

    LIMX_NODISCARD static constexpr SizeType GetInlineCapacity()
    {
        return InlineCapacity;
    }

    // ========================================================================
    // 容量
    // ========================================================================

    /// 预分配容量
    void Reserve(SizeType newCapacity)
    {
        if (newCapacity <= m_Capacity) return;
        Grow(newCapacity);
    }

    // ========================================================================
    // 迭代器
    // ========================================================================

    LIMX_NODISCARD T* begin() { return m_Data; }
    LIMX_NODISCARD T* end() { return m_Data + m_Size; }
    LIMX_NODISCARD const T* begin() const { return m_Data; }
    LIMX_NODISCARD const T* end() const { return m_Data + m_Size; }

private:
    /// 获取内联缓冲区指针
    T* GetInlineBuffer()
    {
        return reinterpret_cast<T*>(&m_InlineStorage);
    }

    /// 确保容量
    void EnsureCapacity(SizeType requiredSize)
    {
        if (requiredSize > m_Capacity)
        {
            SizeType newCapacity = m_Capacity * 2;
            if (newCapacity < requiredSize)
            {
                newCapacity = requiredSize;
            }
            Grow(newCapacity);
        }
    }

    /// 扩容
    void Grow(SizeType newCapacity)
    {
        T* newData = static_cast<T*>(
            GetDefaultAllocator().Allocate(
                newCapacity * sizeof(T), alignof(T)));

        // 移动已有元素
        if (m_Size > 0)
        {
            Memory::MoveConstructItems(newData, m_Data, m_Size);
            Memory::DestructItems(m_Data, m_Size);
        }

        // 释放旧堆存储
        if (!IsUsingInlineStorage())
        {
            GetDefaultAllocator().Deallocate(m_Data);
        }

        m_Data = newData;
        m_Capacity = newCapacity;
    }

    T*       m_Data;      ///< 数据指针 (指向内联或堆)
    SizeType m_Size;      ///< 当前元素数
    SizeType m_Capacity;  ///< 当前容量

    /// 内联存储 — 对齐的原始字节
    alignas(alignof(T)) char m_InlineStorage[InlineCapacity * sizeof(T)];
};

} // namespace Limx
