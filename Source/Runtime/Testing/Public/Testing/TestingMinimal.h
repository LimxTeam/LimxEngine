/*******************************************************************************
 * 文件: TestingMinimal.h
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   Testing 模块最小包含头 — 预编译头 (PCH) 入口
 *   测试源文件只需 #include "Testing/TestingMinimal.h" 即可获得完整测试设施
 *
 * 设计哲学:
 *   一头文件走天下 — 与 Core/CoreMinimal.h 保持一致的聚合约定
 *   顺序明确 — API 宏 → 用例基类 → 注册表 → 运行器 → 断言宏 → 追踪分配器
 *
 * 包含内容:
 *   Core/CoreMinimal.h        — 完整类型系统
 *   Testing/TestingAPI.h      — 导出宏
 *   Testing/FTestCase.h       — ITestCase / FTestContext
 *   Testing/FTestRegistry.h   — 全局注册表
 *   Testing/FTestRunner.h     — 运行器与运行选项
 *   Testing/TestMacros.h      — LIMX_TEST / LIMX_EXPECT_* / LIMX_REQUIRE_*
 *   Testing/FTrackingAllocator.h — 泄漏追踪分配器
 *
 * 依赖关系:
 *   聚合 Testing 模块全部公开头文件
 *
 ******************************************************************************/

#pragma once

#include "Core/CoreMinimal.h"

#include "Testing/TestingAPI.h"
#include "Testing/FTestCase.h"
#include "Testing/FTestRegistry.h"
#include "Testing/FTestRunner.h"
#include "Testing/TestMacros.h"
#include "Testing/FTrackingAllocator.h"
