/*******************************************************************************
 * 文件: FSpinLock.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   自旋锁 — 轻量级线程同步原语
 *   在短临界区场景下比互斥锁更高效，避免内核态切换
 *   用于低竞争的短临界区保护、无锁数据结构辅助锁等场景
 *
 * 设计哲学:
 *   忙等待 — 循环 CAS 尝试获取，不进入内核等待
 *   Pause 指令 — 自旋时插入 _mm_pause 降低 CPU 功耗
 *   不可重入 — 同一线程重复 Lock 会死锁
 *
 * 技术特性:
 *   - FSpinLock: 自旋锁
 *   - Lock/Unlock: 加锁/解锁
 *   - TryLock: 尝试加锁 (不阻塞)
 *   - TScopedLock<FSpinLock>: RAII 锁守卫
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Misc/FAtomicCounter.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"

// _mm_pause 内建函数
#if LIMX_COMPILER_MSVC
extern "C" void _mm_pause();
#pragma intrinsic(_mm_pause)
#endif

// 原子操作内建函数 (可能已在 FAtomicCounter.h 中声明)
#if LIMX_COMPILER_MSVC
extern "C"
{
    long __cdecl _InterlockedExchange(long volatile*, long);
    long __cdecl _InterlockedCompareExchange(
        long volatile*, long, long);
}
#pragma intrinsic(_InterlockedExchange)
#pragma intrinsic(_InterlockedCompareExchange)
#endif

namespace Limx
{

/// 自旋锁
class FSpinLock
{
public:
    FSpinLock() : m_Lock(0) {}

    // 不可拷贝/移动
    FSpinLock(const FSpinLock&) = delete;
    FSpinLock& operator=(const FSpinLock&) = delete;
    FSpinLock(FSpinLock&&) = delete;
    FSpinLock& operator=(FSpinLock&&) = delete;

    /// 加锁 (忙等待)
    void Lock()
    {
#if LIMX_COMPILER_MSVC
        while (_InterlockedExchange(
            reinterpret_cast<long volatile*>(&m_Lock), 1) != 0)
        {
            // 自旋等待 — 先检查再 CAS，减少总线竞争
            while (m_Lock != 0)
            {
                _mm_pause();
            }
        }
#else
        LIMX_ASSERT(false); // 非 MSVC 平台未实现
#endif
    }

    /// 解锁
    void Unlock()
    {
#if LIMX_COMPILER_MSVC
        _InterlockedExchange(
            reinterpret_cast<long volatile*>(&m_Lock), 0);
#else
        m_Lock = 0;
#endif
    }

    /// 尝试加锁 (不阻塞)
    /// @return 是否成功获取锁
    LIMX_NODISCARD bool TryLock()
    {
#if LIMX_COMPILER_MSVC
        return _InterlockedCompareExchange(
            reinterpret_cast<long volatile*>(&m_Lock),
            1, 0) == 0;
#else
        if (m_Lock == 0) { m_Lock = 1; return true; }
        return false;
#endif
    }

    /// 是否已锁定 (仅用于调试断言)
    LIMX_NODISCARD bool IsLocked() const
    {
        return m_Lock != 0;
    }

private:
    volatile Int32 m_Lock;  ///< 0=未锁定, 1=已锁定
};

/// RAII 锁守卫 — 构造时加锁，析构时解锁
/// @tparam LockType 锁类型 (需提供 Lock()/Unlock())
template<typename LockType>
class TScopedLock
{
public:
    explicit TScopedLock(LockType& lock)
        : m_Lock(lock)
    {
        m_Lock.Lock();
    }

    ~TScopedLock()
    {
        m_Lock.Unlock();
    }

    // 不可拷贝/移动
    TScopedLock(const TScopedLock&) = delete;
    TScopedLock& operator=(const TScopedLock&) = delete;
    TScopedLock(TScopedLock&&) = delete;
    TScopedLock& operator=(TScopedLock&&) = delete;

private:
    LockType& m_Lock;
};

} // namespace Limx
