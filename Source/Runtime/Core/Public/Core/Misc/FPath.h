/*******************************************************************************
 * 文件: FPath.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   文件路径工具 — 零 STL 依赖的路径操作静态工具类
 *   提供路径拼接、拆分、扩展名提取、规范化等操作
 *   统一使用正斜杠 '/' 作为引擎内部路径分隔符
 *
 * 设计哲学:
 *   静态工具类 — 所有方法为 static，操作 FString 参数
 *   正斜杠统一 — 引擎内部路径一律使用 '/'，仅在 OS API 调用前转换
 *   不可变操作 — 所有方法返回新 FString，不修改输入
 *
 * 技术特性:
 *   - Combine: 路径拼接 (自动处理分隔符)
 *   - GetExtension: 获取扩展名 (".png")
 *   - GetFilename: 获取文件名 ("mesh.fbx")
 *   - GetFilenameWithoutExtension: 去扩展名文件名 ("mesh")
 *   - GetDirectory: 获取目录部分 ("/Assets/Models")
 *   - ChangeExtension: 更换扩展名
 *   - Normalize: 规范化 (统一分隔符, 消除 .. 和 .)
 *   - IsAbsolute: 是否为绝对路径
 *
 * 依赖关系:
 *   内部: Core/Containers/FString.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Containers/FString.h"

namespace Limx
{

/// 文件路径工具 — 静态方法集合
struct FPath
{
    /// 引擎内部路径分隔符
    static constexpr AnsiChar kSeparator = '/';

    // ========================================================================
    // 路径拼接
    // ========================================================================

    /// 拼接两个路径段 — 自动处理分隔符
    LIMX_NODISCARD static FString Combine(const FString& base,
                                           const FString& relative)
    {
        if (base.IsEmpty())
        {
            return relative;
        }
        if (relative.IsEmpty())
        {
            return base;
        }

        // 检查 base 是否以分隔符结尾
        bool baseEndsWithSep = IsSeparator(base[base.GetLength() - 1]);
        // 检查 relative 是否以分隔符开头
        bool relStartsWithSep = IsSeparator(relative[0]);

        FString result = base;
        if (baseEndsWithSep && relStartsWithSep)
        {
            // 两边都有分隔符 — 跳过 relative 的开头分隔符
            result.Append(relative.GetCStr() + 1, relative.GetLength() - 1);
        }
        else if (!baseEndsWithSep && !relStartsWithSep)
        {
            // 两边都没有分隔符 — 插入一个
            result.Append("/", 1);
            result.Append(relative.GetCStr(), relative.GetLength());
        }
        else
        {
            // 恰好一边有分隔符
            result.Append(relative.GetCStr(), relative.GetLength());
        }

        return result;
    }

    /// 多段路径拼接
    LIMX_NODISCARD static FString Combine(const FString& a,
                                           const FString& b,
                                           const FString& c)
    {
        return Combine(Combine(a, b), c);
    }

    // ========================================================================
    // 路径拆分
    // ========================================================================

    /// 获取文件扩展名 (含点号, 如 ".png")
    /// 无扩展名返回空字符串
    LIMX_NODISCARD static FString GetExtension(const FString& path)
    {
        SizeType dotPos = FindLastDot(path);
        if (dotPos == FString::kNPos)
        {
            return FString();
        }
        // 确保点号在最后一个分隔符之后
        SizeType sepPos = FindLastSeparator(path);
        if (sepPos != FString::kNPos && dotPos < sepPos)
        {
            return FString();
        }
        return path.Substring(dotPos);
    }

    /// 获取文件名 (含扩展名, 如 "mesh.fbx")
    LIMX_NODISCARD static FString GetFilename(const FString& path)
    {
        SizeType sepPos = FindLastSeparator(path);
        if (sepPos == FString::kNPos)
        {
            return path;
        }
        return path.Substring(sepPos + 1);
    }

    /// 获取不含扩展名的文件名 (如 "mesh")
    LIMX_NODISCARD static FString GetFilenameWithoutExtension(
        const FString& path)
    {
        FString filename = GetFilename(path);
        SizeType dotPos = FindLastDot(filename);
        if (dotPos == FString::kNPos)
        {
            return filename;
        }
        return filename.Substring(0, dotPos);
    }

    /// 获取目录部分 (如 "/Assets/Models")
    /// 无目录部分返回空字符串
    LIMX_NODISCARD static FString GetDirectory(const FString& path)
    {
        SizeType sepPos = FindLastSeparator(path);
        if (sepPos == FString::kNPos)
        {
            return FString();
        }
        return path.Substring(0, sepPos);
    }

    /// 更换扩展名
    /// @param newExtension 新扩展名 (含点号, 如 ".dds")
    LIMX_NODISCARD static FString ChangeExtension(const FString& path,
                                                    const FString& newExtension)
    {
        SizeType dotPos = FindLastDot(path);
        SizeType sepPos = FindLastSeparator(path);
        if (dotPos == FString::kNPos ||
            (sepPos != FString::kNPos && dotPos < sepPos))
        {
            // 无扩展名 — 直接追加
            FString result = path;
            result.Append(newExtension.GetCStr(), newExtension.GetLength());
            return result;
        }
        FString result = path.Substring(0, dotPos);
        result.Append(newExtension.GetCStr(), newExtension.GetLength());
        return result;
    }

    // ========================================================================
    // 路径规范化
    // ========================================================================

    /// 规范化路径 — 统一分隔符为 '/'，消除冗余
    LIMX_NODISCARD static FString Normalize(const FString& path)
    {
        if (path.IsEmpty())
        {
            return path;
        }

        // 先统一反斜杠为正斜杠
        FString normalized = path;
        ReplaceSeparators(normalized);

        // 消除连续分隔符 (保留开头的 // 用于 UNC 路径)
        FString result;
        bool prevWasSep = false;
        for (SizeType index = 0; index < normalized.GetLength(); ++index)
        {
            AnsiChar ch = normalized[index];
            if (ch == '/')
            {
                if (!prevWasSep || index <= 1)
                {
                    result.Append(&ch, 1);
                }
                prevWasSep = true;
            }
            else
            {
                result.Append(&ch, 1);
                prevWasSep = false;
            }
        }

        // 去除尾部分隔符 (除非路径仅为 "/" 或 "X:/")
        SizeType len = result.GetLength();
        if (len > 1 && result[len - 1] == '/')
        {
            // 保留 "X:/" 形式
            if (len == 3 && result[1] == ':')
            {
                return result;
            }
            return result.Substring(0, len - 1);
        }

        return result;
    }

    // ========================================================================
    // 路径查询
    // ========================================================================

    /// 是否为绝对路径
    /// Windows: "X:/" 或 "//" (UNC) 开头
    LIMX_NODISCARD static bool IsAbsolute(const FString& path)
    {
        if (path.GetLength() >= 2)
        {
            // 驱动器号: "C:/" 或 "C:\"
            if (path[1] == ':' && IsAlpha(path[0]))
            {
                return true;
            }
            // UNC 路径: "//" 或 "\\"
            if (IsSeparator(path[0]) && IsSeparator(path[1]))
            {
                return true;
            }
        }
        // Unix 绝对路径
        if (path.GetLength() >= 1 && path[0] == '/')
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

    /// 路径是否含扩展名
    LIMX_NODISCARD static bool HasExtension(const FString& path)
    {
        return !GetExtension(path).IsEmpty();
    }

private:
    // ========================================================================
    // 辅助函数
    // ========================================================================

    static bool IsSeparator(AnsiChar ch)
    {
        return ch == '/' || ch == '\\';
    }

    static bool IsAlpha(AnsiChar ch)
    {
        return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
    }

    /// 查找最后一个分隔符位置
    static SizeType FindLastSeparator(const FString& path)
    {
        SizeType result = FString::kNPos;
        for (SizeType index = 0; index < path.GetLength(); ++index)
        {
            if (IsSeparator(path[index]))
            {
                result = index;
            }
        }
        return result;
    }

    /// 查找最后一个点号位置
    static SizeType FindLastDot(const FString& path)
    {
        SizeType result = FString::kNPos;
        for (SizeType index = 0; index < path.GetLength(); ++index)
        {
            if (path[index] == '.')
            {
                result = index;
            }
        }
        return result;
    }

    /// 将路径中的反斜杠替换为正斜杠 (原地修改)
    static void ReplaceSeparators(FString& path)
    {
        for (SizeType index = 0; index < path.GetLength(); ++index)
        {
            if (path[index] == '\\')
            {
                path[index] = '/';
            }
        }
    }
};

} // namespace Limx
