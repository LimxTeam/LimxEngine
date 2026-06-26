/*******************************************************************************
 * 文件: FNoise.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   噪声函数库 — Perlin/Simplex 噪声的零 STL 实现
 *   提供 1D/2D/3D 噪声、分形叠加 (FBM)、湍流等变体
 *   用于程序化纹理、地形生成、粒子扰动、动画噪声等场景
 *
 * 设计哲学:
 *   纯函数 — 无状态，相同输入始终返回相同输出 [-1, 1]
 *   梯度噪声 — 基于经典 Perlin 改进版 (2002)
 *   分形叠加 — FBM (Fractal Brownian Motion) 多频率混合
 *
 * 技术特性:
 *   - Perlin2D/Perlin3D: 2D/3D Perlin 噪声
 *   - FBM2D/FBM3D: 分形布朗运动 (多八度叠加)
 *   - Turbulence2D: 湍流 (绝对值叠加)
 *   - 输出范围: [-1.0, 1.0]
 *   - 置换表: 编译时 256 元素排列表
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

/// 噪声函数库
struct FNoise
{
    // ========================================================================
    // 2D Perlin 噪声
    // ========================================================================

    /// 2D Perlin 噪声 — 返回 [-1, 1]
    LIMX_NODISCARD static Float32 Perlin2D(Float32 x, Float32 y)
    {
        // 网格坐标
        Int32 xi = FloorToInt(x) & 255;
        Int32 yi = FloorToInt(y) & 255;

        // 小数部分
        Float32 xf = x - FMath::Floor(x);
        Float32 yf = y - FMath::Floor(y);

        // Fade 曲线
        Float32 u = Fade(xf);
        Float32 v = Fade(yf);

        // 哈希
        const UInt8* p = GetPermutation();
        Int32 aa = p[p[xi] + yi];
        Int32 ab = p[p[xi] + yi + 1];
        Int32 ba = p[p[xi + 1] + yi];
        Int32 bb = p[p[xi + 1] + yi + 1];

        // 梯度并插值
        Float32 x1 = Lerp(Grad2D(aa, xf, yf),
                           Grad2D(ba, xf - 1.0f, yf), u);
        Float32 x2 = Lerp(Grad2D(ab, xf, yf - 1.0f),
                           Grad2D(bb, xf - 1.0f, yf - 1.0f), u);

        return Lerp(x1, x2, v);
    }

    // ========================================================================
    // 3D Perlin 噪声
    // ========================================================================

    /// 3D Perlin 噪声 — 返回 [-1, 1]
    LIMX_NODISCARD static Float32 Perlin3D(Float32 x, Float32 y,
                                             Float32 z)
    {
        Int32 xi = FloorToInt(x) & 255;
        Int32 yi = FloorToInt(y) & 255;
        Int32 zi = FloorToInt(z) & 255;

        Float32 xf = x - FMath::Floor(x);
        Float32 yf = y - FMath::Floor(y);
        Float32 zf = z - FMath::Floor(z);

        Float32 u = Fade(xf);
        Float32 v = Fade(yf);
        Float32 w = Fade(zf);

        const UInt8* p = GetPermutation();
        Int32 a  = p[xi] + yi;
        Int32 aa = p[a] + zi;
        Int32 ab = p[a + 1] + zi;
        Int32 b  = p[xi + 1] + yi;
        Int32 ba = p[b] + zi;
        Int32 bb = p[b + 1] + zi;

        Float32 result = Lerp(
            Lerp(
                Lerp(Grad3D(p[aa], xf, yf, zf),
                     Grad3D(p[ba], xf - 1.0f, yf, zf), u),
                Lerp(Grad3D(p[ab], xf, yf - 1.0f, zf),
                     Grad3D(p[bb], xf - 1.0f, yf - 1.0f, zf), u),
                v),
            Lerp(
                Lerp(Grad3D(p[aa + 1], xf, yf, zf - 1.0f),
                     Grad3D(p[ba + 1], xf - 1.0f, yf, zf - 1.0f), u),
                Lerp(Grad3D(p[ab + 1], xf, yf - 1.0f, zf - 1.0f),
                     Grad3D(p[bb + 1], xf - 1.0f, yf - 1.0f,
                            zf - 1.0f), u),
                v),
            w);

        return result;
    }

    // ========================================================================
    // 分形布朗运动 (FBM)
    // ========================================================================

    /// 2D FBM — 多八度 Perlin 叠加
    /// @param octaves     八度数 (层数)
    /// @param lacunarity  频率倍增因子 (通常 2.0)
    /// @param persistence 振幅衰减因子 (通常 0.5)
    LIMX_NODISCARD static Float32 FBM2D(Float32 x, Float32 y,
                                          Int32 octaves = 6,
                                          Float32 lacunarity = 2.0f,
                                          Float32 persistence = 0.5f)
    {
        Float32 result = 0.0f;
        Float32 amplitude = 1.0f;
        Float32 frequency = 1.0f;
        Float32 maxValue = 0.0f;

        for (Int32 oct = 0; oct < octaves; ++oct)
        {
            result += Perlin2D(x * frequency, y * frequency) *
                      amplitude;
            maxValue += amplitude;
            amplitude *= persistence;
            frequency *= lacunarity;
        }

        return result / maxValue;
    }

    /// 3D FBM
    LIMX_NODISCARD static Float32 FBM3D(Float32 x, Float32 y,
                                          Float32 z,
                                          Int32 octaves = 6,
                                          Float32 lacunarity = 2.0f,
                                          Float32 persistence = 0.5f)
    {
        Float32 result = 0.0f;
        Float32 amplitude = 1.0f;
        Float32 frequency = 1.0f;
        Float32 maxValue = 0.0f;

        for (Int32 oct = 0; oct < octaves; ++oct)
        {
            result += Perlin3D(x * frequency, y * frequency,
                                z * frequency) * amplitude;
            maxValue += amplitude;
            amplitude *= persistence;
            frequency *= lacunarity;
        }

        return result / maxValue;
    }

    // ========================================================================
    // 湍流
    // ========================================================================

    /// 2D 湍流 — 绝对值叠加
    LIMX_NODISCARD static Float32 Turbulence2D(Float32 x, Float32 y,
                                                 Int32 octaves = 6,
                                                 Float32 lacunarity = 2.0f,
                                                 Float32 persistence = 0.5f)
    {
        Float32 result = 0.0f;
        Float32 amplitude = 1.0f;
        Float32 frequency = 1.0f;
        Float32 maxValue = 0.0f;

        for (Int32 oct = 0; oct < octaves; ++oct)
        {
            result += FMath::Abs(
                Perlin2D(x * frequency, y * frequency)) * amplitude;
            maxValue += amplitude;
            amplitude *= persistence;
            frequency *= lacunarity;
        }

        return result / maxValue;
    }

private:
    // ========================================================================
    // 辅助函数
    // ========================================================================

    /// Fade 曲线: 6t^5 - 15t^4 + 10t^3
    static Float32 Fade(Float32 t)
    {
        return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    }

    /// 线性插值
    static Float32 Lerp(Float32 a, Float32 b, Float32 t)
    {
        return a + t * (b - a);
    }

    /// 2D 梯度函数
    static Float32 Grad2D(Int32 hash, Float32 x, Float32 y)
    {
        Int32 h = hash & 3;
        Float32 u = (h & 2) == 0 ? x : -x;
        Float32 v = (h & 1) == 0 ? y : -y;
        return u + v;
    }

    /// 3D 梯度函数 (Perlin 改进版)
    static Float32 Grad3D(Int32 hash, Float32 x, Float32 y,
                            Float32 z)
    {
        Int32 h = hash & 15;
        Float32 u = h < 8 ? x : y;
        Float32 v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
        return ((h & 1) == 0 ? u : -u) +
               ((h & 2) == 0 ? v : -v);
    }

    /// Floor 并转为 Int32
    static Int32 FloorToInt(Float32 value)
    {
        Int32 intValue = static_cast<Int32>(value);
        return value < static_cast<Float32>(intValue)
                   ? intValue - 1 : intValue;
    }

    /// 获取置换表 (256 * 2 元素，重复一次避免越界)
    static const UInt8* GetPermutation()
    {
        static const UInt8 kTable[512] =
        {
            151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225,
            140,36,103,30,69,142,8,99,37,240,21,10,23,190,6,148,
            247,120,234,75,0,26,197,62,94,252,219,203,117,35,11,32,
            57,177,33,88,237,149,56,87,174,20,125,136,171,168,68,175,
            74,165,71,134,139,48,27,166,77,146,158,231,83,111,229,122,
            60,211,133,230,220,105,92,41,55,46,245,40,244,102,143,54,
            65,25,63,161,1,216,80,73,209,76,132,187,208,89,18,169,
            200,196,135,130,116,188,159,86,164,100,109,198,173,186,3,64,
            52,217,226,250,124,123,5,202,38,147,118,126,255,82,85,212,
            207,206,59,227,47,16,58,17,182,189,28,42,223,183,170,213,
            119,248,152,2,44,154,163,70,221,153,101,155,167,43,172,9,
            129,22,39,253,19,98,108,110,79,113,224,232,178,185,112,104,
            218,246,97,228,251,34,242,193,238,210,144,12,191,179,162,241,
            81,51,145,235,249,14,239,107,49,192,214,31,181,199,106,157,
            184,84,204,176,115,121,50,45,127,4,150,254,138,236,205,93,
            222,114,67,29,24,72,243,141,128,195,78,66,215,61,156,180,
            // 重复
            151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225,
            140,36,103,30,69,142,8,99,37,240,21,10,23,190,6,148,
            247,120,234,75,0,26,197,62,94,252,219,203,117,35,11,32,
            57,177,33,88,237,149,56,87,174,20,125,136,171,168,68,175,
            74,165,71,134,139,48,27,166,77,146,158,231,83,111,229,122,
            60,211,133,230,220,105,92,41,55,46,245,40,244,102,143,54,
            65,25,63,161,1,216,80,73,209,76,132,187,208,89,18,169,
            200,196,135,130,116,188,159,86,164,100,109,198,173,186,3,64,
            52,217,226,250,124,123,5,202,38,147,118,126,255,82,85,212,
            207,206,59,227,47,16,58,17,182,189,28,42,223,183,170,213,
            119,248,152,2,44,154,163,70,221,153,101,155,167,43,172,9,
            129,22,39,253,19,98,108,110,79,113,224,232,178,185,112,104,
            218,246,97,228,251,34,242,193,238,210,144,12,191,179,162,241,
            81,51,145,235,249,14,239,107,49,192,214,31,181,199,106,157,
            184,84,204,176,115,121,50,45,127,4,150,254,138,236,205,93,
            222,114,67,29,24,72,243,141,128,195,78,66,215,61,156,180
        };
        return kTable;
    }
};

} // namespace Limx
