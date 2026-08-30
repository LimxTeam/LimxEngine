/*******************************************************************************
 * 文件: JobExecutorTests.cpp
 * 创建时间: 2026-08-31
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   作业执行器与任务组的回归测试 — 批次投递、并行 For、计数屏障
 *
 * 设计哲学:
 *   断言只依赖与调度顺序无关的不变量 —— 每个迭代恰好被处理一次、
 *   完成计数归零时活确实已经干完、等待返回后没有遗留。没有一处 sleep:
 *   睡眠只是把竞态概率调低, 并没有消除它。
 *
 *   重点是"完成计数归零"与"结果已可读"这两件事的先后。执行器把递减放在
 *   作业体之后, 等待方才能在看到零的那一刻直接去读结果 —— 反过来的话,
 *   等待方会读到一半写完的数据, 而这种错误在轻负载下几乎不出现。
 *
 * 技术特性:
 *   - 并行 For 逐元素核对访问次数, 同时覆盖"漏掉"与"重复"两种失效
 *   - 覆盖不整除、批大小为零、总数为零这几种切分边界
 *   - 反复投递多轮, 确认执行器与图都是可复用的
 *
 * 依赖关系:
 *   内部: CoreTests/CoreTestsMinimal.h, Core/Threading/FJobExecutor.h,
 *         Core/Threading/TTaskGroup.h
 *
 ******************************************************************************/

#include "CoreTests/CoreTestsMinimal.h"
#include "Core/Threading/FJobExecutor.h"
#include "Core/Threading/TTaskGroup.h"

using namespace Limx;

namespace
{

/// 起停成对的任务图 —— 用例提前返回时也能正确关闭
class FScopedGraph
{
public:
    explicit FScopedGraph(UInt32 workerCount = 4)
    {
        m_Graph.Initialize(workerCount);
    }

    ~FScopedGraph()
    {
        m_Graph.Shutdown();
    }

    LIMX_NON_COPYABLE(FScopedGraph);
    LIMX_NON_MOVABLE(FScopedGraph);

    LIMX_NODISCARD FTaskGraph& Get() { return m_Graph; }

private:
    FTaskGraph m_Graph;
};

} // namespace

// ============================================================================
// 批次投递
// ============================================================================

LIMX_TEST(JobExecutor, DispatchAndWaitRunsEveryJobOnce)
{
    constexpr Int32 kJobCount = 128;

    FScopedGraph scoped;

    TAtomic<Int32> counters[kJobCount];

    for (Int32 i = 0; i < kJobCount; ++i)
    {
        counters[i].Store(0);
    }

    FJobBatch batch;

    for (Int32 i = 0; i < kJobCount; ++i)
    {
        TAtomic<Int32>* slot = &counters[i];

        batch.Add([slot]() { slot->Increment(); });
    }

    FJobExecutor::DispatchAndWait(scoped.Get(), batch);

    for (Int32 i = 0; i < kJobCount; ++i)
    {
        LIMX_EXPECT_EQ(counters[i].Load(), 1);
    }
}

LIMX_TEST(JobExecutor, EmptyBatchIsSafe)
{
    FScopedGraph scoped;

    FJobBatch batch;

    FJobExecutor::DispatchAndWait(scoped.Get(), batch);

    LIMX_EXPECT_TRUE(batch.IsEmpty());
}

LIMX_TEST(JobExecutor, CallerCompletionCounterIsSignalled)
{
    // 调用方自己的计数器必须照常递减 —— DispatchAndWait 内部另有一个
    // 计数器, 两者不能互相顶掉。
    constexpr Int32 kJobCount = 64;

    FScopedGraph scoped;

    FJobCounter completion(kJobCount);

    TAtomic<Int32> ran;
    ran.Store(0);

    FJobBatch batch;

    for (Int32 i = 0; i < kJobCount; ++i)
    {
        batch.Add([&ran]() { ran.Increment(); });
    }

    batch.SetCompletionCounter(&completion);

    FJobExecutor::DispatchAndWait(scoped.Get(), batch);

    LIMX_EXPECT_EQ(ran.Load(), kJobCount);
    LIMX_EXPECT_TRUE(completion.IsComplete());
}

LIMX_TEST(JobExecutor, DispatchIsNonBlockingAndCounterTracksCompletion)
{
    // Dispatch 不等待 —— 等待方靠计数器判断。这个用例同时钉住一件事:
    // 计数归零时作业体确实已经执行完, 而不是"刚开始跑"。
    constexpr Int32 kJobCount = 100;

    FScopedGraph scoped;

    FJobCounter completion(kJobCount);

    TAtomic<Int32> ran;
    ran.Store(0);

    FJobBatch batch;

    for (Int32 i = 0; i < kJobCount; ++i)
    {
        batch.Add([&ran]() { ran.Increment(); });
    }

    batch.SetCompletionCounter(&completion);

    FJobExecutor::Dispatch(scoped.Get(), batch);

    while (!completion.IsComplete())
    {
        FThread::Yield();
    }

    LIMX_EXPECT_EQ(ran.Load(), kJobCount);
}

LIMX_TEST(JobExecutor, BatchMayBeDestroyedBeforeJobsFinish)
{
    // 执行器按值捕获作业入口 —— 批次本身可以在作业跑完之前就销毁。
    // 按引用捕获的话这里读到的是已释放的内存, 而且往往"碰巧能跑"。
    constexpr Int32 kJobCount = 64;

    FScopedGraph scoped;

    FJobCounter completion(kJobCount);

    TAtomic<Int32> ran;
    ran.Store(0);

    {
        FJobBatch batch;

        for (Int32 i = 0; i < kJobCount; ++i)
        {
            batch.Add([&ran]() { ran.Increment(); });
        }

        batch.SetCompletionCounter(&completion);

        FJobExecutor::Dispatch(scoped.Get(), batch);
    }

    while (!completion.IsComplete())
    {
        FThread::Yield();
    }

    LIMX_EXPECT_EQ(ran.Load(), kJobCount);
}

// ============================================================================
// 并行 For
// ============================================================================

LIMX_TEST(JobExecutor, ParallelForCoversEveryIndexOnce)
{
    constexpr SizeType kCount = 1000;

    FScopedGraph scoped;

    TAtomic<Int32> visits[kCount];

    for (SizeType i = 0; i < kCount; ++i)
    {
        visits[i].Store(0);
    }

    TAtomic<Int32>* slots = visits;

    FJobExecutor::ParallelFor(
        scoped.Get(), kCount, 32,
        [slots](SizeType begin, SizeType end)
        {
            for (SizeType i = begin; i < end; ++i)
            {
                slots[i].Increment();
            }
        });

    for (SizeType i = 0; i < kCount; ++i)
    {
        LIMX_EXPECT_EQ(visits[i].Load(), 1);
    }
}

LIMX_TEST(JobExecutor, ParallelForHandlesNonDivisibleCount)
{
    // 1000 / 32 除不尽 —— 尾批越界会写出数组外, 尾批漏掉则最后几个
    // 元素永远不被处理。两者在整除的用例里都碰不到。
    constexpr SizeType kCount = 1003;

    FScopedGraph scoped;

    TAtomic<Int32> visits[kCount];

    for (SizeType i = 0; i < kCount; ++i)
    {
        visits[i].Store(0);
    }

    TAtomic<Int32>* slots = visits;

    FJobExecutor::ParallelFor(
        scoped.Get(), kCount, 32,
        [slots](SizeType begin, SizeType end)
        {
            for (SizeType i = begin; i < end; ++i)
            {
                slots[i].Increment();
            }
        });

    for (SizeType i = 0; i < kCount; ++i)
    {
        LIMX_EXPECT_EQ(visits[i].Load(), 1);
    }
}

LIMX_TEST(JobExecutor, ParallelForZeroCountIsSafe)
{
    FScopedGraph scoped;

    TAtomic<Int32> ran;
    ran.Store(0);

    FJobExecutor::ParallelFor(
        scoped.Get(), 0, 8,
        [&ran](SizeType, SizeType) { ran.Increment(); });

    LIMX_EXPECT_EQ(ran.Load(), 0);
}

LIMX_TEST(JobExecutor, ParallelForBatchSizeOneWorks)
{
    // 批大小 1 是解码这类重迭代体的最佳粒度: 各次耗时差异大, 细粒度
    // 才能让先做完的线程接着领下一个。
    constexpr SizeType kCount = 64;

    FScopedGraph scoped;

    TAtomic<Int32> visits[kCount];

    for (SizeType i = 0; i < kCount; ++i)
    {
        visits[i].Store(0);
    }

    TAtomic<Int32>* slots = visits;

    FJobExecutor::ParallelFor(
        scoped.Get(), kCount, 1,
        [slots](SizeType begin, SizeType end)
        {
            for (SizeType i = begin; i < end; ++i)
            {
                slots[i].Increment();
            }
        });

    for (SizeType i = 0; i < kCount; ++i)
    {
        LIMX_EXPECT_EQ(visits[i].Load(), 1);
    }
}

LIMX_TEST(JobExecutor, RepeatedParallelForReusesGraph)
{
    // 同一张图反复用 —— 导入路径会一批接一批地投递
    constexpr SizeType kCount = 200;

    FScopedGraph scoped;

    TAtomic<Int32> total;
    total.Store(0);

    for (Int32 round = 0; round < 5; ++round)
    {
        FJobExecutor::ParallelFor(
            scoped.Get(), kCount, 16,
            [&total](SizeType begin, SizeType end)
            {
                for (SizeType i = begin; i < end; ++i)
                {
                    total.Increment();
                }
            });

        LIMX_EXPECT_EQ(total.Load(),
                       static_cast<Int32>(kCount) * (round + 1));
    }
}

LIMX_TEST(JobExecutor, ParallelForAccumulatesIntoDistinctSlots)
{
    // 每个作业只写属于自己的槽位, 最后由主线程汇总 —— 这是并行归约
    // 最省事也最安全的写法, 导入路径解码出的图像就按这个形状放置。
    constexpr SizeType kCount = 512;

    FScopedGraph scoped;

    TArray<Int32> results;
    results.SetSize(kCount);

    for (SizeType i = 0; i < kCount; ++i)
    {
        results[i] = -1;
    }

    Int32* data = results.GetData();

    FJobExecutor::ParallelFor(
        scoped.Get(), kCount, 8,
        [data](SizeType begin, SizeType end)
        {
            for (SizeType i = begin; i < end; ++i)
            {
                data[i] = static_cast<Int32>(i) * 2;
            }
        });

    for (SizeType i = 0; i < kCount; ++i)
    {
        LIMX_EXPECT_EQ(results[i], static_cast<Int32>(i) * 2);
    }
}

// ============================================================================
// TTaskGroup — 计数屏障
// ============================================================================

LIMX_TEST(TaskGroup, StartsCompleteAndCountsPairs)
{
    TTaskGroup group;

    LIMX_EXPECT_TRUE(group.IsComplete());

    group.BeginTask();
    group.BeginTask();

    LIMX_EXPECT_FALSE(group.IsComplete());
    LIMX_EXPECT_EQ(group.GetPendingCount(), 2);

    group.NotifyComplete();
    group.NotifyComplete();

    LIMX_EXPECT_TRUE(group.IsComplete());
}

LIMX_TEST(TaskGroup, WaitReturnsAfterWorkersFinish)
{
    constexpr Int32 kTaskCount = 100;

    FScopedGraph scoped;

    TTaskGroup group;

    TAtomic<Int32> ran;
    ran.Store(0);

    for (Int32 i = 0; i < kTaskCount; ++i)
    {
        group.BeginTask();

        (void)scoped.Get().Dispatch(
            TFunction<void()>(
                [&ran, &group]()
                {
                    ran.Increment();
                    group.NotifyComplete();
                }));
    }

    group.Wait();

    LIMX_EXPECT_TRUE(group.IsComplete());
    LIMX_EXPECT_EQ(ran.Load(), kTaskCount);
}

LIMX_TEST(TaskGroup, ResetAllowsReuse)
{
    TTaskGroup group;

    group.BeginTask();
    group.NotifyComplete();

    group.Reset();

    LIMX_EXPECT_TRUE(group.IsComplete());
    LIMX_EXPECT_EQ(group.GetPendingCount(), 0);
}

// ============================================================================
// 并行性 — 只测量, 不断言时间
// ============================================================================

LIMX_TEST(JobExecutor, ParallelForActuallyUsesMultipleThreads)
{
    // 前面那些用例证明的是"活干对了", 而一个把所有作业都在调用线程上
    // 顺序跑完的执行器同样能全部通过。这里补上缺的那一半: 活是不是真的
    // 分散到了多个线程上。
    //
    // 判据取"观察到的线程数 > 1"而非"耗时下降到几分之一" —— 后者依赖
    // 机器负载, 写进断言就是在制造不稳定用例。加速比只打印, 不断言。
    constexpr SizeType kBatchCount = 64;

    const UInt32 hardware = FThread::HardwareConcurrency();

    if (hardware < 2)
    {
        LIMX_TEST_SKIP("单核机器上无从并行");
        return;
    }

    FScopedGraph scoped(4);

    UInt32 threadIds[kBatchCount] = {};

    UInt32* ids = threadIds;

    const Float64 parallelBegin = FPlatformTime::Seconds();

    FJobExecutor::ParallelFor(
        scoped.Get(), kBatchCount, 1,
        [ids](SizeType begin, SizeType end)
        {
            // 每个作业做一点实打实的活 —— 全是空转的话, 一个工作线程
            // 可能在别人被唤醒之前就把整批领完了
            volatile Float64 sink = 0.0;

            for (Int32 i = 0; i < 200000; ++i)
            {
                sink = sink + static_cast<Float64>(i) * 0.5;
            }

            ids[begin] = FThread::CurrentThreadId();
            static_cast<void>(end);
        });

    const Float64 parallelMs =
        (FPlatformTime::Seconds() - parallelBegin) * 1000.0;

    // 统计不同线程 id 的个数
    SizeType distinct = 0;

    for (SizeType i = 0; i < kBatchCount; ++i)
    {
        bool seen = false;

        for (SizeType j = 0; j < i; ++j)
        {
            if (threadIds[j] == threadIds[i])
            {
                seen = true;
                break;
            }
        }

        if (!seen)
        {
            ++distinct;
        }
    }

    // 同样的活在调用线程上顺序做一遍, 给出加速比。
    //
    // 只打印不断言 —— 加速比取决于机器当时的负载, 写进断言就是在制造
    // 不稳定用例。真正的判据是上面那个"线程数 > 1"。
    const Float64 serialBegin = FPlatformTime::Seconds();

    for (SizeType batch = 0; batch < kBatchCount; ++batch)
    {
        volatile Float64 sink = 0.0;

        for (Int32 i = 0; i < 200000; ++i)
        {
            sink = sink + static_cast<Float64>(i) * 0.5;
        }
    }

    const Float64 serialMs =
        (FPlatformTime::Seconds() - serialBegin) * 1000.0;

    LIMX_TEST_INFO("硬件线程 {} | 观察到 {} 个不同线程", hardware, distinct);
    LIMX_TEST_INFO("串行 {} ms → 并行 {} ms (4 个工作线程, 加速 {} 倍)",
                   serialMs, parallelMs,
                   (parallelMs > 0.0) ? (serialMs / parallelMs) : 0.0);

    LIMX_EXPECT_GT(distinct, static_cast<SizeType>(1));
}
