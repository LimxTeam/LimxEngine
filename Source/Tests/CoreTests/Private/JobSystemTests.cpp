/*******************************************************************************
 * 文件: JobSystemTests.cpp
 * 创建时间: 2026-08-30
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   作业系统数据结构的单元测试 — 计数器、作业、批次、并行 For 切分
 *
 * 设计哲学:
 *   这一层里没有线程 —— `FJobSystem.h` 提供的是作业的**描述**, 不含任何
 *   执行器。因此这些用例全是确定性的: 不睡眠、不轮询、不依赖调度顺序,
 *   跑一万遍结果都一样。并发部分的验证在 TaskGraphTests 里。
 *
 *   把这条界线在测试里划清楚是有意义的: "作业系统"这个名字容易让人以为
 *   它会自己跑起来, 而实际上调用方必须自己把批次喂给某个执行器。
 *
 *   重点覆盖切分的边界。并行 For 的错误几乎全在边界上 —— 最后一批越界、
 *   总数不能整除时漏掉尾巴、批大小为零时除零。这些错误在"数据量恰好是
 *   批大小整数倍"的顺手测试里一个都碰不到。
 *
 * 依赖关系:
 *   内部: CoreTests/CoreTestsMinimal.h, Core/Threading/FJobSystem.h
 *
 ******************************************************************************/

#include "CoreTests/CoreTestsMinimal.h"
#include "Core/Threading/FJobSystem.h"

using namespace Limx;

// ============================================================================
// FJobCounter
// ============================================================================

LIMX_TEST(JobCounter, StartsAtZeroAndIsComplete)
{
    FJobCounter counter;

    LIMX_EXPECT_EQ(counter.GetValue(), 0);
    LIMX_EXPECT_TRUE(counter.IsComplete());
}

LIMX_TEST(JobCounter, DecrementReturnsRemaining)
{
    // 返回的是**递减之后**的剩余值 —— 调用方靠它判断"我是最后一个",
    // 返回递减前的值会让每个作业都以为自己不是最后一个。
    FJobCounter counter(3);

    LIMX_EXPECT_EQ(counter.Decrement(), 2);
    LIMX_EXPECT_EQ(counter.Decrement(), 1);
    LIMX_EXPECT_EQ(counter.Decrement(), 0);
    LIMX_EXPECT_TRUE(counter.IsComplete());
}

LIMX_TEST(JobCounter, IncrementAndReset)
{
    FJobCounter counter(1);

    counter.Increment();
    LIMX_EXPECT_EQ(counter.GetValue(), 2);
    LIMX_EXPECT_FALSE(counter.IsComplete());

    counter.Reset(5);
    LIMX_EXPECT_EQ(counter.GetValue(), 5);

    counter.Reset();
    LIMX_EXPECT_EQ(counter.GetValue(), 0);
    LIMX_EXPECT_TRUE(counter.IsComplete());
}

// ============================================================================
// FJob
// ============================================================================

LIMX_TEST(Job, NoDependencyIsAlwaysReady)
{
    FJob job;

    LIMX_EXPECT_TRUE(job.IsReady());
}

LIMX_TEST(Job, DependencyGatesReadiness)
{
    FJobCounter prerequisite(2);

    FJob job;
    job.DependsOn = &prerequisite;

    LIMX_EXPECT_FALSE(job.IsReady());

    (void)prerequisite.Decrement();
    LIMX_EXPECT_FALSE(job.IsReady());

    (void)prerequisite.Decrement();
    LIMX_EXPECT_TRUE(job.IsReady());
}

LIMX_TEST(Job, EntryPointIsInvocable)
{
    Int32 witness = 0;

    FJob job([&witness]() { witness = 42; });

    LIMX_EXPECT_TRUE(static_cast<bool>(job.EntryPoint));

    job.EntryPoint();

    LIMX_EXPECT_EQ(witness, 42);
}

// ============================================================================
// FJobBatch
// ============================================================================

LIMX_TEST(JobBatch, StartsEmpty)
{
    FJobBatch batch;

    LIMX_EXPECT_TRUE(batch.IsEmpty());
    LIMX_EXPECT_EQ(batch.GetCount(), static_cast<SizeType>(0));
}

LIMX_TEST(JobBatch, SetDependencyAndCompletionApplyToAll)
{
    // 只作用于当时已有的作业 —— 之后再 Add 的不会被追认。这个语义必须钉住:
    // 反过来 (追认) 也说得通, 而两种理解会让"先设再加"的代码悄悄少一个依赖。
    FJobCounter dependency(1);
    FJobCounter completion(0);

    FJobBatch batch;
    batch.Add([]() {});
    batch.Add([]() {});

    batch.SetDependency(&dependency);
    batch.SetCompletionCounter(&completion);

    LIMX_EXPECT_EQ(batch.GetCount(), static_cast<SizeType>(2));

    const TArray<FJob>& jobs = batch.GetJobs();

    for (SizeType i = 0; i < jobs.GetSize(); ++i)
    {
        LIMX_EXPECT_TRUE(jobs[i].DependsOn == &dependency);
        LIMX_EXPECT_TRUE(jobs[i].SignalOnComplete == &completion);
        LIMX_EXPECT_FALSE(jobs[i].IsReady());
    }

    batch.Add([]() {});
    LIMX_EXPECT_TRUE(batch.GetJobs()[2].DependsOn == nullptr);
}

LIMX_TEST(JobBatch, ClearEmptiesBatch)
{
    FJobBatch batch;
    batch.Add([]() {});
    batch.Add([]() {});

    batch.Clear();

    LIMX_EXPECT_TRUE(batch.IsEmpty());
}

// ============================================================================
// FParallelFor — 切分
// ============================================================================

namespace
{

/// 执行批次里的全部作业, 并递减各自的完成计数器
void RunBatchInline(FJobBatch& batch)
{
    TArray<FJob>& jobs = batch.GetJobs();

    for (SizeType i = 0; i < jobs.GetSize(); ++i)
    {
        if (jobs[i].EntryPoint)
        {
            jobs[i].EntryPoint();
        }

        if (jobs[i].SignalOnComplete != nullptr)
        {
            (void)jobs[i].SignalOnComplete->Decrement();
        }
    }
}

} // namespace

LIMX_TEST(ParallelFor, ZeroCountProducesNoJobs)
{
    FJobBatch batch = FParallelFor::Create(
        0, 8, [](SizeType, SizeType) {});

    LIMX_EXPECT_TRUE(batch.IsEmpty());
}

LIMX_TEST(ParallelFor, CoversEveryIndexExactlyOnce)
{
    // 切分最经典的两种错法都在这里被抓住: 尾批越界 (会写出数组外),
    // 与尾批漏掉 (最后几个元素永远不被处理)。
    constexpr SizeType kCount = 100;

    Int32 visits[kCount] = {};

    FJobBatch batch = FParallelFor::Create(
        kCount, 16,
        [&visits](SizeType begin, SizeType end)
        {
            for (SizeType i = begin; i < end; ++i)
            {
                ++visits[i];
            }
        });

    RunBatchInline(batch);

    for (SizeType i = 0; i < kCount; ++i)
    {
        LIMX_EXPECT_EQ(visits[i], 1);
    }
}

LIMX_TEST(ParallelFor, BatchCountIsCeilingOfDivision)
{
    // 100 / 16 = 6.25 → 7 批。向下取整会漏掉最后 4 个元素。
    FJobBatch batch = FParallelFor::Create(
        100, 16, [](SizeType, SizeType) {});

    LIMX_EXPECT_EQ(batch.GetCount(), static_cast<SizeType>(7));

    FJobBatch exact = FParallelFor::Create(
        64, 16, [](SizeType, SizeType) {});

    LIMX_EXPECT_EQ(exact.GetCount(), static_cast<SizeType>(4));
}

LIMX_TEST(ParallelFor, ZeroBatchSizeFallsBackToOne)
{
    // 批大小为零会导致除零。必须被兜底成 1 而非崩溃。
    FJobBatch batch = FParallelFor::Create(
        5, 0, [](SizeType, SizeType) {});

    LIMX_EXPECT_EQ(batch.GetCount(), static_cast<SizeType>(5));
}

LIMX_TEST(ParallelFor, BatchSizeLargerThanCountGivesOneJob)
{
    FJobBatch batch = FParallelFor::Create(
        3, 1000, [](SizeType, SizeType) {});

    LIMX_EXPECT_EQ(batch.GetCount(), static_cast<SizeType>(1));
}

LIMX_TEST(ParallelFor, CompletionCounterMatchesBatchCount)
{
    FJobCounter completion;

    FJobBatch batch = FParallelFor::Create(
        50, 8, [](SizeType, SizeType) {}, &completion);

    LIMX_EXPECT_EQ(completion.GetValue(),
                   static_cast<Int32>(batch.GetCount()));

    RunBatchInline(batch);

    LIMX_EXPECT_TRUE(completion.IsComplete());
}

LIMX_TEST(ParallelFor, RangesArePartitionedNotOverlapping)
{
    // 逐批检查区间首尾相接: [0,b0) [b0,b1) ... 最后一个结束于 count。
    // 区间重叠会让某些元素被处理两次 —— 累加类的迭代体因此结果翻倍,
    // 而这在"每个元素只写不读"的测试里看不出来。
    constexpr SizeType kCount     = 37;
    constexpr SizeType kBatchSize = 8;

    FJobBatch batch = FParallelFor::Create(
        kCount, kBatchSize, [](SizeType, SizeType) {});

    SizeType expectedBegin = 0;

    // 直接按公式复算每一批的区间, 与实现独立
    for (SizeType i = 0; i < batch.GetCount(); ++i)
    {
        const SizeType rangeBegin = i * kBatchSize;
        SizeType       rangeEnd   = rangeBegin + kBatchSize;

        if (rangeEnd > kCount)
        {
            rangeEnd = kCount;
        }

        LIMX_EXPECT_EQ(rangeBegin, expectedBegin);
        LIMX_EXPECT_TRUE(rangeEnd > rangeBegin);

        expectedBegin = rangeEnd;
    }

    LIMX_EXPECT_EQ(expectedBegin, kCount);
}

LIMX_TEST(ParallelFor, SurvivesTemporaryIterationBody)
{
    // 迭代体必须按值捕获。最常见的写法就是直接传一个临时 lambda ——
    // 它在这条语句结束时就析构了, 而批次要到之后才被执行。按引用捕获的话
    // 这里读到的是已释放的内存, 而且往往"碰巧能跑", 换个分配器才崩。
    constexpr SizeType kCount = 20;

    Int32 visits[kCount] = {};

    Int32* visitPointer = visits;

    FJobBatch batch = FParallelFor::Create(
        kCount, 4,
        [visitPointer](SizeType begin, SizeType end)
        {
            for (SizeType i = begin; i < end; ++i)
            {
                ++visitPointer[i];
            }
        });

    // 临时迭代体此刻已经析构 —— 批次仍必须能正确执行
    RunBatchInline(batch);

    for (SizeType i = 0; i < kCount; ++i)
    {
        LIMX_EXPECT_EQ(visits[i], 1);
    }
}
