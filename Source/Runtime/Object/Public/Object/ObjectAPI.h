// ============================================================
// 文件名称：ObjectAPI.h
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：DLL 边界控制 — 静态库时展开为空，动态库时控制符号导出/导入
// 功能描述：LIMX_OBJECT_API 宏定义，供 Object 模块所有公开类使用
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M1.0 Object 模块)    │
// ============================================================

#pragma once

#if defined(LIMX_OBJECT_STATIC)
    #define LIMX_OBJECT_API
#elif defined(LIMX_OBJECT_EXPORTS)
    #define LIMX_OBJECT_API __declspec(dllexport)
#else
    #define LIMX_OBJECT_API __declspec(dllimport)
#endif
