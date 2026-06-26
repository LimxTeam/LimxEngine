/*******************************************************************************
 * 文件: FPathUtils.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   路径操作工具 — 文件路径字符串的解析与操作
 *   提供扩展名提取、目录分离、路径合并、规范化等功能
 *   用于资产加载、文件系统操作、配置路径处理等场景
 *
 * 设计哲学:
 *   纯字符串操作 — 不访问文件系统，仅操作路径字符串
 *   FString 原生 — 输入输出均为 FString
 *   平台无关 — 内部统一使用 '/' 分隔符
 *
 * 技术特性:
 *   - GetExtension: 获取扩展名
 *   - GetFileName: 获取文件名 (含扩展名)
 *   - GetFileNameWithoutExtension: 获取文件名 (不含扩展名)
 *   - GetDirectory: 获取目录路径
 *   - Combine: 合并路径
 *   - Normalize: 规范化路径分隔符
 *   - ChangeExtension: 更换扩展名
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Containers/FString.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Containers/FString.h"

namespace Limx
{

/// 路径操作工具
struct FPathUtils
{
    /// 路径分隔符
    static constexpr AnsiChar kSeparator = '/';
    static constexpr AnsiChar kWindowsSeparator = '\\';
    static constexpr AnsiChar kExtensionDot = '.';

    // ========================================================================
    // 提取
    // ========================================================================

    /// 获取扩展名 (含点，如 ".png")
    /// 无扩展名返回空字符串
    LIMX_NODISCARD static FString GetExtension(const FString& path)
    {
        SizeType length = path.GetLength();
        for (SizeType index = length; index > 0; --index)
        {
            AnsiChar ch = path[index - 1];
            if (ch == kExtensionDot)
            {
                // 确保点不在分隔符之后的首位
                if (index > 1 &&
                    path[index - 2] != kSeparator &&
                    path[index - 2] != kWindowsSeparator)
                {
                    // 从点位置到末尾
                    FString result;
                    for (SizeType copyIndex = index - 1;
                         copyIndex < length; ++copyIndex)
                    {
                        AnsiChar buf[2] = { path[copyIndex], '\0' };
                        result += buf;
                    }
                    return result;
                }
                return FString();
            }
            if (ch == kSeparator || ch == kWindowsSeparator)
            {
                break;
            }
        }
        return FString();
    }

    /// 获取文件名 (含扩展名)
    LIMX_NODISCARD static FString GetFileName(const FString& path)
    {
        SizeType length = path.GetLength();
        SizeType lastSep = FindLastSeparator(path);

        if (lastSep == length) return path;

        FString result;
        for (SizeType index = lastSep + 1;
             index < length; ++index)
        {
            AnsiChar buf[2] = { path[index], '\0' };
            result += buf;
        }
        return result;
    }

    /// 获取文件名 (不含扩展名)
    LIMX_NODISCARD static FString GetFileNameWithoutExtension(
        const FString& path)
    {
        FString fileName = GetFileName(path);
        SizeType length = fileName.GetLength();

        // 从末尾向前找点
        for (SizeType index = length; index > 0; --index)
        {
            if (fileName[index - 1] == kExtensionDot)
            {
                FString result;
                for (SizeType copyIndex = 0;
                     copyIndex < index - 1; ++copyIndex)
                {
                    AnsiChar buf[2] = { fileName[copyIndex], '\0' };
                    result += buf;
                }
                return result;
            }
        }
        return fileName;
    }

    /// 获取目录路径 (不含末尾分隔符)
    LIMX_NODISCARD static FString GetDirectory(const FString& path)
    {
        SizeType lastSep = FindLastSeparator(path);
        SizeType length = path.GetLength();

        if (lastSep == length) return FString();

        FString result;
        for (SizeType index = 0; index < lastSep; ++index)
        {
            AnsiChar buf[2] = { path[index], '\0' };
            result += buf;
        }
        return result;
    }

    // ========================================================================
    // 操作
    // ========================================================================

    /// 合并两个路径
    LIMX_NODISCARD static FString Combine(const FString& basePath,
                                            const FString& relativePath)
    {
        if (basePath.GetLength() == 0) return relativePath;
        if (relativePath.GetLength() == 0) return basePath;

        FString result = basePath;

        // 确保 base 以分隔符结尾
        AnsiChar lastChar = basePath[basePath.GetLength() - 1];
        if (lastChar != kSeparator && lastChar != kWindowsSeparator)
        {
            AnsiChar sepBuf[2] = { kSeparator, '\0' };
            result += sepBuf;
        }

        // 移除 relative 前的分隔符
        SizeType startIndex = 0;
        if (relativePath.GetLength() > 0 &&
            (relativePath[0] == kSeparator ||
             relativePath[0] == kWindowsSeparator))
        {
            startIndex = 1;
        }

        for (SizeType index = startIndex;
             index < relativePath.GetLength(); ++index)
        {
            AnsiChar buf[2] = { relativePath[index], '\0' };
            result += buf;
        }

        return result;
    }

    /// 规范化路径 — 统一为 '/' 分隔符
    LIMX_NODISCARD static FString Normalize(const FString& path)
    {
        FString result = path;
        for (SizeType index = 0;
             index < result.GetLength(); ++index)
        {
            if (result[index] == kWindowsSeparator)
            {
                // 通过逐字符构建新字符串
                // 注: FString 可能不提供单字符写入，使用替代方案
            }
        }

        // 使用直接构建方式
        FString normalized;
        for (SizeType index = 0;
             index < path.GetLength(); ++index)
        {
            AnsiChar ch = path[index];
            if (ch == kWindowsSeparator) ch = kSeparator;
            AnsiChar buf[2] = { ch, '\0' };
            normalized += buf;
        }
        return normalized;
    }

    /// 更换扩展名
    LIMX_NODISCARD static FString ChangeExtension(
        const FString& path, const FString& newExtension)
    {
        FString dir = GetDirectory(path);
        FString baseName = GetFileNameWithoutExtension(path);

        FString result;
        if (dir.GetLength() > 0)
        {
            result = dir;
            AnsiChar sepBuf[2] = { kSeparator, '\0' };
            result += sepBuf;
        }
        result += baseName;
        result += newExtension;
        return result;
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 是否有扩展名
    LIMX_NODISCARD static bool HasExtension(const FString& path)
    {
        return GetExtension(path).GetLength() > 0;
    }

    /// 是否为绝对路径
    LIMX_NODISCARD static bool IsAbsolute(const FString& path)
    {
        if (path.GetLength() == 0) return false;

        // Unix: 以 '/' 开头
        if (path[0] == kSeparator) return true;

        // Windows: 以 X:/ 或 X:\ 开头
        if (path.GetLength() >= 3 &&
            ((path[0] >= 'A' && path[0] <= 'Z') ||
             (path[0] >= 'a' && path[0] <= 'z')) &&
            path[1] == ':' &&
            (path[2] == kSeparator ||
             path[2] == kWindowsSeparator))
        {
            return true;
        }

        return false;
    }

    /// 是否为相对路径
    LIMX_NODISCARD static bool IsRelative(const FString& path)
    {
        return !IsAbsolute(path);
    }

private:
    /// 查找最后一个分隔符的位置
    /// 未找到返回 path.GetLength()
    LIMX_NODISCARD static SizeType FindLastSeparator(
        const FString& path)
    {
        SizeType length = path.GetLength();
        for (SizeType index = length; index > 0; --index)
        {
            AnsiChar ch = path[index - 1];
            if (ch == kSeparator || ch == kWindowsSeparator)
            {
                return index - 1;
            }
        }
        return length; // 未找到
    }
};

} // namespace Limx
