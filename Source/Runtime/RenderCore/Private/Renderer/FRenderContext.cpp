// ============================================================
// 文件名称：FRenderContext.cpp
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：RAII + 显式生命周期 — 所有 GPU 资源在 Shutdown 中
//          按依赖逆序释放，析构函数作为安全网兜底。
// 功能描述：FRenderContext 完整实现 — 设备创建、交换链管理、
//          帧同步资源分配、BeginFrame/EndFrame 帧循环。
// 技术特性：N 帧并行 (Frames-in-Flight) 通过独立栅栏/信号量
//          实现 CPU-GPU 流水线并行; 交换链过期自动重建。
//
// ── 函数表 ──────────────────────────────────────────────────
// │ 函数名                      │ 描述                           │
// │────────────────────────────│───────────────────────────────│
// │ Initialize()               │ 设备→交换链→帧资源 完整初始化     │
// │ Shutdown()                 │ WaitIdle→帧资源→交换链→设备 释放  │
// │ BeginFrame()               │ WaitFence→AcquireImage→ResetCmd │
// │ EndFrame()                 │ Submit→Present→AdvanceFrame     │
// │ RecreateSwapchain()        │ WaitIdle→销毁旧链→创建新链        │
// │ CreateFrameResources()     │ 为每帧分配同步原语和命令缓冲区     │
// │ DestroyFrameResources()    │ 释放全部帧资源                   │
// │ CreateSwapchain()          │ 调用 IRHIDevice::CreateSwapchain │
// │ DestroySwapchain()         │ 调用 IRHIDevice::DestroySwapchain│
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-06  │ LimxTeam  │ 初始创建                        │
// ============================================================

#include "RenderCore/Renderer/FRenderContext.h"
#include "ApplicationCore/Window/FWindow.h"

namespace Limx
{

// 日志分类
LIMX_DECLARE_LOG_CATEGORY(LogRenderer)
LIMX_DEFINE_LOG_CATEGORY(LogRenderer)

// ============================================================================
// 析构
// ============================================================================

FRenderContext::~FRenderContext()
{
    Shutdown();
}

// ============================================================================
// Initialize — 完整初始化链路
// ============================================================================

ERHIResult FRenderContext::Initialize(const FRenderContextDesc& desc)
{
    LIMX_CHECK(desc.Window != nullptr && desc.Window->IsValid());

    m_Window             = desc.Window;
    m_MaxFramesInFlight  = desc.MaxFramesInFlight;
    m_IsVSyncEnabled     = desc.IsVSyncEnabled;

    // 创建 RHI 设备 (内部完成 VkInstance→Surface→PhysicalDevice→LogicalDevice)
    m_Device = CreateRHIDevice(m_Window->GetNativeHandle(),
                                desc.EnableValidation);
    if (!m_Device)
    {
        LIMX_LOG(LogRenderer, Error,
            "[RenderContext] RHI 设备创建失败");
        return ERHIResult::ErrorUnknown;
    }

    // 创建交换链
    ERHIResult result = CreateSwapchain();
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
            "[RenderContext] 交换链创建失败: {}",
            static_cast<Int32>(result));
        return result;
    }

    // 创建帧同步资源
    result = CreateFrameResources();
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
            "[RenderContext] 帧资源创建失败: {}",
            static_cast<Int32>(result));
        return result;
    }

    // 资源管理器最后初始化 —— 它的上传路径依赖一次性命令缓冲区,
    // 而后者需要设备与命令池均已就绪。
    result = m_ResourceManager.Initialize(m_Device.Get(), this);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
            "[RenderContext] 资源管理器初始化失败: {}",
            static_cast<Int32>(result));
        return result;
    }

    LIMX_LOG(LogRenderer, Log,
        "[RenderContext] 初始化完成 — 帧并行数:{} VSync:{}",
        m_MaxFramesInFlight, m_IsVSyncEnabled ? 1 : 0);

    return ERHIResult::Success;
}

// ============================================================================
// Shutdown — 完整关闭链路
// ============================================================================

void FRenderContext::Shutdown()
{
    if (!m_Device)
    {
        return;
    }

    // 等待 GPU 空闲，确保所有提交的命令都已完成
    m_Device->WaitIdle();

    // 资源先于帧资源销毁 —— 上传路径会用到一次性命令缓冲区,
    // 反过来就会在命令池已销毁后再去申请缓冲区。
    m_ResourceManager.Shutdown();

    DestroyFrameResources();
    DestroySwapchain();

    m_Device.Reset();
    m_Window = nullptr;

    LIMX_LOG(LogRenderer, Log, "[RenderContext] 已关闭");
}

// ============================================================================
// BeginFrame — 帧开始
// ============================================================================

ERHIResult FRenderContext::BeginFrame()
{
    LIMX_CHECK(m_Device.Get() != nullptr);

    if (m_Frames.GetSize() == 0 || m_CommandBuffers.GetSize() == 0)
    {
        return ERHIResult::ErrorInvalidHandle;
    }

    FrameData& frame = m_Frames[m_CurrentFrame];

    // 等待当前帧的栅栏 (确保该帧的 GPU 工作已完成)
    ERHIResult result = m_Device->WaitForFence(frame.InFlightFence,
                                                 0xFFFFFFFFFFFFFFFFULL);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
            "[RenderContext] WaitForFence 失败: {}",
            static_cast<Int32>(result));
        return result;
    }

    m_Device->ResetFence(frame.InFlightFence);

    ++m_FrameCounter;

    // 栅栏已通过 —— MaxFramesInFlight 帧之前提交的命令确定已执行完毕,
    // 此刻回收退役资源不会碰到 GPU 仍在读的对象。
    m_ResourceManager.ProcessPendingReleases();

    // 获取下一个交换链图像
    result = m_Device->AcquireNextImage(
        m_Swapchain,
        frame.ImageAvailableSemaphore,
        FRHIFenceHandle(),  // 不使用栅栏信号
        m_CurrentImageIndex);

    if (result == ERHIResult::ErrorOutOfDate)
    {
        // 交换链过期；由 FRenderer 编排 Pass 资源释放和完整重建
        return ERHIResult::ErrorOutOfDate;
    }

    if (result == ERHIResult::SuboptimalSwapchain)
    {
        return ERHIResult::SuboptimalSwapchain;
    }

    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
            "[RenderContext] AcquireNextImage 失败: {}",
            static_cast<Int32>(result));
        return result;
    }

    // 重置并开始命令缓冲区
    IRHICommandBuffer* commandBuffer =
        m_CommandBuffers[m_CurrentFrame].Get();
    commandBuffer->Reset();
    commandBuffer->Begin();

    return ERHIResult::Success;
}

// ============================================================================
// EndFrame — 帧结束
// ============================================================================

ERHIResult FRenderContext::EndFrame()
{
    LIMX_CHECK(m_Device.Get() != nullptr);

    FrameData& frame = m_Frames[m_CurrentFrame];

    // 结束命令缓冲区录制
    IRHICommandBuffer* commandBuffer =
        m_CommandBuffers[m_CurrentFrame].Get();

    commandBuffer->End();

    // 提交命令缓冲区
    FRHISubmitInfo submitInfo = {};

    FRHICommandBufferHandle cmdHandle = frame.CommandBuffer;
    submitInfo.CommandBuffers     = &cmdHandle;
    submitInfo.CommandBufferCount = 1;

    // 等待图像可用信号量 — 在颜色附件输出阶段等待
    EPipelineStageFlags waitStage =
        EPipelineStageFlags::ColorAttachmentOutput;
    submitInfo.WaitSemaphores     = &frame.ImageAvailableSemaphore;
    submitInfo.WaitStages         = &waitStage;
    submitInfo.WaitSemaphoreCount = 1;

    // 发出渲染完成信号量 — 按 swapchain image 索引
    if (m_CurrentImageIndex >= m_RenderFinishedSemaphores.GetSize())
    {
        LIMX_LOG(LogRenderer, Error,
            "[RenderContext] 当前交换链图像索引越界: {} / {}",
            m_CurrentImageIndex, m_RenderFinishedSemaphores.GetSize());
        return ERHIResult::ErrorInvalidHandle;
    }

    FRHISemaphoreHandle renderFinishedSem =
        m_RenderFinishedSemaphores[m_CurrentImageIndex];
    submitInfo.SignalSemaphores     = &renderFinishedSem;
    submitInfo.SignalSemaphoreCount = 1;

    // 渲染提交 — 不附加 fence (fence 需覆盖 Present 完成)
    ERHIResult result = m_Device->Submit(
        EQueueType::Graphics, submitInfo, FRHIFenceHandle());

    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
            "[RenderContext] Submit 失败: {}",
            static_cast<Int32>(result));
        return result;
    }

    // 呈现
    FRHIPresentInfo presentInfo = {};
    presentInfo.WaitSemaphores     = &renderFinishedSem;
    presentInfo.WaitSemaphoreCount = 1;
    presentInfo.Swapchain          = m_Swapchain;
    presentInfo.ImageIndex         = m_CurrentImageIndex;

    ERHIResult presentResult = m_Device->Present(presentInfo);

    if (!IsRHISuccess(presentResult) &&
        presentResult != ERHIResult::ErrorOutOfDate &&
        presentResult != ERHIResult::SuboptimalSwapchain)
    {
        LIMX_LOG(LogRenderer, Error,
            "[RenderContext] Present 失败: {}",
            static_cast<Int32>(presentResult));
    }

    // 空提交 + fence — 确保 fence 在 Present 完成后才被 signal
    // 队列操作按提交顺序执行，此 fence 覆盖前面的 Submit 和 Present
    FRHISubmitInfo emptySubmit = {};
    result = m_Device->Submit(
        EQueueType::Graphics, emptySubmit, frame.InFlightFence);

    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
            "[RenderContext] Fence Submit 失败: {}",
            static_cast<Int32>(result));
        return result;
    }

    // 推进帧索引 (环形)
    m_CurrentFrame = (m_CurrentFrame + 1) % m_MaxFramesInFlight;

    if (presentResult == ERHIResult::ErrorOutOfDate ||
        presentResult == ERHIResult::SuboptimalSwapchain)
    {
        return presentResult;
    }

    return IsRHISuccess(presentResult)
        ? ERHIResult::Success
        : presentResult;
}

// ============================================================================
// RecreateSwapchain — 交换链重建
// ============================================================================

ERHIResult FRenderContext::RecreateSwapchain()
{
    LIMX_CHECK(m_Device.Get() != nullptr);

    // 等待 GPU 空闲
    m_Device->WaitIdle();

    // 交换链图像数量可能变化，帧同步资源和 per-image 信号量必须一起重建
    DestroyFrameResources();

    // 销毁旧交换链
    DestroySwapchain();

    // 创建新交换链
    ERHIResult result = CreateSwapchain();
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
            "[RenderContext] 交换链重建失败: {}",
            static_cast<Int32>(result));
        return result;
    }

    m_CurrentFrame      = 0;
    m_CurrentImageIndex = 0;

    result = CreateFrameResources();
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
            "[RenderContext] 交换链重建后帧资源创建失败: {}",
            static_cast<Int32>(result));
        return result;
    }

    LIMX_LOG(LogRenderer, Log,
        "[RenderContext] 交换链重建完成");

    return ERHIResult::Success;
}

// ============================================================================
// GetCurrentCommandBuffer
// ============================================================================

IRHICommandBuffer* FRenderContext::GetCurrentCommandBuffer() const
{
    if (m_CommandBuffers.GetSize() == 0)
    {
        return nullptr;
    }
    return m_CommandBuffers[m_CurrentFrame].Get();
}

// ============================================================================
// GetSwapchainExtent
// ============================================================================

FRHIExtent2D FRenderContext::GetSwapchainExtent() const
{
    if (m_Device && m_Swapchain.IsValid())
    {
        return m_Device->GetSwapchainExtent(m_Swapchain);
    }
    return FRHIExtent2D{ 0, 0 };
}

// ============================================================================
// GetSwapchainFormat
// ============================================================================

EPixelFormat FRenderContext::GetSwapchainFormat() const
{
    if (m_Device && m_Swapchain.IsValid())
    {
        return m_Device->GetSwapchainFormat(m_Swapchain);
    }
    return EPixelFormat::Unknown;
}

// ============================================================================
// CreateSwapchain — 内部交换链创建
// ============================================================================

ERHIResult FRenderContext::CreateSwapchain()
{
    FRHISwapchainDesc swapDesc = {};
    swapDesc.Width              = m_Window->GetWidth();
    swapDesc.Height             = m_Window->GetHeight();
    swapDesc.PreferredFormat    = EPixelFormat::BGRA8_SRGB;
    swapDesc.BufferCount        = m_MaxFramesInFlight + 1;
    swapDesc.IsVSyncEnabled     = m_IsVSyncEnabled;
    swapDesc.NativeWindowHandle = m_Window->GetNativeHandle();
    swapDesc.DebugName          = "MainSwapchain";

    return m_Device->CreateSwapchain(swapDesc, m_Swapchain);
}

// ============================================================================
// DestroySwapchain — 内部交换链销毁
// ============================================================================

void FRenderContext::DestroySwapchain()
{
    if (m_Device && m_Swapchain.IsValid())
    {
        m_Device->DestroySwapchain(m_Swapchain);
    }
}

// ============================================================================
// CreateFrameResources — 为每帧分配同步原语和命令缓冲区
// ============================================================================

ERHIResult FRenderContext::CreateFrameResources()
{
    m_Frames.Clear();
    m_CommandBuffers.Clear();

    for (UInt32 i = 0; i < m_MaxFramesInFlight; ++i)
    {
        FrameData frame = {};

        // 创建命令池 (每帧独立，可独立重置)
        ERHIResult result = m_Device->CreateCommandPool(
            EQueueType::Graphics, frame.CommandPool);
        if (!IsRHISuccess(result))
        {
            return result;
        }

        // 分配命令缓冲区
        result = m_Device->AllocateCommandBuffer(
            frame.CommandPool, ECommandBufferLevel::Primary,
            frame.CommandBuffer);
        if (!IsRHISuccess(result))
        {
            return result;
        }

        // 创建同步原语
        result = m_Device->CreateSemaphore(frame.ImageAvailableSemaphore);
        if (!IsRHISuccess(result))
        {
            return result;
        }

        // 创建栅栏 (初始状态为 signaled，首帧 WaitForFence 不会阻塞)
        result = m_Device->CreateFence(true, frame.InFlightFence);
        if (!IsRHISuccess(result))
        {
            return result;
        }

        m_Frames.Add(frame);

        // 创建命令缓冲区包装器
        auto commandBuffer = CreateRHICommandBuffer(
            m_Device.Get(), frame.CommandBuffer);
        m_CommandBuffers.Add(static_cast<TUniquePtr<IRHICommandBuffer>&&>(commandBuffer));
    }

    // 渲染完成信号量 — 按 swapchain image 索引分配
    // 避免 Present 的 swapchain 内部跟踪与帧索引不对齐导致验证错误
    UInt32 imageCount = m_Device->GetSwapchainImageCount(m_Swapchain);
    m_RenderFinishedSemaphores.Clear();

    for (UInt32 i = 0; i < imageCount; ++i)
    {
        FRHISemaphoreHandle semaphore;
        ERHIResult result = m_Device->CreateSemaphore(semaphore);
        if (!IsRHISuccess(result))
        {
            return result;
        }
        m_RenderFinishedSemaphores.Add(semaphore);
    }

    LIMX_LOG(LogRenderer, Log,
        "[RenderContext] 帧资源创建完成 — {} 帧, {} 渲染信号量",
        m_MaxFramesInFlight, imageCount);

    return ERHIResult::Success;
}

// ============================================================================
// DestroyFrameResources — 释放全部帧资源
// ============================================================================

void FRenderContext::DestroyFrameResources()
{
    if (!m_Device)
    {
        return;
    }

    // 先销毁命令缓冲区包装器 (释放对句柄的引用)
    m_CommandBuffers.Clear();

    // 释放 RHI 资源
    for (UInt32 i = 0; i < m_Frames.GetSize(); ++i)
    {
        FrameData& frame = m_Frames[i];

        m_Device->DestroyFence(frame.InFlightFence);
        m_Device->DestroySemaphore(frame.ImageAvailableSemaphore);
        m_Device->FreeCommandBuffer(frame.CommandBuffer);
        m_Device->DestroyCommandPool(frame.CommandPool);
    }

    m_Frames.Clear();

    // 销毁 per-image 渲染完成信号量
    for (UInt32 i = 0; i < m_RenderFinishedSemaphores.GetSize(); ++i)
    {
        m_Device->DestroySemaphore(m_RenderFinishedSemaphores[i]);
    }
    m_RenderFinishedSemaphores.Clear();
}

// ============================================================================
// BeginSingleTimeCommands — 分配并开始录制一次性命令缓冲区
// ============================================================================

IRHICommandBuffer* FRenderContext::BeginSingleTimeCommands()
{
    LIMX_CHECK(m_Device.Get() != nullptr);
    LIMX_CHECK(m_Frames.GetSize() > 0);

    // 使用第 0 帧的命令池分配临时命令缓冲区
    FRHICommandBufferHandle cmdHandle;
    ERHIResult result = m_Device->AllocateCommandBuffer(
        m_Frames[0].CommandPool, ECommandBufferLevel::Primary, cmdHandle);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
            "[RenderContext] 一次性命令缓冲区分配失败");
        return nullptr;
    }

    // 创建命令缓冲区包装器并开始录制
    auto cmdBuffer = CreateRHICommandBuffer(m_Device.Get(), cmdHandle);
    IRHICommandBuffer* rawPtr = cmdBuffer.Get();

    result = rawPtr->Begin();
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
            "[RenderContext] 一次性命令缓冲区录制开始失败");
        m_Device->FreeCommandBuffer(cmdHandle);
        return nullptr;
    }

    // 将包装器所有权转移到临时存储 (用索引追踪)
    m_SingleTimeCmdBuffers.Add(
        static_cast<TUniquePtr<IRHICommandBuffer>&&>(cmdBuffer));
    m_SingleTimeCmdHandles.Add(cmdHandle);

    return rawPtr;
}

// ============================================================================
// EndSingleTimeCommands — 结束录制、提交、等待、释放
// ============================================================================

void FRenderContext::EndSingleTimeCommands(IRHICommandBuffer* commandBuffer)
{
    LIMX_CHECK(commandBuffer != nullptr);
    LIMX_CHECK(m_Device.Get() != nullptr);

    // 结束命令录制
    commandBuffer->End();

    // 查找对应的句柄
    SizeType foundIndex = m_SingleTimeCmdBuffers.GetSize();
    for (SizeType i = 0; i < m_SingleTimeCmdBuffers.GetSize(); ++i)
    {
        if (m_SingleTimeCmdBuffers[i].Get() == commandBuffer)
        {
            foundIndex = i;
            break;
        }
    }

    LIMX_CHECK(foundIndex < m_SingleTimeCmdBuffers.GetSize());

    FRHICommandBufferHandle cmdHandle = m_SingleTimeCmdHandles[foundIndex];

    // 提交到图形队列 (无等待/信号信号量)
    FRHISubmitInfo submitInfo = {};
    submitInfo.CommandBuffers     = &cmdHandle;
    submitInfo.CommandBufferCount = 1;

    m_Device->Submit(EQueueType::Graphics, submitInfo);

    // 等待队列完成
    m_Device->WaitIdle();

    // 释放命令缓冲区
    m_SingleTimeCmdBuffers.RemoveAt(foundIndex);
    m_SingleTimeCmdHandles.RemoveAt(foundIndex);
    m_Device->FreeCommandBuffer(cmdHandle);
}

} // namespace Limx
