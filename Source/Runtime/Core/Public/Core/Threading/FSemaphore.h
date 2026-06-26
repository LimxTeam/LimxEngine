/*******************************************************************************
 * 文件: FSemaphore.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   计数信号量 — 控制对有限资源的并发访问
 *   封装 Windows CreateSemaphore API
 *   用于限制线程池并发数、生产者-消费者缓冲区等场景
 *
 * 设计哲学:
 *   RAII — 构造时创建信号量，析构时自动关闭句柄
 *   计数语义 — 初始计数 = 可用资源数，Acquire 减 1，Release 加 1
 *   不可拷贝 — 信号量句柄是独占资源
 *
 * 技术特性:
 *   - Acquire: 等待信号量 (计数 > 0 时递减并继续)
 *   - TryAcquire: 非阻塞尝试获取
 *   - Release: 释放信号量 (递增计数)
 *   - 可配置初始计数和最大计数
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h
 *   外部: Windows API (CreateSemaphoreW, ReleaseSemaphore, WaitForSingleObject)
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"

// Windows 信号量 API 前向声明
#if LIMX_PLATFORM_WINDOWS
#ifndef _WINDOWS_
extern "C"
{
    void* __stdcall CreateSemaphoreW(void* lpSemaphoreAttributes,
                                      long lInitialCount,
                                      long lMaximumCount,
                                      const wchar_t* lpName);
    int __stdcall ReleaseSemaphore(void* hSemaphore,
                                    long lReleaseCount,
                                    long* lpPreviousCount);
    unsigned long __stdcall WaitForSingleObject(void* hHandle,
                                                 unsigned long dwMilliseconds);
    int __stdcall CloseHandle(void* hObject);
}
#endif // _WINDOWS_
#endif

namespace Limx
{

/// 计数信号量 — 控制并发资源访问
class FSemaphore
{
public:
    // ========================================================================
    // 构造与析构
    // ========================================================================

    /// 构造信号量
    /// @param initialCount 初始可用计数
    /// @param maxCount     最大计数
    explicit FSemaphore(Int32 initialCount, Int32 maxCount)
        : m_Handle(nullptr)
    {
        LIMX_ASSERT(initialCount >= 0);
        LIMX_ASSERT(maxCount > 0);
        LIMX_ASSERT(initialCount <= maxCount);

#if LIMX_PLATFORM_WINDOWS
        m_Handle = CreateSemaphoreW(
            nullptr,
            static_cast<long>(initialCount),
            static_cast<long>(maxCount),
            nullptr);
        LIMX_ASSERT(m_Handle != nullptr);
#endif
    }

    /// 便捷构造 — 初始计数 = 最大计数
    explicit FSemaphore(Int32 count)
        : FSemaphore(count, count)
    {
    }

    ~FSemaphore()
    {
        if (m_Handle)
        {
#if LIMX_PLATFORM_WINDOWS
            CloseHandle(m_Handle);
#endif
        }
    }

    // 不可拷贝
    FSemaphore(const FSemaphore&) = delete;
    FSemaphore& operator=(const FSemaphore&) = delete;

    // 可移动
    FSemaphore(FSemaphore&& other) noexcept
        : m_Handle(other.m_Handle)
    {
        other.m_Handle = nullptr;
    }

    FSemaphore& operator=(FSemaphore&& other) noexcept
    {
        if (this != &other)
        {
            if (m_Handle)
            {
#if LIMX_PLATFORM_WINDOWS
                CloseHandle(m_Handle);
#endif
            }
            m_Handle = other.m_Handle;
            other.m_Handle = nullptr;
        }
        return *this;
    }

    // ========================================================================
    // 获取与释放
    // ========================================================================

    /// 获取信号量 (阻塞) — 计数减 1
    void Acquire()
    {
#if LIMX_PLATFORM_WINDOWS
        WaitForSingleObject(m_Handle, 0xFFFFFFFF);
#endif
    }

    /// 尝试获取信号量 (非阻塞) — 返回是否成功
    LIMX_NODISCARD bool TryAcquire()
    {
#if LIMX_PLATFORM_WINDOWS
        return WaitForSingleObject(m_Handle, 0) == 0;
#else
        return false;
#endif
    }

    /// 限时获取 — 返回是否成功
    LIMX_NODISCARD bool TryAcquireFor(UInt32 timeoutMs)
    {
#if LIMX_PLATFORM_WINDOWS
        return WaitForSingleObject(
            m_Handle, static_cast<unsigned long>(timeoutMs)) == 0;
#else
        return false;
#endif
    }

    /// 释放信号量 — 计数加 releaseCount
    /// @param releaseCount 释放数量 (默认 1)
    void Release(Int32 releaseCount = 1)
    {
        LIMX_ASSERT(releaseCount > 0);
#if LIMX_PLATFORM_WINDOWS
        ReleaseSemaphore(m_Handle, static_cast<long>(releaseCount), nullptr);
#endif
    }

    /// 获取原生句柄
    LIMX_NODISCARD void* GetNativeHandle() const { return m_Handle; }

private:
    void* m_Handle;  ///< OS 信号量句柄
};

} // namespace Limx
