/*******************************************************************************
 * 文件: FEasing.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   缓动函数 — 动画插值曲线库
 *   提供 30+ 标准缓动函数 (Ease In/Out/InOut)
 *   用于 UI 动画、相机运动、粒子效果、属性过渡等场景
 *
 * 设计哲学:
 *   纯数学 — 所有函数为 constexpr 静态方法，零状态
 *   归一化输入 — 参数 t∈[0,1]，输出通常也在 [0,1] 范围
 *   Robert Penner — 基于经典 Robert Penner 缓动方程
 *
 * 技术特性:
 *   - Linear, Quad, Cubic, Quart, Quint: 多项式缓动
 *   - Sine, Expo, Circ: 三角/指数/圆形缓动
 *   - Back, Elastic, Bounce: 回弹/弹性/弹跳缓动
 *   - 每种均提供 EaseIn/EaseOut/EaseInOut 三个变体
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Math/FMath.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Math/FMath.h"

namespace Limx
{

/// 缓动函数库
struct FEasing
{
    // ========================================================================
    // Linear
    // ========================================================================

    LIMX_NODISCARD static Float32 Linear(Float32 t)
    {
        return t;
    }

    // ========================================================================
    // Quad (t^2)
    // ========================================================================

    LIMX_NODISCARD static Float32 EaseInQuad(Float32 t)
    {
        return t * t;
    }

    LIMX_NODISCARD static Float32 EaseOutQuad(Float32 t)
    {
        return t * (2.0f - t);
    }

    LIMX_NODISCARD static Float32 EaseInOutQuad(Float32 t)
    {
        if (t < 0.5f)
            return 2.0f * t * t;
        return -1.0f + (4.0f - 2.0f * t) * t;
    }

    // ========================================================================
    // Cubic (t^3)
    // ========================================================================

    LIMX_NODISCARD static Float32 EaseInCubic(Float32 t)
    {
        return t * t * t;
    }

    LIMX_NODISCARD static Float32 EaseOutCubic(Float32 t)
    {
        Float32 f = t - 1.0f;
        return f * f * f + 1.0f;
    }

    LIMX_NODISCARD static Float32 EaseInOutCubic(Float32 t)
    {
        if (t < 0.5f)
            return 4.0f * t * t * t;
        Float32 f = 2.0f * t - 2.0f;
        return 0.5f * f * f * f + 1.0f;
    }

    // ========================================================================
    // Quart (t^4)
    // ========================================================================

    LIMX_NODISCARD static Float32 EaseInQuart(Float32 t)
    {
        return t * t * t * t;
    }

    LIMX_NODISCARD static Float32 EaseOutQuart(Float32 t)
    {
        Float32 f = t - 1.0f;
        return 1.0f - f * f * f * f;
    }

    LIMX_NODISCARD static Float32 EaseInOutQuart(Float32 t)
    {
        if (t < 0.5f)
            return 8.0f * t * t * t * t;
        Float32 f = t - 1.0f;
        return 1.0f - 8.0f * f * f * f * f;
    }

    // ========================================================================
    // Quint (t^5)
    // ========================================================================

    LIMX_NODISCARD static Float32 EaseInQuint(Float32 t)
    {
        return t * t * t * t * t;
    }

    LIMX_NODISCARD static Float32 EaseOutQuint(Float32 t)
    {
        Float32 f = t - 1.0f;
        return f * f * f * f * f + 1.0f;
    }

    LIMX_NODISCARD static Float32 EaseInOutQuint(Float32 t)
    {
        if (t < 0.5f)
            return 16.0f * t * t * t * t * t;
        Float32 f = 2.0f * t - 2.0f;
        return 0.5f * f * f * f * f * f + 1.0f;
    }

    // ========================================================================
    // Sine
    // ========================================================================

    LIMX_NODISCARD static Float32 EaseInSine(Float32 t)
    {
        return 1.0f - FMath::Cos(t * FMath::kHalfPi);
    }

    LIMX_NODISCARD static Float32 EaseOutSine(Float32 t)
    {
        return FMath::Sin(t * FMath::kHalfPi);
    }

    LIMX_NODISCARD static Float32 EaseInOutSine(Float32 t)
    {
        return 0.5f * (1.0f - FMath::Cos(FMath::kPi * t));
    }

    // ========================================================================
    // Expo
    // ========================================================================

    LIMX_NODISCARD static Float32 EaseInExpo(Float32 t)
    {
        if (t <= 0.0f) return 0.0f;
        return FMath::Pow(2.0f, 10.0f * (t - 1.0f));
    }

    LIMX_NODISCARD static Float32 EaseOutExpo(Float32 t)
    {
        if (t >= 1.0f) return 1.0f;
        return 1.0f - FMath::Pow(2.0f, -10.0f * t);
    }

    LIMX_NODISCARD static Float32 EaseInOutExpo(Float32 t)
    {
        if (t <= 0.0f) return 0.0f;
        if (t >= 1.0f) return 1.0f;
        if (t < 0.5f)
            return 0.5f * FMath::Pow(2.0f, 20.0f * t - 10.0f);
        return 1.0f - 0.5f * FMath::Pow(
            2.0f, -20.0f * t + 10.0f);
    }

    // ========================================================================
    // Circ
    // ========================================================================

    LIMX_NODISCARD static Float32 EaseInCirc(Float32 t)
    {
        return 1.0f - FMath::Sqrt(1.0f - t * t);
    }

    LIMX_NODISCARD static Float32 EaseOutCirc(Float32 t)
    {
        Float32 f = t - 1.0f;
        return FMath::Sqrt(1.0f - f * f);
    }

    LIMX_NODISCARD static Float32 EaseInOutCirc(Float32 t)
    {
        if (t < 0.5f)
            return 0.5f * (1.0f - FMath::Sqrt(
                1.0f - 4.0f * t * t));
        Float32 f = 2.0f * t - 2.0f;
        return 0.5f * (FMath::Sqrt(1.0f - f * f) + 1.0f);
    }

    // ========================================================================
    // Back (超调回弹)
    // ========================================================================

    LIMX_NODISCARD static Float32 EaseInBack(Float32 t)
    {
        constexpr Float32 kS = 1.70158f;
        return t * t * ((kS + 1.0f) * t - kS);
    }

    LIMX_NODISCARD static Float32 EaseOutBack(Float32 t)
    {
        constexpr Float32 kS = 1.70158f;
        Float32 f = t - 1.0f;
        return f * f * ((kS + 1.0f) * f + kS) + 1.0f;
    }

    LIMX_NODISCARD static Float32 EaseInOutBack(Float32 t)
    {
        constexpr Float32 kS = 1.70158f * 1.525f;
        if (t < 0.5f)
        {
            Float32 f = 2.0f * t;
            return 0.5f * f * f * ((kS + 1.0f) * f - kS);
        }
        Float32 f = 2.0f * t - 2.0f;
        return 0.5f * (f * f * ((kS + 1.0f) * f + kS) + 2.0f);
    }

    // ========================================================================
    // Elastic (弹性)
    // ========================================================================

    LIMX_NODISCARD static Float32 EaseInElastic(Float32 t)
    {
        if (t <= 0.0f) return 0.0f;
        if (t >= 1.0f) return 1.0f;
        return -FMath::Pow(2.0f, 10.0f * t - 10.0f) *
            FMath::Sin((10.0f * t - 10.75f) *
                       (2.0f * FMath::kPi / 3.0f));
    }

    LIMX_NODISCARD static Float32 EaseOutElastic(Float32 t)
    {
        if (t <= 0.0f) return 0.0f;
        if (t >= 1.0f) return 1.0f;
        return FMath::Pow(2.0f, -10.0f * t) *
            FMath::Sin((10.0f * t - 0.75f) *
                       (2.0f * FMath::kPi / 3.0f)) + 1.0f;
    }

    LIMX_NODISCARD static Float32 EaseInOutElastic(Float32 t)
    {
        if (t <= 0.0f) return 0.0f;
        if (t >= 1.0f) return 1.0f;
        constexpr Float32 kC = 2.0f * FMath::kPi / 4.5f;
        if (t < 0.5f)
        {
            return -0.5f * FMath::Pow(2.0f, 20.0f * t - 10.0f) *
                FMath::Sin((20.0f * t - 11.125f) * kC);
        }
        return 0.5f * FMath::Pow(2.0f, -20.0f * t + 10.0f) *
            FMath::Sin((20.0f * t - 11.125f) * kC) + 1.0f;
    }

    // ========================================================================
    // Bounce (弹跳)
    // ========================================================================

    LIMX_NODISCARD static Float32 EaseOutBounce(Float32 t)
    {
        constexpr Float32 kN = 7.5625f;
        constexpr Float32 kD = 2.75f;

        if (t < 1.0f / kD)
        {
            return kN * t * t;
        }
        else if (t < 2.0f / kD)
        {
            Float32 f = t - 1.5f / kD;
            return kN * f * f + 0.75f;
        }
        else if (t < 2.5f / kD)
        {
            Float32 f = t - 2.25f / kD;
            return kN * f * f + 0.9375f;
        }
        else
        {
            Float32 f = t - 2.625f / kD;
            return kN * f * f + 0.984375f;
        }
    }

    LIMX_NODISCARD static Float32 EaseInBounce(Float32 t)
    {
        return 1.0f - EaseOutBounce(1.0f - t);
    }

    LIMX_NODISCARD static Float32 EaseInOutBounce(Float32 t)
    {
        if (t < 0.5f)
            return 0.5f * EaseInBounce(2.0f * t);
        return 0.5f * EaseOutBounce(2.0f * t - 1.0f) + 0.5f;
    }

    // ========================================================================
    // SmoothStep (Hermite)
    // ========================================================================

    LIMX_NODISCARD static Float32 SmoothStep(Float32 t)
    {
        return t * t * (3.0f - 2.0f * t);
    }

    LIMX_NODISCARD static Float32 SmootherStep(Float32 t)
    {
        return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    }
};

} // namespace Limx
