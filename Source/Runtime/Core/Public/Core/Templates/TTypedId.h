/*******************************************************************************
 * 文件: TTypedId.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   类型安全 ID — 防混淆的强类型标识符
 *   不同标签类型的 ID 之间无法隐式转换或比较
 *   用于实体 ID、资源句柄、组件索引等需要防混淆的场景
 *
 * 设计哲学:
 *   标签类型 — 以空结构体标签区分不同含义的 ID
 *   零开销 — 编译时类型检查，运行时等价于裸整数
 *   无效值 — 提供 Invalid() 静态常量表示无效 ID
 *
 * 技术特性:
 *   - TTypedId<TagType, ValueType>: 强类型 ID
 *   - IsValid: 有效性检查
 *   - GetValue: 获取底层值
 *   - Invalid: 无效 ID 常量
 *   - operator==/!=/<: 同类型比较
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

/// 类型安全 ID
/// @tparam TagType 标签类型 (空结构体，仅用于区分类型)
/// @tparam ValueType 底层值类型 (默认 UInt32)
template<typename TagType, typename ValueType = UInt32>
class TTypedId
{
    static constexpr ValueType kInvalidValue =
        static_cast<ValueType>(-1);

public:
    /// 默认构造 — 无效 ID
    TTypedId() : m_Value(kInvalidValue) {}

    /// 从值构造
    explicit TTypedId(ValueType value) : m_Value(value) {}

    /// 无效 ID
    LIMX_NODISCARD static TTypedId Invalid()
    {
        return TTypedId();
    }

    /// 从索引构造 (语义更明确)
    LIMX_NODISCARD static TTypedId FromIndex(ValueType index)
    {
        return TTypedId(index);
    }

    // ========================================================================
    // 访问
    // ========================================================================

    /// 获取底层值
    LIMX_NODISCARD ValueType GetValue() const
    {
        return m_Value;
    }

    /// 是否有效
    LIMX_NODISCARD bool IsValid() const
    {
        return m_Value != kInvalidValue;
    }

    /// 布尔转换
    LIMX_NODISCARD explicit operator bool() const
    {
        return IsValid();
    }

    // ========================================================================
    // 修改
    // ========================================================================

    /// 重置为无效
    void Invalidate()
    {
        m_Value = kInvalidValue;
    }

    // ========================================================================
    // 比较 (仅同类型可比较)
    // ========================================================================

    LIMX_NODISCARD bool operator==(
        const TTypedId& other) const
    {
        return m_Value == other.m_Value;
    }

    LIMX_NODISCARD bool operator!=(
        const TTypedId& other) const
    {
        return m_Value != other.m_Value;
    }

    LIMX_NODISCARD bool operator<(
        const TTypedId& other) const
    {
        return m_Value < other.m_Value;
    }

    LIMX_NODISCARD bool operator<=(
        const TTypedId& other) const
    {
        return m_Value <= other.m_Value;
    }

    LIMX_NODISCARD bool operator>(
        const TTypedId& other) const
    {
        return m_Value > other.m_Value;
    }

    LIMX_NODISCARD bool operator>=(
        const TTypedId& other) const
    {
        return m_Value >= other.m_Value;
    }

private:
    ValueType m_Value;  ///< 底层值
};

/// 声明类型安全 ID 的便捷宏
/// 用法: LIMX_DECLARE_TYPED_ID(FEntityId)
///   => struct FEntityId_Tag {};
///      using FEntityId = TTypedId<FEntityId_Tag>;
#define LIMX_DECLARE_TYPED_ID(Name) \
    struct Name##_Tag {}; \
    using Name = TTypedId<Name##_Tag>

/// 带自定义值类型的声明
#define LIMX_DECLARE_TYPED_ID_EX(Name, ValType) \
    struct Name##_Tag {}; \
    using Name = TTypedId<Name##_Tag, ValType>

} // namespace Limx
