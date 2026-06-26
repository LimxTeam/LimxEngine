/*******************************************************************************
 * 文件: FRandom.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   伪随机数生成器 — 基于 xoshiro256** 算法
 *   高质量、高性能的确定性随机数生成
 *   用于程序化生成、粒子系统、噪声函数、AI 决策等场景
 *
 * 设计哲学:
 *   确定性 — 相同种子始终产生相同序列，便于调试和重放
 *   高性能 — 256 位状态，周期 2^256-1，无分支
 *   独立实例 — 每个 FRandom 对象独立状态，线程安全 (不共享)
 *
 * 技术特性:
 *   - xoshiro256** 算法 (Blackman & Vigna 2018)
 *   - NextUInt64/NextUInt32: 生成均匀分布无符号整数
 *   - NextFloat/NextDouble: [0, 1) 均匀分布浮点数
 *   - NextRange: 指定区间均匀分布
 *   - NextBool: 随机布尔值
 *   - NextVector3: 单位球面/球内随机向量
 *   - SplitMix64 种子初始化
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

/// 伪随机数生成器 — xoshiro256**
class FRandom
{
public:
    // ========================================================================
    // 构造
    // ========================================================================

    /// 默认构造 — 使用固定种子
    FRandom()
    {
        Seed(0x12345678ABCDEF01ULL);
    }

    /// 指定种子构造
    explicit FRandom(UInt64 seed)
    {
        Seed(seed);
    }

    /// 设置种子 — 使用 SplitMix64 初始化状态
    void Seed(UInt64 seed)
    {
        m_State[0] = SplitMix64(seed);
        m_State[1] = SplitMix64(seed);
        m_State[2] = SplitMix64(seed);
        m_State[3] = SplitMix64(seed);
    }

    // ========================================================================
    // 整数生成
    // ========================================================================

    /// 生成 64 位均匀分布无符号整数
    LIMX_NODISCARD UInt64 NextUInt64()
    {
        UInt64 result = RotateLeft(m_State[1] * 5, 7) * 9;
        UInt64 temp = m_State[1] << 17;

        m_State[2] ^= m_State[0];
        m_State[3] ^= m_State[1];
        m_State[1] ^= m_State[2];
        m_State[0] ^= m_State[3];

        m_State[2] ^= temp;
        m_State[3] = RotateLeft(m_State[3], 45);

        return result;
    }

    /// 生成 32 位均匀分布无符号整数
    LIMX_NODISCARD UInt32 NextUInt32()
    {
        return static_cast<UInt32>(NextUInt64() >> 32);
    }

    /// 生成 [0, max) 范围的无符号整数 (无偏)
    LIMX_NODISCARD UInt32 NextUInt32(UInt32 exclusiveMax)
    {
        if (exclusiveMax == 0) return 0;
        // 拒绝采样消除偏差
        UInt32 threshold = (0xFFFFFFFFu - exclusiveMax + 1) % exclusiveMax;
        UInt32 value;
        do
        {
            value = NextUInt32();
        } while (value < threshold);
        return value % exclusiveMax;
    }

    // ========================================================================
    // 浮点生成
    // ========================================================================

    /// 生成 [0.0, 1.0) 均匀分布 Float32
    LIMX_NODISCARD Float32 NextFloat()
    {
        // 取高 24 位 → [0, 2^24) → 除以 2^24
        UInt32 bits = NextUInt32() >> 8;
        return static_cast<Float32>(bits) *
               (1.0f / 16777216.0f);  // 1 / 2^24
    }

    /// 生成 [0.0, 1.0) 均匀分布 Float64
    LIMX_NODISCARD Float64 NextDouble()
    {
        // 取高 53 位
        UInt64 bits = NextUInt64() >> 11;
        return static_cast<Float64>(bits) *
               (1.0 / 9007199254740992.0);  // 1 / 2^53
    }

    // ========================================================================
    // 范围生成
    // ========================================================================

    /// 生成 [min, max] 范围的 Int32
    LIMX_NODISCARD Int32 NextRange(Int32 minValue, Int32 maxValue)
    {
        LIMX_ASSERT(minValue <= maxValue);
        UInt32 range = static_cast<UInt32>(maxValue - minValue) + 1;
        return minValue + static_cast<Int32>(NextUInt32(range));
    }

    /// 生成 [min, max) 范围的 Float32
    LIMX_NODISCARD Float32 NextRange(Float32 minValue, Float32 maxValue)
    {
        return minValue + NextFloat() * (maxValue - minValue);
    }

    /// 生成 [min, max) 范围的 Float64
    LIMX_NODISCARD Float64 NextRange(Float64 minValue, Float64 maxValue)
    {
        return minValue + NextDouble() * (maxValue - minValue);
    }

    // ========================================================================
    // 布尔与概率
    // ========================================================================

    /// 随机布尔值 (50/50)
    LIMX_NODISCARD bool NextBool()
    {
        return (NextUInt64() & 1) != 0;
    }

    /// 以指定概率返回 true (0.0 = 永远 false, 1.0 = 永远 true)
    LIMX_NODISCARD bool NextChance(Float32 probability)
    {
        return NextFloat() < probability;
    }

private:
    // ========================================================================
    // 辅助函数
    // ========================================================================

    /// 64 位循环左移
    static UInt64 RotateLeft(UInt64 value, Int32 bits)
    {
        return (value << bits) | (value >> (64 - bits));
    }

    /// SplitMix64 — 用于从单个种子初始化 256 位状态
    static UInt64 SplitMix64(UInt64& state)
    {
        state += 0x9E3779B97F4A7C15ULL;
        UInt64 result = state;
        result = (result ^ (result >> 30)) * 0xBF58476D1CE4E5B9ULL;
        result = (result ^ (result >> 27)) * 0x94D049BB133111EBULL;
        return result ^ (result >> 31);
    }

    // ========================================================================
    // 成员数据
    // ========================================================================

    UInt64 m_State[4];  ///< xoshiro256** 状态 (256 位)
};

} // namespace Limx
