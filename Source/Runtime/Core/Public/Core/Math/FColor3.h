/*******************************************************************************
 * 文件: FColor3.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   3 通道颜色 — RGB Float32 精度颜色类型
 *   提供 HDR 颜色、色空间转换 (线性/sRGB/HSV)
 *   用于材质颜色参数、光照颜色、HDR 渲染等场景
 *
 * 设计哲学:
 *   无上限 — Float32 通道支持 HDR 值 (>1.0)
 *   色空间明确 — 线性与 sRGB 转换通过静态方法显式进行
 *   与 FLinearColor 区分 — FColor3 为 3 通道，无 Alpha
 *
 * 技术特性:
 *   - FColor3: RGB Float32 颜色
 *   - ToSRGB/FromSRGB: 线性与 sRGB 互转
 *   - ToHSV/FromHSV: HSV 色彩空间互转
 *   - Luminance: 亮度 (BT.709 系数)
 *   - Lerp: 线性插值
 *   - operator+/-/*: 颜色算术
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

/// RGB Float32 颜色 (HDR)
struct FColor3
{
    Float32 R;  ///< 红通道
    Float32 G;  ///< 绿通道
    Float32 B;  ///< 蓝通道

    // ========================================================================
    // 构造
    // ========================================================================

    FColor3()
        : R(0.0f), G(0.0f), B(0.0f)
    {
    }

    FColor3(Float32 r, Float32 g, Float32 b)
        : R(r), G(g), B(b)
    {
    }

    explicit FColor3(Float32 gray)
        : R(gray), G(gray), B(gray)
    {
    }

    // ========================================================================
    // 预定义颜色
    // ========================================================================

    LIMX_NODISCARD static FColor3 Black()
    {
        return FColor3(0.0f, 0.0f, 0.0f);
    }
    LIMX_NODISCARD static FColor3 White()
    {
        return FColor3(1.0f, 1.0f, 1.0f);
    }
    LIMX_NODISCARD static FColor3 Red()
    {
        return FColor3(1.0f, 0.0f, 0.0f);
    }
    LIMX_NODISCARD static FColor3 Green()
    {
        return FColor3(0.0f, 1.0f, 0.0f);
    }
    LIMX_NODISCARD static FColor3 Blue()
    {
        return FColor3(0.0f, 0.0f, 1.0f);
    }
    LIMX_NODISCARD static FColor3 Yellow()
    {
        return FColor3(1.0f, 1.0f, 0.0f);
    }
    LIMX_NODISCARD static FColor3 Cyan()
    {
        return FColor3(0.0f, 1.0f, 1.0f);
    }
    LIMX_NODISCARD static FColor3 Magenta()
    {
        return FColor3(1.0f, 0.0f, 1.0f);
    }

    // ========================================================================
    // 色空间转换
    // ========================================================================

    /// 线性 → sRGB
    LIMX_NODISCARD FColor3 ToSRGB() const
    {
        return FColor3(
            LinearToSRGBChannel(R),
            LinearToSRGBChannel(G),
            LinearToSRGBChannel(B));
    }

    /// sRGB → 线性
    LIMX_NODISCARD FColor3 FromSRGB() const
    {
        return FColor3(
            SRGBToLinearChannel(R),
            SRGBToLinearChannel(G),
            SRGBToLinearChannel(B));
    }

    /// 线性 RGB → HSV
    /// @param outH 色相 [0, 360)
    /// @param outS 饱和度 [0, 1]
    /// @param outV 亮度 [0, +∞)
    void ToHSV(Float32& outH, Float32& outS,
               Float32& outV) const
    {
        Float32 maxVal = FMath::Max(R, FMath::Max(G, B));
        Float32 minVal = FMath::Min(R, FMath::Min(G, B));
        Float32 delta = maxVal - minVal;

        outV = maxVal;

        if (maxVal < 1e-6f)
        {
            outS = 0.0f;
            outH = 0.0f;
            return;
        }

        outS = delta / maxVal;

        if (delta < 1e-6f)
        {
            outH = 0.0f;
            return;
        }

        if (maxVal == R)
        {
            outH = 60.0f * ((G - B) / delta);
        }
        else if (maxVal == G)
        {
            outH = 60.0f * (2.0f + (B - R) / delta);
        }
        else
        {
            outH = 60.0f * (4.0f + (R - G) / delta);
        }

        if (outH < 0.0f) outH += 360.0f;
    }

    /// HSV → 线性 RGB
    LIMX_NODISCARD static FColor3 FromHSV(
        Float32 h, Float32 s, Float32 v)
    {
        if (s < 1e-6f) return FColor3(v);

        h = h / 60.0f;
        Int32 sectorIdx = static_cast<Int32>(h) % 6;
        Float32 f = h - static_cast<Int32>(h);

        Float32 p = v * (1.0f - s);
        Float32 q = v * (1.0f - s * f);
        Float32 t = v * (1.0f - s * (1.0f - f));

        switch (sectorIdx)
        {
        case 0: return FColor3(v, t, p);
        case 1: return FColor3(q, v, p);
        case 2: return FColor3(p, v, t);
        case 3: return FColor3(p, q, v);
        case 4: return FColor3(t, p, v);
        default: return FColor3(v, p, q);
        }
    }

    // ========================================================================
    // 属性
    // ========================================================================

    /// BT.709 亮度
    LIMX_NODISCARD Float32 Luminance() const
    {
        return R * 0.2126f + G * 0.7152f + B * 0.0722f;
    }

    /// 是否为黑色
    LIMX_NODISCARD bool IsBlack() const
    {
        return R < 1e-6f && G < 1e-6f && B < 1e-6f;
    }

    // ========================================================================
    // 插值
    // ========================================================================

    LIMX_NODISCARD static FColor3 Lerp(
        const FColor3& a, const FColor3& b, Float32 t)
    {
        return FColor3(
            a.R + (b.R - a.R) * t,
            a.G + (b.G - a.G) * t,
            a.B + (b.B - a.B) * t);
    }

    // ========================================================================
    // 算术运算
    // ========================================================================

    LIMX_NODISCARD FColor3 operator+(
        const FColor3& other) const
    {
        return FColor3(R + other.R, G + other.G,
                       B + other.B);
    }

    LIMX_NODISCARD FColor3 operator-(
        const FColor3& other) const
    {
        return FColor3(R - other.R, G - other.G,
                       B - other.B);
    }

    LIMX_NODISCARD FColor3 operator*(
        const FColor3& other) const
    {
        return FColor3(R * other.R, G * other.G,
                       B * other.B);
    }

    LIMX_NODISCARD FColor3 operator*(Float32 scalar) const
    {
        return FColor3(R * scalar, G * scalar,
                       B * scalar);
    }

    LIMX_NODISCARD FColor3 operator/(Float32 scalar) const
    {
        Float32 inv = 1.0f / scalar;
        return FColor3(R * inv, G * inv, B * inv);
    }

    FColor3& operator+=(const FColor3& other)
    {
        R += other.R; G += other.G; B += other.B;
        return *this;
    }

    FColor3& operator*=(Float32 scalar)
    {
        R *= scalar; G *= scalar; B *= scalar;
        return *this;
    }

    LIMX_NODISCARD bool operator==(
        const FColor3& other) const
    {
        return FMath::Abs(R - other.R) < 1e-6f &&
               FMath::Abs(G - other.G) < 1e-6f &&
               FMath::Abs(B - other.B) < 1e-6f;
    }

    LIMX_NODISCARD bool operator!=(
        const FColor3& other) const
    {
        return !(*this == other);
    }

private:
    LIMX_NODISCARD static Float32 LinearToSRGBChannel(
        Float32 linear)
    {
        if (linear <= 0.0031308f)
            return linear * 12.92f;
        return 1.055f *
            FMath::Pow(linear, 1.0f / 2.4f) - 0.055f;
    }

    LIMX_NODISCARD static Float32 SRGBToLinearChannel(
        Float32 srgb)
    {
        if (srgb <= 0.04045f)
            return srgb / 12.92f;
        return FMath::Pow(
            (srgb + 0.055f) / 1.055f, 2.4f);
    }
};

LIMX_NODISCARD inline FColor3 operator*(
    Float32 scalar, const FColor3& color)
{
    return color * scalar;
}

} // namespace Limx
