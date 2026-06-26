/*******************************************************************************
 * 文件: TBitField.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   位域 — 编译时位字段的类型安全访问
 *   将整数类型的指定位范围抽象为独立字段，提供读写接口
 *   用于 GPU 寄存器映射、网络协议位打包、紧凑状态标志等场景
 *
 * 设计哲学:
 *   编译时定义 — 位偏移和位宽通过模板参数编译时确定
 *   零开销 — 所有操作内联，编译后与手写位操作等价
 *   类型安全 — 模板参数约束值范围，防止越界写入
 *
 * 技术特性:
 *   - TBitField<StorageType, Offset, Width>: 位字段访问器
 *   - Get: 从存储值中提取字段
 *   - Set: 将字段值写入存储值
 *   - GetMask: 获取字段掩码
 *   - GetMaxValue: 获取字段最大值
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"

namespace Limx
{

/// 位字段访问器
/// @tparam StorageType 底层存储类型 (UInt8/UInt16/UInt32/UInt64)
/// @tparam Offset 位偏移 (从 LSB 起)
/// @tparam Width 位宽度
template<typename StorageType, SizeType Offset, SizeType Width>
struct TBitField
{
    static_assert(Width > 0,
        "Bit field width must be > 0");
    static_assert(Offset + Width <= sizeof(StorageType) * 8,
        "Bit field exceeds storage type size");

    /// 字段掩码 (未移位)
    static constexpr StorageType kFieldMask =
        (Width == sizeof(StorageType) * 8)
            ? static_cast<StorageType>(~StorageType(0))
            : static_cast<StorageType>(
                  (StorageType(1) << Width) - 1);

    /// 移位后掩码
    static constexpr StorageType kShiftedMask =
        static_cast<StorageType>(kFieldMask << Offset);

    /// 字段最大值
    static constexpr StorageType kMaxValue = kFieldMask;

    /// 从存储值中提取字段值
    LIMX_NODISCARD static constexpr StorageType Get(
        StorageType storage)
    {
        return static_cast<StorageType>(
            (storage >> Offset) & kFieldMask);
    }

    /// 将字段值写入存储值
    static constexpr void Set(
        StorageType& storage, StorageType value)
    {
        storage = static_cast<StorageType>(
            (storage & ~kShiftedMask) |
            ((value & kFieldMask) << Offset));
    }

    /// 获取字段掩码
    LIMX_NODISCARD static constexpr StorageType GetMask()
    {
        return kShiftedMask;
    }

    /// 获取位偏移
    LIMX_NODISCARD static constexpr SizeType GetOffset()
    {
        return Offset;
    }

    /// 获取位宽度
    LIMX_NODISCARD static constexpr SizeType GetWidth()
    {
        return Width;
    }

    /// 获取最大值
    LIMX_NODISCARD static constexpr StorageType GetMaxValue()
    {
        return kMaxValue;
    }

    /// 测试字段是否全零
    LIMX_NODISCARD static constexpr bool IsZero(
        StorageType storage)
    {
        return Get(storage) == 0;
    }

    /// 测试字段是否为最大值
    LIMX_NODISCARD static constexpr bool IsFull(
        StorageType storage)
    {
        return Get(storage) == kMaxValue;
    }
};

/// 便捷宏 — 定义位字段类型别名
/// 用法: LIMX_BITFIELD(FMyFlags, UInt32, IsVisible, 0, 1)
///   => using FMyFlags_IsVisible = TBitField<UInt32, 0, 1>;
#define LIMX_BITFIELD(TypeName, Storage, Field, Off, Wid) \
    using TypeName##_##Field = TBitField<Storage, Off, Wid>

} // namespace Limx
