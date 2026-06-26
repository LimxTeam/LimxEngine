// ============================================================
// 文件名称：LaunchMinimal.h
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：最小化包含 — Launch 模块的预编译头入口,
//          聚合 Luminance 模块完整类型系统。
// 功能描述：Launch 模块所有源文件的统一包含点。
// 技术特性：PCH 友好 — 作为 Launch 模块的预编译头根文件。
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建                        │
// ============================================================

#pragma once

// Luminance 完整渲染基础设施 (含 RenderCore + ApplicationCore + Platform + Core)
#include "Renderer/RendererMinimal.h"

// Luminance 公共头文件
#include "ApplicationCore/Window/FWindow.h"
#include "RenderCore/Renderer/FRenderContext.h"
#include "Renderer/Renderer/FRenderer.h"

// Engine 模块 — M1.0 对象系统 + 场景图 + 渲染桥接
#include "Engine/EngineMinimal.h"
