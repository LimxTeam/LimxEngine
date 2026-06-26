/*******************************************************************************
 * 文件: FLine.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   直线段 — 2D/3D 线段的端点表示与参数化求值
 *   提供长度、方向、点到线段距离、线段间最近点等操作
 *   用于碰撞检测辅助、调试绘制、路径线段、线框渲染等场景
 *
 * 设计哲学:
 *   端点表示 — 以起点 Start 和终点 End 定义线段
 *   参数化 — t∈[0,1] 映射线段上的点
 *   值类型 — 轻量 POD 风格，可自由拷贝
 *
 * 技术特性:
 *   - FLine2D: 2D 线段
 *   - FLine3D: 3D 线段
 *   - GetPointAt: 参数化求点
 *   - GetLength: 长度
 *   - ClosestPointTo: 点到线段最近点
 *   - DistanceTo: 点到线段距离
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

/// 2D 线段
struct FLine2D
{
    FVector2 Start;  ///< 起点
    FVector2 End;    ///< 终点

    FLine2D() = default;

    FLine2D(const FVector2& start, const FVector2& end)
        : Start(start), End(end)
    {
    }

    /// 参数化求点 t∈[0,1]
    LIMX_NODISCARD FVector2 GetPointAt(Float32 t) const
    {
        return Start + (End - Start) * t;
    }

    /// 中点
    LIMX_NODISCARD FVector2 GetMidpoint() const
    {
        return (Start + End) * 0.5f;
    }

    /// 方向 (未归一化)
    LIMX_NODISCARD FVector2 GetDirection() const
    {
        return End - Start;
    }

    /// 长度
    LIMX_NODISCARD Float32 GetLength() const
    {
        return (End - Start).Length();
    }

    /// 长度平方
    LIMX_NODISCARD Float32 GetLengthSquared() const
    {
        return (End - Start).LengthSquared();
    }

    /// 点到线段最近点的参数 t
    LIMX_NODISCARD Float32 ClosestParamTo(
        const FVector2& point) const
    {
        FVector2 direction = End - Start;
        Float32 lenSq = direction.LengthSquared();
        if (lenSq < 1e-8f) return 0.0f;

        Float32 t = FVector2::Dot(
            point - Start, direction) / lenSq;
        return FMath::Clamp(t, 0.0f, 1.0f);
    }

    /// 点到线段最近点
    LIMX_NODISCARD FVector2 ClosestPointTo(
        const FVector2& point) const
    {
        return GetPointAt(ClosestParamTo(point));
    }

    /// 点到线段距离
    LIMX_NODISCARD Float32 DistanceTo(
        const FVector2& point) const
    {
        return (point - ClosestPointTo(point)).Length();
    }

    /// 点到线段距离平方
    LIMX_NODISCARD Float32 DistanceSquaredTo(
        const FVector2& point) const
    {
        return (point - ClosestPointTo(point)).LengthSquared();
    }
};

/// 3D 线段
struct FLine3D
{
    FVector3 Start;  ///< 起点
    FVector3 End;    ///< 终点

    FLine3D() = default;

    FLine3D(const FVector3& start, const FVector3& end)
        : Start(start), End(end)
    {
    }

    /// 参数化求点 t∈[0,1]
    LIMX_NODISCARD FVector3 GetPointAt(Float32 t) const
    {
        return Start + (End - Start) * t;
    }

    /// 中点
    LIMX_NODISCARD FVector3 GetMidpoint() const
    {
        return (Start + End) * 0.5f;
    }

    /// 方向 (未归一化)
    LIMX_NODISCARD FVector3 GetDirection() const
    {
        return End - Start;
    }

    /// 长度
    LIMX_NODISCARD Float32 GetLength() const
    {
        return (End - Start).Length();
    }

    /// 长度平方
    LIMX_NODISCARD Float32 GetLengthSquared() const
    {
        return (End - Start).LengthSquared();
    }

    /// 点到线段最近点的参数 t
    LIMX_NODISCARD Float32 ClosestParamTo(
        const FVector3& point) const
    {
        FVector3 direction = End - Start;
        Float32 lenSq = direction.LengthSquared();
        if (lenSq < 1e-8f) return 0.0f;

        Float32 t = FVector3::Dot(
            point - Start, direction) / lenSq;
        return FMath::Clamp(t, 0.0f, 1.0f);
    }

    /// 点到线段最近点
    LIMX_NODISCARD FVector3 ClosestPointTo(
        const FVector3& point) const
    {
        return GetPointAt(ClosestParamTo(point));
    }

    /// 点到线段距离
    LIMX_NODISCARD Float32 DistanceTo(
        const FVector3& point) const
    {
        return (point - ClosestPointTo(point)).Length();
    }

    /// 点到线段距离平方
    LIMX_NODISCARD Float32 DistanceSquaredTo(
        const FVector3& point) const
    {
        return (point - ClosestPointTo(point)).LengthSquared();
    }

    /// 两线段间最短距离的参数对
    /// @param other 另一线段
    /// @param outT 本线段上的参数
    /// @param outU 另一线段上的参数
    void ClosestParamsToSegment(
        const FLine3D& other,
        Float32& outT, Float32& outU) const
    {
        FVector3 d1 = End - Start;
        FVector3 d2 = other.End - other.Start;
        FVector3 r = Start - other.Start;

        Float32 a = FVector3::Dot(d1, d1);
        Float32 e = FVector3::Dot(d2, d2);
        Float32 f = FVector3::Dot(d2, r);

        if (a < 1e-8f && e < 1e-8f)
        {
            outT = 0.0f;
            outU = 0.0f;
            return;
        }

        if (a < 1e-8f)
        {
            outT = 0.0f;
            outU = FMath::Clamp(f / e, 0.0f, 1.0f);
            return;
        }

        Float32 c = FVector3::Dot(d1, r);
        if (e < 1e-8f)
        {
            outU = 0.0f;
            outT = FMath::Clamp(-c / a, 0.0f, 1.0f);
            return;
        }

        Float32 b = FVector3::Dot(d1, d2);
        Float32 denom = a * e - b * b;

        if (denom > 1e-8f)
        {
            outT = FMath::Clamp(
                (b * f - c * e) / denom, 0.0f, 1.0f);
        }
        else
        {
            outT = 0.0f;
        }

        outU = (b * outT + f) / e;

        if (outU < 0.0f)
        {
            outU = 0.0f;
            outT = FMath::Clamp(-c / a, 0.0f, 1.0f);
        }
        else if (outU > 1.0f)
        {
            outU = 1.0f;
            outT = FMath::Clamp(
                (b - c) / a, 0.0f, 1.0f);
        }
    }

    /// 两线段间最短距离
    LIMX_NODISCARD Float32 DistanceToSegment(
        const FLine3D& other) const
    {
        Float32 t, u;
        ClosestParamsToSegment(other, t, u);
        FVector3 p1 = GetPointAt(t);
        FVector3 p2 = other.GetPointAt(u);
        return (p2 - p1).Length();
    }
};

} // namespace Limx
