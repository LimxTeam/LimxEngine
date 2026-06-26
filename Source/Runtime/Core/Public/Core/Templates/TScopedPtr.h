/*******************************************************************************
 * 文件: TScopedPtr.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   作用域指针 — 自动析构的单对象拥有指针
 *   离开作用域时自动通过指定分配器释放对象
 *   与 TUniquePtr 不同，TScopedPtr 不可移动，强制绑定作用域
 *   用于局部临时对象、测试夹具、确保清理等场景
 *
 * 设计哲学:
 *   不可移动 — 比 TUniquePtr 更严格，保证对象仅在当前作用域有效
 *   分配器感知 — 通过默认分配器释放，与引擎内存体系一致
 *   轻量 — 仅包装原始指针 + 析构逻辑
 *
 * 技术特性:
 *   - TScopedPtr<T>: 作用域绑定的拥有指针
 *   - Get: 获取原始指针
 *   - Release: 释放所有权 (不析构)
 *   - Reset: 替换所指对象
 *   - MakeScopedPtr: 工厂函数
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h,
 *          Core/Memory/DefaultAllocator.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Memory/MemoryOps.h"
#include "Core/Memory/IAllocator.h"
#include "Core/Memory/DefaultAllocator.h"

namespace Limx
{

/// 作用域指针 — 不可拷贝/不可移动的拥有指针
/// @tparam T 指向的对象类型
template<typename T>
class TScopedPtr
{
public:
    /// 默认构造 — 空指针
    TScopedPtr() : m_Ptr(nullptr) {}

    /// 从原始指针构造 (获取所有权)
    explicit TScopedPtr(T* ptr) : m_Ptr(ptr) {}

    /// 空指针构造
    TScopedPtr(decltype(nullptr)) : m_Ptr(nullptr) {}

    /// 析构 — 自动销毁所指对象
    ~TScopedPtr()
    {
        DestroyObject();
    }

    // 不可拷贝
    TScopedPtr(const TScopedPtr&) = delete;
    TScopedPtr& operator=(const TScopedPtr&) = delete;

    // 不可移动
    TScopedPtr(TScopedPtr&&) = delete;
    TScopedPtr& operator=(TScopedPtr&&) = delete;

    // ========================================================================
    // 访问
    // ========================================================================

    /// 获取原始指针
    LIMX_NODISCARD T* Get() const { return m_Ptr; }

    /// 解引用
    LIMX_NODISCARD T& operator*() const
    {
        LIMX_ASSERT(m_Ptr != nullptr);
        return *m_Ptr;
    }

    /// 成员访问
    LIMX_NODISCARD T* operator->() const
    {
        LIMX_ASSERT(m_Ptr != nullptr);
        return m_Ptr;
    }

    // ========================================================================
    // 有效性
    // ========================================================================

    /// 是否有效 (非空)
    LIMX_NODISCARD bool IsValid() const
    {
        return m_Ptr != nullptr;
    }

    /// 布尔转换
    LIMX_NODISCARD explicit operator bool() const
    {
        return m_Ptr != nullptr;
    }

    // ========================================================================
    // 修改
    // ========================================================================

    /// 释放所有权 (不析构，返回原始指针)
    LIMX_NODISCARD T* Release()
    {
        T* ptr = m_Ptr;
        m_Ptr = nullptr;
        return ptr;
    }

    /// 替换所指对象 (析构旧对象)
    void Reset(T* newPtr = nullptr)
    {
        if (m_Ptr != newPtr)
        {
            DestroyObject();
            m_Ptr = newPtr;
        }
    }

private:
    /// 销毁所指对象
    void DestroyObject()
    {
        if (m_Ptr != nullptr)
        {
            m_Ptr->~T();
            GetDefaultAllocator().Deallocate(m_Ptr);
            m_Ptr = nullptr;
        }
    }

    T* m_Ptr;  ///< 原始指针
};

/// 工厂函数 — 创建 TScopedPtr
template<typename T, typename... Args>
TScopedPtr<T> MakeScopedPtr(Args&&... args)
{
    void* memory = GetDefaultAllocator().Allocate(
        sizeof(T), alignof(T));
    T* object = new (memory) T(
        static_cast<Args&&>(args)...);
    return TScopedPtr<T>(object);
}

} // namespace Limx
