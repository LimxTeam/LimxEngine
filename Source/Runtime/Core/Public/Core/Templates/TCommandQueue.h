/*******************************************************************************
 * 文件: TCommandQueue.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   命令队列 — 延迟执行的命令缓冲区
 *   将操作封装为命令对象，按序列排队后统一执行
 *   用于渲染命令录制、撤销/重做系统、延迟任务执行等场景
 *
 * 设计哲学:
 *   类型擦除 — 通过 TFunction 封装任意可调用命令
 *   批量执行 — 累积命令后一次性 Flush 执行
 *   可清空 — 支持丢弃所有待执行命令
 *
 * 技术特性:
 *   - TCommandQueue: 延迟执行命令队列
 *   - Enqueue: 入队命令
 *   - Flush: 执行所有待执行命令
 *   - Clear: 丢弃所有待执行命令
 *   - GetPendingCount: 待执行命令数
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Templates/TFunction.h,
 *          Core/Containers/TArray.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Templates/TFunction.h"
#include "Core/Containers/TArray.h"

namespace Limx
{

/// 命令队列
class TCommandQueue
{
public:
    TCommandQueue() = default;
    ~TCommandQueue() = default;

    // 不可拷贝
    TCommandQueue(const TCommandQueue&) = delete;
    TCommandQueue& operator=(const TCommandQueue&) = delete;

    // 可移动
    TCommandQueue(TCommandQueue&& other) noexcept
        : m_Commands(MoveTemp(other.m_Commands))
    {
    }

    TCommandQueue& operator=(TCommandQueue&& other) noexcept
    {
        if (this != &other)
        {
            m_Commands = MoveTemp(other.m_Commands);
        }
        return *this;
    }

    // ========================================================================
    // 入队
    // ========================================================================

    /// 入队命令
    void Enqueue(TFunction<void()> command)
    {
        m_Commands.Add(MoveTemp(command));
    }

    // ========================================================================
    // 执行
    // ========================================================================

    /// 执行所有待执行命令并清空队列
    void Flush()
    {
        // 先移走命令列表，防止 Flush 期间入队导致迭代问题
        TArray<TFunction<void()>> commands = MoveTemp(m_Commands);
        m_Commands.Clear();

        for (SizeType commandIndex = 0;
             commandIndex < commands.GetSize(); ++commandIndex)
        {
            commands[commandIndex]();
        }
    }

    /// 执行最多 maxCount 条命令
    /// @return 实际执行的命令数
    SizeType FlushPartial(SizeType maxCount)
    {
        SizeType executeCount = maxCount;
        if (executeCount > m_Commands.GetSize())
        {
            executeCount = m_Commands.GetSize();
        }

        for (SizeType commandIndex = 0;
             commandIndex < executeCount; ++commandIndex)
        {
            m_Commands[commandIndex]();
        }

        // 移除已执行的命令 (从前往后)
        if (executeCount > 0 &&
            executeCount < m_Commands.GetSize())
        {
            // 将剩余命令前移
            SizeType remaining =
                m_Commands.GetSize() - executeCount;
            TArray<TFunction<void()>> leftover;
            leftover.Reserve(remaining);
            for (SizeType moveIndex = executeCount;
                 moveIndex < m_Commands.GetSize(); ++moveIndex)
            {
                leftover.Add(MoveTemp(m_Commands[moveIndex]));
            }
            m_Commands = MoveTemp(leftover);
        }
        else if (executeCount == m_Commands.GetSize())
        {
            m_Commands.Clear();
        }

        return executeCount;
    }

    // ========================================================================
    // 管理
    // ========================================================================

    /// 丢弃所有待执行命令
    void Clear()
    {
        m_Commands.Clear();
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 待执行命令数
    LIMX_NODISCARD SizeType GetPendingCount() const
    {
        return m_Commands.GetSize();
    }

    /// 是否为空
    LIMX_NODISCARD bool IsEmpty() const
    {
        return m_Commands.GetSize() == 0;
    }

private:
    TArray<TFunction<void()>> m_Commands;  ///< 命令列表
};

} // namespace Limx
