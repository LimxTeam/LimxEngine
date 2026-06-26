/*******************************************************************************
 * 文件: FPlatformTime.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   高精度平台计时器 — 提供亚微秒级精度的时间测量
 *   封装 Windows QueryPerformanceCounter/QueryPerformanceFrequency
 *   用于帧计时、性能分析、动画更新等场景
 *
 * 设计哲学:
 *   静态工具类 — 所有方法为 static，无需实例化
 *   延迟初始化 — 频率值在首次调用时缓存
 *   双精度秒 — 返回 Float64 秒，避免溢出和精度损失
 *
 * 技术特性:
 *   - Seconds(): 返回自进程启动以来的秒数 (Float64)
 *   - Cycles(): 返回原始 QPC 计数
 *   - CyclesToSeconds/SecondsToCycles: 互转
 *   - GetFrequency(): QPC 频率 (每秒 tick 数)
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h
 *   外部: Windows API (QueryPerformanceCounter/Frequency)
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"

// Windows 高精度计时器前向声明
#if LIMX_PLATFORM_WINDOWS
#ifndef _WINDOWS_
extern "C"
{
    // BOOL QueryPerformanceCounter(LARGE_INTEGER* lpPerformanceCount);
    int __stdcall QueryPerformanceCounter(long long* lpPerformanceCount);
    // BOOL QueryPerformanceFrequency(LARGE_INTEGER* lpFrequency);
    int __stdcall QueryPerformanceFrequency(long long* lpFrequency);
}
#endif // _WINDOWS_
#endif

namespace Limx
{

/// 高精度平台计时器
struct FPlatformTime
{
    /// 获取 QPC 频率 (每秒 tick 数)
    LIMX_NODISCARD static Int64 GetFrequency()
    {
        static Int64 s_Frequency = InitFrequency();
        return s_Frequency;
    }

    /// 获取原始 QPC 计数
    LIMX_NODISCARD static Int64 Cycles()
    {
#if LIMX_PLATFORM_WINDOWS
        Int64 counter = 0;
#ifdef _WINDOWS_
        QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&counter));
#else
        QueryPerformanceCounter(&counter);
#endif
        return counter;
#else
        return 0;
#endif
    }

    /// 获取自进程启动以来的秒数 (Float64，亚微秒精度)
    LIMX_NODISCARD static Float64 Seconds()
    {
        return CyclesToSeconds(Cycles());
    }

    /// QPC 计数 → 秒
    LIMX_NODISCARD static Float64 CyclesToSeconds(Int64 cycles)
    {
        return static_cast<Float64>(cycles) * GetSecondsPerCycle();
    }

    /// 秒 → QPC 计数
    LIMX_NODISCARD static Int64 SecondsToCycles(Float64 seconds)
    {
        return static_cast<Int64>(seconds * static_cast<Float64>(GetFrequency()));
    }

    /// QPC 计数 → 毫秒
    LIMX_NODISCARD static Float64 CyclesToMilliseconds(Int64 cycles)
    {
        return CyclesToSeconds(cycles) * 1000.0;
    }

    /// QPC 计数 → 微秒
    LIMX_NODISCARD static Float64 CyclesToMicroseconds(Int64 cycles)
    {
        return CyclesToSeconds(cycles) * 1000000.0;
    }

    /// 每个 QPC tick 的秒数 (缓存值)
    LIMX_NODISCARD static Float64 GetSecondsPerCycle()
    {
        static Float64 s_SecondsPerCycle =
            1.0 / static_cast<Float64>(GetFrequency());
        return s_SecondsPerCycle;
    }

private:
    static Int64 InitFrequency()
    {
#if LIMX_PLATFORM_WINDOWS
        Int64 frequency = 0;
#ifdef _WINDOWS_
        QueryPerformanceFrequency(reinterpret_cast<LARGE_INTEGER*>(&frequency));
#else
        QueryPerformanceFrequency(&frequency);
#endif
        return frequency;
#else
        return 1000000000LL;  // 后备: 纳秒分辨率
#endif
    }
};

/// RAII 计时器 — 自动测量作用域内的执行时间 (秒)
class FScopedTimer
{
public:
    explicit FScopedTimer(Float64& outElapsedSeconds)
        : m_StartCycles(FPlatformTime::Cycles())
        , m_OutElapsed(outElapsedSeconds)
    {
    }

    ~FScopedTimer()
    {
        Int64 endCycles = FPlatformTime::Cycles();
        m_OutElapsed = FPlatformTime::CyclesToSeconds(
            endCycles - m_StartCycles);
    }

    // 禁止拷贝
    FScopedTimer(const FScopedTimer&) = delete;
    FScopedTimer& operator=(const FScopedTimer&) = delete;

private:
    Int64    m_StartCycles;
    Float64& m_OutElapsed;
};

} // namespace Limx
