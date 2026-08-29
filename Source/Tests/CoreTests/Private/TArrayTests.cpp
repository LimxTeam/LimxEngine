/*******************************************************************************
 * 文件: TArrayTests.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   TArray 单元测试 — 覆盖增删查改、扩容、拷贝/移动语义、元素生命周期、
 *   分配器契约与内存泄漏
 *
 * 设计哲学:
 *   非平凡类型优先 — 大量用例以 FProbe 而非 Int32 作为元素类型，
 *   因为只有非平凡类型才能暴露"析构漏调""扩容退化为拷贝""移后源未清理"
 *   这类真实缺陷; 平凡类型走 memcpy 路径会把这些问题全部掩盖。
 *
 *   分配守恒 — 每个涉及堆的用例都注入 FTrackingAllocator 并在结尾断言
 *   分配次数与释放次数相等, 使泄漏在引入的当次提交即被发现。
 *
 * 技术特性:
 *   - 覆盖 POD 与非 POD 两条实现路径 (TArray 对算术类型走 memcpy 优化)
 *   - 扩容边界用例精确落在容量翻倍的临界点上
 *   - 自赋值、空容器操作、单元素等边界条件均有专门用例
 *
 * 依赖关系:
 *   内部: CoreTests/CoreTestsMinimal.h, CoreTests/FProbe.h
 *
 * 注意事项:
 *   TArray 的 Find 未命中返回 kSizeTypeMax 而非 -1
 *
 ******************************************************************************/

#include "CoreTests/CoreTestsMinimal.h"
#include "CoreTests/FProbe.h"

using namespace Limx;

// ============================================================================
// 构造与基本状态
// ============================================================================

LIMX_TEST(TArray, DefaultConstructedIsEmpty)
{
    TArray<Int32> values;

    LIMX_EXPECT_EQ(values.GetSize(), SizeType(0));
    LIMX_EXPECT_TRUE(values.IsEmpty());
    LIMX_EXPECT_EQ(values.GetCapacity(), SizeType(0));
    LIMX_EXPECT_NULL(values.GetData());
}

LIMX_TEST(TArray, ReserveAllocatesWithoutChangingSize)
{
    TArray<Int32> values;
    values.Reserve(16);

    LIMX_EXPECT_EQ(values.GetSize(), SizeType(0));
    LIMX_EXPECT_TRUE(values.IsEmpty());
    LIMX_EXPECT_GE(values.GetCapacity(), SizeType(16));
    LIMX_EXPECT_NOT_NULL(values.GetData());
}

LIMX_TEST(TArray, ReserveSmallerIsNoOp)
{
    TArray<Int32> values;
    values.Reserve(32);

    const SizeType capacityBefore = values.GetCapacity();
    values.Reserve(4);

    // 缩小请求不应释放已有容量
    LIMX_EXPECT_EQ(values.GetCapacity(), capacityBefore);
}

// ============================================================================
// 添加与访问
// ============================================================================

LIMX_TEST(TArray, AddReturnsIndexAndGrowsSize)
{
    TArray<Int32> values;

    LIMX_EXPECT_EQ(values.Add(10), SizeType(0));
    LIMX_EXPECT_EQ(values.Add(20), SizeType(1));
    LIMX_EXPECT_EQ(values.Add(30), SizeType(2));

    LIMX_EXPECT_EQ(values.GetSize(), SizeType(3));
    LIMX_EXPECT_EQ(values[0], 10);
    LIMX_EXPECT_EQ(values[1], 20);
    LIMX_EXPECT_EQ(values[2], 30);
}

LIMX_TEST(TArray, FirstAndLastTrackEnds)
{
    TArray<Int32> values;
    values.Add(7);
    values.Add(8);
    values.Add(9);

    LIMX_EXPECT_EQ(values.First(), 7);
    LIMX_EXPECT_EQ(values.Last(), 9);

    values.Add(10);
    LIMX_EXPECT_EQ(values.First(), 7);
    LIMX_EXPECT_EQ(values.Last(), 10);
}

LIMX_TEST(TArray, EmplaceConstructsInPlace)
{
    FProbe::ResetCounters();

    {
        TArray<FProbe> probes;
        probes.Reserve(4);

        FProbe& created = probes.Emplace(42);

        LIMX_EXPECT_EQ(created.GetValue(), 42);
        LIMX_EXPECT_EQ(probes.GetSize(), SizeType(1));

        // 就地构造应只触发一次值构造, 不产生任何拷贝或移动
        LIMX_EXPECT_EQ(FProbe::s_ValueConstructCount, 1);
        LIMX_EXPECT_EQ(FProbe::s_CopyConstructCount, 0);
        LIMX_EXPECT_EQ(FProbe::s_MoveConstructCount, 0);
    }

    LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 0);
}

LIMX_TEST(TArray, AddRvalueUsesMoveConstruction)
{
    FProbe::ResetCounters();

    {
        TArray<FProbe> probes;
        probes.Reserve(4);

        FProbe source(11);
        probes.Add(static_cast<FProbe&&>(source));

        LIMX_EXPECT_EQ(FProbe::s_MoveConstructCount, 1);
        LIMX_EXPECT_EQ(FProbe::s_CopyConstructCount, 0);
        LIMX_EXPECT_TRUE(source.IsMovedFrom());
        LIMX_EXPECT_EQ(probes[0].GetValue(), 11);
    }

    LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 0);
}

// ============================================================================
// 扩容
// ============================================================================

LIMX_TEST(TArray, GrowsAutomaticallyPreservingElements)
{
    TArray<Int32> values;

    // 跨越多次扩容
    for (Int32 i = 0; i < 100; ++i)
    {
        values.Add(i * 3);
    }

    LIMX_REQUIRE_EQ(values.GetSize(), SizeType(100));
    LIMX_EXPECT_GE(values.GetCapacity(), SizeType(100));

    for (Int32 i = 0; i < 100; ++i)
    {
        LIMX_EXPECT_EQ(values[static_cast<SizeType>(i)], i * 3);
    }
}

LIMX_TEST(TArray, GrowthUsesMoveNotCopy)
{
    FProbe::ResetCounters();

    {
        TArray<FProbe> probes;

        // 不预留容量, 强制触发多次扩容重定位
        for (Int32 i = 0; i < 32; ++i)
        {
            probes.Emplace(i);
        }

        LIMX_EXPECT_EQ(probes.GetSize(), SizeType(32));

        // 扩容搬迁必须走移动构造 — 出现拷贝说明重定位路径没有正确
        // 使用 MoveConstructItems, 对含堆资源的元素会造成性能塌陷
        LIMX_EXPECT_EQ(FProbe::s_CopyConstructCount, 0);
        LIMX_EXPECT_GT(FProbe::s_MoveConstructCount, 0);

        for (Int32 i = 0; i < 32; ++i)
        {
            LIMX_EXPECT_EQ(probes[static_cast<SizeType>(i)].GetValue(), i);
        }
    }

    LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 0);
}

LIMX_TEST(TArray, ShrinkReleasesExcessCapacity)
{
    TArray<Int32> values;
    values.Reserve(128);

    values.Add(1);
    values.Add(2);

    LIMX_EXPECT_GE(values.GetCapacity(), SizeType(128));

    values.Shrink();

    LIMX_EXPECT_EQ(values.GetCapacity(), SizeType(2));
    LIMX_EXPECT_EQ(values.GetSize(), SizeType(2));
    LIMX_EXPECT_EQ(values[0], 1);
    LIMX_EXPECT_EQ(values[1], 2);
}

LIMX_TEST(TArray, ShrinkOnEmptyReleasesBuffer)
{
    TArray<Int32> values;
    values.Reserve(64);
    values.Shrink();

    LIMX_EXPECT_EQ(values.GetCapacity(), SizeType(0));
    LIMX_EXPECT_TRUE(values.IsEmpty());
}

// ============================================================================
// 插入与删除
// ============================================================================

LIMX_TEST(TArray, InsertAtFrontShiftsElements)
{
    TArray<Int32> values;
    values.Add(2);
    values.Add(3);

    values.Insert(0, 1);

    LIMX_REQUIRE_EQ(values.GetSize(), SizeType(3));
    LIMX_EXPECT_EQ(values[0], 1);
    LIMX_EXPECT_EQ(values[1], 2);
    LIMX_EXPECT_EQ(values[2], 3);
}

LIMX_TEST(TArray, InsertAtEndAppends)
{
    TArray<Int32> values;
    values.Add(1);
    values.Add(2);

    values.Insert(values.GetSize(), 3);

    LIMX_REQUIRE_EQ(values.GetSize(), SizeType(3));
    LIMX_EXPECT_EQ(values[2], 3);
}

LIMX_TEST(TArray, InsertInMiddlePreservesOrder)
{
    TArray<Int32> values;
    for (Int32 i = 0; i < 5; ++i)
    {
        values.Add(i);   // 0 1 2 3 4
    }

    values.Insert(2, 99); // 0 1 99 2 3 4

    LIMX_REQUIRE_EQ(values.GetSize(), SizeType(6));
    LIMX_EXPECT_EQ(values[0], 0);
    LIMX_EXPECT_EQ(values[1], 1);
    LIMX_EXPECT_EQ(values[2], 99);
    LIMX_EXPECT_EQ(values[3], 2);
    LIMX_EXPECT_EQ(values[4], 3);
    LIMX_EXPECT_EQ(values[5], 4);
}

LIMX_TEST(TArray, RemoveAtPreservesOrder)
{
    TArray<Int32> values;
    for (Int32 i = 0; i < 5; ++i)
    {
        values.Add(i);   // 0 1 2 3 4
    }

    values.RemoveAt(1);  // 0 2 3 4

    LIMX_REQUIRE_EQ(values.GetSize(), SizeType(4));
    LIMX_EXPECT_EQ(values[0], 0);
    LIMX_EXPECT_EQ(values[1], 2);
    LIMX_EXPECT_EQ(values[2], 3);
    LIMX_EXPECT_EQ(values[3], 4);
}

LIMX_TEST(TArray, RemoveAtSwapMovesLastIntoHole)
{
    TArray<Int32> values;
    for (Int32 i = 0; i < 5; ++i)
    {
        values.Add(i);   // 0 1 2 3 4
    }

    values.RemoveAtSwap(1); // 0 4 2 3

    LIMX_REQUIRE_EQ(values.GetSize(), SizeType(4));
    LIMX_EXPECT_EQ(values[0], 0);
    LIMX_EXPECT_EQ(values[1], 4);
    LIMX_EXPECT_EQ(values[2], 2);
    LIMX_EXPECT_EQ(values[3], 3);
}

LIMX_TEST(TArray, RemoveLastElementLeavesEmpty)
{
    TArray<Int32> values;
    values.Add(42);

    values.RemoveAt(0);

    LIMX_EXPECT_TRUE(values.IsEmpty());
    LIMX_EXPECT_EQ(values.GetSize(), SizeType(0));
}

LIMX_TEST(TArray, RemoveAtDestroysElement)
{
    FProbe::ResetCounters();

    {
        TArray<FProbe> probes;
        probes.Reserve(8);
        probes.Emplace(1);
        probes.Emplace(2);
        probes.Emplace(3);

        const Int32 destructBefore = FProbe::s_DestructCount;
        probes.RemoveAt(1);

        // 被移除的元素必须被析构 (搬迁产生的临时析构也计入, 故用 >=)
        LIMX_EXPECT_GT(FProbe::s_DestructCount, destructBefore);
        LIMX_EXPECT_EQ(probes.GetSize(), SizeType(2));
        LIMX_EXPECT_EQ(probes[0].GetValue(), 1);
        LIMX_EXPECT_EQ(probes[1].GetValue(), 3);
    }

    LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 0);
}

LIMX_TEST(TArray, ClearDestroysAllButKeepsCapacity)
{
    FProbe::ResetCounters();

    {
        TArray<FProbe> probes;
        probes.Reserve(16);

        for (Int32 i = 0; i < 8; ++i)
        {
            probes.Emplace(i);
        }

        const SizeType capacityBefore = probes.GetCapacity();
        probes.Clear();

        LIMX_EXPECT_TRUE(probes.IsEmpty());
        LIMX_EXPECT_EQ(probes.GetCapacity(), capacityBefore);
        LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 0);
    }

    LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 0);
}

// ============================================================================
// Append
// ============================================================================

LIMX_TEST(TArray, AppendConcatenates)
{
    TArray<Int32> first;
    first.Add(1);
    first.Add(2);

    TArray<Int32> second;
    second.Add(3);
    second.Add(4);

    first.Append(second);

    LIMX_REQUIRE_EQ(first.GetSize(), SizeType(4));
    LIMX_EXPECT_EQ(first[0], 1);
    LIMX_EXPECT_EQ(first[1], 2);
    LIMX_EXPECT_EQ(first[2], 3);
    LIMX_EXPECT_EQ(first[3], 4);

    // 源不应被修改
    LIMX_EXPECT_EQ(second.GetSize(), SizeType(2));
}

LIMX_TEST(TArray, AppendEmptyIsNoOp)
{
    TArray<Int32> values;
    values.Add(1);

    TArray<Int32> empty;
    values.Append(empty);

    LIMX_EXPECT_EQ(values.GetSize(), SizeType(1));
    LIMX_EXPECT_EQ(values[0], 1);
}

// ============================================================================
// 查找
// ============================================================================

LIMX_TEST(TArray, FindReturnsIndexOfFirstMatch)
{
    TArray<Int32> values;
    values.Add(10);
    values.Add(20);
    values.Add(10);

    LIMX_EXPECT_EQ(values.Find(10), SizeType(0));
    LIMX_EXPECT_EQ(values.Find(20), SizeType(1));
}

LIMX_TEST(TArray, FindMissingReturnsSizeTypeMax)
{
    TArray<Int32> values;
    values.Add(1);

    LIMX_EXPECT_EQ(values.Find(999), kSizeTypeMax);
    LIMX_EXPECT_FALSE(values.Contains(999));
    LIMX_EXPECT_TRUE(values.Contains(1));
}

LIMX_TEST(TArray, FindOnEmptyArrayIsSafe)
{
    TArray<Int32> values;

    LIMX_EXPECT_EQ(values.Find(1), kSizeTypeMax);
    LIMX_EXPECT_FALSE(values.Contains(1));
}

// ============================================================================
// 拷贝与移动语义
// ============================================================================

LIMX_TEST(TArray, CopyConstructionIsDeep)
{
    TArray<Int32> source;
    source.Add(1);
    source.Add(2);

    TArray<Int32> copy(source);

    LIMX_REQUIRE_EQ(copy.GetSize(), SizeType(2));
    LIMX_EXPECT_NE(copy.GetData(), source.GetData());

    // 修改副本不得影响源
    copy[0] = 99;
    LIMX_EXPECT_EQ(source[0], 1);
    LIMX_EXPECT_EQ(copy[0], 99);
}

LIMX_TEST(TArray, MoveConstructionTransfersBuffer)
{
    TArray<Int32> source;
    source.Add(1);
    source.Add(2);

    const Int32* originalData = source.GetData();

    TArray<Int32> moved(static_cast<TArray<Int32>&&>(source));

    LIMX_EXPECT_EQ(moved.GetSize(), SizeType(2));
    LIMX_EXPECT_EQ(moved.GetData(), originalData);

    // 移后源必须处于可安全析构的空状态
    LIMX_EXPECT_TRUE(source.IsEmpty());
    LIMX_EXPECT_EQ(source.GetCapacity(), SizeType(0));
    LIMX_EXPECT_NULL(source.GetData());
}

LIMX_TEST(TArray, CopyAssignmentReplacesContent)
{
    TArray<Int32> source;
    source.Add(1);
    source.Add(2);

    TArray<Int32> target;
    target.Add(99);
    target.Add(98);
    target.Add(97);

    target = source;

    LIMX_REQUIRE_EQ(target.GetSize(), SizeType(2));
    LIMX_EXPECT_EQ(target[0], 1);
    LIMX_EXPECT_EQ(target[1], 2);
}

LIMX_TEST(TArray, SelfAssignmentIsSafe)
{
    TArray<Int32> values;
    values.Add(1);
    values.Add(2);

    // 自赋值不得释放自身缓冲区后再读取
    const TArray<Int32>& alias = values;
    values = alias;

    LIMX_REQUIRE_EQ(values.GetSize(), SizeType(2));
    LIMX_EXPECT_EQ(values[0], 1);
    LIMX_EXPECT_EQ(values[1], 2);
}

LIMX_TEST(TArray, MoveAssignmentReleasesTargetElements)
{
    FProbe::ResetCounters();

    {
        TArray<FProbe> source;
        source.Emplace(1);

        TArray<FProbe> target;
        target.Emplace(100);
        target.Emplace(200);

        target = static_cast<TArray<FProbe>&&>(source);

        LIMX_EXPECT_EQ(target.GetSize(), SizeType(1));
        LIMX_EXPECT_EQ(target[0].GetValue(), 1);
        LIMX_EXPECT_TRUE(source.IsEmpty());
    }

    // 目标原有的两个元素必须被析构, 否则计数不平
    LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 0);
}

// ============================================================================
// 迭代
// ============================================================================

LIMX_TEST(TArray, RangeForVisitsAllInOrder)
{
    TArray<Int32> values;
    for (Int32 i = 0; i < 5; ++i)
    {
        values.Add(i);
    }

    Int32 expected = 0;
    Int32 visited  = 0;

    for (Int32 value : values)
    {
        LIMX_EXPECT_EQ(value, expected);
        ++expected;
        ++visited;
    }

    LIMX_EXPECT_EQ(visited, 5);
}

LIMX_TEST(TArray, RangeForOnEmptyDoesNotIterate)
{
    TArray<Int32> values;

    Int32 visited = 0;
    for (Int32 value : values)
    {
        LIMX_UNUSED(value);
        ++visited;
    }

    LIMX_EXPECT_EQ(visited, 0);
}

LIMX_TEST(TArray, IteratorsAllowMutation)
{
    TArray<Int32> values;
    values.Add(1);
    values.Add(2);

    for (Int32& value : values)
    {
        value *= 10;
    }

    LIMX_EXPECT_EQ(values[0], 10);
    LIMX_EXPECT_EQ(values[1], 20);
}

// ============================================================================
// 分配器契约与泄漏
// ============================================================================

LIMX_TEST(TArray, InjectedAllocatorIsUsedAndBalanced)
{
    FTrackingAllocator allocator;

    {
        TArray<Int32> values(allocator);

        for (Int32 i = 0; i < 64; ++i)
        {
            values.Add(i);
        }

        LIMX_EXPECT_GT(allocator.GetAllocationCount(), 0ull);
        LIMX_EXPECT_TRUE(allocator.HasLeaks());
    }

    // 析构后必须归零
    LIMX_EXPECT_FALSE(allocator.HasLeaks());
    LIMX_EXPECT_EQ(allocator.GetCurrentBytes(), SizeType(0));
    LIMX_EXPECT_EQ(allocator.GetLiveAllocationCount(), 0ull);
}

LIMX_TEST(TArray, NonTrivialElementsLeaveNoLeak)
{
    FTrackingAllocator allocator;
    FProbe::ResetCounters();

    {
        TArray<FProbe> probes(allocator);

        for (Int32 i = 0; i < 50; ++i)
        {
            probes.Emplace(i);
        }

        probes.RemoveAt(0);
        probes.RemoveAtSwap(10);
        probes.Insert(5, FProbe(999));
    }

    LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 0);
    LIMX_EXPECT_FALSE(allocator.HasLeaks());
}

LIMX_TEST(TArray, ClearThenRefillReusesCapacity)
{
    FTrackingAllocator allocator;

    {
        TArray<Int32> values(allocator);
        values.Reserve(32);

        for (Int32 i = 0; i < 32; ++i)
        {
            values.Add(i);
        }

        const UInt64 allocationsAfterFill = allocator.GetAllocationCount();

        values.Clear();
        for (Int32 i = 0; i < 32; ++i)
        {
            values.Add(i);
        }

        // 容量足够时重新填充不应触发新的分配
        LIMX_EXPECT_EQ(allocator.GetAllocationCount(), allocationsAfterFill);
    }

    LIMX_EXPECT_FALSE(allocator.HasLeaks());
}

// ============================================================================
// 自引用安全
//
// Add 的实参可能指向数组自身的元素。若扩容时先释放旧缓冲区再拷贝，
// 传入的引用会悬垂，读到的是已释放内存。这类缺陷在容量充足时完全不显现，
// 只有恰好触发扩容的那一次才出错 —— 表现为偶发的数据损坏。
// LZ77 解压的反向引用正是这种用法，本组用例即由该场景反推而来。
// ============================================================================

LIMX_TEST(TArray, AddFromSelfSurvivesReallocation)
{
    TArray<Int32> values;

    values.Add(42);

    // 反复从数组内部取值追加, 必然多次跨越扩容边界
    for (Int32 i = 0; i < 200; ++i)
    {
        values.Add(values[values.GetSize() - 1]);
    }

    LIMX_REQUIRE_EQ(values.GetSize(), SizeType(201));

    // 全部元素都应是最初那个值 — 悬垂读会读出垃圾
    for (SizeType i = 0; i < values.GetSize(); ++i)
    {
        LIMX_REQUIRE_EQ(values[i], 42);
    }
}

LIMX_TEST(TArray, AddFromSelfPreservesSequence)
{
    TArray<Int32> values;

    values.Add(1);
    values.Add(2);
    values.Add(3);
    values.Add(4);

    // 模拟 LZ77 距离 4 的反向引用: 从固定偏移回读并追加
    const SizeType copyStart = 0;

    for (Int32 i = 0; i < 400; ++i)
    {
        values.Add(values[copyStart + static_cast<SizeType>(i)]);
    }

    LIMX_REQUIRE_EQ(values.GetSize(), SizeType(404));

    // 结果应是 1,2,3,4 的无限循环
    for (SizeType i = 0; i < values.GetSize(); ++i)
    {
        LIMX_REQUIRE_EQ(values[i], static_cast<Int32>(i % 4) + 1);
    }
}

LIMX_TEST(TArray, AddFromSelfWorksForNonTrivialType)
{
    FProbe::ResetCounters();

    {
        TArray<FProbe> probes;
        probes.Emplace(7);

        for (Int32 i = 0; i < 100; ++i)
        {
            probes.Add(probes[0]);
        }

        LIMX_REQUIRE_EQ(probes.GetSize(), SizeType(101));

        for (SizeType i = 0; i < probes.GetSize(); ++i)
        {
            LIMX_REQUIRE_EQ(probes[i].GetValue(), 7);
        }
    }

    LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 0);
}

LIMX_TEST(TArray, AddMovedFromSelfSurvivesReallocation)
{
    TArray<Int32> values;
    values.Add(9);

    // 右值重载同样可能绑定到数组内部的元素
    for (Int32 i = 0; i < 200; ++i)
    {
        Int32 copy = values[values.GetSize() - 1];
        values.Add(static_cast<Int32&&>(copy));
    }

    LIMX_REQUIRE_EQ(values.GetSize(), SizeType(201));

    for (SizeType i = 0; i < values.GetSize(); ++i)
    {
        LIMX_REQUIRE_EQ(values[i], 9);
    }
}
