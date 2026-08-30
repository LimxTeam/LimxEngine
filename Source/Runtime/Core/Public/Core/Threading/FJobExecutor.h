/*******************************************************************************
 * 文件: FJobExecutor.h
 * 创建时间: 2026-08-31
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   作业执行器 — 把 FJobBatch 投递到 FTaskGraph 上真正跑起来
 *
 * 设计哲学:
 *   建在 FTaskGraph 之上, 而不是再写一个线程池。引擎里只该有一个调度器 ——
 *   两个线程池会各自按硬件线程数开工作线程, 于是核心被超额认领, 两边的
 *   任务互相抢占, 总吞吐反而低于单个池子。
 *
 *   而且 FTaskGraph 已经被测过了: 工作线程、就绪队列、依赖计数、
 *   WaitForAll 的待办计数, 都有回归用例钉着。再写一个等于把那些坑
 *   重新踩一遍。
 *
 *   `FJobSystem.h` 提供的是作业的**描述** (计数器、作业、批次、并行 For
 *   的切分), 本身不含执行器。这个分层是刻意的: 描述可以在没有调度器的
 *   环境里构造与测试 (那些用例全是确定性的, 不涉线程), 执行则单独一层。
 *
 * 技术特性:
 *   - Dispatch 非阻塞, DispatchAndWait 只等本批次而非整张图
 *   - 完成计数器与调用方自己的计数器互不干扰
 *   - 逐作业断言依赖已满足 —— 作业间依赖属于 FTaskGraph 的任务边
 *
 * 依赖关系:
 *   内部: Core/Threading/FJobSystem.h, Core/Threading/FTaskGraph.h
 *
 * 注意事项:
 *   不要在工作线程内部调用 DispatchAndWait/ParallelFor —— 那会让该线程
 *   阻塞在等待上, 若所有工作线程都这么做, 整张图就再没人推进了。
 *
 ******************************************************************************/

#pragma once

#include "Core/Threading/FJobSystem.h"
#include "Core/Threading/FTaskGraph.h"

namespace Limx
{

// ============================================================================
// FJobExecutor — 作业批次的执行入口
// ============================================================================

class FJobExecutor
{
public:
    FJobExecutor()                               = delete;
    ~FJobExecutor()                              = delete;
    FJobExecutor(const FJobExecutor&)            = delete;
    FJobExecutor& operator=(const FJobExecutor&) = delete;

    /// 把一个批次投递到任务图上
    ///
    /// 返回后批次里的作业**已全部提交**, 但未必已完成。等待方式有两种:
    /// 给批次设一个完成计数器自己轮询, 或者改用 DispatchAndWait。
    ///
    /// 批次里的作业必须都已就绪。作业间的依赖不由这一层表达 ——
    /// `FJob::DependsOn` 是一个计数器, 而计数器变为零时没有任何人会被
    /// 唤醒; 真正的依赖边在 FTaskGraph 上 (AddDependency)。需要"A 批完了
    /// 再跑 B 批"时, 顺序调用 DispatchAndWait(A) 与 Dispatch(B) 即可 ——
    /// 那正是 FJobBatch::SetDependency 的粒度 (它给整批设同一个计数器)。
    static void Dispatch(FTaskGraph& graph, FJobBatch& batch)
    {
        DispatchInternal(graph, batch, nullptr);
    }

    /// 投递并等待**本批次**完成
    ///
    /// 只等这一批, 不等整张图 —— 图上可能还有别处提交的任务, 用
    /// FTaskGraph::WaitForAll 会把它们也一并等上, 那不是调用方的本意。
    static void DispatchAndWait(FTaskGraph& graph, FJobBatch& batch)
    {
        FJobCounter completion(static_cast<Int32>(batch.GetCount()));

        DispatchInternal(graph, batch, &completion);

        while (!completion.IsComplete())
        {
            FThread::Yield();
        }
    }

    /// 并行 For —— 建批、投递、等待
    ///
    /// @param count     总迭代次数
    /// @param batchSize 每个作业处理的迭代数; 0 会被兜底成 1
    /// @param body      迭代体 (参数: 起始索引, 结束索引)
    ///
    /// 批大小要按单次迭代的成本来定: 迭代体很轻时, 一个作业只做一次迭代
    /// 会让调度开销盖过收益 (每个作业都要一次堆分配、一次入队、一次唤醒);
    /// 迭代体很重时 (比如解一张 JPEG), 批大小取 1 反而最好 —— 各次耗时
    /// 差异大, 细粒度才能让先做完的线程接着领下一个。
    static void ParallelFor(
        FTaskGraph& graph,
        SizeType count,
        SizeType batchSize,
        const TFunction<void(SizeType, SizeType)>& body)
    {
        if (count == 0)
        {
            return;
        }

        FJobBatch batch = FParallelFor::Create(count, batchSize, body);

        DispatchAndWait(graph, batch);
    }

private:
    /// @param extra 额外的完成计数器 —— 与作业自带的那个并行递减,
    ///              两者互不干扰
    static void DispatchInternal(FTaskGraph& graph,
                                 FJobBatch&  batch,
                                 FJobCounter* extra)
    {
        TArray<FJob>& jobs = batch.GetJobs();

        for (SizeType index = 0; index < jobs.GetSize(); ++index)
        {
            FJob& job = jobs[index];

            LIMX_ASSERT(job.IsReady());

            // 按值捕获 —— 批次可能在作业跑完之前就被销毁
            TFunction<void()> entryPoint = job.EntryPoint;
            FJobCounter*      signal     = job.SignalOnComplete;

            (void)graph.Dispatch(
                TFunction<void()>(
                    [entryPoint, signal, extra]()
                    {
                        if (entryPoint)
                        {
                            entryPoint();
                        }

                        // 递减放在最后: 计数归零即代表"活已经干完了",
                        // 等待方看到零之后会立刻去读结果
                        if (signal != nullptr)
                        {
                            (void)signal->Decrement();
                        }

                        if (extra != nullptr)
                        {
                            (void)extra->Decrement();
                        }
                    }));
        }
    }
};

} // namespace Limx
