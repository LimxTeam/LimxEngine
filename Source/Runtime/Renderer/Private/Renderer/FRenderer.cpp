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
#include "Core/Math/FHalton.h"
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
#include "Renderer/RenderPass/FShadowAtlasPass.h"
#include "Renderer/RenderPass/FClusterLightPass.h"
#include "Renderer/RenderPass/FGpuCullPass.h"
#include "Renderer/RenderPass/FMeshletCullPass.h"
#include "Renderer/RenderPass/FMeshletDepthPass.h"
#include "Renderer/RenderPass/FRayTracedShadowPass.h"
#include "Renderer/RenderPass/FRayTracedAoPass.h"
#include "Renderer/RenderPass/FRayTracedReflectionPass.h"
#include "Renderer/RenderPass/FTaaPass.h"
#include "Renderer/RenderPass/FGtaoPass.h"
#include "Renderer/RenderPass/FBloomPass.h"
#include "Renderer/RenderPass/FSkyPass.h"
#include "RenderCore/Environment/FEnvironmentMap.h"
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

    // GPU 计时器 —— 硬件不支持时会自行停用, 不影响后续初始化
    m_GpuProfiler.Initialize(device);

    // 并行命令录制器
    if (m_ParallelRecording)
    {
        const ERHIResult recorderResult =
            m_Recorder.Initialize(device, frameCount, m_RecordThreadCount);

        if (recorderResult != ERHIResult::Success)
        {
            LIMX_LOG(LogRenderer, Warning,
                     "[Renderer] 并行录制器初始化失败, 退回内联录制");
            m_ParallelRecording = false;
        }
    }

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
    // bindless 表 —— 必须在 FMaterialManager 之后, 占位纹理来自它
    {
        const ERHIResult bindlessResult = m_BindlessTable.Initialize(
            device, frameCount,
            FMaterialManager::Get().GetDefaultTextureView(),
            FMaterialManager::Get().GetDefaultSampler());

        if (!IsRHISuccess(bindlessResult))
        {
            LIMX_LOG(LogRenderer, Error, "[Renderer] bindless 表初始化失败");
            return bindlessResult;
        }
    }

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
    result = CreateDrawObjectSetLayout();
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[Renderer] 逐物体描述符集布局创建失败");
        return result;
    }

    result = CreatePipelineLayout();
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[Renderer] 管线布局创建失败");
        return result;
    }

    // 初始化 Pass 系统 —— 阴影 Pass 的 Order 最小, 先于深度预 Pass 执行
    m_ShadowPass      = MakeUnique<FShadowPass>();
    m_ShadowAtlasPass = MakeUnique<FShadowAtlasPass>();
    m_DepthPrePass = MakeUnique<FDepthPrePass>();
    m_SkyPass      = MakeUnique<FSkyPass>();
    m_ForwardPass  = MakeUnique<FForwardPass>();
    m_PostProcessPass = MakeUnique<FPostProcessPass>();
    m_PassManager  = MakeUnique<FPassManager>();
    m_RayTracedShadowPass = MakeUnique<FRayTracedShadowPass>();
    m_RayTracedAoPass     = MakeUnique<FRayTracedAoPass>();
    m_RayTracedReflectionPass = MakeUnique<FRayTracedReflectionPass>();

    m_ClusterLightPass = MakeUnique<FClusterLightPass>();
    m_GpuCullPass      = MakeUnique<FGpuCullPass>();
    m_MeshletCullPass  = MakeUnique<FMeshletCullPass>();
    m_MeshletDepthPass = MakeUnique<FMeshletDepthPass>();
    m_TaaPass          = MakeUnique<FTaaPass>();
    m_GtaoPass         = MakeUnique<FGtaoPass>();
    m_BloomPass        = MakeUnique<FBloomPass>();

    m_PassManager->RegisterPass(m_ShadowPass.Get());
    m_PassManager->RegisterPass(m_ShadowAtlasPass.Get());
    m_PassManager->RegisterPass(m_GpuCullPass.Get());
    m_PassManager->RegisterPass(m_MeshletCullPass.Get());
    m_PassManager->RegisterPass(m_MeshletDepthPass.Get());
    m_PassManager->RegisterPass(m_ClusterLightPass.Get());
    m_PassManager->RegisterPass(m_GtaoPass.Get());
    m_PassManager->RegisterPass(m_TaaPass.Get());
    m_PassManager->RegisterPass(m_BloomPass.Get());
    m_PassManager->RegisterPass(m_DepthPrePass.Get());
    m_PassManager->RegisterPass(m_RayTracedShadowPass.Get());
    m_PassManager->RegisterPass(m_RayTracedAoPass.Get());
    m_PassManager->RegisterPass(m_RayTracedReflectionPass.Get());
    m_PassManager->RegisterPass(m_SkyPass.Get());
    m_PassManager->RegisterPass(m_ForwardPass.Get());

    // 前向 Pass 是目前唯一批次数足以受益于并行录制的通道 —— 阴影与深度
    // 预通道的绘制命令更简单, 且它们各自的批次列表就是前向的子集。
    // 等 Day 9 的 GPU 驱动剔除落地, 这几条路径会一起重排。
    if (m_ParallelRecording && m_Recorder.IsInitialized())
    {
        m_ForwardPass->SetRecorder(&m_Recorder);

        // 阴影 Pass 才是 CPU 的大头 —— 逐 Pass 实测 8.17 ms 录制 / 0.66 ms
        // GPU, 占整帧 55%。三级级联乘以全部投射体, 而前向只有 2.48 ms。
        m_ShadowPass->SetRecorder(&m_Recorder);
        m_DepthPrePass->SetRecorder(&m_Recorder);
    }
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
    setupInfo.ViewProjSetLayout   = m_DescSetLayout;
    setupInfo.DrawObjectSetLayout = m_DrawObjectSetLayout;
    setupInfo.MaxFramesInFlight   = m_Context->GetMaxFramesInFlight();

    result = m_PassManager->SetupAll(setupInfo);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[Renderer] PassManager SetupAll 失败");
        return result;
    }

    // IBL 占位图必须在光照描述符集之前建好 —— 后者的 binding 2/3/4 要写它
    result = CreateFallbackCubeMap();
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[Renderer] IBL 占位立方体贴图创建失败");
        return result;
    }

    result = CreateFallbackLut();
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error, "[Renderer] BRDF 占位查找表创建失败");
        return result;
    }

    // TAA 的速度输入与 GTAO 的深度/法线输入都来自深度预通道的附件 ——
    // 必须在 SetupAll 之后接, 那时它们才存在。
    if (m_TaaPass && m_DepthPrePass)
    {
        m_TaaPass->SetVelocityView(m_DepthPrePass->GetVelocityView());
    }

    if (m_GtaoPass && m_DepthPrePass && m_PassManager)
    {
        if (m_RayTracedShadowPass)
        {
            m_RayTracedShadowPass->SetInputs(
                m_DepthPrePass->GetSharedDepthTexture(),
                m_PassManager->GetSharedDepthView(),
                m_DepthPrePass->GetNormalView());
        }

        if (m_RayTracedAoPass)
        {
            m_RayTracedAoPass->SetInputs(
                m_DepthPrePass->GetSharedDepthTexture(),
                m_PassManager->GetSharedDepthView(),
                m_DepthPrePass->GetNormalView());
        }

        if (m_RayTracedReflectionPass)
        {
            m_RayTracedReflectionPass->SetInputs(
                m_DepthPrePass->GetSharedDepthTexture(),
                m_PassManager->GetSharedDepthView(),
                m_DepthPrePass->GetNormalView());
        }

        m_GtaoPass->SetInputs(m_DepthPrePass->GetSharedDepthTexture(),
                              m_PassManager->GetSharedDepthView(),
                              m_DepthPrePass->GetNormalView());
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

    m_GpuProfiler.Shutdown(device);
    m_Recorder.Shutdown(device);
    m_BindlessTable.Shutdown(device);

    // 1. 关闭 Pass 系统 (内部销毁共享深度和帧缓冲)
    if (m_PassManager)
    {
        m_PassManager->ShutdownAll(device);
    }
    m_PassManager.Reset();
    m_PostProcessPass.Reset();
    m_ForwardPass.Reset();
    m_SkyPass.Reset();
    m_DepthPrePass.Reset();
    m_RayTracingScene.Shutdown();
    m_RayTracedShadowPass.Reset();
    m_RayTracedAoPass.Reset();
    m_RayTracedReflectionPass.Reset();
    m_GpuCullPass.Reset();
    m_ShadowAtlasPass.Reset();
    m_ShadowPass.Reset();

    // 2. 销毁管线布局
    if (device != nullptr)
    {
        device->DestroyPipelineLayout(m_PipelineLayout);
        device->DestroyDescSetLayout(m_DrawObjectSetLayout);
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

bool FRenderer::IsRayTracedReflectionEnabled() const
{
    return m_RayTracedReflectionPass &&
           m_RayTracedReflectionPass->IsEnabled();
}

bool FRenderer::SetRayTracedReflectionEnabled(bool enabled)
{
    if (!m_RayTracedReflectionPass)
    {
        return false;
    }

    if (!enabled)
    {
        m_RayTracedReflectionPass->SetEnabled(false);
        return true;
    }

    if (!SetRayTracingEnabled(true))
    {
        return false;
    }

    m_RayTracedReflectionPass->SetEnabled(true);
    return true;
}

bool FRenderer::IsRayTracedAoEnabled() const
{
    return m_RayTracedAoPass && m_RayTracedAoPass->IsEnabled();
}

bool FRenderer::SetRayTracedAoEnabled(bool enabled)
{
    if (!m_RayTracedAoPass)
    {
        return false;
    }

    if (!enabled)
    {
        m_RayTracedAoPass->SetEnabled(false);
        return true;
    }

    if (!SetRayTracingEnabled(true))
    {
        return false;
    }

    m_RayTracedAoPass->SetEnabled(true);
    return true;
}

bool FRenderer::IsRayTracedShadowsEnabled() const
{
    return m_RayTracedShadowPass && m_RayTracedShadowPass->IsEnabled();
}

bool FRenderer::SetRayTracedShadowsEnabled(bool enabled)
{
    if (!m_RayTracedShadowPass)
    {
        return false;
    }

    if (!enabled)
    {
        m_RayTracedShadowPass->SetEnabled(false);
        return true;
    }

    // 光追阴影要读加速结构 —— 顺带把它打开。
    //
    // 不顺带打开的话, 调用方要记住"先开加速结构再开阴影", 而忘了的表现
    // 是掩码保持上一帧内容: 静止场景里那与正确结果完全一样。
    if (!SetRayTracingEnabled(true))
    {
        return false;
    }

    m_RayTracedShadowPass->SetEnabled(true);
    return true;
}

bool FRenderer::SetRayTracingEnabled(bool enabled)
{
    if (!enabled)
    {
        m_RayTracingEnabled = false;
        return true;
    }

    if (m_Context == nullptr || m_Context->GetDevice() == nullptr)
    {
        return false;
    }

    if (!m_Context->GetDevice()->IsRayTracingSupported())
    {
        // 返回 false 而不是静默降级。
        //
        // 静默降级的后果是调用方以为光追开着, 而任何依赖它的判据都在一棵
        // 不存在的树上通过 —— "跳过"绝不能表现为"通过"。
        LIMX_LOG(LogRenderer, Error,
                 "[光追场景] 设备不支持光线追踪 — 无法启用");
        return false;
    }

    if (!m_RayTracingScene.IsValid())
    {
        if (!IsRHISuccess(m_RayTracingScene.Initialize(m_Context->GetDevice())))
        {
            return false;
        }
    }

    m_RayTracingEnabled = true;
    return true;
}

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
    const Float64 acquireBegin = FPlatformTime::Seconds();

    ERHIResult result = m_Context->BeginFrame();

    const Float64 acquireEnd = FPlatformTime::Seconds();
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

    const Float64 updateBegin = acquireEnd;

    // GPU 计时开帧 —— 必须在任何 RenderPass 之外, 因为要重置查询池
    {
        IRHICommandBuffer* timingBuffer =
            m_Context->GetCurrentCommandBuffer();

        if (timingBuffer != nullptr)
        {
            m_GpuProfiler.BeginFrame(timingBuffer, m_GpuFrameNumber);
        }
    }

    if (m_ParallelRecording && m_Recorder.IsInitialized())
    {
        m_Recorder.BeginFrame(frameIndex);
    }

    // ---- 光追加速结构 ----
    //
    // 必须排在所有通道之前: 任何读 TLAS 的着色器都要求它这一帧已经建好。
    // 排在后面的话读到的是上一帧的树 —— 静止场景里那与正确结果完全相同,
    // 直到物体开始动。
    //
    // 用的是**未经相机剔除**的投射体列表: 光线会打到视锥之外的东西,
    // 拿剔除后的列表建树, 反射里就会缺掉屏幕外的物体。
    if (m_RayTracingEnabled && m_RayTracingScene.IsValid())
    {
        if (!IsRHISuccess(m_RayTracingScene.Update(m_ShadowCasterObjects)))
        {
            LIMX_LOG(LogRenderer, Error, "[光追场景] 本帧更新失败");
        }

        IRHICommandBuffer* rtCommandBuffer =
            m_Context->GetCurrentCommandBuffer();

        m_RayTracingScene.RecordBuild(rtCommandBuffer);
    }

    // 光追阴影通道每帧要知道: 树在哪、相机在哪、照哪盏灯
    if (m_RayTracedShadowPass && m_RayTracedShadowPass->IsEnabled())
    {
        m_RayTracedShadowPass->SetTlas(m_RayTracingScene.GetTlas());

        m_RayTracedShadowPass->SetCameraParams(
            m_Camera.GetProjectionMatrix() * m_Camera.GetViewMatrix(),
            m_Camera.GetNearPlane(), m_Camera.GetFarPlane());

        // 第一盏投影的光源。
        //
        // 一次只处理一盏 —— 掩码是单通道的。选"第一盏投影的"而不是
        // "第一盏", 是因为不投影的光源根本不需要算阴影, 而把算力花在
        // 它身上不会有任何画面变化, 于是"选错了灯"这件事看不出来。
        const FLightManager& lights = FLightManager::Get();

        bool   found            = false;
        UInt32 shadowLightIndex = 0;

        for (UInt32 i = 0; i < lights.GetActiveLightCount(); ++i)
        {
            const FLight& light = lights.GetLight(i);

            if (light.CastsShadow())
            {
                m_RayTracedShadowPass->SetLight(light.ToGpuData());
                shadowLightIndex = i;
                found = true;
                break;
            }
        }

        // 连续若干帧都没有光源才报, 而且只报一次。
        //
        // 启动时光追阴影是在场景加载之前打开的, 于是最初几帧必然还没有光源
        // —— 那是正常的启动过程。每帧一条 Error 的话, 正常启动看起来像出了
        // 故障, 而**真正持续没有光源**的情形反而淹没在里面。
        //
        // 门槛取 30 帧: 场景加载再慢也就几帧, 而持续三十帧没有投影光源
        // 说明这个场景真的没有 —— 那时光追阴影的掩码一直是陈的, 值得说。
        constexpr UInt32 kNoLightWarnFrames = 30;

        if (found)
        {
            m_RayTracedShadowNoLightFrames = 0;
        }
        else if (m_RayTracedShadowNoLightFrames <= kNoLightWarnFrames)
        {
            ++m_RayTracedShadowNoLightFrames;

            if (m_RayTracedShadowNoLightFrames == kNoLightWarnFrames)
            {
                LIMX_LOG(LogRenderer, Warning,
                         "[光追阴影] 连续 {} 帧没有投影的光源 — "
                         "掩码一直是上一帧的内容",
                         kNoLightWarnFrames);
            }
        }

        FLightManager::Get().SetRayTracedShadowLight(
            found ? static_cast<Int32>(shadowLightIndex) : -1);
    }
    else
    {
        // 关掉时必须写回 -1, 否则着色器会继续读一张不再更新的掩码 ——
        // 静止场景里那与"还开着"完全一样。
        FLightManager::Get().SetRayTracedShadowLight(-1);
    }

    // 光追产出的开关 —— 写进光照 UBO, 让着色器知道该读哪一张图。
    //
    // 由这里决定而不是让着色器按图的内容猜: 一张全 1 的 AO 与"AO 通道
    // 没跑"长得一模一样, 而那正是最需要分开的两件事。
    {
        Float32 flags = 0.0f;

        if (m_RayTracedAoPass && m_RayTracedAoPass->IsEnabled())
        {
            flags += 1.0f;
        }

        if (m_RayTracedReflectionPass &&
            m_RayTracedReflectionPass->IsEnabled())
        {
            flags += 2.0f;
        }

        FLightManager::Get().SetRayTracedFlags(flags);
    }

    if (m_RayTracedAoPass && m_RayTracedAoPass->IsEnabled())
    {
        m_RayTracedAoPass->SetTlas(m_RayTracingScene.GetTlas());
        m_RayTracedAoPass->SetCameraParams(
            m_Camera.GetProjectionMatrix() * m_Camera.GetViewMatrix(),
            m_Camera.GetPosition());

        m_RayTracedAoPass->SetDepthRange(m_Camera.GetNearPlane(),
                                         m_Camera.GetFarPlane());
    }

    if (m_RayTracedReflectionPass && m_RayTracedReflectionPass->IsEnabled())
    {
        m_RayTracedReflectionPass->SetTlas(m_RayTracingScene.GetTlas());

        m_RayTracedReflectionPass->SetCameraParams(
            m_Camera.GetProjectionMatrix() * m_Camera.GetViewMatrix(),
            m_Camera.GetPosition());

        m_RayTracedReflectionPass->SetSceneBuffers(
            m_RayTracingScene.GetGeometryTable(),
            m_RayTracingScene.GetGeometryTableBytes(),
            m_BindlessTable.GetMaterialBuffer(frameIndex),
            m_BindlessTable.GetMaterialBufferBytes());

        // 主光源 —— 与光追阴影选同一盏 (第一盏投影的)
        const FLightManager& reflectionLights = FLightManager::Get();

        for (UInt32 i = 0; i < reflectionLights.GetActiveLightCount(); ++i)
        {
            const FLight& light = reflectionLights.GetLight(i);

            if (light.CastsShadow())
            {
                m_RayTracedReflectionPass->SetLight(light.ToGpuData());
                break;
            }
        }
    }

    // 材质表每帧整体上传 —— 见 FBindlessTable::Upload 的说明
    m_BindlessTable.Upload(frameIndex);

    // 每帧上传材质脏数据
    FMaterialManager::Get().UploadDirtyMaterials(&m_BindlessTable);

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

    // GPU 驱动剔除的视锥 —— 用**不含抖动**的投影矩阵
    //
    // 抖动每帧改变亚像素偏移, 用它算出的视锥边界会逐帧漂移。漂移本身无害
    // (远小于一个物体), 但会让"GPU 路径与 CPU 路径一致"这条判据变成逐帧
    // 不同, 无法比对。
    //
    // FCamera::GetProjectionMatrix() 本来就是不含抖动的 —— 抖动只加在
    // UpdateUniformBuffer 里的一份拷贝上, 相机自己那份从不被改。
    if (m_GpuCullPass)
    {
        m_GpuCullPass->SetViewFrustum(
            FGpuCullPass::kCameraView,
            FFrustum::FromViewProjection(m_Camera.GetProjectionMatrix() *
                                         m_Camera.GetViewMatrix()));

        // meshlet 剔除用**同一个**视锥。
        //
        // 各算各的话, 两级剔除会在边界上出现分歧 —— 实例级留下的东西
        // meshlet 级全剔掉, 或者反过来。而那只在物体刚好压着视锥边界时
        // 出现, 是那种"某个视角下少一块"的缺陷。
        if (m_MeshletCullPass)
        {
            m_MeshletCullPass->SetFrustum(
                FFrustum::FromViewProjection(m_Camera.GetProjectionMatrix() *
                                             m_Camera.GetViewMatrix()));

            m_MeshletCullPass->SetCameraPosition(m_Camera.GetPosition());
        }

        // 三级级联各占一个视图。阴影通道没有有效光源时它们的视锥是上一帧
        // 留下的 —— 无所谓, 那时阴影通道只清不画, 没人读那几段命令。
        UInt32 viewCount = FGpuCullPass::kFirstCascadeView;

        if (m_ShadowPass && m_ShadowPass->HasValidLight())
        {
            for (UInt32 cascade = 0; cascade < FShadowPass::kCascadeCount;
                 ++cascade)
            {
                m_GpuCullPass->SetViewFrustum(
                    FGpuCullPass::kFirstCascadeView + cascade,
                    FFrustum::FromViewProjection(
                        m_ShadowPass->GetCascadeViewProj(cascade)));
            }

            viewCount = FGpuCullPass::kFirstCascadeView +
                        FShadowPass::kCascadeCount;
        }

        m_GpuCullPass->SetViewCount(viewCount);
    }

    // 分簇参数必须在 UploadLightData **之前**设 —— 那一步会把它们打包进 UBO
    {
        const FRHIExtent2D clusterExtent = m_Context->GetSwapchainExtent();

        FLightManager::Get().SetClusterParams(
            m_ClusteredLighting,
            m_Camera.GetNearPlane(),
            m_Camera.GetFarPlane(),
            static_cast<Float32>(clusterExtent.Width),
            static_cast<Float32>(clusterExtent.Height));
    }

    // 每帧上传光照 UBO (含当前相机位置)
    FLightManager::Get().UploadLightData(frameIndex, m_Camera.GetPosition());

    // 分簇剔除的输入 —— 必须在 UploadLightData 之后,
    // 因为活跃光源数是那一步算出来的。
    //
    // 投影矩阵传**未抖动**的那一个: 抖动每帧改变亚像素偏移, 用它算出的
    // 簇边界会逐帧漂移不到一个像素 —— 那本身无害, 但会让"分簇结果与暴力
    // 法一致"这条验收判据变成逐帧不同, 无法比对。
    if (m_ClusterLightPass)
    {
        m_ClusterLightPass->SetCameraParams(
            m_Camera.GetViewMatrix(),
            m_Camera.GetProjectionMatrix(),
            m_Camera.GetNearPlane(),
            m_Camera.GetFarPlane());

        m_ClusterLightPass->SetLightSource(
            FLightManager::Get().GetLightStorageBuffer(frameIndex),
            FLightManager::Get().GetActiveLightCount());

        m_ClusterLightPass->SetEnabled(m_ClusteredLighting);
    }

    // GTAO 与分簇用同一对相机矩阵 —— 都要未抖动的那一个
    if (m_GtaoPass)
    {
        m_GtaoPass->SetCameraParams(m_Camera.GetViewMatrix(),
                                    m_Camera.GetProjectionMatrix(),
                                    m_Camera.GetNearPlane(),
                                    m_Camera.GetFarPlane());
    }

    const Float64 recordBegin = FPlatformTime::Seconds();

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
        execInfo.GpuCull               = m_GpuCullPass.Get();
        execInfo.MeshletCull           = m_MeshletCullPass.Get();
        execInfo.Camera                = &m_Camera;
        execInfo.ViewProjDescriptorSet = m_DescriptorSets[frameIndex];
        execInfo.PipelineLayout        = m_PipelineLayout;
        execInfo.LightingDescriptorSet = m_LightDescriptorSets[frameIndex];
        execInfo.BindlessDescriptorSet = m_BindlessTable.GetDescriptorSet(frameIndex);
        execInfo.Profiler              = &m_GpuProfiler;

        m_PassManager->ExecuteAll(commandBuffer, execInfo);
    }

    m_FrameStats.DrawCallCount =
        static_cast<UInt32>(m_RenderObjects.GetSize());

    // 场景 Pass 完成后回调 — 供 UI 渲染叠加等操作录制到同一命令缓冲区
    if (m_PostSceneRenderCallback)
    {
        m_PostSceneRenderCallback();
    }

    // GPU 计时收帧 —— 打下整帧终点并非阻塞回读若干帧之前的那一组。
    //
    // 放在 UI 回调之后: 整帧终点应当覆盖这一帧提交的全部 GPU 工作, 否则
    // "各 Pass 之和 vs 整帧"的差额会把 UI 的开销算成"漏埋"。
    {
        IRHICommandBuffer* timingBuffer =
            m_Context->GetCurrentCommandBuffer();

        if (timingBuffer != nullptr)
        {
            m_GpuProfiler.EndFrame(timingBuffer, m_Context->GetDevice());
        }
    }

    ++m_GpuFrameNumber;

    const Float64 presentBegin = FPlatformTime::Seconds();

    // 结束帧 (提交 + 呈现)
    result = m_Context->EndFrame();

    // ---- CPU 分项 ----
    //
    // 指数滑动平均, 系数 0.05 —— 单帧分项被调度抖动主导, 而这些数字是
    // 用来判断"该优化哪一段"的, 需要趋势而非瞬时值。
    {
        const Float64 now = FPlatformTime::Seconds();

        constexpr Float64 kAlpha = 0.05;

        const Float64 acquireMs = (acquireEnd - acquireBegin) * 1000.0;
        const Float64 updateMs  = (recordBegin - updateBegin) * 1000.0;
        const Float64 recordMs  = (presentBegin - recordBegin) * 1000.0;
        const Float64 presentMs = (now - presentBegin) * 1000.0;
        const Float64 totalMs   = (now - acquireBegin) * 1000.0;

        m_CpuTiming.AcquireMs =
            m_CpuTiming.AcquireMs * (1.0 - kAlpha) + acquireMs * kAlpha;
        m_CpuTiming.UpdateMs =
            m_CpuTiming.UpdateMs * (1.0 - kAlpha) + updateMs * kAlpha;
        m_CpuTiming.RecordMs =
            m_CpuTiming.RecordMs * (1.0 - kAlpha) + recordMs * kAlpha;
        m_CpuTiming.PresentMs =
            m_CpuTiming.PresentMs * (1.0 - kAlpha) + presentMs * kAlpha;
        m_CpuTiming.TotalMs =
            m_CpuTiming.TotalMs * (1.0 - kAlpha) + totalMs * kAlpha;
    }
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
// 环境光照
// ============================================================================

void FRenderer::SetEnvironmentMap(const FEnvironmentMap* environment)
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

    const bool hasEnvironment = (environment != nullptr) &&
                                environment->IsValid();

    if (m_SkyPass)
    {
        m_SkyPass->SetEnvironmentMap(
            device,
            hasEnvironment ? environment->GetCubeView()
                           : FRHITextureViewHandle(),
            hasEnvironment ? environment->GetSampler() : FRHISamplerHandle());
    }

    // 描述符集在多帧之间共享, 而正在执行的帧可能仍在采样旧的辐照度贴图。
    // 换关卡本就要停顿, 这里等一次 GPU 空闲比引入一套延迟更新简单得多。
    device->WaitIdle();

    if (hasEnvironment)
    {
        UpdateIblDescriptors(environment->GetIrradianceView(),
                             environment->GetPrefilteredView(),
                             environment->GetBrdfLutView(),
                             environment->GetSampler(),
                             environment->GetBrdfSampler());

        FLightManager::Get().EnableIbl(m_IblIntensity,
                                       environment->GetPrefilteredMaxLod());
    }
    else
    {
        // 退回占位图。不能只关开关就了事 —— 描述符里若留着已释放的视图,
        // 下一次销毁那张图像时验证层会指出它仍被描述符集引用。
        UpdateIblDescriptors(m_FallbackCubeView, m_FallbackCubeView,
                             m_FallbackLutView,
                             m_LinearClampSampler, m_LinearClampSampler);

        FLightManager::Get().DisableIbl();
    }
}

void FRenderer::UpdateIblDescriptors(FRHITextureViewHandle irradianceView,
                                     FRHITextureViewHandle prefilteredView,
                                     FRHITextureViewHandle brdfLutView,
                                     FRHISamplerHandle     cubeSampler,
                                     FRHISamplerHandle     lutSampler)
{
    IRHIDevice* device = m_Context->GetDevice();

    if (device == nullptr || !irradianceView.IsValid() ||
        !prefilteredView.IsValid() || !brdfLutView.IsValid() ||
        !cubeSampler.IsValid() || !lutSampler.IsValid())
    {
        return;
    }

    for (SizeType i = 0; i < m_LightDescriptorSets.GetSize(); ++i)
    {
        FRHIDescriptorWrite iblWrites[3];

        iblWrites[0] = FRHIDescriptorWrite::CombinedImageSampler(
            m_LightDescriptorSets[i], 2, irradianceView, cubeSampler,
            EImageLayout::ShaderReadOnly);

        iblWrites[1] = FRHIDescriptorWrite::CombinedImageSampler(
            m_LightDescriptorSets[i], 3, prefilteredView, cubeSampler,
            EImageLayout::ShaderReadOnly);

        iblWrites[2] = FRHIDescriptorWrite::CombinedImageSampler(
            m_LightDescriptorSets[i], 4, brdfLutView, lutSampler,
            EImageLayout::ShaderReadOnly);

        device->UpdateDescriptorSets(iblWrites, 3);
    }
}

void FRenderer::SetIblIntensity(Float32 intensity)
{
    m_IblIntensity = intensity;

    if (FLightManager::Get().IsIblEnabled())
    {
        // 只改强度, LOD 上限保持原样 —— 它由预滤波贴图的级数决定,
        // 与强度无关
        FLightManager::Get().EnableIbl(
            intensity, FLightManager::Get().GetIblPrefilteredMaxLod());
    }
}

void FRenderer::SetSkyIntensity(Float32 intensity)
{
    if (m_SkyPass)
    {
        m_SkyPass->SetIntensity(intensity);
    }
}

Float32 FRenderer::GetSkyIntensity() const
{
    return m_SkyPass ? m_SkyPass->GetIntensity() : 1.0f;
}

bool FRenderer::HasEnvironmentMap() const
{
    return m_SkyPass && m_SkyPass->HasEnvironmentMap();
}

// ============================================================================
// CreateFallbackCubeMap — 1x1 黑色立方体贴图
// ============================================================================

ERHIResult FRenderer::CreateFallbackCubeMap()
{
    IRHIDevice* device = m_Context->GetDevice();

    FRHITextureDesc cubeDesc = {};
    cubeDesc.Type          = ETextureType::TextureCube;
    cubeDesc.Format        = EPixelFormat::RGBA16_SFLOAT;
    cubeDesc.Extent.Width  = 1;
    cubeDesc.Extent.Height = 1;
    cubeDesc.Extent.Depth  = 1;
    cubeDesc.MipLevels     = 1;
    cubeDesc.ArrayLayers   = 6;
    cubeDesc.Usage         = static_cast<ETextureUsage>(
        static_cast<UInt32>(ETextureUsage::Sampled) |
        static_cast<UInt32>(ETextureUsage::TransferDst));
    cubeDesc.MemoryUsage   = EMemoryUsage::GpuOnly;
    cubeDesc.DebugName     = "Renderer.FallbackCube";

    ERHIResult result = device->CreateTexture(cubeDesc, m_FallbackCubeTexture);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    // 清成黑色。新建图像的内容是未定义的, 不清就等于绑了一张随机噪声 ——
    // 那样"没有环境贴图"时物体会被一层随机颜色照亮。
    IRHICommandBuffer* commandBuffer = m_Context->BeginSingleTimeCommands();

    if (commandBuffer == nullptr)
    {
        return ERHIResult::ErrorUnknown;
    }

    commandBuffer->TransitionImageLayout(
        m_FallbackCubeTexture,
        EImageLayout::Undefined,
        EImageLayout::TransferDst,
        EPipelineStageFlags::TopOfPipe,
        EPipelineStageFlags::Transfer,
        EAccessFlags::None,
        EAccessFlags::TransferWrite,
        0, 1, 0, 6);

    commandBuffer->ClearColorImage(m_FallbackCubeTexture,
                                   EImageLayout::TransferDst,
                                   FLinearColor(0.0f, 0.0f, 0.0f, 1.0f));

    commandBuffer->TransitionImageLayout(
        m_FallbackCubeTexture,
        EImageLayout::TransferDst,
        EImageLayout::ShaderReadOnly,
        EPipelineStageFlags::Transfer,
        EPipelineStageFlags::FragmentShader,
        EAccessFlags::TransferWrite,
        EAccessFlags::ShaderRead,
        0, 1, 0, 6);

    m_Context->EndSingleTimeCommands(commandBuffer);

    FRHITextureViewDesc viewDesc = {};
    viewDesc.Texture         = m_FallbackCubeTexture;
    viewDesc.ViewType        = ETextureType::TextureCube;
    viewDesc.Format          = EPixelFormat::RGBA16_SFLOAT;
    viewDesc.BaseMipLevel    = 0;
    viewDesc.MipLevelCount   = 1;
    viewDesc.BaseArrayLayer  = 0;
    viewDesc.ArrayLayerCount = 6;

    result = device->CreateTextureView(viewDesc, m_FallbackCubeView);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    FRHISamplerDesc samplerDesc     = FRHISamplerDesc::LinearClamp();
    samplerDesc.IsAnisotropyEnabled = false;
    samplerDesc.MaxLod              = 1.0f;

    return device->CreateSampler(samplerDesc, m_LinearClampSampler);
}

// ============================================================================
// CreateFallbackLut — 1x1 黑色 2D 纹理
// ============================================================================

ERHIResult FRenderer::CreateFallbackLut()
{
    IRHIDevice* device = m_Context->GetDevice();

    // 格式与真正的查找表一致 (RG16F): 描述符本身不校验格式, 但保持一致
    // 能让"换成占位图之后表现变了"这类问题少一个可能的原因。
    FRHITextureDesc lutDesc = {};
    lutDesc.Type          = ETextureType::Texture2D;
    lutDesc.Format        = EPixelFormat::RG16_SFLOAT;
    lutDesc.Extent.Width  = 1;
    lutDesc.Extent.Height = 1;
    lutDesc.Extent.Depth  = 1;
    lutDesc.MipLevels     = 1;
    lutDesc.ArrayLayers   = 1;
    lutDesc.Usage         = static_cast<ETextureUsage>(
        static_cast<UInt32>(ETextureUsage::Sampled) |
        static_cast<UInt32>(ETextureUsage::TransferDst));
    lutDesc.MemoryUsage   = EMemoryUsage::GpuOnly;
    lutDesc.DebugName     = "Renderer.FallbackLut";

    ERHIResult result = device->CreateTexture(lutDesc, m_FallbackLutTexture);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    IRHICommandBuffer* commandBuffer = m_Context->BeginSingleTimeCommands();

    if (commandBuffer == nullptr)
    {
        return ERHIResult::ErrorUnknown;
    }

    commandBuffer->TransitionImageLayout(
        m_FallbackLutTexture,
        EImageLayout::Undefined,
        EImageLayout::TransferDst,
        EPipelineStageFlags::TopOfPipe,
        EPipelineStageFlags::Transfer,
        EAccessFlags::None,
        EAccessFlags::TransferWrite);

    commandBuffer->ClearColorImage(m_FallbackLutTexture,
                                   EImageLayout::TransferDst,
                                   FLinearColor(0.0f, 0.0f, 0.0f, 1.0f));

    commandBuffer->TransitionImageLayout(
        m_FallbackLutTexture,
        EImageLayout::TransferDst,
        EImageLayout::ShaderReadOnly,
        EPipelineStageFlags::Transfer,
        EPipelineStageFlags::FragmentShader,
        EAccessFlags::TransferWrite,
        EAccessFlags::ShaderRead);

    m_Context->EndSingleTimeCommands(commandBuffer);

    FRHITextureViewDesc viewDesc = {};
    viewDesc.Texture         = m_FallbackLutTexture;
    viewDesc.ViewType        = ETextureType::Texture2D;
    viewDesc.Format          = EPixelFormat::RG16_SFLOAT;
    viewDesc.BaseMipLevel    = 0;
    viewDesc.MipLevelCount   = 1;
    viewDesc.BaseArrayLayer  = 0;
    viewDesc.ArrayLayerCount = 1;

    return device->CreateTextureView(viewDesc, m_FallbackLutView);
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

    device->DestroySampler(m_LinearClampSampler);
    device->DestroyTextureView(m_FallbackCubeView);
    device->DestroyTexture(m_FallbackCubeTexture);

    device->DestroyTextureView(m_FallbackLutView);
    device->DestroyTexture(m_FallbackLutTexture);
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
            sizeof(FViewProjUBO)     // range = 整个结构 (三个 mat4)
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

ERHIResult FRenderer::CreateDrawObjectSetLayout()
{
    // 顶点着色器读模型矩阵与材质下标; 片段着色器不读 (材质下标经 flat
    // varying 传下去)。StageFlags 只写 Vertex 就够 —— 多写一个阶段不会出错,
    // 但那等于对着色器的实际用法撒谎, 下一个人照着它加代码会以为片段阶段
    // 也能直接索引, 而片段阶段拿不到 gl_InstanceIndex。
    FRHIDescriptorBinding binding = {};
    binding.Binding    = 0;
    binding.Type       = EDescriptorType::StorageBuffer;
    binding.Count      = 1;
    binding.StageFlags = EShaderStage::Vertex;

    FRHIDescSetLayoutDesc layoutDesc = {};
    layoutDesc.Bindings     = &binding;
    layoutDesc.BindingCount = 1;
    layoutDesc.DebugName    = "DrawObjectSetLayout_Set3";

    return m_Context->GetDevice()->CreateDescSetLayout(layoutDesc,
                                                       m_DrawObjectSetLayout);
}

ERHIResult FRenderer::CreatePipelineLayout()
{
    IRHIDevice* device = m_Context->GetDevice();

    // Push Constant: 逐物体 Model 矩阵 (mat4 = 64 bytes, 顶点着色器可见)
    FRHIPushConstantRange pushConstantRange = {};

    // 只有顶点阶段读它。
    //
    // 材质下标原本也在 push constant 里, 片段阶段要读, 所以可见阶段得含
    // Fragment。GPU 驱动之后材质下标随模型矩阵一起进了 set 3 的 storage
    // buffer, 由顶点着色器经 flat varying 传给片段阶段 —— 片段阶段不再
    // 声明 push constant, 这里就不该再写 Fragment。
    //
    // 多写一个阶段不会出错, 但那等于对着色器的实际用法撒谎。
    pushConstantRange.StageFlags = EShaderStage::Vertex;
    pushConstantRange.Offset     = 0;
    pushConstantRange.Size       = sizeof(FViewPushConstant);

    // 3 套描述符集布局:
    //   set 0 = m_DescSetLayout       (ViewProj UBO + 棋盘格纹理)
    //   set 1 = bindless 表           (材质 SSBO + 全局纹理数组)
    //   set 2 = FLightManager 布局    (光照 UBO)
    //
    // set 1 从"每材质一个描述符集"换成了一个全局集: 绘制时只绑一次,
    // 材质靠 push constant 里的下标区分。这是 GPU 驱动渲染的前提 ——
    // 间接绘制没有"逐 draw 绑描述符集"这回事。
    // set 3 = 逐物体数据 (模型矩阵 + 包围球 + 材质下标)
    //
    // 从 push constant 搬到这里的理由不是"更现代", 是**间接绘制根本没有逐
    // draw 推送 push constant 这回事** —— 一次 DrawIndexedIndirect 覆盖几百
    // 个物体, 而 push constant 在整次调用里是常量。
    //
    // 四套集正好是 Vulkan 保证的 maxBoundDescriptorSets 下限。再加就要查询
    // 设备上限了。
    FRHIDescSetLayoutHandle setLayouts[4] =
    {
        m_DescSetLayout,
        m_BindlessTable.GetLayout(),
        FLightManager::Get().GetDescSetLayout(),
        m_DrawObjectSetLayout
    };

    FRHIPipelineLayoutDesc layoutDesc = {};
    layoutDesc.SetLayouts             = setLayouts;
    layoutDesc.SetLayoutCount         = 4;
    layoutDesc.PushConstantRanges     = &pushConstantRange;
    layoutDesc.PushConstantRangeCount = 1;
    layoutDesc.DebugName              = "PBRPipelineLayout";

    ERHIResult result = device->CreatePipelineLayout(layoutDesc, m_PipelineLayout);

    if (IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Log,
                 "[Renderer] PBR 管线布局创建完成 "
                 "(set0=ViewProj, set1=材质, set2=光照, set3=逐物体)");
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

        // set 2 binding 0 = 光照 UBO, binding 1 = 阴影贴图,
        //       binding 2 = 辐照度, binding 3 = 预滤波, binding 4 = BRDF 表
        //
        // 阴影贴图在整个运行期都是同一张纹理, 因此只需在这里写一次;
        // 内容每帧被阴影 Pass 重写, 但描述符指向的对象不变。
        //
        // 三张 IBL 贴图先写占位图。着色器里出现的描述符必须在管线绑定时
        // 有效, 留空等到加载环境贴图时再写是不行的 —— 中间任何一帧都会违规。
        FRHIDescriptorWrite writes[14];

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

        writes[2] = FRHIDescriptorWrite::CombinedImageSampler(
            descSet,
            2,
            m_FallbackCubeView,
            m_LinearClampSampler,
            EImageLayout::ShaderReadOnly);

        writes[3] = FRHIDescriptorWrite::CombinedImageSampler(
            descSet,
            3,
            m_FallbackCubeView,
            m_LinearClampSampler,
            EImageLayout::ShaderReadOnly);

        writes[4] = FRHIDescriptorWrite::CombinedImageSampler(
            descSet,
            4,
            m_FallbackLutView,
            m_LinearClampSampler,
            EImageLayout::ShaderReadOnly);

        // binding 5 — 光源数组 storage buffer
        //
        // 光源从 UBO 挪出来之后, binding 0 里只剩全局参数 (288 字节)。
        writes[5] = FRHIDescriptorWrite::StorageBuffer(
            descSet,
            5,
            FLightManager::Get().GetLightStorageBuffer(i),
            0,
            sizeof(FLightData) * kMaxLightCount);

        // binding 6/7 — 分簇的产出
        //
        // 这两个缓冲区由 FClusterLightPass 持有, 而它在 SetupAll 里已经建好
        // (本函数在 SetupAll 之后调用)。尺寸与分辨率无关, 所以写一次就够,
        // 交换链重建也不必重写 —— 那正是选固定簇网格的理由之一。
        writes[6] = FRHIDescriptorWrite::StorageBuffer(
            descSet, 6,
            m_ClusterLightPass->GetClusterGridBuffer(i), 0,
            static_cast<UInt64>(kClusterCount) * 8u);

        writes[7] = FRHIDescriptorWrite::StorageBuffer(
            descSet, 7,
            m_ClusterLightPass->GetLightIndexBuffer(i), 0,
            static_cast<UInt64>(kClusterLightIndexCapacity) * 4u);

        // binding 8 — 屏幕空间环境光遮蔽
        //
        // GTAO 关闭时那张图被清成 1, 所以这里无条件写、着色器无条件读。
        writes[8] = FRHIDescriptorWrite::CombinedImageSampler(
            descSet, 8,
            m_GtaoPass->GetAoView(), m_LinearClampSampler,
            EImageLayout::ShaderReadOnly);

        // binding 12/13 — 光追 AO 与光追反射
        //
        // 与阴影掩码同理: 无条件绑定, 生效与否由 UBO 里的标志位说了算。
        // 让着色器按图的内容去猜的话, 一张全 1 的 AO 与"AO 没跑"分不开。
        //
        // 设备不支持光追时这两张图不存在, 退回 GTAO 那张 —— 那时标志位是
        // 零, 着色器根本不会读它们, 这里只是不让描述符为空。
        const FRHITextureViewHandle rtAoView =
            (m_RayTracedAoPass &&
             m_RayTracedAoPass->GetAoView().IsValid())
                ? m_RayTracedAoPass->GetAoView()
                : m_GtaoPass->GetAoView();

        const FRHITextureViewHandle rtReflectionView =
            (m_RayTracedReflectionPass &&
             m_RayTracedReflectionPass->GetReflectionView().IsValid())
                ? m_RayTracedReflectionPass->GetReflectionView()
                : m_GtaoPass->GetAoView();

        writes[12] = FRHIDescriptorWrite::CombinedImageSampler(
            descSet, 12, rtAoView, m_LinearClampSampler,
            EImageLayout::ShaderReadOnly);

        writes[13] = FRHIDescriptorWrite::CombinedImageSampler(
            descSet, 13, rtReflectionView, m_LinearClampSampler,
            EImageLayout::ShaderReadOnly);

        // binding 9/10 — 聚光灯阴影的每块数据与图集本身
        //
        // 与 binding 1 的级联贴图同理: 图集在整个运行期都是同一张纹理,
        // 内容每帧被图集 Pass 重写, 但描述符指向的对象不变。
        writes[9] = FRHIDescriptorWrite::StorageBuffer(
            descSet, 9,
            FLightManager::Get().GetSpotShadowBuffer(i), 0,
            static_cast<UInt64>(sizeof(FSpotShadowData)) * kShadowTileCount);

        writes[10] = FRHIDescriptorWrite::CombinedImageSampler(
            descSet, 10,
            m_ShadowAtlasPass->GetAtlasView(),
            m_ShadowAtlasPass->GetAtlasSampler(),
            EImageLayout::ShaderReadOnly);

        // binding 11 — 光追阴影的可见度掩码
        //
        // 设备不支持光追时掩码不存在, 这里退回 AO 那张图。
        //
        // 退回一张**内容全是 1** 的图是刻意的: 着色器无条件读这个绑定,
        // 而 UBO 里的"哪一盏灯走光追"在那种情形下是 -1, 所以读到的值根本
        // 不会被用上。真正要防的是描述符为空 —— 那是管线绑定时的违规,
        // 与光追支不支持无关。
        const FRHITextureViewHandle maskView =
            (m_RayTracedShadowPass &&
             m_RayTracedShadowPass->GetShadowMaskView().IsValid())
                ? m_RayTracedShadowPass->GetShadowMaskView()
                : m_GtaoPass->GetAoView();

        writes[11] = FRHIDescriptorWrite::CombinedImageSampler(
            descSet, 11, maskView, m_LinearClampSampler,
            EImageLayout::ShaderReadOnly);

        device->UpdateDescriptorSets(writes, 14);

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
// SetTaaEnabled — 抖动与解析同开同关
// ============================================================================

void FRenderer::SetTaaEnabled(bool enabled)
{
    m_TemporalJitterEnabled = enabled;

    if (!m_TaaPass || !m_PostProcessPass)
    {
        return;
    }

    m_TaaPass->SetEnabled(enabled);

    RefreshPostProcessChain();
}

// ============================================================================
// SetBloomEnabled
// ============================================================================

void FRenderer::SetBloomEnabled(bool enabled)
{
    m_BloomEnabled = enabled;

    if (m_BloomPass)
    {
        m_BloomPass->SetEnabled(enabled);
    }

    RefreshPostProcessChain();
}

// ============================================================================
// RefreshPostProcessChain — 按开关状态重接整条链
// ============================================================================

void FRenderer::RefreshPostProcessChain()
{
    if (!m_PostProcessPass || !m_PassManager || m_Context == nullptr)
    {
        return;
    }

    IRHIDevice* const device = m_Context->GetDevice();

    if (device == nullptr)
    {
        return;
    }

    // 链: HDR 目标 → [TAA] → [泛光] → 色调映射
    //
    // 逐级往下传"上一级的输出", 关掉的那一级直接跳过。这样加第四级时只要
    // 在这里插一段, 而不必去改每一个开关的 Setter。
    FRHITextureViewHandle current = m_PassManager->GetSharedColorView();

    if (m_TaaPass && m_TaaPass->IsEnabled())
    {
        current = m_TaaPass->GetResolveView();
    }

    // 泛光的**输入**是它前一级的输出, 而不是固定的 HDR 目标 —— 开着 TAA
    // 时泛光要在消过锯齿的图上做, 否则锯齿边缘的高频会被放大成一圈爬行的
    // 亮边, 而 TAA 已经在它之前跑完, 无从补救。
    if (m_BloomPass && m_BloomPass->IsEnabled())
    {
        m_BloomPass->SetSourceView(current);
        current = m_BloomPass->GetOutputView();
    }

    // 只在开关翻转时改一次描述符, 不逐帧改 —— 改一个正在被上一帧使用的
    // 描述符集是验证层错误。
    device->WaitIdle();

    m_PostProcessPass->UpdateSourceDescriptor(device, current);
}

// ============================================================================
// SetGtaoEnabled
// ============================================================================

void FRenderer::SetGtaoEnabled(bool enabled)
{
    m_GtaoEnabled = enabled;

    if (m_GtaoPass)
    {
        m_GtaoPass->SetEnabled(enabled);
    }
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

    const FMatrix viewMatrix   = m_Camera.GetViewMatrix();
    const FMatrix projNoJitter = m_Camera.GetProjectionMatrix();

    // TAA 亚像素抖动。
    //
    // 只改写进 UBO 的这份拷贝, 不回写相机 —— 剔除视锥由 FSceneManager 从
    // 相机矩阵导出, 抖动若进了那里, 每帧的可见集合会在边界物体上反复跳变,
    // 表现为画面边缘物体闪烁, 而那看起来像是剔除余量不够。
    FMatrix projJittered = projNoJitter;

    m_CurrentJitter = FVector2(0.0f, 0.0f);

    if (m_TemporalJitterEnabled)
    {
        Float32 offsetX = 0.0f;
        Float32 offsetY = 0.0f;

        // 下标从 1 开始 —— Halton 的 0 号点恒为 0, 那是像素角点而非采样位置
        HaltonJitterPixels(
            static_cast<UInt32>(m_GpuFrameNumber % kJitterPeriod) + 1u,
            offsetX, offsetY);

        // 像素 → NDC。NDC 的 x 与 y 各横跨 2 个单位, 所以是 2/尺寸。
        m_CurrentJitter = FVector2(
            offsetX * 2.0f / static_cast<Float32>(FMath::Max(extent.Width, 1u)),
            offsetY * 2.0f / static_cast<Float32>(FMath::Max(extent.Height, 1u)));

        // 矩阵那一步单独成函数是为了能脱离 GPU 测 —— "偏移是否与深度无关"
        // 这个性质在画面上看不出来 (两种写法都"在抖"), 只能靠数值断言。
        // 见 FHalton.h 里 ApplyJitterToProjection 的说明。
        ApplyJitterToProjection(projJittered, m_CurrentJitter);
    }

    viewProj.View     = viewMatrix;
    viewProj.Proj     = projJittered;
    viewProj.ViewProj = projJittered * viewMatrix;

    viewProj.ViewProjNoJitter = projNoJitter * viewMatrix;

    // 上一帧的 Proj * View (无抖动) —— 速度矢量靠它算。
    //
    // 第一帧没有"上一帧", 此时填本帧的矩阵, 速度就恒等于零。这比填单位
    // 矩阵好: 单位矩阵会让第一帧的速度等于整个 viewProj 变换的量, 那是个
    // 巨大的假运动, TAA 接上以后表现为开场第一帧的整屏拖影。
    viewProj.PrevViewProjNoJitter =
        m_HasPrevViewProj ? m_PrevViewProjNoJitter : viewProj.ViewProjNoJitter;

    // 映射并写入 Uniform Buffer
    void* mappedPtr = nullptr;
    ERHIResult result =
        device->MapBuffer(m_UniformBuffers[frameIndex], &mappedPtr);
    if (IsRHISuccess(result))
    {
        MemCopy(mappedPtr, &viewProj, sizeof(FViewProjUBO));
        device->UnmapBuffer(m_UniformBuffers[frameIndex]);
    }

    // 存本帧矩阵供下一帧用。
    //
    // 必须在这里 (写完 UBO 之后) 存, 而不是在下一帧开头算 —— 下一帧开头时
    // 相机已经被 Tick 更新过了, 那时读到的是新矩阵, 速度会恒等于零而且没
    // 有任何报错。这类"少一帧延迟"的错位是速度缓冲最典型的失效方式。
    m_PrevViewProjNoJitter = viewProj.ViewProjNoJitter;
    m_HasPrevViewProj      = true;
}

// ============================================================================
// SetMeshletCullEnabled
// ============================================================================

bool FRenderer::SetMeshletCullEnabled(bool enabled)
{
    if (!m_MeshletCullPass)
    {
        return false;
    }

    m_MeshletCullPass->SetEnabled(enabled);

    return true;
}

bool FRenderer::SetMeshletDepthEnabled(bool enabled)
{
    if (!m_MeshletDepthPass || !m_MeshletCullPass)
    {
        return false;
    }

    // 打开光栅化就连带打开剔除 —— 它的输入就是剔除的输出。
    //
    // 不连带的话, 只开光栅化得到的是一张空深度图, 而"通道没跑"与"场景
    // 里什么都不可见"在那张图上分不开。
    if (enabled)
    {
        m_MeshletCullPass->SetEnabled(true);
    }

    m_MeshletDepthPass->SetEnabled(enabled);

    return true;
}

} // namespace Limx
