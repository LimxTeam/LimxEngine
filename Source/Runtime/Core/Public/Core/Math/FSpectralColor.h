/*******************************************************************************
 * 文件: FSpectralColor.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   光谱颜色 — 离散波长采样的功率分布
 *   存储 N 个波长区间的辐射功率，支持光谱到 XYZ/RGB 转换
 *   用于物理准确光谱渲染、荧光材质、色散玻璃等场景
 *
 * 设计哲学:
 *   离散采样 — 固定 N 个等间距波长区间 [380nm, 780nm]
 *   编译时大小 — NumSamples 为模板参数，内嵌数组
 *   物理量 — 功率密度 (W/nm)，非归一化颜色
 *
 * 技术特性:
 *   - TSpectralColor<N>: N 点离散光谱
 *   - ToXYZ: 转换为 CIE XYZ (三刺激值积分)
 *   - ToRGB: 转换为 sRGB
 *   - operator+/*: 光谱叠加/缩放
 *   - GetPowerAt: 指定波长插值采样
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

/// 离散光谱颜色
/// @tparam NumSamples 波长采样点数 (默认 16, 对应 ~25nm 间距)
template<Int32 NumSamples = 16>
class TSpectralColor
{
    static_assert(NumSamples >= 2,
        "NumSamples must be >= 2");

    /// 可见光波长范围 [nm]
    static constexpr Float32 kLambdaMin = 380.0f;
    static constexpr Float32 kLambdaMax = 780.0f;

public:
    TSpectralColor()
    {
        for (Int32 sampleIdx = 0;
             sampleIdx < NumSamples; ++sampleIdx)
        {
            m_Power[sampleIdx] = 0.0f;
        }
    }

    explicit TSpectralColor(Float32 uniformPower)
    {
        for (Int32 sampleIdx = 0;
             sampleIdx < NumSamples; ++sampleIdx)
        {
            m_Power[sampleIdx] = uniformPower;
        }
    }

    // ========================================================================
    // 工厂方法
    // ========================================================================

    /// D65 标准光源 (归一化)
    LIMX_NODISCARD static TSpectralColor D65()
    {
        return TSpectralColor(1.0f);
    }

    /// 零功率光谱
    LIMX_NODISCARD static TSpectralColor Zero()
    {
        return TSpectralColor(0.0f);
    }

    // ========================================================================
    // 采样访问
    // ========================================================================

    /// 按索引获取功率
    LIMX_NODISCARD Float32 operator[](Int32 index) const
    {
        LIMX_ASSERT(index >= 0 && index < NumSamples);
        return m_Power[index];
    }

    LIMX_NODISCARD Float32& operator[](Int32 index)
    {
        LIMX_ASSERT(index >= 0 && index < NumSamples);
        return m_Power[index];
    }

    /// 按波长 (nm) 线性插值采样
    LIMX_NODISCARD Float32 GetPowerAt(
        Float32 wavelengthNm) const
    {
        Float32 t = (wavelengthNm - kLambdaMin) /
                    (kLambdaMax - kLambdaMin);
        t = FMath::Clamp(t, 0.0f, 1.0f);

        Float32 fIdx = t * (NumSamples - 1);
        Int32 loIdx = static_cast<Int32>(fIdx);
        Int32 hiIdx = loIdx + 1;

        if (hiIdx >= NumSamples)
        {
            return m_Power[NumSamples - 1];
        }

        Float32 alpha = fIdx - static_cast<Float32>(loIdx);
        return m_Power[loIdx] * (1.0f - alpha) +
               m_Power[hiIdx] * alpha;
    }

    /// 第 i 个采样点对应的波长 (nm)
    LIMX_NODISCARD static Float32 GetWavelengthAt(
        Int32 index)
    {
        Float32 t = static_cast<Float32>(index) /
                    static_cast<Float32>(NumSamples - 1);
        return kLambdaMin +
               t * (kLambdaMax - kLambdaMin);
    }

    LIMX_NODISCARD static constexpr Int32 GetNumSamples()
    {
        return NumSamples;
    }

    // ========================================================================
    // 色彩转换
    // ========================================================================

    /// 转换为 CIE XYZ (使用 CIE 1931 色度匹配函数的近似)
    LIMX_NODISCARD FVector3 ToXYZ() const
    {
        FVector3 xyz(0.0f, 0.0f, 0.0f);
        Float32 deltaLambda =
            (kLambdaMax - kLambdaMin) /
            static_cast<Float32>(NumSamples);

        for (Int32 sampleIdx = 0;
             sampleIdx < NumSamples; ++sampleIdx)
        {
            Float32 lambda = GetWavelengthAt(sampleIdx);
            Float32 power = m_Power[sampleIdx];

            // CIE 1931 色度匹配函数近似
            // (Wyman 2013 高斯近似)
            Float32 xBar =
                GaussianXBar(lambda);
            Float32 yBar =
                GaussianYBar(lambda);
            Float32 zBar =
                GaussianZBar(lambda);

            xyz.X += power * xBar * deltaLambda;
            xyz.Y += power * yBar * deltaLambda;
            xyz.Z += power * zBar * deltaLambda;
        }

        return xyz;
    }

    /// 转换为线性 sRGB
    LIMX_NODISCARD FVector3 ToLinearRGB() const
    {
        FVector3 xyz = ToXYZ();

        // XYZ → sRGB (D65 白点)
        return FVector3(
            3.2406f * xyz.X - 1.5372f * xyz.Y -
            0.4986f * xyz.Z,
            -0.9689f * xyz.X + 1.8758f * xyz.Y +
            0.0415f * xyz.Z,
            0.0557f * xyz.X - 0.2040f * xyz.Y +
            1.0570f * xyz.Z);
    }

    // ========================================================================
    // 光谱算术
    // ========================================================================

    LIMX_NODISCARD TSpectralColor operator+(
        const TSpectralColor& other) const
    {
        TSpectralColor result;
        for (Int32 sampleIdx = 0;
             sampleIdx < NumSamples; ++sampleIdx)
        {
            result.m_Power[sampleIdx] =
                m_Power[sampleIdx] +
                other.m_Power[sampleIdx];
        }
        return result;
    }

    LIMX_NODISCARD TSpectralColor operator*(
        const TSpectralColor& other) const
    {
        TSpectralColor result;
        for (Int32 sampleIdx = 0;
             sampleIdx < NumSamples; ++sampleIdx)
        {
            result.m_Power[sampleIdx] =
                m_Power[sampleIdx] *
                other.m_Power[sampleIdx];
        }
        return result;
    }

    LIMX_NODISCARD TSpectralColor operator*(
        Float32 scalar) const
    {
        TSpectralColor result;
        for (Int32 sampleIdx = 0;
             sampleIdx < NumSamples; ++sampleIdx)
        {
            result.m_Power[sampleIdx] =
                m_Power[sampleIdx] * scalar;
        }
        return result;
    }

    TSpectralColor& operator+=(
        const TSpectralColor& other)
    {
        for (Int32 sampleIdx = 0;
             sampleIdx < NumSamples; ++sampleIdx)
        {
            m_Power[sampleIdx] += other.m_Power[sampleIdx];
        }
        return *this;
    }

    TSpectralColor& operator*=(Float32 scalar)
    {
        for (Int32 sampleIdx = 0;
             sampleIdx < NumSamples; ++sampleIdx)
        {
            m_Power[sampleIdx] *= scalar;
        }
        return *this;
    }

    // ========================================================================
    // 属性
    // ========================================================================

    /// 总辐射功率
    LIMX_NODISCARD Float32 GetTotalPower() const
    {
        Float32 total = 0.0f;
        for (Int32 sampleIdx = 0;
             sampleIdx < NumSamples; ++sampleIdx)
        {
            total += m_Power[sampleIdx];
        }
        return total;
    }

    LIMX_NODISCARD bool IsBlack() const
    {
        return GetTotalPower() < 1e-8f;
    }

private:
    /// CIE x̄(λ) 近似 (三高斯之和)
    LIMX_NODISCARD static Float32 GaussianXBar(
        Float32 lambda)
    {
        return 1.056f * GaussianFit(lambda, 599.8f, 37.9f, 31.0f) +
               0.362f * GaussianFit(lambda, 442.0f, 16.0f, 26.7f) -
               0.065f * GaussianFit(lambda, 501.1f, 20.4f, 26.2f);
    }

    /// CIE ȳ(λ) 近似
    LIMX_NODISCARD static Float32 GaussianYBar(
        Float32 lambda)
    {
        return 0.821f * GaussianFit(lambda, 568.8f, 46.9f, 40.5f) +
               0.286f * GaussianFit(lambda, 530.9f, 16.3f, 31.1f);
    }

    /// CIE z̄(λ) 近似
    LIMX_NODISCARD static Float32 GaussianZBar(
        Float32 lambda)
    {
        return 1.217f * GaussianFit(lambda, 437.0f, 11.8f, 36.0f) +
               0.681f * GaussianFit(lambda, 459.0f, 26.0f, 13.8f);
    }

    LIMX_NODISCARD static Float32 GaussianFit(
        Float32 lambda, Float32 mu,
        Float32 t1, Float32 t2)
    {
        Float32 delta = lambda - mu;
        Float32 sigma = (delta < 0.0f) ? t1 : t2;
        return FMath::Exp(
            -0.5f * delta * delta / (sigma * sigma));
    }

    Float32 m_Power[NumSamples];  ///< 各波长区间功率
};

/// 常用别名
using FSpectralColor16 = TSpectralColor<16>;
using FSpectralColor32 = TSpectralColor<32>;

} // namespace Limx
