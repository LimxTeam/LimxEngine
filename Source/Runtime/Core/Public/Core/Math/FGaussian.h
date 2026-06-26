/*******************************************************************************
 * 文件: FGaussian.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   高斯分布 — 1D/2D 高斯函数与权重计算
 *   提供高斯核计算、采样权重生成、双边权重等操作
 *   用于图像模糊滤波、重要性采样权重、抗锯齿、深度感知模糊等
 *
 * 设计哲学:
 *   数学工具集 — 静态方法，无状态
 *   参数化 — μ/σ 均可配置，不硬编码
 *   精度控制 — 可选截断半径
 *
 * 技术特性:
 *   - FGaussian: 高斯分布工具集
 *   - Evaluate1D: 1D 高斯函数值
 *   - Evaluate2D: 2D 各向同性/各向异性高斯
 *   - ComputeWeights: 生成 N 点高斯核权重 (归一化)
 *   - BilateralWeight: 双边滤波空间权重
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

/// 高斯分布工具集
struct FGaussian
{
    // ========================================================================
    // 1D 高斯
    // ========================================================================

    /// 1D 高斯函数值
    /// @param x 输入值
    /// @param mu 均值
    /// @param sigma 标准差
    LIMX_NODISCARD static Float32 Evaluate1D(
        Float32 x,
        Float32 mu = 0.0f,
        Float32 sigma = 1.0f)
    {
        LIMX_ASSERT(sigma > 0.0f);
        Float32 delta = (x - mu) / sigma;
        return FMath::Exp(-0.5f * delta * delta);
    }

    /// 归一化 1D 高斯 (积分=1)
    LIMX_NODISCARD static Float32 Evaluate1DNormalized(
        Float32 x,
        Float32 mu = 0.0f,
        Float32 sigma = 1.0f)
    {
        LIMX_ASSERT(sigma > 0.0f);
        constexpr Float32 kInvSqrt2Pi =
            0.39894228040143267793f;
        Float32 delta = (x - mu) / sigma;
        return (kInvSqrt2Pi / sigma) *
               FMath::Exp(-0.5f * delta * delta);
    }

    // ========================================================================
    // 2D 高斯
    // ========================================================================

    /// 2D 各向同性高斯 (sigma 在两个方向相同)
    LIMX_NODISCARD static Float32 Evaluate2D(
        Float32 x, Float32 y,
        Float32 sigma = 1.0f)
    {
        LIMX_ASSERT(sigma > 0.0f);
        Float32 distSq = x * x + y * y;
        Float32 twoSigmaSq = 2.0f * sigma * sigma;
        return FMath::Exp(-distSq / twoSigmaSq);
    }

    /// 2D 各向异性高斯 (σX, σY 独立)
    LIMX_NODISCARD static Float32 Evaluate2DAnisotropic(
        Float32 x, Float32 y,
        Float32 sigmaX, Float32 sigmaY)
    {
        LIMX_ASSERT(sigmaX > 0.0f && sigmaY > 0.0f);
        Float32 dx = x / sigmaX;
        Float32 dy = y / sigmaY;
        return FMath::Exp(-0.5f * (dx * dx + dy * dy));
    }

    // ========================================================================
    // 核权重生成
    // ========================================================================

    /// 生成 N 点 1D 高斯核权重 (归一化至总和=1)
    /// @param outWeights 输出权重数组 (长度必须 >= count)
    /// @param count 核点数 (建议奇数)
    /// @param sigma 标准差
    static void ComputeWeights(Float32* outWeights,
                                Int32 count,
                                Float32 sigma = 1.0f)
    {
        LIMX_ASSERT(outWeights != nullptr);
        LIMX_ASSERT(count > 0);
        LIMX_ASSERT(sigma > 0.0f);

        Int32 halfCount = count / 2;
        Float32 sum = 0.0f;

        for (Int32 weightIdx = 0;
             weightIdx < count; ++weightIdx)
        {
            Float32 x = static_cast<Float32>(
                weightIdx - halfCount);
            outWeights[weightIdx] =
                Evaluate1D(x, 0.0f, sigma);
            sum += outWeights[weightIdx];
        }

        // 归一化
        if (sum > 1e-8f)
        {
            Float32 invSum = 1.0f / sum;
            for (Int32 weightIdx = 0;
                 weightIdx < count; ++weightIdx)
            {
                outWeights[weightIdx] *= invSum;
            }
        }
    }

    // ========================================================================
    // 双边权重
    // ========================================================================

    /// 双边滤波空间权重 (像素距离)
    LIMX_NODISCARD static Float32 BilateralSpatial(
        Float32 distSq, Float32 sigma)
    {
        LIMX_ASSERT(sigma > 0.0f);
        return FMath::Exp(
            -distSq / (2.0f * sigma * sigma));
    }

    /// 双边滤波值域权重 (像素差异)
    LIMX_NODISCARD static Float32 BilateralRange(
        Float32 valueDiff, Float32 sigma)
    {
        LIMX_ASSERT(sigma > 0.0f);
        return FMath::Exp(
            -valueDiff * valueDiff /
            (2.0f * sigma * sigma));
    }

    // ========================================================================
    // 分布属性
    // ========================================================================

    /// 从 Sigma 计算截断半径 (覆盖 nSigma 个标准差)
    LIMX_NODISCARD static Float32 GetTruncationRadius(
        Float32 sigma, Float32 nSigma = 3.0f)
    {
        return sigma * nSigma;
    }

    /// CDF (累积分布函数) — 使用误差函数近似
    LIMX_NODISCARD static Float32 CDF(
        Float32 x, Float32 mu, Float32 sigma)
    {
        LIMX_ASSERT(sigma > 0.0f);
        Float32 z = (x - mu) / (sigma * FMath::kSqrt2);
        return 0.5f * (1.0f + ErfApprox(z));
    }

private:
    /// Abramowitz & Stegun 误差函数近似 (最大误差 1.5e-7)
    LIMX_NODISCARD static Float32 ErfApprox(Float32 x)
    {
        Float32 sign = (x >= 0.0f) ? 1.0f : -1.0f;
        x = FMath::Abs(x);

        constexpr Float32 a1 =  0.254829592f;
        constexpr Float32 a2 = -0.284496736f;
        constexpr Float32 a3 =  1.421413741f;
        constexpr Float32 a4 = -1.453152027f;
        constexpr Float32 a5 =  1.061405429f;
        constexpr Float32 p  =  0.3275911f;

        Float32 t = 1.0f / (1.0f + p * x);
        Float32 poly = t * (a1 + t * (a2 + t * (a3 +
                       t * (a4 + t * a5))));
        return sign *
               (1.0f - poly * FMath::Exp(-x * x));
    }
};

} // namespace Limx
