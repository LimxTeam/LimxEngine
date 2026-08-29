// ============================================================
// 文件名称：FVulkanDeviceResources.cpp
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：资源创建遵循"描述符→Vulkan 对象→内存绑定→句柄注册"四步流程，
//          销毁时先从资源池移除再销毁 Vulkan 对象，保证句柄即时失效。
// 功能描述：FVulkanDevice 资源管理实现 — 缓冲区、纹理、纹理视图、
//          采样器、着色器模块的创建与销毁，以及缓冲区映射/取消映射。
// 技术特性：内存分配通过 FindMemoryType 匹配最优内存堆；
//          纹理创建自动推导 VkImageType/VkImageCreateFlags；
//          着色器模块直接接收 SPIR-V 字节码。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                          │ 描述                      │
// │────────────────────────────────│─────────────────────────│
// │ CreateBuffer()                 │ 创建 VkBuffer + 内存绑定   │
// │ DestroyBuffer()                │ 销毁缓冲区                │
// │ MapBuffer()                    │ 映射缓冲区到 CPU          │
// │ UnmapBuffer()                  │ 取消映射                  │
// │ CreateTexture()                │ 创建 VkImage + 内存绑定    │
// │ DestroyTexture()               │ 销毁纹理                  │
// │ CreateTextureView()            │ 创建 VkImageView          │
// │ DestroyTextureView()           │ 销毁纹理视图              │
// │ CreateSampler()                │ 创建 VkSampler            │
// │ DestroySampler()               │ 销毁采样器                │
// │ CreateShader()                 │ 创建 VkShaderModule       │
// │ DestroyShader()                │ 销毁着色器模块             │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-06  │ LimxTeam  │ 初始创建                        │
// ============================================================

#include "Vulkan/FVulkanDevice.h"

namespace Limx
{

// ============================================================================
// 缓冲区
// ============================================================================

ERHIResult FVulkanDevice::CreateBuffer(const FRHIBufferDesc& desc,
                                        FRHIBufferHandle& outHandle)
{
    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size        = desc.Size;
    bufferInfo.usage       = ToVkBufferUsageFlags(desc.Usage);
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer buffer = VK_NULL_HANDLE;
    VkResult vkResult = vkCreateBuffer(m_Device, &bufferInfo, nullptr,
                                        &buffer);
    if (vkResult != VK_SUCCESS)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] vkCreateBuffer 失败: {}", (Int32)vkResult);
        return ERHIResult::ErrorOutOfDeviceMemory;
    }

    // 查询内存需求
    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(m_Device, buffer, &memReqs);

    // 经分配器子分配显存 —— 缓冲区始终是线性资源
    VkMemoryPropertyFlags memProps = ToVkMemoryPropertyFlags(
        desc.MemoryUsage);

    FVulkanAllocation allocation;
    ERHIResult allocResult = m_MemoryAllocator.Allocate(
        memReqs, memProps, ESuballocationType::Linear, allocation);

    if (!IsRHISuccess(allocResult))
    {
        vkDestroyBuffer(m_Device, buffer, nullptr);
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] 缓冲区显存分配失败: {} 字节", (UInt64)memReqs.size);
        return allocResult;
    }

    // 绑定到子分配所在的偏移
    vkResult = vkBindBufferMemory(m_Device, buffer,
                                   allocation.Memory, allocation.Offset);
    if (vkResult != VK_SUCCESS)
    {
        m_MemoryAllocator.Free(allocation);
        vkDestroyBuffer(m_Device, buffer, nullptr);
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] vkBindBufferMemory 失败: {}", (Int32)vkResult);
        return ERHIResult::ErrorUnknown;
    }

    // 主机可见的块在创建时已整块映射, 此处直接取子分配对应的地址;
    // 对同一 VkDeviceMemory 重复调用 vkMapMemory 会违反规范
    void* mappedPtr = allocation.MappedPtr;

    // 注册到资源池
    FVulkanBufferData data;
    data.Buffer    = buffer;
    data.Allocation = allocation;
    data.Size      = desc.Size;
    data.MappedPtr = mappedPtr;

    outHandle = m_Buffers.Allocate(data);

    // 设置调试名称
    if (desc.DebugName != nullptr && m_IsValidationEnabled)
    {
        VkDebugUtilsObjectNameInfoEXT nameInfo = {};
        nameInfo.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
        nameInfo.objectType   = VK_OBJECT_TYPE_BUFFER;
        nameInfo.objectHandle = reinterpret_cast<UInt64>(buffer);
        nameInfo.pObjectName  = desc.DebugName;

        auto setNameFunc =
            reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
                vkGetDeviceProcAddr(m_Device,
                    "vkSetDebugUtilsObjectNameEXT"));
        if (setNameFunc != nullptr)
        {
            setNameFunc(m_Device, &nameInfo);
        }
    }

    return ERHIResult::Success;
}

void FVulkanDevice::DestroyBuffer(FRHIBufferHandle& handle)
{
    FVulkanBufferData* data = m_Buffers.Get(handle);
    if (data == nullptr)
    {
        return;
    }

    // 映射由块统一持有, 单个子分配不做 Unmap
    vkDestroyBuffer(m_Device, data->Buffer, nullptr);
    m_MemoryAllocator.Free(data->Allocation);
    data->MappedPtr = nullptr;

    m_Buffers.Free(handle);
}

ERHIResult FVulkanDevice::MapBuffer(FRHIBufferHandle handle,
                                      void** outMappedPtr)
{
    FVulkanBufferData* data = m_Buffers.Get(handle);
    if (data == nullptr)
    {
        return ERHIResult::ErrorInvalidHandle;
    }

    // 如果已持久映射，直接返回
    if (data->MappedPtr != nullptr)
    {
        *outMappedPtr = data->MappedPtr;
        return ERHIResult::Success;
    }

    // 映射由显存块在创建时统一建立并长期持有。若此处对子分配再次调用
    // vkMapMemory, 会对同一 VkDeviceMemory 产生第二个活跃映射 —— 规范禁止。
    // 因此拿不到地址只有一种可能: 该缓冲区位于非主机可见的内存类型上。
    if (data->Allocation.MappedPtr == nullptr)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] MapBuffer 失败 — 缓冲区位于设备本地内存, 不可主机访问");
        return ERHIResult::ErrorInvalidParameter;
    }

    data->MappedPtr = data->Allocation.MappedPtr;
    *outMappedPtr   = data->MappedPtr;

    return ERHIResult::Success;
}

void FVulkanDevice::UnmapBuffer(FRHIBufferHandle handle)
{
    FVulkanBufferData* data = m_Buffers.Get(handle);
    if (data == nullptr)
    {
        return;
    }

    // 持久映射归块所有, 子分配层面不做 Unmap —— 解除映射会让同块内
    // 其他缓冲区的指针一并失效。这里仅清除本资源的便捷引用。
    data->MappedPtr = nullptr;
}

// ============================================================================
// 纹理
// ============================================================================

ERHIResult FVulkanDevice::CreateTexture(const FRHITextureDesc& desc,
                                          FRHITextureHandle& outHandle)
{
    VkImageCreateInfo imageInfo = {};
    imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType     = ToVkImageType(desc.Type);
    imageInfo.format        = ToVkFormat(desc.Format);
    imageInfo.extent.width  = desc.Extent.Width;
    imageInfo.extent.height = desc.Extent.Height;
    imageInfo.extent.depth  = desc.Extent.Depth;
    imageInfo.mipLevels     = desc.MipLevels;
    imageInfo.arrayLayers   = desc.ArrayLayers;
    imageInfo.samples       = ToVkSampleCountFlagBits(desc.Samples);
    imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage         = ToVkImageUsageFlags(desc.Usage);
    imageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = ToVkImageLayout(desc.InitialLayout);

    // Cube 纹理需要 VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT
    if (desc.Type == ETextureType::TextureCube ||
        desc.Type == ETextureType::TextureCubeArray)
    {
        imageInfo.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }

    VkImage image = VK_NULL_HANDLE;
    VkResult vkResult = vkCreateImage(m_Device, &imageInfo, nullptr,
                                       &image);
    if (vkResult != VK_SUCCESS)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] vkCreateImage 失败: {}", (Int32)vkResult);
        return ERHIResult::ErrorOutOfDeviceMemory;
    }

    // 查询内存需求
    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(m_Device, image, &memReqs);

    // 经分配器子分配显存
    //
    // 平铺模式决定粒度类别: OPTIMAL 图像是非线性资源, 不能与缓冲区等
    // 线性资源共享同一个 bufferImageGranularity 页, 否则行为未定义。
    VkMemoryPropertyFlags memProps = ToVkMemoryPropertyFlags(
        desc.MemoryUsage);

    const ESuballocationType suballocationType =
        (imageInfo.tiling == VK_IMAGE_TILING_LINEAR)
            ? ESuballocationType::Linear
            : ESuballocationType::NonLinear;

    FVulkanAllocation allocation;
    ERHIResult allocResult = m_MemoryAllocator.Allocate(
        memReqs, memProps, suballocationType, allocation);

    if (!IsRHISuccess(allocResult))
    {
        vkDestroyImage(m_Device, image, nullptr);
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] 纹理显存分配失败: {} 字节", (UInt64)memReqs.size);
        return allocResult;
    }

    // 绑定到子分配所在的偏移
    vkResult = vkBindImageMemory(m_Device, image,
                                  allocation.Memory, allocation.Offset);
    if (vkResult != VK_SUCCESS)
    {
        m_MemoryAllocator.Free(allocation);
        vkDestroyImage(m_Device, image, nullptr);
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] vkBindImageMemory 失败: {}", (Int32)vkResult);
        return ERHIResult::ErrorUnknown;
    }

    // 注册到资源池
    FVulkanTextureData data;
    data.Image            = image;
    data.Allocation       = allocation;
    data.Format           = imageInfo.format;
    data.Extent           = imageInfo.extent;
    data.MipLevels        = desc.MipLevels;
    data.ArrayLayers      = desc.ArrayLayers;
    data.RhiFormat        = desc.Format;
    data.IsSwapchainImage = false;

    outHandle = m_Textures.Allocate(data);

    return ERHIResult::Success;
}

void FVulkanDevice::DestroyTexture(FRHITextureHandle& handle)
{
    FVulkanTextureData* data = m_Textures.Get(handle);
    if (data == nullptr)
    {
        return;
    }

    // 交换链图像的内存由呈现引擎拥有, 引擎侧既不分配也不释放
    if (!data->IsSwapchainImage)
    {
        vkDestroyImage(m_Device, data->Image, nullptr);
        m_MemoryAllocator.Free(data->Allocation);
    }

    m_Textures.Free(handle);
}

// ============================================================================
// 纹理视图
// ============================================================================

ERHIResult FVulkanDevice::CreateTextureView(
    const FRHITextureViewDesc& desc,
    FRHITextureViewHandle& outHandle)
{
    const FVulkanTextureData* texData = m_Textures.Get(desc.Texture);
    if (texData == nullptr)
    {
        return ERHIResult::ErrorInvalidHandle;
    }

    VkFormat viewFormat = (desc.Format != EPixelFormat::Unknown)
        ? ToVkFormat(desc.Format)
        : texData->Format;

    EPixelFormat rhiFormat = (desc.Format != EPixelFormat::Unknown)
        ? desc.Format
        : texData->RhiFormat;

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image    = texData->Image;
    viewInfo.viewType = ToVkImageViewType(desc.ViewType);
    viewInfo.format   = viewFormat;

    viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;

    viewInfo.subresourceRange.aspectMask =
        GetVkImageAspectFlags(rhiFormat);
    viewInfo.subresourceRange.baseMipLevel   = desc.BaseMipLevel;
    viewInfo.subresourceRange.levelCount     = desc.MipLevelCount;
    viewInfo.subresourceRange.baseArrayLayer = desc.BaseArrayLayer;
    viewInfo.subresourceRange.layerCount     = desc.ArrayLayerCount;

    VkImageView imageView = VK_NULL_HANDLE;
    VkResult vkResult = vkCreateImageView(m_Device, &viewInfo, nullptr,
                                           &imageView);
    if (vkResult != VK_SUCCESS)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] vkCreateImageView 失败: {}", (Int32)vkResult);
        return ERHIResult::ErrorOutOfDeviceMemory;
    }

    FVulkanTextureViewData data;
    data.ImageView = imageView;

    outHandle = m_TextureViews.Allocate(data);
    return ERHIResult::Success;
}

void FVulkanDevice::DestroyTextureView(FRHITextureViewHandle& handle)
{
    FVulkanTextureViewData* data = m_TextureViews.Get(handle);
    if (data == nullptr)
    {
        return;
    }

    vkDestroyImageView(m_Device, data->ImageView, nullptr);
    m_TextureViews.Free(handle);
}

// ============================================================================
// 采样器
// ============================================================================

ERHIResult FVulkanDevice::CreateSampler(const FRHISamplerDesc& desc,
                                          FRHISamplerHandle& outHandle)
{
    VkSamplerCreateInfo samplerInfo = {};
    samplerInfo.sType            = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter        = ToVkFilter(desc.MagFilter);
    samplerInfo.minFilter        = ToVkFilter(desc.MinFilter);
    samplerInfo.mipmapMode       = ToVkSamplerMipmapMode(desc.MipmapMode);
    samplerInfo.addressModeU     = ToVkSamplerAddressMode(desc.AddressModeU);
    samplerInfo.addressModeV     = ToVkSamplerAddressMode(desc.AddressModeV);
    samplerInfo.addressModeW     = ToVkSamplerAddressMode(desc.AddressModeW);
    samplerInfo.mipLodBias       = desc.MipLodBias;
    samplerInfo.anisotropyEnable = desc.IsAnisotropyEnabled ? VK_TRUE
                                                             : VK_FALSE;
    samplerInfo.maxAnisotropy    = desc.MaxAnisotropy;
    samplerInfo.compareEnable    = desc.IsCompareEnabled ? VK_TRUE
                                                          : VK_FALSE;
    samplerInfo.compareOp        = ToVkCompareOp(desc.CompareOp);
    samplerInfo.minLod           = desc.MinLod;
    samplerInfo.maxLod           = desc.MaxLod;

    // 边框颜色: 根据值选择最接近的预定义颜色
    if (desc.BorderColor[0] >= 0.5f &&
        desc.BorderColor[1] >= 0.5f &&
        desc.BorderColor[2] >= 0.5f)
    {
        samplerInfo.borderColor =
            VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    }
    else if (desc.BorderColor[3] < 0.5f)
    {
        samplerInfo.borderColor =
            VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    }
    else
    {
        samplerInfo.borderColor =
            VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    }

    samplerInfo.unnormalizedCoordinates = VK_FALSE;

    VkSampler sampler = VK_NULL_HANDLE;
    VkResult vkResult = vkCreateSampler(m_Device, &samplerInfo, nullptr,
                                          &sampler);
    if (vkResult != VK_SUCCESS)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] vkCreateSampler 失败: {}", (Int32)vkResult);
        return ERHIResult::ErrorOutOfDeviceMemory;
    }

    FVulkanSamplerData data;
    data.Sampler = sampler;

    outHandle = m_Samplers.Allocate(data);
    return ERHIResult::Success;
}

void FVulkanDevice::DestroySampler(FRHISamplerHandle& handle)
{
    FVulkanSamplerData* data = m_Samplers.Get(handle);
    if (data == nullptr)
    {
        return;
    }

    vkDestroySampler(m_Device, data->Sampler, nullptr);
    m_Samplers.Free(handle);
}

// ============================================================================
// 着色器
// ============================================================================

ERHIResult FVulkanDevice::CreateShader(const FRHIShaderDesc& desc,
                                        FRHIShaderHandle& outHandle)
{
    if (desc.ByteCode == nullptr || desc.ByteCodeSize == 0)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] 着色器字节码为空");
        return ERHIResult::ErrorInvalidParameter;
    }

    // SPIR-V 字节码必须 4 字节对齐
    if (desc.ByteCodeSize % 4 != 0)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] SPIR-V 字节码大小不是 4 的倍数: {}",
            desc.ByteCodeSize);
        return ERHIResult::ErrorShaderCompilation;
    }

    VkShaderModuleCreateInfo createInfo = {};
    createInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = static_cast<SizeType>(desc.ByteCodeSize);
    createInfo.pCode    = reinterpret_cast<const UInt32*>(desc.ByteCode);

    VkShaderModule shaderModule = VK_NULL_HANDLE;
    VkResult vkResult = vkCreateShaderModule(m_Device, &createInfo,
                                               nullptr, &shaderModule);
    if (vkResult != VK_SUCCESS)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] vkCreateShaderModule 失败: {}", (Int32)vkResult);
        return ERHIResult::ErrorShaderCompilation;
    }

    FVulkanShaderData data;
    data.Module = shaderModule;
    data.Stage  = desc.Stage;

    outHandle = m_Shaders.Allocate(data);

    // 设置调试名称
    if (desc.DebugName != nullptr && m_IsValidationEnabled)
    {
        VkDebugUtilsObjectNameInfoEXT nameInfo = {};
        nameInfo.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
        nameInfo.objectType   = VK_OBJECT_TYPE_SHADER_MODULE;
        nameInfo.objectHandle = reinterpret_cast<UInt64>(shaderModule);
        nameInfo.pObjectName  = desc.DebugName;

        auto setNameFunc =
            reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
                vkGetDeviceProcAddr(m_Device,
                    "vkSetDebugUtilsObjectNameEXT"));
        if (setNameFunc != nullptr)
        {
            setNameFunc(m_Device, &nameInfo);
        }
    }

    return ERHIResult::Success;
}

void FVulkanDevice::DestroyShader(FRHIShaderHandle& handle)
{
    FVulkanShaderData* data = m_Shaders.Get(handle);
    if (data == nullptr)
    {
        return;
    }

    vkDestroyShaderModule(m_Device, data->Module, nullptr);
    m_Shaders.Free(handle);
}

// ============================================================================
// 句柄访问辅助 — 缓冲区/纹理/纹理视图
// ============================================================================

VkBuffer FVulkanDevice::GetVkBuffer(FRHIBufferHandle handle) const
{
    const FVulkanBufferData* data = m_Buffers.Get(handle);
    return (data != nullptr) ? data->Buffer : VK_NULL_HANDLE;
}

VkImage FVulkanDevice::GetVkImage(FRHITextureHandle handle) const
{
    const FVulkanTextureData* data = m_Textures.Get(handle);
    return (data != nullptr) ? data->Image : VK_NULL_HANDLE;
}

EPixelFormat FVulkanDevice::GetTextureFormat(
    FRHITextureHandle handle) const
{
    const FVulkanTextureData* data = m_Textures.Get(handle);
    return (data != nullptr) ? data->RhiFormat : EPixelFormat::Unknown;
}

VkImageView FVulkanDevice::GetVkImageView(
    FRHITextureViewHandle handle) const
{
    const FVulkanTextureViewData* data = m_TextureViews.Get(handle);
    return (data != nullptr) ? data->ImageView : VK_NULL_HANDLE;
}

VkSampler FVulkanDevice::GetVkSampler(
    FRHISamplerHandle handle) const
{
    const FVulkanSamplerData* data = m_Samplers.Get(handle);
    return (data != nullptr) ? data->Sampler : VK_NULL_HANDLE;
}

} // namespace Limx
