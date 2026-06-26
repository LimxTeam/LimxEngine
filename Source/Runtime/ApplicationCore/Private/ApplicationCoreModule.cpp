// ============================================================
// 文件名称：ApplicationCoreModule.cpp
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：编译时验证优先 — 通过 static_assert 确认核心类型正确。
// 功能描述：LimxApplicationCore 模块入口 — 编译时类型验证。
// 技术特性：static_assert 编译期断言。
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建                        │
// ============================================================

#include "ApplicationCore/ApplicationCoreMinimal.h"
#include "ApplicationCore/Window/FWindow.h"
#include "ApplicationCore/Input/FInputManager.h"

namespace Limx
{

namespace
{

static_assert(sizeof(FWindowDesc) > 0,
    "FWindowDesc 大小必须大于 0");

static_assert(sizeof(FWindow) > 0,
    "FWindow 大小必须大于 0");

static_assert(sizeof(FInputManager) > 0,
    "FInputManager 大小必须大于 0");

} // anonymous namespace

} // namespace Limx
