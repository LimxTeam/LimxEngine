/*******************************************************************************
 * 文件: TestingAPI.h
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   LIMX_TESTING_API 宏定义 — 供 Testing 模块所有公开类使用
 *
 * 设计哲学:
 *   DLL 边界控制 — 静态库时展开为空，动态库时控制符号导出/导入
 *
 * 依赖关系:
 *   无
 *
 ******************************************************************************/

#pragma once

#if defined(LIMX_TESTING_STATIC)
    #define LIMX_TESTING_API
#elif defined(LIMX_TESTING_EXPORTS)
    #define LIMX_TESTING_API __declspec(dllexport)
#else
    #define LIMX_TESTING_API __declspec(dllimport)
#endif
