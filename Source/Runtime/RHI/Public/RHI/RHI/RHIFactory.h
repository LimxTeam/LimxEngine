// ============================================================
// 文件名称：RHIFactory.h
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：工厂模式隔离后端实现 — 上层模块仅依赖 IRHIDevice 和
//          IRHICommandBuffer 抽象接口，具体 Vulkan/DX12 后端由
//          工厂函数在链接时决定，实现零耦合的后端可替换性。
// 功能描述：RHI 设备与命令缓冲区的工厂创建/销毁函数。
//          上层模块 (Luminance) 通过本头文件获取 GPU 设备实例，
//          无需直接包含任何 Vulkan 私有头文件。
// 技术特性：返回 TUniquePtr 确保 RAII 生命周期管理；
//          Initialize 内部完成实例创建、物理设备选择、逻辑设备创建
//          等完整初始化链路。
//
// ── 函数表 ──────────────────────────────────────────────────
// │ 函数名                      │ 描述                           │
// │────────────────────────────│───────────────────────────────│
// │ CreateRHIDevice()          │ 创建并初始化 RHI 设备实例         │
// │ CreateRHICommandBuffer()   │ 创建命令缓冲区包装器              │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-06  │ LimxTeam  │ 初始创建                        │
// ============================================================

#pragma once

#include "RHI/RHIMinimal.h"
#include "RHI/RHI/IRHIDevice.h"
#include "RHI/RHI/IRHICommandBuffer.h"

namespace Limx
{

// ============================================================================
// RHI 设备工厂
// ============================================================================

/// 创建并初始化 RHI 设备
/// 内部执行完整的 Vulkan 初始化链路:
///   VkInstance → DebugMessenger → VkSurfaceKHR → PhysicalDevice → LogicalDevice → DescriptorPool
///
/// @param nativeWindowHandle  原生窗口句柄 (Windows: HWND)
/// @param enableValidation    是否启用 Vulkan 验证层 (Debug/Development 推荐开启)
/// @return 初始化完成的设备实例; 失败返回空 TUniquePtr
TUniquePtr<IRHIDevice> CreateRHIDevice(void* nativeWindowHandle,
                                        bool enableValidation);

// ============================================================================
// 命令缓冲区工厂
// ============================================================================

/// 创建命令缓冲区包装器
/// 将已分配的命令缓冲区句柄包装为 IRHICommandBuffer 接口实例,
/// 用于录制绘制/计算/拷贝命令。
///
/// @param device  所属 RHI 设备 (必须是 CreateRHIDevice 返回的实例)
/// @param handle  已通过 IRHIDevice::AllocateCommandBuffer 分配的句柄
/// @return 命令缓冲区接口实例; 失败返回空 TUniquePtr
TUniquePtr<IRHICommandBuffer> CreateRHICommandBuffer(
    IRHIDevice* device,
    FRHICommandBufferHandle handle);

} // namespace Limx
