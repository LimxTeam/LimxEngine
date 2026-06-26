// ============================================================
// 文件名称：LScene.h
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：场景即容器 — LScene 是 Limx 引擎的顶级运行时上下文，
//          拥有所有 LNode，驱动 Tick 循环，管理生命周期，是 VFX
//          行业中"场景"概念的直接映射。
// 功能描述：LScene — 场景容器，拥有所有 LNode，驱动 Tick/OnBegin/OnEnd
//          生命周期，通过 SpawnNode<T>/RemoveNode 管理节点，通过
//          AddSystem<T>/GetSystem<T> 管理引擎服务。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                     │ 描述                          │
// │──────────────────────────│────────────────────────────│
// │ Create(name)              │ [静态工厂] 创建 LScene        │
// │ SpawnNode<T>(name, xform) │ 创建并注册 LNode              │
// │ RemoveNode(node)          │ 延迟销毁节点                  │
// │ FindNode(name)            │ 按名称查找节点                │
// │ GetNodesOfType(type, out) │ 按类型枚举节点                │
// │ AddSystem<T>(name)        │ 添加引擎服务                  │
// │ GetSystem<T>()            │ 获取引擎服务                  │
// │ OnBegin()                 │ 场景开始播放                  │
// │ Tick(dt)                  │ 驱动所有节点和系统 Tick       │
// │ OnEnd()                   │ 场景停止播放                  │
// │ GetSceneName()            │ 返回场景名称                  │
//
// ── 结构体字段表 ──────────────────────────────────────────────
// │ 字段名           │ 类型                     │ 描述         │
// │─────────────────│─────────────────────────│────────────│
// │ m_SceneName      │ FName                   │ 场景名称     │
// │ m_Nodes          │ TArray<LNode*>          │ 节点数组(拥有)│
// │ m_PendingRemove  │ TArray<LNode*>          │ 待删除节点   │
// │ m_Systems        │ TMap<FName,LSystem*>    │ 服务表(拥有) │
// │ m_HasBegun       │ bool                    │ 是否已播放   │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M1.0 Engine 模块)    │
// ============================================================

#pragma once

#include "Engine/LNode.h"
#include "Engine/LSystem.h"

namespace Limx
{

// ============================================================================
// LScene — 场景容器（顶级运行时上下文）
// ============================================================================

class LIMX_ENGINE_API LScene : public LObject
{
    LOBJECT_BODY(LScene)

public:
    LScene();
    ~LScene() override;

    // ====================================================================
    // 工厂
    // ====================================================================

    /// 创建新场景（通过 LRegistry::Spawn）
    LIMX_NODISCARD static LScene* Create(FName name);

    // ====================================================================
    // 节点生命周期
    // ====================================================================

    /// 创建 T 类型节点并加入场景
    /// @param name      节点调试名称
    /// @param transform 初始变换（应用到 RootSpatial）
    template<typename T>
    T* SpawnNode(FName name, const FTransform& transform = FTransform())
    {
        T* node = LRegistry::Get().Spawn<T>(name);
        LIMX_CHECK(node != nullptr);

        node->InternalSetScene(this);
        m_Nodes.Add(node);
        node->OnAddedToScene(this);

        // 自动创建 LSpatialTrait 作为根变换
        LSpatialTrait* root = node->AddTrait<LSpatialTrait>(FName("RootSpatial"));
        root->SetLocalTransform(transform);
        node->SetRootSpatial(root);

        if (m_HasBegun)
        {
            node->OnBegin();
        }

        return node;
    }

    /// 延迟移除节点（本帧 Tick 结束后执行）
    void RemoveNode(LNode* node);

    // ====================================================================
    // 节点查询
    // ====================================================================

    LIMX_NODISCARD LNode* FindNode(FName name) const;
    void GetNodesOfType(LType* type, TArray<LNode*>& outNodes) const;
    LIMX_NODISCARD const TArray<LNode*>& GetAllNodes() const { return m_Nodes; }
    LIMX_NODISCARD SizeType GetNodeCount() const { return m_Nodes.GetSize(); }

    // ====================================================================
    // 引擎服务（LSystem）
    // ====================================================================

    template<typename T>
    T* AddSystem(FName systemName = FName("System"))
    {
        T* system = LRegistry::Get().Spawn<T>(systemName);
        LIMX_CHECK(system != nullptr);

        system->InternalSetScene(this);
        m_Systems.Add(systemName, system);

        if (m_HasBegun)
        {
            system->OnStart();
        }

        return system;
    }

    template<typename T>
    LIMX_NODISCARD T* GetSystem() const
    {
        for (auto& pair : m_Systems)
        {
            if (pair.Value->IsA<T>())
            {
                return static_cast<T*>(pair.Value);
            }
        }
        return nullptr;
    }

    // ====================================================================
    // 生命周期
    // ====================================================================

    /// 场景开始播放（驱动所有节点和系统 OnBegin/OnStart）
    void OnBegin();

    /// 每帧驱动所有节点 Tick + 系统 Tick，末尾刷新待删除节点
    void Tick(Float32 deltaTime);

    /// 场景停止播放（逆序 OnEnd + 系统 OnStop）
    void OnEnd();

    // ====================================================================
    // 元数据
    // ====================================================================

    LIMX_NODISCARD FName GetSceneName() const { return m_SceneName; }
    void SetSceneName(FName name)              { m_SceneName = name; }

private:
    /// 刷新并销毁待删除节点
    void FlushPendingRemove();

    FName                m_SceneName;
    TArray<LNode*>       m_Nodes;
    TArray<LNode*>       m_PendingRemove;
    TMap<FName, LSystem*> m_Systems;
    bool                 m_HasBegun = false;
};

} // namespace Limx
