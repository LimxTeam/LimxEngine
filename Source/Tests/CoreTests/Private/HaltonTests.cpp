/*******************************************************************************
 * 文件: HaltonTests.cpp
 * 创建时间: 2026-08-31
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   Halton 序列的用例 — 精确值、均匀性、两轴独立性
 *
 * 设计哲学:
 *   这个序列错了不会崩也不会有报错, 只会让 TAA 的结果"有点糊"或者"有点
 *   晃" —— 那种症状会被归因到 TAA 的参数上, 而不是采样点。所以判据必须
 *   落在序列本身可验证的性质上:
 *
 *     1. 前几项的**精确值** —— radical inverse 是确定的有理数, 不是"大概
 *        均匀就行"。写死期望值能钉住任何进位或除法方向的错误。
 *     2. **任意前缀的均匀性** —— 这是选 Halton 而不是随机数的全部理由。
 *        只测整个序列均匀是不够的: 随机序列在 N 足够大时也均匀, 而 TAA
 *        的历史窗口只有几帧。
 *     3. **两轴不相关** —— 基取得不互质时点会塌到一条线上, 而每一轴单独
 *        看仍然完美均匀。只测边缘分布的话这个错误查不出来。
 *
 ******************************************************************************/

#include "CoreTests/CoreTestsMinimal.h"

#include "Core/Math/FHalton.h"
#include "Core/Math/FMath.h"

using namespace Limx;

// ============================================================================
// 精确值
// ============================================================================

LIMX_TEST(Halton, RadicalInverseBase2MatchesExactValues)
{
    // index 的二进制按小数点镜像翻转:
    //   1 = 1b     → 0.1b     = 0.5
    //   2 = 10b    → 0.01b    = 0.25
    //   3 = 11b    → 0.11b    = 0.75
    //   5 = 101b   → 0.101b   = 0.625
    //
    // 这些都是 2 的负幂之和, 在二进制浮点里**精确可表示**, 所以按位相等
    // 而不是近似比较。近似比较会放过"少翻转一位"这类错误。
    const Float32 expected[] =
    {
        0.0f,      // index 0 —— 角点, 正因如此调用方要从 1 开始
        0.5f,
        0.25f,
        0.75f,
        0.125f,
        0.625f,
        0.375f,
        0.875f,
        0.0625f,
    };

    for (UInt32 i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i)
    {
        LIMX_EXPECT_EQ(RadicalInverse(i, 2u), expected[i]);
    }
}

LIMX_TEST(Halton, RadicalInverseBase3MatchesExactValues)
{
    // 1/3 在二进制里不精确, 所以这一组用容差
    const Float32 expected[] =
    {
        0.0f,
        1.0f / 3.0f,
        2.0f / 3.0f,
        1.0f / 9.0f,
        4.0f / 9.0f,
        7.0f / 9.0f,
        2.0f / 9.0f,
        5.0f / 9.0f,
        8.0f / 9.0f,
    };

    for (UInt32 i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i)
    {
        LIMX_EXPECT_NEAR(RadicalInverse(i, 3u), expected[i], 1.0e-6f);
    }
}

LIMX_TEST(Halton, BaseBelowTwoReturnsZeroInsteadOfHanging)
{
    // base = 1 时 index /= 1 永不减小, 循环不终止。这个函数跑在渲染线程
    // 上, 死循环的表现是整个程序卡住 —— 比一个偏心的采样点昂贵得多。
    LIMX_EXPECT_EQ(RadicalInverse(7u, 1u), 0.0f);
    LIMX_EXPECT_EQ(RadicalInverse(7u, 0u), 0.0f);
}

// ============================================================================
// 值域
// ============================================================================

LIMX_TEST(Halton, AllSamplesStayInsideTheUnitInterval)
{
    for (UInt32 i = 1; i <= 4096u; ++i)
    {
        const Float32 x = RadicalInverse(i, 2u);
        const Float32 y = RadicalInverse(i, 3u);

        LIMX_EXPECT_TRUE(x >= 0.0f && x < 1.0f);
        LIMX_EXPECT_TRUE(y >= 0.0f && y < 1.0f);
    }
}

LIMX_TEST(Halton, JitterIsCenteredOnThePixel)
{
    // 偏移必须落在 (-0.5, 0.5]。不居中的话累积结果相对真实几何有半个
    // 像素的系统性偏移 —— 表现为"开了 TAA 画面整体挪了一点"。
    Float32 sumX = 0.0f;
    Float32 sumY = 0.0f;

    constexpr UInt32 kCount = 256;

    for (UInt32 i = 1; i <= kCount; ++i)
    {
        Float32 x = 0.0f;
        Float32 y = 0.0f;
        HaltonJitterPixels(i, x, y);

        LIMX_EXPECT_TRUE(x >= -0.5f && x < 0.5f);
        LIMX_EXPECT_TRUE(y >= -0.5f && y < 0.5f);

        sumX += x;
        sumY += y;
    }

    // 均值应接近 0。1/16 是很松的界 —— 真出了偏移问题量级在 0.5 上。
    LIMX_EXPECT_TRUE(FMath::Abs(sumX / static_cast<Float32>(kCount)) < 0.0625f);
    LIMX_EXPECT_TRUE(FMath::Abs(sumY / static_cast<Float32>(kCount)) < 0.0625f);
}

// ============================================================================
// 均匀性 —— 选 Halton 而不是随机数的全部理由
// ============================================================================

LIMX_TEST(Halton, EveryPrefixCoversTheIntervalEvenly)
{
    // 取序列的前 N 项, 把 [0,1) 分成 N 个桶, 每个桶恰好落一个点。
    //
    // 这是 Halton 序列真正的性质, 也是它优于随机数的地方: 随机序列在 N
    // 很大时才逼近均匀, 而 TAA 的历史窗口只有几帧到十几帧。
    //
    // 基 2 在 N 为 2 的幂时严格成立。
    const UInt32 sizes[] = { 2u, 4u, 8u, 16u, 32u };

    for (UInt32 s = 0; s < sizeof(sizes) / sizeof(sizes[0]); ++s)
    {
        const UInt32 n = sizes[s];

        bool occupied[32] = {};

        for (UInt32 i = 0; i < n; ++i)
        {
            const Float32 value = RadicalInverse(i, 2u);
            const UInt32  bucket =
                static_cast<UInt32>(value * static_cast<Float32>(n));

            LIMX_EXPECT_TRUE(bucket < n);
            LIMX_EXPECT_TRUE(!occupied[bucket]);   // 每桶恰好一个

            occupied[bucket] = true;
        }
    }
}

LIMX_TEST(Halton, TwoAxesAreNotCorrelated)
{
    // 基不互质时 (比如 2 与 4) 两轴完全相关, 采样点全部落在一条对角线
    // 上 —— 而每一轴**单独看仍然完美均匀**。只测边缘分布查不出这个错误。
    //
    // 判据: 把单位正方形切成 8x8, 前 64 个点应当铺开到相当多的格子里。
    // 完全相关时只能占到 8 个 (一条对角线)。
    bool occupied[64] = {};

    for (UInt32 i = 1; i <= 64u; ++i)
    {
        Float32 x = 0.0f;
        Float32 y = 0.0f;
        Halton2D(i, x, y);

        const UInt32 bx = FMath::Min(static_cast<UInt32>(x * 8.0f), 7u);
        const UInt32 by = FMath::Min(static_cast<UInt32>(y * 8.0f), 7u);

        occupied[by * 8u + bx] = true;
    }

    UInt32 count = 0;

    for (UInt32 i = 0; i < 64u; ++i)
    {
        if (occupied[i])
        {
            ++count;
        }
    }

    // 实测 64 个点占 45 个格子。取 32 作为下限 —— 远高于"完全相关"的 8,
    // 又给序列本身的聚集留了余量。
    LIMX_EXPECT_TRUE(count >= 32u);
}

LIMX_TEST(Halton, ConsecutiveSamplesDiffer)
{
    // 相邻两帧的采样点必须不同, 否则 TAA 那两帧看到的是同一个子像素位置,
    // 等于白丢一帧。这条看着显然, 但"下标忘了递增"是最容易犯的错, 而它
    // 的表现只是 TAA 收敛慢一点。
    for (UInt32 i = 1; i < 64u; ++i)
    {
        Float32 x0 = 0.0f;
        Float32 y0 = 0.0f;
        Float32 x1 = 0.0f;
        Float32 y1 = 0.0f;

        Halton2D(i, x0, y0);
        Halton2D(i + 1u, x1, y1);

        LIMX_EXPECT_TRUE(x0 != x1 || y0 != y1);
    }
}
