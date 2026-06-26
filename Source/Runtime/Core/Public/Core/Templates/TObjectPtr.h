/*******************************************************************************
 * 文件: TObjectPtr.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   弱引用对象指针 — 可空的非拥有指针包装
 *   不参与引用计数或生命周期管理，仅提供有效性检查语义
 *   用于缓存引用、观察者模式、组件间弱关联等场景
 *
 * 设计哲学:
 *   非拥有 — 不控制对象生命周期，类似原始指针但更安全
 *   显式空检查 — 通过 IsValid()/operator bool 显式检查
 *   类型安全 — 模板参数绑定具体类型，支持继承转换
 *
 * 技术特性:
 *   - TObjectPtr<T>: 非拥有对象指针
 *   - Get: 获取原始指针
 *   - IsValid: 有效性检查
 *   - Reset: 重置为空
 *   - operator->/operator*: 解引用
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

/// 非拥有对象指针
/// @tparam T 指向的对象类型
template<typename T>
class TObjectPtr
{
public:
    /// 默认构造 — 空指针
    TObjectPtr() : m_Ptr(nullptr) {}

    /// 从原始指针构造
    explicit TObjectPtr(T* ptr) : m_Ptr(ptr) {}

    /// 空指针构造
    TObjectPtr(decltype(nullptr)) : m_Ptr(nullptr) {}

    /// 从派生类指针构造
    template<typename U>
    TObjectPtr(TObjectPtr<U> other)
        : m_Ptr(other.Get())
    {
    }

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

    /// 重置为新指针
    void Reset(T* ptr = nullptr)
    {
        m_Ptr = ptr;
    }

    /// 空指针赋值
    TObjectPtr& operator=(decltype(nullptr))
    {
        m_Ptr = nullptr;
        return *this;
    }

    /// 原始指针赋值
    TObjectPtr& operator=(T* ptr)
    {
        m_Ptr = ptr;
        return *this;
    }

    /// 派生类指针赋值
    template<typename U>
    TObjectPtr& operator=(TObjectPtr<U> other)
    {
        m_Ptr = other.Get();
        return *this;
    }

    // ========================================================================
    // 比较
    // ========================================================================

    LIMX_NODISCARD bool operator==(
        const TObjectPtr& other) const
    {
        return m_Ptr == other.m_Ptr;
    }

    LIMX_NODISCARD bool operator!=(
        const TObjectPtr& other) const
    {
        return m_Ptr != other.m_Ptr;
    }

    LIMX_NODISCARD bool operator==(decltype(nullptr)) const
    {
        return m_Ptr == nullptr;
    }

    LIMX_NODISCARD bool operator!=(decltype(nullptr)) const
    {
        return m_Ptr != nullptr;
    }

    LIMX_NODISCARD bool operator==(const T* ptr) const
    {
        return m_Ptr == ptr;
    }

    LIMX_NODISCARD bool operator!=(const T* ptr) const
    {
        return m_Ptr != ptr;
    }

private:
    T* m_Ptr;  ///< 原始指针
};

} // namespace Limx
