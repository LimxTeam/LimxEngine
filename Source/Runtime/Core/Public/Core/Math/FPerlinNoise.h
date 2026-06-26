/*******************************************************************************
 * 文件: FPerlinNoise.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   Perlin 噪声 — 2D/3D 梯度噪声生成器
 *   基于改进的 Perlin 噪声算法 (Ken Perlin 2002)
 *   用于程序化地形、纹理生成、粒子扰动、云雾效果等场景
 *
 * 设计哲学:
 *   确定性 — 相同输入永远产生相同输出
 *   可平铺 — 基于置换表的周期性保证无缝平铺
 *   分形叠加 — 提供 FBM (分形布朗运动) 多倍频叠加
 *
 * 技术特性:
 *   - FPerlinNoise: Perlin 噪声生成器
 *   - Noise2D/Noise3D: 2D/3D 噪声采样
 *   - FBM2D/FBM3D: 分形布朗运动 (多倍频叠加)
 *   - 输出范围约 [-1, 1]
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

/// Perlin 噪声生成器
class FPerlinNoise
{
public:
    /// 使用默认置换表构造
    FPerlinNoise()
    {
        InitDefaultPermutation();
    }

    /// 使用种子构造 (打乱置换表)
    explicit FPerlinNoise(UInt32 seed)
    {
        InitDefaultPermutation();
        ShufflePermutation(seed);
    }

    // ========================================================================
    // 2D 噪声
    // ========================================================================

    /// 2D Perlin 噪声
    /// @return 约 [-1, 1] 范围的噪声值
    LIMX_NODISCARD Float32 Noise2D(
        Float32 x, Float32 y) const
    {
        // 确定晶格单元
        Int32 xi = FloorToInt(x);
        Int32 yi = FloorToInt(y);

        // 单元内相对坐标
        Float32 xf = x - static_cast<Float32>(xi);
        Float32 yf = y - static_cast<Float32>(yi);

        // 缓动曲线
        Float32 u = Fade(xf);
        Float32 v = Fade(yf);

        // 哈希角点
        Int32 aa = Perm(Perm(xi) + yi);
        Int32 ab = Perm(Perm(xi) + yi + 1);
        Int32 ba = Perm(Perm(xi + 1) + yi);
        Int32 bb = Perm(Perm(xi + 1) + yi + 1);

        // 梯度点积 + 插值
        Float32 x1 = Lerp(Grad2D(aa, xf, yf),
                           Grad2D(ba, xf - 1.0f, yf), u);
        Float32 x2 = Lerp(Grad2D(ab, xf, yf - 1.0f),
                           Grad2D(bb, xf - 1.0f, yf - 1.0f),
                           u);

        return Lerp(x1, x2, v);
    }

    // ========================================================================
    // 3D 噪声
    // ========================================================================

    /// 3D Perlin 噪声
    LIMX_NODISCARD Float32 Noise3D(
        Float32 x, Float32 y, Float32 z) const
    {
        Int32 xi = FloorToInt(x);
        Int32 yi = FloorToInt(y);
        Int32 zi = FloorToInt(z);

        Float32 xf = x - static_cast<Float32>(xi);
        Float32 yf = y - static_cast<Float32>(yi);
        Float32 zf = z - static_cast<Float32>(zi);

        Float32 u = Fade(xf);
        Float32 v = Fade(yf);
        Float32 w = Fade(zf);

        Int32 aaa = Perm(Perm(Perm(xi) + yi) + zi);
        Int32 aba = Perm(Perm(Perm(xi) + yi + 1) + zi);
        Int32 aab = Perm(Perm(Perm(xi) + yi) + zi + 1);
        Int32 abb = Perm(Perm(Perm(xi) + yi + 1) + zi + 1);
        Int32 baa = Perm(Perm(Perm(xi + 1) + yi) + zi);
        Int32 bba = Perm(Perm(Perm(xi + 1) + yi + 1) + zi);
        Int32 bab = Perm(Perm(Perm(xi + 1) + yi) + zi + 1);
        Int32 bbb = Perm(Perm(Perm(xi + 1) + yi + 1) +
                         zi + 1);

        Float32 x1 = Lerp(
            Grad3D(aaa, xf, yf, zf),
            Grad3D(baa, xf - 1.0f, yf, zf), u);
        Float32 x2 = Lerp(
            Grad3D(aba, xf, yf - 1.0f, zf),
            Grad3D(bba, xf - 1.0f, yf - 1.0f, zf), u);
        Float32 y1 = Lerp(x1, x2, v);

        x1 = Lerp(
            Grad3D(aab, xf, yf, zf - 1.0f),
            Grad3D(bab, xf - 1.0f, yf, zf - 1.0f), u);
        x2 = Lerp(
            Grad3D(abb, xf, yf - 1.0f, zf - 1.0f),
            Grad3D(bbb, xf - 1.0f, yf - 1.0f, zf - 1.0f),
            u);
        Float32 y2 = Lerp(x1, x2, v);

        return Lerp(y1, y2, w);
    }

    // ========================================================================
    // 分形布朗运动 (FBM)
    // ========================================================================

    /// 2D FBM
    /// @param octaves 倍频数 (层数)
    /// @param lacunarity 频率倍增因子 (通常 2.0)
    /// @param persistence 振幅衰减因子 (通常 0.5)
    LIMX_NODISCARD Float32 FBM2D(
        Float32 x, Float32 y,
        Int32 octaves = 6,
        Float32 lacunarity = 2.0f,
        Float32 persistence = 0.5f) const
    {
        Float32 total = 0.0f;
        Float32 frequency = 1.0f;
        Float32 amplitude = 1.0f;
        Float32 maxAmplitude = 0.0f;

        for (Int32 octave = 0;
             octave < octaves; ++octave)
        {
            total += Noise2D(x * frequency,
                             y * frequency) * amplitude;
            maxAmplitude += amplitude;
            amplitude *= persistence;
            frequency *= lacunarity;
        }

        return total / maxAmplitude;
    }

    /// 3D FBM
    LIMX_NODISCARD Float32 FBM3D(
        Float32 x, Float32 y, Float32 z,
        Int32 octaves = 6,
        Float32 lacunarity = 2.0f,
        Float32 persistence = 0.5f) const
    {
        Float32 total = 0.0f;
        Float32 frequency = 1.0f;
        Float32 amplitude = 1.0f;
        Float32 maxAmplitude = 0.0f;

        for (Int32 octave = 0;
             octave < octaves; ++octave)
        {
            total += Noise3D(x * frequency,
                             y * frequency,
                             z * frequency) * amplitude;
            maxAmplitude += amplitude;
            amplitude *= persistence;
            frequency *= lacunarity;
        }

        return total / maxAmplitude;
    }

private:
    static constexpr Int32 kTableSize = 256;
    Int32 m_Perm[kTableSize * 2]; ///< 置换表 (重复一次避免取模)

    /// 初始化默认置换表 (Ken Perlin 原始表)
    void InitDefaultPermutation()
    {
        static constexpr Int32 kDefaultPerm[kTableSize] = {
            151,160,137,91,90,15,131,13,201,95,96,53,194,233,
            7,225,140,36,103,30,69,142,8,99,37,240,21,10,23,
            190,6,148,247,120,234,75,0,26,197,62,94,252,219,
            203,117,35,11,32,57,177,33,88,237,149,56,87,174,
            20,125,136,171,168,68,175,74,165,71,134,139,48,27,
            166,77,146,158,231,83,111,229,122,60,211,133,230,
            220,105,92,41,55,46,245,40,244,102,143,54,65,25,
            63,161,1,216,80,73,209,76,132,187,208,89,18,169,
            200,196,135,130,116,188,159,86,164,100,109,198,173,
            186,3,64,52,217,226,250,124,123,5,202,38,147,118,
            126,255,82,85,212,207,206,59,227,47,16,58,17,182,
            189,28,42,223,183,170,213,119,248,152,2,44,154,163,
            70,221,153,101,155,167,43,172,9,129,22,39,253,19,
            98,108,110,79,113,224,232,178,185,112,104,218,246,
            97,228,251,34,242,193,238,210,144,12,191,179,162,
            241,81,51,145,235,249,14,239,107,49,192,214,31,181,
            199,106,157,184,84,204,176,115,121,50,45,127,4,150,
            254,138,236,205,93,222,114,67,29,24,72,243,141,128,
            195,78,66,215,61,156,180
        };

        for (Int32 permIdx = 0;
             permIdx < kTableSize; ++permIdx)
        {
            m_Perm[permIdx] = kDefaultPerm[permIdx];
            m_Perm[permIdx + kTableSize] =
                kDefaultPerm[permIdx];
        }
    }

    /// 使用种子打乱置换表 (Fisher-Yates)
    void ShufflePermutation(UInt32 seed)
    {
        // 简易 LCG 随机数
        UInt32 state = seed;
        auto nextRand = [&state]() -> UInt32
        {
            state = state * 1664525u + 1013904223u;
            return state;
        };

        for (Int32 shuffleIdx = kTableSize - 1;
             shuffleIdx > 0; --shuffleIdx)
        {
            Int32 swapIdx = static_cast<Int32>(
                nextRand() % static_cast<UInt32>(
                    shuffleIdx + 1));

            // 交换
            Int32 temp = m_Perm[shuffleIdx];
            m_Perm[shuffleIdx] = m_Perm[swapIdx];
            m_Perm[swapIdx] = temp;
        }

        // 复制到后半部
        for (Int32 copyIdx = 0;
             copyIdx < kTableSize; ++copyIdx)
        {
            m_Perm[copyIdx + kTableSize] = m_Perm[copyIdx];
        }
    }

    /// 置换表查找 (带周期性)
    LIMX_NODISCARD Int32 Perm(Int32 index) const
    {
        return m_Perm[index & (kTableSize - 1)];
    }

    /// 缓动函数 — 6t⁵ - 15t⁴ + 10t³
    LIMX_NODISCARD static Float32 Fade(Float32 t)
    {
        return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    }

    /// 线性插值
    LIMX_NODISCARD static Float32 Lerp(
        Float32 a, Float32 b, Float32 t)
    {
        return a + (b - a) * t;
    }

    /// 向下取整
    LIMX_NODISCARD static Int32 FloorToInt(Float32 value)
    {
        Int32 intVal = static_cast<Int32>(value);
        return (value < static_cast<Float32>(intVal))
            ? intVal - 1 : intVal;
    }

    /// 2D 梯度
    LIMX_NODISCARD static Float32 Grad2D(
        Int32 hash, Float32 x, Float32 y)
    {
        Int32 h = hash & 3;
        Float32 u = (h < 2) ? x : y;
        Float32 v = (h < 2) ? y : x;
        return ((h & 1) ? -u : u) +
               ((h & 2) ? -v : v);
    }

    /// 3D 梯度
    LIMX_NODISCARD static Float32 Grad3D(
        Int32 hash, Float32 x, Float32 y, Float32 z)
    {
        Int32 h = hash & 15;
        Float32 u = (h < 8) ? x : y;
        Float32 v = (h < 4) ? y
            : ((h == 12 || h == 14) ? x : z);
        return ((h & 1) ? -u : u) +
               ((h & 2) ? -v : v);
    }
};

} // namespace Limx
