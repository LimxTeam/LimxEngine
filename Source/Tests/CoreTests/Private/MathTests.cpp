/*******************************************************************************
 * 文件: MathTests.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   数学库单元测试 — FVector2/3/4、FQuat、FMatrix、FTransform、FMath
 *   以数学恒等式而非硬编码数值为断言依据
 *
 * 设计哲学:
 *   用恒等式而非期望值 — 断言 "M * M⁻¹ == I"、"|normalize(v)| == 1"、
 *   "Cross(X,Y) == Z" 这类必然成立的关系，而不是把某次运行的输出抄成期望值。
 *   前者能发现真正的数学错误，后者只能锁死当前实现（哪怕它是错的）。
 *
 *   约定必须被测试固定 — 矩阵是行主序存储、列向量语义 (v' = M·v)、平移位于
 *   第四列；坐标系为右手系。这些约定一旦被无意改动，整条渲染管线都会静默出错，
 *   因此用例显式断言它们，使约定本身成为回归目标。
 *
 * 技术特性:
 *   - 浮点比较一律走容差, 绝不使用 == 比较 Float32
 *   - 逆矩阵、四元数共轭等可逆运算做往返验证 (round-trip)
 *   - 旋转用例同时校验方向与手性, 避免符号翻转被漏过
 *
 * 依赖关系:
 *   内部: CoreTests/CoreTestsMinimal.h
 *
 * 注意事项:
 *   Float32 精度下累积误差明显, 容差取 1e-4 而非 1e-6
 *
 ******************************************************************************/

#include "CoreTests/CoreTestsMinimal.h"

using namespace Limx;

namespace
{

/// Float32 运算的通用容差 — 单精度下多步运算的累积误差量级
constexpr Float32 kTolerance = 1.0e-4f;

/// 断言两个向量在容差内相等
bool VectorsNearlyEqual(const FVector3& a, const FVector3& b,
                        Float32 tolerance = kTolerance)
{
    return FMath::Abs(a.X - b.X) <= tolerance &&
           FMath::Abs(a.Y - b.Y) <= tolerance &&
           FMath::Abs(a.Z - b.Z) <= tolerance;
}

/// 断言两个矩阵在容差内相等
bool MatricesNearlyEqual(const FMatrix& a, const FMatrix& b,
                         Float32 tolerance = kTolerance)
{
    for (Int32 row = 0; row < 4; ++row)
    {
        for (Int32 col = 0; col < 4; ++col)
        {
            if (FMath::Abs(a.M[row][col] - b.M[row][col]) > tolerance)
            {
                return false;
            }
        }
    }

    return true;
}

} // namespace

// ============================================================================
// FVector3 — 基本运算
// ============================================================================

LIMX_TEST(FVector3, DefaultIsZero)
{
    FVector3 v;

    LIMX_EXPECT_NEAR(v.X, 0.0f, kTolerance);
    LIMX_EXPECT_NEAR(v.Y, 0.0f, kTolerance);
    LIMX_EXPECT_NEAR(v.Z, 0.0f, kTolerance);
    LIMX_EXPECT_TRUE(v.IsNearlyZero());
}

LIMX_TEST(FVector3, LengthMatchesPythagoras)
{
    // 3-4-5 直角三角形
    FVector3 v(3.0f, 4.0f, 0.0f);

    LIMX_EXPECT_NEAR(v.Length(), 5.0f, kTolerance);
    LIMX_EXPECT_NEAR(v.LengthSquared(), 25.0f, kTolerance);
}

LIMX_TEST(FVector3, NormalizeProducesUnitLength)
{
    FVector3 v(3.0f, 4.0f, 12.0f);
    FVector3 normalized = v.GetSafeNormal();

    LIMX_EXPECT_NEAR(normalized.Length(), 1.0f, kTolerance);
    LIMX_EXPECT_TRUE(normalized.IsNormalized());

    // 方向不变: 归一化向量乘回原长度应还原
    FVector3 restored = normalized * v.Length();
    LIMX_EXPECT_TRUE(VectorsNearlyEqual(restored, v));
}

LIMX_TEST(FVector3, SafeNormalOfZeroDoesNotProduceNaN)
{
    FVector3 zero(0.0f, 0.0f, 0.0f);
    FVector3 normalized = zero.GetSafeNormal();

    // 零向量归一化必须回退到确定值而非产生 NaN
    LIMX_EXPECT_FALSE(FMath::IsNaN(normalized.X));
    LIMX_EXPECT_FALSE(FMath::IsNaN(normalized.Y));
    LIMX_EXPECT_FALSE(FMath::IsNaN(normalized.Z));
}

LIMX_TEST(FVector3, DotOfOrthogonalAxesIsZero)
{
    const FVector3 axisX(1.0f, 0.0f, 0.0f);
    const FVector3 axisY(0.0f, 1.0f, 0.0f);
    const FVector3 axisZ(0.0f, 0.0f, 1.0f);

    LIMX_EXPECT_NEAR(FVector3::Dot(axisX, axisY), 0.0f, kTolerance);
    LIMX_EXPECT_NEAR(FVector3::Dot(axisY, axisZ), 0.0f, kTolerance);
    LIMX_EXPECT_NEAR(FVector3::Dot(axisZ, axisX), 0.0f, kTolerance);

    // 自点积等于长度平方
    LIMX_EXPECT_NEAR(FVector3::Dot(axisX, axisX), 1.0f, kTolerance);
}

LIMX_TEST(FVector3, DotIsCommutative)
{
    FVector3 a(1.0f, 2.0f, 3.0f);
    FVector3 b(4.0f, -5.0f, 6.0f);

    LIMX_EXPECT_NEAR(FVector3::Dot(a, b), FVector3::Dot(b, a), kTolerance);
}

LIMX_TEST(FVector3, CrossFollowsRightHandRule)
{
    const FVector3 axisX(1.0f, 0.0f, 0.0f);
    const FVector3 axisY(0.0f, 1.0f, 0.0f);
    const FVector3 axisZ(0.0f, 0.0f, 1.0f);

    // 右手系的定义性恒等式 — 手性一旦翻转, 整条渲染管线的法线与剔除都会错
    LIMX_EXPECT_TRUE(VectorsNearlyEqual(FVector3::Cross(axisX, axisY), axisZ));
    LIMX_EXPECT_TRUE(VectorsNearlyEqual(FVector3::Cross(axisY, axisZ), axisX));
    LIMX_EXPECT_TRUE(VectorsNearlyEqual(FVector3::Cross(axisZ, axisX), axisY));
}

LIMX_TEST(FVector3, CrossIsAntiCommutative)
{
    FVector3 a(1.0f, 2.0f, 3.0f);
    FVector3 b(4.0f, 5.0f, 6.0f);

    FVector3 ab = FVector3::Cross(a, b);
    FVector3 ba = FVector3::Cross(b, a);

    LIMX_EXPECT_TRUE(VectorsNearlyEqual(ab, ba * -1.0f));
}

LIMX_TEST(FVector3, CrossIsOrthogonalToBothOperands)
{
    FVector3 a(1.0f, 2.0f, 3.0f);
    FVector3 b(-4.0f, 5.0f, 6.0f);

    FVector3 cross = FVector3::Cross(a, b);

    LIMX_EXPECT_NEAR(FVector3::Dot(cross, a), 0.0f, 1.0e-3f);
    LIMX_EXPECT_NEAR(FVector3::Dot(cross, b), 0.0f, 1.0e-3f);
}

LIMX_TEST(FVector3, CrossOfParallelVectorsIsZero)
{
    FVector3 a(2.0f, 4.0f, 6.0f);
    FVector3 b = a * 3.0f;

    FVector3 cross = FVector3::Cross(a, b);

    LIMX_EXPECT_TRUE(cross.IsNearlyZero(1.0e-3f));
}

LIMX_TEST(FVector3, DistanceIsSymmetric)
{
    FVector3 a(1.0f, 2.0f, 3.0f);
    FVector3 b(4.0f, 6.0f, 3.0f);

    LIMX_EXPECT_NEAR(FVector3::Distance(a, b), 5.0f, kTolerance);
    LIMX_EXPECT_NEAR(FVector3::Distance(b, a), 5.0f, kTolerance);
    LIMX_EXPECT_NEAR(FVector3::DistanceSquared(a, b), 25.0f, kTolerance);
}

LIMX_TEST(FVector3, LerpHitsEndpoints)
{
    FVector3 a(0.0f, 0.0f, 0.0f);
    FVector3 b(10.0f, 20.0f, 30.0f);

    LIMX_EXPECT_TRUE(VectorsNearlyEqual(FVector3::Lerp(a, b, 0.0f), a));
    LIMX_EXPECT_TRUE(VectorsNearlyEqual(FVector3::Lerp(a, b, 1.0f), b));
    LIMX_EXPECT_TRUE(VectorsNearlyEqual(FVector3::Lerp(a, b, 0.5f),
                                        FVector3(5.0f, 10.0f, 15.0f)));
}

LIMX_TEST(FVector3, ReflectMirrorsAcrossNormal)
{
    // 沿 -Y 入射, 撞击 +Y 法线的平面, 应反射为 +Y
    FVector3 incident(0.0f, -1.0f, 0.0f);
    FVector3 normal(0.0f, 1.0f, 0.0f);

    FVector3 reflected = incident.Reflect(normal);

    LIMX_EXPECT_TRUE(VectorsNearlyEqual(reflected, FVector3(0.0f, 1.0f, 0.0f)));
}

LIMX_TEST(FVector3, ReflectPreservesLength)
{
    FVector3 incident(1.0f, -2.0f, 0.5f);
    FVector3 normal = FVector3(0.3f, 1.0f, -0.2f).GetSafeNormal();

    FVector3 reflected = incident.Reflect(normal);

    // 镜面反射是等距变换, 长度必须守恒
    LIMX_EXPECT_NEAR(reflected.Length(), incident.Length(), 1.0e-3f);
}

LIMX_TEST(FVector3, ProjectOnToGivesParallelComponent)
{
    FVector3 v(3.0f, 4.0f, 0.0f);
    FVector3 direction(1.0f, 0.0f, 0.0f);

    FVector3 projected = v.ProjectOnTo(direction);

    LIMX_EXPECT_TRUE(VectorsNearlyEqual(projected, FVector3(3.0f, 0.0f, 0.0f)));
}

LIMX_TEST(FVector3, ProjectOnToPlaneRemovesNormalComponent)
{
    FVector3 v(3.0f, 4.0f, 5.0f);
    FVector3 planeNormal(0.0f, 1.0f, 0.0f);

    FVector3 projected = v.ProjectOnToPlane(planeNormal);

    // 投影结果在平面内, 与法线正交
    LIMX_EXPECT_NEAR(FVector3::Dot(projected, planeNormal), 0.0f, kTolerance);
    LIMX_EXPECT_NEAR(projected.Y, 0.0f, kTolerance);
}

// ============================================================================
// FQuat — 四元数旋转
// ============================================================================

LIMX_TEST(FQuat, DefaultIsIdentity)
{
    FQuat q;

    LIMX_EXPECT_NEAR(q.W, 1.0f, kTolerance);
    LIMX_EXPECT_NEAR(q.X, 0.0f, kTolerance);
    LIMX_EXPECT_TRUE(q.IsNormalized());

    // 单位四元数不改变任何向量
    FVector3 v(1.0f, 2.0f, 3.0f);
    LIMX_EXPECT_TRUE(VectorsNearlyEqual(q.RotateVector(v), v));
}

LIMX_TEST(FQuat, FromAxisAngleIsNormalized)
{
    FQuat q = FQuat::FromAxisAngle(FVector3(0.0f, 0.0f, 1.0f),
                                   FMath::kPi * 0.25f);

    LIMX_EXPECT_NEAR(q.Length(), 1.0f, kTolerance);
    LIMX_EXPECT_TRUE(q.IsNormalized());
}

LIMX_TEST(FQuat, RotateNinetyDegreesAboutZMapsXToY)
{
    FQuat q = FQuat::FromAxisAngle(FVector3(0.0f, 0.0f, 1.0f),
                                   FMath::kPi * 0.5f);

    FVector3 rotated = q.RotateVector(FVector3(1.0f, 0.0f, 0.0f));

    // 右手系绕 +Z 逆时针 90°: X 轴转到 Y 轴
    LIMX_EXPECT_TRUE(VectorsNearlyEqual(rotated, FVector3(0.0f, 1.0f, 0.0f),
                                        1.0e-3f));
}

LIMX_TEST(FQuat, RotationAboutAxisLeavesAxisUnchanged)
{
    const FVector3 axis = FVector3(1.0f, 2.0f, 3.0f).GetSafeNormal();
    FQuat q = FQuat::FromAxisAngle(axis, 1.234f);

    FVector3 rotated = q.RotateVector(axis);

    // 旋转轴是旋转的不动点
    LIMX_EXPECT_TRUE(VectorsNearlyEqual(rotated, axis, 1.0e-3f));
}

LIMX_TEST(FQuat, RotationPreservesLength)
{
    FQuat q = FQuat::FromAxisAngle(FVector3(0.3f, -0.5f, 0.8f).GetSafeNormal(),
                                   2.1f);

    FVector3 v(1.0f, -2.0f, 3.0f);
    FVector3 rotated = q.RotateVector(v);

    // 旋转是等距变换
    LIMX_EXPECT_NEAR(rotated.Length(), v.Length(), 1.0e-3f);
}

LIMX_TEST(FQuat, UnrotateUndoesRotate)
{
    FQuat q = FQuat::FromAxisAngle(FVector3(0.0f, 1.0f, 0.0f), 0.7f);

    FVector3 original(1.0f, 2.0f, 3.0f);
    FVector3 roundTrip = q.UnrotateVector(q.RotateVector(original));

    LIMX_EXPECT_TRUE(VectorsNearlyEqual(roundTrip, original, 1.0e-3f));
}

LIMX_TEST(FQuat, InverseUndoesRotation)
{
    FQuat q = FQuat::FromAxisAngle(FVector3(1.0f, 1.0f, 0.0f).GetSafeNormal(),
                                   1.5f);
    FQuat inverse = q.Inverse();

    FVector3 original(2.0f, -1.0f, 0.5f);
    FVector3 roundTrip = inverse.RotateVector(q.RotateVector(original));

    LIMX_EXPECT_TRUE(VectorsNearlyEqual(roundTrip, original, 1.0e-3f));
}

LIMX_TEST(FQuat, ConjugateOfUnitQuatEqualsInverse)
{
    FQuat q = FQuat::FromAxisAngle(FVector3(0.0f, 0.0f, 1.0f), 0.9f);

    FQuat conjugate = q.Conjugate();
    FQuat inverse   = q.Inverse();

    // 单位四元数的共轭即其逆
    LIMX_EXPECT_TRUE(conjugate.Equals(inverse, 1.0e-3f));
}

LIMX_TEST(FQuat, BasisVectorsAreOrthonormal)
{
    FQuat q = FQuat::FromAxisAngle(FVector3(0.2f, 0.9f, -0.3f).GetSafeNormal(),
                                   1.1f);

    FVector3 forward = q.GetForward();
    FVector3 right   = q.GetRight();
    FVector3 up      = q.GetUp();

    // 三个基向量应为单位长度且两两正交
    LIMX_EXPECT_NEAR(forward.Length(), 1.0f, 1.0e-3f);
    LIMX_EXPECT_NEAR(right.Length(), 1.0f, 1.0e-3f);
    LIMX_EXPECT_NEAR(up.Length(), 1.0f, 1.0e-3f);

    LIMX_EXPECT_NEAR(FVector3::Dot(forward, right), 0.0f, 1.0e-3f);
    LIMX_EXPECT_NEAR(FVector3::Dot(right, up), 0.0f, 1.0e-3f);
    LIMX_EXPECT_NEAR(FVector3::Dot(up, forward), 0.0f, 1.0e-3f);
}

LIMX_TEST(FQuat, ZeroAngleRotationIsIdentity)
{
    FQuat q = FQuat::FromAxisAngle(FVector3(0.0f, 1.0f, 0.0f), 0.0f);

    FVector3 v(1.0f, 2.0f, 3.0f);
    LIMX_EXPECT_TRUE(VectorsNearlyEqual(q.RotateVector(v), v, 1.0e-4f));
}

// ============================================================================
// FMatrix — 4x4 变换矩阵
// ============================================================================

LIMX_TEST(FMatrix, IdentityLeavesPointsUnchanged)
{
    FMatrix identity = FMatrix::Identity();
    FVector3 point(1.0f, 2.0f, 3.0f);

    LIMX_EXPECT_TRUE(
        VectorsNearlyEqual(identity.TransformPosition(point), point));
}

LIMX_TEST(FMatrix, TranslationStoresOffsetInFourthColumn)
{
    // 约定固化: 行主序存储, 平移位于第四列
    FMatrix translation = FMatrix::Translation(FVector3(5.0f, 6.0f, 7.0f));

    LIMX_EXPECT_NEAR(translation.M[0][3], 5.0f, kTolerance);
    LIMX_EXPECT_NEAR(translation.M[1][3], 6.0f, kTolerance);
    LIMX_EXPECT_NEAR(translation.M[2][3], 7.0f, kTolerance);

    LIMX_EXPECT_TRUE(VectorsNearlyEqual(translation.GetTranslation(),
                                        FVector3(5.0f, 6.0f, 7.0f)));
}

LIMX_TEST(FMatrix, TranslationAppliesToPositionButNotDirection)
{
    FMatrix translation = FMatrix::Translation(FVector3(10.0f, 0.0f, 0.0f));
    FVector3 vector(1.0f, 0.0f, 0.0f);

    // 点受平移影响 (w=1)
    LIMX_EXPECT_TRUE(VectorsNearlyEqual(translation.TransformPosition(vector),
                                        FVector3(11.0f, 0.0f, 0.0f)));

    // 方向不受平移影响 (w=0)
    LIMX_EXPECT_TRUE(VectorsNearlyEqual(translation.TransformDirection(vector),
                                        FVector3(1.0f, 0.0f, 0.0f)));
}

LIMX_TEST(FMatrix, ScaleMultipliesComponents)
{
    FMatrix scale = FMatrix::Scale(FVector3(2.0f, 3.0f, 4.0f));
    FVector3 point(1.0f, 1.0f, 1.0f);

    LIMX_EXPECT_TRUE(VectorsNearlyEqual(scale.TransformPosition(point),
                                        FVector3(2.0f, 3.0f, 4.0f)));

    LIMX_EXPECT_TRUE(VectorsNearlyEqual(scale.GetScale(),
                                        FVector3(2.0f, 3.0f, 4.0f)));
}

LIMX_TEST(FMatrix, RotationZMapsXToY)
{
    FMatrix rotation = FMatrix::RotationZ(FMath::kPi * 0.5f);

    FVector3 rotated = rotation.TransformDirection(FVector3(1.0f, 0.0f, 0.0f));

    LIMX_EXPECT_TRUE(VectorsNearlyEqual(rotated, FVector3(0.0f, 1.0f, 0.0f),
                                        1.0e-3f));
}

LIMX_TEST(FMatrix, RotationPreservesLength)
{
    FMatrix rotation = FMatrix::RotationAxis(
        FVector3(0.3f, 0.5f, -0.8f).GetSafeNormal(), 1.7f);

    FVector3 v(1.0f, 2.0f, -3.0f);
    FVector3 rotated = rotation.TransformDirection(v);

    LIMX_EXPECT_NEAR(rotated.Length(), v.Length(), 1.0e-3f);
}

LIMX_TEST(FMatrix, TransposeIsInvolution)
{
    FMatrix m = FMatrix::RotationY(0.6f) *
                FMatrix::Translation(FVector3(1.0f, 2.0f, 3.0f));

    // 转置两次回到原矩阵
    LIMX_EXPECT_TRUE(MatricesNearlyEqual(m.Transpose().Transpose(), m));
}

LIMX_TEST(FMatrix, IdentityDeterminantIsOne)
{
    LIMX_EXPECT_NEAR(FMatrix::Identity().Determinant(), 1.0f, kTolerance);
}

LIMX_TEST(FMatrix, ScaleDeterminantIsProductOfFactors)
{
    FMatrix scale = FMatrix::Scale(FVector3(2.0f, 3.0f, 4.0f));

    // 行列式等于体积缩放因子
    LIMX_EXPECT_NEAR(scale.Determinant(), 24.0f, 1.0e-3f);
}

LIMX_TEST(FMatrix, RotationDeterminantIsOne)
{
    FMatrix rotation = FMatrix::RotationAxis(
        FVector3(1.0f, 1.0f, 1.0f).GetSafeNormal(), 0.8f);

    // 纯旋转不改变体积也不翻转手性
    LIMX_EXPECT_NEAR(rotation.Determinant(), 1.0f, 1.0e-3f);
}

LIMX_TEST(FMatrix, InverseTimesOriginalIsIdentity)
{
    FMatrix m = FMatrix::Translation(FVector3(3.0f, -2.0f, 5.0f)) *
                FMatrix::RotationY(0.9f) *
                FMatrix::Scale(FVector3(2.0f, 2.0f, 2.0f));

    FMatrix inverse = m.Inverse();

    // 逆矩阵的定义性恒等式 — 两个方向都验证
    LIMX_EXPECT_TRUE(MatricesNearlyEqual(m * inverse, FMatrix::Identity(),
                                         1.0e-3f));
    LIMX_EXPECT_TRUE(MatricesNearlyEqual(inverse * m, FMatrix::Identity(),
                                         1.0e-3f));
}

LIMX_TEST(FMatrix, InverseUndoesPointTransform)
{
    FMatrix m = FMatrix::Translation(FVector3(1.0f, 2.0f, 3.0f)) *
                FMatrix::RotationZ(0.5f);

    FVector3 original(4.0f, -5.0f, 6.0f);
    FVector3 roundTrip = m.Inverse().TransformPosition(
        m.TransformPosition(original));

    LIMX_EXPECT_TRUE(VectorsNearlyEqual(roundTrip, original, 1.0e-3f));
}

LIMX_TEST(FMatrix, MultiplicationIsAssociative)
{
    FMatrix a = FMatrix::RotationX(0.3f);
    FMatrix b = FMatrix::Translation(FVector3(1.0f, 2.0f, 3.0f));
    FMatrix c = FMatrix::Scale(FVector3(2.0f, 2.0f, 2.0f));

    LIMX_EXPECT_TRUE(MatricesNearlyEqual((a * b) * c, a * (b * c), 1.0e-3f));
}

LIMX_TEST(FMatrix, IdentityIsMultiplicativeUnit)
{
    FMatrix m = FMatrix::RotationY(1.2f) *
                FMatrix::Translation(FVector3(4.0f, 5.0f, 6.0f));

    FMatrix identity = FMatrix::Identity();

    LIMX_EXPECT_TRUE(MatricesNearlyEqual(m * identity, m));
    LIMX_EXPECT_TRUE(MatricesNearlyEqual(identity * m, m));
}

LIMX_TEST(FMatrix, LookAtPlacesEyeAtOrigin)
{
    const FVector3 eye(0.0f, 0.0f, 5.0f);
    const FVector3 target(0.0f, 0.0f, 0.0f);
    const FVector3 up(0.0f, 1.0f, 0.0f);

    FMatrix view = FMatrix::LookAt(eye, target, up);

    // 视图矩阵把相机位置映射到观察空间原点
    FVector3 eyeInViewSpace = view.TransformPosition(eye);
    LIMX_EXPECT_TRUE(VectorsNearlyEqual(eyeInViewSpace,
                                        FVector3(0.0f, 0.0f, 0.0f), 1.0e-3f));
}

LIMX_TEST(FMatrix, LookAtPutsTargetOnNegativeZ)
{
    const FVector3 eye(0.0f, 0.0f, 5.0f);
    const FVector3 target(0.0f, 0.0f, 0.0f);
    const FVector3 up(0.0f, 1.0f, 0.0f);

    FMatrix view = FMatrix::LookAt(eye, target, up);
    FVector3 targetInView = view.TransformPosition(target);

    // 右手观察空间约定: 视线方向为 -Z
    LIMX_EXPECT_NEAR(targetInView.X, 0.0f, 1.0e-3f);
    LIMX_EXPECT_NEAR(targetInView.Y, 0.0f, 1.0e-3f);
    LIMX_EXPECT_LT(targetInView.Z, 0.0f);
}

LIMX_TEST(FMatrix, GetRowAndGetColumnAreConsistent)
{
    FMatrix m = FMatrix::Translation(FVector3(1.0f, 2.0f, 3.0f));

    // M[row][col] 应同时可由 GetRow(row)[col] 与 GetColumn(col)[row] 取得
    for (Int32 row = 0; row < 4; ++row)
    {
        FVector4 rowVector = m.GetRow(row);

        LIMX_EXPECT_NEAR(rowVector.X, m.M[row][0], kTolerance);
        LIMX_EXPECT_NEAR(rowVector.W, m.M[row][3], kTolerance);
    }

    for (Int32 col = 0; col < 4; ++col)
    {
        FVector4 columnVector = m.GetColumn(col);

        LIMX_EXPECT_NEAR(columnVector.X, m.M[0][col], kTolerance);
        LIMX_EXPECT_NEAR(columnVector.W, m.M[3][col], kTolerance);
    }
}

// ============================================================================
// FMath — 标量工具
// ============================================================================

LIMX_TEST(FMath, AbsHandlesBothSigns)
{
    LIMX_EXPECT_NEAR(FMath::Abs(-3.5f), 3.5f, kTolerance);
    LIMX_EXPECT_NEAR(FMath::Abs(3.5f), 3.5f, kTolerance);
    LIMX_EXPECT_NEAR(FMath::Abs(0.0f), 0.0f, kTolerance);
}

LIMX_TEST(FMath, ClampBoundsValue)
{
    LIMX_EXPECT_NEAR(FMath::Clamp(5.0f, 0.0f, 10.0f), 5.0f, kTolerance);
    LIMX_EXPECT_NEAR(FMath::Clamp(-5.0f, 0.0f, 10.0f), 0.0f, kTolerance);
    LIMX_EXPECT_NEAR(FMath::Clamp(15.0f, 0.0f, 10.0f), 10.0f, kTolerance);
}

LIMX_TEST(FMath, MinMaxSelectCorrectly)
{
    LIMX_EXPECT_EQ(FMath::Min(3, 7), 3);
    LIMX_EXPECT_EQ(FMath::Max(3, 7), 7);
    LIMX_EXPECT_NEAR(FMath::Min(-1.5f, 2.5f), -1.5f, kTolerance);
    LIMX_EXPECT_NEAR(FMath::Max(-1.5f, 2.5f), 2.5f, kTolerance);
}

LIMX_TEST(FMath, LerpHitsEndpoints)
{
    LIMX_EXPECT_NEAR(FMath::Lerp(0.0f, 10.0f, 0.0f), 0.0f, kTolerance);
    LIMX_EXPECT_NEAR(FMath::Lerp(0.0f, 10.0f, 1.0f), 10.0f, kTolerance);
    LIMX_EXPECT_NEAR(FMath::Lerp(0.0f, 10.0f, 0.25f), 2.5f, kTolerance);
}

LIMX_TEST(FMath, SqrtMatchesSquare)
{
    LIMX_EXPECT_NEAR(FMath::Sqrt(16.0f), 4.0f, kTolerance);
    LIMX_EXPECT_NEAR(FMath::Sqrt(2.0f) * FMath::Sqrt(2.0f), 2.0f, 1.0e-3f);
}

LIMX_TEST(FMath, TrigonometricIdentityHolds)
{
    // sin² + cos² == 1, 在多个角度上验证
    const Float32 angles[] = { 0.0f, 0.3f, 1.0f, 2.5f, -1.7f, 3.14f };

    for (Float32 angle : angles)
    {
        const Float32 s = FMath::Sin(angle);
        const Float32 c = FMath::Cos(angle);

        LIMX_EXPECT_NEAR(s * s + c * c, 1.0f, 1.0e-4f);
    }
}

LIMX_TEST(FMath, SinCosMatchesIndividualCalls)
{
    const Float32 angle = 0.876f;

    Float32 s = 0.0f;
    Float32 c = 0.0f;
    FMath::SinCos(angle, s, c);

    LIMX_EXPECT_NEAR(s, FMath::Sin(angle), 1.0e-5f);
    LIMX_EXPECT_NEAR(c, FMath::Cos(angle), 1.0e-5f);
}

LIMX_TEST(FMath, DegreesRadiansRoundTrip)
{
    const Float32 degrees = 137.5f;

    const Float32 radians = FMath::DegreesToRadians(degrees);
    const Float32 restored = FMath::RadiansToDegrees(radians);

    LIMX_EXPECT_NEAR(restored, degrees, 1.0e-3f);
}

LIMX_TEST(FMath, IsNaNDetectsInvalidValues)
{
    LIMX_EXPECT_FALSE(FMath::IsNaN(1.0f));
    LIMX_EXPECT_FALSE(FMath::IsNaN(0.0f));

    // 0/0 产生 NaN — 用变量避免编译期常量折叠
    volatile Float32 zero = 0.0f;
    const Float32 nan = zero / zero;
    LIMX_EXPECT_TRUE(FMath::IsNaN(nan));
}

// ============================================================================
// 投影矩阵 — 手性与 NDC 约定
//
// 这一组用例存在的原因: LookAt 是右手系 (视线 -Z)，而投影矩阵一度按左手系
// (视线 +Z) 构造。两者单独看都"对"，组合起来却让每个可见点的裁剪空间 w 为负，
// 从而被整体裁掉 —— 画面只剩清屏色，校验层不报任何错。
// 因此断言的对象不是投影矩阵本身，而是 "view → proj" 这条完整链路。
// ============================================================================

namespace
{

/// 相机放在 +Z 侧注视原点时的视图矩阵 — 与引擎默认的右手约定一致
FMatrix MakeTestView(const FVector3& eye)
{
    return FMatrix::LookAt(eye, FVector3(0.0f, 0.0f, 0.0f),
                           FVector3(0.0f, 1.0f, 0.0f));
}

/// 把世界坐标点走完 view → proj → 透视除法, 得到 NDC
/// @param outW 裁剪空间 w — 正值表示点在相机前方
FVector3 WorldToNdc(const FMatrix& view, const FMatrix& projection,
                    const FVector3& worldPosition, Float32& outW)
{
    const FVector4 viewSpace =
        view.TransformVector4(FVector4(worldPosition.X, worldPosition.Y,
                                       worldPosition.Z, 1.0f));
    const FVector4 clip = projection.TransformVector4(viewSpace);

    outW = clip.W;

    if (FMath::Abs(clip.W) < 1.0e-8f)
    {
        return FVector3(0.0f, 0.0f, 0.0f);
    }

    return FVector3(clip.X / clip.W, clip.Y / clip.W, clip.Z / clip.W);
}

} // namespace

LIMX_TEST(FMatrix, PerspectiveKeepsWPositiveInFrontOfCamera)
{
    const FMatrix view = MakeTestView(FVector3(0.0f, 0.0f, 5.0f));
    const FMatrix proj = FMatrix::Perspective(
        FMath::DegreesToRadians(45.0f), 16.0f / 9.0f, 0.1f, 100.0f);

    Float32 w = 0.0f;
    const FVector3 ndc = WorldToNdc(view, proj,
                                    FVector3(0.0f, 0.0f, 0.0f), w);

    // w 为负意味着点落在相机背后 —— 手性不匹配时全场景都会踩中这一条
    LIMX_EXPECT_GT(w, 0.0f);

    LIMX_EXPECT_NEAR(ndc.X, 0.0f, kTolerance);
    LIMX_EXPECT_NEAR(ndc.Y, 0.0f, kTolerance);
    LIMX_EXPECT_GE(ndc.Z, 0.0f);
    LIMX_EXPECT_LE(ndc.Z, 1.0f);
}

LIMX_TEST(FMatrix, PerspectiveGivesNegativeWBehindCamera)
{
    const FMatrix view = MakeTestView(FVector3(0.0f, 0.0f, 5.0f));
    const FMatrix proj = FMatrix::Perspective(
        FMath::DegreesToRadians(45.0f), 16.0f / 9.0f, 0.1f, 100.0f);

    // 相机在 +Z=5 处朝原点看, z=10 的点位于其背后
    Float32 w = 0.0f;
    WorldToNdc(view, proj, FVector3(0.0f, 0.0f, 10.0f), w);

    LIMX_EXPECT_LT(w, 0.0f);
}

LIMX_TEST(FMatrix, PerspectiveMapsNearPlaneToZeroDepth)
{
    const Float32 nearPlane = 0.5f;
    const Float32 farPlane  = 200.0f;

    const FMatrix view = MakeTestView(FVector3(0.0f, 0.0f, 5.0f));
    const FMatrix proj = FMatrix::Perspective(
        FMath::DegreesToRadians(60.0f), 1.0f, nearPlane, farPlane);

    // 相机沿 -Z 看, 近裁剪面上的点世界坐标 z = 5 - near
    Float32 w = 0.0f;
    const FVector3 ndc = WorldToNdc(view, proj,
                                    FVector3(0.0f, 0.0f, 5.0f - nearPlane), w);

    LIMX_EXPECT_GT(w, 0.0f);
    LIMX_EXPECT_NEAR(ndc.Z, 0.0f, 1.0e-3f);
}

LIMX_TEST(FMatrix, PerspectiveMapsFarPlaneToUnitDepth)
{
    const Float32 nearPlane = 0.5f;
    const Float32 farPlane  = 200.0f;

    const FMatrix view = MakeTestView(FVector3(0.0f, 0.0f, 5.0f));
    const FMatrix proj = FMatrix::Perspective(
        FMath::DegreesToRadians(60.0f), 1.0f, nearPlane, farPlane);

    Float32 w = 0.0f;
    const FVector3 ndc = WorldToNdc(view, proj,
                                    FVector3(0.0f, 0.0f, 5.0f - farPlane), w);

    LIMX_EXPECT_GT(w, 0.0f);
    LIMX_EXPECT_NEAR(ndc.Z, 1.0f, 1.0e-3f);
}

LIMX_TEST(FMatrix, PerspectiveDepthIncreasesWithDistance)
{
    const FMatrix view = MakeTestView(FVector3(0.0f, 0.0f, 5.0f));
    const FMatrix proj = FMatrix::Perspective(
        FMath::DegreesToRadians(45.0f), 16.0f / 9.0f, 0.1f, 100.0f);

    Float32 nearW = 0.0f;
    Float32 farW  = 0.0f;

    const FVector3 nearNdc =
        WorldToNdc(view, proj, FVector3(0.0f, 0.0f, 0.0f), nearW);
    const FVector3 farNdc =
        WorldToNdc(view, proj, FVector3(0.0f, 0.0f, -20.0f), farW);

    LIMX_EXPECT_GT(nearW, 0.0f);
    LIMX_EXPECT_GT(farW, 0.0f);

    // 深度必须随距离单调递增, 否则深度测试的方向整体反了
    LIMX_EXPECT_LT(nearNdc.Z, farNdc.Z);
}

LIMX_TEST(FMatrix, PerspectiveFollowsVulkanYDown)
{
    const FMatrix view = MakeTestView(FVector3(0.0f, 0.0f, 5.0f));
    const FMatrix proj = FMatrix::Perspective(
        FMath::DegreesToRadians(45.0f), 1.0f, 0.1f, 100.0f);

    Float32 w = 0.0f;

    // 世界空间中位于视线上方的点, 在 Vulkan NDC 中应落到 y 的负半区
    const FVector3 ndc = WorldToNdc(view, proj,
                                    FVector3(0.0f, 1.0f, 0.0f), w);

    LIMX_EXPECT_GT(w, 0.0f);
    LIMX_EXPECT_LT(ndc.Y, 0.0f);
}

LIMX_TEST(FMatrix, PerspectiveRespectsAspectRatio)
{
    const Float32 aspect = 16.0f / 9.0f;

    const FMatrix view = MakeTestView(FVector3(0.0f, 0.0f, 5.0f));
    const FMatrix proj = FMatrix::Perspective(
        FMath::DegreesToRadians(45.0f), aspect, 0.1f, 100.0f);

    Float32 wx = 0.0f;
    Float32 wy = 0.0f;

    const FVector3 ndcX =
        WorldToNdc(view, proj, FVector3(1.0f, 0.0f, 0.0f), wx);
    const FVector3 ndcY =
        WorldToNdc(view, proj, FVector3(0.0f, 1.0f, 0.0f), wy);

    // 同样的世界位移, 水平方向被宽高比压缩了 aspect 倍
    LIMX_EXPECT_NEAR(FMath::Abs(ndcY.Y) / FMath::Abs(ndcX.X), aspect,
                     1.0e-3f);
}

LIMX_TEST(FMatrix, OrthoMapsNearFarToZeroOne)
{
    const Float32 nearPlane = 1.0f;
    const Float32 farPlane  = 50.0f;

    const FMatrix view = MakeTestView(FVector3(0.0f, 0.0f, 5.0f));
    const FMatrix proj = FMatrix::Ortho(-10.0f, 10.0f, -10.0f, 10.0f,
                                        nearPlane, farPlane);

    Float32 w = 0.0f;

    const FVector3 nearNdc = WorldToNdc(view, proj,
                                        FVector3(0.0f, 0.0f, 5.0f - nearPlane), w);
    LIMX_EXPECT_NEAR(w, 1.0f, kTolerance);
    LIMX_EXPECT_NEAR(nearNdc.Z, 0.0f, 1.0e-3f);

    const FVector3 farNdc = WorldToNdc(view, proj,
                                       FVector3(0.0f, 0.0f, 5.0f - farPlane), w);
    LIMX_EXPECT_NEAR(w, 1.0f, kTolerance);
    LIMX_EXPECT_NEAR(farNdc.Z, 1.0f, 1.0e-3f);
}

LIMX_TEST(FMatrix, ViewProjectionRoundTripsUnitCubeCorners)
{
    const FMatrix view = MakeTestView(FVector3(0.0f, 2.5f, 5.0f));
    const FMatrix proj = FMatrix::Perspective(
        FMath::DegreesToRadians(45.0f), 16.0f / 9.0f, 0.1f, 100.0f);

    // 原点处的单位立方体应完整落在裁剪体内 —— 这正是演示场景的几何
    for (UInt32 corner = 0; corner < 8; ++corner)
    {
        const FVector3 point(
            (corner & 1u) != 0u ? 0.5f : -0.5f,
            (corner & 2u) != 0u ? 0.5f : -0.5f,
            (corner & 4u) != 0u ? 0.5f : -0.5f);

        Float32 w = 0.0f;
        const FVector3 ndc = WorldToNdc(view, proj, point, w);

        LIMX_EXPECT_GT(w, 0.0f);
        LIMX_EXPECT_GE(ndc.X, -1.0f);
        LIMX_EXPECT_LE(ndc.X, 1.0f);
        LIMX_EXPECT_GE(ndc.Y, -1.0f);
        LIMX_EXPECT_LE(ndc.Y, 1.0f);
        LIMX_EXPECT_GE(ndc.Z, 0.0f);
        LIMX_EXPECT_LE(ndc.Z, 1.0f);
    }
}

// ============================================================================
// 视锥体 — 平面提取与包围盒剔除
//
// 视锥的正确性没有中间状态: 平面提取错了, 要么什么都剔不掉 (性能白丢),
// 要么把可见物体剔掉 (画面缺物体)。两种都不会报错。
// 因此用例覆盖三类结果 —— 完全在内、完全在外、相交 —— 并显式钉住
// "近平面按 Vulkan 深度约定 (z ≥ 0) 提取" 这条与 OpenGL 不同的约定。
// ============================================================================

namespace
{

/// 构造一个位于 +Z 侧朝原点看的测试视锥
FFrustum MakeTestFrustum(const FVector3& eye,
                         Float32 nearPlane = 0.1f,
                         Float32 farPlane  = 100.0f)
{
    const FMatrix view = MakeTestView(eye);
    const FMatrix proj = FMatrix::Perspective(
        FMath::DegreesToRadians(60.0f), 1.0f, nearPlane, farPlane);

    // 列向量语义: clip = P · V · v, 因此传入 P × V
    return FFrustum::FromViewProjection(proj * view);
}

/// 以中心与半边长构造包围盒
FBoundingBox MakeBox(const FVector3& center, Float32 halfExtent)
{
    const FVector3 extent(halfExtent, halfExtent, halfExtent);
    return FBoundingBox(center - extent, center + extent);
}

} // namespace

LIMX_TEST(FFrustum, PlanesAreNormalized)
{
    const FFrustum frustum = MakeTestFrustum(FVector3(0.0f, 0.0f, 10.0f));

    // 法线未归一化时 SignedDistance 不再是真实距离, 保守剔除的余量判断会失真
    for (Int32 index = 0; index < FFrustum::kPlaneCount; ++index)
    {
        LIMX_EXPECT_NEAR(frustum.GetPlane(
            static_cast<FrustumPlane>(index)).Normal.Length(), 1.0f, 1.0e-3f);
    }
}

LIMX_TEST(FFrustum, AcceptsBoxAtFocusPoint)
{
    const FFrustum frustum = MakeTestFrustum(FVector3(0.0f, 0.0f, 10.0f));

    LIMX_EXPECT_TRUE(frustum.TestPoint(FVector3(0.0f, 0.0f, 0.0f)));
    LIMX_EXPECT_TRUE(frustum.IsAABBVisible(
        MakeBox(FVector3(0.0f, 0.0f, 0.0f), 0.5f)));
}

LIMX_TEST(FFrustum, RejectsBoxBehindCamera)
{
    const FFrustum frustum = MakeTestFrustum(FVector3(0.0f, 0.0f, 10.0f));

    // 相机在 z=10 朝 -Z 看, z=20 的盒子完全在其身后。
    // 近平面若按 OpenGL 的 row3+row2 提取, 这一条会漏判。
    LIMX_EXPECT_FALSE(frustum.IsAABBVisible(
        MakeBox(FVector3(0.0f, 0.0f, 20.0f), 1.0f)));
}

LIMX_TEST(FFrustum, RejectsBoxBeyondFarPlane)
{
    const FFrustum frustum =
        MakeTestFrustum(FVector3(0.0f, 0.0f, 10.0f), 0.1f, 50.0f);

    // 远平面在 z = 10 - 50 = -40 处
    LIMX_EXPECT_FALSE(frustum.IsAABBVisible(
        MakeBox(FVector3(0.0f, 0.0f, -60.0f), 1.0f)));
}

LIMX_TEST(FFrustum, RejectsBoxOutsideLateralPlanes)
{
    const FFrustum frustum = MakeTestFrustum(FVector3(0.0f, 0.0f, 10.0f));

    // 60° 垂直视场、宽高比 1 时, 原点处的半宽约为 10*tan(30°) ≈ 5.77
    LIMX_EXPECT_FALSE(frustum.IsAABBVisible(
        MakeBox(FVector3(50.0f, 0.0f, 0.0f), 1.0f)));
    LIMX_EXPECT_FALSE(frustum.IsAABBVisible(
        MakeBox(FVector3(0.0f, 50.0f, 0.0f), 1.0f)));
}

LIMX_TEST(FFrustum, ReportsIntersectForStraddlingBox)
{
    const FFrustum frustum = MakeTestFrustum(FVector3(0.0f, 0.0f, 10.0f));

    // 跨越远平面的大盒子既非完全在内也非完全在外
    const FrustumResult result = frustum.TestAABB(
        MakeBox(FVector3(0.0f, 0.0f, 0.0f), 500.0f));

    LIMX_EXPECT_TRUE(result == FrustumResult::Intersect);
}

LIMX_TEST(FFrustum, ReportsInsideForFullyContainedBox)
{
    const FFrustum frustum = MakeTestFrustum(FVector3(0.0f, 0.0f, 10.0f));

    const FrustumResult result = frustum.TestAABB(
        MakeBox(FVector3(0.0f, 0.0f, 0.0f), 0.25f));

    LIMX_EXPECT_TRUE(result == FrustumResult::Inside);
}

LIMX_TEST(FFrustum, NearPlaneSitsExactlyAtNearDistance)
{
    const Float32 nearPlane = 2.0f;
    const Float32 eyeZ      = 10.0f;

    const FFrustum frustum =
        MakeTestFrustum(FVector3(0.0f, 0.0f, eyeZ), nearPlane, 100.0f);

    // 相机在 z = 10 朝 -Z 看, 近平面应落在 z = 8 处。
    //
    // 这里的容差刻意收得很紧: OpenGL 的 row3 + row2 在同样参数下会把近平面
    // 推到 z ≈ 9.0 (即距相机约 1.01 而非 2.0)。若只测"相机背后的点被剔除",
    // 两种公式都能通过 —— 用例必须卡在两者的分歧区间上才有意义。
    const Float32 justInside  = eyeZ - nearPlane - 0.1f;   // z = 7.9, 可见
    const Float32 justOutside = eyeZ - nearPlane + 0.1f;   // z = 8.1, 不可见
    const Float32 openGlOnly  = eyeZ - nearPlane + 0.5f;   // z = 8.5, 分歧区间

    LIMX_EXPECT_TRUE(frustum.TestPoint(FVector3(0.0f, 0.0f, justInside)));
    LIMX_EXPECT_FALSE(frustum.TestPoint(FVector3(0.0f, 0.0f, justOutside)));
    LIMX_EXPECT_FALSE(frustum.TestPoint(FVector3(0.0f, 0.0f, openGlOnly)));

    // 包围盒路径必须与点路径给出一致的结论
    LIMX_EXPECT_FALSE(frustum.IsAABBVisible(
        MakeBox(FVector3(0.0f, 0.0f, openGlOnly), 0.2f)));
}

LIMX_TEST(FFrustum, CullingKeepsEveryVisibleBoxOfAGrid)
{
    const FFrustum frustum = MakeTestFrustum(FVector3(0.0f, 0.0f, 10.0f));

    // 网格化扫描: 凡是中心点在视锥内的盒子, 包围盒测试必须同样判为可见。
    // 反过来不成立 (盒子可以只有边角在内), 因此只断言这一个方向。
    UInt32 visibleCount = 0;

    for (Int32 x = -10; x <= 10; ++x)
    {
        for (Int32 y = -10; y <= 10; ++y)
        {
            for (Int32 z = -10; z <= 10; ++z)
            {
                const FVector3 center(static_cast<Float32>(x),
                                      static_cast<Float32>(y),
                                      static_cast<Float32>(z));

                if (!frustum.TestPoint(center))
                {
                    continue;
                }

                ++visibleCount;
                LIMX_EXPECT_TRUE(frustum.IsAABBVisible(MakeBox(center, 0.4f)));
            }
        }
    }

    // 扫描区域必须真的落在视锥里, 否则这条用例什么都没测到
    LIMX_EXPECT_GT(visibleCount, 0u);
}
