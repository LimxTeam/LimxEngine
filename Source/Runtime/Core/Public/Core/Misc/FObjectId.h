/*******************************************************************************
 * 文件: FObjectId.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   全局对象标识 — 由类型 ID 和实例 ID 组成的唯一标识符
 *   在引擎运行时唯一标识每个对象实例
 *   用于对象注册表、序列化引用、调试追踪等场景
 *
 * 设计哲学:
 *   紧凑表示 — 128 位 (TypeId:64 + InstanceId:64)
 *   全局唯一 — 原子递增的实例计数器保证唯一性
 *   可比较可哈希 — 支持排序、哈希表键
 *
 * 技术特性:
 *   - FObjectId: 128 位对象标识 (TypeId + InstanceId)
 *   - Generate: 自动生成新的唯一 ID
 *   - GetTypeId/GetInstanceId: 分量访问
 *   - 比较操作符: ==, !=, <
 *   - IsValid: 有效性检查
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

/// 全局对象标识 — TypeId(64) + InstanceId(64)
struct FObjectId
{
    UInt64 TypeId;      ///< 类型标识 (由 FTypeId 生成)
    UInt64 InstanceId;  ///< 实例标识 (全局递增)

    // ========================================================================
    // 构造
    // ========================================================================

    /// 默认构造 — 无效 ID
    constexpr FObjectId()
        : TypeId(0)
        , InstanceId(0)
    {
    }

    /// 从分量构造
    constexpr FObjectId(UInt64 typeId, UInt64 instanceId)
        : TypeId(typeId)
        , InstanceId(instanceId)
    {
    }

    // ========================================================================
    // 生成
    // ========================================================================

    /// 生成新的唯一对象 ID
    /// @param typeId 对象的类型 ID
    /// @return 全局唯一的 FObjectId
    static FObjectId Generate(UInt64 typeId)
    {
        static UInt64 s_NextInstanceId = 1;
        return FObjectId(typeId, s_NextInstanceId++);
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 获取类型 ID
    LIMX_NODISCARD constexpr UInt64 GetTypeId() const
    {
        return TypeId;
    }

    /// 获取实例 ID
    LIMX_NODISCARD constexpr UInt64 GetInstanceId() const
    {
        return InstanceId;
    }

    /// 是否有效 (非零)
    LIMX_NODISCARD constexpr bool IsValid() const
    {
        return TypeId != 0 && InstanceId != 0;
    }

    /// 无效 ID 常量
    LIMX_NODISCARD static constexpr FObjectId Invalid()
    {
        return FObjectId(0, 0);
    }

    // ========================================================================
    // 比较
    // ========================================================================

    LIMX_NODISCARD constexpr bool operator==(
        const FObjectId& other) const
    {
        return TypeId == other.TypeId &&
               InstanceId == other.InstanceId;
    }

    LIMX_NODISCARD constexpr bool operator!=(
        const FObjectId& other) const
    {
        return !(*this == other);
    }

    LIMX_NODISCARD constexpr bool operator<(
        const FObjectId& other) const
    {
        if (TypeId != other.TypeId) return TypeId < other.TypeId;
        return InstanceId < other.InstanceId;
    }

    // ========================================================================
    // 哈希
    // ========================================================================

    /// 计算哈希值 (用于 TMap/TSet)
    LIMX_NODISCARD UInt64 GetHash() const
    {
        // FNV-1a 风格组合
        UInt64 hash = TypeId;
        hash ^= InstanceId + 0x9E3779B97F4A7C15ull +
                 (hash << 12) + (hash >> 4);
        return hash;
    }
};

} // namespace Limx
