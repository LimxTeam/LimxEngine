/*******************************************************************************
 * 文件: THandle.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   通用句柄 — 索引 + 代 (Generation) 的轻量引用标识符
 *   通过代标记检测悬挂引用，避免 use-after-free
 *   用于 ECS 实体 ID、资源句柄、对象池引用等需要稳定标识的场景
 *
 * 设计哲学:
 *   打包存储 — 索引和代打包到单个 UInt64 中 (高 32 位=代, 低 32 位=索引)
 *   零开销 — 8 字节 POD，可值传递，无虚函数
 *   类型安全 — 模板 Tag 参数区分不同用途的句柄
 *
 * 技术特性:
 *   - THandle<Tag>: 类型化句柄 (不同 Tag 不可混用)
 *   - GetIndex: 获取索引部分
 *   - GetGeneration: 获取代部分
 *   - IsValid: 是否非空
 *   - kInvalid: 无效句柄常量
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

/// 通用句柄 — 索引 + 代
/// @tparam Tag 类型标签 (用于区分不同用途的句柄)
template<typename Tag>
struct THandle
{
    UInt64 Packed;

    /// 无效句柄常量
    static constexpr UInt64 kInvalidPacked = 0xFFFFFFFFFFFFFFFFULL;

    // ========================================================================
    // 构造
    // ========================================================================

    /// 默认构造 — 无效句柄
    constexpr THandle() : Packed(kInvalidPacked) {}

    /// 从索引和代构造
    constexpr THandle(UInt32 index, UInt32 generation)
        : Packed((static_cast<UInt64>(generation) << 32) |
                  static_cast<UInt64>(index))
    {
    }

    /// 从打包值构造
    constexpr explicit THandle(UInt64 packed) : Packed(packed) {}

    // ========================================================================
    // 访问
    // ========================================================================

    /// 获取索引 (低 32 位)
    LIMX_NODISCARD constexpr UInt32 GetIndex() const
    {
        return static_cast<UInt32>(Packed & 0xFFFFFFFFULL);
    }

    /// 获取代 (高 32 位)
    LIMX_NODISCARD constexpr UInt32 GetGeneration() const
    {
        return static_cast<UInt32>(Packed >> 32);
    }

    /// 是否有效 (非无效值)
    LIMX_NODISCARD constexpr bool IsValid() const
    {
        return Packed != kInvalidPacked;
    }

    /// 显式布尔转换
    LIMX_NODISCARD constexpr explicit operator bool() const
    {
        return IsValid();
    }

    /// 使句柄失效
    void Invalidate()
    {
        Packed = kInvalidPacked;
    }

    // ========================================================================
    // 比较
    // ========================================================================

    LIMX_NODISCARD constexpr bool operator==(const THandle& other) const
    {
        return Packed == other.Packed;
    }

    LIMX_NODISCARD constexpr bool operator!=(const THandle& other) const
    {
        return Packed != other.Packed;
    }

    LIMX_NODISCARD constexpr bool operator<(const THandle& other) const
    {
        return Packed < other.Packed;
    }

    // ========================================================================
    // 哈希
    // ========================================================================

    /// 获取哈希值 (直接使用打包值)
    LIMX_NODISCARD constexpr UInt64 GetHash() const
    {
        return Packed;
    }

    // ========================================================================
    // 工厂
    // ========================================================================

    /// 创建无效句柄
    LIMX_NODISCARD static constexpr THandle Invalid()
    {
        return THandle();
    }
};

} // namespace Limx
