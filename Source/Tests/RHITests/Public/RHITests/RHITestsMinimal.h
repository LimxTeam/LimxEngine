/*******************************************************************************
 * 文件: RHITestsMinimal.h
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   RHITests 预编译头 — 聚合测试框架与被测的 RHI 公开设施
 *
 * 设计哲学:
 *   两类用例共存 — 显存子分配算法被刻意设计为与图形 API 无关, 无需设备即可
 *   完整覆盖; 而同步语义 (管线屏障) 只有在真实 VkDevice 上才谈得上验证,
 *   因此本头文件同时聚合 RHI 的设备工厂与命令录制接口。
 *
 *   RHI/RHIMinimal.h 必须排在最前 — 它在 Core 头文件之前引入 <windows.h>。
 *   反过来的顺序会让 Core 里对 Win32 函数的前向声明先于真实声明被编译,
 *   触发 C2373/C4273 (而 /WX 会把后者变成错误)。
 *
 * 依赖关系:
 *   Testing/TestingMinimal.h + RHI 公开头 (含设备工厂与命令缓冲区接口)
 *
 ******************************************************************************/

#pragma once

// 必须最先 — 见上方设计哲学中关于 <windows.h> 顺序的说明
#include "RHI/RHIMinimal.h"

#include "Testing/TestingMinimal.h"

#include "RHI/Memory/FSuballocationRegistry.h"
#include "RHI/RHI/RHIDefinitions.h"
#include "RHI/RHI/RHIResources.h"
#include "RHI/RHI/IRHIDevice.h"
#include "RHI/RHI/IRHICommandBuffer.h"
#include "RHI/RHI/RHIFactory.h"
