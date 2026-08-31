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
    ECommandBufferLevel level,
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
    allocInfo.level              =
        (level == ECommandBufferLevel::Secondary)
            ? VK_COMMAND_BUFFER_LEVEL_SECONDARY
            : VK_COMMAND_BUFFER_LEVEL_PRIMARY;
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
    data.Level         = level;
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

    // 按驱动报的数量取全 — 截断会让期望格式落在看不见的尾巴里, 于是悄悄
    // 退回 surfaceFormats[0], 画面色彩空间错了却没有任何提示。
    if (formatCount == 0)
    {
        LIMX_LOG(LogRHI, Error, "[Vulkan] 表面未报告任何可用格式");
        return ERHIResult::ErrorSurfaceLost;
    }

    constexpr SizeType kInlineFormats = 32;
    TSmallVector<VkSurfaceFormatKHR, kInlineFormats> surfaceFormats;
    ResizeZeroed(surfaceFormats, static_cast<SizeType>(formatCount));

    UInt32 formatQuery = formatCount;
    vkGetPhysicalDeviceSurfaceFormatsKHR(
        m_PhysicalDevice, m_Surface, &formatQuery, surfaceFormats.GetData());

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

    // 取全 — 截断会让 Mailbox 落在看不见的尾巴里, 于是关掉垂直同步也悄悄
    // 退回 FIFO, 表现为"这台机器就是延迟高", 没有任何提示。
    constexpr SizeType kInlinePresentModes = 8;
    TSmallVector<VkPresentModeKHR, kInlinePresentModes> presentModes;
    ResizeZeroed(presentModes, static_cast<SizeType>(presentModeCount));

    UInt32 pmQuery = presentModeCount;
    vkGetPhysicalDeviceSurfacePresentModesKHR(
        m_PhysicalDevice, m_Surface, &pmQuery, presentModes.GetData());

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

    // 获取 VkImage 数组 — 数量必须与上面记下的 ImageCount 一致
    //
    // 这一处的截断比别处更隐蔽: swapData.ImageCount 记的是驱动报的**全部**
    // 数量, 而截断后只有前几张图像拿到了纹理句柄。于是
    // GetSwapchainImageCount() 说有 N 张, GetSwapchainImage(N-1) 却拿到无效
    // 句柄 —— 帧循环按前者建资源、按后者取图像, 崩在离这里很远的地方。
    constexpr SizeType kInlineSwapImages = 8;
    TSmallVector<VkImage, kInlineSwapImages> swapImages;
    ResizeZeroed(swapImages, static_cast<SizeType>(swapImageCount));

    UInt32 imgQuery = swapImageCount;
    vkGetSwapchainImagesKHR(m_Device, swapchain, &imgQuery,
                             swapImages.GetData());

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
    // ------------------------------------------------------------------
    // 数量不设上限 — 内联容量 + 分配器回退
    //
    // 这里**不能**像管线屏障那样分批下发, 原因是提交的语义与屏障不同:
    // VkSubmitInfo 的等待/信号信号量只作用于它自己那一批。
    //
    //   等待信号量若只挂在第 0 批, 就只挡住第 0 批 —— 后面几批的命令不受
    //     它约束, 可以在等待条件还没满足时就开跑。也不能在每一批上都挂同
    //     一个等待: 二值信号量的等待会把它置回未触发态, 一次触发只能配一
    //     次等待, 重复等待是非法的。
    //
    //   信号信号量若只挂在最后一批, 就只表示最后一批完成。批次之间没有隐
    //     式的执行顺序, 前面几批可能还在跑, 等待方却已经被放行 —— 这正是
    //     "看起来跑通了, 数据却是半成品"的来源。
    //
    // 要让分批与单批等价, 就得额外造信号量把各批串起来, 等于为"看起来优
    // 雅"引进一套新的生命周期管理和一批新的失败模式。正确答案是保持单个
    // VkSubmitInfo, 数组超出内联容量时退到分配器。
    //
    // 内联容量取原先的硬上限, 因此常见路径一次堆分配都没有 —— 只有过去
    // 会被静默丢弃的那些情况才会碰到分配器。
    // ------------------------------------------------------------------

    constexpr SizeType kInlineSemaphores = 8;
    constexpr SizeType kInlineCmdBuffers = 16;

    TSmallVector<VkSemaphore, kInlineSemaphores>          waitSemaphores;
    TSmallVector<VkPipelineStageFlags, kInlineSemaphores> waitStages;
    TSmallVector<VkCommandBuffer, kInlineCmdBuffers>      commandBuffers;
    TSmallVector<VkSemaphore, kInlineSemaphores>          signalSemaphores;

    // 数组为空却给了非零计数是调用方的错误, 必须出声 —— 静默当作 0 处理
    // 正是"这批活没提交, 一切看起来却正常"的那条路径。
    UInt32 waitCount = submitInfo.WaitSemaphoreCount;
    if (waitCount > 0
        && (submitInfo.WaitSemaphores == nullptr
            || submitInfo.WaitStages == nullptr))
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] Submit: 等待信号量数组或阶段数组为空但计数为 {}",
            waitCount);
        waitCount = 0;
    }

    waitSemaphores.Reserve(static_cast<SizeType>(waitCount));
    waitStages.Reserve(static_cast<SizeType>(waitCount));

    for (UInt32 i = 0; i < waitCount; ++i)
    {
        waitSemaphores.Add(GetVkSemaphore(submitInfo.WaitSemaphores[i]));
        waitStages.Add(ToVkPipelineStageFlags(submitInfo.WaitStages[i]));
    }

    UInt32 cmdCount = submitInfo.CommandBufferCount;
    if (cmdCount > 0 && submitInfo.CommandBuffers == nullptr)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] Submit: 命令缓冲区数组为空但计数为 {}", cmdCount);
        cmdCount = 0;
    }

    commandBuffers.Reserve(static_cast<SizeType>(cmdCount));

    for (UInt32 i = 0; i < cmdCount; ++i)
    {
        const VkCommandBuffer handle =
            GetVkCommandBuffer(submitInfo.CommandBuffers[i]);

        if (handle == VK_NULL_HANDLE)
        {
            // 无效句柄意味着这一整批录好的命令不会执行。默默跳过等于凭空
            // 少做一批活, 而 vkQueueSubmit 照样返回 VK_SUCCESS。
            LIMX_LOG(LogRHI, Error,
                "[Vulkan] Submit: 第 {} 个命令缓冲区句柄无效, "
                "该批命令不会执行", i);
            continue;
        }

        commandBuffers.Add(handle);
    }

    UInt32 signalCount = submitInfo.SignalSemaphoreCount;
    if (signalCount > 0 && submitInfo.SignalSemaphores == nullptr)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] Submit: 信号信号量数组为空但计数为 {}", signalCount);
        signalCount = 0;
    }

    signalSemaphores.Reserve(static_cast<SizeType>(signalCount));

    for (UInt32 i = 0; i < signalCount; ++i)
    {
        signalSemaphores.Add(
            GetVkSemaphore(submitInfo.SignalSemaphores[i]));
    }

    const UInt32 finalWaitCount =
        static_cast<UInt32>(waitSemaphores.GetSize());
    const UInt32 finalCmdCount =
        static_cast<UInt32>(commandBuffers.GetSize());
    const UInt32 finalSignalCount =
        static_cast<UInt32>(signalSemaphores.GetSize());

    VkSubmitInfo vkSubmitInfo = {};
    vkSubmitInfo.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    vkSubmitInfo.waitSemaphoreCount   = finalWaitCount;
    vkSubmitInfo.pWaitSemaphores      = waitSemaphores.GetData();
    vkSubmitInfo.pWaitDstStageMask    = waitStages.GetData();
    vkSubmitInfo.commandBufferCount   = finalCmdCount;
    vkSubmitInfo.pCommandBuffers      = commandBuffers.GetData();
    vkSubmitInfo.signalSemaphoreCount = finalSignalCount;
    vkSubmitInfo.pSignalSemaphores    = signalSemaphores.GetData();

    VkFence fence = GetVkFence(signalFence);

    // 空提交 (无命令/信号量) 且仅需 signal fence:
    // 使用 submitCount=0 确保 fence 在所有先前队列操作完成后才被 signal
    // (包括 vkQueuePresentKHR 等非 Submit 类队列操作)
    bool isEmptySubmit = (finalWaitCount == 0 && finalCmdCount == 0
                          && finalSignalCount == 0);

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

    // 转换等待信号量 — 数量不设上限
    //
    // VkPresentInfoKHR 只有一个等待数组, 连"分批"这个选项都不存在。丢掉
    // 一个等待信号量的后果是: 渲染还没画完就把图像交给呈现引擎, 屏幕上
    // 出现半张旧帧半张新帧, 而 vkQueuePresentKHR 返回 VK_SUCCESS。
    constexpr SizeType kInlineSemaphores = 8;
    TSmallVector<VkSemaphore, kInlineSemaphores> waitSemaphores;

    UInt32 waitCount = presentInfo.WaitSemaphoreCount;
    if (waitCount > 0 && presentInfo.WaitSemaphores == nullptr)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] Present: 等待信号量数组为空但计数为 {}", waitCount);
        waitCount = 0;
    }

    waitSemaphores.Reserve(static_cast<SizeType>(waitCount));

    for (UInt32 i = 0; i < waitCount; ++i)
    {
        waitSemaphores.Add(GetVkSemaphore(presentInfo.WaitSemaphores[i]));
    }

    UInt32 imageIndex = presentInfo.ImageIndex;

    VkPresentInfoKHR vkPresentInfo = {};
    vkPresentInfo.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    vkPresentInfo.waitSemaphoreCount =
        static_cast<UInt32>(waitSemaphores.GetSize());
    vkPresentInfo.pWaitSemaphores    = waitSemaphores.GetData();
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
    // 读初始化时定下的那个值。
    //
    // 原来的实现每次调用都重新枚举一遍设备扩展, 而且**只看扩展不看特性** ——
    // 驱动完全可以报告扩展却把特性位关掉 (虚拟化与软件实现里很常见)。那时
    // 这个函数报"支持", 而任何一次加速结构调用都会失败, 报的却是别的错。
    //
    // 现在它与"设备创建时到底启用了没有"是同一个事实。
    return m_RayTracingAvailable;
}

bool FVulkanDevice::HasDeviceExtension(const AnsiChar* name) const
{
    if (name == nullptr)
    {
        return false;
    }

    UInt32 extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(
        m_PhysicalDevice, nullptr, &extensionCount, nullptr);

    if (extensionCount == 0)
    {
        return false;
    }

    // 取全 — 截断的后果是目标扩展落在看不见的尾巴里, 于是本函数报"不支持",
    // 而调用方据此关掉一整套功能。没有任何一层会说"我只看了前 512 个"。
    constexpr SizeType kInlineExtensions = 512;
    TSmallVector<VkExtensionProperties, kInlineExtensions> extensions;
    ResizeZeroed(extensions, static_cast<SizeType>(extensionCount));

    UInt32 extQuery = extensionCount;
    vkEnumerateDeviceExtensionProperties(
        m_PhysicalDevice, nullptr, &extQuery, extensions.GetData());

    for (UInt32 i = 0; i < extQuery; ++i)
    {
        const AnsiChar* ext = extensions[i].extensionName;

        Int32 c = 0;
        while (name[c] != '\0' && ext[c] == name[c])
        {
            ++c;
        }

        // 两端都到头才算命中。
        //
        // 只判"name 走完了"是前缀匹配 —— VK_KHR_ray_query 会命中一个叫
        // VK_KHR_ray_query_something 的扩展。这个引擎已经在着色器布局检查
        // 上栽过一次同样的跟头 (fragMaterialIndex 命中 materialIndex),
        // 所以这里把两个终止条件都写上。
        if (name[c] == '\0' && ext[c] == '\0')
        {
            return true;
        }
    }

    return false;
}

bool FVulkanDevice::IsDrawIndirectFirstInstanceSupported() const
{
    // 报的是**物理设备支持不支持**, 而不是"创建逻辑设备时启用了没有"。
    // 两者本该一致 —— 创建时是无条件把查到的值填进去的 —— 但把它们绑成
    // 同一个来源就没有"启用了却报不支持"这种可能。
    return m_DeviceFeatures.drawIndirectFirstInstance != VK_FALSE;
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

    // 取全 — 截断的后果是目标扩展落在看不见的尾巴里, 于是本函数报"不支持",
    // 而调用方据此关掉一整套功能。没有任何一层会说"我只看了前 512 个"。
    constexpr SizeType kInlineExtensions = 512;
    TSmallVector<VkExtensionProperties, kInlineExtensions> extensions;
    ResizeZeroed(extensions, static_cast<SizeType>(extensionCount));

    UInt32 extQuery = extensionCount;
    vkEnumerateDeviceExtensionProperties(
        m_PhysicalDevice, nullptr, &extQuery, extensions.GetData());

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
