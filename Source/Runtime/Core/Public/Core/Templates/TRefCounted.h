/*******************************************************************************
 * 文件: TRefCounted.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   引用计数基类 — CRTP 非虚函数侵入式引用计数
 *   子类继承 TRefCounted<T>，获得 AddRef/Release 语义
 *   用于 GPU 资源、资产对象、插件实例等需要手动生命周期管理的对象
 *
 * 设计哲学:
 *   CRTP 无虚表 — 编译时多态，零虚函数开销
 *   侵入式 — 引用计数内嵌于对象，无额外控制块
 *   显式管理 — AddRef/Release 配合 TRefPtr 使用
 *
 * 技术特性:
 *   - TRefCounted<T>: CRTP 引用计数基类
 *   - AddRef/Release: 手动引用计数
 *   - GetRefCount: 查询当前引用数
 *   - TRefPtr<T>: RAII 智能指针封装
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/TypeTraits/TypeTraits.h"
#include "Core/Memory/MemoryOps.h"
#include "Core/Memory/DefaultAllocator.h"

namespace Limx
{

/// CRTP 引用计数基类
/// @tparam DerivedType 派生类型 (CRTP)
template<typename DerivedType>
class TRefCounted
{
public:
    TRefCounted() : m_RefCount(0) {}

    TRefCounted(const TRefCounted&)
        : m_RefCount(0)
    {
    }

    TRefCounted& operator=(const TRefCounted&)
    {
        return *this;
    }

    // ========================================================================
    // 引用计数
    // ========================================================================

    /// 增加引用计数
    void AddRef()
    {
        ++m_RefCount;
    }

    /// 减少引用计数，为零时调用 Delete()
    void Release()
    {
        LIMX_ASSERT(m_RefCount > 0);
        --m_RefCount;
        if (m_RefCount == 0)
        {
            static_cast<DerivedType*>(this)->Delete();
        }
    }

    /// 查询引用计数
    LIMX_NODISCARD UInt32 GetRefCount() const
    {
        return m_RefCount;
    }

    /// 是否唯一持有
    LIMX_NODISCARD bool IsUnique() const
    {
        return m_RefCount == 1;
    }

protected:
    ~TRefCounted() = default;

    /// 默认删除实现 (派生类可重写)
    ///
    /// 走默认分配器, 与 MakeRefCounted 配对。原先是裸 delete, 而项目
    /// 禁止裸 new —— 也就是说当时根本没有合规的方式创建这类对象, 这个
    /// 设施是不可用的。
    ///
    /// 派生类若自行改写 Delete(), 就要自己保证与其分配方式配对。
    void Delete()
    {
        auto* self = static_cast<DerivedType*>(this);

        self->~DerivedType();

        GetDefaultAllocator().Deallocate(self);
    }

private:
    UInt32 m_RefCount;  ///< 引用计数
};

// ============================================================================
// TRefPtr — RAII 智能指针
// ============================================================================

/// 配合 TRefCounted 使用的 RAII 智能指针
/// @tparam T 必须继承 TRefCounted<T>
template<typename T>
class TRefPtr
{
public:
    TRefPtr() : m_Ptr(nullptr) {}

    explicit TRefPtr(T* ptr) : m_Ptr(ptr)
    {
        if (m_Ptr != nullptr)
        {
            m_Ptr->AddRef();
        }
    }

    TRefPtr(const TRefPtr& other) : m_Ptr(other.m_Ptr)
    {
        if (m_Ptr != nullptr)
        {
            m_Ptr->AddRef();
        }
    }

    TRefPtr(TRefPtr&& other) : m_Ptr(other.m_Ptr)
    {
        other.m_Ptr = nullptr;
    }

    ~TRefPtr()
    {
        if (m_Ptr != nullptr)
        {
            m_Ptr->Release();
        }
    }

    TRefPtr& operator=(const TRefPtr& other)
    {
        if (this != &other)
        {
            if (m_Ptr != nullptr)
            {
                m_Ptr->Release();
            }
            m_Ptr = other.m_Ptr;
            if (m_Ptr != nullptr)
            {
                m_Ptr->AddRef();
            }
        }
        return *this;
    }

    TRefPtr& operator=(TRefPtr&& other)
    {
        if (this != &other)
        {
            if (m_Ptr != nullptr)
            {
                m_Ptr->Release();
            }
            m_Ptr = other.m_Ptr;
            other.m_Ptr = nullptr;
        }
        return *this;
    }

    TRefPtr& operator=(T* ptr)
    {
        if (m_Ptr != ptr)
        {
            if (m_Ptr != nullptr)
            {
                m_Ptr->Release();
            }
            m_Ptr = ptr;
            if (m_Ptr != nullptr)
            {
                m_Ptr->AddRef();
            }
        }
        return *this;
    }

    // ========================================================================
    // 访问
    // ========================================================================

    LIMX_NODISCARD T* operator->() const
    {
        LIMX_ASSERT(m_Ptr != nullptr);
        return m_Ptr;
    }

    LIMX_NODISCARD T& operator*() const
    {
        LIMX_ASSERT(m_Ptr != nullptr);
        return *m_Ptr;
    }

    LIMX_NODISCARD T* Get() const { return m_Ptr; }

    LIMX_NODISCARD explicit operator bool() const
    {
        return m_Ptr != nullptr;
    }

    // ========================================================================
    // 管理
    // ========================================================================

    void Reset()
    {
        if (m_Ptr != nullptr)
        {
            m_Ptr->Release();
            m_Ptr = nullptr;
        }
    }

    /// 释放所有权 (不调用 Release)
    LIMX_NODISCARD T* Detach()
    {
        T* ptr = m_Ptr;
        m_Ptr = nullptr;
        return ptr;
    }

    // ========================================================================
    // 比较
    // ========================================================================

    LIMX_NODISCARD bool operator==(
        const TRefPtr& other) const
    {
        return m_Ptr == other.m_Ptr;
    }

    LIMX_NODISCARD bool operator!=(
        const TRefPtr& other) const
    {
        return m_Ptr != other.m_Ptr;
    }

    LIMX_NODISCARD bool operator==(T* ptr) const
    {
        return m_Ptr == ptr;
    }

    LIMX_NODISCARD bool operator!=(T* ptr) const
    {
        return m_Ptr != ptr;
    }

private:
    T* m_Ptr;  ///< 持有的对象指针
};

// ============================================================================
// MakeRefCounted — 工厂函数
// ============================================================================

/// 创建一个引用计数对象, 返回持有其初始引用的 TRefPtr
///
/// 必须走这里而不是自行分配: TRefCounted::Delete() 用默认分配器归还内存,
/// 分配那一侧要是换了地方, 就是从一个堆分配、往另一个堆归还。
///
/// 注意初始引用计数是 1, 且这个引用归返回的 TRefPtr 所有 —— 所以下面
/// 不能再 AddRef, 那会让对象永不释放。
template<typename T, typename... Args>
LIMX_NODISCARD TRefPtr<T> MakeRefCounted(Args&&... args)
{
    IAllocator& allocator = GetDefaultAllocator();

    void* memory = allocator.Allocate(
        sizeof(T),
        alignof(T) > kDefaultAlignment ? alignof(T) : kDefaultAlignment);

    T* object = new (memory) T(Forward<Args>(args)...);

    // TRefPtr 的构造会 AddRef, 而 TRefCounted 的初始计数是 0, 因此
    // 这一步之后计数恰好为 1。
    return TRefPtr<T>(object);
}

} // namespace Limx
