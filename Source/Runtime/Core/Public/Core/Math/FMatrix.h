/*******************************************************************************
 * 文件: FMatrix.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   4x4 变换矩阵 — 渲染管线的核心数学类型
 *   支持平移、旋转、缩放、投影等空间变换操作
 *   行主序存储 (Row-Major)，兼容 DirectX/Vulkan 布局
 *
 * 设计哲学:
 *   行主序 — M[Row][Col]，与着色器 row_major 一致
 *   右手系 — 前方 +Z，上方 +Y，右方 +X
 *   值语义 — 矩阵是不可变的值类型
 *   SIMD 预留 — 每行 4 个 Float32 对齐到 16 字节
 *
 * 技术特性:
 *   - 矩阵乘法: operator*
 *   - 逆矩阵: Inverse()
 *   - 转置: Transpose()
 *   - 行列式: Determinant()
 *   - 变换构造: Translation, RotationX/Y/Z, Scale, LookAt, Perspective, Ortho
 *   - 向量变换: TransformPosition, TransformDirection, TransformVector4
 *
 * 依赖关系:
 *   内部: Core/Math/FMath.h, Core/Math/FVector.h
 *
 ******************************************************************************/

#pragma once

#include "Core/Math/FMath.h"
#include "Core/Math/FVector.h"

namespace Limx
{

/// 4x4 行主序矩阵 — M[行][列]
struct FMatrix
{
    // 行主序: M[0] = 第一行, M[1] = 第二行 ...
    Float32 M[4][4];

    // ========================================================================
    // 常量
    // ========================================================================

    static const FMatrix kIdentity;
    static const FMatrix kZero;

    // ========================================================================
    // 构造
    // ========================================================================

    /// 默认构造 — 未初始化 (性能考量)
    FMatrix() = default;

    /// 从 4 个行向量构造
    constexpr FMatrix(const FVector4& row0, const FVector4& row1,
                      const FVector4& row2, const FVector4& row3)
        : M{
            {row0.X, row0.Y, row0.Z, row0.W},
            {row1.X, row1.Y, row1.Z, row1.W},
            {row2.X, row2.Y, row2.Z, row2.W},
            {row3.X, row3.Y, row3.Z, row3.W}
          }
    {
    }

    /// 从 16 个元素构造 (行主序)
    constexpr FMatrix(Float32 m00, Float32 m01, Float32 m02, Float32 m03,
                      Float32 m10, Float32 m11, Float32 m12, Float32 m13,
                      Float32 m20, Float32 m21, Float32 m22, Float32 m23,
                      Float32 m30, Float32 m31, Float32 m32, Float32 m33)
        : M{
            {m00, m01, m02, m03},
            {m10, m11, m12, m13},
            {m20, m21, m22, m23},
            {m30, m31, m32, m33}
          }
    {
    }

    // ========================================================================
    // 矩阵运算
    // ========================================================================

    /// 矩阵乘法
    LIMX_NODISCARD FMatrix operator*(const FMatrix& other) const
    {
        FMatrix result;
        for (Int32 row = 0; row < 4; ++row)
        {
            for (Int32 col = 0; col < 4; ++col)
            {
                result.M[row][col] =
                    M[row][0] * other.M[0][col] +
                    M[row][1] * other.M[1][col] +
                    M[row][2] * other.M[2][col] +
                    M[row][3] * other.M[3][col];
            }
        }
        return result;
    }

    FMatrix& operator*=(const FMatrix& other)
    {
        *this = *this * other;
        return *this;
    }

    /// 标量乘法
    LIMX_NODISCARD FMatrix operator*(Float32 scalar) const
    {
        FMatrix result;
        for (Int32 row = 0; row < 4; ++row)
        {
            for (Int32 col = 0; col < 4; ++col)
            {
                result.M[row][col] = M[row][col] * scalar;
            }
        }
        return result;
    }

    /// 矩阵加法
    LIMX_NODISCARD FMatrix operator+(const FMatrix& other) const
    {
        FMatrix result;
        for (Int32 row = 0; row < 4; ++row)
        {
            for (Int32 col = 0; col < 4; ++col)
            {
                result.M[row][col] = M[row][col] + other.M[row][col];
            }
        }
        return result;
    }

    /// 比较
    LIMX_NODISCARD bool Equals(const FMatrix& other,
                                Float32 tolerance = FMath::kSmallNumber) const
    {
        for (Int32 row = 0; row < 4; ++row)
        {
            for (Int32 col = 0; col < 4; ++col)
            {
                if (FMath::Abs(M[row][col] - other.M[row][col]) > tolerance)
                {
                    return false;
                }
            }
        }
        return true;
    }

    // ========================================================================
    // 转置
    // ========================================================================

    LIMX_NODISCARD FMatrix Transpose() const
    {
        return FMatrix(
            M[0][0], M[1][0], M[2][0], M[3][0],
            M[0][1], M[1][1], M[2][1], M[3][1],
            M[0][2], M[1][2], M[2][2], M[3][2],
            M[0][3], M[1][3], M[2][3], M[3][3]
        );
    }

    // ========================================================================
    // 行列式
    // ========================================================================

    LIMX_NODISCARD Float32 Determinant() const
    {
        // 按第一行展开 Laplace
        Float32 a = M[0][0], b = M[0][1], c = M[0][2], d = M[0][3];

        Float32 det3x3_0 =
            M[1][1] * (M[2][2] * M[3][3] - M[2][3] * M[3][2]) -
            M[1][2] * (M[2][1] * M[3][3] - M[2][3] * M[3][1]) +
            M[1][3] * (M[2][1] * M[3][2] - M[2][2] * M[3][1]);

        Float32 det3x3_1 =
            M[1][0] * (M[2][2] * M[3][3] - M[2][3] * M[3][2]) -
            M[1][2] * (M[2][0] * M[3][3] - M[2][3] * M[3][0]) +
            M[1][3] * (M[2][0] * M[3][2] - M[2][2] * M[3][0]);

        Float32 det3x3_2 =
            M[1][0] * (M[2][1] * M[3][3] - M[2][3] * M[3][1]) -
            M[1][1] * (M[2][0] * M[3][3] - M[2][3] * M[3][0]) +
            M[1][3] * (M[2][0] * M[3][1] - M[2][1] * M[3][0]);

        Float32 det3x3_3 =
            M[1][0] * (M[2][1] * M[3][2] - M[2][2] * M[3][1]) -
            M[1][1] * (M[2][0] * M[3][2] - M[2][2] * M[3][0]) +
            M[1][2] * (M[2][0] * M[3][1] - M[2][1] * M[3][0]);

        return a * det3x3_0 - b * det3x3_1 + c * det3x3_2 - d * det3x3_3;
    }

    // ========================================================================
    // 逆矩阵 (伴随矩阵法)
    // ========================================================================

    LIMX_NODISCARD FMatrix Inverse() const
    {
        // 通过余因子矩阵计算逆矩阵
        Float32 a00 = M[0][0], a01 = M[0][1], a02 = M[0][2], a03 = M[0][3];
        Float32 a10 = M[1][0], a11 = M[1][1], a12 = M[1][2], a13 = M[1][3];
        Float32 a20 = M[2][0], a21 = M[2][1], a22 = M[2][2], a23 = M[2][3];
        Float32 a30 = M[3][0], a31 = M[3][1], a32 = M[3][2], a33 = M[3][3];

        Float32 b00 = a00 * a11 - a01 * a10;
        Float32 b01 = a00 * a12 - a02 * a10;
        Float32 b02 = a00 * a13 - a03 * a10;
        Float32 b03 = a01 * a12 - a02 * a11;
        Float32 b04 = a01 * a13 - a03 * a11;
        Float32 b05 = a02 * a13 - a03 * a12;
        Float32 b06 = a20 * a31 - a21 * a30;
        Float32 b07 = a20 * a32 - a22 * a30;
        Float32 b08 = a20 * a33 - a23 * a30;
        Float32 b09 = a21 * a32 - a22 * a31;
        Float32 b10 = a21 * a33 - a23 * a31;
        Float32 b11 = a22 * a33 - a23 * a32;

        Float32 det = b00 * b11 - b01 * b10 + b02 * b09 +
                      b03 * b08 - b04 * b07 + b05 * b06;

        LIMX_ASSERT(FMath::Abs(det) > FMath::kSmallNumber);
        Float32 invDet = 1.0f / det;

        FMatrix result;
        result.M[0][0] = ( a11 * b11 - a12 * b10 + a13 * b09) * invDet;
        result.M[0][1] = (-a01 * b11 + a02 * b10 - a03 * b09) * invDet;
        result.M[0][2] = ( a31 * b05 - a32 * b04 + a33 * b03) * invDet;
        result.M[0][3] = (-a21 * b05 + a22 * b04 - a23 * b03) * invDet;
        result.M[1][0] = (-a10 * b11 + a12 * b08 - a13 * b07) * invDet;
        result.M[1][1] = ( a00 * b11 - a02 * b08 + a03 * b07) * invDet;
        result.M[1][2] = (-a30 * b05 + a32 * b02 - a33 * b01) * invDet;
        result.M[1][3] = ( a20 * b05 - a22 * b02 + a23 * b01) * invDet;
        result.M[2][0] = ( a10 * b10 - a11 * b08 + a13 * b06) * invDet;
        result.M[2][1] = (-a00 * b10 + a01 * b08 - a03 * b06) * invDet;
        result.M[2][2] = ( a30 * b04 - a31 * b02 + a33 * b00) * invDet;
        result.M[2][3] = (-a20 * b04 + a21 * b02 - a23 * b00) * invDet;
        result.M[3][0] = (-a10 * b09 + a11 * b07 - a12 * b06) * invDet;
        result.M[3][1] = ( a00 * b09 - a01 * b07 + a02 * b06) * invDet;
        result.M[3][2] = (-a30 * b03 + a31 * b01 - a32 * b00) * invDet;
        result.M[3][3] = ( a20 * b03 - a21 * b01 + a22 * b00) * invDet;
        return result;
    }

    // ========================================================================
    // 向量变换
    // ========================================================================

    /// 变换位置点 (应用平移) — 隐含 w=1
    LIMX_NODISCARD FVector3 TransformPosition(const FVector3& position) const
    {
        return FVector3(
            M[0][0] * position.X + M[0][1] * position.Y + M[0][2] * position.Z + M[0][3],
            M[1][0] * position.X + M[1][1] * position.Y + M[1][2] * position.Z + M[1][3],
            M[2][0] * position.X + M[2][1] * position.Y + M[2][2] * position.Z + M[2][3]
        );
    }

    /// 变换方向向量 (不应用平移) — 隐含 w=0
    LIMX_NODISCARD FVector3 TransformDirection(const FVector3& direction) const
    {
        return FVector3(
            M[0][0] * direction.X + M[0][1] * direction.Y + M[0][2] * direction.Z,
            M[1][0] * direction.X + M[1][1] * direction.Y + M[1][2] * direction.Z,
            M[2][0] * direction.X + M[2][1] * direction.Y + M[2][2] * direction.Z
        );
    }

    /// 变换 4D 向量
    LIMX_NODISCARD FVector4 TransformVector4(const FVector4& v) const
    {
        return FVector4(
            M[0][0] * v.X + M[0][1] * v.Y + M[0][2] * v.Z + M[0][3] * v.W,
            M[1][0] * v.X + M[1][1] * v.Y + M[1][2] * v.Z + M[1][3] * v.W,
            M[2][0] * v.X + M[2][1] * v.Y + M[2][2] * v.Z + M[2][3] * v.W,
            M[3][0] * v.X + M[3][1] * v.Y + M[3][2] * v.Z + M[3][3] * v.W
        );
    }

    // ========================================================================
    // 提取分量
    // ========================================================================

    /// 提取平移分量
    LIMX_NODISCARD FVector3 GetTranslation() const
    {
        return FVector3(M[0][3], M[1][3], M[2][3]);
    }

    /// 提取缩放分量 (每个轴的基向量长度)
    LIMX_NODISCARD FVector3 GetScale() const
    {
        return FVector3(
            FVector3(M[0][0], M[0][1], M[0][2]).Length(),
            FVector3(M[1][0], M[1][1], M[1][2]).Length(),
            FVector3(M[2][0], M[2][1], M[2][2]).Length()
        );
    }

    /// 获取行向量
    LIMX_NODISCARD FVector4 GetRow(Int32 row) const
    {
        LIMX_ASSERT(row >= 0 && row < 4);
        return FVector4(M[row][0], M[row][1], M[row][2], M[row][3]);
    }

    /// 获取列向量
    LIMX_NODISCARD FVector4 GetColumn(Int32 col) const
    {
        LIMX_ASSERT(col >= 0 && col < 4);
        return FVector4(M[0][col], M[1][col], M[2][col], M[3][col]);
    }

    // ========================================================================
    // 工厂函数 — 构造常用变换矩阵
    // ========================================================================

    /// 单位矩阵
    LIMX_NODISCARD static constexpr FMatrix Identity()
    {
        return FMatrix(
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        );
    }

    /// 平移矩阵
    LIMX_NODISCARD static constexpr FMatrix Translation(const FVector3& offset)
    {
        return FMatrix(
            1.0f, 0.0f, 0.0f, offset.X,
            0.0f, 1.0f, 0.0f, offset.Y,
            0.0f, 0.0f, 1.0f, offset.Z,
            0.0f, 0.0f, 0.0f, 1.0f
        );
    }

    /// 缩放矩阵
    LIMX_NODISCARD static constexpr FMatrix Scale(const FVector3& scale)
    {
        return FMatrix(
            scale.X, 0.0f,    0.0f,    0.0f,
            0.0f,    scale.Y, 0.0f,    0.0f,
            0.0f,    0.0f,    scale.Z, 0.0f,
            0.0f,    0.0f,    0.0f,    1.0f
        );
    }

    /// 绕 X 轴旋转 (弧度)
    LIMX_NODISCARD static FMatrix RotationX(Float32 radians)
    {
        Float32 s, c;
        FMath::SinCos(radians, s, c);
        return FMatrix(
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f,  c,   -s,   0.0f,
            0.0f,  s,    c,   0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        );
    }

    /// 绕 Y 轴旋转 (弧度)
    LIMX_NODISCARD static FMatrix RotationY(Float32 radians)
    {
        Float32 s, c;
        FMath::SinCos(radians, s, c);
        return FMatrix(
             c,   0.0f,  s,   0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            -s,   0.0f,  c,   0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        );
    }

    /// 绕 Z 轴旋转 (弧度)
    LIMX_NODISCARD static FMatrix RotationZ(Float32 radians)
    {
        Float32 s, c;
        FMath::SinCos(radians, s, c);
        return FMatrix(
             c,   -s,   0.0f, 0.0f,
             s,    c,   0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        );
    }

    /// 绕任意轴旋转 (Rodrigues)
    LIMX_NODISCARD static FMatrix RotationAxis(const FVector3& axis, Float32 radians)
    {
        FVector3 n = axis.GetSafeNormal();
        Float32 s, c;
        FMath::SinCos(radians, s, c);
        Float32 t = 1.0f - c;

        return FMatrix(
            t * n.X * n.X + c,       t * n.X * n.Y - s * n.Z, t * n.X * n.Z + s * n.Y, 0.0f,
            t * n.X * n.Y + s * n.Z, t * n.Y * n.Y + c,       t * n.Y * n.Z - s * n.X, 0.0f,
            t * n.X * n.Z - s * n.Y, t * n.Y * n.Z + s * n.X, t * n.Z * n.Z + c,       0.0f,
            0.0f,                     0.0f,                     0.0f,                     1.0f
        );
    }

    /// LookAt 矩阵 (右手系)
    /// @param eye    相机位置
    /// @param target 注视目标
    /// @param up     世界上方向
    LIMX_NODISCARD static FMatrix LookAt(const FVector3& eye,
                                          const FVector3& target,
                                          const FVector3& up)
    {
        FVector3 forward = (target - eye).GetSafeNormal();
        FVector3 right = FVector3::Cross(forward, up).GetSafeNormal();
        FVector3 newUp = FVector3::Cross(right, forward);

        return FMatrix(
             right.X,    right.Y,    right.Z,   -FVector3::Dot(right, eye),
             newUp.X,    newUp.Y,    newUp.Z,   -FVector3::Dot(newUp, eye),
            -forward.X, -forward.Y, -forward.Z,  FVector3::Dot(forward, eye),
             0.0f,       0.0f,       0.0f,       1.0f
        );
    }

    /// 透视投影矩阵 (Vulkan NDC: Y 翻转, Z 范围 [0, 1])
    /// @param fovY         垂直视场角 (弧度)
    /// @param aspectRatio  宽高比
    /// @param nearPlane    近裁剪面
    /// @param farPlane     远裁剪面
    LIMX_NODISCARD static FMatrix Perspective(Float32 fovY, Float32 aspectRatio,
                                               Float32 nearPlane, Float32 farPlane)
    {
        Float32 tanHalfFov = FMath::Tan(fovY * 0.5f);
        Float32 rangeInv = 1.0f / (farPlane - nearPlane);

        return FMatrix(
            1.0f / (aspectRatio * tanHalfFov), 0.0f,                  0.0f,                            0.0f,
            0.0f,                              -1.0f / tanHalfFov,    0.0f,                            0.0f,
            0.0f,                              0.0f,                  farPlane * rangeInv,             -farPlane * nearPlane * rangeInv,
            0.0f,                              0.0f,                  1.0f,                            0.0f
        );
    }

    /// 正交投影矩阵 (Vulkan NDC)
    LIMX_NODISCARD static FMatrix Ortho(Float32 left, Float32 right,
                                         Float32 bottom, Float32 top,
                                         Float32 nearPlane, Float32 farPlane)
    {
        Float32 invRL = 1.0f / (right - left);
        Float32 invTB = 1.0f / (top - bottom);
        Float32 invFN = 1.0f / (farPlane - nearPlane);

        return FMatrix(
            2.0f * invRL, 0.0f,          0.0f,     -(right + left) * invRL,
            0.0f,        -2.0f * invTB,  0.0f,      (top + bottom) * invTB,
            0.0f,         0.0f,          invFN,     -nearPlane * invFN,
            0.0f,         0.0f,          0.0f,      1.0f
        );
    }
};

// 常量定义
inline constexpr FMatrix FMatrix::kIdentity = FMatrix::Identity();
inline constexpr FMatrix FMatrix::kZero = FMatrix(
    0.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 0.0f
);

} // namespace Limx
