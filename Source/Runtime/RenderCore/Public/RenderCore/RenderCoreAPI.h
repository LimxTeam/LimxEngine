// ============================================================
// 文件名称：RenderCoreAPI.h
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：DLL 导出宏定义 — 统一 RenderCore 模块的符号可见性。
// 功能描述：定义 LIMX_RENDERCORE_API 宏。
// 技术特性：静态库模式下为空宏。
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建                        │
// ============================================================

#pragma once

#ifdef LIMX_RENDERCORE_EXPORTS
    #define LIMX_RENDERCORE_API __declspec(dllexport)
#else
    #define LIMX_RENDERCORE_API __declspec(dllimport)
#endif
