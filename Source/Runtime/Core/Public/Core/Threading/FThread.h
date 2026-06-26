/*******************************************************************************
 * 文件: FThread.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   线程封装 — 零 STL 依赖的 OS 线程管理
 *   封装 Windows CreateThread API，提供创建/等待/分离等操作
 *   支持线程名称设置 (调试器可见)、栈大小配置、线程亲和性
 *
 * 设计哲学:
 *   RAII — 析构时自动 Join 未分离的线程，防止孤儿线程
 *   可调用对象 — 接受 TFunction 作为线程入口，支持 lambda/成员函数
 *   不可拷贝 — 线程是独占资源，仅支持移动语义
 *
 * 技术特性:
 *   - CreateThread: Windows 线程创建
 *   - Join: 等待线程结束
 *   - Detach: 分离线程 (不再管理)
 *   - SetThreadName: 通过 SetThreadDescription 设置调试器可见名称
 *   - GetCurrentThreadId: 获取当前线程 ID
 *   - Sleep: 挂起当前线程指定毫秒
 *   - Yield: 让出当前时间片
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h,
 *          Core/Templates/TFunction.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Templates/TFunction.h"

// Windows 线程 API 前向声明
#if LIMX_PLATFORM_WINDOWS
#ifndef _WINDOWS_
extern "C"
{
    // 线程创建/管理
    void* __stdcall CreateThread(
        void* lpThreadAttributes,
        unsigned long long dwStackSize,
        unsigned long (__stdcall* lpStartAddress)(void*),
        void* lpParameter,
        unsigned long dwCreationFlags,
        unsigned long* lpThreadId);

    unsigned long __stdcall WaitForSingleObject(
        void* hHandle, unsigned long dwMilliseconds);

    int __stdcall CloseHandle(void* hObject);

    // 线程控制
    void __stdcall Sleep(unsigned long dwMilliseconds);
    int  __stdcall SwitchToThread(void);
    unsigned long __stdcall GetCurrentThreadId(void);

    // 线程名称 (Windows 10 1607+)
    long __stdcall SetThreadDescription(void* hThread,
                                         const wchar_t* lpThreadDescription);
}
#endif // _WINDOWS_
#endif

namespace Limx
{

/// 线程优先级
enum class ThreadPriority : UInt8
{
    Lowest   = 0,
    Low      = 1,
    Normal   = 2,
    High     = 3,
    Highest  = 4,
    Critical = 5
};

/// 线程封装 — RAII 管理的 OS 线程
class FThread
{
public:
    /// 默认栈大小 (0 = OS 默认, 通常 1 MB)
    static constexpr UInt32 kDefaultStackSize = 0;

    // ========================================================================
    // 构造与析构
    // ========================================================================

    /// 默认构造 — 未启动
    FThread()
        : m_Handle(nullptr)
        , m_ThreadId(0)
        , m_IsJoinable(false)
    {
    }

    /// 从可调用对象构造并立即启动线程
    explicit FThread(TFunction<void()>&& threadFunc,
                     UInt32 stackSize = kDefaultStackSize)
        : m_Handle(nullptr)
        , m_ThreadId(0)
        , m_IsJoinable(false)
    {
        Start(MoveTemp(threadFunc), stackSize);
    }

    /// 移动构造
    FThread(FThread&& other) noexcept
        : m_Handle(other.m_Handle)
        , m_ThreadId(other.m_ThreadId)
        , m_IsJoinable(other.m_IsJoinable)
    {
        other.m_Handle = nullptr;
        other.m_ThreadId = 0;
        other.m_IsJoinable = false;
    }

    /// 析构 — 自动 Join 未分离的线程
    ~FThread()
    {
        if (m_IsJoinable)
        {
            Join();
        }
    }

    /// 移动赋值
    FThread& operator=(FThread&& other) noexcept
    {
        if (this != &other)
        {
            if (m_IsJoinable)
            {
                Join();
            }
            m_Handle = other.m_Handle;
            m_ThreadId = other.m_ThreadId;
            m_IsJoinable = other.m_IsJoinable;
            other.m_Handle = nullptr;
            other.m_ThreadId = 0;
            other.m_IsJoinable = false;
        }
        return *this;
    }

    // 不可拷贝
    FThread(const FThread&) = delete;
    FThread& operator=(const FThread&) = delete;

    // ========================================================================
    // 线程控制
    // ========================================================================

    /// 启动线程
    void Start(TFunction<void()>&& threadFunc,
               UInt32 stackSize = kDefaultStackSize)
    {
        LIMX_ASSERT(!m_IsJoinable);

        // 在堆上分配 TFunction — 线程入口函数需要持有所有权
        auto* context = new ThreadContext();
        context->Function = MoveTemp(threadFunc);

#if LIMX_PLATFORM_WINDOWS
        unsigned long threadId = 0;
        m_Handle = CreateThread(
            nullptr,
            static_cast<unsigned long long>(stackSize),
            &ThreadEntryPoint,
            context,
            0,
            &threadId);

        if (m_Handle)
        {
            m_ThreadId = static_cast<UInt32>(threadId);
            m_IsJoinable = true;
        }
        else
        {
            delete context;
            LIMX_ASSERT(false);
        }
#endif
    }

    /// 等待线程结束 (阻塞)
    void Join()
    {
        if (m_IsJoinable && m_Handle)
        {
#if LIMX_PLATFORM_WINDOWS
            // INFINITE = 0xFFFFFFFF
            WaitForSingleObject(m_Handle, 0xFFFFFFFF);
            CloseHandle(m_Handle);
#endif
            m_Handle = nullptr;
            m_ThreadId = 0;
            m_IsJoinable = false;
        }
    }

    /// 分离线程 — 不再管理，线程结束时自动清理
    void Detach()
    {
        if (m_IsJoinable && m_Handle)
        {
#if LIMX_PLATFORM_WINDOWS
            CloseHandle(m_Handle);
#endif
            m_Handle = nullptr;
            m_ThreadId = 0;
            m_IsJoinable = false;
        }
    }

    /// 是否可 Join
    LIMX_NODISCARD bool IsJoinable() const { return m_IsJoinable; }

    /// 获取线程 ID
    LIMX_NODISCARD UInt32 GetThreadId() const { return m_ThreadId; }

    /// 获取原生句柄
    LIMX_NODISCARD void* GetNativeHandle() const { return m_Handle; }

    // ========================================================================
    // 静态工具
    // ========================================================================

    /// 获取当前线程 ID
    LIMX_NODISCARD static UInt32 CurrentThreadId()
    {
#if LIMX_PLATFORM_WINDOWS
        return static_cast<UInt32>(::GetCurrentThreadId());
#else
        return 0;
#endif
    }

    /// 挂起当前线程指定毫秒
    static void SleepMs(UInt32 milliseconds)
    {
#if LIMX_PLATFORM_WINDOWS
        ::Sleep(static_cast<unsigned long>(milliseconds));
#endif
    }

    /// 让出当前时间片
    static void Yield()
    {
#if LIMX_PLATFORM_WINDOWS
        SwitchToThread();
#endif
    }

    /// 获取硬件并发数 (逻辑 CPU 数)
    LIMX_NODISCARD static UInt32 HardwareConcurrency();

private:
    // ========================================================================
    // 线程上下文
    // ========================================================================

    struct ThreadContext
    {
        TFunction<void()> Function;
    };

    /// Windows 线程入口点 (静态 C 调用约定)
    static unsigned long __stdcall ThreadEntryPoint(void* parameter)
    {
        auto* context = static_cast<ThreadContext*>(parameter);
        if (context->Function)
        {
            context->Function();
        }
        delete context;
        return 0;
    }

    // ========================================================================
    // 成员数据
    // ========================================================================

    void*  m_Handle;      ///< OS 线程句柄
    UInt32 m_ThreadId;    ///< 线程 ID
    bool   m_IsJoinable;  ///< 是否可 Join
};

} // namespace Limx
