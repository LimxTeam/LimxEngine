/*******************************************************************************
 * 文件: TPair.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   通用键值对类型 — 替代 std::pair 的零 STL 依赖实现
 *   用于需要将两个相关值绑定在一起的场景
 *   与 TMap 的 TKeyValuePair 不同，TPair 不限定语义为 Key/Value
 *
 * 设计哲学:
 *   值语义 — 轻量 POD 风格聚合类型
 *   显式命名 — First/Second 明确语义
 *   constexpr — 完全编译时可用
 *
 * 技术特性:
 *   - 成员: First, Second
 *   - 比较: 字典序比较
 *   - 工厂: MakePair(a, b)
 *   - 结构化绑定兼容 (C++17 聚合)
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/TypeTraits/TypeTraits.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/TypeTraits/TypeTraits.h"

namespace Limx
{

/// 通用键值对
/// @tparam TFirst  第一个元素的类型
/// @tparam TSecond 第二个元素的类型
template<typename TFirst, typename TSecond>
struct TPair
{
    using FirstType = TFirst;
    using SecondType = TSecond;

    TFirst  First;
    TSecond Second;

    // ========================================================================
    // 构造
    // ========================================================================

    /// 默认构造
    constexpr TPair() : First{}, Second{} {}

    /// 值构造
    constexpr TPair(const TFirst& inFirst, const TSecond& inSecond)
        : First(inFirst), Second(inSecond) {}

    /// 移动构造
    constexpr TPair(TFirst&& inFirst, TSecond&& inSecond)
        : First(MoveTemp(inFirst)), Second(MoveTemp(inSecond)) {}

    /// 混合构造
    constexpr TPair(const TFirst& inFirst, TSecond&& inSecond)
        : First(inFirst), Second(MoveTemp(inSecond)) {}

    constexpr TPair(TFirst&& inFirst, const TSecond& inSecond)
        : First(MoveTemp(inFirst)), Second(inSecond) {}

    // ========================================================================
    // 比较 — 字典序
    // ========================================================================

    LIMX_NODISCARD constexpr bool operator==(const TPair& other) const
    {
        return First == other.First && Second == other.Second;
    }

    LIMX_NODISCARD constexpr bool operator!=(const TPair& other) const
    {
        return !(*this == other);
    }

    LIMX_NODISCARD constexpr bool operator<(const TPair& other) const
    {
        if (First < other.First) return true;
        if (other.First < First) return false;
        return Second < other.Second;
    }

    LIMX_NODISCARD constexpr bool operator>(const TPair& other) const
    {
        return other < *this;
    }

    LIMX_NODISCARD constexpr bool operator<=(const TPair& other) const
    {
        return !(other < *this);
    }

    LIMX_NODISCARD constexpr bool operator>=(const TPair& other) const
    {
        return !(*this < other);
    }
};

/// 工厂函数 — 自动推导类型
template<typename TFirst, typename TSecond>
LIMX_NODISCARD constexpr TPair<DecayT<TFirst>, DecayT<TSecond>>
MakePair(TFirst&& first, TSecond&& second)
{
    return TPair<DecayT<TFirst>, DecayT<TSecond>>(
        Forward<TFirst>(first), Forward<TSecond>(second));
}

} // namespace Limx
