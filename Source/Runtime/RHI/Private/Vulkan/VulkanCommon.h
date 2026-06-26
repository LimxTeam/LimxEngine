// ============================================================
// 文件名称：VulkanCommon.h
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：集中管理 Vulkan 头文件包含与 RHI↔Vulkan 枚举转换，
//          所有转换函数为 inline 避免链接开销，直接映射的枚举使用
//          static_cast 零开销转换，位掩码枚举使用逐位翻译。
// 功能描述：Vulkan 后端公共基础设施 — 头文件包含、错误检查宏、
//          全部 RHI 枚举到 Vulkan 原生类型的转换函数。
// 技术特性：所有转换函数为 constexpr/inline，编译期可求值；
//          VK_CHECK 宏在调试构建中断言 Vulkan 调用结果。
//
// ── 函数表 ──────────────────────────────────────────────────
// │ 函数名                          │ 描述                       │
// │────────────────────────────────│──────────────────────────│
// │ ToVkFormat()                   │ EPixelFormat → VkFormat    │
// │ ToVkPrimitiveTopology()        │ 直接映射                   │
// │ ToVkBlendFactor()              │ 直接映射                   │
// │ ToVkBlendOp()                  │ 直接映射                   │
// │ ToVkCompareOp()                │ 直接映射                   │
// │ ToVkStencilOp()                │ 直接映射                   │
// │ ToVkCullModeFlags()            │ 直接映射                   │
// │ ToVkFrontFace()                │ 直接映射                   │
// │ ToVkPolygonMode()              │ 直接映射                   │
// │ ToVkFilter()                   │ 直接映射                   │
// │ ToVkSamplerAddressMode()       │ 直接映射                   │
// │ ToVkSamplerMipmapMode()        │ 直接映射                   │
// │ ToVkAttachmentLoadOp()         │ 直接映射                   │
// │ ToVkAttachmentStoreOp()        │ 直接映射                   │
// │ ToVkVertexInputRate()          │ 直接映射                   │
// │ ToVkIndexType()                │ 直接映射                   │
// │ ToVkShaderStageFlags()         │ 位掩码直接映射              │
// │ ToVkAccessFlags()              │ 位掩码直接映射              │
// │ ToVkPipelineStageFlags()       │ 位掩码直接映射              │
// │ ToVkSampleCountFlagBits()      │ 直接映射                   │
// │ ToVkColorComponentFlags()      │ 直接映射                   │
// │ ToVkBufferUsageFlags()         │ 逐位翻译                   │
// │ ToVkImageUsageFlags()          │ 逐位翻译                   │
// │ ToVkImageLayout()              │ switch 映射                │
// │ ToVkImageType()                │ switch 映射                │
// │ ToVkImageViewType()            │ switch 映射                │
// │ ToVkDescriptorType()           │ switch 映射                │
// │ ToVkPipelineBindPoint()        │ switch 映射                │
// │ ToVkQueryType()                │ switch 映射                │
// │ ToVkMemoryPropertyFlags()      │ switch 映射                │
// │ CollectVkDynamicStates()       │ 位掩码→VkDynamicState 数组  │
// │ ToEPixelFormat()               │ VkFormat → EPixelFormat    │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-06  │ LimxTeam  │ 初始创建                        │
// ============================================================

#pragma once

// ============================================================================
// RHI + Core 头文件 — 必须在 Vulkan 头文件之前包含
// RHIMinimal.h 设置 WIN32_LEAN_AND_MEAN/NOMINMAX 并包含 Core 完整类型系统，
// 确保 Windows 宏不会污染 Core 头文件。
// ============================================================================

#include "RHI/RHIMinimal.h"
#include "RHI/RHI/RHIDefinitions.h"
#include "RHI/RHI/RHIResources.h"
#include "RHI/RHI/RHIPipelineState.h"

// ============================================================================
// Vulkan 头文件包含
// ============================================================================

#define VK_USE_PLATFORM_WIN32_KHR
// 注意: 不要定义 VK_NO_PROTOTYPES — 即使值为 0, #ifndef 也会检测到宏已定义而跳过原型
#include <vulkan/vulkan.h>

namespace Limx
{

// 引入 Core::Memory 子命名空间的内存操作函数到 Limx 作用域
using Memory::MemCopy;
using Memory::MemMove;
using Memory::MemSet;
using Memory::MemZero;
using Memory::MemCompare;

// ============================================================================
// RHI 日志分类声明
// ============================================================================

LIMX_DECLARE_LOG_CATEGORY(LogRHI)

// ============================================================================
// VK_CHECK — Vulkan 调用结果检查宏
// ============================================================================

#if LIMX_BUILD_DEBUG || LIMX_BUILD_DEVELOPMENT
    #define VK_CHECK(vkCall)                                                   \
        do {                                                                   \
            VkResult vkResult_ = (vkCall);                                     \
            LIMX_ASSERT(vkResult_ == VK_SUCCESS);                              \
        } while (false)
#else
    #define VK_CHECK(vkCall) (vkCall)
#endif

// ============================================================================
// 直接映射枚举 — 值序与 Vulkan 一一对应，static_cast 零开销
// ============================================================================

inline VkPrimitiveTopology ToVkPrimitiveTopology(EPrimitiveTopology topology)
{
    return static_cast<VkPrimitiveTopology>(topology);
}

inline VkBlendFactor ToVkBlendFactor(EBlendFactor factor)
{
    return static_cast<VkBlendFactor>(factor);
}

inline VkBlendOp ToVkBlendOp(EBlendOp op)
{
    return static_cast<VkBlendOp>(op);
}

inline VkCompareOp ToVkCompareOp(ECompareOp op)
{
    return static_cast<VkCompareOp>(op);
}

inline VkStencilOp ToVkStencilOp(EStencilOp op)
{
    return static_cast<VkStencilOp>(op);
}

inline VkCullModeFlags ToVkCullModeFlags(ECullMode mode)
{
    return static_cast<VkCullModeFlags>(mode);
}

inline VkFrontFace ToVkFrontFace(EFrontFace face)
{
    return static_cast<VkFrontFace>(face);
}

inline VkPolygonMode ToVkPolygonMode(EPolygonMode mode)
{
    return static_cast<VkPolygonMode>(mode);
}

inline VkFilter ToVkFilter(EFilter filter)
{
    return static_cast<VkFilter>(filter);
}

inline VkSamplerAddressMode ToVkSamplerAddressMode(ESamplerAddressMode mode)
{
    return static_cast<VkSamplerAddressMode>(mode);
}

inline VkSamplerMipmapMode ToVkSamplerMipmapMode(ESamplerMipmapMode mode)
{
    return static_cast<VkSamplerMipmapMode>(mode);
}

inline VkAttachmentLoadOp ToVkAttachmentLoadOp(ELoadOp op)
{
    return static_cast<VkAttachmentLoadOp>(op);
}

inline VkAttachmentStoreOp ToVkAttachmentStoreOp(EStoreOp op)
{
    return static_cast<VkAttachmentStoreOp>(op);
}

inline VkVertexInputRate ToVkVertexInputRate(EVertexInputRate rate)
{
    return static_cast<VkVertexInputRate>(rate);
}

inline VkIndexType ToVkIndexType(EIndexType type)
{
    return static_cast<VkIndexType>(type);
}

// ============================================================================
// 位掩码直接映射 — 位位置与 Vulkan 完全一致
// ============================================================================

inline VkShaderStageFlags ToVkShaderStageFlags(EShaderStage stage)
{
    return static_cast<VkShaderStageFlags>(stage);
}

inline VkAccessFlags ToVkAccessFlags(EAccessFlags flags)
{
    return static_cast<VkAccessFlags>(flags);
}

inline VkPipelineStageFlags ToVkPipelineStageFlags(EPipelineStageFlags flags)
{
    return static_cast<VkPipelineStageFlags>(flags);
}

inline VkSampleCountFlagBits ToVkSampleCountFlagBits(ESampleCount count)
{
    return static_cast<VkSampleCountFlagBits>(count);
}

inline VkColorComponentFlags ToVkColorComponentFlags(EColorWriteMask mask)
{
    return static_cast<VkColorComponentFlags>(mask);
}

// ============================================================================
// EBufferUsage → VkBufferUsageFlags — 逐位翻译 (位序不同)
// ============================================================================

inline VkBufferUsageFlags ToVkBufferUsageFlags(EBufferUsage usage)
{
    VkBufferUsageFlags flags = 0;
    auto raw = static_cast<UInt32>(usage);

    if (raw & static_cast<UInt32>(EBufferUsage::VertexBuffer))
        flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (raw & static_cast<UInt32>(EBufferUsage::IndexBuffer))
        flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (raw & static_cast<UInt32>(EBufferUsage::UniformBuffer))
        flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (raw & static_cast<UInt32>(EBufferUsage::StorageBuffer))
        flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (raw & static_cast<UInt32>(EBufferUsage::IndirectBuffer))
        flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (raw & static_cast<UInt32>(EBufferUsage::TransferSrc))
        flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (raw & static_cast<UInt32>(EBufferUsage::TransferDst))
        flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (raw & static_cast<UInt32>(EBufferUsage::UniformTexel))
        flags |= VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
    if (raw & static_cast<UInt32>(EBufferUsage::StorageTexel))
        flags |= VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
    if (raw & static_cast<UInt32>(EBufferUsage::AccelStructBuild))
        flags |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    if (raw & static_cast<UInt32>(EBufferUsage::AccelStructStorage))
        flags |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR;
    if (raw & static_cast<UInt32>(EBufferUsage::ShaderBindingTable))
        flags |= VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR;
    if (raw & static_cast<UInt32>(EBufferUsage::ShaderDeviceAddress))
        flags |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    return flags;
}

// ============================================================================
// ETextureUsage → VkImageUsageFlags — 逐位翻译 (位序不同)
// ============================================================================

inline VkImageUsageFlags ToVkImageUsageFlags(ETextureUsage usage)
{
    VkImageUsageFlags flags = 0;
    auto raw = static_cast<UInt32>(usage);

    if (raw & static_cast<UInt32>(ETextureUsage::Sampled))
        flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (raw & static_cast<UInt32>(ETextureUsage::Storage))
        flags |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (raw & static_cast<UInt32>(ETextureUsage::ColorAttachment))
        flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (raw & static_cast<UInt32>(ETextureUsage::DepthStencilAttachment))
        flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (raw & static_cast<UInt32>(ETextureUsage::TransientAttachment))
        flags |= VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    if (raw & static_cast<UInt32>(ETextureUsage::InputAttachment))
        flags |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    if (raw & static_cast<UInt32>(ETextureUsage::TransferSrc))
        flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (raw & static_cast<UInt32>(ETextureUsage::TransferDst))
        flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (raw & static_cast<UInt32>(ETextureUsage::ShadingRate))
        flags |= VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;

    return flags;
}

// ============================================================================
// EPixelFormat → VkFormat — switch 查表
// ============================================================================

inline VkFormat ToVkFormat(EPixelFormat format)
{
    switch (format)
    {
        case EPixelFormat::R8_UNORM:              return VK_FORMAT_R8_UNORM;
        case EPixelFormat::R8_SNORM:              return VK_FORMAT_R8_SNORM;
        case EPixelFormat::R8_UINT:               return VK_FORMAT_R8_UINT;
        case EPixelFormat::R8_SINT:               return VK_FORMAT_R8_SINT;
        case EPixelFormat::RG8_UNORM:             return VK_FORMAT_R8G8_UNORM;
        case EPixelFormat::RG8_SNORM:             return VK_FORMAT_R8G8_SNORM;
        case EPixelFormat::RG8_UINT:              return VK_FORMAT_R8G8_UINT;
        case EPixelFormat::RG8_SINT:              return VK_FORMAT_R8G8_SINT;
        case EPixelFormat::RGBA8_UNORM:           return VK_FORMAT_R8G8B8A8_UNORM;
        case EPixelFormat::RGBA8_SNORM:           return VK_FORMAT_R8G8B8A8_SNORM;
        case EPixelFormat::RGBA8_UINT:            return VK_FORMAT_R8G8B8A8_UINT;
        case EPixelFormat::RGBA8_SINT:            return VK_FORMAT_R8G8B8A8_SINT;
        case EPixelFormat::RGBA8_SRGB:            return VK_FORMAT_R8G8B8A8_SRGB;
        case EPixelFormat::BGRA8_UNORM:           return VK_FORMAT_B8G8R8A8_UNORM;
        case EPixelFormat::BGRA8_SRGB:            return VK_FORMAT_B8G8R8A8_SRGB;
        case EPixelFormat::R16_UNORM:             return VK_FORMAT_R16_UNORM;
        case EPixelFormat::R16_SNORM:             return VK_FORMAT_R16_SNORM;
        case EPixelFormat::R16_UINT:              return VK_FORMAT_R16_UINT;
        case EPixelFormat::R16_SINT:              return VK_FORMAT_R16_SINT;
        case EPixelFormat::R16_SFLOAT:            return VK_FORMAT_R16_SFLOAT;
        case EPixelFormat::RG16_UNORM:            return VK_FORMAT_R16G16_UNORM;
        case EPixelFormat::RG16_SNORM:            return VK_FORMAT_R16G16_SNORM;
        case EPixelFormat::RG16_UINT:             return VK_FORMAT_R16G16_UINT;
        case EPixelFormat::RG16_SINT:             return VK_FORMAT_R16G16_SINT;
        case EPixelFormat::RG16_SFLOAT:           return VK_FORMAT_R16G16_SFLOAT;
        case EPixelFormat::RGBA16_UNORM:          return VK_FORMAT_R16G16B16A16_UNORM;
        case EPixelFormat::RGBA16_SNORM:          return VK_FORMAT_R16G16B16A16_SNORM;
        case EPixelFormat::RGBA16_UINT:           return VK_FORMAT_R16G16B16A16_UINT;
        case EPixelFormat::RGBA16_SINT:           return VK_FORMAT_R16G16B16A16_SINT;
        case EPixelFormat::RGBA16_SFLOAT:         return VK_FORMAT_R16G16B16A16_SFLOAT;
        case EPixelFormat::R32_UINT:              return VK_FORMAT_R32_UINT;
        case EPixelFormat::R32_SINT:              return VK_FORMAT_R32_SINT;
        case EPixelFormat::R32_SFLOAT:            return VK_FORMAT_R32_SFLOAT;
        case EPixelFormat::RG32_UINT:             return VK_FORMAT_R32G32_UINT;
        case EPixelFormat::RG32_SINT:             return VK_FORMAT_R32G32_SINT;
        case EPixelFormat::RG32_SFLOAT:           return VK_FORMAT_R32G32_SFLOAT;
        case EPixelFormat::RGB32_UINT:            return VK_FORMAT_R32G32B32_UINT;
        case EPixelFormat::RGB32_SINT:            return VK_FORMAT_R32G32B32_SINT;
        case EPixelFormat::RGB32_SFLOAT:          return VK_FORMAT_R32G32B32_SFLOAT;
        case EPixelFormat::RGBA32_UINT:           return VK_FORMAT_R32G32B32A32_UINT;
        case EPixelFormat::RGBA32_SINT:           return VK_FORMAT_R32G32B32A32_SINT;
        case EPixelFormat::RGBA32_SFLOAT:         return VK_FORMAT_R32G32B32A32_SFLOAT;
        case EPixelFormat::R64_SFLOAT:            return VK_FORMAT_R64_SFLOAT;
        case EPixelFormat::B10G11R11_UFLOAT_PACK32: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        case EPixelFormat::E5B9G9R9_UFLOAT_PACK32: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
        case EPixelFormat::A2R10G10B10_UNORM_PACK32: return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
        case EPixelFormat::A2B10G10R10_UNORM_PACK32: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case EPixelFormat::D16_UNORM:             return VK_FORMAT_D16_UNORM;
        case EPixelFormat::D32_SFLOAT:            return VK_FORMAT_D32_SFLOAT;
        case EPixelFormat::D24_UNORM_S8_UINT:     return VK_FORMAT_D24_UNORM_S8_UINT;
        case EPixelFormat::D32_SFLOAT_S8_UINT:    return VK_FORMAT_D32_SFLOAT_S8_UINT;
        case EPixelFormat::S8_UINT:               return VK_FORMAT_S8_UINT;
        case EPixelFormat::BC1_UNORM:             return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case EPixelFormat::BC1_SRGB:              return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
        case EPixelFormat::BC2_UNORM:             return VK_FORMAT_BC2_UNORM_BLOCK;
        case EPixelFormat::BC2_SRGB:              return VK_FORMAT_BC2_SRGB_BLOCK;
        case EPixelFormat::BC3_UNORM:             return VK_FORMAT_BC3_UNORM_BLOCK;
        case EPixelFormat::BC3_SRGB:              return VK_FORMAT_BC3_SRGB_BLOCK;
        case EPixelFormat::BC4_UNORM:             return VK_FORMAT_BC4_UNORM_BLOCK;
        case EPixelFormat::BC4_SNORM:             return VK_FORMAT_BC4_SNORM_BLOCK;
        case EPixelFormat::BC5_UNORM:             return VK_FORMAT_BC5_UNORM_BLOCK;
        case EPixelFormat::BC5_SNORM:             return VK_FORMAT_BC5_SNORM_BLOCK;
        case EPixelFormat::BC6H_UFLOAT:           return VK_FORMAT_BC6H_UFLOAT_BLOCK;
        case EPixelFormat::BC6H_SFLOAT:           return VK_FORMAT_BC6H_SFLOAT_BLOCK;
        case EPixelFormat::BC7_UNORM:             return VK_FORMAT_BC7_UNORM_BLOCK;
        case EPixelFormat::BC7_SRGB:              return VK_FORMAT_BC7_SRGB_BLOCK;
        default:                                  return VK_FORMAT_UNDEFINED;
    }
}

// ============================================================================
// VkFormat → EPixelFormat — 反向映射 (用于交换链格式查询)
// ============================================================================

inline EPixelFormat ToEPixelFormat(VkFormat format)
{
    switch (format)
    {
        case VK_FORMAT_R8G8B8A8_UNORM:    return EPixelFormat::RGBA8_UNORM;
        case VK_FORMAT_R8G8B8A8_SRGB:     return EPixelFormat::RGBA8_SRGB;
        case VK_FORMAT_B8G8R8A8_UNORM:    return EPixelFormat::BGRA8_UNORM;
        case VK_FORMAT_B8G8R8A8_SRGB:     return EPixelFormat::BGRA8_SRGB;
        case VK_FORMAT_R16G16B16A16_SFLOAT: return EPixelFormat::RGBA16_SFLOAT;
        case VK_FORMAT_R32G32B32A32_SFLOAT: return EPixelFormat::RGBA32_SFLOAT;
        case VK_FORMAT_D16_UNORM:         return EPixelFormat::D16_UNORM;
        case VK_FORMAT_D32_SFLOAT:        return EPixelFormat::D32_SFLOAT;
        case VK_FORMAT_D24_UNORM_S8_UINT: return EPixelFormat::D24_UNORM_S8_UINT;
        case VK_FORMAT_D32_SFLOAT_S8_UINT: return EPixelFormat::D32_SFLOAT_S8_UINT;
        default:                          return EPixelFormat::Unknown;
    }
}

// ============================================================================
// EImageLayout → VkImageLayout — switch 映射
// ============================================================================

inline VkImageLayout ToVkImageLayout(EImageLayout layout)
{
    switch (layout)
    {
        case EImageLayout::Undefined:
            return VK_IMAGE_LAYOUT_UNDEFINED;
        case EImageLayout::General:
            return VK_IMAGE_LAYOUT_GENERAL;
        case EImageLayout::ColorAttachment:
            return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        case EImageLayout::DepthStencilAttachment:
            return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        case EImageLayout::DepthStencilReadOnly:
            return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        case EImageLayout::ShaderReadOnly:
            return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        case EImageLayout::TransferSrc:
            return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        case EImageLayout::TransferDst:
            return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        case EImageLayout::Preinitialized:
            return VK_IMAGE_LAYOUT_PREINITIALIZED;
        case EImageLayout::PresentSrc:
            return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        case EImageLayout::DepthReadOnlyStencilAttachment:
            return VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
        case EImageLayout::DepthAttachmentStencilReadOnly:
            return VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL;
        case EImageLayout::DepthAttachment:
            return VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        case EImageLayout::DepthReadOnly:
            return VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
        case EImageLayout::StencilAttachment:
            return VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL;
        case EImageLayout::StencilReadOnly:
            return VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL;
        case EImageLayout::ReadOnly:
            return VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
        case EImageLayout::AttachmentOptimal:
            return VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
        default:
            return VK_IMAGE_LAYOUT_UNDEFINED;
    }
}

// ============================================================================
// ETextureType → VkImageType / VkImageViewType
// ============================================================================

inline VkImageType ToVkImageType(ETextureType type)
{
    switch (type)
    {
        case ETextureType::Texture1D:
        case ETextureType::Texture1DArray:
            return VK_IMAGE_TYPE_1D;
        case ETextureType::Texture2D:
        case ETextureType::Texture2DArray:
        case ETextureType::TextureCube:
        case ETextureType::TextureCubeArray:
            return VK_IMAGE_TYPE_2D;
        case ETextureType::Texture3D:
            return VK_IMAGE_TYPE_3D;
        default:
            return VK_IMAGE_TYPE_2D;
    }
}

inline VkImageViewType ToVkImageViewType(ETextureType type)
{
    switch (type)
    {
        case ETextureType::Texture1D:        return VK_IMAGE_VIEW_TYPE_1D;
        case ETextureType::Texture2D:        return VK_IMAGE_VIEW_TYPE_2D;
        case ETextureType::Texture3D:        return VK_IMAGE_VIEW_TYPE_3D;
        case ETextureType::TextureCube:      return VK_IMAGE_VIEW_TYPE_CUBE;
        case ETextureType::Texture1DArray:   return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
        case ETextureType::Texture2DArray:   return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        case ETextureType::TextureCubeArray: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        default:                             return VK_IMAGE_VIEW_TYPE_2D;
    }
}

// ============================================================================
// EDescriptorType → VkDescriptorType
// ============================================================================

inline VkDescriptorType ToVkDescriptorType(EDescriptorType type)
{
    switch (type)
    {
        case EDescriptorType::Sampler:                return VK_DESCRIPTOR_TYPE_SAMPLER;
        case EDescriptorType::CombinedImageSampler:   return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        case EDescriptorType::SampledImage:           return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        case EDescriptorType::StorageImage:           return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        case EDescriptorType::UniformTexelBuffer:     return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        case EDescriptorType::StorageTexelBuffer:     return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
        case EDescriptorType::UniformBuffer:          return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case EDescriptorType::StorageBuffer:          return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case EDescriptorType::UniformBufferDynamic:   return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        case EDescriptorType::StorageBufferDynamic:   return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        case EDescriptorType::InputAttachment:        return VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        case EDescriptorType::AccelerationStructure:
            return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        default:
            return VK_DESCRIPTOR_TYPE_SAMPLER;
    }
}

// ============================================================================
// EPipelineBindPoint → VkPipelineBindPoint
// ============================================================================

inline VkPipelineBindPoint ToVkPipelineBindPoint(EPipelineBindPoint point)
{
    switch (point)
    {
        case EPipelineBindPoint::Graphics:   return VK_PIPELINE_BIND_POINT_GRAPHICS;
        case EPipelineBindPoint::Compute:    return VK_PIPELINE_BIND_POINT_COMPUTE;
        case EPipelineBindPoint::RayTracing:
            return VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR;
        default:
            return VK_PIPELINE_BIND_POINT_GRAPHICS;
    }
}

// ============================================================================
// EQueryType → VkQueryType (顺序不同)
// ============================================================================

inline VkQueryType ToVkQueryType(EQueryType type)
{
    switch (type)
    {
        case EQueryType::Occlusion:          return VK_QUERY_TYPE_OCCLUSION;
        case EQueryType::Timestamp:          return VK_QUERY_TYPE_TIMESTAMP;
        case EQueryType::PipelineStatistics: return VK_QUERY_TYPE_PIPELINE_STATISTICS;
        default:                             return VK_QUERY_TYPE_OCCLUSION;
    }
}

// ============================================================================
// EMemoryUsage → VkMemoryPropertyFlags
// ============================================================================

inline VkMemoryPropertyFlags ToVkMemoryPropertyFlags(EMemoryUsage usage)
{
    switch (usage)
    {
        case EMemoryUsage::GpuOnly:
            return VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        case EMemoryUsage::CpuToGpu:
            return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                 | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        case EMemoryUsage::GpuToCpu:
            return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                 | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                 | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        case EMemoryUsage::CpuOnly:
            return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                 | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        default:
            return VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    }
}

// ============================================================================
// EDynamicState → VkDynamicState 数组 — 位掩码展开
// ============================================================================

// 返回实际写入的动态状态数量
inline UInt32 CollectVkDynamicStates(EDynamicState mask,
                                     VkDynamicState* outStates,
                                     UInt32 maxCount)
{
    UInt32 count = 0;
    auto raw = static_cast<UInt32>(mask);

    struct BitMapping
    {
        UInt32         Bit;
        VkDynamicState State;
    };

    static constexpr BitMapping kMappings[] =
    {
        { 1u << 0,  VK_DYNAMIC_STATE_VIEWPORT },
        { 1u << 1,  VK_DYNAMIC_STATE_SCISSOR },
        { 1u << 2,  VK_DYNAMIC_STATE_LINE_WIDTH },
        { 1u << 3,  VK_DYNAMIC_STATE_DEPTH_BIAS },
        { 1u << 4,  VK_DYNAMIC_STATE_BLEND_CONSTANTS },
        { 1u << 5,  VK_DYNAMIC_STATE_DEPTH_BOUNDS },
        { 1u << 6,  VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK },
        { 1u << 7,  VK_DYNAMIC_STATE_STENCIL_WRITE_MASK },
        { 1u << 8,  VK_DYNAMIC_STATE_STENCIL_REFERENCE },
        { 1u << 9,  VK_DYNAMIC_STATE_CULL_MODE },
        { 1u << 10, VK_DYNAMIC_STATE_FRONT_FACE },
        { 1u << 11, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY },
        { 1u << 12, VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE },
        { 1u << 13, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE },
        { 1u << 14, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE },
        { 1u << 15, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP },
        { 1u << 16, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE },
        { 1u << 17, VK_DYNAMIC_STATE_STENCIL_OP },
        { 1u << 18, VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE },
        { 1u << 19, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE },
        { 1u << 20, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE },
    };

    for (const auto& mapping : kMappings)
    {
        if ((raw & mapping.Bit) && count < maxCount)
        {
            outStates[count++] = mapping.State;
        }
    }

    return count;
}

// ============================================================================
// VkImageAspectFlags 辅助 — 根据格式判断 aspect
// ============================================================================

inline VkImageAspectFlags GetVkImageAspectFlags(EPixelFormat format)
{
    if (IsDepthStencilFormat(format))
    {
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    if (IsDepthFormat(format))
    {
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    }
    if (IsStencilFormat(format))
    {
        return VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

} // namespace Limx
