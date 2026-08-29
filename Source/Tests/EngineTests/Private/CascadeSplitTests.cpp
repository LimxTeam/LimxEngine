/*******************************************************************************
 * 文件: CascadeSplitTests.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   级联阴影切分算法的单元测试
 *
 * 设计哲学:
 *   切分距离决定每一级覆盖多远, 而它的错误方式全都是**静默**的:
 *   序列不严格递增 → 某一级退化为零厚度, 着色器仍会去采样它, 采到的是
 *   未初始化的深度; 最远一级与覆盖距离差几个 ulp → 最远处留下一圈没有
 *   阴影的环带; 近平面为零 → 对数项得到非有限值, 一路传进正交矩阵,
 *   表现为阴影整体消失。
 *
 *   三种症状都不会报错, 也都不像是"切分算错了"。因此这里把不变式逐条
 *   钉死: 单调性、端点精确、lambda 两端退化为纯对数/纯均匀、退化输入的
 *   兜底行为。
 *
 * 依赖关系:
 *   内部: EngineTests/EngineTestsMinimal.h, Renderer/RenderPass/FShadowPass.h
 *
 ******************************************************************************/

#include "EngineTests/EngineTestsMinimal.h"
#include "Renderer/RenderPass/FShadowPass.h"

using namespace Limx;

namespace
{

constexpr UInt32 kMaxTestCascades = 8;

} // namespace

// ============================================================================
// 基本形状
// ============================================================================

LIMX_TEST(CascadeSplit, FirstSplitIsNearPlane)
{
    Float32 splits[4] = {};

    FShadowPass::ComputeCascadeSplits(0.1f, 100.0f, 3, 0.75f, splits);

    LIMX_EXPECT_NEAR(splits[0], 0.1f, 1.0e-5f);
}

LIMX_TEST(CascadeSplit, LastSplitIsExactlyShadowDistance)
{
    // 最远一级必须**精确**等于覆盖距离。着色器用 "距离 > 最后一级边界
    // 则判为无遮挡", 差几个 ulp 就会在最远处留下一圈没有阴影的环带。
    Float32 splits[4] = {};

    FShadowPass::ComputeCascadeSplits(0.1f, 60.0f, 3, 0.75f, splits);

    LIMX_EXPECT_TRUE(splits[3] == 60.0f);
}

LIMX_TEST(CascadeSplit, IsStrictlyIncreasing)
{
    // 序列不严格递增会让某一级退化为零厚度, 而着色器仍会采样它
    const Float32 lambdas[] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };

    for (SizeType li = 0; li < sizeof(lambdas) / sizeof(lambdas[0]); ++li)
    {
        for (UInt32 count = 1; count <= 6; ++count)
        {
            Float32 splits[kMaxTestCascades + 1] = {};

            FShadowPass::ComputeCascadeSplits(0.1f, 200.0f, count,
                                              lambdas[li], splits);

            for (UInt32 i = 0; i < count; ++i)
            {
                LIMX_EXPECT_TRUE(splits[i] < splits[i + 1]);
            }
        }
    }
}

LIMX_TEST(CascadeSplit, AllSplitsWithinRange)
{
    Float32 splits[4] = {};

    FShadowPass::ComputeCascadeSplits(0.5f, 80.0f, 3, 0.75f, splits);

    for (UInt32 i = 0; i <= 3; ++i)
    {
        LIMX_EXPECT_TRUE(splits[i] >= 0.5f);
        LIMX_EXPECT_TRUE(splits[i] <= 80.0f);
    }
}

// ============================================================================
// lambda 的两个端点
// ============================================================================

LIMX_TEST(CascadeSplit, LambdaZeroIsUniform)
{
    // lambda=0 应退化为均匀切分: 每级厚度相同
    Float32 splits[5] = {};

    FShadowPass::ComputeCascadeSplits(0.0f, 100.0f, 4, 0.0f, splits);

    const Float32 expectedStep = (splits[4] - splits[0]) / 4.0f;

    for (UInt32 i = 0; i < 4; ++i)
    {
        const Float32 step = splits[i + 1] - splits[i];
        LIMX_EXPECT_NEAR(step, expectedStep, 0.01f);
    }
}

LIMX_TEST(CascadeSplit, LambdaOneIsLogarithmic)
{
    // lambda=1 应退化为对数切分: 相邻两级的**比值**相同
    Float32 splits[4] = {};

    FShadowPass::ComputeCascadeSplits(1.0f, 1000.0f, 3, 1.0f, splits);

    const Float32 ratio0 = splits[1] / splits[0];
    const Float32 ratio1 = splits[2] / splits[1];
    const Float32 ratio2 = splits[3] / splits[2];

    LIMX_EXPECT_NEAR(ratio1, ratio0, 0.05f);
    LIMX_EXPECT_NEAR(ratio2, ratio0, 0.05f);
}

LIMX_TEST(CascadeSplit, LogarithmicPutsMorePrecisionNear)
{
    // 对数切分的第一级必须比均匀切分的第一级薄 —— 这正是它的意义所在:
    // 把精度花在脚下, 而非三十米外。
    Float32 uniform[4] = {};
    Float32 logarithmic[4] = {};

    FShadowPass::ComputeCascadeSplits(0.1f, 100.0f, 3, 0.0f, uniform);
    FShadowPass::ComputeCascadeSplits(0.1f, 100.0f, 3, 1.0f, logarithmic);

    LIMX_EXPECT_TRUE(logarithmic[1] < uniform[1]);
    LIMX_EXPECT_TRUE(logarithmic[2] < uniform[2]);
}

// ============================================================================
// 退化输入
// ============================================================================

LIMX_TEST(CascadeSplit, ZeroNearPlaneIsClamped)
{
    // 近平面为零时对数项要对 far/0 取幂 —— 非有限值会一路传进正交矩阵,
    // 表现为阴影整体消失。必须被夹到一个极小正数。
    Float32 splits[4] = {};

    FShadowPass::ComputeCascadeSplits(0.0f, 50.0f, 3, 1.0f, splits);

    for (UInt32 i = 0; i <= 3; ++i)
    {
        LIMX_EXPECT_TRUE(splits[i] > 0.0f);

        // 有限性检查: 非有限值与自身比较不相等
        LIMX_EXPECT_TRUE(splits[i] == splits[i]);
        LIMX_EXPECT_TRUE(splits[i] < 1.0e30f);
    }

    for (UInt32 i = 0; i < 3; ++i)
    {
        LIMX_EXPECT_TRUE(splits[i] < splits[i + 1]);
    }
}

LIMX_TEST(CascadeSplit, ShadowDistanceBelowNearIsClamped)
{
    // 覆盖距离小于近平面时整个级联会退化为零厚度甚至倒序
    Float32 splits[4] = {};

    FShadowPass::ComputeCascadeSplits(10.0f, 2.0f, 3, 0.75f, splits);

    for (UInt32 i = 0; i < 3; ++i)
    {
        LIMX_EXPECT_TRUE(splits[i] < splits[i + 1]);
    }

    // 兜底后的最远距离至少比近平面远一个单位
    LIMX_EXPECT_TRUE(splits[3] >= 11.0f);
}

LIMX_TEST(CascadeSplit, LambdaIsClampedToUnitRange)
{
    // 越界的 lambda 不应产生越界的切分
    Float32 low[4]  = {};
    Float32 high[4] = {};

    FShadowPass::ComputeCascadeSplits(0.1f, 100.0f, 3, -5.0f, low);
    FShadowPass::ComputeCascadeSplits(0.1f, 100.0f, 3, 5.0f, high);

    for (UInt32 i = 0; i < 3; ++i)
    {
        LIMX_EXPECT_TRUE(low[i] < low[i + 1]);
        LIMX_EXPECT_TRUE(high[i] < high[i + 1]);
    }

    // 夹取后应分别等价于 lambda=0 与 lambda=1
    Float32 uniform[4] = {};
    Float32 logarithmic[4] = {};

    FShadowPass::ComputeCascadeSplits(0.1f, 100.0f, 3, 0.0f, uniform);
    FShadowPass::ComputeCascadeSplits(0.1f, 100.0f, 3, 1.0f, logarithmic);

    for (UInt32 i = 0; i <= 3; ++i)
    {
        LIMX_EXPECT_NEAR(low[i], uniform[i], 0.01f);
        LIMX_EXPECT_NEAR(high[i], logarithmic[i], 0.01f);
    }
}

LIMX_TEST(CascadeSplit, NullOutputAndZeroCountAreSafe)
{
    // 防御性: 这两种输入不该崩
    FShadowPass::ComputeCascadeSplits(0.1f, 100.0f, 3, 0.75f, nullptr);

    Float32 splits[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
    FShadowPass::ComputeCascadeSplits(0.1f, 100.0f, 0, 0.75f, splits);

    // 级数为零时不应写入任何内容
    LIMX_EXPECT_TRUE(splits[0] == -1.0f);
}

LIMX_TEST(CascadeSplit, SingleCascadeSpansWholeRange)
{
    Float32 splits[2] = {};

    FShadowPass::ComputeCascadeSplits(0.5f, 40.0f, 1, 0.75f, splits);

    LIMX_EXPECT_NEAR(splits[0], 0.5f, 1.0e-5f);
    LIMX_EXPECT_TRUE(splits[1] == 40.0f);
}
