// ============================================================
// 文件名称：ObjectMinimal.h
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：最小包含集 — 作为 Object 模块的 PCH 入口，聚合所有
//          Object 层公开头文件，外部模块只需包含此文件即可使用
//          完整对象系统。
// 功能描述：Object 模块统一入口头文件 (预编译头)
//          包含: ObjectAPI / LObjectFlags / LType / LObject /
//                LRegistry / ObjectMacros (含 LOBJECT_BODY 等宏)
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M1.0 Object 模块)    │
// ============================================================

#pragma once

#include "Core/CoreMinimal.h"

#include "Object/ObjectAPI.h"
#include "Object/LObjectFlags.h"
#include "Object/LType.h"
#include "Object/LObject.h"
#include "Object/LRegistry.h"
#include "Object/ObjectMacros.h"
