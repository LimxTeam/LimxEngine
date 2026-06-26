/*******************************************************************************
 * 文件: TStableVector.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   稳定向量 — 元素指针不失效的动态数组
 *   通过分页存储保证已有元素的地址在容器增长时不变
 *   用于需要持久指针/引用的场景，如组件存储、观察者列表等
 *
 * 设计哲学:
 *   分页链表 — 固定大小页面组成的数组，新增元素分配新页面
 *   指针稳定 — 已有页面永不移动，元素地址恒定
 *   O(1) 追加 — 尾部追加在当前页有空间时为 O(1)
 *
 * 技术特性:
 *   - TStableVector<T, PageSize>: 稳定向量
 *   - Add: 追加元素
 *   - operator[]: 随机访问 (二级索引)
 *   - GetCount: 元素数量
 *   - Clear: 清空 (不释放页面内存)
 *   - ReleaseAll: 清空并释放所有页面
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Memory/MemoryOps.h,
 *          Core/Memory/DefaultAllocator.h, Core/Containers/TArray.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Memory/MemoryOps.h"
#include "Core/Memory/IAllocator.h"
#include "Core/Memory/DefaultAllocator.h"
#include "Core/Containers/TArray.h"

namespace Limx
{

/// 稳定向量
/// @tparam T 元素类型
/// @tparam PageSize 每页元素数 (默认 64)
template<typename T, SizeType PageSize = 64>
class TStableVector
{
public:
    TStableVector()
        : m_Count(0)
    {
    }

    ~TStableVector()
    {
        DestroyAll();
        DeallocatePages();
    }

    // 不可拷贝
    TStableVector(const TStableVector&) = delete;
    TStableVector& operator=(const TStableVector&) = delete;

    // 可移动
    TStableVector(TStableVector&& other) noexcept
        : m_Pages(MoveTemp(other.m_Pages))
        , m_Count(other.m_Count)
    {
        other.m_Count = 0;
    }

    TStableVector& operator=(TStableVector&& other) noexcept
    {
        if (this != &other)
        {
            DestroyAll();
            DeallocatePages();
            m_Pages = MoveTemp(other.m_Pages);
            m_Count = other.m_Count;
            other.m_Count = 0;
        }
        return *this;
    }

    // ========================================================================
    // 添加
    // ========================================================================

    /// 追加元素 (拷贝)
    T& Add(const T& element)
    {
        EnsureCapacity();
        SizeType pageIdx = m_Count / PageSize;
        SizeType slotIdx = m_Count % PageSize;
        T* slot = &m_Pages[pageIdx][slotIdx];
        new (slot) T(element);
        ++m_Count;
        return *slot;
    }

    /// 追加元素 (移动)
    T& Add(T&& element)
    {
        EnsureCapacity();
        SizeType pageIdx = m_Count / PageSize;
        SizeType slotIdx = m_Count % PageSize;
        T* slot = &m_Pages[pageIdx][slotIdx];
        new (slot) T(MoveTemp(element));
        ++m_Count;
        return *slot;
    }

    /// 默认构造追加
    T& AddDefault()
    {
        EnsureCapacity();
        SizeType pageIdx = m_Count / PageSize;
        SizeType slotIdx = m_Count % PageSize;
        T* slot = &m_Pages[pageIdx][slotIdx];
        new (slot) T();
        ++m_Count;
        return *slot;
    }

    // ========================================================================
    // 访问
    // ========================================================================

    /// 随机访问 (只读)
    LIMX_NODISCARD const T& operator[](
        SizeType index) const
    {
        LIMX_ASSERT(index < m_Count);
        SizeType pageIdx = index / PageSize;
        SizeType slotIdx = index % PageSize;
        return m_Pages[pageIdx][slotIdx];
    }

    /// 随机访问 (可写)
    LIMX_NODISCARD T& operator[](SizeType index)
    {
        LIMX_ASSERT(index < m_Count);
        SizeType pageIdx = index / PageSize;
        SizeType slotIdx = index % PageSize;
        return m_Pages[pageIdx][slotIdx];
    }

    /// 获取元素指针 (稳定 — 不会因容器增长而失效)
    LIMX_NODISCARD T* GetPtr(SizeType index)
    {
        LIMX_ASSERT(index < m_Count);
        SizeType pageIdx = index / PageSize;
        SizeType slotIdx = index % PageSize;
        return &m_Pages[pageIdx][slotIdx];
    }

    LIMX_NODISCARD const T* GetPtr(SizeType index) const
    {
        LIMX_ASSERT(index < m_Count);
        SizeType pageIdx = index / PageSize;
        SizeType slotIdx = index % PageSize;
        return &m_Pages[pageIdx][slotIdx];
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 元素数量
    LIMX_NODISCARD SizeType GetCount() const
    {
        return m_Count;
    }

    /// 是否为空
    LIMX_NODISCARD bool IsEmpty() const
    {
        return m_Count == 0;
    }

    /// 页面数
    LIMX_NODISCARD SizeType GetPageCount() const
    {
        return m_Pages.GetSize();
    }

    /// 总容量 (已分配页面可容纳的元素数)
    LIMX_NODISCARD SizeType GetCapacity() const
    {
        return m_Pages.GetSize() * PageSize;
    }

    // ========================================================================
    // 清空
    // ========================================================================

    /// 清空元素 (保留页面内存)
    void Clear()
    {
        DestroyAll();
        m_Count = 0;
    }

    /// 清空并释放所有页面
    void ReleaseAll()
    {
        DestroyAll();
        DeallocatePages();
        m_Count = 0;
    }

private:
    /// 确保有空间容纳下一个元素
    void EnsureCapacity()
    {
        SizeType neededPages = (m_Count / PageSize) + 1;
        while (m_Pages.GetSize() < neededPages)
        {
            T* page = static_cast<T*>(
                GetDefaultAllocator().Allocate(
                    PageSize * sizeof(T), alignof(T)));
            m_Pages.Add(page);
        }
    }

    /// 析构所有已构造的元素
    void DestroyAll()
    {
        for (SizeType elemIdx = 0;
             elemIdx < m_Count; ++elemIdx)
        {
            SizeType pageIdx = elemIdx / PageSize;
            SizeType slotIdx = elemIdx % PageSize;
            m_Pages[pageIdx][slotIdx].~T();
        }
    }

    /// 释放所有页面内存
    void DeallocatePages()
    {
        for (SizeType pageIdx = 0;
             pageIdx < m_Pages.GetSize(); ++pageIdx)
        {
            GetDefaultAllocator().Deallocate(m_Pages[pageIdx]);
        }
        m_Pages.Clear();
    }

    TArray<T*> m_Pages;   ///< 页面指针数组
    SizeType   m_Count;   ///< 元素数量
};

} // namespace Limx
