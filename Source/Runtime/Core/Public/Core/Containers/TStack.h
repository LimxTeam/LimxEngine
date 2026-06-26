/*******************************************************************************
 * 文件: TStack.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   栈 — 后进先出 (LIFO) 容器
 *   基于 TArray 实现，提供 Push/Pop/Top 语义
 *   用于表达式求值、撤销栈、DFS 遍历、状态保存恢复等场景
 *
 * 设计哲学:
 *   TArray 委托 — 底层使用动态数组，连续存储，缓存友好
 *   LIFO 语义 — 仅暴露栈顶操作
 *   简洁接口 — Push/Pop/Top/IsEmpty/GetCount
 *
 * 技术特性:
 *   - TStack<T>: LIFO 栈
 *   - Push: 入栈
 *   - Pop: 出栈
 *   - Top: 查看栈顶
 *   - GetCount/IsEmpty: 查询
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Containers/TArray.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Containers/TArray.h"

namespace Limx
{

/// LIFO 栈
/// @tparam T 元素类型
template<typename T>
class TStack
{
public:
    TStack() = default;

    explicit TStack(SizeType reserveCapacity)
    {
        m_Data.Reserve(reserveCapacity);
    }

    // ========================================================================
    // 入栈/出栈
    // ========================================================================

    /// 入栈 (拷贝)
    void Push(const T& element)
    {
        m_Data.Add(element);
    }

    /// 入栈 (移动)
    void Push(T&& element)
    {
        m_Data.Add(MoveTemp(element));
    }

    /// 出栈 (移除栈顶元素)
    void Pop()
    {
        LIMX_ASSERT(m_Data.GetSize() > 0);
        m_Data.RemoveAt(m_Data.GetSize() - 1);
    }

    /// 出栈并返回栈顶元素
    T PopValue()
    {
        LIMX_ASSERT(m_Data.GetSize() > 0);
        SizeType lastIdx = m_Data.GetSize() - 1;
        T value = MoveTemp(m_Data[lastIdx]);
        m_Data.RemoveAt(lastIdx);
        return value;
    }

    // ========================================================================
    // 访问
    // ========================================================================

    /// 栈顶元素 (只读)
    LIMX_NODISCARD const T& Top() const
    {
        LIMX_ASSERT(m_Data.GetSize() > 0);
        return m_Data[m_Data.GetSize() - 1];
    }

    /// 栈顶元素 (可写)
    LIMX_NODISCARD T& Top()
    {
        LIMX_ASSERT(m_Data.GetSize() > 0);
        return m_Data[m_Data.GetSize() - 1];
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 元素数量
    LIMX_NODISCARD SizeType GetCount() const
    {
        return m_Data.GetSize();
    }

    /// 是否为空
    LIMX_NODISCARD bool IsEmpty() const
    {
        return m_Data.GetSize() == 0;
    }

    /// 清空
    void Clear() { m_Data.Clear(); }

    /// 预分配
    void Reserve(SizeType capacity)
    {
        m_Data.Reserve(capacity);
    }

private:
    TArray<T> m_Data;  ///< 底层动态数组
};

} // namespace Limx
