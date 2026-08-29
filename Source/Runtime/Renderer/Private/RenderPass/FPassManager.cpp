// ============================================================
// 文件名称：FPassManager.cpp
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：中央协调者 — FPassManager 作为 Pass 生命周期的唯一管理入口，
//          创建和持有所有 Pass 共享的深度缓冲区，按 Order 排序后顺序
//          执行 Pass，保证渲染顺序正确性 (DepthPrePass→ForwardPass)。
// 功能描述：FPassManager 完整实现 — Pass 注册/注销，共享深度缓冲区
//          创建/销毁，SetupAll/ExecuteAll/OnResizeAll/ShutdownAll 全流程。
// 技术特性：TArray<IRenderPass*> 非拥有存储，按 Order 插入排序保证顺序;
//          共享深度纹理 D32_SFLOAT 与交换链同尺寸;
//          ExecuteAll 构建 FRenderPassContext 注入共享资源后顺序执行;
//          ShutdownAll 按注册逆序调用 Pass::Shutdown。
//
// ── 函数表 ──────────────────────────────────────────────────
// │ 函数名               │ 描述                                │
// │─────────────────────│────────────────────────────────────│
// │ RegisterPass()       │ 按 Order 插入排序注册 Pass            │
// │ UnregisterPass()     │ 从列表中移除 Pass 引用                │
// │ SetupAll()           │ 创建共享深度 + 调用所有 Pass::Setup   │
// │ ExecuteAll()         │ 构建 Context + 调用所有 Pass::Execute │
// │ OnResizeAll()        │ 重建共享深度 + 调用 Pass::OnResize    │
// │ ShutdownAll()        │ 调用 Pass::Shutdown + 销毁共享深度    │
// │ CreateSharedDepth()  │ 创建 D32_SFLOAT 共享深度缓冲区        │
// │ DestroySharedDepth() │ 销毁共享深度缓冲区                    │
// │ SortPassesByOrder()  │ 按 GetOrder() 升序气泡排序 Pass       │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M0.5 Pass 管理器)     │
// ============================================================

#include "Renderer/RenderPass/FPassManager.h"

namespace Limx
{

LIMX_DECLARE_LOG_CATEGORY(LogRenderer)

// ============================================================================
// 析构
// ============================================================================

FPassManager::~FPassManager()
{
    // 析构时 ShutdownAll 应由外部显式调用，此处仅断言检查
    LIMX_ENSURE(!m_IsInitialized);
}

// ============================================================================
// RegisterPass — 按 Order 升序插入 Pass (非拥有)
// ============================================================================

void FPassManager::RegisterPass(IRenderPass* pass)
{
    LIMX_CHECK(pass != nullptr);

    m_Passes.Add(pass);

    // 保持 m_Passes 按 Order 升序排列
    SortPassesByOrder();

    LIMX_LOG(LogRenderer, Log,
             "[PassManager] 注册 Pass: {} (Order={})",
             pass->GetName(), pass->GetOrder());
}

// ============================================================================
// UnregisterPass — 移除 Pass 引用 (不释放)
// ============================================================================

void FPassManager::UnregisterPass(IRenderPass* pass)
{
    LIMX_CHECK(pass != nullptr);

    for (SizeType i = 0; i < m_Passes.GetSize(); ++i)
    {
        if (m_Passes[i] == pass)
        {
            m_Passes.RemoveAtSwap(i);
            SortPassesByOrder();

            LIMX_LOG(LogRenderer, Log,
                     "[PassManager] 注销 Pass: {}",
                     pass->GetName());
            return;
        }
    }

    LIMX_LOG(LogRenderer, Warning,
             "[PassManager] 尝试注销未注册的 Pass: {}",
             pass->GetName());
}

// ============================================================================
// SetupAll — 创建共享深度缓冲区 + 初始化所有 Pass
// ============================================================================

ERHIResult FPassManager::SetupAll(const FPassSetupInfo& info)
{
    LIMX_CHECK(info.Device != nullptr);

    // 缓存初始化信息供 OnResizeAll 使用
    m_Device              = info.Device;
    m_Swapchain           = info.Swapchain;
    m_SwapchainFormat     = info.SwapchainFormat;
    m_SwapchainImageCount = info.SwapchainImageCount;

    // 1. 创建共享深度缓冲区 (D32_SFLOAT, 交换链尺寸)
    ERHIResult result = CreateSharedDepth(info.Device, info.SwapchainExtent);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
                 "[PassManager] 共享深度缓冲区创建失败");
        return result;
    }

    // 2. 构建 FPassSetupDesc 并调用所有 Pass 的 Setup()
    FPassSetupDesc setupDesc = {};
    setupDesc.Device               = info.Device;
    setupDesc.Swapchain            = info.Swapchain;
    setupDesc.SwapchainFormat      = info.SwapchainFormat;
    setupDesc.SwapchainExtent      = info.SwapchainExtent;
    setupDesc.SwapchainImageCount  = info.SwapchainImageCount;
    setupDesc.PipelineLayout       = info.PipelineLayout;
    setupDesc.SharedDepthTexture   = m_SharedDepthTexture;
    setupDesc.SharedDepthTextureView = m_SharedDepthTextureView;

    for (SizeType i = 0; i < m_Passes.GetSize(); ++i)
    {
        result = m_Passes[i]->Setup(setupDesc);
        if (!IsRHISuccess(result))
        {
            LIMX_LOG(LogRenderer, Error,
                     "[PassManager] Pass '{}' Setup 失败",
                     m_Passes[i]->GetName());
            return result;
        }
    }

    m_IsInitialized = true;

    LIMX_LOG(LogRenderer, Log,
             "[PassManager] SetupAll 完成 — {} 个 Pass",
             m_Passes.GetSize());

    return ERHIResult::Success;
}

// ============================================================================
// ExecuteAll — 按 Order 顺序执行所有 Pass
// ============================================================================

void FPassManager::ExecuteAll(IRHICommandBuffer*     commandBuffer,
                               const FPassExecuteInfo& info)
{
    LIMX_CHECK(commandBuffer != nullptr);

    // 构建 FRenderPassContext — 注入共享资源和场景信息
    FRenderPassContext context = {};
    context.FrameIndex             = info.FrameIndex;
    context.ImageIndex             = info.ImageIndex;
    context.SwapchainExtent        = info.SwapchainExtent;
    context.RenderObjects          = info.RenderObjects;
    context.TranslucentObjects     = info.TranslucentObjects;
    context.ViewProjDescriptorSet  = info.ViewProjDescriptorSet;
    context.PipelineLayout         = info.PipelineLayout;
    context.LightingDescriptorSet  = info.LightingDescriptorSet;
    context.SharedDepthTexture     = m_SharedDepthTexture;
    context.SharedDepthTextureView = m_SharedDepthTextureView;

    // 按 Order 顺序执行 Pass (已在 Register/Sort 时保证顺序)
    for (SizeType i = 0; i < m_Passes.GetSize(); ++i)
    {
        m_Passes[i]->Execute(commandBuffer, context);
    }
}

// ============================================================================
// OnResizeAll — 重建共享深度缓冲区 + 通知所有 Pass 重建
// ============================================================================

ERHIResult FPassManager::OnResizeAll(IRHIDevice*         device,
                                      FRHISwapchainHandle swapchain,
                                      FRHIExtent2D        newExtent,
                                      UInt32              swapchainImageCount)
{
    m_Swapchain           = swapchain;
    m_SwapchainImageCount = swapchainImageCount;

    // 1. 销毁并重建共享深度缓冲区
    DestroySharedDepth(device);

    ERHIResult result = CreateSharedDepth(device, newExtent);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
                 "[PassManager] OnResize: 共享深度缓冲区重建失败");
        return result;
    }

    // 2. 通知所有 Pass 重建尺寸相关资源
    for (SizeType i = 0; i < m_Passes.GetSize(); ++i)
    {
        result = m_Passes[i]->OnResize(device,
                                        swapchain,
                                        newExtent,
                                        swapchainImageCount,
                                        m_SharedDepthTexture,
                                        m_SharedDepthTextureView);
        if (!IsRHISuccess(result))
        {
            LIMX_LOG(LogRenderer, Error,
                     "[PassManager] Pass '{}' OnResize 失败",
                     m_Passes[i]->GetName());
            return result;
        }
    }

    LIMX_LOG(LogRenderer, Log,
             "[PassManager] OnResizeAll 完成 — {}x{}",
             newExtent.Width, newExtent.Height);

    return ERHIResult::Success;
}

// ============================================================================
// ReleaseSwapchainResources — 交换链销毁前释放尺寸相关资源
// ============================================================================

void FPassManager::ReleaseSwapchainResources(IRHIDevice* device)
{
    if (device == nullptr)
    {
        return;
    }

    for (SizeType i = m_Passes.GetSize(); i > 0; --i)
    {
        m_Passes[i - 1]->ReleaseSwapchainResources(device);
    }

    DestroySharedDepth(device);
}

// ============================================================================
// ShutdownAll — 关闭所有 Pass + 销毁共享深度缓冲区
// ============================================================================

void FPassManager::ShutdownAll(IRHIDevice* device)
{
    if (device == nullptr)
    {
        return;
    }

    // 按逆序关闭 Pass (ForwardPass → DepthPrePass)
    for (SizeType i = m_Passes.GetSize(); i > 0; --i)
    {
        m_Passes[i - 1]->Shutdown(device);
    }

    // 销毁共享深度缓冲区
    DestroySharedDepth(device);

    m_IsInitialized = false;

    LIMX_LOG(LogRenderer, Log,
             "[PassManager] ShutdownAll 完成");
}

// ============================================================================
// CreateSharedDepth — 创建 D32_SFLOAT 共享深度纹理 + 纹理视图
// ============================================================================

ERHIResult FPassManager::CreateSharedDepth(IRHIDevice* device,
                                             FRHIExtent2D extent)
{
    // 创建深度纹理 — D32_SFLOAT, 交换链尺寸, 深度附件 + 采样用途
    FRHITextureDesc depthDesc = FRHITextureDesc::DepthStencil(
        extent.Width, extent.Height,
        EPixelFormat::D32_SFLOAT,
        ESampleCount::Count1);
    depthDesc.DebugName = "SharedDepthBuffer";

    ERHIResult result = device->CreateTexture(depthDesc, m_SharedDepthTexture);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
                 "[PassManager] 共享深度纹理创建失败 ({}x{})",
                 extent.Width, extent.Height);
        return result;
    }

    // 创建深度纹理视图
    FRHITextureViewDesc viewDesc = {};
    viewDesc.Texture         = m_SharedDepthTexture;
    viewDesc.ViewType        = ETextureType::Texture2D;
    viewDesc.Format          = EPixelFormat::D32_SFLOAT;
    viewDesc.BaseMipLevel    = 0;
    viewDesc.MipLevelCount   = 1;
    viewDesc.BaseArrayLayer  = 0;
    viewDesc.ArrayLayerCount = 1;

    result = device->CreateTextureView(viewDesc, m_SharedDepthTextureView);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
                 "[PassManager] 共享深度纹理视图创建失败");
        device->DestroyTexture(m_SharedDepthTexture);
        return result;
    }

    LIMX_LOG(LogRenderer, Log,
             "[PassManager] 共享深度缓冲区创建完成 — D32_SFLOAT {}x{}",
             extent.Width, extent.Height);

    return ERHIResult::Success;
}

// ============================================================================
// DestroySharedDepth — 销毁共享深度纹理 + 纹理视图
// ============================================================================

void FPassManager::DestroySharedDepth(IRHIDevice* device)
{
    if (m_SharedDepthTextureView.IsValid())
    {
        device->DestroyTextureView(m_SharedDepthTextureView);
    }
    if (m_SharedDepthTexture.IsValid())
    {
        device->DestroyTexture(m_SharedDepthTexture);
    }
}

// ============================================================================
// SortPassesByOrder — 按 GetOrder() 升序排序 (气泡排序，Pass 数量极小)
// ============================================================================

void FPassManager::SortPassesByOrder()
{
    SizeType n = m_Passes.GetSize();
    for (SizeType i = 0; i < n; ++i)
    {
        for (SizeType j = 0; j + 1 < n - i; ++j)
        {
            if (m_Passes[j]->GetOrder() > m_Passes[j + 1]->GetOrder())
            {
                IRenderPass* temp = m_Passes[j];
                m_Passes[j]       = m_Passes[j + 1];
                m_Passes[j + 1]   = temp;
            }
        }
    }
}

} // namespace Limx
