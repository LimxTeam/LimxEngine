// ============================================================
// 文件名称：FPassManager.h
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：序列化 Pass 调度 — FPassManager 作为 Pass 生命周期的中央协调者，
//          负责共享深度缓冲区的创建与销毁，按 Order 排序后顺序执行 Pass，
//          为后续 Render Graph (依赖 DAG 驱动) 奠定基础。
// 功能描述：Pass 管理器 — 提供 Pass 注册、初始化、执行、重建、销毁功能。
//          创建和持有场景共享深度缓冲区 (所有 Pass 共用同一深度纹理)；
//          SetupAll 按依赖顺序初始化所有已注册 Pass；
//          ExecuteAll 按 Order 顺序在同一命令缓冲区中依次录制各 Pass 命令；
//          OnResizeAll 重建共享深度缓冲区并通知各 Pass 重建尺寸相关资源；
//          ShutdownAll 按逆序销毁所有 Pass 及共享深度缓冲区。
// 技术特性：TArray<IRenderPass*> 非拥有指针存储 (Pass 由调用方管理生命周期);
//          按 GetOrder() 升序排序 Pass 执行顺序;
//          共享深度纹理 D32_SFLOAT 随交换链尺寸动态重建;
//          ExecuteAll 在构建 FRenderPassContext 时注入所有共享资源。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                    │ 描述                           │
// │──────────────────────────│───────────────────────────────│
// │ RegisterPass()           │ 注册一个 Pass (非拥有)           │
// │ UnregisterPass()         │ 注销一个已注册 Pass              │
// │ SetupAll()               │ 创建共享深度 + 初始化所有 Pass    │
// │ ExecuteAll()             │ 按 Order 顺序执行所有 Pass       │
// │ OnResizeAll()            │ 重建共享深度 + 通知所有 Pass 重建  │
// │ ShutdownAll()            │ 销毁所有 Pass + 共享深度缓冲区    │
//
// ── 结构体字段表 ──────────────────────────────────────────────
// │ 字段名                    │ 类型                           │ 描述          │
// │──────────────────────────│──────────────────────────────│──────────────│
// │ m_Passes                 │ TArray<IRenderPass*>          │ 注册的 Pass 列表│
// │ m_SharedDepthTexture     │ FRHITextureHandle             │ 共享深度纹理    │
// │ m_SharedDepthTextureView │ FRHITextureViewHandle         │ 共享深度纹理视图 │
// │ m_Device                 │ IRHIDevice*                   │ GPU 设备 (非拥有)│
// │ m_Swapchain              │ FRHISwapchainHandle           │ 交换链句柄      │
// │ m_SwapchainFormat        │ EPixelFormat                  │ 交换链格式缓存  │
// │ m_SwapchainImageCount    │ UInt32                        │ 图像数量缓存   │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M0.5 Pass 管理器)     │
// │ 2026-04-07  │ LimxTeam  │ M0.5 集成: FPassExecuteInfo 新增光照 │
// ============================================================

#pragma once

#include "Renderer/RenderPass/IRenderPass.h"

namespace Limx
{

// ============================================================================
// FPassSetupInfo — SetupAll 调用时由外部提供的全局初始化信息
// ============================================================================

struct FPassSetupInfo
{
    /// GPU 设备接口 (非拥有)
    IRHIDevice*              Device              = nullptr;

    /// 交换链句柄
    FRHISwapchainHandle      Swapchain;

    /// 交换链像素格式
    EPixelFormat             SwapchainFormat     = EPixelFormat::Unknown;

    /// 交换链当前尺寸
    FRHIExtent2D             SwapchainExtent     = {};

    /// 交换链图像数量
    UInt32                   SwapchainImageCount = 0;

    /// 管线布局句柄 (含 set 0 + Push Constant)
    FRHIPipelineLayoutHandle PipelineLayout;
};

// ============================================================================
// FPassExecuteInfo — ExecuteAll 调用时由外部提供的每帧执行信息
// ============================================================================

struct FPassExecuteInfo
{
    /// 当前帧索引 (0 ~ MaxFramesInFlight-1)
    UInt32 FrameIndex = 0;

    /// 当前交换链图像索引 (0 ~ SwapchainImageCount-1)
    UInt32 ImageIndex = 0;

    /// 交换链当前尺寸
    FRHIExtent2D SwapchainExtent = {};

    /// 不透明与蒙版批次 (只读) — 已按材质/网格状态聚类
    const TArray<FRenderObject>* RenderObjects = nullptr;

    /// 半透明批次 (只读) — 已按到相机的距离由远及近排序
    const TArray<FRenderObject>* TranslucentObjects = nullptr;

    /// 阴影投射体 (只读) — 未经相机视锥剔除的不透明与蒙版批次
    const TArray<FRenderObject>* ShadowCasterObjects = nullptr;

    /// set 0 描述符集 — ViewProj UBO + 纹理 (对应当前 FrameIndex)
    FRHIDescriptorSetHandle ViewProjDescriptorSet;

    /// 管线布局句柄
    FRHIPipelineLayoutHandle PipelineLayout;

    /// set 2 光照描述符集 — 含 FLightingUBO (对应当前 FrameIndex)
    FRHIDescriptorSetHandle LightingDescriptorSet;
};

// ============================================================================
// FPassManager — Render Pass 管理器
// ============================================================================

class FPassManager
{
public:
    LIMX_NON_COPYABLE(FPassManager);

    FPassManager() = default;
    ~FPassManager();

    // ====================================================================
    // Pass 注册
    // ====================================================================

    /// 注册一个渲染 Pass (非拥有，生命周期由调用方管理)
    /// @param pass  Pass 指针，Setup 后有效
    void RegisterPass(IRenderPass* pass);

    /// 注销一个已注册的渲染 Pass (不释放，仅移除引用)
    /// @param pass  要移除的 Pass 指针
    void UnregisterPass(IRenderPass* pass);

    // ====================================================================
    // 生命周期
    // ====================================================================

    /// 初始化所有已注册 Pass
    /// 1. 创建共享深度缓冲区 (D32_SFLOAT, 交换链尺寸)
    /// 2. 按 Order 排序 Pass
    /// 3. 调用每个 Pass 的 Setup()
    /// @param info  全局初始化信息
    /// @return Success 或错误码
    ERHIResult SetupAll(const FPassSetupInfo& info);

    /// 在同一命令缓冲区中按 Order 顺序执行所有 Pass
    /// 构建 FRenderPassContext 并依次调用各 Pass 的 Execute()
    /// @param commandBuffer  当前帧命令缓冲区
    /// @param info           每帧执行信息
    void ExecuteAll(IRHICommandBuffer*     commandBuffer,
                    const FPassExecuteInfo& info);

    /// 交换链重建后重建尺寸相关资源
    /// 1. 重建共享深度缓冲区
    /// 2. 调用所有 Pass 的 OnResize()
    /// @param device               GPU 设备
    /// @param swapchain            新交换链句柄
    /// @param newExtent            新交换链尺寸
    /// @param swapchainImageCount  新交换链图像数量
    /// @return Success 或错误码
    ERHIResult OnResizeAll(IRHIDevice*         device,
                            FRHISwapchainHandle swapchain,
                            FRHIExtent2D        newExtent,
                            UInt32              swapchainImageCount);

    /// 交换链销毁前释放所有尺寸相关资源。
    void ReleaseSwapchainResources(IRHIDevice* device);

    /// 销毁所有 Pass + 共享深度缓冲区
    /// @param device GPU 设备
    void ShutdownAll(IRHIDevice* device);

    // ====================================================================
    // 查询
    // ====================================================================

    /// 获取已注册 Pass 数量
    LIMX_NODISCARD SizeType GetPassCount() const
    {
        return m_Passes.GetSize();
    }

    /// 管理器是否已初始化 (SetupAll 调用后为 true)
    LIMX_NODISCARD bool IsInitialized() const
    {
        return m_IsInitialized;
    }

private:
    // ====================================================================
    // 共享深度缓冲区
    // ====================================================================

    /// 创建共享深度纹理 + 纹理视图 (D32_SFLOAT)
    ERHIResult CreateSharedDepth(IRHIDevice* device, FRHIExtent2D extent);

    /// 销毁共享深度纹理 + 纹理视图
    void DestroySharedDepth(IRHIDevice* device);

    /// 按 GetOrder() 升序对 m_Passes 排序
    void SortPassesByOrder();

    // ====================================================================
    // 成员
    // ====================================================================

    /// 已注册的 Pass 列表 (非拥有指针，按 Order 升序排列)
    TArray<IRenderPass*>  m_Passes;

    /// 共享深度纹理 — 所有 Pass 共用 (D32_SFLOAT, 随交换链尺寸变化)
    FRHITextureHandle     m_SharedDepthTexture;

    /// 共享深度纹理视图
    FRHITextureViewHandle m_SharedDepthTextureView;

    /// 缓存的初始化信息 (OnResizeAll 时需要 swapchain/format/imageCount)
    IRHIDevice*           m_Device              = nullptr;
    FRHISwapchainHandle   m_Swapchain;
    EPixelFormat          m_SwapchainFormat     = EPixelFormat::Unknown;
    UInt32                m_SwapchainImageCount = 0;

    /// 已初始化标志
    bool                  m_IsInitialized = false;
};

} // namespace Limx
