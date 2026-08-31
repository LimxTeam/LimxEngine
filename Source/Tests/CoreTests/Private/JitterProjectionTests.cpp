/*******************************************************************************
 * 文件: JitterProjectionTests.cpp
 * 创建时间: 2026-08-31
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   TAA 抖动作用到投影矩阵之后的性质 — 偏移必须与深度无关
 *
 * 设计哲学:
 *   这条性质在画面上看不出来。把抖动写进第 3 列 (M[0][3]) 而不是第 2 列
 *   时, 画面同样"在抖", 只是抖动幅度随深度衰减: 近处物体抖得多、远处几乎
 *   不抖。表现是 TAA 对远景没效果, 而那会被归因到 TAA 的历史权重或深度
 *   钳制上 —— 离真正的原因隔了三层。
 *
 *   GPU 侧的 --gbuffer-check 抓不到这个: 它验证的是覆盖掩码逐帧变化, 而
 *   两种写法都会让覆盖变化。只有把矩阵变换的结果逐点算出来才分得清。
 *
 *   所以判据是: 取同一条视线上深度差 100 倍的两个点, 它们的 NDC 偏移必须
 *   相等, 且都等于给定的抖动量。
 *
 ******************************************************************************/

#include "CoreTests/CoreTestsMinimal.h"

#include "Core/Math/FHalton.h"
#include "Core/Math/FMatrix.h"
#include "Core/Math/FVector.h"

using namespace Limx;

namespace
{

/// 视空间点 → NDC (做透视除法)
FVector2 ProjectToNdc(const FMatrix& projection, FVector3 viewPosition)
{
    const FVector4 clip = projection.TransformVector4(
        FVector4(viewPosition.X, viewPosition.Y, viewPosition.Z, 1.0f));

    return FVector2(clip.X / clip.W, clip.Y / clip.W);
}

FMatrix TestProjection()
{
    // 60 度垂直视场, 16:9, 近 0.1 远 1000 —— 与引擎默认相机同量级
    return FMatrix::Perspective(1.0471975512f, 16.0f / 9.0f, 0.1f, 1000.0f);
}

} // namespace

// ============================================================================
// 与深度无关 —— 本文件存在的理由
// ============================================================================

LIMX_TEST(JitterProjection, OffsetIsIndependentOfDepth)
{
    const FMatrix baseProjection = TestProjection();

    FMatrix jittered = baseProjection;
    const FVector2 jitter(0.003f, -0.002f);
    ApplyJitterToProjection(jittered, jitter);

    // 同一条视线上深度差 100 倍的两个点。视空间 -Z 为前方。
    //
    // 两点必须在同一条视线上, 否则 NDC 本来就不同, 比较偏移就没有意义 ——
    // 所以 x、y 随 z 等比例放大。
    const FVector3 near(0.2f, 0.15f, -1.0f);
    const FVector3 far(20.0f, 15.0f, -100.0f);

    const FVector2 nearBefore = ProjectToNdc(baseProjection, near);
    const FVector2 nearAfter  = ProjectToNdc(jittered, near);

    const FVector2 farBefore = ProjectToNdc(baseProjection, far);
    const FVector2 farAfter  = ProjectToNdc(jittered, far);

    // 先确认两点确实在同一条视线上 —— 不然这条用例在验一个假前提
    LIMX_EXPECT_NEAR(nearBefore.X, farBefore.X, 1.0e-5f);
    LIMX_EXPECT_NEAR(nearBefore.Y, farBefore.Y, 1.0e-5f);

    const Float32 nearShiftX = nearAfter.X - nearBefore.X;
    const Float32 nearShiftY = nearAfter.Y - nearBefore.Y;
    const Float32 farShiftX  = farAfter.X - farBefore.X;
    const Float32 farShiftY  = farAfter.Y - farBefore.Y;

    // 两处的偏移必须相等
    LIMX_EXPECT_NEAR(nearShiftX, farShiftX, 1.0e-6f);
    LIMX_EXPECT_NEAR(nearShiftY, farShiftY, 1.0e-6f);

    // 而且必须**等于给定的抖动量**。只比"两处相等"是不够的: 一个恒为零
    // 的实现 (抖动根本没生效) 也满足"两处相等"。
    LIMX_EXPECT_NEAR(nearShiftX, jitter.X, 1.0e-6f);
    LIMX_EXPECT_NEAR(nearShiftY, jitter.Y, 1.0e-6f);
}

LIMX_TEST(JitterProjection, OffsetHoldsAcrossTheWholeDepthRange)
{
    const FMatrix baseProjection = TestProjection();

    FMatrix jittered = baseProjection;
    const FVector2 jitter(-0.0015f, 0.0008f);
    ApplyJitterToProjection(jittered, jitter);

    // 从近平面附近一路扫到远平面附近。深度相关的错误在近处误差最小,
    // 只测一个深度很容易恰好落在误差可以忽略的那一段。
    const Float32 depths[] =
    {
        0.15f, 0.5f, 1.0f, 5.0f, 25.0f, 100.0f, 500.0f, 900.0f,
    };

    for (SizeType i = 0; i < sizeof(depths) / sizeof(depths[0]); ++i)
    {
        const Float32 z = depths[i];

        // 同一条视线: x、y 按 z 等比例缩放
        const FVector3 point(0.3f * z, -0.2f * z, -z);

        const FVector2 before = ProjectToNdc(baseProjection, point);
        const FVector2 after  = ProjectToNdc(jittered, point);

        LIMX_EXPECT_NEAR(after.X - before.X, jitter.X, 1.0e-5f);
        LIMX_EXPECT_NEAR(after.Y - before.Y, jitter.Y, 1.0e-5f);
    }
}

// ============================================================================
// 不该动的东西不能动
// ============================================================================

LIMX_TEST(JitterProjection, ZeroJitterLeavesTheMatrixBitIdentical)
{
    // 抖动关闭时走的是同一条代码路径 (偏移为零), 结果必须与未抖动的矩阵
    // 逐位相同 —— 差一个 ulp 就会让"静止相机速度精确为零"这条不变量失效。
    const FMatrix baseProjection = TestProjection();

    FMatrix jittered = baseProjection;
    ApplyJitterToProjection(jittered, FVector2(0.0f, 0.0f));

    for (Int32 row = 0; row < 4; ++row)
    {
        for (Int32 col = 0; col < 4; ++col)
        {
            LIMX_EXPECT_EQ(jittered.M[row][col], baseProjection.M[row][col]);
        }
    }
}

LIMX_TEST(JitterProjection, DepthMappingIsUntouched)
{
    // 抖动只该改变横纵位置, 不该改变深度。改到深度上会让深度预通道与前向
    // Pass 的 Equal 测试失配 —— 整个画面消失。
    const FMatrix baseProjection = TestProjection();

    FMatrix jittered = baseProjection;
    ApplyJitterToProjection(jittered, FVector2(0.004f, 0.004f));

    const Float32 depths[] = { 0.15f, 1.0f, 50.0f, 900.0f };

    for (SizeType i = 0; i < sizeof(depths) / sizeof(depths[0]); ++i)
    {
        const FVector3 point(0.3f * depths[i], -0.2f * depths[i], -depths[i]);

        const FVector4 before = baseProjection.TransformVector4(
            FVector4(point.X, point.Y, point.Z, 1.0f));
        const FVector4 after = jittered.TransformVector4(
            FVector4(point.X, point.Y, point.Z, 1.0f));

        LIMX_EXPECT_EQ(after.Z, before.Z);
        LIMX_EXPECT_EQ(after.W, before.W);
    }
}

// ============================================================================
// 与序列连起来
// ============================================================================

LIMX_TEST(JitterProjection, SequenceProducesDistinctOffsetsWithinOnePeriod)
{
    // 一个周期内的 16 个偏移必须两两不同。相同的话那两帧对 TAA 是重复的
    // 采样, 等于白丢一帧, 而画面上只表现为收敛稍慢。
    constexpr UInt32 kPeriod = 16;

    Float32 offsetsX[kPeriod] = {};
    Float32 offsetsY[kPeriod] = {};

    for (UInt32 i = 0; i < kPeriod; ++i)
    {
        HaltonJitterPixels(i + 1u, offsetsX[i], offsetsY[i]);
    }

    for (UInt32 i = 0; i < kPeriod; ++i)
    {
        for (UInt32 j = i + 1u; j < kPeriod; ++j)
        {
            LIMX_EXPECT_TRUE(offsetsX[i] != offsetsX[j] ||
                             offsetsY[i] != offsetsY[j]);
        }
    }
}
