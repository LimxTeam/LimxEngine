// ============================================================
// 文件名称：FCursorManager.h
// 创建时间：2026-04-08
// 创建者  ：LimxTeam
// 设计哲学：集中管理 — 统一管理系统光标形状切换和可见性，
//          对应 UE5 ICursor/FGenericPlatformApplicationMisc 中
//          光标相关功能。编辑器拖拽、分割器拖动、控件悬停等
//          场景均通过此管理器设置光标形状。
// 功能描述：FCursorManager — 系统光标管理器（全局单例）
//          ECursorType — 光标形状枚举
// 技术特性：Win32 LoadCursor + SetCursor + ShowCursor API 封装；
//          引用计数式隐藏/显示；栈式光标覆盖（PushCursor/PopCursor）
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                    │ 描述                           │
// │──────────────────────────│───────────────────────────────│
// │ Get()                    │ 获取全局单例                    │
// │ SetCursor(type)          │ 设置当前光标形状                │
// │ GetCurrentCursor()       │ 获取当前光标类型                │
// │ PushCursor(type)         │ 压入覆盖光标（栈式）             │
// │ PopCursor()              │ 弹出覆盖光标                   │
// │ ShowCursor()             │ 显示光标                       │
// │ HideCursor()             │ 隐藏光标                       │
// │ IsCursorVisible()        │ 光标是否可见                   │
// │ LockCursor(rect)         │ 将光标限制在矩形区域内          │
// │ UnlockCursor()           │ 解除光标限制                   │
//
// ── 结构体字段表 ──────────────────────────────────────────────
// │ 字段名              │ 类型                │ 描述           │
// │────────────────────│──────────────────│───────────────│
// │ m_CurrentCursor     │ ECursorType      │ 当前光标类型    │
// │ m_CursorStack       │ TArray<ECursor>  │ 覆盖光标栈     │
// │ m_ShowCount         │ Int32            │ 显示引用计数    │
// │ m_SystemCursors     │ void*[Count]     │ 预加载的系统光标 │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-08  │ LimxTeam  │ 初始创建                        │
// ============================================================

#pragma once

#include "ApplicationCore/ApplicationCoreMinimal.h"

namespace Limx
{

// ============================================================================
// ECursorType — 光标形状枚举
// ============================================================================

enum class ECursorType : UInt8
{
    Arrow,            // 标准箭头
    TextEditBeam,     // 文本编辑 I-beam
    ResizeLeftRight,  // 水平调整
    ResizeUpDown,     // 垂直调整
    ResizeSouthEast,  // 对角调整 (左上→右下)
    ResizeSouthWest,  // 对角调整 (右上→左下)
    CardinalCross,    // 十字移动
    Crosshairs,       // 十字准心
    Hand,             // 手型（超链接/拖拽）
    GrabHand,         // 抓取手型（拖拽中）
    SlashedCircle,    // 禁止圆（不可放置）
    EyeDropper,       // 取色器
    Wait,             // 等待/沙漏
    None,             // 不显示
    Count,            // 枚举数量标记
};

// ============================================================================
// FCursorManager — 系统光标管理器（全局单例）
// ============================================================================

class FCursorManager
{
    LIMX_NON_COPYABLE(FCursorManager);
    LIMX_NON_MOVABLE(FCursorManager);

public:
    /// 获取全局单例
    LIMX_NODISCARD static FCursorManager& Get();

    // ── 光标形状 ──────────────────────────────────────────────

    /// 设置当前光标形状（清除栈内覆盖）
    void SetCursor(ECursorType type);

    /// 获取当前实际显示的光标类型
    LIMX_NODISCARD ECursorType GetCurrentCursor() const;

    /// 压入覆盖光标 — 临时改变光标形状（如分割器拖拽时）
    /// 调用者必须配对调用 PopCursor
    void PushCursor(ECursorType type);

    /// 弹出覆盖光标 — 恢复到前一个光标
    void PopCursor();

    // ── 可见性 ────────────────────────────────────────────────

    /// 显示光标（引用计数 +1）
    void ShowCursor();

    /// 隐藏光标（引用计数 -1）
    void HideCursor();

    /// 光标当前是否可见
    LIMX_NODISCARD bool IsCursorVisible() const
    {
        return m_ShowCount >= 0;
    }

    // ── 光标锁定 ──────────────────────────────────────────────

    /// 将光标限制在指定矩形区域内（屏幕坐标）
    /// @param left, top, right, bottom 屏幕像素坐标
    void LockCursor(Int32 left, Int32 top, Int32 right, Int32 bottom);

    /// 解除光标限制
    void UnlockCursor();

    /// 光标是否处于锁定状态
    LIMX_NODISCARD bool IsCursorLocked() const { return m_IsLocked; }

    // ── 光标位置 ──────────────────────────────────────────────

    /// 获取光标屏幕坐标
    LIMX_NODISCARD FVector2 GetCursorPosition() const;

    /// 设置光标屏幕坐标
    void SetCursorPosition(const FVector2& screenPos);

private:
    FCursorManager();
    ~FCursorManager();

    /// 加载所有系统光标资源
    void LoadSystemCursors();

    /// 应用光标形状到系统
    void ApplyCursor(ECursorType type);

    ECursorType          m_BaseCursor = ECursorType::Arrow;
    TArray<ECursorType>  m_CursorStack;
    Int32                m_ShowCount  = 0;
    bool                 m_IsLocked   = false;

    /// 预加载的系统光标句柄 (HCURSOR as void*)
    void* m_SystemCursors[static_cast<SizeType>(ECursorType::Count)] = {};
};

} // namespace Limx
