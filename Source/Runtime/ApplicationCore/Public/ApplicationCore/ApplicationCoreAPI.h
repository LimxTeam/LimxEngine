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

#ifdef LIMX_APPLICATIONCORE_EXPORTS
    #define LIMX_APPLICATIONCORE_API __declspec(dllexport)
#else
    #define LIMX_APPLICATIONCORE_API __declspec(dllimport)
#endif
