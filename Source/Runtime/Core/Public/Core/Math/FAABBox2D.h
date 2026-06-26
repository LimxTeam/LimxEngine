/*******************************************************************************
 * 文件: FAABBox2D.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   2D 轴对齐包围盒 (AABB) — Min/Max 表示的 2D 矩形区域
 *   提供包含、相交、合并、扩展、变换等操作
 *   用于 2D 碰撞检测、UI 裁剪矩形、可见性剔除等场景
 *
 * 设计哲学:
 *   Min/Max 表示 — 非 XY+WH，便于 SIMD 化
 *   值类型 — 轻量可拷贝
 *   与 FRect 区分 — FRect 为 XY+WH UI 矩形，FAABBox2D 为物理 Min/Max
 *
 * 技术特性:
 *   - FAABBox2D: 2D 轴对齐包围盒
 *   - ContainsPoint/ContainsBox: 包含测试
 *   - Intersects: 相交测试
 *   - Merge: 合并包围盒
 *   - Expand/Inflate: 扩展
 *   - GetCenter/GetExtent/GetArea: 属性查询
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

/// 2D 轴对齐包围盒 (Min/Max 表示)
struct FAABBox2D
{
    FVector2 Min;  ///< 最小点 (左下)
    FVector2 Max;  ///< 最大点 (右上)

    // ========================================================================
    // 构造
    // ========================================================================

    FAABBox2D()
        : Min( 3.402823e+38f,  3.402823e+38f)
        , Max(-3.402823e+38f, -3.402823e+38f)
    {
    }

    FAABBox2D(const FVector2& min, const FVector2& max)
        : Min(min), Max(max)
    {
    }

    FAABBox2D(Float32 minX, Float32 minY,
              Float32 maxX, Float32 maxY)
        : Min(minX, minY), Max(maxX, maxY)
    {
    }

    /// 从中心和半尺寸构造
    LIMX_NODISCARD static FAABBox2D FromCenterExtent(
        const FVector2& center,
        const FVector2& halfExtent)
    {
        return FAABBox2D(center - halfExtent,
                         center + halfExtent);
    }

    /// 无效 (反转) 包围盒 (适合开始 Merge)
    LIMX_NODISCARD static FAABBox2D Invalid()
    {
        return FAABBox2D();
    }

    /// 退化点
    LIMX_NODISCARD static FAABBox2D Point(
        const FVector2& p)
    {
        return FAABBox2D(p, p);
    }

    // ========================================================================
    // 属性
    // ========================================================================

    LIMX_NODISCARD FVector2 GetCenter() const
    {
        return (Min + Max) * 0.5f;
    }

    LIMX_NODISCARD FVector2 GetExtent() const
    {
        return (Max - Min) * 0.5f;
    }

    LIMX_NODISCARD FVector2 GetSize() const
    {
        return Max - Min;
    }

    LIMX_NODISCARD Float32 GetWidth() const
    {
        return Max.X - Min.X;
    }

    LIMX_NODISCARD Float32 GetHeight() const
    {
        return Max.Y - Min.Y;
    }

    LIMX_NODISCARD Float32 GetArea() const
    {
        FVector2 size = GetSize();
        return FMath::Max(0.0f, size.X) *
               FMath::Max(0.0f, size.Y);
    }

    LIMX_NODISCARD Float32 GetPerimeter() const
    {
        FVector2 size = GetSize();
        return 2.0f * (FMath::Max(0.0f, size.X) +
                       FMath::Max(0.0f, size.Y));
    }

    /// 是否为有效包围盒
    LIMX_NODISCARD bool IsValid() const
    {
        return Min.X <= Max.X && Min.Y <= Max.Y;
    }

    // ========================================================================
    // 包含测试
    // ========================================================================

    LIMX_NODISCARD bool ContainsPoint(
        const FVector2& point) const
    {
        return point.X >= Min.X && point.X <= Max.X &&
               point.Y >= Min.Y && point.Y <= Max.Y;
    }

    LIMX_NODISCARD bool ContainsBox(
        const FAABBox2D& other) const
    {
        return other.Min.X >= Min.X &&
               other.Max.X <= Max.X &&
               other.Min.Y >= Min.Y &&
               other.Max.Y <= Max.Y;
    }

    // ========================================================================
    // 相交
    // ========================================================================

    LIMX_NODISCARD bool Intersects(
        const FAABBox2D& other) const
    {
        return Min.X <= other.Max.X &&
               Max.X >= other.Min.X &&
               Min.Y <= other.Max.Y &&
               Max.Y >= other.Min.Y;
    }

    /// 计算相交区域
    LIMX_NODISCARD FAABBox2D Intersection(
        const FAABBox2D& other) const
    {
        FAABBox2D result(
            FVector2(FMath::Max(Min.X, other.Min.X),
                     FMath::Max(Min.Y, other.Min.Y)),
            FVector2(FMath::Min(Max.X, other.Max.X),
                     FMath::Min(Max.Y, other.Max.Y)));

        if (!result.IsValid())
            return FAABBox2D::Invalid();
        return result;
    }

    // ========================================================================
    // 扩展
    // ========================================================================

    /// 扩展以包含点
    void ExpandToContain(const FVector2& point)
    {
        Min.X = FMath::Min(Min.X, point.X);
        Min.Y = FMath::Min(Min.Y, point.Y);
        Max.X = FMath::Max(Max.X, point.X);
        Max.Y = FMath::Max(Max.Y, point.Y);
    }

    /// 扩展以包含另一包围盒
    void ExpandToContain(const FAABBox2D& other)
    {
        Min.X = FMath::Min(Min.X, other.Min.X);
        Min.Y = FMath::Min(Min.Y, other.Min.Y);
        Max.X = FMath::Max(Max.X, other.Max.X);
        Max.Y = FMath::Max(Max.Y, other.Max.Y);
    }

    /// 向外膨胀
    LIMX_NODISCARD FAABBox2D Inflate(Float32 amount) const
    {
        return FAABBox2D(
            Min - FVector2(amount, amount),
            Max + FVector2(amount, amount));
    }

    LIMX_NODISCARD FAABBox2D Inflate(
        const FVector2& amount) const
    {
        return FAABBox2D(Min - amount, Max + amount);
    }

    // ========================================================================
    // 合并
    // ========================================================================

    LIMX_NODISCARD static FAABBox2D Merge(
        const FAABBox2D& a, const FAABBox2D& b)
    {
        return FAABBox2D(
            FVector2(FMath::Min(a.Min.X, b.Min.X),
                     FMath::Min(a.Min.Y, b.Min.Y)),
            FVector2(FMath::Max(a.Max.X, b.Max.X),
                     FMath::Max(a.Max.Y, b.Max.Y)));
    }

    // ========================================================================
    // 比较
    // ========================================================================

    LIMX_NODISCARD bool operator==(
        const FAABBox2D& other) const
    {
        return FMath::Abs(Min.X - other.Min.X) < 1e-6f &&
               FMath::Abs(Min.Y - other.Min.Y) < 1e-6f &&
               FMath::Abs(Max.X - other.Max.X) < 1e-6f &&
               FMath::Abs(Max.Y - other.Max.Y) < 1e-6f;
    }

    LIMX_NODISCARD bool operator!=(
        const FAABBox2D& other) const
    {
        return !(*this == other);
    }
};

} // namespace Limx
