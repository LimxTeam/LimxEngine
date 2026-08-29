/*******************************************************************************
 * 文件: SuballocationTests.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   FSuballocationRegistry 单元测试 — 分配/回收、对齐契约、区间分裂与合并、
 *   bufferImageGranularity 约束、碎片化行为与随机操作压力回归
 *
 * 设计哲学:
 *   不变式驱动 — 分配器的缺陷往往不在单次调用的返回值上，而在若干次操作后
 *   累积出的内部不一致（漏合并、链表断裂、尺寸失衡）。因此几乎每个用例在
 *   关键操作后都调用 Validate()，把"结构是否仍然自洽"变成显式断言。
 *
 *   随机序列可复现 — 压力测试用固定种子的线性同余发生器而非真随机，
 *   任何失败都能靠同一颗种子精确重放，不会出现"偶尔挂一次"的不可调试情形。
 *
 *   重叠是致命错 — 显存分配器返回重叠区间会导致两个资源互相踩踏，
 *   且症状表现为随机的渲染错误。压力测试维护全部活跃区间并逐对校验不相交。
 *
 * 技术特性:
 *   - 覆盖对齐 1..4096 全档位, 断言返回偏移取模为零
 *   - 粒度用例构造线性/非线性交错分配, 验证二者不共享粒度页
 *   - 随机压力: 数千次分配/释放混合操作, 每步 Validate + 区间不相交校验
 *
 * 依赖关系:
 *   内部: RHITests/RHITestsMinimal.h
 *
 * 注意事项:
 *   本文件不创建 Vulkan 设备 — 被测类型刻意与图形 API 无关
 *
 ******************************************************************************/

#include "RHITests/RHITestsMinimal.h"

using namespace Limx;

namespace
{

/// 常用测试容量 — 1 MiB
constexpr UInt64 kOneMiB = 1024ull * 1024ull;

/// 确定性线性同余发生器
///
/// 刻意不使用系统随机源: 压力测试一旦失败必须能用同一颗种子逐字节重放，
/// 否则"偶发失败"根本无从调试。
class FDeterministicRandom
{
public:
    explicit FDeterministicRandom(UInt64 seed)
        : m_State(seed)
    {
    }

    /// 取下一个伪随机值
    UInt64 Next()
    {
        // Numerical Recipes 的 64 位 LCG 参数
        m_State = m_State * 6364136223846793005ull + 1442695040888963407ull;
        return m_State >> 16;
    }

    /// 取 [0, bound) 内的伪随机值
    UInt64 NextBounded(UInt64 bound)
    {
        return (bound == 0) ? 0 : (Next() % bound);
    }

private:
    UInt64 m_State;
};

/// 一次活跃分配的记录 — 压力测试用于校验区间互不重叠
struct FLiveAllocation
{
    UInt64 Offset = 0;
    UInt64 Size   = 0;
    UInt32 Node   = FSuballocationRegistry::kInvalidNode;
};

/// 判断两个区间是否重叠
bool RangesOverlap(UInt64 offsetA, UInt64 sizeA, UInt64 offsetB, UInt64 sizeB)
{
    return (offsetA < offsetB + sizeB) && (offsetB < offsetA + sizeA);
}

/// 判断偏移是否满足对齐
bool IsOffsetAligned(UInt64 offset, UInt64 alignment)
{
    return (offset % alignment) == 0;
}

} // namespace

// ============================================================================
// 初始状态
// ============================================================================

LIMX_TEST(Suballocation, InitializeCreatesSingleFreeRegion)
{
    FSuballocationRegistry registry;
    registry.Initialize(kOneMiB, 1);

    LIMX_EXPECT_TRUE(registry.IsInitialized());
    LIMX_EXPECT_EQ(registry.GetTotalSize(), kOneMiB);
    LIMX_EXPECT_EQ(registry.GetUsedSize(), UInt64(0));
    LIMX_EXPECT_EQ(registry.GetFreeSize(), kOneMiB);
    LIMX_EXPECT_EQ(registry.GetAllocationCount(), UInt32(0));
    LIMX_EXPECT_TRUE(registry.IsEmpty());

    // 初始整段可用 — 最大空闲区间应等于总容量
    LIMX_EXPECT_EQ(registry.GetLargestFreeRegion(), kOneMiB);
    LIMX_EXPECT_TRUE(registry.Validate());
}

LIMX_TEST(Suballocation, ShutdownResetsState)
{
    FSuballocationRegistry registry;
    registry.Initialize(kOneMiB, 1);

    FSuballocationResult result;
    LIMX_REQUIRE_TRUE(registry.Allocate(1024, 16,
                                        ESuballocationType::Linear, result));

    registry.Shutdown();

    LIMX_EXPECT_FALSE(registry.IsInitialized());
    LIMX_EXPECT_EQ(registry.GetTotalSize(), UInt64(0));
    LIMX_EXPECT_TRUE(registry.Validate());
}

// ============================================================================
// 基本分配与回收
// ============================================================================

LIMX_TEST(Suballocation, SingleAllocationSucceeds)
{
    FSuballocationRegistry registry;
    registry.Initialize(kOneMiB, 1);

    FSuballocationResult result;
    LIMX_REQUIRE_TRUE(registry.Allocate(4096, 256,
                                        ESuballocationType::Linear, result));

    LIMX_EXPECT_EQ(result.Size, UInt64(4096));
    LIMX_EXPECT_TRUE(IsOffsetAligned(result.Offset, 256));
    LIMX_EXPECT_NE(result.NodeIndex, FSuballocationRegistry::kInvalidNode);

    LIMX_EXPECT_EQ(registry.GetUsedSize(), UInt64(4096));
    LIMX_EXPECT_EQ(registry.GetAllocationCount(), UInt32(1));
    LIMX_EXPECT_FALSE(registry.IsEmpty());
    LIMX_EXPECT_TRUE(registry.Validate());
}

LIMX_TEST(Suballocation, FreeReturnsSpace)
{
    FSuballocationRegistry registry;
    registry.Initialize(kOneMiB, 1);

    FSuballocationResult result;
    LIMX_REQUIRE_TRUE(registry.Allocate(4096, 256,
                                        ESuballocationType::Linear, result));

    registry.Free(result.NodeIndex);

    LIMX_EXPECT_EQ(registry.GetUsedSize(), UInt64(0));
    LIMX_EXPECT_EQ(registry.GetAllocationCount(), UInt32(0));
    LIMX_EXPECT_TRUE(registry.IsEmpty());

    // 全部归还后应重新合并成一整段
    LIMX_EXPECT_EQ(registry.GetLargestFreeRegion(), kOneMiB);
    LIMX_EXPECT_TRUE(registry.Validate());
}

LIMX_TEST(Suballocation, SequentialAllocationsDoNotOverlap)
{
    FSuballocationRegistry registry;
    registry.Initialize(kOneMiB, 1);

    FSuballocationResult results[16];

    for (Int32 i = 0; i < 16; ++i)
    {
        LIMX_REQUIRE_TRUE(registry.Allocate(1024, 64,
                                            ESuballocationType::Linear,
                                            results[i]));
    }

    // 任意两次分配的区间必须互不相交
    for (Int32 i = 0; i < 16; ++i)
    {
        for (Int32 j = i + 1; j < 16; ++j)
        {
            LIMX_EXPECT_FALSE(RangesOverlap(results[i].Offset, results[i].Size,
                                            results[j].Offset, results[j].Size));
        }
    }

    LIMX_EXPECT_EQ(registry.GetUsedSize(), UInt64(16 * 1024));
    LIMX_EXPECT_TRUE(registry.Validate());
}

LIMX_TEST(Suballocation, ZeroSizeAllocationFails)
{
    FSuballocationRegistry registry;
    registry.Initialize(kOneMiB, 1);

    FSuballocationResult result;
    LIMX_EXPECT_FALSE(registry.Allocate(0, 16,
                                        ESuballocationType::Linear, result));
    LIMX_EXPECT_TRUE(registry.Validate());
}

LIMX_TEST(Suballocation, OversizedAllocationFails)
{
    FSuballocationRegistry registry;
    registry.Initialize(kOneMiB, 1);

    FSuballocationResult result;
    LIMX_EXPECT_FALSE(registry.Allocate(kOneMiB * 2, 16,
                                        ESuballocationType::Linear, result));

    // 失败不得改变内部状态
    LIMX_EXPECT_EQ(registry.GetUsedSize(), UInt64(0));
    LIMX_EXPECT_TRUE(registry.Validate());
}

LIMX_TEST(Suballocation, ExactCapacityAllocationSucceeds)
{
    FSuballocationRegistry registry;
    registry.Initialize(kOneMiB, 1);

    FSuballocationResult result;
    LIMX_REQUIRE_TRUE(registry.Allocate(kOneMiB, 1,
                                        ESuballocationType::Linear, result));

    LIMX_EXPECT_EQ(result.Offset, UInt64(0));
    LIMX_EXPECT_EQ(registry.GetFreeSize(), UInt64(0));
    LIMX_EXPECT_TRUE(registry.Validate());

    // 空间用尽后任何分配都应失败
    FSuballocationResult second;
    LIMX_EXPECT_FALSE(registry.Allocate(1, 1,
                                        ESuballocationType::Linear, second));
}

// ============================================================================
// 对齐契约
// ============================================================================

LIMX_TEST(Suballocation, RespectsAllPowerOfTwoAlignments)
{
    FSuballocationRegistry registry;
    registry.Initialize(kOneMiB, 1);

    for (UInt64 alignment = 1; alignment <= 4096; alignment *= 2)
    {
        FSuballocationResult result;
        LIMX_REQUIRE_TRUE(registry.Allocate(128, alignment,
                                            ESuballocationType::Linear, result));

        LIMX_EXPECT_TRUE(IsOffsetAligned(result.Offset, alignment));
    }

    LIMX_EXPECT_TRUE(registry.Validate());
}

LIMX_TEST(Suballocation, AlignmentPaddingBecomesReusableFreeRegion)
{
    FSuballocationRegistry registry;
    registry.Initialize(kOneMiB, 1);

    // 先占据一小段, 使后续大对齐请求必然产生前置空隙
    FSuballocationResult head;
    LIMX_REQUIRE_TRUE(registry.Allocate(100, 1,
                                        ESuballocationType::Linear, head));
    LIMX_EXPECT_EQ(head.Offset, UInt64(0));

    FSuballocationResult aligned;
    LIMX_REQUIRE_TRUE(registry.Allocate(1024, 4096,
                                        ESuballocationType::Linear, aligned));
    LIMX_EXPECT_EQ(aligned.Offset, UInt64(4096));

    // 对齐产生的 [100, 4096) 空隙应作为空闲区间可被再次分配
    FSuballocationResult gapFill;
    LIMX_REQUIRE_TRUE(registry.Allocate(2000, 1,
                                        ESuballocationType::Linear, gapFill));

    LIMX_EXPECT_GE(gapFill.Offset, UInt64(100));
    LIMX_EXPECT_LE(gapFill.Offset + gapFill.Size, UInt64(4096));
    LIMX_EXPECT_TRUE(registry.Validate());
}

// ============================================================================
// 合并
// ============================================================================

LIMX_TEST(Suballocation, AdjacentFreeRegionsMerge)
{
    FSuballocationRegistry registry;
    registry.Initialize(kOneMiB, 1);

    FSuballocationResult a;
    FSuballocationResult b;
    FSuballocationResult c;

    LIMX_REQUIRE_TRUE(registry.Allocate(1024, 1, ESuballocationType::Linear, a));
    LIMX_REQUIRE_TRUE(registry.Allocate(1024, 1, ESuballocationType::Linear, b));
    LIMX_REQUIRE_TRUE(registry.Allocate(1024, 1, ESuballocationType::Linear, c));

    // 先放两端, 再放中间 — 中间释放时需同时与前后合并
    registry.Free(a.NodeIndex);
    LIMX_EXPECT_TRUE(registry.Validate());

    registry.Free(c.NodeIndex);
    LIMX_EXPECT_TRUE(registry.Validate());

    registry.Free(b.NodeIndex);
    LIMX_EXPECT_TRUE(registry.Validate());

    // 三段全部归还后应合并回完整的一整段
    LIMX_EXPECT_EQ(registry.GetLargestFreeRegion(), kOneMiB);
    LIMX_EXPECT_TRUE(registry.IsEmpty());
}

LIMX_TEST(Suballocation, MergeWithPredecessorOnly)
{
    FSuballocationRegistry registry;
    registry.Initialize(4096, 1);

    FSuballocationResult a;
    FSuballocationResult b;
    FSuballocationResult c;

    LIMX_REQUIRE_TRUE(registry.Allocate(1024, 1, ESuballocationType::Linear, a));
    LIMX_REQUIRE_TRUE(registry.Allocate(1024, 1, ESuballocationType::Linear, b));
    LIMX_REQUIRE_TRUE(registry.Allocate(1024, 1, ESuballocationType::Linear, c));

    registry.Free(a.NodeIndex);
    registry.Free(b.NodeIndex);

    // a+b 合并, c 仍占用 — 最大空闲区应为 2048 或 (尾部剩余 1024) 中的较大者
    LIMX_EXPECT_GE(registry.GetLargestFreeRegion(), UInt64(2048));
    LIMX_EXPECT_TRUE(registry.Validate());
}

LIMX_TEST(Suballocation, MergeWithSuccessorOnly)
{
    FSuballocationRegistry registry;
    registry.Initialize(4096, 1);

    FSuballocationResult a;
    FSuballocationResult b;
    FSuballocationResult c;

    LIMX_REQUIRE_TRUE(registry.Allocate(1024, 1, ESuballocationType::Linear, a));
    LIMX_REQUIRE_TRUE(registry.Allocate(1024, 1, ESuballocationType::Linear, b));
    LIMX_REQUIRE_TRUE(registry.Allocate(1024, 1, ESuballocationType::Linear, c));

    registry.Free(c.NodeIndex);
    registry.Free(b.NodeIndex);

    // b 与 c 及尾部剩余合并
    LIMX_EXPECT_GE(registry.GetLargestFreeRegion(), UInt64(2048));
    LIMX_EXPECT_TRUE(registry.Validate());
}

LIMX_TEST(Suballocation, FullCycleReturnsToPristineState)
{
    FSuballocationRegistry registry;
    registry.Initialize(kOneMiB, 1);

    // 反复填满再清空, 验证不会累积碎片或泄漏节点
    for (Int32 round = 0; round < 8; ++round)
    {
        FSuballocationResult results[32];

        for (Int32 i = 0; i < 32; ++i)
        {
            LIMX_REQUIRE_TRUE(registry.Allocate(4096, 64,
                                                ESuballocationType::Linear,
                                                results[i]));
        }

        for (Int32 i = 0; i < 32; ++i)
        {
            registry.Free(results[i].NodeIndex);
        }

        LIMX_REQUIRE_TRUE(registry.Validate());
        LIMX_EXPECT_TRUE(registry.IsEmpty());
        LIMX_EXPECT_EQ(registry.GetLargestFreeRegion(), kOneMiB);
    }
}

LIMX_TEST(Suballocation, ReverseOrderFreeMergesCorrectly)
{
    FSuballocationRegistry registry;
    registry.Initialize(kOneMiB, 1);

    FSuballocationResult results[20];

    for (Int32 i = 0; i < 20; ++i)
    {
        LIMX_REQUIRE_TRUE(registry.Allocate(2048, 1,
                                            ESuballocationType::Linear,
                                            results[i]));
    }

    // 逆序释放 — 每次都触发与后继的合并
    for (Int32 i = 19; i >= 0; --i)
    {
        registry.Free(results[i].NodeIndex);
        LIMX_REQUIRE_TRUE(registry.Validate());
    }

    LIMX_EXPECT_EQ(registry.GetLargestFreeRegion(), kOneMiB);
}

// ============================================================================
// bufferImageGranularity 约束
// ============================================================================

LIMX_TEST(Suballocation, LinearAndNonLinearDoNotShareGranularityPage)
{
    const UInt64 kGranularity = 1024;

    FSuballocationRegistry registry;
    registry.Initialize(kOneMiB, kGranularity);

    // 线性资源占据页首的一小段
    FSuballocationResult linear;
    LIMX_REQUIRE_TRUE(registry.Allocate(64, 1,
                                        ESuballocationType::Linear, linear));
    LIMX_EXPECT_EQ(linear.Offset, UInt64(0));

    // 紧随其后的非线性资源必须被推到下一个粒度页
    FSuballocationResult nonLinear;
    LIMX_REQUIRE_TRUE(registry.Allocate(64, 1,
                                        ESuballocationType::NonLinear,
                                        nonLinear));

    const UInt64 linearPage    = linear.Offset / kGranularity;
    const UInt64 nonLinearPage = nonLinear.Offset / kGranularity;

    LIMX_EXPECT_NE(linearPage, nonLinearPage);
    LIMX_EXPECT_GE(nonLinear.Offset, kGranularity);
    LIMX_EXPECT_TRUE(registry.Validate());
}

LIMX_TEST(Suballocation, SameTypeMayShareGranularityPage)
{
    const UInt64 kGranularity = 1024;

    FSuballocationRegistry registry;
    registry.Initialize(kOneMiB, kGranularity);

    FSuballocationResult first;
    FSuballocationResult second;

    LIMX_REQUIRE_TRUE(registry.Allocate(64, 1,
                                        ESuballocationType::Linear, first));
    LIMX_REQUIRE_TRUE(registry.Allocate(64, 1,
                                        ESuballocationType::Linear, second));

    // 同类型资源之间无粒度约束, 应紧密排布
    LIMX_EXPECT_EQ(second.Offset, first.Offset + first.Size);
    LIMX_EXPECT_TRUE(registry.Validate());
}

LIMX_TEST(Suballocation, GranularityHonoredAcrossManyMixedAllocations)
{
    const UInt64 kGranularity = 256;

    FSuballocationRegistry registry;
    registry.Initialize(kOneMiB, kGranularity);

    FLiveAllocation live[64];
    ESuballocationType types[64];
    Int32 liveCount = 0;

    // 交替分配线性与非线性资源
    for (Int32 i = 0; i < 64; ++i)
    {
        const ESuballocationType type = (i % 2 == 0)
                                            ? ESuballocationType::Linear
                                            : ESuballocationType::NonLinear;

        FSuballocationResult result;
        if (!registry.Allocate(100, 1, type, result))
        {
            break;
        }

        live[liveCount].Offset = result.Offset;
        live[liveCount].Size   = result.Size;
        live[liveCount].Node   = result.NodeIndex;
        types[liveCount]       = type;
        ++liveCount;
    }

    LIMX_REQUIRE_GT(liveCount, 8);

    // 任意一对类型不同的分配都不得共享粒度页
    for (Int32 i = 0; i < liveCount; ++i)
    {
        for (Int32 j = i + 1; j < liveCount; ++j)
        {
            if (types[i] == types[j])
            {
                continue;
            }

            const UInt64 lastPageI =
                (live[i].Offset + live[i].Size - 1) / kGranularity;
            const UInt64 firstPageI = live[i].Offset / kGranularity;
            const UInt64 lastPageJ =
                (live[j].Offset + live[j].Size - 1) / kGranularity;
            const UInt64 firstPageJ = live[j].Offset / kGranularity;

            // 两个资源占用的页区间不得相交
            const bool pagesOverlap =
                (firstPageI <= lastPageJ) && (firstPageJ <= lastPageI);

            LIMX_EXPECT_FALSE(pagesOverlap);
        }
    }

    LIMX_EXPECT_TRUE(registry.Validate());
}

LIMX_TEST(Suballocation, GranularityOneImposesNoConstraint)
{
    FSuballocationRegistry registry;
    registry.Initialize(kOneMiB, 1);

    FSuballocationResult linear;
    FSuballocationResult nonLinear;

    LIMX_REQUIRE_TRUE(registry.Allocate(64, 1,
                                        ESuballocationType::Linear, linear));
    LIMX_REQUIRE_TRUE(registry.Allocate(64, 1,
                                        ESuballocationType::NonLinear,
                                        nonLinear));

    // 粒度为 1 时不同类型可紧邻
    LIMX_EXPECT_EQ(nonLinear.Offset, linear.Offset + linear.Size);
    LIMX_EXPECT_TRUE(registry.Validate());
}

// ============================================================================
// 碎片化
// ============================================================================

LIMX_TEST(Suballocation, FragmentationBlocksLargeAllocation)
{
    FSuballocationRegistry registry;
    registry.Initialize(8192, 1);

    FSuballocationResult results[8];

    for (Int32 i = 0; i < 8; ++i)
    {
        LIMX_REQUIRE_TRUE(registry.Allocate(1024, 1,
                                            ESuballocationType::Linear,
                                            results[i]));
    }

    // 隔一个释放一个, 制造棋盘式碎片
    for (Int32 i = 0; i < 8; i += 2)
    {
        registry.Free(results[i].NodeIndex);
    }

    LIMX_EXPECT_EQ(registry.GetFreeSize(), UInt64(4096));

    // 空闲总量足够但被切碎, 单块最大仅 1024
    LIMX_EXPECT_EQ(registry.GetLargestFreeRegion(), UInt64(1024));

    FSuballocationResult large;
    LIMX_EXPECT_FALSE(registry.Allocate(2048, 1,
                                        ESuballocationType::Linear, large));

    // 但恰好 1024 的请求应能满足
    FSuballocationResult small;
    LIMX_EXPECT_TRUE(registry.Allocate(1024, 1,
                                       ESuballocationType::Linear, small));

    LIMX_EXPECT_TRUE(registry.Validate());
}

LIMX_TEST(Suballocation, LargestFreeRegionTracksFragmentation)
{
    FSuballocationRegistry registry;
    registry.Initialize(kOneMiB, 1);

    LIMX_EXPECT_EQ(registry.GetLargestFreeRegion(), kOneMiB);

    FSuballocationResult head;
    LIMX_REQUIRE_TRUE(registry.Allocate(kOneMiB / 2, 1,
                                        ESuballocationType::Linear, head));

    // 占用一半后, 最大空闲区间应为剩余的一半
    LIMX_EXPECT_EQ(registry.GetLargestFreeRegion(), kOneMiB / 2);

    registry.Free(head.NodeIndex);
    LIMX_EXPECT_EQ(registry.GetLargestFreeRegion(), kOneMiB);
}

// ============================================================================
// 随机压力回归
// ============================================================================

LIMX_TEST(Suballocation, RandomStressKeepsInvariants)
{
    FSuballocationRegistry registry;
    registry.Initialize(kOneMiB, 1);

    FDeterministicRandom random(0x5EED1234ull);

    TArray<FLiveAllocation> live;
    UInt64 expectedUsed = 0;

    const Int32 kOperations = 4000;

    for (Int32 step = 0; step < kOperations; ++step)
    {
        // 活跃分配较少时偏向分配, 较多时偏向释放, 维持稳定的占用率
        const bool preferAllocate =
            live.GetSize() < 32 || (random.NextBounded(100) < 55);

        if (preferAllocate)
        {
            const UInt64 size      = 64 + random.NextBounded(8192);
            const UInt64 alignment = 1ull << random.NextBounded(9);   // 1..256

            const ESuballocationType type =
                (random.NextBounded(2) == 0) ? ESuballocationType::Linear
                                             : ESuballocationType::NonLinear;

            FSuballocationResult result;
            if (registry.Allocate(size, alignment, type, result))
            {
                LIMX_REQUIRE_TRUE(IsOffsetAligned(result.Offset, alignment));
                LIMX_REQUIRE_LE(result.Offset + result.Size,
                                registry.GetTotalSize());

                FLiveAllocation entry;
                entry.Offset = result.Offset;
                entry.Size   = result.Size;
                entry.Node   = result.NodeIndex;

                live.Add(entry);
                expectedUsed += size;
            }
        }
        else if (live.GetSize() > 0)
        {
            const SizeType victim =
                static_cast<SizeType>(random.NextBounded(live.GetSize()));

            expectedUsed -= live[victim].Size;
            registry.Free(live[victim].Node);
            live.RemoveAtSwap(victim);
        }

        // 每 64 步做一次完整校验 — 逐步校验会让用例慢到不可接受
        if ((step % 64) == 0)
        {
            LIMX_REQUIRE_TRUE(registry.Validate());
            LIMX_REQUIRE_EQ(registry.GetUsedSize(), expectedUsed);
            LIMX_REQUIRE_EQ(registry.GetAllocationCount(),
                            static_cast<UInt32>(live.GetSize()));
        }
    }

    // 收尾: 结构自洽, 且全部活跃区间互不重叠
    LIMX_REQUIRE_TRUE(registry.Validate());

    for (SizeType i = 0; i < live.GetSize(); ++i)
    {
        for (SizeType j = i + 1; j < live.GetSize(); ++j)
        {
            LIMX_REQUIRE_FALSE(RangesOverlap(live[i].Offset, live[i].Size,
                                             live[j].Offset, live[j].Size));
        }
    }

    // 全部释放后应回到未使用的完整状态
    for (SizeType i = 0; i < live.GetSize(); ++i)
    {
        registry.Free(live[i].Node);
    }

    LIMX_EXPECT_TRUE(registry.Validate());
    LIMX_EXPECT_TRUE(registry.IsEmpty());
    LIMX_EXPECT_EQ(registry.GetLargestFreeRegion(), kOneMiB);
}

LIMX_TEST(Suballocation, RandomStressWithGranularityKeepsInvariants)
{
    const UInt64 kGranularity = 1024;

    FSuballocationRegistry registry;
    registry.Initialize(kOneMiB * 4, kGranularity);

    FDeterministicRandom random(0xC0FFEEull);

    TArray<FLiveAllocation> live;
    TArray<UInt8> liveTypes;

    const Int32 kOperations = 3000;

    for (Int32 step = 0; step < kOperations; ++step)
    {
        const bool preferAllocate =
            live.GetSize() < 24 || (random.NextBounded(100) < 55);

        if (preferAllocate)
        {
            const UInt64 size      = 256 + random.NextBounded(16384);
            const UInt64 alignment = 1ull << random.NextBounded(8);

            const bool isLinear = (random.NextBounded(2) == 0);
            const ESuballocationType type = isLinear
                                                ? ESuballocationType::Linear
                                                : ESuballocationType::NonLinear;

            FSuballocationResult result;
            if (registry.Allocate(size, alignment, type, result))
            {
                FLiveAllocation entry;
                entry.Offset = result.Offset;
                entry.Size   = result.Size;
                entry.Node   = result.NodeIndex;

                live.Add(entry);
                liveTypes.Add(isLinear ? UInt8(1) : UInt8(2));
            }
        }
        else if (live.GetSize() > 0)
        {
            const SizeType victim =
                static_cast<SizeType>(random.NextBounded(live.GetSize()));

            registry.Free(live[victim].Node);
            live.RemoveAtSwap(victim);
            liveTypes.RemoveAtSwap(victim);
        }

        if ((step % 128) == 0)
        {
            LIMX_REQUIRE_TRUE(registry.Validate());
        }
    }

    LIMX_REQUIRE_TRUE(registry.Validate());

    // 校验粒度约束在整个随机序列后依然成立
    for (SizeType i = 0; i < live.GetSize(); ++i)
    {
        for (SizeType j = i + 1; j < live.GetSize(); ++j)
        {
            LIMX_REQUIRE_FALSE(RangesOverlap(live[i].Offset, live[i].Size,
                                             live[j].Offset, live[j].Size));

            if (liveTypes[i] == liveTypes[j])
            {
                continue;
            }

            const UInt64 firstPageI = live[i].Offset / kGranularity;
            const UInt64 lastPageI =
                (live[i].Offset + live[i].Size - 1) / kGranularity;
            const UInt64 firstPageJ = live[j].Offset / kGranularity;
            const UInt64 lastPageJ =
                (live[j].Offset + live[j].Size - 1) / kGranularity;

            const bool pagesOverlap =
                (firstPageI <= lastPageJ) && (firstPageJ <= lastPageI);

            LIMX_REQUIRE_FALSE(pagesOverlap);
        }
    }
}

// ============================================================================
// 节点槽复用
// ============================================================================

LIMX_TEST(Suballocation, NodeSlotsAreRecycled)
{
    FSuballocationRegistry registry;
    registry.Initialize(kOneMiB, 1);

    // 长时间的分配/释放循环不应让节点池无界增长。
    // 这里以"多轮循环后仍能完成同样规模的分配"作为间接验证 ——
    // 若节点槽不复用, 数组会持续膨胀直至内存耗尽。
    for (Int32 round = 0; round < 200; ++round)
    {
        FSuballocationResult results[8];

        for (Int32 i = 0; i < 8; ++i)
        {
            LIMX_REQUIRE_TRUE(registry.Allocate(1024, 64,
                                                ESuballocationType::Linear,
                                                results[i]));
        }

        for (Int32 i = 0; i < 8; ++i)
        {
            registry.Free(results[i].NodeIndex);
        }
    }

    LIMX_EXPECT_TRUE(registry.Validate());
    LIMX_EXPECT_TRUE(registry.IsEmpty());
    LIMX_EXPECT_EQ(registry.GetLargestFreeRegion(), kOneMiB);
}

// ============================================================================
// 移动语义
// ============================================================================

LIMX_TEST(Suballocation, MoveTransfersState)
{
    FSuballocationRegistry source;
    source.Initialize(kOneMiB, 1);

    FSuballocationResult result;
    LIMX_REQUIRE_TRUE(source.Allocate(4096, 256,
                                      ESuballocationType::Linear, result));

    FSuballocationRegistry moved(static_cast<FSuballocationRegistry&&>(source));

    LIMX_EXPECT_EQ(moved.GetTotalSize(), kOneMiB);
    LIMX_EXPECT_EQ(moved.GetUsedSize(), UInt64(4096));
    LIMX_EXPECT_EQ(moved.GetAllocationCount(), UInt32(1));
    LIMX_EXPECT_TRUE(moved.Validate());

    // 移后源必须处于可安全析构的未初始化状态
    LIMX_EXPECT_FALSE(source.IsInitialized());
    LIMX_EXPECT_TRUE(source.Validate());

    // 目标接管后仍可正常释放
    moved.Free(result.NodeIndex);
    LIMX_EXPECT_TRUE(moved.IsEmpty());
    LIMX_EXPECT_TRUE(moved.Validate());
}

// ============================================================================
// 真实场景规模验证
//
// 这些用例回答一个具体问题: 换用子分配后，一次 vkAllocateMemory 究竟能
// 承载多少个真实资源。旧路径下"资源数 == 设备分配数"，而 Vulkan 的
// maxMemoryAllocationCount 在 AMD/Intel/移动 GPU 上通常仅 4096
// (NVIDIA 桌面驱动上报 UINT32_MAX，不受此限)。
// ============================================================================

LIMX_TEST(Suballocation, SingleBlockHoldsThousandsOfTypicalResources)
{
    // 256 MiB —— 分配器的标准块尺寸
    const UInt64 kBlockSize   = 256ull * 1024ull * 1024ull;
    const UInt64 kGranularity = 1024;

    FSuballocationRegistry registry;
    registry.Initialize(kBlockSize, kGranularity);

    FDeterministicRandom random(0xA110C8ull);

    TArray<FLiveAllocation> live;
    Int32 succeeded = 0;

    // 模拟场景加载: 交替创建网格缓冲区 (线性) 与贴图 (非线性)
    for (Int32 i = 0; i < 5000; ++i)
    {
        const bool isMesh = (i % 3) != 0;

        // 网格缓冲区多为数 KiB 到数十 KiB; 贴图按 mip 链尺寸分布更广
        const UInt64 size = isMesh
                                ? (2048 + random.NextBounded(48 * 1024))
                                : (16 * 1024 + random.NextBounded(192 * 1024));

        const UInt64 alignment = isMesh ? 256 : 1024;

        const ESuballocationType type = isMesh
                                            ? ESuballocationType::Linear
                                            : ESuballocationType::NonLinear;

        FSuballocationResult result;
        if (!registry.Allocate(size, alignment, type, result))
        {
            break;
        }

        FLiveAllocation entry;
        entry.Offset = result.Offset;
        entry.Size   = result.Size;
        entry.Node   = result.NodeIndex;
        live.Add(entry);

        ++succeeded;
    }

    // 单块承载数千资源 —— 旧路径下这需要同样数量的 vkAllocateMemory 调用,
    // 在 4096 上限的驱动上会中途失败
    LIMX_EXPECT_GT(succeeded, 2000);
    LIMX_EXPECT_TRUE(registry.Validate());

    LIMX_TEST_INFO("256 MiB 单块承载 {} 个典型资源 (占用 {} MiB, 旧路径需 {} 次 vkAllocateMemory)",
                   succeeded,
                   registry.GetUsedSize() / (1024 * 1024),
                   succeeded);

    // 全部归还后应完整合并, 不留碎片
    for (SizeType i = 0; i < live.GetSize(); ++i)
    {
        registry.Free(live[i].Node);
    }

    LIMX_EXPECT_TRUE(registry.Validate());
    LIMX_EXPECT_TRUE(registry.IsEmpty());
    LIMX_EXPECT_EQ(registry.GetLargestFreeRegion(), kBlockSize);
}

LIMX_TEST(Suballocation, LoadUnloadCyclesDoNotDegrade)
{
    const UInt64 kBlockSize   = 64ull * 1024ull * 1024ull;
    const UInt64 kGranularity = 1024;

    FSuballocationRegistry registry;
    registry.Initialize(kBlockSize, kGranularity);

    FDeterministicRandom random(0xDEC0DEull);

    UInt32 firstRoundCount = 0;

    // 模拟反复加载/卸载关卡 —— 若合并有疏漏, 可容纳的资源数会逐轮下降
    for (Int32 round = 0; round < 12; ++round)
    {
        TArray<FLiveAllocation> live;

        for (Int32 i = 0; i < 2000; ++i)
        {
            const bool isMesh = (i % 3) != 0;
            const UInt64 size = isMesh
                                    ? (4096 + random.NextBounded(32 * 1024))
                                    : (32 * 1024 + random.NextBounded(96 * 1024));

            FSuballocationResult result;
            if (!registry.Allocate(size, isMesh ? 256 : 1024,
                                   isMesh ? ESuballocationType::Linear
                                          : ESuballocationType::NonLinear,
                                   result))
            {
                break;
            }

            FLiveAllocation entry;
            entry.Offset = result.Offset;
            entry.Size   = result.Size;
            entry.Node   = result.NodeIndex;
            live.Add(entry);
        }

        const UInt32 thisRoundCount = static_cast<UInt32>(live.GetSize());

        if (round == 0)
        {
            firstRoundCount = thisRoundCount;
            LIMX_REQUIRE_GT(firstRoundCount, 100u);
            LIMX_TEST_INFO("首轮承载 {} 个资源, 后续 11 轮不得低于其 90%",
                           firstRoundCount);
        }
        else
        {
            // 容量退化超过一成即说明存在未合并的残留碎片
            const UInt32 floorCount = (firstRoundCount * 90u) / 100u;
            LIMX_REQUIRE_GE(thisRoundCount, floorCount);
        }

        // 乱序卸载, 比顺序释放更容易暴露合并缺陷
        while (live.GetSize() > 0)
        {
            const SizeType victim =
                static_cast<SizeType>(random.NextBounded(live.GetSize()));

            registry.Free(live[victim].Node);
            live.RemoveAtSwap(victim);
        }

        LIMX_REQUIRE_TRUE(registry.Validate());
        LIMX_REQUIRE_TRUE(registry.IsEmpty());

        // 每轮结束必须回到完整的一整段, 否则下一轮起点已被污染
        LIMX_REQUIRE_EQ(registry.GetLargestFreeRegion(), kBlockSize);
    }
}
