/*******************************************************************************
 * 文件: FTestRegistry.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   全局测试注册表实现 + ITestCase 构造期自动注册
 *   注册表状态为三个具常量初始化的文件作用域静态变量
 *
 * 设计哲学:
 *   常量初始化优先于动态初始化 — g_ListHead/g_ListTail/g_Count 均为
 *   指针与整数的零初始化, C++ 保证其在任何静态对象的构造函数运行之前完成。
 *   因此无论 ITestCase 静态实例分布在多少个翻译单元、链接顺序如何，
 *   Register 被调用时注册表一定处于有效状态。
 *
 * 技术特性:
 *   - 尾插 O(1) 且保持定义顺序, 使报告输出稳定可 diff
 *   - 零堆分配 — 链表节点即 ITestCase 自身
 *
 * 依赖关系:
 *   内部: Testing/TestingMinimal.h
 *
 * 注意事项:
 *   静态初始化在 main 之前单线程完成, 因此 Register 无需加锁
 *
 ******************************************************************************/

#include "Testing/TestingMinimal.h"

namespace Limx
{

namespace
{

// ============================================================================
// 注册表状态
//
// 三者均为常量初始化 (指针置空 / 计数置零), 位于 .bss 段, 在任何动态
// 初始化之前就绪 — 这是本注册表可以被静态构造函数安全调用的根本原因。
// ============================================================================

ITestCase* g_ListHead = nullptr;
ITestCase* g_ListTail = nullptr;
UInt32     g_Count    = 0;

} // namespace

// ============================================================================
// ITestCase — 构造期自动注册
// ============================================================================

ITestCase::ITestCase(const AnsiChar* suite, const AnsiChar* name,
                     const AnsiChar* file, Int32 line)
    : m_Suite(suite)
    , m_Name(name)
    , m_File(file)
    , m_Line(line)
    , m_Next(nullptr)
{
    FTestRegistry::Register(this);
}

// ============================================================================
// FTestRegistry
// ============================================================================

void FTestRegistry::Register(ITestCase* testCase)
{
    if (testCase == nullptr)
    {
        return;
    }

    testCase->SetNext(nullptr);

    if (g_ListTail == nullptr)
    {
        g_ListHead = testCase;
        g_ListTail = testCase;
    }
    else
    {
        g_ListTail->SetNext(testCase);
        g_ListTail = testCase;
    }

    ++g_Count;
}

ITestCase* FTestRegistry::GetHead()
{
    return g_ListHead;
}

UInt32 FTestRegistry::GetCount()
{
    return g_Count;
}

} // namespace Limx
