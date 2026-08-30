/*******************************************************************************
 * 文件: TTaskGroup.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   任务组 — 一个"我提交了 N 件事, 告诉我什么时候都完了"的计数屏障
 *
 * 设计哲学:
 *   只是一个屏障, 不是调度器。它不持有线程、不决定谁在哪跑 —— 任务投给谁
 *   由调用方决定 (通常是 FTaskGraph)。这样它既能配合任务图, 也能用在
 *   "自己开了几个线程"的临时场合。
 *
 *   原先它持有一个 `FJobSystem*`。而 `class FJobSystem` 全项目只存在于
 *   本文件的一句前向声明里 —— 那个指针永远指不到任何东西, 两个访问器
 *   也就永远只是在搬运空指针。已移除。
 *
 * 技术特性:
 *   - BeginTask/NotifyComplete: 手动配对的计数
 *   - Wait: 让出时间片地等待, 而非空转
 *   - IsComplete: 非阻塞完成检测
 *   - Reset: 重置供下次使用
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h,
 *          Core/Threading/FAtomic.h, Core/Threading/FThread.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Threading/FAtomic.h"
#include "Core/Threading/FThread.h"
#include "Core/Templates/TFunction.h"

namespace Limx
{

/// 任务组 — 并发任务提交与等待
class TTaskGroup
{
public:
    using FTaskFunc = TFunction<void()>;

    TTaskGroup()
        : m_PendingCount(0)
    {
    }

    // 不可拷贝
    TTaskGroup(const TTaskGroup&) = delete;
    TTaskGroup& operator=(const TTaskGroup&) = delete;

    // ========================================================================
    // 任务提交
    // ========================================================================

    /// 手动提交任务 (调用者负责调用 NotifyComplete)
    void BeginTask()
    {
        m_PendingCount.FetchAdd(1);
    }

    /// 通知一个任务完成
    void NotifyComplete()
    {
        // FetchSub 返回的是**递减前**的值, 因此"递减前必须为正"才说明
        // 这次递减是有配对的。判成 >= 0 会放过一次多余的通知, 而那会让
        // 计数变负, 之后的 Wait 永远等不到零。
        const Int32 before = m_PendingCount.FetchSub(1);

        LIMX_ASSERT(before > 0);

        static_cast<void>(before);
    }

    // ========================================================================
    // 等待
    // ========================================================================

    /// 等待所有任务完成
    ///
    /// 让出时间片而非空转。原先是纯自旋加一条内存屏障, 注释里写着
    /// "在工程实践中可嵌入 Yield" —— 而那正是必须做的事: 等待方占满一个
    /// 核心空转时, 它等的那些任务少了一个可用核心, 于是等得更久。
    /// 工作线程数按硬件线程数减一来定, 这一个核心的差别是实打实的。
    void Wait()
    {
        while (m_PendingCount.Load() > 0)
        {
            FThread::Yield();
        }
    }

    /// 非阻塞完成检测
    LIMX_NODISCARD bool IsComplete() const
    {
        return m_PendingCount.Load() == 0;
    }

    /// 获取待完成任务数
    LIMX_NODISCARD Int32 GetPendingCount() const
    {
        return m_PendingCount.Load();
    }

    // ========================================================================
    // 重置
    // ========================================================================

    /// 重置供下次使用 (须在 IsComplete() 为真后调用)
    void Reset()
    {
        LIMX_ASSERT(IsComplete());
        m_PendingCount.Store(0);
    }

private:
    TAtomic<Int32> m_PendingCount;  ///< 未完成任务数
};

} // namespace Limx
