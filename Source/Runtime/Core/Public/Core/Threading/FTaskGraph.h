/*******************************************************************************
 * 文件: FTaskGraph.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   引擎任务系统 — 线程池 + 任务依赖图 + 异步分发
 *   提供高性能的并行任务调度，支持任务间依赖关系
 *   用于渲染管线、资产加载、物理模拟等并行工作负载
 *
 * 设计哲学:
 *   工作窃取 — 空闲线程从全局队列中获取任务
 *   引用计数依赖 — 前置任务完成时原子递减后继任务的等待计数
 *   Fire-and-forget — 提交后无需手动管理生命周期
 *   可等待 — 通过 FTaskHandle 等待特定任务完成
 *
 * 技术特性:
 *   - FTask: 任务基类 (可调用 + 依赖计数 + 后继列表)
 *   - FTaskHandle: 任务句柄 (等待/查询状态)
 *   - FTaskGraph: 全局任务管理器 (线程池 + 任务队列 + 分发)
 *   - 原子依赖计数: 无锁依赖解析
 *   - 工作线程: 自动创建 (硬件线程数 - 1)
 *
 * 依赖关系:
 *   内部: Core/Threading/FThread.h, Core/Threading/FAtomic.h,
 *          Core/Threading/FMutex.h, Core/Templates/TFunction.h,
 *          Core/Containers/TArray.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Threading/FAtomic.h"
#include "Core/Threading/FMutex.h"
#include "Core/Threading/FThread.h"
#include "Core/Templates/TFunction.h"
#include "Core/Containers/TArray.h"

namespace Limx
{

// 前向声明
class FTaskGraph;

// ============================================================================
// 任务状态
// ============================================================================

enum class TaskState : UInt8
{
    Pending    = 0,  ///< 等待前置任务完成
    Ready      = 1,  ///< 所有前置已完成，等待调度
    Running    = 2,  ///< 正在执行
    Completed  = 3   ///< 已完成
};

// ============================================================================
// FTask — 任务节点
// ============================================================================

/// 任务节点 — 包含可调用体 + 依赖关系
class FTask
{
public:
    /// @param owner 创建它的调度器 —— 后继任务就绪时要入回**同一个**队列
    explicit FTask(FTaskGraph* owner = nullptr)
        : m_State(static_cast<Int32>(TaskState::Pending))
        , m_WaitCount(0)
        , m_RefCount(1)
        , m_Owner(owner)
    {
    }

    ~FTask() = default;

    // 不可拷贝
    FTask(const FTask&) = delete;
    FTask& operator=(const FTask&) = delete;

    /// 设置任务体
    void SetWork(TFunction<void()>&& work)
    {
        m_Work = MoveTemp(work);
    }

    /// 添加后继任务 — 当前任务完成后通知后继
    void AddSuccessor(FTask* successor)
    {
        FScopeLock lock(m_SuccessorMutex);
        m_Successors.Add(successor);
        // 后继任务的等待计数 +1
        successor->m_WaitCount.FetchAdd(1);
    }

    /// 执行任务
    void Execute()
    {
        m_State.Store(static_cast<Int32>(TaskState::Running));
        if (m_Work)
        {
            m_Work();
        }
        m_State.Store(static_cast<Int32>(TaskState::Completed));

        // 通知所有后继任务
        NotifySuccessors();
    }

    /// 获取当前状态
    LIMX_NODISCARD TaskState GetState() const
    {
        return static_cast<TaskState>(m_State.Load());
    }

    /// 是否已完成
    LIMX_NODISCARD bool IsCompleted() const
    {
        return GetState() == TaskState::Completed;
    }

    /// 引用计数管理
    void AddRef()
    {
        m_RefCount.Increment();
    }

    void Release()
    {
        if (m_RefCount.Decrement() == 0)
        {
            this->~FTask();
            GetDefaultAllocator().Deallocate(this);
        }
    }

    /// 尝试标记为就绪 — 当等待计数为 0 时返回 true
    LIMX_NODISCARD bool TrySetReady()
    {
        if (m_WaitCount.Load() == 0)
        {
            Int32 expected = static_cast<Int32>(TaskState::Pending);
            return m_State.CompareExchange(
                expected, static_cast<Int32>(TaskState::Ready));
        }
        return false;
    }

private:
    /// 通知后继任务 — 递减等待计数，若归零则入队
    void NotifySuccessors();

    TFunction<void()>  m_Work;            ///< 任务体
    TAtomic<Int32>     m_State;           ///< 任务状态
    TAtomic<Int32>     m_WaitCount;       ///< 等待的前置任务数
    TAtomic<Int32>     m_RefCount;        ///< 引用计数
    TArray<FTask*>     m_Successors;      ///< 后继任务列表
    FMutex             m_SuccessorMutex;  ///< 后继列表保护锁

    /// 所属调度器
    ///
    /// 必须逐任务记住, 不能在 NotifySuccessors 里取全局单例 —— 那样一来,
    /// 任何非单例的 FTaskGraph 实例, 它的后继任务都会被投递到全局那一个
    /// 队列里去。全局实例若未初始化, 这些任务就永远不会被执行, 而调用方
    /// 只会看到 WaitForAll 一直等不到结果。
    FTaskGraph*        m_Owner;
};

// ============================================================================
// FTaskHandle — 任务句柄
// ============================================================================

/// 任务句柄 — 用于等待和查询任务状态
class FTaskHandle
{
public:
    FTaskHandle() : m_Task(nullptr) {}

    explicit FTaskHandle(FTask* task)
        : m_Task(task)
    {
        if (m_Task)
        {
            m_Task->AddRef();
        }
    }

    FTaskHandle(const FTaskHandle& other)
        : m_Task(other.m_Task)
    {
        if (m_Task)
        {
            m_Task->AddRef();
        }
    }

    FTaskHandle(FTaskHandle&& other) noexcept
        : m_Task(other.m_Task)
    {
        other.m_Task = nullptr;
    }

    ~FTaskHandle()
    {
        if (m_Task)
        {
            m_Task->Release();
        }
    }

    FTaskHandle& operator=(const FTaskHandle& other)
    {
        if (this != &other)
        {
            if (m_Task)
            {
                m_Task->Release();
            }
            m_Task = other.m_Task;
            if (m_Task)
            {
                m_Task->AddRef();
            }
        }
        return *this;
    }

    FTaskHandle& operator=(FTaskHandle&& other) noexcept
    {
        if (this != &other)
        {
            if (m_Task)
            {
                m_Task->Release();
            }
            m_Task = other.m_Task;
            other.m_Task = nullptr;
        }
        return *this;
    }

    /// 是否有效
    LIMX_NODISCARD bool IsValid() const { return m_Task != nullptr; }

    /// 是否已完成
    LIMX_NODISCARD bool IsCompleted() const
    {
        return m_Task && m_Task->IsCompleted();
    }

    /// 忙等待完成 (让出时间片)
    void Wait() const
    {
        if (m_Task)
        {
            while (!m_Task->IsCompleted())
            {
                FThread::Yield();
            }
        }
    }

    /// 获取内部任务指针 (不增加引用)
    LIMX_NODISCARD FTask* GetTask() const { return m_Task; }

private:
    FTask* m_Task;
};

// ============================================================================
// FTaskGraph — 全局任务调度器
// ============================================================================

/// 任务调度器 — 线程池 + 任务队列
class FTaskGraph
{
    /// 任务队列节点
    struct QueueEntry
    {
        FTask* Task;
    };

public:
    // ========================================================================
    // 生命周期
    // ========================================================================

    FTaskGraph()
        : m_IsRunning(false)
        , m_WorkerCount(0)
    {
    }

    ~FTaskGraph()
    {
        Shutdown();
    }

    // 不可拷贝/移动
    FTaskGraph(const FTaskGraph&) = delete;
    FTaskGraph& operator=(const FTaskGraph&) = delete;

    /// 初始化 — 启动工作线程
    /// @param workerCount 工作线程数 (0 = 自动, 硬件线程数 - 1)
    void Initialize(UInt32 workerCount = 0)
    {
        if (m_IsRunning.Load())
        {
            return;
        }

        if (workerCount == 0)
        {
            // 留一个核给主线程 —— 工作线程占满全部硬件线程时, 主线程的
            // 提交与等待会和它们争抢, 反而拖慢整体。
            const UInt32 hardware = FThread::HardwareConcurrency();

            workerCount = (hardware > 1) ? (hardware - 1) : 1;
        }

        m_IsRunning.Store(true);
        m_WorkerCount = workerCount;

        m_Workers.Reserve(workerCount);
        for (UInt32 index = 0; index < workerCount; ++index)
        {
            m_Workers.Add(FThread(
                TFunction<void()>([this]() { WorkerLoop(); })));
        }
    }

    /// 关闭 — 等待所有任务完成并停止工作线程
    void Shutdown()
    {
        if (!m_IsRunning.Load())
        {
            return;
        }

        // 先排空再停。原先直接置停止标志, 队列里没跑完的任务会被整批丢弃 ——
        // 它们持有的调度器引用也就永远不会释放, 那是一次确定的内存泄漏。
        // 而函数的文档一直写的是"等待所有任务完成"。
        WaitForAll();

        m_IsRunning.Store(false);

        // 唤醒所有工作线程
        m_WakeCondition.NotifyAll();

        // 等待所有工作线程结束
        for (SizeType index = 0; index < m_Workers.GetSize(); ++index)
        {
            if (m_Workers[index].IsJoinable())
            {
                m_Workers[index].Join();
            }
        }
        m_Workers.Clear();
    }

    // ========================================================================
    // 任务创建与提交
    // ========================================================================

    /// 创建任务 (不立即提交)
    LIMX_NODISCARD FTaskHandle CreateTask(TFunction<void()>&& work)
    {
        void* memory = GetDefaultAllocator().Allocate(
            sizeof(FTask), alignof(FTask));
        FTask* task = new (memory) FTask(this);
        task->SetWork(MoveTemp(work));
        return FTaskHandle(task);
    }

    /// 添加任务依赖 — prerequisite 完成后才能执行 dependent
    void AddDependency(const FTaskHandle& prerequisite,
                       const FTaskHandle& dependent)
    {
        LIMX_ASSERT(prerequisite.IsValid() && dependent.IsValid());
        prerequisite.GetTask()->AddSuccessor(dependent.GetTask());
    }

    /// 提交任务到调度器
    void Submit(const FTaskHandle& handle)
    {
        LIMX_ASSERT(handle.IsValid());
        FTask* task = handle.GetTask();
        task->AddRef();  // 调度器持有引用

        if (task->TrySetReady())
        {
            EnqueueReady(task);
        }
        // 如果不 ready，等前置任务 NotifySuccessors 时入队
    }

    /// 便捷方法 — 创建并立即提交无依赖任务
    LIMX_NODISCARD FTaskHandle Dispatch(TFunction<void()>&& work)
    {
        FTaskHandle handle = CreateTask(MoveTemp(work));
        Submit(handle);
        return handle;
    }

    /// 等待所有已提交任务完成 (含它们派生出的后继)
    ///
    /// 判据只有一个: 待办数归零。它在入队时加、执行完后减, 因此"排队中"
    /// 与"执行中"都被同一个计数覆盖, 两者之间不存在空隙。
    void WaitForAll()
    {
        while (m_PendingTaskCount.Load() > 0)
        {
            FThread::Yield();
        }
    }

    /// 当前尚未完成的任务数 (排队中 + 执行中)
    LIMX_NODISCARD Int32 GetPendingTaskCount() const
    {
        return m_PendingTaskCount.Load();
    }

    // ========================================================================
    // 获取全局实例
    // ========================================================================

    static FTaskGraph& Get()
    {
        static FTaskGraph s_Instance;
        return s_Instance;
    }

private:
    friend class FTask;

    // ========================================================================
    // 工作线程循环
    // ========================================================================

    void WorkerLoop()
    {
        while (m_IsRunning.Load())
        {
            FTask* task = DequeueReady();
            if (task)
            {
                m_ActiveTaskCount.FetchAdd(1);
                task->Execute();
                m_ActiveTaskCount.FetchSub(1);
                task->Release();  // 释放调度器引用

                // 减在最后: Execute 内部可能通过 NotifySuccessors 再入队
                // 若干后继, 那些任务的待办数已经在入队时加过了。先减再执行
                // 会让计数在两者之间短暂归零。
                m_PendingTaskCount.FetchSub(1);
            }
            else
            {
                // 无任务 — 等待唤醒
                FScopeLock lock(m_WakeMutex);
                if (m_IsRunning.Load())
                {
                    (void)m_WakeCondition.WaitFor(m_WakeMutex, 5);
                }
            }
        }
    }

    /// 将就绪任务入队
    void EnqueueReady(FTask* task)
    {
        // 待办数在**入队之前**加, 在执行完之后减 —— 这样它在任务的整个
        // 生命周期里都不为零, 中间没有任何窗口。
        //
        // 原先 WaitForAll 是"先看队列空、再看执行中为零", 这两个条件之间
        // 存在一个真实的空隙: 工作线程刚把任务取出队列、还没来得及给
        // 执行中计数加一。那一瞬间两个条件同时成立, WaitForAll 就会在任务
        // 还没跑的情况下返回。这种竞态在轻负载下几乎不出现, 一旦出现表现
        // 为"偶尔少算了一个任务", 极难复现。
        m_PendingTaskCount.FetchAdd(1);

        {
            FScopeLock lock(m_QueueMutex);
            m_ReadyQueue.Add(task);
        }
        m_WakeCondition.NotifyOne();
    }

    /// 从就绪队列取出一个任务
    FTask* DequeueReady()
    {
        FScopeLock lock(m_QueueMutex);
        if (m_ReadyQueue.IsEmpty())
        {
            return nullptr;
        }
        FTask* task = m_ReadyQueue[m_ReadyQueue.GetSize() - 1];
        m_ReadyQueue.RemoveAt(m_ReadyQueue.GetSize() - 1);
        return task;
    }

    // ========================================================================
    // 成员数据
    // ========================================================================

    TArray<FThread>    m_Workers;          ///< 工作线程池
    TArray<FTask*>     m_ReadyQueue;       ///< 就绪任务队列
    FMutex             m_QueueMutex;       ///< 队列保护锁
    FMutex             m_WakeMutex;        ///< 条件变量配套锁
    FConditionVariable m_WakeCondition;    ///< 工作线程唤醒条件
    TAtomic<bool>      m_IsRunning;        ///< 运行标志
    TAtomic<Int32>     m_ActiveTaskCount;  ///< 当前执行中的任务数

    /// 尚未完成的任务数 —— 排队中与执行中都算
    TAtomic<Int32>     m_PendingTaskCount;
    UInt32             m_WorkerCount;      ///< 工作线程数量
};

// ============================================================================
// FTask 内联实现 (依赖 FTaskGraph)
// ============================================================================

inline void FTask::NotifySuccessors()
{
    FScopeLock lock(m_SuccessorMutex);
    for (SizeType index = 0; index < m_Successors.GetSize(); ++index)
    {
        FTask* successor = m_Successors[index];
        // 原子递减等待计数
        Int32 remaining = successor->m_WaitCount.FetchSub(1) - 1;
        if (remaining == 0)
        {
            // 所有前置完成 — 标记就绪并入队
            Int32 expected = static_cast<Int32>(TaskState::Pending);
            bool wasSet = successor->m_State.CompareExchange(
                expected, static_cast<Int32>(TaskState::Ready));

            // 入回**它自己**那个调度器, 而不是全局单例
            if (wasSet && successor->m_Owner != nullptr)
            {
                successor->m_Owner->EnqueueReady(successor);
            }
        }
    }
}

} // namespace Limx
