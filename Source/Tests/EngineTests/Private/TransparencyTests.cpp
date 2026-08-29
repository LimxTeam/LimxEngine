/*******************************************************************************
 * 文件: TransparencyTests.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   半透明分类规则与排序策略的单元测试
 *
 * 设计哲学:
 *   分类规则是最容易在扩展时漏掉的一环 — 新增一种混合模式时, 若某处
 *   判断没跟着更新, 该模式就会在某个 Pass 里走错路径。画面上只表现为
 *   "这个材质有时候不对", 而它究竟错在哪个 Pass 完全看不出来。
 *   这里把"哪些模式需要混合"逐个钉死。
 *
 *   排序策略测的是**顺序**而非距离数值 — Alpha 混合不满足交换律,
 *   唯一重要的是"远的先画"。用几组精心构造的位置断言最终次序,
 *   比断言某个距离值更贴近真正的正确性要求。
 *
 * 技术特性:
 *   - 不需要 GPU: 分类是纯函数, 排序谓词只依赖包围盒与相机位置
 *   - 排序用例走真实的 Sort() 而非手工比较, 覆盖谓词与排序算法的配合
 *
 * 依赖关系:
 *   内部: EngineTests/EngineTestsMinimal.h, Core/Containers/TSortAlgorithms.h
 *
 ******************************************************************************/

#include "EngineTests/EngineTestsMinimal.h"
#include "Core/Containers/TSortAlgorithms.h"

using namespace Limx;

namespace
{

/// 构造一个位于指定中心的批次
FRenderObject MakeObjectAt(const FVector3& center, const AnsiChar* name)
{
    FRenderObject object;

    const FVector3 extent(0.5f, 0.5f, 0.5f);
    object.WorldBounds = FBoundingBox(center - extent, center + extent);
    object.DebugName   = name;

    return object;
}

} // namespace

// ============================================================================
// IsBlendedMode — 分类规则
// ============================================================================

LIMX_TEST(BlendMode, OpaqueIsNotBlended)
{
    LIMX_EXPECT_FALSE(IsBlendedMode(EMaterialBlendMode::Opaque));
}

LIMX_TEST(BlendMode, MaskedIsNotBlended)
{
    // Masked 靠 discard 实现镂空 —— 不混合, 照常写深度, 因此留在不透明列表。
    // 把它误判为半透明会让它退出深度预 Pass, 前向 Pass 的 Equal 深度测试
    // 随即失去依据。
    LIMX_EXPECT_FALSE(IsBlendedMode(EMaterialBlendMode::Masked));
}

LIMX_TEST(BlendMode, TranslucentIsBlended)
{
    LIMX_EXPECT_TRUE(IsBlendedMode(EMaterialBlendMode::Translucent));
}

LIMX_TEST(BlendMode, AdditiveIsBlended)
{
    // Additive 同样需要混合与由远及近 —— 漏掉它会让粒子/辉光走不透明路径,
    // 表现为"叠加效果变成实心色块"。
    LIMX_EXPECT_TRUE(IsBlendedMode(EMaterialBlendMode::Additive));
}

LIMX_TEST(BlendMode, ClassificationIsExhaustive)
{
    // 逐个列举全部取值 —— 新增模式而忘了归类时, 这条用例不会自动失败,
    // 但计数断言会提醒有取值没被覆盖到。
    const EMaterialBlendMode all[] = {
        EMaterialBlendMode::Opaque,
        EMaterialBlendMode::Masked,
        EMaterialBlendMode::Translucent,
        EMaterialBlendMode::Additive,
    };

    UInt32 blendedCount = 0;

    for (SizeType i = 0; i < sizeof(all) / sizeof(all[0]); ++i)
    {
        if (IsBlendedMode(all[i]))
        {
            ++blendedCount;
        }
    }

    LIMX_EXPECT_EQ(blendedCount, 2u);
}

// ============================================================================
// FMaterialParams::GetBlendMode — 整数与枚举的往返
// ============================================================================

LIMX_TEST(BlendMode, ParamsRoundTripThroughUInt32)
{
    // BlendMode 在 UBO 里是裸 uint (std140 要求)。往返一致是着色器里
    // BLEND_MASKED 常量能对上的前提。
    FMaterialParams params;

    params.BlendMode = static_cast<UInt32>(EMaterialBlendMode::Masked);
    LIMX_EXPECT_TRUE(params.GetBlendMode() == EMaterialBlendMode::Masked);

    params.BlendMode = static_cast<UInt32>(EMaterialBlendMode::Translucent);
    LIMX_EXPECT_TRUE(params.GetBlendMode() == EMaterialBlendMode::Translucent);

    // 着色器里写死的是数值 1, 这里钉住它
    LIMX_EXPECT_EQ(static_cast<UInt32>(EMaterialBlendMode::Masked), 1u);
}

// ============================================================================
// FTranslucentBackToFrontLess — 排序策略
// ============================================================================

LIMX_TEST(TranslucentSort, FarObjectComesFirst)
{
    FTranslucentBackToFrontLess less;
    less.CameraPosition = FVector3(0.0f, 0.0f, 0.0f);

    // 变量名避开 near / far —— windef.h 把它们定义成了宏, 用作标识符
    // 会被展开成空, 报出一串与本意毫无关系的语法错误。
    const FRenderObject nearObject =
        MakeObjectAt(FVector3(0.0f, 0.0f, 5.0f), "near");
    const FRenderObject farObject =
        MakeObjectAt(FVector3(0.0f, 0.0f, 50.0f), "far");

    LIMX_EXPECT_TRUE(less(farObject, nearObject));
    LIMX_EXPECT_FALSE(less(nearObject, farObject));
}

LIMX_TEST(TranslucentSort, EqualDistanceIsNotLess)
{
    // 严格弱序要求: 距离相同的两个批次互不小于对方。
    // 违反这一点会让某些排序实现进入未定义行为。
    FTranslucentBackToFrontLess less;
    less.CameraPosition = FVector3(0.0f, 0.0f, 0.0f);

    const FRenderObject a = MakeObjectAt(FVector3(3.0f, 0.0f, 0.0f), "a");
    const FRenderObject b = MakeObjectAt(FVector3(0.0f, 3.0f, 0.0f), "b");

    LIMX_EXPECT_FALSE(less(a, b));
    LIMX_EXPECT_FALSE(less(b, a));
}

LIMX_TEST(TranslucentSort, DistanceIsMeasuredFromCameraNotOrigin)
{
    // 相机不在原点时, 排序必须以相机为基准。用原点会在相机远离场景时
    // 给出完全相反的顺序。
    FTranslucentBackToFrontLess less;
    less.CameraPosition = FVector3(0.0f, 0.0f, 100.0f);

    const FRenderObject nearCamera =
        MakeObjectAt(FVector3(0.0f, 0.0f, 90.0f), "nearCamera");
    const FRenderObject nearOrigin =
        MakeObjectAt(FVector3(0.0f, 0.0f, 0.0f), "nearOrigin");

    // 靠近原点的那个离相机更远, 应当排在前面
    LIMX_EXPECT_TRUE(less(nearOrigin, nearCamera));
}

LIMX_TEST(TranslucentSort, SortsWholeListBackToFront)
{
    FTranslucentBackToFrontLess less;
    less.CameraPosition = FVector3(0.0f, 0.0f, -10.0f);

    TArray<FRenderObject> objects;
    objects.Add(MakeObjectAt(FVector3(0.0f, 0.0f, 0.0f), "d10"));
    objects.Add(MakeObjectAt(FVector3(0.0f, 0.0f, 40.0f), "d50"));
    objects.Add(MakeObjectAt(FVector3(0.0f, 0.0f, 10.0f), "d20"));
    objects.Add(MakeObjectAt(FVector3(0.0f, 0.0f, 20.0f), "d30"));

    Sort(objects.GetData(), objects.GetSize(), less);

    // 由远及近
    LIMX_EXPECT_TRUE(objects[0].DebugName == FString("d50"));
    LIMX_EXPECT_TRUE(objects[1].DebugName == FString("d30"));
    LIMX_EXPECT_TRUE(objects[2].DebugName == FString("d20"));
    LIMX_EXPECT_TRUE(objects[3].DebugName == FString("d10"));
}

LIMX_TEST(TranslucentSort, OrderIsMonotonicForManyObjects)
{
    FTranslucentBackToFrontLess less;
    less.CameraPosition = FVector3(2.0f, 3.0f, -5.0f);

    TArray<FRenderObject> objects;

    // 刻意以乱序插入
    for (Int32 i = 0; i < 64; ++i)
    {
        const Float32 z = static_cast<Float32>((i * 37) % 64);
        objects.Add(MakeObjectAt(FVector3(0.0f, 0.0f, z), "obj"));
    }

    Sort(objects.GetData(), objects.GetSize(), less);

    // 排序后距离必须单调不增
    Float32 previousDistance = 1.0e30f;

    for (SizeType i = 0; i < objects.GetSize(); ++i)
    {
        const Float32 distance =
            (objects[i].WorldBounds.GetCenter() -
             less.CameraPosition).LengthSquared();

        LIMX_EXPECT_TRUE(distance <= previousDistance);
        previousDistance = distance;
    }
}
