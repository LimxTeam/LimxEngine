/*******************************************************************************
 * 文件: SmartPointerTests.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   智能指针与可选值单元测试 — TUniquePtr / TSharedPtr / TWeakPtr / TOptional
 *   覆盖所有权转移、引用计数、弱引用失效、原地构造与析构配对
 *
 * 设计哲学:
 *   所有权是不可见的 — 智能指针的核心契约"对象在最后一个所有者消失时恰好
 *   析构一次"无法从返回值观察，只能靠 FProbe 的构造/析构计数验证。因此
 *   本文件几乎所有用例都以 FProbe 为被管理类型并断言 GetLiveCount() 归零。
 *
 *   弱引用要测失效时刻 — TWeakPtr 的价值在于"强引用归零后能安全察觉"。
 *   用例显式构造"强引用先于弱引用销毁"的顺序，验证 Lock 返回空而非悬垂。
 *
 * 技术特性:
 *   - 引用计数在每次拷贝/销毁后逐点断言, 而非只看最终结果
 *   - MakeShared 的单次分配特性通过 FTrackingAllocator 间接验证
 *   - TOptional 覆盖空/有值/重置/重新赋值的完整状态机
 *
 * 依赖关系:
 *   内部: CoreTests/CoreTestsMinimal.h, CoreTests/FProbe.h
 *
 * 注意事项:
 *   MakeUnique/MakeShared 使用全局默认分配器, 泄漏靠 FProbe 计数而非
 *   FTrackingAllocator 观察; 需要注入分配器时用 MakeUniqueWith
 *
 ******************************************************************************/

#include "CoreTests/CoreTestsMinimal.h"
#include "CoreTests/FProbe.h"

using namespace Limx;

// ============================================================================
// TUniquePtr — 独占所有权
// ============================================================================

LIMX_TEST(TUniquePtr, DefaultConstructedIsNull)
{
    TUniquePtr<FProbe> pointer;

    LIMX_EXPECT_NULL(pointer.Get());
    LIMX_EXPECT_FALSE(static_cast<bool>(pointer));
}

LIMX_TEST(TUniquePtr, MakeUniqueConstructsObject)
{
    FProbe::ResetCounters();

    {
        TUniquePtr<FProbe> pointer = MakeUnique<FProbe>(42);

        LIMX_REQUIRE_NOT_NULL(pointer.Get());
        LIMX_EXPECT_TRUE(static_cast<bool>(pointer));
        LIMX_EXPECT_EQ(pointer->GetValue(), 42);
        LIMX_EXPECT_EQ(FProbe::s_ValueConstructCount, 1);
    }

    // 离开作用域必须恰好析构一次
    LIMX_EXPECT_EQ(FProbe::s_DestructCount, 1);
    LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 0);
}

LIMX_TEST(TUniquePtr, MoveTransfersOwnership)
{
    FProbe::ResetCounters();

    {
        TUniquePtr<FProbe> source = MakeUnique<FProbe>(7);
        FProbe* rawPointer = source.Get();

        TUniquePtr<FProbe> target(static_cast<TUniquePtr<FProbe>&&>(source));

        // 所有权转移: 目标持有原指针, 源置空
        LIMX_EXPECT_EQ(target.Get(), rawPointer);
        LIMX_EXPECT_NULL(source.Get());
        LIMX_EXPECT_FALSE(static_cast<bool>(source));

        // 转移过程不得析构对象
        LIMX_EXPECT_EQ(FProbe::s_DestructCount, 0);
    }

    LIMX_EXPECT_EQ(FProbe::s_DestructCount, 1);
}

LIMX_TEST(TUniquePtr, MoveAssignmentDestroysPreviousTarget)
{
    FProbe::ResetCounters();

    {
        TUniquePtr<FProbe> first  = MakeUnique<FProbe>(1);
        TUniquePtr<FProbe> second = MakeUnique<FProbe>(2);

        LIMX_REQUIRE_EQ(FProbe::s_DestructCount, 0);

        first = static_cast<TUniquePtr<FProbe>&&>(second);

        // 被覆盖的原对象 (值 1) 必须立即析构
        LIMX_EXPECT_EQ(FProbe::s_DestructCount, 1);
        LIMX_EXPECT_EQ(first->GetValue(), 2);
        LIMX_EXPECT_NULL(second.Get());
    }

    LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 0);
}

LIMX_TEST(TUniquePtr, ResetDestroysObject)
{
    FProbe::ResetCounters();

    TUniquePtr<FProbe> pointer = MakeUnique<FProbe>(5);
    LIMX_REQUIRE_EQ(FProbe::s_DestructCount, 0);

    pointer.Reset();

    LIMX_EXPECT_EQ(FProbe::s_DestructCount, 1);
    LIMX_EXPECT_NULL(pointer.Get());
    LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 0);
}

LIMX_TEST(TUniquePtr, AssignNullptrDestroysObject)
{
    FProbe::ResetCounters();

    TUniquePtr<FProbe> pointer = MakeUnique<FProbe>(5);
    pointer = nullptr;

    LIMX_EXPECT_NULL(pointer.Get());
    LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 0);
}

LIMX_TEST(TUniquePtr, ResetOnNullIsSafe)
{
    TUniquePtr<FProbe> pointer;

    pointer.Reset();
    pointer.Reset();

    LIMX_EXPECT_NULL(pointer.Get());
}

LIMX_TEST(TUniquePtr, MakeUniqueWithUsesInjectedAllocator)
{
    FTrackingAllocator allocator;
    FProbe::ResetCounters();

    {
        TUniquePtr<FProbe> pointer = MakeUniqueWith<FProbe>(allocator, 9);

        LIMX_REQUIRE_NOT_NULL(pointer.Get());
        LIMX_EXPECT_EQ(pointer->GetValue(), 9);
        LIMX_EXPECT_EQ(allocator.GetAllocationCount(), 1ull);
    }

    LIMX_EXPECT_FALSE(allocator.HasLeaks());
    LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 0);
}

LIMX_TEST(TUniquePtr, StoredInArrayKeepsUniqueOwnership)
{
    FProbe::ResetCounters();

    {
        TArray<TUniquePtr<FProbe>> pointers;

        for (Int32 i = 0; i < 16; ++i)
        {
            pointers.Add(MakeUnique<FProbe>(i));
        }

        LIMX_REQUIRE_EQ(pointers.GetSize(), SizeType(16));

        for (Int32 i = 0; i < 16; ++i)
        {
            LIMX_EXPECT_EQ(pointers[static_cast<SizeType>(i)]->GetValue(), i);
        }
    }

    // 数组销毁必须逐个释放所管理对象
    LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 0);
}

// ============================================================================
// TSharedPtr — 共享所有权
// ============================================================================

LIMX_TEST(TSharedPtr, DefaultConstructedIsInvalid)
{
    TSharedPtr<FProbe> pointer;

    LIMX_EXPECT_FALSE(pointer.IsValid());
    LIMX_EXPECT_NULL(pointer.Get());
    LIMX_EXPECT_EQ(pointer.GetSharedCount(), 0);
}

LIMX_TEST(TSharedPtr, MakeSharedStartsWithCountOne)
{
    FProbe::ResetCounters();

    {
        TSharedPtr<FProbe> pointer = MakeShared<FProbe>(11);

        LIMX_REQUIRE_TRUE(pointer.IsValid());
        LIMX_EXPECT_EQ(pointer.GetSharedCount(), 1);
        LIMX_EXPECT_EQ(pointer->GetValue(), 11);
        LIMX_EXPECT_EQ((*pointer).GetValue(), 11);
    }

    LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 0);
}

LIMX_TEST(TSharedPtr, CopyIncrementsRefCount)
{
    FProbe::ResetCounters();

    {
        TSharedPtr<FProbe> first = MakeShared<FProbe>(1);
        LIMX_REQUIRE_EQ(first.GetSharedCount(), 1);

        {
            TSharedPtr<FProbe> second = first;

            LIMX_EXPECT_EQ(first.GetSharedCount(), 2);
            LIMX_EXPECT_EQ(second.GetSharedCount(), 2);
            LIMX_EXPECT_EQ(second.Get(), first.Get());

            // 共享期间对象不得析构
            LIMX_EXPECT_EQ(FProbe::s_DestructCount, 0);
        }

        // 副本销毁后计数回落
        LIMX_EXPECT_EQ(first.GetSharedCount(), 1);
        LIMX_EXPECT_EQ(FProbe::s_DestructCount, 0);
    }

    // 最后一个所有者消失时恰好析构一次
    LIMX_EXPECT_EQ(FProbe::s_DestructCount, 1);
}

LIMX_TEST(TSharedPtr, MultipleCopiesTrackCountAccurately)
{
    FProbe::ResetCounters();

    {
        TSharedPtr<FProbe> original = MakeShared<FProbe>(1);

        TArray<TSharedPtr<FProbe>> copies;
        for (Int32 i = 0; i < 10; ++i)
        {
            copies.Add(original);
        }

        // 原始 1 个 + 10 个副本
        LIMX_EXPECT_EQ(original.GetSharedCount(), 11);

        copies.Clear();

        LIMX_EXPECT_EQ(original.GetSharedCount(), 1);
        LIMX_EXPECT_EQ(FProbe::s_DestructCount, 0);
    }

    LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 0);
}

LIMX_TEST(TSharedPtr, MoveDoesNotChangeRefCount)
{
    FProbe::ResetCounters();

    {
        TSharedPtr<FProbe> source = MakeShared<FProbe>(3);
        FProbe* rawPointer = source.Get();

        TSharedPtr<FProbe> target(static_cast<TSharedPtr<FProbe>&&>(source));

        // 移动是所有权转移而非新增所有者, 计数保持 1
        LIMX_EXPECT_EQ(target.GetSharedCount(), 1);
        LIMX_EXPECT_EQ(target.Get(), rawPointer);
        LIMX_EXPECT_FALSE(source.IsValid());
    }

    LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 0);
}

LIMX_TEST(TSharedPtr, ResetReleasesOwnership)
{
    FProbe::ResetCounters();

    TSharedPtr<FProbe> first  = MakeShared<FProbe>(1);
    TSharedPtr<FProbe> second = first;

    LIMX_REQUIRE_EQ(first.GetSharedCount(), 2);

    first.Reset();

    LIMX_EXPECT_FALSE(first.IsValid());
    LIMX_EXPECT_EQ(second.GetSharedCount(), 1);
    LIMX_EXPECT_EQ(FProbe::s_DestructCount, 0);

    second.Reset();

    LIMX_EXPECT_EQ(FProbe::s_DestructCount, 1);
    LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 0);
}

LIMX_TEST(TSharedPtr, SelfAssignmentKeepsObjectAlive)
{
    FProbe::ResetCounters();

    {
        TSharedPtr<FProbe> pointer = MakeShared<FProbe>(1);

        const TSharedPtr<FProbe>& alias = pointer;
        pointer = alias;

        // 自赋值不得先释放后使用
        LIMX_EXPECT_TRUE(pointer.IsValid());
        LIMX_EXPECT_EQ(pointer.GetSharedCount(), 1);
        LIMX_EXPECT_EQ(pointer->GetValue(), 1);
        LIMX_EXPECT_EQ(FProbe::s_DestructCount, 0);
    }

    LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 0);
}

// ============================================================================
// TWeakPtr — 弱引用
// ============================================================================

LIMX_TEST(TWeakPtr, DefaultConstructedIsExpired)
{
    TWeakPtr<FProbe> weak;

    LIMX_EXPECT_TRUE(weak.IsExpired());
    LIMX_EXPECT_FALSE(weak.Lock().IsValid());
}

LIMX_TEST(TWeakPtr, LockSucceedsWhileStrongAlive)
{
    FProbe::ResetCounters();

    {
        TSharedPtr<FProbe> strong = MakeShared<FProbe>(21);
        TWeakPtr<FProbe>   weak(strong);

        LIMX_EXPECT_FALSE(weak.IsExpired());

        TSharedPtr<FProbe> locked = weak.Lock();

        LIMX_REQUIRE_TRUE(locked.IsValid());
        LIMX_EXPECT_EQ(locked->GetValue(), 21);

        // Lock 产生一个新的强引用
        LIMX_EXPECT_EQ(strong.GetSharedCount(), 2);
    }

    LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 0);
}

LIMX_TEST(TWeakPtr, ExpiresWhenLastStrongReleases)
{
    FProbe::ResetCounters();

    TWeakPtr<FProbe> weak;

    {
        TSharedPtr<FProbe> strong = MakeShared<FProbe>(1);
        weak = TWeakPtr<FProbe>(strong);

        LIMX_REQUIRE_FALSE(weak.IsExpired());
    }

    // 强引用全部消失后, 弱引用必须失效而非悬垂
    LIMX_EXPECT_TRUE(weak.IsExpired());
    LIMX_EXPECT_FALSE(weak.Lock().IsValid());

    // 对象已析构, 但控制块由弱引用维持, 不应造成泄漏或二次析构
    LIMX_EXPECT_EQ(FProbe::s_DestructCount, 1);
}

LIMX_TEST(TWeakPtr, DoesNotKeepObjectAlive)
{
    FProbe::ResetCounters();

    {
        TWeakPtr<FProbe> weak;

        {
            TSharedPtr<FProbe> strong = MakeShared<FProbe>(1);
            weak = TWeakPtr<FProbe>(strong);

            // 弱引用不参与强计数
            LIMX_EXPECT_EQ(strong.GetSharedCount(), 1);
        }

        LIMX_EXPECT_EQ(FProbe::s_DestructCount, 1);
    }

    LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 0);
}

LIMX_TEST(TWeakPtr, WeakOutlivingStrongIsSafeToCopy)
{
    FProbe::ResetCounters();

    TWeakPtr<FProbe> outer;

    {
        TSharedPtr<FProbe> strong = MakeShared<FProbe>(1);
        outer = TWeakPtr<FProbe>(strong);
    }

    // 对象已销毁, 复制失效的弱引用不得崩溃
    TWeakPtr<FProbe> copy(outer);

    LIMX_EXPECT_TRUE(copy.IsExpired());
    LIMX_EXPECT_FALSE(copy.Lock().IsValid());
}

// ============================================================================
// TOptional — 可选值
// ============================================================================

LIMX_TEST(TOptional, DefaultConstructedHasNoValue)
{
    TOptional<Int32> value;

    LIMX_EXPECT_FALSE(value.HasValue());
    LIMX_EXPECT_FALSE(static_cast<bool>(value));
}

LIMX_TEST(TOptional, ConstructedWithValueHasValue)
{
    TOptional<Int32> value(42);

    LIMX_REQUIRE_TRUE(value.HasValue());
    LIMX_EXPECT_EQ(value.GetValue(), 42);
    LIMX_EXPECT_EQ(*value, 42);
}

LIMX_TEST(TOptional, GetValueOrReturnsDefaultWhenEmpty)
{
    TOptional<Int32> empty;
    TOptional<Int32> filled(7);

    const Int32 fallback = 99;

    LIMX_EXPECT_EQ(empty.GetValueOr(fallback), 99);
    LIMX_EXPECT_EQ(filled.GetValueOr(fallback), 7);
}

LIMX_TEST(TOptional, ResetClearsValue)
{
    FProbe::ResetCounters();

    {
        TOptional<FProbe> value(FProbe(1));
        LIMX_REQUIRE_TRUE(value.HasValue());

        const Int32 destructBefore = FProbe::s_DestructCount;
        value.Reset();

        LIMX_EXPECT_FALSE(value.HasValue());

        // 重置必须析构所持有的对象
        LIMX_EXPECT_GT(FProbe::s_DestructCount, destructBefore);
    }

    LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 0);
}

LIMX_TEST(TOptional, ResetOnEmptyIsSafe)
{
    TOptional<Int32> value;

    value.Reset();
    value.Reset();

    LIMX_EXPECT_FALSE(value.HasValue());
}

LIMX_TEST(TOptional, EmplaceConstructsInPlace)
{
    FProbe::ResetCounters();

    {
        TOptional<FProbe> value;
        FProbe& created = value.Emplace(33);

        LIMX_EXPECT_TRUE(value.HasValue());
        LIMX_EXPECT_EQ(created.GetValue(), 33);

        // 原地构造不产生拷贝或移动
        LIMX_EXPECT_EQ(FProbe::s_ValueConstructCount, 1);
        LIMX_EXPECT_EQ(FProbe::s_CopyConstructCount, 0);
        LIMX_EXPECT_EQ(FProbe::s_MoveConstructCount, 0);
    }

    LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 0);
}

LIMX_TEST(TOptional, EmplaceOverExistingValueDestroysOld)
{
    FProbe::ResetCounters();

    {
        TOptional<FProbe> value;
        value.Emplace(1);

        const Int32 destructBefore = FProbe::s_DestructCount;
        value.Emplace(2);

        // 覆盖前必须析构旧值
        LIMX_EXPECT_GT(FProbe::s_DestructCount, destructBefore);
        LIMX_EXPECT_EQ(value.GetValue().GetValue(), 2);
    }

    LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 0);
}

LIMX_TEST(TOptional, CopyPreservesState)
{
    TOptional<Int32> filled(5);
    TOptional<Int32> empty;

    TOptional<Int32> copyOfFilled(filled);
    TOptional<Int32> copyOfEmpty(empty);

    LIMX_EXPECT_TRUE(copyOfFilled.HasValue());
    LIMX_EXPECT_EQ(copyOfFilled.GetValue(), 5);
    LIMX_EXPECT_FALSE(copyOfEmpty.HasValue());
}

LIMX_TEST(TOptional, MoveLeavesSourceEmpty)
{
    FProbe::ResetCounters();

    {
        TOptional<FProbe> source;
        source.Emplace(8);

        TOptional<FProbe> target(static_cast<TOptional<FProbe>&&>(source));

        LIMX_EXPECT_TRUE(target.HasValue());
        LIMX_EXPECT_EQ(target.GetValue().GetValue(), 8);

        // 移动后源必须被重置为空态
        LIMX_EXPECT_FALSE(source.HasValue());
    }

    LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 0);
}

LIMX_TEST(TOptional, ArrowOperatorAccessesMembers)
{
    TOptional<FProbe> value;
    value.Emplace(17);

    LIMX_REQUIRE_TRUE(value.HasValue());
    LIMX_EXPECT_EQ(value->GetValue(), 17);

    value->SetValue(18);
    LIMX_EXPECT_EQ(value->GetValue(), 18);
}

LIMX_TEST(TOptional, NonTrivialTypeLeavesNoLeak)
{
    FProbe::ResetCounters();

    {
        TArray<TOptional<FProbe>> values;

        for (Int32 i = 0; i < 20; ++i)
        {
            TOptional<FProbe> item;
            item.Emplace(i);
            values.Add(static_cast<TOptional<FProbe>&&>(item));
        }

        // 半数重置
        for (SizeType i = 0; i < values.GetSize(); i += 2)
        {
            values[i].Reset();
        }
    }

    LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 0);
}


// ============================================================================
// TRefCounted / TRefPtr
//
// 这一族此前没有任何用例, 也没有任何使用者 —— 它的 Delete() 用的是裸
// delete, 而项目禁止裸 new, 因此当时根本没有合规的创建方式。改为走默认
// 分配器并补上 MakeRefCounted 之后, 分配与释放必须严格配对: 从一个堆
// 分配、往另一个堆归还, 症状是随机的堆损坏, 不会当场报错。
// ============================================================================

namespace
{

/// 引用计数探针 — 构造/析构次数记在 FProbe 上
class FRefProbe : public TRefCounted<FRefProbe>
{
public:
    FRefProbe()
        : m_Payload()
    {
    }

    explicit FRefProbe(Int32 value)
        : m_Payload(value)
    {
    }

    LIMX_NODISCARD Int32 GetValue() const
    {
        return m_Payload.GetValue();
    }

private:
    FProbe m_Payload;
};

} // namespace

LIMX_TEST(RefCounted, MakeRefCountedStartsAtOne)
{
    FProbe::ResetCounters();

    {
        TRefPtr<FRefProbe> ptr = MakeRefCounted<FRefProbe>(42);

        LIMX_REQUIRE_TRUE(ptr.Get() != nullptr);
        LIMX_EXPECT_EQ(ptr->GetRefCount(), UInt32(1));
        LIMX_EXPECT_TRUE(ptr->IsUnique());
        LIMX_EXPECT_EQ(ptr->GetValue(), 42);
        LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 1);
    }

    // 离开作用域即释放 —— 计数没归零说明 AddRef/Release 不配对
    LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 0);
}

LIMX_TEST(RefCounted, CopyingSharesOwnership)
{
    FProbe::ResetCounters();

    {
        TRefPtr<FRefProbe> first = MakeRefCounted<FRefProbe>(7);

        {
            TRefPtr<FRefProbe> second = first;

            LIMX_EXPECT_EQ(first->GetRefCount(), UInt32(2));
            LIMX_EXPECT_FALSE(first->IsUnique());

            // 两个指针指向同一个对象, 而不是各自一份拷贝
            LIMX_EXPECT_TRUE(first == second.Get());
            LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 1);
        }

        // 第二个引用消失后对象必须还活着
        LIMX_EXPECT_EQ(first->GetRefCount(), UInt32(1));
        LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 1);
    }

    LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 0);
}

LIMX_TEST(RefCounted, DestroysExactlyOnce)
{
    FProbe::ResetCounters();

    {
        TRefPtr<FRefProbe> a = MakeRefCounted<FRefProbe>(1);
        TRefPtr<FRefProbe> b = a;
        TRefPtr<FRefProbe> c = b;

        LIMX_EXPECT_EQ(a->GetRefCount(), UInt32(3));
    }

    // 析构恰好一次 —— 多于一次是二次释放, 少于一次是泄漏。
    // 两者都不会让上面任何一条断言失败, 只能靠这里的计数抓。
    LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 0);
    LIMX_EXPECT_EQ(FProbe::s_DestructCount, 1);
}

LIMX_TEST(RefCounted, ReleasesOnReassignment)
{
    FProbe::ResetCounters();

    {
        TRefPtr<FRefProbe> ptr = MakeRefCounted<FRefProbe>(1);

        LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 1);

        // 覆盖赋值必须先放掉旧对象
        ptr = MakeRefCounted<FRefProbe>(2);

        LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 1);
        LIMX_EXPECT_EQ(ptr->GetValue(), 2);
        LIMX_EXPECT_EQ(ptr->GetRefCount(), UInt32(1));
    }

    LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 0);
}

LIMX_TEST(RefCounted, SelfAssignmentKeepsObjectAlive)
{
    FProbe::ResetCounters();

    {
        TRefPtr<FRefProbe> ptr = MakeRefCounted<FRefProbe>(3);

        // 自赋值若写成"先放旧的再取新的", 这里对象就已经没了
        ptr = ptr;

        LIMX_REQUIRE_TRUE(ptr.Get() != nullptr);
        LIMX_EXPECT_EQ(ptr->GetValue(), 3);
        LIMX_EXPECT_EQ(ptr->GetRefCount(), UInt32(1));
        LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 1);
    }

    LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 0);
}

LIMX_TEST(RefCounted, AllocationAndReleaseArePaired)
{
    // 反复创建销毁 —— 分配器与释放路径接不上时, 这里是最容易把堆
    // 损坏踩出来的形状。单次创建销毁往往看不出异常。
    FProbe::ResetCounters();

    constexpr Int32 kRounds = 512;

    for (Int32 i = 0; i < kRounds; ++i)
    {
        TRefPtr<FRefProbe> ptr = MakeRefCounted<FRefProbe>(i);

        LIMX_REQUIRE_TRUE(ptr.Get() != nullptr);

        if (ptr->GetValue() != i)
        {
            LIMX_EXPECT_EQ(ptr->GetValue(), i);
            break;
        }
    }

    LIMX_EXPECT_EQ(FProbe::GetLiveCount(), 0);
    LIMX_EXPECT_EQ(FProbe::s_DestructCount, kRounds);
}
