/*******************************************************************************
 * 文件: OctahedralTests.cpp
 * 创建时间: 2026-08-31
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   八面体法线编解码的往返用例 — 覆盖边界方向
 *
 * 设计哲学:
 *   风险全在边界上。这套编码在球面内部区域怎么写都大致能用, 出问题的地方
 *   永远是那几处: z 恰好为 0 (上下半球的分界)、某个分量恰好为 0 (轴向)、
 *   以及 GLSL 的 sign(0) 返回 0 这个坑 —— 后者会让整条向量塌成零向量。
 *
 *   这些方向在真实场景里概率不高但一定会出现 (任何轴对齐的面), 而它们出错
 *   的表现是"某些面的 AO 不对", 看起来像参数没调好。
 *
 *   这份用例测的是 CPU 实现 (FOctahedral.h)。它与着色器里的
 *   gbuffer_common.h 逐行对应, 但**没有编译期保障** —— 两者不一致时这里
 *   全绿而画面上法线偏斜。改任一份都要同步改另一份。
 *
 ******************************************************************************/

#include "EngineTests/EngineTestsMinimal.h"

#include "RenderCore/Profiling/FOctahedral.h"

using namespace Limx;

namespace
{

/// 半精度浮点在 [-1,1] 的精度约 5e-4, 编码本身的误差远小于此。
///
/// 取 1e-5 是 CPU 实现的容差 —— 它只做 float 运算, 不经过 RG16_SFLOAT。
/// 真实管线上的误差由存储格式主导, 那一项在 GPU 侧验。
constexpr Float32 kTolerance = 1.0e-5f;

/// 编码再解码, 返回与原方向的夹角余弦
Float32 RoundTripDot(FVector3 n)
{
    const FVector2 encoded = EncodeOctahedralNormal(n);
    const FVector3 decoded = DecodeOctahedralNormal(encoded);

    return n.X * decoded.X + n.Y * decoded.Y + n.Z * decoded.Z;
}

FVector3 Normalized(Float32 x, Float32 y, Float32 z)
{
    const Float32 length = FMath::Sqrt(x * x + y * y + z * z);

    return FVector3(x / length, y / length, z / length);
}

} // namespace

// ============================================================================
// 轴向
// ============================================================================

LIMX_TEST(Octahedral, AxisDirectionsRoundTrip)
{
    // 六个轴向 —— 每一个都有两个分量恰好为 0, 正是 sign(0) 那个坑的触发点
    const FVector3 axes[6] =
    {
        FVector3( 1.0f,  0.0f,  0.0f),
        FVector3(-1.0f,  0.0f,  0.0f),
        FVector3( 0.0f,  1.0f,  0.0f),
        FVector3( 0.0f, -1.0f,  0.0f),
        FVector3( 0.0f,  0.0f,  1.0f),
        FVector3( 0.0f,  0.0f, -1.0f),
    };

    for (SizeType i = 0; i < 6; ++i)
    {
        LIMX_EXPECT_NEAR(RoundTripDot(axes[i]), 1.0f, kTolerance);
    }
}

// ============================================================================
// 半球分界
// ============================================================================

LIMX_TEST(Octahedral, EquatorRoundTrip)
{
    // z = 0 是上下半球的分界。编码在这里走的是"折叠"分支的边缘, 而折叠
    // 公式与不折叠公式必须在这条线上给出同样的结果 —— 不然赤道附近会有
    // 一圈不连续, 表现为水平面与竖直面交界处的 AO 突变。
    constexpr SizeType kSamples = 64;

    for (SizeType i = 0; i < kSamples; ++i)
    {
        const Float32 angle =
            2.0f * FMath::kPi * static_cast<Float32>(i) /
            static_cast<Float32>(kSamples);

        const FVector3 n(FMath::Cos(angle), FMath::Sin(angle), 0.0f);

        LIMX_EXPECT_NEAR(RoundTripDot(n), 1.0f, kTolerance);
    }
}

LIMX_TEST(Octahedral, JustBelowEquatorRoundTrip)
{
    // 紧贴赤道下方 —— 走折叠分支。与上面那条一起, 覆盖分界的两侧。
    constexpr SizeType kSamples = 64;

    for (SizeType i = 0; i < kSamples; ++i)
    {
        const Float32 angle =
            2.0f * FMath::kPi * static_cast<Float32>(i) /
            static_cast<Float32>(kSamples);

        const FVector3 n = Normalized(FMath::Cos(angle),
                                      FMath::Sin(angle),
                                      -1.0e-4f);

        LIMX_EXPECT_NEAR(RoundTripDot(n), 1.0f, kTolerance);
    }
}

// ============================================================================
// 全球面
// ============================================================================

LIMX_TEST(Octahedral, WholeSphereRoundTrip)
{
    // 均匀铺满球面。用斐波那契球而非经纬网格 —— 后者在两极密集、赤道稀疏,
    // 恰好在最容易出错的赤道附近采样最少。
    constexpr SizeType kSamples = 4096;

    const Float32 goldenAngle = FMath::kPi * (3.0f - FMath::Sqrt(5.0f));

    Float32 worstDot = 1.0f;

    for (SizeType i = 0; i < kSamples; ++i)
    {
        const Float32 t = static_cast<Float32>(i) /
                          static_cast<Float32>(kSamples - 1);

        const Float32 z      = 1.0f - 2.0f * t;
        const Float32 radius = FMath::Sqrt(FMath::Max(0.0f, 1.0f - z * z));
        const Float32 theta  = goldenAngle * static_cast<Float32>(i);

        const FVector3 n(radius * FMath::Cos(theta),
                         radius * FMath::Sin(theta),
                         z);

        const Float32 dot = RoundTripDot(n);

        if (dot < worstDot)
        {
            worstDot = dot;
        }
    }

    LIMX_EXPECT_NEAR(worstDot, 1.0f, kTolerance);
}

// ============================================================================
// 退化输入
// ============================================================================

LIMX_TEST(Octahedral, ZeroVectorDecodesToValidDirection)
{
    // 零法线来自源资产缺失 (FMeshVertex::Normal 的注释写明可能是零向量)。
    //
    // 它没有"正确答案", 但必须解出一个**单位**向量 —— 否则 GTAO 的余弦
    // 加权会算出无意义的值, 而那看起来像是那个物体的 AO 参数不对。
    const FVector2 encoded = EncodeOctahedralNormal(FVector3(0.0f, 0.0f, 0.0f));
    const FVector3 decoded = DecodeOctahedralNormal(encoded);

    const Float32 lengthSq = decoded.X * decoded.X +
                             decoded.Y * decoded.Y +
                             decoded.Z * decoded.Z;

    LIMX_EXPECT_NEAR(lengthSq, 1.0f, kTolerance);
}

LIMX_TEST(Octahedral, EncodedRangeStaysWithinUnitSquare)
{
    // 编码结果必须落在 [-1,1]^2 —— 附件是 RG16_SFLOAT, 超出这个范围虽然
    // 存得下, 但解码公式假定了它。越界会让解码出的 z 为负而折叠分支又
    // 判不出来, 结果是一个非单位向量。
    constexpr SizeType kSamples = 1024;

    const Float32 goldenAngle = FMath::kPi * (3.0f - FMath::Sqrt(5.0f));

    for (SizeType i = 0; i < kSamples; ++i)
    {
        const Float32 t = static_cast<Float32>(i) /
                          static_cast<Float32>(kSamples - 1);

        const Float32 z      = 1.0f - 2.0f * t;
        const Float32 radius = FMath::Sqrt(FMath::Max(0.0f, 1.0f - z * z));
        const Float32 theta  = goldenAngle * static_cast<Float32>(i);

        const FVector2 e = EncodeOctahedralNormal(
            FVector3(radius * FMath::Cos(theta),
                     radius * FMath::Sin(theta),
                     z));

        LIMX_EXPECT_LE(FMath::Abs(e.X), 1.0f + kTolerance);
        LIMX_EXPECT_LE(FMath::Abs(e.Y), 1.0f + kTolerance);
    }
}
