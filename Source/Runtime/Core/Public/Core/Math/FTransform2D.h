/*******************************************************************************
 * 文件: FTransform2D.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   2D 变换 — 平移、旋转、缩放的二维复合变换
 *   以 TRS 顺序合成变换矩阵，支持变换点/向量和变换组合
 *   用于 2D UI 布局、精灵变换、物理 2D 坐标系等场景
 *
 * 设计哲学:
 *   TRS 顺序 — Scale → Rotate → Translate
 *   FMatrix3 后端 — 最终合成为 3x3 矩阵
 *   值类型 — 轻量可拷贝
 *
 * 技术特性:
 *   - FTransform2D: 2D TRS 变换
 *   - ToMatrix: 合成 3x3 矩阵
 *   - TransformPoint: 点变换 (含平移)
 *   - TransformVector: 向量变换 (不含平移)
 *   - Combine: 变换组合
 *   - Inverse: 逆变换
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Math/FMath.h,
 *          Core/Math/FVector.h, Core/Math/FMatrix3.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Math/FMath.h"
#include "Core/Math/FVector.h"
#include "Core/Math/FMatrix3.h"

namespace Limx
{

/// 2D TRS 变换
struct FTransform2D
{
    FVector2 Translation;  ///< 平移
    Float32  Rotation;     ///< 旋转角 (弧度)
    FVector2 Scale;        ///< 缩放

    // ========================================================================
    // 构造
    // ========================================================================

    FTransform2D()
        : Translation(0.0f, 0.0f)
        , Rotation(0.0f)
        , Scale(1.0f, 1.0f)
    {
    }

    FTransform2D(const FVector2& translation,
                 Float32 rotation,
                 const FVector2& scale)
        : Translation(translation)
        , Rotation(rotation)
        , Scale(scale)
    {
    }

    /// 单位变换
    LIMX_NODISCARD static FTransform2D Identity()
    {
        return FTransform2D();
    }

    // ========================================================================
    // 矩阵合成
    // ========================================================================

    /// 合成 3x3 变换矩阵 (TRS 顺序)
    LIMX_NODISCARD FMatrix3 ToMatrix() const
    {
        Float32 c = FMath::Cos(Rotation);
        Float32 s = FMath::Sin(Rotation);

        // [Scale] * [Rotate] * [Translate]
        return FMatrix3(
            Scale.X * c, -Scale.Y * s, Translation.X,
            Scale.X * s,  Scale.Y * c, Translation.Y,
            0.0f,         0.0f,        1.0f);
    }

    // ========================================================================
    // 变换操作
    // ========================================================================

    /// 变换点 (含平移)
    LIMX_NODISCARD FVector2 TransformPoint(
        const FVector2& point) const
    {
        Float32 c = FMath::Cos(Rotation);
        Float32 s = FMath::Sin(Rotation);

        Float32 scaledX = point.X * Scale.X;
        Float32 scaledY = point.Y * Scale.Y;

        return FVector2(
            c * scaledX - s * scaledY + Translation.X,
            s * scaledX + c * scaledY + Translation.Y);
    }

    /// 变换向量 (不含平移)
    LIMX_NODISCARD FVector2 TransformVector(
        const FVector2& vector) const
    {
        Float32 c = FMath::Cos(Rotation);
        Float32 s = FMath::Sin(Rotation);

        Float32 scaledX = vector.X * Scale.X;
        Float32 scaledY = vector.Y * Scale.Y;

        return FVector2(
            c * scaledX - s * scaledY,
            s * scaledX + c * scaledY);
    }

    /// 逆变换点
    LIMX_NODISCARD FVector2 InverseTransformPoint(
        const FVector2& point) const
    {
        FVector2 local = point - Translation;
        Float32 c = FMath::Cos(-Rotation);
        Float32 s = FMath::Sin(-Rotation);

        Float32 rotX = c * local.X - s * local.Y;
        Float32 rotY = s * local.X + c * local.Y;

        Float32 invScaleX = (FMath::Abs(Scale.X) < 1e-8f)
            ? 0.0f : 1.0f / Scale.X;
        Float32 invScaleY = (FMath::Abs(Scale.Y) < 1e-8f)
            ? 0.0f : 1.0f / Scale.Y;

        return FVector2(rotX * invScaleX, rotY * invScaleY);
    }

    // ========================================================================
    // 组合与逆
    // ========================================================================

    /// 组合两个变换 (this * other, 先 other 后 this)
    LIMX_NODISCARD FTransform2D Combine(
        const FTransform2D& other) const
    {
        FVector2 combinedTranslation =
            TransformPoint(other.Translation);
        Float32 combinedRotation =
            Rotation + other.Rotation;
        FVector2 combinedScale(
            Scale.X * other.Scale.X,
            Scale.Y * other.Scale.Y);

        return FTransform2D(
            combinedTranslation,
            combinedRotation,
            combinedScale);
    }

    /// 逆变换
    LIMX_NODISCARD FTransform2D Inverse() const
    {
        Float32 invScaleX = (FMath::Abs(Scale.X) < 1e-8f)
            ? 0.0f : 1.0f / Scale.X;
        Float32 invScaleY = (FMath::Abs(Scale.Y) < 1e-8f)
            ? 0.0f : 1.0f / Scale.Y;

        Float32 invRotation = -Rotation;
        Float32 c = FMath::Cos(invRotation);
        Float32 s = FMath::Sin(invRotation);

        Float32 tx = -(c * Translation.X * invScaleX -
                       s * Translation.Y * invScaleX);
        Float32 ty = -(s * Translation.X * invScaleY +
                       c * Translation.Y * invScaleY);

        return FTransform2D(
            FVector2(tx, ty),
            invRotation,
            FVector2(invScaleX, invScaleY));
    }

    // ========================================================================
    // 修改
    // ========================================================================

    void SetTranslation(const FVector2& t)
    {
        Translation = t;
    }

    void SetRotation(Float32 radians)
    {
        Rotation = radians;
    }

    void SetScale(const FVector2& s) { Scale = s; }
    void SetScale(Float32 uniform)
    {
        Scale = FVector2(uniform, uniform);
    }

    // ========================================================================
    // 比较
    // ========================================================================

    LIMX_NODISCARD bool operator==(
        const FTransform2D& other) const
    {
        return FMath::Abs(
                   Translation.X - other.Translation.X)
                   < 1e-6f &&
               FMath::Abs(
                   Translation.Y - other.Translation.Y)
                   < 1e-6f &&
               FMath::Abs(Rotation - other.Rotation)
                   < 1e-6f &&
               FMath::Abs(Scale.X - other.Scale.X)
                   < 1e-6f &&
               FMath::Abs(Scale.Y - other.Scale.Y)
                   < 1e-6f;
    }

    LIMX_NODISCARD bool operator!=(
        const FTransform2D& other) const
    {
        return !(*this == other);
    }
};

} // namespace Limx
