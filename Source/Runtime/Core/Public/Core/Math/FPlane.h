/*******************************************************************************
 * 文件: FPlane.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   3D 平面 — 由法线和到原点的有符号距离定义
 *   方程: Dot(Normal, P) + Distance = 0
 *   支持半空间测试、点到平面距离、平面变换等操作
 *   用于视锥裁剪、BSP 分割、碰撞检测、反射计算等场景
 *
 * 设计哲学:
 *   法线+距离表示 — Normal (单位向量) + D (有符号距离)
 *   值语义 — 16 字节 POD 类型
 *   右手系约定 — 法线正方向为"前方" (正半空间)
 *
 * 技术特性:
 *   - FromPointNormal: 从点和法线构造
 *   - FromThreePoints: 从三个共面点构造
 *   - SignedDistance: 点到平面的有符号距离
 *   - ClassifyPoint: 点在平面哪一侧 (前/后/上)
 *   - ProjectPoint: 将点投影到平面上
 *   - Normalize: 归一化平面 (使法线为单位向量)
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

/// 点相对于平面的分类
enum class PlaneClassification : UInt8
{
    Front,   ///< 正半空间 (法线方向)
    Back,    ///< 负半空间 (法线反方向)
    OnPlane  ///< 在平面上
};

/// 3D 平面 — Normal · P + D = 0
struct FPlane
{
    FVector3 Normal;   ///< 平面法线 (应为单位向量)
    Float32  D;        ///< 到原点的有符号距离 (负值表示原点在正半空间)

    // ========================================================================
    // 构造
    // ========================================================================

    /// 默认构造 — XY 平面 (法线 +Z)
    FPlane()
        : Normal(0.0f, 0.0f, 1.0f)
        , D(0.0f)
    {
    }

    /// 直接指定法线和距离
    FPlane(const FVector3& normal, Float32 distance)
        : Normal(normal)
        , D(distance)
    {
    }

    /// 从法线和平面上一点构造
    LIMX_NODISCARD static FPlane FromPointNormal(
        const FVector3& point, const FVector3& normal)
    {
        return FPlane(normal, -FVector3::Dot(normal, point));
    }

    /// 从三个点构造 (逆时针绕序确定法线方向)
    LIMX_NODISCARD static FPlane FromThreePoints(
        const FVector3& a, const FVector3& b, const FVector3& c)
    {
        FVector3 normal = FVector3::Cross(b - a, c - a).GetSafeNormal();
        return FromPointNormal(a, normal);
    }

    // ========================================================================
    // 距离与分类
    // ========================================================================

    /// 点到平面的有符号距离
    /// 正值 = 正半空间 (法线方向), 负值 = 负半空间
    LIMX_NODISCARD Float32 SignedDistance(const FVector3& point) const
    {
        return FVector3::Dot(Normal, point) + D;
    }

    /// 点到平面的绝对距离
    LIMX_NODISCARD Float32 AbsDistance(const FVector3& point) const
    {
        return FMath::Abs(SignedDistance(point));
    }

    /// 分类点相对于平面的位置
    LIMX_NODISCARD PlaneClassification ClassifyPoint(
        const FVector3& point, Float32 epsilon = 1.0e-4f) const
    {
        Float32 dist = SignedDistance(point);
        if (dist > epsilon)
        {
            return PlaneClassification::Front;
        }
        if (dist < -epsilon)
        {
            return PlaneClassification::Back;
        }
        return PlaneClassification::OnPlane;
    }

    // ========================================================================
    // 投影与反射
    // ========================================================================

    /// 将点投影到平面上
    LIMX_NODISCARD FVector3 ProjectPoint(const FVector3& point) const
    {
        Float32 dist = SignedDistance(point);
        return point - Normal * dist;
    }

    /// 反射向量 (相对于平面法线)
    LIMX_NODISCARD FVector3 ReflectVector(
        const FVector3& vector) const
    {
        return vector - Normal * (2.0f * FVector3::Dot(Normal, vector));
    }

    /// 反射点 (相对于平面)
    LIMX_NODISCARD FVector3 ReflectPoint(const FVector3& point) const
    {
        Float32 dist = SignedDistance(point);
        return point - Normal * (2.0f * dist);
    }

    // ========================================================================
    // 归一化
    // ========================================================================

    /// 归一化平面 (使法线为单位向量)
    LIMX_NODISCARD FPlane GetNormalized() const
    {
        Float32 length = Normal.Length();
        if (length < 1.0e-8f)
        {
            return *this;
        }
        Float32 invLength = 1.0f / length;
        return FPlane(Normal * invLength, D * invLength);
    }

    /// 原地归一化
    void Normalize()
    {
        Float32 length = Normal.Length();
        if (length >= 1.0e-8f)
        {
            Float32 invLength = 1.0f / length;
            Normal = Normal * invLength;
            D *= invLength;
        }
    }

    // ========================================================================
    // 翻转
    // ========================================================================

    /// 翻转平面 (法线反向)
    LIMX_NODISCARD FPlane GetFlipped() const
    {
        return FPlane(Normal * -1.0f, -D);
    }

    // ========================================================================
    // 比较
    // ========================================================================

    LIMX_NODISCARD bool operator==(const FPlane& other) const
    {
        return Normal == other.Normal &&
               FMath::Abs(D - other.D) < 1.0e-6f;
    }

    LIMX_NODISCARD bool operator!=(const FPlane& other) const
    {
        return !(*this == other);
    }
};

} // namespace Limx
