// ============================================================
// 文件名称：LType.h
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：轻量 RTTI — 不依赖 C++ typeid/dynamic_cast，完全自主运行时
//          类型系统，支持继承链查询和工厂实例化。
// 功能描述：LType — Limx 引擎运行时类型描述符，记录类型名称、父类型、
//          对象大小和工厂函数，并维护全局类型注册表。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                  │ 描述                           │
// │────────────────────────│───────────────────────────────│
// │ GetName()              │ 返回类型名 (FName)              │
// │ GetParentType()        │ 返回父类型指针 (可为 nullptr)    │
// │ GetObjectSize()        │ 返回实例大小 (sizeof)           │
// │ IsDerivedFrom(other)   │ 检查继承链是否包含 other        │
// │ Instantiate()          │ 通过工厂函数创建实例            │
// │ Register(type)         │ [静态] 注册类型到全局表         │
// │ Find(name)             │ [静态] 按名称查找类型           │
// │ GetAll(out)            │ [静态] 获取所有已注册类型       │
//
// ── 结构体字段表 ──────────────────────────────────────────────
// │ 字段名          │ 类型                │ 描述              │
// │────────────────│────────────────────│──────────────────│
// │ m_Name         │ FName              │ 类型名称           │
// │ m_ParentType   │ LType*             │ 父类型 (nullptr=根)│
// │ m_ObjectSize   │ SizeType           │ sizeof(ClassName) │
// │ m_Factory      │ TFunction<LObject*>│ 实例化工厂函数     │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M1.0 Object 模块)    │
// ============================================================

#pragma once

#include "Object/ObjectAPI.h"
#include "Core/CoreMinimal.h"

namespace Limx
{

// 前向声明
class LObject;

// ============================================================================
// LType — 运行时类型描述符
// ============================================================================

class LIMX_OBJECT_API LType
{
public:
    LIMX_NON_COPYABLE(LType);

    /// 构造类型描述符
    /// @param name       类型名 (如 "LNode")
    /// @param parent     父类型，根类型传 nullptr
    /// @param objectSize sizeof(实际类型)
    /// @param factory    创建实例的工厂函数（返回堆分配的新对象）
    LType(FName               name,
          LType*              parent,
          SizeType            objectSize,
          TFunction<LObject*()> factory);

    ~LType() = default;

    // ====================================================================
    // 查询接口
    // ====================================================================

    /// 返回类型名称
    LIMX_NODISCARD FName GetName() const { return m_Name; }

    /// 返回父类型（根类型返回 nullptr）
    LIMX_NODISCARD LType* GetParentType() const { return m_ParentType; }

    /// 返回对象大小（sizeof）
    LIMX_NODISCARD SizeType GetObjectSize() const { return m_ObjectSize; }

    /// 检查本类型是否派生自 other（含自身）
    /// @param other  目标祖先类型
    LIMX_NODISCARD bool IsDerivedFrom(const LType* other) const;

    // ====================================================================
    // 实例化
    // ====================================================================

    /// 通过工厂函数创建本类型的新实例
    /// 调用者负责生命周期 — 通常通过 LRegistry::Spawn 间接使用
    /// @return 新实例指针，工厂为空时返回 nullptr
    LIMX_NODISCARD LObject* Instantiate() const;

    // ====================================================================
    // 全局类型注册表（静态接口）
    // ====================================================================

    /// 注册类型到全局表（IMPLEMENT_LTYPE 宏自动调用）
    static void Register(LType* type);

    /// 按名称查找已注册类型
    /// @return 找到返回指针，未找到返回 nullptr
    LIMX_NODISCARD static LType* Find(FName name);

    /// 获取所有已注册类型
    static void GetAll(TArray<LType*>& outTypes);

private:
    FName                 m_Name;
    LType*                m_ParentType;
    SizeType              m_ObjectSize;
    TFunction<LObject*()> m_Factory;

    /// 全局类型注册表（懒初始化，线程安全由 C++11 静态初始化保证）
    static TMap<FName, LType*>& GetRegistry();
};

} // namespace Limx
