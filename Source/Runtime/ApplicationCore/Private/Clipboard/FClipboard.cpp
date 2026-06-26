// ============================================================
// 文件名称：FClipboard.cpp
// 创建时间：2026-04-08
// 创建者  ：LimxTeam
// 设计哲学：Win32 剪贴板 API 封装 — OpenClipboard/SetClipboardData/
//          GetClipboardData 三步式操作，使用 CF_UNICODETEXT 格式
//          确保多语言支持，FString (ANSI) ↔ UTF-16 自动转换
// 功能描述：FClipboard 实现
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-08  │ LimxTeam  │ 初始创建                        │
// ============================================================

#include "ApplicationCore/ApplicationCoreMinimal.h"
#include "ApplicationCore/Clipboard/FClipboard.h"

namespace Limx
{

LIMX_DECLARE_LOG_CATEGORY(LogApplicationCore);

// ============================================================================
// CopyText — 复制文本到剪贴板
// ============================================================================

bool FClipboard::CopyText(const FString& text)
{
    if (!::OpenClipboard(nullptr))
    {
        LIMX_LOG(LogApplicationCore, Warning,
                 "FClipboard::CopyText — 无法打开剪贴板");
        return false;
    }

    ::EmptyClipboard();

    // 计算 UTF-16 缓冲区大小
    // 先将 ANSI FString 转换为宽字符
    const char* srcData  = text.GetCStr();
    SizeType    srcLen   = text.GetLength();

    // MultiByteToWideChar 计算所需宽字符数
    Int32 wideCharCount = ::MultiByteToWideChar(
        CP_UTF8, 0, srcData, static_cast<Int32>(srcLen), nullptr, 0);

    if (wideCharCount <= 0 && srcLen > 0)
    {
        ::CloseClipboard();
        return false;
    }

    // 分配全局内存（剪贴板要求 GlobalAlloc）
    SizeType bufferBytes = static_cast<SizeType>(wideCharCount + 1) *
                           sizeof(WideChar);
    HGLOBAL hMem = ::GlobalAlloc(GMEM_MOVEABLE, bufferBytes);
    if (hMem == nullptr)
    {
        ::CloseClipboard();
        return false;
    }

    WideChar* dest = static_cast<WideChar*>(::GlobalLock(hMem));
    if (dest == nullptr)
    {
        ::GlobalFree(hMem);
        ::CloseClipboard();
        return false;
    }

    // 执行转换
    ::MultiByteToWideChar(
        CP_UTF8, 0, srcData, static_cast<Int32>(srcLen),
        dest, wideCharCount);
    dest[wideCharCount] = L'\0';

    ::GlobalUnlock(hMem);

    // 设置剪贴板数据
    if (::SetClipboardData(CF_UNICODETEXT, hMem) == nullptr)
    {
        ::GlobalFree(hMem);
        ::CloseClipboard();
        LIMX_LOG(LogApplicationCore, Warning,
                 "FClipboard::CopyText — SetClipboardData 失败");
        return false;
    }

    ::CloseClipboard();
    return true;
}

// ============================================================================
// PasteText — 从剪贴板粘贴文本
// ============================================================================

bool FClipboard::PasteText(FString& outText)
{
    outText = FString();

    if (!::IsClipboardFormatAvailable(CF_UNICODETEXT))
    {
        return false;
    }

    if (!::OpenClipboard(nullptr))
    {
        LIMX_LOG(LogApplicationCore, Warning,
                 "FClipboard::PasteText — 无法打开剪贴板");
        return false;
    }

    HANDLE hData = ::GetClipboardData(CF_UNICODETEXT);
    if (hData == nullptr)
    {
        ::CloseClipboard();
        return false;
    }

    const WideChar* wideData =
        static_cast<const WideChar*>(::GlobalLock(hData));
    if (wideData == nullptr)
    {
        ::CloseClipboard();
        return false;
    }

    // 计算宽字符串长度
    SizeType wideLen = 0;
    while (wideData[wideLen] != L'\0')
    {
        ++wideLen;
    }

    // 转换为 UTF-8 (FString 存储格式)
    Int32 utf8Count = ::WideCharToMultiByte(
        CP_UTF8, 0, wideData, static_cast<Int32>(wideLen),
        nullptr, 0, nullptr, nullptr);

    if (utf8Count > 0)
    {
        // 使用临时缓冲区
        TArray<AnsiChar> buffer;
        for (Int32 i = 0; i < utf8Count + 1; ++i)
        {
            buffer.Add('\0');
        }

        ::WideCharToMultiByte(
            CP_UTF8, 0, wideData, static_cast<Int32>(wideLen),
            &buffer[0], utf8Count, nullptr, nullptr);
        buffer[static_cast<SizeType>(utf8Count)] = '\0';

        outText = FString(&buffer[0]);
    }

    ::GlobalUnlock(hData);
    ::CloseClipboard();
    return true;
}

// ============================================================================
// HasText — 检查剪贴板是否有文本
// ============================================================================

bool FClipboard::HasText()
{
    return ::IsClipboardFormatAvailable(CF_UNICODETEXT) != 0;
}

// ============================================================================
// Clear — 清空剪贴板
// ============================================================================

bool FClipboard::Clear()
{
    if (!::OpenClipboard(nullptr))
    {
        return false;
    }

    ::EmptyClipboard();
    ::CloseClipboard();
    return true;
}

} // namespace Limx
