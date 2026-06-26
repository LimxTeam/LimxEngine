// ============================================================
// 文件名称：ObjectMacros.h
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：零样板代码 — 通过宏自动生成 RTTI 样板，与 LHT 反射工具链
//          集成，避免手写重复的 StaticType() 和 GetType() 实现。
// 功能描述：LOBJECT_BODY / IMPLEMENT_LTYPE / LCast 宏定义
//          - LOBJECT_BODY(ClassName) : 在 class body 内使用，声明 StaticType()/GetType()
//          - IMPLEMENT_LTYPE(Cls, Parent) : 在 .cpp 内使用，注册类型到 LType 全局表
//          - LCast<T>(ptr) : 安全向下转型，失败返回 nullptr
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M1.0 Object 模块)    │
// ============================================================

#pragma once

#include "Object/LType.h"

namespace Limx
{

// ============================================================================
// LOBJECT_BODY — 在 class body 内使用（public 区域）
// 生成: StaticType() 静态方法 + GetType() 虚函数覆盖
// ============================================================================

#define LOBJECT_BODY(ClassName)                                              \
public:                                                                      \
    /** 返回本类型的 LType 描述符（类级别，静态单例） */                         \
    static  ::Limx::LType* StaticType();                                     \
    /** 返回本实例的实际运行时类型 */                                            \
    ::Limx::LType* GetType() const override { return StaticType(); }         \
    /** 对 placement-new 创建的对象执行本类型析构 */                              \
    void DestroySelf() override { this->~ClassName(); }                      \
private:

// ============================================================================
// IMPLEMENT_LTYPE — 在 .cpp 文件内使用，注册类型到全局 LType 表
// 使用 placement new 在引擎分配器的内存上构造对象（不是裸 new）
// ============================================================================

#define IMPLEMENT_LTYPE(ClassName, ParentClassName)                          \
::Limx::LType* ClassName::StaticType()                                      \
{                                                                            \
    static ::Limx::LType s_Type(                                             \
        FName(#ClassName),                                                   \
        ParentClassName::StaticType(),                                       \
        sizeof(ClassName),                                                   \
        []() -> ::Limx::LObject*                                             \
        {                                                                    \
            void* mem = ::Limx::DefaultAllocator::GetDefault().Allocate(     \
                sizeof(ClassName), LIMX_ALIGNOF(ClassName));                 \
            return ::new(mem) ClassName();                                   \
        }                                                                    \
    );                                                                       \
    static bool s_Registered = false;                                        \
    if (!s_Registered)                                                       \
    {                                                                        \
        s_Registered = true;                                                 \
        ::Limx::LType::Register(&s_Type);                                    \
    }                                                                        \
    return &s_Type;                                                          \
}

// ============================================================================
// IMPLEMENT_LTYPE_ABSTRACT — 抽象类专用，factory 传 nullptr
// 有纯虚函数的类不能实例化，使用此宏代替 IMPLEMENT_LTYPE
// ============================================================================

#define IMPLEMENT_LTYPE_ABSTRACT(ClassName, ParentClassName)                 \
::Limx::LType* ClassName::StaticType()                                      \
{                                                                            \
    static ::Limx::LType s_Type(                                             \
        FName(#ClassName),                                                   \
        ParentClassName::StaticType(),                                       \
        sizeof(ClassName),                                                   \
        nullptr   /* 抽象类：无工厂函数 */                                    \
    );                                                                       \
    static bool s_Registered = false;                                        \
    if (!s_Registered)                                                       \
    {                                                                        \
        s_Registered = true;                                                 \
        ::Limx::LType::Register(&s_Type);                                    \
    }                                                                        \
    return &s_Type;                                                          \
}

// ============================================================================
// LCast<T> — 安全运行时向下转型
// 使用 LType::IsDerivedFrom 检查，失败返回 nullptr（不抛异常）
// ============================================================================

template<typename T, typename SrcT>
FORCEINLINE T* LCast(SrcT* obj)
{
    if (obj == nullptr)
    {
        return nullptr;
    }
    if (obj->GetType()->IsDerivedFrom(T::StaticType()))
    {
        return static_cast<T*>(obj);
    }
    return nullptr;
}

template<typename T, typename SrcT>
FORCEINLINE const T* LCast(const SrcT* obj)
{
    return LCast<T>(const_cast<SrcT*>(obj));
}

} // namespace Limx
