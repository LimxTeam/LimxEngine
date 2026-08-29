// ============================================================
// 文件名称：LMeshTrait.h
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：引用而非拥有 — LMeshTrait 只持有资源句柄，GPU 缓冲区的
//          所有权属于 FRenderResourceManager。此前 Trait 直接握着
//          FRHIBufferHandle，等于把资源生命周期分散到了场景图的每个
//          节点上；一旦要加载任意资产，就没有任何一处能回答"这块显存
//          还有人用吗"。改为句柄引用后，答案只有资源管理器一处。
//
//          一个网格按材质切分为多段，因此导出的是渲染批次列表而非
//          单个渲染对象 — Sponza 这类单网格多材质的场景没有别的表达方式。
// 功能描述：LMeshTrait — 网格渲染 Trait，持有网格资源句柄与逐槽位材质，
//          通过 BuildRenderObjects 输出绘制批次供 FSceneManager 使用。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                        │ 描述                        │
// │─────────────────────────────│──────────────────────────│
// │ SetMesh(manager, handle)     │ 绑定资源管理器与网格句柄     │
// │ ClearMesh()                  │ 解除绑定并释放引用          │
// │ GetMeshHandle()              │ 当前网格句柄                │
// │ SetMaterial(mat)             │ 设置默认 PBR 材质           │
// │ SetSectionMaterial(slot,mat) │ 设置指定材质槽位的材质       │
// │ SetVisible(bool)             │ 控制可见性                  │
// │ IsVisible()                  │ 当前是否可见                │
// │ BuildRenderObjects(out)      │ 追加绘制批次供渲染          │
// │ HasValidMesh()               │ 句柄是否指向存活的网格       │
//
// ── 结构体字段表 ──────────────────────────────────────────────
// │ 字段名             │ 类型                     │ 描述          │
// │───────────────────│─────────────────────────│─────────────│
// │ m_ResourceManager  │ FRenderResourceManager*  │ 资源管理器    │
// │ m_MeshHandle       │ FMeshResourceHandle      │ 网格句柄      │
// │ m_Material         │ FMaterial*               │ 默认材质      │
// │ m_SectionMaterials │ TArray<FMaterial*>       │ 逐槽位材质    │
// │ m_IsVisible        │ bool                     │ 可见性标志    │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                             │
// │─────────────│──────────│─────────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M1.0 Engine 渲染桥接)   │
// │ 2026-08-29  │ LimxTeam  │ 改为资源句柄引用, 支持多材质分段   │
// ============================================================

#pragma once

#include "Engine/LSpatialTrait.h"
#include "Renderer/Renderer/FRenderer.h"
#include "RenderCore/Material/FMaterial.h"
#include "RenderCore/Resources/FRenderResourceManager.h"
#include "RHI/RHI/RHIResources.h"

namespace Limx
{

// ============================================================================
// LMeshTrait — 网格渲染 Trait
// ============================================================================

class LIMX_ENGINE_API LMeshTrait : public LSpatialTrait
{
    LOBJECT_BODY(LMeshTrait)

public:
    LMeshTrait();
    ~LMeshTrait() override;

    // ====================================================================
    // 网格资源
    // ====================================================================

    /// 绑定网格资源
    ///
    /// Trait 在此对资源加一次引用, 在 ClearMesh/析构时释放。引用计数归零
    /// 不会立即卸载资源, 由 FRenderResourceManager::CollectUnreferenced 收割。
    /// @param manager 资源管理器 (非拥有, 必须活得比本 Trait 长)
    /// @param handle  网格句柄
    void SetMesh(FRenderResourceManager* manager, FMeshResourceHandle handle);

    /// 解除网格绑定并释放引用
    void ClearMesh();

    LIMX_NODISCARD FMeshResourceHandle GetMeshHandle() const
    {
        return m_MeshHandle;
    }

    LIMX_NODISCARD FRenderResourceManager* GetResourceManager() const
    {
        return m_ResourceManager;
    }

    /// 句柄是否仍指向存活的网格
    LIMX_NODISCARD bool HasValidMesh() const;

    /// 取网格资源 — 句柄失效返回 nullptr
    LIMX_NODISCARD const FMeshResource* GetMeshResource() const;

    // ====================================================================
    // 材质
    // ====================================================================

    /// 设置默认 PBR 材质（非拥有引用）— 用于未指定槽位材质的分段
    void SetMaterial(FMaterial* material) { m_Material = material; }
    LIMX_NODISCARD FMaterial* GetMaterial() const { return m_Material; }

    /// 设置指定材质槽位的材质（非拥有引用）
    ///
    /// 槽位号来自 FMeshSection::MaterialSlot, 即资产自身的材质索引。
    /// 数组按需增长, 未设置的槽位回落到默认材质。
    /// @param slot     材质槽位, 负值直接忽略
    /// @param material 材质, 可为 nullptr 表示清除
    void SetSectionMaterial(Int32 slot, FMaterial* material);

    /// 取指定槽位的材质 — 未设置时返回默认材质
    LIMX_NODISCARD FMaterial* GetSectionMaterial(Int32 slot) const;

    // ====================================================================
    // 可见性
    // ====================================================================

    void SetVisible(bool visible) { m_IsVisible = visible; }
    LIMX_NODISCARD bool IsVisible() const { return m_IsVisible; }

    // ====================================================================
    // 渲染数据导出
    // ====================================================================

    /// 把本 Trait 的全部绘制批次追加到输出列表, 供 FSceneManager 每帧调用
    ///
    /// 追加而非覆写 —— 一个网格可能产出多个批次, 调用方通常在同一个
    /// 列表上遍历整个场景。
    /// @param outObjects 输出列表 (追加)
    /// @return 追加的批次数; 0 表示不可见或资源无效
    UInt32 BuildRenderObjects(TArray<FRenderObject>& outObjects) const;

    // ====================================================================
    // 生命周期覆盖
    // ====================================================================

    void OnAttached(LNode* owner) override;
    void OnDetached() override;

private:
    FRenderResourceManager* m_ResourceManager = nullptr;
    FMeshResourceHandle     m_MeshHandle;
    FMaterial*              m_Material        = nullptr;
    TArray<FMaterial*>      m_SectionMaterials;
    bool                    m_IsVisible       = true;
};

} // namespace Limx
