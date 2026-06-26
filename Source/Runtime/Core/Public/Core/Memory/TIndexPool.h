/*******************************************************************************
 * 文件: TIndexPool.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   索引池 — 可复用整数 ID 分配器
 *   维护空闲索引列表，Acquire 返回唯一索引，Release 放回复用
 *   用于实体 ID 分配、组件插槽管理、GPU 资源槽位分配等场景
 *
 * 设计哲学:
 *   紧凑复用 — 释放的索引放入空闲栈，下次优先复用
 *   范围可控 — 最大索引数编译时或运行时指定
 *   O(1) 操作 — Acquire/Release 均为 O(1)
 *
 * 技术特性:
 *   - TIndexPool: 整数索引分配器
 *   - Acquire: 分配一个空闲索引
 *   - Release: 归还索引
 *   - IsAllocated: 检查索引是否已分配
 *   - GetActiveCount: 已分配数量
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h,
 *          Core/Containers/TArray.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Containers/TArray.h"

namespace Limx
{

/// 整数索引分配器
class TIndexPool
{
    static constexpr UInt32 kInvalidIndex =
        static_cast<UInt32>(-1);

public:
    TIndexPool()
        : m_NextFresh(0)
        , m_ActiveCount(0)
    {
    }

    explicit TIndexPool(UInt32 initialReserve)
        : m_NextFresh(0)
        , m_ActiveCount(0)
    {
        m_FreeStack.Reserve(initialReserve);
    }

    // ========================================================================
    // 分配 / 归还
    // ========================================================================

    /// 分配一个空闲索引
    LIMX_NODISCARD UInt32 Acquire()
    {
        UInt32 index;
        if (m_FreeStack.GetSize() > 0)
        {
            index =
                m_FreeStack[m_FreeStack.GetSize() - 1];
            m_FreeStack.RemoveAt(
                m_FreeStack.GetSize() - 1);
        }
        else
        {
            index = m_NextFresh++;
        }

        ++m_ActiveCount;
        return index;
    }

    /// 归还索引
    void Release(UInt32 index)
    {
        LIMX_ASSERT(index < m_NextFresh);
        LIMX_ASSERT(m_ActiveCount > 0);
        m_FreeStack.Add(index);
        --m_ActiveCount;
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 是否为已分配的活跃索引
    LIMX_NODISCARD bool IsAllocated(UInt32 index) const
    {
        if (index >= m_NextFresh) return false;
        // 检查是否在空闲栈中
        for (SizeType freeIdx = 0;
             freeIdx < m_FreeStack.GetSize(); ++freeIdx)
        {
            if (m_FreeStack[freeIdx] == index)
                return false;
        }
        return true;
    }

    /// 当前已分配数量
    LIMX_NODISCARD UInt32 GetActiveCount() const
    {
        return m_ActiveCount;
    }

    /// 曾经分配的最大索引数 (含空闲)
    LIMX_NODISCARD UInt32 GetAllocatedRange() const
    {
        return m_NextFresh;
    }

    /// 空闲槽数
    LIMX_NODISCARD SizeType GetFreeCount() const
    {
        return m_FreeStack.GetSize();
    }

    /// 是否有空闲槽
    LIMX_NODISCARD bool HasFreeIndices() const
    {
        return !m_FreeStack.IsEmpty();
    }

    // ========================================================================
    // 重置
    // ========================================================================

    /// 完全重置 (所有分配失效)
    void Reset()
    {
        m_FreeStack.Clear();
        m_NextFresh  = 0;
        m_ActiveCount = 0;
    }

    /// 预热指定数量的索引 (预生成空闲池)
    void Prewarm(UInt32 count)
    {
        m_FreeStack.Reserve(count);
        for (UInt32 preIdx = count; preIdx > 0; --preIdx)
        {
            m_FreeStack.Add(m_NextFresh++);
        }
    }

private:
    TArray<UInt32> m_FreeStack;   ///< 空闲索引栈
    UInt32         m_NextFresh;   ///< 下一个全新索引
    UInt32         m_ActiveCount; ///< 已分配数量
};

} // namespace Limx
