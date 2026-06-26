/*******************************************************************************
 * 文件: FFrustum.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   视锥体 — 由 6 个平面围成的截锥体积
 *   支持点/球/AABB 的可见性测试
 *   用于视锥裁剪、遮挡查询、LOD 选择等渲染管线关键路径
 *
 * 设计哲学:
 *   6 平面表示 — 近/远/左/右/上/下 六个裁剪面
 *   从 VP 矩阵提取 — 直接从视图投影矩阵反向提取平面方程
 *   早退优化 — 任一平面裁掉即可立即返回不可见
 *
 * 技术特性:
 *   - FromViewProjection: 从 VP 矩阵提取 6 平面
 *   - TestPoint: 点可见性
 *   - TestSphere: 球可见性 (完全可见/部分可见/不可见)
 *   - TestAABB: AABB 可见性
 *
 * 依赖关系:
 *   内部: Core/Math/FPlane.h, Core/Math/FSphere.h,
 *          Core/Math/FBoundingBox.h, Core/Math/FMatrix.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Math/FPlane.h"
#include "Core/Math/FSphere.h"
#include "Core/Math/FBoundingBox.h"
#include "Core/Math/FMatrix.h"

namespace Limx
{

/// 可见性测试结果
enum class FrustumResult : UInt8
{
    Outside,   ///< 完全在视锥外
    Inside,    ///< 完全在视锥内
    Intersect  ///< 部分相交
};

/// 视锥平面索引
enum class FrustumPlane : UInt8
{
    Left   = 0,
    Right  = 1,
    Bottom = 2,
    Top    = 3,
    Near   = 4,
    Far    = 5
};

/// 视锥体 — 6 平面裁剪体
struct FFrustum
{
    static constexpr Int32 kPlaneCount = 6;
    FPlane Planes[kPlaneCount];

    // ========================================================================
    // 构造
    // ========================================================================

    FFrustum() = default;

    /// 从视图-投影矩阵提取 6 个裁剪平面 (Gribb/Hartmann 方法)
    /// @param viewProjection 视图矩阵 × 投影矩阵 (行主序)
    LIMX_NODISCARD static FFrustum FromViewProjection(
        const FMatrix& viewProjection)
    {
        FFrustum frustum;

        // 行主序矩阵: M[row][col]
        // 左平面:   row3 + row0
        frustum.Planes[0] = ExtractPlane(
            viewProjection, 3, 0, 1.0f);
        // 右平面:   row3 - row0
        frustum.Planes[1] = ExtractPlane(
            viewProjection, 3, 0, -1.0f);
        // 下平面:   row3 + row1
        frustum.Planes[2] = ExtractPlane(
            viewProjection, 3, 1, 1.0f);
        // 上平面:   row3 - row1
        frustum.Planes[3] = ExtractPlane(
            viewProjection, 3, 1, -1.0f);
        // 近平面:   row3 + row2
        frustum.Planes[4] = ExtractPlane(
            viewProjection, 3, 2, 1.0f);
        // 远平面:   row3 - row2
        frustum.Planes[5] = ExtractPlane(
            viewProjection, 3, 2, -1.0f);

        // 归一化所有平面
        for (Int32 index = 0; index < kPlaneCount; ++index)
        {
            frustum.Planes[index].Normalize();
        }

        return frustum;
    }

    // ========================================================================
    // 可见性测试
    // ========================================================================

    /// 点可见性测试
    LIMX_NODISCARD bool TestPoint(const FVector3& point) const
    {
        for (Int32 index = 0; index < kPlaneCount; ++index)
        {
            if (Planes[index].SignedDistance(point) < 0.0f)
            {
                return false;
            }
        }
        return true;
    }

    /// 球可见性测试
    LIMX_NODISCARD FrustumResult TestSphere(const FSphere& sphere) const
    {
        bool allInside = true;

        for (Int32 index = 0; index < kPlaneCount; ++index)
        {
            Float32 dist =
                Planes[index].SignedDistance(sphere.Center);

            if (dist < -sphere.Radius)
            {
                return FrustumResult::Outside;
            }

            if (dist < sphere.Radius)
            {
                allInside = false;
            }
        }

        return allInside ? FrustumResult::Inside
                         : FrustumResult::Intersect;
    }

    /// 球快速可见性 (仅判断是否可能可见)
    LIMX_NODISCARD bool IsSphereVisible(const FSphere& sphere) const
    {
        for (Int32 index = 0; index < kPlaneCount; ++index)
        {
            if (Planes[index].SignedDistance(sphere.Center)
                < -sphere.Radius)
            {
                return false;
            }
        }
        return true;
    }

    /// AABB 可见性测试
    LIMX_NODISCARD FrustumResult TestAABB(
        const FBoundingBox& box) const
    {
        bool allInside = true;

        for (Int32 index = 0; index < kPlaneCount; ++index)
        {
            // 找到 AABB 上距平面最远的正点 (p-vertex)
            // 和最近的负点 (n-vertex)
            FVector3 positiveVertex = box.Min;
            FVector3 negativeVertex = box.Max;

            if (Planes[index].Normal.X >= 0.0f)
            {
                positiveVertex.X = box.Max.X;
                negativeVertex.X = box.Min.X;
            }
            if (Planes[index].Normal.Y >= 0.0f)
            {
                positiveVertex.Y = box.Max.Y;
                negativeVertex.Y = box.Min.Y;
            }
            if (Planes[index].Normal.Z >= 0.0f)
            {
                positiveVertex.Z = box.Max.Z;
                negativeVertex.Z = box.Min.Z;
            }

            // p-vertex 在负侧 → 完全在外
            if (Planes[index].SignedDistance(positiveVertex) < 0.0f)
            {
                return FrustumResult::Outside;
            }

            // n-vertex 在负侧 → 部分相交
            if (Planes[index].SignedDistance(negativeVertex) < 0.0f)
            {
                allInside = false;
            }
        }

        return allInside ? FrustumResult::Inside
                         : FrustumResult::Intersect;
    }

    /// AABB 快速可见性
    LIMX_NODISCARD bool IsAABBVisible(const FBoundingBox& box) const
    {
        return TestAABB(box) != FrustumResult::Outside;
    }

    /// 获取指定平面
    LIMX_NODISCARD const FPlane& GetPlane(FrustumPlane plane) const
    {
        return Planes[static_cast<UInt8>(plane)];
    }

private:
    /// 从矩阵提取平面辅助
    /// rowA ± rowB 的组合
    static FPlane ExtractPlane(const FMatrix& m,
                                Int32 rowA, Int32 rowB,
                                Float32 sign)
    {
        FVector3 normal(
            m.M[rowA][0] + sign * m.M[rowB][0],
            m.M[rowA][1] + sign * m.M[rowB][1],
            m.M[rowA][2] + sign * m.M[rowB][2]);
        Float32 d = m.M[rowA][3] + sign * m.M[rowB][3];
        return FPlane(normal, d);
    }
};

} // namespace Limx
