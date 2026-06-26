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

} // namespace Limx
