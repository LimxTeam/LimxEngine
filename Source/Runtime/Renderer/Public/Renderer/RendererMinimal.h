// ============================================================
// 文件名称：RendererMinimal.h
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：最小化包含 — Renderer 模块的预编译头入口,
//          聚合 RenderCore + RHI + Core 完整类型系统。
// 功能描述：Renderer 模块所有源文件的统一包含点。
//          包含 RenderCore 完整基础设施 (交换链/着色器/相机/材质/光照/几何)。
// 技术特性：PCH 友好 — 作为 Renderer 模块的预编译头根文件。
//
// ── 更新历史 ──────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-06  │ LimxTeam  │ 初始创建 (原名 LuminanceMinimal.h) │
// │ 2026-04-07  │ LimxTeam  │ 重命名 + 拆分 (Luminance → Renderer) │
// ============================================================

#pragma once

// RenderCore 完整基础设施 (含 Platform + Core + RHI + ApplicationCore)
#include "RenderCore/RenderCoreMinimal.h"

// ApplicationCore 公共接口
#include "ApplicationCore/Window/FWindow.h"
#include "ApplicationCore/Input/FInputManager.h"

// RenderCore 公共接口
#include "RenderCore/Renderer/FRenderContext.h"
#include "RenderCore/Shaders/FShaderManager.h"
