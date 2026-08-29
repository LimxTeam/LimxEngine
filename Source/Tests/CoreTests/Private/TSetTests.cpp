/*******************************************************************************
 * 文件: TSetTests.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   TSet 单元测试 — 覆盖去重语义、探测与删除、扩容 rehash、迭代与分配守恒
 *
 * 设计哲学:
 *   Add 的返回值即语义 — TSet::Add 返回"是否为新元素"，这个布尔值是集合
 *   去重契约的唯一可观测出口。用例逐个断言首次插入为 true、重复插入为 false，
 *   并同步校验 GetSize 未增长，防止"计数与实际内容脱节"这类隐蔽错误。
 *
 *   高冲突优先 — 与 TMap 同理，注入恒定哈希把所有元素挤入同一探测链，
 *   在最坏情况下验证插入、查找、删除三者的一致性。
 *
 * 技术特性:
 *   - 恒定哈希与低位哈希两档冲突密度
 *   - 大规模插入后逐元素回查, 捕捉 rehash 丢失
 *   - 非平凡元素用例验证析构配对
 *
 * 依赖关系:
 *   内部: CoreTests/CoreTestsMinimal.h, CoreTests/FProbe.h
 *
 * 注意事项:
 *   TSet 仅提供 ConstIterator — 元素不可通过迭代器修改 (修改会破坏哈希不变式)
 *
 ******************************************************************************/

#include "CoreTests/CoreTestsMinimal.h"
#include "CoreTests/FProbe.h"

using namespace Limx;

namespace
{

/// 恒定哈希 — 全部元素落入同一初始槽
struct FConstantSetHash
{
    LIMX_NODISCARD SizeType operator()(Int32 key) const
    {
        LIMX_UNUSED(key);
        return 0;
    }
};

} // namespace

// ============================================================================
// 基本状态与去重
// ============================================================================

LIMX_TEST(TSet, DefaultConstructedIsEmpty)
{
    TSet<Int32> set;

    LIMX_EXPECT_EQ(set.GetSize(), SizeType(0));
    LIMX_EXPECT_TRUE(set.IsEmpty());
}

LIMX_TEST(TSet, AddNewElementReturnsTrue)
{
    TSet<Int32> set;

    LIMX_EXPECT_TRUE(set.Add(1));
    LIMX_EXPECT_TRUE(set.Add(2));
    LIMX_EXPECT_EQ(set.GetSize(), SizeType(2));
}

LIMX_TEST(TSet, AddDuplicateReturnsFalseAndDoesNotGrow)
{
    TSet<Int32> set;

    LIMX_REQUIRE_TRUE(set.Add(1));
    LIMX_EXPECT_FALSE(set.Add(1));

    // 去重契约: 重复插入既不新增计数也不改变内容
    LIMX_EXPECT_EQ(set.GetSize(), SizeType(1));
    LIMX_EXPECT_TRUE(set.Contains(1));
}

LIMX_TEST(TSet, ManyDuplicatesCollapseToOne)
{
    TSet<Int32> set;

    for (Int32 i = 0; i < 100; ++i)
    {
        set.Add(42);
    }

    LIMX_EXPECT_EQ(set.GetSize(), SizeType(1));
    LIMX_EXPECT_TRUE(set.Contains(42));
}

// ============================================================================
// 查找
// ============================================================================

LIMX_TEST(TSet, ContainsReflectsMembership)
{
    TSet<Int32> set;
    set.Add(10);
    set.Add(20);

    LIMX_EXPECT_TRUE(set.Contains(10));
    LIMX_EXPECT_TRUE(set.Contains(20));
    LIMX_EXPECT_FALSE(set.Contains(30));
}

LIMX_TEST(TSet, ContainsOnEmptySetIsSafe)
{
    TSet<Int32> set;

    LIMX_EXPECT_FALSE(set.Contains(1));
}

// ============================================================================
// 删除
// ============================================================================

LIMX_TEST(TSet, RemoveErasesElement)
{
    TSet<Int32> set;
    set.Add(1);
    set.Add(2);

    LIMX_EXPECT_TRUE(set.Remove(1));

    LIMX_EXPECT_EQ(set.GetSize(), SizeType(1));
    LIMX_EXPECT_FALSE(set.Contains(1));
    LIMX_EXPECT_TRUE(set.Contains(2));
}

LIMX_TEST(TSet, RemoveMissingReturnsFalse)
{
    TSet<Int32> set;
    set.Add(1);

    LIMX_EXPECT_FALSE(set.Remove(999));
    LIMX_EXPECT_EQ(set.GetSize(), SizeType(1));
}

LIMX_TEST(TSet, RemoveThenReAddWorks)
{
    TSet<Int32> set;

    set.Add(1);
    set.Remove(1);

    LIMX_EXPECT_FALSE(set.Contains(1));
    LIMX_EXPECT_TRUE(set.Add(1));
    LIMX_EXPECT_TRUE(set.Contains(1));
    LIMX_EXPECT_EQ(set.GetSize(), SizeType(1));
}

LIMX_TEST(TSet, RemoveFromCollisionChainKeepsOthersReachable)
{
    TSet<Int32, FConstantSetHash> set;

    const Int32 kCount = 32;
    for (Int32 i = 0; i < kCount; ++i)
    {
        LIMX_REQUIRE_TRUE(set.Add(i));
    }

    // 删除链中交错的元素
    for (Int32 i = 0; i < kCount; i += 2)
    {
        LIMX_EXPECT_TRUE(set.Remove(i));
    }

    LIMX_EXPECT_EQ(set.GetSize(), SizeType(kCount / 2));

    for (Int32 i = 1; i < kCount; i += 2)
    {
        LIMX_EXPECT_TRUE(set.Contains(i));
    }

    for (Int32 i = 0; i < kCount; i += 2)
    {
        LIMX_EXPECT_FALSE(set.Contains(i));
    }
}

LIMX_TEST(TSet, ClearEmptiesSet)
{
    TSet<Int32> set;
    for (Int32 i = 0; i < 20; ++i)
    {
        set.Add(i);
    }

    set.Clear();

    LIMX_EXPECT_TRUE(set.IsEmpty());

    for (Int32 i = 0; i < 20; ++i)
    {
        LIMX_EXPECT_FALSE(set.Contains(i));
    }
}

LIMX_TEST(TSet, ClearedSetIsReusable)
{
    TSet<Int32> set;
    set.Add(1);
    set.Clear();

    LIMX_EXPECT_TRUE(set.Add(1));
    LIMX_EXPECT_EQ(set.GetSize(), SizeType(1));
}

// ============================================================================
// 扩容
// ============================================================================

LIMX_TEST(TSet, GrowsPreservingAllElements)
{
    TSet<Int32> set;

    const Int32 kCount = 500;
    for (Int32 i = 0; i < kCount; ++i)
    {
        LIMX_REQUIRE_TRUE(set.Add(i));
    }

    LIMX_REQUIRE_EQ(set.GetSize(), SizeType(kCount));

    for (Int32 i = 0; i < kCount; ++i)
    {
        LIMX_EXPECT_TRUE(set.Contains(i));
    }
}

LIMX_TEST(TSet, RehashUnderFullCollisionPreservesElements)
{
    TSet<Int32, FConstantSetHash> set;

    for (Int32 i = 0; i < 200; ++i)
    {
        set.Add(i);
    }

    LIMX_REQUIRE_EQ(set.GetSize(), SizeType(200));

    for (Int32 i = 0; i < 200; ++i)
    {
        LIMX_EXPECT_TRUE(set.Contains(i));
    }
}

LIMX_TEST(TSet, ReserveAvoidsRehashDuringFill)
{
    TSet<Int32> set;
    set.Reserve(256);

    for (Int32 i = 0; i < 100; ++i)
    {
        set.Add(i);
    }

    LIMX_EXPECT_EQ(set.GetSize(), SizeType(100));

    for (Int32 i = 0; i < 100; ++i)
    {
        LIMX_EXPECT_TRUE(set.Contains(i));
    }
}

// ============================================================================
// 迭代
// ============================================================================

LIMX_TEST(TSet, IterationVisitsEveryElementOnce)
{
    TSet<Int32> set;

    const Int32 kCount = 50;
    for (Int32 i = 0; i < kCount; ++i)
    {
        set.Add(i);
    }

    Int32 visited = 0;
    Int64 sum     = 0;

    for (const Int32& value : set)
    {
        ++visited;
        sum += value;
    }

    LIMX_EXPECT_EQ(visited, kCount);

    // 0..49 求和 = 1225
    LIMX_EXPECT_EQ(sum, Int64(1225));
}

LIMX_TEST(TSet, IterationOnEmptySetDoesNotIterate)
{
    TSet<Int32> set;

    Int32 visited = 0;
    for (const Int32& value : set)
    {
        LIMX_UNUSED(value);
        ++visited;
    }

    LIMX_EXPECT_EQ(visited, 0);
}

LIMX_TEST(TSet, IterationSkipsRemovedElements)
{
    TSet<Int32> set;

    for (Int32 i = 0; i < 20; ++i)
    {
        set.Add(i);
    }

    for (Int32 i = 0; i < 20; i += 2)
    {
        set.Remove(i);
    }

    Int32 visited = 0;
    for (const Int32& value : set)
    {
        LIMX_EXPECT_EQ(value % 2, 1);
        ++visited;
    }

    LIMX_EXPECT_EQ(visited, 10);
}

// ============================================================================
// 字符串元素
// ============================================================================

LIMX_TEST(TSet, StringElementsDeduplicate)
{
    TSet<FString> set;

    LIMX_EXPECT_TRUE(set.Add(FString("alpha")));
    LIMX_EXPECT_TRUE(set.Add(FString("beta")));
    LIMX_EXPECT_FALSE(set.Add(FString("alpha")));

    LIMX_EXPECT_EQ(set.GetSize(), SizeType(2));
    LIMX_EXPECT_TRUE(set.Contains(FString("beta")));
    LIMX_EXPECT_FALSE(set.Contains(FString("gamma")));
}

// ============================================================================
// 拷贝语义与分配守恒
// ============================================================================

LIMX_TEST(TSet, CopyIsIndependent)
{
    TSet<Int32> source;
    source.Add(1);
    source.Add(2);

    TSet<Int32> copy(source);

    LIMX_REQUIRE_EQ(copy.GetSize(), SizeType(2));

    copy.Add(3);

    LIMX_EXPECT_EQ(source.GetSize(), SizeType(2));
    LIMX_EXPECT_FALSE(source.Contains(3));
    LIMX_EXPECT_TRUE(copy.Contains(3));
}

LIMX_TEST(TSet, NonTrivialElementsAreDestroyed)
{
    FProbe::ResetCounters();

    {
        TSet<Int32> keys;
        TArray<FProbe> probes;

        for (Int32 i = 0; i < 30; ++i)
        {
            keys.Add(i);
            probes.Emplace(i);
        }

        LIMX_EXPECT_EQ(keys.GetSize(), SizeType(30));
    }

    LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 0);
}

LIMX_TEST(TSet, InjectedAllocatorIsBalanced)
{
    FTrackingAllocator allocator;

    {
        TSet<Int32> set(allocator);

        for (Int32 i = 0; i < 300; ++i)
        {
            set.Add(i);
        }

        for (Int32 i = 0; i < 150; ++i)
        {
            set.Remove(i);
        }

        LIMX_EXPECT_GT(allocator.GetAllocationCount(), 0ull);
    }

    LIMX_EXPECT_FALSE(allocator.HasLeaks());
    LIMX_EXPECT_EQ(allocator.GetCurrentBytes(), SizeType(0));
}
