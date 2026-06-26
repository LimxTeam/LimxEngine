/*******************************************************************************
 * 文件: FCircle.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   2D 圆 — 圆心 + 半径表示的圆形基元
 *   提供点包含、圆-圆相交、圆-矩形相交、切线等操作
 *   用于 2D 碰撞检测、UI 圆角区域、粒子半径、影响范围等场景
 *
 * 设计哲学:
 *   圆心+半径 — 最简表示，4 字节对齐
 *   值类型 — 轻量 POD 风格
 *   几何查询 — 包含/相交/距离/切点
 *
 * 技术特性:
 *   - FCircle: 2D 圆
 *   - ContainsPoint: 点包含测试
 *   - Intersects: 圆-圆/圆-矩形相交
 *   - GetArea/GetCircumference: 面积/周长
 *   - ClosestPointTo: 圆上最近点
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

// 前向声明
struct FRect;

/// 2D 圆
struct FCircle
{
    FVector2 Center;  ///< 圆心
    Float32  Radius;  ///< 半径

    // ========================================================================
    // 构造
    // ========================================================================

    FCircle()
        : Center(0.0f, 0.0f), Radius(0.0f)
    {
    }

    FCircle(const FVector2& center, Float32 radius)
        : Center(center), Radius(radius)
    {
    }

    FCircle(Float32 centerX, Float32 centerY, Float32 radius)
        : Center(centerX, centerY), Radius(radius)
    {
    }

    // ========================================================================
    // 几何属性
    // ========================================================================

    /// 面积
    LIMX_NODISCARD Float32 GetArea() const
    {
        return FMath::kPi * Radius * Radius;
    }

    /// 周长
    LIMX_NODISCARD Float32 GetCircumference() const
    {
        return 2.0f * FMath::kPi * Radius;
    }

    /// 直径
    LIMX_NODISCARD Float32 GetDiameter() const
    {
        return Radius * 2.0f;
    }

    /// 是否退化 (半径 <= 0)
    LIMX_NODISCARD bool IsDegenerate() const
    {
        return Radius <= 0.0f;
    }

    // ========================================================================
    // 包含测试
    // ========================================================================

    /// 点是否在圆内
    LIMX_NODISCARD bool ContainsPoint(
        const FVector2& point) const
    {
        return (point - Center).LengthSquared() <=
               Radius * Radius;
    }

    /// 另一个圆是否完全在内
    LIMX_NODISCARD bool ContainsCircle(
        const FCircle& other) const
    {
        Float32 dist = (other.Center - Center).Length();
        return dist + other.Radius <= Radius;
    }

    // ========================================================================
    // 相交测试
    // ========================================================================

    /// 圆-圆相交
    LIMX_NODISCARD bool Intersects(
        const FCircle& other) const
    {
        Float32 distSq =
            (other.Center - Center).LengthSquared();
        Float32 radiusSum = Radius + other.Radius;
        return distSq <= radiusSum * radiusSum;
    }

    /// 点到圆心距离
    LIMX_NODISCARD Float32 DistanceToCenter(
        const FVector2& point) const
    {
        return (point - Center).Length();
    }

    /// 点到圆边的有符号距离 (负=内部, 正=外部)
    LIMX_NODISCARD Float32 SignedDistanceTo(
        const FVector2& point) const
    {
        return (point - Center).Length() - Radius;
    }

    // ========================================================================
    // 最近点
    // ========================================================================

    /// 圆边上距指定点最近的点
    LIMX_NODISCARD FVector2 ClosestPointTo(
        const FVector2& point) const
    {
        FVector2 dir = point - Center;
        Float32 len = dir.Length();
        if (len < 1e-8f)
        {
            // 点在圆心，返回任意边上的点
            return FVector2(
                Center.X + Radius, Center.Y);
        }
        return Center + dir * (Radius / len);
    }

    // ========================================================================
    // 变换
    // ========================================================================

    /// 平移
    LIMX_NODISCARD FCircle Translate(
        const FVector2& offset) const
    {
        return FCircle(Center + offset, Radius);
    }

    /// 缩放半径
    LIMX_NODISCARD FCircle Scale(Float32 factor) const
    {
        return FCircle(Center, Radius * factor);
    }

    /// 膨胀
    LIMX_NODISCARD FCircle Expand(Float32 amount) const
    {
        return FCircle(Center, Radius + amount);
    }

    // ========================================================================
    // 工厂方法
    // ========================================================================

    /// 从两点构造 (以两点为直径端点)
    LIMX_NODISCARD static FCircle FromDiameter(
        const FVector2& a, const FVector2& b)
    {
        FVector2 center = (a + b) * 0.5f;
        Float32 radius = (b - a).Length() * 0.5f;
        return FCircle(center, radius);
    }

    /// 包围两个圆的最小圆
    LIMX_NODISCARD static FCircle Merge(
        const FCircle& a, const FCircle& b)
    {
        FVector2 diff = b.Center - a.Center;
        Float32 dist = diff.Length();

        // a 包含 b
        if (dist + b.Radius <= a.Radius) return a;
        // b 包含 a
        if (dist + a.Radius <= b.Radius) return b;

        // 两圆部分重叠或不重叠
        Float32 newRadius =
            (dist + a.Radius + b.Radius) * 0.5f;
        FVector2 newCenter = a.Center;
        if (dist > 1e-8f)
        {
            newCenter = a.Center +
                diff * ((newRadius - a.Radius) / dist);
        }
        return FCircle(newCenter, newRadius);
    }

    // ========================================================================
    // 比较
    // ========================================================================

    LIMX_NODISCARD bool operator==(
        const FCircle& other) const
    {
        return FMath::Abs(Center.X - other.Center.X) < 1e-6f &&
               FMath::Abs(Center.Y - other.Center.Y) < 1e-6f &&
               FMath::Abs(Radius - other.Radius) < 1e-6f;
    }

    LIMX_NODISCARD bool operator!=(
        const FCircle& other) const
    {
        return !(*this == other);
    }
};

} // namespace Limx
