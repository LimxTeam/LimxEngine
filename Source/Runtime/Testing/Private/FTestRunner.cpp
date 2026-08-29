/*******************************************************************************
 * 文件: FTestRunner.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   测试运行器实现 — 过滤、执行、计时、报告、命令行解析
 *   输出直写标准输出句柄, 保证 CI 与管道重定向都能采集到完整报告
 *
 * 设计哲学:
 *   报告以失败为中心 — 默认只打印失败用例的完整定位信息与失败明细，
 *   通过的用例仅计入汇总。这样上千个用例的输出仍能一屏看完关键结论，
 *   需要逐条确认时再用 --verbose 展开。
 *
 *   零 CRT 输出 — 不使用 printf/puts, 改用 Win32 WriteFile 直写句柄，
 *   并显式将控制台输出代码页设为 UTF-8 以正确呈现中文失败信息。
 *
 * 技术特性:
 *   - 单次遍历完成过滤与执行, 无中间容器
 *   - 每个用例独占 FTestContext, 失败不污染后续用例
 *   - 子串匹配为朴素 O(n*m) 实现 — 过滤器长度极短, 无需 KMP
 *   - 计时精度取决于 QueryPerformanceCounter 频率
 *
 * 依赖关系:
 *   内部: Testing/TestingMinimal.h, Core/HAL/FPlatformTime.h,
 *          Core/Containers/FStringBuilder.h
 *   外部: kernel32 (GetStdHandle / WriteFile / SetConsoleOutputCP)
 *
 * 注意事项:
 *   用例体内若发生未捕获的访问违例, 整个进程会崩溃 — 运行器不做异常隔离
 *   (引擎禁用异常, 且崩溃本身即是需要修复的测试信号)
 *
 ******************************************************************************/

#include "Testing/TestingMinimal.h"
#include "Core/HAL/FPlatformTime.h"
#include "Core/Containers/FStringBuilder.h"

// ============================================================================
// Win32 控制台输出前向声明 — 避免包含 windows.h 污染引擎命名空间
// ============================================================================

#if LIMX_PLATFORM_WINDOWS
#ifndef _WINDOWS_
extern "C"
{
    void* __stdcall GetStdHandle(unsigned long nStdHandle);

    int __stdcall WriteFile(
        void* hFile,
        const void* lpBuffer,
        unsigned long nNumberOfBytesToWrite,
        unsigned long* lpNumberOfBytesWritten,
        void* lpOverlapped);

    int __stdcall SetConsoleOutputCP(unsigned int wCodePageID);
}
#endif // _WINDOWS_
#endif

namespace Limx
{

namespace
{

// ============================================================================
// 输出常量
// ============================================================================

/// STD_OUTPUT_HANDLE — Win32 定义为 (DWORD)-11
constexpr unsigned long kStdOutputHandle = static_cast<unsigned long>(-11);

/// UTF-8 代码页
constexpr unsigned int kCodePageUtf8 = 65001;

/// 报告分隔线
constexpr const AnsiChar* kSeparator =
    "================================================================================";

// ============================================================================
// 字符串工具 — 零 CRT
// ============================================================================

/// 计算 C 字符串长度
SizeType CStringLength(const AnsiChar* text)
{
    if (text == nullptr)
    {
        return 0;
    }

    SizeType length = 0;
    while (text[length] != '\0')
    {
        ++length;
    }

    return length;
}

/// 判断 text 是否包含子串 pattern — 空 pattern 视为匹配
bool ContainsSubstring(const AnsiChar* text, const AnsiChar* pattern)
{
    if (pattern == nullptr || pattern[0] == '\0')
    {
        return true;
    }

    if (text == nullptr)
    {
        return false;
    }

    const SizeType textLength    = CStringLength(text);
    const SizeType patternLength = CStringLength(pattern);

    if (patternLength > textLength)
    {
        return false;
    }

    const SizeType lastStart = textLength - patternLength;

    for (SizeType start = 0; start <= lastStart; ++start)
    {
        SizeType offset = 0;
        while (offset < patternLength && text[start + offset] == pattern[offset])
        {
            ++offset;
        }

        if (offset == patternLength)
        {
            return true;
        }
    }

    return false;
}

/// 判断两个 C 字符串是否完全相同
bool AreCStringsEqual(const AnsiChar* left, const AnsiChar* right)
{
    if (left == nullptr || right == nullptr)
    {
        return left == right;
    }

    SizeType index = 0;
    while (left[index] != '\0' && right[index] != '\0')
    {
        if (left[index] != right[index])
        {
            return false;
        }

        ++index;
    }

    return left[index] == right[index];
}

// ============================================================================
// 过滤
// ============================================================================

/// 判断用例是否匹配过滤器
bool MatchesFilter(const ITestCase& testCase, const FTestRunOptions& options)
{
    return ContainsSubstring(testCase.GetSuite(), options.SuiteFilter) &&
           ContainsSubstring(testCase.GetName(), options.NameFilter);
}

} // namespace

// ============================================================================
// 标准输出写入
// ============================================================================

void FTestRunner::Write(const AnsiChar* text)
{
    if (text == nullptr)
    {
        return;
    }

    const SizeType length = CStringLength(text);
    if (length == 0)
    {
        return;
    }

#if LIMX_PLATFORM_WINDOWS
    void* handle = GetStdHandle(kStdOutputHandle);
    if (handle == nullptr)
    {
        return;
    }

    unsigned long written = 0;
    WriteFile(handle, text, static_cast<unsigned long>(length), &written, nullptr);
#endif
}

void FTestRunner::WriteLine(const AnsiChar* text)
{
    if (text != nullptr)
    {
        Write(text);
    }

    Write("\n");
}

// ============================================================================
// RunAll
// ============================================================================

FTestRunSummary FTestRunner::RunAll(const FTestRunOptions& options)
{
#if LIMX_PLATFORM_WINDOWS
    // 报告含中文, 未设为 UTF-8 时控制台会输出乱码
    SetConsoleOutputCP(kCodePageUtf8);
#endif

    FTestRunSummary summary;

    // ------------------------------------------------------------------
    // 统计匹配用例数 — 用于进度显示的分母
    // ------------------------------------------------------------------

    UInt32 matchedCount = 0;
    for (const ITestCase* cursor = FTestRegistry::GetHead();
         cursor != nullptr;
         cursor = cursor->GetNext())
    {
        if (MatchesFilter(*cursor, options))
        {
            ++matchedCount;
        }
    }

    summary.TotalTests = matchedCount;

    // ------------------------------------------------------------------
    // 报告头
    // ------------------------------------------------------------------

    WriteLine(kSeparator);
    WriteLine("  Limx Engine — 单元测试");
    WriteLine(kSeparator);
    WriteLine(StringFormat("  已注册用例: {}    匹配过滤器: {}",
                           FTestRegistry::GetCount(), matchedCount).GetCStr());
    WriteLine();

    // ------------------------------------------------------------------
    // 仅列举模式
    // ------------------------------------------------------------------

    if (options.ListOnly)
    {
        for (const ITestCase* cursor = FTestRegistry::GetHead();
             cursor != nullptr;
             cursor = cursor->GetNext())
        {
            if (!MatchesFilter(*cursor, options))
            {
                continue;
            }

            WriteLine(StringFormat("  {}.{}",
                                   cursor->GetSuite(), cursor->GetName()).GetCStr());
        }

        WriteLine();
        return summary;
    }

    // ------------------------------------------------------------------
    // 执行
    // ------------------------------------------------------------------

    const Float64 startTime = FPlatformTime::Seconds();

    UInt32 executedIndex = 0;

    for (ITestCase* cursor = FTestRegistry::GetHead();
         cursor != nullptr;
         cursor = cursor->GetNext())
    {
        if (!MatchesFilter(*cursor, options))
        {
            continue;
        }

        ++executedIndex;

        FTestContext context;
        cursor->Run(context);

        summary.TotalChecks += context.GetCheckCount();
        summary.FailedChecks +=
            static_cast<UInt32>(context.GetFailureCount());

        // ---- 跳过 ----
        if (context.IsSkipped())
        {
            ++summary.SkippedTests;

            WriteLine(StringFormat("[{}/{}] {}.{} — 跳过 ({})",
                                   executedIndex, matchedCount,
                                   cursor->GetSuite(), cursor->GetName(),
                                   context.GetSkipReason() != nullptr
                                       ? context.GetSkipReason()
                                       : "未说明原因").GetCStr());
            continue;
        }

        // ---- 失败 ----
        if (context.HasFailed())
        {
            ++summary.FailedTests;

            WriteLine(StringFormat("[失败] {}.{}",
                                   cursor->GetSuite(), cursor->GetName()).GetCStr());
            WriteLine(StringFormat("       定义于 {}:{}",
                                   cursor->GetFile(), cursor->GetLine()).GetCStr());

            const TArray<FTestFailure>& failures = context.GetFailures();
            for (SizeType i = 0; i < failures.GetSize(); ++i)
            {
                const FTestFailure& failure = failures[i];

                WriteLine(StringFormat("       {}:{}",
                                       failure.File, failure.Line).GetCStr());
                WriteLine(StringFormat("         {}",
                                       failure.Message).GetCStr());
            }

            WriteLine();

            if (options.StopOnFirstFailure)
            {
                WriteLine("  已启用 --stop-on-failure, 停止后续用例");
                WriteLine();
                break;
            }

            continue;
        }

        // ---- 通过 ----
        ++summary.PassedTests;

        if (options.Verbose)
        {
            WriteLine(StringFormat("[{}/{}] {}.{} — 通过 ({} 项检查)",
                                   executedIndex, matchedCount,
                                   cursor->GetSuite(), cursor->GetName(),
                                   context.GetCheckCount()).GetCStr());
        }
    }

    summary.ElapsedSeconds = FPlatformTime::Seconds() - startTime;

    // ------------------------------------------------------------------
    // 汇总
    // ------------------------------------------------------------------

    WriteLine(kSeparator);
    WriteLine(StringFormat("  用例: {} 总计 | {} 通过 | {} 失败 | {} 跳过",
                           summary.TotalTests, summary.PassedTests,
                           summary.FailedTests, summary.SkippedTests).GetCStr());
    WriteLine(StringFormat("  检查: {} 总计 | {} 失败",
                           summary.TotalChecks, summary.FailedChecks).GetCStr());
    WriteLine(StringFormat("  耗时: {} 秒", summary.ElapsedSeconds).GetCStr());
    WriteLine(StringFormat("  结果: {}",
                           summary.IsSuccess() ? "全部通过" : "存在失败").GetCStr());
    WriteLine(kSeparator);

    return summary;
}

// ============================================================================
// Main — 命令行入口
// ============================================================================

Int32 FTestRunner::Main(Int32 argc, AnsiChar** argv)
{
    FTestRunOptions options;

    for (Int32 i = 1; i < argc; ++i)
    {
        const AnsiChar* argument = argv[i];

        if (AreCStringsEqual(argument, "--help") ||
            AreCStringsEqual(argument, "-h"))
        {
            WriteLine("Limx 单元测试运行器");
            WriteLine();
            WriteLine("用法: <可执行文件> [选项]");
            WriteLine();
            WriteLine("选项:");
            WriteLine("  --suite <子串>       只运行套件名包含该子串的用例");
            WriteLine("  --test <子串>        只运行用例名包含该子串的用例");
            WriteLine("  --list               只列举匹配的用例, 不执行");
            WriteLine("  --verbose            打印每个用例的结果行");
            WriteLine("  --stop-on-failure    首个失败后停止");
            WriteLine("  --help               显示本帮助");
            WriteLine();
            WriteLine("退出码: 0 全部通过 | 1 存在失败 | 2 参数错误");
            return 0;
        }

        if (AreCStringsEqual(argument, "--suite"))
        {
            if (i + 1 >= argc)
            {
                WriteLine("错误: --suite 需要一个参数");
                return 2;
            }

            options.SuiteFilter = argv[++i];
            continue;
        }

        if (AreCStringsEqual(argument, "--test"))
        {
            if (i + 1 >= argc)
            {
                WriteLine("错误: --test 需要一个参数");
                return 2;
            }

            options.NameFilter = argv[++i];
            continue;
        }

        if (AreCStringsEqual(argument, "--list"))
        {
            options.ListOnly = true;
            continue;
        }

        if (AreCStringsEqual(argument, "--verbose"))
        {
            options.Verbose = true;
            continue;
        }

        if (AreCStringsEqual(argument, "--stop-on-failure"))
        {
            options.StopOnFirstFailure = true;
            continue;
        }

        WriteLine(StringFormat("错误: 无法识别的参数 '{}'", argument).GetCStr());
        WriteLine("使用 --help 查看可用选项");
        return 2;
    }

    const FTestRunSummary summary = RunAll(options);

    return summary.IsSuccess() ? 0 : 1;
}

} // namespace Limx
