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
    void Delete()
    {
        delete static_cast<DerivedType*>(this);
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

} // namespace Limx
