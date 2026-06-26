// ============================================================
// 文件名称：ITickable.h
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：最小接口原则 — ITickable 只声明 Tick 和 IsTickEnabled，
//          避免接口污染，LScene 驱动 Tick 时只依赖此接口。
// 功能描述：ITickable 纯虚接口 — 所有需要每帧更新的对象实现此接口
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M1.0 Engine 模块)    │
// ============================================================

#pragma once

#include "Engine/EngineAPI.h"
#include "Core/CoreMinimal.h"

namespace Limx
{

// ============================================================================
// ITickable — 每帧更新接口
// ============================================================================

class LIMX_ENGINE_API ITickable
{
public:
    virtual ~ITickable() = default;

    /// 每帧更新回调
    /// @param deltaTime 帧间隔时间（秒）
    virtual void Tick(Float32 deltaTime) = 0;

    /// 返回当前是否参与 Tick（可用于暂停/休眠优化）
    virtual bool IsTickEnabled() const { return true; }
};

} // namespace Limx
