// ============================================================
// 文件名称：FVulkanDeviceInit.cpp
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：初始化链路严格按顺序执行，任何步骤失败立即返回错误码，
//          析构时逆序销毁保证 Vulkan 对象依赖关系正确。
// 功能描述：FVulkanDevice 构造/析构/初始化实现 — 覆盖 VkInstance 创建、
//          调试信使注册、Win32 表面创建、物理设备评分选择、
//          逻辑设备与队列创建、全局描述符池分配。
// 技术特性：物理设备选择采用评分机制 (独显优先、显存优先)；
//          队列族查找支持图形/计算/传输/呈现四种类型；
//          验证层仅在 enableValidation=true 时激活。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                          │ 描述                      │
// │────────────────────────────────│─────────────────────────│
// │ FVulkanDevice()                │ 默认构造 (零初始化)        │
// │ ~FVulkanDevice()               │ 逆序销毁全部 Vulkan 对象   │
// │ Initialize()                   │ 完整初始化链路入口         │
// │ CreateInstance()               │ 创建 VkInstance           │
// │ CreateDebugMessenger()         │ 注册调试回调              │
// │ CreateSurface()                │ 创建 Win32 VkSurfaceKHR   │
// │ SelectPhysicalDevice()         │ 评分选择最佳物理设备       │
// │ CreateLogicalDevice()          │ 创建逻辑设备与队列        │
// │ CreateDescriptorPool()         │ 创建全局描述符池          │
// │ FindMemoryType()               │ 查找内存类型索引          │
// │ GetVkQueue()                   │ 按类型获取队列            │
// │ GetQueueFamilyIndex()          │ 按类型获取队列族索引       │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-06  │ LimxTeam  │ 初始创建                        │
// ============================================================

#include "Vulkan/FVulkanDevice.h"
#include "Core/HAL/FPlatformFile.h"

namespace Limx
{

// ============================================================================
// 调试回调
// ============================================================================

static VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT      messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT             messageType,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
    void*                                       userData)
{
    (void)messageType;
    (void)userData;

    if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
    {
        LIMX_LOG(LogRHI, Error, "[Vulkan] {}", callbackData->pMessage);
    }
    else if (messageSeverity >=
             VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
    {
        LIMX_LOG(LogRHI, Warning, "[Vulkan] {}", callbackData->pMessage);
    }

    return VK_FALSE;
}

// ============================================================================
// 构造/析构
// ============================================================================

FVulkanDevice::FVulkanDevice() = default;

FVulkanDevice::~FVulkanDevice()
{
    if (m_Device != VK_NULL_HANDLE)
    {
        vkDeviceWaitIdle(m_Device);
    }

    LIMX_LOG(LogRHI, Display,
        "[Vulkan] 管线创建 — 共 {} 条, 累计 {} ms",
        m_PipelineCreateCount, m_PipelineCreateMs);

    // 管线缓存写回 —— 必须在 vkDestroyDevice 之前
    SavePipelineCache();

    if (m_PipelineCache != VK_NULL_HANDLE)
    {
        vkDestroyPipelineCache(m_Device, m_PipelineCache, nullptr);
        m_PipelineCache = VK_NULL_HANDLE;
    }

    // 销毁描述符池
    if (m_DescriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(m_Device, m_DescriptorPool, nullptr);
    }

    // 关闭显存分配器 — 必须早于 vkDestroyDevice:
    // 它内部要调用 vkUnmapMemory / vkFreeMemory, 设备销毁后这些句柄即失效
    if (m_MemoryAllocator.IsInitialized())
    {
        m_MemoryAllocator.LogStats("设备关闭前");
        m_MemoryAllocator.Shutdown();
    }

    // 销毁逻辑设备
    if (m_Device != VK_NULL_HANDLE)
    {
        vkDestroyDevice(m_Device, nullptr);
    }

    // 销毁表面
    if (m_Surface != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(m_Instance, m_Surface, nullptr);
    }

    // 销毁调试信使
    if (m_DebugMessenger != VK_NULL_HANDLE)
    {
        auto destroyFunc =
            reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(m_Instance,
                    "vkDestroyDebugUtilsMessengerEXT"));
        if (destroyFunc != nullptr)
        {
            destroyFunc(m_Instance, m_DebugMessenger, nullptr);
        }
    }

    // 销毁实例
    if (m_Instance != VK_NULL_HANDLE)
    {
        vkDestroyInstance(m_Instance, nullptr);
    }
}

// ============================================================================
// Initialize — 完整初始化链路
// ============================================================================

ERHIResult FVulkanDevice::Initialize(void* nativeWindowHandle,
                                      bool enableValidation,
                                      bool enableSyncValidation)
{
    m_IsValidationEnabled     = enableValidation;
    m_IsSyncValidationEnabled = enableValidation && enableSyncValidation;

    if (enableSyncValidation && !enableValidation)
    {
        LIMX_LOG(LogRHI, Warning,
            "[Vulkan] 请求了同步验证但未启用验证层 — 同步验证不会生效");
    }

    ERHIResult result = CreateInstance();
    if (!IsRHISuccess(result))
    {
        return result;
    }

    if (m_IsValidationEnabled)
    {
        result = CreateDebugMessenger();
        if (!IsRHISuccess(result))
        {
            return result;
        }
    }

    result = CreateSurface(nativeWindowHandle);
    if (!IsRHISuccess(result))
    {
        return result;
    }

    result = SelectPhysicalDevice();
    if (!IsRHISuccess(result))
    {
        return result;
    }

    result = CreateLogicalDevice();
    if (!IsRHISuccess(result))
    {
        return result;
    }

    // 显存分配器必须在逻辑设备就绪后、任何资源创建之前初始化
    result = m_MemoryAllocator.Initialize(m_Device, m_MemoryProperties,
                                          m_DeviceProperties.limits);
    if (!IsRHISuccess(result))
    {
        return result;
    }

    // 管线缓存要在任何管线创建之前就绪
    LoadPipelineCache();

    result = CreateDescriptorPool();
    if (!IsRHISuccess(result))
    {
        return result;
    }

    // 缓存厂商名称
    switch (m_DeviceProperties.vendorID)
    {
        case 0x1002: MemCopy(m_VendorName, "AMD", 4); break;
        case 0x10DE: MemCopy(m_VendorName, "NVIDIA", 7); break;
        case 0x8086: MemCopy(m_VendorName, "Intel", 6); break;
        case 0x13B5: MemCopy(m_VendorName, "ARM", 4); break;
        case 0x5143: MemCopy(m_VendorName, "Qualcomm", 9); break;
        default:     MemCopy(m_VendorName, "Unknown", 8); break;
    }

    LIMX_LOG(LogRHI, Log,
        "[Vulkan] 设备初始化完成: {} ({})",
        m_DeviceProperties.deviceName, m_VendorName);

    return ERHIResult::Success;
}

// ============================================================================
// CreateInstance
// ============================================================================

ERHIResult FVulkanDevice::CreateInstance()
{
    // ------------------------------------------------------------------
    // API 版本协商
    //
    // 请求高于 loader 支持的 apiVersion 会让 vkCreateInstance 返回
    // VK_ERROR_INCOMPATIBLE_DRIVER，因此先查询实例支持的版本再取最小值。
    // vkEnumerateInstanceVersion 自 Vulkan 1.1 起提供；在 1.0 loader 上
    // 该符号解析为空，此处按 1.0 处理并在后续检查中拒绝。
    // ------------------------------------------------------------------

    UInt32 instanceVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion(&instanceVersion) != VK_SUCCESS)
    {
        instanceVersion = VK_API_VERSION_1_0;
    }

    if (instanceVersion < kLimxMinimumApiVersion)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] 实例版本过低: {}.{}.{} — 引擎要求至少 1.3",
            VK_API_VERSION_MAJOR(instanceVersion),
            VK_API_VERSION_MINOR(instanceVersion),
            VK_API_VERSION_PATCH(instanceVersion));
        return ERHIResult::ErrorIncompatibleDriver;
    }

    m_ApiVersion = (instanceVersion < kLimxTargetApiVersion)
                       ? instanceVersion
                       : kLimxTargetApiVersion;

    LIMX_LOG(LogRHI, Log,
        "[Vulkan] 实例版本 {}.{}.{} — 协商 API 版本 {}.{}",
        VK_API_VERSION_MAJOR(instanceVersion),
        VK_API_VERSION_MINOR(instanceVersion),
        VK_API_VERSION_PATCH(instanceVersion),
        VK_API_VERSION_MAJOR(m_ApiVersion),
        VK_API_VERSION_MINOR(m_ApiVersion));

    VkApplicationInfo appInfo = {};
    appInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName   = "LimxEngine";
    appInfo.applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
    appInfo.pEngineName        = "Limx";
    appInfo.engineVersion      = VK_MAKE_API_VERSION(0, 1, 0, 0);
    appInfo.apiVersion         = m_ApiVersion;

    // 必需的实例扩展
    const char* instanceExtensions[] =
    {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
    };

    UInt32 extensionCount = m_IsValidationEnabled ? 3 : 2;

    // 验证层
    const char* validationLayers[] =
    {
        "VK_LAYER_KHRONOS_validation",
    };

    VkInstanceCreateInfo createInfo = {};
    createInfo.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo        = &appInfo;
    createInfo.enabledExtensionCount   = extensionCount;
    createInfo.ppEnabledExtensionNames = instanceExtensions;

    // 同步验证开关 —— 必须在实例创建时挂上 pNext, 建好之后无法再开。
    const VkValidationFeatureEnableEXT enabledFeatures[] =
    {
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT,
    };

    VkValidationFeaturesEXT validationFeatures = {};
    validationFeatures.sType =
        VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
    validationFeatures.enabledValidationFeatureCount = 1;
    validationFeatures.pEnabledValidationFeatures    = enabledFeatures;

    if (m_IsValidationEnabled)
    {
        createInfo.enabledLayerCount   = 1;
        createInfo.ppEnabledLayerNames = validationLayers;

        if (m_IsSyncValidationEnabled)
        {
            createInfo.pNext = &validationFeatures;
            LIMX_LOG(LogRHI, Log, "[Vulkan] 已启用同步验证");
        }
    }

    VkResult vkResult = vkCreateInstance(&createInfo, nullptr, &m_Instance);
    if (vkResult != VK_SUCCESS)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] vkCreateInstance 失败: {}", (Int32)vkResult);
        return ERHIResult::ErrorUnknown;
    }

    return ERHIResult::Success;
}

// ============================================================================
// CreateDebugMessenger
// ============================================================================

ERHIResult FVulkanDevice::CreateDebugMessenger()
{
    VkDebugUtilsMessengerCreateInfoEXT createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createInfo.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
      | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
      | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
      | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = VulkanDebugCallback;

    auto createFunc =
        reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_Instance,
                "vkCreateDebugUtilsMessengerEXT"));

    if (createFunc == nullptr)
    {
        LIMX_LOG(LogRHI, Warning,
            "[Vulkan] vkCreateDebugUtilsMessengerEXT 不可用");
        return ERHIResult::Success;
    }

    VkResult vkResult = createFunc(m_Instance, &createInfo, nullptr,
                                    &m_DebugMessenger);
    if (vkResult != VK_SUCCESS)
    {
        LIMX_LOG(LogRHI, Warning,
            "[Vulkan] 调试信使创建失败: {}", (Int32)vkResult);
    }

    return ERHIResult::Success;
}

// ============================================================================
// CreateSurface
// ============================================================================

ERHIResult FVulkanDevice::CreateSurface(void* nativeWindowHandle)
{
    VkWin32SurfaceCreateInfoKHR createInfo = {};
    createInfo.sType     = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    createInfo.hinstance = GetModuleHandleW(nullptr);
    createInfo.hwnd      = static_cast<HWND>(nativeWindowHandle);

    VkResult vkResult = vkCreateWin32SurfaceKHR(m_Instance, &createInfo,
                                                  nullptr, &m_Surface);
    if (vkResult != VK_SUCCESS)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] vkCreateWin32SurfaceKHR 失败: {}", (Int32)vkResult);
        return ERHIResult::ErrorSurfaceLost;
    }

    return ERHIResult::Success;
}

// ============================================================================
// SelectPhysicalDevice — 评分机制选择最佳 GPU
// ============================================================================

ERHIResult FVulkanDevice::SelectPhysicalDevice()
{
    UInt32 deviceCount = 0;
    vkEnumeratePhysicalDevices(m_Instance, &deviceCount, nullptr);

    if (deviceCount == 0)
    {
        LIMX_LOG(LogRHI, Error, "[Vulkan] 未找到支持 Vulkan 的 GPU");
        return ERHIResult::ErrorDeviceLost;
    }

    // 使用栈缓冲区存储物理设备列表 (通常 ≤8 个 GPU)
    constexpr UInt32 kMaxPhysicalDevices = 16;
    VkPhysicalDevice devices[kMaxPhysicalDevices];
    if (deviceCount > kMaxPhysicalDevices)
    {
        deviceCount = kMaxPhysicalDevices;
    }
    vkEnumeratePhysicalDevices(m_Instance, &deviceCount, devices);

    // 评分选择
    Int32 bestScore = -1;
    VkPhysicalDevice bestDevice = VK_NULL_HANDLE;

    for (UInt32 i = 0; i < deviceCount; ++i)
    {
        VkPhysicalDeviceProperties properties;
        VkPhysicalDeviceFeatures features;
        vkGetPhysicalDeviceProperties(devices[i], &properties);
        vkGetPhysicalDeviceFeatures(devices[i], &features);

        // 检查是否支持所需的队列族
        UInt32 queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(
            devices[i], &queueFamilyCount, nullptr);
        if (queueFamilyCount == 0)
        {
            continue;
        }

        // 检查是否有图形队列支持
        constexpr UInt32 kMaxQueueFamilies = 16;
        VkQueueFamilyProperties queueProps[kMaxQueueFamilies];
        UInt32 queryCount = queueFamilyCount;
        if (queryCount > kMaxQueueFamilies)
        {
            queryCount = kMaxQueueFamilies;
        }
        vkGetPhysicalDeviceQueueFamilyProperties(
            devices[i], &queryCount, queueProps);

        bool hasGraphicsQueue = false;
        for (UInt32 q = 0; q < queryCount; ++q)
        {
            if (queueProps[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            {
                hasGraphicsQueue = true;
                break;
            }
        }

        if (!hasGraphicsQueue)
        {
            continue;
        }

        // 检查交换链扩展支持
        UInt32 extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(
            devices[i], nullptr, &extensionCount, nullptr);

        bool hasSwapchainExtension = false;
        if (extensionCount > 0)
        {
            constexpr UInt32 kMaxExtensions = 512;
            VkExtensionProperties extensions[kMaxExtensions];
            UInt32 extQuery = extensionCount;
            if (extQuery > kMaxExtensions)
            {
                extQuery = kMaxExtensions;
            }
            vkEnumerateDeviceExtensionProperties(
                devices[i], nullptr, &extQuery, extensions);

            for (UInt32 e = 0; e < extQuery; ++e)
            {
                // 手动比较扩展名
                const char* target = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
                const char* ext = extensions[e].extensionName;
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
                    hasSwapchainExtension = true;
                    break;
                }
            }
        }

        if (!hasSwapchainExtension)
        {
            continue;
        }

        // 评分
        Int32 score = 0;

        // 独显 +10000 分
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        {
            score += 10000;
        }
        else if (properties.deviceType ==
                 VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
        {
            score += 1000;
        }

        // 显存大小 (每 256MB 加 1 分)
        VkPhysicalDeviceMemoryProperties memProps;
        vkGetPhysicalDeviceMemoryProperties(devices[i], &memProps);
        for (UInt32 h = 0; h < memProps.memoryHeapCount; ++h)
        {
            if (memProps.memoryHeaps[h].flags &
                VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            {
                score += static_cast<Int32>(
                    memProps.memoryHeaps[h].size / (256ULL * 1024 * 1024));
            }
        }

        // 最大纹理尺寸加分
        score += static_cast<Int32>(
            properties.limits.maxImageDimension2D / 4096);

        // 几何着色器支持加分
        if (features.geometryShader)
        {
            score += 100;
        }

        // 各向异性过滤支持加分
        if (features.samplerAnisotropy)
        {
            score += 100;
        }

        if (score > bestScore)
        {
            bestScore = score;
            bestDevice = devices[i];
        }
    }

    if (bestDevice == VK_NULL_HANDLE)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] 未找到满足要求的 GPU");
        return ERHIResult::ErrorDeviceLost;
    }

    m_PhysicalDevice = bestDevice;
    vkGetPhysicalDeviceProperties(m_PhysicalDevice, &m_DeviceProperties);
    vkGetPhysicalDeviceFeatures(m_PhysicalDevice, &m_DeviceFeatures);
    vkGetPhysicalDeviceMemoryProperties(
        m_PhysicalDevice, &m_MemoryProperties);

    // ------------------------------------------------------------------
    // 设备 API 版本可能低于实例版本 — 再次收敛协商版本。
    // pNext 特性链中的结构体只有在设备 apiVersion 覆盖该版本时才允许
    // 挂接，否则 vkGetPhysicalDeviceFeatures2 行为未定义。
    // ------------------------------------------------------------------

    if (m_DeviceProperties.apiVersion < kLimxMinimumApiVersion)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] GPU '{}' API 版本过低: {}.{}.{} — 引擎要求至少 1.3",
            m_DeviceProperties.deviceName,
            VK_API_VERSION_MAJOR(m_DeviceProperties.apiVersion),
            VK_API_VERSION_MINOR(m_DeviceProperties.apiVersion),
            VK_API_VERSION_PATCH(m_DeviceProperties.apiVersion));
        return ERHIResult::ErrorIncompatibleDriver;
    }

    if (m_DeviceProperties.apiVersion < m_ApiVersion)
    {
        m_ApiVersion = m_DeviceProperties.apiVersion;
    }

    // ------------------------------------------------------------------
    // 查询核心版本特性 — 设备创建时据此裁剪要启用的特性集
    // ------------------------------------------------------------------

    m_DeviceFeatures11.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    m_DeviceFeatures12.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    m_DeviceFeatures13.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    m_DeviceFeatures14.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES;

    m_DeviceFeatures11.pNext = &m_DeviceFeatures12;
    m_DeviceFeatures12.pNext = &m_DeviceFeatures13;
    m_DeviceFeatures13.pNext =
        (m_ApiVersion >= VK_API_VERSION_1_4) ? &m_DeviceFeatures14 : nullptr;
    m_DeviceFeatures14.pNext = nullptr;

    VkPhysicalDeviceFeatures2 features2 = {};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &m_DeviceFeatures11;

    vkGetPhysicalDeviceFeatures2(m_PhysicalDevice, &features2);

    LIMX_LOG(LogRHI, Log,
        "[Vulkan] 选择 GPU: {} (评分: {}, API {}.{}.{})",
        m_DeviceProperties.deviceName, bestScore,
        VK_API_VERSION_MAJOR(m_DeviceProperties.apiVersion),
        VK_API_VERSION_MINOR(m_DeviceProperties.apiVersion),
        VK_API_VERSION_PATCH(m_DeviceProperties.apiVersion));

    LIMX_LOG(LogRHI, Log,
        "[Vulkan] 关键特性 — DynamicRendering:{} Sync2:{} "
        "TimelineSemaphore:{} BufferDeviceAddress:{} DescriptorIndexing:{} "
        "Maintenance5:{} DemoteToHelper:{}",
        m_DeviceFeatures13.dynamicRendering != VK_FALSE,
        m_DeviceFeatures13.synchronization2 != VK_FALSE,
        m_DeviceFeatures12.timelineSemaphore != VK_FALSE,
        m_DeviceFeatures12.bufferDeviceAddress != VK_FALSE,
        m_DeviceFeatures12.descriptorIndexing != VK_FALSE,
        m_DeviceFeatures14.maintenance5 != VK_FALSE,
        m_DeviceFeatures13.shaderDemoteToHelperInvocation != VK_FALSE);

    // 引擎渲染路径强依赖 Sync2 + DynamicRendering，缺失则无法运行
    if (m_DeviceFeatures13.dynamicRendering == VK_FALSE ||
        m_DeviceFeatures13.synchronization2 == VK_FALSE)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] GPU '{}' 缺少必需特性 dynamicRendering/synchronization2",
            m_DeviceProperties.deviceName);
        return ERHIResult::ErrorIncompatibleDriver;
    }

    return ERHIResult::Success;
}

// ============================================================================
// CreateLogicalDevice — 创建逻辑设备与队列
// ============================================================================

ERHIResult FVulkanDevice::CreateLogicalDevice()
{
    // 获取队列族属性
    UInt32 queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(
        m_PhysicalDevice, &queueFamilyCount, nullptr);

    constexpr UInt32 kMaxQueueFamilies = 16;
    VkQueueFamilyProperties queueProps[kMaxQueueFamilies];
    UInt32 queryCount = queueFamilyCount;
    if (queryCount > kMaxQueueFamilies)
    {
        queryCount = kMaxQueueFamilies;
    }
    vkGetPhysicalDeviceQueueFamilyProperties(
        m_PhysicalDevice, &queryCount, queueProps);

    // 查找队列族索引
    m_GraphicsQueueFamily = 0xFFFFFFFF;
    m_ComputeQueueFamily  = 0xFFFFFFFF;
    m_TransferQueueFamily = 0xFFFFFFFF;
    m_PresentQueueFamily  = 0xFFFFFFFF;

    for (UInt32 i = 0; i < queryCount; ++i)
    {
        // 图形队列
        if ((queueProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            m_GraphicsQueueFamily == 0xFFFFFFFF)
        {
            m_GraphicsQueueFamily = i;
        }

        // 独立计算队列 (非图形)
        if ((queueProps[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
            !(queueProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            m_ComputeQueueFamily == 0xFFFFFFFF)
        {
            m_ComputeQueueFamily = i;
        }

        // 独立传输队列 (非图形非计算)
        if ((queueProps[i].queueFlags & VK_QUEUE_TRANSFER_BIT) &&
            !(queueProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            !(queueProps[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
            m_TransferQueueFamily == 0xFFFFFFFF)
        {
            m_TransferQueueFamily = i;
        }

        // 呈现队列
        VkBool32 presentSupport = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(
            m_PhysicalDevice, i, m_Surface, &presentSupport);
        if (presentSupport && m_PresentQueueFamily == 0xFFFFFFFF)
        {
            m_PresentQueueFamily = i;
        }
    }

    // 回退: 计算队列未找到独立的，使用图形队列
    if (m_ComputeQueueFamily == 0xFFFFFFFF)
    {
        m_ComputeQueueFamily = m_GraphicsQueueFamily;
    }

    // 回退: 传输队列未找到独立的，使用图形队列
    if (m_TransferQueueFamily == 0xFFFFFFFF)
    {
        m_TransferQueueFamily = m_GraphicsQueueFamily;
    }

    // 时间戳有效位数 —— 取图形队列族的, 因为逐 Pass 计时都打在图形队列上
    if (m_GraphicsQueueFamily != 0xFFFFFFFF &&
        m_GraphicsQueueFamily < queryCount)
    {
        m_TimestampValidBits =
            queueProps[m_GraphicsQueueFamily].timestampValidBits;
    }

    if (m_GraphicsQueueFamily == 0xFFFFFFFF ||
        m_PresentQueueFamily == 0xFFFFFFFF)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] 未找到图形或呈现队列族");
        return ERHIResult::ErrorDeviceLost;
    }

    // 收集唯一的队列族索引
    UInt32 uniqueFamilies[4];
    UInt32 uniqueCount = 0;

    auto addUnique = [&](UInt32 family)
    {
        for (UInt32 i = 0; i < uniqueCount; ++i)
        {
            if (uniqueFamilies[i] == family)
            {
                return;
            }
        }
        uniqueFamilies[uniqueCount++] = family;
    };

    addUnique(m_GraphicsQueueFamily);
    addUnique(m_ComputeQueueFamily);
    addUnique(m_TransferQueueFamily);
    addUnique(m_PresentQueueFamily);

    // 创建队列创建信息
    Float32 queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfos[4];
    for (UInt32 i = 0; i < uniqueCount; ++i)
    {
        MemZero(&queueCreateInfos[i], sizeof(VkDeviceQueueCreateInfo));
        queueCreateInfos[i].sType =
            VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfos[i].queueFamilyIndex = uniqueFamilies[i];
        queueCreateInfos[i].queueCount       = 1;
        queueCreateInfos[i].pQueuePriorities = &queuePriority;
    }

    // 设备特性
    VkPhysicalDeviceFeatures deviceFeatures = {};
    deviceFeatures.samplerAnisotropy = m_DeviceFeatures.samplerAnisotropy;
    deviceFeatures.fillModeNonSolid  = m_DeviceFeatures.fillModeNonSolid;
    deviceFeatures.wideLines         = m_DeviceFeatures.wideLines;
    deviceFeatures.geometryShader    = m_DeviceFeatures.geometryShader;
    deviceFeatures.tessellationShader =
        m_DeviceFeatures.tessellationShader;
    deviceFeatures.multiDrawIndirect =
        m_DeviceFeatures.multiDrawIndirect;
    deviceFeatures.fragmentStoresAndAtomics =
        m_DeviceFeatures.fragmentStoresAndAtomics;
    deviceFeatures.vertexPipelineStoresAndAtomics =
        m_DeviceFeatures.vertexPipelineStoresAndAtomics;
    deviceFeatures.shaderInt64  = m_DeviceFeatures.shaderInt64;
    deviceFeatures.depthClamp   = m_DeviceFeatures.depthClamp;
    deviceFeatures.pipelineStatisticsQuery =
        m_DeviceFeatures.pipelineStatisticsQuery;

    // 必需的设备扩展
    const char* deviceExtensions[] =
    {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };

    // ------------------------------------------------------------------
    // 版本特性裁剪
    //
    // 只启用 PickPhysicalDevice 中经 vkGetPhysicalDeviceFeatures2 确认
    // 设备支持的特性。请求任一不支持的特性都会使 vkCreateDevice 返回
    // VK_ERROR_FEATURE_NOT_PRESENT 并导致整个设备创建失败。
    // ------------------------------------------------------------------

    // Vulkan 1.2 特性 (Timeline Semaphore / 描述符索引 / 设备地址等)
    VkPhysicalDeviceVulkan12Features features12 = {};
    features12.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.timelineSemaphore =
        m_DeviceFeatures12.timelineSemaphore;
    features12.descriptorIndexing =
        m_DeviceFeatures12.descriptorIndexing;

    // descriptorIndexing 只是个汇总标志 —— 它表示"设备支持描述符索引的
    // 最小特性集", 但启用它**不会**自动启用任何一个具体子特性。下面这
    // 几项 bindless 都要用到, 必须各自开。
    //
    // 一律照抄设备实际支持的值 (m_DeviceFeatures12 是查询结果): 请求任一
    // 不支持的特性会让 vkCreateDevice 直接返回 FEATURE_NOT_PRESENT, 整个
    // 设备创建失败。设备不支持时这里传 FALSE, 而 FBindlessTable 会在创建
    // 布局时失败并退回旧路径。
    features12.descriptorBindingPartiallyBound =
        m_DeviceFeatures12.descriptorBindingPartiallyBound;
    features12.descriptorBindingSampledImageUpdateAfterBind =
        m_DeviceFeatures12.descriptorBindingSampledImageUpdateAfterBind;
    features12.descriptorBindingStorageBufferUpdateAfterBind =
        m_DeviceFeatures12.descriptorBindingStorageBufferUpdateAfterBind;
    features12.descriptorBindingVariableDescriptorCount =
        m_DeviceFeatures12.descriptorBindingVariableDescriptorCount;
    features12.shaderSampledImageArrayNonUniformIndexing =
        m_DeviceFeatures12.shaderSampledImageArrayNonUniformIndexing;
    features12.shaderStorageBufferArrayNonUniformIndexing =
        m_DeviceFeatures12.shaderStorageBufferArrayNonUniformIndexing;
    features12.runtimeDescriptorArray =
        m_DeviceFeatures12.runtimeDescriptorArray;
    features12.bufferDeviceAddress =
        m_DeviceFeatures12.bufferDeviceAddress;
    features12.scalarBlockLayout =
        m_DeviceFeatures12.scalarBlockLayout;
    features12.hostQueryReset =
        m_DeviceFeatures12.hostQueryReset;
    features12.separateDepthStencilLayouts =
        m_DeviceFeatures12.separateDepthStencilLayouts;

    // bindless 三项关键子特性 —— 缺任何一项 FBindlessTable 都建不起来
    LIMX_LOG(LogRHI, Display,
        "[Vulkan] bindless 子特性 — PartiallyBound:{} "
        "SampledImageUpdateAfterBind:{} NonUniformIndexing:{}",
        m_DeviceFeatures12.descriptorBindingPartiallyBound != VK_FALSE,
        m_DeviceFeatures12.descriptorBindingSampledImageUpdateAfterBind
            != VK_FALSE,
        m_DeviceFeatures12.shaderSampledImageArrayNonUniformIndexing
            != VK_FALSE);

    // Vulkan 1.3 特性 (Dynamic Rendering / Sync2) — 已在设备选择阶段
    // 校验 dynamicRendering/synchronization2 必须存在
    VkPhysicalDeviceVulkan13Features features13 = {};
    features13.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = m_DeviceFeatures13.dynamicRendering;
    features13.synchronization2 = m_DeviceFeatures13.synchronization2;
    features13.maintenance4     = m_DeviceFeatures13.maintenance4;

    // 着色器语言特性 —— 面向 SPIR-V 1.6 编译时 glslang 会为 discard 等语句
    // 生成 DemoteToHelperInvocation / TerminateInvocation 能力。若不启用对应
    // 设备特性, vkCreateShaderModule 会因能力未声明而被验证层拒绝。
    features13.shaderDemoteToHelperInvocation =
        m_DeviceFeatures13.shaderDemoteToHelperInvocation;
    features13.shaderTerminateInvocation =
        m_DeviceFeatures13.shaderTerminateInvocation;
    features13.shaderZeroInitializeWorkgroupMemory =
        m_DeviceFeatures13.shaderZeroInitializeWorkgroupMemory;

    // 特性链
    features12.pNext = &features13;
    features13.pNext = nullptr;

    VkDeviceCreateInfo createInfo = {};
    createInfo.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.pNext                   = &features12;
    createInfo.queueCreateInfoCount    = uniqueCount;
    createInfo.pQueueCreateInfos       = queueCreateInfos;
    createInfo.enabledExtensionCount   =
        static_cast<UInt32>(LIMX_ARRAY_COUNT(deviceExtensions));
    createInfo.ppEnabledExtensionNames = deviceExtensions;
    createInfo.pEnabledFeatures        = &deviceFeatures;

    VkResult vkResult = vkCreateDevice(m_PhysicalDevice, &createInfo,
                                        nullptr, &m_Device);
    if (vkResult != VK_SUCCESS)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] vkCreateDevice 失败: {}", (Int32)vkResult);
        return ERHIResult::ErrorUnknown;
    }

    // 获取队列
    vkGetDeviceQueue(m_Device, m_GraphicsQueueFamily, 0,
                      &m_GraphicsQueue);
    vkGetDeviceQueue(m_Device, m_ComputeQueueFamily, 0,
                      &m_ComputeQueue);
    vkGetDeviceQueue(m_Device, m_TransferQueueFamily, 0,
                      &m_TransferQueue);
    vkGetDeviceQueue(m_Device, m_PresentQueueFamily, 0,
                      &m_PresentQueue);

    LIMX_LOG(LogRHI, Log,
        "[Vulkan] 逻辑设备创建完成 — 图形:{} 计算:{} 传输:{} 呈现:{}",
        m_GraphicsQueueFamily, m_ComputeQueueFamily,
        m_TransferQueueFamily, m_PresentQueueFamily);

    return ERHIResult::Success;
}

// ============================================================================
// CreateDescriptorPool — 全局描述符池
// ============================================================================

ERHIResult FVulkanDevice::CreateDescriptorPool()
{
    // 预分配各类型描述符的容量
    VkDescriptorPoolSize poolSizes[] =
    {
        { VK_DESCRIPTOR_TYPE_SAMPLER,                1024 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          4096 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1024 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,   256  },
        { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,   256  },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         4096 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         4096 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1024 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1024 },
        { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,        512  },
    };

    VkDescriptorPoolCreateInfo createInfo = {};
    createInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    // UPDATE_AFTER_BIND_BIT 是 bindless 纹理表要用的 —— 场景加载时逐张
    // 注册贴图, 而那时描述符集可能已经被绑过。没有这个标志时, 布局里
    // 声明了 UPDATE_AFTER_BIND 的集根本分配不出来。
    createInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT
                             | VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    createInfo.maxSets       = 8192;
    createInfo.poolSizeCount = 11;
    createInfo.pPoolSizes    = poolSizes;

    VkResult vkResult = vkCreateDescriptorPool(m_Device, &createInfo,
                                                nullptr, &m_DescriptorPool);
    if (vkResult != VK_SUCCESS)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] vkCreateDescriptorPool 失败: {}", (Int32)vkResult);
        return ERHIResult::ErrorOutOfHostMemory;
    }

    return ERHIResult::Success;
}

// ============================================================================
// FindMemoryType
// ============================================================================

UInt32 FVulkanDevice::FindMemoryType(
    UInt32 typeFilter,
    VkMemoryPropertyFlags properties) const
{
    for (UInt32 i = 0; i < m_MemoryProperties.memoryTypeCount; ++i)
    {
        if ((typeFilter & (1u << i)) &&
            (m_MemoryProperties.memoryTypes[i].propertyFlags &
             properties) == properties)
        {
            return i;
        }
    }

    LIMX_LOG(LogRHI, Error,
        "[Vulkan] 未找到匹配的内存类型 (filter={} props={})",
        FHex(typeFilter), FHex(static_cast<UInt32>(properties)));

    return 0xFFFFFFFF;
}

// ============================================================================
// 队列辅助
// ============================================================================

VkQueue FVulkanDevice::GetVkQueue(EQueueType type) const
{
    switch (type)
    {
        case EQueueType::Graphics: return m_GraphicsQueue;
        case EQueueType::Compute:  return m_ComputeQueue;
        case EQueueType::Transfer: return m_TransferQueue;
        default:                   return m_GraphicsQueue;
    }
}

UInt32 FVulkanDevice::GetQueueFamilyIndex(EQueueType type) const
{
    switch (type)
    {
        case EQueueType::Graphics: return m_GraphicsQueueFamily;
        case EQueueType::Compute:  return m_ComputeQueueFamily;
        case EQueueType::Transfer: return m_TransferQueueFamily;
        default:                   return m_GraphicsQueueFamily;
    }
}

// ============================================================================
// 管线缓存
// ============================================================================

void FVulkanDevice::LoadPipelineCache()
{
    m_PipelineCachePath = FString("Intermediate/PipelineCache.bin");

    TArray<UInt8> blob;

    if (FPlatformFile::Exists(m_PipelineCachePath))
    {
        blob = FPlatformFile::ReadAllBytes(m_PipelineCachePath);
    }

    VkPipelineCacheCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;

    // 内容与驱动版本、设备强绑定。Vulkan 在缓存头部记了设备 UUID 与驱动
    // 版本, 不匹配时驱动自己会忽略这份数据并从空开始 —— 不需要我们判断,
    // 也不该自己解析那个头部 (它的布局是实现细节)。
    if (!blob.IsEmpty())
    {
        createInfo.initialDataSize = blob.GetSize();
        createInfo.pInitialData    = blob.GetData();
    }

    const VkResult result = vkCreatePipelineCache(
        m_Device, &createInfo, nullptr, &m_PipelineCache);

    if (result != VK_SUCCESS)
    {
        // 缓存建不出来不是致命错误 —— 退回逐次编译即可
        LIMX_LOG(LogRHI, Warning,
            "[Vulkan] vkCreatePipelineCache 失败: {} — 本次不使用管线缓存",
            (Int32)result);

        m_PipelineCache = VK_NULL_HANDLE;
        return;
    }

    LIMX_LOG(LogRHI, Display,
        "[Vulkan] 管线缓存 — 装入 {} 字节 (来自 {})",
        blob.GetSize(),
        blob.IsEmpty() ? "空, 首次运行或已失效" : m_PipelineCachePath.GetCStr());
}

void FVulkanDevice::SavePipelineCache()
{
    if (m_PipelineCache == VK_NULL_HANDLE)
    {
        return;
    }

    SizeType size = 0;

    if (vkGetPipelineCacheData(m_Device, m_PipelineCache, &size, nullptr)
        != VK_SUCCESS || size == 0)
    {
        return;
    }

    TArray<UInt8> blob;
    blob.SetSize(size);

    if (vkGetPipelineCacheData(m_Device, m_PipelineCache, &size, blob.GetData())
        != VK_SUCCESS)
    {
        return;
    }

    // 目录可能不存在 (干净克隆里 Intermediate/ 不入库)
    FPlatformFile::CreateDirectoryTree(FString("Intermediate"));

    if (FPlatformFile::WriteAllBytes(m_PipelineCachePath, blob.GetData(), size))
    {
        LIMX_LOG(LogRHI, Display,
            "[Vulkan] 管线缓存 — 写回 {} 字节", size);
    }
    else
    {
        LIMX_LOG(LogRHI, Warning,
            "[Vulkan] 管线缓存写回失败: {}", m_PipelineCachePath.GetCStr());
    }
}

} // namespace Limx
