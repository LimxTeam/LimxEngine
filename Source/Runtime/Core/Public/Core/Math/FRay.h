/*******************************************************************************
 * 文件: FRay.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   射线 — 3D 空间中从原点沿方向无限延伸的半直线
 *   支持与 AABB、平面、球体的相交测试
 *   用于光线投射、拾取检测、视线检测、光线追踪等场景
 *
 * 设计哲学:
 *   原点+方向表示 — Origin + Direction (Direction 应为单位向量)
 *   值语义 — 轻量 POD 类型，24 字节
 *   预计算倒数 — 可选缓存方向分量倒数加速批量 AABB 测试
 *
 * 技术特性:
 *   - PointAt(t): 沿射线参数化取点 Origin + t * Direction
 *   - IntersectsAABB: 射线-AABB 相交 (Slab 方法)
 *   - IntersectsPlane: 射线-平面相交
 *   - IntersectsSphere: 射线-球相交
 *   - ClosestPoint: 射线上距给定点最近的点
 *
 * 依赖关系:
 *   内部: Core/Math/FVector.h, Core/Math/FMath.h,
 *          Core/Math/FBoundingBox.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Math/FMath.h"
#include "Core/Math/FVector.h"
#include "Core/Math/FBoundingBox.h"

namespace Limx
{

// 前向声明 (FPlane, FSphere 在后续头文件定义)
struct FPlane;
struct FSphere;

/// 射线 — 原点 + 方向
struct FRay
{
    FVector3 Origin;     ///< 射线原点
    FVector3 Direction;  ///< 射线方向 (应为单位向量)

    // ========================================================================
    // 构造
    // ========================================================================

    /// 默认构造 — 从原点沿 +Z 方向
    FRay()
        : Origin(0.0f, 0.0f, 0.0f)
        , Direction(0.0f, 0.0f, 1.0f)
    {
    }

    /// 指定原点和方向
    FRay(const FVector3& origin, const FVector3& direction)
        : Origin(origin)
        , Direction(direction)
    {
    }

    // ========================================================================
    // 参数化
    // ========================================================================

    /// 沿射线取点: Origin + t * Direction
    LIMX_NODISCARD FVector3 PointAt(Float32 t) const
    {
        return Origin + Direction * t;
    }

    // ========================================================================
    // 射线-AABB 相交 (Slab 方法)
    // ========================================================================

    /// 射线-AABB 相交测试
    /// @param box     包围盒
    /// @param outTMin 命中时的最近参数 (可选)
    /// @param outTMax 命中时的最远参数 (可选)
    /// @return 是否相交
    LIMX_NODISCARD bool IntersectsAABB(
        const FBoundingBox& box,
        Float32* outTMin = nullptr,
        Float32* outTMax = nullptr) const
    {
        Float32 tMin = 0.0f;
        Float32 tMax = 3.402823466e+38f;

        // X 轴
        if (!SlabTest(Origin.X, Direction.X,
                      box.Min.X, box.Max.X, tMin, tMax))
        {
            return false;
        }

        // Y 轴
        if (!SlabTest(Origin.Y, Direction.Y,
                      box.Min.Y, box.Max.Y, tMin, tMax))
        {
            return false;
        }

        // Z 轴
        if (!SlabTest(Origin.Z, Direction.Z,
                      box.Min.Z, box.Max.Z, tMin, tMax))
        {
            return false;
        }

        if (outTMin) *outTMin = tMin;
        if (outTMax) *outTMax = tMax;
        return true;
    }

    // ========================================================================
    // 射线-球相交
    // ========================================================================

    /// 射线-球相交测试
    /// @param center  球心
    /// @param radius  半径
    /// @param outT    命中时的最近参数 (可选)
    /// @return 是否相交
    LIMX_NODISCARD bool IntersectsSphere(
        const FVector3& center,
        Float32 radius,
        Float32* outT = nullptr) const
    {
        FVector3 oc = Origin - center;
        Float32 a = FVector3::Dot(Direction, Direction);
        Float32 b = 2.0f * FVector3::Dot(oc, Direction);
        Float32 c = FVector3::Dot(oc, oc) - radius * radius;
        Float32 discriminant = b * b - 4.0f * a * c;

        if (discriminant < 0.0f)
        {
            return false;
        }

        Float32 sqrtDisc = FMath::Sqrt(discriminant);
        Float32 invDenom = 1.0f / (2.0f * a);

        // 最近的正参数
        Float32 t0 = (-b - sqrtDisc) * invDenom;
        Float32 t1 = (-b + sqrtDisc) * invDenom;

        if (t0 >= 0.0f)
        {
            if (outT) *outT = t0;
            return true;
        }
        if (t1 >= 0.0f)
        {
            if (outT) *outT = t1;
            return true;
        }

        return false;
    }

    // ========================================================================
    // 射线-平面相交
    // ========================================================================

    /// 射线-平面相交 (法线 + 平面上一点)
    /// @param planeNormal 平面法线 (单位向量)
    /// @param planePoint  平面上任意一点
    /// @param outT        命中参数 (可选)
    /// @return 是否相交 (平行时返回 false)
    LIMX_NODISCARD bool IntersectsPlaneByPoint(
        const FVector3& planeNormal,
        const FVector3& planePoint,
        Float32* outT = nullptr) const
    {
        Float32 denom = FVector3::Dot(planeNormal, Direction);

        // 射线近乎平行于平面
        if (FMath::Abs(denom) < 1.0e-6f)
        {
            return false;
        }

        Float32 t = FVector3::Dot(planePoint - Origin, planeNormal) /
                    denom;

        if (t < 0.0f)
        {
            return false;
        }

        if (outT) *outT = t;
        return true;
    }

    // ========================================================================
    // 最近点
    // ========================================================================

    /// 射线上距给定点最近的点
    LIMX_NODISCARD FVector3 ClosestPoint(const FVector3& point) const
    {
        Float32 t = FVector3::Dot(point - Origin, Direction);
        if (t < 0.0f) t = 0.0f;
        return PointAt(t);
    }

    /// 射线上距给定点最近的参数 t (钳位到 >= 0)
    LIMX_NODISCARD Float32 ClosestParameter(
        const FVector3& point) const
    {
        Float32 t = FVector3::Dot(point - Origin, Direction);
        return t < 0.0f ? 0.0f : t;
    }

    // ========================================================================
    // 比较
    // ========================================================================

    LIMX_NODISCARD bool operator==(const FRay& other) const
    {
        return Origin == other.Origin && Direction == other.Direction;
    }

    LIMX_NODISCARD bool operator!=(const FRay& other) const
    {
        return !(*this == other);
    }

private:
    /// Slab 测试辅助 — 单轴区间裁剪
    static bool SlabTest(Float32 origin, Float32 direction,
                          Float32 slabMin, Float32 slabMax,
                          Float32& tMin, Float32& tMax)
    {
        if (FMath::Abs(direction) < 1.0e-8f)
        {
            // 射线平行于该轴的 slab
            return origin >= slabMin && origin <= slabMax;
        }

        Float32 invDir = 1.0f / direction;
        Float32 t0 = (slabMin - origin) * invDir;
        Float32 t1 = (slabMax - origin) * invDir;

        if (t0 > t1)
        {
            // 交换
            Float32 temp = t0;
            t0 = t1;
            t1 = temp;
        }

        if (t0 > tMin) tMin = t0;
        if (t1 < tMax) tMax = t1;

        return tMin <= tMax;
    }
};

} // namespace Limx
