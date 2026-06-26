/*******************************************************************************
 * 文件: TCallback.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   轻量回调 — 函数指针 + 用户上下文的极简回调封装
 *   比 TFunction 更轻量，无堆分配，无类型擦除开销
 *   用于 C 风格回调接口、中断处理、底层事件回调等场景
 *
 * 设计哲学:
 *   极简零开销 — 仅存储函数指针和 void* 上下文
 *   可空 — 默认构造为空回调，调用前需检查
 *   POD 兼容 — 可安全 memcpy，可放入共享内存
 *
 * 技术特性:
 *   - TCallback<Ret(Args...)>: 带签名的回调
 *   - Bind: 绑定函数指针和上下文
 *   - Invoke: 调用 (断言非空)
 *   - TryInvoke: 安全调用 (空时返回默认值)
 *   - IsValid: 是否已绑定
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

// 前向声明
template<typename Signature>
class TCallback;

/// 轻量回调 — 函数指针 + 上下文
/// @tparam Ret 返回类型
/// @tparam Args 参数类型
template<typename Ret, typename... Args>
class TCallback<Ret(Args...)>
{
public:
    /// 回调函数签名 — 第一个参数为用户上下文
    using FunctionType = Ret(*)(void* context, Args...);

    /// 默认构造 — 空回调
    constexpr TCallback()
        : m_Function(nullptr)
        , m_Context(nullptr)
    {
    }

    /// 从函数指针和上下文构造
    constexpr TCallback(FunctionType function, void* context = nullptr)
        : m_Function(function)
        , m_Context(context)
    {
    }

    /// 绑定函数指针和上下文
    void Bind(FunctionType function, void* context = nullptr)
    {
        m_Function = function;
        m_Context = context;
    }

    /// 解除绑定
    void Unbind()
    {
        m_Function = nullptr;
        m_Context = nullptr;
    }

    /// 调用 (断言非空)
    Ret Invoke(Args... args) const
    {
        LIMX_ASSERT(m_Function != nullptr);
        return m_Function(m_Context, args...);
    }

    /// 调用操作符
    Ret operator()(Args... args) const
    {
        return Invoke(args...);
    }

    /// 是否已绑定
    LIMX_NODISCARD constexpr bool IsValid() const
    {
        return m_Function != nullptr;
    }

    /// 布尔转换
    LIMX_NODISCARD constexpr explicit operator bool() const
    {
        return IsValid();
    }

    /// 获取函数指针
    LIMX_NODISCARD constexpr FunctionType GetFunction() const
    {
        return m_Function;
    }

    /// 获取上下文
    LIMX_NODISCARD constexpr void* GetContext() const
    {
        return m_Context;
    }

    /// 比较
    LIMX_NODISCARD constexpr bool operator==(
        const TCallback& other) const
    {
        return m_Function == other.m_Function &&
               m_Context == other.m_Context;
    }

    LIMX_NODISCARD constexpr bool operator!=(
        const TCallback& other) const
    {
        return !(*this == other);
    }

private:
    FunctionType m_Function;  ///< 回调函数指针
    void*        m_Context;   ///< 用户上下文
};

/// void 返回值特化的安全调用辅助
template<typename... Args>
void TryInvoke(const TCallback<void(Args...)>& callback,
                Args... args)
{
    if (callback.IsValid())
    {
        callback.Invoke(args...);
    }
}

} // namespace Limx
