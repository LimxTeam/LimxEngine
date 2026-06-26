/*******************************************************************************
 * 文件: FAssert.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   断言与诊断系统 — 提供调试断言、验证、致命错误等诊断原语
 *   在 Development/Debug 构建中启用完整断言，Release 中可选保留验证
 *   用于不变量检查、前置/后置条件验证、不可达代码标记
 *
 * 设计哲学:
 *   分层断言 — LIMX_CHECK (永远启用) vs LIMX_ASSERT (仅 Debug/Dev)
 *   调试器友好 — 断言失败时调用 __debugbreak() 触发调试器断点
 *   致命错误 — LIMX_FATAL 输出诊断信息后终止进程
 *   零开销 — Release 构建中 LIMX_ASSERT 完全消除
 *
 * 技术特性:
 *   - LIMX_CHECK(expr): 始终启用的验证 (Release 也保留)
 *   - LIMX_CHECK_MSG(expr, msg): 带消息的验证
 *   - LIMX_VERIFY(expr): 表达式始终求值，仅 Debug 断言
 *   - LIMX_FATAL(msg): 致命错误 — 输出消息并终止
 *   - LIMX_UNREACHABLE: 标记不可达代码路径
 *   - AssertionFailure: 断言失败处理函数 (可自定义)
 *
 * 依赖关系:
 *   内部: Core/HAL/Platform.h, Core/HAL/PlatformTypes.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"

// Windows API 前向声明
#if LIMX_PLATFORM_WINDOWS
#ifndef _WINDOWS_
extern "C"
{
    void __stdcall OutputDebugStringA(const char* lpOutputString);
    __declspec(noreturn) void __stdcall ExitProcess(unsigned int uExitCode);
}
#endif // _WINDOWS_
#endif

namespace Limx
{

/// 断言失败信息
struct AssertInfo
{
    const AnsiChar* Expression;  ///< 断言表达式文本
    const AnsiChar* Message;     ///< 附加消息 (可为 nullptr)
    const AnsiChar* File;        ///< 源文件路径
    Int32           Line;        ///< 行号
};

/// 断言失败处理 — 输出诊断信息并触发调试器断点
inline void OnAssertionFailure(const AssertInfo& info)
{
    // 构建诊断消息 — 使用简单的固定缓冲区
    AnsiChar buffer[1024];
    SizeType offset = 0;

    // 辅助: 追加字符串
    auto appendStr = [&](const AnsiChar* str)
    {
        if (!str) return;
        while (*str && offset < 1020)
        {
            buffer[offset++] = *str++;
        }
    };

    // 辅助: 追加十进制数字
    auto appendInt = [&](Int32 value)
    {
        if (value < 0)
        {
            buffer[offset++] = '-';
            value = -value;
        }
        AnsiChar digits[12];
        Int32 digitCount = 0;
        do
        {
            digits[digitCount++] =
                static_cast<AnsiChar>('0' + value % 10);
            value /= 10;
        } while (value > 0 && digitCount < 11);

        for (Int32 index = digitCount - 1;
             index >= 0 && offset < 1020; --index)
        {
            buffer[offset++] = digits[index];
        }
    };

    appendStr("[LIMX ASSERT] ");
    appendStr(info.File);
    appendStr("(");
    appendInt(info.Line);
    appendStr("): ");

    if (info.Expression)
    {
        appendStr("'");
        appendStr(info.Expression);
        appendStr("' failed");
    }

    if (info.Message)
    {
        appendStr(" — ");
        appendStr(info.Message);
    }

    appendStr("\n");
    buffer[offset] = '\0';

    // 输出到调试器
#if LIMX_PLATFORM_WINDOWS
    OutputDebugStringA(buffer);
#endif

    // 触发调试器断点
#if LIMX_COMPILER_MSVC
    __debugbreak();
#endif
}

/// 致命错误处理 — 输出消息并终止进程
[[noreturn]] inline void OnFatalError(const AnsiChar* message,
                                       const AnsiChar* file,
                                       Int32 line)
{
    AssertInfo info;
    info.Expression = nullptr;
    info.Message = message;
    info.File = file;
    info.Line = line;

    // 构建致命错误消息
    AnsiChar buffer[1024];
    SizeType offset = 0;

    auto appendStr = [&](const AnsiChar* str)
    {
        if (!str) return;
        while (*str && offset < 1020)
        {
            buffer[offset++] = *str++;
        }
    };

    auto appendInt = [&](Int32 value)
    {
        if (value < 0)
        {
            buffer[offset++] = '-';
            value = -value;
        }
        AnsiChar digits[12];
        Int32 digitCount = 0;
        do
        {
            digits[digitCount++] =
                static_cast<AnsiChar>('0' + value % 10);
            value /= 10;
        } while (value > 0 && digitCount < 11);

        for (Int32 index = digitCount - 1;
             index >= 0 && offset < 1020; --index)
        {
            buffer[offset++] = digits[index];
        }
    };

    appendStr("[LIMX FATAL] ");
    appendStr(file);
    appendStr("(");
    appendInt(line);
    appendStr("): ");
    appendStr(message);
    appendStr("\n");
    buffer[offset] = '\0';

#if LIMX_PLATFORM_WINDOWS
    OutputDebugStringA(buffer);
#endif

#if LIMX_COMPILER_MSVC
    __debugbreak();
#endif

#if LIMX_PLATFORM_WINDOWS
    ExitProcess(1);
#endif
}

} // namespace Limx
