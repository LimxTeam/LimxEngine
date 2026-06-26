// ============================================================
// 文件名称：FInputManager.h
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：轮询式输入 — 每帧查询按键/鼠标状态，无回调注册，
//          简单直接，适合渲染循环驱动的实时应用。
// 功能描述：键盘和鼠标输入状态管理器 — 跟踪当前帧按键按下/释放状态、
//          鼠标按钮状态、鼠标位置和帧间位移 (Delta)。
//          由 FWindow 消息处理器在收到 WM_KEY*/WM_MOUSE* 时调用
//          OnKey*/OnMouse* 方法更新内部状态。
// 技术特性：256 键位静态数组，零堆分配；
//          左/右/中/侧键鼠标按钮状态；
//          每帧 BeginFrame 重置 Delta 累加器；
//          全局单例访问 (Get())。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                    │ 描述                           │
// │──────────────────────────│───────────────────────────────│
// │ Get()                    │ 获取全局单例引用                  │
// │ BeginFrame()             │ 帧开始时重置 Delta 累加器         │
// │ OnKeyDown()              │ WM_KEYDOWN/WM_SYSKEYDOWN 回调  │
// │ OnKeyUp()                │ WM_KEYUP/WM_SYSKEYUP 回调      │
// │ OnMouseButtonDown()      │ 鼠标按钮按下回调                  │
// │ OnMouseButtonUp()        │ 鼠标按钮释放回调                  │
// │ OnMouseMove()            │ WM_MOUSEMOVE 回调               │
// │ OnRawMouseDelta()        │ WM_INPUT 原始鼠标增量回调         │
// │ IsKeyDown()              │ 查询按键是否按下                  │
// │ IsMouseButtonDown()      │ 查询鼠标按钮是否按下              │
// │ GetMousePosition()       │ 获取鼠标客户区坐标                │
// │ GetMouseDelta()          │ 获取帧间鼠标位移量                │
//
// ── 结构体字段表 ──────────────────────────────────────────────
// │ 字段名                    │ 类型               │ 描述        │
// │──────────────────────────│──────────────────│────────────│
// │ m_KeyStates              │ bool[256]         │ 按键状态数组  │
// │ m_MouseButtons           │ bool[5]           │ 鼠标按钮状态  │
// │ m_MousePositionX         │ Float32           │ 鼠标 X 坐标  │
// │ m_MousePositionY         │ Float32           │ 鼠标 Y 坐标  │
// │ m_MouseDeltaX            │ Float32           │ 帧间 X 位移  │
// │ m_MouseDeltaY            │ Float32           │ 帧间 Y 位移  │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建                        │
// ============================================================

#pragma once

#include "ApplicationCore/ApplicationCoreMinimal.h"

namespace Limx
{

// ============================================================================
// EMouseButton — 鼠标按钮枚举
// ============================================================================

#ifndef LIMX_EMOUSEBUTTON_DEFINED
#define LIMX_EMOUSEBUTTON_DEFINED
enum class EMouseButton : UInt8
{
    Left   = 0,
    Right  = 1,
    Middle = 2,
    Thumb1 = 3,
    Thumb2 = 4,
    None   = 255,
};
#endif

// ============================================================================
// FInputManager — 键盘和鼠标输入状态管理器
// ============================================================================

class FInputManager
{
public:
    LIMX_NON_COPYABLE(FInputManager);

    /// 获取全局单例引用
    static FInputManager& Get();

    /// 帧开始时重置 Delta 累加器
    void BeginFrame();

    // ---- 键盘事件 (由 FWindow 消息处理器调用) ----

    /// WM_KEYDOWN / WM_SYSKEYDOWN 回调
    void OnKeyDown(UInt8 virtualKey);

    /// WM_KEYUP / WM_SYSKEYUP 回调
    void OnKeyUp(UInt8 virtualKey);

    // ---- 鼠标事件 (由 FWindow 消息处理器调用) ----

    /// 鼠标按钮按下回调
    void OnMouseButtonDown(EMouseButton button);

    /// 鼠标按钮释放回调
    void OnMouseButtonUp(EMouseButton button);

    /// WM_MOUSEMOVE 回调 (客户区坐标)
    void OnMouseMove(Float32 positionX, Float32 positionY);

    /// WM_INPUT 原始鼠标增量回调 (不受 DPI 缩放影响)
    void OnRawMouseDelta(Float32 deltaX, Float32 deltaY);

    // ---- 状态查询 ----

    /// 查询按键是否按下
    LIMX_NODISCARD bool IsKeyDown(UInt8 virtualKey) const
    {
        return m_KeyStates[virtualKey];
    }

    /// 查询鼠标按钮是否按下
    LIMX_NODISCARD bool IsMouseButtonDown(EMouseButton button) const
    {
        UInt8 index = static_cast<UInt8>(button);
        return index < kMouseButtonCount && m_MouseButtons[index];
    }

    /// 查询是否任意鼠标按钮处于按下状态
    LIMX_NODISCARD bool IsAnyMouseButtonDown() const
    {
        for (UInt8 i = 0; i < kMouseButtonCount; ++i)
        {
            if (m_MouseButtons[i])
            {
                return true;
            }
        }
        return false;
    }

    /// 获取鼠标客户区坐标
    LIMX_NODISCARD Float32 GetMousePositionX() const
    {
        return m_MousePositionX;
    }
    LIMX_NODISCARD Float32 GetMousePositionY() const
    {
        return m_MousePositionY;
    }

    /// 获取帧间鼠标位移量
    LIMX_NODISCARD Float32 GetMouseDeltaX() const
    {
        return m_MouseDeltaX;
    }
    LIMX_NODISCARD Float32 GetMouseDeltaY() const
    {
        return m_MouseDeltaY;
    }

private:
    static constexpr UInt8 kMouseButtonCount = 5;

    FInputManager();
    ~FInputManager() = default;

    // 256 键位静态数组 (Win32 Virtual-Key Codes 范围 0-255)
    bool    m_KeyStates[256] = {};

    // 鼠标按钮: 左/右/中/侧键1/侧键2
    bool    m_MouseButtons[kMouseButtonCount] = {};

    // 鼠标客户区坐标
    Float32 m_MousePositionX = 0.0f;
    Float32 m_MousePositionY = 0.0f;

    // 帧间鼠标位移 (每帧 BeginFrame 时重置)
    Float32 m_MouseDeltaX = 0.0f;
    Float32 m_MouseDeltaY = 0.0f;
};

} // namespace Limx
