/*******************************************************************************
 * 文件名称：RHIResources.h
 * 创建时间：2025-07-27
 * 创建者  ：LimxTeam
 * 设计哲学：类型安全句柄 + 描述符驱动创建，所有 GPU 资源通过不透明句柄引用，
 *          公开接口不暴露 Vulkan 原生类型，资源创建使用描述符结构体传参，
 *          句柄基于 THandle<Tag> 模板提供代 (Generation) 悬挂检测。
 * 功能描述：RHI 资源句柄类型与资源创建描述符定义，涵盖缓冲区、纹理、
 *          纹理视图、采样器、着色器模块、渲染通道、帧缓冲、管线布局、
 *          描述符集布局、管线状态对象、栅栏、信号量等全部 GPU 资源。
 * 技术特性：句柄为 8 字节 POD (索引+代)，描述符结构体为纯 POD 聚合体，
 *          两者均可值传递、可哈希、可序列化。
 *
 * ── 句柄类型表 ──────────────────────────────────────────────
 * │ 句柄类型                   │ 描述                          │
 * │───────────────────────────│──────────────────────────────│
 * │ FRHIBufferHandle          │ GPU 缓冲区                     │
 * │ FRHITextureHandle         │ GPU 纹理/图像                   │
 * │ FRHITextureViewHandle     │ 纹理视图                       │
 * │ FRHISamplerHandle         │ 采样器状态                      │
 * │ FRHIShaderHandle          │ 着色器模块                      │
 * │ FRHIRenderPassHandle      │ 渲染通道                       │
 * │ FRHIFramebufferHandle     │ 帧缓冲                        │
 * │ FRHIPipelineLayoutHandle  │ 管线布局                       │
 * │ FRHIDescSetLayoutHandle   │ 描述符集布局                    │
 * │ FRHIDescriptorSetHandle   │ 描述符集实例                    │
 * │ FRHIGraphicsPipelineHandle│ 图形管线状态对象                 │
 * │ FRHIComputePipelineHandle │ 计算管线状态对象                 │
 * │ FRHIFenceHandle           │ CPU-GPU 同步栅栏                │
 * │ FRHISemaphoreHandle       │ GPU-GPU 同步信号量              │
 * │ FRHICommandPoolHandle     │ 命令池                         │
 * │ FRHICommandBufferHandle   │ 命令缓冲区                      │
 * │ FRHISwapchainHandle       │ 交换链                         │
 * │ FRHIQueryPoolHandle       │ 查询池 (时间戳/遮挡)             │
 *
 * ── 描述符结构体表 ──────────────────────────────────────────
 * │ 结构体名                   │ 描述                          │
 * │───────────────────────────│──────────────────────────────│
 * │ FRHIBufferDesc            │ 缓冲区创建描述符                 │
 * │ FRHITextureDesc           │ 纹理创建描述符                   │
 * │ FRHITextureViewDesc       │ 纹理视图创建描述符                │
 * │ FRHISamplerDesc           │ 采样器创建描述符                  │
 * │ FRHIShaderDesc            │ 着色器模块创建描述符               │
 * │ FRHIAttachmentDesc        │ 渲染通道附件描述                  │
 * │ FRHISubpassDesc           │ 子通道描述                       │
 * │ FRHIRenderPassDesc        │ 渲染通道创建描述符                 │
 * │ FRHIFramebufferDesc       │ 帧缓冲创建描述符                  │
 * │ FRHIDescriptorBinding     │ 描述符绑定项                     │
 * │ FRHIDescSetLayoutDesc     │ 描述符集布局创建描述符              │
 * │ FRHIPipelineLayoutDesc    │ 管线布局创建描述符                 │
 * │ FRHISwapchainDesc         │ 交换链创建描述符                   │
 *
 * ── 更新历史 ────────────────────────────────────────────────
 * │ 日期         │ 作者       │ 描述                          │
 * │─────────────│──────────│──────────────────────────────│
 * │ 2025-07-27  │ LimxTeam  │ 初始创建                       │
 * ============================================================
 ******************************************************************************/

#pragma once

#include "Core/CoreTypes.h"
#include "RHI/RHI/RHIDefinitions.h"

namespace Limx
{

// ============================================================================
// 句柄标签类型 — 每个标签对应一种 GPU 资源，保证句柄不可混用
// ============================================================================

namespace RHITags
{
    struct BufferTag {};
    struct TextureTag {};
    struct TextureViewTag {};
    struct SamplerTag {};
    struct ShaderTag {};
    struct RenderPassTag {};
    struct FramebufferTag {};
    struct PipelineLayoutTag {};
    struct DescSetLayoutTag {};
    struct DescriptorSetTag {};
    struct GraphicsPipelineTag {};
    struct ComputePipelineTag {};
    struct FenceTag {};
    struct SemaphoreTag {};
    struct CommandPoolTag {};
    struct CommandBufferTag {};
    struct SwapchainTag {};
    struct QueryPoolTag {};
} // namespace RHITags

// ============================================================================
// 类型化句柄 — 基于 THandle<Tag>，8 字节 POD (索引32位 + 代32位)
// ============================================================================

using FRHIBufferHandle          = THandle<RHITags::BufferTag>;
using FRHITextureHandle         = THandle<RHITags::TextureTag>;
using FRHITextureViewHandle     = THandle<RHITags::TextureViewTag>;
using FRHISamplerHandle         = THandle<RHITags::SamplerTag>;
using FRHIShaderHandle          = THandle<RHITags::ShaderTag>;
using FRHIRenderPassHandle      = THandle<RHITags::RenderPassTag>;
using FRHIFramebufferHandle     = THandle<RHITags::FramebufferTag>;
using FRHIPipelineLayoutHandle  = THandle<RHITags::PipelineLayoutTag>;
using FRHIDescSetLayoutHandle   = THandle<RHITags::DescSetLayoutTag>;
using FRHIDescriptorSetHandle   = THandle<RHITags::DescriptorSetTag>;
using FRHIGraphicsPipelineHandle = THandle<RHITags::GraphicsPipelineTag>;
using FRHIComputePipelineHandle = THandle<RHITags::ComputePipelineTag>;
using FRHIFenceHandle           = THandle<RHITags::FenceTag>;
using FRHISemaphoreHandle       = THandle<RHITags::SemaphoreTag>;
using FRHICommandPoolHandle     = THandle<RHITags::CommandPoolTag>;
using FRHICommandBufferHandle   = THandle<RHITags::CommandBufferTag>;
using FRHISwapchainHandle       = THandle<RHITags::SwapchainTag>;
using FRHIQueryPoolHandle       = THandle<RHITags::QueryPoolTag>;

// ============================================================================
// FRHIBufferDesc — 缓冲区创建描述符
// ============================================================================

struct FRHIBufferDesc
{
    // 缓冲区大小 (字节)
    UInt64 Size = 0;

    // 用途标志 (位掩码组合)
    EBufferUsage Usage = EBufferUsage::None;

    // 内存分配策略
    EMemoryUsage MemoryUsage = EMemoryUsage::GpuOnly;

    // 调试名称 (可选，用于 GPU 调试工具标记)
    const char* DebugName = nullptr;

    // 便捷工厂: 顶点缓冲区
    static FRHIBufferDesc Vertex(UInt64 size, EMemoryUsage memory = EMemoryUsage::GpuOnly)
    {
        FRHIBufferDesc desc;
        desc.Size = size;
        desc.Usage = EBufferUsage::VertexBuffer | EBufferUsage::TransferDst;
        desc.MemoryUsage = memory;
        return desc;
    }

    // 便捷工厂: 索引缓冲区
    static FRHIBufferDesc Index(UInt64 size, EMemoryUsage memory = EMemoryUsage::GpuOnly)
    {
        FRHIBufferDesc desc;
        desc.Size = size;
        desc.Usage = EBufferUsage::IndexBuffer | EBufferUsage::TransferDst;
        desc.MemoryUsage = memory;
        return desc;
    }

    // 便捷工厂: Uniform 缓冲区
    static FRHIBufferDesc Uniform(UInt64 size)
    {
        FRHIBufferDesc desc;
        desc.Size = size;
        desc.Usage = EBufferUsage::UniformBuffer;
        desc.MemoryUsage = EMemoryUsage::CpuToGpu;
        return desc;
    }

    // 便捷工厂: Storage 缓冲区
    static FRHIBufferDesc Storage(UInt64 size, EMemoryUsage memory = EMemoryUsage::GpuOnly)
    {
        FRHIBufferDesc desc;
        desc.Size = size;
        desc.Usage = EBufferUsage::StorageBuffer | EBufferUsage::TransferDst;
        desc.MemoryUsage = memory;
        return desc;
    }

    // 便捷工厂: 暂存缓冲区 (CPU→GPU 上传)
    static FRHIBufferDesc Staging(UInt64 size)
    {
        FRHIBufferDesc desc;
        desc.Size = size;
        desc.Usage = EBufferUsage::TransferSrc;
        desc.MemoryUsage = EMemoryUsage::CpuOnly;
        return desc;
    }
};

// ============================================================================
// FRHITextureDesc — 纹理创建描述符
// ============================================================================

struct FRHITextureDesc
{
    // 纹理类型
    ETextureType Type = ETextureType::Texture2D;

    // 像素格式
    EPixelFormat Format = EPixelFormat::RGBA8_UNORM;

    // 尺寸
    FRHIExtent3D Extent = { 1, 1, 1 };

    // Mip 层级数 (1 = 无 mipmap)
    UInt32 MipLevels = 1;

    // 数组层数 (Cube 纹理为 6 的倍数)
    UInt32 ArrayLayers = 1;

    // 多重采样数
    ESampleCount Samples = ESampleCount::Count1;

    // 用途标志 (位掩码组合)
    ETextureUsage Usage = ETextureUsage::Sampled;

    // 内存分配策略
    EMemoryUsage MemoryUsage = EMemoryUsage::GpuOnly;

    // 初始布局
    EImageLayout InitialLayout = EImageLayout::Undefined;

    // 调试名称
    const char* DebugName = nullptr;

    // 便捷工厂: 2D 纹理
    static FRHITextureDesc Texture2D(
        UInt32 width, UInt32 height,
        EPixelFormat format = EPixelFormat::RGBA8_UNORM,
        UInt32 mipLevels = 1,
        ETextureUsage usage = ETextureUsage::Sampled | ETextureUsage::TransferDst)
    {
        FRHITextureDesc desc;
        desc.Type = ETextureType::Texture2D;
        desc.Format = format;
        desc.Extent = { width, height, 1 };
        desc.MipLevels = mipLevels;
        desc.Usage = usage;
        return desc;
    }

    // 便捷工厂: 渲染目标
    static FRHITextureDesc RenderTarget(
        UInt32 width, UInt32 height,
        EPixelFormat format = EPixelFormat::RGBA8_UNORM,
        ESampleCount samples = ESampleCount::Count1)
    {
        FRHITextureDesc desc;
        desc.Type = ETextureType::Texture2D;
        desc.Format = format;
        desc.Extent = { width, height, 1 };
        desc.Samples = samples;
        desc.Usage = ETextureUsage::ColorAttachment | ETextureUsage::Sampled;
        return desc;
    }

    // 便捷工厂: 深度模板附件
    static FRHITextureDesc DepthStencil(
        UInt32 width, UInt32 height,
        EPixelFormat format = EPixelFormat::D32_SFLOAT,
        ESampleCount samples = ESampleCount::Count1)
    {
        FRHITextureDesc desc;
        desc.Type = ETextureType::Texture2D;
        desc.Format = format;
        desc.Extent = { width, height, 1 };
        desc.Samples = samples;
        desc.Usage = ETextureUsage::DepthStencilAttachment | ETextureUsage::Sampled;
        return desc;
    }

    // 便捷工厂: Cube 贴图
    static FRHITextureDesc Cube(
        UInt32 size,
        EPixelFormat format = EPixelFormat::RGBA16_SFLOAT,
        UInt32 mipLevels = 1)
    {
        FRHITextureDesc desc;
        desc.Type = ETextureType::TextureCube;
        desc.Format = format;
        desc.Extent = { size, size, 1 };
        desc.MipLevels = mipLevels;
        desc.ArrayLayers = 6;
        desc.Usage = ETextureUsage::Sampled | ETextureUsage::TransferDst;
        return desc;
    }

    // 便捷工厂: 存储纹理 (Compute 写入)
    static FRHITextureDesc StorageTexture(
        UInt32 width, UInt32 height,
        EPixelFormat format = EPixelFormat::RGBA16_SFLOAT)
    {
        FRHITextureDesc desc;
        desc.Type = ETextureType::Texture2D;
        desc.Format = format;
        desc.Extent = { width, height, 1 };
        desc.Usage = ETextureUsage::Storage | ETextureUsage::Sampled;
        return desc;
    }
};

// ============================================================================
// FRHITextureViewDesc — 纹理视图创建描述符
// ============================================================================

struct FRHITextureViewDesc
{
    // 源纹理句柄
    FRHITextureHandle Texture;

    // 视图类型 (可与源纹理类型不同，例如 Cube→2DArray)
    ETextureType ViewType = ETextureType::Texture2D;

    // 像素格式 (可与源纹理格式兼容但不同，例如 RGBA8_UNORM→RGBA8_SRGB)
    EPixelFormat Format = EPixelFormat::Unknown;

    // Mip 子范围
    UInt32 BaseMipLevel = 0;
    UInt32 MipLevelCount = 1;

    // 数组层子范围
    UInt32 BaseArrayLayer = 0;
    UInt32 ArrayLayerCount = 1;
};

// ============================================================================
// FRHISamplerDesc — 采样器创建描述符
// ============================================================================

struct FRHISamplerDesc
{
    // 缩小/放大过滤
    EFilter MinFilter = EFilter::Linear;
    EFilter MagFilter = EFilter::Linear;

    // Mipmap 过滤
    ESamplerMipmapMode MipmapMode = ESamplerMipmapMode::Linear;

    // 寻址模式 (UVW)
    ESamplerAddressMode AddressModeU = ESamplerAddressMode::Repeat;
    ESamplerAddressMode AddressModeV = ESamplerAddressMode::Repeat;
    ESamplerAddressMode AddressModeW = ESamplerAddressMode::Repeat;

    // Mip LOD 偏移
    Float32 MipLodBias = 0.0f;

    // 各向异性过滤
    bool    IsAnisotropyEnabled = true;
    Float32 MaxAnisotropy       = 16.0f;

    // 比较采样 (用于 PCF 阴影)
    bool       IsCompareEnabled = false;
    ECompareOp CompareOp        = ECompareOp::Less;

    // LOD 范围
    Float32 MinLod = 0.0f;
    Float32 MaxLod = 1000.0f;

    // 边框颜色 (ClampToBorder 时使用)
    Float32 BorderColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

    // 便捷工厂: 线性采样 + 重复寻址
    static FRHISamplerDesc LinearRepeat()
    {
        FRHISamplerDesc desc;
        return desc;
    }

    // 便捷工厂: 最近邻采样 + 重复寻址
    static FRHISamplerDesc NearestRepeat()
    {
        FRHISamplerDesc desc;
        desc.MinFilter = EFilter::Nearest;
        desc.MagFilter = EFilter::Nearest;
        desc.MipmapMode = ESamplerMipmapMode::Nearest;
        desc.IsAnisotropyEnabled = false;
        return desc;
    }

    // 便捷工厂: 线性采样 + 钳位寻址
    static FRHISamplerDesc LinearClamp()
    {
        FRHISamplerDesc desc;
        desc.AddressModeU = ESamplerAddressMode::ClampToEdge;
        desc.AddressModeV = ESamplerAddressMode::ClampToEdge;
        desc.AddressModeW = ESamplerAddressMode::ClampToEdge;
        return desc;
    }

    // 便捷工厂: PCF 阴影采样器
    static FRHISamplerDesc Shadow()
    {
        FRHISamplerDesc desc;
        desc.AddressModeU = ESamplerAddressMode::ClampToBorder;
        desc.AddressModeV = ESamplerAddressMode::ClampToBorder;
        desc.AddressModeW = ESamplerAddressMode::ClampToBorder;
        desc.IsCompareEnabled = true;
        desc.CompareOp = ECompareOp::LessOrEqual;
        desc.IsAnisotropyEnabled = false;
        desc.BorderColor[0] = 1.0f;
        desc.BorderColor[1] = 1.0f;
        desc.BorderColor[2] = 1.0f;
        desc.BorderColor[3] = 1.0f;
        return desc;
    }
};

// ============================================================================
// FRHIShaderDesc — 着色器模块创建描述符
// ============================================================================

struct FRHIShaderDesc
{
    // 着色器阶段
    EShaderStage Stage = EShaderStage::Vertex;

    // SPIR-V 字节码指针
    const UInt8* ByteCode = nullptr;

    // SPIR-V 字节码大小 (字节)
    UInt64 ByteCodeSize = 0;

    // 入口函数名 (通常为 "main")
    const char* EntryPoint = "main";

    // 调试名称
    const char* DebugName = nullptr;
};

// ============================================================================
// FRHIAttachmentDesc — 渲染通道附件描述
// ============================================================================

struct FRHIAttachmentDesc
{
    // 像素格式
    EPixelFormat Format = EPixelFormat::RGBA8_UNORM;

    // 多重采样数
    ESampleCount Samples = ESampleCount::Count1;

    // 加载/存储操作
    ELoadOp  LoadOp  = ELoadOp::Clear;
    EStoreOp StoreOp = EStoreOp::Store;

    // 模板加载/存储操作
    ELoadOp  StencilLoadOp  = ELoadOp::DontCare;
    EStoreOp StencilStoreOp = EStoreOp::DontCare;

    // 布局转换
    EImageLayout InitialLayout = EImageLayout::Undefined;
    EImageLayout FinalLayout   = EImageLayout::PresentSrc;
};

// ============================================================================
// FRHIAttachmentReference — 子通道附件引用
// ============================================================================

struct FRHIAttachmentReference
{
    // 附件索引 (对应 FRHIRenderPassDesc::Attachments 数组)
    UInt32 AttachmentIndex = 0xFFFFFFFF;

    // 子通道中使用的布局
    EImageLayout Layout = EImageLayout::ColorAttachment;
};

// ============================================================================
// FRHISubpassDesc — 子通道描述
// ============================================================================

struct FRHISubpassDesc
{
    // 颜色附件引用
    const FRHIAttachmentReference* ColorAttachments    = nullptr;
    UInt32                         ColorAttachmentCount = 0;

    // 输入附件引用
    const FRHIAttachmentReference* InputAttachments    = nullptr;
    UInt32                         InputAttachmentCount = 0;

    // 深度模板附件引用 (最多一个)
    const FRHIAttachmentReference* DepthStencilAttachment = nullptr;

    // 保留附件索引 (不被本子通道使用但需保留内容)
    const UInt32* PreserveAttachments    = nullptr;
    UInt32        PreserveAttachmentCount = 0;
};

// ============================================================================
// FRHISubpassDependency — 子通道依赖
// ============================================================================

struct FRHISubpassDependency
{
    // 源/目标子通道索引 (0xFFFFFFFF 表示外部)
    UInt32 SrcSubpass = 0xFFFFFFFF;
    UInt32 DstSubpass = 0;

    // 源/目标管线阶段
    EPipelineStageFlags SrcStageMask = EPipelineStageFlags::ColorAttachmentOutput;
    EPipelineStageFlags DstStageMask = EPipelineStageFlags::ColorAttachmentOutput;

    // 源/目标访问标志
    EAccessFlags SrcAccessMask = EAccessFlags::None;
    EAccessFlags DstAccessMask = EAccessFlags::ColorAttachmentWrite;
};

// ============================================================================
// FRHIRenderPassDesc — 渲染通道创建描述符
// ============================================================================

struct FRHIRenderPassDesc
{
    // 附件数组
    const FRHIAttachmentDesc* Attachments    = nullptr;
    UInt32                    AttachmentCount = 0;

    // 子通道数组
    const FRHISubpassDesc* Subpasses    = nullptr;
    UInt32                 SubpassCount = 0;

    // 子通道依赖数组
    const FRHISubpassDependency* Dependencies    = nullptr;
    UInt32                       DependencyCount = 0;

    // 调试名称
    const char* DebugName = nullptr;
};

// ============================================================================
// FRHIFramebufferDesc — 帧缓冲创建描述符
// ============================================================================

struct FRHIFramebufferDesc
{
    // 关联的渲染通道
    FRHIRenderPassHandle RenderPass;

    // 附件纹理视图数组
    const FRHITextureViewHandle* Attachments    = nullptr;
    UInt32                       AttachmentCount = 0;

    // 帧缓冲尺寸
    UInt32 Width  = 0;
    UInt32 Height = 0;
    UInt32 Layers = 1;

    // 调试名称
    const char* DebugName = nullptr;
};

// ============================================================================
// FRHIDeviceMemoryStats — 设备显存统计
// ============================================================================

/// 分配器视角的显存占用
///
/// 与资源管理器的统计是**两个不同的口径**, 两个都需要:
///   资源管理器只统计经它注册的网格与纹理 —— 那是关卡加载/卸载能控制的
///     部分, 也正是"切关卡后显存该回落到零"这条判据的对象。
///   分配器统计的是真实占用 —— 渲染目标、阴影贴图、IBL 的几张立方体贴图、
///     每帧的 UBO 全都不经资源管理器, 却实打实占着显存。
///
/// 只看前者会让人以为显存已经归零, 而实际上还有几十 MB 挂在渲染器上;
/// 只看后者则分不清哪些是关卡资产、哪些是引擎自身的常驻开销。
struct FRHIDeviceMemoryStats
{
    /// 存活的设备内存分配数 (vkAllocateMemory 口径)
    UInt32 AllocationCount = 0;

    /// 设备允许的最大分配数
    UInt32 AllocationLimit = 0;

    /// 已向驱动申请的显存总量 —— 含分块中尚未使用的部分
    UInt64 ReservedBytes = 0;

    /// 已被实际占用的显存总量
    UInt64 UsedBytes = 0;
};

// ============================================================================
// FRHIDescriptorBinding — 描述符绑定项
// ============================================================================

struct FRHIDescriptorBinding
{
    // 绑定索引
    UInt32 Binding = 0;

    // 描述符类型
    EDescriptorType Type = EDescriptorType::UniformBuffer;

    // 描述符数量 (数组大小，通常为 1)
    UInt32 Count = 1;

    // 可见的着色器阶段
    EShaderStage StageFlags = EShaderStage::All;

    /// 允许数组中存在未写入的槽位 (VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT)
    ///
    /// bindless 的大纹理数组必然是稀疏的 —— 声明 1024 个槽位而场景只用了
    /// 69 个。不置这个标志时 Vulkan 要求每个槽位都被写过, 否则绘制时报
    /// "descriptor not valid"。
    ///
    /// 它只是允许"没写", 不是允许"读到没写的"。着色器仍然不能索引到未写
    /// 的槽位 —— 那是未定义行为, 而且验证层不一定抓得到, 因为索引是运行时
    /// 计算的。材质里的纹理下标必须保证有效, 缺贴图时指向占位纹理而非 -1。
    bool PartiallyBound = false;

    /// 描述符可在绑定后更新 (VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT)
    ///
    /// 允许在命令缓冲区已经绑定该集之后再写入其中的描述符。纹理表随场景
    /// 加载增长时需要它 —— 否则每加一张贴图就要等 GPU 空闲。
    bool UpdateAfterBind = false;
};

// ============================================================================
// FRHIDescSetLayoutDesc — 描述符集布局创建描述符
// ============================================================================

struct FRHIDescSetLayoutDesc
{
    // 绑定项数组
    const FRHIDescriptorBinding* Bindings    = nullptr;
    UInt32                       BindingCount = 0;

    // 调试名称
    const char* DebugName = nullptr;
};

// ============================================================================
// FRHIPushConstantRange — Push Constant 范围
// ============================================================================

struct FRHIPushConstantRange
{
    // 可见的着色器阶段
    EShaderStage StageFlags = EShaderStage::Vertex | EShaderStage::Fragment;

    // 偏移 (字节)
    UInt32 Offset = 0;

    // 大小 (字节，Vulkan 要求最少 128 字节)
    UInt32 Size = 0;
};

// ============================================================================
// FRHIPipelineLayoutDesc — 管线布局创建描述符
// ============================================================================

struct FRHIPipelineLayoutDesc
{
    // 描述符集布局数组 (set 0, set 1, ...)
    const FRHIDescSetLayoutHandle* SetLayouts    = nullptr;
    UInt32                         SetLayoutCount = 0;

    // Push Constant 范围数组
    const FRHIPushConstantRange* PushConstantRanges    = nullptr;
    UInt32                       PushConstantRangeCount = 0;

    // 调试名称
    const char* DebugName = nullptr;
};

// ============================================================================
// FRHIDescriptorWrite — 描述符集写入项
// ============================================================================

struct FRHIDescriptorWrite
{
    // 目标描述符集
    FRHIDescriptorSetHandle DescriptorSet;

    // 绑定索引
    UInt32 Binding = 0;

    // 数组中的起始元素 (通常为 0)
    UInt32 ArrayElement = 0;

    // 描述符类型
    EDescriptorType Type = EDescriptorType::UniformBuffer;

    // ── 缓冲区绑定 (UniformBuffer / StorageBuffer) ──

    // 缓冲区句柄
    FRHIBufferHandle Buffer;

    // 缓冲区偏移 (字节)
    UInt64 BufferOffset = 0;

    // 缓冲区绑定范围 (字节, 0 = 整个缓冲区)
    UInt64 BufferRange  = 0;

    // ── 图像绑定 (CombinedImageSampler / SampledImage / StorageImage) ──

    // 纹理视图句柄
    FRHITextureViewHandle ImageView;

    // 采样器句柄
    FRHISamplerHandle Sampler;

    // 图像布局
    EImageLayout ImageLayout = EImageLayout::ShaderReadOnly;

    // 便捷工厂: Uniform 缓冲区绑定
    static FRHIDescriptorWrite UniformBuffer(
        FRHIDescriptorSetHandle set,
        UInt32 binding,
        FRHIBufferHandle buffer,
        UInt64 offset = 0,
        UInt64 range  = 0)
    {
        FRHIDescriptorWrite write;
        write.DescriptorSet = set;
        write.Binding       = binding;
        write.Type          = EDescriptorType::UniformBuffer;
        write.Buffer        = buffer;
        write.BufferOffset  = offset;
        write.BufferRange   = range;
        return write;
    }

    // 便捷工厂: Combined Image Sampler 绑定
    static FRHIDescriptorWrite CombinedImageSampler(
        FRHIDescriptorSetHandle set,
        UInt32 binding,
        FRHITextureViewHandle imageView,
        FRHISamplerHandle sampler,
        EImageLayout layout = EImageLayout::ShaderReadOnly)
    {
        FRHIDescriptorWrite write;
        write.DescriptorSet = set;
        write.Binding       = binding;
        write.Type          = EDescriptorType::CombinedImageSampler;
        write.ImageView     = imageView;
        write.Sampler       = sampler;
        write.ImageLayout   = layout;
        return write;
    }

    // 便捷工厂: Storage 缓冲区绑定
    static FRHIDescriptorWrite StorageBuffer(
        FRHIDescriptorSetHandle set,
        UInt32 binding,
        FRHIBufferHandle buffer,
        UInt64 offset = 0,
        UInt64 range  = 0)
    {
        FRHIDescriptorWrite write;
        write.DescriptorSet = set;
        write.Binding       = binding;
        write.Type          = EDescriptorType::StorageBuffer;
        write.Buffer        = buffer;
        write.BufferOffset  = offset;
        write.BufferRange   = range;
        return write;
    }
};

// ============================================================================
// FRHISwapchainDesc — 交换链创建描述符
// ============================================================================

struct FRHISwapchainDesc
{
    // 期望尺寸
    UInt32 Width  = 0;
    UInt32 Height = 0;

    // 期望格式 (驱动可能选择兼容的替代格式)
    EPixelFormat PreferredFormat = EPixelFormat::BGRA8_SRGB;

    // 期望帧缓冲数量 (通常 2=双缓冲, 3=三缓冲)
    UInt32 BufferCount = 3;

    // 是否启用垂直同步
    bool IsVSyncEnabled = false;

    // 原生窗口句柄 (HWND on Windows)
    void* NativeWindowHandle = nullptr;

    // 调试名称
    const char* DebugName = nullptr;
};

// ============================================================================
// 命令缓冲区级别
// ============================================================================

enum class ECommandBufferLevel : UInt8
{
    /// 主命令缓冲区 — 直接提交到队列
    Primary = 0,

    /// 次级命令缓冲区 — 只能被主缓冲区通过 ExecuteCommands 调用
    ///
    /// 存在的理由是并行录制: 多个线程各自往自己的次级缓冲区里录制绘制
    /// 命令, 主线程最后把它们串起来。Vulkan 不允许多个线程同时往同一个
    /// 命令缓冲区写, 也不允许共用一个命令池 —— 命令池必须由调用方外部
    /// 同步, 所以每个录制线程都要有自己的池。
    Secondary,
};

// ============================================================================
// FRHICommandBufferInheritance — 次级缓冲区的继承信息
// ============================================================================

/// 次级命令缓冲区在渲染通道内录制时, 必须预先声明它将在哪个通道、哪个
/// 子通道、哪个帧缓冲里被执行。
///
/// 这不是可选的形式主义: 驱动要据此确定附件格式与采样数, 才能在录制阶段
/// 就把管线状态编译到位。填错的表现通常不是报错, 而是执行时行为未定义。
struct FRHICommandBufferInheritance
{
    /// 将在其中执行的渲染通道
    FRHIRenderPassHandle RenderPass;

    /// 子通道下标
    UInt32 Subpass = 0;

    /// 将在其中执行的帧缓冲 (可为空句柄, 但给出能让驱动做更多优化)
    FRHIFramebufferHandle Framebuffer;
};

// ============================================================================
// FRHIQueryPoolDesc — 查询池描述符
// ============================================================================

enum class EQueryType : UInt8
{
    Occlusion = 0,
    Timestamp,
    PipelineStatistics,

    Count
};

struct FRHIQueryPoolDesc
{
    // 查询类型
    EQueryType Type = EQueryType::Timestamp;

    // 查询数量
    UInt32 QueryCount = 0;

    // 调试名称
    const char* DebugName = nullptr;
};

} // namespace Limx
