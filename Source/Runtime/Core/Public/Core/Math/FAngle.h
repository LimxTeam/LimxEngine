/*******************************************************************************
 * 文件: FAngle.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   角度封装 — 弧度/度数强类型角度，防止混用错误
 *   提供弧度与度数的强类型区分和显式转换
 *   用于旋转参数传递、相机视角、物理关节限制等需要避免单位混淆的场景
 *
 * 设计哲学:
 *   强类型防混淆 — FRadians 和 FDegrees 为不同类型，不可隐式转换
 *   显式转换 — ToRadians()/ToDegrees() 语义明确
 *   零开销 — 编译时内联，等价于裸 Float32 运算
 *
 * 技术特性:
 *   - FRadians: 弧度角度类型
 *   - FDegrees: 度数角度类型
 *   - ToRadians/ToDegrees: 显式单位转换
 *   - operator+/-/*: 角度算术
 *   - Normalize: 规范化到标准范围
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

// 前向声明
struct FDegrees;

/// 弧度角度
struct FRadians
{
    Float32 Value;

    FRadians() : Value(0.0f) {}
    explicit FRadians(Float32 radians)
        : Value(radians)
    {}

    // ========================================================================
    // 转换
    // ========================================================================

    LIMX_NODISCARD FDegrees ToDegrees() const;

    LIMX_NODISCARD Float32 GetValue() const
    {
        return Value;
    }

    // ========================================================================
    // 规范化
    // ========================================================================

    /// 规范化到 [0, 2π)
    LIMX_NODISCARD FRadians Normalize() const
    {
        Float32 v = fmodf(Value, FMath::kTwoPi);
        if (v < 0.0f) v += FMath::kTwoPi;
        return FRadians(v);
    }

    /// 规范化到 [-π, π]
    LIMX_NODISCARD FRadians NormalizeSigned() const
    {
        Float32 v = fmodf(Value, FMath::kTwoPi);
        if (v > FMath::kPi) v -= FMath::kTwoPi;
        if (v < -FMath::kPi) v += FMath::kTwoPi;
        return FRadians(v);
    }

    // ========================================================================
    // 算术
    // ========================================================================

    LIMX_NODISCARD FRadians operator+(
        const FRadians& other) const
    {
        return FRadians(Value + other.Value);
    }

    LIMX_NODISCARD FRadians operator-(
        const FRadians& other) const
    {
        return FRadians(Value - other.Value);
    }

    LIMX_NODISCARD FRadians operator*(
        Float32 scalar) const
    {
        return FRadians(Value * scalar);
    }

    LIMX_NODISCARD FRadians operator/(
        Float32 scalar) const
    {
        return FRadians(Value / scalar);
    }

    LIMX_NODISCARD FRadians operator-() const
    {
        return FRadians(-Value);
    }

    FRadians& operator+=(const FRadians& other)
    {
        Value += other.Value;
        return *this;
    }

    FRadians& operator-=(const FRadians& other)
    {
        Value -= other.Value;
        return *this;
    }

    // ========================================================================
    // 比较
    // ========================================================================

    LIMX_NODISCARD bool operator==(
        const FRadians& other) const
    {
        return FMath::Abs(Value - other.Value) <
               FMath::kEpsilon;
    }

    LIMX_NODISCARD bool operator!=(
        const FRadians& other) const
    {
        return !(*this == other);
    }

    LIMX_NODISCARD bool operator<(
        const FRadians& other) const
    {
        return Value < other.Value;
    }

    LIMX_NODISCARD bool operator<=(
        const FRadians& other) const
    {
        return Value <= other.Value;
    }

    LIMX_NODISCARD bool operator>(
        const FRadians& other) const
    {
        return Value > other.Value;
    }

    LIMX_NODISCARD bool operator>=(
        const FRadians& other) const
    {
        return Value >= other.Value;
    }
};

/// 度数角度
struct FDegrees
{
    Float32 Value;

    FDegrees() : Value(0.0f) {}
    explicit FDegrees(Float32 degrees)
        : Value(degrees)
    {}

    // ========================================================================
    // 转换
    // ========================================================================

    LIMX_NODISCARD FRadians ToRadians() const
    {
        return FRadians(Value * FMath::kDegToRad);
    }

    LIMX_NODISCARD Float32 GetValue() const
    {
        return Value;
    }

    // ========================================================================
    // 规范化
    // ========================================================================

    /// 规范化到 [0, 360)
    LIMX_NODISCARD FDegrees Normalize() const
    {
        Float32 v = fmodf(Value, 360.0f);
        if (v < 0.0f) v += 360.0f;
        return FDegrees(v);
    }

    /// 规范化到 [-180, 180]
    LIMX_NODISCARD FDegrees NormalizeSigned() const
    {
        Float32 v = fmodf(Value, 360.0f);
        if (v > 180.0f) v -= 360.0f;
        if (v < -180.0f) v += 360.0f;
        return FDegrees(v);
    }

    // ========================================================================
    // 算术
    // ========================================================================

    LIMX_NODISCARD FDegrees operator+(
        const FDegrees& other) const
    {
        return FDegrees(Value + other.Value);
    }

    LIMX_NODISCARD FDegrees operator-(
        const FDegrees& other) const
    {
        return FDegrees(Value - other.Value);
    }

    LIMX_NODISCARD FDegrees operator*(
        Float32 scalar) const
    {
        return FDegrees(Value * scalar);
    }

    LIMX_NODISCARD FDegrees operator/(
        Float32 scalar) const
    {
        return FDegrees(Value / scalar);
    }

    LIMX_NODISCARD FDegrees operator-() const
    {
        return FDegrees(-Value);
    }

    FDegrees& operator+=(const FDegrees& other)
    {
        Value += other.Value;
        return *this;
    }

    FDegrees& operator-=(const FDegrees& other)
    {
        Value -= other.Value;
        return *this;
    }

    // ========================================================================
    // 比较
    // ========================================================================

    LIMX_NODISCARD bool operator==(
        const FDegrees& other) const
    {
        return FMath::Abs(Value - other.Value) <
               FMath::kKindaSmall;
    }

    LIMX_NODISCARD bool operator!=(
        const FDegrees& other) const
    {
        return !(*this == other);
    }

    LIMX_NODISCARD bool operator<(
        const FDegrees& other) const
    {
        return Value < other.Value;
    }

    LIMX_NODISCARD bool operator<=(
        const FDegrees& other) const
    {
        return Value <= other.Value;
    }

    LIMX_NODISCARD bool operator>(
        const FDegrees& other) const
    {
        return Value > other.Value;
    }

    LIMX_NODISCARD bool operator>=(
        const FDegrees& other) const
    {
        return Value >= other.Value;
    }
};

// ============================================================================
// FRadians::ToDegrees 实现 (需 FDegrees 定义后才能写)
// ============================================================================

inline FDegrees FRadians::ToDegrees() const
{
    return FDegrees(Value * FMath::kRadToDeg);
}

// ============================================================================
// 用户定义字面量 (UDL)
// ============================================================================

/// 弧度字面量: 1.0_rad
LIMX_NODISCARD inline FRadians operator""_rad(
    long double v)
{
    return FRadians(static_cast<Float32>(v));
}

/// 度数字面量: 90.0_deg
LIMX_NODISCARD inline FDegrees operator""_deg(
    long double v)
{
    return FDegrees(static_cast<Float32>(v));
}

} // namespace Limx
