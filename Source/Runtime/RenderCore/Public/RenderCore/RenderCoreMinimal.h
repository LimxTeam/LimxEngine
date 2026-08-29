// ============================================================
// 文件名称：RenderCoreMinimal.h
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：最小化包含 — RenderCore 模块的预编译头入口,
//          聚合 Platform (Core + RHI) + ApplicationCore 基础设施。
// 功能描述：RenderCore 模块所有源文件的统一包含点。
// 技术特性：PCH 友好 — 作为 RenderCore 模块的预编译头根文件。
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建                        │
// ============================================================

#pragma once

// Platform + Core 完整基础设施
#include "RHI/RHIMinimal.h"

// 本模块的 API 导出宏 —— 公开类需要它标注 DLL 边界。
// 不包含会让 LIMX_xxx_API 在使用处保持未定义, 编译器把它当作普通标识符,
// 错误信息会指向莫名其妙的位置。
#include "RenderCore/RenderCoreAPI.h"

// RHI 公共接口
#include "RHI/RHI/RHIDefinitions.h"
#include "RHI/RHI/RHIResources.h"
#include "RHI/RHI/RHIPipelineState.h"
#include "RHI/RHI/IRHIDevice.h"
#include "RHI/RHI/IRHICommandBuffer.h"
#include "RHI/RHI/RHIFactory.h"

// ApplicationCore 公共接口
#include "ApplicationCore/ApplicationCoreMinimal.h"

// GPU 资源管理
#include "RenderCore/Resources/FRenderResources.h"
#include "RenderCore/Resources/FRenderResourceManager.h"
