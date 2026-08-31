/*******************************************************************************
 * 文件: FClusterGrid.h
 * 创建时间: 2026-08-31
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   分簇光照的网格数学 — 切片映射、簇包围盒、球体相交
 *
 * 设计哲学:
 *   与 Shaders/Builtin/cluster_common.h 逐行对应 — 这份 CPU 实现存在的理由
 *     是让分簇的正确性可以脱离 GPU 测。分簇错了的表现是"某些角度下某些光
 *     突然不亮", 那在真实场景里极难复现, 更难归因; 而这套数学的风险全在
 *     边界上 (恰好落在切片边界的深度、恰好擦过簇边界的光球), 那些情况用
 *     数值断言一次就能钉死。
 *
 *     两份实现必须逐行对应。它们不一致时的表现是: CPU 测试全绿, 而画面上
 *     某些簇少算了光。改动任一份都要同步改另一份 —— 这一点没有编译期保障,
 *     只能靠这条注释、用例, 以及 --light-cull-check 的逐像素比对。
 *
 *   网格尺寸固定, 不随分辨率变化 — 这是一个刻意的取舍。随分辨率变化的
 *     网格意味着簇缓冲区要在交换链重建时重新分配, 而描述符集由 FRenderer
 *     持有、Pass 的 OnResize 拿不到描述符句柄 —— 那条路径上"描述符仍指向
 *     已销毁的缓冲区"是个只在 resize 时触发、平时完全看不见的坑。固定网格
 *     把这一整类问题从结构上消掉了。
 *
 *     代价是高分辨率下每个簇覆盖的像素更多, 剔除粒度相对变粗。1280x720 下
 *     每簇 40x40 像素 (刚好), 3840x2160 下是 120x120 (偏粗)。真到 4K 成为
 *     主要场景时, 正确的做法是把网格尺寸做成启动期常量并重建全部簇资源,
 *     而不是让它每帧变化。
 *
 * 技术特性:
 *   - Z 方向按指数切片: 近处切得密, 远处切得疏, 与透视投影的深度分布匹配
 *   - 簇包围盒在**视空间**计算, 与光源位置同一空间, 相交测试不必换算
 *
 * 依赖关系:
 *   内部: Core/Math/FVector.h, Core/Math/FMatrix.h, Core/Math/FMath.h
 *
 ******************************************************************************/

#pragma once

#include "RenderCore/RenderCoreMinimal.h"

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Math/FVector.h"
#include "Core/Math/FMatrix.h"
#include "Core/Math/FMath.h"

namespace Limx
{

// ============================================================================
// 网格尺寸
// ============================================================================

/// 屏幕横向的簇数
///
/// 32x18 恰好是 16:9, 于是 1280x720 下每簇 40x40 像素 —— 方形簇让"簇的
/// 屏幕投影"与"光球的屏幕投影"形状接近, 相交测试的保守程度在两个方向上
/// 一致。非方形簇会让某一个方向系统性地多留余量。
inline constexpr UInt32 kClusterGridX = 32;

/// 屏幕纵向的簇数
inline constexpr UInt32 kClusterGridY = 18;

/// 深度方向的切片数
///
/// 32 片指数分布覆盖 near..far。片数太少则每片跨越的深度范围过大, 远处
/// 的簇会包住整条视锥而剔不掉任何东西; 太多则簇总数上升, 剔除本身的开销
/// 盖过收益。
inline constexpr UInt32 kClusterGridZ = 32;

/// 簇总数
inline constexpr UInt32 kClusterCount =
    kClusterGridX * kClusterGridY * kClusterGridZ;

/// 全局光源索引表的容量 (条目数)
///
/// 平均每簇 32 盏。超出时必须**报出来**而不是静默截断 —— 静默截断的表现
/// 是"画面某些区域的光少了几盏", 那看起来像是衰减参数的问题。
inline constexpr UInt32 kClusterLightIndexCapacity = kClusterCount * 32u;

// ============================================================================
// 深度切片
// ============================================================================

/// 指数切片的映射系数
///
/// 切片下标由 `slice = log2(-z) * Scale + Bias` 给出。取对数是因为透视
/// 投影下深度精度本身就是对数分布的: 线性切片会让近处的几片挤在一起而远
/// 处的一片横跨几十米。
struct FClusterSliceMapping
{
    Float32 Scale = 0.0f;
    Float32 Bias  = 0.0f;
};

/// 由近远平面算出切片映射系数
///
/// @param nearPlane 近平面距离 (正值)
/// @param farPlane  远平面距离 (正值, 必须大于 nearPlane)
LIMX_NODISCARD inline FClusterSliceMapping ComputeSliceMapping(
    Float32 nearPlane, Float32 farPlane)
{
    FClusterSliceMapping mapping;

    // 退化输入直接返回零系数。让 log 去处理非法输入会得到 NaN, 而 NaN 传进
    // 簇下标的计算之后是一个越界的负数, 表现为随机读取索引表 —— 那比"没有
    // 分簇"难查得多。
    if (nearPlane <= 0.0f || farPlane <= nearPlane)
    {
        return mapping;
    }

    const Float32 logRatio =
        FMath::Log2(farPlane) - FMath::Log2(nearPlane);

    mapping.Scale = static_cast<Float32>(kClusterGridZ) / logRatio;
    mapping.Bias  = -static_cast<Float32>(kClusterGridZ) *
                    FMath::Log2(nearPlane) / logRatio;

    return mapping;
}

/// 视空间深度 → 切片下标
///
/// @param viewDepth 视空间深度的绝对值 (正值; 视空间 -Z 为前方, 传 -z)
LIMX_NODISCARD inline UInt32 SliceForViewDepth(
    Float32 viewDepth, const FClusterSliceMapping& mapping)
{
    if (viewDepth <= 0.0f)
    {
        return 0u;
    }

    const Float32 raw = FMath::Log2(viewDepth) * mapping.Scale + mapping.Bias;

    // 钳在 [0, kClusterGridZ-1]。近平面之前与远平面之后的片元都归到边界
    // 那一片 —— 它们本来就不该被绘制, 归到边界比越界读安全。
    if (raw <= 0.0f)
    {
        return 0u;
    }

    const UInt32 slice = static_cast<UInt32>(raw);

    return (slice >= kClusterGridZ) ? (kClusterGridZ - 1u) : slice;
}

/// 切片下标 → 该片的近端深度 (视空间绝对值)
LIMX_NODISCARD inline Float32 SliceNearDepth(
    UInt32 slice, Float32 nearPlane, Float32 farPlane)
{
    if (nearPlane <= 0.0f || farPlane <= nearPlane)
    {
        return nearPlane;
    }

    const Float32 t = static_cast<Float32>(slice) /
                      static_cast<Float32>(kClusterGridZ);

    return nearPlane * FMath::Pow(farPlane / nearPlane, t);
}

// ============================================================================
// 簇包围盒
// ============================================================================

/// 视空间轴对齐包围盒
struct FClusterBounds
{
    FVector3 Min;
    FVector3 Max;
};

/// 把 NDC 的一点反投影到视空间
///
/// 只取方向, 不关心长度 —— 调用方随后按深度缩放。
LIMX_NODISCARD inline FVector3 UnprojectToViewRay(
    Float32 ndcX, Float32 ndcY, const FMatrix& inverseProjection)
{
    // z 取 1 (Vulkan NDC 的远平面)。取 0 或 1 都能定出同一条射线, 但远平面
    // 那个点的 w 更大, 除法的相对误差更小。
    const FVector4 clip(ndcX, ndcY, 1.0f, 1.0f);
    const FVector4 view = inverseProjection.TransformVector4(clip);

    if (FMath::Abs(view.W) < 1.0e-9f)
    {
        return FVector3(0.0f, 0.0f, -1.0f);
    }

    return FVector3(view.X / view.W, view.Y / view.W, view.Z / view.W);
}

/// 计算一个簇在视空间的包围盒
///
/// @param clusterX/Y/Z 簇在网格中的三维下标
/// @param inverseProjection 投影矩阵的逆 (**必须是未抖动的那一个**)
/// @param nearPlane/farPlane 相机近远平面
///
/// 用未抖动的投影是必须的: 抖动每帧改变亚像素偏移, 用它算出的簇边界会逐
/// 帧漂移不到一个像素 —— 那本身无害, 但会让"分簇结果与暴力法一致"这条
/// 验收判据变成逐帧不同, 无法比对。
LIMX_NODISCARD inline FClusterBounds ComputeClusterBounds(
    UInt32 clusterX, UInt32 clusterY, UInt32 clusterZ,
    const FMatrix& inverseProjection,
    Float32 nearPlane, Float32 farPlane)
{
    // 簇在 NDC 的横纵范围。Vulkan 的 NDC: x,y ∈ [-1,1], y 向下。
    const Float32 minNdcX =
        static_cast<Float32>(clusterX) /
            static_cast<Float32>(kClusterGridX) * 2.0f - 1.0f;
    const Float32 maxNdcX =
        static_cast<Float32>(clusterX + 1u) /
            static_cast<Float32>(kClusterGridX) * 2.0f - 1.0f;
    const Float32 minNdcY =
        static_cast<Float32>(clusterY) /
            static_cast<Float32>(kClusterGridY) * 2.0f - 1.0f;
    const Float32 maxNdcY =
        static_cast<Float32>(clusterY + 1u) /
            static_cast<Float32>(kClusterGridY) * 2.0f - 1.0f;

    // 四个角的视空间射线方向
    const FVector3 rays[4] =
    {
        UnprojectToViewRay(minNdcX, minNdcY, inverseProjection),
        UnprojectToViewRay(maxNdcX, minNdcY, inverseProjection),
        UnprojectToViewRay(minNdcX, maxNdcY, inverseProjection),
        UnprojectToViewRay(maxNdcX, maxNdcY, inverseProjection),
    };

    const Float32 nearDepth = SliceNearDepth(clusterZ, nearPlane, farPlane);
    const Float32 farDepth  = SliceNearDepth(clusterZ + 1u, nearPlane, farPlane);

    FClusterBounds bounds;

    bool first = true;

    // 八个角: 四条射线各与近端平面、远端平面求交。
    //
    // 射线是从原点出发的, 方向的 z 分量为负 (视空间 -Z 为前方)。要落在
    // 深度 d 处, 缩放系数是 d / (-ray.Z)。
    for (UInt32 i = 0; i < 4; ++i)
    {
        const Float32 rayZ = -rays[i].Z;

        if (rayZ < 1.0e-9f)
        {
            continue;
        }

        const Float32 depths[2] = { nearDepth, farDepth };

        for (UInt32 d = 0; d < 2; ++d)
        {
            const Float32 t = depths[d] / rayZ;

            const FVector3 corner(rays[i].X * t, rays[i].Y * t, -depths[d]);

            if (first)
            {
                bounds.Min = corner;
                bounds.Max = corner;
                first      = false;
            }
            else
            {
                bounds.Min.X = FMath::Min(bounds.Min.X, corner.X);
                bounds.Min.Y = FMath::Min(bounds.Min.Y, corner.Y);
                bounds.Min.Z = FMath::Min(bounds.Min.Z, corner.Z);
                bounds.Max.X = FMath::Max(bounds.Max.X, corner.X);
                bounds.Max.Y = FMath::Max(bounds.Max.Y, corner.Y);
                bounds.Max.Z = FMath::Max(bounds.Max.Z, corner.Z);
            }
        }
    }

    if (first)
    {
        // 一条有效射线都没有 —— 逆投影矩阵不对。给一个退化的空盒, 它与
        // 任何球都不相交, 于是这个簇不会拿到光源。
        bounds.Min = FVector3(0.0f, 0.0f, 0.0f);
        bounds.Max = FVector3(0.0f, 0.0f, 0.0f);
    }

    return bounds;
}

// ============================================================================
// 相交测试
// ============================================================================

/// 球与轴对齐包围盒是否相交
///
/// 判据是"盒内离球心最近的点到球心的距离是否不超过半径"。这是精确判据,
/// 不是保守近似 —— 用盒心到球心的距离减去盒的半对角线那种写法会漏掉贴着
/// 盒角的球, 而漏掉的后果是那个簇少算一盏光。
LIMX_NODISCARD inline bool SphereIntersectsAABB(
    const FVector3& center, Float32 radius,
    const FVector3& boundsMin, const FVector3& boundsMax)
{
    const Float32 closestX = FMath::Clamp(center.X, boundsMin.X, boundsMax.X);
    const Float32 closestY = FMath::Clamp(center.Y, boundsMin.Y, boundsMax.Y);
    const Float32 closestZ = FMath::Clamp(center.Z, boundsMin.Z, boundsMax.Z);

    const Float32 dx = center.X - closestX;
    const Float32 dy = center.Y - closestY;
    const Float32 dz = center.Z - closestZ;

    return (dx * dx + dy * dy + dz * dz) <= (radius * radius);
}

/// 三维簇下标 → 线性下标
///
/// 排布顺序是 x 变化最快、z 最慢。片段着色器按这个顺序查表, 计算着色器
/// 按这个顺序写表 —— 两边不一致的表现是光照整体错位到别的屏幕区域, 而
/// 画面依然"有光", 看起来像是光源位置摆错了。
LIMX_NODISCARD inline UInt32 ClusterLinearIndex(
    UInt32 clusterX, UInt32 clusterY, UInt32 clusterZ)
{
    return clusterX +
           clusterY * kClusterGridX +
           clusterZ * kClusterGridX * kClusterGridY;
}

} // namespace Limx
