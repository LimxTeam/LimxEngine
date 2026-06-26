/*******************************************************************************
 * 文件: TTaskGroup.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   任务组 — 多任务并发提交与等待机制
 *   将一批任务提交到 FJobSystem，等待所有任务完成后继续
 *   用于并行几何处理、物理步进、批量资源加载等场景
 *
 * 设计哲学:
 *   计数器屏障 — 原子计数器跟踪未完成任务数
 *   可复用 — Reset() 后可再次提交新批次任务
 *   非阻塞轮询 — IsComplete() 允许主线程轮询
 *
 * 技术特性:
 *   - TTaskGroup: 任务组
 *   - AddTask: 提交单个任务
 *   - Wait: 自旋等待全部完成
 *   - IsComplete: 非阻塞完成检测
 *   - Reset: 重置供下次使用
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h,
 *          Core/Threading/FAtomic.h, Core/Threading/FJobSystem.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Threading/FAtomic.h"
#include "Core/Templates/TFunction.h"

namespace Limx
{

// 前向声明
class FJobSystem;

/// 任务组 — 并发任务提交与等待
class TTaskGroup
{
public:
    using FTaskFunc = TFunction<void()>;

    TTaskGroup()
        : m_PendingCount(0)
        , m_JobSystem(nullptr)
    {
    }

    explicit TTaskGroup(FJobSystem* jobSystem)
        : m_PendingCount(0)
        , m_JobSystem(jobSystem)
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
        Int32 remaining = m_PendingCount.FetchSub(1);
        LIMX_ASSERT(remaining > 0);
    }

    // ========================================================================
    // 等待
    // ========================================================================

    /// 自旋等待所有任务完成
    void Wait()
    {
        while (m_PendingCount.Load() > 0)
        {
            // 自旋 — 在工程实践中可嵌入 Yield 或帮助执行任务
            AtomicOps::MemoryBarrier();
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

    // ========================================================================
    // 访问
    // ========================================================================

    void SetJobSystem(FJobSystem* jobSystem)
    {
        m_JobSystem = jobSystem;
    }

    LIMX_NODISCARD FJobSystem* GetJobSystem() const
    {
        return m_JobSystem;
    }

private:
    TAtomic<Int32> m_PendingCount;  ///< 未完成任务数
    FJobSystem*    m_JobSystem;     ///< 关联的任务系统
};

} // namespace Limx
