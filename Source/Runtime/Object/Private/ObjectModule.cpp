// ============================================================
// 文件名称：ObjectModule.cpp
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：编译期验证 — 通过 static_assert 确保关键对象尺寸符合预期，
//          防止跨模块 ABI 不兼容问题。
// 功能描述：LimxObject 模块入口 + 编译时尺寸验证
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M1.0 Object 模块)    │
// ============================================================

#include "Object/ObjectMinimal.h"

namespace Limx
{

// 日志类别定义 — Object 模块所有 .cpp 通过 LIMX_DECLARE_LOG_CATEGORY(LogObject) 引用
LIMX_DEFINE_LOG_CATEGORY(LogObject)

// ============================================================================
// 编译时验证
// ============================================================================

// EObjectFlags 应能容纳至少 8 个标志位
static_assert(sizeof(EObjectFlags) == 4,
              "EObjectFlags 必须为 4 字节 (UInt32)");

// LObject 应包含 FGuid (16B) + FName (8B) + EObjectFlags (4B) = 至少 28B
static_assert(sizeof(LObject) >= 28,
              "LObject 尺寸不符合预期，请检查成员布局");

} // namespace Limx
