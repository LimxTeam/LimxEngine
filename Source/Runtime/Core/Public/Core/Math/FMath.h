/*******************************************************************************
 * 文件: FMath.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   引擎数学基础库 — 替代 <cmath> 的零 STL 依赖数学函数集
 *   提供三角函数、幂函数、取整函数、插值函数等常用数学运算
 *   所有函数使用 Float32/Float64 类型，与引擎类型系统一致
 *
 * 设计哲学:
 *   零 STL — 通过编译器内建函数 (__builtin_*) 或 CRT 前向声明实现
 *   constexpr 优先 — 尽可能在编译时求值
 *   SIMD 预留 — 后续可替换为 SIMD 实现而不改变接口
 *
 * 技术特性:
 *   - 常量: kPi, kTwoPi, kHalfPi, kInvPi, kEpsilon, kSmallNumber
 *   - 基础: Abs, Sign, Min, Max, Clamp, Saturate
 *   - 插值: Lerp, InverseLerp, SmoothStep, Step
 *   - 幂/根: Sqrt, InvSqrt, Pow, Exp, Log, Log2
 *   - 三角: Sin, Cos, Tan, ASin, ACos, ATan, ATan2
 *   - 取整: Floor, Ceil, Round, Truncate, Fmod, Frac
 *   - 角度: DegreesToRadians, RadiansToDegrees
 *   - 比较: IsNearlyEqual, IsNearlyZero, IsFinite, IsNaN
 *
 * 依赖关系:
 *   内部: Core/HAL/Platform.h, Core/HAL/PlatformTypes.h, Core/CoreMacros.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"

// ============================================================================
// CRT 数学函数前向声明 — 无需 #include <cmath>
// MSVC 编译器识别这些声明后自动优化为内建指令
// ============================================================================


extern "C"
{
    double sqrt(double x);
    double sin(double x);
    double cos(double x);
    double tan(double x);
    double asin(double x);
    double acos(double x);
    double atan(double x);
    double atan2(double y, double x);
    double pow(double base, double exponent);
    double exp(double x);
    double log(double x);
    double log2(double x);
    double floor(double x);
    double ceil(double x);
    double round(double x);
    double fmod(double x, double y);
    double fabs(double x);

    float sqrtf(float x);
    float sinf(float x);
    float cosf(float x);
    float tanf(float x);
    float asinf(float x);
    float acosf(float x);
    float atanf(float x);
    float atan2f(float y, float x);
    float powf(float base, float exponent);
    float expf(float x);
    float logf(float x);
    float log2f(float x);
    float floorf(float x);
    float ceilf(float x);
    float roundf(float x);
    float fmodf(float x, float y);
    float fabsf(float x);
}


namespace Limx
{

/// 引擎数学函数集 — 静态方法类
struct FMath
{
    // ========================================================================
    // 数学常量 (Float32)
    // ========================================================================

    static constexpr Float32 kPi            = 3.14159265358979323846f;
    static constexpr Float32 kTwoPi         = 6.28318530717958647692f;
    static constexpr Float32 kHalfPi        = 1.57079632679489661923f;
    static constexpr Float32 kInvPi         = 0.31830988618379067154f;
    static constexpr Float32 kInvTwoPi      = 0.15915494309189533577f;
    static constexpr Float32 kSqrt2         = 1.41421356237309504880f;
    static constexpr Float32 kInvSqrt2      = 0.70710678118654752440f;
    static constexpr Float32 kEuler         = 2.71828182845904523536f;
    static constexpr Float32 kGoldenRatio   = 1.61803398874989484820f;

    // 精度阈值
    static constexpr Float32 kEpsilon       = 1.192092896e-07f;  // FLT_EPSILON
    static constexpr Float32 kSmallNumber   = 1.e-8f;
    static constexpr Float32 kKindaSmall    = 1.e-4f;
    static constexpr Float32 kBigNumber     = 3.4e+38f;

    // 角度转换
    static constexpr Float32 kDegToRad      = kPi / 180.0f;
    static constexpr Float32 kRadToDeg      = 180.0f / kPi;

    // ========================================================================
    // 数学常量 (Float64)
    // ========================================================================

    static constexpr Float64 kPi64          = 3.14159265358979323846;
    static constexpr Float64 kTwoPi64       = 6.28318530717958647692;
    static constexpr Float64 kEpsilon64     = 2.2204460492503131e-16;

    // ========================================================================
    // 基础运算
    // ========================================================================

    /// 绝对值
    LIMX_NODISCARD static constexpr FORCEINLINE Float32 Abs(Float32 x)
    {
        return x >= 0.0f ? x : -x;
    }

    LIMX_NODISCARD static constexpr FORCEINLINE Float64 Abs(Float64 x)
    {
        return x >= 0.0 ? x : -x;
    }

    LIMX_NODISCARD static constexpr FORCEINLINE Int32 Abs(Int32 x)
    {
        return x >= 0 ? x : -x;
    }

    LIMX_NODISCARD static constexpr FORCEINLINE Int64 Abs(Int64 x)
    {
        return x >= 0 ? x : -x;
    }

    /// 符号 — 返回 -1, 0, 1
    LIMX_NODISCARD static constexpr FORCEINLINE Float32 Sign(Float32 x)
    {
        return (x > 0.0f) ? 1.0f : ((x < 0.0f) ? -1.0f : 0.0f);
    }

    LIMX_NODISCARD static constexpr FORCEINLINE Int32 Sign(Int32 x)
    {
        return (x > 0) ? 1 : ((x < 0) ? -1 : 0);
    }

    /// 最小值
    template<typename T>
    LIMX_NODISCARD static constexpr FORCEINLINE T Min(T a, T b)
    {
        return (a < b) ? a : b;
    }

    /// 最大值
    template<typename T>
    LIMX_NODISCARD static constexpr FORCEINLINE T Max(T a, T b)
    {
        return (a > b) ? a : b;
    }

    /// 钳制到 [minVal, maxVal] 范围
    template<typename T>
    LIMX_NODISCARD static constexpr FORCEINLINE T Clamp(T value, T minVal, T maxVal)
    {
        return Min(Max(value, minVal), maxVal);
    }

    /// 钳制到 [0, 1] 范围
    LIMX_NODISCARD static constexpr FORCEINLINE Float32 Saturate(Float32 x)
    {
        return Clamp(x, 0.0f, 1.0f);
    }

    /// 取两个值中绝对值较大的
    LIMX_NODISCARD static constexpr FORCEINLINE Float32 MaxAbs(Float32 a, Float32 b)
    {
        return Abs(a) > Abs(b) ? a : b;
    }

    // ========================================================================
    // 插值
    // ========================================================================

    /// 线性插值 — Lerp(a, b, t) = a + t * (b - a)
    LIMX_NODISCARD static constexpr FORCEINLINE Float32 Lerp(
        Float32 a, Float32 b, Float32 t)
    {
        return a + t * (b - a);
    }

    LIMX_NODISCARD static constexpr FORCEINLINE Float64 Lerp(
        Float64 a, Float64 b, Float64 t)
    {
        return a + t * (b - a);
    }

    /// 反向线性插值 — 求 t 使得 Lerp(a, b, t) = value
    LIMX_NODISCARD static constexpr FORCEINLINE Float32 InverseLerp(
        Float32 a, Float32 b, Float32 value)
    {
        return (b - a) != 0.0f ? (value - a) / (b - a) : 0.0f;
    }

    /// Hermite 平滑插值 — SmoothStep(0) = 0, SmoothStep(1) = 1
    LIMX_NODISCARD static constexpr FORCEINLINE Float32 SmoothStep(
        Float32 edge0, Float32 edge1, Float32 x)
    {
        Float32 t = Saturate(InverseLerp(edge0, edge1, x));
        return t * t * (3.0f - 2.0f * t);
    }

    /// 更平滑的 Hermite 插值 (Ken Perlin)
    LIMX_NODISCARD static constexpr FORCEINLINE Float32 SmootherStep(
        Float32 edge0, Float32 edge1, Float32 x)
    {
        Float32 t = Saturate(InverseLerp(edge0, edge1, x));
        return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    }

    /// 阶跃函数 — x < edge 返回 0，否则返回 1
    LIMX_NODISCARD static constexpr FORCEINLINE Float32 Step(
        Float32 edge, Float32 x)
    {
        return x < edge ? 0.0f : 1.0f;
    }

    // ========================================================================
    // 幂与根
    // ========================================================================

    /// 平方根
    LIMX_NODISCARD static FORCEINLINE Float32 Sqrt(Float32 x)
    {
        return sqrtf(x);
    }

    LIMX_NODISCARD static FORCEINLINE Float64 Sqrt(Float64 x)
    {
        return sqrt(x);
    }

    /// 倒数平方根 — 1.0 / sqrt(x)
    LIMX_NODISCARD static FORCEINLINE Float32 InvSqrt(Float32 x)
    {
        return 1.0f / sqrtf(x);
    }

    /// 平方
    LIMX_NODISCARD static constexpr FORCEINLINE Float32 Square(Float32 x)
    {
        return x * x;
    }

    LIMX_NODISCARD static constexpr FORCEINLINE Float64 Square(Float64 x)
    {
        return x * x;
    }

    /// 立方
    LIMX_NODISCARD static constexpr FORCEINLINE Float32 Cube(Float32 x)
    {
        return x * x * x;
    }

    /// 幂
    LIMX_NODISCARD static FORCEINLINE Float32 Pow(Float32 base, Float32 exponent)
    {
        return powf(base, exponent);
    }

    LIMX_NODISCARD static FORCEINLINE Float64 Pow(Float64 base, Float64 exponent)
    {
        return pow(base, exponent);
    }

    /// 自然指数
    LIMX_NODISCARD static FORCEINLINE Float32 Exp(Float32 x)
    {
        return expf(x);
    }

    /// 自然对数
    LIMX_NODISCARD static FORCEINLINE Float32 Log(Float32 x)
    {
        return logf(x);
    }

    /// 以 2 为底的对数
    LIMX_NODISCARD static FORCEINLINE Float32 Log2(Float32 x)
    {
        return log2f(x);
    }

    /// 以 10 为底的对数
    LIMX_NODISCARD static FORCEINLINE Float32 Log10(Float32 x)
    {
        return logf(x) * 0.4342944819032518f;  // 1 / ln(10)
    }

    // ========================================================================
    // 三角函数
    // ========================================================================

    /// 正弦 (弧度)
    LIMX_NODISCARD static FORCEINLINE Float32 Sin(Float32 radians)
    {
        return sinf(radians);
    }

    LIMX_NODISCARD static FORCEINLINE Float64 Sin(Float64 radians)
    {
        return sin(radians);
    }

    /// 余弦 (弧度)
    LIMX_NODISCARD static FORCEINLINE Float32 Cos(Float32 radians)
    {
        return cosf(radians);
    }

    LIMX_NODISCARD static FORCEINLINE Float64 Cos(Float64 radians)
    {
        return cos(radians);
    }

    /// 同时计算正弦和余弦 (某些平台可优化为单条指令)
    static FORCEINLINE void SinCos(Float32 radians,
                                    Float32& outSin, Float32& outCos)
    {
        outSin = sinf(radians);
        outCos = cosf(radians);
    }

    /// 正切 (弧度)
    LIMX_NODISCARD static FORCEINLINE Float32 Tan(Float32 radians)
    {
        return tanf(radians);
    }

    /// 反正弦 — 返回 [-π/2, π/2]
    LIMX_NODISCARD static FORCEINLINE Float32 ASin(Float32 x)
    {
        return asinf(Clamp(x, -1.0f, 1.0f));
    }

    /// 反余弦 — 返回 [0, π]
    LIMX_NODISCARD static FORCEINLINE Float32 ACos(Float32 x)
    {
        return acosf(Clamp(x, -1.0f, 1.0f));
    }

    /// 反正切 — 返回 [-π/2, π/2]
    LIMX_NODISCARD static FORCEINLINE Float32 ATan(Float32 x)
    {
        return atanf(x);
    }

    /// 双参数反正切 — 返回 [-π, π]
    LIMX_NODISCARD static FORCEINLINE Float32 ATan2(Float32 y, Float32 x)
    {
        return atan2f(y, x);
    }

    // ========================================================================
    // 取整
    // ========================================================================

    /// 向下取整
    LIMX_NODISCARD static FORCEINLINE Float32 Floor(Float32 x)
    {
        return floorf(x);
    }

    /// 向上取整
    LIMX_NODISCARD static FORCEINLINE Float32 Ceil(Float32 x)
    {
        return ceilf(x);
    }

    /// 四舍五入
    LIMX_NODISCARD static FORCEINLINE Float32 Round(Float32 x)
    {
        return roundf(x);
    }

    /// 截断 (向零取整)
    LIMX_NODISCARD static FORCEINLINE Float32 Truncate(Float32 x)
    {
        return static_cast<Float32>(static_cast<Int32>(x));
    }

    /// 向下取整为整数
    LIMX_NODISCARD static FORCEINLINE Int32 FloorToInt(Float32 x)
    {
        return static_cast<Int32>(floorf(x));
    }

    /// 向上取整为整数
    LIMX_NODISCARD static FORCEINLINE Int32 CeilToInt(Float32 x)
    {
        return static_cast<Int32>(ceilf(x));
    }

    /// 四舍五入为整数
    LIMX_NODISCARD static FORCEINLINE Int32 RoundToInt(Float32 x)
    {
        return static_cast<Int32>(roundf(x));
    }

    /// 浮点取模
    LIMX_NODISCARD static FORCEINLINE Float32 Fmod(Float32 x, Float32 y)
    {
        return fmodf(x, y);
    }

    /// 小数部分 — Frac(3.7) = 0.7, Frac(-1.3) = 0.7
    LIMX_NODISCARD static FORCEINLINE Float32 Frac(Float32 x)
    {
        return x - floorf(x);
    }

    // ========================================================================
    // 角度转换
    // ========================================================================

    /// 角度 → 弧度
    LIMX_NODISCARD static constexpr FORCEINLINE Float32 DegreesToRadians(Float32 degrees)
    {
        return degrees * kDegToRad;
    }

    /// 弧度 → 角度
    LIMX_NODISCARD static constexpr FORCEINLINE Float32 RadiansToDegrees(Float32 radians)
    {
        return radians * kRadToDeg;
    }

    /// 将角度规范化到 [0, 360) 范围
    LIMX_NODISCARD static FORCEINLINE Float32 NormalizeAngle(Float32 degrees)
    {
        degrees = Fmod(degrees, 360.0f);
        if (degrees < 0.0f)
        {
            degrees += 360.0f;
        }
        return degrees;
    }

    /// 将角度规范化到 [-180, 180) 范围
    LIMX_NODISCARD static FORCEINLINE Float32 NormalizeSignedAngle(Float32 degrees)
    {
        degrees = NormalizeAngle(degrees);
        if (degrees >= 180.0f)
        {
            degrees -= 360.0f;
        }
        return degrees;
    }

    // ========================================================================
    // 比较与检查
    // ========================================================================

    /// 近似相等比较
    LIMX_NODISCARD static constexpr FORCEINLINE bool IsNearlyEqual(
        Float32 a, Float32 b, Float32 tolerance = kSmallNumber)
    {
        return Abs(a - b) <= tolerance;
    }

    LIMX_NODISCARD static constexpr FORCEINLINE bool IsNearlyEqual(
        Float64 a, Float64 b, Float64 tolerance = 1.e-8)
    {
        return Abs(a - b) <= tolerance;
    }

    /// 近似为零
    LIMX_NODISCARD static constexpr FORCEINLINE bool IsNearlyZero(
        Float32 x, Float32 tolerance = kSmallNumber)
    {
        return Abs(x) <= tolerance;
    }

    /// 是否为有限数 (非 NaN、非 Inf)
    LIMX_NODISCARD static FORCEINLINE bool IsFinite(Float32 x)
    {
        // IEEE 754: Inf/NaN 的指数位全 1
        union { Float32 f; UInt32 u; } converter;
        converter.f = x;
        return (converter.u & 0x7F800000U) != 0x7F800000U;
    }

    /// 是否为 NaN
    LIMX_NODISCARD static FORCEINLINE bool IsNaN(Float32 x)
    {
        // NaN: 指数位全 1 且尾数非零
        union { Float32 f; UInt32 u; } converter;
        converter.f = x;
        return (converter.u & 0x7FFFFFFFU) > 0x7F800000U;
    }

    /// 是否为无穷大
    LIMX_NODISCARD static FORCEINLINE bool IsInfinite(Float32 x)
    {
        union { Float32 f; UInt32 u; } converter;
        converter.f = x;
        return (converter.u & 0x7FFFFFFFU) == 0x7F800000U;
    }

    // ========================================================================
    // 位运算辅助
    // ========================================================================

    /// 选择 — condition 为 true 返回 a，否则返回 b (无分支版本)
    LIMX_NODISCARD static constexpr FORCEINLINE Float32 Select(
        bool condition, Float32 a, Float32 b)
    {
        return condition ? a : b;
    }

    /// 将 x 映射到 [0, 1] 范围内的锯齿波
    LIMX_NODISCARD static FORCEINLINE Float32 Wrap01(Float32 x)
    {
        return Frac(x);
    }

    /// 将 x 映射到 [minVal, maxVal] 范围内 (循环)
    LIMX_NODISCARD static FORCEINLINE Float32 Wrap(
        Float32 x, Float32 minVal, Float32 maxVal)
    {
        Float32 range = maxVal - minVal;
        return minVal + Frac((x - minVal) / range) * range;
    }
};

} // namespace Limx
