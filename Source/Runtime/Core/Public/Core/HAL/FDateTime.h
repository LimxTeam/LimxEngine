/*******************************************************************************
 * 文件: FDateTime.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   日期时间类型 — 表示日历日期与时刻
 *   内部以 Int64 存储自 0001-01-01 00:00:00 UTC 以来的 100 纳秒刻度
 *   (与 Windows FILETIME / .NET DateTime 刻度体系一致)
 *   用于文件时间戳、日志时间、资产元数据等场景
 *
 * 设计哲学:
 *   刻度存储 — 100 纳秒精度，覆盖完整日历范围
 *   值语义 — 不可变值类型，所有方法返回新实例
 *   UTC 优先 — 内部始终 UTC，本地时间通过显式转换
 *
 * 技术特性:
 *   - 存储: Int64 刻度 (100 纳秒/刻度)
 *   - 工厂: Now() (当前 UTC 时间), FromComponents (年月日时分秒)
 *   - 分量: GetYear, GetMonth, GetDay, GetHour, GetMinute, GetSecond
 *   - 比较: ==, !=, <, >, <=, >=
 *   - 算术: + FTimespan, - FTimespan, - FDateTime
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/HAL/FTimespan.h
 *   外部: Windows API (GetSystemTimeAsFileTime)
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/HAL/FTimespan.h"

// Windows 时间 API 前向声明
#if LIMX_PLATFORM_WINDOWS
#ifdef _WINDOWS_
// Windows 头文件已包含 — 使用真实类型别名
using FILETIME_T = FILETIME;
// API 函数已由 Windows 头文件声明
#else
extern "C"
{
    // FILETIME 结构: 两个 DWORD
    struct FILETIME_T
    {
        unsigned long LowDateTime;
        unsigned long HighDateTime;
    };

    void __stdcall GetSystemTimeAsFileTime(FILETIME_T* lpSystemTimeAsFileTime);
}
#endif // _WINDOWS_
#endif

namespace Limx
{

/// 日期时间 — 100 纳秒刻度精度
struct FDateTime
{
    /// 每种时间单位的刻度数 (1 刻度 = 100 纳秒)
    static constexpr Int64 kTicksPerMicrosecond = 10LL;
    static constexpr Int64 kTicksPerMillisecond = 10000LL;
    static constexpr Int64 kTicksPerSecond      = 10000000LL;
    static constexpr Int64 kTicksPerMinute      = 600000000LL;
    static constexpr Int64 kTicksPerHour        = 36000000000LL;
    static constexpr Int64 kTicksPerDay         = 864000000000LL;

    /// Windows FILETIME 纪元偏移
    /// FILETIME 从 1601-01-01 开始，我们从 0001-01-01 开始
    /// 1601-01-01 距 0001-01-01 的天数 * kTicksPerDay
    static constexpr Int64 kFileTimeEpochOffset = 504911232000000000LL;

    /// Unix 纪元偏移 (1970-01-01 距 0001-01-01)
    static constexpr Int64 kUnixEpochTicks = 621355968000000000LL;

    // ========================================================================
    // 构造
    // ========================================================================

    /// 默认构造 — 零刻度 (0001-01-01)
    constexpr FDateTime() : m_Ticks(0) {}

    /// 从刻度构造
    constexpr explicit FDateTime(Int64 ticks) : m_Ticks(ticks) {}

    // ========================================================================
    // 工厂方法
    // ========================================================================

    /// 获取当前 UTC 时间
    LIMX_NODISCARD static FDateTime Now()
    {
#if LIMX_PLATFORM_WINDOWS
        FILETIME_T fileTime;
        GetSystemTimeAsFileTime(&fileTime);
#ifdef _WINDOWS_
        Int64 ticks = static_cast<Int64>(fileTime.dwLowDateTime) |
                      (static_cast<Int64>(fileTime.dwHighDateTime) << 32);
#else
        Int64 ticks = static_cast<Int64>(fileTime.LowDateTime) |
                      (static_cast<Int64>(fileTime.HighDateTime) << 32);
#endif
        // FILETIME 从 1601-01-01 开始，转换到 0001-01-01
        return FDateTime(ticks + kFileTimeEpochOffset);
#else
        return FDateTime();
#endif
    }

    /// 从日期分量构造 (UTC)
    LIMX_NODISCARD static FDateTime FromComponents(
        Int32 year, Int32 month, Int32 day,
        Int32 hour = 0, Int32 minute = 0, Int32 second = 0)
    {
        // 计算从 0001-01-01 到指定日期的天数
        Int64 totalDays = DateToDays(year, month, day);
        Int64 ticks = totalDays * kTicksPerDay +
                      static_cast<Int64>(hour) * kTicksPerHour +
                      static_cast<Int64>(minute) * kTicksPerMinute +
                      static_cast<Int64>(second) * kTicksPerSecond;
        return FDateTime(ticks);
    }

    /// 从 Unix 时间戳 (秒) 构造
    LIMX_NODISCARD static constexpr FDateTime FromUnixTimestamp(Int64 seconds)
    {
        return FDateTime(kUnixEpochTicks +
                         seconds * kTicksPerSecond);
    }

    // ========================================================================
    // 分量访问
    // ========================================================================

    /// 获取年份
    LIMX_NODISCARD Int32 GetYear() const
    {
        Int32 year, month, day;
        DaysToDate(m_Ticks / kTicksPerDay, year, month, day);
        return year;
    }

    /// 获取月份 (1-12)
    LIMX_NODISCARD Int32 GetMonth() const
    {
        Int32 year, month, day;
        DaysToDate(m_Ticks / kTicksPerDay, year, month, day);
        return month;
    }

    /// 获取日 (1-31)
    LIMX_NODISCARD Int32 GetDay() const
    {
        Int32 year, month, day;
        DaysToDate(m_Ticks / kTicksPerDay, year, month, day);
        return day;
    }

    /// 获取小时 (0-23)
    LIMX_NODISCARD Int32 GetHour() const
    {
        return static_cast<Int32>(
            (m_Ticks / kTicksPerHour) % 24);
    }

    /// 获取分钟 (0-59)
    LIMX_NODISCARD Int32 GetMinute() const
    {
        return static_cast<Int32>(
            (m_Ticks / kTicksPerMinute) % 60);
    }

    /// 获取秒 (0-59)
    LIMX_NODISCARD Int32 GetSecond() const
    {
        return static_cast<Int32>(
            (m_Ticks / kTicksPerSecond) % 60);
    }

    /// 获取毫秒 (0-999)
    LIMX_NODISCARD Int32 GetMillisecond() const
    {
        return static_cast<Int32>(
            (m_Ticks / kTicksPerMillisecond) % 1000);
    }

    /// 获取内部刻度
    LIMX_NODISCARD constexpr Int64 GetTicks() const { return m_Ticks; }

    /// 转换为 Unix 时间戳 (秒)
    LIMX_NODISCARD constexpr Int64 ToUnixTimestamp() const
    {
        return (m_Ticks - kUnixEpochTicks) / kTicksPerSecond;
    }

    // ========================================================================
    // 算术运算符
    // ========================================================================

    LIMX_NODISCARD constexpr FDateTime operator+(
        const FTimespan& span) const
    {
        return FDateTime(
            m_Ticks + span.GetTotalMicroseconds() * kTicksPerMicrosecond);
    }

    LIMX_NODISCARD constexpr FDateTime operator-(
        const FTimespan& span) const
    {
        return FDateTime(
            m_Ticks - span.GetTotalMicroseconds() * kTicksPerMicrosecond);
    }

    LIMX_NODISCARD constexpr FTimespan operator-(
        const FDateTime& other) const
    {
        Int64 tickDiff = m_Ticks - other.m_Ticks;
        return FTimespan::FromMicroseconds(
            tickDiff / kTicksPerMicrosecond);
    }

    // ========================================================================
    // 比较运算符
    // ========================================================================

    LIMX_NODISCARD constexpr bool operator==(const FDateTime& other) const
    {
        return m_Ticks == other.m_Ticks;
    }

    LIMX_NODISCARD constexpr bool operator!=(const FDateTime& other) const
    {
        return m_Ticks != other.m_Ticks;
    }

    LIMX_NODISCARD constexpr bool operator<(const FDateTime& other) const
    {
        return m_Ticks < other.m_Ticks;
    }

    LIMX_NODISCARD constexpr bool operator>(const FDateTime& other) const
    {
        return m_Ticks > other.m_Ticks;
    }

    LIMX_NODISCARD constexpr bool operator<=(const FDateTime& other) const
    {
        return m_Ticks <= other.m_Ticks;
    }

    LIMX_NODISCARD constexpr bool operator>=(const FDateTime& other) const
    {
        return m_Ticks >= other.m_Ticks;
    }

private:
    // ========================================================================
    // 日历辅助
    // ========================================================================

    /// 是否为闰年
    static constexpr bool IsLeapYear(Int32 year)
    {
        return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    }

    /// 获取某月天数
    static Int32 DaysInMonth(Int32 year, Int32 month)
    {
        static constexpr Int32 kDaysPerMonth[] =
            { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        if (month == 2 && IsLeapYear(year))
        {
            return 29;
        }
        return kDaysPerMonth[month - 1];
    }

    /// 日期转天数 (从 0001-01-01 算起)
    static Int64 DateToDays(Int32 year, Int32 month, Int32 day)
    {
        // 调整 year-1，因为从 0001 年开始
        Int32 y = year - 1;
        Int64 totalDays = static_cast<Int64>(y) * 365 +
                          y / 4 - y / 100 + y / 400;
        for (Int32 m = 1; m < month; ++m)
        {
            totalDays += DaysInMonth(year, m);
        }
        totalDays += day - 1;
        return totalDays;
    }

    /// 天数转日期
    static void DaysToDate(Int64 totalDays, Int32& outYear,
                            Int32& outMonth, Int32& outDay)
    {
        // 估算年份
        Int32 year = static_cast<Int32>(totalDays / 365) + 1;
        while (DateToDays(year + 1, 1, 1) <= totalDays)
        {
            year++;
        }

        Int64 remainingDays = totalDays - DateToDays(year, 1, 1);

        Int32 month = 1;
        while (month < 12)
        {
            Int32 daysInCurrent = DaysInMonth(year, month);
            if (remainingDays < daysInCurrent)
            {
                break;
            }
            remainingDays -= daysInCurrent;
            month++;
        }

        outYear = year;
        outMonth = month;
        outDay = static_cast<Int32>(remainingDays) + 1;
    }

    // ========================================================================
    // 成员数据
    // ========================================================================

    Int64 m_Ticks;  ///< 100 纳秒刻度 (自 0001-01-01 UTC)
};

} // namespace Limx
