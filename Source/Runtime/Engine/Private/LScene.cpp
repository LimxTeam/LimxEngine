// ============================================================
// 文件名称：LScene.cpp
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：延迟销毁 — RemoveNode 将节点推入 m_PendingRemove，
//          FlushPendingRemove 在 Tick 末尾批量销毁，避免在遍历中修改
//          节点数组导致的迭代器失效。
// 功能描述：LScene 完整实现 — 节点生命周期、Tick 驱动、System 管理
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M1.0 Engine 模块)    │
// ============================================================

#include "Engine/EngineMinimal.h"

namespace Limx
{

LIMX_DECLARE_LOG_CATEGORY(LogEngine)

IMPLEMENT_LTYPE(LScene, LObject)

// ============================================================================
// 工厂
// ============================================================================

LScene* LScene::Create(FName name)
{
    LScene* scene = LRegistry::Get().Spawn<LScene>(name);
    LIMX_CHECK(scene != nullptr);
    scene->SetSceneName(name);
    LIMX_LOG(LogEngine, Log, "[LScene] 场景 '{}' 已创建", name.GetCStr());
    return scene;
}

// ============================================================================
// 构造 / 析构
// ============================================================================

LScene::LScene()
    : m_SceneName(FName("Unnamed"))
    , m_HasBegun(false)
{
}

LScene::~LScene()
{
    if (m_HasBegun)
    {
        OnEnd();
    }

    // 销毁所有系统
    for (auto& pair : m_Systems)
    {
        LRegistry::Get().Destroy(pair.Value);
    }
    m_Systems.Clear();

    // 销毁所有节点（及其所有 Trait）
    for (SizeType i = 0; i < m_Nodes.GetSize(); ++i)
    {
        LNode* node = m_Nodes[i];
        // 销毁节点上所有 Trait
        TArray<LTrait*> traits;
        node->GetAllTraits(traits);
        for (SizeType j = 0; j < traits.GetSize(); ++j)
        {
            LRegistry::Get().Destroy(traits[j]);
        }
        LRegistry::Get().Destroy(node);
    }
    m_Nodes.Clear();
    m_PendingRemove.Clear();
}

// ============================================================================
// 节点管理
// ============================================================================

void LScene::RemoveNode(LNode* node)
{
    if (node == nullptr)
    {
        return;
    }
    node->MarkForDestroy();
    m_PendingRemove.Add(node);
}

LNode* LScene::FindNode(FName name) const
{
    for (SizeType i = 0; i < m_Nodes.GetSize(); ++i)
    {
        if (m_Nodes[i]->GetName() == name)
        {
            return m_Nodes[i];
        }
    }
    return nullptr;
}

void LScene::GetNodesOfType(LType* type, TArray<LNode*>& outNodes) const
{
    outNodes.Clear();
    for (SizeType i = 0; i < m_Nodes.GetSize(); ++i)
    {
        if (m_Nodes[i]->GetType()->IsDerivedFrom(type))
        {
            outNodes.Add(m_Nodes[i]);
        }
    }
}

// ============================================================================
// 生命周期
// ============================================================================

void LScene::OnBegin()
{
    if (m_HasBegun)
    {
        return;
    }

    m_HasBegun = true;

    // 启动所有 System
    for (auto& pair : m_Systems)
    {
        pair.Value->OnStart();
    }

    // 启动所有 Node
    for (SizeType i = 0; i < m_Nodes.GetSize(); ++i)
    {
        m_Nodes[i]->OnBegin();
    }

    LIMX_LOG(LogEngine, Log, "[LScene] 场景 '{}' 开始播放", m_SceneName.GetCStr());
}

void LScene::Tick(Float32 deltaTime)
{
    if (!m_HasBegun)
    {
        return;
    }

    // Tick 所有非待删除节点
    for (SizeType i = 0; i < m_Nodes.GetSize(); ++i)
    {
        LNode* node = m_Nodes[i];
        if (!node->IsPendingDestroy())
        {
            node->Tick(deltaTime);
        }
    }

    // Tick 所有 System
    for (auto& pair : m_Systems)
    {
        pair.Value->Tick(deltaTime);
    }

    // 刷新待删除节点
    FlushPendingRemove();
}

void LScene::OnEnd()
{
    if (!m_HasBegun)
    {
        return;
    }

    m_HasBegun = false;

    // 逆序结束 Node（先后加入的后结束）
    for (SizeType i = m_Nodes.GetSize(); i > 0; --i)
    {
        m_Nodes[i - 1]->OnEnd();
    }

    // 逆序停止 System
    for (auto& pair : m_Systems)
    {
        pair.Value->OnStop();
    }

    LIMX_LOG(LogEngine, Log, "[LScene] 场景 '{}' 停止播放", m_SceneName.GetCStr());
}

// ============================================================================
// FlushPendingRemove — 批量销毁待删除节点
// ============================================================================

void LScene::FlushPendingRemove()
{
    if (m_PendingRemove.IsEmpty())
    {
        return;
    }

    for (SizeType i = 0; i < m_PendingRemove.GetSize(); ++i)
    {
        LNode* node = m_PendingRemove[i];

        // 从主数组移除
        for (SizeType j = 0; j < m_Nodes.GetSize(); ++j)
        {
            if (m_Nodes[j] == node)
            {
                m_Nodes.RemoveAtSwap(j);
                break;
            }
        }

        node->OnRemoved();

        // 销毁所有 Trait
        TArray<LTrait*> traits;
        node->GetAllTraits(traits);
        for (SizeType j = 0; j < traits.GetSize(); ++j)
        {
            LRegistry::Get().Destroy(traits[j]);
        }

        LRegistry::Get().Destroy(node);
    }

    m_PendingRemove.Clear();
}

} // namespace Limx
