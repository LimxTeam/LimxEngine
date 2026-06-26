// ============================================================
// 文件名称：LNode.cpp
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：Trait 驱动 — LNode 自身不包含游戏逻辑，只负责 Trait
//          的容器管理和生命周期委托。
// 功能描述：LNode 完整实现 — Trait 管理、生命周期、变换委托
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

IMPLEMENT_LTYPE(LNode, LObject)

// ============================================================================
// 构造 / 析构
// ============================================================================

LNode::LNode()
    : m_Scene(nullptr)
    , m_RootSpatial(nullptr)
    , m_HasBegun(false)
{
}

LNode::~LNode()
{
    // Trait 由 LScene/LRegistry 负责销毁，此处仅清空引用
    m_Traits.Clear();
    m_RootSpatial = nullptr;
}

// ============================================================================
// Trait 管理
// ============================================================================

LTrait* LNode::GetTraitByName(FName name) const
{
    LTrait* const* found = m_Traits.Find(name);
    return (found != nullptr) ? *found : nullptr;
}

void LNode::RemoveTrait(FName traitName)
{
    LTrait** found = m_Traits.Find(traitName);
    if (found == nullptr)
    {
        return;
    }

    LTrait* trait = *found;
    if (m_HasBegun)
    {
        trait->OnEnd();
    }
    trait->OnDetached();
    trait->InternalSetOwner(nullptr);
    m_Traits.Remove(traitName);

    LRegistry::Get().Destroy(trait);
}

void LNode::GetAllTraits(TArray<LTrait*>& outTraits) const
{
    outTraits.Clear();
    for (auto& pair : m_Traits)
    {
        outTraits.Add(pair.Value);
    }
}

// ============================================================================
// 空间变换委托
// ============================================================================

void LNode::SetRootSpatial(LSpatialTrait* spatial)
{
    m_RootSpatial = spatial;
}

FTransform LNode::GetWorldTransform() const
{
    if (m_RootSpatial != nullptr)
    {
        return m_RootSpatial->GetWorldTransform();
    }
    return FTransform();
}

void LNode::SetWorldLocation(const FVector3& loc)
{
    if (m_RootSpatial != nullptr)
    {
        m_RootSpatial->SetWorldLocation(loc);
    }
}

FVector3 LNode::GetWorldLocation() const
{
    if (m_RootSpatial != nullptr)
    {
        return m_RootSpatial->GetWorldLocation();
    }
    return FVector3(0.0f, 0.0f, 0.0f);
}

// ============================================================================
// 生命周期
// ============================================================================

void LNode::OnBegin()
{
    m_HasBegun = true;
    for (auto& pair : m_Traits)
    {
        pair.Value->OnBegin();
    }
}

void LNode::Tick(Float32 deltaTime)
{
    for (auto& pair : m_Traits)
    {
        LTrait* trait = pair.Value;
        if (trait->IsEnabled())
        {
            trait->Tick(deltaTime);
        }
    }
}

void LNode::OnEnd()
{
    m_HasBegun = false;
    for (auto& pair : m_Traits)
    {
        pair.Value->OnEnd();
    }
}

} // namespace Limx
