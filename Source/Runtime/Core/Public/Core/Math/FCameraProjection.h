/*******************************************************************************
 * 文件: FCameraProjection.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   相机投影 — 透视/正交投影矩阵参数化封装
 *   统一管理 FOV/Near/Far 等参数，生成 4x4 投影矩阵
 *   用于渲染相机设置、阴影投影、反射投影等场景
 *
 * 设计哲学:
 *   参数封装 — 以语义化字段存储投影参数，而非直接矩阵
 *   延迟计算 — 参数变更后调用 BuildMatrix() 才更新矩阵
 *   Vulkan 约定 — NDC Z 在 [0,1]，Y 轴向下
 *
 * 技术特性:
 *   - FPerspectiveProjection: 透视投影参数 + 矩阵构建
 *   - FOrthographicProjection: 正交投影参数 + 矩阵构建
 *   - BuildMatrix: 生成 Vulkan NDC 投影矩阵
 *   - GetFrustumCorners: 获取视锥体角点
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Math/FMath.h,
 *          Core/Math/FMatrix.h, Core/Math/FVector.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Math/FMath.h"
#include "Core/Math/FMatrix.h"
#include "Core/Math/FVector.h"

namespace Limx
{

/// 透视投影参数
struct FPerspectiveProjection
{
    Float32 FovYRadians;  ///< 垂直视场角 (弧度)
    Float32 AspectRatio;  ///< 宽高比 (宽/高)
    Float32 NearPlane;    ///< 近裁剪面距离
    Float32 FarPlane;     ///< 远裁剪面距离 (0 = 无限远)

    // ========================================================================
    // 构造
    // ========================================================================

    FPerspectiveProjection()
        : FovYRadians(FMath::kPi / 4.0f)
        , AspectRatio(16.0f / 9.0f)
        , NearPlane(0.1f)
        , FarPlane(10000.0f)
    {
    }

    FPerspectiveProjection(Float32 fovYRadians,
                            Float32 aspectRatio,
                            Float32 nearPlane,
                            Float32 farPlane)
        : FovYRadians(fovYRadians)
        , AspectRatio(aspectRatio)
        , NearPlane(nearPlane)
        , FarPlane(farPlane)
    {
    }

    // ========================================================================
    // 工厂
    // ========================================================================

    LIMX_NODISCARD static FPerspectiveProjection FromDegrees(
        Float32 fovYDegrees, Float32 aspectRatio,
        Float32 nearPlane, Float32 farPlane)
    {
        return FPerspectiveProjection(
            fovYDegrees * FMath::kDegToRad,
            aspectRatio, nearPlane, farPlane);
    }

    // ========================================================================
    // 矩阵构建 (Vulkan NDC: Z=[0,1], Y向下翻转)
    // ========================================================================

    LIMX_NODISCARD FMatrix BuildMatrix() const
    {
        Float32 tanHalfFovY = FMath::Tan(
            FovYRadians * 0.5f);

        FMatrix result;

        result.M[0][0] = 1.0f / (AspectRatio * tanHalfFovY);
        result.M[1][1] = -1.0f / tanHalfFovY; // Vulkan Y 翻转
        result.M[2][3] = -1.0f;
        result.M[3][3] = 0.0f;

        if (FarPlane <= 0.0f)
        {
            // 无限远裁剪面 (Reversed-Z)
            result.M[2][2] = 0.0f;
            result.M[3][2] = NearPlane;
        }
        else
        {
            Float32 range = FarPlane / (NearPlane - FarPlane);
            result.M[2][2] = range;
            result.M[3][2] = range * NearPlane;
        }

        return result;
    }

    // ========================================================================
    // 属性
    // ========================================================================

    /// 水平视场角 (弧度)
    LIMX_NODISCARD Float32 GetFovXRadians() const
    {
        return 2.0f * FMath::ATan(
            FMath::Tan(FovYRadians * 0.5f) * AspectRatio);
    }

    /// 近平面高度
    LIMX_NODISCARD Float32 GetNearHeight() const
    {
        return 2.0f * NearPlane *
               FMath::Tan(FovYRadians * 0.5f);
    }

    /// 近平面宽度
    LIMX_NODISCARD Float32 GetNearWidth() const
    {
        return GetNearHeight() * AspectRatio;
    }
};

/// 正交投影参数
struct FOrthographicProjection
{
    Float32 Left;    ///< 左裁剪面
    Float32 Right;   ///< 右裁剪面
    Float32 Bottom;  ///< 下裁剪面
    Float32 Top;     ///< 上裁剪面
    Float32 Near;    ///< 近裁剪面
    Float32 Far;     ///< 远裁剪面

    // ========================================================================
    // 构造
    // ========================================================================

    FOrthographicProjection()
        : Left(-1.0f), Right(1.0f)
        , Bottom(-1.0f), Top(1.0f)
        , Near(0.0f), Far(100.0f)
    {
    }

    FOrthographicProjection(Float32 left, Float32 right,
                             Float32 bottom, Float32 top,
                             Float32 nearPlane, Float32 farPlane)
        : Left(left), Right(right)
        , Bottom(bottom), Top(top)
        , Near(nearPlane), Far(farPlane)
    {
    }

    /// 从宽高对称构造
    LIMX_NODISCARD static FOrthographicProjection
    Symmetric(Float32 halfWidth, Float32 halfHeight,
              Float32 nearPlane, Float32 farPlane)
    {
        return FOrthographicProjection(
            -halfWidth, halfWidth,
            -halfHeight, halfHeight,
            nearPlane, farPlane);
    }

    // ========================================================================
    // 矩阵构建 (Vulkan NDC: Z=[0,1], Y向下翻转)
    // ========================================================================

    LIMX_NODISCARD FMatrix BuildMatrix() const
    {
        Float32 rml = Right - Left;
        Float32 tmb = Top - Bottom;
        Float32 fmn = Far - Near;

        LIMX_ASSERT(rml > 1e-8f && tmb > 1e-8f && fmn > 1e-8f);

        FMatrix result;

        result.M[0][0] = 2.0f / rml;
        result.M[1][1] = -2.0f / tmb; // Vulkan Y 翻转
        result.M[2][2] = -1.0f / fmn;

        result.M[3][0] = -(Right + Left) / rml;
        result.M[3][1] = -(Top + Bottom) / tmb;
        result.M[3][2] = -Near / fmn;
        result.M[3][3] = 1.0f;

        return result;
    }

    // ========================================================================
    // 属性
    // ========================================================================

    LIMX_NODISCARD Float32 GetWidth() const
    {
        return Right - Left;
    }

    LIMX_NODISCARD Float32 GetHeight() const
    {
        return Top - Bottom;
    }

    LIMX_NODISCARD Float32 GetDepth() const
    {
        return Far - Near;
    }

    LIMX_NODISCARD FVector2 GetCenter() const
    {
        return FVector2(
            (Left + Right) * 0.5f,
            (Bottom + Top) * 0.5f);
    }
};

} // namespace Limx
