/*******************************************************************************
 * 文件: GpuTimestampTests.cpp
 * 创建时间: 2026-08-30
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   GPU 时间戳差值计算的用例 — 掩码与回绕
 *
 * 设计哲学:
 *   回绕是"正常运行永远遇不到、遇到时数字荒谬"的那一类情形。真实硬件上
 *   64 位、1 ns/tick 的计数器要走将近六百年才回绕一次, 因此不可能靠跑
 *   引擎来覆盖它 —— 只能把逻辑提成纯函数, 直接喂构造出来的边界值。
 *
 *   同样重要的是: 回绕处理错了不会崩、不会报错, 只会让某一帧的某个 Pass
 *   显示一个荒谬的毫秒数。而那种离群值一旦混进平均值, 整张性能表就都不
 *   能信了。
 *
 ******************************************************************************/

#include "RHITests/RHITestsMinimal.h"

#include "RHI/RHI/IRHIDevice.h"

using namespace Limx;

// ============================================================================
// 掩码构造
// ============================================================================

LIMX_TEST(GpuTimestamp, MaskForZeroBitsIsZero)
{
    // 有效位数为 0 表示该队列根本不支持时间戳
    LIMX_EXPECT_EQ(MakeTimestampMask(0), UInt64(0));
}

LIMX_TEST(GpuTimestamp, MaskForFullWidthIsAllOnes)
{
    const UInt64 allOnes = ~static_cast<UInt64>(0);

    // 位数必须走运行期, 不能是字面量。
    //
    // 写成 MakeTimestampMask(64) 时 MSVC 会常量折叠, 而 1ull << 64 是 UB ——
    // 折叠的结果恰好是 0, 再减 1 回绕成全 1, 于是即便去掉 >= 64 的特判,
    // 用例照样通过。实测确认过: 那个变异用字面量写法抓不住。
    //
    // 走运行期就不同了: x86 的移位指令把位移量掩成 6 位, 1 << (64 & 63)
    // 得到 1, 掩码变成 0; 1 << (100 & 63) 得到 2^36, 掩码变成 2^36-1。
    // 两者都与全 1 相去甚远。
    volatile UInt32 runtimeBits = 64;
    LIMX_EXPECT_EQ(MakeTimestampMask(runtimeBits), allOnes);

    runtimeBits = 100;
    LIMX_EXPECT_EQ(MakeTimestampMask(runtimeBits), allOnes);

    runtimeBits = 255;
    LIMX_EXPECT_EQ(MakeTimestampMask(runtimeBits), allOnes);
}

LIMX_TEST(GpuTimestamp, MaskForPartialWidth)
{
    LIMX_EXPECT_EQ(MakeTimestampMask(1),  UInt64(0x1));
    LIMX_EXPECT_EQ(MakeTimestampMask(8),  UInt64(0xFF));
    LIMX_EXPECT_EQ(MakeTimestampMask(32), UInt64(0xFFFFFFFF));
    LIMX_EXPECT_EQ(MakeTimestampMask(36), UInt64(0xFFFFFFFFF));
    LIMX_EXPECT_EQ(MakeTimestampMask(63), UInt64(0x7FFFFFFFFFFFFFFF));
}

// ============================================================================
// 普通差值
// ============================================================================

LIMX_TEST(GpuTimestamp, PlainDifference)
{
    const UInt64 mask = MakeTimestampMask(64);

    LIMX_EXPECT_EQ(ComputeTimestampDelta(100, 350, mask), UInt64(250));
    LIMX_EXPECT_EQ(ComputeTimestampDelta(0, 0, mask),     UInt64(0));
}

LIMX_TEST(GpuTimestamp, HighBitsAreMaskedOff)
{
    // 36 位有效 —— 高 28 位是驱动未规定的内容
    const UInt32 bits = 36;
    const UInt64 mask = MakeTimestampMask(bits);

    const UInt64 begin = 1000;
    const UInt64 end   = 4000;

    // 往两个值的高位塞入不同的垃圾, 结果必须不受影响
    const UInt64 garbageA = static_cast<UInt64>(0xABCD) << bits;
    const UInt64 garbageB = static_cast<UInt64>(0x1234) << bits;

    LIMX_EXPECT_EQ(
        ComputeTimestampDelta(begin | garbageA, end | garbageB, mask),
        UInt64(3000));

    // 不掩的话高位垃圾会主导结果 —— 这一条是上面那条的对照
    const UInt64 unmasked = (end | garbageB) - (begin | garbageA);
    LIMX_EXPECT_NE(unmasked, UInt64(3000));
}

// ============================================================================
// 回绕
// ============================================================================

LIMX_TEST(GpuTimestamp, WrapAroundAcrossCounterEnd)
{
    // 8 位计数器, 从 250 走到 10 —— 中间越过了 255→0
    const UInt64 mask = MakeTimestampMask(8);

    // 250 → 255 是 5 tick, 255 → 0 是 1 tick, 0 → 10 是 10 tick, 共 16
    LIMX_EXPECT_EQ(ComputeTimestampDelta(250, 10, mask), UInt64(16));
}

LIMX_TEST(GpuTimestamp, WrapAroundExactlyOneTick)
{
    const UInt64 mask = MakeTimestampMask(8);

    // 255 → 0 恰好一个 tick
    LIMX_EXPECT_EQ(ComputeTimestampDelta(255, 0, mask), UInt64(1));
}

LIMX_TEST(GpuTimestamp, WrapAroundAtFullWidth)
{
    const UInt64 mask = MakeTimestampMask(64);
    const UInt64 nearMax = ~static_cast<UInt64>(0) - 4;

    // 全宽下的回绕: max-4 → 5 应为 10 tick
    LIMX_EXPECT_EQ(ComputeTimestampDelta(nearMax, 5, mask), UInt64(10));
}

LIMX_TEST(GpuTimestamp, WrapAroundNeverReturnsZeroOrHuge)
{
    // 回绕处理错误的两种典型后果:
    //   返回 0        —— 谎报成"这段不耗时", 最难被发现
    //   返回接近周期  —— 某一帧突然显示天文数字
    //
    // 这个用例横扫一圈回绕点, 确认两者都不发生。
    const UInt32 bits = 12;
    const UInt64 mask = MakeTimestampMask(bits);
    const UInt64 period = mask + 1;

    for (UInt64 begin = period - 8; begin < period; ++begin)
    {
        for (UInt64 end = 0; end < 8; ++end)
        {
            const UInt64 delta = ComputeTimestampDelta(begin, end, mask);

            const UInt64 expected = period - begin + end;

            LIMX_EXPECT_EQ(delta, expected);
            LIMX_EXPECT_GT(delta, UInt64(0));
            LIMX_EXPECT_LT(delta, UInt64(16));
        }
    }
}

// ============================================================================
// 单调性
// ============================================================================

LIMX_TEST(GpuTimestamp, DeltaGrowsWithEnd)
{
    // 在不跨越回绕点的区间内, 终点越晚差值越大 —— 这条性质看似显然,
    // 但掩码写错 (例如掩成了 validBits-1 位) 会让它在某个点上断掉。
    const UInt64 mask = MakeTimestampMask(16);

    UInt64 previous = 0;

    for (UInt64 end = 100; end < 60000; end += 977)
    {
        const UInt64 delta = ComputeTimestampDelta(100, end, mask);

        LIMX_EXPECT_GE(delta, previous);
        previous = delta;
    }
}
