// ============================================================
// 文件名称：FVulkanDevice.h
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：单一所有权设计 — FVulkanDevice 拥有 VkInstance→VkDevice
//          完整生命周期，资源通过 TVulkanResourcePool 句柄化管理，
//          析构时保证先 WaitIdle 再逆序销毁所有 Vulkan 对象。
// 功能描述：Vulkan 后端 IRHIDevice 实现 — 管理 Vulkan 实例、物理设备、
//          逻辑设备、队列，以及全部 GPU 资源的创建/销毁/查询。
//          通过 Initialize() 完成完整的 Vulkan 初始化链路。
// 技术特性：TVulkanResourcePool 句柄化资源管理；VkDebugUtilsMessenger
//          调试层支持；VkDescriptorPool 全局描述符池；
//          内存类型查找辅助函数。
//
// ── 结构体表 (Vulkan 资源内部数据) ──────────────────────────────
// │ 结构体名                     │ 描述                         │
// │───────────────────────────│───────────────────────────│
// │ FVulkanBufferData          │ VkBuffer + VkDeviceMemory   │
// │ FVulkanTextureData         │ VkImage + VkDeviceMemory    │
// │ FVulkanTextureViewData     │ VkImageView                 │
// │ FVulkanSamplerData         │ VkSampler                   │
// │ FVulkanShaderData          │ VkShaderModule + 阶段       │
// │ FVulkanRenderPassData      │ VkRenderPass                │
// │ FVulkanFramebufferData     │ VkFramebuffer               │
// │ FVulkanPipelineLayoutData  │ VkPipelineLayout            │
// │ FVulkanDescSetLayoutData   │ VkDescriptorSetLayout       │
// │ FVulkanDescriptorSetData   │ VkDescriptorSet             │
// │ FVulkanGraphicsPipelineData│ VkPipeline                  │
// │ FVulkanComputePipelineData │ VkPipeline                  │
// │ FVulkanFenceData           │ VkFence                     │
// │ FVulkanSemaphoreData       │ VkSemaphore                 │
// │ FVulkanCommandPoolData     │ VkCommandPool + 队列类型    │
// │ FVulkanCommandBufferData   │ VkCommandBuffer + 所属池    │
// │ FVulkanSwapchainData       │ VkSwapchainKHR + 图像数组   │
// │ FVulkanQueryPoolData       │ VkQueryPool + 类型/数量     │
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                          │ 描述                      │
// │────────────────────────────────│─────────────────────────│
// │ Initialize()                   │ 完整 Vulkan 初始化链路     │
// │ FindMemoryType()               │ 查找满足要求的内存类型索引  │
// │ GetVkQueue()                   │ 按队列类型获取 VkQueue     │
// │ GetQueueFamilyIndex()          │ 按队列类型获取族索引       │
// │ (全部 IRHIDevice 纯虚方法)      │ 见 IRHIDevice.h          │
//
// ── 字段表 ──────────────────────────────────────────────────
// │ 字段名                     │ 类型                      │ 描述          │
// │───────────────────────────│─────────────────────────│─────────────│
// │ m_Instance                │ VkInstance               │ Vulkan 实例   │
// │ m_DebugMessenger          │ VkDebugUtilsMessengerEXT │ 调试信使      │
// │ m_PhysicalDevice          │ VkPhysicalDevice         │ 物理设备      │
// │ m_Device                  │ VkDevice                 │ 逻辑设备      │
// │ m_Surface                 │ VkSurfaceKHR             │ 窗口表面      │
// │ m_GraphicsQueue           │ VkQueue                  │ 图形队列      │
// │ m_ComputeQueue            │ VkQueue                  │ 计算队列      │
// │ m_TransferQueue           │ VkQueue                  │ 传输队列      │
// │ m_PresentQueue            │ VkQueue                  │ 呈现队列      │
// │ m_DescriptorPool          │ VkDescriptorPool         │ 全局描述符池   │
// │ m_IsValidationEnabled     │ bool                     │ 验证层开关    │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-06  │ LimxTeam  │ 初始创建                        │
// ============================================================

#pragma once

#include "Vulkan/VulkanCommon.h"
#include "RHI/RHI/IRHIDevice.h"
#include "RHI/RHI/IRHICommandBuffer.h"
#include "Vulkan/TVulkanResourcePool.h"
#include "Vulkan/FVulkanMemoryAllocator.h"

namespace Limx
{

// ============================================================================
// Vulkan 资源内部数据结构
// ============================================================================

struct FVulkanBufferData
{
    VkBuffer     Buffer = VK_NULL_HANDLE;
    VkDeviceSize Size   = 0;

    /// 显存来自分配器的子分配 — 不再持有独立的 VkDeviceMemory
    FVulkanAllocation Allocation;

    /// 主机可访问地址 — 等于 Allocation.MappedPtr, 冗余保存以便热路径直取
    void* MappedPtr = nullptr;
};

struct FVulkanTextureData
{
    VkImage        Image            = VK_NULL_HANDLE;

    /// 显存来自分配器 — 交换链图像由呈现引擎拥有内存, 此处保持无效
    FVulkanAllocation Allocation;

    VkFormat       Format           = VK_FORMAT_UNDEFINED;
    VkExtent3D     Extent           = { 0, 0, 0 };
    UInt32         MipLevels        = 1;
    UInt32         ArrayLayers      = 1;
    EPixelFormat   RhiFormat        = EPixelFormat::Unknown;
    bool           IsSwapchainImage = false;
};

struct FVulkanTextureViewData
{
    VkImageView ImageView = VK_NULL_HANDLE;
};

struct FVulkanSamplerData
{
    VkSampler Sampler = VK_NULL_HANDLE;
};

struct FVulkanShaderData
{
    VkShaderModule Module = VK_NULL_HANDLE;
    EShaderStage   Stage  = EShaderStage::None;
};

struct FVulkanRenderPassData
{
    VkRenderPass RenderPass = VK_NULL_HANDLE;
};

struct FVulkanFramebufferData
{
    VkFramebuffer Framebuffer = VK_NULL_HANDLE;
};

struct FVulkanPipelineLayoutData
{
    VkPipelineLayout Layout = VK_NULL_HANDLE;
};

struct FVulkanDescSetLayoutData
{
    VkDescriptorSetLayout Layout = VK_NULL_HANDLE;
};

struct FVulkanDescriptorSetData
{
    VkDescriptorSet Set = VK_NULL_HANDLE;
};

struct FVulkanGraphicsPipelineData
{
    VkPipeline Pipeline = VK_NULL_HANDLE;
};

struct FVulkanComputePipelineData
{
    VkPipeline Pipeline = VK_NULL_HANDLE;
};

struct FVulkanFenceData
{
    VkFence Fence = VK_NULL_HANDLE;
};

struct FVulkanSemaphoreData
{
    VkSemaphore Semaphore = VK_NULL_HANDLE;
};

struct FVulkanCommandPoolData
{
    VkCommandPool Pool      = VK_NULL_HANDLE;
    EQueueType    QueueType = EQueueType::Graphics;
};

struct FVulkanCommandBufferData
{
    VkCommandBuffer     CommandBuffer = VK_NULL_HANDLE;
    FRHICommandPoolHandle OwnerPool;

    /// 分配时的级别 —— ExecuteCommands 要据此拦住"把主缓冲当次级执行"
    ECommandBufferLevel Level = ECommandBufferLevel::Primary;
};

struct FVulkanSwapchainData
{
    VkSwapchainKHR           Swapchain  = VK_NULL_HANDLE;
    TArray<VkImage>          Images;
    TArray<FRHITextureViewHandle> ImageViews;
    TArray<FRHITextureHandle>     ImageTextures;
    VkFormat                 Format     = VK_FORMAT_UNDEFINED;
    VkExtent2D               Extent     = { 0, 0 };
    UInt32                   ImageCount = 0;
    EPixelFormat             RhiFormat  = EPixelFormat::Unknown;
};

struct FVulkanQueryPoolData
{
    VkQueryPool Pool  = VK_NULL_HANDLE;
    EQueryType  Type  = EQueryType::Timestamp;
    UInt32      Count = 0;
};

// ============================================================================
// FVulkanDevice — IRHIDevice Vulkan 实现
// ============================================================================

class FVulkanDevice final : public IRHIDevice
{
public:
    FVulkanDevice();
    ~FVulkanDevice() override;

    // ====================================================================
    // 初始化
    // ====================================================================

    /// 完整 Vulkan 初始化: 实例→表面→物理设备→逻辑设备→队列→描述符池
    /// @param nativeWindowHandle   Win32 HWND 窗口句柄
    /// @param enableValidation     是否启用 Vulkan 验证层
    /// @param enableSyncValidation 是否额外启用同步验证 (需 enableValidation)
    ERHIResult Initialize(void* nativeWindowHandle,
                          bool enableValidation,
                          bool enableSyncValidation = false);

    // ====================================================================
    // IRHIDevice 接口实现 — 缓冲区
    // ====================================================================

    ERHIResult CreateBuffer(const FRHIBufferDesc& desc,
                            FRHIBufferHandle& outHandle) override;
    void DestroyBuffer(FRHIBufferHandle& handle) override;
    ERHIResult MapBuffer(FRHIBufferHandle handle,
                          void** outMappedPtr) override;
    void UnmapBuffer(FRHIBufferHandle handle) override;

    // ====================================================================
    // IRHIDevice 接口实现 — 纹理
    // ====================================================================

    ERHIResult CreateTexture(const FRHITextureDesc& desc,
                              FRHITextureHandle& outHandle) override;
    void DestroyTexture(FRHITextureHandle& handle) override;
    ERHIResult CreateTextureView(const FRHITextureViewDesc& desc,
                                  FRHITextureViewHandle& outHandle) override;
    void DestroyTextureView(FRHITextureViewHandle& handle) override;

    // ====================================================================
    // IRHIDevice 接口实现 — 采样器
    // ====================================================================

    ERHIResult CreateSampler(const FRHISamplerDesc& desc,
                              FRHISamplerHandle& outHandle) override;
    void DestroySampler(FRHISamplerHandle& handle) override;

    // ====================================================================
    // IRHIDevice 接口实现 — 着色器
    // ====================================================================

    ERHIResult CreateShader(const FRHIShaderDesc& desc,
                             FRHIShaderHandle& outHandle) override;
    void DestroyShader(FRHIShaderHandle& handle) override;

    // ====================================================================
    // IRHIDevice 接口实现 — 渲染通道与帧缓冲
    // ====================================================================

    ERHIResult CreateRenderPass(const FRHIRenderPassDesc& desc,
                                 FRHIRenderPassHandle& outHandle) override;
    void DestroyRenderPass(FRHIRenderPassHandle& handle) override;
    ERHIResult CreateFramebuffer(const FRHIFramebufferDesc& desc,
                                  FRHIFramebufferHandle& outHandle) override;
    void DestroyFramebuffer(FRHIFramebufferHandle& handle) override;

    // ====================================================================
    // IRHIDevice 接口实现 — 描述符布局与管线布局
    // ====================================================================

    ERHIResult CreateDescSetLayout(const FRHIDescSetLayoutDesc& desc,
                                    FRHIDescSetLayoutHandle& outHandle) override;
    void DestroyDescSetLayout(FRHIDescSetLayoutHandle& handle) override;
    ERHIResult CreatePipelineLayout(const FRHIPipelineLayoutDesc& desc,
                                     FRHIPipelineLayoutHandle& outHandle) override;
    void DestroyPipelineLayout(FRHIPipelineLayoutHandle& handle) override;

    // ====================================================================
    // IRHIDevice 接口实现 — 管线状态对象
    // ====================================================================

    ERHIResult CreateGraphicsPipeline(const FRHIGraphicsPipelineDesc& desc,
                                       FRHIGraphicsPipelineHandle& outHandle) override;
    void DestroyGraphicsPipeline(FRHIGraphicsPipelineHandle& handle) override;
    ERHIResult CreateComputePipeline(const FRHIComputePipelineDesc& desc,
                                      FRHIComputePipelineHandle& outHandle) override;
    void DestroyComputePipeline(FRHIComputePipelineHandle& handle) override;

    // ====================================================================
    // IRHIDevice 接口实现 — 描述符集
    // ====================================================================

    ERHIResult AllocateDescriptorSet(FRHIDescSetLayoutHandle layout,
                                      FRHIDescriptorSetHandle& outHandle) override;
    void FreeDescriptorSet(FRHIDescriptorSetHandle& handle) override;
    void UpdateDescriptorSets(const FRHIDescriptorWrite* writes,
                               UInt32 writeCount) override;

    // ====================================================================
    // IRHIDevice 接口实现 — 同步原语
    // ====================================================================

    ERHIResult CreateFence(bool isSignaled,
                            FRHIFenceHandle& outHandle) override;
    void DestroyFence(FRHIFenceHandle& handle) override;
    ERHIResult WaitForFence(FRHIFenceHandle handle,
                             UInt64 timeoutNanoseconds) override;
    ERHIResult ResetFence(FRHIFenceHandle handle) override;
    ERHIResult CreateSemaphore(FRHISemaphoreHandle& outHandle) override;
    void DestroySemaphore(FRHISemaphoreHandle& handle) override;

    // ====================================================================
    // IRHIDevice 接口实现 — 命令池与命令缓冲区
    // ====================================================================

    ERHIResult CreateCommandPool(EQueueType queueType,
                                  FRHICommandPoolHandle& outHandle) override;
    void DestroyCommandPool(FRHICommandPoolHandle& handle) override;
    ERHIResult ResetCommandPool(FRHICommandPoolHandle handle) override;
    ERHIResult AllocateCommandBuffer(FRHICommandPoolHandle pool,
                                      ECommandBufferLevel level,
                                      FRHICommandBufferHandle& outHandle) override;
    void FreeCommandBuffer(FRHICommandBufferHandle& handle) override;

    // ====================================================================
    // IRHIDevice 接口实现 — 交换链
    // ====================================================================

    ERHIResult CreateSwapchain(const FRHISwapchainDesc& desc,
                                FRHISwapchainHandle& outHandle) override;
    void DestroySwapchain(FRHISwapchainHandle& handle) override;
    ERHIResult AcquireNextImage(FRHISwapchainHandle swapchain,
                                 FRHISemaphoreHandle signalSemaphore,
                                 FRHIFenceHandle signalFence,
                                 UInt32& outImageIndex) override;
    UInt32 GetSwapchainImageCount(FRHISwapchainHandle swapchain) override;

    LIMX_NODISCARD FRHIDeviceMemoryStats GetDeviceMemoryStats() const override;
    FRHITextureHandle GetSwapchainImage(
        FRHISwapchainHandle swapchain, UInt32 imageIndex) override;
    FRHITextureViewHandle GetSwapchainImageView(
        FRHISwapchainHandle swapchain, UInt32 imageIndex) override;
    EPixelFormat GetSwapchainFormat(FRHISwapchainHandle swapchain) override;
    FRHIExtent2D GetSwapchainExtent(FRHISwapchainHandle swapchain) override;

    // ====================================================================
    // IRHIDevice 接口实现 — 查询池
    // ====================================================================

    ERHIResult CreateQueryPool(const FRHIQueryPoolDesc& desc,
                                FRHIQueryPoolHandle& outHandle) override;
    void DestroyQueryPool(FRHIQueryPoolHandle& handle) override;
    ERHIResult GetQueryResults(FRHIQueryPoolHandle handle,
                                UInt32 firstQuery,
                                UInt32 queryCount,
                                UInt64* outResults,
                                bool wait) override;
    Float32 GetTimestampPeriod() const override;
    UInt32 GetTimestampValidBits() const override;

    // ====================================================================
    // IRHIDevice 接口实现 — 命令提交与呈现
    // ====================================================================

    ERHIResult Submit(EQueueType queue,
                       const FRHISubmitInfo& submitInfo,
                       FRHIFenceHandle signalFence) override;
    ERHIResult Present(const FRHIPresentInfo& presentInfo) override;
    ERHIResult WaitIdle() override;

    // ====================================================================
    // IRHIDevice 接口实现 — 设备信息查询
    // ====================================================================

    const char* GetDeviceName() const override;
    const char* GetDeviceVendor() const override;
    UInt64 GetDedicatedVideoMemory() const override;
    UInt32 GetMaxTextureSize() const override;
    UInt32 GetMaxPushConstantSize() const override;
    Float32 GetMaxAnisotropy() const override;
    EFormatFeature GetFormatFeatures(EPixelFormat format) const override;
    bool IsRayTracingSupported() const override;
    bool IsMeshShaderSupported() const override;

    // ====================================================================
    // Vulkan 内部访问 (供 FVulkanCommandBuffer 使用)
    // ====================================================================

    /// 通过句柄获取 VkBuffer
    LIMX_NODISCARD VkBuffer GetVkBuffer(FRHIBufferHandle handle) const;

    /// 通过句柄获取 VkImage
    LIMX_NODISCARD VkImage GetVkImage(FRHITextureHandle handle) const;

    /// 通过句柄获取纹理的 RHI 格式
    LIMX_NODISCARD EPixelFormat GetTextureFormat(
        FRHITextureHandle handle) const;

    /// 通过句柄获取 VkImageView
    LIMX_NODISCARD VkImageView GetVkImageView(
        FRHITextureViewHandle handle) const;

    /// 通过句柄获取 VkSampler
    LIMX_NODISCARD VkSampler GetVkSampler(
        FRHISamplerHandle handle) const;

    /// 通过句柄获取 VkRenderPass
    LIMX_NODISCARD VkRenderPass GetVkRenderPass(
        FRHIRenderPassHandle handle) const;

    /// 通过句柄获取 VkFramebuffer
    LIMX_NODISCARD VkFramebuffer GetVkFramebuffer(
        FRHIFramebufferHandle handle) const;

    /// 通过句柄获取 VkPipeline (图形)
    LIMX_NODISCARD VkPipeline GetVkGraphicsPipeline(
        FRHIGraphicsPipelineHandle handle) const;

    /// 通过句柄获取 VkPipeline (计算)
    LIMX_NODISCARD VkPipeline GetVkComputePipeline(
        FRHIComputePipelineHandle handle) const;

    /// 通过句柄获取 VkPipelineLayout
    LIMX_NODISCARD VkPipelineLayout GetVkPipelineLayout(
        FRHIPipelineLayoutHandle handle) const;

    /// 通过句柄获取 VkDescriptorSet
    LIMX_NODISCARD VkDescriptorSet GetVkDescriptorSet(
        FRHIDescriptorSetHandle handle) const;

    /// 通过句柄获取 VkCommandBuffer
    LIMX_NODISCARD VkCommandBuffer GetVkCommandBuffer(
        FRHICommandBufferHandle handle) const;

    /// 通过句柄获取 VkFence
    LIMX_NODISCARD VkFence GetVkFence(FRHIFenceHandle handle) const;

    /// 通过句柄获取 VkSemaphore
    LIMX_NODISCARD VkSemaphore GetVkSemaphore(
        FRHISemaphoreHandle handle) const;

    /// 通过句柄获取 VkQueryPool
    LIMX_NODISCARD VkQueryPool GetVkQueryPool(
        FRHIQueryPoolHandle handle) const;

    /// 获取 VkDevice 原生句柄
    LIMX_NODISCARD VkDevice GetVkDevice() const { return m_Device; }

private:
    // ====================================================================
    // 初始化辅助方法
    // ====================================================================

    ERHIResult CreateInstance();
    ERHIResult CreateDebugMessenger();
    ERHIResult CreateSurface(void* nativeWindowHandle);
    ERHIResult SelectPhysicalDevice();
    ERHIResult CreateLogicalDevice();
    ERHIResult CreateDescriptorPool();

    // ====================================================================
    // 内部辅助方法
    // ====================================================================

    /// 查找满足类型过滤器和属性要求的内存类型索引
    UInt32 FindMemoryType(UInt32 typeFilter,
                          VkMemoryPropertyFlags properties) const;

    /// 按队列类型获取 VkQueue
    VkQueue GetVkQueue(EQueueType type) const;

    /// 按队列类型获取队列族索引
    UInt32 GetQueueFamilyIndex(EQueueType type) const;

    // ====================================================================
    // Vulkan 核心对象
    // ====================================================================

    VkInstance                 m_Instance        = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT   m_DebugMessenger  = VK_NULL_HANDLE;
    VkPhysicalDevice           m_PhysicalDevice  = VK_NULL_HANDLE;
    VkDevice                   m_Device          = VK_NULL_HANDLE;
    VkSurfaceKHR               m_Surface         = VK_NULL_HANDLE;

    // ====================================================================
    // 队列
    // ====================================================================

    VkQueue m_GraphicsQueue = VK_NULL_HANDLE;
    VkQueue m_ComputeQueue  = VK_NULL_HANDLE;
    VkQueue m_TransferQueue = VK_NULL_HANDLE;
    VkQueue m_PresentQueue  = VK_NULL_HANDLE;

    UInt32 m_GraphicsQueueFamily = 0xFFFFFFFF;
    UInt32 m_ComputeQueueFamily  = 0xFFFFFFFF;
    UInt32 m_TransferQueueFamily = 0xFFFFFFFF;
    UInt32 m_PresentQueueFamily  = 0xFFFFFFFF;

    // ====================================================================
    // 设备属性
    // ====================================================================

    VkPhysicalDeviceProperties       m_DeviceProperties   = {};

    /// 管线缓存 — 跨进程持久化到磁盘
    ///
    /// 驱动把编译好的管线状态存进去, 下次启动直接复用, 省掉重新编译
    /// 着色器的时间。缓存内容与驱动版本、设备强绑定, 换了显卡或更新了
    /// 驱动就会失效 —— Vulkan 在头部记了 UUID, 不匹配时驱动自己会忽略,
    /// 不需要我们判断。
    VkPipelineCache                  m_PipelineCache = VK_NULL_HANDLE;

    /// 缓存文件路径
    FString                          m_PipelineCachePath;

    /// 累计的图形管线创建耗时 (毫秒) 与条数 — 用来量缓存的收益
    Float64                          m_PipelineCreateMs    = 0.0;
    UInt32                           m_PipelineCreateCount = 0;

    /// 从磁盘装载管线缓存
    void LoadPipelineCache();

    /// 把管线缓存写回磁盘
    void SavePipelineCache();

    /// 图形队列的时间戳有效位数 (0 = 该队列不支持时间戳)
    ///
    /// 这是**队列族**属性而非设备属性 —— 同一块卡上不同队列族的位数可以
    /// 不同, 甚至有的队列族完全不支持时间戳。
    UInt32                           m_TimestampValidBits = 0;
    VkPhysicalDeviceFeatures         m_DeviceFeatures     = {};
    VkPhysicalDeviceMemoryProperties m_MemoryProperties   = {};

    // ====================================================================
    // 核心版本特性 — 由 vkGetPhysicalDeviceFeatures2 查询填充
    //
    // 设备创建时只启用这里查询到为 VK_TRUE 的特性，避免在不支持
    // 该特性的 GPU 上导致 vkCreateDevice 失败。
    // ====================================================================

    VkPhysicalDeviceVulkan11Features m_DeviceFeatures11 = {};
    VkPhysicalDeviceVulkan12Features m_DeviceFeatures12 = {};
    VkPhysicalDeviceVulkan13Features m_DeviceFeatures13 = {};
    VkPhysicalDeviceVulkan14Features m_DeviceFeatures14 = {};

    /// 实际协商出的 Vulkan API 版本 (min(实例支持, 设备支持, 引擎目标))
    UInt32 m_ApiVersion = 0;

    // ====================================================================
    // 显存分配器
    //
    // 所有缓冲区与纹理的显存都经它供应。逐资源调用 vkAllocateMemory 会在
    // 资源数逼近 maxMemoryAllocationCount (通常 4096) 时直接失败, 分配器
    // 以大块子分配把设备分配次数与资源数量解耦。
    // ====================================================================

    FVulkanMemoryAllocator m_MemoryAllocator;

    // ====================================================================
    // 全局描述符池
    // ====================================================================

    VkDescriptorPool m_DescriptorPool = VK_NULL_HANDLE;

    // ====================================================================
    // 配置
    // ====================================================================

    bool m_IsValidationEnabled = false;

    // 同步验证 —— 验证层里对屏障/信号量做符号化建模的那一档。它能确定性地
    // 指出"这次读没有被前面那次写的屏障覆盖", 与 GPU 实际调度无关, 因此是
    // 唯一能把同步缺陷从"偶发数据撕裂"变成"必现报错"的手段。
    // 代价是每条命令都要过一遍模型, 开销显著, 所以默认关闭, 由调用方按需开启。
    bool m_IsSyncValidationEnabled = false;

    // ====================================================================
    // 厂商名称缓存
    // ====================================================================

    char m_VendorName[64] = {};

    // ====================================================================
    // 资源池
    // ====================================================================

    TVulkanResourcePool<FVulkanBufferData,
        RHITags::BufferTag>              m_Buffers;
    TVulkanResourcePool<FVulkanTextureData,
        RHITags::TextureTag>             m_Textures;
    TVulkanResourcePool<FVulkanTextureViewData,
        RHITags::TextureViewTag>         m_TextureViews;
    TVulkanResourcePool<FVulkanSamplerData,
        RHITags::SamplerTag>             m_Samplers;
    TVulkanResourcePool<FVulkanShaderData,
        RHITags::ShaderTag>              m_Shaders;
    TVulkanResourcePool<FVulkanRenderPassData,
        RHITags::RenderPassTag>          m_RenderPasses;
    TVulkanResourcePool<FVulkanFramebufferData,
        RHITags::FramebufferTag>         m_Framebuffers;
    TVulkanResourcePool<FVulkanPipelineLayoutData,
        RHITags::PipelineLayoutTag>      m_PipelineLayouts;
    TVulkanResourcePool<FVulkanDescSetLayoutData,
        RHITags::DescSetLayoutTag>       m_DescSetLayouts;
    TVulkanResourcePool<FVulkanDescriptorSetData,
        RHITags::DescriptorSetTag>       m_DescriptorSets;
    TVulkanResourcePool<FVulkanGraphicsPipelineData,
        RHITags::GraphicsPipelineTag>    m_GraphicsPipelines;
    TVulkanResourcePool<FVulkanComputePipelineData,
        RHITags::ComputePipelineTag>     m_ComputePipelines;
    TVulkanResourcePool<FVulkanFenceData,
        RHITags::FenceTag>               m_Fences;
    TVulkanResourcePool<FVulkanSemaphoreData,
        RHITags::SemaphoreTag>           m_Semaphores;
    TVulkanResourcePool<FVulkanCommandPoolData,
        RHITags::CommandPoolTag>         m_CommandPools;
    TVulkanResourcePool<FVulkanCommandBufferData,
        RHITags::CommandBufferTag>       m_CommandBuffers;
    TVulkanResourcePool<FVulkanSwapchainData,
        RHITags::SwapchainTag>           m_Swapchains;
    TVulkanResourcePool<FVulkanQueryPoolData,
        RHITags::QueryPoolTag>           m_QueryPools;
};

} // namespace Limx
