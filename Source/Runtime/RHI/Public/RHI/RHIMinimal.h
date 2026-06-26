/*******************************************************************************
 * 文件: RHIMinimal.h
 * 创建时间: 2025-07-27
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   RHI 模块最小包含头文件 — 预编译头 (PCH) 的推荐入口
 *   包含 Core 完整类型系统 + RHI 基础定义
 *   RHI 模块内所有源文件只需 #include "RHI/RHIMinimal.h"
 *
 * 设计哲学:
 *   一头文件走天下 — 业务代码的唯一必须包含点
 *   PCH 友好 — 作为预编译头的根文件，加速增量编译
 *
 * 包含内容:
 *   Core/CoreMinimal.h  — 完整 Core 类型系统 + 反射宏
 *   RHI 基础定义        — 渲染硬件接口枚举与类型
 *
 * 依赖关系:
 *   外部: LimxCore
 *
 * 更新历史:
 *   2025-07-27 — 初始创建 (原名 PlatformMinimal.h)
 *   2026-04-07 — 重命名为 RHIMinimal.h (Platform → RHI 模块重命名)
 *
 ******************************************************************************/

#pragma once

// Windows 系统头文件 — 必须在 Core 头文件之前包含
// Vulkan SDK 会间接引入 <windows.h>，若 Core 头文件的前向声明
// 先于真实 Windows 声明被编译，将导致类型/签名冲突 (C2733/C4273)。
// 在 PCH 中预先包含 Windows 头文件可彻底避免此问题。
#if defined(_WIN32) || defined(_WIN64)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// 取消 Windows 宏污染 — 防止与引擎标识符冲突
#ifdef Yield
#undef Yield
#endif
#ifdef CreateFile
#undef CreateFile
#endif
#ifdef DeleteFile
#undef DeleteFile
#endif
#ifdef CreateDirectory
#undef CreateDirectory
#endif
#ifdef RemoveDirectory
#undef RemoveDirectory
#endif
#ifdef GetFileAttributes
#undef GetFileAttributes
#endif
#ifdef GetObject
#undef GetObject
#endif
#ifdef GetClassName
#undef GetClassName
#endif
#ifdef LoadLibrary
#undef LoadLibrary
#endif
#ifdef FreeLibrary
#undef FreeLibrary
#endif
#endif

// Core 完整类型系统
#include "Core/CoreMinimal.h"
