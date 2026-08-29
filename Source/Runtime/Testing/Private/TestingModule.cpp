/*******************************************************************************
 * 文件: TestingModule.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   LimxTesting 模块入口 + 编译时布局验证
 *
 * 设计哲学:
 *   编译期验证 — 测试框架自身的关键不变量若被破坏, 所有测试结果都不可信,
 *   因此把这些约束固化为 static_assert 在构建阶段就拦下。
 *
 * 技术特性:
 *   - 验证追踪分配器头部尺寸不超过默认对齐, 否则头部内联记账方案失效
 *
 * 依赖关系:
 *   内部: Testing/TestingMinimal.h
 *
 ******************************************************************************/

#include "Testing/TestingMinimal.h"

namespace Limx
{

// ============================================================================
// 编译时验证
// ============================================================================

// FTrackingAllocator 将记账头藏在用户指针之前, 要求头部能放进一个对齐单位。
// 若 SizeType 变宽导致头部超过 kDefaultAlignment, 头部会越界写入前一块内存。
static_assert(2 * sizeof(SizeType) <= kDefaultAlignment,
              "FTrackingAllocator 记账头超出默认对齐, 头部内联方案失效");

// FTestContext 依赖 TArray 收集失败, 用例结束即析构, 不得有静态存储期要求
static_assert(sizeof(FTestContext) > 0,
              "FTestContext 必须是完整类型");

// ============================================================================
// API 宏展开校验
//
// 本模块是静态库, API 宏必须展开为空。若 API 头里 _EXPORTS 的判断排在
// _STATIC 之前 (LBT 对静态库两个宏都定义), 宏会展开为 __declspec(dllexport),
// 使含模板成员的导出类触发 C4251 并在 /WX 下变成编译错误。
// 把这一点固化为编译期断言, 让顺序退化在构建阶段就被拦下。
// ============================================================================

static_assert(sizeof(LIMX_STRINGIFY(LIMX_TESTING_API)) == 1,
              "LIMX_TESTING_API 应展开为空 — 检查该模块 API 头中 _STATIC 与 _EXPORTS 的判断顺序");

} // namespace Limx
