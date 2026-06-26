/*******************************************************************************
 * 文件: FScope.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   RAII 作用域守卫 — 通用 defer 机制
 *   在作用域退出时自动执行注册的回调函数
 *   用于资源清理、锁释放、状态恢复等需要保证执行的场景
 *
 * 设计哲学:
 *   零开销抽象 — 编译器可内联消除 TFunction 开销
 *   不可拷贝 — 每个守卫绑定唯一的清理职责
 *   可取消 — 支持 Dismiss() 取消执行
 *
 * 技术特性:
 *   - FScopeGuard: 作用域退出时执行回调
 *   - LIMX_DEFER: 宏糖 — 类似 Go 的 defer 语义
 *   - Dismiss: 取消执行
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Templates/TFunction.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Templates/TFunction.h"

namespace Limx
{

/// 作用域守卫 — RAII 风格的 defer
class FScopeGuard
{
public:
    /// 从可调用对象构造
    explicit FScopeGuard(TFunction<void()> callback)
        : m_Callback(MoveTemp(callback))
        , m_IsActive(true)
    {
    }

    /// 析构时执行回调
    ~FScopeGuard()
    {
        if (m_IsActive)
        {
            m_Callback();
        }
    }

    // 不可拷贝
    FScopeGuard(const FScopeGuard&) = delete;
    FScopeGuard& operator=(const FScopeGuard&) = delete;

    // 可移动
    FScopeGuard(FScopeGuard&& other) noexcept
        : m_Callback(MoveTemp(other.m_Callback))
        , m_IsActive(other.m_IsActive)
    {
        other.m_IsActive = false;
    }

    /// 取消执行 — 守卫析构时不再调用回调
    void Dismiss() { m_IsActive = false; }

    /// 是否仍然活跃
    LIMX_NODISCARD bool IsActive() const { return m_IsActive; }

private:
    TFunction<void()> m_Callback;   ///< 清理回调
    bool              m_IsActive;   ///< 是否活跃
};

/// 轻量模板作用域守卫 — 避免 TFunction 开销
/// 适用于 lambda 直接传入的场景
template<typename Callable>
class TScopeGuard
{
public:
    explicit TScopeGuard(Callable&& callback)
        : m_Callback(MoveTemp(callback))
        , m_IsActive(true)
    {
    }

    ~TScopeGuard()
    {
        if (m_IsActive)
        {
            m_Callback();
        }
    }

    TScopeGuard(const TScopeGuard&) = delete;
    TScopeGuard& operator=(const TScopeGuard&) = delete;

    TScopeGuard(TScopeGuard&& other) noexcept
        : m_Callback(MoveTemp(other.m_Callback))
        , m_IsActive(other.m_IsActive)
    {
        other.m_IsActive = false;
    }

    void Dismiss() { m_IsActive = false; }
    LIMX_NODISCARD bool IsActive() const { return m_IsActive; }

private:
    Callable m_Callback;
    bool     m_IsActive;
};

/// 工厂函数 — 自动推导 lambda 类型
template<typename Callable>
LIMX_NODISCARD TScopeGuard<Callable> MakeScopeGuard(
    Callable&& callback)
{
    return TScopeGuard<Callable>(
        Forward<Callable>(callback));
}

} // namespace Limx

/// LIMX_DEFER 宏 — 类似 Go 的 defer
/// 用法: LIMX_DEFER { cleanup_code; };
#define LIMX_DEFER_CONCAT_IMPL(a, b) a##b
#define LIMX_DEFER_CONCAT(a, b) LIMX_DEFER_CONCAT_IMPL(a, b)
#define LIMX_DEFER \
    auto LIMX_DEFER_CONCAT(_limxDefer_, __LINE__) = \
        ::Limx::MakeScopeGuard([&]()
