/*******************************************************************************
 * 文件: EngineTestsMinimal.h
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   EngineTests 预编译头 — 聚合测试框架与被测的 Engine 公开设施
 *
 * 设计哲学:
 *   只覆盖不需要 GPU 的部分 — 场景图、Trait 层级与世界变换合成都是纯 CPU 逻辑,
 *   却恰恰是"物体渲染到了错误位置"和"关闭时崩溃"这两类问题的源头。
 *   把它们与需要设备的渲染路径分开测, 才能在没有显卡的环境里也跑得起来。
 *
 * 依赖关系:
 *   Engine 的公开头 + Testing/TestingMinimal.h
 *
 * 注意事项:
 *   Engine 头必须先行 —— 它引入 windows.h。Testing 与 Core 的部分头在
 *   windows.h 缺席时会自行前向声明 Win32 API, 顺序颠倒会与真正的声明
 *   撞成 "different linkage" 的一片报错。
 *
 ******************************************************************************/

#pragma once

// Engine 头先行, 原因见上方注意事项
#include "Engine/EngineMinimal.h"

#include "Testing/TestingMinimal.h"
