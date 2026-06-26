/*******************************************************************************
 * 文件: FColorHSL.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   HSL 颜色 — 色相/饱和度/亮度表示的颜色类型
 *   提供 HSL ↔ RGB 互转，色彩调整操作
 *   用于颜色选择器、调色板、图像后处理等场景
 *
 * 设计哲学:
 *   HSL 直觉化 — H[0,360)/S[0,1]/L[0,1]，更符合艺术家直觉
 *   与 FColor3 互转 — 提供无损往返转换
 *   值类型 — 轻量可拷贝
 *
 * 技术特性:
 *   - FColorHSL: H/S/L 颜色
 *   - ToRGB/FromRGB: 与线性 RGB 互转
 *   - AdjustHue/AdjustSaturation/AdjustLightness: 调整
 *   - Lerp: 色相感知插值
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Math/FMath.h,
 *          Core/Math/FColor3.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Math/FMath.h"
#include "Core/Math/FColor3.h"

namespace Limx
{

/// HSL 颜色
struct FColorHSL
{
    Float32 H;  ///< 色相 [0, 360)
    Float32 S;  ///< 饱和度 [0, 1]
    Float32 L;  ///< 亮度 [0, 1]

    // ========================================================================
    // 构造
    // ========================================================================

    FColorHSL()
        : H(0.0f), S(0.0f), L(0.0f)
    {
    }

    FColorHSL(Float32 h, Float32 s, Float32 l)
        : H(h), S(s), L(l)
    {
    }

    // ========================================================================
    // 与 RGB 互转
    // ========================================================================

    /// 从线性 RGB 转换
    LIMX_NODISCARD static FColorHSL FromRGB(
        const FColor3& rgb)
    {
        Float32 maxVal = FMath::Max(rgb.R,
            FMath::Max(rgb.G, rgb.B));
        Float32 minVal = FMath::Min(rgb.R,
            FMath::Min(rgb.G, rgb.B));
        Float32 delta = maxVal - minVal;

        FColorHSL result;
        result.L = (maxVal + minVal) * 0.5f;

        if (delta < 1e-6f)
        {
            result.H = 0.0f;
            result.S = 0.0f;
            return result;
        }

        result.S = (result.L < 0.5f)
            ? delta / (maxVal + minVal)
            : delta / (2.0f - maxVal - minVal);

        if (maxVal == rgb.R)
        {
            result.H = 60.0f *
                ((rgb.G - rgb.B) / delta);
        }
        else if (maxVal == rgb.G)
        {
            result.H = 60.0f *
                (2.0f + (rgb.B - rgb.R) / delta);
        }
        else
        {
            result.H = 60.0f *
                (4.0f + (rgb.R - rgb.G) / delta);
        }

        if (result.H < 0.0f) result.H += 360.0f;
        return result;
    }

    /// 转换为线性 RGB
    LIMX_NODISCARD FColor3 ToRGB() const
    {
        if (S < 1e-6f)
        {
            return FColor3(L);
        }

        Float32 q = (L < 0.5f)
            ? L * (1.0f + S)
            : L + S - L * S;
        Float32 p = 2.0f * L - q;

        return FColor3(
            HueToRGB(p, q, H / 360.0f + 1.0f / 3.0f),
            HueToRGB(p, q, H / 360.0f),
            HueToRGB(p, q, H / 360.0f - 1.0f / 3.0f));
    }

    // ========================================================================
    // 调整
    // ========================================================================

    /// 调整色相 (旋转)
    LIMX_NODISCARD FColorHSL AdjustHue(
        Float32 delta) const
    {
        Float32 newH = fmodf(H + delta, 360.0f);
        if (newH < 0.0f) newH += 360.0f;
        return FColorHSL(newH, S, L);
    }

    /// 调整饱和度
    LIMX_NODISCARD FColorHSL AdjustSaturation(
        Float32 delta) const
    {
        return FColorHSL(H,
            FMath::Clamp(S + delta, 0.0f, 1.0f), L);
    }

    /// 调整亮度
    LIMX_NODISCARD FColorHSL AdjustLightness(
        Float32 delta) const
    {
        return FColorHSL(H, S,
            FMath::Clamp(L + delta, 0.0f, 1.0f));
    }

    // ========================================================================
    // 插值
    // ========================================================================

    /// 色相感知线性插值 (沿最短色相路径)
    LIMX_NODISCARD static FColorHSL Lerp(
        const FColorHSL& a, const FColorHSL& b,
        Float32 t)
    {
        Float32 hueDelta = b.H - a.H;

        // 走最短路径
        if (hueDelta > 180.0f) hueDelta -= 360.0f;
        if (hueDelta < -180.0f) hueDelta += 360.0f;

        Float32 newH =
            fmodf(a.H + hueDelta * t, 360.0f);
        if (newH < 0.0f) newH += 360.0f;

        return FColorHSL(
            newH,
            a.S + (b.S - a.S) * t,
            a.L + (b.L - a.L) * t);
    }

    // ========================================================================
    // 比较
    // ========================================================================

    LIMX_NODISCARD bool operator==(
        const FColorHSL& other) const
    {
        return FMath::Abs(H - other.H) < 1e-4f &&
               FMath::Abs(S - other.S) < 1e-6f &&
               FMath::Abs(L - other.L) < 1e-6f;
    }

    LIMX_NODISCARD bool operator!=(
        const FColorHSL& other) const
    {
        return !(*this == other);
    }

private:
    LIMX_NODISCARD static Float32 HueToRGB(
        Float32 p, Float32 q, Float32 t)
    {
        if (t < 0.0f) t += 1.0f;
        if (t > 1.0f) t -= 1.0f;

        if (t < 1.0f / 6.0f)
            return p + (q - p) * 6.0f * t;
        if (t < 0.5f)
            return q;
        if (t < 2.0f / 3.0f)
            return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
        return p;
    }
};

} // namespace Limx
