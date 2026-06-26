/*******************************************************************************
 * 文件: FSphere.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   包围球 — 3D 空间中由中心和半径定义的球体
 *   支持包含测试、相交测试、合并等操作
 *   用于粗筛碰撞检测、LOD 选择、遮挡剔除等场景
 *
 * 设计哲学:
 *   中心+半径表示 — Center + Radius (16 字节)
 *   值语义 — 轻量 POD 类型
 *   与 AABB/射线/平面 互操作
 *
 * 技术特性:
 *   - Contains: 点/球包含测试
 *   - Intersects: 球-球/球-AABB 相交
 *   - Merge: 合并两个包围球
 *   - GetBoundingBox: 转换为 AABB
 *   - IsInsidePlane: 平面裁剪测试
 *
 * 依赖关系:
 *   内部: Core/Math/FVector.h, Core/Math/FMath.h,
 *          Core/Math/FBoundingBox.h, Core/Math/FPlane.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Math/FMath.h"
#include "Core/Math/FVector.h"
#include "Core/Math/FBoundingBox.h"
#include "Core/Math/FPlane.h"

namespace Limx
{

/// 包围球 — 中心 + 半径
struct FSphere
{
    FVector3 Center;  ///< 球心
    Float32  Radius;  ///< 半径

    // ========================================================================
    // 构造
    // ========================================================================

    /// 默认构造 — 原点, 半径 0
    FSphere()
        : Center(0.0f, 0.0f, 0.0f)
        , Radius(0.0f)
    {
    }

    /// 指定中心和半径
    FSphere(const FVector3& center, Float32 radius)
        : Center(center)
        , Radius(radius)
    {
    }

    // ========================================================================
    // 包含测试
    // ========================================================================

    /// 点是否在球内
    LIMX_NODISCARD bool Contains(const FVector3& point) const
    {
        return (point - Center).LengthSquared() <= Radius * Radius;
    }

    /// 另一个球是否完全在内部
    LIMX_NODISCARD bool Contains(const FSphere& other) const
    {
        Float32 dist = (other.Center - Center).Length();
        return dist + other.Radius <= Radius;
    }

    // ========================================================================
    // 相交测试
    // ========================================================================

    /// 球-球相交
    LIMX_NODISCARD bool Intersects(const FSphere& other) const
    {
        Float32 distSq = (other.Center - Center).LengthSquared();
        Float32 radiusSum = Radius + other.Radius;
        return distSq <= radiusSum * radiusSum;
    }

    /// 球-AABB 相交
    LIMX_NODISCARD bool Intersects(const FBoundingBox& box) const
    {
        return box.DistanceSquaredTo(Center) <= Radius * Radius;
    }

    // ========================================================================
    // 平面裁剪
    // ========================================================================

    /// 球是否完全在平面正半空间 (法线方向)
    LIMX_NODISCARD bool IsInFrontOfPlane(const FPlane& plane) const
    {
        return plane.SignedDistance(Center) >= Radius;
    }

    /// 球是否完全在平面负半空间
    LIMX_NODISCARD bool IsBehindPlane(const FPlane& plane) const
    {
        return plane.SignedDistance(Center) <= -Radius;
    }

    /// 球是否与平面相交
    LIMX_NODISCARD bool IntersectsPlane(const FPlane& plane) const
    {
        return FMath::Abs(plane.SignedDistance(Center)) < Radius;
    }

    // ========================================================================
    // 合并
    // ========================================================================

    /// 合并两个包围球 — 返回包含两者的最小包围球
    LIMX_NODISCARD static FSphere Merge(const FSphere& a,
                                          const FSphere& b)
    {
        FVector3 delta = b.Center - a.Center;
        Float32 dist = delta.Length();

        // a 包含 b
        if (dist + b.Radius <= a.Radius)
        {
            return a;
        }

        // b 包含 a
        if (dist + a.Radius <= b.Radius)
        {
            return b;
        }

        // 一般情况
        Float32 newRadius = (dist + a.Radius + b.Radius) * 0.5f;
        FVector3 newCenter = a.Center;
        if (dist > 1.0e-6f)
        {
            newCenter = a.Center +
                        delta * ((newRadius - a.Radius) / dist);
        }

        return FSphere(newCenter, newRadius);
    }

    // ========================================================================
    // 转换
    // ========================================================================

    /// 转换为轴对齐包围盒
    LIMX_NODISCARD FBoundingBox GetBoundingBox() const
    {
        FVector3 extent(Radius, Radius, Radius);
        return FBoundingBox(Center - extent, Center + extent);
    }

    /// 球的体积 (4/3 * π * r³)
    LIMX_NODISCARD Float32 GetVolume() const
    {
        return (4.0f / 3.0f) * FMath::kPi * Radius * Radius * Radius;
    }

    /// 球的表面积 (4 * π * r²)
    LIMX_NODISCARD Float32 GetSurfaceArea() const
    {
        return 4.0f * FMath::kPi * Radius * Radius;
    }

    // ========================================================================
    // 比较
    // ========================================================================

    LIMX_NODISCARD bool operator==(const FSphere& other) const
    {
        return Center == other.Center &&
               FMath::Abs(Radius - other.Radius) < 1.0e-6f;
    }

    LIMX_NODISCARD bool operator!=(const FSphere& other) const
    {
        return !(*this == other);
    }
};

} // namespace Limx
