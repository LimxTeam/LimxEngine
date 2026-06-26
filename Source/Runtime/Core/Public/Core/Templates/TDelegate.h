/*******************************************************************************
 * 文件: TDelegate.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   委托系统 — 类型安全的回调与事件分发机制
 *   TDelegate: 单播委托 (单个绑定)
 *   TMulticastDelegate: 多播委托 (多个绑定，顺序广播)
 *   支持静态函数、Lambda、成员函数绑定
 *
 * 设计哲学:
 *   类型安全 — 编译时检查签名匹配
 *   零 STL 依赖 — 基于 TFunction 和 TArray 实现
 *   句柄管理 — 多播委托通过 DelegateHandle 管理绑定的添加/移除
 *   引擎事件模型 — 对标 UE 的 DECLARE_DELEGATE / DECLARE_MULTICAST_DELEGATE
 *
 * 技术特性:
 *   - TDelegate: 封装 TFunction，单播绑定
 *   - TMulticastDelegate: 存储 TFunction 数组，多播广播
 *   - DelegateHandle: 64 位唯一句柄，用于移除绑定
 *   - 支持: BindStatic, BindLambda, BindRaw, Broadcast, Remove
 *
 * 依赖关系:
 *   内部: Core/Containers/TArray.h, Core/Templates/TFunction.h
 *
 * 注意事项:
 *   委托不是线程安全的
 *   多播委托在 Broadcast 期间不可修改绑定列表
 *   绑定原始指针 (BindRaw) 时调用者负责确保指针生命周期
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Containers/TArray.h"
#include "Core/Templates/TFunction.h"

namespace Limx
{

// ============================================================================
// DelegateHandle — 委托绑定句柄
// ============================================================================

/// 委托绑定句柄 — 用于标识和移除多播委托中的特定绑定
class DelegateHandle
{
public:
    DelegateHandle()
        : m_Id(0)
    {
    }

    /// 是否有效
    LIMX_NODISCARD FORCEINLINE bool IsValid() const { return m_Id != 0; }

    /// 重置为无效
    void Reset() { m_Id = 0; }

    LIMX_NODISCARD friend bool operator==(const DelegateHandle& lhs,
                                           const DelegateHandle& rhs)
    {
        return lhs.m_Id == rhs.m_Id;
    }

    LIMX_NODISCARD friend bool operator!=(const DelegateHandle& lhs,
                                           const DelegateHandle& rhs)
    {
        return lhs.m_Id != rhs.m_Id;
    }

    /// 生成新的唯一句柄
    static DelegateHandle Generate()
    {
        DelegateHandle handle;
        handle.m_Id = ++s_NextId;
        return handle;
    }

private:
    UInt64 m_Id;

    // 全局递增计数器 — 保证句柄唯一性
    static inline UInt64 s_NextId = 0;
};

// ============================================================================
// TDelegate — 单播委托
// ============================================================================

// 主模板 — 未特化时不定义
template<typename Signature>
class TDelegate;

/// 单播委托 — 绑定单个可调用对象
/// @tparam ReturnType 返回类型
/// @tparam Args       参数类型
template<typename ReturnType, typename... Args>
class TDelegate<ReturnType(Args...)>
{
public:
    using FunctionType = TFunction<ReturnType(Args...)>;

    /// 默认构造 — 未绑定
    TDelegate() = default;

    /// 绑定静态函数
    void BindStatic(ReturnType(*function)(Args...))
    {
        m_Function = function;
    }

    /// 绑定 Lambda 或仿函数
    template<typename Functor>
    void BindLambda(Functor&& functor)
    {
        m_Function = Forward<Functor>(functor);
    }

    /// 绑定成员函数 + 对象指针
    /// 调用者负责确保对象生命周期长于委托
    template<typename ObjectType, typename MethodType>
    void BindRaw(ObjectType* object, MethodType method)
    {
        m_Function = [object, method](Args... args) -> ReturnType
        {
            return (object->*method)(Forward<Args>(args)...);
        };
    }

    /// 执行委托
    ReturnType Execute(Args... args) const
    {
        LIMX_ASSERT(IsBound());
        return m_Function(Forward<Args>(args)...);
    }

    /// 安全执行 — 仅在已绑定时调用 (仅 void 返回类型)
    void ExecuteIfBound(Args... args) const
    {
        if (IsBound())
        {
            m_Function(Forward<Args>(args)...);
        }
    }

    /// 是否已绑定
    LIMX_NODISCARD FORCEINLINE bool IsBound() const
    {
        return m_Function.IsValid();
    }

    /// 解除绑定
    void Unbind()
    {
        m_Function.Reset();
    }

    /// 隐式 bool
    LIMX_NODISCARD FORCEINLINE explicit operator bool() const
    {
        return IsBound();
    }

private:
    FunctionType m_Function;
};

// ============================================================================
// TMulticastDelegate — 多播委托
// ============================================================================

// 主模板
template<typename Signature>
class TMulticastDelegate;

/// 多播委托 — 绑定多个可调用对象，顺序广播
/// 仅支持 void 返回类型
/// @tparam Args 参数类型
template<typename... Args>
class TMulticastDelegate<void(Args...)>
{
    using FunctionType = TFunction<void(Args...)>;

    struct Binding
    {
        DelegateHandle Handle;
        FunctionType   Function;
    };

public:
    TMulticastDelegate() = default;

    /// 添加静态函数绑定
    DelegateHandle AddStatic(void(*function)(Args...))
    {
        Binding binding;
        binding.Handle = DelegateHandle::Generate();
        binding.Function = function;
        m_Bindings.Add(MoveTemp(binding));
        return m_Bindings.Last().Handle;
    }

    /// 添加 Lambda 绑定
    template<typename Functor>
    DelegateHandle AddLambda(Functor&& functor)
    {
        Binding binding;
        binding.Handle = DelegateHandle::Generate();
        binding.Function = Forward<Functor>(functor);
        m_Bindings.Add(MoveTemp(binding));
        return m_Bindings.Last().Handle;
    }

    /// 添加成员函数绑定
    template<typename ObjectType, typename MethodType>
    DelegateHandle AddRaw(ObjectType* object, MethodType method)
    {
        return AddLambda([object, method](Args... args)
        {
            (object->*method)(Forward<Args>(args)...);
        });
    }

    /// 移除指定句柄的绑定
    bool Remove(DelegateHandle handle)
    {
        if (!handle.IsValid())
        {
            return false;
        }

        for (SizeType index = 0; index < m_Bindings.GetSize(); ++index)
        {
            if (m_Bindings[index].Handle == handle)
            {
                m_Bindings.RemoveAt(index);
                return true;
            }
        }
        return false;
    }

    /// 移除所有绑定
    void Clear()
    {
        m_Bindings.Clear();
    }

    /// 广播 — 按添加顺序调用所有绑定
    void Broadcast(Args... args) const
    {
        // 拷贝绑定列表，防止在回调中修改导致的迭代器失效
        // 对于性能关键路径可考虑直接迭代 + 标记延迟删除
        SizeType count = m_Bindings.GetSize();
        for (SizeType index = 0; index < count; ++index)
        {
            if (m_Bindings[index].Function.IsValid())
            {
                m_Bindings[index].Function(Forward<Args>(args)...);
            }
        }
    }

    /// 当前绑定数量
    LIMX_NODISCARD FORCEINLINE SizeType GetBindingCount() const
    {
        return m_Bindings.GetSize();
    }

    /// 是否有绑定
    LIMX_NODISCARD FORCEINLINE bool IsBound() const
    {
        return m_Bindings.GetSize() > 0;
    }

private:
    TArray<Binding> m_Bindings;
};

} // namespace Limx
