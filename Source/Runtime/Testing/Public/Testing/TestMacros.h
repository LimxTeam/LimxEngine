/*******************************************************************************
 * 文件: TestMacros.h
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   测试定义宏与断言宏 — LIMX_TEST 定义用例，EXPECT 与 REQUIRE 两族宏做检查
 *   EXPECT 系列失败后继续执行，一次运行可暴露同一用例中的多个问题
 *   REQUIRE 系列失败后立即 return 退出用例，用于前置条件不满足时避免级联崩溃
 *
 * 设计哲学:
 *   失败信息自解释 — 断言失败时同时打印源表达式文本与实际求值结果，
 *   例如 "EXPECT_EQ(arr.GetSize(), 3) 失败: 实际 2, 期望 3"，无需重跑调试器。
 *
 *   值描述分级降级 — 通过 C++23 concept 探测类型能否构造 FStringFormatArg：
 *   枚举转 Int64 打印、可格式化类型直接打印、其余类型退化为占位文本。
 *   这样任意用户类型都能参与断言而不会导致编译失败。
 *
 *   宏零副作用 — 每个宏参数在展开中只求值一次并绑定到 const 引用，
 *   避免 LIMX_EXPECT_EQ(Pop(), 1) 这类带副作用表达式被重复执行。
 *
 * 技术特性:
 *   - 单次求值: 参数经函数模板按 const& 传递，宏内不重复展开表达式
 *   - 类型安全比较: TLeft/TRight 独立模板参数，支持异类型比较
 *   - 浮点容差: LIMX_EXPECT_NEAR 显式指定绝对容差, 拒绝 == 比较浮点
 *   - 自动注册: LIMX_TEST 生成的静态实例在 main 前入表
 *
 * 依赖关系:
 *   内部: Testing/FTestCase.h, Testing/FTestRegistry.h,
 *          Core/Containers/FStringFormat.h, Core/TypeTraits/TypeTraits.h
 *
 * 注意事项:
 *   同一 (Suite, Test) 名字对在整个可执行文件内必须唯一，否则违反 ODR
 *   断言宏只能在 LIMX_TEST 体内使用 — 它们引用宏生成的 limxTestContext 形参
 *   LIMX_EXPECT_NEAR 使用绝对容差, 比较量级差异巨大的值时需自行换算相对误差
 *
 ******************************************************************************/

#pragma once

#include "Core/CoreMinimal.h"
#include "Core/Containers/FString.h"
#include "Core/Containers/FStringFormat.h"
#include "Core/Math/FMath.h"
#include "Testing/FTestCase.h"
#include "Testing/FTestRegistry.h"
#include "Testing/FTestRunner.h"
#include "Testing/TestingAPI.h"

namespace Limx
{
namespace Testing
{

// ============================================================================
// 值描述 — 将任意类型转为可读文本用于失败信息
// ============================================================================

/// 探测类型能否用于构造 FStringFormatArg
template<typename T>
concept CFormatArgConstructible = requires(const T& value)
{
    ::Limx::FStringFormatArg(value);
};

/// 将值转为人类可读文本
///
/// 分三级处理:
///   1. 枚举      — 转 Int64 打印其整数值 (enum class 无隐式转换, 需显式处理)
///   2. 可格式化  — 交由 FStringFormatArg 打印 (整数/浮点/bool/字符串/指针)
///   3. 其余      — 退化为占位文本, 保证任意类型都能参与断言
template<typename T>
LIMX_NODISCARD FString DescribeValue(const T& value)
{
    using TBare = RemoveCVT<RemoveReferenceT<T>>;

    if constexpr (IsEnumV<TBare>)
    {
        return StringFormat("{}", static_cast<Int64>(value));
    }
    else if constexpr (CFormatArgConstructible<TBare>)
    {
        return StringFormat("{}", value);
    }
    else
    {
        return StringFormat("<{}字节不可打印值>",
                            static_cast<UInt64>(sizeof(TBare)));
    }
}

// ============================================================================
// 检查实现 — 所有断言宏最终汇聚到这些函数模板
//
// 统一返回 bool: true 表示检查通过。REQUIRE 系列宏据此决定是否 return。
// ============================================================================

/// 记录一次失败并按需置位中止标志
LIMX_TESTING_API bool ReportCheckFailure(FTestContext& context,
                                         const FString&  message,
                                         const AnsiChar* file,
                                         Int32           line,
                                         bool            isRequired);

/// 布尔条件检查
inline bool CheckBool(FTestContext& context, bool actual, bool expected,
                      const AnsiChar* expression, const AnsiChar* file,
                      Int32 line, bool isRequired)
{
    if (actual == expected)
    {
        context.ReportPass();
        return true;
    }

    return ReportCheckFailure(
        context,
        StringFormat("{}({}) 失败: 实际为 {}",
                     expected ? "EXPECT_TRUE" : "EXPECT_FALSE",
                     expression, actual),
        file, line, isRequired);
}

/// 相等性检查
template<typename TLeft, typename TRight>
bool CheckEqual(FTestContext& context, const TLeft& lhs, const TRight& rhs,
                const AnsiChar* lhsExpression, const AnsiChar* rhsExpression,
                const AnsiChar* file, Int32 line, bool isRequired)
{
    if (lhs == rhs)
    {
        context.ReportPass();
        return true;
    }

    return ReportCheckFailure(
        context,
        StringFormat("EXPECT_EQ({}, {}) 失败: 实际 {}, 期望 {}",
                     lhsExpression, rhsExpression,
                     DescribeValue(lhs), DescribeValue(rhs)),
        file, line, isRequired);
}

/// 不等性检查
template<typename TLeft, typename TRight>
bool CheckNotEqual(FTestContext& context, const TLeft& lhs, const TRight& rhs,
                   const AnsiChar* lhsExpression, const AnsiChar* rhsExpression,
                   const AnsiChar* file, Int32 line, bool isRequired)
{
    if (!(lhs == rhs))
    {
        context.ReportPass();
        return true;
    }

    return ReportCheckFailure(
        context,
        StringFormat("EXPECT_NE({}, {}) 失败: 两者同为 {}",
                     lhsExpression, rhsExpression, DescribeValue(lhs)),
        file, line, isRequired);
}

/// 关系比较检查 — 由调用方传入已求值的比较结果与运算符文本
template<typename TLeft, typename TRight>
bool CheckRelation(FTestContext& context, bool comparisonResult,
                   const TLeft& lhs, const TRight& rhs,
                   const AnsiChar* lhsExpression, const AnsiChar* op,
                   const AnsiChar* rhsExpression,
                   const AnsiChar* file, Int32 line, bool isRequired)
{
    if (comparisonResult)
    {
        context.ReportPass();
        return true;
    }

    return ReportCheckFailure(
        context,
        StringFormat("EXPECT({} {} {}) 失败: 左值 {}, 右值 {}",
                     lhsExpression, op, rhsExpression,
                     DescribeValue(lhs), DescribeValue(rhs)),
        file, line, isRequired);
}

/// 浮点近似相等检查 — 绝对容差
inline bool CheckNear(FTestContext& context, Float64 actual, Float64 expected,
                      Float64 tolerance, const AnsiChar* actualExpression,
                      const AnsiChar* expectedExpression,
                      const AnsiChar* file, Int32 line, bool isRequired)
{
    const Float64 difference = FMath::Abs(actual - expected);

    if (difference <= tolerance)
    {
        context.ReportPass();
        return true;
    }

    return ReportCheckFailure(
        context,
        StringFormat("EXPECT_NEAR({}, {}) 失败: 实际 {}, 期望 {}, 偏差 {} > 容差 {}",
                     actualExpression, expectedExpression,
                     actual, expected, difference, tolerance),
        file, line, isRequired);
}

/// C 字符串内容相等检查 — 比较内容而非指针
LIMX_TESTING_API bool CheckStringEqual(FTestContext& context,
                                       const AnsiChar* actual,
                                       const AnsiChar* expected,
                                       const AnsiChar* actualExpression,
                                       const AnsiChar* expectedExpression,
                                       const AnsiChar* file, Int32 line,
                                       bool isRequired);

/// 指针空/非空检查
inline bool CheckPointerNull(FTestContext& context, const void* pointer,
                             bool expectNull, const AnsiChar* expression,
                             const AnsiChar* file, Int32 line, bool isRequired)
{
    const bool isNull = (pointer == nullptr);

    if (isNull == expectNull)
    {
        context.ReportPass();
        return true;
    }

    return ReportCheckFailure(
        context,
        StringFormat("{}({}) 失败: 实际为 {}",
                     expectNull ? "EXPECT_NULL" : "EXPECT_NOT_NULL",
                     expression,
                     isNull ? "nullptr" : DescribeValue(pointer).GetCStr()),
        file, line, isRequired);
}

} // namespace Testing
} // namespace Limx

// ============================================================================
// LIMX_TEST — 定义并自动注册一个测试用例
//
// 用法:
//     LIMX_TEST(TArray, AddIncreasesSize)
//     {
//         TArray<Int32> values;
//         values.Add(42);
//         LIMX_EXPECT_EQ(values.GetSize(), 1u);
//     }
//
// 展开产物:
//     1. LimxTestCases 命名空间内的一个 ITestCase 派生类
//     2. 该类的一个静态实例 — 构造期自动注册到 FTestRegistry
//     3. Run 成员函数的定义头, 紧随其后的 { } 即用例体
//
// 约束: 同一可执行文件内 (Suite, Test) 名字对必须唯一
// ============================================================================

#define LIMX_TEST(SuiteName, TestName)                                         \
    namespace LimxTestCases                                                    \
    {                                                                          \
    class FLimxTest_##SuiteName##_##TestName final : public ::Limx::ITestCase  \
    {                                                                          \
    public:                                                                    \
        FLimxTest_##SuiteName##_##TestName()                                   \
            : ::Limx::ITestCase(#SuiteName, #TestName, __FILE__, __LINE__)     \
        {                                                                      \
        }                                                                      \
                                                                               \
        void Run(::Limx::FTestContext& limxTestContext) override;              \
    };                                                                         \
                                                                               \
    static FLimxTest_##SuiteName##_##TestName                                  \
        g_LimxTestInstance_##SuiteName##_##TestName;                           \
    }                                                                          \
                                                                               \
    void LimxTestCases::FLimxTest_##SuiteName##_##TestName::Run(               \
        [[maybe_unused]] ::Limx::FTestContext& limxTestContext)

// ============================================================================
// 断言宏 — EXPECT (失败后继续) / REQUIRE (失败后退出用例)
// ============================================================================

/// EXPECT 系列: 求值并记录, 无论成败都继续执行后续语句
#define LIMX_EXPECT_TRUE(Expression)                                           \
    ((void)::Limx::Testing::CheckBool(limxTestContext, (Expression), true,     \
                                      #Expression, __FILE__, __LINE__, false))

#define LIMX_EXPECT_FALSE(Expression)                                          \
    ((void)::Limx::Testing::CheckBool(limxTestContext, (Expression), false,    \
                                      #Expression, __FILE__, __LINE__, false))

#define LIMX_EXPECT_EQ(Actual, Expected)                                       \
    ((void)::Limx::Testing::CheckEqual(limxTestContext, (Actual), (Expected),  \
                                       #Actual, #Expected,                     \
                                       __FILE__, __LINE__, false))

#define LIMX_EXPECT_NE(Actual, Expected)                                       \
    ((void)::Limx::Testing::CheckNotEqual(limxTestContext, (Actual),           \
                                          (Expected), #Actual, #Expected,      \
                                          __FILE__, __LINE__, false))

#define LIMX_EXPECT_LT(Left, Right)                                            \
    ((void)::Limx::Testing::CheckRelation(limxTestContext, ((Left) < (Right)), \
                                          (Left), (Right), #Left, "<", #Right, \
                                          __FILE__, __LINE__, false))

#define LIMX_EXPECT_LE(Left, Right)                                            \
    ((void)::Limx::Testing::CheckRelation(limxTestContext, ((Left) <= (Right)),\
                                          (Left), (Right), #Left, "<=",        \
                                          #Right, __FILE__, __LINE__, false))

#define LIMX_EXPECT_GT(Left, Right)                                            \
    ((void)::Limx::Testing::CheckRelation(limxTestContext, ((Left) > (Right)), \
                                          (Left), (Right), #Left, ">", #Right, \
                                          __FILE__, __LINE__, false))

#define LIMX_EXPECT_GE(Left, Right)                                            \
    ((void)::Limx::Testing::CheckRelation(limxTestContext, ((Left) >= (Right)),\
                                          (Left), (Right), #Left, ">=",        \
                                          #Right, __FILE__, __LINE__, false))

#define LIMX_EXPECT_NEAR(Actual, Expected, Tolerance)                          \
    ((void)::Limx::Testing::CheckNear(limxTestContext,                         \
                                      static_cast<::Limx::Float64>(Actual),    \
                                      static_cast<::Limx::Float64>(Expected),  \
                                      static_cast<::Limx::Float64>(Tolerance), \
                                      #Actual, #Expected,                      \
                                      __FILE__, __LINE__, false))

#define LIMX_EXPECT_STREQ(Actual, Expected)                                    \
    ((void)::Limx::Testing::CheckStringEqual(limxTestContext, (Actual),        \
                                             (Expected), #Actual, #Expected,   \
                                             __FILE__, __LINE__, false))

#define LIMX_EXPECT_NULL(Pointer)                                              \
    ((void)::Limx::Testing::CheckPointerNull(limxTestContext, (Pointer), true, \
                                             #Pointer, __FILE__, __LINE__,     \
                                             false))

#define LIMX_EXPECT_NOT_NULL(Pointer)                                          \
    ((void)::Limx::Testing::CheckPointerNull(limxTestContext, (Pointer),       \
                                             false, #Pointer, __FILE__,        \
                                             __LINE__, false))

/// REQUIRE 系列: 失败时立即退出当前用例, 用于后续语句依赖该前置条件的场景
#define LIMX_REQUIRE_TRUE(Expression)                                          \
    do                                                                         \
    {                                                                          \
        if (!::Limx::Testing::CheckBool(limxTestContext, (Expression), true,   \
                                        #Expression, __FILE__, __LINE__, true))\
        {                                                                      \
            return;                                                            \
        }                                                                      \
    } while (false)

#define LIMX_REQUIRE_FALSE(Expression)                                         \
    do                                                                         \
    {                                                                          \
        if (!::Limx::Testing::CheckBool(limxTestContext, (Expression), false,  \
                                        #Expression, __FILE__, __LINE__, true))\
        {                                                                      \
            return;                                                            \
        }                                                                      \
    } while (false)

#define LIMX_REQUIRE_EQ(Actual, Expected)                                      \
    do                                                                         \
    {                                                                          \
        if (!::Limx::Testing::CheckEqual(limxTestContext, (Actual), (Expected),\
                                         #Actual, #Expected, __FILE__,         \
                                         __LINE__, true))                      \
        {                                                                      \
            return;                                                            \
        }                                                                      \
    } while (false)

#define LIMX_REQUIRE_NE(Actual, Expected)                                      \
    do                                                                         \
    {                                                                          \
        if (!::Limx::Testing::CheckNotEqual(limxTestContext, (Actual),         \
                                            (Expected), #Actual, #Expected,    \
                                            __FILE__, __LINE__, true))         \
        {                                                                      \
            return;                                                            \
        }                                                                      \
    } while (false)

/// REQUIRE 关系比较 — 与 EXPECT 系列一一对应, 失败即退出用例
#define LIMX_REQUIRE_RELATION_IMPL(Left, Op, OpText, Right)                        do                                                                             {                                                                                  if (!::Limx::Testing::CheckRelation(limxTestContext,                                                               ((Left) Op (Right)),                                                           (Left), (Right), #Left, OpText,                                                #Right, __FILE__, __LINE__, true))         {                                                                                  return;                                                                    }                                                                          } while (false)

#define LIMX_REQUIRE_LT(Left, Right)     LIMX_REQUIRE_RELATION_IMPL(Left, <, "<", Right)

#define LIMX_REQUIRE_LE(Left, Right)     LIMX_REQUIRE_RELATION_IMPL(Left, <=, "<=", Right)

#define LIMX_REQUIRE_GT(Left, Right)     LIMX_REQUIRE_RELATION_IMPL(Left, >, ">", Right)

#define LIMX_REQUIRE_GE(Left, Right)     LIMX_REQUIRE_RELATION_IMPL(Left, >=, ">=", Right)

#define LIMX_REQUIRE_NOT_NULL(Pointer)                                         \
    do                                                                         \
    {                                                                          \
        if (!::Limx::Testing::CheckPointerNull(limxTestContext, (Pointer),     \
                                               false, #Pointer, __FILE__,      \
                                               __LINE__, true))                \
        {                                                                      \
            return;                                                            \
        }                                                                      \
    } while (false)

// ============================================================================
// 控制宏
// ============================================================================

/// 无条件判定当前用例失败
#define LIMX_TEST_FAIL(Message)                                                \
    ((void)::Limx::Testing::ReportCheckFailure(                                \
        limxTestContext, ::Limx::FString(Message), __FILE__, __LINE__, false))

/// 从用例内向报告输出一行信息
///
/// 用于让用例把量化结论 (吞吐量、承载数、耗时) 带进报告，而不是只留下
/// 一个"通过"。参数可以是任何 StringFormat 支持的表达式。
#define LIMX_TEST_INFO(FormatText, ...)                                            ::Limx::FTestRunner::WriteLine(                                                    ::Limx::StringFormat("       · " FormatText, ##__VA_ARGS__).GetCStr())

/// 跳过当前用例 — 既不计通过也不计失败
#define LIMX_TEST_SKIP(Reason)                                                 \
    do                                                                         \
    {                                                                          \
        limxTestContext.Skip(Reason);                                          \
        return;                                                                \
    } while (false)
