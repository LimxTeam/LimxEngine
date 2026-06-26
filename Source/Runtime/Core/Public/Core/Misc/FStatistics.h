/*******************************************************************************
 * 文件: FStatistics.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   运行时性能统计 — 计数器、平均值、最小/最大值跟踪
 *   提供轻量级的数值统计收集工具
 *   用于帧时间分析、内存使用跟踪、渲染统计等场景
 *
 * 设计哲学:
 *   增量更新 — 每次 Record 只更新累加值和计数，无动态分配
 *   快照友好 — 可随时获取当前统计数据的快照
 *   零依赖 — 仅依赖基础类型
 *
 * 技术特性:
 *   - FCounter: 原子计数器 (递增/递减/重置)
 *   - FStatAccumulator: 增量统计 (计数/总和/平均/最小/最大)
 *   - FMovingAverage: 滑动窗口平均值
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"

namespace Limx
{

/// 简单计数器 — 递增/递减/重置
struct FCounter
{
    Int64 Value;

    constexpr FCounter() : Value(0) {}
    constexpr explicit FCounter(Int64 initial) : Value(initial) {}

    void Increment() { ++Value; }
    void Decrement() { --Value; }
    void Add(Int64 amount) { Value += amount; }
    void Reset() { Value = 0; }
    LIMX_NODISCARD Int64 GetValue() const { return Value; }
};

/// 增量统计累加器 — 计数/总和/平均/最小/最大
class FStatAccumulator
{
public:
    FStatAccumulator()
        : m_Count(0)
        , m_Sum(0.0)
        , m_SumSquared(0.0)
        , m_Min(1e38)
        , m_Max(-1e38)
    {
    }

    /// 记录一个样本值
    void Record(Float64 value)
    {
        ++m_Count;
        m_Sum += value;
        m_SumSquared += value * value;

        if (value < m_Min) m_Min = value;
        if (value > m_Max) m_Max = value;
    }

    /// 重置所有统计
    void Reset()
    {
        m_Count = 0;
        m_Sum = 0.0;
        m_SumSquared = 0.0;
        m_Min = 1e38;
        m_Max = -1e38;
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 样本数
    LIMX_NODISCARD UInt64 GetCount() const { return m_Count; }

    /// 总和
    LIMX_NODISCARD Float64 GetSum() const { return m_Sum; }

    /// 平均值 (无样本时返回 0)
    LIMX_NODISCARD Float64 GetAverage() const
    {
        return m_Count > 0
            ? m_Sum / static_cast<Float64>(m_Count)
            : 0.0;
    }

    /// 最小值
    LIMX_NODISCARD Float64 GetMin() const
    {
        return m_Count > 0 ? m_Min : 0.0;
    }

    /// 最大值
    LIMX_NODISCARD Float64 GetMax() const
    {
        return m_Count > 0 ? m_Max : 0.0;
    }

    /// 方差 (总体方差)
    LIMX_NODISCARD Float64 GetVariance() const
    {
        if (m_Count < 2) return 0.0;
        Float64 avg = GetAverage();
        return m_SumSquared / static_cast<Float64>(m_Count) -
               avg * avg;
    }

    /// 是否有数据
    LIMX_NODISCARD bool HasData() const { return m_Count > 0; }

private:
    UInt64  m_Count;       ///< 样本数
    Float64 m_Sum;         ///< 总和
    Float64 m_SumSquared;  ///< 平方和 (用于方差计算)
    Float64 m_Min;         ///< 最小值
    Float64 m_Max;         ///< 最大值
};

/// 滑动窗口平均值
/// @tparam WindowSize 窗口大小 (编译时常量)
template<SizeType WindowSize>
class FMovingAverage
{
    static_assert(WindowSize > 0,
        "FMovingAverage window size must be > 0");

public:
    FMovingAverage()
        : m_WriteIndex(0)
        , m_Count(0)
        , m_Sum(0.0)
    {
        for (SizeType index = 0; index < WindowSize; ++index)
        {
            m_Samples[index] = 0.0;
        }
    }

    /// 添加新样本
    void Record(Float64 value)
    {
        // 减去被覆盖的旧值
        if (m_Count >= WindowSize)
        {
            m_Sum -= m_Samples[m_WriteIndex];
        }

        m_Samples[m_WriteIndex] = value;
        m_Sum += value;

        m_WriteIndex = (m_WriteIndex + 1) % WindowSize;
        if (m_Count < WindowSize) ++m_Count;
    }

    /// 当前平均值
    LIMX_NODISCARD Float64 GetAverage() const
    {
        return m_Count > 0
            ? m_Sum / static_cast<Float64>(m_Count)
            : 0.0;
    }

    /// 最新样本值
    LIMX_NODISCARD Float64 GetLatest() const
    {
        if (m_Count == 0) return 0.0;
        SizeType lastIndex = (m_WriteIndex + WindowSize - 1) %
                              WindowSize;
        return m_Samples[lastIndex];
    }

    /// 已收集的样本数
    LIMX_NODISCARD SizeType GetCount() const { return m_Count; }

    /// 窗口是否已满
    LIMX_NODISCARD bool IsFull() const
    {
        return m_Count >= WindowSize;
    }

    /// 重置
    void Reset()
    {
        m_WriteIndex = 0;
        m_Count = 0;
        m_Sum = 0.0;
        for (SizeType index = 0; index < WindowSize; ++index)
        {
            m_Samples[index] = 0.0;
        }
    }

    /// 窗口容量
    LIMX_NODISCARD static constexpr SizeType GetWindowSize()
    {
        return WindowSize;
    }

private:
    Float64  m_Samples[WindowSize];  ///< 环形样本缓冲
    SizeType m_WriteIndex;           ///< 下一个写入位置
    SizeType m_Count;                ///< 已收集样本数
    Float64  m_Sum;                  ///< 窗口内总和
};

} // namespace Limx
