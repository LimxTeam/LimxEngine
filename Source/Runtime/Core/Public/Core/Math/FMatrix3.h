/*******************************************************************************
 * 文件: FMatrix3.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   3x3 矩阵 — 行主序 3x3 矩阵
 *   提供旋转、缩放、2D 仿射变换、逆矩阵、转置等操作
 *   用于法线变换、2D 变换、惯性张量、协方差矩阵等场景
 *
 * 设计哲学:
 *   行主序 — M[Row][Col]，与 FMatrix (4x4) 一致
 *   值类型 — 9 个 Float32，轻量可拷贝
 *   静态工厂 — 提供 MakeRotation/MakeScale 等便捷构造
 *
 * 技术特性:
 *   - FMatrix3: 3x3 行主序矩阵
 *   - operator*: 矩阵乘法
 *   - TransformVector: 向量变换
 *   - Transpose/Inverse/Determinant: 矩阵运算
 *   - MakeRotation/MakeScale: 工厂方法
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Math/FMath.h,
 *          Core/Math/FVector.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Math/FMath.h"
#include "Core/Math/FVector.h"

namespace Limx
{

/// 3x3 行主序矩阵
struct FMatrix3
{
    Float32 M[3][3];  ///< M[行][列]

    // ========================================================================
    // 构造
    // ========================================================================

    /// 默认构造 — 零矩阵
    FMatrix3()
    {
        M[0][0] = 0.0f; M[0][1] = 0.0f; M[0][2] = 0.0f;
        M[1][0] = 0.0f; M[1][1] = 0.0f; M[1][2] = 0.0f;
        M[2][0] = 0.0f; M[2][1] = 0.0f; M[2][2] = 0.0f;
    }

    /// 从 9 个元素构造 (行主序)
    FMatrix3(Float32 m00, Float32 m01, Float32 m02,
             Float32 m10, Float32 m11, Float32 m12,
             Float32 m20, Float32 m21, Float32 m22)
    {
        M[0][0] = m00; M[0][1] = m01; M[0][2] = m02;
        M[1][0] = m10; M[1][1] = m11; M[1][2] = m12;
        M[2][0] = m20; M[2][1] = m21; M[2][2] = m22;
    }

    /// 单位矩阵
    LIMX_NODISCARD static FMatrix3 Identity()
    {
        return FMatrix3(
            1.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 1.0f);
    }

    /// 零矩阵
    LIMX_NODISCARD static FMatrix3 Zero()
    {
        return FMatrix3();
    }

    // ========================================================================
    // 工厂方法
    // ========================================================================

    /// 绕 Z 轴旋转 (2D 旋转)
    LIMX_NODISCARD static FMatrix3 MakeRotationZ(
        Float32 radians)
    {
        Float32 c = FMath::Cos(radians);
        Float32 s = FMath::Sin(radians);
        return FMatrix3(
             c, -s, 0.0f,
             s,  c, 0.0f,
            0.0f, 0.0f, 1.0f);
    }

    /// 绕 X 轴旋转
    LIMX_NODISCARD static FMatrix3 MakeRotationX(
        Float32 radians)
    {
        Float32 c = FMath::Cos(radians);
        Float32 s = FMath::Sin(radians);
        return FMatrix3(
            1.0f, 0.0f, 0.0f,
            0.0f,  c,   -s,
            0.0f,  s,    c);
    }

    /// 绕 Y 轴旋转
    LIMX_NODISCARD static FMatrix3 MakeRotationY(
        Float32 radians)
    {
        Float32 c = FMath::Cos(radians);
        Float32 s = FMath::Sin(radians);
        return FMatrix3(
             c,   0.0f,  s,
            0.0f, 1.0f, 0.0f,
            -s,   0.0f,  c);
    }

    /// 缩放矩阵
    LIMX_NODISCARD static FMatrix3 MakeScale(
        Float32 sx, Float32 sy, Float32 sz)
    {
        return FMatrix3(
            sx,   0.0f, 0.0f,
            0.0f, sy,   0.0f,
            0.0f, 0.0f, sz);
    }

    LIMX_NODISCARD static FMatrix3 MakeScale(
        const FVector3& scale)
    {
        return MakeScale(scale.X, scale.Y, scale.Z);
    }

    // ========================================================================
    // 矩阵运算
    // ========================================================================

    /// 矩阵乘法
    LIMX_NODISCARD FMatrix3 operator*(
        const FMatrix3& other) const
    {
        FMatrix3 result;
        for (Int32 row = 0; row < 3; ++row)
        {
            for (Int32 col = 0; col < 3; ++col)
            {
                result.M[row][col] =
                    M[row][0] * other.M[0][col] +
                    M[row][1] * other.M[1][col] +
                    M[row][2] * other.M[2][col];
            }
        }
        return result;
    }

    /// 标量乘法
    LIMX_NODISCARD FMatrix3 operator*(Float32 scalar) const
    {
        FMatrix3 result;
        for (Int32 row = 0; row < 3; ++row)
        {
            for (Int32 col = 0; col < 3; ++col)
            {
                result.M[row][col] = M[row][col] * scalar;
            }
        }
        return result;
    }

    /// 矩阵加法
    LIMX_NODISCARD FMatrix3 operator+(
        const FMatrix3& other) const
    {
        FMatrix3 result;
        for (Int32 row = 0; row < 3; ++row)
        {
            for (Int32 col = 0; col < 3; ++col)
            {
                result.M[row][col] =
                    M[row][col] + other.M[row][col];
            }
        }
        return result;
    }

    /// 转置
    LIMX_NODISCARD FMatrix3 Transpose() const
    {
        return FMatrix3(
            M[0][0], M[1][0], M[2][0],
            M[0][1], M[1][1], M[2][1],
            M[0][2], M[1][2], M[2][2]);
    }

    /// 行列式
    LIMX_NODISCARD Float32 Determinant() const
    {
        return M[0][0] * (M[1][1] * M[2][2] -
                           M[1][2] * M[2][1]) -
               M[0][1] * (M[1][0] * M[2][2] -
                           M[1][2] * M[2][0]) +
               M[0][2] * (M[1][0] * M[2][1] -
                           M[1][1] * M[2][0]);
    }

    /// 逆矩阵 (行列式为零时返回零矩阵)
    LIMX_NODISCARD FMatrix3 Inverse() const
    {
        Float32 det = Determinant();
        if (FMath::Abs(det) < 1e-8f)
        {
            return Zero();
        }

        Float32 invDet = 1.0f / det;

        FMatrix3 result;
        result.M[0][0] = (M[1][1] * M[2][2] -
                           M[1][2] * M[2][1]) * invDet;
        result.M[0][1] = (M[0][2] * M[2][1] -
                           M[0][1] * M[2][2]) * invDet;
        result.M[0][2] = (M[0][1] * M[1][2] -
                           M[0][2] * M[1][1]) * invDet;
        result.M[1][0] = (M[1][2] * M[2][0] -
                           M[1][0] * M[2][2]) * invDet;
        result.M[1][1] = (M[0][0] * M[2][2] -
                           M[0][2] * M[2][0]) * invDet;
        result.M[1][2] = (M[0][2] * M[1][0] -
                           M[0][0] * M[1][2]) * invDet;
        result.M[2][0] = (M[1][0] * M[2][1] -
                           M[1][1] * M[2][0]) * invDet;
        result.M[2][1] = (M[0][1] * M[2][0] -
                           M[0][0] * M[2][1]) * invDet;
        result.M[2][2] = (M[0][0] * M[1][1] -
                           M[0][1] * M[1][0]) * invDet;

        return result;
    }

    // ========================================================================
    // 向量变换
    // ========================================================================

    /// 变换 3D 向量
    LIMX_NODISCARD FVector3 TransformVector(
        const FVector3& v) const
    {
        return FVector3(
            M[0][0] * v.X + M[0][1] * v.Y + M[0][2] * v.Z,
            M[1][0] * v.X + M[1][1] * v.Y + M[1][2] * v.Z,
            M[2][0] * v.X + M[2][1] * v.Y + M[2][2] * v.Z);
    }

    /// 变换 2D 向量 (忽略第三行/列)
    LIMX_NODISCARD FVector2 TransformVector2D(
        const FVector2& v) const
    {
        return FVector2(
            M[0][0] * v.X + M[0][1] * v.Y,
            M[1][0] * v.X + M[1][1] * v.Y);
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 获取行向量
    LIMX_NODISCARD FVector3 GetRow(Int32 row) const
    {
        return FVector3(M[row][0], M[row][1], M[row][2]);
    }

    /// 获取列向量
    LIMX_NODISCARD FVector3 GetColumn(Int32 col) const
    {
        return FVector3(M[0][col], M[1][col], M[2][col]);
    }

    /// 设置行
    void SetRow(Int32 row, const FVector3& v)
    {
        M[row][0] = v.X;
        M[row][1] = v.Y;
        M[row][2] = v.Z;
    }

    /// 设置列
    void SetColumn(Int32 col, const FVector3& v)
    {
        M[0][col] = v.X;
        M[1][col] = v.Y;
        M[2][col] = v.Z;
    }
};

} // namespace Limx
