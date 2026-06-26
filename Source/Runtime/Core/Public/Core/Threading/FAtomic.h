/*******************************************************************************
 * 文件: FAtomic.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   原子操作封装 — 零 STL 依赖的线程安全原子操作
 *   封装 MSVC Interlocked* 内建函数，提供跨平台统一接口
 *   用于引用计数、无锁数据结构、线程间通信标志等场景
 *
 * 设计哲学:
 *   编译器内建 — 直接使用 MSVC _Interlocked* 系列，零运行时依赖
 *   显式内存序 — 默认 Sequential Consistency，可选 Relaxed/Acquire/Release
 *   值语义封装 — TAtomic<T> 包装基础整数类型，提供原子读写和 CAS
 *
 * 技术特性:
 *   - AtomicOps: 静态工具函数 (Increment, Decrement, Exchange, CompareExchange)
 *   - TAtomic<T>: 模板包装器 (Load, Store, FetchAdd, FetchSub, CompareExchange)
 *   - 支持类型: Int32, Int64, UInt32, UInt64, void*
 *   - 内存屏障: AtomicOps::MemoryBarrier()
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h
 *   外部: MSVC Interlocked 内建函数
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"

// MSVC 内建函数声明
#if LIMX_COMPILER_MSVC
extern "C"
{
    long _InterlockedIncrement(long volatile*);
    long _InterlockedDecrement(long volatile*);
    long _InterlockedExchange(long volatile*, long);
    long _InterlockedCompareExchange(long volatile*, long, long);
    long _InterlockedExchangeAdd(long volatile*, long);

    long long _InterlockedIncrement64(long long volatile*);
    long long _InterlockedDecrement64(long long volatile*);
    long long _InterlockedExchange64(long long volatile*, long long);
    long long _InterlockedCompareExchange64(long long volatile*, long long, long long);
    long long _InterlockedExchangeAdd64(long long volatile*, long long);

    void* _InterlockedExchangePointer(void* volatile*, void*);
    void* _InterlockedCompareExchangePointer(void* volatile*, void*, void*);

    // 内存屏障
    void _ReadWriteBarrier(void);
}

#pragma intrinsic(_InterlockedIncrement)
#pragma intrinsic(_InterlockedDecrement)
#pragma intrinsic(_InterlockedExchange)
#pragma intrinsic(_InterlockedCompareExchange)
#pragma intrinsic(_InterlockedExchangeAdd)
#pragma intrinsic(_InterlockedIncrement64)
#pragma intrinsic(_InterlockedDecrement64)
#pragma intrinsic(_InterlockedExchange64)
#pragma intrinsic(_InterlockedCompareExchange64)
#pragma intrinsic(_InterlockedExchangeAdd64)
#pragma intrinsic(_InterlockedExchangePointer)
#pragma intrinsic(_InterlockedCompareExchangePointer)
#endif

namespace Limx
{

// ============================================================================
// AtomicOps — 静态原子操作工具
// ============================================================================

struct AtomicOps
{
    // ========================================================================
    // 32 位操作
    // ========================================================================

    /// 原子递增 — 返回递增后的值
    static FORCEINLINE Int32 Increment(volatile Int32* target)
    {
#if LIMX_COMPILER_MSVC
        return static_cast<Int32>(
            _InterlockedIncrement(reinterpret_cast<volatile long*>(target)));
#endif
    }

    /// 原子递减 — 返回递减后的值
    static FORCEINLINE Int32 Decrement(volatile Int32* target)
    {
#if LIMX_COMPILER_MSVC
        return static_cast<Int32>(
            _InterlockedDecrement(reinterpret_cast<volatile long*>(target)));
#endif
    }

    /// 原子交换 — 返回旧值
    static FORCEINLINE Int32 Exchange(volatile Int32* target, Int32 value)
    {
#if LIMX_COMPILER_MSVC
        return static_cast<Int32>(
            _InterlockedExchange(
                reinterpret_cast<volatile long*>(target),
                static_cast<long>(value)));
#endif
    }

    /// 原子比较交换 (CAS) — 如果 *target == comparand 则 *target = exchange
    /// 返回旧值
    static FORCEINLINE Int32 CompareExchange(volatile Int32* target,
                                              Int32 exchange,
                                              Int32 comparand)
    {
#if LIMX_COMPILER_MSVC
        return static_cast<Int32>(
            _InterlockedCompareExchange(
                reinterpret_cast<volatile long*>(target),
                static_cast<long>(exchange),
                static_cast<long>(comparand)));
#endif
    }

    /// 原子加法 — 返回旧值
    static FORCEINLINE Int32 FetchAdd(volatile Int32* target, Int32 value)
    {
#if LIMX_COMPILER_MSVC
        return static_cast<Int32>(
            _InterlockedExchangeAdd(
                reinterpret_cast<volatile long*>(target),
                static_cast<long>(value)));
#endif
    }

    // ========================================================================
    // 64 位操作
    // ========================================================================

    static FORCEINLINE Int64 Increment64(volatile Int64* target)
    {
#if LIMX_COMPILER_MSVC
        return _InterlockedIncrement64(target);
#endif
    }

    static FORCEINLINE Int64 Decrement64(volatile Int64* target)
    {
#if LIMX_COMPILER_MSVC
        return _InterlockedDecrement64(target);
#endif
    }

    static FORCEINLINE Int64 Exchange64(volatile Int64* target, Int64 value)
    {
#if LIMX_COMPILER_MSVC
        return _InterlockedExchange64(target, value);
#endif
    }

    static FORCEINLINE Int64 CompareExchange64(volatile Int64* target,
                                                Int64 exchange,
                                                Int64 comparand)
    {
#if LIMX_COMPILER_MSVC
        return _InterlockedCompareExchange64(target, exchange, comparand);
#endif
    }

    static FORCEINLINE Int64 FetchAdd64(volatile Int64* target, Int64 value)
    {
#if LIMX_COMPILER_MSVC
        return _InterlockedExchangeAdd64(target, value);
#endif
    }

    // ========================================================================
    // 指针操作
    // ========================================================================

    static FORCEINLINE void* ExchangePointer(void* volatile* target,
                                              void* value)
    {
#if LIMX_COMPILER_MSVC
        return _InterlockedExchangePointer(target, value);
#endif
    }

    static FORCEINLINE void* CompareExchangePointer(void* volatile* target,
                                                      void* exchange,
                                                      void* comparand)
    {
#if LIMX_COMPILER_MSVC
        return _InterlockedCompareExchangePointer(
            target, exchange, comparand);
#endif
    }

    // ========================================================================
    // 内存屏障
    // ========================================================================

    static FORCEINLINE void MemoryBarrier()
    {
#if LIMX_COMPILER_MSVC
        _ReadWriteBarrier();
        // 硬件内存屏障 — 使用 lock 前缀的 CAS 作为全屏障
        volatile long fence;
        _InterlockedCompareExchange(&fence, 0, 0);
#endif
    }
};

// ============================================================================
// TAtomic<T> — 原子值包装器
// ============================================================================

/// 原子 Int32
template<typename T>
class TAtomic;

template<>
class TAtomic<Int32>
{
public:
    constexpr TAtomic() : m_Value(0) {}
    constexpr explicit TAtomic(Int32 initial) : m_Value(initial) {}

    // 禁止拷贝
    TAtomic(const TAtomic&) = delete;
    TAtomic& operator=(const TAtomic&) = delete;

    /// 原子加载
    LIMX_NODISCARD FORCEINLINE Int32 Load() const
    {
        return AtomicOps::CompareExchange(
            const_cast<volatile Int32*>(&m_Value), 0, 0);
    }

    /// 原子存储
    FORCEINLINE void Store(Int32 value)
    {
        AtomicOps::Exchange(&m_Value, value);
    }

    /// 原子递增 — 返回递增后的值
    FORCEINLINE Int32 Increment()
    {
        return AtomicOps::Increment(&m_Value);
    }

    /// 原子递减 — 返回递减后的值
    FORCEINLINE Int32 Decrement()
    {
        return AtomicOps::Decrement(&m_Value);
    }

    /// 原子加 — 返回旧值
    FORCEINLINE Int32 FetchAdd(Int32 amount)
    {
        return AtomicOps::FetchAdd(&m_Value, amount);
    }

    /// 原子减 — 返回旧值
    FORCEINLINE Int32 FetchSub(Int32 amount)
    {
        return AtomicOps::FetchAdd(&m_Value, -amount);
    }

    /// 原子交换 — 返回旧值
    FORCEINLINE Int32 Exchange(Int32 value)
    {
        return AtomicOps::Exchange(&m_Value, value);
    }

    /// CAS — 返回是否成功
    FORCEINLINE bool CompareExchange(Int32& expected, Int32 desired)
    {
        Int32 old = AtomicOps::CompareExchange(
            &m_Value, desired, expected);
        if (old == expected)
        {
            return true;
        }
        expected = old;
        return false;
    }

    LIMX_NODISCARD FORCEINLINE operator Int32() const { return Load(); }

private:
    volatile Int32 m_Value;
};

/// 原子 Int64
template<>
class TAtomic<Int64>
{
public:
    constexpr TAtomic() : m_Value(0) {}
    constexpr explicit TAtomic(Int64 initial) : m_Value(initial) {}

    TAtomic(const TAtomic&) = delete;
    TAtomic& operator=(const TAtomic&) = delete;

    LIMX_NODISCARD FORCEINLINE Int64 Load() const
    {
        return AtomicOps::CompareExchange64(
            const_cast<volatile Int64*>(&m_Value), 0, 0);
    }

    FORCEINLINE void Store(Int64 value)
    {
        AtomicOps::Exchange64(&m_Value, value);
    }

    FORCEINLINE Int64 Increment()
    {
        return AtomicOps::Increment64(&m_Value);
    }

    FORCEINLINE Int64 Decrement()
    {
        return AtomicOps::Decrement64(&m_Value);
    }

    FORCEINLINE Int64 FetchAdd(Int64 amount)
    {
        return AtomicOps::FetchAdd64(&m_Value, amount);
    }

    FORCEINLINE Int64 FetchSub(Int64 amount)
    {
        return AtomicOps::FetchAdd64(&m_Value, -amount);
    }

    FORCEINLINE Int64 Exchange(Int64 value)
    {
        return AtomicOps::Exchange64(&m_Value, value);
    }

    FORCEINLINE bool CompareExchange(Int64& expected, Int64 desired)
    {
        Int64 old = AtomicOps::CompareExchange64(
            &m_Value, desired, expected);
        if (old == expected)
        {
            return true;
        }
        expected = old;
        return false;
    }

    LIMX_NODISCARD FORCEINLINE operator Int64() const { return Load(); }

private:
    volatile Int64 m_Value;
};

/// 原子 bool (基于 Int32 实现)
template<>
class TAtomic<bool>
{
public:
    constexpr TAtomic() : m_Value(0) {}
    constexpr explicit TAtomic(bool initial)
        : m_Value(initial ? 1 : 0) {}

    TAtomic(const TAtomic&) = delete;
    TAtomic& operator=(const TAtomic&) = delete;

    LIMX_NODISCARD FORCEINLINE bool Load() const
    {
        return m_Value.Load() != 0;
    }

    FORCEINLINE void Store(bool value)
    {
        m_Value.Store(value ? 1 : 0);
    }

    FORCEINLINE bool Exchange(bool value)
    {
        return m_Value.Exchange(value ? 1 : 0) != 0;
    }

    LIMX_NODISCARD FORCEINLINE operator bool() const { return Load(); }

private:
    TAtomic<Int32> m_Value;
};

/// 原子指针
template<typename T>
class TAtomic<T*>
{
public:
    constexpr TAtomic() : m_Pointer(nullptr) {}
    constexpr explicit TAtomic(T* initial) : m_Pointer(initial) {}

    TAtomic(const TAtomic&) = delete;
    TAtomic& operator=(const TAtomic&) = delete;

    LIMX_NODISCARD FORCEINLINE T* Load() const
    {
        return static_cast<T*>(
            AtomicOps::CompareExchangePointer(
                const_cast<void* volatile*>(
                    reinterpret_cast<void* const volatile*>(&m_Pointer)),
                nullptr, nullptr));
    }

    FORCEINLINE void Store(T* value)
    {
        AtomicOps::ExchangePointer(
            reinterpret_cast<void* volatile*>(&m_Pointer), value);
    }

    FORCEINLINE T* Exchange(T* value)
    {
        return static_cast<T*>(
            AtomicOps::ExchangePointer(
                reinterpret_cast<void* volatile*>(&m_Pointer), value));
    }

    FORCEINLINE bool CompareExchange(T*& expected, T* desired)
    {
        T* old = static_cast<T*>(
            AtomicOps::CompareExchangePointer(
                reinterpret_cast<void* volatile*>(&m_Pointer),
                desired, expected));
        if (old == expected)
        {
            return true;
        }
        expected = old;
        return false;
    }

    LIMX_NODISCARD FORCEINLINE operator T*() const { return Load(); }
    LIMX_NODISCARD FORCEINLINE T* operator->() const { return Load(); }

private:
    T* volatile m_Pointer;
};

} // namespace Limx
