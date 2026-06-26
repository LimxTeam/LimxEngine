/*******************************************************************************
 * 文件: TFixedArray.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   固定容量动态数组 — 内联存储，无堆分配
 *   编译时确定最大容量，运行时动态添加/删除元素
 *   用于小集合、栈上临时缓冲、嵌入式结构中的可变数组等场景
 *
 * 设计哲学:
 *   内联存储 — 元素存储在对象自身内存中，零堆分配
 *   固定上限 — 编译时确定最大容量，超出触发断言
 *   TArray 兼容 — 提供类似 TArray 的接口 (Add/Remove/operator[])
 *
 * 技术特性:
 *   - 存储: alignas(T) char[sizeof(T) * N] 内联缓冲
 *   - Add/EmplaceAdd: 添加元素 (断言不超出容量)
 *   - RemoveAt: 删除指定索引元素 (交换尾部)
 *   - operator[]: 索引访问
 *   - GetSize/GetCapacity: 动态大小 / 编译时容量
 *   - 支持迭代器 (begin/end)
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h,
 *          Core/Memory/MemoryOps.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Memory/MemoryOps.h"

namespace Limx
{

/// 固定容量动态数组 — 内联存储，无堆分配
/// @tparam T 元素类型
/// @tparam N 最大容量
template<typename T, SizeType N>
class TFixedArray
{
    static_assert(N > 0, "TFixedArray capacity must be > 0");

public:
    // ========================================================================
    // 构造与析构
    // ========================================================================

    TFixedArray() : m_Size(0) {}

    ~TFixedArray()
    {
        Clear();
    }

    /// 拷贝构造
    TFixedArray(const TFixedArray& other) : m_Size(0)
    {
        for (SizeType index = 0; index < other.m_Size; ++index)
        {
            new (GetPtr(index)) T(other[index]);
        }
        m_Size = other.m_Size;
    }

    /// 拷贝赋值
    TFixedArray& operator=(const TFixedArray& other)
    {
        if (this != &other)
        {
            Clear();
            for (SizeType index = 0; index < other.m_Size; ++index)
            {
                new (GetPtr(index)) T(other[index]);
            }
            m_Size = other.m_Size;
        }
        return *this;
    }

    /// 移动构造
    TFixedArray(TFixedArray&& other) noexcept : m_Size(0)
    {
        for (SizeType index = 0; index < other.m_Size; ++index)
        {
            new (GetPtr(index)) T(MoveTemp(other[index]));
        }
        m_Size = other.m_Size;
        other.Clear();
    }

    /// 移动赋值
    TFixedArray& operator=(TFixedArray&& other) noexcept
    {
        if (this != &other)
        {
            Clear();
            for (SizeType index = 0; index < other.m_Size; ++index)
            {
                new (GetPtr(index)) T(MoveTemp(other[index]));
            }
            m_Size = other.m_Size;
            other.Clear();
        }
        return *this;
    }

    // ========================================================================
    // 添加
    // ========================================================================

    /// 添加元素 (拷贝)
    void Add(const T& value)
    {
        LIMX_ASSERT(m_Size < N);
        new (GetPtr(m_Size)) T(value);
        ++m_Size;
    }

    /// 添加元素 (移动)
    void Add(T&& value)
    {
        LIMX_ASSERT(m_Size < N);
        new (GetPtr(m_Size)) T(MoveTemp(value));
        ++m_Size;
    }

    // ========================================================================
    // 删除
    // ========================================================================

    /// 删除指定索引元素 (与尾部交换，不保序)
    void RemoveAtSwap(SizeType index)
    {
        LIMX_ASSERT(index < m_Size);

        (*this)[index].~T();

        if (index + 1 < m_Size)
        {
            new (GetPtr(index)) T(MoveTemp((*this)[m_Size - 1]));
            (*this)[m_Size - 1].~T();
        }
        --m_Size;
    }

    /// 删除指定索引元素 (保持顺序，后续元素前移)
    void RemoveAt(SizeType index)
    {
        LIMX_ASSERT(index < m_Size);

        (*this)[index].~T();

        for (SizeType moveIndex = index;
             moveIndex + 1 < m_Size; ++moveIndex)
        {
            new (GetPtr(moveIndex))
                T(MoveTemp((*this)[moveIndex + 1]));
            (*this)[moveIndex + 1].~T();
        }
        --m_Size;
    }

    /// 删除最后一个元素
    void RemoveLast()
    {
        LIMX_ASSERT(m_Size > 0);
        (*this)[m_Size - 1].~T();
        --m_Size;
    }

    /// 清空所有元素
    void Clear()
    {
        for (SizeType index = 0; index < m_Size; ++index)
        {
            (*this)[index].~T();
        }
        m_Size = 0;
    }

    // ========================================================================
    // 访问
    // ========================================================================

    LIMX_NODISCARD T& operator[](SizeType index)
    {
        LIMX_ASSERT(index < m_Size);
        return *GetPtr(index);
    }

    LIMX_NODISCARD const T& operator[](SizeType index) const
    {
        LIMX_ASSERT(index < m_Size);
        return *GetPtr(index);
    }

    LIMX_NODISCARD T& Front()
    {
        LIMX_ASSERT(m_Size > 0);
        return *GetPtr(0);
    }

    LIMX_NODISCARD const T& Front() const
    {
        LIMX_ASSERT(m_Size > 0);
        return *GetPtr(0);
    }

    LIMX_NODISCARD T& Back()
    {
        LIMX_ASSERT(m_Size > 0);
        return *GetPtr(m_Size - 1);
    }

    LIMX_NODISCARD const T& Back() const
    {
        LIMX_ASSERT(m_Size > 0);
        return *GetPtr(m_Size - 1);
    }

    LIMX_NODISCARD T* GetData()
    {
        return GetPtr(0);
    }

    LIMX_NODISCARD const T* GetData() const
    {
        return GetPtr(0);
    }

    // ========================================================================
    // 状态
    // ========================================================================

    LIMX_NODISCARD SizeType GetSize() const { return m_Size; }
    LIMX_NODISCARD static constexpr SizeType GetCapacity() { return N; }
    LIMX_NODISCARD bool IsEmpty() const { return m_Size == 0; }
    LIMX_NODISCARD bool IsFull() const { return m_Size == N; }

    // ========================================================================
    // 迭代器
    // ========================================================================

    LIMX_NODISCARD T* begin() { return GetPtr(0); }
    LIMX_NODISCARD const T* begin() const { return GetPtr(0); }
    LIMX_NODISCARD T* end() { return GetPtr(m_Size); }
    LIMX_NODISCARD const T* end() const { return GetPtr(m_Size); }

private:
    /// 获取索引处的指针
    T* GetPtr(SizeType index)
    {
        return reinterpret_cast<T*>(m_Storage) + index;
    }

    const T* GetPtr(SizeType index) const
    {
        return reinterpret_cast<const T*>(m_Storage) + index;
    }

    // ========================================================================
    // 成员数据
    // ========================================================================

    alignas(T) UInt8 m_Storage[sizeof(T) * N];  ///< 内联存储
    SizeType m_Size;                              ///< 当前元素数
};

} // namespace Limx
