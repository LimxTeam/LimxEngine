// ============================================================
// 文件名称：LSystem.h
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：引擎服务分层 — LSystem 将跨节点的全局服务（物理、音频、AI）
//          与场景生命周期绑定，通过 LScene::AddSystem<T> 注册，
//          避免全局单例污染。
// 功能描述：LSystem 基类 — 引擎子系统，挂载在 LScene 上，拥有完整的
//          OnStart/Tick/OnStop 生命周期，适合实现渲染同步、物理世界等。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名       │ 描述                                      │
// │────────────│─────────────────────────────────────────│
// │ GetScene() │ 返回宿主 LScene                           │
// │ OnStart()  │ [纯虚] 场景开始播放时初始化               │
// │ Tick(dt)   │ [纯虚] 每帧调用                          │
// │ OnStop()   │ [纯虚] 场景停止时清理                    │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M1.0 Engine 模块)    │
// ============================================================

#pragma once

#include "Engine/EngineAPI.h"
#include "Object/ObjectMinimal.h"

namespace Limx
{

// 前向声明
class LScene;

// ============================================================================
// LSystem — 场景级引擎服务基类
// ============================================================================

class LIMX_ENGINE_API LSystem : public LObject
{
    LOBJECT_BODY(LSystem)

public:
    LSystem();
    ~LSystem() override = default;

    /// 返回宿主场景
    LIMX_NODISCARD LScene* GetScene() const { return m_Scene; }

    /// 内部接口（仅 LScene 调用）
    void InternalSetScene(LScene* scene) { m_Scene = scene; }

    // ====================================================================
    // 生命周期（纯虚，派生类必须实现）
    // ====================================================================

    virtual void OnStart() = 0;
    virtual void Tick(Float32 deltaTime) = 0;
    virtual void OnStop() = 0;

protected:
    LScene* m_Scene = nullptr;
};

} // namespace Limx
