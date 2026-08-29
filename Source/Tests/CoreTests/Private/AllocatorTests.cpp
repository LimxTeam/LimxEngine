/*******************************************************************************
 * 文件: AllocatorTests.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   内存分配器单元测试 — LinearAllocator / FStackAllocator / BlockAllocator /
 *   FPoolAllocator，覆盖对齐契约、容量边界、回收复用与标记回退
 *
 * 设计哲学:
 *   对齐是分配器的第一契约 — 返回未对齐的指针会在 SIMD 加载处崩溃，
 *   且崩溃点距离分配点极远，几乎不可能靠调试定位。因此每个分配器都用
 *   16/32/64/128/256 五档对齐逐一验证返回地址取模为零。
 *
 *   容量边界要测"恰好"与"超出" — 分配器最常见的缺陷是容量检查写成
 *   严格小于或漏算对齐填充，只有精确落在剩余容量上的用例能暴露它。
 *
 *   写入-回读验证可用性 — 仅断言指针非空不足以证明内存真的可用；
 *   用例向分配到的区域写入模式数据并回读，确认区域间不重叠、不越界。
 *
 * 技术特性:
 *   - 多档对齐验证覆盖 SSE(16)/AVX(32)/AVX-512(64) 及超对齐场景
 *   - 重叠检测: 连续分配多块并写入唯一模式, 逐块回读校验
 *   - BlockAllocator 的自由列表复用通过"释放后再分配得到同一地址"验证
 *
 * 依赖关系:
 *   内部: CoreTests/CoreTestsMinimal.h
 *
 * 注意事项:
 *   LinearAllocator 的 Deallocate 是空操作 — 这是设计使然, 用例不应期待回收
 *
 ******************************************************************************/

#include "CoreTests/CoreTestsMinimal.h"

using namespace Limx;

namespace
{

/// 判断地址是否满足对齐要求
bool IsAligned(const void* pointer, SizeType alignment)
{
    return (reinterpret_cast<UInt64>(pointer) %
            static_cast<UInt64>(alignment)) == 0;
}

/// 向内存块写入以种子派生的模式, 用于验证区域独立性
void FillPattern(void* memory, SizeType size, UInt8 seed)
{
    UInt8* bytes = static_cast<UInt8*>(memory);
    for (SizeType i = 0; i < size; ++i)
    {
        bytes[i] = static_cast<UInt8>(seed + static_cast<UInt8>(i));
    }
}

/// 校验内存块内容与写入的模式一致
bool CheckPattern(const void* memory, SizeType size, UInt8 seed)
{
    const UInt8* bytes = static_cast<const UInt8*>(memory);
    for (SizeType i = 0; i < size; ++i)
    {
        if (bytes[i] != static_cast<UInt8>(seed + static_cast<UInt8>(i)))
        {
            return false;
        }
    }

    return true;
}

} // namespace

// ============================================================================
// LinearAllocator — 线性分配, 只进不退
// ============================================================================

LIMX_TEST(LinearAllocator, StartsEmpty)
{
    LinearAllocator allocator(1024);

    LIMX_EXPECT_EQ(allocator.GetUsed(), SizeType(0));
    LIMX_EXPECT_EQ(allocator.GetCapacity(), SizeType(1024));
    LIMX_EXPECT_EQ(allocator.GetRemaining(), SizeType(1024));
}

LIMX_TEST(LinearAllocator, AllocateAdvancesOffset)
{
    LinearAllocator allocator(1024);

    void* first = allocator.Allocate(64, 16);
    LIMX_REQUIRE_NOT_NULL(first);

    LIMX_EXPECT_GE(allocator.GetUsed(), SizeType(64));
    LIMX_EXPECT_LE(allocator.GetRemaining(), SizeType(1024 - 64));
}

LIMX_TEST(LinearAllocator, RespectsAllAlignments)
{
    LinearAllocator allocator(64 * 1024);

    for (SizeType alignment = 16; alignment <= 256; alignment *= 2)
    {
        void* block = allocator.Allocate(32, alignment);

        LIMX_REQUIRE_NOT_NULL(block);
        LIMX_EXPECT_TRUE(IsAligned(block, alignment));
    }
}

LIMX_TEST(LinearAllocator, SuccessiveBlocksDoNotOverlap)
{
    LinearAllocator allocator(8192);

    const SizeType kBlockSize = 64;
    void* blocks[16] = {};

    for (SizeType i = 0; i < 16; ++i)
    {
        blocks[i] = allocator.Allocate(kBlockSize, 16);
        LIMX_REQUIRE_NOT_NULL(blocks[i]);
        FillPattern(blocks[i], kBlockSize, static_cast<UInt8>(i * 7 + 1));
    }

    // 若区域重叠, 后写入的模式会覆盖先前的块
    for (SizeType i = 0; i < 16; ++i)
    {
        LIMX_EXPECT_TRUE(
            CheckPattern(blocks[i], kBlockSize, static_cast<UInt8>(i * 7 + 1)));
    }
}

LIMX_TEST(LinearAllocator, ExhaustionReturnsNull)
{
    LinearAllocator allocator(256);

    void* first = allocator.Allocate(200, 16);
    LIMX_REQUIRE_NOT_NULL(first);

    // 剩余空间不足以容纳请求, 必须返回 nullptr 而非越界
    void* second = allocator.Allocate(200, 16);
    LIMX_EXPECT_NULL(second);
}

LIMX_TEST(LinearAllocator, ResetReclaimsEverything)
{
    LinearAllocator allocator(1024);

    LIMX_UNUSED(allocator.Allocate(512, 16));
    LIMX_REQUIRE_GT(allocator.GetUsed(), SizeType(0));

    allocator.Reset();

    LIMX_EXPECT_EQ(allocator.GetUsed(), SizeType(0));
    LIMX_EXPECT_EQ(allocator.GetRemaining(), allocator.GetCapacity());

    // 重置后应能再次分配同样大小
    LIMX_EXPECT_NOT_NULL(allocator.Allocate(512, 16));
}

LIMX_TEST(LinearAllocator, MarkAndRestoreRollsBack)
{
    LinearAllocator allocator(2048);

    LIMX_UNUSED(allocator.Allocate(128, 16));
    const SizeType mark = allocator.SaveMark();
    const SizeType usedAtMark = allocator.GetUsed();

    LIMX_UNUSED(allocator.Allocate(256, 16));
    LIMX_UNUSED(allocator.Allocate(256, 16));

    LIMX_REQUIRE_GT(allocator.GetUsed(), usedAtMark);

    allocator.RestoreMark(mark);

    LIMX_EXPECT_EQ(allocator.GetUsed(), usedAtMark);
}

LIMX_TEST(LinearAllocator, DeallocateIsNoOpByDesign)
{
    LinearAllocator allocator(1024);

    void* block = allocator.Allocate(128, 16);
    LIMX_REQUIRE_NOT_NULL(block);
    const SizeType usedBefore = allocator.GetUsed();

    // 线性分配器不支持单块回收 — Deallocate 是空操作
    allocator.Deallocate(block);

    LIMX_EXPECT_EQ(allocator.GetUsed(), usedBefore);
}

// ============================================================================
// FStackAllocator — LIFO 分配
// ============================================================================

LIMX_TEST(FStackAllocator, StartsEmpty)
{
    FStackAllocator allocator(4096);

    LIMX_EXPECT_TRUE(allocator.IsEmpty());
    LIMX_EXPECT_EQ(allocator.GetUsed(), SizeType(0));
    LIMX_EXPECT_EQ(allocator.GetCapacity(), SizeType(4096));
}

LIMX_TEST(FStackAllocator, AllocateTypedReturnsAlignedStorage)
{
    FStackAllocator allocator(4096);

    Int32* integers = allocator.AllocateTyped<Int32>(16);
    LIMX_REQUIRE_NOT_NULL(integers);
    LIMX_EXPECT_TRUE(IsAligned(integers, alignof(Int32)));

    for (Int32 i = 0; i < 16; ++i)
    {
        integers[i] = i * 5;
    }

    for (Int32 i = 0; i < 16; ++i)
    {
        LIMX_EXPECT_EQ(integers[i], i * 5);
    }
}

LIMX_TEST(FStackAllocator, MarkerFreesInLifoOrder)
{
    FStackAllocator allocator(4096);

    LIMX_UNUSED(allocator.Allocate(128, 16));
    const StackMarker marker = allocator.GetMarker();
    const SizeType usedAtMarker = allocator.GetUsed();

    LIMX_UNUSED(allocator.Allocate(256, 16));
    LIMX_UNUSED(allocator.Allocate(512, 16));

    LIMX_REQUIRE_GT(allocator.GetUsed(), usedAtMarker);

    allocator.FreeToMarker(marker);

    LIMX_EXPECT_EQ(allocator.GetUsed(), usedAtMarker);
}

LIMX_TEST(FStackAllocator, ScopeMarkerRestoresOnDestruction)
{
    FStackAllocator allocator(4096);

    LIMX_UNUSED(allocator.Allocate(64, 16));
    const SizeType usedBeforeScope = allocator.GetUsed();

    {
        FScopeMarker scope(allocator);

        LIMX_UNUSED(allocator.Allocate(512, 16));
        LIMX_UNUSED(allocator.Allocate(512, 16));

        LIMX_EXPECT_GT(allocator.GetUsed(), usedBeforeScope);
    }

    // 离开作用域自动回退到进入时的位置
    LIMX_EXPECT_EQ(allocator.GetUsed(), usedBeforeScope);
}

LIMX_TEST(FStackAllocator, ExhaustionReturnsNull)
{
    FStackAllocator allocator(256);

    LIMX_REQUIRE_NOT_NULL(allocator.Allocate(200, 16));
    LIMX_EXPECT_NULL(allocator.Allocate(200, 16));
}

LIMX_TEST(FStackAllocator, ResetEmptiesStack)
{
    FStackAllocator allocator(1024);

    LIMX_UNUSED(allocator.Allocate(512, 16));
    allocator.Reset();

    LIMX_EXPECT_TRUE(allocator.IsEmpty());
    LIMX_EXPECT_EQ(allocator.GetUsed(), SizeType(0));
}

LIMX_TEST(FStackAllocator, RespectsAllAlignments)
{
    FStackAllocator allocator(64 * 1024);

    for (SizeType alignment = 16; alignment <= 256; alignment *= 2)
    {
        void* block = allocator.Allocate(32, alignment);

        LIMX_REQUIRE_NOT_NULL(block);
        LIMX_EXPECT_TRUE(IsAligned(block, alignment));
    }
}

// ============================================================================
// BlockAllocator — 定长块 + 自由列表
// ============================================================================

LIMX_TEST(BlockAllocator, ReportsConfiguredBlockSize)
{
    BlockAllocator allocator(64);

    LIMX_EXPECT_GE(allocator.GetBlockSize(), SizeType(64));
    LIMX_EXPECT_EQ(allocator.GetAllocatedCount(), SizeType(0));
}

LIMX_TEST(BlockAllocator, AllocateTracksCount)
{
    BlockAllocator allocator(64);

    void* first  = allocator.Allocate(64, 16);
    void* second = allocator.Allocate(64, 16);

    LIMX_REQUIRE_NOT_NULL(first);
    LIMX_REQUIRE_NOT_NULL(second);
    LIMX_EXPECT_NE(first, second);
    LIMX_EXPECT_EQ(allocator.GetAllocatedCount(), SizeType(2));
}

LIMX_TEST(BlockAllocator, FreedBlockIsReused)
{
    BlockAllocator allocator(64);

    void* first = allocator.Allocate(64, 16);
    LIMX_REQUIRE_NOT_NULL(first);

    allocator.Deallocate(first);
    LIMX_EXPECT_EQ(allocator.GetAllocatedCount(), SizeType(0));

    // 自由列表应把刚归还的块原样交回
    void* second = allocator.Allocate(64, 16);
    LIMX_EXPECT_EQ(second, first);
    LIMX_EXPECT_EQ(allocator.GetAllocatedCount(), SizeType(1));
}

LIMX_TEST(BlockAllocator, GrowsBeyondSinglePage)
{
    BlockAllocator allocator(32);

    // 超过单页块数, 强制分配新页
    const SizeType kCount = 200;
    void* blocks[kCount] = {};

    for (SizeType i = 0; i < kCount; ++i)
    {
        blocks[i] = allocator.Allocate(32, 16);
        LIMX_REQUIRE_NOT_NULL(blocks[i]);
        FillPattern(blocks[i], 32, static_cast<UInt8>(i));
    }

    LIMX_EXPECT_EQ(allocator.GetAllocatedCount(), kCount);

    // 跨页分配的块之间不得重叠
    for (SizeType i = 0; i < kCount; ++i)
    {
        LIMX_EXPECT_TRUE(CheckPattern(blocks[i], 32, static_cast<UInt8>(i)));
    }

    for (SizeType i = 0; i < kCount; ++i)
    {
        allocator.Deallocate(blocks[i]);
    }

    LIMX_EXPECT_EQ(allocator.GetAllocatedCount(), SizeType(0));
}

LIMX_TEST(BlockAllocator, DeallocateNullIsSafe)
{
    BlockAllocator allocator(64);

    allocator.Deallocate(nullptr);

    LIMX_EXPECT_EQ(allocator.GetAllocatedCount(), SizeType(0));
}

LIMX_TEST(BlockAllocator, ResetReclaimsAll)
{
    BlockAllocator allocator(64);

    for (Int32 i = 0; i < 10; ++i)
    {
        LIMX_UNUSED(allocator.Allocate(64, 16));
    }

    LIMX_REQUIRE_EQ(allocator.GetAllocatedCount(), SizeType(10));

    allocator.Reset();

    LIMX_EXPECT_EQ(allocator.GetAllocatedCount(), SizeType(0));
    LIMX_EXPECT_NOT_NULL(allocator.Allocate(64, 16));
}

// ============================================================================
// FPoolAllocator — 分桶池
// ============================================================================

LIMX_TEST(FPoolAllocator, BucketSizesAreAscending)
{
    // 分桶尺寸必须单调递增, 否则尺寸到桶的映射会错位
    for (SizeType i = 1; i < FPoolAllocator::GetBucketCount(); ++i)
    {
        LIMX_EXPECT_GT(FPoolAllocator::GetBucketSize(i),
                       FPoolAllocator::GetBucketSize(i - 1));
    }
}

LIMX_TEST(FPoolAllocator, AllocatesAcrossAllBuckets)
{
    FPoolAllocator allocator;

    for (SizeType i = 0; i < FPoolAllocator::GetBucketCount(); ++i)
    {
        const SizeType size = FPoolAllocator::GetBucketSize(i);

        void* block = allocator.Allocate(size, 8);
        LIMX_REQUIRE_NOT_NULL(block);

        FillPattern(block, size, static_cast<UInt8>(i + 1));
        LIMX_EXPECT_TRUE(CheckPattern(block, size, static_cast<UInt8>(i + 1)));

        allocator.Deallocate(block, size);
    }
}

LIMX_TEST(FPoolAllocator, HandlesSizeLargerThanMaxPool)
{
    FPoolAllocator allocator;

    const SizeType oversized = FPoolAllocator::GetMaxPoolSize() * 4;

    // 超出池管理范围的请求必须回退到通用路径而非失败
    void* block = allocator.Allocate(oversized, 16);
    LIMX_REQUIRE_NOT_NULL(block);

    FillPattern(block, oversized, 0xA5);
    LIMX_EXPECT_TRUE(CheckPattern(block, oversized, 0xA5));

    allocator.Deallocate(block, oversized);
}

LIMX_TEST(FPoolAllocator, ManyAllocationsDoNotOverlap)
{
    FPoolAllocator allocator;

    const SizeType kCount = 64;
    const SizeType kSize  = 64;
    void* blocks[kCount] = {};

    for (SizeType i = 0; i < kCount; ++i)
    {
        blocks[i] = allocator.Allocate(kSize, 8);
        LIMX_REQUIRE_NOT_NULL(blocks[i]);
        FillPattern(blocks[i], kSize, static_cast<UInt8>(i * 3 + 1));
    }

    for (SizeType i = 0; i < kCount; ++i)
    {
        LIMX_EXPECT_TRUE(
            CheckPattern(blocks[i], kSize, static_cast<UInt8>(i * 3 + 1)));
    }

    for (SizeType i = 0; i < kCount; ++i)
    {
        allocator.Deallocate(blocks[i], kSize);
    }
}

LIMX_TEST(FPoolAllocator, DeallocateNullIsSafe)
{
    FPoolAllocator allocator;

    allocator.Deallocate(nullptr);
    allocator.Deallocate(nullptr, 64);
}

// ============================================================================
// 默认分配器
// ============================================================================

LIMX_TEST(DefaultAllocator, RespectsAllAlignments)
{
    IAllocator& allocator = GetDefaultAllocator();

    for (SizeType alignment = 16; alignment <= 256; alignment *= 2)
    {
        void* block = allocator.Allocate(128, alignment);

        LIMX_REQUIRE_NOT_NULL(block);
        LIMX_EXPECT_TRUE(IsAligned(block, alignment));

        allocator.Deallocate(block);
    }
}

LIMX_TEST(DefaultAllocator, ReallocatePreservesContent)
{
    IAllocator& allocator = GetDefaultAllocator();

    UInt8* block = static_cast<UInt8*>(allocator.Allocate(64, 16));
    LIMX_REQUIRE_NOT_NULL(block);

    FillPattern(block, 64, 0x11);

    UInt8* grown = static_cast<UInt8*>(allocator.Reallocate(block, 256, 16));
    LIMX_REQUIRE_NOT_NULL(grown);

    // 扩容后原有 64 字节内容必须保留
    LIMX_EXPECT_TRUE(CheckPattern(grown, 64, 0x11));

    allocator.Deallocate(grown);
}

LIMX_TEST(DefaultAllocator, DeallocateNullIsSafe)
{
    GetDefaultAllocator().Deallocate(nullptr);
}
