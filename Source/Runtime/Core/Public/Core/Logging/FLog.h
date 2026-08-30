/*******************************************************************************
 * 文件: FLog.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   引擎日志系统 — 分类日志、严重级别、多输出目标
 *   提供结构化日志记录，支持按分类过滤和按级别筛选
 *   通过 Sink 接口支持输出到控制台、文件、编辑器等多个目标
 *
 * 设计哲学:
 *   分类日志 — 每条日志属于一个 LogCategory (如 LogCore, LogRender)
 *   分级过滤 — 每个分类独立设置最低输出级别
 *   Sink 模型 — 日志输出到可配置的 Sink 链 (控制台/文件/远程)
 *   零开销关闭 — Release 构建中 Verbose/VeryVerbose 编译时消除
 *   格式化集成 — 使用 FStringFormat 的 {} 占位符语法
 *
 * 技术特性:
 *   - LogVerbosity: Fatal, Error, Warning, Display, Log, Verbose, VeryVerbose
 *   - LogCategory: 命名分类 + 编译时/运行时最低级别
 *   - ILogSink: 日志输出接口 (Write 方法)
 *   - ConsoleLogSink: 默认控制台输出 (OutputDebugString + stdout)
 *   - FLog: 静态日志管理器 (注册 Sink, 分发日志)
 *   - LIMX_LOG 宏: 日志入口点，自动附加文件名/行号
 *
 * 依赖关系:
 *   内部: Core/Containers/FString.h, Core/Containers/FStringFormat.h,
 *          Core/Containers/TArray.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Containers/FString.h"
#include "Core/Containers/FStringFormat.h"
#include "Core/Containers/TArray.h"

// Windows 调试输出前向声明
#if LIMX_PLATFORM_WINDOWS
#ifndef _WINDOWS_
extern "C"
{
    void __stdcall OutputDebugStringA(const char* lpOutputString);

    // 文件日志 Sink 需要的 Win32 API
    void* __stdcall CreateFileA(
        const char* lpFileName,
        unsigned long dwDesiredAccess,
        unsigned long dwShareMode,
        void* lpSecurityAttributes,
        unsigned long dwCreationDisposition,
        unsigned long dwFlagsAndAttributes,
        void* hTemplateFile);
    int __stdcall WriteFile(
        void* hFile,
        const void* lpBuffer,
        unsigned long nNumberOfBytesToWrite,
        unsigned long* lpNumberOfBytesWritten,
        void* lpOverlapped);
    int __stdcall CloseHandle(void* hObject);
    int __stdcall FlushFileBuffers(void* hFile);
}
#endif // _WINDOWS_
#endif

// CRT 输出前向声明
extern "C"
{
    int puts(const char* str);
}

namespace Limx
{

// ============================================================================
// LogVerbosity — 日志严重级别
// ============================================================================

/// 日志严重级别 — 数值越小越严重
enum class LogVerbosity : UInt8
{
    Fatal       = 0,  ///< 致命错误 — 触发断言/崩溃
    Error       = 1,  ///< 可恢复错误
    Warning     = 2,  ///< 警告
    Display     = 3,  ///< 面向用户的重要信息
    Log         = 4,  ///< 一般信息
    Verbose     = 5,  ///< 详细调试信息
    VeryVerbose = 6,  ///< 极详细调试信息

    All         = 6,  ///< 等价于 VeryVerbose
    Count       = 7
};

/// 严重级别前缀字符串
LIMX_NODISCARD inline const AnsiChar* GetVerbosityPrefix(LogVerbosity verbosity)
{
    switch (verbosity)
    {
    case LogVerbosity::Fatal:       return "FATAL";
    case LogVerbosity::Error:       return "ERROR";
    case LogVerbosity::Warning:     return "WARN ";
    case LogVerbosity::Display:     return "DISP ";
    case LogVerbosity::Log:         return "LOG  ";
    case LogVerbosity::Verbose:     return "VERB ";
    case LogVerbosity::VeryVerbose: return "VVERB";
    default:                        return "?????";
    }
}

// ============================================================================
// LogCategory — 日志分类
// ============================================================================

/// 日志分类 — 命名标识 + 编译时/运行时最低级别
struct LogCategory
{
    const AnsiChar* Name;                  ///< 分类名称
    LogVerbosity    CompileTimeVerbosity;  ///< 编译时最低级别
    LogVerbosity    RuntimeVerbosity;      ///< 运行时最低级别 (可动态调整)

    constexpr LogCategory(const AnsiChar* inName,
                          LogVerbosity compileTime = LogVerbosity::All,
                          LogVerbosity runtime = LogVerbosity::Log)
        : Name(inName)
        , CompileTimeVerbosity(compileTime)
        , RuntimeVerbosity(runtime)
    {
    }

    /// 是否应该输出该级别的日志
    LIMX_NODISCARD FORCEINLINE bool ShouldLog(LogVerbosity verbosity) const
    {
        return static_cast<UInt8>(verbosity) <=
               static_cast<UInt8>(RuntimeVerbosity);
    }
};

// ============================================================================
// ILogSink — 日志输出接口
// ============================================================================

/// 日志输出目标接口
class ILogSink
{
public:
    virtual ~ILogSink() = default;

    /// 写入一条格式化后的日志
    /// @param category  日志分类
    /// @param verbosity 严重级别
    /// @param message   完整格式化消息
    virtual void Write(const LogCategory& category,
                       LogVerbosity verbosity,
                       const AnsiChar* message) = 0;

    /// 刷新缓冲 (可选)
    virtual void Flush() {}
};

// ============================================================================
// ConsoleLogSink — 控制台输出
// ============================================================================

/// 默认控制台日志输出
/// 格式: [LEVEL] CategoryName: Message
class ConsoleLogSink final : public ILogSink
{
public:
    void Write(const LogCategory& category,
               LogVerbosity verbosity,
               const AnsiChar* message) override
    {
        // 格式化: [LEVEL] Category: Message\n
        FString formatted = StringFormat("[{}] {}: {}",
            GetVerbosityPrefix(verbosity),
            category.Name,
            message);

#if LIMX_PLATFORM_WINDOWS
        // 输出到 Visual Studio 调试窗口
        FString withNewline = formatted;
        withNewline.Append("\n", 1);
        OutputDebugStringA(withNewline.GetCStr());
#endif
        // 输出到 stdout
        puts(formatted.GetCStr());
    }
};

// ============================================================================
// FileLogSink — 文件输出
// ============================================================================

/// 文件日志输出 — 写入指定路径的日志文件
/// 每条日志立即刷新到磁盘，确保崩溃前的日志不丢失
class FileLogSink final : public ILogSink
{
public:
    FileLogSink() = default;

    ~FileLogSink() override
    {
        Close();
    }

    LIMX_NON_COPYABLE(FileLogSink);

    /// 打开日志文件 (覆盖写入)
    /// @param filePath 日志文件路径 (ANSI)
    /// @return true 成功打开
    bool Open(const AnsiChar* filePath)
    {
#if LIMX_PLATFORM_WINDOWS
        m_Handle = CreateFileA(
            filePath,
            0x40000000UL,           // GENERIC_WRITE
            0x00000001UL,           // FILE_SHARE_READ
            nullptr,
            2,                      // CREATE_ALWAYS
            0x00000080UL,           // FILE_ATTRIBUTE_NORMAL
            nullptr);
        return m_Handle != reinterpret_cast<void*>(
            static_cast<long long>(-1)) && m_Handle != nullptr;
#else
        static_cast<void>(filePath);
        return false;
#endif
    }

    /// 关闭日志文件
    void Close()
    {
#if LIMX_PLATFORM_WINDOWS
        if (m_Handle != nullptr && m_Handle !=
            reinterpret_cast<void*>(static_cast<long long>(-1)))
        {
            CloseHandle(m_Handle);
            m_Handle = nullptr;
        }
#endif
    }

    void Write(const LogCategory& category,
               LogVerbosity verbosity,
               const AnsiChar* message) override
    {
#if LIMX_PLATFORM_WINDOWS
        if (m_Handle == nullptr || m_Handle ==
            reinterpret_cast<void*>(static_cast<long long>(-1)))
        {
            return;
        }

        // 格式化: [LEVEL] Category: Message\n
        FString formatted = StringFormat("[{}] {}: {}\n",
            GetVerbosityPrefix(verbosity),
            category.Name,
            message);

        unsigned long bytesWritten = 0;
        WriteFile(m_Handle,
                  formatted.GetCStr(),
                  static_cast<unsigned long>(formatted.GetLength()),
                  &bytesWritten, nullptr);

        // 立即刷新 — 确保崩溃前日志不丢失
        FlushFileBuffers(m_Handle);
#else
        static_cast<void>(category);
        static_cast<void>(verbosity);
        static_cast<void>(message);
#endif
    }

    void Flush() override
    {
#if LIMX_PLATFORM_WINDOWS
        if (m_Handle != nullptr && m_Handle !=
            reinterpret_cast<void*>(static_cast<long long>(-1)))
        {
            FlushFileBuffers(m_Handle);
        }
#endif
    }

private:
    void* m_Handle = nullptr;
};

// ============================================================================
// FLog — 全局日志管理器
// ============================================================================

/// 全局日志管理器 — 管理 Sink 注册和日志分发
struct FLog
{
    /// 注册日志输出目标
    static void AddSink(ILogSink* sink)
    {
        GetSinks().Add(sink);
    }

    /// 移除日志输出目标
    static void RemoveSink(ILogSink* sink)
    {
        TArray<ILogSink*>& sinks = GetSinks();
        for (SizeType index = 0; index < sinks.GetSize(); ++index)
        {
            if (sinks[index] == sink)
            {
                sinks.RemoveAt(index);
                return;
            }
        }
    }

    /// 刷新所有 Sink
    static void FlushAll()
    {
        TArray<ILogSink*>& sinks = GetSinks();
        for (SizeType index = 0; index < sinks.GetSize(); ++index)
        {
            sinks[index]->Flush();
        }
    }

    /// 获取/初始化默认控制台 Sink
    static ConsoleLogSink& GetDefaultConsoleSink()
    {
        static ConsoleLogSink s_ConsoleSink;
        return s_ConsoleSink;
    }

    /// 确保至少有一个 Sink 已注册
    static void EnsureInitialized()
    {
        static bool s_IsInitialized = false;
        if (!s_IsInitialized)
        {
            AddSink(&GetDefaultConsoleSink());
            s_IsInitialized = true;
        }
    }

    /// 核心日志分发 — 将消息发送到所有已注册的 Sink
    static void LogMessage(const LogCategory& category,
                           LogVerbosity verbosity,
                           const AnsiChar* message)
    {
        if (!category.ShouldLog(verbosity))
        {
            return;
        }

        EnsureInitialized();

        TArray<ILogSink*>& sinks = GetSinks();
        for (SizeType index = 0; index < sinks.GetSize(); ++index)
        {
            sinks[index]->Write(category, verbosity, message);
        }

        // Fatal 级别触发断言
        if (verbosity == LogVerbosity::Fatal)
        {
            LIMX_ASSERT(false);
        }
    }

    /// 带格式化的日志分发
    template<typename... Args>
    static void LogFormatted(const LogCategory& category,
                             LogVerbosity verbosity,
                             const AnsiChar* fmt,
                             Args&&... args)
    {
        if (!category.ShouldLog(verbosity))
        {
            return;
        }

        FString message = StringFormat(fmt, Forward<Args>(args)...);
        LogMessage(category, verbosity, message.GetCStr());
    }

private:
    /// 获取全局 Sink 列表
    static TArray<ILogSink*>& GetSinks()
    {
        static TArray<ILogSink*> s_Sinks;
        return s_Sinks;
    }
};

// ============================================================================
// 预定义日志分类
// ============================================================================

/// 核心模块日志分类
inline LogCategory LogCore("LogCore", LogVerbosity::All, LogVerbosity::Log);

// ============================================================================
// 日志宏
// ============================================================================

/// 声明日志分类 (在头文件中使用)
#define LIMX_DECLARE_LOG_CATEGORY(CategoryName) \
    extern ::Limx::LogCategory CategoryName;

/// 定义日志分类 (在 .cpp 文件中使用)
#define LIMX_DEFINE_LOG_CATEGORY(CategoryName) \
    ::Limx::LogCategory CategoryName(#CategoryName, \
        ::Limx::LogVerbosity::All, ::Limx::LogVerbosity::Log);

/// 带自定义级别的定义
#define LIMX_DEFINE_LOG_CATEGORY_VERBOSITY(CategoryName, DefaultVerbosity) \
    ::Limx::LogCategory CategoryName(#CategoryName, \
        ::Limx::LogVerbosity::All, DefaultVerbosity);

/// 日志宏 — 主入口
/// 用法: LIMX_LOG(LogCore, Log, "Player {} entered zone {}", name, zoneId)
#define LIMX_LOG(Category, Verbosity, Format, ...) \
    do \
    { \
        if (static_cast<::Limx::UInt8>(::Limx::LogVerbosity::Verbosity) <= \
            static_cast<::Limx::UInt8>((Category).CompileTimeVerbosity)) \
        { \
            ::Limx::FLog::LogFormatted( \
                (Category), \
                ::Limx::LogVerbosity::Verbosity, \
                Format __VA_OPT__(,) __VA_ARGS__); \
        } \
    } while (false)

/// 便捷宏 — 各级别
#define LIMX_LOG_FATAL(Category, Format, ...) \
    LIMX_LOG(Category, Fatal, Format __VA_OPT__(,) __VA_ARGS__)

#define LIMX_LOG_ERROR(Category, Format, ...) \
    LIMX_LOG(Category, Error, Format __VA_OPT__(,) __VA_ARGS__)

#define LIMX_LOG_WARNING(Category, Format, ...) \
    LIMX_LOG(Category, Warning, Format __VA_OPT__(,) __VA_ARGS__)

#define LIMX_LOG_DISPLAY(Category, Format, ...) \
    LIMX_LOG(Category, Display, Format __VA_OPT__(,) __VA_ARGS__)

#define LIMX_LOG_VERBOSE(Category, Format, ...) \
    LIMX_LOG(Category, Verbose, Format __VA_OPT__(,) __VA_ARGS__)

// Release 构建中去除 Verbose 日志
#if LIMX_BUILD_SHIPPING
    #undef LIMX_LOG_VERBOSE
    #define LIMX_LOG_VERBOSE(Category, Format, ...) do {} while (false)
#endif

} // namespace Limx
