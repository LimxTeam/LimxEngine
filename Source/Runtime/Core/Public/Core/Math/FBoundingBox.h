/*******************************************************************************
 * 文件: FBoundingBox.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   轴对齐包围盒 (AABB) — 3D 空间中的矩形包围体
 *   存储最小点和最大点，支持相交测试、合并、扩展等操作
 *   用于场景裁剪、碰撞检测粗筛、空间划分等场景
 *
 * 设计哲学:
 *   最小/最大点表示 — Min/Max 两个 FVector3 完整描述包围盒
 *   值语义 — 轻量值类型，可安全拷贝和按值传递
 *   链式操作 — 扩展/合并返回新实例，不修改原始数据
 *
 * 技术特性:
 *   - 存储: FVector3 Min + FVector3 Max (24 字节)
 *   - FromCenterExtent: 从中心+半径构造
 *   - Contains: 点/盒包含测试
 *   - Intersects: 盒-盒相交测试
 *   - Union: 合并两个包围盒
 *   - Expand: 按标量扩展
 *   - GetCenter/GetExtent/GetSize: 几何属性查询
 *   - GetVolume/GetSurfaceArea: 体积/表面积
 *
 * 依赖关系:
 *   内部: Core/Math/FVector.h, Core/Math/FMath.h
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

/// 轴对齐包围盒 (AABB)
struct FBoundingBox
{
    FVector3 Min;  ///< 最小角点
    FVector3 Max;  ///< 最大角点

    // ========================================================================
    // 构造
    // ========================================================================

    /// 默认构造 — 无效包围盒 (反转的极值)
    FBoundingBox()
        : Min(FVector3( 3.402823466e+38f,  3.402823466e+38f,
                         3.402823466e+38f))
        , Max(FVector3(-3.402823466e+38f, -3.402823466e+38f,
                        -3.402823466e+38f))
    {
    }

    /// 从最小/最大点构造
    FBoundingBox(const FVector3& inMin, const FVector3& inMax)
        : Min(inMin), Max(inMax)
    {
    }

    /// 从中心和半径构造
    LIMX_NODISCARD static FBoundingBox FromCenterExtent(
        const FVector3& center, const FVector3& extent)
    {
        return FBoundingBox(center - extent, center + extent);
    }

    /// 从单点构造 (零大小包围盒)
    LIMX_NODISCARD static FBoundingBox FromPoint(const FVector3& point)
    {
        return FBoundingBox(point, point);
    }

    // ========================================================================
    // 几何属性
    // ========================================================================

    /// 中心点
    LIMX_NODISCARD FVector3 GetCenter() const
    {
        return (Min + Max) * 0.5f;
    }

    /// 半径 (从中心到角点的各轴分量)
    LIMX_NODISCARD FVector3 GetExtent() const
    {
        return (Max - Min) * 0.5f;
    }

    /// 各轴尺寸
    LIMX_NODISCARD FVector3 GetSize() const
    {
        return Max - Min;
    }

    /// 体积
    LIMX_NODISCARD Float32 GetVolume() const
    {
        FVector3 size = GetSize();
        return size.X * size.Y * size.Z;
    }

    /// 表面积
    LIMX_NODISCARD Float32 GetSurfaceArea() const
    {
        FVector3 size = GetSize();
        return 2.0f * (size.X * size.Y +
                        size.Y * size.Z +
                        size.Z * size.X);
    }

    /// 是否有效 (Min <= Max)
    LIMX_NODISCARD bool IsValid() const
    {
        return Min.X <= Max.X && Min.Y <= Max.Y && Min.Z <= Max.Z;
    }

    // ========================================================================
    // 包含测试
    // ========================================================================

    /// 点是否在包围盒内
    LIMX_NODISCARD bool Contains(const FVector3& point) const
    {
        return point.X >= Min.X && point.X <= Max.X &&
               point.Y >= Min.Y && point.Y <= Max.Y &&
               point.Z >= Min.Z && point.Z <= Max.Z;
    }

    /// 另一个包围盒是否完全在内部
    LIMX_NODISCARD bool Contains(const FBoundingBox& other) const
    {
        return other.Min.X >= Min.X && other.Max.X <= Max.X &&
               other.Min.Y >= Min.Y && other.Max.Y <= Max.Y &&
               other.Min.Z >= Min.Z && other.Max.Z <= Max.Z;
    }

    // ========================================================================
    // 相交测试
    // ========================================================================

    /// 盒-盒相交测试
    LIMX_NODISCARD bool Intersects(const FBoundingBox& other) const
    {
        if (Max.X < other.Min.X || Min.X > other.Max.X) return false;
        if (Max.Y < other.Min.Y || Min.Y > other.Max.Y) return false;
        if (Max.Z < other.Min.Z || Min.Z > other.Max.Z) return false;
        return true;
    }

    /// 计算两个盒的相交部分 (可能无效)
    LIMX_NODISCARD FBoundingBox Intersection(
        const FBoundingBox& other) const
    {
        FVector3 newMin(
            FMath::Max(Min.X, other.Min.X),
            FMath::Max(Min.Y, other.Min.Y),
            FMath::Max(Min.Z, other.Min.Z));
        FVector3 newMax(
            FMath::Min(Max.X, other.Max.X),
            FMath::Min(Max.Y, other.Max.Y),
            FMath::Min(Max.Z, other.Max.Z));
        return FBoundingBox(newMin, newMax);
    }

    // ========================================================================
    // 合并与扩展
    // ========================================================================

    /// 合并另一个包围盒
    LIMX_NODISCARD FBoundingBox Union(const FBoundingBox& other) const
    {
        FVector3 newMin(
            FMath::Min(Min.X, other.Min.X),
            FMath::Min(Min.Y, other.Min.Y),
            FMath::Min(Min.Z, other.Min.Z));
        FVector3 newMax(
            FMath::Max(Max.X, other.Max.X),
            FMath::Max(Max.Y, other.Max.Y),
            FMath::Max(Max.Z, other.Max.Z));
        return FBoundingBox(newMin, newMax);
    }

    /// 扩展包围盒以包含指定点
    LIMX_NODISCARD FBoundingBox ExpandToInclude(
        const FVector3& point) const
    {
        FVector3 newMin(
            FMath::Min(Min.X, point.X),
            FMath::Min(Min.Y, point.Y),
            FMath::Min(Min.Z, point.Z));
        FVector3 newMax(
            FMath::Max(Max.X, point.X),
            FMath::Max(Max.Y, point.Y),
            FMath::Max(Max.Z, point.Z));
        return FBoundingBox(newMin, newMax);
    }

    /// 按标量均匀扩展
    LIMX_NODISCARD FBoundingBox Expand(Float32 amount) const
    {
        FVector3 offset(amount, amount, amount);
        return FBoundingBox(Min - offset, Max + offset);
    }

    /// 按各轴分别扩展
    LIMX_NODISCARD FBoundingBox Expand(const FVector3& amount) const
    {
        return FBoundingBox(Min - amount, Max + amount);
    }

    // ========================================================================
    // 距离
    // ========================================================================

    /// 点到包围盒的最近距离 (点在内部返回 0)
    LIMX_NODISCARD Float32 DistanceTo(const FVector3& point) const
    {
        Float32 distSq = DistanceSquaredTo(point);
        return FMath::Sqrt(distSq);
    }

    /// 点到包围盒的最近距离平方
    LIMX_NODISCARD Float32 DistanceSquaredTo(
        const FVector3& point) const
    {
        Float32 distSq = 0.0f;

        // X 轴
        if (point.X < Min.X)
        {
            Float32 delta = Min.X - point.X;
            distSq += delta * delta;
        }
        else if (point.X > Max.X)
        {
            Float32 delta = point.X - Max.X;
            distSq += delta * delta;
        }

        // Y 轴
        if (point.Y < Min.Y)
        {
            Float32 delta = Min.Y - point.Y;
            distSq += delta * delta;
        }
        else if (point.Y > Max.Y)
        {
            Float32 delta = point.Y - Max.Y;
            distSq += delta * delta;
        }

        // Z 轴
        if (point.Z < Min.Z)
        {
            Float32 delta = Min.Z - point.Z;
            distSq += delta * delta;
        }
        else if (point.Z > Max.Z)
        {
            Float32 delta = point.Z - Max.Z;
            distSq += delta * delta;
        }

        return distSq;
    }

    // ========================================================================
    // 比较
    // ========================================================================

    LIMX_NODISCARD bool operator==(const FBoundingBox& other) const
    {
        return Min == other.Min && Max == other.Max;
    }

    LIMX_NODISCARD bool operator!=(const FBoundingBox& other) const
    {
        return !(*this == other);
    }
};

} // namespace Limx
