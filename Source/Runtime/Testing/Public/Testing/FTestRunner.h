/*******************************************************************************
 * 文件: FTestRunner.h
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   测试运行器 — 遍历注册表执行用例、收集统计、输出报告、返回进程退出码
 *   支持按套件名/用例名子串过滤，支持只列举不执行，支持首个失败即停
 *   提供 Main 入口, 测试可执行文件的 main 只需一行转发
 *
 * 设计哲学:
 *   直写标准输出 — Core 的 ConsoleLogSink 走 OutputDebugStringA，仅调试器可见，
 *   对 CI 无用。运行器改用 Win32 GetStdHandle + WriteFile 直写 stdout，
 *   保证管道重定向、CI 日志采集都能拿到完整报告。
 *
 *   退出码即结论 — 有任一用例失败返回 1，全部通过返回 0，
 *   使 CI 无需解析文本即可判定构建成败。
 *
 * 技术特性:
 *   - 报告分两段: 逐用例进度行 + 末尾失败明细与汇总
 *   - 计时基于 FPlatformTime::Seconds, 单位秒, 精度取决于 QPC 频率
 *   - 过滤为大小写敏感子串匹配, 空过滤器匹配全部
 *   - 单个用例失败不影响后续用例执行 (除非启用 --stop-on-failure)
 *
 * 依赖关系:
 *   内部: Testing/FTestCase.h, Testing/FTestRegistry.h, Testing/TestingAPI.h,
 *          Core/HAL/FPlatformTime.h
 *   外部: kernel32 (GetStdHandle / WriteFile)
 *
 * 注意事项:
 *   运行器假设注册在 main 之前完成 — 运行期动态注册的用例不会被执行
 *   输出为 UTF-8 字节流, 控制台代码页非 65001 时中文可能显示异常
 *
 ******************************************************************************/

#pragma once

#include "Core/CoreMinimal.h"
#include "Testing/FTestCase.h"
#include "Testing/FTestRegistry.h"
#include "Testing/TestingAPI.h"

namespace Limx
{

// ============================================================================
// FTestRunOptions — 运行配置
// ============================================================================

/// 测试运行选项
struct FTestRunOptions
{
    /// 套件名过滤 — 子串匹配, nullptr 表示不过滤
    const AnsiChar* SuiteFilter = nullptr;

    /// 用例名过滤 — 子串匹配, nullptr 表示不过滤
    const AnsiChar* NameFilter = nullptr;

    /// 首个用例失败后立即停止
    bool StopOnFirstFailure = false;

    /// 逐用例打印进度行 (默认仅打印失败用例)
    bool Verbose = false;

    /// 只列举匹配的用例, 不执行
    bool ListOnly = false;
};

// ============================================================================
// FTestRunSummary — 运行汇总
// ============================================================================

/// 一次运行的统计结果
struct FTestRunSummary
{
    /// 匹配过滤器的用例总数
    UInt32 TotalTests = 0;

    /// 全部检查均通过的用例数
    UInt32 PassedTests = 0;

    /// 至少一次检查失败的用例数
    UInt32 FailedTests = 0;

    /// 主动跳过的用例数
    UInt32 SkippedTests = 0;

    /// 执行的检查总次数
    UInt32 TotalChecks = 0;

    /// 失败的检查次数
    UInt32 FailedChecks = 0;

    /// 总耗时 (秒)
    Float64 ElapsedSeconds = 0.0;

    /// 是否全部通过 — 跳过不算失败
    ///
    /// 一个用例都没跑到**不算通过**。零个用例意味着零个失败, 于是"没有
    /// 失败"这个条件恒真 —— 而过滤器打错字、套件被改名、静态注册被链接器
    /// 丢掉, 全都表现为零个用例。
    ///
    /// 这不是假想: ci.yml 与 verify.ps1 各有五步按字面名字过滤套件。把
    /// BlendMode 改个名字, 那一步照样绿, 而它什么都没测。
    LIMX_NODISCARD bool IsSuccess() const
    {
        return FailedTests == 0 && TotalTests > 0;
    }
};

// ============================================================================
// FTestRunner — 运行器
// ============================================================================

/// 测试运行器 — 全静态接口
class LIMX_TESTING_API FTestRunner
{
public:
    FTestRunner()                              = delete;
    ~FTestRunner()                             = delete;
    FTestRunner(const FTestRunner&)            = delete;
    FTestRunner& operator=(const FTestRunner&) = delete;

    /// 执行全部匹配过滤器的用例并输出报告
    /// @param options 运行选项
    /// @return 运行汇总
    static FTestRunSummary RunAll(const FTestRunOptions& options);

    /// 命令行入口 — 解析参数后调用 RunAll
    ///
    /// 支持的参数:
    ///   --suite <子串>        只运行套件名包含该子串的用例
    ///   --test <子串>         只运行用例名包含该子串的用例
    ///   --list                只列举匹配的用例, 不执行
    ///   --verbose             打印每个用例的结果行
    ///   --stop-on-failure     首个失败后停止
    ///   --help                打印用法
    ///
    /// @return 进程退出码 — 0 表示全部通过, 1 表示存在失败, 2 表示参数错误
    static Int32 Main(Int32 argc, AnsiChar** argv);

    /// 向标准输出写入文本 (不追加换行)
    static void Write(const AnsiChar* text);

    /// 向标准输出写入一行 (追加换行)
    static void WriteLine(const AnsiChar* text = nullptr);
};

} // namespace Limx
