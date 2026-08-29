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

// ============================================================================
// API 宏展开校验
//
// 本模块是静态库, API 宏必须展开为空。若 API 头里 _EXPORTS 的判断排在
// _STATIC 之前 (LBT 对静态库两个宏都定义), 宏会展开为 __declspec(dllexport),
// 使含模板成员的导出类触发 C4251 并在 /WX 下变成编译错误。
// 把这一点固化为编译期断言, 让顺序退化在构建阶段就被拦下。
// ============================================================================

static_assert(sizeof(LIMX_STRINGIFY(LIMX_APPLICATIONCORE_API)) == 1,
              "LIMX_APPLICATIONCORE_API 应展开为空 — 检查该模块 API 头中 _STATIC 与 _EXPORTS 的判断顺序");

} // namespace Limx
