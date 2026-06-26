// ============================================================
// 文件名称：RenderCoreModule.cpp
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：编译时验证优先 — 通过 static_assert 确认核心类型正确。
// 功能描述：LimxRenderCore 模块入口 — 编译时类型验证。
// 技术特性：static_assert 编译期断言。
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建                        │
// ============================================================

#include "RenderCore/RenderCoreMinimal.h"
#include "RenderCore/Renderer/FRenderContext.h"
#include "RenderCore/Shaders/FShaderManager.h"

namespace Limx
{

namespace
{

static_assert(sizeof(FRenderContextDesc) > 0,
    "FRenderContextDesc 大小必须大于 0");

static_assert(sizeof(FRenderContext) > 0,
    "FRenderContext 大小必须大于 0");

} // anonymous namespace

} // namespace Limx
