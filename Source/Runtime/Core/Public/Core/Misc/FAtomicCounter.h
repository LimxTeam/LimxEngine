/*******************************************************************************
 * 文件: FAtomicCounter.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   原子计数器 — 线程安全的整数计数器
 *   基于 MSVC 编译器内建原子操作实现
 *   用于引用计数、线程安全统计、无锁并发计数等场景
 *
 * 设计哲学:
 *   零 STL — 直接使用 MSVC _Interlocked* 内建函数
 *   最小接口 — 仅提供递增/递减/读取/交换等基础操作
 *   内存序可控 — 使用全屏障保证顺序一致性
 *
 * 技术特性:
 *   - FAtomicCounter32: 32 位原子计数器
 *   - FAtomicCounter64: 64 位原子计数器
 *   - Increment/Decrement: 原子递增/递减
 *   - Add/Subtract: 原子加减
 *   - Exchange/CompareExchange: 原子交换/CAS
 *   - Load/Store: 原子读写
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h
 *   外部: MSVC _Interlocked* 内建函数
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"

// MSVC 原子操作内建函数声明
#if LIMX_COMPILER_MSVC
extern "C"
{
    long __cdecl _InterlockedIncrement(long volatile*);
    long __cdecl _InterlockedDecrement(long volatile*);
    long __cdecl _InterlockedExchangeAdd(long volatile*, long);
    long __cdecl _InterlockedExchange(long volatile*, long);
    long __cdecl _InterlockedCompareExchange(
        long volatile*, long, long);

    __int64 __cdecl _InterlockedIncrement64(__int64 volatile*);
    __int64 __cdecl _InterlockedDecrement64(__int64 volatile*);
    __int64 __cdecl _InterlockedExchangeAdd64(
        __int64 volatile*, __int64);
    __int64 __cdecl _InterlockedExchange64(
        __int64 volatile*, __int64);
    __int64 __cdecl _InterlockedCompareExchange64(
        __int64 volatile*, __int64, __int64);
}

#pragma intrinsic(_InterlockedIncrement)
#pragma intrinsic(_InterlockedDecrement)
#pragma intrinsic(_InterlockedExchangeAdd)
#pragma intrinsic(_InterlockedExchange)
#pragma intrinsic(_InterlockedCompareExchange)
#pragma intrinsic(_InterlockedIncrement64)
#pragma intrinsic(_InterlockedDecrement64)
#pragma intrinsic(_InterlockedExchangeAdd64)
#pragma intrinsic(_InterlockedExchange64)
#pragma intrinsic(_InterlockedCompareExchange64)
#endif

namespace Limx
{

/// 32 位原子计数器
class FAtomicCounter32
{
public:
    /// 默认构造 — 初始值为 0
    FAtomicCounter32() : m_Value(0) {}

    /// 指定初始值
    explicit FAtomicCounter32(Int32 initialValue)
        : m_Value(initialValue) {}

    // 不可拷贝
    FAtomicCounter32(const FAtomicCounter32&) = delete;
    FAtomicCounter32& operator=(const FAtomicCounter32&) = delete;

    // ========================================================================
    // 递增/递减
    // ========================================================================

    /// 原子递增，返回递增后的值
    LIMX_NODISCARD Int32 Increment()
    {
#if LIMX_COMPILER_MSVC
        return static_cast<Int32>(
            _InterlockedIncrement(
                reinterpret_cast<long volatile*>(&m_Value)));
#else
        return ++m_Value;
#endif
    }

    /// 原子递减，返回递减后的值
    LIMX_NODISCARD Int32 Decrement()
    {
#if LIMX_COMPILER_MSVC
        return static_cast<Int32>(
            _InterlockedDecrement(
                reinterpret_cast<long volatile*>(&m_Value)));
#else
        return --m_Value;
#endif
    }

    // ========================================================================
    // 加减
    // ========================================================================

    /// 原子加，返回加之前的值
    LIMX_NODISCARD Int32 Add(Int32 amount)
    {
#if LIMX_COMPILER_MSVC
        return static_cast<Int32>(
            _InterlockedExchangeAdd(
                reinterpret_cast<long volatile*>(&m_Value),
                static_cast<long>(amount)));
#else
        Int32 old = m_Value;
        m_Value += amount;
        return old;
#endif
    }

    /// 原子减，返回减之前的值
    LIMX_NODISCARD Int32 Subtract(Int32 amount)
    {
        return Add(-amount);
    }

    // ========================================================================
    // 交换
    // ========================================================================

    /// 原子交换，返回交换前的值
    LIMX_NODISCARD Int32 Exchange(Int32 newValue)
    {
#if LIMX_COMPILER_MSVC
        return static_cast<Int32>(
            _InterlockedExchange(
                reinterpret_cast<long volatile*>(&m_Value),
                static_cast<long>(newValue)));
#else
        Int32 old = m_Value;
        m_Value = newValue;
        return old;
#endif
    }

    /// 原子比较交换 (CAS)
    /// 如果当前值 == expected，则设为 desired，返回交换前的值
    LIMX_NODISCARD Int32 CompareExchange(
        Int32 desired, Int32 expected)
    {
#if LIMX_COMPILER_MSVC
        return static_cast<Int32>(
            _InterlockedCompareExchange(
                reinterpret_cast<long volatile*>(&m_Value),
                static_cast<long>(desired),
                static_cast<long>(expected)));
#else
        Int32 old = m_Value;
        if (m_Value == expected) m_Value = desired;
        return old;
#endif
    }

    // ========================================================================
    // 读写
    // ========================================================================

    /// 原子读取
    LIMX_NODISCARD Int32 Load() const
    {
#if LIMX_COMPILER_MSVC
        // 通过 CAS(0,0) 实现原子读取
        return static_cast<Int32>(
            _InterlockedCompareExchange(
                const_cast<long volatile*>(
                    reinterpret_cast<const long volatile*>(&m_Value)),
                0, 0));
#else
        return m_Value;
#endif
    }

    /// 原子存储
    void Store(Int32 value)
    {
        (void)Exchange(value);
    }

private:
    volatile Int32 m_Value;
};

/// 64 位原子计数器
class FAtomicCounter64
{
public:
    FAtomicCounter64() : m_Value(0) {}
    explicit FAtomicCounter64(Int64 initialValue)
        : m_Value(initialValue) {}

    FAtomicCounter64(const FAtomicCounter64&) = delete;
    FAtomicCounter64& operator=(const FAtomicCounter64&) = delete;

    LIMX_NODISCARD Int64 Increment()
    {
#if LIMX_COMPILER_MSVC
        return static_cast<Int64>(
            _InterlockedIncrement64(
                reinterpret_cast<__int64 volatile*>(&m_Value)));
#else
        return ++m_Value;
#endif
    }

    LIMX_NODISCARD Int64 Decrement()
    {
#if LIMX_COMPILER_MSVC
        return static_cast<Int64>(
            _InterlockedDecrement64(
                reinterpret_cast<__int64 volatile*>(&m_Value)));
#else
        return --m_Value;
#endif
    }

    LIMX_NODISCARD Int64 Add(Int64 amount)
    {
#if LIMX_COMPILER_MSVC
        return static_cast<Int64>(
            _InterlockedExchangeAdd64(
                reinterpret_cast<__int64 volatile*>(&m_Value),
                static_cast<__int64>(amount)));
#else
        Int64 old = m_Value;
        m_Value += amount;
        return old;
#endif
    }

    LIMX_NODISCARD Int64 Subtract(Int64 amount)
    {
        return Add(-amount);
    }

    LIMX_NODISCARD Int64 Exchange(Int64 newValue)
    {
#if LIMX_COMPILER_MSVC
        return static_cast<Int64>(
            _InterlockedExchange64(
                reinterpret_cast<__int64 volatile*>(&m_Value),
                static_cast<__int64>(newValue)));
#else
        Int64 old = m_Value;
        m_Value = newValue;
        return old;
#endif
    }

    LIMX_NODISCARD Int64 CompareExchange(
        Int64 desired, Int64 expected)
    {
#if LIMX_COMPILER_MSVC
        return static_cast<Int64>(
            _InterlockedCompareExchange64(
                reinterpret_cast<__int64 volatile*>(&m_Value),
                static_cast<__int64>(desired),
                static_cast<__int64>(expected)));
#else
        Int64 old = m_Value;
        if (m_Value == expected) m_Value = desired;
        return old;
#endif
    }

    LIMX_NODISCARD Int64 Load() const
    {
#if LIMX_COMPILER_MSVC
        return static_cast<Int64>(
            _InterlockedCompareExchange64(
                const_cast<__int64 volatile*>(
                    reinterpret_cast<const __int64 volatile*>(
                        &m_Value)),
                0, 0));
#else
        return m_Value;
#endif
    }

    void Store(Int64 value)
    {
        (void)Exchange(value);
    }

private:
    volatile Int64 m_Value;
};

/// 默认原子计数器 (平台位宽)
using FAtomicCounter = FAtomicCounter64;

} // namespace Limx
