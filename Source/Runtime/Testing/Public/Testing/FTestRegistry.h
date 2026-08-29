/*******************************************************************************
 * 文件: FTestRegistry.h
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   全局测试用例注册表 — 维护进程内所有 ITestCase 的侵入式单向链表
 *   ITestCase 构造时调用 Register 自动入表，注册顺序即链接顺序
 *
 * 设计哲学:
 *   零动态初始化 — 注册表状态是三个具有常量初始化的文件作用域静态变量
 *   (头指针/尾指针/计数)。C++ 保证它们在任何动态初始化之前完成零初始化，
 *   因此无论 ITestCase 静态实例在哪个翻译单元、以何种顺序构造，注册都安全。
 *
 *   尾插保序 — 维护尾指针使注册为 O(1) 且保持定义顺序，让测试报告的输出
 *   顺序稳定可复现，便于 diff 两次运行的结果。
 *
 * 技术特性:
 *   - Register/GetHead/GetCount 均为 O(1)
 *   - 无堆分配 — 链表节点即 ITestCase 自身
 *   - 无锁 — 静态初始化在 main 之前单线程完成，运行期只读
 *
 * 依赖关系:
 *   内部: Testing/FTestCase.h, Testing/TestingAPI.h
 *
 * 注意事项:
 *   注册表不拥有用例所有权 — ITestCase 实例具静态存储期，进程退出时自动销毁
 *   运行期禁止再注册 — Register 仅应由静态实例的构造函数调用
 *
 ******************************************************************************/

#pragma once

#include "Testing/FTestCase.h"
#include "Testing/TestingAPI.h"

namespace Limx
{

// ============================================================================
// FTestRegistry — 全局用例注册表
// ============================================================================

/// 测试用例注册表 — 全静态接口，无实例
class LIMX_TESTING_API FTestRegistry
{
public:
    // 纯静态工具类, 禁止实例化
    FTestRegistry()                                = delete;
    ~FTestRegistry()                               = delete;
    FTestRegistry(const FTestRegistry&)            = delete;
    FTestRegistry& operator=(const FTestRegistry&) = delete;

    /// 将用例追加到注册链表尾部 — 由 ITestCase 构造函数调用
    /// @param testCase 待注册用例, 必须具有静态存储期
    static void Register(ITestCase* testCase);

    /// 注册链表头 — 遍历入口, 无用例时返回 nullptr
    LIMX_NODISCARD static ITestCase* GetHead();

    /// 已注册用例总数
    LIMX_NODISCARD static UInt32 GetCount();
};

} // namespace Limx
