/*******************************************************************************
 * 文件: TEnumFlags.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   类型安全枚举位标志 — 对 enum class 提供位运算支持
 *   通过宏自动生成位运算操作符重载，保持 enum class 的类型安全
 *   用于渲染状态标志、权限位、功能开关等位标志场景
 *
 * 设计哲学:
 *   宏生成 — LIMX_ENUM_FLAGS 宏为指定枚举生成全套位运算操作符
 *   类型安全 — 不允许隐式整数转换，所有操作返回原枚举类型
 *   TEnumFlags 包装 — 提供显式的标志集合类型 (可选)
 *
 * 技术特性:
 *   - LIMX_ENUM_FLAGS(EnumType): 生成 |, &, ^, ~, |=, &=, ^= 操作符
 *   - TEnumFlags<E>: 可选的标志集合包装器 (Set/Clear/Has/IsEmpty)
 *   - HasFlag/SetFlag/ClearFlag: 便捷函数
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

/// 类型安全枚举标志集合包装器
/// @tparam E enum class 类型 (底层类型应为整数)
template<typename E>
class TEnumFlags
{
    using UnderlyingType = typename TUnderlyingType<E>::Type;

public:
    /// 默认构造 — 无标志
    constexpr TEnumFlags() : m_Value(static_cast<UnderlyingType>(0)) {}

    /// 从单个枚举值构造
    constexpr TEnumFlags(E flag)
        : m_Value(static_cast<UnderlyingType>(flag)) {}

    /// 从底层整数构造 (显式)
    constexpr explicit TEnumFlags(UnderlyingType value)
        : m_Value(value) {}

    // ========================================================================
    // 设置与清除
    // ========================================================================

    /// 设置标志
    constexpr void Set(E flag)
    {
        m_Value |= static_cast<UnderlyingType>(flag);
    }

    /// 清除标志
    constexpr void Clear(E flag)
    {
        m_Value &= ~static_cast<UnderlyingType>(flag);
    }

    /// 切换标志
    constexpr void Toggle(E flag)
    {
        m_Value ^= static_cast<UnderlyingType>(flag);
    }

    /// 清除所有标志
    constexpr void ClearAll()
    {
        m_Value = static_cast<UnderlyingType>(0);
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 是否包含指定标志
    LIMX_NODISCARD constexpr bool Has(E flag) const
    {
        UnderlyingType f = static_cast<UnderlyingType>(flag);
        return (m_Value & f) == f;
    }

    /// 是否包含任意一个指定标志
    LIMX_NODISCARD constexpr bool HasAny(TEnumFlags flags) const
    {
        return (m_Value & flags.m_Value) != 0;
    }

    /// 是否包含所有指定标志
    LIMX_NODISCARD constexpr bool HasAll(TEnumFlags flags) const
    {
        return (m_Value & flags.m_Value) == flags.m_Value;
    }

    /// 是否无标志
    LIMX_NODISCARD constexpr bool IsEmpty() const
    {
        return m_Value == static_cast<UnderlyingType>(0);
    }

    /// 获取底层值
    LIMX_NODISCARD constexpr UnderlyingType GetValue() const
    {
        return m_Value;
    }

    // ========================================================================
    // 位运算操作符
    // ========================================================================

    LIMX_NODISCARD constexpr TEnumFlags operator|(TEnumFlags other) const
    {
        return TEnumFlags(
            static_cast<UnderlyingType>(m_Value | other.m_Value));
    }

    LIMX_NODISCARD constexpr TEnumFlags operator&(TEnumFlags other) const
    {
        return TEnumFlags(
            static_cast<UnderlyingType>(m_Value & other.m_Value));
    }

    LIMX_NODISCARD constexpr TEnumFlags operator^(TEnumFlags other) const
    {
        return TEnumFlags(
            static_cast<UnderlyingType>(m_Value ^ other.m_Value));
    }

    LIMX_NODISCARD constexpr TEnumFlags operator~() const
    {
        return TEnumFlags(
            static_cast<UnderlyingType>(~m_Value));
    }

    constexpr TEnumFlags& operator|=(TEnumFlags other)
    {
        m_Value |= other.m_Value;
        return *this;
    }

    constexpr TEnumFlags& operator&=(TEnumFlags other)
    {
        m_Value &= other.m_Value;
        return *this;
    }

    constexpr TEnumFlags& operator^=(TEnumFlags other)
    {
        m_Value ^= other.m_Value;
        return *this;
    }

    // ========================================================================
    // 比较
    // ========================================================================

    LIMX_NODISCARD constexpr bool operator==(TEnumFlags other) const
    {
        return m_Value == other.m_Value;
    }

    LIMX_NODISCARD constexpr bool operator!=(TEnumFlags other) const
    {
        return m_Value != other.m_Value;
    }

private:
    UnderlyingType m_Value;
};

} // namespace Limx

/// 为 enum class 生成位运算操作符重载
/// 在枚举定义后、namespace 中使用
#define LIMX_ENUM_FLAGS(EnumType)                                             \
    LIMX_NODISCARD inline constexpr EnumType operator|(                       \
        EnumType lhs, EnumType rhs)                                           \
    {                                                                         \
        using T = Limx::TUnderlyingType<EnumType>::Type;                      \
        return static_cast<EnumType>(                                         \
            static_cast<T>(lhs) | static_cast<T>(rhs));                       \
    }                                                                         \
    LIMX_NODISCARD inline constexpr EnumType operator&(                       \
        EnumType lhs, EnumType rhs)                                           \
    {                                                                         \
        using T = Limx::TUnderlyingType<EnumType>::Type;                      \
        return static_cast<EnumType>(                                         \
            static_cast<T>(lhs) & static_cast<T>(rhs));                       \
    }                                                                         \
    LIMX_NODISCARD inline constexpr EnumType operator^(                       \
        EnumType lhs, EnumType rhs)                                           \
    {                                                                         \
        using T = Limx::TUnderlyingType<EnumType>::Type;                      \
        return static_cast<EnumType>(                                         \
            static_cast<T>(lhs) ^ static_cast<T>(rhs));                       \
    }                                                                         \
    LIMX_NODISCARD inline constexpr EnumType operator~(EnumType val)          \
    {                                                                         \
        using T = Limx::TUnderlyingType<EnumType>::Type;                      \
        return static_cast<EnumType>(~static_cast<T>(val));                   \
    }                                                                         \
    inline constexpr EnumType& operator|=(EnumType& lhs, EnumType rhs)        \
    {                                                                         \
        lhs = lhs | rhs;                                                      \
        return lhs;                                                           \
    }                                                                         \
    inline constexpr EnumType& operator&=(EnumType& lhs, EnumType rhs)        \
    {                                                                         \
        lhs = lhs & rhs;                                                      \
        return lhs;                                                           \
    }                                                                         \
    inline constexpr EnumType& operator^=(EnumType& lhs, EnumType rhs)        \
    {                                                                         \
        lhs = lhs ^ rhs;                                                      \
        return lhs;                                                           \
    }

/// 便捷函数: 检查枚举标志是否包含指定位
template<typename E>
LIMX_NODISCARD inline constexpr bool HasFlag(E flags, E flag)
{
    using T = typename Limx::TUnderlyingType<E>::Type;
    return (static_cast<T>(flags) & static_cast<T>(flag)) ==
           static_cast<T>(flag);
}
