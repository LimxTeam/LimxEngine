// ============================================================
// 文件名称：LSpatialTrait.cpp
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：变换树线性遍历 — GetWorldTransform 沿父链递归合成，
//          链深度通常 <8，性能可接受；未来可缓存脏标记优化。
// 功能描述：LSpatialTrait 完整实现 — 父子层级管理、本地/世界变换
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

IMPLEMENT_LTYPE(LSpatialTrait, LTrait)

// ============================================================================
// 构造 / 析构
// ============================================================================

LSpatialTrait::LSpatialTrait()
    : m_LocalTransform()
    , m_Parent(nullptr)
{
}

LSpatialTrait::~LSpatialTrait()
{
    // 析构时自动从父级移除，避免悬空指针
    if (m_Parent != nullptr)
    {
        DetachFromParent();
    }

    // 子级的所有权归 LNode/LScene, 但它们的 m_Parent 指向本对象。
    // 只清空 m_Children 而不解除子级的反向指针, 就会留下一批 m_Parent 悬垂的
    // Trait —— 它们随后析构时会往已释放的内存里写, 表现为关闭阶段的访问违规。
    // 销毁顺序不该成为正确性的前提, 因此这里主动把反向指针清掉。
    for (SizeType i = 0; i < m_Children.GetSize(); ++i)
    {
        if (m_Children[i] != nullptr)
        {
            m_Children[i]->m_Parent = nullptr;
        }
    }

    m_Children.Clear();
}

// ============================================================================
// 变换操作
// ============================================================================

void LSpatialTrait::SetLocalTransform(const FTransform& t)
{
    m_LocalTransform = t;
}

FTransform LSpatialTrait::GetWorldTransform() const
{
    if (m_Parent == nullptr)
    {
        return m_LocalTransform;
    }
    FTransform parentWorld = m_Parent->GetWorldTransform();
    return parentWorld * m_LocalTransform;
}

void LSpatialTrait::SetWorldLocation(const FVector3& location)
{
    if (m_Parent == nullptr)
    {
        m_LocalTransform.Translation = location;
    }
    else
    {
        FTransform parentWorld = m_Parent->GetWorldTransform();
        // FTransform 无 Inverse()，直接从世界位置减去父级平移后反旋转
        FVector3 relativePos = location - parentWorld.Translation;
        FQuat parentRotInv   = FQuat(parentWorld.Rotation.X,
                                     parentWorld.Rotation.Y,
                                     parentWorld.Rotation.Z,
                                     -parentWorld.Rotation.W);
        m_LocalTransform.Translation = parentRotInv.RotateVector(relativePos);
    }
}

FVector3 LSpatialTrait::GetWorldLocation() const
{
    return GetWorldTransform().Translation;
}

void LSpatialTrait::SetWorldRotation(const FQuat& rotation)
{
    if (m_Parent == nullptr)
    {
        m_LocalTransform.Rotation = rotation;
    }
    else
    {
        FQuat parentRot = m_Parent->GetWorldTransform().Rotation;
        m_LocalTransform.Rotation = parentRot.Inverse() * rotation;
    }
}

FQuat LSpatialTrait::GetWorldRotation() const
{
    return GetWorldTransform().Rotation;
}

void LSpatialTrait::SetLocalLocation(const FVector3& location)
{
    m_LocalTransform.Translation = location;
}

FVector3 LSpatialTrait::GetLocalLocation() const
{
    return m_LocalTransform.Translation;
}

// ============================================================================
// 生命周期
// ============================================================================

void LSpatialTrait::OnAttached(LNode* owner)
{
    LTrait::OnAttached(owner);

    if (owner == nullptr)
    {
        return;
    }

    // 挂到节点的根空间 Trait 之下, 使本地变换成为"相对节点"的变换。
    //
    // 根空间 Trait 自己会走到这里但取到的 root 是 nullptr ——
    // LScene::SpawnNode 先 AddTrait 再 SetRootSpatial, 顺序保证了这一点;
    // 即便顺序改变, AttachTo 也会拒绝把节点挂到自己身上。
    LSpatialTrait* root = owner->GetRootSpatial();

    if (root != nullptr && root != this && m_Parent == nullptr)
    {
        AttachTo(root);
    }
}

void LSpatialTrait::OnDetached()
{
    DetachFromParent();
    LTrait::OnDetached();
}

// ============================================================================
// 层级管理
// ============================================================================

void LSpatialTrait::AttachTo(LSpatialTrait* parent)
{
    if (parent == this || parent == m_Parent)
    {
        return;
    }

    // 先从旧父级分离
    if (m_Parent != nullptr)
    {
        DetachFromParent();
    }

    m_Parent = parent;
    if (m_Parent != nullptr)
    {
        m_Parent->m_Children.Add(this);
    }
}

void LSpatialTrait::DetachFromParent()
{
    if (m_Parent == nullptr)
    {
        return;
    }

    // 从父级子数组中移除 this
    for (SizeType i = 0; i < m_Parent->m_Children.GetSize(); ++i)
    {
        if (m_Parent->m_Children[i] == this)
        {
            m_Parent->m_Children.RemoveAtSwap(i);
            break;
        }
    }
    m_Parent = nullptr;
}

} // namespace Limx
