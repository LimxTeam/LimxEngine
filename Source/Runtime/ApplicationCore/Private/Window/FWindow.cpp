// ============================================================
// 文件名称：FWindow.cpp
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：薄封装原则 — Win32 窗口操作的直接映射，
//          最小化抽象开销，零 STL 依赖。
// 功能描述：FWindow 的完整实现 — 窗口类注册、窗口创建、
//          消息泵循环、WM_SIZE/WM_CLOSE 处理、资源清理。
// 技术特性：WNDCLASSEXW + CreateWindowExW；
//          GWLP_USERDATA 存储 this 指针实现回调路由；
//          AdjustWindowRectEx 确保客户区精确尺寸。
//
// ── 函数表 ──────────────────────────────────────────────────
// │ 函数名                      │ 描述                           │
// │────────────────────────────│───────────────────────────────│
// │ Create()                   │ 注册窗口类 + 创建窗口            │
// │ Destroy()                  │ DestroyWindow + UnregisterClass │
// │ ProcessMessages()          │ PeekMessage 非阻塞消息泵         │
// │ WindowProcStatic()         │ 静态回调 → GWLP_USERDATA 路由   │
// │ HandleMessage()            │ 实例消息处理                     │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-06  │ LimxTeam  │ 初始创建                        │
// ============================================================

#include "ApplicationCore/Window/FWindow.h"
#include "ApplicationCore/Input/FInputManager.h"

namespace Limx
{

// 日志分类
LIMX_DECLARE_LOG_CATEGORY(LogWindow)
LIMX_DEFINE_LOG_CATEGORY(LogWindow)

// 静态成员初始化
bool FWindow::s_IsClassRegistered = false;

// ============================================================================
// 析构
// ============================================================================

FWindow::~FWindow()
{
    Destroy();
}

// ============================================================================
// Create — 注册窗口类并创建窗口
// ============================================================================

bool FWindow::Create(const FWindowDesc& desc)
{
    // 防止重复创建
    if (m_Handle != nullptr)
    {
        LIMX_LOG(LogWindow, Warning,
            "[Window] 窗口已存在，忽略重复创建请求");
        return true;
    }

    HINSTANCE hInstance = GetModuleHandleW(nullptr);

    // 注册窗口类 (进程生命周期内仅一次)
    if (!s_IsClassRegistered)
    {
        WNDCLASSEXW windowClass = {};
        windowClass.cbSize        = sizeof(WNDCLASSEXW);
        windowClass.style         = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc   = reinterpret_cast<WNDPROC>(WindowProcStatic);
        windowClass.cbClsExtra    = 0;
        windowClass.cbWndExtra    = 0;
        windowClass.hInstance     = hInstance;
        windowClass.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
        windowClass.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = nullptr;
        windowClass.lpszMenuName  = nullptr;
        windowClass.lpszClassName = kWindowClassName;
        windowClass.hIconSm       = LoadIconW(nullptr, IDI_APPLICATION);

        if (RegisterClassExW(&windowClass) == 0)
        {
            LIMX_LOG(LogWindow, Error,
                "[Window] RegisterClassExW 失败: {}",
                static_cast<Int32>(GetLastError()));
            return false;
        }

        s_IsClassRegistered = true;
    }

    // 计算窗口样式
    DWORD windowStyle = WS_OVERLAPPEDWINDOW;
    if (!desc.IsResizable)
    {
        // 移除最大化按钮和调整边框
        windowStyle &= ~(WS_MAXIMIZEBOX | WS_THICKFRAME);
    }

    // 根据客户区尺寸计算窗口尺寸 (含标题栏和边框)
    RECT windowRect = {};
    windowRect.left   = 0;
    windowRect.top    = 0;
    windowRect.right  = static_cast<LONG>(desc.Width);
    windowRect.bottom = static_cast<LONG>(desc.Height);
    AdjustWindowRectEx(&windowRect, windowStyle, FALSE, 0);

    Int32 windowWidth  = windowRect.right - windowRect.left;
    Int32 windowHeight = windowRect.bottom - windowRect.top;

    // 创建窗口
    HWND hwnd = CreateWindowExW(
        0,                          // 扩展样式
        kWindowClassName,           // 窗口类名
        desc.Title,                 // 窗口标题
        windowStyle,                // 窗口样式
        CW_USEDEFAULT,              // X 位置
        CW_USEDEFAULT,              // Y 位置
        windowWidth,                // 窗口宽度 (含边框)
        windowHeight,               // 窗口高度 (含标题栏)
        nullptr,                    // 父窗口
        nullptr,                    // 菜单
        hInstance,                  // 实例句柄
        nullptr                     // 创建参数
    );

    if (hwnd == nullptr)
    {
        LIMX_LOG(LogWindow, Error,
            "[Window] CreateWindowExW 失败: {}",
            static_cast<Int32>(GetLastError()));
        return false;
    }

    // 将 this 指针存入窗口用户数据，供 WindowProc 回调路由
    SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                       reinterpret_cast<LONG_PTR>(this));

    m_Handle = hwnd;
    m_Width  = desc.Width;
    m_Height = desc.Height;
    m_IsMinimized = false;
    m_WasResized  = false;
    m_ShouldClose = false;

    // 注册原始输入设备 (鼠标) 以接收 WM_INPUT 消息
    RAWINPUTDEVICE rawInputDevice = {};
    rawInputDevice.usUsagePage = 0x01;  // HID_USAGE_PAGE_GENERIC
    rawInputDevice.usUsage     = 0x02;  // HID_USAGE_GENERIC_MOUSE
    rawInputDevice.dwFlags     = 0;
    rawInputDevice.hwndTarget  = hwnd;
    RegisterRawInputDevices(&rawInputDevice, 1, sizeof(RAWINPUTDEVICE));

    // 显示窗口
    if (desc.IsVisible)
    {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }

    LIMX_LOG(LogWindow, Log,
        "[Window] 窗口创建完成: {}x{}", m_Width, m_Height);

    return true;
}

// ============================================================================
// Destroy — 销毁窗口
// ============================================================================

void FWindow::Destroy()
{
    if (m_Handle != nullptr)
    {
        DestroyWindow(static_cast<HWND>(m_Handle));
        m_Handle = nullptr;

        LIMX_LOG(LogWindow, Log, "[Window] 窗口已销毁");
    }

    m_Width       = 0;
    m_Height      = 0;
    m_IsMinimized = false;
    m_WasResized  = false;
    m_ShouldClose = false;
}

// ============================================================================
// ProcessMessages — 非阻塞消息泵
// ============================================================================

bool FWindow::ProcessMessages()
{
    MSG message = {};

    // 处理所有待处理的消息 (非阻塞)
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        if (message.message == WM_QUIT)
        {
            m_ShouldClose = true;
            return false;
        }

        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return !m_ShouldClose;
}

// ============================================================================
// WindowProcStatic — 静态窗口过程
// ============================================================================

Int64 __stdcall FWindow::WindowProcStatic(void* hwnd, UInt32 message,
                                           UInt64 wParam, Int64 lParam)
{
    // 从窗口用户数据获取 FWindow 实例指针
    FWindow* window = reinterpret_cast<FWindow*>(
        GetWindowLongPtrW(static_cast<HWND>(hwnd), GWLP_USERDATA));

    if (window != nullptr)
    {
        return window->HandleMessage(hwnd, message, wParam, lParam);
    }

    // 窗口尚未关联实例 (创建过程中的早期消息)
    return DefWindowProcW(static_cast<HWND>(hwnd), message, wParam, lParam);
}

// ============================================================================
// HandleMessage — 实例消息处理
// ============================================================================

Int64 FWindow::HandleMessage(void* hwnd, UInt32 message,
                              UInt64 wParam, Int64 lParam)
{
    HWND hWnd = static_cast<HWND>(hwnd);

    switch (message)
    {
    case WM_SIZE:
    {
        UInt32 newWidth  = static_cast<UInt32>(lParam & 0xFFFF);
        UInt32 newHeight = static_cast<UInt32>((lParam >> 16) & 0xFFFF);

        m_IsMinimized = (wParam == SIZE_MINIMIZED);

        if (!m_IsMinimized && (newWidth != m_Width || newHeight != m_Height))
        {
            m_Width      = newWidth;
            m_Height     = newHeight;
            m_WasResized = true;

            LIMX_LOG(LogWindow, Verbose,
                "[Window] 尺寸变化: {}x{}", m_Width, m_Height);
        }

        return 0;
    }

    case WM_CLOSE:
    {
        m_ShouldClose = true;
        PostQuitMessage(0);
        return 0;
    }

    case WM_DESTROY:
    {
        PostQuitMessage(0);
        return 0;
    }

    case WM_GETMINMAXINFO:
    {
        // 设置最小窗口尺寸，防止客户区尺寸为 0
        MINMAXINFO* minMaxInfo = reinterpret_cast<MINMAXINFO*>(lParam);
        minMaxInfo->ptMinTrackSize.x = 320;
        minMaxInfo->ptMinTrackSize.y = 240;
        return 0;
    }

    // ---- 键盘事件 ----

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    {
        if (wParam < 256)
        {
            FInputManager::Get().OnKeyDown(
                static_cast<UInt8>(wParam));
        }
        return 0;
    }

    case WM_KEYUP:
    case WM_SYSKEYUP:
    {
        if (wParam < 256)
        {
            FInputManager::Get().OnKeyUp(
                static_cast<UInt8>(wParam));
        }
        return 0;
    }

    // ---- 鼠标按钮事件 ----

    case WM_LBUTTONDOWN:
    {
        FInputManager::Get().OnMouseButtonDown(EMouseButton::Left);
        SetCapture(hWnd);
        return 0;
    }
    case WM_LBUTTONUP:
    {
        FInputManager::Get().OnMouseButtonUp(EMouseButton::Left);
        if (!FInputManager::Get().IsAnyMouseButtonDown())
        {
            ReleaseCapture();
        }
        return 0;
    }

    case WM_RBUTTONDOWN:
    {
        FInputManager::Get().OnMouseButtonDown(EMouseButton::Right);
        SetCapture(hWnd);
        return 0;
    }
    case WM_RBUTTONUP:
    {
        FInputManager::Get().OnMouseButtonUp(EMouseButton::Right);
        if (!FInputManager::Get().IsAnyMouseButtonDown())
        {
            ReleaseCapture();
        }
        return 0;
    }

    case WM_MBUTTONDOWN:
    {
        FInputManager::Get().OnMouseButtonDown(EMouseButton::Middle);
        SetCapture(hWnd);
        return 0;
    }
    case WM_MBUTTONUP:
    {
        FInputManager::Get().OnMouseButtonUp(EMouseButton::Middle);
        if (!FInputManager::Get().IsAnyMouseButtonDown())
        {
            ReleaseCapture();
        }
        return 0;
    }

    case WM_XBUTTONDOWN:
    {
        UInt16 xButton = static_cast<UInt16>((wParam >> 16) & 0xFFFFu);
        if (xButton == XBUTTON1)
        {
            FInputManager::Get().OnMouseButtonDown(EMouseButton::Thumb1);
        }
        else if (xButton == XBUTTON2)
        {
            FInputManager::Get().OnMouseButtonDown(EMouseButton::Thumb2);
        }
        SetCapture(hWnd);
        return TRUE;
    }
    case WM_XBUTTONUP:
    {
        UInt16 xButton = static_cast<UInt16>((wParam >> 16) & 0xFFFFu);
        if (xButton == XBUTTON1)
        {
            FInputManager::Get().OnMouseButtonUp(EMouseButton::Thumb1);
        }
        else if (xButton == XBUTTON2)
        {
            FInputManager::Get().OnMouseButtonUp(EMouseButton::Thumb2);
        }
        if (!FInputManager::Get().IsAnyMouseButtonDown())
        {
            ReleaseCapture();
        }
        return TRUE;
    }

    // ---- 鼠标移动 ----

    case WM_MOUSEMOVE:
    {
        Float32 mouseX = static_cast<Float32>(
            static_cast<Int16>(lParam & 0xFFFF));
        Float32 mouseY = static_cast<Float32>(
            static_cast<Int16>((lParam >> 16) & 0xFFFF));
        FInputManager::Get().OnMouseMove(mouseX, mouseY);
        return 0;
    }

    // ---- 原始输入 (用于鼠标 Delta) ----

    case WM_INPUT:
    {
        RAWINPUT raw = {};
        UINT rawSize = sizeof(RAWINPUT);
        GetRawInputData(
            reinterpret_cast<HRAWINPUT>(lParam),
            RID_INPUT, &raw, &rawSize, sizeof(RAWINPUTHEADER));

        if (raw.header.dwType == RIM_TYPEMOUSE)
        {
            Float32 deltaX = static_cast<Float32>(
                raw.data.mouse.lLastX);
            Float32 deltaY = static_cast<Float32>(
                raw.data.mouse.lLastY);
            FInputManager::Get().OnRawMouseDelta(deltaX, deltaY);
        }
        return 0;
    }

    default:
        break;
    }

    return DefWindowProcW(hWnd, message, wParam, lParam);
}

} // namespace Limx
