/*******************************************************************************
 * 文件: ArrayRemovalTests.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   TArray 元素移除路径的回归测试 — 构造/析构计数与所有权语义
 *
 * 设计哲学:
 *   这组用例存在的直接原因是一个真实缺陷: RemoveAt 曾经实现为
 *   "析构目标 → 移动构造整段前移 → 析构源段", 而源段与目标段重叠 ——
 *   前移之后, 源段上放的已经是搬过来的**活对象**, 再析构等于销毁了
 *   仍在数组里的元素。
 *
 *   它潜伏了很久, 因为绝大多数 TArray 装的是句柄、浮点这类平凡析构类型,
 *   DestructItems 对它们是空操作。直到 TArray<TUniquePtr<T>> 第一次用上
 *   RemoveAt, 才以"删一个元素连带释放掉后面几个"的形式炸出来。
 *
 *   因此这里测的不是"元素还在不在", 而是**析构次数**: 只有计数才能
 *   分辨"逻辑上被移除"与"对象被销毁", 而这正是缺陷所在的那一层。
 *
 * 技术特性:
 *   - 用 FProbe 统计各类构造与析构次数
 *   - 覆盖首/中/尾三个位置, 以及带所有权的 TUniquePtr 元素
 *
 * 依赖关系:
 *   内部: CoreTests/CoreTestsMinimal.h, CoreTests/FProbe.h
 *
 ******************************************************************************/

#include "CoreTests/CoreTestsMinimal.h"
#include "CoreTests/FProbe.h"
#include "Core/Templates/TUniquePtr.h"

using namespace Limx;

namespace
{

/// 记录自身销毁的探针 — 用于验证"逻辑移除"与"对象销毁"的一致性
struct FOwnedProbe
{
    Int32* Counter = nullptr;
    Int32  Value   = 0;

    FOwnedProbe(Int32* counter, Int32 value)
        : Counter(counter), Value(value)
    {
    }

    ~FOwnedProbe()
    {
        if (Counter != nullptr)
        {
            ++(*Counter);
        }
    }

    FOwnedProbe(const FOwnedProbe&)            = delete;
    FOwnedProbe& operator=(const FOwnedProbe&) = delete;
};

} // namespace

// ============================================================================
// RemoveAt — 析构次数
// ============================================================================

LIMX_TEST(TArrayRemoval, RemoveAtDestroysExactlyOneElement)
{
    FProbe::ResetCounters();

    {
        TArray<FProbe> array;
        for (Int32 i = 0; i < 5; ++i)
        {
            array.Add(FProbe(i));
        }

        const Int32 destructsBefore = FProbe::s_DestructCount;

        array.RemoveAt(0);

        // 移除一个元素, 净析构次数必须恰好是 1。
        //
        // 搬移过程本身可能产生额外的构造/析构配对 (取决于实现走移动赋值
        // 还是移动构造), 但**净**减少的对象数只能是 1 —— 这条断言正是
        // 旧实现失败的地方: 它一次销毁了目标之后的全部元素。
        LIMX_EXPECT_EQ(array.GetSize(), static_cast<SizeType>(4));

        const Int32 netDestructs = FProbe::s_DestructCount - destructsBefore;
        LIMX_EXPECT_EQ(netDestructs, 1);
    }
}

LIMX_TEST(TArrayRemoval, RemoveAtPreservesRemainingValues)
{
    TArray<Int32> array;
    for (Int32 i = 0; i < 6; ++i)
    {
        array.Add(i * 10);
    }

    array.RemoveAt(2);

    LIMX_REQUIRE_TRUE(array.GetSize() == 5);
    LIMX_EXPECT_EQ(array[0], 0);
    LIMX_EXPECT_EQ(array[1], 10);
    LIMX_EXPECT_EQ(array[2], 30);
    LIMX_EXPECT_EQ(array[3], 40);
    LIMX_EXPECT_EQ(array[4], 50);
}

LIMX_TEST(TArrayRemoval, RemoveAtFirstMiddleLast)
{
    for (SizeType removeIndex = 0; removeIndex < 4; ++removeIndex)
    {
        Int32 destroyed = 0;

        {
            TArray<TUniquePtr<FOwnedProbe>> array;

            for (Int32 i = 0; i < 4; ++i)
            {
                array.Add(MakeUnique<FOwnedProbe>(&destroyed, i));
            }

            LIMX_REQUIRE_TRUE(array.GetSize() == 4);

            array.RemoveAt(removeIndex);

            // 移除一个 —— 只能有一个对象被销毁
            LIMX_EXPECT_EQ(destroyed, 1);
            LIMX_EXPECT_EQ(array.GetSize(), static_cast<SizeType>(3));

            // 余下的元素必须仍然可用 (不是已释放的内存)
            Int32 sum = 0;
            for (SizeType i = 0; i < array.GetSize(); ++i)
            {
                LIMX_REQUIRE_TRUE(array[i].Get() != nullptr);
                sum += array[i]->Value;
            }

            // 0+1+2+3 = 6, 减去被移除的那个
            LIMX_EXPECT_EQ(sum, 6 - static_cast<Int32>(removeIndex));
        }

        // 作用域结束后全部销毁
        LIMX_EXPECT_EQ(destroyed, 4);
    }
}

LIMX_TEST(TArrayRemoval, RepeatedRemoveAtFrontIsSafe)
{
    // 反复从头部移除 —— 每次都会搬移整段, 是重叠区间最容易出错的模式
    Int32 destroyed = 0;

    {
        TArray<TUniquePtr<FOwnedProbe>> array;

        for (Int32 i = 0; i < 8; ++i)
        {
            array.Add(MakeUnique<FOwnedProbe>(&destroyed, i));
        }

        for (Int32 round = 0; round < 8; ++round)
        {
            array.RemoveAt(0);

            LIMX_EXPECT_EQ(destroyed, round + 1);
            LIMX_EXPECT_EQ(array.GetSize(),
                           static_cast<SizeType>(7 - round));

            // 剩余元素的值必须是连续递增的尾段
            for (SizeType i = 0; i < array.GetSize(); ++i)
            {
                LIMX_REQUIRE_TRUE(array[i].Get() != nullptr);
                LIMX_EXPECT_EQ(array[i]->Value,
                               round + 1 + static_cast<Int32>(i));
            }
        }
    }

    LIMX_EXPECT_EQ(destroyed, 8);
}

LIMX_TEST(TArrayRemoval, RemoveAtLastElement)
{
    Int32 destroyed = 0;

    {
        TArray<TUniquePtr<FOwnedProbe>> array;
        array.Add(MakeUnique<FOwnedProbe>(&destroyed, 7));

        array.RemoveAt(0);

        LIMX_EXPECT_EQ(destroyed, 1);
        LIMX_EXPECT_EQ(array.GetSize(), static_cast<SizeType>(0));
    }

    LIMX_EXPECT_EQ(destroyed, 1);
}

// ============================================================================
// RemoveAtSwap / RemoveLast — 与 RemoveAt 同样的所有权要求
// ============================================================================

LIMX_TEST(TArrayRemoval, RemoveAtSwapDestroysExactlyOne)
{
    Int32 destroyed = 0;

    {
        TArray<TUniquePtr<FOwnedProbe>> array;
        for (Int32 i = 0; i < 5; ++i)
        {
            array.Add(MakeUnique<FOwnedProbe>(&destroyed, i));
        }

        array.RemoveAtSwap(1);

        LIMX_EXPECT_EQ(destroyed, 1);
        LIMX_EXPECT_EQ(array.GetSize(), static_cast<SizeType>(4));

        // 末尾元素被搬到了下标 1
        LIMX_REQUIRE_TRUE(array[1].Get() != nullptr);
        LIMX_EXPECT_EQ(array[1]->Value, 4);
    }

    LIMX_EXPECT_EQ(destroyed, 5);
}

LIMX_TEST(TArrayRemoval, RemoveLastDestroysExactlyOne)
{
    Int32 destroyed = 0;

    {
        TArray<TUniquePtr<FOwnedProbe>> array;
        for (Int32 i = 0; i < 3; ++i)
        {
            array.Add(MakeUnique<FOwnedProbe>(&destroyed, i));
        }

        array.RemoveLast();

        LIMX_EXPECT_EQ(destroyed, 1);
        LIMX_EXPECT_EQ(array.GetSize(), static_cast<SizeType>(2));
    }

    LIMX_EXPECT_EQ(destroyed, 3);
}

// ============================================================================
// Insert — 反向搬移路径
// ============================================================================

LIMX_TEST(TArrayRemoval, InsertPreservesOwnership)
{
    Int32 destroyed = 0;

    {
        TArray<TUniquePtr<FOwnedProbe>> array;
        for (Int32 i = 0; i < 4; ++i)
        {
            array.Add(MakeUnique<FOwnedProbe>(&destroyed, i));
        }

        array.Insert(1, MakeUnique<FOwnedProbe>(&destroyed, 99));

        // 插入不应销毁任何已有元素
        LIMX_EXPECT_EQ(destroyed, 0);
        LIMX_EXPECT_EQ(array.GetSize(), static_cast<SizeType>(5));

        const Int32 expected[5] = { 0, 99, 1, 2, 3 };

        for (SizeType i = 0; i < array.GetSize(); ++i)
        {
            LIMX_REQUIRE_TRUE(array[i].Get() != nullptr);
            LIMX_EXPECT_EQ(array[i]->Value, expected[i]);
        }
    }

    LIMX_EXPECT_EQ(destroyed, 5);
}
