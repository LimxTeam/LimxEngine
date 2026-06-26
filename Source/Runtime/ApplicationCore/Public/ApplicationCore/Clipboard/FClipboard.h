// ============================================================
// 文件名称：FClipboard.h
// 创建时间：2026-04-08
// 创建者  ：LimxTeam
// 设计哲学：平台剪贴板封装 — 对应 UE5 FPlatformApplicationMisc
//          中的剪贴板功能。提供纯文本的复制/粘贴操作，
//          所有操作线程安全（Win32 OpenClipboard 是进程级锁）。
// 功能描述：FClipboard — 系统剪贴板工具类（全部静态方法）
// 技术特性：Win32 OpenClipboard/SetClipboardData/GetClipboardData
//          封装，UTF-16 ↔ FString (ANSI) 自动转换
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                    │ 描述                           │
// │──────────────────────────│───────────────────────────────│
// │ CopyText(text)           │ 将文本复制到系统剪贴板           │
// │ PasteText(outText)       │ 从系统剪贴板粘贴文本             │
// │ HasText()                │ 剪贴板中是否有文本数据           │
// │ Clear()                  │ 清空剪贴板                      │
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
// FClipboard — 系统剪贴板工具类
// ============================================================================

class FClipboard
{
public:
    /// 将文本复制到系统剪贴板
    /// @param text 要复制的文本内容
    /// @return true 操作成功; false 无法打开剪贴板
    static bool CopyText(const FString& text);

    /// 从系统剪贴板粘贴文本
    /// @param outText 输出: 剪贴板中的文本内容
    /// @return true 成功获取文本; false 剪贴板无文本或无法打开
    static bool PasteText(FString& outText);

    /// 剪贴板中是否有文本数据
    /// @return true 包含 CF_UNICODETEXT 或 CF_TEXT 格式
    LIMX_NODISCARD static bool HasText();

    /// 清空剪贴板
    /// @return true 操作成功
    static bool Clear();

private:
    FClipboard() = delete;
    ~FClipboard() = delete;
};

} // namespace Limx
