/*******************************************************************************
 * 文件: FCommandLine.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   命令行参数解析器 — 解析引擎启动参数
 *   支持标志 (-flag)、键值对 (-key=value)、位置参数
 *   用于引擎启动配置、调试开关、渲染选项覆盖等场景
 *
 * 设计哲学:
 *   全局单例 — 进程启动时初始化一次，全局只读访问
 *   惰性解析 — 原始字符串存储，首次查询时按需解析
 *   不可变 — 初始化后参数不可修改
 *
 * 技术特性:
 *   - Initialize(argc, argv): 从 main 参数初始化
 *   - Initialize(cmdLine): 从完整命令行字符串初始化
 *   - HasFlag("-flag"): 检查标志是否存在
 *   - GetValue("-key"): 获取键值对的值
 *   - GetArg(index): 获取位置参数
 *   - GetOriginal(): 获取原始命令行字符串
 *
 * 依赖关系:
 *   内部: Core/Containers/FString.h, Core/Containers/TArray.h,
 *          Core/Templates/TPair.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Containers/FString.h"
#include "Core/Containers/TArray.h"
#include "Core/Templates/TPair.h"

namespace Limx
{

/// 命令行参数解析器
class FCommandLine
{
public:
    // ========================================================================
    // 初始化
    // ========================================================================

    /// 从 main(argc, argv) 初始化
    static void Initialize(Int32 argc, const AnsiChar* const* argv)
    {
        Instance().m_Args.Clear();
        Instance().m_Flags.Clear();
        Instance().m_Pairs.Clear();
        Instance().m_Original = FString();

        // 拼接原始命令行
        FString original;
        for (Int32 index = 0; index < argc; ++index)
        {
            if (index > 0)
            {
                original = original + FString(" ");
            }
            original = original + FString(argv[index]);
        }
        Instance().m_Original = original;

        // 解析参数 (跳过 argv[0] — 程序路径)
        for (Int32 index = 1; index < argc; ++index)
        {
            ParseToken(argv[index]);
        }
    }

    /// 从完整命令行字符串初始化
    static void Initialize(const AnsiChar* cmdLine)
    {
        Instance().m_Args.Clear();
        Instance().m_Flags.Clear();
        Instance().m_Pairs.Clear();
        Instance().m_Original = FString(cmdLine ? cmdLine : "");

        if (!cmdLine)
        {
            return;
        }

        // 简单分词 — 按空格拆分 (不处理引号)
        const AnsiChar* current = cmdLine;
        while (*current)
        {
            // 跳过空白
            while (*current == ' ' || *current == '\t')
            {
                ++current;
            }
            if (*current == '\0')
            {
                break;
            }

            // 读取 token
            const AnsiChar* tokenStart = current;
            while (*current && *current != ' ' && *current != '\t')
            {
                ++current;
            }

            SizeType tokenLength = static_cast<SizeType>(
                current - tokenStart);
            if (tokenLength > 0)
            {
                FString token(tokenStart,
                              static_cast<SizeType>(tokenLength));
                ParseToken(token.GetCStr());
            }
        }
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 检查标志是否存在 (如 "-verbose", "-nolog")
    LIMX_NODISCARD static bool HasFlag(const AnsiChar* flag)
    {
        FString flagStr(flag);
        for (SizeType index = 0;
             index < Instance().m_Flags.GetSize(); ++index)
        {
            if (Instance().m_Flags[index] == flagStr)
            {
                return true;
            }
        }
        return false;
    }

    /// 获取键值对的值 (如 "-key=value" 返回 "value")
    /// 未找到返回空字符串
    LIMX_NODISCARD static FString GetValue(const AnsiChar* key)
    {
        FString keyStr(key);
        for (SizeType index = 0;
             index < Instance().m_Pairs.GetSize(); ++index)
        {
            if (Instance().m_Pairs[index].First == keyStr)
            {
                return Instance().m_Pairs[index].Second;
            }
        }
        return FString();
    }

    /// 获取键值对的值 (带默认值)
    LIMX_NODISCARD static FString GetValue(const AnsiChar* key,
                                             const AnsiChar* defaultValue)
    {
        FString result = GetValue(key);
        if (result.IsEmpty())
        {
            return FString(defaultValue);
        }
        return result;
    }

    /// 获取位置参数 (非标志/非键值对的普通参数)
    LIMX_NODISCARD static FString GetArg(SizeType index)
    {
        if (index < Instance().m_Args.GetSize())
        {
            return Instance().m_Args[index];
        }
        return FString();
    }

    /// 位置参数数量
    LIMX_NODISCARD static SizeType GetArgCount()
    {
        return Instance().m_Args.GetSize();
    }

    /// 获取原始命令行字符串
    LIMX_NODISCARD static const FString& GetOriginal()
    {
        return Instance().m_Original;
    }

private:
    FCommandLine() = default;

    static FCommandLine& Instance()
    {
        static FCommandLine s_Instance;
        return s_Instance;
    }

    /// 解析单个 token
    static void ParseToken(const AnsiChar* token)
    {
        if (!token || !*token)
        {
            return;
        }

        // 以 '-' 开头 — 标志或键值对
        if (token[0] == '-')
        {
            const AnsiChar* keyStart = token + 1;
            // 跳过第二个 '-' (支持 --key=value)
            if (*keyStart == '-')
            {
                ++keyStart;
            }

            // 查找 '='
            const AnsiChar* equalSign = keyStart;
            while (*equalSign && *equalSign != '=')
            {
                ++equalSign;
            }

            if (*equalSign == '=')
            {
                // 键值对
                FString key(keyStart, static_cast<SizeType>(
                    equalSign - keyStart));
                FString value(equalSign + 1);
                Instance().m_Pairs.Add(
                    MakePair(MoveTemp(key), MoveTemp(value)));
            }
            else
            {
                // 纯标志
                Instance().m_Flags.Add(FString(keyStart));
            }
        }
        else
        {
            // 位置参数
            Instance().m_Args.Add(FString(token));
        }
    }

    // ========================================================================
    // 成员数据
    // ========================================================================

    FString                         m_Original;  ///< 原始命令行
    TArray<FString>                 m_Args;      ///< 位置参数
    TArray<FString>                 m_Flags;     ///< 标志列表
    TArray<TPair<FString, FString>> m_Pairs;     ///< 键值对列表
};

} // namespace Limx
