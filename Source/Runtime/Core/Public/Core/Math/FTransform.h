/*******************************************************************************
 * 文件: FTransform.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   复合变换类型 — 位置 (Translation) + 旋转 (Rotation) + 缩放 (Scale)
 *   引擎中所有空间实体 (Actor/Component) 的基础变换表示
 *   支持变换组合、逆变换、点/向量变换、插值
 *
 * 设计哲学:
 *   SRT 分离存储 — 避免矩阵分解的精度损失和性能开销
 *   四元数旋转 — 无万向锁，支持平滑插值
 *   延迟矩阵 — 仅在需要时计算完整 4x4 矩阵
 *   值语义 — 轻量值类型，可安全拷贝和传递
 *
 * 技术特性:
 *   - SRT 存储: FVector3 Scale + FQuat Rotation + FVector3 Translation
 *   - 变换组合: operator* (先 Scale → Rotation → Translation)
 *   - 逆变换: Inverse()
 *   - 点/向量变换: TransformPosition, TransformDirection, InverseTransformPosition
 *   - 插值: Lerp (分别插值 S/R/T)
 *   - 矩阵转换: ToMatrix(), FromMatrix()
 *
 * 依赖关系:
 *   内部: Core/Math/FMath.h, Core/Math/FVector.h,
 *          Core/Math/FQuat.h, Core/Math/FMatrix.h
 *
 ******************************************************************************/

#pragma once

#include "Core/Math/FMath.h"
#include "Core/Math/FVector.h"
#include "Core/Math/FQuat.h"
#include "Core/Math/FMatrix.h"

namespace Limx
{

/// 复合变换 — Scale → Rotate → Translate (SRT 顺序)
struct FTransform
{
    FQuat    Rotation;     ///< 旋转 (四元数)
    FVector3 Translation;  ///< 平移
    FVector3 Scale3D;      ///< 三轴缩放

    // 常量
    static const FTransform kIdentity;

    // ========================================================================
    // 构造
    // ========================================================================

    /// 默认构造 — 单位变换
    constexpr FTransform()
        : Rotation()
        , Translation(0.0f, 0.0f, 0.0f)
        , Scale3D(1.0f, 1.0f, 1.0f)
    {
    }

    /// 完整构造
    constexpr FTransform(const FQuat& inRotation,
                          const FVector3& inTranslation,
                          const FVector3& inScale = FVector3(1.0f, 1.0f, 1.0f))
        : Rotation(inRotation)
        , Translation(inTranslation)
        , Scale3D(inScale)
    {
    }

    /// 仅平移
    constexpr explicit FTransform(const FVector3& inTranslation)
        : Rotation()
        , Translation(inTranslation)
        , Scale3D(1.0f, 1.0f, 1.0f)
    {
    }

    /// 仅旋转
    constexpr explicit FTransform(const FQuat& inRotation)
        : Rotation(inRotation)
        , Translation(0.0f, 0.0f, 0.0f)
        , Scale3D(1.0f, 1.0f, 1.0f)
    {
    }

    // ========================================================================
    // 变换点和向量
    // ========================================================================

    /// 变换位置点 — 应用 Scale → Rotate → Translate
    LIMX_NODISCARD FVector3 TransformPosition(const FVector3& position) const
    {
        FVector3 scaled = position * Scale3D;
        FVector3 rotated = Rotation.RotateVector(scaled);
        return rotated + Translation;
    }

    /// 变换方向向量 — 应用 Scale → Rotate (不应用平移)
    LIMX_NODISCARD FVector3 TransformDirection(const FVector3& direction) const
    {
        FVector3 scaled = direction * Scale3D;
        return Rotation.RotateVector(scaled);
    }

    /// 变换方向向量 (无缩放) — 仅应用旋转
    LIMX_NODISCARD FVector3 TransformDirectionNoScale(
        const FVector3& direction) const
    {
        return Rotation.RotateVector(direction);
    }

    /// 逆变换位置点
    LIMX_NODISCARD FVector3 InverseTransformPosition(
        const FVector3& position) const
    {
        FVector3 translated = position - Translation;
        FVector3 rotated = Rotation.UnrotateVector(translated);
        // 逆缩放
        return FVector3(
            Scale3D.X != 0.0f ? rotated.X / Scale3D.X : 0.0f,
            Scale3D.Y != 0.0f ? rotated.Y / Scale3D.Y : 0.0f,
            Scale3D.Z != 0.0f ? rotated.Z / Scale3D.Z : 0.0f
        );
    }

    /// 逆变换方向向量
    LIMX_NODISCARD FVector3 InverseTransformDirection(
        const FVector3& direction) const
    {
        FVector3 rotated = Rotation.UnrotateVector(direction);
        return FVector3(
            Scale3D.X != 0.0f ? rotated.X / Scale3D.X : 0.0f,
            Scale3D.Y != 0.0f ? rotated.Y / Scale3D.Y : 0.0f,
            Scale3D.Z != 0.0f ? rotated.Z / Scale3D.Z : 0.0f
        );
    }

    // ========================================================================
    // 变换组合
    // ========================================================================

    /// 变换组合: result = this * other
    /// 即先应用 other，再应用 this
    LIMX_NODISCARD FTransform operator*(const FTransform& other) const
    {
        FTransform result;

        // 缩放组合
        result.Scale3D = Scale3D * other.Scale3D;

        // 旋转组合
        result.Rotation = Rotation * other.Rotation;
        result.Rotation.Normalize();

        // 平移: 对 other 的平移先缩放再旋转，然后加上 this 的平移
        result.Translation = TransformPosition(other.Translation);

        return result;
    }

    FTransform& operator*=(const FTransform& other)
    {
        *this = *this * other;
        return *this;
    }

    // ========================================================================
    // 逆变换
    // ========================================================================

    /// 逆变换 — 使得 transform * transform.Inverse() ≈ Identity
    LIMX_NODISCARD FTransform Inverse() const
    {
        FTransform result;

        // 逆缩放
        result.Scale3D = FVector3(
            FMath::Abs(Scale3D.X) > FMath::kSmallNumber
                ? 1.0f / Scale3D.X : 0.0f,
            FMath::Abs(Scale3D.Y) > FMath::kSmallNumber
                ? 1.0f / Scale3D.Y : 0.0f,
            FMath::Abs(Scale3D.Z) > FMath::kSmallNumber
                ? 1.0f / Scale3D.Z : 0.0f
        );

        // 逆旋转
        result.Rotation = Rotation.Conjugate();

        // 逆平移: -R^-1 * S^-1 * T
        FVector3 scaledTranslation = Translation * result.Scale3D;
        result.Translation = -(result.Rotation.RotateVector(scaledTranslation));

        return result;
    }

    // ========================================================================
    // 矩阵转换
    // ========================================================================

    /// 转换为 4x4 变换矩阵 (Scale → Rotate → Translate)
    LIMX_NODISCARD FMatrix ToMatrix() const
    {
        FMatrix rotationMatrix = Rotation.ToMatrix();
        FMatrix scaleMatrix = FMatrix::Scale(Scale3D);
        FMatrix translationMatrix = FMatrix::Translation(Translation);

        // SRT: 先缩放，再旋转，最后平移
        return translationMatrix * rotationMatrix * scaleMatrix;
    }

    /// 从 4x4 矩阵提取 SRT
    LIMX_NODISCARD static FTransform FromMatrix(const FMatrix& matrix)
    {
        FTransform result;

        // 提取缩放 (每行基向量的长度)
        FVector3 row0(matrix.M[0][0], matrix.M[0][1], matrix.M[0][2]);
        FVector3 row1(matrix.M[1][0], matrix.M[1][1], matrix.M[1][2]);
        FVector3 row2(matrix.M[2][0], matrix.M[2][1], matrix.M[2][2]);

        result.Scale3D = FVector3(row0.Length(), row1.Length(), row2.Length());

        // 检测负缩放 (行列式 < 0)
        Float32 det = matrix.Determinant();
        if (det < 0.0f)
        {
            result.Scale3D.X = -result.Scale3D.X;
        }

        // 构建去缩放的旋转矩阵
        FMatrix rotMatrix = FMatrix::Identity();
        if (result.Scale3D.X > FMath::kSmallNumber)
        {
            Float32 invSX = 1.0f / result.Scale3D.X;
            rotMatrix.M[0][0] = matrix.M[0][0] * invSX;
            rotMatrix.M[0][1] = matrix.M[0][1] * invSX;
            rotMatrix.M[0][2] = matrix.M[0][2] * invSX;
        }
        if (result.Scale3D.Y > FMath::kSmallNumber)
        {
            Float32 invSY = 1.0f / result.Scale3D.Y;
            rotMatrix.M[1][0] = matrix.M[1][0] * invSY;
            rotMatrix.M[1][1] = matrix.M[1][1] * invSY;
            rotMatrix.M[1][2] = matrix.M[1][2] * invSY;
        }
        if (result.Scale3D.Z > FMath::kSmallNumber)
        {
            Float32 invSZ = 1.0f / result.Scale3D.Z;
            rotMatrix.M[2][0] = matrix.M[2][0] * invSZ;
            rotMatrix.M[2][1] = matrix.M[2][1] * invSZ;
            rotMatrix.M[2][2] = matrix.M[2][2] * invSZ;
        }

        result.Rotation = FQuat::FromMatrix(rotMatrix);

        // 提取平移
        result.Translation = FVector3(
            matrix.M[0][3], matrix.M[1][3], matrix.M[2][3]);

        return result;
    }

    // ========================================================================
    // 插值
    // ========================================================================

    /// 线性插值 — 分别对 S/R/T 插值 (旋转使用 Slerp)
    LIMX_NODISCARD static FTransform Lerp(const FTransform& a,
                                           const FTransform& b,
                                           Float32 t)
    {
        return FTransform(
            FQuat::Slerp(a.Rotation, b.Rotation, t),
            FVector3::Lerp(a.Translation, b.Translation, t),
            FVector3::Lerp(a.Scale3D, b.Scale3D, t)
        );
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 是否为单位变换
    LIMX_NODISCARD bool IsIdentity(Float32 tolerance = FMath::kSmallNumber) const
    {
        return Translation.IsNearlyZero(tolerance) &&
               Rotation.Equals(FQuat::kIdentity, tolerance) &&
               Scale3D.Equals(FVector3::kOne, tolerance);
    }

    /// 是否包含负缩放 (镜像)
    LIMX_NODISCARD bool HasNegativeScale() const
    {
        return Scale3D.X < 0.0f || Scale3D.Y < 0.0f || Scale3D.Z < 0.0f;
    }

    /// 是否为均匀缩放
    LIMX_NODISCARD bool IsUniformScale(Float32 tolerance = FMath::kSmallNumber) const
    {
        return FMath::IsNearlyEqual(Scale3D.X, Scale3D.Y, tolerance) &&
               FMath::IsNearlyEqual(Scale3D.Y, Scale3D.Z, tolerance);
    }

    /// 获取前方向
    LIMX_NODISCARD FVector3 GetForward() const
    {
        return Rotation.GetForward();
    }

    /// 获取右方向
    LIMX_NODISCARD FVector3 GetRight() const
    {
        return Rotation.GetRight();
    }

    /// 获取上方向
    LIMX_NODISCARD FVector3 GetUp() const
    {
        return Rotation.GetUp();
    }

    // ========================================================================
    // 比较
    // ========================================================================

    LIMX_NODISCARD bool Equals(const FTransform& other,
                                Float32 tolerance = FMath::kSmallNumber) const
    {
        return Translation.Equals(other.Translation, tolerance) &&
               Rotation.Equals(other.Rotation, tolerance) &&
               Scale3D.Equals(other.Scale3D, tolerance);
    }

    LIMX_NODISCARD bool operator==(const FTransform& other) const
    {
        return Translation == other.Translation &&
               Rotation == other.Rotation &&
               Scale3D == other.Scale3D;
    }

    LIMX_NODISCARD bool operator!=(const FTransform& other) const
    {
        return !(*this == other);
    }
};

// 常量定义
inline constexpr FTransform FTransform::kIdentity = FTransform();

} // namespace Limx
