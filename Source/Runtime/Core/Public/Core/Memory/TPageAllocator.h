/*******************************************************************************
 * 文件: TPageAllocator.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   页式分配器 — 基于操作系统虚拟内存的大块分配器
 *   通过 VirtualAlloc/VirtualFree 直接管理虚拟内存页
 *   用于大块内存分配、线性分配器后端、渲染缓冲区等场景
 *
 * 设计哲学:
 *   操作系统直达 — 绕过 CRT 堆，直接调用虚拟内存 API
 *   页对齐 — 分配粒度为系统页大小 (通常 4KB / 64KB)
 *   保留+提交分离 — 支持先保留地址空间再按需提交
 *
 * 技术特性:
 *   - Allocate: 保留并提交虚拟内存页
 *   - Deallocate: 释放虚拟内存页
 *   - Reserve: 仅保留地址空间
 *   - Commit: 提交已保留的页
 *   - Decommit: 取消提交但保留地址空间
 *   - GetPageSize: 获取系统页大小
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h
 *   外部: Windows VirtualAlloc/VirtualFree
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"

// VirtualAlloc/VirtualFree 前向声明 (避免包含 Windows.h)
#if LIMX_PLATFORM_WINDOWS
#ifndef _WINDOWS_
extern "C"
{
    __declspec(dllimport) void* __stdcall VirtualAlloc(
        void* lpAddress, unsigned long long dwSize,
        unsigned long flAllocationType,
        unsigned long flProtect);

    __declspec(dllimport) int __stdcall VirtualFree(
        void* lpAddress, unsigned long long dwSize,
        unsigned long dwFreeType);
}
#endif // _WINDOWS_
#endif

namespace Limx
{

/// 页式分配器 — 虚拟内存页分配
struct FPageAllocator
{
    // Windows 虚拟内存常量
    static constexpr unsigned long kMemCommit   = 0x00001000;
    static constexpr unsigned long kMemReserve  = 0x00002000;
    static constexpr unsigned long kMemRelease  = 0x00008000;
    static constexpr unsigned long kMemDecommit = 0x00004000;
    static constexpr unsigned long kPageReadWrite = 0x04;

    // ========================================================================
    // 分配与释放
    // ========================================================================

    /// 分配虚拟内存 (保留 + 提交)
    /// @param size 请求大小 (会向上对齐到页大小)
    /// @return 页对齐的内存指针，失败返回 nullptr
    LIMX_NODISCARD static void* Allocate(SizeType size)
    {
#if LIMX_PLATFORM_WINDOWS
        return VirtualAlloc(
            nullptr,
            static_cast<unsigned long long>(size),
            kMemReserve | kMemCommit,
            kPageReadWrite);
#else
        LIMX_UNUSED(size);
        return nullptr;
#endif
    }

    /// 释放虚拟内存
    static void Deallocate(void* ptr)
    {
        if (!ptr) return;
#if LIMX_PLATFORM_WINDOWS
        VirtualFree(ptr, 0, kMemRelease);
#else
        LIMX_UNUSED(ptr);
#endif
    }

    // ========================================================================
    // 保留/提交分离操作
    // ========================================================================

    /// 仅保留地址空间 (不分配物理内存)
    /// @param size 保留大小
    LIMX_NODISCARD static void* Reserve(SizeType size)
    {
#if LIMX_PLATFORM_WINDOWS
        return VirtualAlloc(
            nullptr,
            static_cast<unsigned long long>(size),
            kMemReserve,
            kPageReadWrite);
#else
        LIMX_UNUSED(size);
        return nullptr;
#endif
    }

    /// 提交已保留的地址范围 (分配物理内存)
    /// @param ptr  已保留的地址
    /// @param size 提交大小
    /// @return 成功返回 true
    LIMX_NODISCARD static bool Commit(void* ptr, SizeType size)
    {
#if LIMX_PLATFORM_WINDOWS
        return VirtualAlloc(
            ptr,
            static_cast<unsigned long long>(size),
            kMemCommit,
            kPageReadWrite) != nullptr;
#else
        LIMX_UNUSED(ptr);
        LIMX_UNUSED(size);
        return false;
#endif
    }

    /// 取消提交 (释放物理内存，保留地址空间)
    /// @param ptr  已提交的地址
    /// @param size 取消提交的大小
    static void Decommit(void* ptr, SizeType size)
    {
#if LIMX_PLATFORM_WINDOWS
        VirtualFree(
            ptr,
            static_cast<unsigned long long>(size),
            kMemDecommit);
#else
        LIMX_UNUSED(ptr);
        LIMX_UNUSED(size);
#endif
    }

    // ========================================================================
    // 系统信息
    // ========================================================================

    /// 获取系统页大小 (字节)
    /// Windows x64 页大小固定 4KB，无需运行时查询
    LIMX_NODISCARD static constexpr SizeType GetPageSize()
    {
        return 4096;
    }

    /// 获取分配粒度 (Windows x64 固定 64KB)
    LIMX_NODISCARD static constexpr SizeType GetAllocationGranularity()
    {
        return 65536;
    }

    /// 向上对齐到页大小
    LIMX_NODISCARD static constexpr SizeType AlignToPageSize(
        SizeType size)
    {
        return (size + GetPageSize() - 1) & ~(GetPageSize() - 1);
    }
};

} // namespace Limx
