// ============================================================
// 文件名称：PlatformModule.cpp
// 创建时间：2025-07-27
// 创建者  ：LimxTeam
// 设计哲学：模块入口 + 编译时类型验证，确保所有 RHI 类型在编译期
//          满足大小、布局、可实例化等基本正确性约束。
// 功能描述：LimxPlatform 模块生命周期管理 (初始化/关闭) 以及
//          RHI 枚举、结构体、句柄、接口的 static_assert 验证。
// 技术特性：static_assert 零运行时开销，仅在编译期执行；
//          ModuleStartup/ModuleShutdown 为未来 Vulkan 实例/设备
//          初始化与清理预留入口。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                    │ 描述                           │
// │──────────────────────────│───────────────────────────────│
// │ ModuleStartup()          │ 模块初始化 (Vulkan 实例创建预留)   │
// │ ModuleShutdown()         │ 模块关闭 (资源清理预留)            │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2025-07-27  │ LimxTeam  │ 初始创建                        │
// ============================================================

#include "RHI/RHIMinimal.h"
#include "RHI/RHI/RHIDefinitions.h"
#include "RHI/RHI/RHIResources.h"
#include "RHI/RHI/RHIPipelineState.h"
#include "RHI/RHI/IRHIDevice.h"
#include "RHI/RHI/IRHICommandBuffer.h"
#include "RHI/RHI/RHIFactory.h"
#include "Vulkan/FVulkanDevice.h"
#include "Vulkan/FVulkanCommandBuffer.h"

namespace Limx
{

// ============================================================================
// RHI 日志分类定义
// ============================================================================

LIMX_DEFINE_LOG_CATEGORY(LogRHI)

// ============================================================================
// 编译时验证 — RHI 枚举底层类型与大小
// ============================================================================

namespace
{
    // ---- 枚举底层类型宽度 ----

    static_assert(sizeof(EPixelFormat) == sizeof(UInt16),
        "EPixelFormat 应为 2 字节 (UInt16)");

    static_assert(sizeof(EPrimitiveTopology) == sizeof(UInt8),
        "EPrimitiveTopology 应为 1 字节 (UInt8)");

    static_assert(sizeof(EBlendFactor) == sizeof(UInt8),
        "EBlendFactor 应为 1 字节 (UInt8)");

    static_assert(sizeof(EBlendOp) == sizeof(UInt8),
        "EBlendOp 应为 1 字节 (UInt8)");

    static_assert(sizeof(ECompareOp) == sizeof(UInt8),
        "ECompareOp 应为 1 字节 (UInt8)");

    static_assert(sizeof(EStencilOp) == sizeof(UInt8),
        "EStencilOp 应为 1 字节 (UInt8)");

    static_assert(sizeof(ECullMode) == sizeof(UInt8),
        "ECullMode 应为 1 字节 (UInt8)");

    static_assert(sizeof(EFrontFace) == sizeof(UInt8),
        "EFrontFace 应为 1 字节 (UInt8)");

    static_assert(sizeof(EPolygonMode) == sizeof(UInt8),
        "EPolygonMode 应为 1 字节 (UInt8)");

    static_assert(sizeof(EShaderStage) == sizeof(UInt32),
        "EShaderStage 应为 4 字节 (UInt32 位掩码)");

    static_assert(sizeof(EBufferUsage) == sizeof(UInt32),
        "EBufferUsage 应为 4 字节 (UInt32 位掩码)");

    static_assert(sizeof(ETextureUsage) == sizeof(UInt32),
        "ETextureUsage 应为 4 字节 (UInt32 位掩码)");

    static_assert(sizeof(ETextureType) == sizeof(UInt8),
        "ETextureType 应为 1 字节 (UInt8)");

    static_assert(sizeof(EFilter) == sizeof(UInt8),
        "EFilter 应为 1 字节 (UInt8)");

    static_assert(sizeof(ESamplerAddressMode) == sizeof(UInt8),
        "ESamplerAddressMode 应为 1 字节 (UInt8)");

    static_assert(sizeof(ELoadOp) == sizeof(UInt8),
        "ELoadOp 应为 1 字节 (UInt8)");

    static_assert(sizeof(EStoreOp) == sizeof(UInt8),
        "EStoreOp 应为 1 字节 (UInt8)");

    static_assert(sizeof(EImageLayout) == sizeof(UInt8),
        "EImageLayout 应为 1 字节 (UInt8)");

    static_assert(sizeof(EQueueType) == sizeof(UInt8),
        "EQueueType 应为 1 字节 (UInt8)");

    static_assert(sizeof(EMemoryUsage) == sizeof(UInt8),
        "EMemoryUsage 应为 1 字节 (UInt8)");

    static_assert(sizeof(EIndexType) == sizeof(UInt8),
        "EIndexType 应为 1 字节 (UInt8)");

    static_assert(sizeof(EColorWriteMask) == sizeof(UInt8),
        "EColorWriteMask 应为 1 字节 (UInt8)");

    static_assert(sizeof(EDescriptorType) == sizeof(UInt8),
        "EDescriptorType 应为 1 字节 (UInt8)");

    static_assert(sizeof(EPipelineBindPoint) == sizeof(UInt8),
        "EPipelineBindPoint 应为 1 字节 (UInt8)");

    static_assert(sizeof(EDynamicState) == sizeof(UInt32),
        "EDynamicState 应为 4 字节 (UInt32 位掩码)");

    static_assert(sizeof(EAccessFlags) == sizeof(UInt32),
        "EAccessFlags 应为 4 字节 (UInt32 位掩码)");

    static_assert(sizeof(EPipelineStageFlags) == sizeof(UInt32),
        "EPipelineStageFlags 应为 4 字节 (UInt32 位掩码)");

    static_assert(sizeof(ERHIResult) == sizeof(Int32),
        "ERHIResult 应为 4 字节 (Int32)");

    static_assert(sizeof(EQueryType) == sizeof(UInt8),
        "EQueryType 应为 1 字节 (UInt8)");

    // ---- 枚举位运算验证 ----

    static_assert((EShaderStage::Vertex | EShaderStage::Fragment)
        == EShaderStage::AllGraphics
            ? false : true,
        "AllGraphics 位掩码不等于 Vertex|Fragment (应包含更多阶段) — 仅验证可编译");

    static_assert(
        static_cast<UInt32>(EBufferUsage::VertexBuffer | EBufferUsage::TransferDst)
        == (1u | 64u),
        "EBufferUsage 位掩码组合验证");

    static_assert(
        static_cast<UInt8>(EColorWriteMask::All) == 0x0F,
        "EColorWriteMask::All 应为 0x0F");

    // ---- 基础结构体布局 ----

    static_assert(sizeof(FRHIExtent2D) == 8,
        "FRHIExtent2D 应为 8 字节 (2 x UInt32)");

    static_assert(sizeof(FRHIExtent3D) == 12,
        "FRHIExtent3D 应为 12 字节 (3 x UInt32)");

    static_assert(sizeof(FRHIOffset2D) == 8,
        "FRHIOffset2D 应为 8 字节 (2 x Int32)");

    static_assert(sizeof(FRHIOffset3D) == 12,
        "FRHIOffset3D 应为 12 字节 (3 x Int32)");

    static_assert(sizeof(FRHIViewport) == 24,
        "FRHIViewport 应为 24 字节 (6 x Float32)");

    static_assert(sizeof(FRHIScissorRect) == 16,
        "FRHIScissorRect 应为 16 字节 (2 x Int32 + 2 x UInt32)");

    static_assert(sizeof(FRHIClearColorValue) == 16,
        "FRHIClearColorValue 应为 16 字节 (4 x Float32)");

    // ---- 句柄大小 ----

    static_assert(sizeof(FRHIBufferHandle) == sizeof(UInt64),
        "FRHIBufferHandle 应为 8 字节");
    static_assert(sizeof(FRHITextureHandle) == sizeof(UInt64),
        "FRHITextureHandle 应为 8 字节");
    static_assert(sizeof(FRHITextureViewHandle) == sizeof(UInt64),
        "FRHITextureViewHandle 应为 8 字节");
    static_assert(sizeof(FRHISamplerHandle) == sizeof(UInt64),
        "FRHISamplerHandle 应为 8 字节");
    static_assert(sizeof(FRHIShaderHandle) == sizeof(UInt64),
        "FRHIShaderHandle 应为 8 字节");
    static_assert(sizeof(FRHIRenderPassHandle) == sizeof(UInt64),
        "FRHIRenderPassHandle 应为 8 字节");
    static_assert(sizeof(FRHIFramebufferHandle) == sizeof(UInt64),
        "FRHIFramebufferHandle 应为 8 字节");
    static_assert(sizeof(FRHIPipelineLayoutHandle) == sizeof(UInt64),
        "FRHIPipelineLayoutHandle 应为 8 字节");
    static_assert(sizeof(FRHIDescSetLayoutHandle) == sizeof(UInt64),
        "FRHIDescSetLayoutHandle 应为 8 字节");
    static_assert(sizeof(FRHIDescriptorSetHandle) == sizeof(UInt64),
        "FRHIDescriptorSetHandle 应为 8 字节");
    static_assert(sizeof(FRHIGraphicsPipelineHandle) == sizeof(UInt64),
        "FRHIGraphicsPipelineHandle 应为 8 字节");
    static_assert(sizeof(FRHIComputePipelineHandle) == sizeof(UInt64),
        "FRHIComputePipelineHandle 应为 8 字节");
    static_assert(sizeof(FRHIFenceHandle) == sizeof(UInt64),
        "FRHIFenceHandle 应为 8 字节");
    static_assert(sizeof(FRHISemaphoreHandle) == sizeof(UInt64),
        "FRHISemaphoreHandle 应为 8 字节");
    static_assert(sizeof(FRHICommandPoolHandle) == sizeof(UInt64),
        "FRHICommandPoolHandle 应为 8 字节");
    static_assert(sizeof(FRHICommandBufferHandle) == sizeof(UInt64),
        "FRHICommandBufferHandle 应为 8 字节");
    static_assert(sizeof(FRHISwapchainHandle) == sizeof(UInt64),
        "FRHISwapchainHandle 应为 8 字节");
    static_assert(sizeof(FRHIQueryPoolHandle) == sizeof(UInt64),
        "FRHIQueryPoolHandle 应为 8 字节");

    // ---- 句柄类型不可混用 (不同 Tag 类型的 THandle 不应隐式转换) ----

    static_assert(!IsSameV<FRHIBufferHandle, FRHITextureHandle>,
        "FRHIBufferHandle 与 FRHITextureHandle 必须为不同类型");
    static_assert(!IsSameV<FRHIFenceHandle, FRHISemaphoreHandle>,
        "FRHIFenceHandle 与 FRHISemaphoreHandle 必须为不同类型");
    static_assert(!IsSameV<FRHIGraphicsPipelineHandle, FRHIComputePipelineHandle>,
        "FRHIGraphicsPipelineHandle 与 FRHIComputePipelineHandle 必须为不同类型");

    // ---- 描述符结构体可实例化 ----

    static_assert(sizeof(FRHIBufferDesc) > 0,
        "FRHIBufferDesc 必须可实例化");
    static_assert(sizeof(FRHITextureDesc) > 0,
        "FRHITextureDesc 必须可实例化");
    static_assert(sizeof(FRHITextureViewDesc) > 0,
        "FRHITextureViewDesc 必须可实例化");
    static_assert(sizeof(FRHISamplerDesc) > 0,
        "FRHISamplerDesc 必须可实例化");
    static_assert(sizeof(FRHIShaderDesc) > 0,
        "FRHIShaderDesc 必须可实例化");
    static_assert(sizeof(FRHIAttachmentDesc) > 0,
        "FRHIAttachmentDesc 必须可实例化");
    static_assert(sizeof(FRHISubpassDesc) > 0,
        "FRHISubpassDesc 必须可实例化");
    static_assert(sizeof(FRHIRenderPassDesc) > 0,
        "FRHIRenderPassDesc 必须可实例化");
    static_assert(sizeof(FRHIFramebufferDesc) > 0,
        "FRHIFramebufferDesc 必须可实例化");
    static_assert(sizeof(FRHIDescriptorBinding) > 0,
        "FRHIDescriptorBinding 必须可实例化");
    static_assert(sizeof(FRHIDescSetLayoutDesc) > 0,
        "FRHIDescSetLayoutDesc 必须可实例化");
    static_assert(sizeof(FRHIPushConstantRange) > 0,
        "FRHIPushConstantRange 必须可实例化");
    static_assert(sizeof(FRHIPipelineLayoutDesc) > 0,
        "FRHIPipelineLayoutDesc 必须可实例化");
    static_assert(sizeof(FRHISwapchainDesc) > 0,
        "FRHISwapchainDesc 必须可实例化");
    static_assert(sizeof(FRHIQueryPoolDesc) > 0,
        "FRHIQueryPoolDesc 必须可实例化");

    // ---- 管线状态描述符 ----

    static_assert(sizeof(FRHIShaderStageDesc) > 0,
        "FRHIShaderStageDesc 必须可实例化");
    static_assert(sizeof(FRHIVertexInputStateDesc) > 0,
        "FRHIVertexInputStateDesc 必须可实例化");
    static_assert(sizeof(FRHIInputAssemblyStateDesc) > 0,
        "FRHIInputAssemblyStateDesc 必须可实例化");
    static_assert(sizeof(FRHIRasterizationStateDesc) > 0,
        "FRHIRasterizationStateDesc 必须可实例化");
    static_assert(sizeof(FRHIMultisampleStateDesc) > 0,
        "FRHIMultisampleStateDesc 必须可实例化");
    static_assert(sizeof(FRHIDepthStencilStateDesc) > 0,
        "FRHIDepthStencilStateDesc 必须可实例化");
    static_assert(sizeof(FRHIColorBlendAttachmentDesc) > 0,
        "FRHIColorBlendAttachmentDesc 必须可实例化");
    static_assert(sizeof(FRHIColorBlendStateDesc) > 0,
        "FRHIColorBlendStateDesc 必须可实例化");
    static_assert(sizeof(FRHIGraphicsPipelineDesc) > 0,
        "FRHIGraphicsPipelineDesc 必须可实例化");
    static_assert(sizeof(FRHIComputePipelineDesc) > 0,
        "FRHIComputePipelineDesc 必须可实例化");

    // ---- 命令缓冲区辅助结构体 ----

    static_assert(sizeof(FRHIRenderPassBeginInfo) > 0,
        "FRHIRenderPassBeginInfo 必须可实例化");
    static_assert(sizeof(FRHIBufferCopyRegion) == 24,
        "FRHIBufferCopyRegion 应为 24 字节 (3 x UInt64)");
    static_assert(sizeof(FRHIMemoryBarrier) > 0,
        "FRHIMemoryBarrier 必须可实例化");
    static_assert(sizeof(FRHIImageMemoryBarrier) > 0,
        "FRHIImageMemoryBarrier 必须可实例化");
    static_assert(sizeof(FRHIBufferMemoryBarrier) > 0,
        "FRHIBufferMemoryBarrier 必须可实例化");

    // ---- 提交与呈现 ----

    static_assert(sizeof(FRHISubmitInfo) > 0,
        "FRHISubmitInfo 必须可实例化");
    static_assert(sizeof(FRHIPresentInfo) > 0,
        "FRHIPresentInfo 必须可实例化");

    // ---- Vulkan 后端实现类 ----

    static_assert(sizeof(FVulkanDevice) > 0,
        "FVulkanDevice 必须可实例化");
    static_assert(sizeof(FVulkanCommandBuffer) > 0,
        "FVulkanCommandBuffer 必须可实例化");

    // 验证 FVulkanDevice 继承自 IRHIDevice
    static_assert(IsBaseOfV<IRHIDevice, FVulkanDevice>,
        "FVulkanDevice 必须继承 IRHIDevice");

    // 验证 FVulkanCommandBuffer 继承自 IRHICommandBuffer
    static_assert(IsBaseOfV<IRHICommandBuffer, FVulkanCommandBuffer>,
        "FVulkanCommandBuffer 必须继承 IRHICommandBuffer");

    // ---- 工具函数验证 ----

    static_assert(GetPixelFormatByteSize(EPixelFormat::RGBA8_UNORM) == 4,
        "RGBA8_UNORM 每像素应为 4 字节");
    static_assert(GetPixelFormatByteSize(EPixelFormat::RGBA16_SFLOAT) == 8,
        "RGBA16_SFLOAT 每像素应为 8 字节");
    static_assert(GetPixelFormatByteSize(EPixelFormat::RGBA32_SFLOAT) == 16,
        "RGBA32_SFLOAT 每像素应为 16 字节");
    static_assert(GetPixelFormatByteSize(EPixelFormat::D32_SFLOAT) == 4,
        "D32_SFLOAT 每像素应为 4 字节");
    static_assert(GetPixelFormatByteSize(EPixelFormat::BC7_UNORM) == 16,
        "BC7_UNORM 每块应为 16 字节");
    static_assert(GetPixelFormatByteSize(EPixelFormat::Unknown) == 0,
        "Unknown 格式应返回 0 字节");

    static_assert(IsDepthFormat(EPixelFormat::D32_SFLOAT) == true,
        "D32_SFLOAT 应为深度格式");
    static_assert(IsDepthFormat(EPixelFormat::RGBA8_UNORM) == false,
        "RGBA8_UNORM 不应为深度格式");
    static_assert(IsStencilFormat(EPixelFormat::D24_UNORM_S8_UINT) == true,
        "D24_UNORM_S8_UINT 应包含模板");
    static_assert(IsCompressedFormat(EPixelFormat::BC3_UNORM) == true,
        "BC3_UNORM 应为压缩格式");
    static_assert(IsSRGBFormat(EPixelFormat::RGBA8_SRGB) == true,
        "RGBA8_SRGB 应为 sRGB 格式");
    static_assert(IsFloatFormat(EPixelFormat::R16_SFLOAT) == true,
        "R16_SFLOAT 应为浮点格式");

    static_assert(GetIndexTypeByteSize(EIndexType::UInt16) == 2,
        "UInt16 索引应为 2 字节");
    static_assert(GetIndexTypeByteSize(EIndexType::UInt32) == 4,
        "UInt32 索引应为 4 字节");

} // anonymous namespace

// ============================================================================
// 模块生命周期
// ============================================================================

/// 模块初始化
void ModuleStartup()
{
}

/// 模块关闭
void ModuleShutdown()
{
}

// ============================================================================
// RHI 工厂函数实现
// ============================================================================

TUniquePtr<IRHIDevice> CreateRHIDevice(void* nativeWindowHandle,
                                        bool enableValidation)
{
    auto device = MakeUnique<FVulkanDevice>();
    ERHIResult result = device->Initialize(nativeWindowHandle,
                                            enableValidation);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRHI, Error,
            "[RHIFactory] RHI 设备初始化失败: {}",
            static_cast<Int32>(result));
        return TUniquePtr<IRHIDevice>();
    }

    // TUniquePtr<FVulkanDevice> → TUniquePtr<IRHIDevice> 隐式向上转换
    return TUniquePtr<IRHIDevice>(device.Release());
}

TUniquePtr<IRHICommandBuffer> CreateRHICommandBuffer(
    IRHIDevice* device,
    FRHICommandBufferHandle handle)
{
    // 安全向下转换 — 当前仅支持 Vulkan 后端
    auto* vulkanDevice = static_cast<FVulkanDevice*>(device);
    auto commandBuffer = MakeUnique<FVulkanCommandBuffer>(
        vulkanDevice, handle);
    return TUniquePtr<IRHICommandBuffer>(commandBuffer.Release());
}

} // namespace Limx
