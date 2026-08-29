// ============================================================
// 文件名称：ApplicationCoreAPI.h
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：DLL 导出宏定义 — 统一 ApplicationCore 模块的符号可见性。
// 功能描述：定义 LIMX_APPLICATIONCORE_API 宏，
//          用于标记需要跨模块公开的类和函数。
// 技术特性：静态库模式下为空宏。
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建                        │
// ============================================================

#pragma once

// 静态库判断必须在最前 —— LBT 对静态库会同时定义 _STATIC 与模块 toml 中
// 声明的 _EXPORTS。若 _EXPORTS 在前, 静态库会被按 dllexport 编译, 进而对
// 含模板成员的导出类触发 C4251, 在 /WX 下变成编译错误。
#if defined(LIMX_APPLICATIONCORE_STATIC)
    #define LIMX_APPLICATIONCORE_API
#elif defined(LIMX_APPLICATIONCORE_EXPORTS)
    #define LIMX_APPLICATIONCORE_API __declspec(dllexport)
#else
    #define LIMX_APPLICATIONCORE_API __declspec(dllimport)
#endif
