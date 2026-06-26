/*******************************************************************************
 * 文件: FVector.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   引擎向量类型 — 2D/3D/4D 浮点向量
 *   提供向量算术、点积、叉积、归一化、投影等几何运算
 *   作为渲染管线中最基础的数学类型
 *
 * 设计哲学:
 *   值语义 — 向量是不可变语义的轻量值类型
 *   运算符重载 — 支持 +, -, *, / 及其赋值版本
 *   constexpr — 尽可能编译时求值
 *   SIMD 预留 — 数据布局兼容 SSE/AVX，后续可无缝切换
 *
 * 技术特性:
 *   - FVector2: 2D 向量 (x, y) — UV 坐标、屏幕空间
 *   - FVector3: 3D 向量 (x, y, z) — 位置、方向、法线
 *   - FVector4: 4D 向量 (x, y, z, w) — 齐次坐标、颜色 RGBA
 *   - 点积: Dot(a, b)
 *   - 叉积: Cross(a, b) (仅 FVector3)
 *   - 归一化: Normalize(), GetSafeNormal()
 *   - 距离: Distance(), DistanceSquared()
 *
 * 依赖关系:
 *   内部: Core/Math/FMath.h
 *
 ******************************************************************************/

#pragma once

#include "Core/Math/FMath.h"

namespace Limx
{

// ============================================================================
// FVector2 — 2D 向量
// ============================================================================

struct FVector2
{
    Float32 X;
    Float32 Y;

    // 常用常量
    static const FVector2 kZero;
    static const FVector2 kOne;
    static const FVector2 kUnitX;
    static const FVector2 kUnitY;

    // 构造
    constexpr FVector2() : X(0.0f), Y(0.0f) {}
    constexpr FVector2(Float32 inX, Float32 inY) : X(inX), Y(inY) {}
    constexpr explicit FVector2(Float32 scalar) : X(scalar), Y(scalar) {}

    // 算术运算符
    LIMX_NODISCARD constexpr FVector2 operator+(const FVector2& other) const
    {
        return FVector2(X + other.X, Y + other.Y);
    }

    LIMX_NODISCARD constexpr FVector2 operator-(const FVector2& other) const
    {
        return FVector2(X - other.X, Y - other.Y);
    }

    LIMX_NODISCARD constexpr FVector2 operator*(Float32 scalar) const
    {
        return FVector2(X * scalar, Y * scalar);
    }

    LIMX_NODISCARD constexpr FVector2 operator/(Float32 scalar) const
    {
        Float32 inv = 1.0f / scalar;
        return FVector2(X * inv, Y * inv);
    }

    LIMX_NODISCARD constexpr FVector2 operator*(const FVector2& other) const
    {
        return FVector2(X * other.X, Y * other.Y);
    }

    LIMX_NODISCARD constexpr FVector2 operator-() const
    {
        return FVector2(-X, -Y);
    }

    // 赋值运算符
    FVector2& operator+=(const FVector2& other) { X += other.X; Y += other.Y; return *this; }
    FVector2& operator-=(const FVector2& other) { X -= other.X; Y -= other.Y; return *this; }
    FVector2& operator*=(Float32 scalar) { X *= scalar; Y *= scalar; return *this; }
    FVector2& operator/=(Float32 scalar) { Float32 inv = 1.0f / scalar; X *= inv; Y *= inv; return *this; }

    // 比较
    LIMX_NODISCARD constexpr bool operator==(const FVector2& other) const
    {
        return X == other.X && Y == other.Y;
    }

    LIMX_NODISCARD constexpr bool operator!=(const FVector2& other) const
    {
        return !(*this == other);
    }

    // 几何运算
    LIMX_NODISCARD constexpr Float32 LengthSquared() const
    {
        return X * X + Y * Y;
    }

    LIMX_NODISCARD Float32 Length() const
    {
        return FMath::Sqrt(LengthSquared());
    }

    LIMX_NODISCARD FVector2 GetSafeNormal(Float32 tolerance = FMath::kSmallNumber) const
    {
        Float32 lenSq = LengthSquared();
        if (lenSq > tolerance)
        {
            Float32 invLen = FMath::InvSqrt(lenSq);
            return FVector2(X * invLen, Y * invLen);
        }
        return kZero;
    }

    void Normalize(Float32 tolerance = FMath::kSmallNumber)
    {
        *this = GetSafeNormal(tolerance);
    }

    LIMX_NODISCARD bool IsNearlyZero(Float32 tolerance = FMath::kSmallNumber) const
    {
        return FMath::Abs(X) <= tolerance && FMath::Abs(Y) <= tolerance;
    }

    LIMX_NODISCARD bool Equals(const FVector2& other,
                                Float32 tolerance = FMath::kSmallNumber) const
    {
        return FMath::Abs(X - other.X) <= tolerance &&
               FMath::Abs(Y - other.Y) <= tolerance;
    }

    // 静态函数
    LIMX_NODISCARD static constexpr Float32 Dot(const FVector2& a, const FVector2& b)
    {
        return a.X * b.X + a.Y * b.Y;
    }

    /// 2D 叉积 — 返回标量 (z 分量)
    LIMX_NODISCARD static constexpr Float32 Cross(const FVector2& a, const FVector2& b)
    {
        return a.X * b.Y - a.Y * b.X;
    }

    LIMX_NODISCARD static Float32 Distance(const FVector2& a, const FVector2& b)
    {
        return (a - b).Length();
    }

    LIMX_NODISCARD static constexpr Float32 DistanceSquared(const FVector2& a, const FVector2& b)
    {
        return (a - b).LengthSquared();
    }

    LIMX_NODISCARD static constexpr FVector2 Lerp(const FVector2& a, const FVector2& b, Float32 t)
    {
        return FVector2(FMath::Lerp(a.X, b.X, t), FMath::Lerp(a.Y, b.Y, t));
    }
};

// 常量定义
inline constexpr FVector2 FVector2::kZero  = FVector2(0.0f, 0.0f);
inline constexpr FVector2 FVector2::kOne   = FVector2(1.0f, 1.0f);
inline constexpr FVector2 FVector2::kUnitX = FVector2(1.0f, 0.0f);
inline constexpr FVector2 FVector2::kUnitY = FVector2(0.0f, 1.0f);

// 标量 * 向量
LIMX_NODISCARD constexpr FVector2 operator*(Float32 scalar, const FVector2& v)
{
    return v * scalar;
}

// ============================================================================
// FVector3 — 3D 向量
// ============================================================================

struct FVector3
{
    Float32 X;
    Float32 Y;
    Float32 Z;

    // 常用常量
    static const FVector3 kZero;
    static const FVector3 kOne;
    static const FVector3 kUnitX;    // 右方
    static const FVector3 kUnitY;    // 上方
    static const FVector3 kUnitZ;    // 前方
    static const FVector3 kUp;
    static const FVector3 kForward;
    static const FVector3 kRight;

    // 构造
    constexpr FVector3() : X(0.0f), Y(0.0f), Z(0.0f) {}
    constexpr FVector3(Float32 inX, Float32 inY, Float32 inZ)
        : X(inX), Y(inY), Z(inZ) {}
    constexpr explicit FVector3(Float32 scalar) : X(scalar), Y(scalar), Z(scalar) {}
    constexpr FVector3(const FVector2& xy, Float32 inZ)
        : X(xy.X), Y(xy.Y), Z(inZ) {}

    // 算术运算符
    LIMX_NODISCARD constexpr FVector3 operator+(const FVector3& other) const
    {
        return FVector3(X + other.X, Y + other.Y, Z + other.Z);
    }

    LIMX_NODISCARD constexpr FVector3 operator-(const FVector3& other) const
    {
        return FVector3(X - other.X, Y - other.Y, Z - other.Z);
    }

    LIMX_NODISCARD constexpr FVector3 operator*(Float32 scalar) const
    {
        return FVector3(X * scalar, Y * scalar, Z * scalar);
    }

    LIMX_NODISCARD constexpr FVector3 operator/(Float32 scalar) const
    {
        Float32 inv = 1.0f / scalar;
        return FVector3(X * inv, Y * inv, Z * inv);
    }

    LIMX_NODISCARD constexpr FVector3 operator*(const FVector3& other) const
    {
        return FVector3(X * other.X, Y * other.Y, Z * other.Z);
    }

    LIMX_NODISCARD constexpr FVector3 operator-() const
    {
        return FVector3(-X, -Y, -Z);
    }

    // 赋值运算符
    FVector3& operator+=(const FVector3& other) { X += other.X; Y += other.Y; Z += other.Z; return *this; }
    FVector3& operator-=(const FVector3& other) { X -= other.X; Y -= other.Y; Z -= other.Z; return *this; }
    FVector3& operator*=(Float32 scalar) { X *= scalar; Y *= scalar; Z *= scalar; return *this; }
    FVector3& operator/=(Float32 scalar) { Float32 inv = 1.0f / scalar; X *= inv; Y *= inv; Z *= inv; return *this; }

    // 比较
    LIMX_NODISCARD constexpr bool operator==(const FVector3& other) const
    {
        return X == other.X && Y == other.Y && Z == other.Z;
    }

    LIMX_NODISCARD constexpr bool operator!=(const FVector3& other) const
    {
        return !(*this == other);
    }

    // 下标访问
    LIMX_NODISCARD FORCEINLINE Float32& operator[](Int32 index)
    {
        LIMX_ASSERT(index >= 0 && index < 3);
        return (&X)[index];
    }

    LIMX_NODISCARD FORCEINLINE Float32 operator[](Int32 index) const
    {
        LIMX_ASSERT(index >= 0 && index < 3);
        return (&X)[index];
    }

    // 几何运算
    LIMX_NODISCARD constexpr Float32 LengthSquared() const
    {
        return X * X + Y * Y + Z * Z;
    }

    LIMX_NODISCARD Float32 Length() const
    {
        return FMath::Sqrt(LengthSquared());
    }

    LIMX_NODISCARD FVector3 GetSafeNormal(Float32 tolerance = FMath::kSmallNumber) const
    {
        Float32 lenSq = LengthSquared();
        if (lenSq > tolerance)
        {
            Float32 invLen = FMath::InvSqrt(lenSq);
            return FVector3(X * invLen, Y * invLen, Z * invLen);
        }
        return kZero;
    }

    void Normalize(Float32 tolerance = FMath::kSmallNumber)
    {
        *this = GetSafeNormal(tolerance);
    }

    LIMX_NODISCARD bool IsNormalized(Float32 tolerance = 0.01f) const
    {
        return FMath::Abs(LengthSquared() - 1.0f) < tolerance;
    }

    LIMX_NODISCARD bool IsNearlyZero(Float32 tolerance = FMath::kSmallNumber) const
    {
        return FMath::Abs(X) <= tolerance &&
               FMath::Abs(Y) <= tolerance &&
               FMath::Abs(Z) <= tolerance;
    }

    LIMX_NODISCARD bool Equals(const FVector3& other,
                                Float32 tolerance = FMath::kSmallNumber) const
    {
        return FMath::Abs(X - other.X) <= tolerance &&
               FMath::Abs(Y - other.Y) <= tolerance &&
               FMath::Abs(Z - other.Z) <= tolerance;
    }

    /// 投影到另一向量上的分量
    LIMX_NODISCARD FVector3 ProjectOnTo(const FVector3& direction) const
    {
        Float32 lenSq = direction.LengthSquared();
        if (lenSq < FMath::kSmallNumber)
        {
            return kZero;
        }
        return direction * (Dot(*this, direction) / lenSq);
    }

    /// 在平面上的投影 (平面法线)
    LIMX_NODISCARD FVector3 ProjectOnToPlane(const FVector3& planeNormal) const
    {
        return *this - ProjectOnTo(planeNormal);
    }

    /// 沿法线反射
    LIMX_NODISCARD FVector3 Reflect(const FVector3& normal) const
    {
        return *this - normal * (2.0f * Dot(*this, normal));
    }

    // 静态函数
    LIMX_NODISCARD static constexpr Float32 Dot(const FVector3& a, const FVector3& b)
    {
        return a.X * b.X + a.Y * b.Y + a.Z * b.Z;
    }

    LIMX_NODISCARD static constexpr FVector3 Cross(const FVector3& a, const FVector3& b)
    {
        return FVector3(
            a.Y * b.Z - a.Z * b.Y,
            a.Z * b.X - a.X * b.Z,
            a.X * b.Y - a.Y * b.X
        );
    }

    LIMX_NODISCARD static Float32 Distance(const FVector3& a, const FVector3& b)
    {
        return (a - b).Length();
    }

    LIMX_NODISCARD static constexpr Float32 DistanceSquared(const FVector3& a, const FVector3& b)
    {
        return (a - b).LengthSquared();
    }

    LIMX_NODISCARD static constexpr FVector3 Lerp(const FVector3& a, const FVector3& b, Float32 t)
    {
        return FVector3(
            FMath::Lerp(a.X, b.X, t),
            FMath::Lerp(a.Y, b.Y, t),
            FMath::Lerp(a.Z, b.Z, t)
        );
    }

    LIMX_NODISCARD static constexpr FVector3 Min(const FVector3& a, const FVector3& b)
    {
        return FVector3(FMath::Min(a.X, b.X), FMath::Min(a.Y, b.Y), FMath::Min(a.Z, b.Z));
    }

    LIMX_NODISCARD static constexpr FVector3 Max(const FVector3& a, const FVector3& b)
    {
        return FVector3(FMath::Max(a.X, b.X), FMath::Max(a.Y, b.Y), FMath::Max(a.Z, b.Z));
    }

    LIMX_NODISCARD static constexpr FVector3 Clamp(const FVector3& value,
                                                     const FVector3& minVal,
                                                     const FVector3& maxVal)
    {
        return FVector3(
            FMath::Clamp(value.X, minVal.X, maxVal.X),
            FMath::Clamp(value.Y, minVal.Y, maxVal.Y),
            FMath::Clamp(value.Z, minVal.Z, maxVal.Z)
        );
    }
};

// 常量定义
inline constexpr FVector3 FVector3::kZero    = FVector3(0.0f, 0.0f, 0.0f);
inline constexpr FVector3 FVector3::kOne     = FVector3(1.0f, 1.0f, 1.0f);
inline constexpr FVector3 FVector3::kUnitX   = FVector3(1.0f, 0.0f, 0.0f);
inline constexpr FVector3 FVector3::kUnitY   = FVector3(0.0f, 1.0f, 0.0f);
inline constexpr FVector3 FVector3::kUnitZ   = FVector3(0.0f, 0.0f, 1.0f);
inline constexpr FVector3 FVector3::kUp      = FVector3(0.0f, 1.0f, 0.0f);
inline constexpr FVector3 FVector3::kForward = FVector3(0.0f, 0.0f, 1.0f);
inline constexpr FVector3 FVector3::kRight   = FVector3(1.0f, 0.0f, 0.0f);

LIMX_NODISCARD constexpr FVector3 operator*(Float32 scalar, const FVector3& v)
{
    return v * scalar;
}

// ============================================================================
// FVector4 — 4D 向量 (齐次坐标 / RGBA)
// ============================================================================

struct FVector4
{
    Float32 X;
    Float32 Y;
    Float32 Z;
    Float32 W;

    static const FVector4 kZero;
    static const FVector4 kOne;

    // 构造
    constexpr FVector4() : X(0.0f), Y(0.0f), Z(0.0f), W(0.0f) {}
    constexpr FVector4(Float32 inX, Float32 inY, Float32 inZ, Float32 inW)
        : X(inX), Y(inY), Z(inZ), W(inW) {}
    constexpr explicit FVector4(Float32 scalar)
        : X(scalar), Y(scalar), Z(scalar), W(scalar) {}
    constexpr FVector4(const FVector3& xyz, Float32 inW)
        : X(xyz.X), Y(xyz.Y), Z(xyz.Z), W(inW) {}

    // 算术运算符
    LIMX_NODISCARD constexpr FVector4 operator+(const FVector4& other) const
    {
        return FVector4(X + other.X, Y + other.Y, Z + other.Z, W + other.W);
    }

    LIMX_NODISCARD constexpr FVector4 operator-(const FVector4& other) const
    {
        return FVector4(X - other.X, Y - other.Y, Z - other.Z, W - other.W);
    }

    LIMX_NODISCARD constexpr FVector4 operator*(Float32 scalar) const
    {
        return FVector4(X * scalar, Y * scalar, Z * scalar, W * scalar);
    }

    LIMX_NODISCARD constexpr FVector4 operator/(Float32 scalar) const
    {
        Float32 inv = 1.0f / scalar;
        return FVector4(X * inv, Y * inv, Z * inv, W * inv);
    }

    LIMX_NODISCARD constexpr FVector4 operator*(const FVector4& other) const
    {
        return FVector4(X * other.X, Y * other.Y, Z * other.Z, W * other.W);
    }

    LIMX_NODISCARD constexpr FVector4 operator-() const
    {
        return FVector4(-X, -Y, -Z, -W);
    }

    FVector4& operator+=(const FVector4& other) { X += other.X; Y += other.Y; Z += other.Z; W += other.W; return *this; }
    FVector4& operator-=(const FVector4& other) { X -= other.X; Y -= other.Y; Z -= other.Z; W -= other.W; return *this; }
    FVector4& operator*=(Float32 scalar) { X *= scalar; Y *= scalar; Z *= scalar; W *= scalar; return *this; }

    // 比较
    LIMX_NODISCARD constexpr bool operator==(const FVector4& other) const
    {
        return X == other.X && Y == other.Y && Z == other.Z && W == other.W;
    }

    LIMX_NODISCARD constexpr bool operator!=(const FVector4& other) const
    {
        return !(*this == other);
    }

    // 下标访问
    LIMX_NODISCARD FORCEINLINE Float32& operator[](Int32 index)
    {
        LIMX_ASSERT(index >= 0 && index < 4);
        return (&X)[index];
    }

    LIMX_NODISCARD FORCEINLINE Float32 operator[](Int32 index) const
    {
        LIMX_ASSERT(index >= 0 && index < 4);
        return (&X)[index];
    }

    // 几何运算
    LIMX_NODISCARD constexpr Float32 LengthSquared3() const
    {
        return X * X + Y * Y + Z * Z;
    }

    LIMX_NODISCARD constexpr Float32 LengthSquared() const
    {
        return X * X + Y * Y + Z * Z + W * W;
    }

    LIMX_NODISCARD Float32 Length3() const
    {
        return FMath::Sqrt(LengthSquared3());
    }

    LIMX_NODISCARD Float32 Length() const
    {
        return FMath::Sqrt(LengthSquared());
    }

    /// 提取 xyz 分量为 FVector3
    LIMX_NODISCARD constexpr FVector3 ToVector3() const
    {
        return FVector3(X, Y, Z);
    }

    LIMX_NODISCARD static constexpr Float32 Dot(const FVector4& a, const FVector4& b)
    {
        return a.X * b.X + a.Y * b.Y + a.Z * b.Z + a.W * b.W;
    }

    LIMX_NODISCARD static constexpr Float32 Dot3(const FVector4& a, const FVector4& b)
    {
        return a.X * b.X + a.Y * b.Y + a.Z * b.Z;
    }

    LIMX_NODISCARD static constexpr FVector4 Lerp(const FVector4& a, const FVector4& b, Float32 t)
    {
        return FVector4(
            FMath::Lerp(a.X, b.X, t),
            FMath::Lerp(a.Y, b.Y, t),
            FMath::Lerp(a.Z, b.Z, t),
            FMath::Lerp(a.W, b.W, t)
        );
    }
};

inline constexpr FVector4 FVector4::kZero = FVector4(0.0f, 0.0f, 0.0f, 0.0f);
inline constexpr FVector4 FVector4::kOne  = FVector4(1.0f, 1.0f, 1.0f, 1.0f);

LIMX_NODISCARD constexpr FVector4 operator*(Float32 scalar, const FVector4& v)
{
    return v * scalar;
}

} // namespace Limx
