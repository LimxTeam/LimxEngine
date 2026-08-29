// ============================================================
// 文件名称：EngineModule.cpp
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：编译期验证 — static_assert 验证关键类尺寸，确保 ABI 一致性。
// 功能描述：LimxEngine 模块入口 + 编译时验证
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M1.0 Engine 模块)    │
// ============================================================

#include "Engine/EngineMinimal.h"

namespace Limx
{

// 日志类别定义 — Engine 模块所有 .cpp 通过 LIMX_DECLARE_LOG_CATEGORY(LogEngine) 引用
LIMX_DEFINE_LOG_CATEGORY(LogEngine)

// LTrait 至少有: LObject(≥28) + 指针(8) + FName(8) + bool(1) = ≥45B
static_assert(sizeof(LTrait) >= 40,
              "LTrait 尺寸不符合预期");

// LNode 至少有: LObject(≥28) + 指针x2(16) + TMap(≥8) + bool(1) = ≥53B
static_assert(sizeof(LNode) >= 40,
              "LNode 尺寸不符合预期");

// ============================================================================
// API 宏展开校验
//
// 本模块是静态库, API 宏必须展开为空。若 API 头里 _EXPORTS 的判断排在
// _STATIC 之前 (LBT 对静态库两个宏都定义), 宏会展开为 __declspec(dllexport),
// 使含模板成员的导出类触发 C4251 并在 /WX 下变成编译错误。
// 把这一点固化为编译期断言, 让顺序退化在构建阶段就被拦下。
// ============================================================================

static_assert(sizeof(LIMX_STRINGIFY(LIMX_ENGINE_API)) == 1,
              "LIMX_ENGINE_API 应展开为空 — 检查该模块 API 头中 _STATIC 与 _EXPORTS 的判断顺序");

} // namespace Limx
