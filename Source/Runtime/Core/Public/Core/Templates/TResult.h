/*******************************************************************************
 * 文件: TResult.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   错误处理类型 — 替代异常的零 STL 依赖 Result 模式
 *   表示操作结果: 成功时持有值 (Ok)，失败时持有错误 (Err)
 *   用于文件 I/O、资产加载、解析等可能失败的操作
 *
 * 设计哲学:
 *   显式错误处理 — 调用者必须检查结果，编译器强制不可忽略
 *   值语义 — 内联存储值或错误，无堆分配
 *   Rust 风格 — 受 Rust Result<T, E> 启发的 API 设计
 *
 * 技术特性:
 *   - Ok<T>: 成功值工厂
 *   - Err<E>: 错误值工厂
 *   - IsOk/IsErr: 状态查询
 *   - GetValue/GetError: 获取值或错误 (断言保护)
 *   - ValueOr: 提供默认值的安全获取
 *   - Map/MapErr: 函数式变换
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h,
 *          Core/TypeTraits/TypeTraits.h, Core/Memory/MemoryOps.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/TypeTraits/TypeTraits.h"
#include "Core/Memory/MemoryOps.h"

namespace Limx
{

// ============================================================================
// TResult<T, E> — 结果类型
// ============================================================================

/// 结果类型 — 成功 (T) 或 失败 (E)
/// @tparam T 成功值类型
/// @tparam E 错误值类型
template<typename T, typename E>
class LIMX_NODISCARD TResult
{
    // 内部存储 — 足够容纳 T 或 E 中较大的那个
    static constexpr SizeType kStorageSize =
        sizeof(T) > sizeof(E) ? sizeof(T) : sizeof(E);
    static constexpr SizeType kStorageAlign =
        alignof(T) > alignof(E) ? alignof(T) : alignof(E);

public:
    // ========================================================================
    // 构造与析构
    // ========================================================================

    /// 成功值构造 (标签分发)
    struct OkTag {};
    struct ErrTag {};

    TResult(OkTag, const T& value)
        : m_IsOk(true)
    {
        new (&m_Storage) T(value);
    }

    TResult(OkTag, T&& value)
        : m_IsOk(true)
    {
        new (&m_Storage) T(MoveTemp(value));
    }

    TResult(ErrTag, const E& error)
        : m_IsOk(false)
    {
        new (&m_Storage) E(error);
    }

    TResult(ErrTag, E&& error)
        : m_IsOk(false)
    {
        new (&m_Storage) E(MoveTemp(error));
    }

    /// 拷贝构造
    TResult(const TResult& other)
        : m_IsOk(other.m_IsOk)
    {
        if (m_IsOk)
        {
            new (&m_Storage) T(other.AsValue());
        }
        else
        {
            new (&m_Storage) E(other.AsError());
        }
    }

    /// 移动构造
    TResult(TResult&& other) noexcept
        : m_IsOk(other.m_IsOk)
    {
        if (m_IsOk)
        {
            new (&m_Storage) T(MoveTemp(other.AsValue()));
        }
        else
        {
            new (&m_Storage) E(MoveTemp(other.AsError()));
        }
    }

    ~TResult()
    {
        Destroy();
    }

    /// 拷贝赋值
    TResult& operator=(const TResult& other)
    {
        if (this != &other)
        {
            Destroy();
            m_IsOk = other.m_IsOk;
            if (m_IsOk)
            {
                new (&m_Storage) T(other.AsValue());
            }
            else
            {
                new (&m_Storage) E(other.AsError());
            }
        }
        return *this;
    }

    /// 移动赋值
    TResult& operator=(TResult&& other) noexcept
    {
        if (this != &other)
        {
            Destroy();
            m_IsOk = other.m_IsOk;
            if (m_IsOk)
            {
                new (&m_Storage) T(MoveTemp(other.AsValue()));
            }
            else
            {
                new (&m_Storage) E(MoveTemp(other.AsError()));
            }
        }
        return *this;
    }

    // ========================================================================
    // 状态查询
    // ========================================================================

    LIMX_NODISCARD bool IsOk() const { return m_IsOk; }
    LIMX_NODISCARD bool IsErr() const { return !m_IsOk; }

    /// 布尔转换 — true 表示成功
    LIMX_NODISCARD explicit operator bool() const { return m_IsOk; }

    // ========================================================================
    // 值访问
    // ========================================================================

    /// 获取成功值引用 (断言保护)
    LIMX_NODISCARD T& GetValue()
    {
        LIMX_ASSERT(m_IsOk);
        return AsValue();
    }

    LIMX_NODISCARD const T& GetValue() const
    {
        LIMX_ASSERT(m_IsOk);
        return AsValue();
    }

    /// 获取错误值引用 (断言保护)
    LIMX_NODISCARD E& GetError()
    {
        LIMX_ASSERT(!m_IsOk);
        return AsError();
    }

    LIMX_NODISCARD const E& GetError() const
    {
        LIMX_ASSERT(!m_IsOk);
        return AsError();
    }

    /// 安全获取值 — 失败时返回默认值
    LIMX_NODISCARD T ValueOr(const T& defaultValue) const
    {
        if (m_IsOk)
        {
            return AsValue();
        }
        return defaultValue;
    }

    /// 安全获取值 — 失败时返回移动的默认值
    LIMX_NODISCARD T ValueOr(T&& defaultValue) const
    {
        if (m_IsOk)
        {
            return AsValue();
        }
        return MoveTemp(defaultValue);
    }

    // ========================================================================
    // 函数式变换
    // ========================================================================

    /// Map — 变换成功值，保留错误
    template<typename Func>
    auto Map(Func&& func) const
        -> TResult<decltype(func(this->AsValue())), E>
    {
        using NewT = decltype(func(this->AsValue()));
        if (m_IsOk)
        {
            return TResult<NewT, E>(
                typename TResult<NewT, E>::OkTag{},
                func(this->AsValue()));
        }
        return TResult<NewT, E>(
            typename TResult<NewT, E>::ErrTag{}, this->AsError());
    }

    /// MapErr — 变换错误值，保留成功值
    template<typename Func>
    auto MapErr(Func&& func) const
        -> TResult<T, decltype(func(this->AsError()))>
    {
        using NewE = decltype(func(this->AsError()));
        if (m_IsOk)
        {
            return TResult<T, NewE>(
                typename TResult<T, NewE>::OkTag{},
                this->AsValue());
        }
        return TResult<T, NewE>(
            typename TResult<T, NewE>::ErrTag{},
            func(this->AsError()));
    }

private:
    void Destroy()
    {
        if (m_IsOk)
        {
            AsValue().~T();
        }
        else
        {
            AsError().~E();
        }
    }

    T& AsValue() { return *reinterpret_cast<T*>(&m_Storage); }
    const T& AsValue() const
    {
        return *reinterpret_cast<const T*>(&m_Storage);
    }

    E& AsError() { return *reinterpret_cast<E*>(&m_Storage); }
    const E& AsError() const
    {
        return *reinterpret_cast<const E*>(&m_Storage);
    }

    alignas(kStorageAlign) char m_Storage[kStorageSize];
    bool m_IsOk;
};

// ============================================================================
// 工厂函数
// ============================================================================

/// 创建成功结果
template<typename T, typename E>
LIMX_NODISCARD TResult<T, E> MakeOk(const T& value)
{
    return TResult<T, E>(typename TResult<T, E>::OkTag{}, value);
}

template<typename T, typename E>
LIMX_NODISCARD TResult<T, E> MakeOk(T&& value)
{
    return TResult<T, E>(typename TResult<T, E>::OkTag{}, MoveTemp(value));
}

/// 创建失败结果
template<typename T, typename E>
LIMX_NODISCARD TResult<T, E> MakeErr(const E& error)
{
    return TResult<T, E>(typename TResult<T, E>::ErrTag{}, error);
}

template<typename T, typename E>
LIMX_NODISCARD TResult<T, E> MakeErr(E&& error)
{
    return TResult<T, E>(typename TResult<T, E>::ErrTag{}, MoveTemp(error));
}

} // namespace Limx
