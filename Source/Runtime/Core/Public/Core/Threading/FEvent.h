/*******************************************************************************
 * 文件: FEvent.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   事件信号 — 线程间等待/通知的同步原语
 *   封装 Windows CreateEvent/SetEvent/ResetEvent/WaitForSingleObject
 *   支持手动重置和自动重置两种模式
 *
 * 设计哲学:
 *   RAII — 构造时创建事件，析构时自动关闭句柄
 *   双模式 — 手动重置 (广播) 和自动重置 (单次唤醒)
 *   不可拷贝 — 事件句柄是独占资源
 *
 * 技术特性:
 *   - ManualReset: 保持信号状态直到显式 Reset()
 *   - AutoReset: Wait 成功后自动重置为非信号状态
 *   - Trigger/Reset: 设置/清除信号
 *   - Wait/WaitFor: 阻塞等待/限时等待
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h
 *   外部: Windows API (CreateEventW, SetEvent, ResetEvent, WaitForSingleObject)
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"

// Windows 事件 API 前向声明
#if LIMX_PLATFORM_WINDOWS
#ifndef _WINDOWS_
extern "C"
{
    void* __stdcall CreateEventW(void* lpEventAttributes,
                                  int bManualReset,
                                  int bInitialState,
                                  const wchar_t* lpName);
    int __stdcall SetEvent(void* hEvent);
    int __stdcall ResetEvent(void* hEvent);
    unsigned long __stdcall WaitForSingleObject(void* hHandle,
                                                 unsigned long dwMilliseconds);
    int __stdcall CloseHandle(void* hObject);
}
#endif // _WINDOWS_
#endif

namespace Limx
{

/// 事件重置模式
enum class EventResetMode : UInt8
{
    ManualReset = 0,  ///< 手动重置 — 保持信号直到 Reset()
    AutoReset   = 1   ///< 自动重置 — Wait 成功后自动重置
};

/// 事件信号 — 线程间等待/通知
class FEvent
{
public:
    // ========================================================================
    // 构造与析构
    // ========================================================================

    /// 构造事件
    /// @param resetMode    重置模式 (手动/自动)
    /// @param initialState 初始状态 (true = 已信号)
    explicit FEvent(EventResetMode resetMode = EventResetMode::AutoReset,
                    bool initialState = false)
        : m_Handle(nullptr)
    {
#if LIMX_PLATFORM_WINDOWS
        m_Handle = CreateEventW(
            nullptr,
            resetMode == EventResetMode::ManualReset ? 1 : 0,
            initialState ? 1 : 0,
            nullptr);
        LIMX_ASSERT(m_Handle != nullptr);
#endif
    }

    ~FEvent()
    {
        if (m_Handle)
        {
#if LIMX_PLATFORM_WINDOWS
            CloseHandle(m_Handle);
#endif
        }
    }

    // 不可拷贝
    FEvent(const FEvent&) = delete;
    FEvent& operator=(const FEvent&) = delete;

    // 可移动
    FEvent(FEvent&& other) noexcept
        : m_Handle(other.m_Handle)
    {
        other.m_Handle = nullptr;
    }

    FEvent& operator=(FEvent&& other) noexcept
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
    // 信号控制
    // ========================================================================

    /// 触发事件 — 设置为信号状态
    void Trigger()
    {
#if LIMX_PLATFORM_WINDOWS
        SetEvent(m_Handle);
#endif
    }

    /// 重置事件 — 设置为非信号状态 (仅手动重置模式需要)
    void Reset()
    {
#if LIMX_PLATFORM_WINDOWS
        ResetEvent(m_Handle);
#endif
    }

    // ========================================================================
    // 等待
    // ========================================================================

    /// 无限等待 — 直到事件被触发
    void Wait()
    {
#if LIMX_PLATFORM_WINDOWS
        WaitForSingleObject(m_Handle, 0xFFFFFFFF);
#endif
    }

    /// 限时等待 — 返回是否被触发 (非超时)
    /// @param timeoutMs 超时毫秒数
    LIMX_NODISCARD bool WaitFor(UInt32 timeoutMs)
    {
#if LIMX_PLATFORM_WINDOWS
        // WAIT_OBJECT_0 = 0
        return WaitForSingleObject(
            m_Handle, static_cast<unsigned long>(timeoutMs)) == 0;
#else
        return false;
#endif
    }

    /// 检查是否已触发 (非阻塞)
    LIMX_NODISCARD bool IsTriggered()
    {
        return WaitFor(0);
    }

    /// 获取原生句柄
    LIMX_NODISCARD void* GetNativeHandle() const { return m_Handle; }

private:
    void* m_Handle;  ///< OS 事件句柄
};

} // namespace Limx
