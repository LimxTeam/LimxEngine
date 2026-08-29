// ============================================================
// 文件名称：LSpatialTrait.h
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：变换树 — LSpatialTrait 通过父子链接构建空间层级，
//          GetWorldTransform() 递归向上合成变换，保证坐标系一致性。
// 功能描述：LSpatialTrait — 具有空间变换的 Trait，提供父子层级、
//          本地变换和世界变换访问接口，是所有有空间位置 Trait 的基类。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                  │ 描述                           │
// │────────────────────────│───────────────────────────────│
// │ GetLocalTransform()    │ 返回本地变换 (相对父级)          │
// │ SetLocalTransform(t)   │ 设置本地变换                   │
// │ GetWorldTransform()    │ 递归计算世界变换                │
// │ SetWorldLocation(loc)  │ 设置世界空间位置               │
// │ GetWorldLocation()     │ 获取世界空间位置               │
// │ SetWorldRotation(rot)  │ 设置世界空间旋转               │
// │ GetWorldRotation()     │ 获取世界空间旋转               │
// │ AttachTo(parent)       │ 附加到父级 LSpatialTrait        │
// │ DetachFromParent()     │ 从父级分离                     │
// │ GetParent()            │ 返回父级 (可为 nullptr)         │
// │ GetChildren()          │ 返回子级数组                   │
//
// ── 结构体字段表 ──────────────────────────────────────────────
// │ 字段名           │ 类型                    │ 描述         │
// │─────────────────│────────────────────────│────────────│
// │ m_LocalTransform │ FTransform             │ 本地变换     │
// │ m_Parent         │ LSpatialTrait*         │ 父级 (非拥有)│
// │ m_Children       │ TArray<LSpatialTrait*> │ 子级数组     │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M1.0 Engine 模块)    │
// ============================================================

#pragma once

#include "Engine/LTrait.h"

namespace Limx
{

// ============================================================================
// LSpatialTrait — 有空间变换的 Trait 基类
// ============================================================================

class LIMX_ENGINE_API LSpatialTrait : public LTrait
{
    LOBJECT_BODY(LSpatialTrait)

public:
    LSpatialTrait();
    ~LSpatialTrait() override;

    // ====================================================================
    // 变换接口
    // ====================================================================

    LIMX_NODISCARD const FTransform& GetLocalTransform() const
    {
        return m_LocalTransform;
    }

    void SetLocalTransform(const FTransform& t);

    /// 递归向上合成世界变换（父级存在时为 parent.World * local）
    LIMX_NODISCARD FTransform GetWorldTransform() const;

    // ====================================================================
    // 便捷世界空间接口
    // ====================================================================

    void SetWorldLocation(const FVector3& location);
    LIMX_NODISCARD FVector3 GetWorldLocation() const;

    void SetWorldRotation(const FQuat& rotation);
    LIMX_NODISCARD FQuat GetWorldRotation() const;

    void SetLocalLocation(const FVector3& location);
    LIMX_NODISCARD FVector3 GetLocalLocation() const;

    // ====================================================================
    // 层级接口
    // ====================================================================

    /// 附加到父级 LSpatialTrait（自动从旧父级分离）
    void AttachTo(LSpatialTrait* parent);

    /// 从当前父级分离，回到根层级
    void DetachFromParent();

    LIMX_NODISCARD LSpatialTrait*                GetParent()   const { return m_Parent; }
    LIMX_NODISCARD const TArray<LSpatialTrait*>& GetChildren() const { return m_Children; }

    // ====================================================================
    // 生命周期覆盖
    // ====================================================================

    /// 附加到节点时自动挂到该节点的根空间 Trait 之下
    ///
    /// 不这样做的话, 节点变换与其上的空间 Trait 是两棵互不相干的树 ——
    /// 节点被摆到某处, 而它的网格/相机仍留在原点, 且没有任何报错。
    void OnAttached(LNode* owner) override;

    /// 从节点分离时脱离层级, 避免留下指向已销毁父级的指针
    void OnDetached() override;

protected:
    FTransform             m_LocalTransform;
    LSpatialTrait*         m_Parent   = nullptr;
    TArray<LSpatialTrait*> m_Children;
};

} // namespace Limx
