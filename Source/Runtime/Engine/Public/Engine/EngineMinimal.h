// ============================================================
// 文件名称：EngineMinimal.h
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：最小包含集 — Engine 模块 PCH 入口，聚合所有 Engine 公开
//          头文件，外部模块只需包含此文件即可使用完整场景图系统。
// 功能描述：Engine 模块统一入口头文件（预编译头）
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M1.0 Engine 模块)    │
// ============================================================

#pragma once

// Windows 头文件必须在 Core/FString.h 之前包含，否则会导致 strncmp/CreateDirectoryA C2375/C2733 冲突
#include "RHI/RHIMinimal.h"

#include "Object/ObjectMinimal.h"

#include "Engine/EngineAPI.h"
#include "Engine/ITickable.h"
#include "Engine/LTrait.h"
#include "Engine/LSpatialTrait.h"
#include "Engine/LNode.h"
#include "Engine/LSystem.h"
#include "Engine/LScene.h"
#include "Engine/Rendering/LMeshTrait.h"
#include "Engine/Rendering/LLightTrait.h"
#include "Engine/Rendering/LCameraTrait.h"
#include "Engine/Rendering/FSceneManager.h"
