// ============================================================
// 文件名称：main.cpp
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：最小化启动器 — 仅负责组装渲染管线组件并运行主循环，
//          所有实际逻辑委托给 Luminance / Engine 模块中的类。
//          Launch ≠ Studio（编辑器），Launch 不组装 UI，仅驱动
//          对象系统、场景图和渲染管线。
// 功能描述：Limx Engine 启动入口 — 创建 Win32 窗口、初始化
//          Vulkan 渲染上下文、构建 LScene 场景图、运行主循环
//          （Tick Scene → SyncScene → RenderFrame）。
// 技术特性：WinMain 入口; FWindow→FRenderContext→FRenderer 渲染管线;
//          M1.0 集成: LScene→LNode→LMeshTrait→FSceneManager;
//          退出时逆序关闭: Scene→Renderer。
//
// ── 函数表 ──────────────────────────────────────────────────
// │ 函数名                      │ 描述                           │
// │────────────────────────────│───────────────────────────────│
// │ wWinMain()                 │ Win32 应用程序入口               │
// │ BuildDemoScene()           │ 构建 M1.0 演示场景               │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建                        │
// │ 2026-04-08  │ LimxTeam  │ M1.1 集成: LScene + FSceneManager│
// │ 2026-04-08  │ LimxTeam  │ 移除 UI 模块依赖，纯渲染管线     │
// ============================================================

#include "Launch/LaunchMinimal.h"

namespace Limx
{

// 日志分类
LIMX_DECLARE_LOG_CATEGORY(LogLaunch)
LIMX_DEFINE_LOG_CATEGORY(LogLaunch)

// ============================================================================
// BuildDemoScene — 将 FRenderer 已创建的 GPU 物体桥接到 LScene
//
// FRenderer::Initialize() 已创建了 3 个演示物体（立方体/球体/地面）的
// GPU 缓冲区和默认材质。此函数为每个物体创建 LNode + LMeshTrait，
// 共享同一套 VBO/IBO/Material，使 FSceneManager::SyncScene() 能够
// 从 LScene 重建渲染对象列表，验证 Engine→Luminance 完整数据通路。
// ============================================================================

static void BuildDemoScene(LScene* scene, FRenderer* renderer)
{
    LIMX_CHECK(scene != nullptr);
    LIMX_CHECK(renderer != nullptr);

    const TArray<FRenderObject>& existingObjects = renderer->GetRenderObjects();
    FMaterial* defaultMaterial = renderer->GetDefaultMaterial();

    for (SizeType i = 0; i < existingObjects.GetSize(); ++i)
    {
        const FRenderObject& srcObj = existingObjects[i];

        // 每个渲染物体对应一个 LNode
        FName nodeName = FName(srcObj.DebugName ? srcObj.DebugName : "SceneNode");
        LNode* node = scene->SpawnNode<LNode>(nodeName, srcObj.Transform);

        // 附加 LMeshTrait — 共享 FRenderer 已创建的 GPU 缓冲区
        LMeshTrait* meshTrait = node->AddTrait<LMeshTrait>(FName("Mesh"));
        meshTrait->SetMeshData(srcObj.VertexBuffer,
                               srcObj.IndexBuffer,
                               srcObj.IndexCount);
        meshTrait->SetMaterial(defaultMaterial);
        meshTrait->SetVisible(true);

        LIMX_LOG(LogLaunch, Log,
                 "[Launch] 场景节点 '{}' 已创建 ({}顶点, {}索引)",
                 nodeName.GetCStr(), srcObj.VertexCount, srcObj.IndexCount);
    }

    LIMX_LOG(LogLaunch, Log,
             "[Launch] LScene 构建完成 — {} 个节点",
             scene->GetNodeCount());
}

} // namespace Limx

/// Win32 应用程序入口
int WINAPI wWinMain(
    _In_ HINSTANCE     hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR        lpCmdLine,
    _In_ int           nCmdShow)
{
    // 消除未使用参数警告
    static_cast<void>(hInstance);
    static_cast<void>(hPrevInstance);
    static_cast<void>(lpCmdLine);
    static_cast<void>(nCmdShow);

    using namespace Limx;

    // ================================================================
    // 0a. 设置工作目录为引擎根目录
    //     exe 位于 Binaries/{Config}/{Platform}/LimxLaunch.exe
    //     引擎根 = exe 路径剥离 4 级 (文件名+3层目录)
    // ================================================================

    {
        WideChar exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);

        // 剥离 4 级: 文件名 → Win64 → Development → Binaries → 引擎根
        for (Int32 level = 0; level < 4; ++level)
        {
            WideChar* lastSlash = wcsrchr(exePath, L'\\');
            if (lastSlash == nullptr)
            {
                lastSlash = wcsrchr(exePath, L'/');
            }
            if (lastSlash != nullptr)
            {
                *lastSlash = L'\0';
            }
        }

        SetCurrentDirectoryW(exePath);
    }

    // ================================================================
    // 0b. 初始化文件日志 — 在所有其他操作之前
    // ================================================================

    FPlatformFile::CreateDirectoryTree(FString("Logs"));
    FileLogSink fileLogSink;
    if (fileLogSink.Open("Logs/LimxEngine.log"))
    {
        FLog::AddSink(&fileLogSink);
    }

    LIMX_LOG(LogLaunch, Log,
        "[Launch] Limx Engine 启动中...");

    // ================================================================
    // 1. 创建窗口
    // ================================================================

    FWindow window;

    FWindowDesc windowDesc = {};
    windowDesc.Width       = 1280;
    windowDesc.Height      = 720;
    windowDesc.Title       = L"Limx Engine — Vulkan Renderer";
    windowDesc.IsResizable = true;
    windowDesc.IsVisible   = true;

    if (!window.Create(windowDesc))
    {
        LIMX_LOG(LogLaunch, Fatal,
            "[Launch] 窗口创建失败，退出");
        return 1;
    }

    // ================================================================
    // 2. 初始化渲染上下文 (RHI 设备 + 交换链 + 帧同步)
    // ================================================================

    FRenderContext renderContext;

    FRenderContextDesc contextDesc = {};
    contextDesc.Window             = &window;
    contextDesc.EnableValidation   = true;
    contextDesc.MaxFramesInFlight  = 2;
    contextDesc.IsVSyncEnabled     = false;

    ERHIResult result = renderContext.Initialize(contextDesc);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogLaunch, Fatal,
            "[Launch] 渲染上下文初始化失败: {}, 退出",
            static_cast<Int32>(result));
        return 2;
    }

    // ================================================================
    // 3. 初始化渲染器 (创建 GPU 资源 + 演示场景 + 材质 + 光照 + Pass 系统)
    // ================================================================

    FRenderer renderer;
    result = renderer.Initialize(&window, &renderContext);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogLaunch, Fatal,
            "[Launch] 渲染器初始化失败: {}, 退出",
            static_cast<Int32>(result));
        return 3;
    }

    // 设置清屏颜色 — Limx 品牌深蓝色调
    renderer.SetClearColor(FLinearColor(0.02f, 0.02f, 0.05f, 1.0f));

    // ================================================================
    // 4. M1.0 集成 — 构建 LScene 场景图 + FSceneManager 桥接
    // ================================================================

    // 4a. 初始化 FSceneManager 渲染桥接单例
    FSceneManager::Get().Initialize(&renderer);

    // 4b. 创建 LScene — M1.0 对象系统的顶级运行时上下文
    LScene* scene = LScene::Create(FName("DemoScene"));
    LIMX_CHECK(scene != nullptr);

    // 4c. 将 FRenderer 已创建的 GPU 物体桥接到 LScene 的 LNode+LMeshTrait
    BuildDemoScene(scene, &renderer);

    // 4d. 场景开始播放 — 驱动所有节点和 Trait 的 OnBegin()
    scene->OnBegin();

    LIMX_LOG(LogLaunch, Log,
        "[Launch] M1.0 初始化完成 — LScene '{}' 已启动，{} 个节点",
        scene->GetSceneName().GetCStr(), scene->GetNodeCount());

    // ================================================================
    // 5. 主循环 — Tick 场景 → 同步渲染数据 → 执行帧渲染
    // ================================================================

    Float64 lastFrameTime = FPlatformTime::Seconds();

    while (window.ProcessMessages())
    {
        // 计算帧间隔
        Float64 currentTime = FPlatformTime::Seconds();
        Float32 deltaTime = static_cast<Float32>(currentTime - lastFrameTime);
        lastFrameTime = currentTime;
        deltaTime = FMath::Clamp(deltaTime, 0.0001f, 0.1f);

        // 5a. 场景 Tick — 驱动所有 LNode/LTrait/LSystem 的 Tick()
        scene->Tick(deltaTime);

        // 5b. 同步场景数据到渲染器 — LMeshTrait → FRenderObject → FRenderer
        FSceneManager::Get().SyncScene(scene, deltaTime);

        // 5c. 渲染帧 — 场景 Pass → 提交+呈现
        renderer.RenderFrame();
    }

    // ================================================================
    // 6. 关闭 (逆序: 场景→桥接→渲染器→上下文→窗口)
    // ================================================================

    LIMX_LOG(LogLaunch, Log,
        "[Launch] 正在关闭...");

    // 6a. 场景停止播放 — 逆序调用所有节点和 Trait 的 OnEnd()
    scene->OnEnd();

    // 6b. 销毁场景 — 注意: LNode 中的 LMeshTrait 仅持有 VBO/IBO 句柄引用，
    //     GPU 缓冲区的实际所有权归 FRenderer，由 Renderer::Shutdown 销毁。
    //     此处销毁 LScene 仅释放对象系统的内存和注册表条目。
    LRegistry::Get().Destroy(scene);
    scene = nullptr;

    // 6c. 关闭渲染桥接
    FSceneManager::Get().Shutdown();

    // 6d. 关闭渲染器和 GPU 资源
    renderer.Shutdown();
    renderContext.Shutdown();
    window.Destroy();

    LIMX_LOG(LogLaunch, Log,
        "[Launch] Limx Engine 已关闭");

    // 移除文件日志 Sink 并关闭
    FLog::RemoveSink(&fileLogSink);
    fileLogSink.Close();

    return 0;
}
