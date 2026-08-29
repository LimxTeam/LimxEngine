/*******************************************************************************
 * 文件: SmokeTests.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   测试框架自检 — 验证注册、断言、值描述、追踪分配器本身工作正常
 *   这些用例失败意味着框架不可信, 其余测试结果均无意义
 *
 * 设计哲学:
 *   先验证尺子再量物 — 框架自检必须最先通过, 因此本文件用例只依赖
 *   最基础的语言设施, 不引入任何被测的 Core 容器。
 *
 * 依赖关系:
 *   内部: CoreTests/CoreTestsMinimal.h
 *
 ******************************************************************************/

#include "CoreTests/CoreTestsMinimal.h"

// ============================================================================
// 断言宏自检
// ============================================================================

LIMX_TEST(Framework, BooleanChecks)
{
    LIMX_EXPECT_TRUE(true);
    LIMX_EXPECT_FALSE(false);
    LIMX_EXPECT_TRUE(1 + 1 == 2);
}

LIMX_TEST(Framework, IntegerEquality)
{
    const Limx::Int32 value = 42;

    LIMX_EXPECT_EQ(value, 42);
    LIMX_EXPECT_NE(value, 43);
    LIMX_EXPECT_LT(value, 43);
    LIMX_EXPECT_LE(value, 42);
    LIMX_EXPECT_GT(value, 41);
    LIMX_EXPECT_GE(value, 42);
}

LIMX_TEST(Framework, FloatingPointTolerance)
{
    const Limx::Float32 computed = 0.1f + 0.2f;

    LIMX_EXPECT_NEAR(computed, 0.3f, 1e-6f);
}

LIMX_TEST(Framework, PointerChecks)
{
    Limx::Int32  value   = 7;
    Limx::Int32* pointer = &value;
    Limx::Int32* nullPointer = nullptr;

    LIMX_EXPECT_NOT_NULL(pointer);
    LIMX_EXPECT_NULL(nullPointer);
    LIMX_REQUIRE_NOT_NULL(pointer);
    LIMX_EXPECT_EQ(*pointer, 7);
}

LIMX_TEST(Framework, StringEquality)
{
    const Limx::AnsiChar* text = "limx";

    LIMX_EXPECT_STREQ(text, "limx");
}

// ============================================================================
// 追踪分配器自检
// ============================================================================

LIMX_TEST(TrackingAllocator, CountsAllocationAndDeallocation)
{
    Limx::FTrackingAllocator allocator;

    LIMX_EXPECT_EQ(allocator.GetAllocationCount(), 0ull);
    LIMX_EXPECT_FALSE(allocator.HasLeaks());

    void* block = allocator.Allocate(64);
    LIMX_REQUIRE_NOT_NULL(block);

    LIMX_EXPECT_EQ(allocator.GetAllocationCount(), 1ull);
    LIMX_EXPECT_EQ(allocator.GetLiveAllocationCount(), 1ull);
    LIMX_EXPECT_EQ(allocator.GetCurrentBytes(), Limx::SizeType(64));
    LIMX_EXPECT_TRUE(allocator.HasLeaks());

    allocator.Deallocate(block);

    LIMX_EXPECT_EQ(allocator.GetDeallocationCount(), 1ull);
    LIMX_EXPECT_EQ(allocator.GetLiveAllocationCount(), 0ull);
    LIMX_EXPECT_EQ(allocator.GetCurrentBytes(), Limx::SizeType(0));
    LIMX_EXPECT_FALSE(allocator.HasLeaks());
}

LIMX_TEST(TrackingAllocator, RespectsRequestedAlignment)
{
    Limx::FTrackingAllocator allocator;

    // 逐级验证对齐 — 头部内联方案不得破坏用户请求的对齐语义
    for (Limx::SizeType alignment = 16; alignment <= 256; alignment *= 2)
    {
        void* block = allocator.Allocate(32, alignment);
        LIMX_REQUIRE_NOT_NULL(block);

        const Limx::UInt64 address = reinterpret_cast<Limx::UInt64>(block);
        LIMX_EXPECT_EQ(address % static_cast<Limx::UInt64>(alignment), 0ull);

        allocator.Deallocate(block);
    }

    LIMX_EXPECT_FALSE(allocator.HasLeaks());
}

LIMX_TEST(TrackingAllocator, ReportsAllocationSize)
{
    Limx::FTrackingAllocator allocator;

    void* block = allocator.Allocate(123);
    LIMX_REQUIRE_NOT_NULL(block);

    LIMX_EXPECT_EQ(allocator.GetAllocationSize(block), Limx::SizeType(123));

    allocator.Deallocate(block);
}

LIMX_TEST(TrackingAllocator, ReallocatePreservesContent)
{
    Limx::FTrackingAllocator allocator;

    Limx::UInt8* block = static_cast<Limx::UInt8*>(allocator.Allocate(16));
    LIMX_REQUIRE_NOT_NULL(block);

    for (Limx::SizeType i = 0; i < 16; ++i)
    {
        block[i] = static_cast<Limx::UInt8>(i);
    }

    Limx::UInt8* grown = static_cast<Limx::UInt8*>(allocator.Reallocate(block, 64));
    LIMX_REQUIRE_NOT_NULL(grown);

    for (Limx::SizeType i = 0; i < 16; ++i)
    {
        LIMX_EXPECT_EQ(grown[i], static_cast<Limx::UInt8>(i));
    }

    allocator.Deallocate(grown);
    LIMX_EXPECT_FALSE(allocator.HasLeaks());
}

LIMX_TEST(TrackingAllocator, PeakTracksHighWaterMark)
{
    Limx::FTrackingAllocator allocator;

    void* first  = allocator.Allocate(100);
    void* second = allocator.Allocate(200);
    LIMX_REQUIRE_NOT_NULL(first);
    LIMX_REQUIRE_NOT_NULL(second);

    LIMX_EXPECT_EQ(allocator.GetPeakBytes(), Limx::SizeType(300));

    allocator.Deallocate(second);
    LIMX_EXPECT_EQ(allocator.GetCurrentBytes(), Limx::SizeType(100));

    // 峰值不随释放回落
    LIMX_EXPECT_EQ(allocator.GetPeakBytes(), Limx::SizeType(300));

    allocator.Deallocate(first);
}

LIMX_TEST(TrackingAllocator, LeakScopeDetectsBaseline)
{
    Limx::FTrackingAllocator allocator;

    void* outer = allocator.Allocate(32);
    LIMX_REQUIRE_NOT_NULL(outer);

    {
        Limx::FLeakScope scope(allocator);

        void* inner = allocator.Allocate(64);
        LIMX_REQUIRE_NOT_NULL(inner);

        LIMX_EXPECT_TRUE(scope.HasLeaked());
        LIMX_EXPECT_EQ(scope.GetLeakedAllocationCount(), Limx::Int64(1));
        LIMX_EXPECT_EQ(scope.GetLeakedBytes(), Limx::Int64(64));

        allocator.Deallocate(inner);

        // 释放后相对基线守恒 — 外层的 outer 不计入本作用域
        LIMX_EXPECT_FALSE(scope.HasLeaked());
    }

    allocator.Deallocate(outer);
    LIMX_EXPECT_FALSE(allocator.HasLeaks());
}
