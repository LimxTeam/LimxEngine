// ============================================================
// 文件名称：LTrait.h
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：组合优于继承 — LTrait 代表可附加到 LNode 的独立行为单元，
//          类似 ECS 中的 Component，通过 LNode::AddTrait<T> 组合到
//          场景节点上，实现行为复用。
// 功能描述：LTrait 基类 — 持有宿主 LNode 弱引用，提供完整附加/分离/
//          生命周期/Tick 钩子，LOBJECT_BODY 宏生成 RTTI 方法。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名              │ 描述                               │
// │──────────────────│─────────────────────────────────│
// │ GetOwner()        │ 返回宿主 LNode (非拥有)             │
// │ GetTraitName()    │ 返回 Trait 名称                    │
// │ SetEnabled(bool)  │ 启用/禁用此 Trait                  │
// │ IsEnabled()       │ 是否启用                           │
// │ OnAttached(node)  │ [虚] 附加到 LNode 后调用           │
// │ OnDetached()      │ [虚] 从 LNode 分离前调用           │
// │ OnBegin()         │ [虚] 场景开始播放时调用            │
// │ Tick(dt)          │ [虚] 每帧调用（IsEnabled 时）      │
// │ OnEnd()           │ [虚] 场景停止播放时调用            │
//
// ── 结构体字段表 ──────────────────────────────────────────────
// │ 字段名        │ 类型    │ 描述                            │
// │──────────────│────────│───────────────────────────────│
// │ m_Owner      │ LNode* │ 宿主节点 (非拥有弱引用)          │
// │ m_TraitName  │ FName  │ Trait 调试名称                  │
// │ m_IsEnabled  │ bool   │ 是否参与 Tick 和事件响应         │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M1.0 Engine 模块)    │
// ============================================================

#pragma once

#include "Engine/EngineAPI.h"
#include "Object/ObjectMinimal.h"

namespace Limx
{

// 前向声明
class LNode;

// ============================================================================
// LTrait — 可附加行为单元基类
// ============================================================================

class LIMX_ENGINE_API LTrait : public LObject
{
    LOBJECT_BODY(LTrait)

public:
    LTrait();
    ~LTrait() override = default;

    // ====================================================================
    // 标识
    // ====================================================================

    /// 返回宿主 LNode（附加后有效，否则返回 nullptr）
    LIMX_NODISCARD LNode* GetOwner() const { return m_Owner; }

    LIMX_NODISCARD FName GetTraitName() const { return m_TraitName; }
    void SetTraitName(FName name)              { m_TraitName = name; }

    // ====================================================================
    // 启用/禁用
    // ====================================================================

    void SetEnabled(bool enabled) { m_IsEnabled = enabled; }
    LIMX_NODISCARD bool IsEnabled() const { return m_IsEnabled; }

    // ====================================================================
    // 生命周期（派生类覆盖）
    // ====================================================================

    /// 附加到 LNode 后调用（m_Owner 已设置）
    virtual void OnAttached(LNode* owner) { (void)owner; }

    /// 从 LNode 分离前调用（m_Owner 将被清空）
    virtual void OnDetached() {}

    /// 场景开始播放时调用
    virtual void OnBegin() {}

    /// 每帧调用（仅当 IsEnabled() 为 true 时）
    virtual void Tick(Float32 deltaTime) { (void)deltaTime; }

    /// 场景停止播放时调用
    virtual void OnEnd() {}

    // ====================================================================
    // 内部接口（仅 LNode 调用）
    // ====================================================================

    void InternalSetOwner(LNode* owner) { m_Owner = owner; }

protected:
    LNode* m_Owner     = nullptr;
    FName  m_TraitName = FName("Trait");
    bool   m_IsEnabled = true;
};

} // namespace Limx
