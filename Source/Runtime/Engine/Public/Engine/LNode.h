// ============================================================
// 文件名称：LNode.h
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：场景图节点 — LNode 是场景的基本实体单元，通过 AddTrait<T>
//          组合行为，不直接继承游戏逻辑，保持架构清晰。
// 功能描述：LNode — 场景节点，持有 Trait 集合和 RootSpatial 变换根，
//          完整生命周期钩子 (OnAddedToScene/OnBegin/Tick/OnEnd/OnRemoved)
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                     │ 描述                          │
// │──────────────────────────│────────────────────────────│
// │ AddTrait<T>(name)         │ 创建并附加 T 类型 Trait       │
// │ GetTrait<T>()             │ 按类型查找 Trait              │
// │ GetTraitByName(name)      │ 按名称查找 Trait              │
// │ RemoveTrait(name)         │ 分离并销毁指定 Trait          │
// │ GetAllTraits(out)         │ 获取所有 Trait 快照           │
// │ SetRootSpatial(spatial)   │ 设置根空间 Trait              │
// │ GetRootSpatial()          │ 获取根空间 Trait              │
// │ GetWorldTransform()       │ 委托给 RootSpatial            │
// │ SetWorldLocation(loc)     │ 委托给 RootSpatial            │
// │ GetWorldLocation()        │ 委托给 RootSpatial            │
// │ GetScene()                │ 返回所属 LScene               │
// │ OnAddedToScene(scene)     │ [虚] 加入 LScene 后调用      │
// │ OnBegin()                 │ [虚] 场景开始播放             │
// │ Tick(dt)                  │ [虚] 每帧 (传递给所有 Trait)  │
// │ OnEnd()                   │ [虚] 场景停止                 │
// │ OnRemoved()               │ [虚] 从 LScene 移除后调用    │
//
// ── 结构体字段表 ──────────────────────────────────────────────
// │ 字段名          │ 类型                          │ 描述     │
// │────────────────│──────────────────────────────│────────│
// │ m_Scene         │ LScene*                      │ 所属场景 │
// │ m_RootSpatial   │ LSpatialTrait*               │ 根变换   │
// │ m_Traits        │ TMap<FName, LTrait*>         │ Trait表 │
// │ m_HasBegun      │ bool                         │ 是否已播放│
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M1.0 Engine 模块)    │
// ============================================================

#pragma once

#include "Engine/LSpatialTrait.h"

namespace Limx
{

// 前向声明
class LScene;

// ============================================================================
// LNode — 场景图节点
// ============================================================================

class LIMX_ENGINE_API LNode : public LObject
{
    LOBJECT_BODY(LNode)

public:
    LNode();
    ~LNode() override;

    // ====================================================================
    // Trait 管理
    // ====================================================================

    /// 创建 T 类型 Trait 并附加到本节点
    /// T 必须继承 LTrait
    template<typename T>
    T* AddTrait(FName traitName = FName("Trait"))
    {
        T* trait = LRegistry::Get().Spawn<T>(traitName);
        LIMX_CHECK(trait != nullptr);

        trait->SetTraitName(traitName);
        trait->InternalSetOwner(this);
        m_Traits.Add(traitName, trait);
        trait->OnAttached(this);

        if (m_HasBegun)
        {
            trait->OnBegin();
        }

        return trait;
    }

    /// 按类型查找第一个匹配的 Trait
    template<typename T>
    LIMX_NODISCARD T* GetTrait() const
    {
        for (auto& pair : m_Traits)
        {
            if (pair.Value->IsA<T>())
            {
                return static_cast<T*>(pair.Value);
            }
        }
        return nullptr;
    }

    /// 按名称查找 Trait
    LIMX_NODISCARD LTrait* GetTraitByName(FName name) const;

    /// 分离并销毁指定名称的 Trait
    void RemoveTrait(FName traitName);

    /// 获取所有 Trait 快照
    void GetAllTraits(TArray<LTrait*>& outTraits) const;

    // ====================================================================
    // 空间变换（委托给 RootSpatial）
    // ====================================================================

    void SetRootSpatial(LSpatialTrait* spatial);
    LIMX_NODISCARD LSpatialTrait* GetRootSpatial() const { return m_RootSpatial; }

    LIMX_NODISCARD FTransform GetWorldTransform() const;
    void                      SetWorldLocation(const FVector3& loc);
    LIMX_NODISCARD FVector3   GetWorldLocation() const;

    // ====================================================================
    // 场景访问
    // ====================================================================

    LIMX_NODISCARD LScene* GetScene() const { return m_Scene; }

    /// 内部接口（仅 LScene 调用）
    void InternalSetScene(LScene* scene) { m_Scene = scene; }

    /// 从 Trait 表中移除一条记录, 不做销毁 (仅 LTrait 析构时调用)
    ///
    /// 存在的理由: LRegistry::Destroy 可以直接销毁一个仍被节点持有的 Trait,
    /// 此时节点表里会留下悬垂指针, 场景销毁时二次释放。让 Trait 在析构中
    /// 主动摘掉自己, 销毁顺序就不再是正确性的前提。
    void InternalForgetTrait(const LTrait* trait);

    // ====================================================================
    // 生命周期（派生类覆盖）
    // ====================================================================

    /// 加入 LScene 后调用
    virtual void OnAddedToScene(LScene* scene) { (void)scene; }

    /// 场景开始播放时调用（OnBegin 所有 Trait）
    virtual void OnBegin();

    /// 每帧调用（Tick 所有启用的 Trait）
    virtual void Tick(Float32 deltaTime);

    /// 场景停止播放时调用
    virtual void OnEnd();

    /// 从 LScene 移除后调用
    virtual void OnRemoved() {}

protected:
    LScene*               m_Scene       = nullptr;
    LSpatialTrait*        m_RootSpatial = nullptr;
    TMap<FName, LTrait*>  m_Traits;
    bool                  m_HasBegun    = false;
};

} // namespace Limx
