/*******************************************************************************
 * 文件: TestMacros.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   断言辅助函数的非内联实现 — 失败记录入口与 C 字符串内容比较
 *
 * 设计哲学:
 *   失败路径外联 — 检查通过是热路径, 保持在头文件内联; 失败构造信息字符串
 *   属于冷路径, 移入 .cpp 以减小每个测试翻译单元的代码膨胀。
 *
 * 技术特性:
 *   - 字符串比较手写逐字节循环, 不依赖 CRT strcmp
 *   - nullptr 与空串视为不同值, 与 C 语义一致
 *
 * 依赖关系:
 *   内部: Testing/TestingMinimal.h
 *
 * 注意事项:
 *   ReportCheckFailure 恒返回 false, 便于 REQUIRE 宏直接以返回值决定是否 return
 *
 ******************************************************************************/

#include "Testing/TestingMinimal.h"

namespace Limx
{
namespace Testing
{

namespace
{

/// 逐字节比较两个 C 字符串是否内容相同 — 不依赖 CRT
bool AreCStringsEqual(const AnsiChar* left, const AnsiChar* right)
{
    if (left == right)
    {
        return true;
    }

    // 只有一侧为空指针时视为不等 (空指针与空串不同)
    if (left == nullptr || right == nullptr)
    {
        return false;
    }

    while (*left != '\0' && *right != '\0')
    {
        if (*left != *right)
        {
            return false;
        }

        ++left;
        ++right;
    }

    return *left == *right;
}

/// 将可能为空的 C 字符串转为可打印形式
const AnsiChar* SafeCString(const AnsiChar* text)
{
    return (text != nullptr) ? text : "<nullptr>";
}

} // namespace

// ============================================================================
// ReportCheckFailure
// ============================================================================

bool ReportCheckFailure(FTestContext& context, const FString& message,
                        const AnsiChar* file, Int32 line, bool isRequired)
{
    context.ReportFailure(file, line, message);

    if (isRequired)
    {
        context.RequestAbort();
    }

    // 恒为 false — REQUIRE 宏据此判定需要提前 return
    return false;
}

// ============================================================================
// CheckStringEqual
// ============================================================================

bool CheckStringEqual(FTestContext& context,
                      const AnsiChar* actual, const AnsiChar* expected,
                      const AnsiChar* actualExpression,
                      const AnsiChar* expectedExpression,
                      const AnsiChar* file, Int32 line, bool isRequired)
{
    if (AreCStringsEqual(actual, expected))
    {
        context.ReportPass();
        return true;
    }

    return ReportCheckFailure(
        context,
        StringFormat("EXPECT_STREQ({}, {}) 失败: 实际 \"{}\", 期望 \"{}\"",
                     actualExpression, expectedExpression,
                     SafeCString(actual), SafeCString(expected)),
        file, line, isRequired);
}

} // namespace Testing
} // namespace Limx
