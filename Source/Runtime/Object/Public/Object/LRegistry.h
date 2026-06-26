// ============================================================
// 文件名称：LRegistry.h
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：中央生命周期管理 — LRegistry 是所有 LObject 的出生地和
//          归宿，通过 Spawn/Destroy 统一控制对象生命周期，避免裸
//          new/delete 散落在业务代码中。
// 功能描述：LRegistry 全局对象注册表单例 — 持有所有活跃 LObject 的
//          弱引用索引（不拥有内存），提供 Spawn<T> 工厂、Destroy
//          安全销毁、Find/GetAll 查询接口。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名              │ 描述                                │
// │──────────────────│───────────────────────────────────│
// │ Get()            │ 获取单例引用                         │
// │ Spawn<T>(name)   │ 创建并注册 T 类型对象                │
// │ Destroy(obj)     │ 安全销毁并注销对象                   │
// │ Find(guid)       │ 按 GUID 查找对象                    │
// │ GetAll(out)      │ 获取所有活跃对象                     │
// │ GetCount()       │ 当前对象数量                        │
// │ Add(obj)         │ 注册已有对象（不创建）               │
// │ Remove(obj)      │ 注销对象引用（不销毁）               │
//
// ── 结构体字段表 ──────────────────────────────────────────────
// │ 字段名        │ 类型                  │ 描述              │
// │──────────────│──────────────────────│──────────────────│
// │ m_Objects    │ TMap<FGuid, LObject*> │ 弱引用注册表      │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M1.0 Object 模块)    │
// ============================================================

#pragma once

#include "Object/ObjectAPI.h"
#include "Object/LObject.h"
#include "Object/ObjectMacros.h"
#include "Core/Memory/DefaultAllocator.h"

namespace Limx
{

// ============================================================================
// LRegistry — 全局对象生命周期管理器（单例）
// ============================================================================

class LIMX_OBJECT_API LRegistry
{
public:
    LIMX_NON_COPYABLE(LRegistry);

    /// 获取全局单例
    static LRegistry& Get();

    // ====================================================================
    // 对象工厂
    // ====================================================================

    /// 创建 T 类型对象并注册到注册表
    /// 内部使用 FDefaultAllocator + placement new，不是裸 new
    /// @param name  对象调试名称
    /// @return 新对象指针，T 必须继承 LObject 且有默认构造函数
    template<typename T>
    LIMX_NODISCARD T* Spawn(FName name = FName("Unnamed"))
    {
        static_assert(
            true, // IsBaseOf<LObject,T> — 依赖 TypeTraits
            "T must derive from LObject"
        );

        // 引擎分配器分配内存
        void* mem = DefaultAllocator::GetDefault().Allocate(
            sizeof(T), LIMX_ALIGNOF(T));

        LIMX_CHECK(mem != nullptr);

        // placement new 在引擎内存上构造对象
        T* obj = ::new(mem) T();

        obj->SetName(name);
        obj->OnCreated();

        return obj;
    }

    /// 安全销毁对象：OnDestroying → OnDestroyed → 析构 → 释放内存
    void Destroy(LObject* obj);

    // ====================================================================
    // 查询接口
    // ====================================================================

    /// 按 FGuid 查找对象，未找到返回 nullptr
    LIMX_NODISCARD LObject* Find(const FGuid& id) const;

    /// 获取所有活跃对象的快照
    void GetAll(TArray<LObject*>& outObjects) const;

    /// 当前活跃对象数量
    LIMX_NODISCARD SizeType GetCount() const;

    // ====================================================================
    // 注册/注销（通常由 LObject 构造/析构自动调用）
    // ====================================================================

    /// 将已存在的 LObject 加入注册表（不创建内存）
    void Add(LObject* obj);

    /// 从注册表移除引用（不销毁内存）
    void Remove(LObject* obj);

private:
    LRegistry()  = default;
    ~LRegistry() = default;

    TMap<FGuid, LObject*> m_Objects;
};

} // namespace Limx
