/*******************************************************************************
 * 文件: FFloat16.h
 * 创建时间: 2026-08-31
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   IEEE 754 半精度 (binary16) 与单精度之间的转换
 *
 * 设计哲学:
 *   完整处理次正规数、无穷与 NaN — 这类转换最常见的写法是只处理正规数
 *     (直接搬指数和尾数), 那对 [2^-14, 65504] 区间内的值完全正确, 而次正
 *     规数会被解成 0、无穷会被解成一个巨大的有限数。两种错误都不会报错,
 *     只会让读回来的数据"看起来差不多对"。
 *
 *   速度缓冲恰好是次正规数的重灾区: 相机几乎静止时的 NDC 位移就在 1e-5
 *     量级, 而半精度的最小正规数是 6.1e-5 — 也就是说"轻微运动"这一整档
 *     全部落在次正规区间里。把它们解成 0 的后果是 TAA 在慢速平移下完全
 *     不做重投影, 表现为拖影。
 *
 * 技术特性:
 *   - 编码走 round-to-nearest-even, 与硬件一致
 *   - 超出半精度范围的值编码为 ±Inf (而不是饱和到 65504)
 *   - 不用任何浮点运算, 纯位操作 — 结果与编译器的浮点设置无关
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"

namespace Limx
{

/// 半精度的位模式 — 用 UInt16 承载, 不引入新类型
///
/// 不做成一个带运算符重载的 FFloat16 类: 半精度在本引擎里只出现在"显存
/// 里的存储格式"这一个位置, 从来不参与计算。给它加运算符只会诱使别人在
/// CPU 上做半精度算术, 而那既慢又损精度。
using Float16Bits = UInt16;

namespace Detail
{

/// 位重解释 — 不经过 memcpy 也不违反严格别名
///
/// C++20 起 union 的"活跃成员之外读取"在 MSVC 上是明确支持的, 而
/// std::bit_cast 需要 <bit> (CRT 头, 本项目不用)。
union FFloatBits
{
    UInt32  AsUInt;
    Float32 AsFloat;
};

} // namespace Detail

/// 半精度位模式 → 单精度
LIMX_NODISCARD inline Float32 Float16ToFloat32(Float16Bits half)
{
    const UInt32 sign     = static_cast<UInt32>(half & 0x8000u) << 16;
    const UInt32 exponent = static_cast<UInt32>((half >> 10) & 0x1Fu);
    const UInt32 mantissa = static_cast<UInt32>(half & 0x03FFu);

    Detail::FFloatBits out;

    if (exponent == 0u)
    {
        if (mantissa == 0u)
        {
            // ±0
            out.AsUInt = sign;
        }
        else
        {
            // 次正规数: 左移到隐含位就位, 移了几次就从指数里扣几次。
            //
            // 半精度次正规数的值是 mantissa * 2^-24。把它写成 1.f * 2^e 的
            // 形式需要先把最高位挪到 bit10, 挪 shift 次之后指数是
            // -14 - shift, 加上单精度的偏置 127 就是下面的 113 - shift。
            UInt32 normalized = mantissa;
            UInt32 shift      = 0u;

            while ((normalized & 0x0400u) == 0u)
            {
                normalized <<= 1;
                ++shift;
            }

            out.AsUInt = sign | ((113u - shift) << 23) |
                         ((normalized & 0x03FFu) << 13);
        }
    }
    else if (exponent == 0x1Fu)
    {
        // Inf 或 NaN。尾数原样搬过去 — 丢掉尾数会把 NaN 变成 Inf, 而那两
        // 者在后续比较里的行为完全不同 (NaN 的任何比较都是 false)。
        out.AsUInt = sign | 0x7F800000u | (mantissa << 13);
    }
    else
    {
        // 正规数: 指数换偏置 (15 → 127), 尾数左对齐
        out.AsUInt = sign | ((exponent + 112u) << 23) | (mantissa << 13);
    }

    return out.AsFloat;
}

/// 单精度 → 半精度位模式 (round-to-nearest-even)
LIMX_NODISCARD inline Float16Bits Float32ToFloat16(Float32 value)
{
    Detail::FFloatBits in;
    in.AsFloat = value;

    const UInt32 bits     = in.AsUInt;
    const UInt32 sign     = (bits >> 16) & 0x8000u;
    const Int32  exponent = static_cast<Int32>((bits >> 23) & 0xFFu) - 127;
    const UInt32 mantissa = bits & 0x007FFFFFu;

    if (((bits >> 23) & 0xFFu) == 0xFFu)
    {
        // Inf / NaN。尾数非零时必须保证结果仍是 NaN —— 单纯右移 13 位可能
        // 把一个尾数很小的 NaN 截成 0, 那就变成了 Inf。
        const UInt32 half = (mantissa != 0u) ? (0x7E00u | (mantissa >> 13))
                                             : 0x7C00u;
        return static_cast<Float16Bits>(sign | half);
    }

    if (exponent > 15)
    {
        // 溢出取 ±Inf 而不是饱和到 65504。
        //
        // 饱和会把"这个值超出了半精度能表示的范围"这件事抹掉, 后续读回来
        // 是一个看似正常的有限数; 取 Inf 至少让问题在第一次使用时暴露。
        return static_cast<Float16Bits>(sign | 0x7C00u);
    }

    if (exponent < -24)
    {
        // 连最小次正规数的一半都不到, 舍入到 ±0
        return static_cast<Float16Bits>(sign);
    }

    if (exponent < -14)
    {
        // 次正规数: 把隐含的 1 显式补回去再右移
        const UInt32 full  = mantissa | 0x00800000u;
        const UInt32 shift = static_cast<UInt32>(-14 - exponent) + 13u;

        const UInt32 result    = full >> shift;
        const UInt32 remainder = full & ((1u << shift) - 1u);
        const UInt32 halfway   = 1u << (shift - 1u);

        // round-to-nearest-even: 正好落在中点时取偶数
        const UInt32 rounded =
            (remainder > halfway || (remainder == halfway && (result & 1u) != 0u))
                ? (result + 1u)
                : result;

        return static_cast<Float16Bits>(sign | rounded);
    }

    // 正规数
    const UInt32 result    = (static_cast<UInt32>(exponent + 15) << 10) |
                             (mantissa >> 13);
    const UInt32 remainder = mantissa & 0x1FFFu;

    const UInt32 rounded =
        (remainder > 0x1000u || (remainder == 0x1000u && (result & 1u) != 0u))
            ? (result + 1u)
            : result;

    // 进位可能把尾数溢出到指数位上 —— 那正是想要的行为 (比如 65519 会进位
    // 成 65536, 半精度表示不了, 结果自然成为 Inf)。不用特判。
    return static_cast<Float16Bits>(sign | rounded);
}

} // namespace Limx
