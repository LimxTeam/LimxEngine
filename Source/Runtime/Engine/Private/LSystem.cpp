// ============================================================
// 文件名称：LSystem.cpp
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：最小基类 — LSystem 仅持有 LScene* 引用和注册逻辑，
//          所有业务行为由派生类通过 OnStart/Tick/OnStop 实现。
// 功能描述：LSystem 完整实现 + IMPLEMENT_LTYPE 注册
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M1.0 Engine 模块)    │
// ============================================================

#include "Engine/EngineMinimal.h"

namespace Limx
{

IMPLEMENT_LTYPE_ABSTRACT(LSystem, LObject)

LSystem::LSystem()
    : m_Scene(nullptr)
{
}

} // namespace Limx
