// ============================================================
// 文件名称：FRenderContext.h
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：单一职责 — 持有 RHI 设备和交换链的生命周期，
//          管理 N 帧并行 (Frames-in-Flight) 的同步原语，
//          对外仅暴露 BeginFrame/EndFrame 帧生命周期接口。
// 功能描述：渲染上下文 — 从窗口句柄创建 RHI 设备，管理交换链，
//          为每帧分配独立的命令池/命令缓冲区/同步信号量/栅栏，
//          处理交换链过期时的自动重建。
// 技术特性：双缓冲/三缓冲帧同步; TUniquePtr 管理设备生命周期;
//          TArray<FrameData> 存储每帧资源; 环形索引管理当前帧。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                      │ 描述                           │
// │────────────────────────────│───────────────────────────────│
// │ Initialize()               │ 创建设备、交换链、帧资源          │
// │ Shutdown()                 │ 等待 GPU 空闲并释放全部资源        │
// │ BeginFrame()               │ 等待栅栏→获取图像→重置命令缓冲区   │
// │ EndFrame()                 │ 提交命令→呈现→推进帧索引           │
// │ RecreateSwapchain()        │ 交换链重建 (窗口尺寸变化时)        │
// │ GetDevice()                │ 获取 RHI 设备接口指针             │
// │ GetCurrentCommandBuffer()  │ 获取当前帧的命令缓冲区接口         │
// │ GetSwapchainExtent()       │ 获取交换链当前尺寸                │
// │ BeginSingleTimeCommands()  │ 分配并开始录制一次性命令缓冲区     │
// │ EndSingleTimeCommands()    │ 结束录制+提交+等待+释放一次性命令   │
//
// ── 结构体字段表 ──────────────────────────────────────────────
// │ 字段名                      │ 类型                   │ 描述          │
// │────────────────────────────│──────────────────────│──────────────│
// │ m_Device                   │ TUniquePtr<IRHIDevice>│ RHI 设备       │
// │ m_Swapchain                │ FRHISwapchainHandle  │ 交换链句柄      │
// │ m_Frames                   │ TArray<FrameData>    │ 帧资源数组      │
// │ m_CommandBuffers           │ TArray<TUniquePtr<>> │ 命令缓冲区包装  │
// │ m_CurrentFrame             │ UInt32               │ 当前帧索引      │
// │ m_CurrentImageIndex        │ UInt32               │ 当前图像索引    │
// │ m_MaxFramesInFlight        │ UInt32               │ 最大并行帧数    │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-06  │ LimxTeam  │ 初始创建                        │
// │ 2026-04-07  │ LimxTeam  │ 添加 SingleTimeCommands 辅助    │
// ============================================================

#pragma once

#include "RenderCore/RenderCoreMinimal.h"

namespace Limx
{

// 前向声明
class FWindow;

// ============================================================================
// FRenderContextDesc — 渲染上下文创建描述符
// ============================================================================

struct FRenderContextDesc
{
    /// 关联的窗口 (必须已创建)
    FWindow* Window = nullptr;

    /// 是否启用 Vulkan 验证层 (Development 配置推荐开启)
    bool EnableValidation = true;

    /// 最大并行帧数 (2=双缓冲, 3=三缓冲)
    UInt32 MaxFramesInFlight = 2;

    /// 是否启用垂直同步
    bool IsVSyncEnabled = false;
};

// ============================================================================
// FRenderContext — 渲染上下文
// ============================================================================

class FRenderContext
{
public:
    LIMX_NON_COPYABLE(FRenderContext);

    FRenderContext() = default;
    ~FRenderContext();

    // ====================================================================
    // 生命周期
    // ====================================================================

    /// 初始化渲染上下文
    /// 创建 RHI 设备 → 创建交换链 → 分配帧同步资源
    ERHIResult Initialize(const FRenderContextDesc& desc);

    /// 关闭渲染上下文
    /// 等待 GPU 空闲 → 释放帧资源 → 销毁交换链 → 销毁设备
    void Shutdown();

    // ====================================================================
    // 帧生命周期
    // ====================================================================

    /// 开始新一帧
    /// 等待当前帧的栅栏 → 获取下一个交换链图像 → 重置命令缓冲区
    /// @return Success 或错误码 (ErrorOutOfDate 表示需要重建交换链)
    ERHIResult BeginFrame();

    /// 结束当前帧
    /// 提交命令缓冲区到图形队列 → 呈现到交换链 → 推进帧索引
    /// @return Success 或错误码
    ERHIResult EndFrame();

    // ====================================================================
    // 交换链管理
    // ====================================================================

    /// 重建交换链 (窗口尺寸变化时调用)
    ERHIResult RecreateSwapchain();

    // ====================================================================
    // 访问器
    // ====================================================================

    /// 获取 RHI 设备接口
    LIMX_NODISCARD IRHIDevice* GetDevice() const { return m_Device.Get(); }

    /// 获取当前帧的命令缓冲区接口
    LIMX_NODISCARD IRHICommandBuffer* GetCurrentCommandBuffer() const;

    /// 获取交换链句柄
    LIMX_NODISCARD FRHISwapchainHandle GetSwapchain() const
    {
        return m_Swapchain;
    }

    /// 获取当前帧索引 (0 ~ MaxFramesInFlight-1)
    LIMX_NODISCARD UInt32 GetCurrentFrameIndex() const
    {
        return m_CurrentFrame;
    }

    /// 获取当前交换链图像索引 (0 ~ SwapchainImageCount-1)
    LIMX_NODISCARD UInt32 GetCurrentImageIndex() const
    {
        return m_CurrentImageIndex;
    }

    /// 获取最大并行帧数
    /// 单调递增的帧序号 — 每次 BeginFrame 自增
    ///
    /// 与 GetCurrentFrameIndex 的区别: 后者在 [0, MaxFramesInFlight) 内循环,
    /// 无法用来判断"某个时刻是否已过去足够多帧"。延迟销毁依赖后一种判断。
    LIMX_NODISCARD UInt64 GetFrameCounter() const { return m_FrameCounter; }

    LIMX_NODISCARD UInt32 GetMaxFramesInFlight() const
    {
        return m_MaxFramesInFlight;
    }

    /// 获取交换链尺寸
    LIMX_NODISCARD FRHIExtent2D GetSwapchainExtent() const;

    /// 获取交换链格式
    LIMX_NODISCARD EPixelFormat GetSwapchainFormat() const;

    // ====================================================================
    // 一次性命令缓冲区 (纹理上传、布局转换等即时操作)
    // ====================================================================

    /// 分配并开始录制一次性命令缓冲区
    /// 返回的指针在 EndSingleTimeCommands 后失效
    IRHICommandBuffer* BeginSingleTimeCommands();

    /// 结束录制、提交到图形队列、等待完成、释放命令缓冲区
    void EndSingleTimeCommands(IRHICommandBuffer* commandBuffer);

    /// 渲染上下文是否已初始化
    // ====================================================================
    // GPU 资源
    // ====================================================================

    /// 取 GPU 资源管理器
    ///
    /// 资源管理器随上下文一同创建与销毁 —— 它持有的 GPU 对象生命周期
    /// 不能长于设备。挂在上下文上而非渲染器上, 是因为资源的消费者不止渲染器:
    /// 场景、材质系统都需要引用同一批资源。
    LIMX_NODISCARD FRenderResourceManager& GetResourceManager()
    {
        return m_ResourceManager;
    }

    LIMX_NODISCARD const FRenderResourceManager& GetResourceManager() const
    {
        return m_ResourceManager;
    }

    LIMX_NODISCARD bool IsInitialized() const
    {
        return m_Device.Get() != nullptr;
    }

private:
    // ====================================================================
    // 每帧同步资源
    // ====================================================================

    struct FrameData
    {
        FRHICommandPoolHandle    CommandPool;
        FRHICommandBufferHandle  CommandBuffer;
        FRHISemaphoreHandle      ImageAvailableSemaphore;
        FRHIFenceHandle          InFlightFence;
    };

    /// 创建帧同步资源
    ERHIResult CreateFrameResources();

    /// 释放帧同步资源
    void DestroyFrameResources();

    /// 创建交换链
    ERHIResult CreateSwapchain();

    /// 销毁交换链
    void DestroySwapchain();

    // ====================================================================
    // 成员
    // ====================================================================

    FWindow* m_Window = nullptr;

    TUniquePtr<IRHIDevice>                  m_Device;

    /// GPU 资源管理器 —— 网格与纹理的唯一所有者
    FRenderResourceManager                  m_ResourceManager;

    FRHISwapchainHandle                     m_Swapchain;
    TArray<FrameData>                       m_Frames;
    TArray<TUniquePtr<IRHICommandBuffer>>   m_CommandBuffers;

    // 渲染完成信号量 — 按 swapchain image 索引 (非帧索引)
    // 避免 Present 的 swapchain 内部跟踪与帧索引不对齐导致验证错误
    TArray<FRHISemaphoreHandle>             m_RenderFinishedSemaphores;

    // 一次性命令缓冲区临时存储 (BeginSingleTimeCommands/EndSingleTimeCommands)
    TArray<TUniquePtr<IRHICommandBuffer>>   m_SingleTimeCmdBuffers;
    TArray<FRHICommandBufferHandle>         m_SingleTimeCmdHandles;

    /// 单调帧序号 — 延迟销毁按它判断资源是否已脱离 GPU 使用
    UInt64 m_FrameCounter      = 0;

    UInt32 m_CurrentFrame      = 0;
    UInt32 m_CurrentImageIndex = 0;
    UInt32 m_MaxFramesInFlight = 2;
    bool   m_IsVSyncEnabled    = false;
};

} // namespace Limx
