/*******************************************************************************
 * 文件: FVersion.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   版本号 — Major.Minor.Patch 语义化版本类型
 *   提供版本比较、字符串格式化、兼容性检查等操作
 *   用于引擎版本标识、资产版本控制、API 兼容性检查等场景
 *
 * 设计哲学:
 *   语义化版本 (SemVer) — Major.Minor.Patch
 *   比较运算 — 全套比较运算符
 *   轻量值类型 — 3 个 UInt16，6 字节
 *
 * 技术特性:
 *   - FVersion: 语义化版本号
 *   - IsCompatibleWith: 向后兼容检查
 *   - ToString: 格式化为字符串
 *   - operator==/</>: 全套比较
 *   - FromPacked: 从打包 UInt64 还原
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

/// 语义化版本号 (Major.Minor.Patch)
struct FVersion
{
    UInt16 Major;  ///< 主版本号 (不兼容变更)
    UInt16 Minor;  ///< 次版本号 (向后兼容新功能)
    UInt16 Patch;  ///< 修订号 (向后兼容问题修复)

    // ========================================================================
    // 构造
    // ========================================================================

    FVersion()
        : Major(0), Minor(0), Patch(0)
    {
    }

    FVersion(UInt16 major, UInt16 minor, UInt16 patch)
        : Major(major), Minor(minor), Patch(patch)
    {
    }

    /// 零版本
    LIMX_NODISCARD static FVersion Zero()
    {
        return FVersion(0, 0, 0);
    }

    // ========================================================================
    // 序列化 / 反序列化
    // ========================================================================

    /// 打包为 UInt64: [Major(16) | Minor(16) | Patch(16) | 0(16)]
    LIMX_NODISCARD UInt64 ToPacked() const
    {
        return (static_cast<UInt64>(Major) << 48) |
               (static_cast<UInt64>(Minor) << 32) |
               (static_cast<UInt64>(Patch) << 16);
    }

    /// 从打包 UInt64 还原
    LIMX_NODISCARD static FVersion FromPacked(
        UInt64 packed)
    {
        return FVersion(
            static_cast<UInt16>((packed >> 48) & 0xFFFF),
            static_cast<UInt16>((packed >> 32) & 0xFFFF),
            static_cast<UInt16>((packed >> 16) & 0xFFFF));
    }

    // ========================================================================
    // 兼容性检查
    // ========================================================================

    /// 是否与目标版本向后兼容
    /// (相同 Major，Minor/Patch 大于等于目标)
    LIMX_NODISCARD bool IsCompatibleWith(
        const FVersion& required) const
    {
        if (Major != required.Major) return false;
        if (Minor > required.Minor) return true;
        if (Minor < required.Minor) return false;
        return Patch >= required.Patch;
    }

    /// 是否为零版本
    LIMX_NODISCARD bool IsZero() const
    {
        return Major == 0 && Minor == 0 && Patch == 0;
    }

    // ========================================================================
    // 递增
    // ========================================================================

    /// 递增 Patch 版本
    LIMX_NODISCARD FVersion IncrementPatch() const
    {
        return FVersion(Major, Minor,
            static_cast<UInt16>(Patch + 1));
    }

    /// 递增 Minor 版本 (Patch 重置)
    LIMX_NODISCARD FVersion IncrementMinor() const
    {
        return FVersion(Major,
            static_cast<UInt16>(Minor + 1), 0);
    }

    /// 递增 Major 版本 (Minor/Patch 重置)
    LIMX_NODISCARD FVersion IncrementMajor() const
    {
        return FVersion(
            static_cast<UInt16>(Major + 1), 0, 0);
    }

    // ========================================================================
    // 格式化
    // ========================================================================

    /// 将版本号写入字符缓冲区，返回写入字节数
    /// 格式: "Major.Minor.Patch"
    SizeType Format(AnsiChar* buffer,
                    SizeType bufferSize) const
    {
        if (buffer == nullptr || bufferSize == 0)
            return 0;

        // 手动整数转字符串
        SizeType pos = 0;
        pos += WriteUInt16(buffer + pos,
            bufferSize - pos, Major);
        if (pos < bufferSize) buffer[pos++] = '.';
        pos += WriteUInt16(buffer + pos,
            bufferSize - pos, Minor);
        if (pos < bufferSize) buffer[pos++] = '.';
        pos += WriteUInt16(buffer + pos,
            bufferSize - pos, Patch);
        if (pos < bufferSize) buffer[pos] = '\0';

        return pos;
    }

    // ========================================================================
    // 比较运算
    // ========================================================================

    LIMX_NODISCARD bool operator==(
        const FVersion& other) const
    {
        return Major == other.Major &&
               Minor == other.Minor &&
               Patch == other.Patch;
    }

    LIMX_NODISCARD bool operator!=(
        const FVersion& other) const
    {
        return !(*this == other);
    }

    LIMX_NODISCARD bool operator<(
        const FVersion& other) const
    {
        if (Major != other.Major) return Major < other.Major;
        if (Minor != other.Minor) return Minor < other.Minor;
        return Patch < other.Patch;
    }

    LIMX_NODISCARD bool operator<=(
        const FVersion& other) const
    {
        return !(other < *this);
    }

    LIMX_NODISCARD bool operator>(
        const FVersion& other) const
    {
        return other < *this;
    }

    LIMX_NODISCARD bool operator>=(
        const FVersion& other) const
    {
        return !(*this < other);
    }

private:
    static SizeType WriteUInt16(AnsiChar* buffer,
                                SizeType bufferSize,
                                UInt16 value)
    {
        if (bufferSize == 0) return 0;

        if (value == 0)
        {
            buffer[0] = '0';
            return 1;
        }

        AnsiChar temp[8];
        Int32 tempLen = 0;
        UInt16 v = value;
        while (v > 0)
        {
            temp[tempLen++] =
                static_cast<AnsiChar>('0' + (v % 10));
            v /= 10;
        }

        SizeType written = 0;
        for (Int32 digitIdx = tempLen - 1;
             digitIdx >= 0 && written < bufferSize;
             --digitIdx)
        {
            buffer[written++] = temp[digitIdx];
        }

        return written;
    }
};

} // namespace Limx
