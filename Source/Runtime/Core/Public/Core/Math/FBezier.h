/*******************************************************************************
 * 文件: FBezier.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   贝塞尔曲线 — 二次和三次贝塞尔曲线求值
 *   支持点求值、切线求值、弧长近似、细分
 *   用于动画路径、UI 曲线编辑器、粒子轨迹、字体渲染等场景
 *
 * 设计哲学:
 *   De Casteljau — 使用 De Casteljau 算法递归求值
 *   FVector3 — 3D 控制点，2D 场景可忽略 Z 分量
 *   静态方法 — 纯数学函数，无状态
 *
 * 技术特性:
 *   - FBezier2: 二次贝塞尔 (3 个控制点)
 *   - FBezier3: 三次贝塞尔 (4 个控制点)
 *   - Evaluate: 按参数 t 求点
 *   - Tangent: 按参数 t 求切线
 *   - Split: 在参数 t 处分割曲线
 *   - ApproxLength: 弧长近似
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Math/FVector.h, Core/Math/FMath.h
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

/// 二次贝塞尔曲线 (3 个控制点: P0, P1, P2)
struct FBezier2
{
    FVector3 P0;  ///< 起点
    FVector3 P1;  ///< 控制点
    FVector3 P2;  ///< 终点

    FBezier2() = default;

    FBezier2(const FVector3& p0, const FVector3& p1,
             const FVector3& p2)
        : P0(p0), P1(p1), P2(p2)
    {
    }

    /// 按参数 t∈[0,1] 求点
    LIMX_NODISCARD FVector3 Evaluate(Float32 t) const
    {
        Float32 oneMinusT = 1.0f - t;
        // B(t) = (1-t)²P0 + 2(1-t)tP1 + t²P2
        return P0 * (oneMinusT * oneMinusT) +
               P1 * (2.0f * oneMinusT * t) +
               P2 * (t * t);
    }

    /// 按参数 t 求切线 (一阶导数)
    LIMX_NODISCARD FVector3 Tangent(Float32 t) const
    {
        Float32 oneMinusT = 1.0f - t;
        // B'(t) = 2(1-t)(P1-P0) + 2t(P2-P1)
        return (P1 - P0) * (2.0f * oneMinusT) +
               (P2 - P1) * (2.0f * t);
    }

    /// 在参数 t 处分割为两条二次贝塞尔
    void Split(Float32 t, FBezier2& outLeft,
               FBezier2& outRight) const
    {
        FVector3 a = Lerp(P0, P1, t);
        FVector3 b = Lerp(P1, P2, t);
        FVector3 c = Lerp(a, b, t);

        outLeft.P0 = P0;
        outLeft.P1 = a;
        outLeft.P2 = c;

        outRight.P0 = c;
        outRight.P1 = b;
        outRight.P2 = P2;
    }

    /// 弧长近似 (分段线性, segments 段)
    LIMX_NODISCARD Float32 ApproxLength(
        Int32 segments = 16) const
    {
        Float32 length = 0.0f;
        FVector3 prev = P0;
        for (Int32 segIndex = 1;
             segIndex <= segments; ++segIndex)
        {
            Float32 t = static_cast<Float32>(segIndex) /
                        static_cast<Float32>(segments);
            FVector3 current = Evaluate(t);
            length += (current - prev).Length();
            prev = current;
        }
        return length;
    }

private:
    static FVector3 Lerp(const FVector3& a, const FVector3& b,
                         Float32 t)
    {
        return a + (b - a) * t;
    }
};

/// 三次贝塞尔曲线 (4 个控制点: P0, P1, P2, P3)
struct FBezier3
{
    FVector3 P0;  ///< 起点
    FVector3 P1;  ///< 控制点 1
    FVector3 P2;  ///< 控制点 2
    FVector3 P3;  ///< 终点

    FBezier3() = default;

    FBezier3(const FVector3& p0, const FVector3& p1,
             const FVector3& p2, const FVector3& p3)
        : P0(p0), P1(p1), P2(p2), P3(p3)
    {
    }

    /// 按参数 t∈[0,1] 求点
    LIMX_NODISCARD FVector3 Evaluate(Float32 t) const
    {
        Float32 oneMinusT = 1.0f - t;
        Float32 tt = t * t;
        Float32 ttt = tt * t;
        Float32 u = oneMinusT;
        Float32 uu = u * u;
        Float32 uuu = uu * u;

        // B(t) = (1-t)³P0 + 3(1-t)²tP1 + 3(1-t)t²P2 + t³P3
        return P0 * uuu +
               P1 * (3.0f * uu * t) +
               P2 * (3.0f * u * tt) +
               P3 * ttt;
    }

    /// 按参数 t 求切线 (一阶导数)
    LIMX_NODISCARD FVector3 Tangent(Float32 t) const
    {
        Float32 oneMinusT = 1.0f - t;
        Float32 tt = t * t;
        Float32 uu = oneMinusT * oneMinusT;

        // B'(t) = 3(1-t)²(P1-P0) + 6(1-t)t(P2-P1) + 3t²(P3-P2)
        return (P1 - P0) * (3.0f * uu) +
               (P2 - P1) * (6.0f * oneMinusT * t) +
               (P3 - P2) * (3.0f * tt);
    }

    /// 按参数 t 求二阶导数
    LIMX_NODISCARD FVector3 SecondDerivative(Float32 t) const
    {
        Float32 oneMinusT = 1.0f - t;
        // B''(t) = 6(1-t)(P2-2P1+P0) + 6t(P3-2P2+P1)
        return (P2 - P1 * 2.0f + P0) * (6.0f * oneMinusT) +
               (P3 - P2 * 2.0f + P1) * (6.0f * t);
    }

    /// 在参数 t 处分割为两条三次贝塞尔 (De Casteljau)
    void Split(Float32 t, FBezier3& outLeft,
               FBezier3& outRight) const
    {
        FVector3 a = Lerp(P0, P1, t);
        FVector3 b = Lerp(P1, P2, t);
        FVector3 c = Lerp(P2, P3, t);
        FVector3 d = Lerp(a, b, t);
        FVector3 e = Lerp(b, c, t);
        FVector3 f = Lerp(d, e, t);

        outLeft.P0 = P0;
        outLeft.P1 = a;
        outLeft.P2 = d;
        outLeft.P3 = f;

        outRight.P0 = f;
        outRight.P1 = e;
        outRight.P2 = c;
        outRight.P3 = P3;
    }

    /// 弧长近似 (分段线性)
    LIMX_NODISCARD Float32 ApproxLength(
        Int32 segments = 32) const
    {
        Float32 length = 0.0f;
        FVector3 prev = P0;
        for (Int32 segIndex = 1;
             segIndex <= segments; ++segIndex)
        {
            Float32 t = static_cast<Float32>(segIndex) /
                        static_cast<Float32>(segments);
            FVector3 current = Evaluate(t);
            length += (current - prev).Length();
            prev = current;
        }
        return length;
    }

    /// 求最近点的参数 t (牛顿迭代近似)
    /// @param point 目标点
    /// @param iterations 迭代次数
    /// @return 最近点的参数 t
    LIMX_NODISCARD Float32 FindNearestT(
        const FVector3& point, Int32 iterations = 8) const
    {
        // 粗搜索 — 均匀采样 10 个点
        constexpr Int32 kSamples = 10;
        Float32 bestT = 0.0f;
        Float32 bestDistSq = (P0 - point).LengthSquared();

        for (Int32 sampleIndex = 1;
             sampleIndex <= kSamples; ++sampleIndex)
        {
            Float32 t = static_cast<Float32>(sampleIndex) /
                        static_cast<Float32>(kSamples);
            Float32 distSq =
                (Evaluate(t) - point).LengthSquared();
            if (distSq < bestDistSq)
            {
                bestDistSq = distSq;
                bestT = t;
            }
        }

        // 牛顿迭代细化
        for (Int32 iterIndex = 0;
             iterIndex < iterations; ++iterIndex)
        {
            FVector3 curvePoint = Evaluate(bestT);
            FVector3 diff = curvePoint - point;
            FVector3 tangent = Tangent(bestT);

            Float32 numerator = FVector3::Dot(diff, tangent);
            FVector3 secondDeriv = SecondDerivative(bestT);
            Float32 denominator =
                FVector3::Dot(tangent, tangent) +
                FVector3::Dot(diff, secondDeriv);

            if (FMath::Abs(denominator) < 1e-8f) break;

            bestT -= numerator / denominator;
            bestT = FMath::Clamp(bestT, 0.0f, 1.0f);
        }

        return bestT;
    }

private:
    static FVector3 Lerp(const FVector3& a, const FVector3& b,
                         Float32 t)
    {
        return a + (b - a) * t;
    }
};

} // namespace Limx
