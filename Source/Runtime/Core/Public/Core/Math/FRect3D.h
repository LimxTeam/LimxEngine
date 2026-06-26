/*******************************************************************************
 * 文件: FRect3D.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   3D 有向矩形区域 — OBB 风格的方向性 3D 矩形面片
 *   以中心点、法线、U/V 轴和半尺寸表示空间中的矩形区域
 *   用于区域光源几何形状、Portal、贴花区域、3D 切割平面等场景
 *
 * 设计哲学:
 *   中心+轴表示 — 比顶点列表更紧凑，旋转变换更高效
 *   法线=U×V — 法线由两轴叉积决定，保证正交性
 *   值类型 — 轻量可拷贝
 *
 * 技术特性:
 *   - FRect3D: 3D 有向矩形
 *   - GetCorners: 获取四个角点
 *   - ContainsPoint: 点包含 (投影到矩形平面)
 *   - GetNormal: 获取法线方向
 *   - GetArea: 面积
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

/// 3D 有向矩形
struct FRect3D
{
    FVector3 Center;    ///< 矩形中心
    FVector3 AxisU;     ///< U 轴方向 (单位向量)
    FVector3 AxisV;     ///< V 轴方向 (单位向量, 须与 AxisU 正交)
    FVector2 HalfSize;  ///< 半尺寸 (U 方向, V 方向)

    // ========================================================================
    // 构造
    // ========================================================================

    FRect3D()
        : Center(0.0f, 0.0f, 0.0f)
        , AxisU(1.0f, 0.0f, 0.0f)
        , AxisV(0.0f, 1.0f, 0.0f)
        , HalfSize(0.5f, 0.5f)
    {
    }

    FRect3D(const FVector3& center,
            const FVector3& axisU,
            const FVector3& axisV,
            const FVector2& halfSize)
        : Center(center)
        , AxisU(axisU)
        , AxisV(axisV)
        , HalfSize(halfSize)
    {
    }

    // ========================================================================
    // 属性
    // ========================================================================

    /// 法线 (AxisU × AxisV)
    LIMX_NODISCARD FVector3 GetNormal() const
    {
        return FVector3::Cross(AxisU, AxisV).GetSafeNormal();
    }

    /// 面积
    LIMX_NODISCARD Float32 GetArea() const
    {
        return 4.0f * HalfSize.X * HalfSize.Y;
    }

    /// 对角线长度
    LIMX_NODISCARD Float32 GetDiagonalLength() const
    {
        return 2.0f * FMath::Sqrt(
            HalfSize.X * HalfSize.X +
            HalfSize.Y * HalfSize.Y);
    }

    // ========================================================================
    // 几何操作
    // ========================================================================

    /// 获取四个角点 (逆时针顺序, 从面法线正方向看)
    void GetCorners(FVector3 outCorners[4]) const
    {
        FVector3 uOffset = AxisU * HalfSize.X;
        FVector3 vOffset = AxisV * HalfSize.Y;

        outCorners[0] = Center - uOffset - vOffset;
        outCorners[1] = Center + uOffset - vOffset;
        outCorners[2] = Center + uOffset + vOffset;
        outCorners[3] = Center - uOffset + vOffset;
    }

    /// 点到矩形平面的距离 (带符号)
    LIMX_NODISCARD Float32 SignedDistanceTo(
        const FVector3& point) const
    {
        return FVector3::Dot(
            GetNormal(), point - Center);
    }

    /// 点投影到矩形面后的局部 UV 坐标
    LIMX_NODISCARD FVector2 ProjectPoint(
        const FVector3& point) const
    {
        FVector3 delta = point - Center;
        return FVector2(
            FVector3::Dot(delta, AxisU),
            FVector3::Dot(delta, AxisV));
    }

    /// 点是否在矩形区域内 (投影后检查 UV 范围)
    LIMX_NODISCARD bool ContainsPoint(
        const FVector3& point,
        Float32 tolerance = 1e-4f) const
    {
        FVector2 uv = ProjectPoint(point);
        return FMath::Abs(uv.X) <=
                   HalfSize.X + tolerance &&
               FMath::Abs(uv.Y) <=
                   HalfSize.Y + tolerance;
    }

    /// 将 UV 坐标转换回世界空间点
    LIMX_NODISCARD FVector3 UVToWorld(
        Float32 u, Float32 v) const
    {
        return Center + AxisU * u + AxisV * v;
    }

    // ========================================================================
    // 变换
    // ========================================================================

    /// 平移
    LIMX_NODISCARD FRect3D Translate(
        const FVector3& offset) const
    {
        return FRect3D(Center + offset, AxisU, AxisV,
                       HalfSize);
    }

    /// 均匀缩放
    LIMX_NODISCARD FRect3D Scale(Float32 factor) const
    {
        return FRect3D(Center, AxisU, AxisV,
                       HalfSize * factor);
    }
};

} // namespace Limx
