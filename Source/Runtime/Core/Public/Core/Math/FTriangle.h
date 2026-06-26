/*******************************************************************************
 * 文件: FTriangle.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   三角形 — 2D/3D 三角形基元
 *   提供面积、法线、重心坐标、点包含测试等操作
 *   用于光栅化、碰撞检测、网格处理、射线求交等场景
 *
 * 设计哲学:
 *   三顶点 — 以 A/B/C 三个顶点定义
 *   重心坐标 — 点到三角形的重心参数化
 *   值类型 — 轻量 POD 风格
 *
 * 技术特性:
 *   - FTriangle2D: 2D 三角形
 *   - FTriangle3D: 3D 三角形
 *   - GetArea: 面积
 *   - GetNormal: 法线 (3D)
 *   - GetBarycentric: 重心坐标
 *   - ContainsPoint: 点包含测试
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Math/FVector.h, Core/Math/FMath.h
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

/// 2D 三角形
struct FTriangle2D
{
    FVector2 A;  ///< 顶点 A
    FVector2 B;  ///< 顶点 B
    FVector2 C;  ///< 顶点 C

    FTriangle2D() = default;

    FTriangle2D(const FVector2& a, const FVector2& b,
                const FVector2& c)
        : A(a), B(b), C(c)
    {
    }

    /// 有符号面积 (正=逆时针)
    LIMX_NODISCARD Float32 GetSignedArea() const
    {
        return 0.5f * ((B.X - A.X) * (C.Y - A.Y) -
                        (C.X - A.X) * (B.Y - A.Y));
    }

    /// 面积
    LIMX_NODISCARD Float32 GetArea() const
    {
        return FMath::Abs(GetSignedArea());
    }

    /// 重心
    LIMX_NODISCARD FVector2 GetCentroid() const
    {
        return FVector2(
            (A.X + B.X + C.X) / 3.0f,
            (A.Y + B.Y + C.Y) / 3.0f);
    }

    /// 重心坐标
    /// @param point 目标点
    /// @param outU A 的权重
    /// @param outV B 的权重
    /// @param outW C 的权重
    void GetBarycentric(const FVector2& point,
                        Float32& outU, Float32& outV,
                        Float32& outW) const
    {
        FVector2 v0 = B - A;
        FVector2 v1 = C - A;
        FVector2 v2 = point - A;

        Float32 d00 = FVector2::Dot(v0, v0);
        Float32 d01 = FVector2::Dot(v0, v1);
        Float32 d11 = FVector2::Dot(v1, v1);
        Float32 d20 = FVector2::Dot(v2, v0);
        Float32 d21 = FVector2::Dot(v2, v1);

        Float32 denom = d00 * d11 - d01 * d01;
        if (FMath::Abs(denom) < 1e-8f)
        {
            outU = outV = outW = 1.0f / 3.0f;
            return;
        }

        Float32 invDenom = 1.0f / denom;
        outV = (d11 * d20 - d01 * d21) * invDenom;
        outW = (d00 * d21 - d01 * d20) * invDenom;
        outU = 1.0f - outV - outW;
    }

    /// 点包含测试 (重心坐标法)
    LIMX_NODISCARD bool ContainsPoint(
        const FVector2& point) const
    {
        Float32 u, v, w;
        GetBarycentric(point, u, v, w);
        return u >= 0.0f && v >= 0.0f && w >= 0.0f;
    }

    /// 周长
    LIMX_NODISCARD Float32 GetPerimeter() const
    {
        return (B - A).Length() +
               (C - B).Length() +
               (A - C).Length();
    }
};

/// 3D 三角形
struct FTriangle3D
{
    FVector3 A;  ///< 顶点 A
    FVector3 B;  ///< 顶点 B
    FVector3 C;  ///< 顶点 C

    FTriangle3D() = default;

    FTriangle3D(const FVector3& a, const FVector3& b,
                const FVector3& c)
        : A(a), B(b), C(c)
    {
    }

    /// 面积
    LIMX_NODISCARD Float32 GetArea() const
    {
        FVector3 cross = FVector3::Cross(B - A, C - A);
        return cross.Length() * 0.5f;
    }

    /// 法线 (未归一化)
    LIMX_NODISCARD FVector3 GetNormal() const
    {
        return FVector3::Cross(B - A, C - A);
    }

    /// 单位法线
    LIMX_NODISCARD FVector3 GetUnitNormal() const
    {
        FVector3 n = GetNormal();
        Float32 len = n.Length();
        if (len < 1e-8f) return FVector3(0.0f, 1.0f, 0.0f);
        return n * (1.0f / len);
    }

    /// 重心
    LIMX_NODISCARD FVector3 GetCentroid() const
    {
        return FVector3(
            (A.X + B.X + C.X) / 3.0f,
            (A.Y + B.Y + C.Y) / 3.0f,
            (A.Z + B.Z + C.Z) / 3.0f);
    }

    /// 重心坐标
    void GetBarycentric(const FVector3& point,
                        Float32& outU, Float32& outV,
                        Float32& outW) const
    {
        FVector3 v0 = B - A;
        FVector3 v1 = C - A;
        FVector3 v2 = point - A;

        Float32 d00 = FVector3::Dot(v0, v0);
        Float32 d01 = FVector3::Dot(v0, v1);
        Float32 d11 = FVector3::Dot(v1, v1);
        Float32 d20 = FVector3::Dot(v2, v0);
        Float32 d21 = FVector3::Dot(v2, v1);

        Float32 denom = d00 * d11 - d01 * d01;
        if (FMath::Abs(denom) < 1e-8f)
        {
            outU = outV = outW = 1.0f / 3.0f;
            return;
        }

        Float32 invDenom = 1.0f / denom;
        outV = (d11 * d20 - d01 * d21) * invDenom;
        outW = (d00 * d21 - d01 * d20) * invDenom;
        outU = 1.0f - outV - outW;
    }

    /// 点到三角形最近点
    LIMX_NODISCARD FVector3 ClosestPointTo(
        const FVector3& point) const
    {
        Float32 u, v, w;
        GetBarycentric(point, u, v, w);

        // 如果在三角形内部则直接投影
        if (u >= 0.0f && v >= 0.0f && w >= 0.0f)
        {
            return A * u + B * v + C * w;
        }

        // 夹持重心坐标到有效范围
        u = FMath::Max(u, 0.0f);
        v = FMath::Max(v, 0.0f);
        w = FMath::Max(w, 0.0f);
        Float32 sum = u + v + w;
        if (sum > 1e-8f)
        {
            Float32 invSum = 1.0f / sum;
            u *= invSum;
            v *= invSum;
            w *= invSum;
        }

        return A * u + B * v + C * w;
    }

    /// 周长
    LIMX_NODISCARD Float32 GetPerimeter() const
    {
        return (B - A).Length() +
               (C - B).Length() +
               (A - C).Length();
    }
};

} // namespace Limx
