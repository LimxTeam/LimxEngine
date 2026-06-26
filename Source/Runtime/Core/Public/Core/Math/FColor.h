/*******************************************************************************
 * 文件: FColor.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   颜色类型 — 渲染管线的基础颜色表示
 *   FColor: 8 位 RGBA 整数颜色 (sRGB 空间, 纹理/UI/顶点色)
 *   FLinearColor: 32 位浮点 RGBA 线性颜色 (光照计算/着色器)
 *   支持 sRGB ↔ 线性空间互转、HSV 互转、预定义常用颜色
 *
 * 设计哲学:
 *   双精度模型 — FColor (存储/传输) + FLinearColor (计算)
 *   物理正确 — 线性空间做光照计算，sRGB 空间做显示/存储
 *   紧凑存储 — FColor 仅 4 字节，可直接传给 GPU
 *
 * 技术特性:
 *   - FColor: RGBA 各 8 位, 4 字节, 可位打包为 UInt32
 *   - FLinearColor: RGBA 各 Float32, 16 字节
 *   - sRGB 转换: Gamma 2.2 近似 + 精确分段函数
 *   - HSV 互转: FromHSV / ToHSV
 *   - 预定义颜色: kBlack, kWhite, kRed, kGreen, kBlue, kTransparent
 *
 * 依赖关系:
 *   内部: Core/Math/FMath.h, Core/HAL/PlatformTypes.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Math/FMath.h"

namespace Limx
{

// 前向声明
struct FLinearColor;

// ============================================================================
// FColor — 8 位 RGBA sRGB 颜色
// ============================================================================

struct FColor
{
    UInt8 R;
    UInt8 G;
    UInt8 B;
    UInt8 A;

    // 预定义颜色
    static const FColor kBlack;
    static const FColor kWhite;
    static const FColor kRed;
    static const FColor kGreen;
    static const FColor kBlue;
    static const FColor kYellow;
    static const FColor kCyan;
    static const FColor kMagenta;
    static const FColor kTransparent;

    // ========================================================================
    // 构造
    // ========================================================================

    constexpr FColor() : R(0), G(0), B(0), A(255) {}

    constexpr FColor(UInt8 inR, UInt8 inG, UInt8 inB, UInt8 inA = 255)
        : R(inR), G(inG), B(inB), A(inA) {}

    /// 从 32 位打包值构造 (0xRRGGBBAA)
    constexpr explicit FColor(UInt32 packed)
        : R(static_cast<UInt8>((packed >> 24) & 0xFF))
        , G(static_cast<UInt8>((packed >> 16) & 0xFF))
        , B(static_cast<UInt8>((packed >>  8) & 0xFF))
        , A(static_cast<UInt8>((packed >>  0) & 0xFF))
    {
    }

    // ========================================================================
    // 打包
    // ========================================================================

    /// 打包为 0xRRGGBBAA
    LIMX_NODISCARD constexpr UInt32 ToPackedRGBA() const
    {
        return (static_cast<UInt32>(R) << 24) |
               (static_cast<UInt32>(G) << 16) |
               (static_cast<UInt32>(B) <<  8) |
               (static_cast<UInt32>(A) <<  0);
    }

    /// 打包为 0xAABBGGRR (GPU 常用格式)
    LIMX_NODISCARD constexpr UInt32 ToPackedABGR() const
    {
        return (static_cast<UInt32>(A) << 24) |
               (static_cast<UInt32>(B) << 16) |
               (static_cast<UInt32>(G) <<  8) |
               (static_cast<UInt32>(R) <<  0);
    }

    // ========================================================================
    // sRGB → 线性转换
    // ========================================================================

    /// 转换为线性颜色 (sRGB → 线性空间)
    LIMX_NODISCARD FLinearColor ToLinear() const;

    /// 从线性颜色构造 (线性 → sRGB)
    LIMX_NODISCARD static FColor FromLinear(const FLinearColor& linear);

    // ========================================================================
    // 比较
    // ========================================================================

    LIMX_NODISCARD constexpr bool operator==(const FColor& other) const
    {
        return R == other.R && G == other.G && B == other.B && A == other.A;
    }

    LIMX_NODISCARD constexpr bool operator!=(const FColor& other) const
    {
        return !(*this == other);
    }
};

// 常量定义
inline constexpr FColor FColor::kBlack       = FColor(  0,   0,   0, 255);
inline constexpr FColor FColor::kWhite       = FColor(255, 255, 255, 255);
inline constexpr FColor FColor::kRed         = FColor(255,   0,   0, 255);
inline constexpr FColor FColor::kGreen       = FColor(  0, 255,   0, 255);
inline constexpr FColor FColor::kBlue        = FColor(  0,   0, 255, 255);
inline constexpr FColor FColor::kYellow      = FColor(255, 255,   0, 255);
inline constexpr FColor FColor::kCyan        = FColor(  0, 255, 255, 255);
inline constexpr FColor FColor::kMagenta     = FColor(255,   0, 255, 255);
inline constexpr FColor FColor::kTransparent = FColor(  0,   0,   0,   0);

// ============================================================================
// FLinearColor — Float32 RGBA 线性颜色
// ============================================================================

struct FLinearColor
{
    Float32 R;
    Float32 G;
    Float32 B;
    Float32 A;

    // 预定义颜色
    static const FLinearColor kBlack;
    static const FLinearColor kWhite;
    static const FLinearColor kRed;
    static const FLinearColor kGreen;
    static const FLinearColor kBlue;
    static const FLinearColor kTransparent;

    // ========================================================================
    // 构造
    // ========================================================================

    constexpr FLinearColor() : R(0.0f), G(0.0f), B(0.0f), A(1.0f) {}

    constexpr FLinearColor(Float32 inR, Float32 inG, Float32 inB,
                           Float32 inA = 1.0f)
        : R(inR), G(inG), B(inB), A(inA) {}

    // ========================================================================
    // 算术运算符
    // ========================================================================

    LIMX_NODISCARD constexpr FLinearColor operator+(
        const FLinearColor& other) const
    {
        return FLinearColor(R + other.R, G + other.G,
                            B + other.B, A + other.A);
    }

    LIMX_NODISCARD constexpr FLinearColor operator-(
        const FLinearColor& other) const
    {
        return FLinearColor(R - other.R, G - other.G,
                            B - other.B, A - other.A);
    }

    LIMX_NODISCARD constexpr FLinearColor operator*(Float32 scalar) const
    {
        return FLinearColor(R * scalar, G * scalar,
                            B * scalar, A * scalar);
    }

    LIMX_NODISCARD constexpr FLinearColor operator*(
        const FLinearColor& other) const
    {
        return FLinearColor(R * other.R, G * other.G,
                            B * other.B, A * other.A);
    }

    LIMX_NODISCARD constexpr FLinearColor operator/(Float32 scalar) const
    {
        Float32 inv = 1.0f / scalar;
        return FLinearColor(R * inv, G * inv, B * inv, A * inv);
    }

    FLinearColor& operator+=(const FLinearColor& other)
    {
        R += other.R; G += other.G; B += other.B; A += other.A;
        return *this;
    }

    FLinearColor& operator*=(Float32 scalar)
    {
        R *= scalar; G *= scalar; B *= scalar; A *= scalar;
        return *this;
    }

    // ========================================================================
    // 钳制
    // ========================================================================

    /// 钳制所有通道到 [0, 1]
    LIMX_NODISCARD FLinearColor Clamped() const
    {
        return FLinearColor(
            FMath::Saturate(R), FMath::Saturate(G),
            FMath::Saturate(B), FMath::Saturate(A)
        );
    }

    // ========================================================================
    // sRGB 转换
    // ========================================================================

    /// 转换为 sRGB FColor (线性 → sRGB 空间)
    LIMX_NODISCARD FColor ToSRGB() const
    {
        return FColor(
            LinearToSRGB8(R),
            LinearToSRGB8(G),
            LinearToSRGB8(B),
            static_cast<UInt8>(FMath::Clamp(
                FMath::RoundToInt(A * 255.0f), 0, 255))
        );
    }

    /// 从 sRGB FColor 构造 (sRGB → 线性空间)
    LIMX_NODISCARD static FLinearColor FromSRGB(const FColor& srgb)
    {
        return FLinearColor(
            SRGB8ToLinear(srgb.R),
            SRGB8ToLinear(srgb.G),
            SRGB8ToLinear(srgb.B),
            static_cast<Float32>(srgb.A) / 255.0f
        );
    }

    // ========================================================================
    // HSV 转换
    // ========================================================================

    /// 从 HSV 构造
    /// @param hue        色相 [0, 360)
    /// @param saturation 饱和度 [0, 1]
    /// @param value      明度 [0, 1]
    /// @param alpha      透明度 [0, 1]
    LIMX_NODISCARD static FLinearColor FromHSV(Float32 hue,
                                                Float32 saturation,
                                                Float32 value,
                                                Float32 alpha = 1.0f)
    {
        hue = FMath::Fmod(hue, 360.0f);
        if (hue < 0.0f)
        {
            hue += 360.0f;
        }

        Float32 c = value * saturation;
        Float32 x = c * (1.0f - FMath::Abs(
            FMath::Fmod(hue / 60.0f, 2.0f) - 1.0f));
        Float32 m = value - c;

        Float32 r, g, b;
        if (hue < 60.0f)       { r = c; g = x; b = 0; }
        else if (hue < 120.0f) { r = x; g = c; b = 0; }
        else if (hue < 180.0f) { r = 0; g = c; b = x; }
        else if (hue < 240.0f) { r = 0; g = x; b = c; }
        else if (hue < 300.0f) { r = x; g = 0; b = c; }
        else                   { r = c; g = 0; b = x; }

        return FLinearColor(r + m, g + m, b + m, alpha);
    }

    /// 转换为 HSV (返回 FVector3: X=Hue, Y=Saturation, Z=Value)
    LIMX_NODISCARD FVector3 ToHSV() const
    {
        Float32 maxComp = FMath::Max(R, FMath::Max(G, B));
        Float32 minComp = FMath::Min(R, FMath::Min(G, B));
        Float32 delta = maxComp - minComp;

        Float32 hue = 0.0f;
        Float32 saturation = (maxComp > FMath::kSmallNumber)
            ? (delta / maxComp) : 0.0f;
        Float32 value = maxComp;

        if (delta > FMath::kSmallNumber)
        {
            if (maxComp == R)
            {
                hue = 60.0f * FMath::Fmod((G - B) / delta, 6.0f);
            }
            else if (maxComp == G)
            {
                hue = 60.0f * ((B - R) / delta + 2.0f);
            }
            else
            {
                hue = 60.0f * ((R - G) / delta + 4.0f);
            }

            if (hue < 0.0f)
            {
                hue += 360.0f;
            }
        }

        return FVector3(hue, saturation, value);
    }

    // ========================================================================
    // 亮度
    // ========================================================================

    /// 感知亮度 (ITU-R BT.709)
    LIMX_NODISCARD Float32 Luminance() const
    {
        return 0.2126f * R + 0.7152f * G + 0.0722f * B;
    }

    // ========================================================================
    // 插值
    // ========================================================================

    LIMX_NODISCARD static constexpr FLinearColor Lerp(
        const FLinearColor& a, const FLinearColor& b, Float32 t)
    {
        return FLinearColor(
            FMath::Lerp(a.R, b.R, t),
            FMath::Lerp(a.G, b.G, t),
            FMath::Lerp(a.B, b.B, t),
            FMath::Lerp(a.A, b.A, t)
        );
    }

    // ========================================================================
    // 比较
    // ========================================================================

    LIMX_NODISCARD bool Equals(const FLinearColor& other,
                                Float32 tolerance = FMath::kSmallNumber) const
    {
        return FMath::Abs(R - other.R) <= tolerance &&
               FMath::Abs(G - other.G) <= tolerance &&
               FMath::Abs(B - other.B) <= tolerance &&
               FMath::Abs(A - other.A) <= tolerance;
    }

    LIMX_NODISCARD constexpr bool operator==(const FLinearColor& other) const
    {
        return R == other.R && G == other.G && B == other.B && A == other.A;
    }

    LIMX_NODISCARD constexpr bool operator!=(const FLinearColor& other) const
    {
        return !(*this == other);
    }

private:
    // ========================================================================
    // sRGB 转换辅助 (精确分段函数)
    // ========================================================================

    /// sRGB 8 位 → 线性 Float32
    static Float32 SRGB8ToLinear(UInt8 srgb)
    {
        Float32 normalized = static_cast<Float32>(srgb) / 255.0f;
        if (normalized <= 0.04045f)
        {
            return normalized / 12.92f;
        }
        return FMath::Pow((normalized + 0.055f) / 1.055f, 2.4f);
    }

    /// 线性 Float32 → sRGB 8 位
    static UInt8 LinearToSRGB8(Float32 linear)
    {
        Float32 clamped = FMath::Saturate(linear);
        Float32 srgb;
        if (clamped <= 0.0031308f)
        {
            srgb = clamped * 12.92f;
        }
        else
        {
            srgb = 1.055f * FMath::Pow(clamped, 1.0f / 2.4f) - 0.055f;
        }
        return static_cast<UInt8>(FMath::Clamp(
            FMath::RoundToInt(srgb * 255.0f), 0, 255));
    }
};

// FLinearColor 常量定义
inline constexpr FLinearColor FLinearColor::kBlack       = FLinearColor(0.0f, 0.0f, 0.0f, 1.0f);
inline constexpr FLinearColor FLinearColor::kWhite       = FLinearColor(1.0f, 1.0f, 1.0f, 1.0f);
inline constexpr FLinearColor FLinearColor::kRed         = FLinearColor(1.0f, 0.0f, 0.0f, 1.0f);
inline constexpr FLinearColor FLinearColor::kGreen       = FLinearColor(0.0f, 1.0f, 0.0f, 1.0f);
inline constexpr FLinearColor FLinearColor::kBlue        = FLinearColor(0.0f, 0.0f, 1.0f, 1.0f);
inline constexpr FLinearColor FLinearColor::kTransparent = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);

// ============================================================================
// FColor 延迟定义的内联方法 (依赖 FLinearColor 完整定义)
// ============================================================================

inline FLinearColor FColor::ToLinear() const
{
    return FLinearColor::FromSRGB(*this);
}

inline FColor FColor::FromLinear(const FLinearColor& linear)
{
    return linear.ToSRGB();
}

LIMX_NODISCARD constexpr FLinearColor operator*(Float32 scalar,
                                                 const FLinearColor& c)
{
    return c * scalar;
}

} // namespace Limx
