/*******************************************************************************
 * 文件: RHITestsMinimal.h
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   RHITests 预编译头 — 聚合测试框架与被测的 RHI 公开设施
 *
 * 设计哲学:
 *   只聚合不依赖 Vulkan 的部分 — 显存子分配算法被刻意设计为与图形 API 无关，
 *   因此测试可执行文件无需创建设备即可完整覆盖它。
 *
 * 依赖关系:
 *   Testing/TestingMinimal.h + RHI 的 API 无关公开头
 *
 ******************************************************************************/

#pragma once

#include "Testing/TestingMinimal.h"

#include "RHI/Memory/FSuballocationRegistry.h"
