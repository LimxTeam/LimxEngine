// ============================================================
// 文件名称：LMeshTrait.cpp
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：每帧重建渲染对象 — BuildRenderObject 每帧调用，从当前
//          世界变换和材质快照填充 FRenderObject，无持久 GPU 句柄缓存。
// 功能描述：LMeshTrait 完整实现 — 网格数据设置、渲染对象构建
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

IMPLEMENT_LTYPE(LMeshTrait, LSpatialTrait)

// ============================================================================
// 构造
// ============================================================================

LMeshTrait::LMeshTrait()
    : m_IndexCount(0)
    , m_Material(nullptr)
    , m_IsVisible(true)
{
}

// ============================================================================
// 网格数据
// ============================================================================

void LMeshTrait::SetMeshData(FRHIBufferHandle vertexBuffer,
                              FRHIBufferHandle indexBuffer,
                              UInt32           indexCount)
{
    m_VertexBuffer = vertexBuffer;
    m_IndexBuffer  = indexBuffer;
    m_IndexCount   = indexCount;
}

bool LMeshTrait::HasValidMesh() const
{
    return m_VertexBuffer.IsValid() &&
           m_IndexBuffer.IsValid() &&
           m_IndexCount > 0;
}

// ============================================================================
// 渲染对象构建
// ============================================================================

bool LMeshTrait::BuildRenderObject(FRenderObject& outObject) const
{
    if (!m_IsVisible || !HasValidMesh() || m_Material == nullptr)
    {
        return false;
    }

    outObject.VertexBuffer           = m_VertexBuffer;
    outObject.IndexBuffer            = m_IndexBuffer;
    outObject.IndexCount             = m_IndexCount;
    outObject.Transform              = GetWorldTransform();
    outObject.MaterialDescriptorSet  = m_Material->GetDescriptorSet();
    outObject.IsAnimated             = false;
    outObject.RotationSpeed          = 0.0f;
    outObject.DebugName              = GetName().IsEmpty()
                                        ? "LMeshTrait"
                                        : GetName().GetCStr();

    return true;
}

// ============================================================================
// 生命周期
// ============================================================================

void LMeshTrait::OnAttached(LNode* owner)
{
    LSpatialTrait::OnAttached(owner);
    LIMX_LOG(LogEngine, Log,
             "[LMeshTrait] 附加到节点 '{}'",
             owner ? owner->GetName().GetCStr() : FString("null"));
}

void LMeshTrait::OnDetached()
{
    LSpatialTrait::OnDetached();
}

} // namespace Limx
