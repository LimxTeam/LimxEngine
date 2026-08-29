/*******************************************************************************
 * 文件: TMapTests.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   TMap 单元测试 — 覆盖增删查改、扩容 rehash、探测链回移删除、
 *   迭代、拷贝/移动语义与分配守恒
 *
 * 设计哲学:
 *   针对回移删除设计 — TMap 采用开放寻址 + BackshiftDelete 而非墓碑标记。
 *   回移删除的正确性风险在于: 删除某个槽后，必须把其后属于同一探测链的
 *   元素前移填补空位，否则链被截断，后续元素永久查不到。这类缺陷在稀疏
 *   数据下不会显现，只有在高冲突下才暴露。
 *
 *   故意坏哈希 — 专门注入一个"所有键映射到同一槽"的哈希函数，
 *   把每次插入都变成最长探测链，使回移逻辑在最坏情况下被完整覆盖。
 *   这是随机数据测不出来的。
 *
 * 技术特性:
 *   - FConstantHash 强制全冲突, 验证探测与回移在退化场景下的正确性
 *   - 大规模插入/交错删除后做全量存在性校验, 捕捉链截断
 *   - 扩容 rehash 前后逐键校验, 确认重哈希未丢失条目
 *
 * 依赖关系:
 *   内部: CoreTests/CoreTestsMinimal.h, CoreTests/FProbe.h
 *
 * 注意事项:
 *   TMap::operator[] 在键不存在时会插入值初始化的条目, 与 Find 语义不同
 *
 ******************************************************************************/

#include "CoreTests/CoreTestsMinimal.h"
#include "CoreTests/FProbe.h"

using namespace Limx;

namespace
{

/// 恒定哈希 — 令所有键落到同一初始槽, 制造最坏情况的探测链
///
/// 用于验证开放寻址的探测、回移删除、扩容在完全退化时依然正确。
struct FConstantHash
{
    LIMX_NODISCARD SizeType operator()(Int32 key) const
    {
        LIMX_UNUSED(key);
        return 0;
    }
};

/// 低位哈希 — 只保留低 3 位, 制造可控的中等冲突密度
struct FLowBitsHash
{
    LIMX_NODISCARD SizeType operator()(Int32 key) const
    {
        return static_cast<SizeType>(static_cast<UInt32>(key) & 0x7u);
    }
};

} // namespace

// ============================================================================
// 基本状态
// ============================================================================

LIMX_TEST(TMap, DefaultConstructedIsEmpty)
{
    TMap<Int32, Int32> map;

    LIMX_EXPECT_EQ(map.GetSize(), SizeType(0));
    LIMX_EXPECT_TRUE(map.IsEmpty());
}

LIMX_TEST(TMap, AddInsertsAndGrowsSize)
{
    TMap<Int32, Int32> map;

    map.Add(1, 100);
    map.Add(2, 200);

    LIMX_EXPECT_EQ(map.GetSize(), SizeType(2));
    LIMX_EXPECT_FALSE(map.IsEmpty());
}

LIMX_TEST(TMap, AddReturnsReferenceToStoredValue)
{
    TMap<Int32, Int32> map;

    Int32& stored = map.Add(7, 70);
    stored = 77;

    const Int32* found = map.Find(7);
    LIMX_REQUIRE_NOT_NULL(found);
    LIMX_EXPECT_EQ(*found, 77);
}

LIMX_TEST(TMap, AddExistingKeyOverwritesValue)
{
    TMap<Int32, Int32> map;

    map.Add(1, 100);
    map.Add(1, 999);

    // 重复键不应增加条目数
    LIMX_EXPECT_EQ(map.GetSize(), SizeType(1));

    const Int32* found = map.Find(1);
    LIMX_REQUIRE_NOT_NULL(found);
    LIMX_EXPECT_EQ(*found, 999);
}

// ============================================================================
// 查找
// ============================================================================

LIMX_TEST(TMap, FindReturnsNullForMissingKey)
{
    TMap<Int32, Int32> map;
    map.Add(1, 100);

    LIMX_EXPECT_NOT_NULL(map.Find(1));
    LIMX_EXPECT_NULL(map.Find(2));
}

LIMX_TEST(TMap, ContainsMatchesFind)
{
    TMap<Int32, Int32> map;
    map.Add(5, 50);

    LIMX_EXPECT_TRUE(map.Contains(5));
    LIMX_EXPECT_FALSE(map.Contains(6));
}

LIMX_TEST(TMap, FindOnEmptyMapIsSafe)
{
    TMap<Int32, Int32> map;

    LIMX_EXPECT_NULL(map.Find(1));
    LIMX_EXPECT_FALSE(map.Contains(1));
}

LIMX_TEST(TMap, SubscriptInsertsDefaultForMissingKey)
{
    TMap<Int32, Int32> map;

    // operator[] 与 Find 语义不同 — 缺失键会被插入
    Int32& value = map[42];

    LIMX_EXPECT_EQ(value, 0);
    LIMX_EXPECT_EQ(map.GetSize(), SizeType(1));
    LIMX_EXPECT_TRUE(map.Contains(42));

    value = 123;
    LIMX_EXPECT_EQ(*map.Find(42), 123);
}

LIMX_TEST(TMap, SubscriptReturnsExistingValue)
{
    TMap<Int32, Int32> map;
    map.Add(1, 111);

    LIMX_EXPECT_EQ(map[1], 111);
    LIMX_EXPECT_EQ(map.GetSize(), SizeType(1));
}

// ============================================================================
// 删除 — 回移删除的正确性
// ============================================================================

LIMX_TEST(TMap, RemoveErasesKey)
{
    TMap<Int32, Int32> map;
    map.Add(1, 100);
    map.Add(2, 200);

    LIMX_EXPECT_TRUE(map.Remove(1));

    LIMX_EXPECT_EQ(map.GetSize(), SizeType(1));
    LIMX_EXPECT_FALSE(map.Contains(1));
    LIMX_EXPECT_TRUE(map.Contains(2));
}

LIMX_TEST(TMap, RemoveMissingKeyReturnsFalse)
{
    TMap<Int32, Int32> map;
    map.Add(1, 100);

    LIMX_EXPECT_FALSE(map.Remove(999));
    LIMX_EXPECT_EQ(map.GetSize(), SizeType(1));
}

LIMX_TEST(TMap, RemoveThenReinsertWorks)
{
    TMap<Int32, Int32> map;
    map.Add(1, 100);
    map.Remove(1);
    map.Add(1, 200);

    LIMX_EXPECT_EQ(map.GetSize(), SizeType(1));

    const Int32* found = map.Find(1);
    LIMX_REQUIRE_NOT_NULL(found);
    LIMX_EXPECT_EQ(*found, 200);
}

LIMX_TEST(TMap, RemoveFromFullCollisionChainKeepsOthersReachable)
{
    // 全部键哈希到同一槽 — 形成一条最长探测链
    TMap<Int32, Int32, FConstantHash> map;

    const Int32 kCount = 32;
    for (Int32 i = 0; i < kCount; ++i)
    {
        map.Add(i, i * 10);
    }

    LIMX_REQUIRE_EQ(map.GetSize(), SizeType(kCount));

    // 删除链中间的元素 — 回移逻辑若有缺陷, 其后元素会失联
    for (Int32 i = 0; i < kCount; i += 2)
    {
        LIMX_EXPECT_TRUE(map.Remove(i));
    }

    LIMX_EXPECT_EQ(map.GetSize(), SizeType(kCount / 2));

    // 未删除的元素必须全部仍可查到且值正确
    for (Int32 i = 1; i < kCount; i += 2)
    {
        const Int32* found = map.Find(i);
        LIMX_REQUIRE_NOT_NULL(found);
        LIMX_EXPECT_EQ(*found, i * 10);
    }

    // 已删除的元素必须全部查不到
    for (Int32 i = 0; i < kCount; i += 2)
    {
        LIMX_EXPECT_NULL(map.Find(i));
    }
}

LIMX_TEST(TMap, RemoveHeadOfCollisionChainKeepsTailReachable)
{
    TMap<Int32, Int32, FConstantHash> map;

    for (Int32 i = 0; i < 16; ++i)
    {
        map.Add(i, i);
    }

    // 反复删除链首 — 每次删除都触发一次完整的链回移
    for (Int32 i = 0; i < 15; ++i)
    {
        LIMX_REQUIRE_TRUE(map.Remove(i));

        // 剩余元素必须全部可达
        for (Int32 j = i + 1; j < 16; ++j)
        {
            const Int32* found = map.Find(j);
            LIMX_REQUIRE_NOT_NULL(found);
            LIMX_EXPECT_EQ(*found, j);
        }
    }

    LIMX_EXPECT_EQ(map.GetSize(), SizeType(1));
}

LIMX_TEST(TMap, InterleavedInsertRemoveStaysConsistent)
{
    TMap<Int32, Int32, FLowBitsHash> map;

    // 交错插入与删除, 用低位哈希制造中等冲突密度
    for (Int32 round = 0; round < 20; ++round)
    {
        for (Int32 i = 0; i < 40; ++i)
        {
            map.Add(i, i + round);
        }

        for (Int32 i = 0; i < 40; i += 3)
        {
            map.Remove(i);
        }

        // 每轮结束后校验完整性
        for (Int32 i = 0; i < 40; ++i)
        {
            const bool shouldExist = (i % 3) != 0;
            const Int32* found = map.Find(i);

            if (shouldExist)
            {
                LIMX_REQUIRE_NOT_NULL(found);
                LIMX_EXPECT_EQ(*found, i + round);
            }
            else
            {
                LIMX_EXPECT_NULL(found);
            }
        }

        map.Clear();
    }
}

LIMX_TEST(TMap, ClearEmptiesMap)
{
    TMap<Int32, Int32> map;
    for (Int32 i = 0; i < 20; ++i)
    {
        map.Add(i, i);
    }

    map.Clear();

    LIMX_EXPECT_TRUE(map.IsEmpty());
    LIMX_EXPECT_EQ(map.GetSize(), SizeType(0));

    for (Int32 i = 0; i < 20; ++i)
    {
        LIMX_EXPECT_NULL(map.Find(i));
    }
}

LIMX_TEST(TMap, ClearedMapIsReusable)
{
    TMap<Int32, Int32> map;
    map.Add(1, 1);
    map.Clear();
    map.Add(2, 2);

    LIMX_EXPECT_EQ(map.GetSize(), SizeType(1));
    LIMX_EXPECT_TRUE(map.Contains(2));
    LIMX_EXPECT_FALSE(map.Contains(1));
}

// ============================================================================
// 扩容 rehash
// ============================================================================

LIMX_TEST(TMap, GrowsAndRehashesPreservingAllEntries)
{
    TMap<Int32, Int32> map;

    const Int32 kCount = 500;
    for (Int32 i = 0; i < kCount; ++i)
    {
        map.Add(i, i * 7);
    }

    LIMX_REQUIRE_EQ(map.GetSize(), SizeType(kCount));

    // 多次扩容 rehash 后所有条目必须完好
    for (Int32 i = 0; i < kCount; ++i)
    {
        const Int32* found = map.Find(i);
        LIMX_REQUIRE_NOT_NULL(found);
        LIMX_EXPECT_EQ(*found, i * 7);
    }
}

LIMX_TEST(TMap, ReserveAvoidsRehashDuringFill)
{
    TMap<Int32, Int32> map;
    map.Reserve(256);

    const SizeType capacityAfterReserve = map.GetCapacity();

    for (Int32 i = 0; i < 100; ++i)
    {
        map.Add(i, i);
    }

    // 预留足够容量后填充不应触发扩容
    LIMX_EXPECT_EQ(map.GetCapacity(), capacityAfterReserve);
    LIMX_EXPECT_EQ(map.GetSize(), SizeType(100));
}

LIMX_TEST(TMap, RehashUnderFullCollisionPreservesEntries)
{
    // 全冲突 + 扩容 — 探测链在 rehash 后必须完整重建
    TMap<Int32, Int32, FConstantHash> map;

    for (Int32 i = 0; i < 200; ++i)
    {
        map.Add(i, i * 3);
    }

    LIMX_REQUIRE_EQ(map.GetSize(), SizeType(200));

    for (Int32 i = 0; i < 200; ++i)
    {
        const Int32* found = map.Find(i);
        LIMX_REQUIRE_NOT_NULL(found);
        LIMX_EXPECT_EQ(*found, i * 3);
    }
}

// ============================================================================
// 迭代
// ============================================================================

LIMX_TEST(TMap, IterationVisitsEveryEntryOnce)
{
    TMap<Int32, Int32> map;

    const Int32 kCount = 50;
    for (Int32 i = 0; i < kCount; ++i)
    {
        map.Add(i, i * 2);
    }

    Int32 visited = 0;
    Int64 keySum  = 0;

    for (const auto& pair : map)
    {
        ++visited;
        keySum += pair.Key;

        // 每个条目的值必须与键匹配
        LIMX_EXPECT_EQ(pair.Value, pair.Key * 2);
    }

    LIMX_EXPECT_EQ(visited, kCount);

    // 0..49 求和 = 1225
    LIMX_EXPECT_EQ(keySum, Int64(1225));
}

LIMX_TEST(TMap, IterationOnEmptyMapDoesNotIterate)
{
    TMap<Int32, Int32> map;

    Int32 visited = 0;
    for (const auto& pair : map)
    {
        LIMX_UNUSED(pair);
        ++visited;
    }

    LIMX_EXPECT_EQ(visited, 0);
}

LIMX_TEST(TMap, IterationSkipsRemovedEntries)
{
    TMap<Int32, Int32> map;

    for (Int32 i = 0; i < 20; ++i)
    {
        map.Add(i, i);
    }

    for (Int32 i = 0; i < 20; i += 2)
    {
        map.Remove(i);
    }

    Int32 visited = 0;
    for (const auto& pair : map)
    {
        // 只应遍历到奇数键
        LIMX_EXPECT_EQ(pair.Key % 2, 1);
        ++visited;
    }

    LIMX_EXPECT_EQ(visited, 10);
}

// ============================================================================
// 字符串键
// ============================================================================

LIMX_TEST(TMap, StringKeysWork)
{
    TMap<FString, Int32> map;

    map.Add(FString("alpha"), 1);
    map.Add(FString("beta"), 2);
    map.Add(FString("gamma"), 3);

    LIMX_EXPECT_EQ(map.GetSize(), SizeType(3));

    const Int32* alpha = map.Find(FString("alpha"));
    LIMX_REQUIRE_NOT_NULL(alpha);
    LIMX_EXPECT_EQ(*alpha, 1);

    LIMX_EXPECT_TRUE(map.Contains(FString("gamma")));
    LIMX_EXPECT_FALSE(map.Contains(FString("delta")));
}

LIMX_TEST(TMap, LongStringKeysWork)
{
    TMap<FString, Int32> map;

    // 跨越 SSO 边界的键
    FString longKey;
    for (Int32 i = 0; i < 100; ++i)
    {
        longKey.AppendChar('k');
    }

    map.Add(longKey, 42);

    const Int32* found = map.Find(longKey);
    LIMX_REQUIRE_NOT_NULL(found);
    LIMX_EXPECT_EQ(*found, 42);
}

// ============================================================================
// 拷贝与移动语义
// ============================================================================

LIMX_TEST(TMap, CopyIsIndependent)
{
    TMap<Int32, Int32> source;
    source.Add(1, 100);
    source.Add(2, 200);

    TMap<Int32, Int32> copy(source);

    LIMX_REQUIRE_EQ(copy.GetSize(), SizeType(2));
    LIMX_EXPECT_EQ(*copy.Find(1), 100);

    copy.Add(3, 300);

    // 修改副本不得影响源
    LIMX_EXPECT_EQ(source.GetSize(), SizeType(2));
    LIMX_EXPECT_FALSE(source.Contains(3));
}

LIMX_TEST(TMap, MoveTransfersOwnership)
{
    TMap<Int32, Int32> source;
    source.Add(1, 100);
    source.Add(2, 200);

    TMap<Int32, Int32> moved(static_cast<TMap<Int32, Int32>&&>(source));

    LIMX_EXPECT_EQ(moved.GetSize(), SizeType(2));
    LIMX_EXPECT_EQ(*moved.Find(1), 100);

    // 移后源必须处于可安全析构的空状态
    LIMX_EXPECT_TRUE(source.IsEmpty());
}

// ============================================================================
// 非平凡值与分配守恒
// ============================================================================

LIMX_TEST(TMap, NonTrivialValuesAreDestroyed)
{
    FProbe::ResetCounters();

    {
        TMap<Int32, FProbe> map;

        for (Int32 i = 0; i < 30; ++i)
        {
            map.Add(i, FProbe(i));
        }

        LIMX_EXPECT_EQ(map.GetSize(), SizeType(30));

        for (Int32 i = 0; i < 15; ++i)
        {
            map.Remove(i);
        }

        LIMX_EXPECT_EQ(map.GetSize(), SizeType(15));
    }

    // 所有值对象必须被析构 — 含被删除的与随 map 析构的
    LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 0);
}

LIMX_TEST(TMap, InjectedAllocatorIsBalanced)
{
    FTrackingAllocator allocator;

    {
        TMap<Int32, Int32> map(allocator);

        for (Int32 i = 0; i < 300; ++i)
        {
            map.Add(i, i);
        }

        for (Int32 i = 0; i < 150; ++i)
        {
            map.Remove(i);
        }

        LIMX_EXPECT_GT(allocator.GetAllocationCount(), 0ull);
    }

    LIMX_EXPECT_FALSE(allocator.HasLeaks());
    LIMX_EXPECT_EQ(allocator.GetCurrentBytes(), SizeType(0));
}
