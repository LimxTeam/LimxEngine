// ============================================================
// 文件名称：FInputManager.cpp
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：轮询式输入 — 每帧查询状态，无回调注册。
// 功能描述：FInputManager 实现 — 全局单例、按键/鼠标状态更新、
//          帧间 Delta 累加与重置。
// 技术特性：静态局部单例 (Meyer's Singleton)；
//          256 键位 bool 数组，零堆分配；
//          BeginFrame 每帧重置 Delta。
//
// ── 函数表 ──────────────────────────────────────────────────
// │ 函数名                      │ 描述                           │
// │──────────────────────────────│───────────────────────────────│
// │ Get()                      │ Meyer's Singleton              │
// │ BeginFrame()               │ 重置帧间 Delta                  │
// │ OnKeyDown/Up()             │ 更新键位状态                    │
// │ OnMouseButtonDown/Up()     │ 更新鼠标按钮状态                │
// │ OnMouseMove()              │ 更新鼠标坐标                    │
// │ OnRawMouseDelta()          │ 累加原始鼠标增量                │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建                        │
// ============================================================

#include "ApplicationCore/Input/FInputManager.h"

namespace Limx
{

// ============================================================================
// 构造
// ============================================================================

FInputManager::FInputManager()
{
    Memory::MemZero(m_KeyStates, sizeof(m_KeyStates));
    Memory::MemZero(m_MouseButtons, sizeof(m_MouseButtons));
}

// ============================================================================
// Get — 全局单例 (Meyer's Singleton)
// ============================================================================

FInputManager& FInputManager::Get()
{
    static FInputManager instance;
    return instance;
}

// ============================================================================
// BeginFrame — 帧开始时重置 Delta 累加器
// ============================================================================

void FInputManager::BeginFrame()
{
    m_MouseDeltaX = 0.0f;
    m_MouseDeltaY = 0.0f;
}

// ============================================================================
// 键盘事件
// ============================================================================

void FInputManager::OnKeyDown(UInt8 virtualKey)
{
    m_KeyStates[virtualKey] = true;
}

void FInputManager::OnKeyUp(UInt8 virtualKey)
{
    m_KeyStates[virtualKey] = false;
}

// ============================================================================
// 鼠标按钮事件
// ============================================================================

void FInputManager::OnMouseButtonDown(EMouseButton button)
{
    UInt8 index = static_cast<UInt8>(button);
    if (index < kMouseButtonCount)
    {
        m_MouseButtons[index] = true;
    }
}

void FInputManager::OnMouseButtonUp(EMouseButton button)
{
    UInt8 index = static_cast<UInt8>(button);
    if (index < kMouseButtonCount)
    {
        m_MouseButtons[index] = false;
    }
}

// ============================================================================
// 鼠标移动事件
// ============================================================================

void FInputManager::OnMouseMove(Float32 positionX, Float32 positionY)
{
    m_MousePositionX = positionX;
    m_MousePositionY = positionY;
}

// ============================================================================
// 原始鼠标增量 — 帧内累加，BeginFrame 时重置
// ============================================================================

void FInputManager::OnRawMouseDelta(Float32 deltaX, Float32 deltaY)
{
    m_MouseDeltaX += deltaX;
    m_MouseDeltaY += deltaY;
}

} // namespace Limx
