/*******************************************************************************
 * 文件: Float16Tests.cpp
 * 创建时间: 2026-08-31
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   半精度与单精度互转的用例 — 覆盖次正规、无穷、NaN 与舍入边界
 *
 * 设计哲学:
 *   正规数区间怎么写都对, 所以这里几乎不测它。真正会出错的是四处:
 *     1. 次正规数 (小于 6.1e-5) —— 只搬指数尾数的写法会把它们全解成 0
 *     2. Inf/NaN —— 尾数被截断的 NaN 会变成 Inf, 而 NaN 的比较全是 false,
 *        Inf 的不是, 后果完全不同
 *     3. 溢出 —— 饱和到 65504 会把"超范围"这件事抹掉
 *     4. 舍入中点 —— 截断而非就近舍入会让往返误差翻倍
 *
 *   全部用位模式断言, 不用浮点比较。位模式是唯一能把"差一个 ulp"和"完全
 *   正确"区分开的判据, 而这类转换的 bug 恰恰就是差几个 ulp。
 *
 ******************************************************************************/

#include "CoreTests/CoreTestsMinimal.h"

#include "Core/Math/FFloat16.h"

using namespace Limx;

namespace
{

/// 取单精度的位模式 — 断言里要按位比, 不按值比
UInt32 FloatBits(Float32 value)
{
    Detail::FFloatBits bits;
    bits.AsFloat = value;
    return bits.AsUInt;
}

} // namespace

// ============================================================================
// 正规数 — 基准线, 保证这套位操作的框架没搭错
// ============================================================================

LIMX_TEST(Float16, NormalValuesDecodeExactly)
{
    // 这些值在半精度里都能精确表示, 解码结果必须逐位等于单精度的对应值
    struct FCase
    {
        Float16Bits Half;
        Float32     Expected;
    };

    const FCase cases[] =
    {
        { 0x3C00u,  1.0f     },
        { 0xBC00u, -1.0f     },
        { 0x3800u,  0.5f     },
        { 0x4000u,  2.0f     },
        { 0xC000u, -2.0f     },
        { 0x0000u,  0.0f     },
        { 0x7BFFu,  65504.0f },   // 半精度能表示的最大有限值
        { 0x0400u,  0.00006103515625f },  // 最小正规数 2^-14
    };

    for (SizeType i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        LIMX_EXPECT_EQ(FloatBits(Float16ToFloat32(cases[i].Half)),
                       FloatBits(cases[i].Expected));
    }
}

LIMX_TEST(Float16, NegativeZeroKeepsItsSign)
{
    // -0 与 +0 的比较结果相等, 所以按值断言这条永远通过。必须按位比。
    LIMX_EXPECT_EQ(FloatBits(Float16ToFloat32(0x8000u)), 0x80000000u);
    LIMX_EXPECT_EQ(FloatBits(Float16ToFloat32(0x0000u)), 0x00000000u);

    LIMX_EXPECT_EQ(static_cast<UInt32>(Float32ToFloat16(-0.0f)), 0x8000u);
    LIMX_EXPECT_EQ(static_cast<UInt32>(Float32ToFloat16(0.0f)), 0x0000u);
}

// ============================================================================
// 次正规数 — 速度缓冲的实际工作区间
// ============================================================================

LIMX_TEST(Float16, SubnormalsDecodeToTheirExactValue)
{
    // 半精度次正规数的值就是 mantissa * 2^-24, 没有隐含的 1。
    //
    // 这一档在速度缓冲里不是边角料: 相机每帧转动 0.001 弧度时的 NDC 位移
    // 就在 1e-3 到 1e-5 之间, 下半截全落在次正规区。解成 0 的后果是慢速
    // 运动下 TAA 完全不做重投影。
    constexpr Float32 kUnit = 5.9604644775390625e-8f;   // 2^-24

    struct FCase
    {
        Float16Bits Half;
        Float32     Expected;
    };

    const FCase cases[] =
    {
        { 0x0001u, 1.0f    * kUnit },   // 最小次正规数
        { 0x0002u, 2.0f    * kUnit },
        { 0x0200u, 512.0f  * kUnit },
        { 0x03FFu, 1023.0f * kUnit },   // 最大次正规数
        { 0x8001u, -1.0f   * kUnit },
    };

    for (SizeType i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        LIMX_EXPECT_EQ(FloatBits(Float16ToFloat32(cases[i].Half)),
                       FloatBits(cases[i].Expected));
    }
}

LIMX_TEST(Float16, EverySubnormalRoundTripsExactly)
{
    // 全部 1023 个正次正规数逐个往返。
    //
    // 逐个而不是抽样: 次正规的解码里有个"左移到隐含位就位"的循环, 移位次数
    // 与前导零个数相关, 抽样很容易恰好只覆盖其中一两种移位次数。
    for (UInt32 raw = 1u; raw <= 0x03FFu; ++raw)
    {
        const Float16Bits half = static_cast<Float16Bits>(raw);
        const Float32     wide = Float16ToFloat32(half);

        LIMX_EXPECT_EQ(static_cast<UInt32>(Float32ToFloat16(wide)), raw);
    }
}

LIMX_TEST(Float16, ValueBelowSmallestSubnormalFlushesToZero)
{
    // 2^-25 恰好是最小次正规数的一半 —— 舍入中点, 取偶数即 0
    LIMX_EXPECT_EQ(static_cast<UInt32>(Float32ToFloat16(2.9802322e-8f)),
                   0x0000u);

    // 更小的值同样归零, 但符号要保住
    LIMX_EXPECT_EQ(static_cast<UInt32>(Float32ToFloat16(-1.0e-20f)), 0x8000u);
}

// ============================================================================
// 无穷与 NaN
// ============================================================================

LIMX_TEST(Float16, InfinityDecodesToInfinity)
{
    const Float32 positive = Float16ToFloat32(0x7C00u);
    const Float32 negative = Float16ToFloat32(0xFC00u);

    LIMX_EXPECT_EQ(FloatBits(positive), 0x7F800000u);
    LIMX_EXPECT_EQ(FloatBits(negative), 0xFF800000u);
}

LIMX_TEST(Float16, NanStaysNan)
{
    // 尾数非零的指数全 1 就是 NaN。解码后必须仍然是 NaN ——
    // 把尾数丢掉会变成 Inf, 而 NaN 的任何比较都是 false, Inf 不是。
    const Float32 decoded = Float16ToFloat32(0x7E00u);
    const UInt32  bits    = FloatBits(decoded);

    LIMX_EXPECT_EQ(bits & 0x7F800000u, 0x7F800000u);   // 指数全 1
    LIMX_EXPECT_TRUE((bits & 0x007FFFFFu) != 0u);      // 尾数非零

    // NaN 与自己不相等 —— 这是 NaN 唯一的可观测特征
    LIMX_EXPECT_TRUE(!(decoded == decoded));
}

LIMX_TEST(Float16, SmallMantissaNanDoesNotBecomeInfinity)
{
    // 单精度 NaN 的尾数只有低位非零时, 单纯右移 13 位会把尾数截成 0 ——
    // 那就静悄悄地变成了 Inf。
    Detail::FFloatBits nan;
    nan.AsUInt = 0x7F800001u;   // 指数全 1, 尾数只有最低位

    const Float16Bits half = Float32ToFloat16(nan.AsFloat);

    LIMX_EXPECT_EQ(static_cast<UInt32>(half) & 0x7C00u, 0x7C00u);  // 指数全 1
    LIMX_EXPECT_TRUE((static_cast<UInt32>(half) & 0x03FFu) != 0u); // 尾数非零
}

// ============================================================================
// 溢出
// ============================================================================

LIMX_TEST(Float16, OverflowBecomesInfinityNotSaturation)
{
    // 饱和到 65504 会把"这个值超范围了"这件事抹掉, 读回来是个看似正常的
    // 有限数。取 Inf 让问题在第一次参与运算时就暴露。
    LIMX_EXPECT_EQ(static_cast<UInt32>(Float32ToFloat16(1.0e10f)), 0x7C00u);
    LIMX_EXPECT_EQ(static_cast<UInt32>(Float32ToFloat16(-1.0e10f)), 0xFC00u);

    // 65520 是舍入到 Inf 的临界点 (65504 与 65536 的中点, 取偶数即 Inf)
    LIMX_EXPECT_EQ(static_cast<UInt32>(Float32ToFloat16(65520.0f)), 0x7C00u);

    // 刚好在临界点之下, 必须仍是最大有限值
    LIMX_EXPECT_EQ(static_cast<UInt32>(Float32ToFloat16(65519.0f)), 0x7BFFu);
}

// ============================================================================
// 舍入
// ============================================================================

LIMX_TEST(Float16, RoundsToNearestEvenAtTheMidpoint)
{
    // 1.0 与它的下一个半精度值 (1 + 2^-10) 的中点。
    // round-to-nearest-even 要求取尾数为偶数的那个, 即 1.0 本身。
    LIMX_EXPECT_EQ(static_cast<UInt32>(Float32ToFloat16(1.00048828125f)),
                   0x3C00u);

    // 下一个中点落在尾数为奇数的一侧, 必须进位
    LIMX_EXPECT_EQ(static_cast<UInt32>(Float32ToFloat16(1.00146484375f)),
                   0x3C02u);

    // 中点之上一点点 —— 无论哪种偶数规则都必须进位
    LIMX_EXPECT_EQ(static_cast<UInt32>(Float32ToFloat16(1.0005f)), 0x3C01u);
}

LIMX_TEST(Float16, EveryFiniteBitPatternRoundTrips)
{
    // 全部 65536 个位模式扫一遍, 有限值必须逐位往返。
    //
    // 穷举而不是抽样: 这套代码的分支按指数分档, 抽样很难保证每一档都被
    // 覆盖到, 而漏掉的那一档正好是次正规或临界指数的概率不低。
    UInt32 finiteCount = 0u;

    for (UInt32 raw = 0u; raw < 0x10000u; ++raw)
    {
        const Float16Bits half = static_cast<Float16Bits>(raw);

        // 跳过 Inf/NaN —— NaN 的位模式不保证往返 (尾数会被规范化)
        if ((raw & 0x7C00u) == 0x7C00u)
        {
            continue;
        }

        ++finiteCount;

        const Float32 wide = Float16ToFloat32(half);
        LIMX_EXPECT_EQ(static_cast<UInt32>(Float32ToFloat16(wide)), raw);
    }

    // 断言真的扫到了预期数量 —— 否则上面的循环整个被跳过也是"全绿"
    LIMX_EXPECT_EQ(finiteCount, 63488u);   // 65536 - 2 * 1024
}
