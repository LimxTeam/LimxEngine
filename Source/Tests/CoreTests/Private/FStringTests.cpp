/*******************************************************************************
 * 文件: FStringTests.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   FString 单元测试 — 覆盖 SSO 边界、堆转换、拼接、查找、切片、
 *   拷贝/移动语义与分配守恒
 *
 * 设计哲学:
 *   围绕 SSO 临界点设计 — FString 的 kSSOCapacity 为 30，长度 29/30/31
 *   分别落在"栈内""恰好填满""必须转堆"三种状态上。SSO 实现的缺陷几乎
 *   全部集中在这条边界及其跨越过程中，因此用例密集覆盖该区间而非泛泛取值。
 *
 *   移动语义需分状态验证 — SSO 状态下移动必须逐字节拷贝内联缓冲区，
 *   堆状态下才能转移指针。两条路径的正确性必须分别断言。
 *
 * 技术特性:
 *   - 长度 0/1/29/30/31/大串 六档覆盖 SSO 全状态空间
 *   - 分配次数断言验证 SSO 阶段确实零堆分配
 *   - 查找类接口的未命中一律返回 kNPos, 用例逐个校验
 *
 * 依赖关系:
 *   内部: CoreTests/CoreTestsMinimal.h
 *
 * 注意事项:
 *   FString::kSSOCapacity 为 30 — 若该常量变更, 本文件的边界用例需同步调整
 *
 ******************************************************************************/

#include "CoreTests/CoreTestsMinimal.h"

using namespace Limx;

namespace
{

/// 构造指定长度的重复字符串 — 用于精确命中 SSO 边界
FString MakeRepeated(AnsiChar character, SizeType length)
{
    FString result;
    for (SizeType i = 0; i < length; ++i)
    {
        result.AppendChar(character);
    }

    return result;
}

} // namespace

// ============================================================================
// 构造与基本状态
// ============================================================================

LIMX_TEST(FString, DefaultConstructedIsEmpty)
{
    FString text;

    LIMX_EXPECT_EQ(text.GetLength(), SizeType(0));
    LIMX_EXPECT_TRUE(text.IsEmpty());
    LIMX_EXPECT_STREQ(text.GetCStr(), "");
}

LIMX_TEST(FString, ConstructFromCString)
{
    FString text("limx");

    LIMX_EXPECT_EQ(text.GetLength(), SizeType(4));
    LIMX_EXPECT_FALSE(text.IsEmpty());
    LIMX_EXPECT_STREQ(text.GetCStr(), "limx");
}

LIMX_TEST(FString, ConstructFromBufferWithExplicitLength)
{
    // 只取前 4 个字符, 忽略其后内容
    FString text("limxengine", 4);

    LIMX_EXPECT_EQ(text.GetLength(), SizeType(4));
    LIMX_EXPECT_STREQ(text.GetCStr(), "limx");
}

LIMX_TEST(FString, IndexAccessReturnsCharacters)
{
    FString text("abc");

    LIMX_EXPECT_EQ(text[0], 'a');
    LIMX_EXPECT_EQ(text[1], 'b');
    LIMX_EXPECT_EQ(text[2], 'c');
}

LIMX_TEST(FString, IsNullTerminated)
{
    FString text("abc");

    // GetCStr 必须可直接交给 C 接口
    LIMX_EXPECT_EQ(text.GetCStr()[text.GetLength()], '\0');
}

// ============================================================================
// SSO 边界 — kSSOCapacity == 30
// ============================================================================

LIMX_TEST(FString, ShortStringUsesNoHeapAllocation)
{
    FTrackingAllocator allocator;

    {
        FString text(allocator);
        text.Append("0123456789");   // 10 字符, 远在 SSO 容量内

        LIMX_EXPECT_EQ(text.GetLength(), SizeType(10));
        LIMX_EXPECT_EQ(allocator.GetAllocationCount(), 0ull);
    }

    LIMX_EXPECT_FALSE(allocator.HasLeaks());
}

LIMX_TEST(FString, ExactlySSOCapacityStaysInline)
{
    FTrackingAllocator allocator;

    {
        FString text(allocator);
        for (SizeType i = 0; i < FString::kSSOCapacity; ++i)
        {
            text.AppendChar('x');
        }

        LIMX_EXPECT_EQ(text.GetLength(), FString::kSSOCapacity);

        // 恰好填满内联缓冲区时不应转堆
        LIMX_EXPECT_EQ(allocator.GetAllocationCount(), 0ull);
    }

    LIMX_EXPECT_FALSE(allocator.HasLeaks());
}

LIMX_TEST(FString, OneOverSSOCapacityMovesToHeap)
{
    FTrackingAllocator allocator;

    {
        FString text(allocator);
        for (SizeType i = 0; i < FString::kSSOCapacity + 1; ++i)
        {
            text.AppendChar('y');
        }

        LIMX_EXPECT_EQ(text.GetLength(), FString::kSSOCapacity + 1);

        // 越过内联容量必须转为堆存储
        LIMX_EXPECT_GT(allocator.GetAllocationCount(), 0ull);

        // 转堆过程中内容必须完整保留
        for (SizeType i = 0; i < text.GetLength(); ++i)
        {
            LIMX_EXPECT_EQ(text[i], 'y');
        }
    }

    LIMX_EXPECT_FALSE(allocator.HasLeaks());
}

LIMX_TEST(FString, SSOToHeapTransitionPreservesContent)
{
    FString text("0123456789012345678901234567");  // 28 字符, 仍在 SSO

    LIMX_REQUIRE_EQ(text.GetLength(), SizeType(28));

    text.Append("ABCDEFGHIJ");                      // 追加后 38 字符, 转堆

    LIMX_REQUIRE_EQ(text.GetLength(), SizeType(38));
    LIMX_EXPECT_STREQ(text.GetCStr(),
                      "0123456789012345678901234567ABCDEFGHIJ");
}

LIMX_TEST(FString, LargeStringRoundTrips)
{
    const SizeType kLength = 1024;
    FString text = MakeRepeated('z', kLength);

    LIMX_REQUIRE_EQ(text.GetLength(), kLength);

    for (SizeType i = 0; i < kLength; ++i)
    {
        LIMX_EXPECT_EQ(text[i], 'z');
    }

    LIMX_EXPECT_EQ(text.GetCStr()[kLength], '\0');
}

// ============================================================================
// 追加
// ============================================================================

LIMX_TEST(FString, AppendCStringConcatenates)
{
    FString text("limx");
    text.Append(" engine");

    LIMX_EXPECT_STREQ(text.GetCStr(), "limx engine");
    LIMX_EXPECT_EQ(text.GetLength(), SizeType(11));
}

LIMX_TEST(FString, AppendStringConcatenates)
{
    FString text("limx");
    FString suffix(" engine");

    text.Append(suffix);

    LIMX_EXPECT_STREQ(text.GetCStr(), "limx engine");

    // 源不应被修改
    LIMX_EXPECT_STREQ(suffix.GetCStr(), " engine");
}

LIMX_TEST(FString, AppendEmptyIsNoOp)
{
    FString text("limx");
    text.Append("");

    LIMX_EXPECT_STREQ(text.GetCStr(), "limx");
    LIMX_EXPECT_EQ(text.GetLength(), SizeType(4));
}

LIMX_TEST(FString, AppendNullPointerIsSafe)
{
    FString text("limx");
    text.Append(static_cast<const AnsiChar*>(nullptr));

    LIMX_EXPECT_STREQ(text.GetCStr(), "limx");
}

LIMX_TEST(FString, AppendCharGrowsByOne)
{
    FString text;

    text.AppendChar('a');
    text.AppendChar('b');

    LIMX_EXPECT_EQ(text.GetLength(), SizeType(2));
    LIMX_EXPECT_STREQ(text.GetCStr(), "ab");
}

LIMX_TEST(FString, RepeatedAppendStaysConsistent)
{
    FString text;

    for (Int32 i = 0; i < 200; ++i)
    {
        text.Append("ab");
    }

    LIMX_EXPECT_EQ(text.GetLength(), SizeType(400));
    LIMX_EXPECT_EQ(text[0], 'a');
    LIMX_EXPECT_EQ(text[399], 'b');
}

// ============================================================================
// 清空
// ============================================================================

LIMX_TEST(FString, ClearEmptiesString)
{
    FString text("limx engine renderer");
    text.Clear();

    LIMX_EXPECT_TRUE(text.IsEmpty());
    LIMX_EXPECT_EQ(text.GetLength(), SizeType(0));
    LIMX_EXPECT_STREQ(text.GetCStr(), "");
}

LIMX_TEST(FString, ClearedStringIsReusable)
{
    FString text("something long enough to be on the heap for sure");
    text.Clear();
    text.Append("reused");

    LIMX_EXPECT_STREQ(text.GetCStr(), "reused");
    LIMX_EXPECT_EQ(text.GetLength(), SizeType(6));
}

// ============================================================================
// 查找
// ============================================================================

LIMX_TEST(FString, FindLocatesSubstring)
{
    FString text("limx engine");

    LIMX_EXPECT_EQ(text.Find("limx"), SizeType(0));
    LIMX_EXPECT_EQ(text.Find("engine"), SizeType(5));
    LIMX_EXPECT_EQ(text.Find(" "), SizeType(4));
}

LIMX_TEST(FString, FindMissingReturnsNPos)
{
    FString text("limx");

    LIMX_EXPECT_EQ(text.Find("engine"), FString::kNPos);
    LIMX_EXPECT_FALSE(text.Contains("engine"));
}

LIMX_TEST(FString, FindCharLocatesCharacter)
{
    FString text("limx");

    LIMX_EXPECT_EQ(text.FindChar('m'), SizeType(2));
    LIMX_EXPECT_EQ(text.FindChar('z'), FString::kNPos);
}

LIMX_TEST(FString, ContainsMatchesFind)
{
    FString text("limx engine renderer");

    LIMX_EXPECT_TRUE(text.Contains("engine"));
    LIMX_EXPECT_TRUE(text.Contains("limx"));
    LIMX_EXPECT_TRUE(text.Contains("renderer"));
    LIMX_EXPECT_FALSE(text.Contains("vulkan"));
}

LIMX_TEST(FString, StartsWithAndEndsWith)
{
    FString text("limx engine");

    LIMX_EXPECT_TRUE(text.StartsWith("limx"));
    LIMX_EXPECT_TRUE(text.StartsWith(""));
    LIMX_EXPECT_FALSE(text.StartsWith("engine"));

    LIMX_EXPECT_TRUE(text.EndsWith("engine"));
    LIMX_EXPECT_TRUE(text.EndsWith(""));
    LIMX_EXPECT_FALSE(text.EndsWith("limx"));
}

LIMX_TEST(FString, PrefixLongerThanStringIsNotMatched)
{
    FString text("ab");

    LIMX_EXPECT_FALSE(text.StartsWith("abcdef"));
    LIMX_EXPECT_FALSE(text.EndsWith("abcdef"));
}

LIMX_TEST(FString, FindOnEmptyStringIsSafe)
{
    FString text;

    LIMX_EXPECT_EQ(text.Find("x"), FString::kNPos);
    LIMX_EXPECT_EQ(text.FindChar('x'), FString::kNPos);
    LIMX_EXPECT_FALSE(text.Contains("x"));
}

// ============================================================================
// 切片
// ============================================================================

LIMX_TEST(FString, SubstringExtractsRange)
{
    FString text("limx engine");
    FString middle = text.Substring(5, 6);

    LIMX_EXPECT_STREQ(middle.GetCStr(), "engine");
    LIMX_EXPECT_EQ(middle.GetLength(), SizeType(6));
}

LIMX_TEST(FString, SubstringBeyondEndClamps)
{
    FString text("limx");

    // 请求超出剩余长度时应截断到末尾而非越界
    FString tail = text.Substring(2, 100);
    LIMX_EXPECT_STREQ(tail.GetCStr(), "mx");

    // 起点越界返回空串
    FString beyond = text.Substring(100, 1);
    LIMX_EXPECT_TRUE(beyond.IsEmpty());
}

LIMX_TEST(FString, LeftAndRightSlice)
{
    FString text("limx engine");

    LIMX_EXPECT_STREQ(text.Left(4).GetCStr(), "limx");
    LIMX_EXPECT_STREQ(text.Right(6).GetCStr(), "engine");
}

LIMX_TEST(FString, LeftAndRightBeyondLengthReturnWhole)
{
    FString text("ab");

    LIMX_EXPECT_STREQ(text.Left(100).GetCStr(), "ab");
    LIMX_EXPECT_STREQ(text.Right(100).GetCStr(), "ab");
}

LIMX_TEST(FString, ZeroLengthSliceIsEmpty)
{
    FString text("limx");

    LIMX_EXPECT_TRUE(text.Left(0).IsEmpty());
    LIMX_EXPECT_TRUE(text.Right(0).IsEmpty());
}

// ============================================================================
// 拷贝与移动语义
// ============================================================================

LIMX_TEST(FString, CopyOfShortStringIsIndependent)
{
    FString source("short");
    FString copy(source);

    LIMX_EXPECT_STREQ(copy.GetCStr(), "short");

    copy.Append("er");

    // 修改副本不得影响源 — SSO 状态下尤其容易因共享缓冲区出错
    LIMX_EXPECT_STREQ(source.GetCStr(), "short");
    LIMX_EXPECT_STREQ(copy.GetCStr(), "shorter");
}

LIMX_TEST(FString, CopyOfHeapStringIsIndependent)
{
    FString source = MakeRepeated('q', 200);
    FString copy(source);

    LIMX_REQUIRE_EQ(copy.GetLength(), SizeType(200));
    LIMX_EXPECT_NE(copy.GetCStr(), source.GetCStr());

    copy.AppendChar('!');

    LIMX_EXPECT_EQ(source.GetLength(), SizeType(200));
    LIMX_EXPECT_EQ(copy.GetLength(), SizeType(201));
}

LIMX_TEST(FString, MoveOfShortStringCopiesInlineBuffer)
{
    FString source("inline");
    FString moved(static_cast<FString&&>(source));

    // SSO 状态无堆指针可转移, 必须逐字节拷贝内联缓冲区
    LIMX_EXPECT_STREQ(moved.GetCStr(), "inline");
    LIMX_EXPECT_EQ(moved.GetLength(), SizeType(6));
}

LIMX_TEST(FString, MoveOfHeapStringTransfersBuffer)
{
    FString source = MakeRepeated('h', 300);
    const AnsiChar* originalData = source.GetCStr();

    FString moved(static_cast<FString&&>(source));

    LIMX_EXPECT_EQ(moved.GetLength(), SizeType(300));
    LIMX_EXPECT_EQ(moved.GetCStr(), originalData);

    // 移后源必须回到可安全析构的空状态
    LIMX_EXPECT_TRUE(source.IsEmpty());
}

LIMX_TEST(FString, AssignmentReplacesContent)
{
    FString source("new content");
    FString target = MakeRepeated('o', 100);

    target = source;

    LIMX_EXPECT_STREQ(target.GetCStr(), "new content");
    LIMX_EXPECT_EQ(target.GetLength(), SizeType(11));
}

LIMX_TEST(FString, SelfAssignmentIsSafe)
{
    FString text = MakeRepeated('s', 100);

    const FString& alias = text;
    text = alias;

    LIMX_EXPECT_EQ(text.GetLength(), SizeType(100));
    LIMX_EXPECT_EQ(text[0], 's');
    LIMX_EXPECT_EQ(text[99], 's');
}

// ============================================================================
// 比较与拼接
// ============================================================================

LIMX_TEST(FString, EqualityComparesContent)
{
    FString first("limx");
    FString second("limx");
    FString different("engine");

    LIMX_EXPECT_TRUE(first == second);
    LIMX_EXPECT_FALSE(first == different);
}

LIMX_TEST(FString, EqualityIsLengthSensitive)
{
    FString shorter("limx");
    FString longer("limxx");

    LIMX_EXPECT_FALSE(shorter == longer);
}

LIMX_TEST(FString, ConcatenationProducesNewString)
{
    FString first("limx");
    FString second(" engine");

    FString combined = first + second;

    LIMX_EXPECT_STREQ(combined.GetCStr(), "limx engine");

    // 操作数不得被修改
    LIMX_EXPECT_STREQ(first.GetCStr(), "limx");
    LIMX_EXPECT_STREQ(second.GetCStr(), " engine");
}

// ============================================================================
// 分配守恒
// ============================================================================

LIMX_TEST(FString, HeavyMutationLeavesNoLeak)
{
    FTrackingAllocator allocator;

    {
        FString text(allocator);

        for (Int32 i = 0; i < 100; ++i)
        {
            text.Append("chunk");
        }

        text.Clear();

        for (Int32 i = 0; i < 50; ++i)
        {
            text.AppendChar('x');
        }
    }

    LIMX_EXPECT_FALSE(allocator.HasLeaks());
    LIMX_EXPECT_EQ(allocator.GetCurrentBytes(), SizeType(0));
}
