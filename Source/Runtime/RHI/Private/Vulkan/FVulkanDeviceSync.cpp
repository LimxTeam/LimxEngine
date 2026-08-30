// ============================================================
// 文件名称：FVulkanDeviceSync.cpp
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：同步原语遵循 Vulkan 规范的语义 — 栅栏用于 CPU↔GPU 同步，
//          信号量用于 GPU↔GPU 队列间同步。交换链创建自动查询表面
//          能力并选择最优格式/呈现模式。
// 功能描述：FVulkanDevice 同步与提交实现 — 栅栏、信号量、命令池、
//          命令缓冲区、交换链、查询池的创建/销毁，以及命令提交、
//          帧呈现、设备空闲等待、设备信息查询。
// 技术特性：交换链自动选择 BGRA8_SRGB 格式并回退到可用格式；
//          呈现模式优先选择 Mailbox (三缓冲无撕裂)；
//          交换链图像自动注册为纹理句柄 + 视图句柄。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                          │ 描述                      │
// │────────────────────────────────│─────────────────────────│
// │ CreateFence()                  │ 创建 VkFence              │
// │ DestroyFence()                 │ 销毁栅栏                  │
// │ WaitForFence()                 │ 等待栅栏信号              │
// │ ResetFence()                   │ 重置栅栏                  │
// │ CreateSemaphore()              │ 创建 VkSemaphore          │
// │ DestroySemaphore()             │ 销毁信号量                │
// │ CreateCommandPool()            │ 创建 VkCommandPool        │
// │ DestroyCommandPool()           │ 销毁命令池                │
// │ ResetCommandPool()             │ 重置命令池                │
// │ AllocateCommandBuffer()        │ 分配命令缓冲区             │
// │ FreeCommandBuffer()            │ 释放命令缓冲区             │
// │ CreateSwapchain()              │ 创建交换链+图像+视图       │
// │ DestroySwapchain()             │ 销毁交换链+关联资源        │
// │ AcquireNextImage()             │ 获取下一帧图像索引         │
// │ GetSwapchainImageCount()       │ 查询交换链图像数量         │
// │ GetSwapchainImageView()        │ 获取指定索引的图像视图      │
// │ GetSwapchainFormat()           │ 查询交换链像素格式         │
// │ GetSwapchainExtent()           │ 查询交换链尺寸             │
// │ CreateQueryPool()              │ 创建查询池                │
// │ DestroyQueryPool()             │ 销毁查询池                │
// │ Submit()                       │ 提交命令缓冲区到队列       │
// │ Present()                      │ 呈现交换链图像             │
// │ WaitIdle()                     │ 等待设备空闲              │
// │ GetDeviceName()                │ 设备名称                  │
// │ GetDeviceVendor()              │ 厂商名称                  │
// │ GetDedicatedVideoMemory()      │ 专用显存大小              │
// │ GetMaxTextureSize()            │ 最大纹理尺寸              │
// │ GetMaxPushConstantSize()       │ 最大 PushConstant 大小    │
// │ GetMaxAnisotropy()             │ 最大各向异性度             │
// │ IsRayTracingSupported()        │ 光追支持查询              │
// │ IsMeshShaderSupported()        │ 网格着色器支持查询         │
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
// 栅栏
// ============================================================================

ERHIResult FVulkanDevice::CreateFence(bool isSignaled,
                                       FRHIFenceHandle& outHandle)
{
    VkFenceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (isSignaled)
    {
        createInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    }

    VkFence fence = VK_NULL_HANDLE;
    VkResult vkResult = vkCreateFence(m_Device, &createInfo, nullptr,
                                       &fence);
    if (vkResult != VK_SUCCESS)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] vkCreateFence 失败: {}", (Int32)vkResult);
        return ERHIResult::ErrorOutOfDeviceMemory;
    }

    FVulkanFenceData data;
    data.Fence = fence;
    outHandle = m_Fences.Allocate(data);

    return ERHIResult::Success;
}

void FVulkanDevice::DestroyFence(FRHIFenceHandle& handle)
{
    FVulkanFenceData* data = m_Fences.Get(handle);
    if (data == nullptr)
    {
        return;
    }

    vkDestroyFence(m_Device, data->Fence, nullptr);
    m_Fences.Free(handle);
}

ERHIResult FVulkanDevice::WaitForFence(FRHIFenceHandle handle,
                                        UInt64 timeoutNanoseconds)
{
    const FVulkanFenceData* data = m_Fences.Get(handle);
    if (data == nullptr)
    {
        return ERHIResult::ErrorInvalidHandle;
    }

    VkResult vkResult = vkWaitForFences(m_Device, 1, &data->Fence,
                                         VK_TRUE, timeoutNanoseconds);
    if (vkResult == VK_TIMEOUT)
    {
        return ERHIResult::Timeout;
    }
    if (vkResult != VK_SUCCESS)
    {
        return ERHIResult::ErrorUnknown;
    }

    return ERHIResult::Success;
}

ERHIResult FVulkanDevice::ResetFence(FRHIFenceHandle handle)
{
    const FVulkanFenceData* data = m_Fences.Get(handle);
    if (data == nullptr)
    {
        return ERHIResult::ErrorInvalidHandle;
    }

    VkResult vkResult = vkResetFences(m_Device, 1, &data->Fence);
    if (vkResult != VK_SUCCESS)
    {
        return ERHIResult::ErrorUnknown;
    }

    return ERHIResult::Success;
}

// ============================================================================
// 信号量
// ============================================================================

ERHIResult FVulkanDevice::CreateSemaphore(
    FRHISemaphoreHandle& outHandle)
{
    VkSemaphoreCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkSemaphore semaphore = VK_NULL_HANDLE;
    VkResult vkResult = vkCreateSemaphore(m_Device, &createInfo, nullptr,
                                           &semaphore);
    if (vkResult != VK_SUCCESS)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] vkCreateSemaphore 失败: {}", (Int32)vkResult);
        return ERHIResult::ErrorOutOfDeviceMemory;
    }

    FVulkanSemaphoreData data;
    data.Semaphore = semaphore;
    outHandle = m_Semaphores.Allocate(data);

    return ERHIResult::Success;
}

void FVulkanDevice::DestroySemaphore(FRHISemaphoreHandle& handle)
{
    FVulkanSemaphoreData* data = m_Semaphores.Get(handle);
    if (data == nullptr)
    {
        return;
    }

    vkDestroySemaphore(m_Device, data->Semaphore, nullptr);
    m_Semaphores.Free(handle);
}

// ============================================================================
// 命令池
// ============================================================================

ERHIResult FVulkanDevice::CreateCommandPool(
    EQueueType queueType,
    FRHICommandPoolHandle& outHandle)
{
    VkCommandPoolCreateInfo createInfo = {};
    createInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    createInfo.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    createInfo.queueFamilyIndex = GetQueueFamilyIndex(queueType);

    VkCommandPool pool = VK_NULL_HANDLE;
    VkResult vkResult = vkCreateCommandPool(m_Device, &createInfo,
                                              nullptr, &pool);
    if (vkResult != VK_SUCCESS)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] vkCreateCommandPool 失败: {}", (Int32)vkResult);
        return ERHIResult::ErrorOutOfDeviceMemory;
    }

    FVulkanCommandPoolData data;
    data.Pool      = pool;
    data.QueueType = queueType;
    outHandle = m_CommandPools.Allocate(data);

    return ERHIResult::Success;
}

void FVulkanDevice::DestroyCommandPool(FRHICommandPoolHandle& handle)
{
    FVulkanCommandPoolData* data = m_CommandPools.Get(handle);
    if (data == nullptr)
    {
        return;
    }

    vkDestroyCommandPool(m_Device, data->Pool, nullptr);
    m_CommandPools.Free(handle);
}

ERHIResult FVulkanDevice::ResetCommandPool(FRHICommandPoolHandle handle)
{
    const FVulkanCommandPoolData* data = m_CommandPools.Get(handle);
    if (data == nullptr)
    {
        return ERHIResult::ErrorInvalidHandle;
    }

    VkResult vkResult = vkResetCommandPool(m_Device, data->Pool, 0);
    if (vkResult != VK_SUCCESS)
    {
        return ERHIResult::ErrorUnknown;
    }

    return ERHIResult::Success;
}

// ============================================================================
// 命令缓冲区
// ============================================================================

ERHIResult FVulkanDevice::AllocateCommandBuffer(
    FRHICommandPoolHandle pool,
    FRHICommandBufferHandle& outHandle)
{
    const FVulkanCommandPoolData* poolData = m_CommandPools.Get(pool);
    if (poolData == nullptr)
    {
        return ERHIResult::ErrorInvalidHandle;
    }

    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool        = poolData->Pool;
    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkResult vkResult = vkAllocateCommandBuffers(m_Device, &allocInfo,
                                                   &commandBuffer);
    if (vkResult != VK_SUCCESS)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] vkAllocateCommandBuffers 失败: {}",
            (Int32)vkResult);
        return ERHIResult::ErrorOutOfDeviceMemory;
    }

    FVulkanCommandBufferData data;
    data.CommandBuffer = commandBuffer;
    data.OwnerPool     = pool;
    outHandle = m_CommandBuffers.Allocate(data);

    return ERHIResult::Success;
}

void FVulkanDevice::FreeCommandBuffer(FRHICommandBufferHandle& handle)
{
    FVulkanCommandBufferData* data = m_CommandBuffers.Get(handle);
    if (data == nullptr)
    {
        return;
    }

    const FVulkanCommandPoolData* poolData =
        m_CommandPools.Get(data->OwnerPool);
    if (poolData != nullptr)
    {
        vkFreeCommandBuffers(m_Device, poolData->Pool, 1,
                              &data->CommandBuffer);
    }

    m_CommandBuffers.Free(handle);
}

// ============================================================================
// 交换链
// ============================================================================

ERHIResult FVulkanDevice::CreateSwapchain(
    const FRHISwapchainDesc& desc,
    FRHISwapchainHandle& outHandle)
{
    // 查询表面能力
    VkSurfaceCapabilitiesKHR surfaceCaps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        m_PhysicalDevice, m_Surface, &surfaceCaps);

    // 选择交换链尺寸
    VkExtent2D extent;
    if (surfaceCaps.currentExtent.width != 0xFFFFFFFF)
    {
        extent = surfaceCaps.currentExtent;
    }
    else
    {
        extent.width  = FMath::Clamp(desc.Width,
            surfaceCaps.minImageExtent.width,
            surfaceCaps.maxImageExtent.width);
        extent.height = FMath::Clamp(desc.Height,
            surfaceCaps.minImageExtent.height,
            surfaceCaps.maxImageExtent.height);
    }

    // 选择图像数量
    UInt32 imageCount = desc.BufferCount;
    if (imageCount < surfaceCaps.minImageCount)
    {
        imageCount = surfaceCaps.minImageCount;
    }
    if (surfaceCaps.maxImageCount > 0 &&
        imageCount > surfaceCaps.maxImageCount)
    {
        imageCount = surfaceCaps.maxImageCount;
    }

    // 查询表面格式
    UInt32 formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(
        m_PhysicalDevice, m_Surface, &formatCount, nullptr);

    constexpr UInt32 kMaxFormats = 32;
    VkSurfaceFormatKHR surfaceFormats[kMaxFormats];
    UInt32 formatQuery = formatCount;
    if (formatQuery > kMaxFormats)
    {
        formatQuery = kMaxFormats;
    }
    vkGetPhysicalDeviceSurfaceFormatsKHR(
        m_PhysicalDevice, m_Surface, &formatQuery, surfaceFormats);

    // 选择格式: 优先选择期望格式
    VkSurfaceFormatKHR selectedFormat = surfaceFormats[0];
    VkFormat desiredFormat = ToVkFormat(desc.PreferredFormat);

    for (UInt32 i = 0; i < formatQuery; ++i)
    {
        if (surfaceFormats[i].format == desiredFormat &&
            surfaceFormats[i].colorSpace ==
                VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            selectedFormat = surfaceFormats[i];
            break;
        }
    }

    // 查询呈现模式
    UInt32 presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(
        m_PhysicalDevice, m_Surface, &presentModeCount, nullptr);

    constexpr UInt32 kMaxPresentModes = 8;
    VkPresentModeKHR presentModes[kMaxPresentModes];
    UInt32 pmQuery = presentModeCount;
    if (pmQuery > kMaxPresentModes)
    {
        pmQuery = kMaxPresentModes;
    }
    vkGetPhysicalDeviceSurfacePresentModesKHR(
        m_PhysicalDevice, m_Surface, &pmQuery, presentModes);

    // 选择呈现模式
    VkPresentModeKHR selectedPresentMode = VK_PRESENT_MODE_FIFO_KHR;

    if (!desc.IsVSyncEnabled)
    {
        // 优先 Mailbox (三缓冲无撕裂)
        for (UInt32 i = 0; i < pmQuery; ++i)
        {
            if (presentModes[i] == VK_PRESENT_MODE_MAILBOX_KHR)
            {
                selectedPresentMode = VK_PRESENT_MODE_MAILBOX_KHR;
                break;
            }
        }

        // 回退到 Immediate
        if (selectedPresentMode == VK_PRESENT_MODE_FIFO_KHR)
        {
            for (UInt32 i = 0; i < pmQuery; ++i)
            {
                if (presentModes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR)
                {
                    selectedPresentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
                    break;
                }
            }
        }
    }

    // 创建交换链
    VkSwapchainCreateInfoKHR createInfo = {};
    createInfo.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface          = m_Surface;
    createInfo.minImageCount    = imageCount;
    createInfo.imageFormat      = selectedFormat.format;
    createInfo.imageColorSpace  = selectedFormat.colorSpace;
    createInfo.imageExtent      = extent;
    createInfo.imageArrayLayers = 1;
    // TRANSFER_SRC 用于截屏与画面回读 —— 没有它就无法把最终画面拷出显存,
    // 而"最终画面到底长什么样"是渲染改动唯一的客观验收依据。
    // 这三个用途在所有实现上都是交换链的必备能力, 不需要查询支持位。
    createInfo.imageUsage       =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    // 队列共享模式
    UInt32 queueFamilyIndices[2] =
    {
        m_GraphicsQueueFamily,
        m_PresentQueueFamily
    };

    if (m_GraphicsQueueFamily != m_PresentQueueFamily)
    {
        createInfo.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices   = queueFamilyIndices;
    }
    else
    {
        createInfo.imageSharingMode      = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform   = surfaceCaps.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode    = selectedPresentMode;
    createInfo.clipped        = VK_TRUE;
    createInfo.oldSwapchain   = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkResult vkResult = vkCreateSwapchainKHR(m_Device, &createInfo,
                                               nullptr, &swapchain);
    if (vkResult != VK_SUCCESS)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] vkCreateSwapchainKHR 失败: {}", (Int32)vkResult);
        return ERHIResult::ErrorUnknown;
    }

    // 获取交换链图像
    UInt32 swapImageCount = 0;
    vkGetSwapchainImagesKHR(m_Device, swapchain, &swapImageCount,
                             nullptr);

    FVulkanSwapchainData swapData;
    swapData.Swapchain  = swapchain;
    swapData.Format     = selectedFormat.format;
    swapData.Extent     = extent;
    swapData.ImageCount = swapImageCount;
    swapData.RhiFormat  = ToEPixelFormat(selectedFormat.format);

    // 获取 VkImage 数组
    constexpr UInt32 kMaxSwapImages = 8;
    VkImage swapImages[kMaxSwapImages];
    UInt32 imgQuery = swapImageCount;
    if (imgQuery > kMaxSwapImages)
    {
        imgQuery = kMaxSwapImages;
    }
    vkGetSwapchainImagesKHR(m_Device, swapchain, &imgQuery, swapImages);

    // 为每个交换链图像创建纹理句柄和视图
    for (UInt32 i = 0; i < imgQuery; ++i)
    {
        // 注册为纹理 (标记为交换链图像，不由我们销毁)
        FVulkanTextureData texData;
        texData.Image            = swapImages[i];
        // 交换链图像的显存归呈现引擎所有 — 保持无效分配句柄,
        // 使 DestroyTexture 不会误将其交给分配器回收
        texData.Allocation       = FVulkanAllocation();
        texData.Format           = selectedFormat.format;
        texData.Extent           = { extent.width, extent.height, 1 };
        texData.MipLevels        = 1;
        texData.ArrayLayers      = 1;
        texData.RhiFormat        = swapData.RhiFormat;
        texData.IsSwapchainImage = true;

        FRHITextureHandle texHandle = m_Textures.Allocate(texData);
        swapData.ImageTextures.Add(texHandle);
        swapData.Images.Add(swapImages[i]);

        // 创建图像视图
        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image    = swapImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format   = selectedFormat.format;

        viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;

        viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel   = 0;
        viewInfo.subresourceRange.levelCount     = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount     = 1;

        VkImageView imageView = VK_NULL_HANDLE;
        VK_CHECK(vkCreateImageView(m_Device, &viewInfo, nullptr,
                                    &imageView));

        FVulkanTextureViewData viewData;
        viewData.ImageView = imageView;

        FRHITextureViewHandle viewHandle =
            m_TextureViews.Allocate(viewData);
        swapData.ImageViews.Add(viewHandle);
    }

    outHandle = m_Swapchains.Allocate(swapData);

    LIMX_LOG(LogRHI, Log,
        "[Vulkan] 交换链创建完成: {}x{} 格式:{} 图像数:{}",
        extent.width, extent.height,
        static_cast<Int32>(selectedFormat.format), swapImageCount);

    return ERHIResult::Success;
}

void FVulkanDevice::DestroySwapchain(FRHISwapchainHandle& handle)
{
    FVulkanSwapchainData* data = m_Swapchains.Get(handle);
    if (data == nullptr)
    {
        return;
    }

    // 先销毁图像视图
    for (SizeType i = 0; i < data->ImageViews.GetSize(); ++i)
    {
        FRHITextureViewHandle viewHandle = data->ImageViews[i];
        FVulkanTextureViewData* viewData = m_TextureViews.Get(viewHandle);
        if (viewData != nullptr)
        {
            vkDestroyImageView(m_Device, viewData->ImageView, nullptr);
            m_TextureViews.Free(viewHandle);
        }
    }

    // 释放纹理句柄 (不销毁 VkImage，交换链管理)
    for (SizeType i = 0; i < data->ImageTextures.GetSize(); ++i)
    {
        FRHITextureHandle texHandle = data->ImageTextures[i];
        m_Textures.Free(texHandle);
    }

    // 销毁交换链
    vkDestroySwapchainKHR(m_Device, data->Swapchain, nullptr);
    m_Swapchains.Free(handle);
}

ERHIResult FVulkanDevice::AcquireNextImage(
    FRHISwapchainHandle swapchain,
    FRHISemaphoreHandle signalSemaphore,
    FRHIFenceHandle signalFence,
    UInt32& outImageIndex)
{
    const FVulkanSwapchainData* swapData = m_Swapchains.Get(swapchain);
    if (swapData == nullptr)
    {
        return ERHIResult::ErrorInvalidHandle;
    }

    VkSemaphore vkSemaphore = GetVkSemaphore(signalSemaphore);
    VkFence vkFence = GetVkFence(signalFence);

    VkResult vkResult = vkAcquireNextImageKHR(
        m_Device, swapData->Swapchain, 0xFFFFFFFFFFFFFFFFULL,
        vkSemaphore, vkFence, &outImageIndex);

    if (vkResult == VK_ERROR_OUT_OF_DATE_KHR)
    {
        return ERHIResult::ErrorOutOfDate;
    }
    if (vkResult == VK_SUBOPTIMAL_KHR)
    {
        return ERHIResult::SuboptimalSwapchain;
    }
    if (vkResult != VK_SUCCESS)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] vkAcquireNextImageKHR 失败: {}", (Int32)vkResult);
        return ERHIResult::ErrorUnknown;
    }

    return ERHIResult::Success;
}

// ============================================================================
// GetDeviceMemoryStats — 把分配器的统计转成 RHI 口径
// ============================================================================

FRHIDeviceMemoryStats FVulkanDevice::GetDeviceMemoryStats() const
{
    const FVulkanMemoryStats source = m_MemoryAllocator.GetStats();

    FRHIDeviceMemoryStats stats = {};
    stats.AllocationCount = source.DeviceAllocationCount;
    stats.AllocationLimit = source.DeviceAllocationLimit;
    stats.ReservedBytes   = static_cast<UInt64>(source.TotalReservedBytes);
    stats.UsedBytes       = static_cast<UInt64>(source.TotalUsedBytes);

    return stats;
}

UInt32 FVulkanDevice::GetSwapchainImageCount(
    FRHISwapchainHandle swapchain)
{
    const FVulkanSwapchainData* data = m_Swapchains.Get(swapchain);
    return (data != nullptr) ? data->ImageCount : 0;
}

FRHITextureHandle FVulkanDevice::GetSwapchainImage(
    FRHISwapchainHandle swapchain, UInt32 imageIndex)
{
    const FVulkanSwapchainData* data = m_Swapchains.Get(swapchain);
    if (data == nullptr || imageIndex >= data->ImageCount)
    {
        return FRHITextureHandle();
    }

    return data->ImageTextures[imageIndex];
}

FRHITextureViewHandle FVulkanDevice::GetSwapchainImageView(
    FRHISwapchainHandle swapchain, UInt32 imageIndex)
{
    const FVulkanSwapchainData* data = m_Swapchains.Get(swapchain);
    if (data == nullptr || imageIndex >= data->ImageCount)
    {
        return FRHITextureViewHandle();
    }

    return data->ImageViews[imageIndex];
}

EPixelFormat FVulkanDevice::GetSwapchainFormat(
    FRHISwapchainHandle swapchain)
{
    const FVulkanSwapchainData* data = m_Swapchains.Get(swapchain);
    return (data != nullptr) ? data->RhiFormat : EPixelFormat::Unknown;
}

FRHIExtent2D FVulkanDevice::GetSwapchainExtent(
    FRHISwapchainHandle swapchain)
{
    const FVulkanSwapchainData* data = m_Swapchains.Get(swapchain);
    if (data != nullptr)
    {
        return { data->Extent.width, data->Extent.height };
    }
    return { 0, 0 };
}

// ============================================================================
// 查询池
// ============================================================================

ERHIResult FVulkanDevice::CreateQueryPool(
    const FRHIQueryPoolDesc& desc,
    FRHIQueryPoolHandle& outHandle)
{
    VkQueryPoolCreateInfo createInfo = {};
    createInfo.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    createInfo.queryType  = ToVkQueryType(desc.Type);
    createInfo.queryCount = desc.QueryCount;

    if (desc.Type == EQueryType::PipelineStatistics)
    {
        createInfo.pipelineStatistics =
            VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT
          | VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT
          | VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT
          | VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
    }

    VkQueryPool pool = VK_NULL_HANDLE;
    VkResult vkResult = vkCreateQueryPool(m_Device, &createInfo, nullptr,
                                           &pool);
    if (vkResult != VK_SUCCESS)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] vkCreateQueryPool 失败: {}", (Int32)vkResult);
        return ERHIResult::ErrorOutOfDeviceMemory;
    }

    FVulkanQueryPoolData data;
    data.Pool  = pool;
    data.Type  = desc.Type;
    data.Count = desc.QueryCount;
    outHandle = m_QueryPools.Allocate(data);

    return ERHIResult::Success;
}

void FVulkanDevice::DestroyQueryPool(FRHIQueryPoolHandle& handle)
{
    FVulkanQueryPoolData* data = m_QueryPools.Get(handle);
    if (data == nullptr)
    {
        return;
    }

    vkDestroyQueryPool(m_Device, data->Pool, nullptr);
    m_QueryPools.Free(handle);
}

ERHIResult FVulkanDevice::GetQueryResults(FRHIQueryPoolHandle handle,
                                           UInt32 firstQuery,
                                           UInt32 queryCount,
                                           UInt64* outResults,
                                           bool wait)
{
    FVulkanQueryPoolData* data = m_QueryPools.Get(handle);

    if (data == nullptr || outResults == nullptr || queryCount == 0)
    {
        return ERHIResult::ErrorInvalidParameter;
    }

    if (firstQuery + queryCount > data->Count)
    {
        return ERHIResult::ErrorInvalidParameter;
    }

    // WITH_AVAILABILITY 不用 —— 它会在每个结果后多塞一个可用位, 使步长
    // 翻倍。这里靠返回值区分就绪与否: 全部就绪返回 VK_SUCCESS, 否则
    // VK_NOT_READY, 而未就绪的槽位保持原值不被写入。
    VkQueryResultFlags flags = VK_QUERY_RESULT_64_BIT;

    if (wait)
    {
        flags |= VK_QUERY_RESULT_WAIT_BIT;
    }

    const VkResult vkResult = vkGetQueryPoolResults(
        m_Device,
        data->Pool,
        firstQuery,
        queryCount,
        static_cast<SizeType>(queryCount) * sizeof(UInt64),
        outResults,
        sizeof(UInt64),
        flags);

    if (vkResult == VK_NOT_READY)
    {
        return ERHIResult::NotReady;
    }

    if (vkResult != VK_SUCCESS)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] vkGetQueryPoolResults 失败: {}", (Int32)vkResult);
        return ERHIResult::ErrorUnknown;
    }

    return ERHIResult::Success;
}

Float32 FVulkanDevice::GetTimestampPeriod() const
{
    return m_DeviceProperties.limits.timestampPeriod;
}

UInt32 FVulkanDevice::GetTimestampValidBits() const
{
    return m_TimestampValidBits;
}

// ============================================================================
// 命令提交与呈现
// ============================================================================

ERHIResult FVulkanDevice::Submit(EQueueType queue,
                                  const FRHISubmitInfo& submitInfo,
                                  FRHIFenceHandle signalFence)
{
    // 转换等待信号量
    constexpr UInt32 kMaxSemaphores = 8;
    constexpr UInt32 kMaxCmdBuffers = 16;

    VkSemaphore waitSemaphores[kMaxSemaphores];
    VkPipelineStageFlags waitStages[kMaxSemaphores];
    UInt32 waitCount = submitInfo.WaitSemaphoreCount;
    if (waitCount > kMaxSemaphores)
    {
        waitCount = kMaxSemaphores;
    }

    for (UInt32 i = 0; i < waitCount; ++i)
    {
        waitSemaphores[i] = GetVkSemaphore(
            submitInfo.WaitSemaphores[i]);
        waitStages[i] = ToVkPipelineStageFlags(
            submitInfo.WaitStages[i]);
    }

    // 转换命令缓冲区
    VkCommandBuffer commandBuffers[kMaxCmdBuffers];
    UInt32 cmdCount = submitInfo.CommandBufferCount;
    if (cmdCount > kMaxCmdBuffers)
    {
        cmdCount = kMaxCmdBuffers;
    }

    for (UInt32 i = 0; i < cmdCount; ++i)
    {
        commandBuffers[i] = GetVkCommandBuffer(
            submitInfo.CommandBuffers[i]);
    }

    // 转换信号信号量
    VkSemaphore signalSemaphores[kMaxSemaphores];
    UInt32 signalCount = submitInfo.SignalSemaphoreCount;
    if (signalCount > kMaxSemaphores)
    {
        signalCount = kMaxSemaphores;
    }

    for (UInt32 i = 0; i < signalCount; ++i)
    {
        signalSemaphores[i] = GetVkSemaphore(
            submitInfo.SignalSemaphores[i]);
    }

    VkSubmitInfo vkSubmitInfo = {};
    vkSubmitInfo.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    vkSubmitInfo.waitSemaphoreCount   = waitCount;
    vkSubmitInfo.pWaitSemaphores      = waitSemaphores;
    vkSubmitInfo.pWaitDstStageMask    = waitStages;
    vkSubmitInfo.commandBufferCount   = cmdCount;
    vkSubmitInfo.pCommandBuffers      = commandBuffers;
    vkSubmitInfo.signalSemaphoreCount = signalCount;
    vkSubmitInfo.pSignalSemaphores    = signalSemaphores;

    VkFence fence = GetVkFence(signalFence);

    // 空提交 (无命令/信号量) 且仅需 signal fence:
    // 使用 submitCount=0 确保 fence 在所有先前队列操作完成后才被 signal
    // (包括 vkQueuePresentKHR 等非 Submit 类队列操作)
    bool isEmptySubmit = (waitCount == 0 && cmdCount == 0 && signalCount == 0);

    VkResult vkResult = isEmptySubmit
        ? vkQueueSubmit(GetVkQueue(queue), 0, nullptr, fence)
        : vkQueueSubmit(GetVkQueue(queue), 1, &vkSubmitInfo, fence);

    if (vkResult != VK_SUCCESS)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] vkQueueSubmit 失败: {}", (Int32)vkResult);
        return ERHIResult::ErrorUnknown;
    }

    return ERHIResult::Success;
}

ERHIResult FVulkanDevice::Present(const FRHIPresentInfo& presentInfo)
{
    const FVulkanSwapchainData* swapData =
        m_Swapchains.Get(presentInfo.Swapchain);
    if (swapData == nullptr)
    {
        return ERHIResult::ErrorInvalidHandle;
    }

    // 转换等待信号量
    constexpr UInt32 kMaxSemaphores = 8;
    VkSemaphore waitSemaphores[kMaxSemaphores];
    UInt32 waitCount = presentInfo.WaitSemaphoreCount;
    if (waitCount > kMaxSemaphores)
    {
        waitCount = kMaxSemaphores;
    }

    for (UInt32 i = 0; i < waitCount; ++i)
    {
        waitSemaphores[i] = GetVkSemaphore(
            presentInfo.WaitSemaphores[i]);
    }

    UInt32 imageIndex = presentInfo.ImageIndex;

    VkPresentInfoKHR vkPresentInfo = {};
    vkPresentInfo.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    vkPresentInfo.waitSemaphoreCount = waitCount;
    vkPresentInfo.pWaitSemaphores    = waitSemaphores;
    vkPresentInfo.swapchainCount     = 1;
    vkPresentInfo.pSwapchains        = &swapData->Swapchain;
    vkPresentInfo.pImageIndices      = &imageIndex;
    vkPresentInfo.pResults           = nullptr;

    VkResult vkResult = vkQueuePresentKHR(m_PresentQueue,
                                            &vkPresentInfo);
    if (vkResult == VK_ERROR_OUT_OF_DATE_KHR ||
        vkResult == VK_SUBOPTIMAL_KHR)
    {
        return ERHIResult::ErrorOutOfDate;
    }
    if (vkResult != VK_SUCCESS)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] vkQueuePresentKHR 失败: {}", (Int32)vkResult);
        return ERHIResult::ErrorUnknown;
    }

    return ERHIResult::Success;
}

ERHIResult FVulkanDevice::WaitIdle()
{
    if (m_Device == VK_NULL_HANDLE)
    {
        return ERHIResult::Success;
    }

    VkResult vkResult = vkDeviceWaitIdle(m_Device);
    if (vkResult != VK_SUCCESS)
    {
        return ERHIResult::ErrorDeviceLost;
    }

    return ERHIResult::Success;
}

// ============================================================================
// 设备信息查询
// ============================================================================

const char* FVulkanDevice::GetDeviceName() const
{
    return m_DeviceProperties.deviceName;
}

const char* FVulkanDevice::GetDeviceVendor() const
{
    return m_VendorName;
}

UInt64 FVulkanDevice::GetDedicatedVideoMemory() const
{
    UInt64 totalDeviceLocal = 0;
    for (UInt32 i = 0; i < m_MemoryProperties.memoryHeapCount; ++i)
    {
        if (m_MemoryProperties.memoryHeaps[i].flags &
            VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
        {
            totalDeviceLocal +=
                m_MemoryProperties.memoryHeaps[i].size;
        }
    }
    return totalDeviceLocal;
}

UInt32 FVulkanDevice::GetMaxTextureSize() const
{
    return m_DeviceProperties.limits.maxImageDimension2D;
}

UInt32 FVulkanDevice::GetMaxPushConstantSize() const
{
    return m_DeviceProperties.limits.maxPushConstantsSize;
}

Float32 FVulkanDevice::GetMaxAnisotropy() const
{
    return m_DeviceProperties.limits.maxSamplerAnisotropy;
}

EFormatFeature FVulkanDevice::GetFormatFeatures(EPixelFormat format) const
{
    VkFormatProperties properties = {};
    vkGetPhysicalDeviceFormatProperties(m_PhysicalDevice, ToVkFormat(format),
                                        &properties);

    // 只关心最优平铺 —— 引擎创建的纹理一律用 VK_IMAGE_TILING_OPTIMAL,
    // 线性平铺的能力位与它无关。
    const VkFormatFeatureFlags flags = properties.optimalTilingFeatures;

    EFormatFeature result = EFormatFeature::None;

    if (flags & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
    {
        result = result | EFormatFeature::SampledImage;
    }

    if (flags & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
    {
        result = result | EFormatFeature::SampledImageLinear;
    }

    if (flags & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)
    {
        result = result | EFormatFeature::StorageImage;
    }

    if (flags & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)
    {
        result = result | EFormatFeature::ColorAttachment;
    }

    if (flags & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)
    {
        result = result | EFormatFeature::ColorAttachmentBlend;
    }

    if (flags & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
    {
        result = result | EFormatFeature::DepthStencilAttachment;
    }

    if (flags & VK_FORMAT_FEATURE_BLIT_SRC_BIT)
    {
        result = result | EFormatFeature::BlitSrc;
    }

    if (flags & VK_FORMAT_FEATURE_BLIT_DST_BIT)
    {
        result = result | EFormatFeature::BlitDst;
    }

    return result;
}

bool FVulkanDevice::IsRayTracingSupported() const
{
    // 通过检查扩展列表判断 (简化实现)
    UInt32 extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(
        m_PhysicalDevice, nullptr, &extensionCount, nullptr);

    if (extensionCount == 0)
    {
        return false;
    }

    constexpr UInt32 kMaxExtensions = 512;
    VkExtensionProperties extensions[kMaxExtensions];
    UInt32 extQuery = extensionCount;
    if (extQuery > kMaxExtensions)
    {
        extQuery = kMaxExtensions;
    }
    vkEnumerateDeviceExtensionProperties(
        m_PhysicalDevice, nullptr, &extQuery, extensions);

    const char* target =
        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME;

    for (UInt32 i = 0; i < extQuery; ++i)
    {
        const char* ext = extensions[i].extensionName;
        bool isMatch = true;
        for (Int32 c = 0; target[c] != '\0'; ++c)
        {
            if (ext[c] != target[c])
            {
                isMatch = false;
                break;
            }
        }
        if (isMatch)
        {
            return true;
        }
    }

    return false;
}

bool FVulkanDevice::IsMeshShaderSupported() const
{
    UInt32 extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(
        m_PhysicalDevice, nullptr, &extensionCount, nullptr);

    if (extensionCount == 0)
    {
        return false;
    }

    constexpr UInt32 kMaxExtensions = 512;
    VkExtensionProperties extensions[kMaxExtensions];
    UInt32 extQuery = extensionCount;
    if (extQuery > kMaxExtensions)
    {
        extQuery = kMaxExtensions;
    }
    vkEnumerateDeviceExtensionProperties(
        m_PhysicalDevice, nullptr, &extQuery, extensions);

    const char* target = VK_EXT_MESH_SHADER_EXTENSION_NAME;

    for (UInt32 i = 0; i < extQuery; ++i)
    {
        const char* ext = extensions[i].extensionName;
        bool isMatch = true;
        for (Int32 c = 0; target[c] != '\0'; ++c)
        {
            if (ext[c] != target[c])
            {
                isMatch = false;
                break;
            }
        }
        if (isMatch)
        {
            return true;
        }
    }

    return false;
}

// ============================================================================
// 句柄访问辅助
// ============================================================================

VkCommandBuffer FVulkanDevice::GetVkCommandBuffer(
    FRHICommandBufferHandle handle) const
{
    const FVulkanCommandBufferData* data = m_CommandBuffers.Get(handle);
    return (data != nullptr) ? data->CommandBuffer : VK_NULL_HANDLE;
}

VkFence FVulkanDevice::GetVkFence(FRHIFenceHandle handle) const
{
    const FVulkanFenceData* data = m_Fences.Get(handle);
    return (data != nullptr) ? data->Fence : VK_NULL_HANDLE;
}

VkSemaphore FVulkanDevice::GetVkSemaphore(
    FRHISemaphoreHandle handle) const
{
    const FVulkanSemaphoreData* data = m_Semaphores.Get(handle);
    return (data != nullptr) ? data->Semaphore : VK_NULL_HANDLE;
}

VkQueryPool FVulkanDevice::GetVkQueryPool(
    FRHIQueryPoolHandle handle) const
{
    const FVulkanQueryPoolData* data = m_QueryPools.Get(handle);
    return (data != nullptr) ? data->Pool : VK_NULL_HANDLE;
}

} // namespace Limx
