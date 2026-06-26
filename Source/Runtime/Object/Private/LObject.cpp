// ============================================================
// 文件名称：LObject.cpp
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：自动生命周期追踪 — 构造时注册到 LRegistry，析构时
//          自动注销，确保注册表始终反映真实活跃对象集合。
// 功能描述：LObject 完整实现 — 构造/析构自动注册注销、标志位操作、
//          LObject::StaticType 根类型注册
//
// ── 函数表 ──────────────────────────────────────────────────
// │ 函数名            │ 描述                                 │
// │────────────────│─────────────────────────────────────│
// │ LObject()      │ 生成 GUID，注册到 LRegistry           │
// │ ~LObject()     │ 从 LRegistry 注销                    │
// │ StaticType()   │ 返回 LObject 根类型描述符              │
// │ HasFlag(flag)  │ 位与检查                              │
// │ SetFlag(flag)  │ 位或设置                              │
// │ ClearFlag(flag)│ 位与非清除                            │
// │ MarkForDestroy │ 设置 PendingDestroy 标志              │
// │ IsPendingDestroy│ 检查 PendingDestroy 标志             │
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
// LObject::StaticType — 根类型（无父类型）
// ============================================================================

LType* LObject::StaticType()
{
    // 根类型：父类型为 nullptr，无工厂函数（通过 LRegistry::Spawn 创建）
    static LType s_Type(
        FName("LObject"),
        nullptr,
        sizeof(LObject),
        nullptr   // 抽象根类型，不直接实例化
    );
    static bool s_Registered = false;
    if (!s_Registered)
    {
        s_Registered = true;
        LType::Register(&s_Type);
    }
    return &s_Type;
}

// ============================================================================
// 构造 / 析构
// ============================================================================

LObject::LObject()
    : m_ObjectId(FGuid::NewGuid())
    , m_Name(FName("Unnamed"))
    , m_Flags(EObjectFlags::None)
{
    // 构造时自动注册到全局 LRegistry（弱引用）
    LRegistry::Get().Add(this);
}

LObject::~LObject()
{
    // 析构时自动从 LRegistry 移除引用（不销毁内存，内存由调用方管理）
    LRegistry::Get().Remove(this);
}

void LObject::DestroySelf()
{
    this->~LObject();
}

// ============================================================================
// 标志位操作
// ============================================================================

bool LObject::HasFlag(EObjectFlags flag) const
{
    return (m_Flags & flag) != EObjectFlags::None;
}

void LObject::SetFlag(EObjectFlags flag)
{
    m_Flags = m_Flags | flag;
}

void LObject::ClearFlag(EObjectFlags flag)
{
    m_Flags = m_Flags & ~flag;
}

void LObject::MarkForDestroy()
{
    SetFlag(EObjectFlags::PendingDestroy);
}

bool LObject::IsPendingDestroy() const
{
    return HasFlag(EObjectFlags::PendingDestroy);
}

} // namespace Limx
