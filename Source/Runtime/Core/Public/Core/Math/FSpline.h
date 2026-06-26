/*******************************************************************************
 * 文件: FSpline.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   Catmull-Rom 样条曲线 — 经过所有控制点的光滑曲线
 *   自动计算切线，曲线穿过每一个控制点
 *   用于相机路径、动画轨迹、地形曲线、粒子路径等场景
 *
 * 设计哲学:
 *   自动切线 — Catmull-Rom 公式自动从相邻点计算切线
 *   参数化 — 按段索引 + 段内参数 t 双层寻址
 *   张力系数 — 可调 alpha 控制曲线松紧度
 *
 * 技术特性:
 *   - FSpline: Catmull-Rom 样条
 *   - AddPoint: 添加控制点
 *   - Evaluate: 按全局参数求点
 *   - EvaluateSegment: 按段求点
 *   - GetTangent: 求切线
 *   - GetTotalLength: 弧长近似
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Math/FVector.h,
 *          Core/Containers/TArray.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Math/FVector.h"
#include "Core/Math/FMath.h"
#include "Core/Containers/TArray.h"

namespace Limx
{

/// Catmull-Rom 样条曲线
class FSpline
{
public:
    FSpline() : m_Alpha(0.5f) {}

    /// 设置张力系数
    /// @param alpha 0.0=均匀, 0.5=向心(默认), 1.0=弦长
    void SetAlpha(Float32 alpha) { m_Alpha = alpha; }
    LIMX_NODISCARD Float32 GetAlpha() const { return m_Alpha; }

    // ========================================================================
    // 控制点
    // ========================================================================

    /// 添加控制点
    void AddPoint(const FVector3& point)
    {
        m_Points.Add(point);
    }

    /// 插入控制点
    void InsertPoint(SizeType index, const FVector3& point)
    {
        if (index >= m_Points.GetSize())
        {
            m_Points.Add(point);
            return;
        }
        m_Points.Add(m_Points[m_Points.GetSize() - 1]);
        for (SizeType moveIdx = m_Points.GetSize() - 2;
             moveIdx > index; --moveIdx)
        {
            m_Points[moveIdx] = m_Points[moveIdx - 1];
        }
        m_Points[index] = point;
    }

    /// 设置控制点
    void SetPoint(SizeType index, const FVector3& point)
    {
        LIMX_ASSERT(index < m_Points.GetSize());
        m_Points[index] = point;
    }

    /// 获取控制点
    LIMX_NODISCARD const FVector3& GetPoint(
        SizeType index) const
    {
        return m_Points[index];
    }

    /// 控制点数
    LIMX_NODISCARD SizeType GetPointCount() const
    {
        return m_Points.GetSize();
    }

    /// 段数 (控制点数 - 1, 至少需要 2 个点)
    LIMX_NODISCARD SizeType GetSegmentCount() const
    {
        if (m_Points.GetSize() < 2) return 0;
        return m_Points.GetSize() - 1;
    }

    /// 清空
    void Clear() { m_Points.Clear(); }

    // ========================================================================
    // 求值
    // ========================================================================

    /// 按段和段内参数求点
    /// @param segmentIndex 段索引 [0, GetSegmentCount()-1]
    /// @param t 段内参数 [0, 1]
    LIMX_NODISCARD FVector3 EvaluateSegment(
        SizeType segmentIndex, Float32 t) const
    {
        LIMX_ASSERT(m_Points.GetSize() >= 2);
        LIMX_ASSERT(segmentIndex < GetSegmentCount());

        // 获取 4 个控制点 (首尾段需要虚拟延伸点)
        FVector3 p0 = GetClampedPoint(
            static_cast<Int64>(segmentIndex) - 1);
        FVector3 p1 = m_Points[segmentIndex];
        FVector3 p2 = m_Points[segmentIndex + 1];
        FVector3 p3 = GetClampedPoint(
            static_cast<Int64>(segmentIndex) + 2);

        return CatmullRom(p0, p1, p2, p3, t);
    }

    /// 按全局参数 u∈[0,1] 求点
    LIMX_NODISCARD FVector3 Evaluate(Float32 u) const
    {
        if (m_Points.GetSize() < 2) return FVector3();

        SizeType segCount = GetSegmentCount();
        Float32 scaled = u * static_cast<Float32>(segCount);

        SizeType segIdx = static_cast<SizeType>(scaled);
        if (segIdx >= segCount) segIdx = segCount - 1;

        Float32 localT = scaled - static_cast<Float32>(segIdx);
        return EvaluateSegment(segIdx, localT);
    }

    /// 按段和段内参数求切线
    LIMX_NODISCARD FVector3 GetTangent(
        SizeType segmentIndex, Float32 t) const
    {
        LIMX_ASSERT(m_Points.GetSize() >= 2);
        LIMX_ASSERT(segmentIndex < GetSegmentCount());

        FVector3 p0 = GetClampedPoint(
            static_cast<Int64>(segmentIndex) - 1);
        FVector3 p1 = m_Points[segmentIndex];
        FVector3 p2 = m_Points[segmentIndex + 1];
        FVector3 p3 = GetClampedPoint(
            static_cast<Int64>(segmentIndex) + 2);

        return CatmullRomTangent(p0, p1, p2, p3, t);
    }

    /// 总弧长近似
    LIMX_NODISCARD Float32 GetTotalLength(
        Int32 samplesPerSegment = 16) const
    {
        if (m_Points.GetSize() < 2) return 0.0f;

        Float32 totalLength = 0.0f;
        SizeType segCount = GetSegmentCount();

        for (SizeType segIdx = 0;
             segIdx < segCount; ++segIdx)
        {
            FVector3 prev = EvaluateSegment(segIdx, 0.0f);
            for (Int32 sampleIdx = 1;
                 sampleIdx <= samplesPerSegment; ++sampleIdx)
            {
                Float32 t = static_cast<Float32>(sampleIdx) /
                            static_cast<Float32>(
                                samplesPerSegment);
                FVector3 current = EvaluateSegment(segIdx, t);
                totalLength += (current - prev).Length();
                prev = current;
            }
        }

        return totalLength;
    }

private:
    /// 获取夹持的控制点 (首尾外延)
    LIMX_NODISCARD FVector3 GetClampedPoint(
        Int64 index) const
    {
        if (index < 0) return m_Points[0];
        if (index >= static_cast<Int64>(m_Points.GetSize()))
        {
            return m_Points[m_Points.GetSize() - 1];
        }
        return m_Points[static_cast<SizeType>(index)];
    }

    /// Catmull-Rom 插值
    static FVector3 CatmullRom(
        const FVector3& p0, const FVector3& p1,
        const FVector3& p2, const FVector3& p3, Float32 t)
    {
        Float32 tt = t * t;
        Float32 ttt = tt * t;

        // 标准 Catmull-Rom 矩阵 (alpha=0.5)
        FVector3 result =
            p0 * (-0.5f * ttt + tt - 0.5f * t) +
            p1 * (1.5f * ttt - 2.5f * tt + 1.0f) +
            p2 * (-1.5f * ttt + 2.0f * tt + 0.5f * t) +
            p3 * (0.5f * ttt - 0.5f * tt);

        return result;
    }

    /// Catmull-Rom 切线 (一阶导数)
    static FVector3 CatmullRomTangent(
        const FVector3& p0, const FVector3& p1,
        const FVector3& p2, const FVector3& p3, Float32 t)
    {
        Float32 tt = t * t;

        FVector3 result =
            p0 * (-1.5f * tt + 2.0f * t - 0.5f) +
            p1 * (4.5f * tt - 5.0f * t) +
            p2 * (-4.5f * tt + 4.0f * t + 0.5f) +
            p3 * (1.5f * tt - 1.0f * t);

        return result;
    }

    TArray<FVector3> m_Points;  ///< 控制点列表
    Float32          m_Alpha;   ///< 张力系数
};

} // namespace Limx
