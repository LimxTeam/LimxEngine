/*******************************************************************************
 * 文件: FTimespan.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   时间距类型 — 表示一段时间间隔（持续时长）
 *   内部以 Int64 微秒存储，支持微秒到天的全精度范围
 *   用于帧增量时间、计时器超时、动画时长等场景
 *
 * 设计哲学:
 *   微秒精度 — 内部 Int64 存储，覆盖 ±292,000 年范围
 *   值语义 — 轻量不可变值类型，可安全拷贝和算术运算
 *   工厂方法 — FromSeconds/FromMilliseconds/FromMicroseconds 显式构造
 *
 * 技术特性:
 *   - 存储: Int64 微秒 (8 字节)
 *   - 工厂: FromSeconds, FromMilliseconds, FromMicroseconds, FromMinutes, FromHours
 *   - 转换: GetTotalSeconds, GetTotalMilliseconds, GetTotalMicroseconds
 *   - 算术: +, -, *, / 运算符
 *   - 比较: ==, !=, <, >, <=, >=
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"

namespace Limx
{

/// 时间距 — 内部以微秒 (Int64) 存储
struct FTimespan
{
    // 常用时间单位的微秒数
    static constexpr Int64 kMicrosecondsPerMillisecond = 1000LL;
    static constexpr Int64 kMicrosecondsPerSecond      = 1000000LL;
    static constexpr Int64 kMicrosecondsPerMinute      = 60000000LL;
    static constexpr Int64 kMicrosecondsPerHour        = 3600000000LL;
    static constexpr Int64 kMicrosecondsPerDay         = 86400000000LL;

    // 常量
    static const FTimespan kZero;
    static const FTimespan kMaxValue;
    static const FTimespan kMinValue;

    // ========================================================================
    // 构造
    // ========================================================================

    /// 默认构造 — 零时长
    constexpr FTimespan() : m_Microseconds(0) {}

    /// 从微秒构造 (私有语义，使用工厂方法)
    constexpr explicit FTimespan(Int64 microseconds)
        : m_Microseconds(microseconds) {}

    // ========================================================================
    // 工厂方法
    // ========================================================================

    LIMX_NODISCARD static constexpr FTimespan FromMicroseconds(Int64 us)
    {
        return FTimespan(us);
    }

    LIMX_NODISCARD static constexpr FTimespan FromMilliseconds(Float64 ms)
    {
        return FTimespan(static_cast<Int64>(
            ms * static_cast<Float64>(kMicrosecondsPerMillisecond)));
    }

    LIMX_NODISCARD static constexpr FTimespan FromSeconds(Float64 seconds)
    {
        return FTimespan(static_cast<Int64>(
            seconds * static_cast<Float64>(kMicrosecondsPerSecond)));
    }

    LIMX_NODISCARD static constexpr FTimespan FromMinutes(Float64 minutes)
    {
        return FTimespan(static_cast<Int64>(
            minutes * static_cast<Float64>(kMicrosecondsPerMinute)));
    }

    LIMX_NODISCARD static constexpr FTimespan FromHours(Float64 hours)
    {
        return FTimespan(static_cast<Int64>(
            hours * static_cast<Float64>(kMicrosecondsPerHour)));
    }

    // ========================================================================
    // 转换
    // ========================================================================

    LIMX_NODISCARD constexpr Int64 GetTotalMicroseconds() const
    {
        return m_Microseconds;
    }

    LIMX_NODISCARD constexpr Float64 GetTotalMilliseconds() const
    {
        return static_cast<Float64>(m_Microseconds) /
               static_cast<Float64>(kMicrosecondsPerMillisecond);
    }

    LIMX_NODISCARD constexpr Float64 GetTotalSeconds() const
    {
        return static_cast<Float64>(m_Microseconds) /
               static_cast<Float64>(kMicrosecondsPerSecond);
    }

    LIMX_NODISCARD constexpr Float64 GetTotalMinutes() const
    {
        return static_cast<Float64>(m_Microseconds) /
               static_cast<Float64>(kMicrosecondsPerMinute);
    }

    LIMX_NODISCARD constexpr Float64 GetTotalHours() const
    {
        return static_cast<Float64>(m_Microseconds) /
               static_cast<Float64>(kMicrosecondsPerHour);
    }

    /// 是否为零时长
    LIMX_NODISCARD constexpr bool IsZero() const
    {
        return m_Microseconds == 0;
    }

    // ========================================================================
    // 算术运算符
    // ========================================================================

    LIMX_NODISCARD constexpr FTimespan operator+(const FTimespan& other) const
    {
        return FTimespan(m_Microseconds + other.m_Microseconds);
    }

    LIMX_NODISCARD constexpr FTimespan operator-(const FTimespan& other) const
    {
        return FTimespan(m_Microseconds - other.m_Microseconds);
    }

    LIMX_NODISCARD constexpr FTimespan operator*(Int64 scalar) const
    {
        return FTimespan(m_Microseconds * scalar);
    }

    LIMX_NODISCARD constexpr FTimespan operator*(Float64 scalar) const
    {
        return FTimespan(static_cast<Int64>(
            static_cast<Float64>(m_Microseconds) * scalar));
    }

    LIMX_NODISCARD constexpr FTimespan operator/(Int64 divisor) const
    {
        return FTimespan(m_Microseconds / divisor);
    }

    LIMX_NODISCARD constexpr FTimespan operator-() const
    {
        return FTimespan(-m_Microseconds);
    }

    FTimespan& operator+=(const FTimespan& other)
    {
        m_Microseconds += other.m_Microseconds;
        return *this;
    }

    FTimespan& operator-=(const FTimespan& other)
    {
        m_Microseconds -= other.m_Microseconds;
        return *this;
    }

    // ========================================================================
    // 比较运算符
    // ========================================================================

    LIMX_NODISCARD constexpr bool operator==(const FTimespan& other) const
    {
        return m_Microseconds == other.m_Microseconds;
    }

    LIMX_NODISCARD constexpr bool operator!=(const FTimespan& other) const
    {
        return m_Microseconds != other.m_Microseconds;
    }

    LIMX_NODISCARD constexpr bool operator<(const FTimespan& other) const
    {
        return m_Microseconds < other.m_Microseconds;
    }

    LIMX_NODISCARD constexpr bool operator>(const FTimespan& other) const
    {
        return m_Microseconds > other.m_Microseconds;
    }

    LIMX_NODISCARD constexpr bool operator<=(const FTimespan& other) const
    {
        return m_Microseconds <= other.m_Microseconds;
    }

    LIMX_NODISCARD constexpr bool operator>=(const FTimespan& other) const
    {
        return m_Microseconds >= other.m_Microseconds;
    }

private:
    Int64 m_Microseconds;  ///< 以微秒为单位的时间距
};

// 常量定义
inline constexpr FTimespan FTimespan::kZero     = FTimespan(0);
inline constexpr FTimespan FTimespan::kMaxValue  = FTimespan(kInt64Max);
inline constexpr FTimespan FTimespan::kMinValue  = FTimespan(kInt64Min);

} // namespace Limx
