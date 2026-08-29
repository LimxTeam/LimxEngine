/*******************************************************************************
 * 文件: CoreAPI.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   动态链接库导出/导入宏定义
 *   为每个模块提供统一的 API 可见性控制
 *   Windows 使用 __declspec(dllexport/dllimport)
 *   Linux/macOS 使用 __attribute__((visibility("default")))
 *
 * 设计哲学:
 *   模块编译时定义 LIMX_xxx_EXPORTS 宏，自动切换导出/导入
 *   静态库额外定义 LIMX_xxx_STATIC，该宏优先级最高使 API 展开为空 ——
 *   LBT 对静态库会同时定义 _STATIC 与模块 toml 中声明的 _EXPORTS，
 *   若 _EXPORTS 判断在前，静态库会被错误地按 dllexport 编译，
 *   进而对含模板成员的导出类触发 C4251
 *   静态库编译时所有 API 宏展开为空
 *
 * 技术特性:
 *   - LIMX_CORE_API: Core 模块导出宏
 *   - LIMX_IMPORT / LIMX_EXPORT: 通用导出/导入原语
 *   - 支持 Windows (__declspec) 和 Unix (__attribute__)
 *
 * 依赖关系:
 *   内部: Core/HAL/Platform.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"

// ============================================================================
// 通用导出/导入原语
// ============================================================================

#if LIMX_PLATFORM_WINDOWS
    #define LIMX_DLLEXPORT __declspec(dllexport)
    #define LIMX_DLLIMPORT __declspec(dllimport)
#elif LIMX_COMPILER_GCC || LIMX_COMPILER_CLANG
    #define LIMX_DLLEXPORT __attribute__((visibility("default")))
    #define LIMX_DLLIMPORT __attribute__((visibility("default")))
#else
    #define LIMX_DLLEXPORT
    #define LIMX_DLLIMPORT
#endif

// ============================================================================
// 模块 API 宏
// ============================================================================

// --- LimxCore ---
#if defined(LIMX_CORE_STATIC)
    #define LIMX_CORE_API
#elif defined(LIMX_CORE_EXPORTS)
    #define LIMX_CORE_API LIMX_DLLEXPORT
#else
    #define LIMX_CORE_API LIMX_DLLIMPORT
#endif

// --- LimxRHI (渲染硬件接口) ---
#if defined(LIMX_RHI_STATIC)
    #define LIMX_RHI_API
#elif defined(LIMX_RHI_EXPORTS)
    #define LIMX_RHI_API LIMX_DLLEXPORT
#else
    #define LIMX_RHI_API LIMX_DLLIMPORT
#endif

// --- LimxRenderer (渲染器) ---
#if defined(LIMX_RENDERER_STATIC)
    #define LIMX_RENDERER_API
#elif defined(LIMX_RENDERER_EXPORTS)
    #define LIMX_RENDERER_API LIMX_DLLEXPORT
#else
    #define LIMX_RENDERER_API LIMX_DLLIMPORT
#endif
