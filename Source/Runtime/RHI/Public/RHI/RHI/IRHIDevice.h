/*******************************************************************************
 * 文件名称：IRHIDevice.h
 * 创建时间：2025-07-27
 * 创建者  ：LimxTeam
 * 设计哲学：纯虚接口隔离后端实现，上层渲染代码仅依赖此接口，不感知具体
 *          图形 API (Vulkan/D3D12)。资源创建/销毁统一通过句柄管理，
 *          避免裸指针泄漏。显式 Acquire/Release 语义确保资源生命周期可审计。
 * 功能描述：RHI 设备抽象接口 — 定义 GPU 逻辑设备的核心操作契约，包括
 *          资源创建与销毁、命令提交、同步原语操作、交换链管理、
 *          设备信息查询等。所有 GPU 资源的生命周期起点。
 * 技术特性：纯虚接口 (无数据成员)，支持多后端实现；
 *          所有创建方法返回类型化句柄，销毁方法接受句柄引用并自动失效；
 *          WaitIdle 保证设备空闲后安全销毁资源。
 *
 * ── 函数/方法表 ──────────────────────────────────────────────
 * │ 函数名                         │ 描述                        │
 * │───────────────────────────────│───────────────────────────│
 * │ CreateBuffer()                │ 创建 GPU 缓冲区              │
 * │ DestroyBuffer()               │ 销毁 GPU 缓冲区              │
 * │ MapBuffer()                   │ 映射缓冲区到 CPU 地址空间      │
 * │ UnmapBuffer()                 │ 取消缓冲区映射                │
 * │ CreateTexture()               │ 创建 GPU 纹理                │
 * │ DestroyTexture()              │ 销毁 GPU 纹理                │
 * │ CreateTextureView()           │ 创建纹理视图                  │
 * │ DestroyTextureView()          │ 销毁纹理视图                  │
 * │ CreateSampler()               │ 创建采样器                   │
 * │ DestroySampler()              │ 销毁采样器                   │
 * │ CreateShader()                │ 创建着色器模块                │
 * │ DestroyShader()               │ 销毁着色器模块                │
 * │ CreateRenderPass()            │ 创建渲染通道                  │
 * │ DestroyRenderPass()           │ 销毁渲染通道                  │
 * │ CreateFramebuffer()           │ 创建帧缓冲                   │
 * │ DestroyFramebuffer()          │ 销毁帧缓冲                   │
 * │ CreateDescSetLayout()         │ 创建描述符集布局               │
 * │ DestroyDescSetLayout()        │ 销毁描述符集布局               │
 * │ CreatePipelineLayout()        │ 创建管线布局                  │
 * │ DestroyPipelineLayout()       │ 销毁管线布局                  │
 * │ CreateGraphicsPipeline()      │ 创建图形管线                  │
 * │ DestroyGraphicsPipeline()     │ 销毁图形管线                  │
 * │ CreateComputePipeline()       │ 创建计算管线                  │
 * │ DestroyComputePipeline()      │ 销毁计算管线                  │
 * │ AllocateDescriptorSet()       │ 分配描述符集                  │
 * │ FreeDescriptorSet()           │ 释放描述符集                  │
 * │ CreateFence()                 │ 创建栅栏                     │
 * │ DestroyFence()                │ 销毁栅栏                     │
 * │ WaitForFence()                │ 等待栅栏信号                  │
 * │ ResetFence()                  │ 重置栅栏                     │
 * │ CreateSemaphore()             │ 创建信号量                   │
 * │ DestroySemaphore()            │ 销毁信号量                   │
 * │ CreateCommandPool()           │ 创建命令池                   │
 * │ DestroyCommandPool()          │ 销毁命令池                   │
 * │ AllocateCommandBuffer()       │ 分配命令缓冲区                │
 * │ FreeCommandBuffer()           │ 释放命令缓冲区                │
 * │ CreateSwapchain()             │ 创建交换链                   │
 * │ DestroySwapchain()            │ 销毁交换链                   │
 * │ AcquireNextImage()            │ 获取交换链下一帧图像索引        │
 * │ Present()                     │ 提交帧到交换链呈现             │
 * │ Submit()                      │ 提交命令缓冲区到队列           │
 * │ WaitIdle()                    │ 等待设备空闲                  │
 * │ GetDeviceName()               │ 获取 GPU 设备名称             │
 * │ GetDeviceVendor()             │ 获取 GPU 厂商名称             │
 * │ GetDedicatedVideoMemory()     │ 获取独显显存大小 (字节)        │
 *
 * ── 更新历史 ────────────────────────────────────────────────
 * │ 日期         │ 作者       │ 描述                        │
 * │─────────────│──────────│───────────────────────────│
 * │ 2025-07-27  │ LimxTeam  │ 初始创建                     │
 * ============================================================
 ******************************************************************************/

#pragma once

#include "RHI/RHI/RHIPipelineState.h"

namespace Limx
{

// ============================================================================
// FRHISubmitInfo — 命令提交信息
// ============================================================================

struct FRHISubmitInfo
{
    // 等待的信号量
    const FRHISemaphoreHandle*    WaitSemaphores    = nullptr;
    const EPipelineStageFlags*    WaitStages        = nullptr;
    UInt32                        WaitSemaphoreCount = 0;

    // 要提交的命令缓冲区
    const FRHICommandBufferHandle* CommandBuffers    = nullptr;
    UInt32                         CommandBufferCount = 0;

    // 完成时发出信号的信号量
    const FRHISemaphoreHandle* SignalSemaphores    = nullptr;
    UInt32                     SignalSemaphoreCount = 0;
};

// ============================================================================
// FRHIPresentInfo — 呈现信息
// ============================================================================

struct FRHIPresentInfo
{
    // 等待的信号量 (通常是渲染完成信号量)
    const FRHISemaphoreHandle* WaitSemaphores    = nullptr;
    UInt32                     WaitSemaphoreCount = 0;

    // 交换链
    FRHISwapchainHandle Swapchain;

    // 图像索引 (由 AcquireNextImage 返回)
    UInt32 ImageIndex = 0;
};

// ============================================================================
// ERHIResult — RHI 操作结果码
// ============================================================================

enum class ERHIResult : Int32
{
    Success              =  0,
    NotReady             =  1,
    Timeout              =  2,
    SuboptimalSwapchain  =  3,

    ErrorUnknown         = -1,
    ErrorOutOfHostMemory = -2,
    ErrorOutOfDeviceMemory = -3,
    ErrorDeviceLost      = -4,
    ErrorSurfaceLost     = -5,
    ErrorOutOfDate       = -6,
    ErrorInvalidHandle   = -7,
    ErrorInvalidParameter = -8,
    ErrorShaderCompilation = -9,
    ErrorIncompatibleDriver = -10,
};

// 判断结果是否成功
//
// 注意 NotReady / Timeout / SuboptimalSwapchain 都是非负值, 因此都算"成功"。
// 需要区分"拿到结果"与"还没好"时, 必须显式比较 ERHIResult::Success ——
// 用 IsRHISuccess 会把 NotReady 当成拿到了结果, 而那时输出缓冲里是旧数据。
inline constexpr bool IsRHISuccess(ERHIResult result)
{
    return static_cast<Int32>(result) >= 0;
}

// ============================================================================
// 时间戳差值计算
// ============================================================================

/// 计算两个原始时间戳之间的 tick 差
///
/// @param begin     起点原始值 (可能含未定义高位)
/// @param end       终点原始值
/// @param validMask 有效位掩码, 由有效位数构造
///
/// 两件事必须在这里处理, 而且都只在极少数情况下才显形:
///
///   高位未定义 —— 有效位数少于 64 时, 时间戳的高位内容是未规定的。
///     不掩掉就做差, 得到的不是"略有偏差"而是完全随机的数。
///
///   计数器回绕 —— 掩码之后仍可能 end < begin。此时按模运算补回一个周期。
///     返回 0 会谎报成"这段不耗时"; 取绝对值会得到接近整个计数器周期的
///     巨大值。两种都比正确答案更难被发现, 因为它们看起来像是"真的测到了"。
///
/// 提成自由函数是为了能脱离 GPU 测试 —— 真实硬件上等一次回绕需要数百年。
LIMX_NODISCARD inline UInt64 ComputeTimestampDelta(UInt64 begin,
                                                   UInt64 end,
                                                   UInt64 validMask)
{
    const UInt64 maskedBegin = begin & validMask;
    const UInt64 maskedEnd   = end   & validMask;

    if (maskedEnd >= maskedBegin)
    {
        return maskedEnd - maskedBegin;
    }

    return validMask - maskedBegin + maskedEnd + 1;
}

/// 由有效位数构造掩码
///
/// 位数为 0 时返回 0 —— 那表示该队列不支持时间戳, 任何差值都无意义。
/// 位数 >= 64 时返回全 1; 直接写 (1 << 64) - 1 是未定义行为。
LIMX_NODISCARD inline UInt64 MakeTimestampMask(UInt32 validBits)
{
    if (validBits == 0)
    {
        return 0;
    }

    if (validBits >= 64)
    {
        return ~static_cast<UInt64>(0);
    }

    return (static_cast<UInt64>(1) << validBits) - 1;
}

// ============================================================================
// IRHIDevice — RHI 设备抽象接口
// ============================================================================

class IRHIDevice
{
public:
    virtual ~IRHIDevice() = default;

    // ====================================================================
    // 缓冲区
    // ====================================================================

    virtual ERHIResult CreateBuffer(const FRHIBufferDesc& desc,
                                    FRHIBufferHandle& outHandle) = 0;
    virtual void DestroyBuffer(FRHIBufferHandle& handle) = 0;

    // 映射缓冲区到 CPU 可访问地址 (仅 CpuToGpu/CpuOnly/GpuToCpu)
    virtual ERHIResult MapBuffer(FRHIBufferHandle handle,
                                  void** outMappedPtr) = 0;
    virtual void UnmapBuffer(FRHIBufferHandle handle) = 0;

    // ====================================================================
    // 纹理
    // ====================================================================

    virtual ERHIResult CreateTexture(const FRHITextureDesc& desc,
                                     FRHITextureHandle& outHandle) = 0;
    virtual void DestroyTexture(FRHITextureHandle& handle) = 0;

    virtual ERHIResult CreateTextureView(const FRHITextureViewDesc& desc,
                                          FRHITextureViewHandle& outHandle) = 0;
    virtual void DestroyTextureView(FRHITextureViewHandle& handle) = 0;

    // ====================================================================
    // 采样器
    // ====================================================================

    virtual ERHIResult CreateSampler(const FRHISamplerDesc& desc,
                                      FRHISamplerHandle& outHandle) = 0;
    virtual void DestroySampler(FRHISamplerHandle& handle) = 0;

    // ====================================================================
    // 着色器
    // ====================================================================

    virtual ERHIResult CreateShader(const FRHIShaderDesc& desc,
                                     FRHIShaderHandle& outHandle) = 0;
    virtual void DestroyShader(FRHIShaderHandle& handle) = 0;

    // ====================================================================
    // 渲染通道与帧缓冲
    // ====================================================================

    virtual ERHIResult CreateRenderPass(const FRHIRenderPassDesc& desc,
                                         FRHIRenderPassHandle& outHandle) = 0;
    virtual void DestroyRenderPass(FRHIRenderPassHandle& handle) = 0;

    virtual ERHIResult CreateFramebuffer(const FRHIFramebufferDesc& desc,
                                          FRHIFramebufferHandle& outHandle) = 0;
    virtual void DestroyFramebuffer(FRHIFramebufferHandle& handle) = 0;

    // ====================================================================
    // 描述符布局与管线布局
    // ====================================================================

    virtual ERHIResult CreateDescSetLayout(const FRHIDescSetLayoutDesc& desc,
                                            FRHIDescSetLayoutHandle& outHandle) = 0;
    virtual void DestroyDescSetLayout(FRHIDescSetLayoutHandle& handle) = 0;

    virtual ERHIResult CreatePipelineLayout(const FRHIPipelineLayoutDesc& desc,
                                             FRHIPipelineLayoutHandle& outHandle) = 0;
    virtual void DestroyPipelineLayout(FRHIPipelineLayoutHandle& handle) = 0;

    // ====================================================================
    // 管线状态对象
    // ====================================================================

    virtual ERHIResult CreateGraphicsPipeline(const FRHIGraphicsPipelineDesc& desc,
                                               FRHIGraphicsPipelineHandle& outHandle) = 0;
    virtual void DestroyGraphicsPipeline(FRHIGraphicsPipelineHandle& handle) = 0;

    virtual ERHIResult CreateComputePipeline(const FRHIComputePipelineDesc& desc,
                                              FRHIComputePipelineHandle& outHandle) = 0;
    virtual void DestroyComputePipeline(FRHIComputePipelineHandle& handle) = 0;

    // ====================================================================
    // 描述符集
    // ====================================================================

    virtual ERHIResult AllocateDescriptorSet(FRHIDescSetLayoutHandle layout,
                                              FRHIDescriptorSetHandle& outHandle) = 0;
    virtual void FreeDescriptorSet(FRHIDescriptorSetHandle& handle) = 0;

    // 批量写入描述符集绑定 (缓冲区/纹理/采样器)
    virtual void UpdateDescriptorSets(const FRHIDescriptorWrite* writes,
                                       UInt32 writeCount) = 0;

    // ====================================================================
    // 同步原语
    // ====================================================================

    virtual ERHIResult CreateFence(bool isSignaled,
                                    FRHIFenceHandle& outHandle) = 0;
    virtual void DestroyFence(FRHIFenceHandle& handle) = 0;
    virtual ERHIResult WaitForFence(FRHIFenceHandle handle,
                                     UInt64 timeoutNanoseconds = 0xFFFFFFFFFFFFFFFFULL) = 0;
    virtual ERHIResult ResetFence(FRHIFenceHandle handle) = 0;

    virtual ERHIResult CreateSemaphore(FRHISemaphoreHandle& outHandle) = 0;
    virtual void DestroySemaphore(FRHISemaphoreHandle& handle) = 0;

    // ====================================================================
    // 命令池与命令缓冲区
    // ====================================================================

    virtual ERHIResult CreateCommandPool(EQueueType queueType,
                                          FRHICommandPoolHandle& outHandle) = 0;
    virtual void DestroyCommandPool(FRHICommandPoolHandle& handle) = 0;
    virtual ERHIResult ResetCommandPool(FRHICommandPoolHandle handle) = 0;

    /// 分配命令缓冲区
    ///
    /// @param level Primary 直接提交; Secondary 只能被主缓冲执行
    virtual ERHIResult AllocateCommandBuffer(FRHICommandPoolHandle pool,
                                              ECommandBufferLevel level,
                                              FRHICommandBufferHandle& outHandle) = 0;
    virtual void FreeCommandBuffer(FRHICommandBufferHandle& handle) = 0;

    // ====================================================================
    // 交换链
    // ====================================================================

    virtual ERHIResult CreateSwapchain(const FRHISwapchainDesc& desc,
                                        FRHISwapchainHandle& outHandle) = 0;
    virtual void DestroySwapchain(FRHISwapchainHandle& handle) = 0;

    // 获取交换链下一帧图像索引
    virtual ERHIResult AcquireNextImage(FRHISwapchainHandle swapchain,
                                         FRHISemaphoreHandle signalSemaphore,
                                         FRHIFenceHandle signalFence,
                                         UInt32& outImageIndex) = 0;

    // 获取交换链图像数量
    virtual UInt32 GetSwapchainImageCount(FRHISwapchainHandle swapchain) = 0;

    // ====================================================================
    // 显存统计
    // ====================================================================

    /// 查询分配器视角的显存占用
    ///
    /// 这是唯一一个不会漏算的口径: 任何走 RHI 创建的资源都在其中, 无需
    /// 每个子系统各自记账。子系统各自记账的做法迟早会漏 —— 新加一类资源
    /// 时忘了登记, 报出来的数字就悄悄偏小了。
    LIMX_NODISCARD virtual FRHIDeviceMemoryStats GetDeviceMemoryStats() const = 0;

    // 获取交换链图像纹理句柄
    virtual FRHITextureHandle GetSwapchainImage(
        FRHISwapchainHandle swapchain, UInt32 imageIndex) = 0;

    // 获取交换链图像视图
    virtual FRHITextureViewHandle GetSwapchainImageView(
        FRHISwapchainHandle swapchain, UInt32 imageIndex) = 0;

    // 获取交换链格式
    virtual EPixelFormat GetSwapchainFormat(FRHISwapchainHandle swapchain) = 0;

    // 获取交换链尺寸
    virtual FRHIExtent2D GetSwapchainExtent(FRHISwapchainHandle swapchain) = 0;

    // ====================================================================
    // 查询池
    // ====================================================================

    virtual ERHIResult CreateQueryPool(const FRHIQueryPoolDesc& desc,
                                        FRHIQueryPoolHandle& outHandle) = 0;
    virtual void DestroyQueryPool(FRHIQueryPoolHandle& handle) = 0;

    /// 回读查询结果
    ///
    /// @param handle     查询池
    /// @param firstQuery 起始下标
    /// @param queryCount 个数
    /// @param outResults 输出缓冲, 至少 queryCount 个 UInt64
    /// @param wait       true 则阻塞直到结果就绪; false 时未就绪返回 NotReady
    ///
    /// 时间戳的单位是设备 tick, 乘 GetTimestampPeriod() 得到纳秒。
    ///
    /// wait=true 会让 CPU 等 GPU —— 逐帧计时应当传 false 并读若干帧之前的
    /// 那一组, 否则量到的是"等待 GPU"而不是"GPU 干活"。
    virtual ERHIResult GetQueryResults(FRHIQueryPoolHandle handle,
                                        UInt32 firstQuery,
                                        UInt32 queryCount,
                                        UInt64* outResults,
                                        bool wait) = 0;

    /// 时间戳一个 tick 对应多少纳秒
    ///
    /// 各家硬件差别很大 (NVIDIA 通常是 1.0, AMD 常见 40 左右), 因此绝不能
    /// 假定 tick 就是纳秒。返回 0 表示该队列不支持时间戳。
    virtual Float32 GetTimestampPeriod() const = 0;

    /// 时间戳的有效位数
    ///
    /// 少于 64 位时高位是未定义的, 必须先掩掉再做差 —— 否则跨越回绕点的
    /// 那一帧会得到一个巨大的负值 (以无符号解释则是天文数字)。
    virtual UInt32 GetTimestampValidBits() const = 0;

    // ====================================================================
    // 命令提交与呈现
    // ====================================================================

    // 提交命令到 GPU 队列
    virtual ERHIResult Submit(EQueueType queue,
                               const FRHISubmitInfo& submitInfo,
                               FRHIFenceHandle signalFence = FRHIFenceHandle()) = 0;

    // 呈现帧到显示器
    virtual ERHIResult Present(const FRHIPresentInfo& presentInfo) = 0;

    // ====================================================================
    // 设备同步
    // ====================================================================

    // 等待设备所有队列空闲 (用于安全销毁资源)
    virtual ERHIResult WaitIdle() = 0;

    // ====================================================================
    // 设备信息查询
    // ====================================================================

    // GPU 设备名称
    virtual const char* GetDeviceName() const = 0;

    // GPU 厂商名称
    virtual const char* GetDeviceVendor() const = 0;

    // 独显显存大小 (字节)
    virtual UInt64 GetDedicatedVideoMemory() const = 0;

    // 支持的最大纹理尺寸
    virtual UInt32 GetMaxTextureSize() const = 0;

    // 支持的最大 Push Constant 大小 (字节)
    virtual UInt32 GetMaxPushConstantSize() const = 0;

    // 支持的最大各向异性等级
    virtual Float32 GetMaxAnisotropy() const = 0;

    // 查询像素格式在最优平铺 (Optimal Tiling) 下支持的能力
    //
    // mip 链生成、后处理的浮点渲染目标、压缩纹理直传, 都依赖具体格式在
    // 具体设备上的能力。这些能力**不是普遍保证的**, 必须查询而非假定。
    virtual EFormatFeature GetFormatFeatures(EPixelFormat format) const = 0;

    // 是否支持光线追踪
    virtual bool IsRayTracingSupported() const = 0;

    // 是否支持 Mesh Shader
    virtual bool IsMeshShaderSupported() const = 0;

protected:
    IRHIDevice() = default;
    LIMX_NON_COPYABLE(IRHIDevice);
    LIMX_NON_MOVABLE(IRHIDevice);
};

} // namespace Limx
