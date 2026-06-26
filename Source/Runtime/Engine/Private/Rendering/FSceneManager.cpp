// ============================================================
// 文件名称：FSceneManager.cpp
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：单向同步 — SyncScene 每帧执行 Engine→Luminance 数据同步，
//          只向 FRenderer 写入，不读取渲染状态，保证单向依赖。
// 功能描述：FSceneManager 完整实现 — 单例初始化、SyncScene 渲染数据聚合
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M1.0 Engine 渲染桥接) │
// ============================================================

#include "Engine/EngineMinimal.h"

namespace Limx
{

LIMX_DECLARE_LOG_CATEGORY(LogEngine)

// ============================================================================
// 单例
// ============================================================================

FSceneManager& FSceneManager::Get()
{
    static FSceneManager s_Instance;
    return s_Instance;
}

// ============================================================================
// 生命周期
// ============================================================================

void FSceneManager::Initialize(FRenderer* renderer)
{
    LIMX_CHECK(renderer != nullptr);
    m_Renderer = renderer;
    LIMX_LOG(LogEngine, Log, "[FSceneManager] 初始化完成，绑定 FRenderer");
}

void FSceneManager::Shutdown()
{
    m_Renderer = nullptr;
    m_SceneRenderObjects.Clear();
    LIMX_LOG(LogEngine, Log, "[FSceneManager] 已关闭");
}

// ============================================================================
// SyncScene — 每帧同步 LScene → FRenderer
// ============================================================================

void FSceneManager::SyncScene(const LScene* scene, Float32 deltaTime)
{
    if (m_Renderer == nullptr || scene == nullptr)
    {
        return;
    }

    (void)deltaTime;

    // ---- 1. 重建渲染对象列表 ----
    m_SceneRenderObjects.Clear();

    const TArray<LNode*>& nodes = scene->GetAllNodes();

    for (SizeType i = 0; i < nodes.GetSize(); ++i)
    {
        LNode* node = nodes[i];
        if (node == nullptr || node->IsPendingDestroy())
        {
            continue;
        }

        // 收集所有启用的 LMeshTrait
        LMeshTrait* meshTrait = node->GetTrait<LMeshTrait>();
        if (meshTrait != nullptr && meshTrait->IsEnabled())
        {
            FRenderObject renderObj;
            if (meshTrait->BuildRenderObject(renderObj))
            {
                m_SceneRenderObjects.Add(renderObj);
            }
        }
    }

    // ---- 2. 将渲染对象列表写入 FRenderer ----
    m_Renderer->SetRenderObjects(m_SceneRenderObjects);

    // ---- 3. 找到主相机并更新 FRenderer::m_Camera ----
    bool foundMainCamera = false;
    for (SizeType i = 0; i < nodes.GetSize(); ++i)
    {
        LNode* node = nodes[i];
        if (node == nullptr || node->IsPendingDestroy())
        {
            continue;
        }

        LCameraTrait* camTrait = node->GetTrait<LCameraTrait>();
        if (camTrait != nullptr && camTrait->IsMain() && camTrait->IsEnabled())
        {
            FCamera& cam = m_Renderer->GetCamera();
            camTrait->SetAspectRatio(cam.GetAspectRatio());

            FMatrix view = camTrait->BuildViewMatrix();
            FMatrix proj = camTrait->BuildProjectionMatrix();
            cam.SetExternalMatrices(view, proj, camTrait->GetCameraWorldPosition());
            foundMainCamera = true;
            break;
        }
    }

    if (!foundMainCamera)
    {
        m_Renderer->GetCamera().ClearExternalMatrices();
    }
}

} // namespace Limx
