/*******************************************************************************
 * 文件: FTypeId.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   编译时类型标识 — 替代 RTTI (typeid) 的零开销类型识别系统
 *   每个类型在编译时获得唯一的 UInt64 标识符
 *   用于类型擦除容器的类型检查、组件系统类型匹配、序列化类型标签等场景
 *
 * 设计哲学:
 *   零开销 — 编译时确定类型 ID，运行时仅比较整数
 *   函数地址唯一性 — 利用模板函数实例化的唯一地址作为类型标识
 *   无 RTTI — 不依赖编译器 RTTI 支持 (/GR-)
 *
 * 技术特性:
 *   - TypeIdOf<T>(): 获取类型 T 的唯一 UInt64 标识
 *   - FTypeId: 包装类型，支持比较和哈希
 *   - 每个不同类型 T 在同一编译单元中具有不同的 ID
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

namespace TypeIdDetail
{
    /// 每个类型 T 实例化独立的函数 — 函数地址作为类型唯一标识
    template<typename T>
    void TypeIdAnchor() {}

    /// 获取类型标识 — 将函数指针转换为 UInt64
    template<typename T>
    LIMX_NODISCARD UInt64 GetTypeIdImpl()
    {
        return reinterpret_cast<UInt64>(&TypeIdAnchor<T>);
    }
} // namespace TypeIdDetail

/// 类型标识 — 唯一标识一个 C++ 类型
struct FTypeId
{
    UInt64 Id;

    /// 默认构造 — 无效 ID
    constexpr FTypeId() : Id(0) {}

    /// 从 ID 构造
    constexpr explicit FTypeId(UInt64 id) : Id(id) {}

    /// 是否有效
    LIMX_NODISCARD constexpr bool IsValid() const { return Id != 0; }

    // 比较运算符
    LIMX_NODISCARD constexpr bool operator==(const FTypeId& other) const
    {
        return Id == other.Id;
    }

    LIMX_NODISCARD constexpr bool operator!=(const FTypeId& other) const
    {
        return Id != other.Id;
    }

    LIMX_NODISCARD constexpr bool operator<(const FTypeId& other) const
    {
        return Id < other.Id;
    }

    /// 获取哈希值 (直接使用 ID)
    LIMX_NODISCARD constexpr UInt64 GetHash() const { return Id; }
};

/// 获取类型 T 的编译时唯一标识
template<typename T>
LIMX_NODISCARD FTypeId TypeIdOf()
{
    return FTypeId(TypeIdDetail::GetTypeIdImpl<T>());
}

} // namespace Limx
