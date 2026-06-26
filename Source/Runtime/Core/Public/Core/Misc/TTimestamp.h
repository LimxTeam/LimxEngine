/*******************************************************************************
 * 文件: TTimestamp.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   时间戳 — 高精度时间点封装
 *   以微秒为单位存储时间点，提供差值计算和比较
 *   用于性能计时、帧时间戳、事件排序、超时检测等场景
 *
 * 设计哲学:
 *   微秒精度 — 内部以 Int64 微秒存储，避免浮点误差
 *   值类型 — 轻量可拷贝，可放入容器
 *   与 FTimespan 区分 — FTimestamp 为时间点, FTimespan 为时间段
 *
 * 技术特性:
 *   - FTimestamp: 高精度时间戳
 *   - Now: 当前时刻
 *   - operator-: 计算时间差 (微秒)
 *   - ElapsedMicroseconds/Milliseconds/Seconds: 经过时间
 *   - IsAfter/IsBefore: 先后比较
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h,
 *          Core/HAL/FPlatformTime.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/HAL/FPlatformTime.h"

namespace Limx
{

/// 高精度时间戳
struct FTimestamp
{
    /// 无效时间戳值
    static constexpr Int64 kInvalidValue = -1;

    // ========================================================================
    // 构造
    // ========================================================================

    FTimestamp() : m_Microseconds(kInvalidValue) {}

    explicit FTimestamp(Int64 microseconds)
        : m_Microseconds(microseconds)
    {}

    /// 获取当前时刻
    LIMX_NODISCARD static FTimestamp Now()
    {
        return FTimestamp(static_cast<Int64>(
            FPlatformTime::Seconds() * 1e6));
    }

    /// 零时刻 (纪元起始)
    LIMX_NODISCARD static FTimestamp Zero()
    {
        return FTimestamp(0);
    }

    /// 无效时间戳
    LIMX_NODISCARD static FTimestamp Invalid()
    {
        return FTimestamp();
    }

    // ========================================================================
    // 有效性
    // ========================================================================

    LIMX_NODISCARD bool IsValid() const
    {
        return m_Microseconds != kInvalidValue;
    }

    // ========================================================================
    // 时间差
    // ========================================================================

    /// 与另一时间戳的差值 (微秒)
    LIMX_NODISCARD Int64 operator-(
        const FTimestamp& other) const
    {
        return m_Microseconds - other.m_Microseconds;
    }

    /// 从此时间戳到现在经过的微秒数
    LIMX_NODISCARD Int64 ElapsedMicroseconds() const
    {
        return FTimestamp::Now().m_Microseconds -
               m_Microseconds;
    }

    /// 从此时间戳到现在经过的毫秒数
    LIMX_NODISCARD Float64 ElapsedMilliseconds() const
    {
        return static_cast<Float64>(
            ElapsedMicroseconds()) * 0.001;
    }

    /// 从此时间戳到现在经过的秒数
    LIMX_NODISCARD Float64 ElapsedSeconds() const
    {
        return static_cast<Float64>(
            ElapsedMicroseconds()) * 1e-6;
    }

    // ========================================================================
    // 偏移
    // ========================================================================

    LIMX_NODISCARD FTimestamp AddMicroseconds(
        Int64 microseconds) const
    {
        return FTimestamp(m_Microseconds + microseconds);
    }

    LIMX_NODISCARD FTimestamp AddMilliseconds(
        Float64 milliseconds) const
    {
        return FTimestamp(m_Microseconds +
            static_cast<Int64>(milliseconds * 1000.0));
    }

    LIMX_NODISCARD FTimestamp AddSeconds(
        Float64 seconds) const
    {
        return FTimestamp(m_Microseconds +
            static_cast<Int64>(seconds * 1e6));
    }

    // ========================================================================
    // 访问
    // ========================================================================

    LIMX_NODISCARD Int64 GetMicroseconds() const
    {
        return m_Microseconds;
    }

    LIMX_NODISCARD Float64 GetSeconds() const
    {
        return static_cast<Float64>(m_Microseconds) *
               1e-6;
    }

    // ========================================================================
    // 比较
    // ========================================================================

    LIMX_NODISCARD bool IsBefore(
        const FTimestamp& other) const
    {
        return m_Microseconds < other.m_Microseconds;
    }

    LIMX_NODISCARD bool IsAfter(
        const FTimestamp& other) const
    {
        return m_Microseconds > other.m_Microseconds;
    }

    LIMX_NODISCARD bool operator==(
        const FTimestamp& other) const
    {
        return m_Microseconds == other.m_Microseconds;
    }

    LIMX_NODISCARD bool operator!=(
        const FTimestamp& other) const
    {
        return m_Microseconds != other.m_Microseconds;
    }

    LIMX_NODISCARD bool operator<(
        const FTimestamp& other) const
    {
        return m_Microseconds < other.m_Microseconds;
    }

    LIMX_NODISCARD bool operator<=(
        const FTimestamp& other) const
    {
        return m_Microseconds <= other.m_Microseconds;
    }

    LIMX_NODISCARD bool operator>(
        const FTimestamp& other) const
    {
        return m_Microseconds > other.m_Microseconds;
    }

    LIMX_NODISCARD bool operator>=(
        const FTimestamp& other) const
    {
        return m_Microseconds >= other.m_Microseconds;
    }

private:
    Int64 m_Microseconds;  ///< 微秒时间点
};

} // namespace Limx
