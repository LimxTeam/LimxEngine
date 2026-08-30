/*******************************************************************************
 * 文件: TaskGraphTests.cpp
 * 创建时间: 2026-08-30
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   任务图调度器的回归测试 — 依赖顺序、并发计数、等待语义、关闭排空
 *
 * 设计哲学:
 *   并发测试最大的风险是它自己不可靠 —— 一个偶尔失败的用例比没有用例
 *   更糟: 团队会开始习惯性重跑, 而真正的回归就混在"又抽风了"里溜过去。
 *
 *   因此这里的断言全部只依赖**与调度顺序无关的不变量**:
 *     每个任务恰好执行一次 (不丢、不重);
 *     有依赖的任务必定在其前置之后完成;
 *     WaitForAll 返回时没有任何任务还没跑完。
 *   没有一处断言"任务 A 先于任务 B 被取出队列"这类事, 那取决于线程调度,
 *   写进断言就是在制造不稳定用例。
 *
 *   也不用 sleep 来"等一等"。睡眠只是把竞态的概率调低, 并没有消除它 ——
 *   在负载高的 CI 机器上照样会翻。同步一律走 WaitForAll。
 *
 * 技术特性:
 *   - 每个用例自建调度器实例并在结束时关闭, 用例之间互不影响
 *   - 顺序断言用原子序号记录, 而非依赖执行时刻
 *   - 覆盖链式、扇出、扇入 (钻石) 三种依赖形态
 *
 * 依赖关系:
 *   内部: CoreTests/CoreTestsMinimal.h, Core/Threading/FTaskGraph.h
 *
 * 注意事项:
 *   任务体里不做断言 —— 断言宏会写测试运行器的状态, 那不是线程安全的。
 *   任务只往原子变量里记录, 断言统一放在 WaitForAll 之后的主线程上。
 *
 ******************************************************************************/

#include "CoreTests/CoreTestsMinimal.h"
#include "Core/Threading/FTaskGraph.h"

using namespace Limx;

namespace
{

/// 起停成对的调度器 —— 用例提前返回时也能正确关闭
class FScopedTaskGraph
{
public:
    explicit FScopedTaskGraph(UInt32 workerCount = 4)
    {
        m_Graph.Initialize(workerCount);
    }

    ~FScopedTaskGraph()
    {
        m_Graph.Shutdown();
    }

    LIMX_NON_COPYABLE(FScopedTaskGraph);
    LIMX_NON_MOVABLE(FScopedTaskGraph);

    LIMX_NODISCARD FTaskGraph& Get() { return m_Graph; }

private:
    FTaskGraph m_Graph;
};

} // namespace

// ============================================================================
// 基本执行
// ============================================================================

LIMX_TEST(TaskGraph, SingleTaskRunsExactlyOnce)
{
    FScopedTaskGraph scoped;

    TAtomic<Int32> runCount;
    runCount.Store(0);

    FTaskHandle task = scoped.Get().Dispatch(
        [&runCount]() { runCount.Increment(); });

    scoped.Get().WaitForAll();

    LIMX_EXPECT_EQ(runCount.Load(), 1);
    LIMX_EXPECT_TRUE(task.IsCompleted());
}

LIMX_TEST(TaskGraph, ManyIndependentTasksAllRunOnce)
{
    // 丢任务与重复执行是调度器最基本的两种失效。用计数而非"是否跑过"
    // 来断言, 才能把两者都盖住: 只查布尔标志的话, 重复执行看不出来。
    constexpr Int32 kTaskCount = 256;

    FScopedTaskGraph scoped;

    TAtomic<Int32> counters[kTaskCount];

    for (Int32 i = 0; i < kTaskCount; ++i)
    {
        counters[i].Store(0);
    }

    for (Int32 i = 0; i < kTaskCount; ++i)
    {
        TAtomic<Int32>* slot = &counters[i];

        (void)scoped.Get().Dispatch([slot]() { slot->Increment(); });
    }

    scoped.Get().WaitForAll();

    for (Int32 i = 0; i < kTaskCount; ++i)
    {
        LIMX_EXPECT_EQ(counters[i].Load(), 1);
    }
}

LIMX_TEST(TaskGraph, WaitForAllLeavesNothingPending)
{
    constexpr Int32 kTaskCount = 128;

    FScopedTaskGraph scoped;

    TAtomic<Int32> done;
    done.Store(0);

    for (Int32 i = 0; i < kTaskCount; ++i)
    {
        (void)scoped.Get().Dispatch([&done]() { done.Increment(); });
    }

    scoped.Get().WaitForAll();

    // 两条断言缺一不可: 计数归零说明调度器认为没事了, 而实际完成数
    // 说明它认为得对。原先的实现在"队列已空但工作线程还没登记"的窗口里
    // 会提前返回 —— 那时第一条成立而第二条不成立。
    LIMX_EXPECT_EQ(scoped.Get().GetPendingTaskCount(), 0);
    LIMX_EXPECT_EQ(done.Load(), kTaskCount);
}

// ============================================================================
// 依赖顺序
// ============================================================================

LIMX_TEST(TaskGraph, DependentRunsAfterPrerequisite)
{
    FScopedTaskGraph scoped;

    TAtomic<Int32> sequence;
    sequence.Store(0);

    TAtomic<Int32> firstOrder;
    TAtomic<Int32> secondOrder;
    firstOrder.Store(0);
    secondOrder.Store(0);

    FTaskHandle first = scoped.Get().CreateTask(
        [&sequence, &firstOrder]() { firstOrder.Store(sequence.Increment()); });

    FTaskHandle second = scoped.Get().CreateTask(
        [&sequence, &secondOrder]() { secondOrder.Store(sequence.Increment()); });

    scoped.Get().AddDependency(first, second);

    scoped.Get().Submit(first);
    scoped.Get().Submit(second);

    scoped.Get().WaitForAll();

    LIMX_EXPECT_EQ(firstOrder.Load(), 1);
    LIMX_EXPECT_EQ(secondOrder.Load(), 2);
}

LIMX_TEST(TaskGraph, LinearChainRunsInOrder)
{
    // 链式依赖: A → B → C → D。每一环都要等上一环。
    constexpr Int32 kChainLength = 8;

    FScopedTaskGraph scoped;

    TAtomic<Int32> sequence;
    sequence.Store(0);

    TAtomic<Int32> order[kChainLength];
    FTaskHandle    tasks[kChainLength];

    for (Int32 i = 0; i < kChainLength; ++i)
    {
        order[i].Store(0);

        TAtomic<Int32>* slot = &order[i];

        tasks[i] = scoped.Get().CreateTask(
            [&sequence, slot]() { slot->Store(sequence.Increment()); });
    }

    for (Int32 i = 1; i < kChainLength; ++i)
    {
        scoped.Get().AddDependency(tasks[i - 1], tasks[i]);
    }

    for (Int32 i = 0; i < kChainLength; ++i)
    {
        scoped.Get().Submit(tasks[i]);
    }

    scoped.Get().WaitForAll();

    for (Int32 i = 0; i < kChainLength; ++i)
    {
        LIMX_EXPECT_EQ(order[i].Load(), i + 1);
    }
}

LIMX_TEST(TaskGraph, DiamondWaitsForBothBranches)
{
    // 钻石: A 之后 B 与 C 并行, 两者都完成后才是 D。
    //
    // 扇入是等待计数最容易出错的地方: 递减不是原子的就会让 D 提前启动,
    // 而"提前启动"在轻负载下往往仍能拿到正确结果 —— 因为 B、C 早就跑完了。
    FScopedTaskGraph scoped;

    TAtomic<Int32> sequence;
    sequence.Store(0);

    TAtomic<Int32> orderA, orderB, orderC, orderD;
    orderA.Store(0);
    orderB.Store(0);
    orderC.Store(0);
    orderD.Store(0);

    FTaskHandle a = scoped.Get().CreateTask(
        [&sequence, &orderA]() { orderA.Store(sequence.Increment()); });
    FTaskHandle b = scoped.Get().CreateTask(
        [&sequence, &orderB]() { orderB.Store(sequence.Increment()); });
    FTaskHandle c = scoped.Get().CreateTask(
        [&sequence, &orderC]() { orderC.Store(sequence.Increment()); });
    FTaskHandle d = scoped.Get().CreateTask(
        [&sequence, &orderD]() { orderD.Store(sequence.Increment()); });

    scoped.Get().AddDependency(a, b);
    scoped.Get().AddDependency(a, c);
    scoped.Get().AddDependency(b, d);
    scoped.Get().AddDependency(c, d);

    scoped.Get().Submit(a);
    scoped.Get().Submit(b);
    scoped.Get().Submit(c);
    scoped.Get().Submit(d);

    scoped.Get().WaitForAll();

    // A 必定第一, D 必定最后; B 与 C 的先后取决于调度, 不作断言
    LIMX_EXPECT_EQ(orderA.Load(), 1);
    LIMX_EXPECT_EQ(orderD.Load(), 4);
    LIMX_EXPECT_TRUE(orderB.Load() > orderA.Load());
    LIMX_EXPECT_TRUE(orderC.Load() > orderA.Load());
    LIMX_EXPECT_TRUE(orderD.Load() > orderB.Load());
    LIMX_EXPECT_TRUE(orderD.Load() > orderC.Load());
}

LIMX_TEST(TaskGraph, WideFanOutThenJoin)
{
    // 一对多再多对一 —— 逐帧并行最常见的形态
    constexpr Int32 kBranchCount = 64;

    FScopedTaskGraph scoped;

    TAtomic<Int32> branchesDone;
    branchesDone.Store(0);

    TAtomic<Int32> branchesAtJoin;
    branchesAtJoin.Store(-1);

    FTaskHandle root = scoped.Get().CreateTask([]() {});

    FTaskHandle join = scoped.Get().CreateTask(
        [&branchesDone, &branchesAtJoin]()
        {
            // 汇合点看到的完成数必须已经是全部 —— 这是扇入的核心保证
            branchesAtJoin.Store(branchesDone.Load());
        });

    FTaskHandle branches[kBranchCount];

    for (Int32 i = 0; i < kBranchCount; ++i)
    {
        branches[i] = scoped.Get().CreateTask(
            [&branchesDone]() { branchesDone.Increment(); });

        scoped.Get().AddDependency(root, branches[i]);
        scoped.Get().AddDependency(branches[i], join);
    }

    scoped.Get().Submit(root);

    for (Int32 i = 0; i < kBranchCount; ++i)
    {
        scoped.Get().Submit(branches[i]);
    }

    scoped.Get().Submit(join);

    scoped.Get().WaitForAll();

    LIMX_EXPECT_EQ(branchesDone.Load(), kBranchCount);
    LIMX_EXPECT_EQ(branchesAtJoin.Load(), kBranchCount);
}

// ============================================================================
// 生命周期
// ============================================================================

LIMX_TEST(TaskGraph, ShutdownDrainsPendingTasks)
{
    // Shutdown 的文档写的是"等待所有任务完成"。原先它只是置停止标志然后
    // join —— 队列里没跑完的任务被整批丢弃, 它们持有的调度器引用也就
    // 永远不会释放。
    constexpr Int32 kTaskCount = 200;

    TAtomic<Int32> done;
    done.Store(0);

    {
        FTaskGraph graph;
        graph.Initialize(4);

        for (Int32 i = 0; i < kTaskCount; ++i)
        {
            (void)graph.Dispatch([&done]() { done.Increment(); });
        }

        // 不调用 WaitForAll —— 关闭本身就该把队列排空
        graph.Shutdown();
    }

    LIMX_EXPECT_EQ(done.Load(), kTaskCount);
}

LIMX_TEST(TaskGraph, LocalInstanceDoesNotLeakIntoGlobal)
{
    // 后继任务就绪时必须入回**它自己**那个调度器。原先这里取的是全局单例,
    // 于是任何非单例实例上的依赖任务都会被投递到全局队列 —— 全局若未
    // 初始化, 它们永远不会执行, 而调用方只看到 WaitForAll 一直等不到。
    FScopedTaskGraph scoped;

    TAtomic<Int32> ran;
    ran.Store(0);

    FTaskHandle first  = scoped.Get().CreateTask([]() {});
    FTaskHandle second = scoped.Get().CreateTask(
        [&ran]() { ran.Increment(); });

    scoped.Get().AddDependency(first, second);

    scoped.Get().Submit(first);
    scoped.Get().Submit(second);

    scoped.Get().WaitForAll();

    LIMX_EXPECT_EQ(ran.Load(), 1);
    LIMX_EXPECT_EQ(scoped.Get().GetPendingTaskCount(), 0);

    // 全局实例从未被初始化, 也就不该有任何遗留
    LIMX_EXPECT_EQ(FTaskGraph::Get().GetPendingTaskCount(), 0);
}

LIMX_TEST(TaskGraph, EmptyWaitReturnsImmediately)
{
    FScopedTaskGraph scoped;

    scoped.Get().WaitForAll();

    LIMX_EXPECT_EQ(scoped.Get().GetPendingTaskCount(), 0);
}

LIMX_TEST(TaskGraph, RepeatedWaitIsSafe)
{
    FScopedTaskGraph scoped;

    TAtomic<Int32> done;
    done.Store(0);

    for (Int32 round = 0; round < 4; ++round)
    {
        for (Int32 i = 0; i < 32; ++i)
        {
            (void)scoped.Get().Dispatch([&done]() { done.Increment(); });
        }

        scoped.Get().WaitForAll();

        LIMX_EXPECT_EQ(done.Load(), (round + 1) * 32);
    }
}
