/*******************************************************************************
 * 文件: FQuat.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   四元数旋转 — 3D 旋转的紧凑无万向锁表示
 *   支持四元数乘法、球面插值 (Slerp)、与矩阵/欧拉角互转
 *   作为引擎 Transform 系统的旋转存储格式
 *
 * 设计哲学:
 *   Hamilton 约定 — q = w + xi + yj + zk
 *   单位四元数 — 所有旋转操作假设输入为单位四元数
 *   值语义 — 四元数是不可变的轻量值类型
 *
 * 技术特性:
 *   - 四元数乘法: operator* (旋转组合)
 *   - 球面插值: Slerp(q1, q2, t)
 *   - 归一化: Normalize(), GetSafeNormal()
 *   - 互转: FromAxisAngle, FromEuler, FromMatrix, ToMatrix, ToEuler
 *   - 向量旋转: RotateVector(v)
 *   - 共轭/逆: Conjugate(), Inverse()
 *
 * 依赖关系:
 *   内部: Core/Math/FMath.h, Core/Math/FVector.h, Core/Math/FMatrix.h
 *
 ******************************************************************************/

#pragma once

#include "Core/Math/FMath.h"
#include "Core/Math/FVector.h"
#include "Core/Math/FMatrix.h"

namespace Limx
{

/// 四元数 — Hamilton 约定: q = W + Xi + Yj + Zk
struct FQuat
{
    Float32 X;  ///< 虚部 i 分量
    Float32 Y;  ///< 虚部 j 分量
    Float32 Z;  ///< 虚部 k 分量
    Float32 W;  ///< 实部

    // 常量
    static const FQuat kIdentity;

    // ========================================================================
    // 构造
    // ========================================================================

    /// 默认构造 — 单位四元数 (无旋转)
    constexpr FQuat() : X(0.0f), Y(0.0f), Z(0.0f), W(1.0f) {}

    /// 分量构造
    constexpr FQuat(Float32 inX, Float32 inY, Float32 inZ, Float32 inW)
        : X(inX), Y(inY), Z(inZ), W(inW) {}

    // ========================================================================
    // 四元数运算
    // ========================================================================

    /// 四元数乘法 — 旋转组合 (先 rhs 再 lhs)
    LIMX_NODISCARD FQuat operator*(const FQuat& other) const
    {
        return FQuat(
            W * other.X + X * other.W + Y * other.Z - Z * other.Y,
            W * other.Y - X * other.Z + Y * other.W + Z * other.X,
            W * other.Z + X * other.Y - Y * other.X + Z * other.W,
            W * other.W - X * other.X - Y * other.Y - Z * other.Z
        );
    }

    FQuat& operator*=(const FQuat& other)
    {
        *this = *this * other;
        return *this;
    }

    /// 标量乘法
    LIMX_NODISCARD constexpr FQuat operator*(Float32 scalar) const
    {
        return FQuat(X * scalar, Y * scalar, Z * scalar, W * scalar);
    }

    /// 四元数加法 (用于插值中间步骤)
    LIMX_NODISCARD constexpr FQuat operator+(const FQuat& other) const
    {
        return FQuat(X + other.X, Y + other.Y, Z + other.Z, W + other.W);
    }

    /// 四元数减法
    LIMX_NODISCARD constexpr FQuat operator-(const FQuat& other) const
    {
        return FQuat(X - other.X, Y - other.Y, Z - other.Z, W - other.W);
    }

    /// 取反
    LIMX_NODISCARD constexpr FQuat operator-() const
    {
        return FQuat(-X, -Y, -Z, -W);
    }

    // ========================================================================
    // 共轭、模、归一化
    // ========================================================================

    /// 共轭 — 虚部取反
    LIMX_NODISCARD constexpr FQuat Conjugate() const
    {
        return FQuat(-X, -Y, -Z, W);
    }

    /// 模的平方
    LIMX_NODISCARD constexpr Float32 LengthSquared() const
    {
        return X * X + Y * Y + Z * Z + W * W;
    }

    /// 模
    LIMX_NODISCARD Float32 Length() const
    {
        return FMath::Sqrt(LengthSquared());
    }

    /// 归一化 (原地修改)
    void Normalize(Float32 tolerance = FMath::kSmallNumber)
    {
        Float32 lenSq = LengthSquared();
        if (lenSq > tolerance)
        {
            Float32 invLen = FMath::InvSqrt(lenSq);
            X *= invLen;
            Y *= invLen;
            Z *= invLen;
            W *= invLen;
        }
        else
        {
            *this = kIdentity;
        }
    }

    /// 安全归一化 — 返回新四元数
    LIMX_NODISCARD FQuat GetNormalized(Float32 tolerance = FMath::kSmallNumber) const
    {
        FQuat result = *this;
        result.Normalize(tolerance);
        return result;
    }

    /// 是否为单位四元数
    LIMX_NODISCARD bool IsNormalized(Float32 tolerance = 0.01f) const
    {
        return FMath::Abs(LengthSquared() - 1.0f) < tolerance;
    }

    /// 逆 — 对单位四元数等价于共轭
    LIMX_NODISCARD FQuat Inverse() const
    {
        Float32 lenSq = LengthSquared();
        if (lenSq > FMath::kSmallNumber)
        {
            Float32 invLenSq = 1.0f / lenSq;
            return FQuat(-X * invLenSq, -Y * invLenSq,
                         -Z * invLenSq,  W * invLenSq);
        }
        return kIdentity;
    }

    // ========================================================================
    // 向量旋转
    // ========================================================================

    /// 用四元数旋转向量: v' = q * v * q^-1
    LIMX_NODISCARD FVector3 RotateVector(const FVector3& v) const
    {
        // 优化公式: v' = v + 2w(u × v) + 2(u × (u × v))
        // 其中 u = (X, Y, Z), w = W
        FVector3 u(X, Y, Z);
        FVector3 uCrossV = FVector3::Cross(u, v);
        FVector3 uCrossUCrossV = FVector3::Cross(u, uCrossV);
        return v + (uCrossV * W + uCrossUCrossV) * 2.0f;
    }

    /// 用逆四元数旋转向量
    LIMX_NODISCARD FVector3 UnrotateVector(const FVector3& v) const
    {
        return Conjugate().RotateVector(v);
    }

    /// 获取四元数的前方向 (+Z)
    LIMX_NODISCARD FVector3 GetForward() const
    {
        return RotateVector(FVector3::kForward);
    }

    /// 获取四元数的右方向 (+X)
    LIMX_NODISCARD FVector3 GetRight() const
    {
        return RotateVector(FVector3::kRight);
    }

    /// 获取四元数的上方向 (+Y)
    LIMX_NODISCARD FVector3 GetUp() const
    {
        return RotateVector(FVector3::kUp);
    }

    // ========================================================================
    // 点积与夹角
    // ========================================================================

    /// 四元数点积
    LIMX_NODISCARD static constexpr Float32 Dot(const FQuat& a, const FQuat& b)
    {
        return a.X * b.X + a.Y * b.Y + a.Z * b.Z + a.W * b.W;
    }

    /// 两个旋转之间的角度 (弧度)
    LIMX_NODISCARD static Float32 AngleBetween(const FQuat& a, const FQuat& b)
    {
        Float32 dot = FMath::Abs(Dot(a, b));
        dot = FMath::Clamp(dot, 0.0f, 1.0f);
        return 2.0f * FMath::ACos(dot);
    }

    // ========================================================================
    // 比较
    // ========================================================================

    LIMX_NODISCARD bool Equals(const FQuat& other,
                                Float32 tolerance = FMath::kSmallNumber) const
    {
        return FMath::Abs(X - other.X) <= tolerance &&
               FMath::Abs(Y - other.Y) <= tolerance &&
               FMath::Abs(Z - other.Z) <= tolerance &&
               FMath::Abs(W - other.W) <= tolerance;
    }

    LIMX_NODISCARD constexpr bool operator==(const FQuat& other) const
    {
        return X == other.X && Y == other.Y && Z == other.Z && W == other.W;
    }

    LIMX_NODISCARD constexpr bool operator!=(const FQuat& other) const
    {
        return !(*this == other);
    }

    // ========================================================================
    // 工厂函数
    // ========================================================================

    /// 从轴角构造
    LIMX_NODISCARD static FQuat FromAxisAngle(const FVector3& axis, Float32 radians)
    {
        Float32 halfAngle = radians * 0.5f;
        Float32 s, c;
        FMath::SinCos(halfAngle, s, c);
        FVector3 n = axis.GetSafeNormal();
        return FQuat(n.X * s, n.Y * s, n.Z * s, c);
    }

    /// 从欧拉角构造 (度, 内旋顺序 ZYX: Yaw → Pitch → Roll)
    /// @param pitch X 轴旋转 (度)
    /// @param yaw   Y 轴旋转 (度)
    /// @param roll  Z 轴旋转 (度)
    LIMX_NODISCARD static FQuat FromEuler(Float32 pitch, Float32 yaw, Float32 roll)
    {
        Float32 halfPitch = FMath::DegreesToRadians(pitch) * 0.5f;
        Float32 halfYaw   = FMath::DegreesToRadians(yaw)   * 0.5f;
        Float32 halfRoll  = FMath::DegreesToRadians(roll)  * 0.5f;

        Float32 sp, cp, sy, cy, sr, cr;
        FMath::SinCos(halfPitch, sp, cp);
        FMath::SinCos(halfYaw,   sy, cy);
        FMath::SinCos(halfRoll,  sr, cr);

        return FQuat(
            cy * sp * cr + sy * cp * sr,
            sy * cp * cr - cy * sp * sr,
            cy * cp * sr - sy * sp * cr,
            cy * cp * cr + sy * sp * sr
        );
    }

    /// 从欧拉角向量构造 (度)
    LIMX_NODISCARD static FQuat FromEuler(const FVector3& eulerDegrees)
    {
        return FromEuler(eulerDegrees.X, eulerDegrees.Y, eulerDegrees.Z);
    }

    /// 转换为欧拉角 (度, Pitch/Yaw/Roll)
    LIMX_NODISCARD FVector3 ToEuler() const
    {
        Float32 pitch, yaw, roll;

        // 万向锁检测
        Float32 sinPitch = 2.0f * (W * X - Y * Z);
        if (FMath::Abs(sinPitch) >= 0.9999f)
        {
            // 万向锁
            pitch = FMath::Sign(sinPitch) * 90.0f;
            yaw = FMath::RadiansToDegrees(
                2.0f * FMath::ATan2(Y, W));
            roll = 0.0f;
        }
        else
        {
            pitch = FMath::RadiansToDegrees(FMath::ASin(sinPitch));
            yaw = FMath::RadiansToDegrees(
                FMath::ATan2(2.0f * (W * Y + X * Z),
                             1.0f - 2.0f * (X * X + Y * Y)));
            roll = FMath::RadiansToDegrees(
                FMath::ATan2(2.0f * (W * Z + X * Y),
                             1.0f - 2.0f * (X * X + Z * Z)));
        }

        return FVector3(pitch, yaw, roll);
    }

    /// 从旋转矩阵的左上 3x3 部分提取四元数
    LIMX_NODISCARD static FQuat FromMatrix(const FMatrix& matrix)
    {
        Float32 trace = matrix.M[0][0] + matrix.M[1][1] + matrix.M[2][2];
        FQuat result;

        if (trace > 0.0f)
        {
            Float32 s = FMath::Sqrt(trace + 1.0f) * 2.0f;
            Float32 invS = 1.0f / s;
            result.W = 0.25f * s;
            result.X = (matrix.M[2][1] - matrix.M[1][2]) * invS;
            result.Y = (matrix.M[0][2] - matrix.M[2][0]) * invS;
            result.Z = (matrix.M[1][0] - matrix.M[0][1]) * invS;
        }
        else if (matrix.M[0][0] > matrix.M[1][1] &&
                 matrix.M[0][0] > matrix.M[2][2])
        {
            Float32 s = FMath::Sqrt(
                1.0f + matrix.M[0][0] - matrix.M[1][1] - matrix.M[2][2]) * 2.0f;
            Float32 invS = 1.0f / s;
            result.W = (matrix.M[2][1] - matrix.M[1][2]) * invS;
            result.X = 0.25f * s;
            result.Y = (matrix.M[0][1] + matrix.M[1][0]) * invS;
            result.Z = (matrix.M[0][2] + matrix.M[2][0]) * invS;
        }
        else if (matrix.M[1][1] > matrix.M[2][2])
        {
            Float32 s = FMath::Sqrt(
                1.0f - matrix.M[0][0] + matrix.M[1][1] - matrix.M[2][2]) * 2.0f;
            Float32 invS = 1.0f / s;
            result.W = (matrix.M[0][2] - matrix.M[2][0]) * invS;
            result.X = (matrix.M[0][1] + matrix.M[1][0]) * invS;
            result.Y = 0.25f * s;
            result.Z = (matrix.M[1][2] + matrix.M[2][1]) * invS;
        }
        else
        {
            Float32 s = FMath::Sqrt(
                1.0f - matrix.M[0][0] - matrix.M[1][1] + matrix.M[2][2]) * 2.0f;
            Float32 invS = 1.0f / s;
            result.W = (matrix.M[1][0] - matrix.M[0][1]) * invS;
            result.X = (matrix.M[0][2] + matrix.M[2][0]) * invS;
            result.Y = (matrix.M[1][2] + matrix.M[2][1]) * invS;
            result.Z = 0.25f * s;
        }

        result.Normalize();
        return result;
    }

    /// 转换为 4x4 旋转矩阵
    LIMX_NODISCARD FMatrix ToMatrix() const
    {
        Float32 x2 = X + X, y2 = Y + Y, z2 = Z + Z;
        Float32 xx = X * x2, xy = X * y2, xz = X * z2;
        Float32 yy = Y * y2, yz = Y * z2, zz = Z * z2;
        Float32 wx = W * x2, wy = W * y2, wz = W * z2;

        return FMatrix(
            1.0f - (yy + zz), xy - wz,           xz + wy,           0.0f,
            xy + wz,           1.0f - (xx + zz),  yz - wx,           0.0f,
            xz - wy,           yz + wx,           1.0f - (xx + yy),  0.0f,
            0.0f,              0.0f,              0.0f,               1.0f
        );
    }

    /// 从一个方向到另一个方向的最短旋转
    LIMX_NODISCARD static FQuat FindBetween(const FVector3& from, const FVector3& to)
    {
        FVector3 a = from.GetSafeNormal();
        FVector3 b = to.GetSafeNormal();
        Float32 dot = FVector3::Dot(a, b);

        if (dot > 0.9999f)
        {
            // 几乎同向
            return kIdentity;
        }

        if (dot < -0.9999f)
        {
            // 几乎反向 — 找一个垂直轴
            FVector3 axis = FVector3::Cross(FVector3::kUnitX, a);
            if (axis.LengthSquared() < FMath::kSmallNumber)
            {
                axis = FVector3::Cross(FVector3::kUnitY, a);
            }
            return FromAxisAngle(axis.GetSafeNormal(), FMath::kPi);
        }

        FVector3 cross = FVector3::Cross(a, b);
        return FQuat(cross.X, cross.Y, cross.Z, 1.0f + dot).GetNormalized();
    }

    // ========================================================================
    // 插值
    // ========================================================================

    /// 球面线性插值 (Slerp)
    LIMX_NODISCARD static FQuat Slerp(const FQuat& a, const FQuat& b, Float32 t)
    {
        Float32 dot = Dot(a, b);

        // 确保走短弧路径
        FQuat bCorrected = b;
        if (dot < 0.0f)
        {
            bCorrected = -b;
            dot = -dot;
        }

        if (dot > 0.9995f)
        {
            // 非常接近 — 用 NLerp 避免除零
            FQuat result = a + (bCorrected - a) * t;
            result.Normalize();
            return result;
        }

        Float32 theta = FMath::ACos(FMath::Clamp(dot, -1.0f, 1.0f));
        Float32 sinTheta = FMath::Sin(theta);
        Float32 factorA = FMath::Sin((1.0f - t) * theta) / sinTheta;
        Float32 factorB = FMath::Sin(t * theta) / sinTheta;

        return FQuat(
            a.X * factorA + bCorrected.X * factorB,
            a.Y * factorA + bCorrected.Y * factorB,
            a.Z * factorA + bCorrected.Z * factorB,
            a.W * factorA + bCorrected.W * factorB
        );
    }

    /// 归一化线性插值 (NLerp) — Slerp 的快速近似
    LIMX_NODISCARD static FQuat NLerp(const FQuat& a, const FQuat& b, Float32 t)
    {
        Float32 dot = Dot(a, b);
        FQuat bCorrected = dot < 0.0f ? -b : b;

        FQuat result = a + (bCorrected - a) * t;
        result.Normalize();
        return result;
    }
};

// 常量定义
inline constexpr FQuat FQuat::kIdentity = FQuat(0.0f, 0.0f, 0.0f, 1.0f);

LIMX_NODISCARD constexpr FQuat operator*(Float32 scalar, const FQuat& q)
{
    return q * scalar;
}

} // namespace Limx
