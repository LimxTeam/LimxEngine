/*******************************************************************************
 * 文件: FTestCase.h
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   测试用例基类与运行上下文 — 定义 ITestCase 接口和 FTestContext 结果收集器
 *   ITestCase 在构造期自动注册到全局链表，无需显式注册调用
 *   FTestContext 收集单个用例内的全部检查结果，支持"失败后继续"与"失败即中止"
 *
 * 设计哲学:
 *   侵入式静态链表注册 — ITestCase 自带 m_Next 指针，注册表仅持有零初始化的
 *   头尾指针。这样注册不依赖任何需要动态初始化的全局对象，彻底规避跨翻译单元
 *   的静态初始化顺序问题（若改用 TArray 作注册容器，其分配器可能尚未构造）。
 *
 *   失败不抛异常 — 引擎禁用异常，EXPECT 系列失败后继续执行以便一次暴露多个问题，
 *   REQUIRE 系列置位中止标志由宏内 return 跳出用例体。
 *
 * 技术特性:
 *   - 零动态初始化注册: 静态链表头为 nullptr 常量初始化
 *   - 失败记录携带 文件/行号/表达式/实际值，便于定位
 *   - 检查计数与失败计数分离，可报告"N 次检查中 M 次失败"
 *   - 用例名与套件名为字面量指针，不做拷贝，零分配
 *
 * 依赖关系:
 *   内部: Core/CoreMinimal.h, Core/Containers/FString.h,
 *          Core/Containers/TArray.h, Testing/TestingAPI.h
 *
 * 注意事项:
 *   ITestCase 实例必须具有静态存储期 — 宏 LIMX_TEST 生成的实例满足此约束
 *   FTestContext 非线程安全，单个用例内的并发检查需外部同步
 *
 ******************************************************************************/

#pragma once

#include "Core/CoreMinimal.h"
#include "Core/Containers/FString.h"
#include "Core/Containers/TArray.h"
#include "Testing/TestingAPI.h"

namespace Limx
{

// ============================================================================
// FTestFailure — 单条失败记录
// ============================================================================

/// 一次失败的检查 — 记录位置与人类可读的失败描述
struct FTestFailure
{
    /// 源文件路径 (__FILE__ 字面量, 不拷贝)
    const AnsiChar* File = nullptr;

    /// 源文件行号
    Int32 Line = 0;

    /// 失败描述 — 形如 "EXPECT_EQ(a, b) 失败: 实际 3 != 期望 4"
    FString Message;
};

// ============================================================================
// FTestContext — 单个用例的运行上下文
// ============================================================================

/// 测试运行上下文 — 收集一个用例内所有检查的结果
///
/// 每个用例独占一个上下文实例。断言宏通过它上报检查结果，
/// 运行器在用例结束后读取统计与失败列表生成报告。
class LIMX_TESTING_API FTestContext
{
public:
    FTestContext() = default;

    // 上下文持有失败列表, 禁止拷贝以避免误用
    FTestContext(const FTestContext&)            = delete;
    FTestContext& operator=(const FTestContext&) = delete;

    // ========================================================================
    // 结果上报
    // ========================================================================

    /// 记录一次通过的检查
    void ReportPass()
    {
        ++m_CheckCount;
    }

    /// 记录一次失败的检查
    /// @param file    源文件 (__FILE__)
    /// @param line    行号 (__LINE__)
    /// @param message 失败描述
    void ReportFailure(const AnsiChar* file, Int32 line, const FString& message)
    {
        ++m_CheckCount;

        FTestFailure failure;
        failure.File    = file;
        failure.Line    = line;
        failure.Message = message;

        m_Failures.Add(static_cast<FTestFailure&&>(failure));
    }

    /// 将本用例标记为跳过 — 跳过的用例既不计通过也不计失败
    /// @param reason 跳过原因
    void Skip(const AnsiChar* reason)
    {
        m_IsSkipped   = true;
        m_SkipReason  = reason;
        m_ShouldAbort = true;
    }

    /// 请求中止当前用例 — REQUIRE 系列断言失败时置位
    void RequestAbort()
    {
        m_ShouldAbort = true;
    }

    // ========================================================================
    // 状态查询
    // ========================================================================

    /// 本用例是否已有失败检查
    LIMX_NODISCARD bool HasFailed() const
    {
        return m_Failures.GetSize() > 0;
    }

    /// 是否应中止当前用例 (REQUIRE 失败或显式跳过)
    LIMX_NODISCARD bool ShouldAbort() const { return m_ShouldAbort; }

    /// 本用例是否被跳过
    LIMX_NODISCARD bool IsSkipped() const { return m_IsSkipped; }

    /// 跳过原因 — 未跳过时返回 nullptr
    LIMX_NODISCARD const AnsiChar* GetSkipReason() const { return m_SkipReason; }

    /// 已执行的检查总数 (含通过与失败)
    LIMX_NODISCARD UInt32 GetCheckCount() const { return m_CheckCount; }

    /// 失败的检查数
    LIMX_NODISCARD SizeType GetFailureCount() const { return m_Failures.GetSize(); }

    /// 失败记录列表
    LIMX_NODISCARD const TArray<FTestFailure>& GetFailures() const { return m_Failures; }

private:
    /// 失败记录
    TArray<FTestFailure> m_Failures;

    /// 已执行检查数
    UInt32 m_CheckCount = 0;

    /// 中止标志
    bool m_ShouldAbort = false;

    /// 跳过标志
    bool m_IsSkipped = false;

    /// 跳过原因
    const AnsiChar* m_SkipReason = nullptr;
};

// ============================================================================
// ITestCase — 测试用例接口
// ============================================================================

/// 测试用例基类
///
/// 构造函数自动将自身链入全局注册链表，因此由 LIMX_TEST 宏生成的静态实例
/// 在程序启动时即完成注册，运行器无需感知用例定义在哪个翻译单元。
class LIMX_TESTING_API ITestCase
{
public:
    /// @param suite 套件名 (字面量, 生命周期需覆盖整个进程)
    /// @param name  用例名 (字面量)
    /// @param file  定义处源文件 (__FILE__)
    /// @param line  定义处行号 (__LINE__)
    ITestCase(const AnsiChar* suite, const AnsiChar* name,
              const AnsiChar* file, Int32 line);

    virtual ~ITestCase() = default;

    ITestCase(const ITestCase&)            = delete;
    ITestCase& operator=(const ITestCase&) = delete;

    /// 用例体 — 由 LIMX_TEST 宏生成的派生类实现
    virtual void Run(FTestContext& context) = 0;

    // ========================================================================
    // 元信息
    // ========================================================================

    LIMX_NODISCARD const AnsiChar* GetSuite() const { return m_Suite; }
    LIMX_NODISCARD const AnsiChar* GetName() const  { return m_Name; }
    LIMX_NODISCARD const AnsiChar* GetFile() const  { return m_File; }
    LIMX_NODISCARD Int32           GetLine() const  { return m_Line; }

    /// 注册链表中的下一个用例 — 仅供 FTestRegistry 遍历使用
    LIMX_NODISCARD ITestCase* GetNext() const { return m_Next; }

    /// 设置链表后继 — 仅供 FTestRegistry 在注册时调用
    void SetNext(ITestCase* next) { m_Next = next; }

private:
    const AnsiChar* m_Suite = nullptr;
    const AnsiChar* m_Name  = nullptr;
    const AnsiChar* m_File  = nullptr;
    Int32           m_Line  = 0;

    /// 侵入式注册链表后继指针
    ITestCase* m_Next = nullptr;
};

} // namespace Limx
