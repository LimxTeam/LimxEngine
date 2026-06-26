/*******************************************************************************
 * 文件: TFrequencyCounter.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   频率计数器 — 按帧/按次数节流的触发器
 *   每 N 次调用触发一次回调，用于降低高频操作的执行频率
 *   用于统计采样、调试输出节流、定期清理、心跳检测等场景
 *
 * 设计哲学:
 *   模板触发间隔 — 编译时或运行时可选触发频率
 *   无回调模式 — 也可手动 Tick() + ShouldFire() 检查
 *   可重置 — Reset() 从头计数
 *
 * 技术特性:
 *   - TFrequencyCounter<Interval>: 固定间隔编译时版本
 *   - FFrequencyCounter: 运行时间隔版本
 *   - Tick: 计数一次
 *   - ShouldFire: 是否到达触发点
 *   - GetCount: 总计数
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Templates/TFunction.h"

namespace Limx
{

/// 固定间隔频率计数器 (编译时间隔)
/// @tparam Interval 触发间隔 (每 Interval 次 Tick 触发一次)
template<UInt32 Interval>
class TFrequencyCounter
{
    static_assert(Interval > 0,
        "Interval must be > 0");

public:
    TFrequencyCounter() : m_Count(0) {}

    // ========================================================================
    // 计数
    // ========================================================================

    /// 计数一次
    /// @return 是否触发 (每 Interval 次返回 true 一次)
    bool Tick()
    {
        ++m_Count;
        return (m_Count % Interval) == 0;
    }

    /// 计数并调用回调
    template<typename Callback>
    void Tick(Callback&& callback)
    {
        if (Tick())
        {
            callback();
        }
    }

    // ========================================================================
    // 查询
    // ========================================================================

    LIMX_NODISCARD bool ShouldFire() const
    {
        return m_Count > 0 && (m_Count % Interval) == 0;
    }

    LIMX_NODISCARD UInt32 GetCount() const
    {
        return m_Count;
    }

    LIMX_NODISCARD UInt32 GetCountSinceLast() const
    {
        return m_Count % Interval;
    }

    LIMX_NODISCARD static constexpr UInt32 GetInterval()
    {
        return Interval;
    }

    // ========================================================================
    // 重置
    // ========================================================================

    void Reset() { m_Count = 0; }

    /// 重置到指定偏移 (可控制初始相位)
    void ResetWithOffset(UInt32 offset)
    {
        m_Count = offset % Interval;
    }

private:
    UInt32 m_Count;  ///< 总计数
};

/// 运行时间隔频率计数器
class FFrequencyCounter
{
public:
    explicit FFrequencyCounter(UInt32 interval = 1)
        : m_Interval(interval)
        , m_Count(0)
    {
        LIMX_ASSERT(interval > 0);
    }

    // ========================================================================
    // 计数
    // ========================================================================

    bool Tick()
    {
        ++m_Count;
        return (m_Count % m_Interval) == 0;
    }

    template<typename Callback>
    void Tick(Callback&& callback)
    {
        if (Tick())
        {
            callback();
        }
    }

    // ========================================================================
    // 查询
    // ========================================================================

    LIMX_NODISCARD bool ShouldFire() const
    {
        return m_Count > 0 && (m_Count % m_Interval) == 0;
    }

    LIMX_NODISCARD UInt32 GetCount() const
    {
        return m_Count;
    }

    LIMX_NODISCARD UInt32 GetInterval() const
    {
        return m_Interval;
    }

    // ========================================================================
    // 修改
    // ========================================================================

    void SetInterval(UInt32 interval)
    {
        LIMX_ASSERT(interval > 0);
        m_Interval = interval;
    }

    void Reset() { m_Count = 0; }

private:
    UInt32 m_Interval;  ///< 触发间隔
    UInt32 m_Count;     ///< 总计数
};

/// 常用别名
using FEveryFrame      = TFrequencyCounter<1>;
using FEvery2Frames    = TFrequencyCounter<2>;
using FEvery4Frames    = TFrequencyCounter<4>;
using FEvery8Frames    = TFrequencyCounter<8>;
using FEvery16Frames   = TFrequencyCounter<16>;
using FEvery64Frames   = TFrequencyCounter<64>;
using FEvery128Frames  = TFrequencyCounter<128>;

} // namespace Limx
