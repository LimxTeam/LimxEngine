// ============================================================
// 文件名称：FCursorManager.cpp
// 创建时间：2026-04-08
// 创建者  ：LimxTeam
// 设计哲学：Win32 光标管理 — LoadCursor 预加载、SetCursor 切换、
//          ClipCursor 限制、ShowCursor 引用计数
// 功能描述：FCursorManager 实现
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-08  │ LimxTeam  │ 初始创建                        │
// ============================================================

#include "ApplicationCore/ApplicationCoreMinimal.h"
#include "ApplicationCore/Cursor/FCursorManager.h"

// Win32 光标常量
#ifndef IDC_ARROW
    #define IDC_ARROW       ((const WideChar*)32512)
    #define IDC_IBEAM       ((const WideChar*)32513)
    #define IDC_WAIT        ((const WideChar*)32514)
    #define IDC_CROSS       ((const WideChar*)32515)
    #define IDC_SIZEALL     ((const WideChar*)32646)
    #define IDC_SIZENWSE    ((const WideChar*)32642)
    #define IDC_SIZENESW    ((const WideChar*)32643)
    #define IDC_SIZEWE      ((const WideChar*)32644)
    #define IDC_SIZENS      ((const WideChar*)32645)
    #define IDC_HAND        ((const WideChar*)32649)
    #define IDC_NO          ((const WideChar*)32648)
#endif

namespace Limx
{

LIMX_DECLARE_LOG_CATEGORY(LogApplicationCore);

// ============================================================================
// 单例
// ============================================================================

FCursorManager& FCursorManager::Get()
{
    static FCursorManager instance;
    return instance;
}

FCursorManager::FCursorManager()
{
    Memory::MemZero(m_SystemCursors, sizeof(m_SystemCursors));
    LoadSystemCursors();
}

FCursorManager::~FCursorManager() = default;

// ============================================================================
// 加载系统光标
// ============================================================================

void FCursorManager::LoadSystemCursors()
{
    // 加载标准 Win32 系统光标
    m_SystemCursors[static_cast<SizeType>(ECursorType::Arrow)]
        = LoadCursorW(nullptr, IDC_ARROW);

    m_SystemCursors[static_cast<SizeType>(ECursorType::TextEditBeam)]
        = LoadCursorW(nullptr, IDC_IBEAM);

    m_SystemCursors[static_cast<SizeType>(ECursorType::ResizeLeftRight)]
        = LoadCursorW(nullptr, IDC_SIZEWE);

    m_SystemCursors[static_cast<SizeType>(ECursorType::ResizeUpDown)]
        = LoadCursorW(nullptr, IDC_SIZENS);

    m_SystemCursors[static_cast<SizeType>(ECursorType::ResizeSouthEast)]
        = LoadCursorW(nullptr, IDC_SIZENWSE);

    m_SystemCursors[static_cast<SizeType>(ECursorType::ResizeSouthWest)]
        = LoadCursorW(nullptr, IDC_SIZENESW);

    m_SystemCursors[static_cast<SizeType>(ECursorType::CardinalCross)]
        = LoadCursorW(nullptr, IDC_SIZEALL);

    m_SystemCursors[static_cast<SizeType>(ECursorType::Crosshairs)]
        = LoadCursorW(nullptr, IDC_CROSS);

    m_SystemCursors[static_cast<SizeType>(ECursorType::Hand)]
        = LoadCursorW(nullptr, IDC_HAND);

    // GrabHand 使用 SizeAll 替代（Win32 无原生抓取手型）
    m_SystemCursors[static_cast<SizeType>(ECursorType::GrabHand)]
        = LoadCursorW(nullptr, IDC_SIZEALL);

    m_SystemCursors[static_cast<SizeType>(ECursorType::SlashedCircle)]
        = LoadCursorW(nullptr, IDC_NO);

    // EyeDropper 使用 Cross 替代（Win32 无原生取色器光标）
    m_SystemCursors[static_cast<SizeType>(ECursorType::EyeDropper)]
        = LoadCursorW(nullptr, IDC_CROSS);

    m_SystemCursors[static_cast<SizeType>(ECursorType::Wait)]
        = LoadCursorW(nullptr, IDC_WAIT);

    // None 不加载光标
    m_SystemCursors[static_cast<SizeType>(ECursorType::None)] = nullptr;

    LIMX_LOG(LogApplicationCore, Log,
             "FCursorManager: 系统光标资源已加载");
}

// ============================================================================
// 光标形状
// ============================================================================

void FCursorManager::SetCursor(ECursorType type)
{
    m_BaseCursor = type;
    // 如果栈不为空，覆盖光标优先
    if (m_CursorStack.GetSize() == 0)
    {
        ApplyCursor(type);
    }
}

ECursorType FCursorManager::GetCurrentCursor() const
{
    if (m_CursorStack.GetSize() > 0)
    {
        return m_CursorStack[m_CursorStack.GetSize() - 1];
    }
    return m_BaseCursor;
}

void FCursorManager::PushCursor(ECursorType type)
{
    m_CursorStack.Add(type);
    ApplyCursor(type);
}

void FCursorManager::PopCursor()
{
    if (m_CursorStack.GetSize() == 0)
    {
        LIMX_LOG(LogApplicationCore, Warning,
                 "FCursorManager: 光标栈为空，忽略 PopCursor");
        return;
    }

    // 移除栈顶
    TArray<ECursorType> temp;
    for (SizeType i = 0; i + 1 < m_CursorStack.GetSize(); ++i)
    {
        temp.Add(m_CursorStack[i]);
    }
    m_CursorStack = static_cast<TArray<ECursorType>&&>(temp);

    // 恢复到前一个光标
    ECursorType restored = (m_CursorStack.GetSize() > 0)
        ? m_CursorStack[m_CursorStack.GetSize() - 1]
        : m_BaseCursor;

    ApplyCursor(restored);
}

void FCursorManager::ApplyCursor(ECursorType type)
{
    SizeType index = static_cast<SizeType>(type);
    if (index >= static_cast<SizeType>(ECursorType::Count))
    {
        return;
    }

    void* cursor = m_SystemCursors[index];
    if (type == ECursorType::None)
    {
        // 隐藏光标时不调用 SetCursor
        return;
    }

    if (cursor != nullptr)
    {
        ::SetCursor(static_cast<HCURSOR>(cursor));
    }
}

// ============================================================================
// 可见性
// ============================================================================

void FCursorManager::ShowCursor()
{
    ++m_ShowCount;
    if (m_ShowCount == 0)
    {
        // 从隐藏变为可见
        ::ShowCursor(TRUE);
    }
}

void FCursorManager::HideCursor()
{
    if (m_ShowCount == 0)
    {
        // 从可见变为隐藏
        ::ShowCursor(FALSE);
    }
    --m_ShowCount;
}

// ============================================================================
// 光标锁定
// ============================================================================

void FCursorManager::LockCursor(Int32 left, Int32 top,
                                 Int32 right, Int32 bottom)
{
    RECT clipRect;
    clipRect.left   = left;
    clipRect.top    = top;
    clipRect.right  = right;
    clipRect.bottom = bottom;
    ::ClipCursor(&clipRect);
    m_IsLocked = true;
}

void FCursorManager::UnlockCursor()
{
    ::ClipCursor(nullptr);
    m_IsLocked = false;
}

// ============================================================================
// 光标位置
// ============================================================================

FVector2 FCursorManager::GetCursorPosition() const
{
    POINT pt;
    ::GetCursorPos(&pt);
    return FVector2(static_cast<Float32>(pt.x),
                    static_cast<Float32>(pt.y));
}

void FCursorManager::SetCursorPosition(const FVector2& screenPos)
{
    ::SetCursorPos(static_cast<Int32>(screenPos.X),
                   static_cast<Int32>(screenPos.Y));
}

} // namespace Limx
