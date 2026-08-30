// ============================================================
// 文件名称：IRenderPass.h
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：接口隔离 — IRenderPass 定义渲染通道的最小公共契约，
//          每个 Pass 独立持有所需 GPU 资源，通过 FRenderPassContext
//          接收场景数据与描述符信息，不感知 FRenderer 内部实现。
//          为后续 Render Graph 系统奠定 Pass 抽象基础。
// 功能描述：渲染通道抽象接口 — 定义所有 Pass 必须实现的生命周期方法：
//          Setup (初始化 GPU 资源)、Execute (录制命令)、OnResize (重建)、
//          Shutdown (释放资源)。同时定义 Pass 初始化描述符 FPassSetupDesc
//          和 Pass 执行上下文 FRenderPassContext。
// 技术特性：纯虚接口 (无数据成员)，派生类按需持有资源句柄；
//          FPassSetupDesc 传递全部初始化所需信息，避免 Pass 依赖 FRenderer；
//          FRenderPassContext 仅含只读场景引用，保证 Pass 执行无副作用。
//
// ── 接口方法表 ──────────────────────────────────────────────
// │ 函数名                    │ 描述                           │
// │──────────────────────────│───────────────────────────────│
// │ GetName()                │ 返回 Pass 调试名称               │
// │ GetOrder()               │ 返回执行优先级 (数字越小越先)       │
// │ Setup()                  │ 创建该 Pass 的全部 GPU 资源        │
// │ Execute()                │ 录制该 Pass 的全部渲染命令         │
// │ OnResize()               │ 交换链重建时重建 Pass GPU 资源     │
// │ Shutdown()               │ 释放该 Pass 的全部 GPU 资源        │
//
// ── 结构体表 ──────────────────────────────────────────────────
// │ 结构体名               │ 描述                               │
// │──────────────────────│───────────────────────────────────│
// │ FPassSetupDesc       │ Pass 初始化描述符 (设备/交换链/共享深度) │
// │ FRenderPassContext   │ Pass 执行上下文 (帧索引/场景/描述符集/光照)│
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M0.5 Pass 抽象层)     │
// │ 2026-04-07  │ LimxTeam  │ M0.5 集成: 新增 LightingDescriptorSet │
// ============================================================

#pragma once

#include "Renderer/RendererMinimal.h"
#include "Renderer/Renderer/FRenderer.h"

namespace Limx
{

// ============================================================================
// FPassSetupDesc — Pass 初始化描述符
// 由 FPassManager 在 SetupAll 时构建并传递给各 Pass 的 Setup()。
// ============================================================================

struct FPassSetupDesc
{
    /// GPU 设备接口 (非拥有)
    IRHIDevice*              Device              = nullptr;

    /// 交换链句柄 — 用于获取图像视图和图像数量
    FRHISwapchainHandle      Swapchain;

    /// 交换链像素格式 — 用于创建颜色附件的 RenderPass
    EPixelFormat             SwapchainFormat     = EPixelFormat::Unknown;

    /// 交换链当前尺寸 — 用于创建 Framebuffer
    FRHIExtent2D             SwapchainExtent     = {};

    /// 交换链图像数量 — 用于分配 Framebuffer 数组
    UInt32                   SwapchainImageCount = 0;

    /// 管线布局句柄 — 含 set 0 描述符集布局 + Model Push Constant
    /// 由 FRenderer 在 CreatePipelineLayout 后传入，Pass 不重复创建
    FRHIPipelineLayoutHandle PipelineLayout;

    /// FPassManager 创建的共享深度纹理句柄
    /// FDepthPrePass 写入深度，FForwardPass 读取深度 (Equal 比较)
    FRHITextureHandle        SharedDepthTexture;

    /// FPassManager 创建的共享深度纹理视图
    FRHITextureViewHandle    SharedDepthTextureView;

    /// FPassManager 创建的共享 HDR 颜色目标
    ///
    /// FForwardPass 画进它 (而非直接画进交换链), FPostProcessPass 采样它。
    FRHITextureHandle        SharedColorTexture;
    FRHITextureViewHandle    SharedColorTextureView;

    /// 共享颜色/深度目标的像素格式
    ///
    /// 由 FPassManager 一处给出而非各 Pass 各自写死: 附件格式必须与实际
    /// 纹理完全一致, 而"改了纹理格式却漏改某个 Pass 的附件描述"这种错误
    /// 只在创建 Framebuffer 时才暴露, 报的还是"附件不兼容"这种无指向的话。
    EPixelFormat             SharedColorFormat   = EPixelFormat::Unknown;
    EPixelFormat             SharedDepthFormat   = EPixelFormat::Unknown;

    /// set 0 (view/proj) 的描述符集布局
    ///
    /// 自建管线布局的 Pass 需要它来共享场景的 set 0 —— Vulkan 按布局
    /// **对象**判定描述符集兼容性, 结构相同但对象不同并不算兼容。
    FRHIDescSetLayoutHandle  ViewProjSetLayout;
};

// ============================================================================
// FRenderPassContext — Pass 执行上下文
// 在 FPassManager::ExecuteAll 中构建，传递给各 Pass 的 Execute()。
// ============================================================================

struct FRenderPassContext
{
    /// 当前帧索引 (0 ~ MaxFramesInFlight-1)
    UInt32 FrameIndex = 0;

    /// 当前交换链图像索引 (0 ~ SwapchainImageCount-1)
    UInt32 ImageIndex = 0;

    /// 交换链当前尺寸
    FRHIExtent2D SwapchainExtent = {};

    /// 不透明与蒙版批次 — 只读引用，Pass 不持有所有权。已按材质/网格聚类
    const TArray<FRenderObject>* RenderObjects = nullptr;

    /// 半透明批次 — 已按到相机的距离由远及近排序
    ///
    /// 与不透明分列而非合并后按标志过滤: 两者的绘制顺序要求相反 ——
    /// 不透明按状态聚类以减少绑定, 半透明必须严格由远及近。
    const TArray<FRenderObject>* TranslucentObjects = nullptr;

    /// 阴影投射体 (只读) — **未经相机视锥剔除**的不透明与蒙版批次
    ///
    /// 相机背后的物体照样会把影子投进画面, 因此阴影 Pass 不能用相机剔除后
    /// 的列表。这里给的是剔除前的全量, 由阴影 Pass 各自按光源视锥再剔一次。
    const TArray<FRenderObject>* ShadowCasterObjects = nullptr;

    /// set 0 的描述符集 — 含 ViewProj UBO (binding 0) 和纹理 (binding 1)
    /// 由 FPassManager::ExecuteAll 从帧数据中取出并填入
    FRHIDescriptorSetHandle ViewProjDescriptorSet;

    /// 管线布局句柄 — 与 FPassSetupDesc::PipelineLayout 相同
    FRHIPipelineLayoutHandle PipelineLayout;

    /// 共享深度纹理句柄 (FDepthPrePass 写入后，FPassManager 更新至此)
    FRHITextureHandle SharedDepthTexture;

    /// 共享深度纹理视图
    FRHITextureViewHandle SharedDepthTextureView;

    /// set 2 光照描述符集 — 含 FLightingUBO (binding 0)
    /// 由 FPassManager::ExecuteAll 从帧数据中取出并填入
    FRHIDescriptorSetHandle LightingDescriptorSet;
};

// ============================================================================
// IRenderPass — 渲染通道抽象接口
// ============================================================================

class IRenderPass
{
public:
    virtual ~IRenderPass() = default;

    // ====================================================================
    // 标识
    // ====================================================================

    /// 返回 Pass 调试名称 (用于日志和 RenderDoc 标记)
    LIMX_NODISCARD virtual const AnsiChar* GetName() const = 0;

    /// 返回执行优先级 — 数字越小越先执行
    /// DepthPrePass = 100, ForwardPass = 200
    LIMX_NODISCARD virtual UInt32 GetOrder() const = 0;

    // ====================================================================
    // 生命周期
    // ====================================================================

    /// 初始化 Pass — 创建 RenderPass、Framebuffer、Pipeline 等 GPU 资源
    /// @param desc  初始化描述符，由 FPassManager 构建并传入
    /// @return Success 或错误码
    virtual ERHIResult Setup(const FPassSetupDesc& desc) = 0;

    /// 录制该 Pass 的渲染命令到命令缓冲区
    /// @param commandBuffer  当前帧的命令缓冲区
    /// @param context        Pass 执行上下文 (场景/描述符集/布局等)
    virtual void Execute(IRHICommandBuffer*       commandBuffer,
                         const FRenderPassContext& context) = 0;

    /// 交换链重建时重建 Pass 的尺寸相关 GPU 资源
    /// @param device               GPU 设备
    /// @param swapchain            新交换链句柄
    /// @param newExtent            新交换链尺寸
    /// @param swapchainImageCount  新交换链图像数量
    /// @param newSharedDepth       新共享深度纹理句柄
    /// @param newSharedDepthView   新共享深度纹理视图
    /// @return Success 或错误码
    virtual ERHIResult OnResize(IRHIDevice*           device,
                                FRHISwapchainHandle   swapchain,
                                FRHIExtent2D          newExtent,
                                UInt32                swapchainImageCount,
                                FRHITextureHandle     newSharedDepth,
                                FRHITextureViewHandle newSharedDepthView,
                                FRHITextureHandle     newSharedColor,
                                FRHITextureViewHandle newSharedColorView) = 0;

    /// 释放依赖交换链尺寸或图像视图的资源。
    /// 在交换链销毁前调用，避免 framebuffer 持有失效的 swapchain image view。
    virtual void ReleaseSwapchainResources(IRHIDevice* device) = 0;

    /// 释放该 Pass 的全部 GPU 资源
    /// @param device GPU 设备 (用于销毁资源)
    virtual void Shutdown(IRHIDevice* device) = 0;

protected:
    IRenderPass() = default;
    LIMX_NON_COPYABLE(IRenderPass);
    LIMX_NON_MOVABLE(IRenderPass);
};

} // namespace Limx
