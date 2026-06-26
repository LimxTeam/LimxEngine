// ============================================================
// 文件名称：FWindow.h
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：薄封装原则 — 仅封装 Win32 窗口创建与消息泵的必要
//          操作，不引入额外抽象层。窗口句柄以 void* 对外暴露，
//          避免上层模块依赖 <windows.h> 类型。
// 功能描述：Win32 原生窗口的创建、销毁、消息处理、尺寸追踪。
//          提供 GetNativeHandle() 供 RHI 设备创建 VkSurfaceKHR。
//          支持窗口尺寸变化检测，用于交换链重建触发。
// 技术特性：WNDCLASSEXW 注册 + CreateWindowExW 创建；
//          静态 WindowProc 回调通过 GWLP_USERDATA 路由到实例；
//          尺寸变化通过 WM_SIZE 消息追踪，最小化检测用于暂停渲染。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                      │ 描述                           │
// │────────────────────────────│───────────────────────────────│
// │ Create()                   │ 创建 Win32 窗口                 │
// │ Destroy()                  │ 销毁窗口并注销窗口类              │
// │ ProcessMessages()          │ 处理消息队列，返回是否继续运行      │
// │ GetNativeHandle()          │ 返回 HWND (void*)              │
// │ GetWidth()                 │ 返回客户区宽度                   │
// │ GetHeight()                │ 返回客户区高度                   │
// │ IsMinimized()              │ 窗口是否最小化                   │
// │ WasResized()               │ 自上次重置后是否发生尺寸变化       │
// │ ResetResizedFlag()         │ 清除尺寸变化标记                 │
//
// ── 结构体字段表 ──────────────────────────────────────────────
// │ 字段名                      │ 类型        │ 描述              │
// │────────────────────────────│────────────│──────────────────│
// │ m_Handle                   │ void*      │ HWND 窗口句柄      │
// │ m_Width                    │ UInt32     │ 客户区宽度          │
// │ m_Height                   │ UInt32     │ 客户区高度          │
// │ m_IsMinimized              │ bool       │ 最小化状态          │
// │ m_WasResized               │ bool       │ 尺寸变化标记        │
// │ m_ShouldClose              │ bool       │ 关闭请求标记        │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-06  │ LimxTeam  │ 初始创建                        │
// ============================================================

#pragma once

#include "ApplicationCore/ApplicationCoreMinimal.h"

namespace Limx
{

// ============================================================================
// FWindowDesc — 窗口创建描述符
// ============================================================================

struct FWindowDesc
{
    /// 客户区宽度 (像素)
    UInt32 Width = 1280;

    /// 客户区高度 (像素)
    UInt32 Height = 720;

    /// 窗口标题 (UTF-16 宽字符)
    const WideChar* Title = L"Limx Engine";

    /// 是否允许用户调整窗口大小
    bool IsResizable = true;

    /// 是否在创建后立即显示
    bool IsVisible = true;
};

// ============================================================================
// FWindow — Win32 原生窗口封装
// ============================================================================

class FWindow
{
public:
    LIMX_NON_COPYABLE(FWindow);

    FWindow() = default;
    ~FWindow();

    /// 创建窗口
    /// @param desc 窗口创建参数
    /// @return true 成功; false 窗口类注册或创建失败
    bool Create(const FWindowDesc& desc);

    /// 销毁窗口并注销窗口类
    void Destroy();

    /// 处理 Win32 消息队列
    /// @return true 窗口仍然存活; false 收到 WM_QUIT 或关闭请求
    bool ProcessMessages();

    /// 获取原生窗口句柄 (HWND)
    /// 用于 RHI 设备创建 VkSurfaceKHR
    LIMX_NODISCARD void* GetNativeHandle() const { return m_Handle; }

    /// 客户区宽度 (像素)
    LIMX_NODISCARD UInt32 GetWidth() const { return m_Width; }

    /// 客户区高度 (像素)
    LIMX_NODISCARD UInt32 GetHeight() const { return m_Height; }

    /// 窗口是否处于最小化状态
    /// 最小化时应暂停渲染以避免交换链尺寸为 0
    LIMX_NODISCARD bool IsMinimized() const { return m_IsMinimized; }

    /// 自上次 ResetResizedFlag() 后窗口尺寸是否发生变化
    /// 用于触发交换链重建
    LIMX_NODISCARD bool WasResized() const { return m_WasResized; }

    /// 清除尺寸变化标记 — 交换链重建完成后调用
    void ResetResizedFlag() { m_WasResized = false; }

    /// 窗口是否有效 (已创建且未关闭)
    LIMX_NODISCARD bool IsValid() const { return m_Handle != nullptr; }

private:
    /// Win32 窗口过程回调 — 静态函数，通过 GWLP_USERDATA 路由到实例
    static Int64 __stdcall WindowProcStatic(void* hwnd, UInt32 message,
                                             UInt64 wParam, Int64 lParam);

    /// 实例窗口过程 — 处理具体消息
    Int64 HandleMessage(void* hwnd, UInt32 message,
                        UInt64 wParam, Int64 lParam);

    void*  m_Handle      = nullptr;  // HWND
    UInt32 m_Width       = 0;
    UInt32 m_Height      = 0;
    bool   m_IsMinimized = false;
    bool   m_WasResized  = false;
    bool   m_ShouldClose = false;

    /// 窗口类名 — 用于注销
    static constexpr const WideChar* kWindowClassName = L"LimxWindowClass";

    /// 窗口类是否已注册 (进程生命周期内只注册一次)
    static bool s_IsClassRegistered;
};

} // namespace Limx
