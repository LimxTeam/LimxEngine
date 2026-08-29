// ============================================================
// 文件名称：FRenderer.cpp
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：渐进式复杂度 — M0.3 实现 VBO+IBO 索引绘制 + UBO + 描述符集 +
//          深度缓冲区 + 程序化几何体 + 纹理采样 (棋盘格)，
//          后续逐步添加 RenderGraph、Pass、材质等。
// 功能描述：FRenderer 完整实现 — 初始化创建 VBO/IBO/UBO/描述符集/
//          深度缓冲区/纹理/RenderPass/帧缓冲/着色器/管线，帧循环中
//          更新 MVP 矩阵并通过 BindVBO→BindIBO→DrawIndexed
//          绘制纹理旋转立方体。
// 技术特性：运行时加载 SPIR-V 着色器 (FShaderLoader + LSC 工具链)；
//          顶点数据通过 VBO 传递 (位置+法线+颜色+UV 交错布局)；
//          索引数据通过 IBO 传递 (UInt16)；
//          MVP 矩阵通过 Uniform Buffer + 描述符集每帧更新；
//          深度缓冲区 D32_SFLOAT 启用深度测试/写入；
//          程序化棋盘格纹理 (RGBA8, 256x256) + combined image sampler；
//          片段着色器半 Lambert 方向光照明 + 纹理采样；
//          交换链重建时自动重建深度缓冲区和帧缓冲。
//
// ── 函数表 ──────────────────────────────────────────────────
// │ 函数名                        │ 描述                           │
// │──────────────────────────────│───────────────────────────────│
// │ Initialize()                 │ 创建全部 GPU 管线资源            │
// │ Shutdown()                   │ 释放全部 GPU 管线资源            │
// │ RenderFrame()                │ 完整单帧渲染循环                 │
// │ OnSwapchainRecreated()       │ 重建帧缓冲                     │
// │ CreateRenderPass()           │ 创建渲染通道                    │
// │ CreateFramebuffers()         │ 创建帧缓冲                     │
// │ DestroyFramebuffers()        │ 销毁帧缓冲                     │
// │ CreateShaders()              │ 创建着色器模块                   │
// │ CreateVertexBuffer()         │ 创建顶点缓冲区 (VBO)            │
// │ CreateIndexBuffer()          │ 创建索引缓冲区 (IBO)            │
// │ CreateUniformBuffers()       │ 创建 Uniform Buffer (MVP)      │
// │ CreateDescriptorResources()  │ 创建描述符集布局+分配描述符集     │
// │ CreateDepthResources()       │ 创建深度纹理+纹理视图            │
// │ DestroyDepthResources()      │ 销毁深度纹理+纹理视图            │
// │ CreatePipelineLayout()       │ 创建管线布局                    │
// │ CreateGraphicsPipeline()     │ 创建图形管线                    │
// │ DestroyPipelineResources()   │ 销毁管线资源                    │
// │ DestroyBufferResources()     │ 销毁缓冲区和描述符资源           │
// │ CreateTextureResources()     │ 创建棋盘格纹理+采样器+视图       │
// │ DestroyTextureResources()    │ 销毁纹理资源                  │
// │ UpdateUniformBuffer()        │ 每帧更新 MVP 矩阵              │
// │ RecordCommands()             │ 录制纹理立方体索引绘制命令      │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-06  │ LimxTeam  │ 初始创建 (清屏渲染)              │
// │ 2026-04-07  │ LimxTeam  │ M0.1 三角形渲染                 │
// │ 2026-04-07  │ LimxTeam  │ M0.2 基础: VBO+UBO+MVP 矩阵    │
// │ 2026-04-07  │ LimxTeam  │ M0.2 深度缓冲区: D32_SFLOAT     │
// │ 2026-04-07  │ LimxTeam  │ M0.3 IBO+立方体+法线光照       │
// │ 2026-04-07  │ LimxTeam  │ M0.3 纹理采样: UV+棋盘格+sampler │
// ============================================================

#include "Renderer/Renderer/FRenderer.h"
#include "RenderCore/Renderer/FRenderContext.h"
#include "ApplicationCore/Window/FWindow.h"
#include "RenderCore/Geometry/FGeometryGenerator.h"
#include "RenderCore/Shaders/FShaderManager.h"
#include "RenderCore/Material/FMaterialManager.h"
#include "RenderCore/Material/FMaterial.h"
#include "RenderCore/Lighting/FLightManager.h"
#include "RenderCore/Lighting/FLight.h"
#include "Renderer/RenderPass/FPassManager.h"
#include "Renderer/RenderPass/FShadowPass.h"
#include "Renderer/RenderPass/FPostProcessPass.h"
#include "Renderer/RenderPass/FForwardPass.h"
#include "Renderer/RenderPass/FDepthPrePass.h"

namespace Limx
{

// 引入 Core::Memory 子命名空间的内存操作函数到当前作用域
using Memory::MemCopy;

// 使用 FRenderContext.cpp 中定义的 LogRenderer
LIMX_DECLARE_LOG_CATEGORY(LogRenderer)

// ============================================================================
// 构造 / 析构
// ============================================================================

FRenderer::FRenderer() = default;

FRenderer::~FRenderer()
{
    Shutdown();
}

// ============================================================================
// Initialize — 创建全部 GPU 管线资源
// ============================================================================

ERHIResult FRenderer::Initialize(FWindow* window, FRenderContext* context)
{
    LIMX_CHECK(window != nullptr && window->IsValid());
    LIMX_CHECK(context != nullptr && context->IsInitialized());

    m_Window  = window;
    m_Context = context;

    IRHIDevice* device    = m_Context->GetDevice();
    UInt32      frameCount = m_Context->GetMaxFramesInFlight();

    // 初始化相机 — 位于 (0, 2.5, -5)，俰视场景中心，45° FOV
    FRHIExtent2D initExtent = m_Context->GetSwapchainExtent();
    Float32 initAspect = static_cast<Float32>(initExtent.Width) /
                         static_cast<Float32>(FMath::Max(initExtent.Height, 1u));
    m_Camera.SetPosition(FVector3(0.0f, 2.5f, -5.0f));
    m_Camera.SetRotation(FMath::kPi, -0.35f);
    m_Camera.SetPerspective(
        FMath::DegreesToRadians(45.0f), initAspect, 0.1f, 100.0f);

    // 按依赖顺序创建 GPU 资源:
    // UBO → 纹理 → set0描述符 → 材质系统 → 光照系统 → 管线布局 → Pass系统
    //
    // 场景网格不在此列 —— 它们由 FRenderResourceManager 拥有, 渲染器只在
    // SetRenderObjects 收到本帧视图时读取。渲染器是消费者, 不是所有者。

    ERHIResult result = CreateUniformBuffers();
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[Renderer] Uniform Buffer 创建失败");
        return result;
    }

    result = CreateTextureResources();
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[Renderer] 纹理资源创建失败");
        return result;
    }

    result = CreateDescriptorResources();
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[Renderer] set0 描述符资源创建失败");
        return result;
    }

    // 初始化材质系统 (set 1)
    result = FMaterialManager::Get().Initialize(device, m_Context);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[Renderer] FMaterialManager 初始化失败");
        return result;
    }

    // 创建默认 PBR 材质 —— 供场景层在未指定材质时兜底使用
    m_DefaultMaterial = FMaterialManager::Get().CreateDefaultMaterial("SceneDefaultMaterial");
    if (m_DefaultMaterial == nullptr)
    {
        LIMX_LOG(LogRenderer, Error, "[Renderer] 默认材质创建失败");
        return ERHIResult::ErrorUnknown;
    }

    // 初始化光照系统 (set 2)
    result = FLightManager::Get().Initialize(device, frameCount);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[Renderer] FLightManager 初始化失败");
        return result;
    }

    // 添加默认光源: 主方向光 (阳光风格)
    FLightManager::Get().AddLight(
        FLight::CreateDirectional(
            FVector3(-0.5f, -1.0f, 0.5f),
            FLinearColor(1.0f, 0.98f, 0.95f, 1.0f),
            3.0f));

    // 添加进补光: 点光源 (左侧)
    FLightManager::Get().AddLight(
        FLight::CreatePoint(
            FVector3(-3.0f, 2.0f, -2.0f),
            FLinearColor(0.4f, 0.6f, 1.0f, 1.0f),
            2.0f,
            8.0f));

    // 创建管线布局 (set 0 + set 1 材质 + set 2 光照 + Push Constant)
    result = CreatePipelineLayout();
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[Renderer] 管线布局创建失败");
        return result;
    }

    // 初始化 Pass 系统 —— 阴影 Pass 的 Order 最小, 先于深度预 Pass 执行
    m_ShadowPass   = MakeUnique<FShadowPass>();
    m_DepthPrePass = MakeUnique<FDepthPrePass>();
    m_ForwardPass  = MakeUnique<FForwardPass>();
    m_PostProcessPass = MakeUnique<FPostProcessPass>();
    m_PassManager  = MakeUnique<FPassManager>();

    m_PassManager->RegisterPass(m_ShadowPass.Get());
    m_PassManager->RegisterPass(m_DepthPrePass.Get());
    m_PassManager->RegisterPass(m_ForwardPass.Get());
    m_PassManager->RegisterPass(m_PostProcessPass.Get());

    FRHISwapchainHandle swapchain  = m_Context->GetSwapchain();
    UInt32              imageCount = device->GetSwapchainImageCount(swapchain);

    FPassSetupInfo setupInfo = {};
    setupInfo.Device              = device;
    setupInfo.Swapchain           = swapchain;
    setupInfo.SwapchainFormat     = m_Context->GetSwapchainFormat();
    setupInfo.SwapchainExtent     = m_Context->GetSwapchainExtent();
    setupInfo.SwapchainImageCount = imageCount;
    setupInfo.PipelineLayout      = m_PipelineLayout;

    result = m_PassManager->SetupAll(setupInfo);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[Renderer] PassManager SetupAll 失败");
        return result;
    }

    // 阴影 Pass 需要一份与 set 0 布局兼容的描述符集来放光源矩阵。
    // 必须在 SetupAll 之后 —— 那时阴影贴图才存在; 也必须在
    // CreateLightingDescriptorSets 之前, 后者要写入阴影贴图视图。
    result = m_ShadowPass->CreateLightUniforms(
        device, m_DescSetLayout, m_TextureView, m_Sampler, frameCount);

    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[Renderer] 阴影光源 UBO 创建失败");
        return result;
    }

    // set 2 光照描述符集必须在 SetupAll 之后创建 —— 它的 binding 1 指向
    // 阴影贴图, 而阴影贴图是阴影 Pass 在 Setup 里建的。
    // 管线布局只依赖描述符集**布局**(来自 FLightManager::Initialize),
    // 不依赖描述符集本身, 因此把这一步后移不影响 CreatePipelineLayout。
    result = CreateLightingDescriptorSets();
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[Renderer] 光照描述符集创建失败");
        return result;
    }

    LIMX_LOG(LogRenderer, Log,
             "[Renderer] M0.5 渲染器初始化完成 — PBR 光照+Pass 系统+材质系统就绪");

    return ERHIResult::Success;
}

// ============================================================================
// Shutdown — 释放全部 GPU 管线资源
// ============================================================================

void FRenderer::Shutdown()
{
    if (m_Context == nullptr)
    {
        return;
    }

    IRHIDevice* device = m_Context->GetDevice();
    if (device != nullptr)
    {
        device->WaitIdle();
    }

    // 1. 关闭 Pass 系统 (内部销毁共享深度和帧缓冲)
    if (m_PassManager)
    {
        m_PassManager->ShutdownAll(device);
    }
    m_PassManager.Reset();
    m_ForwardPass.Reset();
    m_DepthPrePass.Reset();

    // 2. 销毁管线布局
    if (device != nullptr)
    {
        device->DestroyPipelineLayout(m_PipelineLayout);
    }

    // 3. 关闭材质系统
    m_DefaultMaterial = nullptr;
    FMaterialManager::Get().Shutdown();

    // 4. 关闭光照系统
    FLightManager::Get().Shutdown();

    // 5. 销毁其他资源
    DestroyTextureResources();
    DestroyBufferResources();

    // 渲染视图只是引用, 清空即可 —— GPU 缓冲区归 FRenderResourceManager
    m_RenderObjects.Clear();

    LIMX_LOG(LogRenderer, Log, "[Renderer] M0.5 渲染器已关闭");

    m_Context = nullptr;
    m_Window  = nullptr;
}

// ============================================================================
// RenderFrame — 单帧渲染
// ============================================================================

void FRenderer::RenderFrame()
{
    if (m_Context == nullptr || m_Window == nullptr)
    {
        return;
    }

    // 窗口最小化时跳过渲染
    if (m_Window->IsMinimized())
    {
        return;
    }

    // 窗口尺寸变化时重建交换链和帧缓冲
    if (m_Window->WasResized())
    {
        if (!IsRHISuccess(RecreateSwapchainResources()))
        {
            return;
        }
    }

    // 帧耗时从这里开始计 —— 前面的最小化/重建分支属于"这一帧没画",
    // 把它们算进平均值会让统计随窗口操作漂移。
    const Float64 frameBeginTime = FPlatformTime::Seconds();

    // 计算帧间隔时间 (deltaTime)
    Float64 currentTime = frameBeginTime;
    Float32 deltaTime = (m_LastFrameTime > 0.0)
        ? static_cast<Float32>(currentTime - m_LastFrameTime)
        : (1.0f / 60.0f);
    m_LastFrameTime = currentTime;

    // 限制 deltaTime 防止窗口拖拽等导致的极端值
    deltaTime = FMath::Clamp(deltaTime, 0.0001f, 0.1f);

    // 处理相机输入 (WASD 移动 + 鼠标右键旋转)
    // 注意: Delta 在上一帧 ProcessMessages 中由 WM_INPUT 累积，
    //       先消费 Delta，再重置
    m_Camera.ProcessInput(deltaTime);

    // 输入系统帧结束 — 重置鼠标 Delta 累加器，为下一帧做准备
    FInputManager::Get().BeginFrame();

    // 开始帧
    ERHIResult result = m_Context->BeginFrame();
    if (result == ERHIResult::ErrorOutOfDate ||
        result == ERHIResult::SuboptimalSwapchain)
    {
        RecreateSwapchainResources();
        return;
    }
    if (!IsRHISuccess(result))
    {
        return;
    }

    // 更新当前帧的 MVP 矩阵
    UInt32 frameIndex = m_Context->GetCurrentFrameIndex();
    UpdateUniformBuffer(frameIndex);

    // 每帧上传材质脏数据
    FMaterialManager::Get().UploadDirtyMaterials();

    // ---- 阴影: 拟合光源视锥 → 写回矩阵 → 更新光源 UBO ----
    //
    // 必须在 UploadLightData 之前: 阴影矩阵是光照 UBO 的一部分, 顺序反了
    // 片段着色器拿到的就是上一帧的矩阵, 快速转动光源时阴影会滞后一帧。
    if (m_ShadowPass)
    {
        const FLightManager& lightManager = FLightManager::Get();

        bool hasDirectionalLight = false;
        FVector3 lightDirection(0.0f, -1.0f, 0.0f);

        for (UInt32 i = 0; i < lightManager.GetLightCount(); ++i)
        {
            const FLight& light = lightManager.GetLight(i);

            if (light.GetType() == ELightType::Directional && light.IsEnabled())
            {
                lightDirection      = light.GetDirection();
                hasDirectionalLight = true;
                break;
            }
        }

        if (hasDirectionalLight && m_SceneBounds.IsValid())
        {
            FShadowPass::FCameraFrustumInfo cameraInfo;
            cameraInfo.Position    = m_Camera.GetPosition();
            cameraInfo.Forward     = m_Camera.GetForwardVector();
            cameraInfo.Up          = FVector3(0.0f, 1.0f, 0.0f);
            cameraInfo.FovY        = m_Camera.GetFovY();
            cameraInfo.AspectRatio = m_Camera.GetAspectRatio();
            cameraInfo.NearPlane   = m_Camera.GetNearPlane();

            // 阴影覆盖距离取场景直径与相机远平面的较小者 —— 场景本身
            // 就那么大时, 把级联铺到几百米外纯属浪费精度。
            const Float32 sceneDiameter =
                m_SceneBounds.GetExtent().Length() * 2.0f;

            cameraInfo.ShadowDistance =
                FMath::Min(sceneDiameter, m_Camera.GetFarPlane());

            m_ShadowPass->SetLightAndBounds(lightDirection, m_SceneBounds,
                                            cameraInfo);
        }

        if (m_ShadowPass->HasValidLight())
        {
            m_ShadowPass->UpdateLightUniform(m_Context->GetDevice(),
                                             frameIndex);

            FCascadedShadowInfo shadowInfo;

            for (UInt32 i = 0; i < FShadowPass::kCascadeCount; ++i)
            {
                shadowInfo.CascadeViewProj[i] =
                    m_ShadowPass->GetCascadeViewProj(i);
                shadowInfo.CascadeSplits[i] = m_ShadowPass->GetCascadeSplit(i);
            }

            shadowInfo.DepthBias = 0.0015f;

            // 法线偏移按最近一级的纹素世界尺寸缩放 —— 用固定值的话,
            // 大场景里偏移不足仍有 acne, 小场景里偏移过大又让阴影脱离物体。
            shadowInfo.NormalBias =
                m_ShadowPass->GetCascadeSplit(0) * 0.004f;

            shadowInfo.ShadowMapSize =
                static_cast<Float32>(FShadowPass::kShadowMapSize);

            FLightManager::Get().SetShadowInfo(shadowInfo);
        }
        else
        {
            FLightManager::Get().DisableShadow();
        }
    }

    // 每帧上传光照 UBO (含当前相机位置)
    FLightManager::Get().UploadLightData(frameIndex, m_Camera.GetPosition());

    // 通过 PassManager 按顺序录制全部 Pass 命令
    IRHICommandBuffer* commandBuffer =
        m_Context->GetCurrentCommandBuffer();
    if (commandBuffer != nullptr)
    {
        FRHIExtent2D extent    = m_Context->GetSwapchainExtent();
        UInt32       imageIndex = m_Context->GetCurrentImageIndex();

        FPassExecuteInfo execInfo = {};
        execInfo.FrameIndex            = frameIndex;
        execInfo.ImageIndex            = imageIndex;
        execInfo.SwapchainExtent       = extent;
        execInfo.RenderObjects         = &m_RenderObjects;
        execInfo.TranslucentObjects    = &m_TranslucentObjects;
        execInfo.ShadowCasterObjects   = &m_ShadowCasterObjects;
        execInfo.ViewProjDescriptorSet = m_DescriptorSets[frameIndex];
        execInfo.PipelineLayout        = m_PipelineLayout;
        execInfo.LightingDescriptorSet = m_LightDescriptorSets[frameIndex];

        m_PassManager->ExecuteAll(commandBuffer, execInfo);
    }

    m_FrameStats.DrawCallCount =
        static_cast<UInt32>(m_RenderObjects.GetSize());

    // 场景 Pass 完成后回调 — 供 UI 渲染叠加等操作录制到同一命令缓冲区
    if (m_PostSceneRenderCallback)
    {
        m_PostSceneRenderCallback();
    }

    // 结束帧 (提交 + 呈现)
    result = m_Context->EndFrame();
    if (result == ERHIResult::ErrorOutOfDate ||
        result == ERHIResult::SuboptimalSwapchain)
    {
        RecreateSwapchainResources();
    }

    RecordFrameTime(static_cast<Float32>(
        (FPlatformTime::Seconds() - frameBeginTime) * 1000.0));
}

// ============================================================================
// 曝光
// ============================================================================

void FRenderer::SetExposure(Float32 exposure)
{
    if (m_PostProcessPass)
    {
        m_PostProcessPass->SetExposure(exposure);
    }
}

Float32 FRenderer::GetExposure() const
{
    return m_PostProcessPass ? m_PostProcessPass->GetExposure() : 1.0f;
}

// ============================================================================
// 帧耗时统计
// ============================================================================

void FRenderer::RecordFrameTime(Float32 frameMilliseconds)
{
    ++m_FrameStats.TotalFrames;

    m_FrameStats.LastFrameMs = frameMilliseconds;

    m_FrameTimeWindow[m_FrameTimeCursor] = frameMilliseconds;
    m_FrameTimeCursor =
        (m_FrameTimeCursor + 1) % FRenderFrameStats::kWindowSize;

    if (m_FrameTimeFilled < FRenderFrameStats::kWindowSize)
    {
        ++m_FrameTimeFilled;
    }

    Float32 total = 0.0f;
    Float32 worst = 0.0f;

    for (UInt32 i = 0; i < m_FrameTimeFilled; ++i)
    {
        total += m_FrameTimeWindow[i];
        worst = FMath::Max(worst, m_FrameTimeWindow[i]);
    }

    m_FrameStats.AverageFrameMs =
        total / static_cast<Float32>(m_FrameTimeFilled);
    m_FrameStats.WorstFrameMs = worst;

    // 平均耗时为零时不做除法 —— 高帧率下单帧耗时可能低于计时器分辨率
    m_FrameStats.AverageFps =
        (m_FrameStats.AverageFrameMs > 0.0f)
            ? 1000.0f / m_FrameStats.AverageFrameMs
            : 0.0f;
}

void FRenderer::ResetFrameStats()
{
    m_FrameStats      = FRenderFrameStats();
    m_FrameTimeCursor = 0;
    m_FrameTimeFilled = 0;

    for (UInt32 i = 0; i < FRenderFrameStats::kWindowSize; ++i)
    {
        m_FrameTimeWindow[i] = 0.0f;
    }
}

// ============================================================================
// OnSwapchainRecreated — 重建帧缓冲
// ============================================================================

ERHIResult FRenderer::OnSwapchainRecreated()
{
    IRHIDevice*         device    = m_Context->GetDevice();
    FRHISwapchainHandle swapchain = m_Context->GetSwapchain();
    FRHIExtent2D        newExtent  = m_Context->GetSwapchainExtent();
    UInt32              imageCount = device->GetSwapchainImageCount(swapchain);

    return m_PassManager->OnResizeAll(device, swapchain, newExtent, imageCount);
}

// ============================================================================
// RecreateSwapchainResources — 完整交换链资源重建
// ============================================================================

ERHIResult FRenderer::RecreateSwapchainResources()
{
    if (m_Context == nullptr || m_PassManager == nullptr)
    {
        return ERHIResult::ErrorInvalidParameter;
    }

    IRHIDevice* device = m_Context->GetDevice();
    if (device == nullptr)
    {
        return ERHIResult::ErrorInvalidHandle;
    }

    device->WaitIdle();
    m_PassManager->ReleaseSwapchainResources(device);

    ERHIResult result = m_Context->RecreateSwapchain();
    if (!IsRHISuccess(result))
    {
        return result;
    }

    result = OnSwapchainRecreated();
    if (m_Window != nullptr)
    {
        m_Window->ResetResizedFlag();
    }

    return result;
}

// ============================================================================
// CreateTextureResources — 程序化棋盘格纹理 + 采样器 + 纹理视图
// ============================================================================

ERHIResult FRenderer::CreateTextureResources()
{
    IRHIDevice* device = m_Context->GetDevice();

    // ---- 生成 256×256 RGBA8 棋盘格纹理数据 ----
    constexpr UInt32 kTexWidth  = 256;
    constexpr UInt32 kTexHeight = 256;
    constexpr UInt32 kCellSize  = 32;   // 每个格子 32×32 像素, 8×8 棋盘格
    constexpr UInt64 kTexDataSize =
        static_cast<UInt64>(kTexWidth) * kTexHeight * 4;

    // 在栈上生成纹理像素数据 (256KB, 可接受)
    UInt8 texPixels[kTexWidth * kTexHeight * 4];

    for (UInt32 row = 0; row < kTexHeight; ++row)
    {
        for (UInt32 col = 0; col < kTexWidth; ++col)
        {
            UInt32 cellX = col / kCellSize;
            UInt32 cellY = row / kCellSize;
            bool isWhite = ((cellX + cellY) % 2) == 0;

            UInt32 pixelIndex = (row * kTexWidth + col) * 4;
            UInt8 colorValue = isWhite
                ? static_cast<UInt8>(230)
                : static_cast<UInt8>(40);

            texPixels[pixelIndex + 0] = colorValue;   // R
            texPixels[pixelIndex + 1] = colorValue;   // G
            texPixels[pixelIndex + 2] = colorValue;   // B
            texPixels[pixelIndex + 3] = 255;           // A
        }
    }

    // ---- 创建 GPU 纹理 ----
    FRHITextureDesc texDesc = FRHITextureDesc::Texture2D(
        kTexWidth, kTexHeight,
        EPixelFormat::RGBA8_UNORM,
        1,
        ETextureUsage::Sampled | ETextureUsage::TransferDst);
    texDesc.DebugName = "CheckerboardTexture";

    ERHIResult result = device->CreateTexture(texDesc, m_Texture);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
                 "[Renderer] 棋盘格纹理创建失败");
        return result;
    }

    // ---- 创建 Staging Buffer 并上传纹理数据 ----
    FRHIBufferDesc stagingDesc = FRHIBufferDesc::Staging(kTexDataSize);
    stagingDesc.DebugName = "TextureStagingBuffer";

    FRHIBufferHandle stagingBuffer;
    result = device->CreateBuffer(stagingDesc, stagingBuffer);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
                 "[Renderer] 纹理 Staging Buffer 创建失败");
        return result;
    }

    // 映射 Staging Buffer 并拷贝像素数据
    void* mappedPtr = nullptr;
    result = device->MapBuffer(stagingBuffer, &mappedPtr);
    if (!IsRHISuccess(result))
    {
        device->DestroyBuffer(stagingBuffer);
        return result;
    }

    MemCopy(mappedPtr, texPixels, kTexDataSize);
    device->UnmapBuffer(stagingBuffer);

    // ---- 通过一次性命令缓冲区上传纹理 ----
    IRHICommandBuffer* cmdBuffer = m_Context->BeginSingleTimeCommands();

    // 布局转换: Undefined → TransferDst
    cmdBuffer->TransitionImageLayout(
        m_Texture,
        EImageLayout::Undefined,
        EImageLayout::TransferDst,
        EPipelineStageFlags::TopOfPipe,
        EPipelineStageFlags::Transfer,
        EAccessFlags::None,
        EAccessFlags::TransferWrite);

    // 拷贝 Staging Buffer → 纹理
    FRHIBufferTextureCopyRegion copyRegion = {};
    copyRegion.BufferOffset      = 0;
    copyRegion.BufferRowLength   = 0;
    copyRegion.BufferImageHeight = 0;
    copyRegion.MipLevel          = 0;
    copyRegion.BaseLayer         = 0;
    copyRegion.LayerCount        = 1;
    copyRegion.TextureOffset     = { 0, 0, 0 };
    copyRegion.TextureExtent     = { kTexWidth, kTexHeight, 1 };

    cmdBuffer->CopyBufferToTexture(
        stagingBuffer,
        m_Texture,
        EImageLayout::TransferDst,
        copyRegion);

    // 布局转换: TransferDst → ShaderReadOnly
    cmdBuffer->TransitionImageLayout(
        m_Texture,
        EImageLayout::TransferDst,
        EImageLayout::ShaderReadOnly,
        EPipelineStageFlags::Transfer,
        EPipelineStageFlags::FragmentShader,
        EAccessFlags::TransferWrite,
        EAccessFlags::ShaderRead);

    m_Context->EndSingleTimeCommands(cmdBuffer);

    // 释放 Staging Buffer
    device->DestroyBuffer(stagingBuffer);

    // ---- 创建纹理视图 ----
    FRHITextureViewDesc viewDesc = {};
    viewDesc.Texture         = m_Texture;
    viewDesc.ViewType        = ETextureType::Texture2D;
    viewDesc.Format          = EPixelFormat::RGBA8_UNORM;
    viewDesc.BaseMipLevel    = 0;
    viewDesc.MipLevelCount   = 1;
    viewDesc.BaseArrayLayer  = 0;
    viewDesc.ArrayLayerCount = 1;

    result = device->CreateTextureView(viewDesc, m_TextureView);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
                 "[Renderer] 纹理视图创建失败");
        return result;
    }

    // ---- 创建采样器 (线性过滤 + 重复寻址) ----
    FRHISamplerDesc samplerDesc = FRHISamplerDesc::LinearRepeat();

    result = device->CreateSampler(samplerDesc, m_Sampler);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
                 "[Renderer] 采样器创建失败");
        return result;
    }

    LIMX_LOG(LogRenderer, Log,
             "[Renderer] 纹理资源创建完成 — {}x{} RGBA8 棋盘格 + 线性采样器",
             kTexWidth, kTexHeight);

    return ERHIResult::Success;
}

// ============================================================================
// DestroyTextureResources — 销毁纹理 + 纹理视图 + 采样器
// ============================================================================

void FRenderer::DestroyTextureResources()
{
    if (m_Context == nullptr)
    {
        return;
    }

    IRHIDevice* device = m_Context->GetDevice();
    if (device == nullptr)
    {
        return;
    }

    device->DestroySampler(m_Sampler);
    device->DestroyTextureView(m_TextureView);
    device->DestroyTexture(m_Texture);
}

// ============================================================================
// CreateUniformBuffers — 每帧一个 View+Proj Uniform Buffer (128 bytes)
// ============================================================================

ERHIResult FRenderer::CreateUniformBuffers()
{
    IRHIDevice* device = m_Context->GetDevice();

    // 每个并行帧一个 Uniform Buffer，避免写入冲突
    UInt32 frameCount = m_Context->GetMaxFramesInFlight();
    m_UniformBuffers.Reserve(frameCount);

    for (UInt32 i = 0; i < frameCount; ++i)
    {
        FRHIBufferDesc bufferDesc =
            FRHIBufferDesc::Uniform(sizeof(FViewProjUBO));
        bufferDesc.DebugName = "ViewProjUniformBuffer";

        FRHIBufferHandle buffer;
        ERHIResult result = device->CreateBuffer(bufferDesc, buffer);
        if (!IsRHISuccess(result))
        {
            return result;
        }

        m_UniformBuffers.Add(buffer);
    }

    LIMX_LOG(LogRenderer, Log,
             "[Renderer] Uniform Buffer 创建完成 — {} 个帧, 每帧 {} 字节",
             frameCount,
             static_cast<UInt64>(sizeof(FViewProjUBO)));

    return ERHIResult::Success;
}

// ============================================================================
// CreateDescriptorResources — 描述符集布局 + 分配描述符集 + 写入绑定
// ============================================================================

ERHIResult FRenderer::CreateDescriptorResources()
{
    IRHIDevice* device = m_Context->GetDevice();
    UInt32 frameCount = m_Context->GetMaxFramesInFlight();

    // ---- 创建描述符集布局: binding=0 UBO, binding=1 CombinedImageSampler ----
    FRHIDescriptorBinding bindings[2] = {};

    // binding 0: MVP Uniform Buffer (顶点着色器)
    bindings[0].Binding    = 0;
    bindings[0].Type       = EDescriptorType::UniformBuffer;
    bindings[0].Count      = 1;
    bindings[0].StageFlags = EShaderStage::Vertex;

    // binding 1: 纹理采样器 (片段着色器)
    bindings[1].Binding    = 1;
    bindings[1].Type       = EDescriptorType::CombinedImageSampler;
    bindings[1].Count      = 1;
    bindings[1].StageFlags = EShaderStage::Fragment;

    FRHIDescSetLayoutDesc layoutDesc = {};
    layoutDesc.Bindings     = bindings;
    layoutDesc.BindingCount = 2;
    layoutDesc.DebugName    = "MVPTexDescSetLayout";

    ERHIResult result =
        device->CreateDescSetLayout(layoutDesc, m_DescSetLayout);
    if (!IsRHISuccess(result))
    {
        return result;
    }

    // ---- 为每帧分配描述符集 ----
    m_DescriptorSets.Reserve(frameCount);

    for (UInt32 i = 0; i < frameCount; ++i)
    {
        FRHIDescriptorSetHandle descSet;
        result = device->AllocateDescriptorSet(m_DescSetLayout, descSet);
        if (!IsRHISuccess(result))
        {
            return result;
        }

        m_DescriptorSets.Add(descSet);

        // 将 Uniform Buffer + 纹理采样器绑定到描述符集
        FRHIDescriptorWrite writes[2] = {};

        // binding 0: View+Proj Uniform Buffer
        writes[0] = FRHIDescriptorWrite::UniformBuffer(
            descSet,
            0,                       // binding = 0
            m_UniformBuffers[i],
            0,                       // offset = 0
            sizeof(FViewProjUBO)     // range = 128 bytes
        );

        // binding 1: 棋盘格纹理 + 采样器
        writes[1] = FRHIDescriptorWrite::CombinedImageSampler(
            descSet,
            1,                       // binding = 1
            m_TextureView,
            m_Sampler,
            EImageLayout::ShaderReadOnly
        );

        device->UpdateDescriptorSets(writes, 2);
    }

    LIMX_LOG(LogRenderer, Log,
             "[Renderer] 描述符资源创建完成 — {} 个描述符集",
             frameCount);

    return ERHIResult::Success;
}

// ============================================================================
// CreatePipelineLayout — 3 套描述符集布局 + Push Constant (Model mat4)
// set 0: ViewProj UBO + 纹理  |  set 1: 材质  |  set 2: 光照 UBO
// ============================================================================

ERHIResult FRenderer::CreatePipelineLayout()
{
    IRHIDevice* device = m_Context->GetDevice();

    // Push Constant: 逐物体 Model 矩阵 (mat4 = 64 bytes, 顶点着色器可见)
    FRHIPushConstantRange pushConstantRange = {};
    pushConstantRange.StageFlags = EShaderStage::Vertex;
    pushConstantRange.Offset     = 0;
    pushConstantRange.Size       = sizeof(FModelPushConstant);

    // 3 套描述符集布局:
    //   set 0 = m_DescSetLayout       (ViewProj UBO + 棋盘格纹理)
    //   set 1 = FMaterialManager 布局  (材质 UBO + 5 个纹理采样器)
    //   set 2 = FLightManager 布局    (光照 UBO)
    FRHIDescSetLayoutHandle setLayouts[3] =
    {
        m_DescSetLayout,
        FMaterialManager::Get().GetDescSetLayout(),
        FLightManager::Get().GetDescSetLayout()
    };

    FRHIPipelineLayoutDesc layoutDesc = {};
    layoutDesc.SetLayouts             = setLayouts;
    layoutDesc.SetLayoutCount         = 3;
    layoutDesc.PushConstantRanges     = &pushConstantRange;
    layoutDesc.PushConstantRangeCount = 1;
    layoutDesc.DebugName              = "PBRPipelineLayout";

    ERHIResult result = device->CreatePipelineLayout(layoutDesc, m_PipelineLayout);

    if (IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Log,
                 "[Renderer] PBR 管线布局创建完成 (set0=ViewProj, set1=材质, set2=光照)");
    }

    return result;
}

// ============================================================================
// CreateLightingDescriptorSets — 为每帧分配 set 2 光照描述符集
// ============================================================================

ERHIResult FRenderer::CreateLightingDescriptorSets()
{
    IRHIDevice* device     = m_Context->GetDevice();
    UInt32      frameCount = m_Context->GetMaxFramesInFlight();

    FRHIDescSetLayoutHandle lightLayout = FLightManager::Get().GetDescSetLayout();

    m_LightDescriptorSets.Reserve(frameCount);

    for (UInt32 i = 0; i < frameCount; ++i)
    {
        FRHIDescriptorSetHandle descSet;
        ERHIResult result = device->AllocateDescriptorSet(lightLayout, descSet);
        if (!IsRHISuccess(result))
        {
            LIMX_LOG(LogRenderer, Error,
                     "[Renderer] 光照描述符集分配失败 (帧 {})", i);
            return result;
        }

        // set 2 binding 0 = 光照 UBO, binding 1 = 阴影贴图
        //
        // 阴影贴图在整个运行期都是同一张纹理, 因此只需在这里写一次;
        // 内容每帧被阴影 Pass 重写, 但描述符指向的对象不变。
        FRHIDescriptorWrite writes[2];

        writes[0] = FRHIDescriptorWrite::UniformBuffer(
            descSet,
            0,
            FLightManager::Get().GetLightUBO(i),
            0,
            sizeof(FLightingUBO));

        writes[1] = FRHIDescriptorWrite::CombinedImageSampler(
            descSet,
            1,
            m_ShadowPass->GetShadowMapView(),
            m_ShadowPass->GetShadowSampler(),
            EImageLayout::ShaderReadOnly);

        device->UpdateDescriptorSets(writes, 2);

        m_LightDescriptorSets.Add(descSet);
    }

    LIMX_LOG(LogRenderer, Log,
             "[Renderer] 光照描述符集创建完成 — {} 个帧, 每帧 sizeof(FLightingUBO)={} 字节",
             frameCount,
             static_cast<UInt64>(sizeof(FLightingUBO)));

    return ERHIResult::Success;
}

// ============================================================================
// DestroyBufferResources — 销毁缓冲区和描述符资源
// ============================================================================

void FRenderer::DestroyBufferResources()
{
    if (m_Context == nullptr)
    {
        return;
    }

    IRHIDevice* device = m_Context->GetDevice();
    if (device == nullptr)
    {
        return;
    }

    // set 2 光照描述符集
    for (SizeType i = 0; i < m_LightDescriptorSets.GetSize(); ++i)
    {
        device->FreeDescriptorSet(m_LightDescriptorSets[i]);
    }
    m_LightDescriptorSets.Clear();

    // set 0 描述符集
    for (SizeType i = 0; i < m_DescriptorSets.GetSize(); ++i)
    {
        device->FreeDescriptorSet(m_DescriptorSets[i]);
    }
    m_DescriptorSets.Clear();

    // set 0 描述符集布局
    device->DestroyDescSetLayout(m_DescSetLayout);

    // ViewProj Uniform Buffer
    for (SizeType i = 0; i < m_UniformBuffers.GetSize(); ++i)
    {
        device->DestroyBuffer(m_UniformBuffers[i]);
    }
    m_UniformBuffers.Clear();
}

// ============================================================================
// UpdateUniformBuffer — 每帧更新 View+Proj 矩阵 (Model 通过 Push Constant)
// ============================================================================

void FRenderer::UpdateUniformBuffer(UInt32 frameIndex)
{
    IRHIDevice* device = m_Context->GetDevice();

    // 更新相机宽高比 (窗口尺寸可能变化)
    FRHIExtent2D extent = m_Context->GetSwapchainExtent();
    Float32 aspectRatio = static_cast<Float32>(extent.Width) /
                          static_cast<Float32>(FMath::Max(extent.Height, 1u));
    m_Camera.SetAspectRatio(aspectRatio);

    // 仅写入 View + Projection (Model 在 Pass::Execute 中通过 Push Constant 逐物体推送)
    FViewProjUBO viewProj;
    viewProj.View = m_Camera.GetViewMatrix();
    viewProj.Proj = m_Camera.GetProjectionMatrix();

    // 映射并写入 Uniform Buffer
    void* mappedPtr = nullptr;
    ERHIResult result =
        device->MapBuffer(m_UniformBuffers[frameIndex], &mappedPtr);
    if (IsRHISuccess(result))
    {
        MemCopy(mappedPtr, &viewProj, sizeof(FViewProjUBO));
        device->UnmapBuffer(m_UniformBuffers[frameIndex]);
    }
}

} // namespace Limx
