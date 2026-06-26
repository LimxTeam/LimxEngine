// ============================================================
// 文件名称：LMeshTrait.h
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：桥接模式 — LMeshTrait 将 Engine 层的节点/变换与 Luminance
//          渲染层的 FRenderObject 解耦，FSceneManager 通过
//          BuildRenderObject() 收集渲染数据，每帧重建渲染列表。
// 功能描述：LMeshTrait — 网格渲染 Trait，持有 GPU 缓冲区句柄和材质引用，
//          通过 BuildRenderObject 输出 FRenderObject 供 FSceneManager 使用。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                       │ 描述                        │
// │────────────────────────────│──────────────────────────│
// │ SetMeshData(vbo,ibo,count)  │ 设置 GPU 缓冲区和索引数量   │
// │ SetMaterial(mat)            │ 设置 PBR 材质               │
// │ SetVisible(bool)            │ 控制可见性                  │
// │ IsVisible()                 │ 当前是否可见                │
// │ BuildRenderObject(out)      │ 填充 FRenderObject 供渲染   │
// │ HasValidMesh()              │ 检查 VBO/IBO 是否有效       │
//
// ── 结构体字段表 ──────────────────────────────────────────────
// │ 字段名           │ 类型                  │ 描述            │
// │─────────────────│──────────────────────│───────────────│
// │ m_VertexBuffer   │ FRHIBufferHandle     │ 顶点缓冲区      │
// │ m_IndexBuffer    │ FRHIBufferHandle     │ 索引缓冲区      │
// │ m_IndexCount     │ UInt32              │ 索引数量        │
// │ m_Material       │ FMaterial*          │ 材质 (非拥有)   │
// │ m_IsVisible      │ bool                │ 可见性标志      │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M1.0 Engine 渲染桥接) │
// ============================================================

#pragma once

#include "Engine/LSpatialTrait.h"
#include "Renderer/Renderer/FRenderer.h"
#include "RenderCore/Material/FMaterial.h"
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
    ~LMeshTrait() override = default;

    // ====================================================================
    // 网格数据
    // ====================================================================

    /// 设置 GPU 缓冲区（由外部创建并传入）
    void SetMeshData(FRHIBufferHandle vertexBuffer,
                     FRHIBufferHandle indexBuffer,
                     UInt32           indexCount);

    LIMX_NODISCARD FRHIBufferHandle GetVertexBuffer() const { return m_VertexBuffer; }
    LIMX_NODISCARD FRHIBufferHandle GetIndexBuffer()  const { return m_IndexBuffer; }
    LIMX_NODISCARD UInt32           GetIndexCount()   const { return m_IndexCount; }

    /// 检查是否有有效的 GPU 缓冲区
    LIMX_NODISCARD bool HasValidMesh() const;

    // ====================================================================
    // 材质
    // ====================================================================

    /// 设置 PBR 材质（非拥有引用）
    void SetMaterial(FMaterial* material) { m_Material = material; }
    LIMX_NODISCARD FMaterial* GetMaterial() const { return m_Material; }

    // ====================================================================
    // 可见性
    // ====================================================================

    void SetVisible(bool visible) { m_IsVisible = visible; }
    LIMX_NODISCARD bool IsVisible() const { return m_IsVisible; }

    // ====================================================================
    // 渲染数据导出
    // ====================================================================

    /// 从当前 Trait 状态填充 FRenderObject，供 FSceneManager 每帧调用
    /// @param outObject  输出的渲染对象
    /// @return 数据有效（有网格和材质）则返回 true
    bool BuildRenderObject(FRenderObject& outObject) const;

    // ====================================================================
    // 生命周期覆盖
    // ====================================================================

    void OnAttached(LNode* owner) override;
    void OnDetached() override;

private:
    FRHIBufferHandle  m_VertexBuffer;
    FRHIBufferHandle  m_IndexBuffer;
    UInt32            m_IndexCount = 0;
    FMaterial*        m_Material   = nullptr;
    bool              m_IsVisible  = true;
};

} // namespace Limx
