/*******************************************************************************
 * 文件: TFixedVector.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   固定容量动态数组 — 栈存储、不超过编译时最大容量的动态数组
 *   无堆分配，适合小型列表、临时收集结果、参数打包等场景
 *   用于渲染命令列表、顶点缓存、材质参数列表等
 *
 * 设计哲学:
 *   栈存储 — 内嵌对齐数组，零堆分配
 *   动态长度 — [0, MaxSize] 范围内自由增删
 *   TArray 接口兼容 — Add/Remove/GetSize/operator[] 与 TArray 一致
 *
 * 技术特性:
 *   - TFixedVector<T, MaxSize>: 固定容量动态数组
 *   - Add/AddUnique: 追加元素
 *   - Remove/RemoveAt: 删除元素
 *   - GetSize/IsEmpty/IsFull: 查询
 *   - begin/end: 范围 for 支持
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

/// 固定容量动态数组
/// @tparam T 元素类型
/// @tparam MaxSize 最大元素数
template<typename T, SizeType MaxSize>
class TFixedVector
{
    static_assert(MaxSize > 0,
        "MaxSize must be > 0");

public:
    TFixedVector() : m_Size(0) {}

    // 拷贝构造
    TFixedVector(const TFixedVector& other)
        : m_Size(0)
    {
        for (SizeType elemIdx = 0;
             elemIdx < other.m_Size; ++elemIdx)
        {
            ConstructAt(m_Size++, other.GetPtr()[elemIdx]);
        }
    }

    // 移动构造
    TFixedVector(TFixedVector&& other)
        : m_Size(0)
    {
        for (SizeType elemIdx = 0;
             elemIdx < other.m_Size; ++elemIdx)
        {
            ConstructAt(m_Size++,
                MoveTemp(other.GetPtr()[elemIdx]));
        }
        other.Clear();
    }

    ~TFixedVector()
    {
        Clear();
    }

    TFixedVector& operator=(const TFixedVector& other)
    {
        if (this == &other) return *this;
        Clear();
        for (SizeType elemIdx = 0;
             elemIdx < other.m_Size; ++elemIdx)
        {
            ConstructAt(m_Size++, other.GetPtr()[elemIdx]);
        }
        return *this;
    }

    TFixedVector& operator=(TFixedVector&& other)
    {
        if (this == &other) return *this;
        Clear();
        for (SizeType elemIdx = 0;
             elemIdx < other.m_Size; ++elemIdx)
        {
            ConstructAt(m_Size++,
                MoveTemp(other.GetPtr()[elemIdx]));
        }
        other.Clear();
        return *this;
    }

    // ========================================================================
    // 增加
    // ========================================================================

    /// 追加元素 (拷贝)
    /// @return 是否成功 (false = 已满)
    bool Add(const T& element)
    {
        if (m_Size >= MaxSize) return false;
        ConstructAt(m_Size++, element);
        return true;
    }

    /// 追加元素 (移动)
    bool Add(T&& element)
    {
        if (m_Size >= MaxSize) return false;
        ConstructAt(m_Size++, MoveTemp(element));
        return true;
    }

    /// 追加元素并断言不满
    void AddChecked(const T& element)
    {
        LIMX_ASSERT(m_Size < MaxSize);
        ConstructAt(m_Size++, element);
    }

    /// 唯一添加 (不重复)
    bool AddUnique(const T& element)
    {
        for (SizeType elemIdx = 0;
             elemIdx < m_Size; ++elemIdx)
        {
            if (GetPtr()[elemIdx] == element) return false;
        }
        return Add(element);
    }

    // ========================================================================
    // 删除
    // ========================================================================

    /// 按索引删除 (保序, O(n))
    void RemoveAt(SizeType index)
    {
        LIMX_ASSERT(index < m_Size);
        GetPtr()[index].~T();
        for (SizeType shiftIdx = index + 1;
             shiftIdx < m_Size; ++shiftIdx)
        {
            MemMove(
                GetPtr() + shiftIdx - 1,
                GetPtr() + shiftIdx,
                sizeof(T));
        }
        --m_Size;
    }

    /// 按索引删除 (交换末尾, O(1))
    void RemoveAtSwap(SizeType index)
    {
        LIMX_ASSERT(index < m_Size);
        GetPtr()[index].~T();
        if (index != m_Size - 1)
        {
            MemMove(
                GetPtr() + index,
                GetPtr() + m_Size - 1,
                sizeof(T));
        }
        --m_Size;
    }

    /// 删除第一个匹配元素
    bool Remove(const T& element)
    {
        for (SizeType elemIdx = 0;
             elemIdx < m_Size; ++elemIdx)
        {
            if (GetPtr()[elemIdx] == element)
            {
                RemoveAt(elemIdx);
                return true;
            }
        }
        return false;
    }

    /// 清空
    void Clear()
    {
        for (SizeType elemIdx = 0;
             elemIdx < m_Size; ++elemIdx)
        {
            GetPtr()[elemIdx].~T();
        }
        m_Size = 0;
    }

    // ========================================================================
    // 访问
    // ========================================================================

    LIMX_NODISCARD T& operator[](SizeType index)
    {
        LIMX_ASSERT(index < m_Size);
        return GetPtr()[index];
    }

    LIMX_NODISCARD const T& operator[](
        SizeType index) const
    {
        LIMX_ASSERT(index < m_Size);
        return GetPtr()[index];
    }

    LIMX_NODISCARD T& First()
    {
        LIMX_ASSERT(m_Size > 0);
        return GetPtr()[0];
    }

    LIMX_NODISCARD const T& First() const
    {
        LIMX_ASSERT(m_Size > 0);
        return GetPtr()[0];
    }

    LIMX_NODISCARD T& Last()
    {
        LIMX_ASSERT(m_Size > 0);
        return GetPtr()[m_Size - 1];
    }

    LIMX_NODISCARD const T& Last() const
    {
        LIMX_ASSERT(m_Size > 0);
        return GetPtr()[m_Size - 1];
    }

    LIMX_NODISCARD T* GetData() { return GetPtr(); }
    LIMX_NODISCARD const T* GetData() const
    {
        return GetPtr();
    }

    // ========================================================================
    // 查询
    // ========================================================================

    LIMX_NODISCARD SizeType GetSize() const
    {
        return m_Size;
    }

    LIMX_NODISCARD static constexpr SizeType GetMaxSize()
    {
        return MaxSize;
    }

    LIMX_NODISCARD bool IsEmpty() const
    {
        return m_Size == 0;
    }

    LIMX_NODISCARD bool IsFull() const
    {
        return m_Size >= MaxSize;
    }

    LIMX_NODISCARD bool Contains(const T& element) const
    {
        for (SizeType elemIdx = 0;
             elemIdx < m_Size; ++elemIdx)
        {
            if (GetPtr()[elemIdx] == element) return true;
        }
        return false;
    }

    // ========================================================================
    // 范围 for
    // ========================================================================

    LIMX_NODISCARD T* begin() { return GetPtr(); }
    LIMX_NODISCARD const T* begin() const
    {
        return GetPtr();
    }
    LIMX_NODISCARD T* end() { return GetPtr() + m_Size; }
    LIMX_NODISCARD const T* end() const
    {
        return GetPtr() + m_Size;
    }

private:
    void ConstructAt(SizeType index, const T& value)
    {
        new (GetPtr() + index) T(value);
    }

    void ConstructAt(SizeType index, T&& value)
    {
        new (GetPtr() + index) T(MoveTemp(value));
    }

    T* GetPtr()
    {
        return reinterpret_cast<T*>(m_Storage);
    }

    const T* GetPtr() const
    {
        return reinterpret_cast<const T*>(m_Storage);
    }

    alignas(T) UInt8 m_Storage[sizeof(T) * MaxSize];
    SizeType m_Size;
};

} // namespace Limx
