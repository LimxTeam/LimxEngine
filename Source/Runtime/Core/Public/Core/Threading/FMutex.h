/*******************************************************************************
 * 文件: FMutex.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   互斥锁与读写锁 — 零 STL 依赖的线程同步原语
 *   封装 Windows SRWLOCK 实现高性能互斥与读写锁
 *   提供 RAII 作用域锁保证异常安全
 *
 * 设计哲学:
 *   SRWLOCK — 比 CRITICAL_SECTION 更轻量 (仅 8 字节)，无需初始化/销毁
 *   RAII 锁 — FScopeLock/FScopeReadLock/FScopeWriteLock 保证解锁
 *   不可递归 — SRWLOCK 不支持递归锁定，设计上避免递归需求
 *
 * 技术特性:
 *   - FMutex: 独占互斥锁 (SRWLOCK Exclusive)
 *   - FRWLock: 读写锁 (SRWLOCK Shared/Exclusive)
 *   - FScopeLock: RAII 独占锁
 *   - FScopeReadLock: RAII 共享读锁
 *   - FScopeWriteLock: RAII 独占写锁
 *   - FConditionVariable: 条件变量 (CONDITION_VARIABLE)
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h
 *   外部: Windows API (SRWLOCK, CONDITION_VARIABLE)
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"

// Windows 同步原语前向声明
#if LIMX_PLATFORM_WINDOWS
#ifdef _WINDOWS_
// Windows 头文件已包含 — 使用真实类型别名
using SRWLOCK_T = SRWLOCK;
using CONDITION_VARIABLE_T = CONDITION_VARIABLE;
// API 函数已由 <synchapi.h> 声明，无需重复
#else
// 无 Windows 头文件 — 使用二进制兼容的前向声明
extern "C"
{
    // SRWLOCK 是一个 void* 大小的结构
    struct SRWLOCK_T
    {
        void* Ptr;
    };

    // CONDITION_VARIABLE 同样是 void* 大小
    struct CONDITION_VARIABLE_T
    {
        void* Ptr;
    };

    void __stdcall InitializeSRWLock(SRWLOCK_T*);
    void __stdcall AcquireSRWLockExclusive(SRWLOCK_T*);
    void __stdcall ReleaseSRWLockExclusive(SRWLOCK_T*);
    void __stdcall AcquireSRWLockShared(SRWLOCK_T*);
    void __stdcall ReleaseSRWLockShared(SRWLOCK_T*);
    int  __stdcall TryAcquireSRWLockExclusive(SRWLOCK_T*);
    int  __stdcall TryAcquireSRWLockShared(SRWLOCK_T*);

    void __stdcall InitializeConditionVariable(CONDITION_VARIABLE_T*);
    void __stdcall WakeConditionVariable(CONDITION_VARIABLE_T*);
    void __stdcall WakeAllConditionVariable(CONDITION_VARIABLE_T*);
    int  __stdcall SleepConditionVariableSRW(
        CONDITION_VARIABLE_T*, SRWLOCK_T*, unsigned long, unsigned long);
}
#endif // _WINDOWS_
#endif // LIMX_PLATFORM_WINDOWS

namespace Limx
{

// ============================================================================
// FMutex — 独占互斥锁
// ============================================================================

/// 独占互斥锁 — 基于 SRWLOCK (8 字节，无需销毁)
class FMutex
{
public:
    FMutex()
    {
#if LIMX_PLATFORM_WINDOWS
        InitializeSRWLock(&m_Lock);
#endif
    }

    // 不可拷贝/移动
    FMutex(const FMutex&) = delete;
    FMutex& operator=(const FMutex&) = delete;

    /// 获取独占锁 (阻塞)
    FORCEINLINE void Lock()
    {
#if LIMX_PLATFORM_WINDOWS
        AcquireSRWLockExclusive(&m_Lock);
#endif
    }

    /// 释放独占锁
    FORCEINLINE void Unlock()
    {
#if LIMX_PLATFORM_WINDOWS
        ReleaseSRWLockExclusive(&m_Lock);
#endif
    }

    /// 尝试获取独占锁 (非阻塞) — 返回是否成功
    LIMX_NODISCARD FORCEINLINE bool TryLock()
    {
#if LIMX_PLATFORM_WINDOWS
        return TryAcquireSRWLockExclusive(&m_Lock) != 0;
#else
        return false;
#endif
    }

private:
    friend class FConditionVariable;

#if LIMX_PLATFORM_WINDOWS
    SRWLOCK_T m_Lock;
#endif
};

// ============================================================================
// FRWLock — 读写锁
// ============================================================================

/// 读写锁 — 多读单写，基于 SRWLOCK
class FRWLock
{
public:
    FRWLock()
    {
#if LIMX_PLATFORM_WINDOWS
        InitializeSRWLock(&m_Lock);
#endif
    }

    FRWLock(const FRWLock&) = delete;
    FRWLock& operator=(const FRWLock&) = delete;

    /// 获取共享读锁 (阻塞)
    FORCEINLINE void ReadLock()
    {
#if LIMX_PLATFORM_WINDOWS
        AcquireSRWLockShared(&m_Lock);
#endif
    }

    /// 释放共享读锁
    FORCEINLINE void ReadUnlock()
    {
#if LIMX_PLATFORM_WINDOWS
        ReleaseSRWLockShared(&m_Lock);
#endif
    }

    /// 获取独占写锁 (阻塞)
    FORCEINLINE void WriteLock()
    {
#if LIMX_PLATFORM_WINDOWS
        AcquireSRWLockExclusive(&m_Lock);
#endif
    }

    /// 释放独占写锁
    FORCEINLINE void WriteUnlock()
    {
#if LIMX_PLATFORM_WINDOWS
        ReleaseSRWLockExclusive(&m_Lock);
#endif
    }

    /// 尝试获取共享读锁 (非阻塞)
    LIMX_NODISCARD FORCEINLINE bool TryReadLock()
    {
#if LIMX_PLATFORM_WINDOWS
        return TryAcquireSRWLockShared(&m_Lock) != 0;
#else
        return false;
#endif
    }

    /// 尝试获取独占写锁 (非阻塞)
    LIMX_NODISCARD FORCEINLINE bool TryWriteLock()
    {
#if LIMX_PLATFORM_WINDOWS
        return TryAcquireSRWLockExclusive(&m_Lock) != 0;
#else
        return false;
#endif
    }

private:
    friend class FConditionVariable;

#if LIMX_PLATFORM_WINDOWS
    SRWLOCK_T m_Lock;
#endif
};

// ============================================================================
// RAII 作用域锁
// ============================================================================

/// RAII 独占锁 — 构造时加锁，析构时解锁
class FScopeLock
{
public:
    explicit FScopeLock(FMutex& mutex)
        : m_Mutex(mutex)
    {
        m_Mutex.Lock();
    }

    ~FScopeLock()
    {
        m_Mutex.Unlock();
    }

    FScopeLock(const FScopeLock&) = delete;
    FScopeLock& operator=(const FScopeLock&) = delete;

private:
    FMutex& m_Mutex;
};

/// RAII 共享读锁
class FScopeReadLock
{
public:
    explicit FScopeReadLock(FRWLock& lock)
        : m_Lock(lock)
    {
        m_Lock.ReadLock();
    }

    ~FScopeReadLock()
    {
        m_Lock.ReadUnlock();
    }

    FScopeReadLock(const FScopeReadLock&) = delete;
    FScopeReadLock& operator=(const FScopeReadLock&) = delete;

private:
    FRWLock& m_Lock;
};

/// RAII 独占写锁
class FScopeWriteLock
{
public:
    explicit FScopeWriteLock(FRWLock& lock)
        : m_Lock(lock)
    {
        m_Lock.WriteLock();
    }

    ~FScopeWriteLock()
    {
        m_Lock.WriteUnlock();
    }

    FScopeWriteLock(const FScopeWriteLock&) = delete;
    FScopeWriteLock& operator=(const FScopeWriteLock&) = delete;

private:
    FRWLock& m_Lock;
};

// ============================================================================
// FConditionVariable — 条件变量
// ============================================================================

/// 条件变量 — 线程间等待/通知机制
class FConditionVariable
{
public:
    FConditionVariable()
    {
#if LIMX_PLATFORM_WINDOWS
        InitializeConditionVariable(&m_CondVar);
#endif
    }

    FConditionVariable(const FConditionVariable&) = delete;
    FConditionVariable& operator=(const FConditionVariable&) = delete;

    /// 唤醒一个等待线程
    void NotifyOne()
    {
#if LIMX_PLATFORM_WINDOWS
        WakeConditionVariable(&m_CondVar);
#endif
    }

    /// 唤醒所有等待线程
    void NotifyAll()
    {
#if LIMX_PLATFORM_WINDOWS
        WakeAllConditionVariable(&m_CondVar);
#endif
    }

    /// 等待 — 释放互斥锁并挂起，被唤醒后重新获取锁
    /// @param mutex 已加锁的互斥锁
    void Wait(FMutex& mutex)
    {
#if LIMX_PLATFORM_WINDOWS
        // INFINITE = 0xFFFFFFFF, Flags = 0 (exclusive)
        SleepConditionVariableSRW(
            &m_CondVar, &mutex.m_Lock, 0xFFFFFFFF, 0);
#endif
    }

    /// 带谓词的等待 — 避免虚假唤醒
    template<typename Predicate>
    void Wait(FMutex& mutex, Predicate predicate)
    {
        while (!predicate())
        {
            Wait(mutex);
        }
    }

    /// 限时等待 (毫秒) — 返回是否被唤醒 (非超时)
    LIMX_NODISCARD bool WaitFor(FMutex& mutex, UInt32 timeoutMs)
    {
#if LIMX_PLATFORM_WINDOWS
        return SleepConditionVariableSRW(
            &m_CondVar, &mutex.m_Lock, timeoutMs, 0) != 0;
#else
        return false;
#endif
    }

private:
#if LIMX_PLATFORM_WINDOWS
    CONDITION_VARIABLE_T m_CondVar;
#endif
};

} // namespace Limx
