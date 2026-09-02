// ============================================================
// 文件名称：LMeshTrait.cpp
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：每帧重建绘制批次 — BuildRenderObjects 每帧从当前世界变换和
//          资源管理器中的网格数据重新构造批次列表。不缓存, 因为缓存的
//          失效条件 (变换变化、资源卸载、材质替换) 比重建本身更容易出错。
//
//          包围盒在此从局部空间变换到世界空间 — 变换 8 个角点再求 AABB,
//          而不是直接变换 Min/Max 两点。后者在有旋转时会得到错误的包围盒,
//          剔除阶段会把边缘物体误剔。
// 功能描述：LMeshTrait 完整实现 — 资源句柄绑定、引用计数、绘制批次导出
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                             │
// │─────────────│──────────│─────────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M1.0 Engine 渲染桥接)   │
// │ 2026-08-29  │ LimxTeam  │ 改为资源句柄引用, 支持多材质分段   │
// ============================================================

#include "Engine/EngineMinimal.h"

namespace Limx
{

LIMX_DECLARE_LOG_CATEGORY(LogEngine)

IMPLEMENT_LTYPE(LMeshTrait, LSpatialTrait)

namespace
{

/// 把局部空间包围盒变换到世界空间
///
/// 变换 8 个角点再重新求 AABB。只变换 Min/Max 两点在存在旋转时会算出
/// 一个既不包含物体、也不与之相交的盒子 —— 剔除阶段会把物体整个丢掉。
FBoundingBox TransformBounds(const FBoundingBox& local,
                             const FTransform& transform)
{
    if (!local.IsValid())
    {
        return local;
    }

    FBoundingBox result;

    for (UInt32 corner = 0; corner < 8; ++corner)
    {
        FVector3 point(
            (corner & 1u) != 0u ? local.Max.X : local.Min.X,
            (corner & 2u) != 0u ? local.Max.Y : local.Min.Y,
            (corner & 4u) != 0u ? local.Max.Z : local.Min.Z);

        result = result.ExpandToInclude(transform.TransformPosition(point));
    }

    return result;
}

} // namespace

// ============================================================================
// 构造 / 析构
// ============================================================================

LMeshTrait::LMeshTrait()
    : m_ResourceManager(nullptr)
    , m_Material(nullptr)
    , m_IsVisible(true)
{
}

LMeshTrait::~LMeshTrait()
{
    ClearMesh();
}

// ============================================================================
// 网格资源
// ============================================================================

void LMeshTrait::SetMesh(FRenderResourceManager* manager,
                          FMeshResourceHandle handle)
{
    // 先加新引用再放旧引用 —— 反过来时如果新旧是同一个网格,
    // 释放可能让引用计数瞬间归零, 被并发的收割逻辑卸载掉。
    if (manager != nullptr && handle.IsValid())
    {
        manager->AddMeshReference(handle);
    }

    ClearMesh();

    m_ResourceManager = manager;
    m_MeshHandle      = handle;
}

void LMeshTrait::ClearMesh()
{
    if (m_ResourceManager != nullptr && m_MeshHandle.IsValid())
    {
        m_ResourceManager->ReleaseMeshReference(m_MeshHandle);
    }

    m_ResourceManager = nullptr;
    m_MeshHandle      = FMeshResourceHandle();
}

const FMeshResource* LMeshTrait::GetMeshResource() const
{
    if (m_ResourceManager == nullptr)
    {
        return nullptr;
    }

    return m_ResourceManager->GetMesh(m_MeshHandle);
}

bool LMeshTrait::HasValidMesh() const
{
    const FMeshResource* resource = GetMeshResource();
    return resource != nullptr && resource->IsValid();
}

// ============================================================================
// 材质
// ============================================================================

void LMeshTrait::SetSectionMaterial(Int32 slot, FMaterial* material)
{
    if (slot < 0)
    {
        return;
    }

    SizeType index = static_cast<SizeType>(slot);

    while (m_SectionMaterials.GetSize() <= index)
    {
        m_SectionMaterials.Add(nullptr);
    }

    m_SectionMaterials[index] = material;
}

FMaterial* LMeshTrait::GetSectionMaterial(Int32 slot) const
{
    if (slot < 0)
    {
        return m_Material;
    }

    SizeType index = static_cast<SizeType>(slot);

    if (index >= m_SectionMaterials.GetSize() ||
        m_SectionMaterials[index] == nullptr)
    {
        return m_Material;
    }

    return m_SectionMaterials[index];
}

// ============================================================================
// 渲染批次导出
// ============================================================================

UInt32 LMeshTrait::BuildRenderObjects(TArray<FRenderObject>& outObjects) const
{
    if (!m_IsVisible)
    {
        return 0;
    }

    const FMeshResource* mesh = GetMeshResource();

    if (mesh == nullptr || !mesh->IsValid())
    {
        return 0;
    }

    const FTransform worldTransform = GetWorldTransform();

    const AnsiChar* debugName = mesh->Name.IsEmpty()
                                    ? "LMeshTrait"
                                    : mesh->Name.GetCStr();

    // 分段为空时整个网格作为单一批次 —— 程序化几何体没有材质切分,
    // 但它仍然需要被绘制。
    if (mesh->Sections.GetSize() == 0)
    {
        FMaterial* material = m_Material;

        if (material == nullptr)
        {
            return 0;
        }

        FRenderObject object;
        object.VertexBuffer          = mesh->VertexBuffer;
        object.VertexCount           = mesh->VertexCount;
        object.VertexStride          = mesh->VertexStride;
        object.IndexBuffer           = mesh->IndexBuffer;
        object.IndexOffset           = 0;
        object.IndexCount            = mesh->IndexCount;
        object.MeshletBuffer         = mesh->MeshletBuffer;
        object.MeshletOffset         = 0;
        object.MeshletCount          = mesh->MeshletCount;
        object.MaxMeshletRadius      = mesh->MaxMeshletRadius;
        object.IndexType             = mesh->IndexType;
        object.Transform             = worldTransform;
        object.WorldBounds           = TransformBounds(mesh->Bounds,
                                                       worldTransform);
        object.MaterialDescriptorSet = material->GetDescriptorSet();

        // bindless 下标 —— 材质在首次被收集时注册进全局表。
        //
        // 放在收集阶段而不是材质创建时: 材质可以在没有渲染器的情况下
        // 存在 (比如资产导入还没建 GPU 资源), 而 bindless 表属于渲染器。
        object.BindlessMaterialIndex = material->GetBindlessIndex();
        object.BlendMode             = material->GetParams().GetBlendMode();
        object.IsDoubleSided         = material->IsDoubleSided();
        object.DebugName             = debugName;

        outObjects.Add(object);
        return 1;
    }

    UInt32 emitted = 0;

    for (SizeType i = 0; i < mesh->Sections.GetSize(); ++i)
    {
        const FMeshSection& section = mesh->Sections[i];

        if (section.IndexCount == 0)
        {
            continue;
        }

        FMaterial* material = GetSectionMaterial(section.MaterialSlot);

        if (material == nullptr)
        {
            continue;
        }

        FRenderObject object;
        object.VertexBuffer          = mesh->VertexBuffer;
        object.VertexCount           = mesh->VertexCount;
        object.VertexStride          = mesh->VertexStride;
        object.IndexBuffer           = mesh->IndexBuffer;
        object.IndexOffset           = section.IndexOffset;
        object.IndexCount            = section.IndexCount;
        object.MeshletBuffer         = mesh->MeshletBuffer;
        object.MeshletOffset         = section.MeshletOffset;
        object.MeshletCount          = section.MeshletCount;
        object.MaxMeshletRadius      = section.MaxMeshletRadius;
        object.IndexType             = mesh->IndexType;
        object.Transform             = worldTransform;
        object.WorldBounds           = TransformBounds(
            section.Bounds.IsValid() ? section.Bounds : mesh->Bounds,
            worldTransform);
        object.MaterialDescriptorSet = material->GetDescriptorSet();

        // bindless 下标 —— 材质在首次被收集时注册进全局表。
        //
        // 放在收集阶段而不是材质创建时: 材质可以在没有渲染器的情况下
        // 存在 (比如资产导入还没建 GPU 资源), 而 bindless 表属于渲染器。
        object.BindlessMaterialIndex = material->GetBindlessIndex();
        object.BlendMode             = material->GetParams().GetBlendMode();
        object.IsDoubleSided         = material->IsDoubleSided();
        object.DebugName             = section.Name.IsEmpty()
                                           ? debugName
                                           : section.Name.GetCStr();

        outObjects.Add(object);
        ++emitted;
    }

    return emitted;
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
