// ============================================================
// 文件名称：LRegistry.cpp
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：统一生命周期控制 — Destroy 是销毁对象的唯一合法路径，
//          确保 OnDestroying/OnDestroyed 回调正确触发，内存通过
//          引擎分配器回收。
// 功能描述：LRegistry 完整实现 — 单例、Spawn/Destroy、Add/Remove、Find/GetAll
//
// ── 函数表 ──────────────────────────────────────────────────
// │ 函数名          │ 描述                                   │
// │──────────────│─────────────────────────────────────  │
// │ Get()         │ 返回全局单例                            │
// │ Destroy(obj)  │ 安全销毁: 回调 → 析构 → 释放内存        │
// │ Find(guid)    │ 按 GUID 查找                           │
// │ GetAll(out)   │ 快照所有活跃对象                        │
// │ GetCount()    │ 返回对象数量                           │
// │ Add(obj)      │ 注册弱引用                             │
// │ Remove(obj)   │ 注销弱引用                             │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M1.0 Object 模块)    │
// ============================================================

#include "Object/ObjectMinimal.h"

namespace Limx
{

LIMX_DECLARE_LOG_CATEGORY(LogObject)

// ============================================================================
// 单例
// ============================================================================

LRegistry& LRegistry::Get()
{
    static LRegistry s_Instance;
    return s_Instance;
}

// ============================================================================
// Destroy — 唯一合法销毁路径
// ============================================================================

void LRegistry::Destroy(LObject* obj)
{
    if (obj == nullptr)
    {
        return;
    }

    // 1. 通知对象即将销毁
    obj->OnDestroying();

    // 2. 从注册表移除（析构函数中也会调用 Remove，但提前移除更安全）
    m_Objects.Remove(obj->GetObjectId());

    // 3. 最后回调
    obj->OnDestroyed();

    // 4. 通过虚拟销毁入口调用真实运行时类型析构
    obj->DestroySelf();

    // 5. 通过引擎分配器释放内存
    DefaultAllocator::GetDefault().Deallocate(obj);
}

// ============================================================================
// 查询
// ============================================================================

LObject* LRegistry::Find(const FGuid& id) const
{
    LObject* const* found = m_Objects.Find(id);
    return (found != nullptr) ? *found : nullptr;
}

void LRegistry::GetAll(TArray<LObject*>& outObjects) const
{
    outObjects.Reserve(m_Objects.GetSize());
    outObjects.Clear();
    for (auto& pair : m_Objects)
    {
        outObjects.Add(pair.Value);
    }
}

SizeType LRegistry::GetCount() const
{
    return m_Objects.GetSize();
}

// ============================================================================
// Add / Remove
// ============================================================================

void LRegistry::Add(LObject* obj)
{
    LIMX_CHECK(obj != nullptr);
    m_Objects.Add(obj->GetObjectId(), obj);
}

void LRegistry::Remove(LObject* obj)
{
    if (obj == nullptr)
    {
        return;
    }
    m_Objects.Remove(obj->GetObjectId());
}

} // namespace Limx
