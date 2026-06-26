// ============================================================
// 文件名称：LObject.h
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：万物之基 — 所有 Limx 引擎对象均继承自 LObject，通过
//          LType RTTI 系统支持运行时类型检查和安全转型，通过
//          LRegistry 追踪全局生命周期。
// 功能描述：LObject 基础对象类 — 持有 FGuid 唯一标识、FName 名称、
//          LType* 运行时类型和 EObjectFlags 状态标志，提供完整
//          生命周期虚函数钩子和 IsA<T>/LCast 类型安全方法。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名              │ 描述                                 │
// │──────────────────│────────────────────────────────────│
// │ GetObjectId()    │ 返回 FGuid 唯一标识                   │
// │ GetName()        │ 返回对象名称 (FName)                  │
// │ SetName(name)    │ 设置对象名称                          │
// │ GetType()        │ 返回运行时 LType 描述符               │
// │ StaticType()     │ [静态] 返回 LObject 本身的 LType      │
// │ IsA<T>()         │ 判断是否为 T 类型或其子类             │
// │ HasFlag(flag)    │ 检查对象标志位                        │
// │ SetFlag(flag)    │ 设置对象标志位                        │
// │ ClearFlag(flag)  │ 清除对象标志位                        │
// │ MarkForDestroy() │ 标记为待销毁                          │
// │ IsPendingDestroy()│ 检查是否待销毁                       │
// │ OnCreated()      │ [虚] 对象创建后回调                   │
// │ OnLoaded()       │ [虚] 从磁盘加载完成后回调             │
// │ OnDestroying()   │ [虚] 开始销毁前回调                   │
// │ OnDestroyed()    │ [虚] 析构前最后回调                   │
//
// ── 结构体字段表 ──────────────────────────────────────────────
// │ 字段名        │ 类型          │ 描述                       │
// │──────────────│──────────────│──────────────────────────│
// │ m_ObjectId   │ FGuid         │ 全局唯一标识               │
// │ m_Name       │ FName         │ 对象调试名称               │
// │ m_Flags      │ EObjectFlags  │ 运行时状态位掩码           │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M1.0 Object 模块)    │
// ============================================================

#pragma once

#include "Object/ObjectAPI.h"
#include "Object/LObjectFlags.h"
#include "Object/LType.h"

namespace Limx
{

// ============================================================================
// LObject — 所有 Limx 引擎对象的基类
// ============================================================================

class LIMX_OBJECT_API LObject
{
public:
    /// 禁止外部直接构造 LObject 及其派生类，使用 LRegistry::Spawn<T>
    LObject();
    virtual ~LObject();

    /// 对 placement-new 创建的对象执行实际运行时类型析构。
    /// LRegistry::Destroy 调用该入口后再交给引擎分配器释放内存。
    virtual void DestroySelf();

    // ====================================================================
    // RTTI — 运行时类型系统
    // ====================================================================

    /// 返回 LObject 自身的 LType 描述符（根类型，无父类型）
    static LType* StaticType();

    /// 返回当前实例的运行时 LType（派生类通过 LOBJECT_BODY 覆盖）
    LIMX_NODISCARD virtual LType* GetType() const { return StaticType(); }

    /// 检查当前对象是否为类型 T 或其子类的实例
    template<typename T>
    LIMX_NODISCARD bool IsA() const
    {
        return GetType()->IsDerivedFrom(T::StaticType());
    }

    // ====================================================================
    // 标识
    // ====================================================================

    /// 返回全局唯一标识（FGuid，创建时自动生成）
    LIMX_NODISCARD const FGuid& GetObjectId() const { return m_ObjectId; }

    /// 返回对象调试名称
    LIMX_NODISCARD FName GetName() const { return m_Name; }

    /// 设置对象调试名称
    void SetName(FName name) { m_Name = name; }

    // ====================================================================
    // 标志位
    // ====================================================================

    LIMX_NODISCARD EObjectFlags GetFlags()          const { return m_Flags; }
    LIMX_NODISCARD bool HasFlag(EObjectFlags flag)  const;
    void SetFlag(EObjectFlags flag);
    void ClearFlag(EObjectFlags flag);

    /// 标记为待销毁（下一帧或显式 Destroy 调用时释放）
    void MarkForDestroy();

    /// 返回是否已被标记为待销毁
    LIMX_NODISCARD bool IsPendingDestroy() const;

    // ====================================================================
    // 生命周期钩子（派生类覆盖）
    // ====================================================================

    /// LRegistry::Spawn 创建后调用
    virtual void OnCreated()    {}

    /// 从磁盘反序列化完成后调用
    virtual void OnLoaded()     {}

    /// 开始销毁前调用（此时对象仍有效）
    virtual void OnDestroying() {}

    /// 析构前最后调用（此时对象即将释放）
    virtual void OnDestroyed()  {}

private:
    FGuid       m_ObjectId;
    FName       m_Name;
    EObjectFlags m_Flags;
};

} // namespace Limx
