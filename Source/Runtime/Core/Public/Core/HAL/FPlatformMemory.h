/*******************************************************************************
 * 文件: FPlatformMemory.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   平台内存信息查询 — 获取系统物理/虚拟内存状态
 *   封装 Windows GlobalMemoryStatusEx API
 *   用于内存预算、资产流式加载决策、诊断输出等场景
 *
 * 设计哲学:
 *   静态工具类 — 所有方法为 static，无需实例化
 *   快照语义 — GetStats() 返回调用时刻的内存快照
 *   字节单位 — 所有值以字节为单位 (UInt64)
 *
 * 技术特性:
 *   - MemoryStats: 物理总量/可用, 虚拟总量/可用, 页文件总量/可用
 *   - GetStats(): 查询当前内存状态
 *   - GetPageSize(): 系统页大小
 *   - GetAllocationGranularity(): VirtualAlloc 分配粒度
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h
 *   外部: Windows API (GlobalMemoryStatusEx, GetSystemInfo)
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"

// Windows 内存 API 前向声明
#if LIMX_PLATFORM_WINDOWS
#ifdef _WINDOWS_
// Windows 头文件已包含 — 使用真实类型别名
using MEMORYSTATUSEX_T = MEMORYSTATUSEX;
using SYSTEM_INFO_T = SYSTEM_INFO;
// API 函数已由 Windows 头文件声明
#else
extern "C"
{
    // MEMORYSTATUSEX 结构
    struct MEMORYSTATUSEX_T
    {
        unsigned long  dwLength;
        unsigned long  dwMemoryLoad;
        unsigned long long ullTotalPhys;
        unsigned long long ullAvailPhys;
        unsigned long long ullTotalPageFile;
        unsigned long long ullAvailPageFile;
        unsigned long long ullTotalVirtual;
        unsigned long long ullAvailVirtual;
        unsigned long long ullAvailExtendedVirtual;
    };

    int __stdcall GlobalMemoryStatusEx(MEMORYSTATUSEX_T* lpBuffer);

    // SYSTEM_INFO 部分字段
    struct SYSTEM_INFO_T
    {
        unsigned long dwOemId;
        unsigned long dwPageSize;
        void*         lpMinimumApplicationAddress;
        void*         lpMaximumApplicationAddress;
        unsigned long long dwActiveProcessorMask;
        unsigned long dwNumberOfProcessors;
        unsigned long dwProcessorType;
        unsigned long dwAllocationGranularity;
        unsigned short wProcessorLevel;
        unsigned short wProcessorRevision;
    };

    void __stdcall GetSystemInfo(SYSTEM_INFO_T* lpSystemInfo);
}
#endif // _WINDOWS_
#endif

namespace Limx
{

/// 内存状态快照
struct MemoryStats
{
    UInt64 PhysicalTotal;        ///< 物理内存总量 (字节)
    UInt64 PhysicalAvailable;    ///< 物理内存可用量 (字节)
    UInt64 VirtualTotal;         ///< 虚拟内存总量 (字节)
    UInt64 VirtualAvailable;     ///< 虚拟内存可用量 (字节)
    UInt64 PageFileTotal;        ///< 页文件总量 (字节)
    UInt64 PageFileAvailable;    ///< 页文件可用量 (字节)
    UInt32 MemoryLoadPercent;    ///< 内存使用率 (0-100%)
};

/// 平台内存信息查询
struct FPlatformMemory
{
    /// 查询当前内存状态
    LIMX_NODISCARD static MemoryStats GetStats()
    {
        MemoryStats stats;
        Memory::MemZero(&stats, sizeof(MemoryStats));

#if LIMX_PLATFORM_WINDOWS
        MEMORYSTATUSEX_T memStatus;
        memStatus.dwLength = sizeof(MEMORYSTATUSEX_T);
        if (GlobalMemoryStatusEx(&memStatus))
        {
            stats.PhysicalTotal     = static_cast<UInt64>(memStatus.ullTotalPhys);
            stats.PhysicalAvailable = static_cast<UInt64>(memStatus.ullAvailPhys);
            stats.VirtualTotal      = static_cast<UInt64>(memStatus.ullTotalVirtual);
            stats.VirtualAvailable  = static_cast<UInt64>(memStatus.ullAvailVirtual);
            stats.PageFileTotal     = static_cast<UInt64>(memStatus.ullTotalPageFile);
            stats.PageFileAvailable = static_cast<UInt64>(memStatus.ullAvailPageFile);
            stats.MemoryLoadPercent = static_cast<UInt32>(memStatus.dwMemoryLoad);
        }
#endif

        return stats;
    }

    /// 获取系统页大小 (通常 4096 字节)
    LIMX_NODISCARD static UInt32 GetPageSize()
    {
        static UInt32 s_PageSize = QueryPageSize();
        return s_PageSize;
    }

    /// 获取 VirtualAlloc 分配粒度 (通常 65536 字节)
    LIMX_NODISCARD static UInt32 GetAllocationGranularity()
    {
        static UInt32 s_Granularity = QueryAllocationGranularity();
        return s_Granularity;
    }

    /// 获取逻辑处理器数量
    LIMX_NODISCARD static UInt32 GetProcessorCount()
    {
        static UInt32 s_Count = QueryProcessorCount();
        return s_Count;
    }

private:
    static UInt32 QueryPageSize()
    {
#if LIMX_PLATFORM_WINDOWS
        SYSTEM_INFO_T info;
        GetSystemInfo(&info);
        return static_cast<UInt32>(info.dwPageSize);
#else
        return 4096;
#endif
    }

    static UInt32 QueryAllocationGranularity()
    {
#if LIMX_PLATFORM_WINDOWS
        SYSTEM_INFO_T info;
        GetSystemInfo(&info);
        return static_cast<UInt32>(info.dwAllocationGranularity);
#else
        return 65536;
#endif
    }

    static UInt32 QueryProcessorCount()
    {
#if LIMX_PLATFORM_WINDOWS
        SYSTEM_INFO_T info;
        GetSystemInfo(&info);
        return static_cast<UInt32>(info.dwNumberOfProcessors);
#else
        return 1;
#endif
    }
};

} // namespace Limx
