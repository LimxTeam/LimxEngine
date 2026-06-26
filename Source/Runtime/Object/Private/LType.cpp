// ============================================================
// 文件名称：LType.cpp
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：懒初始化注册表 — 全局 TMap 通过函数内静态变量保证
//          C++11 线程安全初始化，无静态初始化顺序问题。
// 功能描述：LType 完整实现 — 类型注册/查找/继承链检查/工厂调用
//
// ── 函数表 ──────────────────────────────────────────────────
// │ 函数名                  │ 描述                           │
// │────────────────────────│───────────────────────────────│
// │ LType()                │ 构造，暂不自动注册               │
// │ IsDerivedFrom(other)   │ 沿 m_ParentType 链向上查找      │
// │ Instantiate()          │ 调用 m_Factory 创建实例         │
// │ Register(type)         │ 写入全局注册表                  │
// │ Find(name)             │ 从全局注册表按名查找            │
// │ GetAll(out)            │ 枚举所有注册类型                │
// │ GetRegistry()          │ 返回全局注册表引用（懒初始化）   │
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
// 构造
// ============================================================================

LType::LType(FName                  name,
             LType*                 parent,
             SizeType               objectSize,
             TFunction<LObject*()>  factory)
    : m_Name(name)
    , m_ParentType(parent)
    , m_ObjectSize(objectSize)
    , m_Factory(MoveTemp(factory))
{
}

// ============================================================================
// IsDerivedFrom — 沿继承链向上查找
// ============================================================================

bool LType::IsDerivedFrom(const LType* other) const
{
    if (other == nullptr)
    {
        return false;
    }

    const LType* current = this;
    while (current != nullptr)
    {
        if (current == other)
        {
            return true;
        }
        current = current->m_ParentType;
    }
    return false;
}

// ============================================================================
// Instantiate — 通过工厂函数创建实例
// ============================================================================

LObject* LType::Instantiate() const
{
    if (!m_Factory)
    {
        LIMX_LOG(LogObject, Error,
                 "[LType] 类型 '{}' 没有注册工厂函数，无法实例化",
                 m_Name.GetCStr());
        return nullptr;
    }
    return m_Factory();
}

// ============================================================================
// 全局注册表
// ============================================================================

TMap<FName, LType*>& LType::GetRegistry()
{
    // 函数内静态变量：C++11 保证线程安全初始化，无静态初始化顺序问题
    static TMap<FName, LType*> s_Registry;
    return s_Registry;
}

void LType::Register(LType* type)
{
    LIMX_CHECK(type != nullptr);
    LIMX_CHECK(!type->m_Name.IsEmpty());

    TMap<FName, LType*>& reg = GetRegistry();

    if (reg.Contains(type->m_Name))
    {
        LIMX_LOG(LogObject, Warning,
                 "[LType] 类型 '{}' 已注册，跳过重复注册",
                 type->m_Name.GetCStr());
        return;
    }

    reg.Add(type->m_Name, type);

    LIMX_LOG(LogObject, Log,
             "[LType] 注册类型: {} (size={} bytes)",
             type->m_Name.GetCStr(), type->m_ObjectSize);
}

LType* LType::Find(FName name)
{
    LType** found = GetRegistry().Find(name);
    return (found != nullptr) ? *found : nullptr;
}

void LType::GetAll(TArray<LType*>& outTypes)
{
    outTypes.Clear();
    for (auto& pair : GetRegistry())
    {
        outTypes.Add(pair.Value);
    }
}

} // namespace Limx
