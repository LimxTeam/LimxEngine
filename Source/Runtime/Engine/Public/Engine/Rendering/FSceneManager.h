// ============================================================
// 文件名称：FSceneManager.h
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：单向数据流 — FSceneManager 是 Engine→Luminance 的单向桥梁，
//          每帧从 LScene 收集渲染数据推送到 FRenderer，不持有双向引用，
//          保证渲染层与场景层的单向依赖。
// 功能描述：FSceneManager 渲染桥接单例 — 每帧调用 SyncScene() 遍历
//          LScene 所有 LNode，收集 LMeshTrait/LCameraTrait 数据，
//          更新 FRenderer 的 m_RenderObjects 和 FCamera。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                    │ 描述                           │
// │─────────────────────────│───────────────────────────────│
// │ Get()                   │ 获取单例                        │
// │ Initialize(renderer)    │ 绑定 FRenderer 实例             │
// │ Shutdown()              │ 解绑并清理                      │
// │ SyncScene(scene, dt)    │ 每帧同步场景数据到渲染器         │
// │ IsInitialized()         │ 是否已初始化                    │
//
// ── 结构体字段表 ──────────────────────────────────────────────
// │ 字段名                │ 类型                   │ 描述      │
// │──────────────────────│───────────────────────│─────────│
// │ m_Renderer           │ FRenderer*             │ 渲染器引用│
// │ m_SceneRenderObjects │ TArray<FRenderObject>  │ 每帧缓存  │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M1.0 Engine 渲染桥接) │
// ============================================================

#pragma once

#include "Engine/EngineAPI.h"
#include "Engine/LScene.h"
#include "Engine/Rendering/LMeshTrait.h"
#include "Engine/Rendering/LCameraTrait.h"
#include "Renderer/Renderer/FRenderer.h"

namespace Limx
{

// ============================================================================
// FSceneManager — Engine→Luminance 渲染桥接单例
// ============================================================================

class LIMX_ENGINE_API FSceneManager
{
public:
    LIMX_NON_COPYABLE(FSceneManager);

    /// 获取全局单例
    static FSceneManager& Get();

    // ====================================================================
    // 生命周期
    // ====================================================================

    /// 绑定 FRenderer — 必须在 FRenderer::Initialize 之后调用
    void Initialize(FRenderer* renderer);

    /// 解绑并清理缓存
    void Shutdown();

    LIMX_NODISCARD bool IsInitialized() const { return m_Renderer != nullptr; }

    // ====================================================================
    // 每帧同步
    // ====================================================================

    /// 遍历 LScene 所有 LNode，收集渲染数据并推送到 FRenderer
    /// - 重建 m_SceneRenderObjects（LMeshTrait 数据）
    /// - 找到 main LCameraTrait 并更新 FRenderer::m_Camera
    void SyncScene(const LScene* scene, Float32 deltaTime);

private:
    FSceneManager()  = default;
    ~FSceneManager() = default;

    FRenderer*           m_Renderer = nullptr;
    TArray<FRenderObject> m_SceneRenderObjects;
};

} // namespace Limx
