// ============================================================
// 文件名称：main.cpp
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：最小化启动器 — 仅负责组装渲染管线组件并运行主循环，
//          所有实际逻辑委托给 Luminance / Engine 模块中的类。
//          Launch ≠ Studio（编辑器），Launch 不组装 UI，仅驱动
//          对象系统、场景图和渲染管线。
//
//          基准场景由命令行参数驱动而非编译期常量 — 剔除率与状态排序的
//          收益只有在"同一份可执行文件、同一个场景、只切换开关"时才可比。
//          把规模写进代码意味着每次对照都要重新编译，两次测量之间就多了
//          一个无法排除的变量。
// 功能描述：Limx Engine 启动入口 — 创建 Win32 窗口、初始化
//          Vulkan 渲染上下文、构建 LScene 场景图、运行主循环
//          （Tick Scene → SyncScene → RenderFrame）。
// 技术特性：WinMain 入口; FWindow→FRenderContext→FRenderer 渲染管线;
//          M1.0 集成: LScene→LNode→LMeshTrait→FSceneManager;
//          基准模式: --grid/--frames/--no-cull/--no-sort;
//          退出时逆序关闭: Scene→Renderer。
//
// ── 函数表 ──────────────────────────────────────────────────
// │ 函数名                      │ 描述                           │
// │────────────────────────────│───────────────────────────────│
// │ wWinMain()                 │ Win32 应用程序入口               │
// │ ParseLaunchOptions()       │ 解析命令行参数                   │
// │ BuildDemoScene()           │ 构建默认演示场景                 │
// │ BuildStressScene()         │ 构建可配置规模的压力场景          │
// │ LoadSceneFromFile()        │ 导入资产并自动摆放相机           │
// │ RunReloadTest()            │ 关卡切换自检 (显存回落验证)       │
// │ LogBenchmarkReport()       │ 输出基准测量报告                 │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                             │
// │─────────────│──────────│─────────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建                          │
// │ 2026-04-08  │ LimxTeam  │ M1.1 集成: LScene + FSceneManager │
// │ 2026-04-08  │ LimxTeam  │ 移除 UI 模块依赖，纯渲染管线       │
// │ 2026-08-29  │ LimxTeam  │ 场景网格改由资源管理器创建         │
// │ 2026-08-29  │ LimxTeam  │ 命令行驱动的压力场景与基准报告     │
// │ 2026-08-29  │ LimxTeam  │ --scene 导入任意 OBJ/glTF 资产     │
// ============================================================

#include "Launch/LaunchMinimal.h"
#include "RenderCore/Material/FMaterialManager.h"
#include "Engine/Rendering/FSceneLoader.h"

namespace Limx
{

// 日志分类
LIMX_DECLARE_LOG_CATEGORY(LogLaunch)
LIMX_DEFINE_LOG_CATEGORY(LogLaunch)

// ============================================================================
// FLaunchOptions — 命令行选项
// ============================================================================

/// 启动选项
struct FLaunchOptions
{
    /// 压力场景的网格边长; 0 表示使用默认演示场景
    UInt32 GridSize = 0;

    /// 自动退出前渲染的帧数; 0 表示一直运行到窗口关闭
    UInt32 FrameLimit = 0;

    /// 开始计入基准统计前跳过的预热帧数
    ///
    /// 前若干帧包含管线首次编译、显存首次触碰、交换链首次呈现,
    /// 耗时是稳态的几倍。混进平均值会让任何对照都失去意义。
    UInt32 WarmupFrames = 60;

    bool EnableCulling = true;
    bool EnableSorting = true;

    /// 待导入的资产路径; 为空时使用内置场景
    FString ScenePath;

    /// 导入时的统一缩放 —— 不同来源的资产单位不一
    Float32 SceneScale = 1.0f;

    /// 是否加载纹理 —— 关闭可把几何吞吐与纹理带宽分开测量
    bool LoadTextures = true;

    /// 相机是否置于场景内部 —— 建筑内景必须开启, 否则只看得到外墙
    bool CameraInside = false;

    /// 关卡切换自检 —— 加载 → 卸载 → 再加载, 逐步报告显存
    ///
    /// 显存能否回落是"引用计数是否真的接通"的唯一硬指标。靠肉眼看画面
    /// 完全看不出泄漏, 而泄漏在关卡反复切换后才会致命。
    bool ReloadTest = false;
};

// ============================================================================
// ParseLaunchOptions — 解析命令行
// ============================================================================

namespace
{

/// 宽字符串转无符号整数 — 非数字返回 fallback
UInt32 ParseUInt32(const WideChar* text, UInt32 fallback)
{
    if (text == nullptr || *text == L'\0')
    {
        return fallback;
    }

    UInt32 value = 0;

    for (const WideChar* cursor = text; *cursor != L'\0'; ++cursor)
    {
        if (*cursor < L'0' || *cursor > L'9')
        {
            return fallback;
        }

        value = value * 10u + static_cast<UInt32>(*cursor - L'0');
    }

    return value;
}

/// 宽字符串转 FString — 仅处理 ASCII, 路径含非 ASCII 时应改走宽字符 API
FString WideToString(const WideChar* text)
{
    FString result;

    if (text == nullptr)
    {
        return result;
    }

    for (const WideChar* cursor = text; *cursor != L'\0'; ++cursor)
    {
        result.AppendChar(static_cast<AnsiChar>(*cursor));
    }

    return result;
}

/// 宽字符串转单精度浮点 — 只支持 "整数[.小数]" 形式
Float32 ParseFloat32(const WideChar* text, Float32 fallback)
{
    if (text == nullptr || *text == L'\0')
    {
        return fallback;
    }

    Float32 integerPart  = 0.0f;
    Float32 fractionPart = 0.0f;
    Float32 fractionScale = 1.0f;
    bool    seenDot      = false;

    for (const WideChar* cursor = text; *cursor != L'\0'; ++cursor)
    {
        if (*cursor == L'.')
        {
            if (seenDot)
            {
                return fallback;
            }
            seenDot = true;
            continue;
        }

        if (*cursor < L'0' || *cursor > L'9')
        {
            return fallback;
        }

        const Float32 digit = static_cast<Float32>(*cursor - L'0');

        if (seenDot)
        {
            fractionScale *= 0.1f;
            fractionPart += digit * fractionScale;
        }
        else
        {
            integerPart = integerPart * 10.0f + digit;
        }
    }

    return integerPart + fractionPart;
}

/// 宽字符串相等比较
bool WideEquals(const WideChar* a, const WideChar* b)
{
    while (*a != L'\0' && *b != L'\0')
    {
        if (*a != *b)
        {
            return false;
        }
        ++a;
        ++b;
    }

    return *a == *b;
}

} // namespace

/// 解析命令行参数
///
/// 直接切分 wWinMain 收到的 lpCmdLine, 不走 CommandLineToArgvW ——
/// 后者要链接 shell32, 而这里的参数形态 (无引号、无空格路径) 用不上它的
/// 引号规则。切分是就地进行的: 分隔空白被改写成终止符, 于是每个片段
/// 本身就是一个独立的宽字符串。
///
/// 支持:
///   --grid N     构建 N×N 的压力场景 (默认: 三物体演示场景)
///   --frames N   渲染 N 帧后自动退出并输出基准报告
///   --warmup N   计入统计前跳过的预热帧数 (默认 60)
///   --no-cull    关闭视锥剔除
///   --no-sort    关闭状态排序
///   --scene P    导入资产文件 P (.obj / .gltf / .glb)
///   --scene-scale S  导入时的统一缩放
///   --no-textures    不加载纹理, 只保留材质常量
///   --camera-inside  相机置于场景内部 (建筑内景用)
///   --reload-test    关卡切换自检: 加载 → 卸载 → 再加载, 报告显存回落
static FLaunchOptions ParseLaunchOptions(WideChar* commandLine)
{
    FLaunchOptions options;

    if (commandLine == nullptr)
    {
        return options;
    }

    /// 参数上限 — 超出部分直接忽略, 基准开关远用不到这么多
    constexpr Int32 kMaxArgs = 32;

    WideChar* tokens[kMaxArgs] = {};
    Int32     tokenCount       = 0;

    WideChar* cursor = commandLine;

    while (*cursor != L'\0' && tokenCount < kMaxArgs)
    {
        // 跳过分隔空白, 顺手把它改写成终止符
        while (*cursor == L' ' || *cursor == L'\t')
        {
            *cursor = L'\0';
            ++cursor;
        }

        if (*cursor == L'\0')
        {
            break;
        }

        tokens[tokenCount++] = cursor;

        while (*cursor != L'\0' && *cursor != L' ' && *cursor != L'\t')
        {
            ++cursor;
        }
    }

    for (Int32 i = 0; i < tokenCount; ++i)
    {
        const WideChar* arg = tokens[i];

        if (WideEquals(arg, L"--grid") && (i + 1) < tokenCount)
        {
            options.GridSize = ParseUInt32(tokens[++i], 0);
        }
        else if (WideEquals(arg, L"--frames") && (i + 1) < tokenCount)
        {
            options.FrameLimit = ParseUInt32(tokens[++i], 0);
        }
        else if (WideEquals(arg, L"--warmup") && (i + 1) < tokenCount)
        {
            options.WarmupFrames = ParseUInt32(tokens[++i], 60);
        }
        else if (WideEquals(arg, L"--no-cull"))
        {
            options.EnableCulling = false;
        }
        else if (WideEquals(arg, L"--no-sort"))
        {
            options.EnableSorting = false;
        }
        else if (WideEquals(arg, L"--scene") && (i + 1) < tokenCount)
        {
            options.ScenePath = WideToString(tokens[++i]);
        }
        else if (WideEquals(arg, L"--scene-scale") && (i + 1) < tokenCount)
        {
            options.SceneScale = ParseFloat32(tokens[++i], 1.0f);
        }
        else if (WideEquals(arg, L"--no-textures"))
        {
            options.LoadTextures = false;
        }
        else if (WideEquals(arg, L"--camera-inside"))
        {
            options.CameraInside = true;
        }
        else if (WideEquals(arg, L"--reload-test"))
        {
            options.ReloadTest = true;
        }
    }

    return options;
}

// ============================================================================
// BuildDemoScene — 通过资源管理器创建演示网格并挂到 LScene
//
// 网格的所有权归 FRenderResourceManager: 这里只把句柄交给 LMeshTrait,
// Trait 在绑定时加引用, 在销毁时释放。渲染器不再持有任何网格显存,
// 因此销毁顺序也不再要求"场景必须先于渲染器"。
// ============================================================================

static void BuildDemoScene(LScene* scene, FRenderContext* context,
                           FRenderer* renderer)
{
    LIMX_CHECK(scene != nullptr);
    LIMX_CHECK(context != nullptr);
    LIMX_CHECK(renderer != nullptr);

    FRenderResourceManager& resources = context->GetResourceManager();
    FMaterial* defaultMaterial        = renderer->GetDefaultMaterial();

    struct FDemoEntry
    {
        const AnsiChar* Name;
        FMeshData       Mesh;
        FVector3        Position;
    };

    FDemoEntry entries[3] =
    {
        { "Cube",   FGeometryGenerator::GenerateCube(),
          FVector3(0.0f, 0.75f, 0.0f) },
        { "Sphere", FGeometryGenerator::GenerateSphere(0.5f, 32, 16),
          FVector3(2.5f, 0.5f, 0.0f) },
        { "Ground", FGeometryGenerator::GeneratePlane(8.0f, 8.0f, 4, 4),
          FVector3(0.0f, 0.0f, 0.0f) },
    };

    for (UInt32 i = 0; i < 3; ++i)
    {
        FDemoEntry& entry = entries[i];

        FName meshName(entry.Name);

        FMeshResourceHandle meshHandle =
            resources.CreateMesh(entry.Mesh, meshName);

        if (!meshHandle.IsValid())
        {
            LIMX_LOG(LogLaunch, Error,
                     "[Launch] 网格 '{}' 上传失败", entry.Name);
            continue;
        }

        FTransform nodeTransform;
        nodeTransform.Translation = entry.Position;

        LNode* node = scene->SpawnNode<LNode>(meshName, nodeTransform);

        LMeshTrait* meshTrait = node->AddTrait<LMeshTrait>(FName("Mesh"));
        meshTrait->SetMesh(&resources, meshHandle);
        meshTrait->SetMaterial(defaultMaterial);
        meshTrait->SetVisible(true);

        const FMeshResource* resource = resources.GetMesh(meshHandle);

        // 交出所有权 —— Trait 已在 SetMesh 中加了自己那份引用。
        // 不放掉创建时的这一份, 网格就永远不会随场景一起变成可回收状态。
        resources.ReleaseMeshReference(meshHandle);

        LIMX_LOG(LogLaunch, Log,
                 "[Launch] 场景节点 '{}' 已创建 ({} 顶点, {} 索引)",
                 entry.Name,
                 resource != nullptr ? resource->VertexCount : 0u,
                 resource != nullptr ? resource->IndexCount : 0u);
    }

    resources.LogStats("演示场景构建后");

    LIMX_LOG(LogLaunch, Log,
             "[Launch] LScene 构建完成 — {} 个节点",
             scene->GetNodeCount());
}

// ============================================================================
// BuildStressScene — 可配置规模的压力场景
//
// 网格与材质都是共享的: gridSize² 个节点只对应 3 份网格资源和 8 份材质。
// 这正是要测的东西 —— 排序能否把"每个节点各绑一次"压到"每种材质绑一次"。
// 若每个节点各持一份独立资源, 排序就无从优化, 测出来的也不是真实场景。
//
// 物体铺成一个平面网格, 相机默认只看得到其中一小片, 剔除率因此天然很高;
// 这与真实关卡里"绝大多数物体在视野之外"的分布一致。
// ============================================================================

static void BuildStressScene(LScene* scene, FRenderContext* context,
                             FRenderer* renderer, UInt32 gridSize)
{
    LIMX_CHECK(scene != nullptr);
    LIMX_CHECK(context != nullptr);
    LIMX_CHECK(renderer != nullptr);

    FRenderResourceManager& resources = context->GetResourceManager();

    // ---- 共享网格 (3 种) ----
    FMeshData meshes[3] =
    {
        FGeometryGenerator::GenerateCube(),
        FGeometryGenerator::GenerateSphere(0.4f, 16, 12),
        FGeometryGenerator::GeneratePlane(0.8f, 0.8f, 2, 2),
    };

    const AnsiChar* meshNames[3] = { "StressCube", "StressSphere",
                                     "StressPlane" };

    FMeshResourceHandle meshHandles[3];

    for (UInt32 i = 0; i < 3; ++i)
    {
        meshHandles[i] = resources.CreateMesh(meshes[i], FName(meshNames[i]));

        if (!meshHandles[i].IsValid())
        {
            LIMX_LOG(LogLaunch, Error,
                     "[Launch] 压力场景网格 '{}' 上传失败", meshNames[i]);
            return;
        }
    }

    // ---- 共享材质 (8 种) ----
    constexpr UInt32 kMaterialCount = 8;
    FMaterial* materials[kMaterialCount] = {};

    for (UInt32 i = 0; i < kMaterialCount; ++i)
    {
        materials[i] = FMaterialManager::Get().CreateMaterial("StressMaterial");

        if (materials[i] == nullptr)
        {
            LIMX_LOG(LogLaunch, Error, "[Launch] 压力场景材质创建失败");
            return;
        }

        const Float32 t =
            static_cast<Float32>(i) / static_cast<Float32>(kMaterialCount - 1);

        materials[i]->SetBaseColor(
            FVector4(0.2f + 0.7f * t, 0.5f, 0.9f - 0.7f * t, 1.0f));
        materials[i]->SetMetallic(t);
        materials[i]->SetRoughness(0.15f + 0.7f * (1.0f - t));
    }

    // ---- 铺开节点 ----
    constexpr Float32 kSpacing = 2.0f;

    const Float32 halfSpan =
        static_cast<Float32>(gridSize - 1) * kSpacing * 0.5f;

    for (UInt32 x = 0; x < gridSize; ++x)
    {
        for (UInt32 z = 0; z < gridSize; ++z)
        {
            const UInt32 linear = x * gridSize + z;

            FTransform nodeTransform;
            nodeTransform.Translation = FVector3(
                static_cast<Float32>(x) * kSpacing - halfSpan,
                0.5f,
                static_cast<Float32>(z) * kSpacing - halfSpan);

            LNode* node =
                scene->SpawnNode<LNode>(FName("StressNode"), nodeTransform);

            LMeshTrait* meshTrait = node->AddTrait<LMeshTrait>(FName("Mesh"));
            meshTrait->SetMesh(&resources, meshHandles[linear % 3]);
            meshTrait->SetMaterial(materials[linear % kMaterialCount]);
            meshTrait->SetVisible(true);
        }
    }

    // 交出创建时的所有权 —— 每个节点都已各自加过引用
    for (UInt32 i = 0; i < 3; ++i)
    {
        resources.ReleaseMeshReference(meshHandles[i]);
    }

    // 相机拉高拉远, 让网格中心落在视野里
    const Float32 viewDistance = halfSpan * 0.6f + 8.0f;

    renderer->GetCamera().SetPosition(
        FVector3(0.0f, viewDistance * 0.5f, -viewDistance));
    renderer->GetCamera().SetRotation(FMath::kPi, -0.45f);

    resources.LogStats("压力场景构建后");

    LIMX_LOG(LogLaunch, Log,
             "[Launch] 压力场景构建完成 — {}×{} = {} 个节点, "
             "共享 3 份网格 / {} 份材质",
             gridSize, gridSize, scene->GetNodeCount(), kMaterialCount);
}

// ============================================================================
// LoadSceneFromFile — 导入资产并把相机摆到能看见整个场景的位置
//
// 相机自动摆位不是锦上添花: 不同资产的单位与尺度相差几个数量级
// (Sponza 是厘米级的两千单位, 典型 glTF 是米级的个位数)。用固定相机
// 打开任意资产, 十有八九是一片空屏, 而这与"导入失败"在画面上毫无区别。
// ============================================================================

static bool LoadSceneFromFile(LScene* scene, FRenderContext* context,
                              FRenderer* renderer,
                              const FLaunchOptions& options,
                              FSceneLoadResult* outResult = nullptr)
{
    FSceneLoadOptions loadOptions;
    loadOptions.UniformScale = options.SceneScale;
    loadOptions.LoadTextures = options.LoadTextures;

    LIMX_LOG(LogLaunch, Log,
             "[Launch] 正在导入资产: {}", options.ScenePath.GetCStr());

    const FSceneLoadResult loadResult =
        FSceneLoader::LoadInto(scene, context, options.ScenePath, loadOptions);

    if (outResult != nullptr)
    {
        *outResult = loadResult;
    }

    if (!loadResult.Succeeded)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Launch] 资产导入失败: {}", loadResult.Error.GetCStr());
        return false;
    }

    LIMX_LOG(LogLaunch, Log,
             "[Launch] 资产导入完成 — 节点 {} | 网格 {} | 材质 {} | 纹理 {} "
             "(缺失 {}) | 顶点 {} | 三角形 {} | 耗时 {} ms",
             loadResult.NodeCount, loadResult.MeshCount,
             loadResult.MaterialCount, loadResult.TextureCount,
             loadResult.MissingTextureCount, loadResult.VertexCount,
             loadResult.TriangleCount, loadResult.ElapsedMilliseconds);

    context->GetResourceManager().LogStats("资产导入后");

    // ---- 按包围盒摆放相机 ----
    //
    // 两种取景互相排斥, 没有一个默认值能同时伺候好两类资产:
    //   - 单个物体: 站在包围球外侧看全貌 (默认)
    //   - 建筑内景: 站在场景内部, 否则只看得到外墙 —— Sponza 正是如此
    // 因此由 --camera-inside 显式选择, 而不是靠包围盒长宽比去猜。
    if (loadResult.Bounds.IsValid())
    {
        const FVector3 center = loadResult.Bounds.GetCenter();
        const FVector3 extent = loadResult.Bounds.GetExtent();

        const Float32 radius = FMath::Max(
            extent.X, FMath::Max(extent.Y, extent.Z));

        FCamera& camera = renderer->GetCamera();

        Float32 farPlane = 1.0f;

        if (options.CameraInside)
        {
            // 沿最长水平轴后撤小半程, 贴近地面, 望向场景另一头。
            //
            // 后撤量刻意只取半边长的 0.45 —— 包围盒的边界处往往正是墙体
            // 内部 (Sponza 的外墙很厚), 贴着边界摆相机会直接把镜头埋进墙里。
            // 取中段能保证落在开阔地面上。
            const bool isXLonger = extent.X >= extent.Z;

            const Float32 kAxisBackoff = 0.45f;

            const Float32 eyeHeight =
                loadResult.Bounds.Min.Y + extent.Y * 0.35f;

            if (isXLonger)
            {
                camera.SetPosition(FVector3(
                    center.X - extent.X * kAxisBackoff, eyeHeight, center.Z));
                // 偏航 -π/2 使前方向量指向 +X
                camera.SetRotation(-FMath::kPi * 0.5f, -0.05f);
            }
            else
            {
                camera.SetPosition(FVector3(
                    center.X, eyeHeight, center.Z - extent.Z * kAxisBackoff));
                camera.SetRotation(FMath::kPi, -0.05f);
            }

            farPlane = radius * 6.0f;
        }
        else
        {
            // 站在包围球外侧约两倍半径处, 60° 视场下能把整个场景收进画面
            const Float32 distance = FMath::Max(radius * 2.0f, 1.0f);

            camera.SetPosition(FVector3(
                center.X, center.Y + radius * 0.35f, center.Z - distance));
            camera.SetRotation(FMath::kPi, -0.15f);

            farPlane = distance + radius * 4.0f;
        }

        // 远裁剪面必须包住整个场景, 否则远端会被整片切掉
        camera.SetPerspective(
            FMath::DegreesToRadians(60.0f),
            camera.GetAspectRatio(),
            FMath::Max(radius * 0.001f, 0.01f),
            farPlane);

        LIMX_LOG(LogLaunch, Log,
                 "[Launch] 场景包围盒 min=({},{},{}) max=({},{},{}), "
                 "取景 {}",
                 loadResult.Bounds.Min.X, loadResult.Bounds.Min.Y,
                 loadResult.Bounds.Min.Z, loadResult.Bounds.Max.X,
                 loadResult.Bounds.Max.Y, loadResult.Bounds.Max.Z,
                 options.CameraInside ? "内部" : "全景");
    }

    return true;
}

// ============================================================================
// RunReloadTest — 关卡切换自检
//
// 加载 → 卸载 → 再加载, 每一步报告显存。中间那一步的显存必须回落到接近
// 初始值, 否则说明引用计数某处没接通 —— 而这类泄漏靠看画面完全看不出来,
// 只有在关卡反复切换后才会以"显存耗尽"的形式暴露。
// ============================================================================

static bool RunReloadTest(FRenderContext* context, FRenderer* renderer,
                          const FLaunchOptions& options)
{
    FRenderResourceManager& resources = context->GetResourceManager();

    const UInt64 baselineBytes = resources.GetStats().GetTotalBytes();

    LIMX_LOG(LogLaunch, Log,
             "[自检] 基线显存 {} KiB", baselineBytes / 1024);

    for (UInt32 round = 0; round < 2; ++round)
    {
        LScene* scene = LScene::Create(FName("ReloadTestScene"));

        if (scene == nullptr)
        {
            LIMX_LOG(LogLaunch, Error, "[自检] 场景创建失败");
            return false;
        }

        FSceneLoadResult loadResult;

        if (!LoadSceneFromFile(scene, context, renderer, options, &loadResult))
        {
            LRegistry::Get().Destroy(scene);
            return false;
        }

        const UInt64 loadedBytes = resources.GetStats().GetTotalBytes();

        LIMX_LOG(LogLaunch, Log,
                 "[自检] 第 {} 轮加载后 — 显存 {} KiB (网格 {} 纹理 {})",
                 round + 1, loadedBytes / 1024,
                 resources.GetStats().MeshCount,
                 resources.GetStats().TextureCount);

        // ---- 卸载 ----
        //
        // 顺序要紧: 先销毁场景 (LMeshTrait 析构释放网格引用), 再销毁材质
        // (释放纹理引用), 最后收割。反过来的话, 材质销毁时 Trait 还活着,
        // 网格引用未放, 收割只能收回纹理。
        LRegistry::Get().Destroy(scene);

        const UInt32 destroyedMaterials =
            FSceneLoader::UnloadMaterials(loadResult);

        const UInt32 collected = resources.CollectUnreferenced();


        // 退役资源要等 MaxFramesInFlight 帧才真正销毁。此处没有帧循环,
        // 直接等 GPU 空闲后冲刷队列。
        context->GetDevice()->WaitIdle();
        const UInt32 flushed = resources.FlushPendingReleases();

        const UInt64 unloadedBytes = resources.GetStats().GetTotalBytes();

        LIMX_LOG(LogLaunch, Log,
                 "[自检] 第 {} 轮卸载后 — 显存 {} KiB "
                 "(销毁材质 {}, 回收资源 {}, 冲刷 {})",
                 round + 1, unloadedBytes / 1024,
                 destroyedMaterials, collected, flushed);

        if (unloadedBytes > baselineBytes)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[自检] 第 {} 轮存在泄漏 — 卸载后 {} KiB > 基线 {} KiB",
                     round + 1, unloadedBytes / 1024, baselineBytes / 1024);
            return false;
        }
    }

    LIMX_LOG(LogLaunch, Log, "[自检] 通过 — 两轮加载卸载后显存完全回落");
    return true;
}

// ============================================================================
// LogBenchmarkReport — 输出基准测量报告
// ============================================================================

static void LogBenchmarkReport(const FLaunchOptions& options,
                               const FRenderer& renderer)
{
    const FRenderFrameStats& frameStats = renderer.GetFrameStats();
    const FSceneSyncStats&   sceneStats = FSceneManager::Get().GetStats();

    LIMX_LOG(LogLaunch, Log, "[基准] ======== 测量结果 ========");
    LIMX_LOG(LogLaunch, Log,
             "[基准] 场景: 网格 {} | 剔除 {} | 排序 {}",
             options.GridSize,
             options.EnableCulling ? "开" : "关",
             options.EnableSorting ? "开" : "关");
    LIMX_LOG(LogLaunch, Log,
             "[基准] 批次: 收集 {} | 剔除 {} | 可见 {} | 三角形 {}",
             sceneStats.BatchCount, sceneStats.CulledCount,
             sceneStats.VisibleCount, sceneStats.VisibleTriangles);
    LIMX_LOG(LogLaunch, Log,
             "[基准] 状态切换: 材质 {} 次 | 网格 {} 次",
             sceneStats.MaterialSwitchCount, sceneStats.MeshSwitchCount);
    LIMX_LOG(LogLaunch, Log,
             "[基准] 帧耗时: 平均 {} ms | 最差 {} ms | 平均帧率 {} | 总帧数 {}",
             frameStats.AverageFrameMs, frameStats.WorstFrameMs,
             frameStats.AverageFps, frameStats.TotalFrames);
    LIMX_LOG(LogLaunch, Log, "[基准] ==========================");
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
    static_cast<void>(nCmdShow);

    using namespace Limx;

    // ================================================================
    // 0a. 解析命令行
    // ================================================================

    const FLaunchOptions launchOptions = ParseLaunchOptions(lpCmdLine);

    // ================================================================
    // 0b. 设置工作目录为引擎根目录
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
    // 0c. 初始化文件日志 — 在所有其他操作之前
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
    // 3. 初始化渲染器 (UBO + 材质 + 光照 + Pass 系统)
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
    FSceneManager::Get().SetCullingEnabled(launchOptions.EnableCulling);
    FSceneManager::Get().SetSortingEnabled(launchOptions.EnableSorting);

    // 4b. 创建 LScene — M1.0 对象系统的顶级运行时上下文
    LScene* scene = LScene::Create(FName("DemoScene"));
    LIMX_CHECK(scene != nullptr);

    // 4c. 通过资源管理器创建场景网格, 挂到 LScene 的 LNode+LMeshTrait
    if (launchOptions.ReloadTest)
    {
        const bool passed =
            RunReloadTest(&renderContext, &renderer, launchOptions);

        LRegistry::Get().Destroy(scene);
        FSceneManager::Get().Shutdown();
        renderer.Shutdown();
        renderContext.Shutdown();
        window.Destroy();

        FLog::RemoveSink(&fileLogSink);
        fileLogSink.Close();

        return passed ? 0 : 4;
    }

    if (!launchOptions.ScenePath.IsEmpty())
    {
        if (!LoadSceneFromFile(scene, &renderContext, &renderer,
                               launchOptions))
        {
            // 导入失败时退回内置演示场景 —— 直接退出会让"路径写错"
            // 与"引擎起不来"这两件事在日志里长得一模一样。
            LIMX_LOG(LogLaunch, Warning,
                     "[Launch] 资产导入失败, 回退到内置演示场景");
            BuildDemoScene(scene, &renderContext, &renderer);
        }
    }
    else if (launchOptions.GridSize > 0)
    {
        BuildStressScene(scene, &renderContext, &renderer,
                         launchOptions.GridSize);
    }
    else
    {
        BuildDemoScene(scene, &renderContext, &renderer);
    }

    // 4d. 场景开始播放 — 驱动所有节点和 Trait 的 OnBegin()
    scene->OnBegin();

    LIMX_LOG(LogLaunch, Log,
        "[Launch] M1.0 初始化完成 — LScene '{}' 已启动，{} 个节点",
        scene->GetSceneName().GetCStr(), scene->GetNodeCount());

    // ================================================================
    // 5. 主循环 — Tick 场景 → 同步渲染数据 → 执行帧渲染
    // ================================================================

    Float64 lastFrameTime = FPlatformTime::Seconds();
    UInt64  loopFrame     = 0;
    bool    statsReset    = false;

    while (window.ProcessMessages())
    {
        // 计算帧间隔
        Float64 currentTime = FPlatformTime::Seconds();
        Float32 deltaTime = static_cast<Float32>(currentTime - lastFrameTime);
        lastFrameTime = currentTime;
        deltaTime = FMath::Clamp(deltaTime, 0.0001f, 0.1f);

        // 5a. 场景 Tick — 驱动所有 LNode/LTrait/LSystem 的 Tick()
        scene->Tick(deltaTime);

        // 5b. 同步场景数据到渲染器 — LMeshTrait → 批次列表 → FRenderer
        FSceneManager::Get().SyncScene(scene, deltaTime);

        // 5c. 渲染帧 — 场景 Pass → 提交+呈现
        renderer.RenderFrame();

        ++loopFrame;

        // 5d. 预热结束后清空统计, 使基准只覆盖稳态帧
        if (!statsReset && loopFrame >= launchOptions.WarmupFrames)
        {
            renderer.ResetFrameStats();
            statsReset = true;
        }

        // 5e. 达到帧数上限则退出 (基准模式)
        if (launchOptions.FrameLimit > 0 &&
            loopFrame >= static_cast<UInt64>(launchOptions.WarmupFrames) +
                         static_cast<UInt64>(launchOptions.FrameLimit))
        {
            LogBenchmarkReport(launchOptions, renderer);
            break;
        }
    }

    // ================================================================
    // 6. 关闭 (逆序: 场景→桥接→渲染器→上下文→窗口)
    // ================================================================

    LIMX_LOG(LogLaunch, Log,
        "[Launch] 正在关闭...");

    // 6a. 场景停止播放 — 逆序调用所有节点和 Trait 的 OnEnd()
    scene->OnEnd();

    // 6b. 销毁场景 — LMeshTrait 析构时释放对网格资源的引用。
    //     网格显存本身归 FRenderResourceManager，随 FRenderContext 一同销毁，
    //     此处只释放对象系统的内存和注册表条目。
    LRegistry::Get().Destroy(scene);
    scene = nullptr;

    // 场景放下了它的引用, 此处统一收割 —— 这一步会在日志里给出
    // "场景销毁后确实没有资源残留"的直接证据。
    {
        UInt32 collected = renderContext.GetResourceManager().CollectUnreferenced();
        LIMX_LOG(LogLaunch, Log,
            "[Launch] 场景销毁后回收了 {} 个未引用资源", collected);
    }

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
