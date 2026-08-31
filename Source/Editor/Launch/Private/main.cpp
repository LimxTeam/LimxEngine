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
#include "RenderCore/Environment/FEnvironmentMap.h"
#include "RenderCore/Profiling/FGpuProfiler.h"
#include "Renderer/RenderPass/FPassManager.h"
#include "AssetPipeline/FImageDecoder.h"
#include "Renderer/RenderPass/FDepthPrePass.h"
#include "Core/Math/FFloat16.h"
#include "RenderCore/Profiling/FOctahedral.h"
#include "RenderCore/Lighting/FClusterGrid.h"
#include "Renderer/RenderPass/FClusterLightPass.h"
#include "Renderer/RenderPass/FGtaoPass.h"
#include "Renderer/RenderPass/FBloomPass.h"
#include "Renderer/RenderPass/FShadowAtlasPass.h"
#include "Renderer/RenderPass/FGpuCullPass.h"

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

    /// 并行命令录制的线程数 (0 = 按硬件并发数)
    UInt32 RecordThreads = 0;

    /// 每隔多少帧强制重建一次交换链 (0 = 不重建)
    ///
    /// OnResize 平时只有窗口缩放时才走, 自动化里没有任何东西会改窗口
    /// 尺寸。这个开关让那条路径可以被回归覆盖。
    UInt32 ResizeEveryFrames = 0;

    /// 是否启用并行命令录制
    ///
    /// 关掉时前向 Pass 走内联路径。两条路径共用同一份绘制代码, 因此
    /// 输出应当逐像素相同 —— 这是并行录制唯一有意义的验收方式。
    bool ParallelRecording = true;

    /// 待导入的资产路径; 为空时使用内置场景
    FString ScenePath;

    /// 导入时的统一缩放 —— 不同来源的资产单位不一
    Float32 SceneScale = 1.0f;

    /// 是否加载纹理 —— 关闭可把几何吞吐与纹理带宽分开测量
    bool LoadTextures = true;

    /// 相机是否置于场景内部 —— 建筑内景必须开启, 否则只看得到外墙
    bool CameraInside = false;

    /// 环境 HDRI 路径 (.hdr); 为空时不加载天空盒
    FString HdriPath;

    /// 天空强度的线性倍数 —— HDRI 的绝对量级因拍摄标定而异
    Float32 SkyIntensity = 1.0f;

    /// IBL 强度的线性倍数 —— 与天空强度分开, 便于单独配平
    ///
    /// 两者的合适取值往往不同: 天空是直接看到的背景, 亮度要与实际观感
    /// 一致; 而环境光是照亮物体的间接光, 常常需要压一压才不至于把
    /// 直接光的层次冲掉。
    Float32 IblIntensity = 1.0f;

    /// 相机朝向覆盖 (弧度) —— 用于可复现的截屏对照
    ///
    /// 默认相机会响应键鼠, 而任何"改了渲染再截一张对比"的验证都要求
    /// 两次截屏的机位逐位相同。有了这两个开关, 天空朝向、白炉测试这类
    /// 判断才有可复现的依据。
    bool    OverrideCameraRotation = false;
    Float32 CameraYaw   = 0.0f;
    Float32 CameraPitch = 0.0f;

    /// 是否输出辐照度贴图六个面的中心值
    ///
    /// 卷积算错在画面上只表现为"环境光好像有点不对"。而六个面中心值
    /// 对应六个正交法线的辐照度, 与源图各方向的平均亮度直接可比 ——
    /// 这是能用一行数字判定卷积对错的地方。
    bool ProbeIrradiance = false;

    /// 白炉自检 —— 跑完 IBL 各级预计算的数值断言后立即退出, 以退出码报告
    ///
    /// 与 --furnace 的区别: 后者是让人看的 (渲染出来对着背景比), 前者是
    /// 让 CI 跑的 (读回数值直接断言)。
    bool FurnaceCheck = false;

    /// G-Buffer 自检: 法线编码与速度矢量的数值校验, 以退出码报告
    bool GBufferCheck = false;

    /// 启用时域抗锯齿 (抖动 + 解析, 同开同关)
    bool TemporalAA = false;

    /// TAA 自检: 断言解析结果比任何单帧都更接近多帧平均, 以退出码报告
    bool TaaCheck = false;

    /// 启用屏幕空间环境光遮蔽
    bool Gtao = false;

    /// 启用泛光
    bool Bloom = false;

    /// 泛光的亮度阈值
    Float32 BloomThreshold = 1.0f;

    /// 泛光的合成强度
    Float32 BloomIntensity = 0.05f;

    /// 构建直角墙角场景 (GTAO 自检的解析基准)
    bool CornerScene = false;

    /// 构建单点光源场景 (泛光自检的点扩散基准)
    bool BloomScene = false;

    /// 泛光自检: 断言点扩散函数的对称性与能量守恒, 以退出码报告
    bool BloomCheck = false;

    /// 构建聚光灯阴影场景 (阴影图集自检的相似三角形基准)
    bool ShadowScene = false;

    /// 阴影自检: 断言阴影边界落在相似三角形算出的位置, 以退出码报告
    bool ShadowCheck = false;

    /// 启用 GPU 驱动的剔除与间接绘制
    bool GpuDriven = false;

    /// GPU 驱动自检: 与逐物体绘制逐像素比对, 以退出码报告
    bool GpuDrivenCheck = false;

    /// 阴影场景换成单盏点光源 (立方体阴影)
    bool PointShadow = false;

    /// 阴影场景里投影聚光灯的总数 (含被测的两盏)
    ///
    /// 多出来的都是**填充灯**, 放在相机视野之外。它们的作用是把被测的两盏
    /// 顶到图集的高列上 —— 块下标 62/63 对应纹素 x = 3072 与 3584, 远超
    /// 交换链的宽度。只用两块的话它们都落在 (0,0)-(1024,512) 里, 而那一片
    /// 恰好被上一个 Pass 留下的裁剪矩形覆盖着, 于是"忘了设裁剪矩形"这类
    /// 缺陷根本暴露不出来。
    UInt32 ShadowLights = 2;

    /// GTAO 的采样半径 (世界单位)
    Float32 AoRadius = 0.8f;

    /// GTAO 自检: 在墙角场景上断言解析值, 以退出码报告
    bool AoCheck = false;

    /// 分簇剔除自检: 回读簇表与 CPU 参照逐簇比对, 以退出码报告
    bool ClusterCheck = false;

    /// 关闭分簇, 强制走暴力法 (性能对照用)
    bool NoClustered = false;

    /// 分簇着色自检: 分簇与暴力法逐像素比对, 以退出码报告
    bool LightCullCheck = false;

    /// 白炉测试 —— 用各方向恒为 1 的合成环境替代 HDRI, 并关掉全部直接光
    ///
    /// 能量守恒唯一的客观判据: 反照率为 1 的表面在这个环境下必须原样反射
    /// 回 1, 也就是物体应当完全消失在背景里。任何一处能量被吞掉或凭空多出,
    /// 都会让它从背景里浮现。
    bool Furnace = false;

    /// 材质阵列的边长; 0 表示不构建
    ///
    /// 横向扫粗糙度、纵向扫金属度的球体阵列 —— 验证 IBL 的标准场景。
    UInt32 MaterialGrid = 0;

    /// 在场景上叠加 N×N 盏点光源 (0 = 不叠加)
    ///
    /// 分簇剔除要有东西可剔才验得了。这个开关是**叠加**而非替换场景 ——
    /// 光源得照到几何上才看得出剔除对不对。
    UInt32 LightGrid = 0;

    /// 是否输出 BRDF 查找表的采样网格
    ///
    /// 这张表有一条硬性质可以自查: F0=1 时 A+B 应当 ≤1 且在低粗糙度下
    /// 接近 1 —— 入射能量既不被凭空放大也不被吞掉。数值一取回来就能验,
    /// 不必等到画面上看出金属偏暗。
    bool ProbeBrdf = false;

    /// 截屏输出路径 (.ppm); 为空时不截屏
    ///
    /// 渲染改动的验收最终要落到画面上。没有一条把画面拷出显存的通路,
    /// "天空朝向对不对""色调映射有没有偏"这类问题就只能靠肉眼隔着屏幕猜。
    FString ScreenshotPath;

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

/// 宽字符串转单精度浮点 — 支持 "[+-]整数[.小数]" 形式
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

    // 负号必须支持: 俯仰角、偏移量这类参数天然可以为负, 而漏掉它的失败方式
    // 是"悄悄退回默认值" —— 命令行看着写对了, 行为却是默认的, 排查时极易
    // 怀疑到功能本身而非参数解析上。
    Float32         sign  = 1.0f;
    const WideChar* start = text;

    if (*start == L'-')
    {
        sign = -1.0f;
        ++start;
    }
    else if (*start == L'+')
    {
        ++start;
    }

    if (*start == L'\0')
    {
        return fallback;
    }

    for (const WideChar* cursor = start; *cursor != L'\0'; ++cursor)
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

    return sign * (integerPart + fractionPart);
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
///   --hdri P         加载 Radiance .hdr 作为环境贴图与天空盒
///   --sky-intensity S 天空强度的线性倍数 (默认 1.0)
///   --ibl-intensity S 环境光照强度的线性倍数 (默认 1.0)
///   --screenshot P   末帧截屏写入 P (二进制 PPM, P6)
///   --camera-yaw R   固定相机偏航角 (弧度)
///   --camera-pitch R 固定相机俯仰角 (弧度)
///   --probe-irradiance 输出辐照度贴图六个面的中心值 (数值校验用)
///   --probe-brdf     输出 BRDF 查找表的采样网格 (数值校验用)
///   --material-grid N  构建 N×N 球体阵列 (横向粗糙度, 纵向金属度)
///   --light-grid N   在场景上叠加 N×N 盏点光源 (分簇剔除的压力源)
///   --furnace        白炉测试: 合成均匀环境 + 关闭直接光
///   --furnace-check  白炉自检: 断言 IBL 各级预计算, 以退出码报告
///   --gbuffer-check  G-Buffer 自检: 法线编码与速度矢量校验, 以退出码报告
///   --taa            启用时域抗锯齿 (Halton 2,3 抖动 + 解析通道)
///   --taa-check      TAA 自检: 与多帧平均比对, 以退出码报告
///   --gtao           启用屏幕空间环境光遮蔽
///   --bloom          启用泛光
///   --bloom-threshold T  泛光的亮度阈值 (默认 1.0)
///   --bloom-intensity I  泛光的合成强度 (默认 0.05)
///   --corner-scene   构建直角墙角场景 (GTAO 自检的解析基准)
///   --bloom-scene    构建单点自发光场景 (泛光自检的点扩散基准)
///   --bloom-check    泛光自检: 点扩散的对称性与能量守恒, 以退出码报告
///   --shadow-scene   构建聚光灯阴影场景 (阴影图集自检的相似三角形基准)
///   --shadow-check   阴影自检: 断言阴影边界的解析位置, 以退出码报告
///   --gpu-driven     GPU 驱动的剔除与间接绘制 (相机通道)
///   --gpu-driven-check GPU 驱动自检: 与逐物体绘制逐像素比对, 以退出码报告
///   --point-shadow   阴影场景换成单盏点光源 (立方体阴影, 影子横跨面边界)
///   --shadow-lights N 阴影场景里投影聚光灯的总数 (默认 2, 上限 128)
///                     超过 64 时多出来的拿不到图集的块, 按无遮挡处理
///   --ao-check       GTAO 自检: 断言墙角处的解析值, 以退出码报告
///   --cluster-check  分簇剔除自检: 回读簇表与 CPU 参照比对, 以退出码报告
///   --no-clustered   关闭分簇, 强制暴力遍历全部光源 (性能对照用)
///   --light-cull-check 分簇着色自检: 与暴力法逐像素比对, 以退出码报告
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
        else if (WideEquals(arg, L"--record-threads") && (i + 1) < tokenCount)
        {
            options.RecordThreads = ParseUInt32(tokens[++i], 0);
        }
        else if (WideEquals(arg, L"--resize-test") && (i + 1) < tokenCount)
        {
            options.ResizeEveryFrames = ParseUInt32(tokens[++i], 0);
        }
        else if (WideEquals(arg, L"--no-parallel-record"))
        {
            options.ParallelRecording = false;
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
        else if (WideEquals(arg, L"--hdri") && (i + 1) < tokenCount)
        {
            options.HdriPath = WideToString(tokens[++i]);
        }
        else if (WideEquals(arg, L"--sky-intensity") && (i + 1) < tokenCount)
        {
            options.SkyIntensity = ParseFloat32(tokens[++i], 1.0f);
        }
        else if (WideEquals(arg, L"--ibl-intensity") && (i + 1) < tokenCount)
        {
            options.IblIntensity = ParseFloat32(tokens[++i], 1.0f);
        }
        else if (WideEquals(arg, L"--screenshot") && (i + 1) < tokenCount)
        {
            options.ScreenshotPath = WideToString(tokens[++i]);
        }
        else if (WideEquals(arg, L"--camera-yaw") && (i + 1) < tokenCount)
        {
            options.CameraYaw = ParseFloat32(tokens[++i], 0.0f);
            options.OverrideCameraRotation = true;
        }
        else if (WideEquals(arg, L"--camera-pitch") && (i + 1) < tokenCount)
        {
            options.CameraPitch = ParseFloat32(tokens[++i], 0.0f);
            options.OverrideCameraRotation = true;
        }
        else if (WideEquals(arg, L"--probe-irradiance"))
        {
            options.ProbeIrradiance = true;
        }
        else if (WideEquals(arg, L"--probe-brdf"))
        {
            options.ProbeBrdf = true;
        }
        else if (WideEquals(arg, L"--material-grid") && (i + 1) < tokenCount)
        {
            options.MaterialGrid = ParseUInt32(tokens[++i], 0);
        }
        else if (WideEquals(arg, L"--light-grid") && (i + 1) < tokenCount)
        {
            options.LightGrid = ParseUInt32(tokens[++i], 0);
        }
        else if (WideEquals(arg, L"--furnace"))
        {
            options.Furnace = true;
        }
        else if (WideEquals(arg, L"--no-clustered"))
        {
            options.NoClustered = true;
        }
        else if (WideEquals(arg, L"--light-cull-check"))
        {
            options.LightCullCheck = true;
        }
        else if (WideEquals(arg, L"--cluster-check"))
        {
            options.ClusterCheck = true;
        }
        else if (WideEquals(arg, L"--gtao"))
        {
            options.Gtao = true;
        }
        else if (WideEquals(arg, L"--bloom"))
        {
            options.Bloom = true;
        }
        else if (WideEquals(arg, L"--bloom-threshold") && (i + 1) < tokenCount)
        {
            options.BloomThreshold = ParseFloat32(tokens[++i], 1.0f);
        }
        else if (WideEquals(arg, L"--bloom-intensity") && (i + 1) < tokenCount)
        {
            options.BloomIntensity = ParseFloat32(tokens[++i], 0.05f);
        }
        else if (WideEquals(arg, L"--corner-scene"))
        {
            options.CornerScene = true;
        }
        else if (WideEquals(arg, L"--bloom-scene"))
        {
            options.BloomScene = true;
        }
        else if (WideEquals(arg, L"--bloom-check"))
        {
            options.BloomCheck = true;
        }
        else if (WideEquals(arg, L"--shadow-scene"))
        {
            options.ShadowScene = true;
        }
        else if (WideEquals(arg, L"--shadow-check"))
        {
            options.ShadowCheck = true;
        }
        else if (WideEquals(arg, L"--gpu-driven"))
        {
            options.GpuDriven = true;
        }
        else if (WideEquals(arg, L"--gpu-driven-check"))
        {
            options.GpuDrivenCheck = true;
        }
        else if (WideEquals(arg, L"--point-shadow"))
        {
            options.PointShadow = true;
        }
        else if (WideEquals(arg, L"--shadow-lights") && (i + 1) < tokenCount)
        {
            options.ShadowLights = ParseUInt32(tokens[++i], 2u);
        }
        else if (WideEquals(arg, L"--ao-radius") && (i + 1) < tokenCount)
        {
            options.AoRadius = ParseFloat32(tokens[++i], 0.8f);
        }
        else if (WideEquals(arg, L"--ao-check"))
        {
            options.AoCheck = true;
        }
        else if (WideEquals(arg, L"--taa-check"))
        {
            options.TaaCheck = true;
        }
        else if (WideEquals(arg, L"--taa"))
        {
            options.TemporalAA = true;
        }
        else if (WideEquals(arg, L"--gbuffer-check"))
        {
            options.GBufferCheck = true;
        }
        else if (WideEquals(arg, L"--furnace-check"))
        {
            options.FurnaceCheck = true;
        }
    }

    return options;
}

// ============================================================================
// FScreenshotCapture — 把最后呈现的画面拷回内存并写成 PPM
//
// 拷贝必须录进**当前帧的**命令缓冲区, 而不能在帧外另起一个一次性提交:
// 交换链图像只在"取得 (acquire) 到呈现 (present)"这段窗口内归应用所有,
// 帧外去转换它的布局是明确的规范违例 —— 验证层会指出"该图像尚未被取得"。
// 因此这里分成两步: 帧内录制拷贝命令, 帧后等 GPU 空闲再读缓冲区。
//
// 写 PPM 而非 PNG: 渲染验收要的是"画面到底是什么颜色", PPM 是逐字节无损、
// 无压缩、格式说明只有三行的容器 —— 引入压缩编码器只会在验收链条上多一个
// 可能出错的环节。需要 PNG 时用任意工具转一次即可。
// ============================================================================

// ============================================================================
// FErrorCountingSink — 统计 Error 及以上级别的日志条数
//
// 存在的理由是一整类"报了错但退出码是 0"的情况。最典型的是显存泄漏:
// 检测发生在 renderContext.Shutdown() 里面, 而各条自检的 passed 值早在
// 那之前就算完并 return 了 —— 泄漏于是只在日志里留一行 Error, 而 CI 看
// 的是退出码。
//
// 专门给泄漏加一个访问器也能解决那一条, 但下一条同类问题还会再出现一次。
// 数 Error 覆盖的是整个类别: 验证层报错、资源管理器抱怨、关闭顺序不对、
// 以及所有还没写出来的。
//
// 只在自检模式下参与判定。普通运行不判 —— 那些场合下 Error 可能是"用户
// 给的路径不存在"之类的正常反馈, 拿它决定退出码会制造假失败。
// ============================================================================
class FErrorCountingSink final : public ILogSink
{
public:
    void Write(const LogCategory& category,
               LogVerbosity       verbosity,
               const AnsiChar*    message) override
    {
        (void)category;
        (void)message;

        if (verbosity == LogVerbosity::Error || verbosity == LogVerbosity::Fatal)
        {
            ++m_Count;
        }
    }

    LIMX_NODISCARD UInt32 GetCount() const { return m_Count; }

private:
    UInt32 m_Count = 0;
};

/// 自检的最终退出码 —— 把"关闭阶段才冒出来的 Error"并进判定
///
/// selfCheckCode 是自检自身失败时该返回的码; 关闭阶段出错另给一个码,
/// 好让日志里一眼看出是哪一类问题。
static int FinalizeSelfCheck(bool                     passed,
                             int                      selfCheckCode,
                             const FErrorCountingSink& errorSink,
                             UInt32                   errorsBeforeShutdown)
{
    // 自检本身的失败优先报告 —— 它比"关闭阶段有 Error"更具体。
    // 反过来的顺序会让一个失败的自检因为顺带有泄漏而只报 8, 丢掉信息。
    if (!passed)
    {
        return selfCheckCode;
    }

    const UInt32 shutdownErrors = errorSink.GetCount() - errorsBeforeShutdown;

    if (shutdownErrors > 0)
    {
        LIMX_LOG(LogLaunch, Display,
                 "[自检] 自检项全部通过, 但关闭阶段出现 {} 条 Error —— "
                 "判定为失败 (显存泄漏、资源未回收一类的问题只在这个阶段"
                 "才会暴露)",
                 shutdownErrors);
        return 8;
    }

    return 0;
}

// ============================================================================
// FGBufferCapture — 法线与速度附件的回读
//
// 与 FScreenshotCapture 分开而不是共用: 那个读的是交换链图像 (布局
// PresentSrc、格式 BGRA8), 这个读的是两张 RG16_SFLOAT 附件 (布局
// ShaderReadOnly)。共用的话两条路径的布局转换都得参数化, 而布局转换写错
// 的表现是验证层报错或读到垃圾, 不值得为省几十行代码去冒险。
//
// **深度附件不在这里回读**: 前向 Pass 对深度的 StoreOp 是 DontCare, 场景
// 渲染结束后深度内容是未定义的。覆盖掩码改用法线附件的哨兵值判定。
// ============================================================================
class FGBufferCapture
{
public:
    /// 准备回读缓冲区 —— 必须在录制拷贝命令之前调用
    bool Request(FRenderContext* context)
    {
        IRHIDevice* device = context->GetDevice();

        if (device == nullptr)
        {
            return false;
        }

        // 先归还上一次的。本类会被连续 Request 两次 (运动帧与静止帧),
        // 不释放就直接覆盖句柄 = 两个缓冲区泄漏, 而泄漏只在关闭时才报。
        Release(device);

        m_Extent = context->GetSwapchainExtent();

        if (m_Extent.Width == 0 || m_Extent.Height == 0)
        {
            return false;
        }

        // RG16_SFLOAT = 每像素 4 字节
        const SizeType byteSize =
            static_cast<SizeType>(m_Extent.Width) * m_Extent.Height * 4;

        const char* const names[2] =
        {
            "GBufferCheck.NormalReadback",
            "GBufferCheck.VelocityReadback",
        };

        for (UInt32 i = 0; i < 2; ++i)
        {
            FRHIBufferDesc desc = {};
            desc.Size        = byteSize;
            desc.Usage       = EBufferUsage::TransferDst;
            desc.MemoryUsage = EMemoryUsage::GpuToCpu;
            desc.DebugName   = names[i];

            if (!IsRHISuccess(device->CreateBuffer(desc, m_Readback[i])))
            {
                LIMX_LOG(LogLaunch, Error,
                         "[GBuffer] 回读缓冲区创建失败: {}", names[i]);
                Release(device);
                return false;
            }
        }

        m_IsPending = true;
        return true;
    }

    /// 帧内录制拷贝命令 —— 由场景渲染后回调驱动
    void RecordCopy(FRenderContext* context, FDepthPrePass* pass)
    {
        if (!m_IsPending || pass == nullptr)
        {
            return;
        }

        IRHICommandBuffer* commandBuffer = context->GetCurrentCommandBuffer();

        if (commandBuffer == nullptr)
        {
            return;
        }

        const FRHITextureHandle textures[2] =
        {
            pass->GetNormalTexture(),
            pass->GetVelocityTexture(),
        };

        for (UInt32 i = 0; i < 2; ++i)
        {
            if (!textures[i].IsValid())
            {
                LIMX_LOG(LogLaunch, Error,
                         "[GBuffer] 第 {} 张附件句柄无效", i);
                return;
            }

            // 深度预通道的 FinalLayout 是 ShaderReadOnly, 而且之后没有任何
            // 通道再动这两张图 —— 所以这里的旧布局是确定的。
            commandBuffer->TransitionImageLayout(
                textures[i],
                EImageLayout::ShaderReadOnly,
                EImageLayout::TransferSrc,
                EPipelineStageFlags::FragmentShader,
                EPipelineStageFlags::Transfer,
                EAccessFlags::ShaderRead,
                EAccessFlags::TransferRead);

            FRHIBufferTextureCopyRegion region = {};
            region.BufferOffset      = 0;
            region.BufferRowLength   = 0;
            region.BufferImageHeight = 0;
            region.MipLevel          = 0;
            region.BaseLayer         = 0;
            region.LayerCount        = 1;
            region.TextureOffset     = { 0, 0, 0 };
            region.TextureExtent     = { m_Extent.Width, m_Extent.Height, 1 };

            commandBuffer->CopyTextureToBuffer(
                textures[i], EImageLayout::TransferSrc, m_Readback[i], region);

            // 必须转回去 —— 下一帧的渲染通道声明的 InitialLayout 是
            // Undefined, 但那意味着"内容可丢弃", 不意味着"任何旧布局都行"。
            // 留在 TransferSrc 上会让下一帧的布局转换从错误的旧布局开始。
            commandBuffer->TransitionImageLayout(
                textures[i],
                EImageLayout::TransferSrc,
                EImageLayout::ShaderReadOnly,
                EPipelineStageFlags::Transfer,
                EPipelineStageFlags::FragmentShader,
                EAccessFlags::TransferRead,
                EAccessFlags::ShaderRead);
        }

        m_IsPending  = false;
        m_IsRecorded = true;
    }

    /// 等 GPU 空闲后把两张图解成 FVector2 数组
    bool Resolve(FRenderContext* context)
    {
        IRHIDevice* device = context->GetDevice();

        if (device == nullptr || !m_IsRecorded)
        {
            LIMX_LOG(LogLaunch, Error, "[GBuffer] 拷贝命令未录制");
            return false;
        }

        device->WaitIdle();

        const SizeType pixelCount =
            static_cast<SizeType>(m_Extent.Width) * m_Extent.Height;

        TArray<FVector2>* const targets[2] = { &m_Normal, &m_Velocity };

        for (UInt32 i = 0; i < 2; ++i)
        {
            void* mapped = nullptr;

            if (!IsRHISuccess(device->MapBuffer(m_Readback[i], &mapped)) ||
                mapped == nullptr)
            {
                LIMX_LOG(LogLaunch, Error, "[GBuffer] 回读缓冲区映射失败");
                return false;
            }

            const Float16Bits* source =
                static_cast<const Float16Bits*>(mapped);

            targets[i]->Clear();
            targets[i]->Reserve(pixelCount);

            for (SizeType p = 0; p < pixelCount; ++p)
            {
                targets[i]->Add(
                    FVector2(Float16ToFloat32(source[p * 2 + 0]),
                             Float16ToFloat32(source[p * 2 + 1])));
            }

            device->UnmapBuffer(m_Readback[i]);
        }

        m_IsRecorded = false;
        return true;
    }

    void Release(IRHIDevice* device)
    {
        if (device == nullptr)
        {
            return;
        }

        for (UInt32 i = 0; i < 2; ++i)
        {
            if (m_Readback[i].IsValid())
            {
                device->DestroyBuffer(m_Readback[i]);
                m_Readback[i] = {};
            }
        }

        m_IsPending  = false;
        m_IsRecorded = false;
    }

    LIMX_NODISCARD const TArray<FVector2>& GetNormal() const
    {
        return m_Normal;
    }

    LIMX_NODISCARD const TArray<FVector2>& GetVelocity() const
    {
        return m_Velocity;
    }

    LIMX_NODISCARD FRHIExtent2D GetExtent() const { return m_Extent; }

private:
    FRHIBufferHandle m_Readback[2] = {};
    FRHIExtent2D     m_Extent      = {};
    TArray<FVector2> m_Normal;
    TArray<FVector2> m_Velocity;
    bool             m_IsPending   = false;
    bool             m_IsRecorded  = false;
};

class FScreenshotCapture
{
public:
    /// 请求在下一帧结束时截屏
    ///
    /// @return 是否成功准备好回读缓冲区
    bool Request(FRenderContext* context)
    {
        IRHIDevice* device = context->GetDevice();

        if (device == nullptr)
        {
            return false;
        }

        m_Extent = context->GetSwapchainExtent();
        m_Format = context->GetSwapchainFormat();

        if (m_Extent.Width == 0 || m_Extent.Height == 0)
        {
            return false;
        }

        const SizeType byteSize =
            static_cast<SizeType>(m_Extent.Width) * m_Extent.Height * 4;

        FRHIBufferDesc readbackDesc = {};
        readbackDesc.Size        = byteSize;
        readbackDesc.Usage       = EBufferUsage::TransferDst;
        readbackDesc.MemoryUsage = EMemoryUsage::GpuToCpu;
        readbackDesc.DebugName   = "Screenshot.Readback";

        if (!IsRHISuccess(device->CreateBuffer(readbackDesc, m_Readback)))
        {
            LIMX_LOG(LogLaunch, Warning,
                     "[Launch] 截屏失败: 回读缓冲区创建失败");
            return false;
        }

        m_IsPending = true;
        return true;
    }

    /// 在帧内录制拷贝命令 —— 由 FRenderer 的场景后回调调用
    void RecordCopy(FRenderContext* context)
    {
        if (!m_IsPending)
        {
            return;
        }

        IRHICommandBuffer* commandBuffer = context->GetCurrentCommandBuffer();
        IRHIDevice*        device        = context->GetDevice();

        if (commandBuffer == nullptr || device == nullptr)
        {
            return;
        }

        const FRHITextureHandle image = device->GetSwapchainImage(
            context->GetSwapchain(), context->GetCurrentImageIndex());

        if (!image.IsValid())
        {
            return;
        }

        // 后处理 Pass 把交换链图像停在 PresentSrc。转为传输源拷出后必须
        // 转回去 —— 呈现要求图像处于 PresentSrc。
        commandBuffer->TransitionImageLayout(
            image,
            EImageLayout::PresentSrc,
            EImageLayout::TransferSrc,
            EPipelineStageFlags::ColorAttachmentOutput,
            EPipelineStageFlags::Transfer,
            EAccessFlags::ColorAttachmentWrite,
            EAccessFlags::TransferRead);

        FRHIBufferTextureCopyRegion region = {};
        region.BufferOffset      = 0;
        region.BufferRowLength   = 0;
        region.BufferImageHeight = 0;
        region.MipLevel          = 0;
        region.BaseLayer         = 0;
        region.LayerCount        = 1;
        region.TextureOffset     = { 0, 0, 0 };
        region.TextureExtent     = { m_Extent.Width, m_Extent.Height, 1 };

        commandBuffer->CopyTextureToBuffer(image, EImageLayout::TransferSrc,
                                           m_Readback, region);

        commandBuffer->TransitionImageLayout(
            image,
            EImageLayout::TransferSrc,
            EImageLayout::PresentSrc,
            EPipelineStageFlags::Transfer,
            EPipelineStageFlags::BottomOfPipe,
            EAccessFlags::TransferRead,
            EAccessFlags::None);

        m_IsPending  = false;
        m_IsRecorded = true;
    }

    /// 等 GPU 空闲后读出缓冲区并写文件
    bool WriteFile(FRenderContext* context, const FString& path)
    {
        IRHIDevice* device = context->GetDevice();

        if (device == nullptr || !m_IsRecorded || !m_Readback.IsValid())
        {
            LIMX_LOG(LogLaunch, Warning, "[Launch] 截屏失败: 拷贝未录制");
            Release(device);
            return false;
        }

        device->WaitIdle();

        void* mapped = nullptr;

        if (!IsRHISuccess(device->MapBuffer(m_Readback, &mapped)) ||
            mapped == nullptr)
        {
            LIMX_LOG(LogLaunch, Warning,
                     "[Launch] 截屏失败: 回读缓冲区映射失败");
            Release(device);
            return false;
        }

        const UInt8*   source     = static_cast<const UInt8*>(mapped);
        const SizeType pixelCount =
            static_cast<SizeType>(m_Extent.Width) * m_Extent.Height;

        const FString header = StringFormat("P6\n{} {}\n255\n",
                                            m_Extent.Width, m_Extent.Height);

        TArray<UInt8> file;
        file.Reserve(header.GetLength() + pixelCount * 3);

        for (SizeType i = 0; i < header.GetLength(); ++i)
        {
            file.Add(static_cast<UInt8>(header[i]));
        }

        // 交换链常见格式是 BGRA, PPM 要求 RGB —— 通道序在这里显式换。
        // 写反的表现是"天是橙的、地是蓝的", 看着像色调映射出了问题。
        const bool isBgra = (m_Format == EPixelFormat::BGRA8_UNORM) ||
                            (m_Format == EPixelFormat::BGRA8_SRGB);

        for (SizeType i = 0; i < pixelCount; ++i)
        {
            const UInt8* texel = source + i * 4;

            file.Add(isBgra ? texel[2] : texel[0]);
            file.Add(texel[1]);
            file.Add(isBgra ? texel[0] : texel[2]);
        }

        device->UnmapBuffer(m_Readback);
        Release(device);

        const bool written = FPlatformFile::WriteAllBytes(path, file.GetData(),
                                                          file.GetSize());

        if (written)
        {
            LIMX_LOG(LogLaunch, Display,
                     "[Launch] 截屏已写入: {} ({}x{})",
                     path.GetCStr(), m_Extent.Width, m_Extent.Height);
        }
        else
        {
            LIMX_LOG(LogLaunch, Warning,
                     "[Launch] 截屏写入失败: {}", path.GetCStr());
        }

        return written;
    }

    /// 等 GPU 空闲后把像素读成 RGB 字节数组 (不写文件)
    ///
    /// 与 WriteFile 共用同一套通道序处理。分簇与暴力法的逐像素比对要的是
    /// 内存里的两张图 —— 落盘再读回来除了慢没有别的好处, 而且会让比对的
    /// 对象变成"两个 PPM 文件", 多一层可能出错的编解码。
    bool ReadPixels(FRenderContext* context, TArray<UInt8>& outPixels)
    {
        IRHIDevice* device = context->GetDevice();

        if (device == nullptr || !m_IsRecorded || !m_Readback.IsValid())
        {
            LIMX_LOG(LogLaunch, Error, "[截屏] 拷贝未录制");
            return false;
        }

        device->WaitIdle();

        void* mapped = nullptr;

        if (!IsRHISuccess(device->MapBuffer(m_Readback, &mapped)) ||
            mapped == nullptr)
        {
            LIMX_LOG(LogLaunch, Error, "[截屏] 回读缓冲区映射失败");
            return false;
        }

        const UInt8*   source     = static_cast<const UInt8*>(mapped);
        const SizeType pixelCount =
            static_cast<SizeType>(m_Extent.Width) * m_Extent.Height;

        const bool isBgra = (m_Format == EPixelFormat::BGRA8_UNORM) ||
                            (m_Format == EPixelFormat::BGRA8_SRGB);

        outPixels.Clear();
        outPixels.Reserve(pixelCount * 3);

        for (SizeType i = 0; i < pixelCount; ++i)
        {
            const UInt8* texel = source + i * 4;

            outPixels.Add(isBgra ? texel[2] : texel[0]);
            outPixels.Add(texel[1]);
            outPixels.Add(isBgra ? texel[0] : texel[2]);
        }

        device->UnmapBuffer(m_Readback);

        m_IsRecorded = false;
        return true;
    }

    void Release(IRHIDevice* device)
    {
        if (device != nullptr && m_Readback.IsValid())
        {
            device->DestroyBuffer(m_Readback);
        }

        m_IsPending  = false;
        m_IsRecorded = false;
    }

private:
    FRHIBufferHandle m_Readback;
    FRHIExtent2D     m_Extent     = {};
    EPixelFormat     m_Format     = EPixelFormat::Unknown;
    bool             m_IsPending  = false;
    bool             m_IsRecorded = false;
};

// ============================================================================
// LoadEnvironmentMap — 解码 HDRI 并转换为环境立方体贴图
//
// 失败时只警告不中断: 天空盒是可选的, 缺了它场景照常渲染 (背景为清屏色)。
// 把"HDRI 路径写错"升级成启动失败, 只会让排查变难。
// ============================================================================

static bool LoadEnvironmentMap(FEnvironmentMap& environmentMap,
                               FRenderContext*  context,
                               FRenderer*       renderer,
                               const FLaunchOptions& options)
{
    const Float64 beginTime = FPlatformTime::Seconds();

    // 关闭 16 位降级 —— 它只对整数格式有意义, 但显式关掉能表明意图:
    // 这条路径上的任何精度损失都是不可接受的
    FImageDecodeOptions decodeOptions;
    decodeOptions.ForceFourChannels       = true;
    decodeOptions.ReduceSixteenBitToEight = false;

    FImageData               image;
    const FImageDecodeResult decodeResult =
        FImageDecoder::DecodeFile(options.HdriPath, image, decodeOptions);

    if (!decodeResult.Succeeded)
    {
        LIMX_LOG(LogLaunch, Warning,
                 "[Launch] HDRI 解码失败 ({}): {}",
                 options.HdriPath.GetCStr(),
                 decodeResult.ErrorMessage.GetCStr());
        return false;
    }

    for (SizeType i = 0; i < decodeResult.Warnings.GetSize(); ++i)
    {
        LIMX_LOG(LogLaunch, Warning,
                 "[Launch] HDRI 警告: {}",
                 decodeResult.Warnings[i].GetCStr());
    }

    // 等距柱状图的宽高比应为 2:1。偏离时仍然继续 —— 转换本身只依赖
    // 球面角映射, 比例不对只会让某些方向的采样密度不均, 而非解不出来。
    if (image.Width != image.Height * 2)
    {
        LIMX_LOG(LogLaunch, Warning,
                 "[Launch] HDRI 宽高比不是 2:1 ({}x{}), 天空可能变形",
                 image.Width, image.Height);
    }

    const ERHIResult buildResult =
        environmentMap.BuildFromEquirect(context, image);

    if (!IsRHISuccess(buildResult))
    {
        LIMX_LOG(LogLaunch, Warning,
                 "[Launch] 立方体贴图转换失败: {}",
                 static_cast<Int32>(buildResult));
        return false;
    }

    if (options.ProbeIrradiance)
    {
        TArray<Float32> irradiance;
        UInt32          faceSize = 0;

        if (environmentMap.ReadbackIrradiance(context, irradiance, faceSize))
        {
            // 六个面的中心纹素 —— 分别对应 +X/-X/+Y/-Y/+Z/-Z 六个法线
            static const AnsiChar* kFaceNames[6] =
            { "+X", "-X", "+Y(上)", "-Y(下)", "+Z", "-Z" };

            const SizeType faceTexels =
                static_cast<SizeType>(faceSize) * faceSize;

            const SizeType centerIndex =
                static_cast<SizeType>(faceSize / 2) * faceSize + faceSize / 2;

            for (UInt32 face = 0; face < 6; ++face)
            {
                const SizeType base = (face * faceTexels + centerIndex) * 4;

                LIMX_LOG(LogLaunch, Display,
                         "[辐照度] {} 面中心 = ({}, {}, {})",
                         kFaceNames[face],
                         irradiance[base + 0],
                         irradiance[base + 1],
                         irradiance[base + 2]);
            }
        }
        else
        {
            LIMX_LOG(LogLaunch, Warning, "[Launch] 辐照度回读失败");
        }
    }

    if (options.ProbeBrdf)
    {
        TArray<Float32> lut;
        UInt32          size = 0;

        if (environmentMap.ReadbackBrdfLut(context, lut, size))
        {
            // 5x5 网格 —— 足以看出表的整体形状与边界行为,
            // 又少到能直接与参考实现逐个对照
            constexpr UInt32 kSteps = 5;

            Float32 worstSum = 0.0f;

            for (UInt32 ry = 0; ry < kSteps; ++ry)
            {
                for (UInt32 rx = 0; rx < kSteps; ++rx)
                {
                    const UInt32 x = (size - 1) * rx / (kSteps - 1);
                    const UInt32 y = (size - 1) * ry / (kSteps - 1);

                    const SizeType index =
                        (static_cast<SizeType>(y) * size + x) * 2;

                    const Float32 a = lut[index];
                    const Float32 b = lut[index + 1];

                    // 纹素中心的坐标 —— 与着色器里的取法一致
                    const Float32 nDotV     = (static_cast<Float32>(x) + 0.5f) /
                                              static_cast<Float32>(size);
                    const Float32 roughness = (static_cast<Float32>(y) + 0.5f) /
                                              static_cast<Float32>(size);

                    LIMX_LOG(LogLaunch, Display,
                             "[BRDF] NdotV={} rough={} → A={} B={} A+B={}",
                             nDotV, roughness, a, b, a + b);

                    if (a + b > worstSum)
                    {
                        worstSum = a + b;
                    }
                }
            }

            LIMX_LOG(LogLaunch, Display,
                     "[BRDF] 网格内 A+B 的最大值 = {} (必须 <= 1)", worstSum);
        }
        else
        {
            LIMX_LOG(LogLaunch, Warning, "[Launch] BRDF 查找表回读失败");
        }
    }

    renderer->SetEnvironmentMap(&environmentMap);
    renderer->SetSkyIntensity(options.SkyIntensity);
    renderer->SetIblIntensity(options.IblIntensity);

    LIMX_LOG(LogLaunch, Display,
             "[Launch] 环境贴图就绪: {} ({}x{} → 立方体 {}, "
             "天空强度 {}, IBL 强度 {}), 总耗时 {} ms",
             options.HdriPath.GetCStr(), image.Width, image.Height,
             environmentMap.GetFaceSize(), options.SkyIntensity,
             options.IblIntensity,
             static_cast<Int32>(
                 (FPlatformTime::Seconds() - beginTime) * 1000.0));

    return true;
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
// BuildFurnaceEnvironment — 各方向辐射度恒为 1 的合成环境 (白炉)
//
// 白炉测试是能量守恒唯一的客观判据。把物体放进一个各向辐射度处处为 1 的
// 环境里, 一个能量守恒、反照率为 1 的表面必须原样反射回 1 —— 也就是说
// 物体应当**完全消失**在背景里, 无论粗糙度、金属度取什么值。
//
// 它之所以有力, 在于它把"看着差不多"变成了"逐像素相等": 任何一处能量
// 被吞掉或凭空多出来, 都会让物体从背景里浮现出来。而这种偏差在真实
// HDRI 下完全看不出 —— 环境本来就有明暗, 物体比背景暗一点毫不可疑。
//
// 尺寸取 64x32 就够: 环境处处相同, 再大也只是重复同一个数。
// ============================================================================

static FImageData BuildFurnaceEnvironment()
{
    constexpr UInt32 kWidth  = 64;
    constexpr UInt32 kHeight = 32;

    FImageData image;
    image.Width          = kWidth;
    image.Height         = kHeight;
    image.Format         = EImageFormat::RGBA32F;
    image.ColorSpace     = EImageColorSpace::Linear;
    image.HasSourceAlpha = false;

    const SizeType texelCount = static_cast<SizeType>(kWidth) * kHeight;

    image.Pixels.SetSize(texelCount * 4 * sizeof(Float32));

    Float32* texels = reinterpret_cast<Float32*>(image.Pixels.GetData());

    for (SizeType i = 0; i < texelCount; ++i)
    {
        texels[i * 4 + 0] = 1.0f;
        texels[i * 4 + 1] = 1.0f;
        texels[i * 4 + 2] = 1.0f;
        texels[i * 4 + 3] = 1.0f;
    }

    return image;
}

// ============================================================================
// ============================================================================
// FClusterCapture — 回读簇表
//
// 三个缓冲区: 簇包围盒、每簇的 (起点,数量)、全局光源索引表。它们都是
// GpuOnly + TransferSrc, 拷进 GpuToCpu 的回读缓冲区之后在 CPU 上比对。
// ============================================================================
class FClusterCapture
{
public:
    bool Request(FRenderContext* context)
    {
        IRHIDevice* device = context->GetDevice();

        if (device == nullptr)
        {
            return false;
        }

        Release(device);

        const UInt64 sizes[3] =
        {
            static_cast<UInt64>(kClusterCount) * 2u * 16u,
            static_cast<UInt64>(kClusterCount) * 8u,
            static_cast<UInt64>(kClusterLightIndexCapacity) * 4u,
        };

        const char* const names[3] =
        {
            "ClusterCheck.Bounds",
            "ClusterCheck.Grid",
            "ClusterCheck.Indices",
        };

        for (UInt32 i = 0; i < 3; ++i)
        {
            FRHIBufferDesc desc = {};
            desc.Size        = sizes[i];
            desc.Usage       = EBufferUsage::TransferDst;
            desc.MemoryUsage = EMemoryUsage::GpuToCpu;
            desc.DebugName   = names[i];

            if (!IsRHISuccess(device->CreateBuffer(desc, m_Readback[i])))
            {
                LIMX_LOG(LogLaunch, Error,
                         "[Cluster] 回读缓冲区创建失败: {}", names[i]);
                Release(device);
                return false;
            }

            m_Sizes[i] = sizes[i];
        }

        m_IsPending = true;
        return true;
    }

    void RecordCopy(FRenderContext* context, FClusterLightPass* pass)
    {
        if (!m_IsPending || pass == nullptr)
        {
            return;
        }

        IRHICommandBuffer* commandBuffer = context->GetCurrentCommandBuffer();

        if (commandBuffer == nullptr)
        {
            return;
        }

        const UInt32 frameIndex = context->GetCurrentFrameIndex();

        const FRHIBufferHandle sources[3] =
        {
            pass->GetClusterBoundsBuffer(frameIndex),
            pass->GetClusterGridBuffer(frameIndex),
            pass->GetLightIndexBuffer(frameIndex),
        };

        for (UInt32 i = 0; i < 3; ++i)
        {
            if (!sources[i].IsValid())
            {
                LIMX_LOG(LogLaunch, Error, "[Cluster] 第 {} 个源缓冲区无效", i);
                return;
            }

            // 计算通道写完之后要等它可读。分簇通道内部已经下了一道
            // ComputeShader → FragmentShader 的屏障, 但那道屏障的目的阶段
            // 不含 Transfer —— 拷贝需要自己的一道。
            FRHIBufferMemoryBarrier barrier = {};
            barrier.SrcAccessMask = EAccessFlags::ShaderWrite;
            barrier.DstAccessMask = EAccessFlags::TransferRead;
            barrier.Buffer        = sources[i];

            commandBuffer->PipelineBarrier(
                EPipelineStageFlags::ComputeShader,
                EPipelineStageFlags::Transfer,
                nullptr, 0, &barrier, 1, nullptr, 0);

            FRHIBufferCopyRegion region = {};
            region.SrcOffset = 0;
            region.DstOffset = 0;
            region.Size      = m_Sizes[i];

            commandBuffer->CopyBuffer(sources[i], m_Readback[i], region);
        }

        m_IsPending  = false;
        m_IsRecorded = true;
    }

    bool Resolve(FRenderContext* context)
    {
        IRHIDevice* device = context->GetDevice();

        if (device == nullptr || !m_IsRecorded)
        {
            LIMX_LOG(LogLaunch, Error, "[Cluster] 拷贝命令未录制");
            return false;
        }

        device->WaitIdle();

        m_Bounds.Clear();
        m_Grid.Clear();
        m_Indices.Clear();

        // 包围盒: 每簇两个 vec4
        {
            void* mapped = nullptr;

            if (!IsRHISuccess(device->MapBuffer(m_Readback[0], &mapped)) ||
                mapped == nullptr)
            {
                return false;
            }

            const Float32* source = static_cast<const Float32*>(mapped);

            m_Bounds.Reserve(static_cast<SizeType>(kClusterCount) * 8u);

            for (SizeType i = 0; i < static_cast<SizeType>(kClusterCount) * 8u;
                 ++i)
            {
                m_Bounds.Add(source[i]);
            }

            device->UnmapBuffer(m_Readback[0]);
        }

        // 每簇 (起点, 数量)
        {
            void* mapped = nullptr;

            if (!IsRHISuccess(device->MapBuffer(m_Readback[1], &mapped)) ||
                mapped == nullptr)
            {
                return false;
            }

            const UInt32* source = static_cast<const UInt32*>(mapped);

            m_Grid.Reserve(static_cast<SizeType>(kClusterCount) * 2u);

            for (SizeType i = 0; i < static_cast<SizeType>(kClusterCount) * 2u;
                 ++i)
            {
                m_Grid.Add(source[i]);
            }

            device->UnmapBuffer(m_Readback[1]);
        }

        // 索引表
        {
            void* mapped = nullptr;

            if (!IsRHISuccess(device->MapBuffer(m_Readback[2], &mapped)) ||
                mapped == nullptr)
            {
                return false;
            }

            const UInt32* source = static_cast<const UInt32*>(mapped);

            m_Indices.Reserve(kClusterLightIndexCapacity);

            for (SizeType i = 0; i < kClusterLightIndexCapacity; ++i)
            {
                m_Indices.Add(source[i]);
            }

            device->UnmapBuffer(m_Readback[2]);
        }

        m_IsRecorded = false;
        return true;
    }

    void Release(IRHIDevice* device)
    {
        if (device == nullptr)
        {
            return;
        }

        for (UInt32 i = 0; i < 3; ++i)
        {
            if (m_Readback[i].IsValid())
            {
                device->DestroyBuffer(m_Readback[i]);
                m_Readback[i] = {};
            }
        }

        m_IsPending  = false;
        m_IsRecorded = false;
    }

    LIMX_NODISCARD const TArray<Float32>& GetBounds() const { return m_Bounds; }
    LIMX_NODISCARD const TArray<UInt32>&  GetGrid() const { return m_Grid; }
    LIMX_NODISCARD const TArray<UInt32>&  GetIndices() const { return m_Indices; }

private:
    FRHIBufferHandle m_Readback[3] = {};
    UInt64           m_Sizes[3]    = {};
    TArray<Float32>  m_Bounds;
    TArray<UInt32>   m_Grid;
    TArray<UInt32>   m_Indices;
    bool             m_IsPending   = false;
    bool             m_IsRecorded  = false;
};

// ============================================================================
// 聚光灯阴影场景的几何 —— 构建与自检共用同一份数字
//
// 单独列出来而不是各写一遍: 自检要拿相似三角形算出影子边界的**解析位置**,
// 而那个算式的输入就是这里的每一个数。两处各写一份的话, 改了场景忘了改
// 自检, 自检会拿旧的解析值去比新的画面 —— 失败在场景这一侧, 而人会去查
// 阴影的实现。
//
// ── 为什么灯必须在相机轴上 ──
//
// 遮挡物在画面上会挡住一部分它自己的影子。挡多少取决于两个放大率:
//   相机把 z=zo 处的东西放大 zc/(zc-zo) 倍;
//   灯把它放大 h/(h-zo) 倍投到墙上。
// 灯比相机离遮挡物近 (h < zc), 所以灯的放大率更大 —— **只要两者同轴**,
// 影子就一定落在遮挡物的像之外, 不会被挡。灯偏离相机轴的话这个保证就没了,
// 影子边界会被遮挡物的像盖住一段, 而那一段恰恰是要量的东西。
// ============================================================================

namespace ShadowScene
{

/// 接收面 (墙) 所在的 z
inline constexpr Float32 kWallZ = 0.0f;

/// 相机 z —— 必须大于灯的 z, 理由见上
inline constexpr Float32 kCameraZ = 10.0f;

/// 主灯 z (在相机轴 x=y=0 上)
inline constexpr Float32 kLightZ = 6.0f;

/// 遮挡物 (薄板) 的中心与半尺寸
inline constexpr Float32 kOccluderZ         = 3.0f;
inline constexpr Float32 kOccluderHalfThick = 0.03f;
inline constexpr Float32 kOccluderCenterY   = -0.8f;
inline constexpr Float32 kOccluderHalfX     = 0.9f;
inline constexpr Float32 kOccluderHalfY     = 0.2f;

/// 主灯的内外锥角 (度)
///
/// 内锥必须把整个测量区都罩住 —— 落在内外锥之间的部分有角度衰减, 亮度
/// 是渐变的, 而"亮/暗"的判据就没有明确的分界了。
inline constexpr Float32 kSpotInnerDeg = 30.0f;
inline constexpr Float32 kSpotOuterDeg = 34.0f;

inline constexpr Float32 kLightRange     = 30.0f;
inline constexpr Float32 kLightIntensity = 6.0f;

/// 第二盏灯 —— 只为占住图集的第二块
///
/// 它的影子不参与解析比对, 但它的存在是必需的: 只有一块的话, "把块偏移
/// 算错" 这类缺陷会退化成"偏移到了一块空白区域", 而空白区域深度是 1.0,
/// 恰好判为无遮挡 —— 与"这盏灯没有影子"长得一样。两块之后, 算错的偏移
/// 会采到**另一盏灯的深度图**, 那是明显不同的结果。
inline constexpr Float32 kLight2X        = -5.5f;
inline constexpr Float32 kLight2InnerDeg = 15.0f;
inline constexpr Float32 kLight2OuterDeg = 18.0f;

inline constexpr Float32 kOccluder2CenterY = 0.5f;
inline constexpr Float32 kOccluder2HalfX   = 0.3f;
inline constexpr Float32 kOccluder2HalfY   = 0.2f;

// ── 点光源模式 ──
//
// 灯离墙很近 (z=2), 于是墙上离灯轴超过 2 单位的地方, "从灯指向片段"的主轴
// 就从 -Z 翻到 ±X 或 ±Y —— 影子会**横跨立方体的面边界**。
//
// 那正是这一组要验的东西: 面选错的表现是影子在某条线上突然断开或错位, 而
// 那条线的位置取决于灯到墙的距离, 不是任何一个显眼的地方。灯放远一点 (比如
// 和聚光灯一样 z=6) 的话, 可见范围内根本不会跨面, 整条选面逻辑就无从判定。

/// 点光源到墙的距离
inline constexpr Float32 kPointLightZ = 2.0f;

/// 点光源的遮挡板
inline constexpr Float32 kPointOccluderZ       = 1.4f;
inline constexpr Float32 kPointOccluderCenterX = 0.85f;
inline constexpr Float32 kPointOccluderHalfX   = 0.35f;
inline constexpr Float32 kPointOccluderHalfY   = 0.2f;

inline constexpr Float32 kPointLightRange = 30.0f;

/// 点光源模式下的相似三角形放大率
LIMX_NODISCARD inline Float32 PointShadowScale(Float32 occluderZ)
{
    return kPointLightZ / (kPointLightZ - occluderZ);
}

/// 点光源模式下遮挡物在画面上的放大率
LIMX_NODISCARD inline Float32 PointImageScale(Float32 occluderZ)
{
    return kCameraZ / (kCameraZ - occluderZ);
}

/// 相似三角形: 灯在 (0,0,h), 遮挡物边缘在 z=ze, 接收面在 z=0
///
/// 边缘坐标 e 投到墙上是 e * h / (h - ze)。薄板有厚度, 靠灯的那一面放大
/// 得多、背光的那一面放大得少 —— 影子的外缘由靠灯的那一面决定, 内缘由
/// 背光的那一面决定。差别只有 2%, 但判据的容差本来也就是几个百分点。
LIMX_NODISCARD inline Float32 ShadowScale(Float32 occluderZ)
{
    return kLightZ / (kLightZ - occluderZ);
}

/// 内锥在墙上照亮的半径
///
/// 扫描线不能超出这个圈: 内外锥之间是角度衰减区, 亮度渐变, 而"亮/暗"的
/// 阈值在渐变区里没有明确的分界。扫描线的端点落进衰减区的话, 端点本身
/// 就是暗的, 判据会以为影子一直延伸到了画面边缘。
LIMX_NODISCARD inline Float32 InnerConeRadiusAtWall()
{
    return kLightZ * FMath::Tan(FMath::DegreesToRadians(kSpotInnerDeg));
}

/// 遮挡物在画面上的放大率 (相机把它投到墙平面上看起来的位置)
LIMX_NODISCARD inline Float32 ImageScale(Float32 occluderZ)
{
    return kCameraZ / (kCameraZ - occluderZ);
}

/// 靠灯那一面的放大率 —— 影子的外缘
LIMX_NODISCARD inline Float32 ShadowScaleFront()
{
    return ShadowScale(kOccluderZ + kOccluderHalfThick);
}

/// 背光那一面的放大率 —— 影子的内缘
LIMX_NODISCARD inline Float32 ShadowScaleBack()
{
    return ShadowScale(kOccluderZ - kOccluderHalfThick);
}

} // namespace ShadowScene

namespace
{

/// 回读一张 RGBA16_SFLOAT 纹理, 返回逐像素亮度
///
/// 泛光自检要读两张 (泛光链首级与合成结果), 而两次读的动作完全一样 ——
/// 复制一遍的话总会有一份忘了转布局, 而那是验证层错误而不是数值错误,
/// 排查方向完全不同。
static bool ReadTextureLuminance(FRenderContext*   context,
                                 FRenderer&        renderer,
                                 FRHITextureHandle texture,
                                 FRHIExtent2D      extent,
                                 TArray<Float32>&  outLuminance)
{
    IRHIDevice* const device = context->GetDevice();

    if (device == nullptr || !texture.IsValid())
    {
        return false;
    }

    const SizeType pixelCount =
        static_cast<SizeType>(extent.Width) * extent.Height;

    FRHIBufferHandle readback;

    FRHIBufferDesc desc = {};
    desc.Size        = pixelCount * 8u;   // RGBA16_SFLOAT
    desc.Usage       = EBufferUsage::TransferDst;
    desc.MemoryUsage = EMemoryUsage::GpuToCpu;
    desc.DebugName   = "BloomCheck.Readback";

    if (!IsRHISuccess(device->CreateBuffer(desc, readback)))
    {
        return false;
    }

    bool recorded = false;

    renderer.SetPostSceneRenderCallback(
        [&recorded, context, texture, readback, extent]()
        {
            IRHICommandBuffer* cmd = context->GetCurrentCommandBuffer();

            if (cmd == nullptr)
            {
                return;
            }

            cmd->TransitionImageLayout(
                texture,
                EImageLayout::ShaderReadOnly,
                EImageLayout::TransferSrc,
                EPipelineStageFlags::FragmentShader,
                EPipelineStageFlags::Transfer,
                EAccessFlags::ShaderRead,
                EAccessFlags::TransferRead);

            FRHIBufferTextureCopyRegion region = {};
            region.BufferOffset      = 0;
            region.BufferRowLength   = 0;
            region.BufferImageHeight = 0;
            region.MipLevel          = 0;
            region.BaseLayer         = 0;
            region.LayerCount        = 1;
            region.TextureOffset     = { 0, 0, 0 };
            region.TextureExtent     = { extent.Width, extent.Height, 1 };

            cmd->CopyTextureToBuffer(texture, EImageLayout::TransferSrc,
                                     readback, region);

            cmd->TransitionImageLayout(
                texture,
                EImageLayout::TransferSrc,
                EImageLayout::ShaderReadOnly,
                EPipelineStageFlags::Transfer,
                EPipelineStageFlags::FragmentShader,
                EAccessFlags::TransferRead,
                EAccessFlags::ShaderRead);

            recorded = true;
        });

    renderer.RenderFrame();

    renderer.SetPostSceneRenderCallback(TFunction<void()>());

    if (!recorded)
    {
        device->DestroyBuffer(readback);
        return false;
    }

    device->WaitIdle();

    void* mapped = nullptr;

    if (!IsRHISuccess(device->MapBuffer(readback, &mapped)) ||
        mapped == nullptr)
    {
        device->DestroyBuffer(readback);
        return false;
    }

    const Float16Bits* src = static_cast<const Float16Bits*>(mapped);

    outLuminance.Clear();
    outLuminance.Reserve(pixelCount);

    for (SizeType i = 0; i < pixelCount; ++i)
    {
        const Float32 r = Float16ToFloat32(src[i * 4 + 0]);
        const Float32 g = Float16ToFloat32(src[i * 4 + 1]);
        const Float32 b = Float16ToFloat32(src[i * 4 + 2]);

        outLuminance.Add(0.2126f * r + 0.7152f * g + 0.0722f * b);
    }

    device->UnmapBuffer(readback);
    device->DestroyBuffer(readback);

    return true;
}

/// 求和
static Float64 SumOf(const TArray<Float32>& values)
{
    Float64 total = 0.0;

    for (SizeType i = 0; i < values.GetSize(); ++i)
    {
        total += static_cast<Float64>(values[i]);
    }

    return total;
}

} // namespace

// ============================================================================
// RunBloomChecks — 泛光的点扩散函数
//
// 一个孤立的亮点经过降采样-升采样链之后应当得到**径向对称、单调衰减**的
// 光晕。那就是这条链的点扩散函数 (PSF), 而它的性质完全由核决定, 与场景无关。
//
// 为什么非要量它: 降采样/升采样链最典型的缺陷是**半纹素偏移**。核的采样坐标
// 算错半个纹素, 每一级都把图像往同一个方向挪一点点, 六级累积下来光晕整体
// 偏离光源好几个像素。而画面上那仍然是"一团发光的东西" —— 没人看得出来它
// 偏了, 除非拿对称性去量。
//
// 三条判据:
//   1. **对称性** — 沿四个轴向, 距中心等距的两点亮度必须接近。这一条直接
//      抓半纹素偏移。
//   2. **单调衰减** — 亮度随距离单调下降。不单调说明某一级的核不是低通的。
//   3. **扩散范围** — 落在光源之外的能量占比。不模糊的实现几乎全部能量都
//      留在光源那几个像素里。
//   4. **多尺度累加** — sum(mip0)/(4*sum(mip1))。6 级链的理论值是 1.2, 与
//      半径、阈值这些可调参数无关。升采样若改成覆盖写, 它掉到 1.0。
//   5. **合成能量** — 合成增量必须等于 强度 x sum(泛光) x 4。一个算出了完美
//      PSF 却根本没合成到画面上的实现, 前四条全部通过。
//
// ── 已知没能覆盖的一种缺陷 ──
//
// 升采样的帐篷偏移若取**源**那一级的纹素尺寸而非目标那一级 (两者差一倍),
// 泛光会比设定的半径糊一倍。实测核外能量占比 0.391 对基线 0.3435 —— 只差
// 14%, 而这个量本身又随 m_FilterRadius 这个可调参数移动。要卡到能抓住它的
// 窄带, 就会在别人正常调半径时误报。
//
// 那是一个 CPU 侧的下标算术错误, 更适合在那一层验; 这里如实记下, 而不是
// 用一个会误报的阈值假装覆盖了它。
// ============================================================================
// ============================================================================
// RunShadowChecks — 阴影边界的解析位置
//
// 判据全部落在**世界坐标**上, 而不是像素上。做法是沿一条世界空间的直线
// 采样: 每一步把世界点用相机自己的 view/proj 投到屏幕, 读那一个像素。
//
// 这样做的收益是不必反推"像素 → 世界"那条映射 —— 反推要重写一遍相机的
// 约定 (右手系、Y 翻转、NDC 的 z 范围), 而重写的那一份与相机真正用的那份
// 之间没有任何东西保证一致。用同一个矩阵正向投, 这个可能性就不存在。
//
// 判据:
//   1. 影子的左右边界落在 ±halfX·k 上 —— k 是相似三角形的放大率。
//   2. 影子的上下边界落在 [yMin·k, yMax·k] 上, 而这两个值**不等**。
//      板子刻意偏离灯轴放, 上下对称的话贴图翻转就看不出来。
//   3. 第二盏灯的照亮区里必须也有暗块 —— 只有一块的话, "块偏移算错"会
//      退化成"偏到空白区", 而空白区深度 1.0 恰好判为无遮挡, 与"没有影子"
//      长得一样。
// ============================================================================

namespace
{

/// 沿世界空间的一条线扫出来的一段暗区
struct FShadowSpan
{
    bool    Found = false;
    Float32 Enter = 0.0f;   // 由亮转暗的世界坐标
    Float32 Exit  = 0.0f;   // 由暗转亮的世界坐标
    Float32 LitLevel    = 0.0f;
    Float32 ShadowLevel = 0.0f;
};

/// 把世界点投到像素并取亮度; 越界返回 -1
static Float32 SampleWorldPoint(const TArray<UInt8>& pixels,
                                UInt32 width, UInt32 height,
                                const FMatrix& viewProj,
                                const FVector3& worldPos)
{
    const FVector4 clip = viewProj.TransformVector4(
        FVector4(worldPos.X, worldPos.Y, worldPos.Z, 1.0f));

    if (clip.W <= 1.0e-6f)
    {
        return -1.0f;
    }

    const Float32 ndcX = clip.X / clip.W;
    const Float32 ndcY = clip.Y / clip.W;

    const Float32 fx = (ndcX * 0.5f + 0.5f) * static_cast<Float32>(width);
    const Float32 fy = (ndcY * 0.5f + 0.5f) * static_cast<Float32>(height);

    const Int32 px = static_cast<Int32>(fx);
    const Int32 py = static_cast<Int32>(fy);

    if (px < 0 || py < 0 ||
        px >= static_cast<Int32>(width) || py >= static_cast<Int32>(height))
    {
        return -1.0f;
    }

    const SizeType offset =
        (static_cast<SizeType>(py) * width + static_cast<SizeType>(px)) * 3u;

    if (offset + 2u >= pixels.GetSize())
    {
        return -1.0f;
    }

    return 0.2126f * static_cast<Float32>(pixels[offset + 0]) +
           0.7152f * static_cast<Float32>(pixels[offset + 1]) +
           0.0722f * static_cast<Float32>(pixels[offset + 2]);
}

/// 沿 axis 方向 (0=x, 1=y) 从 from 扫到 to, 找出中间那段暗区
///
/// 阈值取该条线上最亮与最暗的中点 —— 固定阈值不行: 曝光与色调映射会整体
/// 缩放亮度, 而那与阴影的对错无关。
static FShadowSpan FindShadowSpan(const TArray<UInt8>& pixels,
                                  UInt32 width, UInt32 height,
                                  const FMatrix& viewProj,
                                  Int32 axis, Float32 fixedCoord,
                                  Float32 from, Float32 to, Float32 step,
                                  Float32 thresholdFraction = 0.5f)
{
    FShadowSpan span;

    TArray<Float32> samples;
    TArray<Float32> coords;

    for (Float32 t = from; t <= to; t += step)
    {
        const FVector3 world =
            (axis == 0) ? FVector3(t, fixedCoord, ShadowScene::kWallZ)
                        : FVector3(fixedCoord, t, ShadowScene::kWallZ);

        const Float32 value =
            SampleWorldPoint(pixels, width, height, viewProj, world);

        if (value < 0.0f)
        {
            continue;
        }

        samples.Add(value);
        coords.Add(t);
    }

    if (samples.GetSize() < 8)
    {
        return span;
    }

    Float32 minValue = samples[0];
    Float32 maxValue = samples[0];

    for (SizeType i = 1; i < samples.GetSize(); ++i)
    {
        minValue = FMath::Min(minValue, samples[i]);
        maxValue = FMath::Max(maxValue, samples[i]);
    }

    span.LitLevel    = maxValue;
    span.ShadowLevel = minValue;

    // 对比度太低就不判定 —— 强行找"中点"会在噪声里找出一段假的暗区,
    // 而那是个通过。宁可报失败: 没有影子本来就该失败。
    if (maxValue - minValue < 20.0f)
    {
        return span;
    }

    // 阈值取"从最暗往最亮走多少比例"。
    //
    // 0.5 (中点) 对聚光灯那一组是对的: 测量区很小, 亮处的亮度基本一致。
    //
    // 点光源那一组不行 —— 灯离墙只有 2 单位, 而测量区横跨 5 单位, 于是
    // N·L 从 1 掉到 0.37, 亮处最暗的地方比中点还暗, 会被判成影子。那时
    // "两端都必须是亮的"这一条直接不成立, 判据报的却是"没找到完整的暗区"。
    //
    // 0.2 对点光源是有物理含义的: 影子区**只有环境光**, 而亮区哪怕衰减到
    // 三分之一也仍然带着直接光。阈值卡在"离环境光两成"的位置, 分的正是
    // "有没有直接光"这件事, 与 N·L 掉多少无关。
    const Float32 threshold =
        minValue + (maxValue - minValue) * thresholdFraction;

    // 两端都必须是亮的 —— 扫描线要完整跨过影子。有一端就在影子里的话,
    // 量到的"边界"只是扫描范围的端点, 而那与影子的位置无关。
    if (samples[0] < threshold ||
        samples[samples.GetSize() - 1] < threshold)
    {
        return span;
    }

    SizeType enterIndex = 0;
    SizeType exitIndex  = 0;

    for (SizeType i = 1; i < samples.GetSize(); ++i)
    {
        if (enterIndex == 0 && samples[i] < threshold)
        {
            enterIndex = i;
        }

        if (enterIndex != 0 && samples[i] >= threshold)
        {
            exitIndex = i;
            break;
        }
    }

    if (enterIndex == 0 || exitIndex == 0)
    {
        return span;
    }

    // 亚采样步长的线性插值 —— 边界通常落在两次采样之间。不插的话量出来
    // 的边界会系统性地偏半步, 而半步就是判据容差的一大块。
    const auto Interpolate = [&](SizeType hi) -> Float32
    {
        const Float32 a = samples[hi - 1];
        const Float32 b = samples[hi];

        if (FMath::Abs(a - b) < 1.0e-4f)
        {
            return coords[hi];
        }

        const Float32 t = (threshold - a) / (b - a);

        return coords[hi - 1] + t * (coords[hi] - coords[hi - 1]);
    };

    span.Found = true;
    span.Enter = Interpolate(enterIndex);
    span.Exit  = Interpolate(exitIndex);

    return span;
}

/// 墙面上一块矩形区域的平均亮度 (世界坐标)
static Float32 MeanLuminanceInRect(const TArray<UInt8>& pixels,
                                   UInt32 width, UInt32 height,
                                   const FMatrix& viewProj,
                                   Float32 xMin, Float32 xMax,
                                   Float32 yMin, Float32 yMax)
{
    Float64 total = 0.0;
    UInt32  count = 0;

    constexpr Int32 kSteps = 16;

    for (Int32 iy = 0; iy <= kSteps; ++iy)
    {
        const Float32 y = yMin + (yMax - yMin) *
                          (static_cast<Float32>(iy) /
                           static_cast<Float32>(kSteps));

        for (Int32 ix = 0; ix <= kSteps; ++ix)
        {
            const Float32 x = xMin + (xMax - xMin) *
                              (static_cast<Float32>(ix) /
                               static_cast<Float32>(kSteps));

            const Float32 value = SampleWorldPoint(
                pixels, width, height, viewProj,
                FVector3(x, y, ShadowScene::kWallZ));

            if (value >= 0.0f)
            {
                total += static_cast<Float64>(value);
                ++count;
            }
        }
    }

    if (count == 0)
    {
        return -1.0f;
    }

    return static_cast<Float32>(total / static_cast<Float64>(count));
}

} // namespace

// ============================================================================
// RunPointShadowChecks — 点光源立方体阴影
//
// 两条判据, 抓的是完全不同的失效方式:
//
//   1. **影子边界落在相似三角形算出的位置上。** 与聚光灯那一组同一个公式,
//      验的是六个面的矩阵、块的连续分配、以及采样的整条链路。
//
//   2. **影子横跨立方体的面边界时不能断。** 灯离墙只有 2 单位, 所以墙上
//      |x| 超过 2 的地方, "从灯指向片段"的主轴就从 -Z 翻到 ±X —— 那一线
//      两侧取的是**不同的两块**。选面的规则若与 C++ 侧算矩阵时的编号不一致,
//      影子会在那条线上突然断开或错位。
//
//      这一条没法靠边界位置发现: 断开处离两条边界都很远, 而边界本身仍然
//      正确。必须专门去量那一段的连续性。
//
// 灯放在相机轴上, 理由与聚光灯那一组相同 —— 遮挡物在画面上的像一定落在它
// 自己影子之外, 边界才不会被自己挡住。
// ============================================================================
static bool RunPointShadowChecks(FRenderContext* context, FRenderer& renderer)
{
    FScreenshotCapture shot;

    if (!shot.Request(context))
    {
        LIMX_LOG(LogLaunch, Error, "[点光阴影] 回读缓冲区准备失败");
        return false;
    }

    renderer.SetPostSceneRenderCallback(
        [&shot, context]() { shot.RecordCopy(context); });

    renderer.RenderFrame();

    renderer.SetPostSceneRenderCallback(TFunction<void()>());

    TArray<UInt8> pixels;

    if (!shot.ReadPixels(context, pixels))
    {
        shot.Release(context->GetDevice());
        LIMX_LOG(LogLaunch, Error, "[点光阴影] 画面回读失败");
        return false;
    }

    shot.Release(context->GetDevice());

    const FRHIExtent2D extent = context->GetSwapchainExtent();

    const FMatrix viewProj = renderer.GetCamera().GetProjectionMatrix() *
                             renderer.GetCamera().GetViewMatrix();

    // ---- 解析值 ----
    const Float32 scaleFront =
        ShadowScene::PointShadowScale(ShadowScene::kPointOccluderZ +
                                      ShadowScene::kOccluderHalfThick);
    const Float32 scaleBack =
        ShadowScene::PointShadowScale(ShadowScene::kPointOccluderZ -
                                      ShadowScene::kOccluderHalfThick);

    const Float32 occluderXMin =
        ShadowScene::kPointOccluderCenterX - ShadowScene::kPointOccluderHalfX;
    const Float32 occluderXMax =
        ShadowScene::kPointOccluderCenterX + ShadowScene::kPointOccluderHalfX;

    // 板子整个在灯轴的 +x 一侧: 内缘用背光面 (放大得少), 外缘用靠灯面
    const Float32 expectedXMin = occluderXMin * scaleBack;
    const Float32 expectedXMax = occluderXMax * scaleFront;

    const Float32 imageFront =
        ShadowScene::PointImageScale(ShadowScene::kPointOccluderZ +
                                     ShadowScene::kOccluderHalfThick);

    const Float32 imageYMax = ShadowScene::kPointOccluderHalfY * imageFront;
    const Float32 shadowYMax = ShadowScene::kPointOccluderHalfY * scaleFront;

    // 横扫的 y: 落在影子的上下缘之内, 又要避开板子自己在画面上的像
    const Float32 scanY = -(imageYMax + shadowYMax) * 0.5f;

    LIMX_LOG(LogLaunch, Display,
             "[点光阴影] 解析值 — 影子 x [{}, {}] y ±{}; 板子的像 y ±{}",
             expectedXMin, expectedXMax, shadowYMax, imageYMax);

    LIMX_LOG(LogLaunch, Display,
             "[点光阴影] 横扫 y={}, 面边界在 x=±{}",
             scanY, ShadowScene::kPointLightZ);

    bool passed = true;

    // ---- 1. 左右边界 ----
    // 扫描范围收在 [-1.2, 5.0]。
    //
    // 再往外 N·L 掉到 0.3 以下, 亮区与环境光已经分不清了 —— 那不是判据的
    // 问题, 是那里本来就没有多少直接光。范围写死而不是取整个视野, 是因为
    // "判据在哪一段有效"必须是明确的。
    const FShadowSpan span = FindShadowSpan(
        pixels, extent.Width, extent.Height, viewProj,
        0, scanY, -1.2f, 5.0f, 0.004f, 0.2f);

    if (!span.Found)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[点光阴影] 横扫没有找到完整的暗区 (最亮 {}, 最暗 {}) —— "
                 "六块是不是没分出来?",
                 span.LitLevel, span.ShadowLevel);
        passed = false;
    }
    else
    {
        const Float32 errMin = FMath::Abs(span.Enter - expectedXMin);
        const Float32 errMax = FMath::Abs(span.Exit - expectedXMax);

        // 容差按影子的宽度取。
        //
        // 实测误差: 内缘 0.021, 外缘 0.103。外缘那 0.103 里约 0.083 有确切
        // 来源 —— 法线偏移把接收点推离墙面约 0.039 单位, 而这一组的相似三角
        // 形放大率高达 3.5, 于是外缘内移 0.083。放大率高是刻意的 (灯离墙只有
        // 2 单位, 影子才跨得过面边界), 代价就是偏移被放大得更明显。
        //
        // 上限由"必须抓住什么"定: 面边界断缝那个缺陷量出来的误差是 2.25,
        // 法线偏移大十倍约 1.0。6% = 0.157 卡在 0.103 与 1.0 之间。
        const Float32 tolerance =
            FMath::Abs(expectedXMax - expectedXMin) * 0.06f;

        LIMX_LOG(LogLaunch, Display,
                 "[点光阴影] 左右边界 — 实测 [{}, {}] 解析 [{}, {}] "
                 "误差 [{}, {}] 容差 {}",
                 span.Enter, span.Exit, expectedXMin, expectedXMax,
                 errMin, errMax, tolerance);

        if (errMin > tolerance || errMax > tolerance)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[点光阴影] 边界偏离解析值超过容差");
            passed = false;
        }
    }

    // ---- 2. 跨面连续性 ----
    //
    // 面边界在 x = ±kPointLightZ。取边界两侧各一段仍在影子里的区间, 逐点
    // 采样, 断言全都是暗的。
    //
    // 选面错了的话, 边界一侧会去取另一块 —— 那一块画的是别的方向上的几何,
    // 于是那一侧要么没有影子 (亮), 要么影子的形状完全不同。
    const Float32 boundary = ShadowScene::kPointLightZ;

    if (span.Found && expectedXMin < boundary && boundary < expectedXMax)
    {
        const Float32 from = FMath::Max(expectedXMin + 0.15f,
                                        boundary - 0.8f);
        const Float32 to   = FMath::Min(expectedXMax - 0.15f,
                                        boundary + 0.8f);

        // 与上面同一个阈值 —— 两处用不同的阈值的话, "边界在这里"与
        // "这一段是暗的"可能互相矛盾。
        const Float32 threshold =
            span.ShadowLevel + (span.LitLevel - span.ShadowLevel) * 0.2f;

        UInt32 total = 0;
        UInt32 dark  = 0;

        for (Float32 x = from; x <= to; x += 0.004f)
        {
            const Float32 value = SampleWorldPoint(
                pixels, extent.Width, extent.Height, viewProj,
                FVector3(x, scanY, ShadowScene::kWallZ));

            if (value < 0.0f)
            {
                continue;
            }

            ++total;

            if (value < threshold)
            {
                ++dark;
            }
        }

        LIMX_LOG(LogLaunch, Display,
                 "[点光阴影] 跨面区间 x [{}, {}] — {} / {} 个采样点是暗的",
                 from, to, dark, total);

        // 采样数本身要断言: 区间算错时循环可能一次都不进, 而"零个亮点"
        // 同样成立 —— 那是个通过。
        if (total < 100u)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[点光阴影] 跨面区间只取到 {} 个采样点 — 判定无效",
                     total);
            passed = false;
        }
        else if (dark * 100u < total * 98u)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[点光阴影] 跨面区间里有 {} / {} 个点是亮的 —— "
                     "面边界两侧取到了不同的块?",
                     total - dark, total);
            passed = false;
        }
    }
    else if (span.Found)
    {
        // 影子没跨过面边界的话, 第二条判据什么也没验。那不是通过。
        LIMX_LOG(LogLaunch, Error,
                 "[点光阴影] 影子 [{}, {}] 没有跨过面边界 {} —— "
                 "选面这一条无从判定",
                 expectedXMin, expectedXMax, boundary);
        passed = false;
    }

    // ---- 3. 图集必须画了六块 ----
    FShadowAtlasPass* const atlas = renderer.GetShadowAtlasPass();

    const UInt32 tileCount =
        (atlas != nullptr) ? atlas->GetRenderedTileCount() : 0u;

    LIMX_LOG(LogLaunch, Display, "[点光阴影] 图集本帧绘制 {} 块", tileCount);

    if (tileCount != kCubeFaceCount)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[点光阴影] 图集应当绘制 {} 块 (立方体六个面), 实际 {}",
                 kCubeFaceCount, tileCount);
        passed = false;
    }

    if (passed)
    {
        LIMX_LOG(LogLaunch, Display,
                 "[点光阴影] 通过 — 边界落在解析位置, 且跨过面边界不断");
    }
    else
    {
        LIMX_LOG(LogLaunch, Error, "[点光阴影] 失败");
    }

    return passed;
}

static bool RunShadowChecks(FRenderContext* context, FRenderer& renderer,
                            UInt32 expectedTiles)
{
    // ---- 抓一帧 ----
    FScreenshotCapture shot;

    if (!shot.Request(context))
    {
        LIMX_LOG(LogLaunch, Error, "[阴影] 回读缓冲区准备失败");
        return false;
    }

    renderer.SetPostSceneRenderCallback(
        [&shot, context]() { shot.RecordCopy(context); });

    renderer.RenderFrame();

    // 立刻摘掉回调 —— shot 是栈上的, 留着的话后面任何一次 RenderFrame
    // 都会去访问一块已经出作用域的内存。
    renderer.SetPostSceneRenderCallback(TFunction<void()>());

    TArray<UInt8> pixels;

    if (!shot.ReadPixels(context, pixels))
    {
        shot.Release(context->GetDevice());
        LIMX_LOG(LogLaunch, Error, "[阴影] 画面回读失败");
        return false;
    }

    shot.Release(context->GetDevice());

    const FRHIExtent2D extent = context->GetSwapchainExtent();

    // 用相机**自己**的矩阵正向投影, 不反推像素到世界的映射
    const FMatrix viewProj = renderer.GetCamera().GetProjectionMatrix() *
                             renderer.GetCamera().GetViewMatrix();

    // ---- 解析值 ----
    const Float32 scaleFront = ShadowScene::ShadowScaleFront();
    const Float32 scaleBack  = ShadowScene::ShadowScaleBack();

    const Float32 expectedXMax =  ShadowScene::kOccluderHalfX * scaleFront;
    const Float32 expectedXMin = -expectedXMax;

    const Float32 occluderYMin =
        ShadowScene::kOccluderCenterY - ShadowScene::kOccluderHalfY;
    const Float32 occluderYMax =
        ShadowScene::kOccluderCenterY + ShadowScene::kOccluderHalfY;

    // 板子整个在灯轴下方, 所以下缘用靠灯那一面 (放大得多),
    // 上缘用背光那一面 (放大得少)
    const Float32 expectedYMin = occluderYMin * scaleFront;
    const Float32 expectedYMax = occluderYMax * scaleBack;

    // 扫描线的位置 —— 必须落在影子里, 又要避开板子自己在画面上的像
    const Float32 imageFront = ShadowScene::ImageScale(
        ShadowScene::kOccluderZ + ShadowScene::kOccluderHalfThick);

    const Float32 imageXMax = ShadowScene::kOccluderHalfX * imageFront;
    const Float32 imageYMin = occluderYMin * imageFront;

    // 横扫: y 取影子下缘与板子像的下缘之间
    const Float32 scanY = (expectedYMin + imageYMin) * 0.5f;

    // 竖扫: x 取影子外缘与板子像的外缘之间
    const Float32 scanX = (expectedXMax + imageXMax) * 0.5f;

    LIMX_LOG(LogLaunch, Display,
             "[阴影] 解析值 — 影子 x [{}, {}] y [{}, {}]; "
             "板子的像 x 外缘 {} y 下缘 {}",
             expectedXMin, expectedXMax, expectedYMin, expectedYMax,
             imageXMax, imageYMin);

    // 扫描范围由内锥半径反推, 不写死。
    //
    // 写死的话, 改了锥角而忘了改这里, 扫描线的端点会落进角度衰减区 ——
    // 端点本身是暗的, 判据于是认为"影子一直延伸到扫描范围之外", 报的
    // 却是"没有找到完整的暗区"。那条错误信息指向的方向完全不对。
    //
    // 0.9 是留给锥边缘的余量: 恰好压在内锥边界上时, 角度衰减的过渡带
    // 已经开始了。
    const Float32 usableRadius = ShadowScene::InnerConeRadiusAtWall() * 0.9f;

    const Float32 scanXLimit =
        FMath::Sqrt(FMath::Max(usableRadius * usableRadius - scanY * scanY,
                               0.01f));
    const Float32 scanYLimit =
        FMath::Sqrt(FMath::Max(usableRadius * usableRadius - scanX * scanX,
                               0.01f));

    LIMX_LOG(LogLaunch, Display,
             "[阴影] 扫描线 — 横扫 y={} 范围 ±{}, 竖扫 x={} 范围 ±{} "
             "(内锥在墙上的半径 {})",
             scanY, scanXLimit, scanX, scanYLimit,
             ShadowScene::InnerConeRadiusAtWall());

    bool passed = true;

    // ---- 1. 左右边界 ----
    const FShadowSpan horizontal = FindShadowSpan(
        pixels, extent.Width, extent.Height, viewProj,
        0, scanY, -scanXLimit, scanXLimit, 0.004f);

    if (!horizontal.Found)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[阴影] 横扫没有找到完整的暗区 (最亮 {}, 最暗 {}) —— "
                 "影子根本没画, 还是对比度不够?",
                 horizontal.LitLevel, horizontal.ShadowLevel);
        passed = false;
    }
    else
    {
        const Float32 errMin = FMath::Abs(horizontal.Enter - expectedXMin);
        const Float32 errMax = FMath::Abs(horizontal.Exit - expectedXMax);

        // 容差是量出来的, 不是猜的。
        //
        // 实测误差 0.013~0.014 (0.8%), 而那 0.8% 有确切的来源: 着色器把
        // 接收点朝光源挪了一个深度偏移 (0.03) 加半个法线偏移 (0.025),
        // 相似三角形因此缩了 (6-0.055)/6 = 0.9%。
        //
        // 上限则由"必须抓住什么"定: 深度偏移大十倍 (0.3) 会让边界移动
        // 0.098 —— 那是肉眼可见的阴影脱离物体 (peter-panning), 必须报错。
        // 4% = 0.073 落在 0.014 与 0.098 之间, 两边各留三到五倍。
        const Float32 tolerance = FMath::Abs(expectedXMax) * 0.04f;

        LIMX_LOG(LogLaunch, Display,
                 "[阴影] 左右边界 — 实测 [{}, {}] 解析 [{}, {}] "
                 "误差 [{}, {}] 容差 {}",
                 horizontal.Enter, horizontal.Exit,
                 expectedXMin, expectedXMax, errMin, errMax, tolerance);

        if (errMin > tolerance || errMax > tolerance)
        {
            LIMX_LOG(LogLaunch, Error, "[阴影] 左右边界偏离解析值超过容差");
            passed = false;
        }
    }

    // ---- 2. 上下边界 ----
    //
    // 上下两条边到灯轴的距离**不等** (板子偏离灯轴放的)。这一条是查贴图
    // 上下翻转的唯一手段 —— 对称的影子翻转之后完全一样。
    const FShadowSpan vertical = FindShadowSpan(
        pixels, extent.Width, extent.Height, viewProj,
        1, scanX, -scanYLimit, scanYLimit, 0.004f);

    if (!vertical.Found)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[阴影] 竖扫没有找到完整的暗区 (最亮 {}, 最暗 {})",
                 vertical.LitLevel, vertical.ShadowLevel);
        passed = false;
    }
    else
    {
        const Float32 errMin = FMath::Abs(vertical.Enter - expectedYMin);
        const Float32 errMax = FMath::Abs(vertical.Exit - expectedYMax);

        // 同上: 实测 0.003~0.018, 而深度偏移大十倍时下缘会移动 0.109。
        // 8% × 0.832 = 0.067 卡在中间。
        const Float32 tolerance =
            FMath::Abs(expectedYMax - expectedYMin) * 0.08f;

        LIMX_LOG(LogLaunch, Display,
                 "[阴影] 上下边界 — 实测 [{}, {}] 解析 [{}, {}] "
                 "误差 [{}, {}] 容差 {}",
                 vertical.Enter, vertical.Exit,
                 expectedYMin, expectedYMax, errMin, errMax, tolerance);

        if (errMin > tolerance || errMax > tolerance)
        {
            LIMX_LOG(LogLaunch, Error, "[阴影] 上下边界偏离解析值超过容差");
            passed = false;
        }
    }

    // ---- 3. 第二块也要画 ----
    const Float32 shadow2XMin =
        ShadowScene::kLight2X - ShadowScene::kOccluder2HalfX * scaleFront;
    const Float32 shadow2XMax =
        ShadowScene::kLight2X + ShadowScene::kOccluder2HalfX * scaleFront;

    const Float32 shadow2YMin =
        (ShadowScene::kOccluder2CenterY - ShadowScene::kOccluder2HalfY) *
        scaleBack;
    const Float32 shadow2YMax =
        (ShadowScene::kOccluder2CenterY + ShadowScene::kOccluder2HalfY) *
        scaleFront;

    const Float32 secondShadow = MeanLuminanceInRect(
        pixels, extent.Width, extent.Height, viewProj,
        shadow2XMin + 0.1f, shadow2XMax - 0.1f,
        shadow2YMin + 0.1f, shadow2YMax - 0.1f);

    // 参照区取第二盏灯照亮区里、影子下方的一块
    const Float32 secondLit = MeanLuminanceInRect(
        pixels, extent.Width, extent.Height, viewProj,
        ShadowScene::kLight2X - 0.4f, ShadowScene::kLight2X + 0.4f,
        -1.2f, -0.6f);

    LIMX_LOG(LogLaunch, Display,
             "[阴影] 第二块 — 影子区平均 {} 照亮区平均 {} "
             "(影子 x [{}, {}] y [{}, {}])",
             secondShadow, secondLit,
             shadow2XMin, shadow2XMax, shadow2YMin, shadow2YMax);

    if (secondShadow < 0.0f || secondLit < 0.0f)
    {
        LIMX_LOG(LogLaunch, Error, "[阴影] 第二块的取样区落在画面之外");
        passed = false;
    }
    else if (secondShadow > secondLit * 0.6f)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[阴影] 第二盏灯的影子不够暗 —— 图集是不是只画了第一块?");
        passed = false;
    }

    // ---- 4. 图集确实画了两块 ----
    FShadowAtlasPass* const atlas = renderer.GetShadowAtlasPass();

    const UInt32 tileCount =
        (atlas != nullptr) ? atlas->GetRenderedTileCount() : 0u;

    LIMX_LOG(LogLaunch, Display,
             "[阴影] 图集本帧绘制 {} 块 (预期 {})", tileCount, expectedTiles);

    if (tileCount != expectedTiles)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[阴影] 图集应当绘制 {} 块, 实际 {} —— 块分配出了问题",
                 expectedTiles, tileCount);
        passed = false;
    }

    if (passed)
    {
        LIMX_LOG(LogLaunch, Display,
                 "[阴影] 通过 — 影子边界落在相似三角形算出的位置上");
    }
    else
    {
        LIMX_LOG(LogLaunch, Error, "[阴影] 失败");
    }

    return passed;
}

static bool RunBloomChecks(FRenderContext* context, FRenderer& renderer)
{
    FBloomPass* const bloom = renderer.GetBloomPass();

    if (bloom == nullptr || !bloom->IsEnabled())
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Bloom] 泛光未启用 — 自检无从判定 (加 --bloom)");
        return false;
    }

    const FRHIExtent2D extent = bloom->GetBloomExtent();

    // 读链的第 0 级 (半分辨率)。那是升采样把全部级别加回来之后的结果, 也就是
    // 完整的 PSF —— 合成之后的图里混着原始场景, 分不出哪部分是泛光。
    TArray<Float32> luminance;

    if (!ReadTextureLuminance(context, renderer, bloom->GetBloomTexture(),
                              extent, luminance))
    {
        LIMX_LOG(LogLaunch, Error, "[Bloom] 泛光缓冲回读失败");
        return false;
    }

    // ---- 亮度质心 ----
    //
    // 用质心而非"最亮的那个像素"定中心: 最亮点在方块内部是一片平顶, 具体
    // 落在哪个像素取决于浮点的最后一位。质心是稳定的, 而且它本身就是对称性
    // 的一个陈述 —— PSF 对称时质心必然在光源中心。
    Float64 sumWeight = 0.0;
    Float64 sumX      = 0.0;
    Float64 sumY      = 0.0;

    Float32 peak = 0.0f;

    for (UInt32 y = 0; y < extent.Height; ++y)
    {
        for (UInt32 x = 0; x < extent.Width; ++x)
        {
            const Float32 value =
                luminance[static_cast<SizeType>(y) * extent.Width + x];

            peak = FMath::Max(peak, value);

            sumWeight += static_cast<Float64>(value);
            sumX      += static_cast<Float64>(value) * x;
            sumY      += static_cast<Float64>(value) * y;
        }
    }

    if (sumWeight < 1.0e-3 || peak < 1.0e-3)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Bloom] 泛光缓冲几乎全黑 (总亮度 {}, 峰值 {}) —— "
                 "阈值是不是太高, 或者链根本没跑?",
                 sumWeight, peak);
        return false;
    }

    const Int32 centerX = static_cast<Int32>(sumX / sumWeight + 0.5);
    const Int32 centerY = static_cast<Int32>(sumY / sumWeight + 0.5);

    const auto Sample = [&luminance, extent](Int32 x, Int32 y) -> Float32
    {
        if (x < 0 || y < 0 ||
            x >= static_cast<Int32>(extent.Width) ||
            y >= static_cast<Int32>(extent.Height))
        {
            return -1.0f;
        }

        return luminance[static_cast<SizeType>(y) * extent.Width + x];
    };

    bool passed = true;

    // 剖面总是打印 —— 判定通过与否都要能看到形状。只在失败时打的话, 通过
    // 的那次就没有可对照的基线, 下一次数值漂移了也无从发现。
    for (Int32 d = 0; d <= 48; d += 8)
    {
        LIMX_LOG(LogLaunch, Display,
                 "[Bloom] 距 {}: -x {} +x {} | -y {} +y {}",
                 d,
                 Sample(centerX - d, centerY), Sample(centerX + d, centerY),
                 Sample(centerX, centerY - d), Sample(centerX, centerY + d));
    }

    // ---- 1. 光晕是否居中 ----
    //
    // 判据不是"距中心等距的两点亮度相等"。那样量出来的主要是**光源自己的
    // 硬边**: 方块边缘处亮度从 33 掉到 12, 半个像素的位置差就是 20% 的相对
    // 差异, 而那与泛光的核毫无关系。第一版就是这么写的, 于是它在一个完全
    // 正确的实现上报了 22.7% 的"不对称"。
    //
    // 改成比较两个质心:
    //   - **平台质心**: 亮度高于峰值 80% 的区域 —— 那就是光源本身的位置
    //   - **光晕质心**: 亮度在峰值 0.5% 到 30% 之间的区域 —— 那是扩散出去
    //     的部分
    //
    // 正确的核是对称的, 于是光晕必然以光源为中心, 两个质心重合。而降采样
    // 或升采样里任何半纹素的坐标偏差都会让每一级往同一个方向挪一点, 六级
    // 累积下来光晕整体偏离光源好几个像素 —— 质心之差直接把它量出来。
    //
    // 这个判据对"中心取整到哪个像素"完全不敏感, 而逐点比较对它很敏感。
    Float64 plateauWeight = 0.0;
    Float64 plateauX      = 0.0;
    Float64 plateauY      = 0.0;

    Float64 haloWeight = 0.0;
    Float64 haloX      = 0.0;
    Float64 haloY      = 0.0;

    for (UInt32 y = 0; y < extent.Height; ++y)
    {
        for (UInt32 x = 0; x < extent.Width; ++x)
        {
            const Float32 value =
                luminance[static_cast<SizeType>(y) * extent.Width + x];

            if (value > peak * 0.8f)
            {
                plateauWeight += static_cast<Float64>(value);
                plateauX      += static_cast<Float64>(value) * x;
                plateauY      += static_cast<Float64>(value) * y;
            }
            else if (value > peak * 0.005f && value < peak * 0.3f)
            {
                haloWeight += static_cast<Float64>(value);
                haloX      += static_cast<Float64>(value) * x;
                haloY      += static_cast<Float64>(value) * y;
            }
        }
    }

    if (plateauWeight < 1.0e-3 || haloWeight < 1.0e-3)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Bloom] 分不出光源与光晕 (平台权重 {}, 光晕权重 {}) —— "
                 "要么泛光没扩散, 要么整张图都是平的",
                 plateauWeight, haloWeight);
        return false;
    }

    const Float64 offsetX = haloX / haloWeight - plateauX / plateauWeight;
    const Float64 offsetY = haloY / haloWeight - plateauY / plateauWeight;

    const Float64 centroidOffset =
        FMath::Sqrt(offsetX * offsetX + offsetY * offsetY);

    // 1.5 像素。光源是个方块而非圆点, 透视又让它不完全对称, 所以两个质心
    // 不会严格重合。而每级半纹素的偏移累积到第 0 级是 6 个像素量级 ——
    // 两者差得很开。
    if (centroidOffset > 1.5)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Bloom] 光晕质心偏离光源 {} 像素 (dx {}, dy {}) —— "
                 "降采样或升采样的采样坐标有半纹素偏移",
                 centroidOffset, offsetX, offsetY);
        passed = false;
    }

    // ---- 2. 单调衰减 ----
    Float32  previous   = -1.0f;
    SizeType inversions = 0;

    for (Int32 d = 0; d <= 60; d += 4)
    {
        // 四个轴向取平均, 抹掉光源本身不是圆形带来的方向性
        Float32  sum   = 0.0f;
        SizeType count = 0;

        const Float32 taps[4] =
        {
            Sample(centerX - d, centerY), Sample(centerX + d, centerY),
            Sample(centerX, centerY - d), Sample(centerX, centerY + d),
        };

        for (UInt32 i = 0; i < 4; ++i)
        {
            if (taps[i] >= 0.0f)
            {
                sum += taps[i];
                ++count;
            }
        }

        if (count == 0)
        {
            continue;
        }

        const Float32 mean = sum / static_cast<Float32>(count);

        // 容差按峰值的百分之一给 —— 远处的半精度量化本身就有抖动
        if (previous >= 0.0f && mean > previous + peak * 0.01f)
        {
            ++inversions;
        }

        previous = mean;
    }

    if (inversions > 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Bloom] 亮度沿半径出现 {} 处回升 —— 某一级的核不是低通的",
                 inversions);
        passed = false;
    }

    // ---- 3. 扩散范围 ----
    //
    // 判据是**落在光源之外的能量占比**, 而不是"亮度降到峰值 10% 的半径"。
    //
    // 后者第一版用过, 但它量的是 PSF 的**核**而不是尾巴: 点光源的泛光峰值
    // 很高 (98) 而尾巴很低 (0.05), 10% 的门槛在离中心 5 像素处就跨过了 ——
    // 哪怕尾巴一直延伸到 48 像素之外。一个完全正确的实现会因此被判失败。
    //
    // 能量占比直接回答"泛光有没有把光散开": 不模糊的实现几乎全部能量都留在
    // 光源那几个像素里, 而正确的链会把三成以上推到 12 像素之外。
    constexpr Int32 kCoreRadius = 12;

    Float64 totalEnergy   = 0.0;
    Float64 outsideEnergy = 0.0;

    for (UInt32 y = 0; y < extent.Height; ++y)
    {
        for (UInt32 x = 0; x < extent.Width; ++x)
        {
            const Float32 value =
                luminance[static_cast<SizeType>(y) * extent.Width + x];

            const Int32 dx = static_cast<Int32>(x) - centerX;
            const Int32 dy = static_cast<Int32>(y) - centerY;

            totalEnergy += static_cast<Float64>(value);

            if (dx * dx + dy * dy > kCoreRadius * kCoreRadius)
            {
                outsideEnergy += static_cast<Float64>(value);
            }
        }
    }

    const Float64 outsideRatio =
        (totalEnergy > 1.0e-6) ? (outsideEnergy / totalEnergy) : 0.0;

    // ---- 多尺度累加的直接证据 ----
    //
    // 升采样把第 i+1 级**加回**第 i 级。于是
    //     sum(mip0) = sum(降采样的 mip0) + 4 * sum(mip1)
    // 明显大于 4*sum(mip1)。若升采样改成覆盖写, 两者相等 —— 而画面上两者
    // 都只是"一团光", 区别仅在于泛光失去了细尺度的层次。
    TArray<Float32> mip1;

    Float64 accumulationRatio = 0.0;

    if (ReadTextureLuminance(context, renderer, bloom->GetMipTexture(1),
                             bloom->GetMipExtent(1), mip1))
    {
        const Float64 mip1Sum = SumOf(mip1);

        accumulationRatio =
            (mip1Sum > 1.0e-6) ? (totalEnergy / (4.0 * mip1Sum)) : 0.0;
    }

    LIMX_LOG(LogLaunch, Display,
             "[Bloom] 诊断: 核外能量占比 {}, 累加比 sum(mip0)/(4*sum(mip1)) = {}",
             outsideRatio, accumulationRatio);

    // 6 级链的理论值是 1.2, 与级数有关而与半径、阈值等可调参数**无关**:
    //   降采样每级总能量减为 1/4, 升采样的帐篷核保持密度, 于是
    //   sum(mip_i) = (6-i)/4^i * S, 代入得 sum(mip0)/(4*sum(mip1)) = 6/5。
    // 实测基线 1.1994, 改成覆盖写是 0.9997 —— 两者差得很开, 而且不会因为
    // 有人调半径或阈值而漂移。
    if (accumulationRatio < 1.1)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Bloom] 累加比只有 {} (6 级链应为 1.2) —— "
                 "升采样没有把各级**加回**上一级, 泛光失去了细尺度的层次",
                 accumulationRatio);
        passed = false;
    }

    // 0.2: 实测正确实现是 0.3 以上, 而不模糊的实现几乎是 0。
    if (outsideRatio < 0.2)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Bloom] 只有 {} 的能量落在光源 {} 像素之外 —— "
                 "泛光链没有真的在扩散",
                 outsideRatio, kCoreRadius);
        passed = false;
    }

    // ---- 4. 合成是否真的把泛光加了上去 ----
    //
    // 前三条验的都是**泛光缓冲**本身。一个算出了完美 PSF 却根本没合成到画面
    // 上的实现, 前三条全部通过 —— 而画面上只是"没有泛光", 与关掉它没有区别。
    //
    // 判据: 合成结果 = 场景 + 强度 x 泛光。把强度设成 0 再渲一次拿到纯场景,
    // 两者之差应当等于 强度 x sum(泛光) x 4 —— 4 是因为泛光是半分辨率,
    // 每个泛光纹素在全分辨率上覆盖 4 个像素。
    const Float64 bloomSum = SumOf(luminance);

    const Float32 intensity = bloom->GetIntensity();

    TArray<Float32> composite;
    TArray<Float32> sceneOnly;

    const FRHIExtent2D fullExtent = context->GetSwapchainExtent();

    bool compositeOk =
        ReadTextureLuminance(context, renderer, bloom->GetOutputTexture(),
                             fullExtent, composite);

    bloom->SetIntensity(0.0f);

    compositeOk = compositeOk &&
        ReadTextureLuminance(context, renderer, bloom->GetOutputTexture(),
                             fullExtent, sceneOnly);

    bloom->SetIntensity(intensity);

    if (!compositeOk)
    {
        LIMX_LOG(LogLaunch, Error, "[Bloom] 合成结果回读失败");
        return false;
    }

    const Float64 delta = SumOf(composite) - SumOf(sceneOnly);

    const Float64 expected =
        static_cast<Float64>(intensity) * bloomSum * 4.0;

    if (expected < 1.0e-3)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Bloom] 预期的合成增量只有 {} —— 强度或泛光总量为零, "
                 "这条判据无从判定",
                 expected);
        passed = false;
    }
    else
    {
        const Float64 ratio = delta / expected;

        // 容差 ±8%。双线性放大不是严格的能量守恒 (边缘像素的权重与内部不同),
        // 而半精度在这个量级上还有量化误差。任何"没合成"或"强度没生效"的
        // 情况给出的比值是 0, "加了两次"是 2 —— 都远在容差之外。
        if (ratio < 0.92 || ratio > 1.08)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[Bloom] 合成增量 {} 与预期 {} 之比是 {} —— "
                     "泛光没有按强度加到画面上",
                     delta, expected, ratio);
            passed = false;
        }
        else if (passed)
        {
            LIMX_LOG(LogLaunch, Log,
                     "[Bloom] 点扩散: 中心 ({},{}), 峰值 {}, "
                     "光晕质心偏离 {} 像素, 核外能量占比 {}; "
                     "合成增量/预期 = {}",
                     centerX, centerY, peak, centroidOffset, outsideRatio,
                     ratio);
        }
    }

    return passed;
}

// ============================================================================
// RunAoChecks — GTAO 的解析判据
//
// 90 度凹角处, 余弦加权的可见度**解析值是 0.5** —— 半个半球被另一面墙挡住。
// 但那是"搜索半径 → ∞"的极限: 有限半径下只能看到墙的一段, 遮挡必然偏小。
// 实测 (夹角 0~0.25 个单位内的均值):
//
//     半径 0.8 → 0.679    半径 2 → 0.586    半径 8 → 0.557
//
// 所以判据不是"等于 0.5", 而是**随半径增大朝 0.5 单调收敛**。那是这个算法
// 的物理签名, 而一个写错的实现 (法线没转视空间、角度约定反了、地平线取错
// 方向) 不会有这个签名 —— 它们要么恒为 1, 要么与半径无关, 要么往错误的方向
// 走。
//
// GTAO 这类算法最危险的失效方式是**输出一张看起来合理但数值无意义的图**:
// 每一种写错都会给出一张"边角发暗"的图, 而那正是人眼期待看到的。
//
// 分箱方式: 回读 AO 与法线, 对法线朝上 (地面) 的像素反投影到 y=0 平面, 用
// 得到的 z 坐标作为"到夹角的距离"。夹角在 z=0。
// ============================================================================

namespace
{

/// 一次采集的结果 —— 按到夹角的距离分箱后的 AO 均值
struct FAoProfile
{
    static constexpr SizeType kBins    = 20;
    static constexpr Float32  kBinSize = 0.25f;

    Float64  Mean[kBins]  = {};
    SizeType Count[kBins] = {};

    SizeType FloorPixels = 0;
    SizeType OutOfRange  = 0;
    bool     Valid       = false;
};

/// 渲一帧, 回读 AO 与法线, 按距离分箱
static FAoProfile CaptureAoProfile(FRenderContext* context,
                                   FRenderer&      renderer,
                                   Float32         radius)
{
    FAoProfile profile;

    FGtaoPass* const gtao      = renderer.GetGtaoPass();
    FDepthPrePass* const depth = renderer.GetDepthPrePass();

    if (gtao == nullptr || depth == nullptr)
    {
        return profile;
    }

    gtao->SetRadius(radius);

    IRHIDevice* const device = context->GetDevice();

    const FRHIExtent2D extent = context->GetSwapchainExtent();

    const SizeType pixelCount =
        static_cast<SizeType>(extent.Width) * extent.Height;

    FRHIBufferHandle aoReadback;
    FRHIBufferHandle normalReadback;

    {
        FRHIBufferDesc desc = {};
        desc.Usage       = EBufferUsage::TransferDst;
        desc.MemoryUsage = EMemoryUsage::GpuToCpu;

        desc.Size      = pixelCount * 2u;
        desc.DebugName = "AoCheck.AO";

        if (!IsRHISuccess(device->CreateBuffer(desc, aoReadback)))
        {
            return profile;
        }

        desc.Size      = pixelCount * 4u;
        desc.DebugName = "AoCheck.Normal";

        if (!IsRHISuccess(device->CreateBuffer(desc, normalReadback)))
        {
            device->DestroyBuffer(aoReadback);
            return profile;
        }
    }

    bool recorded = false;

    renderer.SetPostSceneRenderCallback(
        [&recorded, context, gtao, depth, aoReadback, normalReadback, extent]()
        {
            IRHICommandBuffer* cmd = context->GetCurrentCommandBuffer();

            if (cmd == nullptr)
            {
                return;
            }

            const FRHITextureHandle sources[2] =
            {
                gtao->GetAoTexture(),
                depth->GetNormalTexture(),
            };

            const FRHIBufferHandle targets[2] = { aoReadback, normalReadback };

            for (UInt32 i = 0; i < 2; ++i)
            {
                cmd->TransitionImageLayout(
                    sources[i],
                    EImageLayout::ShaderReadOnly,
                    EImageLayout::TransferSrc,
                    EPipelineStageFlags::FragmentShader,
                    EPipelineStageFlags::Transfer,
                    EAccessFlags::ShaderRead,
                    EAccessFlags::TransferRead);

                FRHIBufferTextureCopyRegion region = {};
                region.BufferOffset      = 0;
                region.BufferRowLength   = 0;
                region.BufferImageHeight = 0;
                region.MipLevel          = 0;
                region.BaseLayer         = 0;
                region.LayerCount        = 1;
                region.TextureOffset     = { 0, 0, 0 };
                region.TextureExtent     = { extent.Width, extent.Height, 1 };

                cmd->CopyTextureToBuffer(sources[i],
                                         EImageLayout::TransferSrc,
                                         targets[i], region);

                cmd->TransitionImageLayout(
                    sources[i],
                    EImageLayout::TransferSrc,
                    EImageLayout::ShaderReadOnly,
                    EPipelineStageFlags::Transfer,
                    EPipelineStageFlags::FragmentShader,
                    EAccessFlags::TransferRead,
                    EAccessFlags::ShaderRead);
            }

            recorded = true;
        });

    renderer.RenderFrame();

    renderer.SetPostSceneRenderCallback(TFunction<void()>());

    if (!recorded)
    {
        device->DestroyBuffer(aoReadback);
        device->DestroyBuffer(normalReadback);
        return profile;
    }

    device->WaitIdle();

    TArray<Float32>  ao;
    TArray<FVector2> normals;

    {
        void* mapped = nullptr;

        if (!IsRHISuccess(device->MapBuffer(aoReadback, &mapped)) ||
            mapped == nullptr)
        {
            device->DestroyBuffer(aoReadback);
            device->DestroyBuffer(normalReadback);
            return profile;
        }

        const Float16Bits* src = static_cast<const Float16Bits*>(mapped);

        ao.Reserve(pixelCount);

        for (SizeType i = 0; i < pixelCount; ++i)
        {
            ao.Add(Float16ToFloat32(src[i]));
        }

        device->UnmapBuffer(aoReadback);
    }

    {
        void* mapped = nullptr;

        if (!IsRHISuccess(device->MapBuffer(normalReadback, &mapped)) ||
            mapped == nullptr)
        {
            device->DestroyBuffer(aoReadback);
            device->DestroyBuffer(normalReadback);
            return profile;
        }

        const Float16Bits* src = static_cast<const Float16Bits*>(mapped);

        normals.Reserve(pixelCount);

        for (SizeType i = 0; i < pixelCount; ++i)
        {
            normals.Add(FVector2(Float16ToFloat32(src[i * 2]),
                                 Float16ToFloat32(src[i * 2 + 1])));
        }

        device->UnmapBuffer(normalReadback);
    }

    device->DestroyBuffer(aoReadback);
    device->DestroyBuffer(normalReadback);

    // ---- 分箱 ----
    const FCamera& camera = renderer.GetCamera();

    const FMatrix inverse =
        (camera.GetProjectionMatrix() * camera.GetViewMatrix()).Inverse();

    const FVector3 cameraPos = camera.GetPosition();

    Float64 sum[FAoProfile::kBins] = {};

    for (UInt32 y = 0; y < extent.Height; ++y)
    {
        for (UInt32 x = 0; x < extent.Width; ++x)
        {
            const SizeType index =
                static_cast<SizeType>(y) * extent.Width + x;

            const Float32 value = ao[index];

            if (value < -1.0e-3f || value > 1.0f + 1.0e-3f)
            {
                ++profile.OutOfRange;
                continue;
            }

            if (FMath::Abs(normals[index].X) > 1.0f ||
                FMath::Abs(normals[index].Y) > 1.0f)
            {
                continue;
            }

            const FVector3 n = DecodeOctahedralNormal(normals[index]);

            if (n.Y < 0.95f)
            {
                continue;
            }

            ++profile.FloorPixels;

            const Float32 ndcX =
                (static_cast<Float32>(x) + 0.5f) /
                    static_cast<Float32>(extent.Width) * 2.0f - 1.0f;
            const Float32 ndcY =
                (static_cast<Float32>(y) + 0.5f) /
                    static_cast<Float32>(extent.Height) * 2.0f - 1.0f;

            const FVector4 farWorld =
                inverse.TransformVector4(FVector4(ndcX, ndcY, 1.0f, 1.0f));

            if (FMath::Abs(farWorld.W) < 1.0e-9f)
            {
                continue;
            }

            const FVector3 dir(farWorld.X / farWorld.W - cameraPos.X,
                               farWorld.Y / farWorld.W - cameraPos.Y,
                               farWorld.Z / farWorld.W - cameraPos.Z);

            if (FMath::Abs(dir.Y) < 1.0e-6f)
            {
                continue;
            }

            const Float32 t = -cameraPos.Y / dir.Y;

            if (t <= 0.0f)
            {
                continue;
            }

            const Float32 worldZ = cameraPos.Z + dir.Z * t;

            if (worldZ < 0.0f)
            {
                continue;
            }

            const SizeType bin =
                static_cast<SizeType>(worldZ / FAoProfile::kBinSize);

            if (bin < FAoProfile::kBins)
            {
                sum[bin] += static_cast<Float64>(value);
                ++profile.Count[bin];
            }
        }
    }

    for (SizeType b = 0; b < FAoProfile::kBins; ++b)
    {
        if (profile.Count[b] > 0)
        {
            profile.Mean[b] =
                sum[b] / static_cast<Float64>(profile.Count[b]);
        }
    }

    profile.Valid = true;

    return profile;
}

/// 第一个样本足够的箱的均值 (最靠近夹角)
static Float64 NearCornerMean(const FAoProfile& profile)
{
    for (SizeType b = 0; b < FAoProfile::kBins; ++b)
    {
        if (profile.Count[b] >= 64)
        {
            return profile.Mean[b];
        }
    }

    return -1.0;
}

} // namespace

static bool RunAoChecks(FRenderContext* context, FRenderer& renderer)
{
    FGtaoPass* const gtao = renderer.GetGtaoPass();

    if (gtao == nullptr || !gtao->IsEnabled())
    {
        LIMX_LOG(LogLaunch, Error,
                 "[AO] GTAO 未启用 — 自检无从判定 (加 --gtao)");
        return false;
    }

    // 两个半径。小的那个用来验"远处开阔地面不该有遮蔽" (它必须远大于半径,
    // 否则那条判据在物理上就不成立); 大的那个用来验收敛趋势。
    constexpr Float32 kSmallRadius = 2.0f;
    constexpr Float32 kLargeRadius = 8.0f;

    const Float32 originalRadius = 2.0f;

    const FAoProfile small = CaptureAoProfile(context, renderer, kSmallRadius);
    const FAoProfile large = CaptureAoProfile(context, renderer, kLargeRadius);

    gtao->SetRadius(originalRadius);

    if (!small.Valid || !large.Valid)
    {
        LIMX_LOG(LogLaunch, Error, "[AO] 采集失败");
        return false;
    }

    bool passed = true;

    // 分箱曲线总是打印 —— 判定通过与否都要能看到形状。只在失败时打的话,
    // 通过的那次就没有可对照的基线, 下一次数值漂移了也无从发现。
    for (SizeType b = 0; b < FAoProfile::kBins; ++b)
    {
        if (small.Count[b] >= 64)
        {
            LIMX_LOG(LogLaunch, Display,
                     "[AO] z {} ~ {}: R=2 → {} | R=8 → {} ({} 样本)",
                     static_cast<Float32>(b) * FAoProfile::kBinSize,
                     static_cast<Float32>(b + 1) * FAoProfile::kBinSize,
                     small.Mean[b], large.Mean[b], small.Count[b]);
        }
    }

    // ---- 1. 值域 ----
    if (small.OutOfRange > 0 || large.OutOfRange > 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[AO] AO 超出 [0,1] 的像素: R=2 有 {} 个, R=8 有 {} 个",
                 small.OutOfRange, large.OutOfRange);
        passed = false;
    }

    // ---- 判定有效性 ----
    const SizeType pixelCount =
        static_cast<SizeType>(context->GetSwapchainExtent().Width) *
        context->GetSwapchainExtent().Height;

    if (small.FloorPixels * 10 < pixelCount)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[AO] 只有 {} / {} 个像素是地面 —— 场景不对, 判定无效",
                 small.FloorPixels, pixelCount);
        return false;
    }

    const Float64 nearSmall = NearCornerMean(small);
    const Float64 nearLarge = NearCornerMean(large);

    if (nearSmall < 0.0 || nearLarge < 0.0)
    {
        LIMX_LOG(LogLaunch, Error, "[AO] 夹角附近样本不足");
        return false;
    }

    // ---- 2. 夹角处确实被遮蔽 ----
    //
    // 上界 0.75: 一个什么都不做的实现给 1.0, 一个只做了一半的给 0.85 以上。
    // 下界 0.40: 低于它说明遮挡被高估, 那通常是角度约定反了。
    if (nearSmall > 0.75 || nearSmall < 0.40)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[AO] 夹角处 (R=2) 的 AO 是 {}, 应在 [0.40, 0.75] —— "
                 "解析极限是 0.5, 有限半径下略高",
                 nearSmall);
        passed = false;
    }

    // ---- 3. 随半径增大朝 0.5 收敛 ----
    //
    // 这是这个算法的物理签名。写错的实现要么与半径无关, 要么往错误的方向走。
    if (nearLarge >= nearSmall - 0.01)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[AO] 半径从 2 增到 8, 夹角处的 AO 从 {} 变成 {} —— "
                 "没有朝 0.5 收敛。遮挡量与搜索半径无关说明实现有误",
                 nearSmall, nearLarge);
        passed = false;
    }

    if (nearLarge < 0.40 || nearLarge > 0.70)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[AO] 夹角处 (R=8) 的 AO 是 {}, 应在 [0.40, 0.70]",
                 nearLarge);
        passed = false;
    }

    // ---- 4. 远处开阔地面 ≈ 1 ----
    //
    // 只在小半径下验。距离必须远大于半径, 否则墙的遮挡在物理上本就存在 ——
    // R=8 时 z=5 处的地面确实被遮住不少, 那不是缺陷。
    Float64 farOpen = -1.0;

    for (SizeType b = FAoProfile::kBins; b > 0; --b)
    {
        if (small.Count[b - 1] >= 64)
        {
            const Float32 distance =
                static_cast<Float32>(b - 1) * FAoProfile::kBinSize;

            if (distance > kSmallRadius * 2.0f)
            {
                farOpen = small.Mean[b - 1];
            }

            break;
        }
    }

    if (farOpen < 0.0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[AO] 最远的箱距离夹角不足 {} 个单位 —— "
                 "取不到开阔地面的样本, 判定无效",
                 kSmallRadius * 2.0f);
        passed = false;
    }
    else if (farOpen < 0.95)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[AO] 开阔地面的 AO 是 {}, 应接近 1 —— 无遮挡处不该有遮蔽",
                 farOpen);
        passed = false;
    }

    // ---- 5. 单调性 ----
    SizeType inversions = 0;

    Float64 previous = -1.0;

    for (SizeType b = 0; b < FAoProfile::kBins; ++b)
    {
        if (small.Count[b] < 64)
        {
            continue;
        }

        if (previous >= 0.0 && small.Mean[b] < previous - 0.02)
        {
            ++inversions;
        }

        previous = small.Mean[b];
    }

    if (inversions > 1)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[AO] 沿离开夹角的方向出现 {} 处非单调下降", inversions);
        passed = false;
    }

    if (passed)
    {
        LIMX_LOG(LogLaunch, Log,
                 "[AO] 夹角处 R=2 → {}, R=8 → {} (朝解析值 0.5 收敛); "
                 "开阔处 {}; {} 个地面像素",
                 nearSmall, nearLarge, farOpen, small.FloorPixels);
    }

    return passed;
}

// ============================================================================
// RunTaaChecks — 证明 TAA 真的在做抗锯齿
//
// TAA 最危险的失效方式是**看起来正常但什么都没做**: 裁剪范围取小了, 历史
// 每帧都被拉回当前值; 或者重投影全部落在屏幕外, 历史一律被拒。两种情况下
// 画面都完全正常, 只是锯齿还在 —— 而"锯齿还在"要对着屏幕看边缘才发现。
//
// 判据是数值的, 分三条:
//
//   1. **有效性**: 抖动 N 帧的**均匀平均**就是超采样的真值。TAA 的输出必须
//      比其中任何单帧都显著更接近那个真值。一个"什么都没做"的 TAA 输出等于
//      单帧, 这一条直接不成立。
//
//   2. **收敛性**: 静止场景下连续两帧的 TAA 输出必须几乎相同。不收敛意味着
//      历史权重或裁剪有问题, 表现是静止画面上的持续闪烁。
//
//   3. **有效性的对照**: 单帧与均值的距离必须**足够大**。若场景本身没有
//      锯齿 (比如全屏纯色), 单帧就已经等于均值, 第一条便退化成"0 < 0",
//      恒不成立或恒成立 —— 都不是判据。
//
// 帧数取 32 (抖动周期 16 的整数倍), 于是均值里每个抖动位置的权重相同, 那
// 才是真正的均匀超采样。不是整数倍的话均值本身就带偏。
// ============================================================================
static bool RunTaaChecks(FRenderContext* context, FRenderer& renderer)
{
    if (renderer.GetTaaPass() == nullptr)
    {
        LIMX_LOG(LogLaunch, Error, "[TAA] 解析通道不存在");
        return false;
    }

    // 抖动周期是 16, 取它的整数倍
    constexpr UInt32 kFrames = 32;

    // ---- 阶段 1: 抖动开、TAA 关 —— 收集 N 帧求均匀平均 ----
    //
    // 这是自检内部才允许的组合。正常运行时两者同开同关 (见
    // FRenderer::SetTaaEnabled 的说明), 但构造超采样真值需要拿到每一帧的
    // 抖动结果, 那只能在解析关闭时取。
    renderer.SetTaaEnabled(false);
    renderer.SetTemporalJitterEnabled(true);

    TArray<Float64> accum;
    TArray<UInt8>   lastFrame;

    for (UInt32 i = 0; i < kFrames; ++i)
    {
        FScreenshotCapture shot;

        if (!shot.Request(context))
        {
            return false;
        }

        renderer.SetPostSceneRenderCallback(
            [&shot, context]() { shot.RecordCopy(context); });

        renderer.RenderFrame();

        // 立刻摘掉回调。shot 是栈上的, 循环下一轮它就没了 —— 而回调里持的
        // 是引用。留着的话, 后面任何一次 RenderFrame 都会去访问一块已经出
        // 作用域的内存, 表现是"dstBuffer is VK_NULL_HANDLE"后接一个访问
        // 违例, 而崩溃点离真正的原因隔着几十帧。
        renderer.SetPostSceneRenderCallback(TFunction<void()>());

        TArray<UInt8> pixels;

        if (!shot.ReadPixels(context, pixels))
        {
            shot.Release(context->GetDevice());
            return false;
        }

        shot.Release(context->GetDevice());

        if (accum.GetSize() == 0)
        {
            accum.Reserve(pixels.GetSize());

            for (SizeType p = 0; p < pixels.GetSize(); ++p)
            {
                accum.Add(0.0);
            }
        }

        if (accum.GetSize() != pixels.GetSize())
        {
            LIMX_LOG(LogLaunch, Error, "[TAA] 帧尺寸在采集途中变了");
            return false;
        }

        for (SizeType p = 0; p < pixels.GetSize(); ++p)
        {
            accum[p] += static_cast<Float64>(pixels[p]);
        }

        lastFrame = pixels;
    }

    TArray<Float64> average;
    average.Reserve(accum.GetSize());

    for (SizeType p = 0; p < accum.GetSize(); ++p)
    {
        average.Add(accum[p] / static_cast<Float64>(kFrames));
    }

    // ---- 阶段 2: TAA 开, 渲到收敛 ----
    renderer.SetTaaEnabled(true);

    for (UInt32 i = 0; i < kFrames; ++i)
    {
        renderer.RenderFrame();
    }

    TArray<UInt8> taaFrame;
    TArray<UInt8> taaFrameNext;

    for (UInt32 pass = 0; pass < 2; ++pass)
    {
        FScreenshotCapture shot;

        if (!shot.Request(context))
        {
            return false;
        }

        renderer.SetPostSceneRenderCallback(
            [&shot, context]() { shot.RecordCopy(context); });

        renderer.RenderFrame();

        renderer.SetPostSceneRenderCallback(TFunction<void()>());

        TArray<UInt8>& target = (pass == 0) ? taaFrame : taaFrameNext;

        if (!shot.ReadPixels(context, target))
        {
            shot.Release(context->GetDevice());
            return false;
        }

        shot.Release(context->GetDevice());
    }

    renderer.SetPostSceneRenderCallback(TFunction<void()>());

    // ---- 判据 ----
    bool passed = true;

    if (taaFrame.GetSize() != average.GetSize() ||
        lastFrame.GetSize() != average.GetSize() ||
        taaFrameNext.GetSize() != average.GetSize())
    {
        LIMX_LOG(LogLaunch, Error, "[TAA] 采集到的尺寸不一致");
        return false;
    }

    Float64 sumTaa    = 0.0;
    Float64 sumSingle = 0.0;
    Float64 sumStable = 0.0;

    for (SizeType p = 0; p < average.GetSize(); ++p)
    {
        const Float64 dTaa =
            static_cast<Float64>(taaFrame[p]) - average[p];
        const Float64 dSingle =
            static_cast<Float64>(lastFrame[p]) - average[p];
        const Float64 dStable =
            static_cast<Float64>(taaFrameNext[p]) -
            static_cast<Float64>(taaFrame[p]);

        sumTaa    += dTaa * dTaa;
        sumSingle += dSingle * dSingle;
        sumStable += dStable * dStable;
    }

    const Float64 count = static_cast<Float64>(average.GetSize());

    const Float64 rmsTaa    = FMath::Sqrt(sumTaa / count);
    const Float64 rmsSingle = FMath::Sqrt(sumSingle / count);
    const Float64 rmsStable = FMath::Sqrt(sumStable / count);

    // 3. 对照: 单帧与均值必须确实有差距, 否则第一条判据无意义
    if (rmsSingle < 0.5)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[TAA] 单帧与多帧平均的 RMS 只有 {} —— 场景里几乎没有抖动"
                 "带来的差异, 这组判据无从判定。抖动是不是没生效?",
                 rmsSingle);
        passed = false;
    }

    // 1. 有效性: TAA 必须显著更接近真值
    //
    // 阈值取 0.5 倍。TAA 的输出是指数滑动平均而非均匀平均, 所以不可能等于
    // 真值; 但"比单帧近一倍以上"是一个不做任何抗锯齿的实现绝对达不到的。
    if (rmsTaa >= rmsSingle * 0.5)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[TAA] 解析结果并不比单帧更接近多帧平均 "
                 "(TAA RMS {} vs 单帧 RMS {}) —— 历史可能每帧都被裁掉了",
                 rmsTaa, rmsSingle);
        passed = false;
    }

    // 2. 收敛性: 静止场景下连续两帧必须几乎相同
    if (rmsStable > 1.0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[TAA] 静止场景下连续两帧的 RMS 差异 {} 过大 —— 未收敛",
                 rmsStable);
        passed = false;
    }

    if (passed)
    {
        LIMX_LOG(LogLaunch, Log,
                 "[TAA] 解析有效: 与 {} 帧平均的 RMS {} vs 单帧 {} "
                 "(近 {} 倍); 连续帧 RMS {}",
                 kFrames, rmsTaa, rmsSingle,
                 (rmsTaa > 1.0e-9) ? (rmsSingle / rmsTaa) : 0.0,
                 rmsStable);
    }

    return passed;
}

// ============================================================================
// RunLightCullChecks — 分簇着色与暴力法的逐像素比对
//
// 这是分簇光照最强的一条验收判据: **同一帧、同一个着色器、只翻转一个布尔
// 值**, 两次的画面必须完全一致。分簇的定义就是"剔掉那些照不到本像素的光",
// 所以正确的剔除对最终颜色的影响必须恰好为零。
//
// 前一条 (--cluster-check) 验的是簇表本身与 CPU 参照一致; 这一条验的是
// 片段着色器**用对了**那张表 —— 切片映射、屏幕分块、索引区间的读取。两者
// 覆盖的是完全不同的失效方式: 簇表全对而片段着色器查错簇, 前一条全绿。
//
// 判据不留容差: 两条路径累加的是同一批光源的同一批贡献, 只是遍历顺序不同。
// 浮点加法不满足结合律, 所以顺序不同**会**带来最低位的差异 —— 因此判据是
// "每个通道差不超过 1/255", 那正好是 8 位输出的一个量化档。
// ============================================================================
static bool RunLightCullChecks(FRenderContext* context, FRenderer& renderer)
{
    FClusterLightPass* const pass = renderer.GetClusterLightPass();

    if (pass == nullptr)
    {
        LIMX_LOG(LogLaunch, Error, "[LightCull] 分簇通道不存在");
        return false;
    }

    FScreenshotCapture brute;
    FScreenshotCapture clustered;

    const bool originalMode = renderer.IsClusteredLighting();

    // ---- 暴力法 ----
    renderer.SetClusteredLighting(false);

    if (!brute.Request(context))
    {
        return false;
    }

    renderer.SetPostSceneRenderCallback(
        [&brute, context]() { brute.RecordCopy(context); });

    renderer.RenderFrame();

    TArray<UInt8> bruteImage;

    if (!brute.ReadPixels(context, bruteImage))
    {
        brute.Release(context->GetDevice());
        return false;
    }

    // ---- 分簇 ----
    renderer.SetClusteredLighting(true);

    if (!clustered.Request(context))
    {
        brute.Release(context->GetDevice());
        return false;
    }

    renderer.SetPostSceneRenderCallback(
        [&clustered, context]() { clustered.RecordCopy(context); });

    renderer.RenderFrame();

    TArray<UInt8> clusteredImage;

    if (!clustered.ReadPixels(context, clusteredImage))
    {
        brute.Release(context->GetDevice());
        clustered.Release(context->GetDevice());
        return false;
    }

    renderer.SetPostSceneRenderCallback(TFunction<void()>());
    renderer.SetClusteredLighting(originalMode);

    brute.Release(context->GetDevice());
    clustered.Release(context->GetDevice());

    // ---- 比对 ----
    bool passed = true;

    if (bruteImage.GetSize() != clusteredImage.GetSize() ||
        bruteImage.GetSize() == 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[LightCull] 两次回读的尺寸不同 ({} vs {})",
                 bruteImage.GetSize(), clusteredImage.GetSize());
        return false;
    }

    SizeType overTolerance = 0;
    UInt32   maxDiff       = 0;
    SizeType nonBlack      = 0;

    for (SizeType i = 0; i < bruteImage.GetSize(); ++i)
    {
        const UInt32 a = bruteImage[i];
        const UInt32 b = clusteredImage[i];

        const UInt32 diff = (a > b) ? (a - b) : (b - a);

        maxDiff = FMath::Max(maxDiff, diff);

        if (diff > 1u)
        {
            ++overTolerance;
        }

        if (a > 8u)
        {
            ++nonBlack;
        }
    }

    // 全黑画面下两张图当然一致 —— 那证明不了任何事。
    if (nonBlack * 10 < bruteImage.GetSize())
    {
        LIMX_LOG(LogLaunch, Error,
                 "[LightCull] 画面几乎全黑 ({} / {} 个通道有值) —— "
                 "比对无意义, 判定无效",
                 nonBlack, bruteImage.GetSize());
        passed = false;
    }

    if (overTolerance > 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[LightCull] {} / {} 个通道的差异超过 1/255 (最大 {}) —— "
                 "分簇剔掉了本该照到的光, 或者查错了簇",
                 overTolerance, bruteImage.GetSize(), maxDiff);
        passed = false;
    }
    else
    {
        LIMX_LOG(LogLaunch, Log,
                 "[LightCull] 分簇与暴力法逐像素一致 "
                 "({} 个通道, 最大差异 {}/255)",
                 bruteImage.GetSize(), maxDiff);
    }

    if (pass->HasOverflowed())
    {
        LIMX_LOG(LogLaunch, Error,
                 "[LightCull] 索引表曾经溢出 — 比对结果不可信");
        passed = false;
    }

    return passed;
}

// ============================================================================
// RunGpuDrivenChecks — GPU 驱动路径与逐物体绘制逐像素比对
//
// 判据是"两条路径画出来的东西**完全一样**"。这一条比"看起来对"强得多:
// GPU 驱动错了的表现高度趋同 —— 剔除多剔了几个 (画面上少几个物体)、
// firstInstance 没接上 (整个场景挤在一个变换上)、分组的起点算错 (某一组画
// 成了另一组的几何)。这三种都不崩、不报错。
//
// 能这样比的前提是**两条路径走的是同一份着色器代码**: 顶点着色器只有一条
// 路径, 总是从 set 3 的 storage buffer 取模型矩阵与材质下标; 逐物体绘制
// 那条路径只是把物体下标经 firstInstance 传进去。两份着色器变体的话, 比出
// 来的差异里就混进了"编译器对两份代码的不同优化"这个因素。分簇光照那一天
// 是同一个理由。
//
// 另外两条判据:
//   - GPU 剔除必须**真的剔掉了东西**。可见数等于总数的话, 一个什么都不做
//     的剔除实现同样能得到正确画面 —— 判据形同虚设。
//   - 可见数必须 >= CPU 剔除后的数量。包围球外接于包围盒, 所以 GPU 保留的
//     一定是 CPU 保留的超集; 反过来就是画面上少东西。
// ============================================================================
static bool RunGpuDrivenChecks(FRenderContext* context, FRenderer& renderer)
{
    FGpuCullPass* const cull = renderer.GetGpuCullPass();

    if (cull == nullptr)
    {
        LIMX_LOG(LogLaunch, Error, "[GPU驱动] 剔除通道不存在");
        return false;
    }

    if (!cull->IsSupported())
    {
        LIMX_LOG(LogLaunch, Error,
                 "[GPU驱动] 本设备不支持 drawIndirectFirstInstance — "
                 "自检无从判定");
        return false;
    }

    const bool originalMode = cull->IsEnabled();

    const auto CaptureFrame = [&](bool gpuDriven,
                                  TArray<UInt8>& outPixels) -> bool
    {
        cull->SetEnabled(gpuDriven);

        FScreenshotCapture shot;

        if (!shot.Request(context))
        {
            return false;
        }

        renderer.SetPostSceneRenderCallback(
            [&shot, context]() { shot.RecordCopy(context); });

        renderer.RenderFrame();

        // 立刻摘掉回调 —— shot 是栈上的
        renderer.SetPostSceneRenderCallback(TFunction<void()>());

        const bool ok = shot.ReadPixels(context, outPixels);

        shot.Release(context->GetDevice());

        return ok;
    };

    TArray<UInt8> cpuImage;
    TArray<UInt8> gpuImage;

    if (!CaptureFrame(false, cpuImage))
    {
        cull->SetEnabled(originalMode);
        LIMX_LOG(LogLaunch, Error, "[GPU驱动] 逐物体路径回读失败");
        return false;
    }

    if (!CaptureFrame(true, gpuImage))
    {
        cull->SetEnabled(originalMode);
        LIMX_LOG(LogLaunch, Error, "[GPU驱动] 间接路径回读失败");
        return false;
    }

    // 可见数的回读隔着并行帧数。
    //
    // 计数器是每帧拷进一份**按帧下标编号**的回读缓冲区的, 而读的是同一个
    // 下标上一轮写的值。所以要拿到刚才那次 GPU 驱动的计数, 必须让帧下标
    // 转回来 —— 再渲 MaxFramesInFlight 帧, 期间 GPU 驱动保持开启。
    //
    // 只渲一帧是不够的: 那读的是**另一个**帧下标的回读缓冲区, 而那一格上
    // 一轮是逐物体路径写的 (其实根本没写), 于是读出 0。
    // 第一版就是这么错的, 而 0 恰好通不过后面"可见数必须大于零"那条 ——
    // 若那条判据当初写成"可见数不能等于总数", 这个错误会一直留着。
    for (UInt32 warm = 0; warm < context->GetMaxFramesInFlight() + 1u; ++warm)
    {
        renderer.RenderFrame();
    }

    const UInt32 objectCount  = cull->GetObjectCount();
    const UInt32 visibleCount = cull->GetVisibleCount();
    const UInt32 groupCount   =
        static_cast<UInt32>(cull->GetGroups().GetSize());

    cull->SetEnabled(originalMode);

    bool passed = true;

    // ---- 1. 逐像素一致 ----
    if (cpuImage.GetSize() != gpuImage.GetSize() || cpuImage.GetSize() == 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[GPU驱动] 两次回读的尺寸不同 ({} vs {})",
                 cpuImage.GetSize(), gpuImage.GetSize());
        return false;
    }

    SizeType overTolerance = 0;
    UInt32   maxDiff       = 0;
    SizeType nonBlack      = 0;

    for (SizeType i = 0; i < cpuImage.GetSize(); ++i)
    {
        const UInt32 a = cpuImage[i];
        const UInt32 b = gpuImage[i];

        const UInt32 diff = (a > b) ? (a - b) : (b - a);

        maxDiff = FMath::Max(maxDiff, diff);

        if (diff > 1u)
        {
            ++overTolerance;
        }

        if (a > 8u)
        {
            ++nonBlack;
        }
    }

    // 全黑画面下两张图当然一致 —— 那证明不了任何事。
    if (nonBlack * 10 < cpuImage.GetSize())
    {
        LIMX_LOG(LogLaunch, Error,
                 "[GPU驱动] 画面几乎全黑 ({} / {} 个通道有值) —— "
                 "比对无意义, 判定无效",
                 nonBlack, cpuImage.GetSize());
        passed = false;
    }

    if (overTolerance > 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[GPU驱动] {} / {} 个通道的差异超过 1/255 (最大 {}) —— "
                 "剔除多剔了, 还是 firstInstance 没接上?",
                 overTolerance, cpuImage.GetSize(), maxDiff);
        passed = false;
    }
    else
    {
        LIMX_LOG(LogLaunch, Display,
                 "[GPU驱动] 两条路径逐像素一致 ({} 个通道, 最大差异 {}/255)",
                 cpuImage.GetSize(), maxDiff);
    }

    // ---- 2. 剔除必须真的剔掉了东西 ----
    LIMX_LOG(LogLaunch, Display,
             "[GPU驱动] 上传 {} 个物体, 相机视图判可见 {} 个, 分成 {} 组, "
             "共 {} 个视图",
             objectCount, visibleCount, groupCount, cull->GetViewCount());

    // 逐视图都报。只报相机那一个的话, 级联视图整段没写过 (计数为 0) 这种
    // 情况看不出来 —— 而那正是"拿未初始化的显存当间接命令"。
    for (UInt32 view = 1; view < cull->GetViewCount(); ++view)
    {
        LIMX_LOG(LogLaunch, Display,
                 "[GPU驱动]   视图 {} 判可见 {} 个",
                 view, cull->GetVisibleCount(view));

        if (cull->GetVisibleCount(view) == 0)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[GPU驱动] 视图 {} 一个可见物体都没有 —— "
                     "那一段间接命令从没被写过?", view);
            passed = false;
        }
    }

    if (objectCount == 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[GPU驱动] 一个物体都没上传 — 判定无效");
        passed = false;
    }
    else if (visibleCount == 0)
    {
        // 画面明明画出了东西, 计数却是 0 —— 那说明计数这条路本身断了,
        // 而不是"真的一个都不可见"。断了的计数会让下面那条"不能一个都不剔"
        // 的判据永远成立, 于是它形同虚设。
        LIMX_LOG(LogLaunch, Error,
                 "[GPU驱动] 可见数为 0 而画面有内容 —— 计数器回读断了, "
                 "后面的判据不可信");
        passed = false;
    }
    else if (visibleCount >= objectCount)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[GPU驱动] 可见 {} 个 / 总共 {} 个 —— 一个都没剔掉。"
                 "一个什么都不做的剔除实现也会给出正确画面, 这条判据因此"
                 "无从判定。换一个有物体在视锥外的相机角度。",
                 visibleCount, objectCount);
        passed = false;
    }

    // ---- 3. 分组必须与物体列表严格对应 ----
    //
    // 逐像素比对抓不到这一条: 分组条件漏掉"单双面"时, 封闭几何用错剔除模式
    // 画出来的图往往完全一样 (背面被正面挡住, 深度测试兜住了)。而错误是实在
    // 的 —— 换成薄片几何 (叶子、旗帜) 立刻现形, 那时半边会消失。
    //
    // 所以这一条直接核结构: 分组必须连续覆盖整个列表, 且同一组里每个物体的
    // 顶点/索引缓冲区、索引宽度、单双面都要与组一致。
    {
        const TArray<FRenderObject>& source = renderer.GetShadowCasterObjects();
        const TArray<FDrawGroup>&    groups = cull->GetGroups();

        UInt32   cursor     = 0;
        SizeType mismatches = 0;

        // 双面物体的数量决定"分组漏看单双面"这条变异抓不抓得住。
        //
        // 一个都没有的话, 分组条件写不写那一项产出的分组完全一样 —— 判据
        // 无从判定, 而它照样返回通过。所以数目要报出来: 通过与"没东西可判"
        // 必须能分开。
        SizeType doubleSidedCount = 0;

        for (SizeType i = 0; i < source.GetSize(); ++i)
        {
            if (source[i].IsDoubleSided)
            {
                ++doubleSidedCount;
            }
        }

        LIMX_LOG(LogLaunch, Display,
                 "[GPU驱动] 源列表 {} 个物体, 其中双面 {} 个",
                 source.GetSize(), doubleSidedCount);

        if (doubleSidedCount == 0 || doubleSidedCount == source.GetSize())
        {
            LIMX_LOG(LogLaunch, Error,
                     "[GPU驱动] 场景里单双面没有混合 —— "
                     "分组是否漏看单双面这一条无从判定");
            passed = false;
        }

        for (SizeType g = 0; g < groups.GetSize(); ++g)
        {
            const FDrawGroup& group = groups[g];

            if (group.FirstCommand != cursor)
            {
                LIMX_LOG(LogLaunch, Error,
                         "[GPU驱动] 第 {} 组的起点是 {}, 应当是 {} —— 分组不连续",
                         g, group.FirstCommand, cursor);
                passed = false;
                break;
            }

            for (UInt32 k = 0; k < group.CommandCount; ++k)
            {
                const SizeType index = static_cast<SizeType>(cursor) + k;

                if (index >= source.GetSize())
                {
                    break;
                }

                const FRenderObject& obj = source[index];

                if (obj.VertexBuffer.Packed != group.VertexBuffer.Packed ||
                    obj.IndexBuffer.Packed  != group.IndexBuffer.Packed ||
                    obj.IndexType           != group.IndexType ||
                    obj.IsDoubleSided       != group.IsDoubleSided)
                {
                    ++mismatches;
                }
            }

            cursor += group.CommandCount;
        }

        if (mismatches > 0)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[GPU驱动] {} 个物体与所属分组的绑定状态不符 —— "
                     "分组条件漏了一项?",
                     mismatches);
            passed = false;
        }

        if (cursor != objectCount)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[GPU驱动] 分组一共覆盖 {} 个物体, 上传了 {} 个",
                     cursor, objectCount);
            passed = false;
        }
    }

    // ---- 4. 分组必须真的合并了绘制 ----
    //
    // 组数等于物体数意味着分组完全没起作用 —— 那时间接绘制的下发次数与逐
    // 物体绘制一样多, CPU 一点也没省。而画面依然完全正确。
    if (objectCount > 0 && groupCount >= objectCount)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[GPU驱动] {} 个物体分成了 {} 组 —— 分组没起作用, "
                 "批次列表是不是没按状态聚类?",
                 objectCount, groupCount);
        passed = false;
    }

    return passed;
}

// ============================================================================
// RunClusterChecks — 分簇剔除的数值自检
//
// 判据是"GPU 算出的簇表与 CPU 参照实现**完全一致**"。这比"看起来对"强得多:
// 分簇错了的表现是某些角度下某些光不亮, 而那在真实场景里极难复现。
//
// 两份实现 (FClusterGrid.h 与 cluster_common.h + 两个 .comp) 逐行对应, 但
// 没有编译期保障。这条检查就是那个保障。
// ============================================================================
static bool RunClusterChecks(FRenderContext* context, FRenderer& renderer)
{
    FClusterLightPass* const pass = renderer.GetClusterLightPass();

    if (pass == nullptr)
    {
        LIMX_LOG(LogLaunch, Error, "[Cluster] 分簇通道不存在");
        return false;
    }

    FLightManager& lights = FLightManager::Get();

    if (lights.GetLightCount() == 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Cluster] 场景里没有光源 — 自检无从判定 "
                 "(用 --light-grid N 造一批)");
        return false;
    }

    FClusterCapture capture;

    if (!capture.Request(context))
    {
        return false;
    }

    renderer.SetPostSceneRenderCallback(
        [&capture, context, pass]()
        {
            capture.RecordCopy(context, pass);
        });

    renderer.RenderFrame();

    if (!capture.Resolve(context))
    {
        capture.Release(context->GetDevice());
        return false;
    }

    renderer.SetPostSceneRenderCallback(TFunction<void()>());

    // ---- CPU 参照 ----
    //
    // 用与 GPU 完全相同的输入: 同一个相机矩阵、同一批光源、同一套网格常量。
    const FCamera& camera = renderer.GetCamera();

    const FMatrix view       = camera.GetViewMatrix();
    const FMatrix projection = camera.GetProjectionMatrix();
    const FMatrix inverse    = projection.Inverse();

    const Float32 nearPlane = camera.GetNearPlane();
    const Float32 farPlane  = camera.GetFarPlane();

    const TArray<Float32>& gpuBounds  = capture.GetBounds();
    const TArray<UInt32>&  gpuGrid    = capture.GetGrid();
    const TArray<UInt32>&  gpuIndices = capture.GetIndices();

    bool passed = true;

    // ---- 1. 簇包围盒 ----
    SizeType boundsMismatch = 0;
    Float32  maxBoundsError = 0.0f;

    for (UInt32 cz = 0; cz < kClusterGridZ; ++cz)
    {
        for (UInt32 cy = 0; cy < kClusterGridY; ++cy)
        {
            for (UInt32 cx = 0; cx < kClusterGridX; ++cx)
            {
                const UInt32 index = ClusterLinearIndex(cx, cy, cz);

                const FClusterBounds expected = ComputeClusterBounds(
                    cx, cy, cz, inverse, nearPlane, farPlane);

                const Float32 actual[6] =
                {
                    gpuBounds[index * 8u + 0u],
                    gpuBounds[index * 8u + 1u],
                    gpuBounds[index * 8u + 2u],
                    gpuBounds[index * 8u + 4u],
                    gpuBounds[index * 8u + 5u],
                    gpuBounds[index * 8u + 6u],
                };

                const Float32 want[6] =
                {
                    expected.Min.X, expected.Min.Y, expected.Min.Z,
                    expected.Max.X, expected.Max.Y, expected.Max.Z,
                };

                // 容差按包围盒尺度给 —— 远处的簇跨越几十米, 绝对容差
                // 在那里没有意义。
                const Float32 scale = FMath::Max(
                    FMath::Max(FMath::Abs(want[3] - want[0]),
                               FMath::Abs(want[5] - want[2])),
                    1.0f);

                bool mismatched = false;

                for (UInt32 k = 0; k < 6; ++k)
                {
                    const Float32 error = FMath::Abs(actual[k] - want[k]);

                    maxBoundsError = FMath::Max(maxBoundsError,
                                                error / scale);

                    if (error > scale * 1.0e-4f)
                    {
                        mismatched = true;
                    }
                }

                if (mismatched)
                {
                    if (boundsMismatch < 3)
                    {
                        LIMX_LOG(LogLaunch, Error,
                                 "[Cluster] 簇 ({},{},{}) 包围盒不符: "
                                 "GPU min=({},{},{}) CPU min=({},{},{})",
                                 cx, cy, cz,
                                 actual[0], actual[1], actual[2],
                                 want[0], want[1], want[2]);
                    }

                    ++boundsMismatch;
                }
            }
        }
    }

    if (boundsMismatch > 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Cluster] {} / {} 个簇的包围盒与 CPU 参照不符",
                 boundsMismatch, kClusterCount);
        passed = false;
    }
    else
    {
        LIMX_LOG(LogLaunch, Log,
                 "[Cluster] 包围盒: {} 个簇全部吻合 (最大相对偏差 {})",
                 kClusterCount, maxBoundsError);
    }

    // ---- 2. 光源分配 ----
    //
    // 逐簇比对光源集合。GPU 与 CPU 都按光源下标升序遍历并追加, 所以两侧
    // 的序列必须**逐项相同**, 不只是集合相同。
    SizeType assignMismatch = 0;
    SizeType totalAssigned  = 0;
    SizeType nonEmpty       = 0;

    for (UInt32 cz = 0; cz < kClusterGridZ; ++cz)
    {
        for (UInt32 cy = 0; cy < kClusterGridY; ++cy)
        {
            for (UInt32 cx = 0; cx < kClusterGridX; ++cx)
            {
                const UInt32 index = ClusterLinearIndex(cx, cy, cz);

                const UInt32 offset = gpuGrid[index * 2u];
                const UInt32 count  = gpuGrid[index * 2u + 1u];

                const FClusterBounds bounds = ComputeClusterBounds(
                    cx, cy, cz, inverse, nearPlane, farPlane);

                // CPU 侧重算这个簇该拿到哪些光
                UInt32 expectedCount = 0;
                bool   sequenceOk    = true;

                UInt32 activeIndex = 0;

                for (UInt32 l = 0; l < lights.GetLightCount(); ++l)
                {
                    const FLight& light = lights.GetLight(l);

                    if (!light.IsEnabled())
                    {
                        continue;
                    }

                    const UInt32 gpuLightIndex = activeIndex;
                    ++activeIndex;

                    // 方向光不参与分簇
                    if (light.GetType() == ELightType::Directional)
                    {
                        continue;
                    }

                    const FVector3 worldPos = light.GetPosition();

                    const FVector4 viewPos4 = view.TransformVector4(
                        FVector4(worldPos.X, worldPos.Y, worldPos.Z, 1.0f));

                    const FVector3 viewPos(viewPos4.X, viewPos4.Y, viewPos4.Z);

                    if (!SphereIntersectsAABB(viewPos, light.GetRange(),
                                              bounds.Min, bounds.Max))
                    {
                        continue;
                    }

                    if (expectedCount < count)
                    {
                        if (gpuIndices[offset + expectedCount] != gpuLightIndex)
                        {
                            sequenceOk = false;
                        }
                    }

                    ++expectedCount;
                }

                totalAssigned += count;

                if (count > 0)
                {
                    ++nonEmpty;
                }

                if (expectedCount != count || !sequenceOk)
                {
                    if (assignMismatch < 3)
                    {
                        LIMX_LOG(LogLaunch, Error,
                                 "[Cluster] 簇 ({},{},{}) 光源分配不符: "
                                 "GPU {} 盏, CPU 参照 {} 盏, 序列一致={}",
                                 cx, cy, cz, count, expectedCount,
                                 sequenceOk ? 1 : 0);
                    }

                    ++assignMismatch;
                }
            }
        }
    }

    if (assignMismatch > 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Cluster] {} / {} 个簇的光源分配与 CPU 参照不符",
                 assignMismatch, kClusterCount);
        passed = false;
    }
    else
    {
        LIMX_LOG(LogLaunch, Log,
                 "[Cluster] 光源分配: {} 个簇全部吻合 "
                 "(其中 {} 个非空, 共 {} 条索引)",
                 kClusterCount, nonEmpty, totalAssigned);
    }

    // ---- 3. 判定有效性 ----
    //
    // 一个"什么都不分配"的实现会让上面两条全部通过 —— CPU 参照也算出零盏,
    // 逐簇比对完美一致。必须确认真的分配出了东西。
    //
    // 5% 这个下限远低于任何真实场景: 演示场景只有一盏点光 (range 8) 时非空
    // 簇就有 12340 / 18432 = 67%。它拦的不是"光源太少", 是"计算通道根本没
    // 执行" —— 那种情况下缓冲区里是未初始化的内容。
    if (nonEmpty * 20 < kClusterCount)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Cluster] 只有 {} / {} 个簇拿到了光源 (不足 5%) —— "
                 "样本不足, 判定无效。计算通道是不是没执行?",
                 nonEmpty, kClusterCount);
        passed = false;
    }

    if (pass->HasOverflowed())
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Cluster] 索引表曾经溢出 — 有光源被丢弃");
        passed = false;
    }

    capture.Release(context->GetDevice());

    return passed;
}

// ============================================================================
// RunGBufferChecks — G-Buffer 的数值自检
//
// 三个阶段, 顺序不能换:
//   A. 相机静止渲一帧 —— 建立"上一帧矩阵"
//   B. 只转动偏航角 (不平移) 再渲一帧 —— 速度必须**非零且逐像素等于预测值**
//   C. 相机保持不动再渲一帧 —— 速度必须**处处恰好为零**
//
// 为什么必须先 B 后 C: 只测 C 的话, 一个"速度恒为零"的实现 (比如上一帧
// 矩阵根本没接进 UBO) 会完美通过。B 证明这条通路真的能产出非零值, C 才
// 证明它在该为零时确实归零。反过来的顺序验证不了任何东西。
//
// 为什么 B 只转不平移: 纯旋转下, 同一条视线上的所有点投影到同一个像素,
// 因而速度与深度无关 —— 可以只凭像素的 NDC 坐标算出精确预测值, 不需要
// 回读深度。而深度在场景渲染后是 DontCare 的, 本来也读不到。
//
// 这一条同时钉住了另一个坑: 若顶点着色器提前做了透视除法再插值 (那是错
// 的, NDC 不能线性插值), 三角形内部的误差会远超容差, 而顶点附近正常。
// ============================================================================
static bool RunGBufferChecks(FRenderContext* context,
                             FRenderer&      renderer,
                             LScene*         scene)
{
    FDepthPrePass* const pass = renderer.GetDepthPrePass();

    if (pass == nullptr)
    {
        LIMX_LOG(LogLaunch, Error, "[GBuffer] 深度预通道不存在");
        return false;
    }

    FCamera& camera = renderer.GetCamera();

    // 只动偏航角, 位置保持不变 —— 深度无关性的前提就是相机不平移
    const Float32 baseYaw   = camera.GetYaw();
    const Float32 basePitch = camera.GetPitch();

    // 2.9 度。够大, 使 NDC 位移 (约 0.09) 远高于半精度的分辨率 (那个量级
    // 上约 6e-5); 又够小, 使上一次剔除得到的可见物体列表依然基本有效。
    constexpr Float32 kYawDelta = 0.05f;

    FGBufferCapture capture;

    // 场景不动, 所以每一帧之前不做 SyncScene —— 它会把相机换成场景里那个
    // 主相机 Trait 的外部矩阵, 把这里的 SetRotation 覆盖掉。
    (void)scene;

    // 逐帧记下抖动量。覆盖翻转那条判据抓不到单轴退化 —— 只有 X 轴抖动
    // 时覆盖照样翻转, 而 TAA 拿到的是一维退化的采样图案, 表现为竖直方向
    // 的锯齿永远消不掉。像素→NDC 的换算就在 FRenderer 里那四行, 没有别的
    // 地方覆盖它。
    FVector2 jitterPerFrame[3] = {};

    // ---- 阶段 A: 建立上一帧矩阵 ----
    camera.SetRotation(baseYaw, basePitch);
    renderer.RenderFrame();
    jitterPerFrame[0] = renderer.GetCurrentJitter();

    const FMatrix viewProjA =
        camera.GetProjectionMatrix() * camera.GetViewMatrix();

    // ---- 阶段 B: 转动 ----
    camera.SetRotation(baseYaw + kYawDelta, basePitch);

    if (!capture.Request(context))
    {
        return false;
    }

    renderer.SetPostSceneRenderCallback(
        [&capture, context, pass]()
        {
            capture.RecordCopy(context, pass);
        });

    renderer.RenderFrame();
    jitterPerFrame[1] = renderer.GetCurrentJitter();

    const FMatrix viewProjB =
        camera.GetProjectionMatrix() * camera.GetViewMatrix();

    if (!capture.Resolve(context))
    {
        capture.Release(context->GetDevice());
        return false;
    }

    // 渲染器实际用的上一帧矩阵必须就是阶段 A 那个。这一条直接钉住"保存
    // 时机错位"这类 bug —— 它比后面的逐像素比较更早、更明确地失败。
    const FMatrix& rendererPrev = renderer.GetPrevViewProjNoJitter();

    Float32 prevMatrixDrift = 0.0f;

    for (Int32 row = 0; row < 4; ++row)
    {
        for (Int32 col = 0; col < 4; ++col)
        {
            prevMatrixDrift = FMath::Max(
                prevMatrixDrift,
                FMath::Abs(rendererPrev.M[row][col] - viewProjB.M[row][col]));
        }
    }

    bool passed = true;

    if (prevMatrixDrift > 1.0e-5f)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[GBuffer] 渲染器保存的上一帧矩阵与本帧矩阵不符 "
                 "(最大偏差 {}) —— 保存时机错位",
                 prevMatrixDrift);
        passed = false;
    }

    // 必须拷贝而不是引用: capture 在阶段 C 会被复用, Resolve 会把这两个
    // 数组整体覆盖掉。持引用的话阶段 C 之后 normalB 指向的就是阶段 C 的
    // 数据 —— 于是"两帧覆盖对比"变成了自己跟自己比, 恒等于 0 个差异。
    const TArray<FVector2> normalB   = capture.GetNormal();
    const TArray<FVector2> velocityB = capture.GetVelocity();
    const FRHIExtent2D     extent    = capture.GetExtent();

    const FMatrix inverseB = viewProjB.Inverse();

    SizeType covered      = 0;
    SizeType mismatched   = 0;
    Float32  maxError     = 0.0f;
    Float32  maxPredicted = 0.0f;

    const FVector3 cameraPosition = camera.GetPosition();

    Float32  maxFacing    = -2.0f;
    Float64  facingSum    = 0.0;
    SizeType facingCount  = 0;
    bool     directionBuckets[512] = {};

    // 半精度在 0.1 量级的 ulp 约 6e-5。取 2e-3 是三十多个 ulp, 足以吸收
    // 存储量化与矩阵求逆的累积误差, 而任何矩阵错位造成的偏差都在 0.1 以上
    // —— 两者相差近两个数量级, 这个阈值不敏感。
    constexpr Float32 kVelocityTolerance = 2.0e-3f;

    for (UInt32 y = 0; y < extent.Height; ++y)
    {
        for (UInt32 x = 0; x < extent.Width; ++x)
        {
            const SizeType index =
                static_cast<SizeType>(y) * extent.Width + x;

            // 哨兵 = 这个像素没有几何
            if (FMath::Abs(normalB[index].X) > 1.0f ||
                FMath::Abs(normalB[index].Y) > 1.0f)
            {
                continue;
            }

            ++covered;

            const Float32 ndcX =
                (static_cast<Float32>(x) + 0.5f) /
                    static_cast<Float32>(extent.Width) * 2.0f - 1.0f;
            const Float32 ndcY =
                (static_cast<Float32>(y) + 0.5f) /
                    static_cast<Float32>(extent.Height) * 2.0f - 1.0f;

            // 反投影到世界空间。取远平面上那个点 —— 纯旋转下同一视线上
            // 任何点算出的速度都相同, 取哪个不影响结果。
            const FVector4 farClip(ndcX, ndcY, 1.0f, 1.0f);
            const FVector4 farWorld = inverseB.TransformVector4(farClip);

            if (FMath::Abs(farWorld.W) < 1.0e-9f)
            {
                continue;
            }

            const FVector4 worldPoint(farWorld.X / farWorld.W,
                                      farWorld.Y / farWorld.W,
                                      farWorld.Z / farWorld.W,
                                      1.0f);

            // ---- 法线: 可见表面必须朝向相机 ----
            //
            // 这一条是真有内容的判据。原先那条"解码后是单位向量"是同义
            // 反复 —— DecodeOctahedralNormal 末尾就做了归一化, 无论输入
            // 是什么它都返回单位向量, 所以那个检查恒真。
            //
            // 背面剔除开着, 所以每个可见片元的法线与视线方向的点积必须
            // 为负 (剪影处趋近于 0)。编码里任何符号错误、折叠分支漏写、
            // 或者通道写反, 都会让一大片像素的点积变正。
            const FVector3 rayDir(worldPoint.X - cameraPosition.X,
                                  worldPoint.Y - cameraPosition.Y,
                                  worldPoint.Z - cameraPosition.Z);

            const Float32 rayLength =
                FMath::Sqrt(rayDir.X * rayDir.X + rayDir.Y * rayDir.Y +
                            rayDir.Z * rayDir.Z);

            if (rayLength > 1.0e-6f)
            {
                const FVector3 decoded =
                    DecodeOctahedralNormal(normalB[index]);

                const Float32 facing =
                    (decoded.X * rayDir.X + decoded.Y * rayDir.Y +
                     decoded.Z * rayDir.Z) / rayLength;

                maxFacing  = FMath::Max(maxFacing, facing);
                facingSum += static_cast<Float64>(facing);
                ++facingCount;

                // 把方向量化进 8x8x8 的格子, 统计占用了多少格。
                //
                // 这一条防的是"整张图是同一个常量"——比如附件根本没写入,
                // 或者法线矩阵算成了零。那种情况下上面两个统计量可能碰巧
                // 落在合格区间里, 但占用格数会是 1。
                const Int32 bx = FMath::Clamp(
                    static_cast<Int32>((decoded.X + 1.0f) * 4.0f), 0, 7);
                const Int32 by = FMath::Clamp(
                    static_cast<Int32>((decoded.Y + 1.0f) * 4.0f), 0, 7);
                const Int32 bz = FMath::Clamp(
                    static_cast<Int32>((decoded.Z + 1.0f) * 4.0f), 0, 7);

                directionBuckets[(bz * 8 + by) * 8 + bx] = true;
            }

            const FVector4 prevClip = viewProjA.TransformVector4(worldPoint);

            if (FMath::Abs(prevClip.W) < 1.0e-9f)
            {
                continue;
            }

            const Float32 predictedX = ndcX - prevClip.X / prevClip.W;
            const Float32 predictedY = ndcY - prevClip.Y / prevClip.W;

            maxPredicted = FMath::Max(
                maxPredicted,
                FMath::Max(FMath::Abs(predictedX), FMath::Abs(predictedY)));

            const Float32 errorX =
                FMath::Abs(velocityB[index].X - predictedX);
            const Float32 errorY =
                FMath::Abs(velocityB[index].Y - predictedY);
            const Float32 error = FMath::Max(errorX, errorY);

            maxError = FMath::Max(maxError, error);

            if (error > kVelocityTolerance)
            {
                if (mismatched < 4)
                {
                    LIMX_LOG(LogLaunch, Error,
                             "[GBuffer] 像素 ({},{}) 速度不符: "
                             "GPU=({},{}) 预测=({},{})",
                             x, y, velocityB[index].X, velocityB[index].Y,
                             predictedX, predictedY);
                }
                ++mismatched;
            }
        }
    }

    const SizeType totalPixels =
        static_cast<SizeType>(extent.Width) * extent.Height;

    // 覆盖率太低说明画面里几乎没有几何 —— 那样上面的循环等于没跑, 而
    // "没跑" 与 "全对" 在结果上无法区分。这是本项目反复踩到的坑。
    if (covered * 20 < totalPixels)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[GBuffer] 覆盖像素仅 {} / {} (不足 5%) —— "
                 "校验样本不足, 判定无效",
                 covered, totalPixels);
        passed = false;
    }

    // 预测值本身必须是个有意义的非零量。若相机其实没转 (或矩阵没更新),
    // 预测值会全是 0, 而 GPU 侧也是 0, 逐像素比较全部通过。
    if (maxPredicted < 0.01f)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[GBuffer] 预测位移最大仅 {} —— 相机实际未转动, 判定无效",
                 maxPredicted);
        passed = false;
    }

    if (mismatched > 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[GBuffer] 运动帧: {} / {} 个覆盖像素的速度与预测不符 "
                 "(最大偏差 {})",
                 mismatched, covered, maxError);
        passed = false;
    }
    else
    {
        LIMX_LOG(LogLaunch, Log,
                 "[GBuffer] 运动帧: {} 个覆盖像素全部吻合 "
                 "(最大偏差 {}, 最大位移 {})",
                 covered, maxError, maxPredicted);
    }

    // ---- 阶段 C: 相机保持不动 ----
    if (!capture.Request(context))
    {
        capture.Release(context->GetDevice());
        return false;
    }

    renderer.RenderFrame();
    jitterPerFrame[2] = renderer.GetCurrentJitter();

    if (!capture.Resolve(context))
    {
        capture.Release(context->GetDevice());
        return false;
    }

    const TArray<FVector2>& velocityC = capture.GetVelocity();
    const TArray<FVector2>& normalC   = capture.GetNormal();

    SizeType nonZero    = 0;
    Float32  maxNonZero = 0.0f;

    for (SizeType index = 0; index < totalPixels; ++index)
    {
        // 恰好为零, 不留容差。相机与物体都没动, 两帧的矩阵逐位相同,
        // 着色器算出的差值就是精确的 0 —— 这里放容差等于放掉了整类
        // "矩阵差了一帧" 的 bug (那类 bug 的残留量可以很小)。
        if (velocityC[index].X != 0.0f || velocityC[index].Y != 0.0f)
        {
            ++nonZero;
            maxNonZero = FMath::Max(
                maxNonZero,
                FMath::Max(FMath::Abs(velocityC[index].X),
                           FMath::Abs(velocityC[index].Y)));
        }
    }

    if (nonZero > 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[GBuffer] 静止帧: {} / {} 个像素速度非零 (最大 {}) —— "
                 "上一帧矩阵保存错位",
                 nonZero, totalPixels, maxNonZero);
        passed = false;
    }
    else
    {
        LIMX_LOG(LogLaunch, Log,
                 "[GBuffer] 静止帧: {} 个像素速度全部为零", totalPixels);
    }

    // ---- 抖动幅度 ----
    Float32 maxJitterX = 0.0f;
    Float32 maxJitterY = 0.0f;

    for (SizeType i = 0; i < 3; ++i)
    {
        maxJitterX = FMath::Max(maxJitterX, FMath::Abs(jitterPerFrame[i].X));
        maxJitterY = FMath::Max(maxJitterY, FMath::Abs(jitterPerFrame[i].Y));
    }

    // 半个像素在 NDC 里就是 1/尺寸 (NDC 横跨 2 个单位)
    const Float32 halfPixelX = 1.0f / static_cast<Float32>(extent.Width);
    const Float32 halfPixelY = 1.0f / static_cast<Float32>(extent.Height);

    if (renderer.IsTemporalJitterEnabled())
    {
        // 两轴都必须动过。取三帧里的最大值而不是逐帧判 —— Halton 基 2 的
        // 1 号点恰好是 0.5, 减去 0.5 之后偏移正好为零, 所以单独一帧的某个
        // 分量为零是合法的。三帧里不可能有两帧撞上同一个下标。
        if (maxJitterX <= 0.0f || maxJitterY <= 0.0f)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[GBuffer] 抖动幅度在某个轴上恒为零 "
                     "(三帧最大 X={} Y={}) —— 采样图案退化成一维",
                     maxJitterX, maxJitterY);
            passed = false;
        }

        // 超过半个像素说明像素→NDC 的换算写错了 (比如漏了那个 2, 或者除
        // 错了维度)。幅度过大的表现是画面明显抖动而不是亚像素抖动。
        if (maxJitterX > halfPixelX * 1.001f ||
            maxJitterY > halfPixelY * 1.001f)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[GBuffer] 抖动幅度超过半个像素 "
                     "(X={} 上限 {}, Y={} 上限 {})",
                     maxJitterX, halfPixelX, maxJitterY, halfPixelY);
            passed = false;
        }
    }
    else
    {
        if (maxJitterX != 0.0f || maxJitterY != 0.0f)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[GBuffer] 抖动未开启但偏移非零 (X={} Y={})",
                     maxJitterX, maxJitterY);
            passed = false;
        }
    }

    // ---- 抖动的正向对照: 覆盖掩码必须逐帧变化 ----
    //
    // 阶段 B 与阶段 C 的相机朝向完全相同, 所以:
    //   抖动关 → 两帧的光栅化覆盖必须**逐像素完全一致**
    //   抖动开 → 亚像素偏移会让轮廓上的像素翻转覆盖状态
    //
    // 没有这一条时, "--taa 是个空开关"这种情况会让上面每一项都完美
    // 通过 —— 速度为零、法线正确、一切正常, 只是抖动根本没生效。而抖动
    // 没生效的后果要等 TAA 接上之后才看得出来 (锯齿消不掉), 那时已经很难
    // 定位到这里。
    SizeType coverageFlips = 0;

    for (SizeType index = 0; index < totalPixels; ++index)
    {
        const bool coveredB = (FMath::Abs(normalB[index].X) <= 1.0f &&
                               FMath::Abs(normalB[index].Y) <= 1.0f);
        const bool coveredC = (FMath::Abs(normalC[index].X) <= 1.0f &&
                               FMath::Abs(normalC[index].Y) <= 1.0f);

        if (coveredB != coveredC)
        {
            ++coverageFlips;
        }
    }

    if (renderer.IsTemporalJitterEnabled())
    {
        // 1280x720 下演示场景的轮廓约数千像素, 半像素抖动实测翻转数百个。
        // 取 32 作为下限 —— 远低于实测值, 又远高于"抖动没生效"时的 0。
        if (coverageFlips < 32)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[GBuffer] 抖动已开启但两帧覆盖只差 {} 个像素 —— "
                     "抖动未真正作用到投影矩阵上",
                     coverageFlips);
            passed = false;
        }
        else
        {
            LIMX_LOG(LogLaunch, Log,
                     "[GBuffer] 抖动生效: 相机不变的两帧覆盖差 {} 个像素",
                     coverageFlips);
        }
    }
    else if (coverageFlips > 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[GBuffer] 抖动未开启, 但相机不变的两帧覆盖差了 {} 个像素 "
                 "—— 存在未受控的每帧变化",
                 coverageFlips);
        passed = false;
    }

    // ---- 法线判据的结论 (数据在运动帧那一轮里已经统计完) ----
    SizeType occupiedBuckets = 0;

    for (SizeType i = 0; i < 512; ++i)
    {
        if (directionBuckets[i])
        {
            ++occupiedBuckets;
        }
    }

    const Float32 meanFacing =
        (facingCount > 0)
            ? static_cast<Float32>(facingSum / static_cast<Float64>(facingCount))
            : 0.0f;

    // 剪影处点积趋近 0, 插值法线在最边上可以略微越过 —— 0.15 是给这一圈
    // 留的余量。任何符号错误造成的偏差都在 1.0 量级, 与它差近一个数量级。
    if (maxFacing > 0.15f)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[GBuffer] 存在背离相机的法线 (最大点积 {}) —— "
                 "八面体编码的符号或折叠分支有误",
                 maxFacing);
        passed = false;
    }

    if (meanFacing > -0.25f)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[GBuffer] 法线与视线的平均点积 {} 偏高 —— "
                 "法线整体朝向不对",
                 meanFacing);
        passed = false;
    }

    // 演示场景有球、立方体与地面, 法线方向应当遍布多个卦限。
    // 只占几个格子说明整张图接近常量。
    if (occupiedBuckets < 20)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[GBuffer] 法线方向只落在 {} 个格子里 —— "
                 "法线缓冲接近常量",
                 occupiedBuckets);
        passed = false;
    }

    if (passed)
    {
        LIMX_LOG(LogLaunch, Log,
                 "[GBuffer] 法线: {} 个像素朝向相机 "
                 "(最大点积 {}, 平均 {}, 占用 {} / 512 个方向格)",
                 facingCount, maxFacing, meanFacing, occupiedBuckets);
    }

    capture.Release(context->GetDevice());
    renderer.SetPostSceneRenderCallback(TFunction<void()>());

    camera.SetRotation(baseYaw, basePitch);

    return passed;
}

// RunFurnaceChecks — 白炉下 IBL 各级预计算结果的数值自检
//
// 三条性质都是解析可知的, 不依赖任何具体环境:
//   辐照度贴图存 E/π, 而 L=1 时 E = ∫cosθ dω = π —— 故应处处为 1
//   预滤波贴图是环境按 GGX 加权的平均 —— 恒为 1 的环境平均后仍是 1
//   BRDF 表的 A+B 是单次散射的方向反照率 —— 它小于 1 的那部分就是
//     GGX 单次散射丢掉的能量, 也正是多次散射补偿要补回来的量
// ============================================================================

static void RunFurnaceChecks(const FEnvironmentMap& environmentMap,
                             FRenderContext*        context)
{
    TArray<Float32> irradiance;
    UInt32          faceSize = 0;

    if (environmentMap.ReadbackIrradiance(context, irradiance, faceSize))
    {
        Float32 minValue = 1.0e30f;
        Float32 maxValue = 0.0f;

        const SizeType texelCount =
            static_cast<SizeType>(faceSize) * faceSize * 6;

        for (SizeType i = 0; i < texelCount; ++i)
        {
            for (SizeType channel = 0; channel < 3; ++channel)
            {
                const Float32 value = irradiance[i * 4 + channel];

                if (value < minValue) { minValue = value; }
                if (value > maxValue) { maxValue = value; }
            }
        }

        LIMX_LOG(LogLaunch, Display,
                 "[白炉] 辐照度贴图 min={} max={} (应处处为 1)",
                 minValue, maxValue);
    }

    TArray<Float32> lut;
    UInt32          lutSize = 0;

    if (environmentMap.ReadbackBrdfLut(context, lut, lutSize))
    {
        // 沿粗糙度扫一列 (取正对视角 n·v≈1), 报出单次散射丢了多少能量
        const UInt32 column = lutSize - 1;

        LIMX_LOG(LogLaunch, Display,
                 "[白炉] 单次散射 GGX 的方向反照率 (n·v≈1):");

        for (UInt32 step = 0; step <= 4; ++step)
        {
            const UInt32 row = (lutSize - 1) * step / 4;

            const SizeType index =
                (static_cast<SizeType>(row) * lutSize + column) * 2;

            const Float32 a = lut[index];
            const Float32 b = lut[index + 1];

            const Float32 roughness = (static_cast<Float32>(row) + 0.5f) /
                                      static_cast<Float32>(lutSize);

            LIMX_LOG(LogLaunch, Display,
                     "         粗糙度 {} → A+B={} (损失 {}%)",
                     roughness, a + b,
                     static_cast<Int32>((1.0f - (a + b)) * 100.0f));
        }
    }
}

// ============================================================================
// RunFurnaceSelfTest — 白炉自检, 以退出码报告结果
//
// 把白炉从"看一眼截图"变成一条可以跑在 CI 里的断言。它一次覆盖整条 IBL 链:
//   等距柱状解码 → 立方体贴图 → mip 链 → 辐照度卷积 → 镜面预滤波 →
//   BRDF 查找表。
//
// 三条判据都是解析可知的, 不依赖任何具体环境贴图:
//   L 恒为 1 时 E = ∫cosθ dω = π, 而辐照度贴图存的是 E/π —— 应处处为 1;
//   恒为 1 的环境按 GGX 加权平均后仍是 1 —— 预滤波每一级都应为 1;
//   BRDF 表的 A+B 是单次散射的方向反照率 —— 不得大于 1 (那意味着凭空
//     多出能量), 且粗糙度趋零时应趋近 1 (镜面无损)。
//
// 容差按 RGBA16F 的量化精度给: 半精度在 1.0 附近的间隔是 2^-10 ≈ 0.001,
// 再加上求积本身的截断, 1% 是合理的界。
// ============================================================================

static bool RunFurnaceSelfTest(FRenderContext* context)
{
    FEnvironmentMap environmentMap;

    const FImageData furnace = BuildFurnaceEnvironment();

    if (!IsRHISuccess(environmentMap.BuildFromEquirect(context, furnace)))
    {
        LIMX_LOG(LogLaunch, Error, "[白炉自检] 合成环境构建失败");
        return false;
    }

    // 容差: 半精度在 1.0 附近的间隔约 0.001, 求积截断再占几分之一个百分点
    constexpr Float32 kTolerance = 0.01f;

    bool passed = true;

    // ---- 一、辐照度处处为 1 ----
    {
        TArray<Float32> pixels;
        UInt32          faceSize = 0;

        if (!environmentMap.ReadbackIrradiance(context, pixels, faceSize))
        {
            LIMX_LOG(LogLaunch, Error, "[白炉自检] 辐照度读回失败");
            return false;
        }

        Float32 minValue = 1.0e30f;
        Float32 maxValue = -1.0e30f;

        const SizeType texelCount =
            static_cast<SizeType>(faceSize) * faceSize * 6;

        for (SizeType i = 0; i < texelCount; ++i)
        {
            for (SizeType channel = 0; channel < 3; ++channel)
            {
                const Float32 value = pixels[i * 4 + channel];

                if (value < minValue) { minValue = value; }
                if (value > maxValue) { maxValue = value; }
            }
        }

        const bool ok = (minValue > 1.0f - kTolerance) &&
                        (maxValue < 1.0f + kTolerance);

        passed = passed && ok;

        LIMX_LOG(LogLaunch, Display,
                 "[白炉自检] 辐照度 min={} max={} — {}",
                 minValue, maxValue, ok ? "通过" : "不通过");
    }

    // ---- 二、预滤波每一级都为 1 ----
    for (UInt32 level = 0; level < FEnvironmentMap::kPrefilterMipLevels;
         ++level)
    {
        TArray<Float32> pixels;
        UInt32          faceSize = 0;

        if (!environmentMap.ReadbackPrefiltered(context, level, pixels,
                                                faceSize))
        {
            LIMX_LOG(LogLaunch, Error,
                     "[白炉自检] 预滤波第 {} 级读回失败", level);
            return false;
        }

        Float32 minValue = 1.0e30f;
        Float32 maxValue = -1.0e30f;

        const SizeType texelCount =
            static_cast<SizeType>(faceSize) * faceSize * 6;

        for (SizeType i = 0; i < texelCount; ++i)
        {
            for (SizeType channel = 0; channel < 3; ++channel)
            {
                const Float32 value = pixels[i * 4 + channel];

                if (value < minValue) { minValue = value; }
                if (value > maxValue) { maxValue = value; }
            }
        }

        const bool ok = (minValue > 1.0f - kTolerance) &&
                        (maxValue < 1.0f + kTolerance);

        passed = passed && ok;

        LIMX_LOG(LogLaunch, Display,
                 "[白炉自检] 预滤波 mip {} ({}x{}) min={} max={} — {}",
                 level, faceSize, faceSize, minValue, maxValue,
                 ok ? "通过" : "不通过");
    }

    // ---- 三、BRDF 表不得凭空造出能量 ----
    {
        TArray<Float32> lut;
        UInt32          size = 0;

        if (!environmentMap.ReadbackBrdfLut(context, lut, size))
        {
            LIMX_LOG(LogLaunch, Error, "[白炉自检] BRDF 表读回失败");
            return false;
        }

        Float32 worstSum      = 0.0f;
        Float32 smoothestSum  = 0.0f;

        for (UInt32 y = 0; y < size; ++y)
        {
            for (UInt32 x = 0; x < size; ++x)
            {
                const SizeType index =
                    (static_cast<SizeType>(y) * size + x) * 2;

                const Float32 sum = lut[index] + lut[index + 1];

                if (sum > worstSum) { worstSum = sum; }

                // 最平滑的一行 (y=0) 应当几乎无损
                if (y == 0 && sum > smoothestSum) { smoothestSum = sum; }
            }
        }

        const bool noGain  = worstSum <= 1.0f + kTolerance;
        const bool mirrorOk = smoothestSum > 1.0f - kTolerance;

        passed = passed && noGain && mirrorOk;

        LIMX_LOG(LogLaunch, Display,
                 "[白炉自检] BRDF A+B 全表最大 {} (须 ≤1), "
                 "最光滑一行 {} (须 ≈1) — {}",
                 worstSum, smoothestSum,
                 (noGain && mirrorOk) ? "通过" : "不通过");
    }

    // 释放顺序与创建相反, 且要先等 GPU 空闲
    if (context->GetDevice() != nullptr)
    {
        context->GetDevice()->WaitIdle();
    }

    environmentMap.Release();

    LIMX_LOG(LogLaunch, Display,
             "[白炉自检] {}", passed ? "全部通过" : "存在不通过项");

    return passed;
}

// ============================================================================
// BuildLightGrid — 在场景上方铺一层 N×N 的点光源
//
// 位置与颜色都是**确定的** (由下标算出, 不用随机数)。分簇剔除的验收判据是
// "分簇结果与暴力法逐像素一致", 那要求同一命令行两次运行产出同一个场景 ——
// 随机光源会让两次的光源集合不同, 于是比较的是两张不同的图, 而差异会被
// 当成剔除的 bug。
//
// 衰减距离取得比格距小: 相邻光源的影响范围重叠但不覆盖全场, 于是每个簇
// 只落进少数几盏光。全部覆盖全场的话分簇不剔除任何东西, 测试就退化成了
// "两条路径都跑了全部光源", 那证明不了剔除是对的。
// ============================================================================

static void BuildLightGrid(UInt32 gridSize)
{
    if (gridSize == 0)
    {
        return;
    }

    FLightManager& lights = FLightManager::Get();

    // 场景大致占据 [-6, 6] 的范围 (地面 10×10, 立方体与球在中间)
    constexpr Float32 kExtent  = 6.0f;
    constexpr Float32 kHeight  = 2.5f;

    const Float32 step =
        (gridSize > 1) ? (2.0f * kExtent / static_cast<Float32>(gridSize - 1))
                       : 0.0f;

    // 衰减距离取格距的 1.5 倍 —— 相邻光源重叠, 但远处的簇碰不到
    const Float32 range = (step > 0.0f) ? (step * 1.5f) : 4.0f;

    UInt32 added = 0;

    for (UInt32 z = 0; z < gridSize; ++z)
    {
        for (UInt32 x = 0; x < gridSize; ++x)
        {
            const Float32 px =
                -kExtent + step * static_cast<Float32>(x);
            const Float32 pz =
                -kExtent + step * static_cast<Float32>(z);

            // 颜色按下标在色相上铺开 —— 剔除错了 (某盏光该亮没亮/不该亮却
            // 亮了) 时颜色差异比亮度差异显眼得多。
            const UInt32 index = z * gridSize + x;

            const Float32 hue =
                static_cast<Float32>(index % 6u) / 6.0f;

            const FLinearColor color(
                0.35f + 0.65f * FMath::Abs(FMath::Cos(hue * 2.0f * FMath::kPi)),
                0.35f + 0.65f * FMath::Abs(FMath::Cos((hue + 0.333f) * 2.0f * FMath::kPi)),
                0.35f + 0.65f * FMath::Abs(FMath::Cos((hue + 0.667f) * 2.0f * FMath::kPi)),
                1.0f);

            const UInt32 handle = lights.AddLight(
                FLight::CreatePoint(FVector3(px, kHeight, pz), color, 2.0f,
                                    range));

            if (handle == 0xFFFFFFFFu)
            {
                break;
            }

            ++added;
        }
    }

    LIMX_LOG(LogLaunch, Display,
             "[Launch] 光源阵列: {}×{} = {} 盏点光源已添加 "
             "(间距 {}, 衰减距离 {})",
             gridSize, gridSize, added, step, range);
}

// ============================================================================
// BuildCornerScene — 两面成直角的大平面
//
// GTAO 的验收基准。90 度凹角处, 余弦加权的可见度**解析值恰好是 0.5** —— 半
// 个半球被另一面墙挡住。这是一个不依赖"看起来对不对"的数, 而 GTAO 这类算法
// 最危险的失效方式恰恰是"输出一张看起来合理但数值无意义的图"。
//
// 场景刻意做得极简: 两个足够大的平面, 相机正对夹角、距离固定。多一个物体都
// 会让解析值不再成立 —— 而"解析值不再成立"与"实现算错了"在结果上无法区分。
//
// 地面在 y=0 的 xz 平面, 墙在 z=0 的 xy 平面 (由地面绕 x 轴转 90 度得到)。
// 夹角是 z=0, y=0 那条线。
// ============================================================================
static void BuildCornerScene(LScene* scene, FRenderContext* context,
                             FRenderer* renderer)
{
    LIMX_CHECK(scene != nullptr);
    LIMX_CHECK(context != nullptr);
    LIMX_CHECK(renderer != nullptr);

    FRenderResourceManager& resources = context->GetResourceManager();
    FMaterial* defaultMaterial        = renderer->GetDefaultMaterial();

    // 20x20 —— 远大于 GTAO 的采样半径, 于是夹角附近的遮挡与无限半平面
    // 几乎没有差别。平面小了的话边缘会漏光, 解析值就不成立。
    FMeshData planeMesh = FGeometryGenerator::GeneratePlane(20.0f, 20.0f, 2, 2);

    FMeshResourceHandle meshHandle =
        resources.CreateMesh(planeMesh, FName("CornerPlane"));

    if (!meshHandle.IsValid())
    {
        LIMX_LOG(LogLaunch, Error, "[Launch] 墙角场景的平面上传失败");
        return;
    }

    struct FCornerEntry
    {
        const AnsiChar* Name;
        FVector3        Position;
        FQuat           Rotation;
    };

    const FCornerEntry entries[2] =
    {
        // 地面: 不旋转, 中心往 +z 挪 10 使夹角落在 z=0
        { "CornerFloor", FVector3(0.0f, 0.0f, 10.0f),
          FQuat::kIdentity },

        // 墙: 绕 x 轴转 90 度让法线朝 +z, 中心往 +y 挪 10
        { "CornerWall", FVector3(0.0f, 10.0f, 0.0f),
          FQuat::FromAxisAngle(FVector3(1.0f, 0.0f, 0.0f),
                               FMath::kHalfPi) },
    };

    for (UInt32 i = 0; i < 2; ++i)
    {
        FTransform nodeTransform;
        nodeTransform.Translation = entries[i].Position;
        nodeTransform.Rotation    = entries[i].Rotation;

        LNode* node = scene->SpawnNode<LNode>(FName(entries[i].Name),
                                              nodeTransform);

        LMeshTrait* meshTrait = node->AddTrait<LMeshTrait>(FName("Mesh"));
        meshTrait->SetMesh(&resources, meshHandle);
        meshTrait->SetMaterial(defaultMaterial);
        meshTrait->SetVisible(true);
    }

    resources.ReleaseMeshReference(meshHandle);

    // 相机: 正对夹角, 略微俯视, 使地面与墙各占屏幕一半左右
    // yaw 0 是朝 -Z 看, 也就是朝着夹角。用 π 的话背对夹角, 屏幕上只有
    // 一片延伸出去的地面 —— 而那看起来"也挺正常", 只是自检永远取不到样本。
    //
    // 俯角 -0.35: 相机在 y=3, 视线中心落在 y=0 平面上距离 3/tan(0.35) ≈ 8.2
    // 处, 即 z ≈ -0.2 —— 正好是夹角。
    renderer->GetCamera().SetPosition(FVector3(0.0f, 3.0f, 8.0f));
    renderer->GetCamera().SetRotation(0.0f, -0.35f);

    LIMX_LOG(LogLaunch, Display,
             "[Launch] 墙角场景已构建 — 两个 20x20 平面成直角, "
             "夹角在 y=0 z=0");
}

// ============================================================================
// BuildBloomScene — 黑背景上的一个自发光小方块
//
// 泛光的验收基准。一个孤立的亮点经过降采样-升采样链之后应当得到一个**径向
// 对称、单调衰减**的光晕 —— 那就是这条链的点扩散函数 (PSF)。
//
// 为什么非要这样验: 降采样/升采样链最典型的缺陷是**半纹素偏移**。核的采样
// 坐标算错半个纹素, 每一级都把图像往同一个方向挪一点点, 六级累积下来光晕
// 整体偏离光源几个像素。而画面上那仍然是"一团发光的东西", 没人看得出来它
// 偏了 —— 除非拿对称性去量。
//
// 场景刻意做到最简: 一个方块, 一盏都没有的光, 黑背景。多任何一样东西, 光晕
// 就不再是单个 PSF 的叠加, 对称性判据也就不成立了。
// ============================================================================
static void BuildBloomScene(LScene* scene, FRenderContext* context,
                            FRenderer* renderer)
{
    LIMX_CHECK(scene != nullptr);
    LIMX_CHECK(context != nullptr);
    LIMX_CHECK(renderer != nullptr);

    FRenderResourceManager& resources = context->GetResourceManager();

    // 自发光材质。用自发光而不是"用一盏很亮的光去照"是因为后者的亮度分布
    // 取决于衰减公式与法线, 方块表面不会是均匀的 —— 而 PSF 的对称性判据
    // 要求光源本身对称。
    FMaterial* emissive =
        FMaterialManager::Get().CreateMaterial("BloomEmissive");

    if (emissive == nullptr)
    {
        LIMX_LOG(LogLaunch, Error, "[Launch] 泛光场景的自发光材质创建失败");
        return;
    }

    // 基色全黑 —— 方块的亮度**只**来自自发光, 不受任何光照影响。
    // 有基色的话它会被环境光照亮, 而环境光是各向同性的, 方块的边缘会因为
    // 法线变化而略有明暗差 —— 那点差异足以破坏对称性判据。
    emissive->SetBaseColor(FVector4(0.0f, 0.0f, 0.0f, 1.0f));
    // 光源缩小之后总能量随面积平方下降, 自发光相应调高 —— 阈值之上要留
    // 足够的信号, 否则泛光缓冲接近全黑, 判据无从判定。
    emissive->SetEmissiveColor(FVector3(60.0f, 60.0f, 60.0f));
    emissive->SetMetallic(0.0f);
    emissive->SetRoughness(1.0f);

    // **接近点光源**的小立方体。
    //
    // 第一版用了 0.4 单位 (半分辨率下约 60 像素宽), 而那让整套判据失效:
    // 一个大光源的"泛光"主要由它自己的形状决定, 而不是由核决定。实测下
    // 把降采样退化成单点采样 (完全不模糊)、或者整条升采样链跳过, 光晕仍然
    // 有 30 像素宽 —— 判据全绿, 而泛光其实什么都没做。
    //
    // 点扩散函数只有在光源接近一个点时才是"函数本身"。0.03 单位在半分辨率
    // 下约 4 像素, 而光晕有 40 像素 —— 十倍的差距, 核的贡献才占主导。
    FMeshData cubeMesh = FGeometryGenerator::GenerateCube();

    FMeshResourceHandle meshHandle =
        resources.CreateMesh(cubeMesh, FName("BloomCube"));

    if (!meshHandle.IsValid())
    {
        LIMX_LOG(LogLaunch, Error, "[Launch] 泛光场景的网格上传失败");
        return;
    }

    FTransform nodeTransform;
    nodeTransform.Translation = FVector3(0.0f, 0.0f, 0.0f);
    nodeTransform.Scale3D     = FVector3(0.03f, 0.03f, 0.03f);

    LNode* node = scene->SpawnNode<LNode>(FName("BloomCube"), nodeTransform);

    LMeshTrait* meshTrait = node->AddTrait<LMeshTrait>(FName("Mesh"));
    meshTrait->SetMesh(&resources, meshHandle);
    meshTrait->SetMaterial(emissive);
    meshTrait->SetVisible(true);

    resources.ReleaseMeshReference(meshHandle);

    // 关掉全部直接光。方块的亮度只来自自发光, 而背景必须是黑的 ——
    // 背景有亮度的话它也会进阈值, 光晕就不再是单个 PSF。
    FLightManager::Get().ClearAllLights();

    // 相机正对方块。yaw 0 是朝 -Z, 所以相机放在 +Z 一侧。
    renderer->GetCamera().SetPosition(FVector3(0.0f, 0.0f, 3.0f));
    renderer->GetCamera().SetRotation(0.0f, 0.0f);

    LIMX_LOG(LogLaunch, Display,
             "[Launch] 泛光场景已构建 — 黑背景上一个 0.03 单位的自发光方块 "
             "(自发光 60.0, 无直接光) —— 接近点光源, 使点扩散函数由核主导");
}

// ============================================================================
// BuildShadowScene — 一堵墙、一块薄板、两盏聚光灯
//
// 阴影图集的验收基准。影子边界的位置由相似三角形唯一确定 (见 ShadowScene
// 命名空间), 所以它是可以**算出来**的 —— 而不是"看着像有影子"。
//
// 为什么非要这样验: 阴影这类缺陷的表现高度趋同。块偏移算错、矩阵没转置、
// 视口与 UV 不一致、深度偏移过大 —— 这四种在画面上都是"影子位置不对"或
// "影子没了", 而人的第一反应永远是去调 bias。只有把边界的位置钉在解析值
// 上, 这几种才区分得开。
//
// 场景刻意做到最简: 一块薄板, 一堵墙, 没有别的几何。多一样东西, 影子就
// 不再是单块板的投影, 相似三角形也就不成立了。
//
// 薄板刻意**偏离灯轴**放 (y 方向), 于是影子的上下两条边到灯轴的距离不等。
// 上下对称的话, 阴影贴图上下翻转这种缺陷会完全看不出来。
// ============================================================================
static void BuildShadowScene(LScene* scene, FRenderContext* context,
                             FRenderer* renderer, UInt32 lightCount,
                             bool pointShadow)
{
    LIMX_CHECK(scene != nullptr);
    LIMX_CHECK(context != nullptr);
    LIMX_CHECK(renderer != nullptr);

    FRenderResourceManager& resources = context->GetResourceManager();

    FMeshData cubeMesh = FGeometryGenerator::GenerateCube();

    FMeshResourceHandle meshHandle =
        resources.CreateMesh(cubeMesh, FName("ShadowCube"));

    if (!meshHandle.IsValid())
    {
        LIMX_LOG(LogLaunch, Error, "[Launch] 阴影场景的网格上传失败");
        return;
    }

    // 纯漫反射的白色材质。粗糙度拉满、金属度为零 —— 高光会在墙上留下一块
    // 亮斑, 而亮斑与阴影的边界混在一起时, "亮/暗"的阈值就不再唯一。
    FMaterial* diffuse =
        FMaterialManager::Get().CreateMaterial("ShadowDiffuse");

    if (diffuse == nullptr)
    {
        LIMX_LOG(LogLaunch, Error, "[Launch] 阴影场景的材质创建失败");
        resources.ReleaseMeshReference(meshHandle);
        return;
    }

    diffuse->SetBaseColor(FVector4(0.8f, 0.8f, 0.8f, 1.0f));
    diffuse->SetMetallic(0.0f);
    diffuse->SetRoughness(1.0f);

    const auto SpawnBox = [&](const FName& name, const FVector3& center,
                              const FVector3& halfExtents)
    {
        FTransform transform;
        transform.Translation = center;
        transform.Scale3D     = FVector3(halfExtents.X * 2.0f,
                                         halfExtents.Y * 2.0f,
                                         halfExtents.Z * 2.0f);

        LNode* node = scene->SpawnNode<LNode>(name, transform);

        LMeshTrait* meshTrait = node->AddTrait<LMeshTrait>(FName("Mesh"));
        meshTrait->SetMesh(&resources, meshHandle);
        meshTrait->SetMaterial(diffuse);
        meshTrait->SetVisible(true);
    };

    // ---- 接收面 ----
    //
    // 前表面正好落在 z = kWallZ, 所以中心要往后退半个厚度。差这半个厚度
    // 的话, 相似三角形算出来的影子边界会整体偏几个百分点 —— 而那个偏差
    // 恰好在容差的量级上, 于是判据变成"有时过有时不过"。
    constexpr Float32 kWallHalfThick = 0.25f;

    SpawnBox(FName("ShadowWall"),
             FVector3(0.0f, 0.0f, ShadowScene::kWallZ - kWallHalfThick),
             FVector3(20.0f, 20.0f, kWallHalfThick));

    if (pointShadow)
    {
        // ---- 点光源模式 ----
        //
        // 只有一盏灯、一块板。与聚光灯那两盏共存的话, 点光源会照亮它们的
        // 影子区, 而那会把"亮/暗"的对比度压下去 —— 判据量的是边界位置,
        // 但边界是靠亮暗中点定的, 中点一挪边界跟着挪。
        SpawnBox(FName("PointOccluder"),
                 FVector3(ShadowScene::kPointOccluderCenterX, 0.0f,
                          ShadowScene::kPointOccluderZ),
                 FVector3(ShadowScene::kPointOccluderHalfX,
                          ShadowScene::kPointOccluderHalfY,
                          ShadowScene::kOccluderHalfThick));

        resources.ReleaseMeshReference(meshHandle);

        FLightManager::Get().ClearAllLights();

        FLight light = FLight::CreatePoint(
            FVector3(0.0f, 0.0f, ShadowScene::kPointLightZ),
            FLinearColor(1.0f, 1.0f, 1.0f, 1.0f),
            ShadowScene::kLightIntensity,
            ShadowScene::kPointLightRange);

        // 衰减关掉 —— 理由与聚光灯那边相同: 判据量的是边界的位置, 亮度的
        // 渐变会让"亮/暗"的中点随位置漂移。
        light.SetAttenuation(1.0f, 0.0f, 0.0f);
        light.SetCastsShadow(true);
        light.SetDebugName("PointShadowLight");

        FLightManager::Get().AddLight(static_cast<FLight&&>(light));

        renderer->GetCamera().SetPosition(
            FVector3(0.0f, 0.0f, ShadowScene::kCameraZ));
        renderer->GetCamera().SetRotation(0.0f, 0.0f);

        LIMX_LOG(LogLaunch, Display,
                 "[Launch] 阴影场景已构建 (点光源模式) — 墙在 z={}, "
                 "薄板在 z={}, 灯在 z={} (相机轴上), 占图集连续六块; "
                 "影子在 |x|={} 处跨过立方体的面边界",
                 ShadowScene::kWallZ, ShadowScene::kPointOccluderZ,
                 ShadowScene::kPointLightZ, ShadowScene::kPointLightZ);
        return;
    }

    // ---- 主灯的遮挡板 ----
    SpawnBox(FName("ShadowOccluder"),
             FVector3(0.0f, ShadowScene::kOccluderCenterY,
                      ShadowScene::kOccluderZ),
             FVector3(ShadowScene::kOccluderHalfX,
                      ShadowScene::kOccluderHalfY,
                      ShadowScene::kOccluderHalfThick));

    // ---- 第二盏灯的遮挡板 ----
    SpawnBox(FName("ShadowOccluder2"),
             FVector3(ShadowScene::kLight2X, ShadowScene::kOccluder2CenterY,
                      ShadowScene::kOccluderZ),
             FVector3(ShadowScene::kOccluder2HalfX,
                      ShadowScene::kOccluder2HalfY,
                      ShadowScene::kOccluderHalfThick));

    resources.ReleaseMeshReference(meshHandle);

    // ---- 光源 ----
    FLightManager::Get().ClearAllLights();

    const auto AddSpot = [](const AnsiChar* name, Float32 x, Float32 innerDeg,
                            Float32 outerDeg)
    {
        FLight light = FLight::CreateSpot(
            FVector3(x, 0.0f, ShadowScene::kLightZ),
            FVector3(0.0f, 0.0f, -1.0f),
            FLinearColor(1.0f, 1.0f, 1.0f, 1.0f),
            ShadowScene::kLightIntensity,
            innerDeg,
            outerDeg,
            ShadowScene::kLightRange);

        // 衰减关掉 —— 只留常量项。
        //
        // 默认的二次衰减会让墙上的亮度从中心到边缘渐变, 而"亮/暗"的阈值
        // 就没有唯一的取法了: 边缘的亮处可能比中心的暗处还暗。测量的是
        // 边界的**位置**, 不是亮度本身, 所以把亮度摊平是对的。
        light.SetAttenuation(1.0f, 0.0f, 0.0f);

        light.SetCastsShadow(true);
        light.SetDebugName(name);

        FLightManager::Get().AddLight(static_cast<FLight&&>(light));
    };

    // ---- 填充灯 ----
    //
    // 先加, 于是被测的两盏拿到**最后**两块。
    //
    // 这不是凑数: 块下标 0 与 1 在图集里是 (0,0) 与 (512,0), 而上一个 Pass
    // 留下的裁剪矩形通常就是交换链大小 (1280×720) —— 那两块正好在里面,
    // 于是"图集 Pass 忘了设裁剪矩形"这类缺陷完全暴露不出来。用满 64 块时
    // 被测的两盏落在下标 62/63, 纹素 x 是 3072 与 3584, 远在交换链之外。
    //
    // 填充灯放在相机视野之外 (|y| ≈ 9, 而相机只看到 ±5.8), 所以它们既不
    // 影响被测区域的亮度, 也不影响解析判据。它们照的是同一堵墙, 因此图集
    // 里那些块是有内容的 —— 空块的话这一步就退化成"多分配了几个下标"。
    // 上限刻意开到块数的两倍 —— 要能构造出"要块的灯比块多"的局面, 否则
    // FLightManager 里那句"超过图集容量"的警告永远不会执行到, 而没执行过的
    // 错误路径与不存在没有区别。
    const UInt32 clampedCount =
        FMath::Clamp(lightCount, 2u, static_cast<UInt32>(kShadowTileCount) * 2u);

    const UInt32 fillerCount = clampedCount - 2u;

    // 灯多过块时, 被测的两盏必须**先**加 —— 否则它们排在后面, 拿不到块,
    // 自检量的就是"没有影子"。块不够时谁被丢掉是个先到先得的事实, 这里
    // 顺序的选择只是让自检有东西可量。
    const bool measuredFirst =
        (clampedCount > static_cast<UInt32>(kShadowTileCount));

    if (measuredFirst)
    {
        AddSpot("ShadowSpotMain", 0.0f,
                ShadowScene::kSpotInnerDeg, ShadowScene::kSpotOuterDeg);

        AddSpot("ShadowSpotSecond", ShadowScene::kLight2X,
                ShadowScene::kLight2InnerDeg, ShadowScene::kLight2OuterDeg);
    }

    for (UInt32 i = 0; i < fillerCount; ++i)
    {
        // 上下两排, 沿 x 铺开。范围保持在墙的 ±20 之内。
        const Float32 row = (i % 2u == 0u) ? 9.0f : -9.0f;
        const Float32 col = -18.0f + 1.2f * static_cast<Float32>(i / 2u);

        // 填充灯的衰减距离刻意压到刚够照到墙 (灯在 z=6, 墙在 z=0)。
        //
        // 沿用被测灯的 30 会把分簇的光源索引表撑爆: 剔除按**包围球**做,
        // 半径 30 的球几乎覆盖整个簇网格, 64 盏就是 64×18432 条索引, 而
        // 容量只有 589824。溢出之后被丢掉的是排在后面的光源 —— 也就是被测
        // 的那两盏, 于是画面整片只剩环境光。
        //
        // 这一条是实测撞出来的: 第一版用了 30, 表现是"阴影自检突然全黑",
        // 而真正的原因在分簇剔除里, 隔着两层。
        FLight light = FLight::CreateSpot(
            FVector3(col, row, ShadowScene::kLightZ),
            FVector3(0.0f, 0.0f, -1.0f),
            FLinearColor(1.0f, 1.0f, 1.0f, 1.0f),
            ShadowScene::kLightIntensity,
            ShadowScene::kLight2InnerDeg,
            ShadowScene::kLight2OuterDeg,
            7.0f);

        light.SetAttenuation(1.0f, 0.0f, 0.0f);
        light.SetCastsShadow(true);
        light.SetDebugName("ShadowSpotFiller");

        FLightManager::Get().AddLight(static_cast<FLight&&>(light));
    }

    // 被测的两盏最后加 —— 拿到最高的两块 (灯多过块时已经先加过了)
    if (!measuredFirst)
    {
        AddSpot("ShadowSpotMain", 0.0f,
                ShadowScene::kSpotInnerDeg, ShadowScene::kSpotOuterDeg);

        AddSpot("ShadowSpotSecond", ShadowScene::kLight2X,
                ShadowScene::kLight2InnerDeg, ShadowScene::kLight2OuterDeg);
    }

    // ---- 相机 ----
    //
    // yaw 0 朝 -Z, 所以相机放在 +Z 一侧、正对墙面。
    renderer->GetCamera().SetPosition(
        FVector3(0.0f, 0.0f, ShadowScene::kCameraZ));
    renderer->GetCamera().SetRotation(0.0f, 0.0f);

    LIMX_LOG(LogLaunch, Display,
             "[Launch] 阴影场景已构建 — 墙在 z={}, 薄板在 z={}, "
             "主灯在 z={} (相机轴上), 投影聚光灯 {} 盏 (其中 {} 盏是填充), "
             "被测的两盏在图集的第 {} 与 {} 块",
             ShadowScene::kWallZ, ShadowScene::kOccluderZ,
             ShadowScene::kLightZ, clampedCount, fillerCount,
             measuredFirst ? 0u : clampedCount - 2u,
             measuredFirst ? 1u : clampedCount - 1u);
}

// ============================================================================
// BuildMaterialGrid — 粗糙度 × 金属度 的球体阵列
//
// 这是验证 IBL 的标准场景, 理由是它把两个自由度摊平成一张图:
//   横向扫粗糙度 —— 预滤波的每一级 mip 都会被看到。级间映射错位、某一级
//     根本没写、mip 选取偏一档, 都会在这一行上表现为某个位置突然跳变。
//   纵向扫金属度 —— 顶行是纯电介质 (只有微弱边缘反射), 底行是纯金属
//     (漫反射为零, 全靠镜面环境项)。金属那一行是否有内容, 直接判定
//     镜面 IBL 到底接没接上。
//
// 用统一的白色基色而非彩色: 反射的颜色应当来自环境, 基色一花, 就分不清
// 看到的是环境的颜色还是材质自己的颜色。
// ============================================================================

static void BuildMaterialGrid(LScene* scene, FRenderContext* context,
                              FRenderer* renderer, UInt32 gridSize)
{
    LIMX_CHECK(scene != nullptr);
    LIMX_CHECK(context != nullptr);
    LIMX_CHECK(renderer != nullptr);

    if (gridSize < 2)
    {
        gridSize = 2;
    }

    FRenderResourceManager& resources = context->GetResourceManager();

    FMeshData sphereMesh = FGeometryGenerator::GenerateSphere(0.45f, 48, 24);

    // 生成器把经纬度映射成了色相 —— 那是演示场景的观感取向, 放在这里
    // 会毁掉整个用意: 反射的颜色应当来自环境, 顶点色一花, 就分不清看到的
    // 是环境的颜色还是网格自带的颜色。这里统一压成白色。
    for (SizeType i = 0; i < sphereMesh.Vertices.GetSize(); ++i)
    {
        sphereMesh.Vertices[i].Color = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
    }

    const FMeshResourceHandle meshHandle =
        resources.CreateMesh(sphereMesh, FName("MaterialGridSphere"));

    if (!meshHandle.IsValid())
    {
        LIMX_LOG(LogLaunch, Error, "[Launch] 材质阵列网格上传失败");
        return;
    }

    constexpr Float32 kSpacing = 1.15f;

    const Float32 halfSpan =
        static_cast<Float32>(gridSize - 1) * kSpacing * 0.5f;

    const Float32 lastIndex = static_cast<Float32>(gridSize - 1);

    for (UInt32 row = 0; row < gridSize; ++row)
    {
        for (UInt32 column = 0; column < gridSize; ++column)
        {
            FMaterial* material =
                FMaterialManager::Get().CreateMaterial("GridMaterial");

            if (material == nullptr)
            {
                LIMX_LOG(LogLaunch, Error, "[Launch] 材质阵列材质创建失败");
                return;
            }

            // 粗糙度不取到 0: 完全光滑的表面反射的是未经滤波的环境贴图,
            // 一个像素就能映出太阳, 结果是一片刺眼的白点 —— 那考的是
            // 色调映射而非预滤波。0.05 起步已经足够看清最光滑那一档。
            const Float32 roughness =
                0.05f + 0.95f * static_cast<Float32>(column) / lastIndex;

            const Float32 metallic =
                static_cast<Float32>(row) / lastIndex;

            // 基色取**恰好** 1.0 而非 0.9: 白炉测试的判据是"物体与背景
            // 逐像素相等", 而反照率一旦小于 1, 物体本就该比背景暗 ——
            // 那样就分不清暗下去的是反照率还是丢掉的能量了。
            material->SetBaseColor(FVector4(1.0f, 1.0f, 1.0f, 1.0f));
            material->SetRoughness(roughness);
            material->SetMetallic(metallic);

            FTransform nodeTransform;
            nodeTransform.Translation = FVector3(
                static_cast<Float32>(column) * kSpacing - halfSpan,
                halfSpan - static_cast<Float32>(row) * kSpacing,
                0.0f);

            LNode* node =
                scene->SpawnNode<LNode>(FName("GridNode"), nodeTransform);

            LMeshTrait* meshTrait = node->AddTrait<LMeshTrait>(FName("Mesh"));
            meshTrait->SetMesh(&resources, meshHandle);
            meshTrait->SetMaterial(material);
            meshTrait->SetVisible(true);
        }
    }

    // 交出创建时的所有权 —— 每个节点都已各自加过引用
    resources.ReleaseMeshReference(meshHandle);

    // 正对阵列中心 —— 阵列铺在 XY 平面上, 相机沿 -Z 看过去
    renderer->GetCamera().SetPosition(
        FVector3(0.0f, 0.0f, halfSpan * 2.4f + 1.5f));
    renderer->GetCamera().SetRotation(0.0f, 0.0f);

    LIMX_LOG(LogLaunch, Display,
             "[Launch] 材质阵列已构建 — {}x{} (横向粗糙度, 纵向金属度)",
             gridSize, gridSize);
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

        // 一半材质设成双面。
        //
        // 不是为了好看 —— 是为了让"绘制分组漏看单双面"这类缺陷有得抓。全部
        // 单面的场景里, 分组条件写不写这一项产出的分组完全一样, 于是
        // --gpu-driven-check 的结构判据无从判定 (实测那条变异在没有双面物体
        // 时逃掉了)。
        //
        // 单双面还决定管线变体, 所以这也顺带覆盖了"一组里混着两种剔除模式"
        // 这条路径。
        materials[i]->SetDoubleSided((i % 2u) == 1u);
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

    // ---- 地面 ----
    //
    // 不是布景。没有地面时级联阴影**落在虚空里** —— 阴影贴图照画, 画面上
    // 一个像素都不受影响, 于是 --gpu-driven-check 那条逐像素判据对整条阴影
    // 路径完全无从判定。
    //
    // 实测: 加地面之前, "阴影通道用相机的视图下标""三级级联全用第 0 级的
    // 视锥""阴影通道的逐物体路径不加段起点"这三条变异一条都抓不住。
    {
        FTransform groundTransform;
        groundTransform.Translation = FVector3(0.0f, -1.0f, 0.0f);
        groundTransform.Scale3D     =
            FVector3(halfSpan * 2.5f + 8.0f, 0.5f, halfSpan * 2.5f + 8.0f);

        LNode* ground =
            scene->SpawnNode<LNode>(FName("StressGround"), groundTransform);

        LMeshTrait* groundMesh = ground->AddTrait<LMeshTrait>(FName("Mesh"));
        groundMesh->SetMesh(&resources, meshHandles[0]);
        groundMesh->SetMaterial(materials[0]);
        groundMesh->SetVisible(true);
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

    // 分项耗时 —— 优化之前必须先知道时间花在哪。
    //
    // "其它"是总耗时减去这四项: 节点构建、材质对象创建、包围盒累积这些
    // 零碎步骤。它若明显偏大, 说明有一块没被埋到点上。
    const Float64 accounted = loadResult.ParseMilliseconds +
                              loadResult.TextureDecodeMilliseconds +
                              loadResult.TextureUploadMilliseconds +
                              loadResult.MeshUploadMilliseconds;

    LIMX_LOG(LogLaunch, Log,
             "[Launch]   分项 — 解析 {} ms | 纹理解码 {} ms | 纹理上传 {} ms "
             "| 网格上传 {} ms | 其它 {} ms",
             loadResult.ParseMilliseconds,
             loadResult.TextureDecodeMilliseconds,
             loadResult.TextureUploadMilliseconds,
             loadResult.MeshUploadMilliseconds,
             loadResult.ElapsedMilliseconds - accounted);

    if (loadResult.DecodedImageBytes > 0 &&
        loadResult.TextureDecodeMilliseconds > 0.0)
    {
        LIMX_LOG(LogLaunch, Log,
                 "[Launch]   解码吞吐 — {} MiB / {} ms = {} MiB/s",
                 loadResult.DecodedImageBytes / (1024 * 1024),
                 loadResult.TextureDecodeMilliseconds,
                 static_cast<Float64>(loadResult.DecodedImageBytes) /
                     (1024.0 * 1024.0) /
                     (loadResult.TextureDecodeMilliseconds / 1000.0));
    }

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

        // 每轮连同环境贴图一起建、一起拆。
        //
        // IBL 的三张贴图挂在**逐帧共享**的光照描述符集上, 而描述符集的
        // 生存期跨越整个进程 —— 换关卡时若只销毁贴图而不改描述符, 集里
        // 留下的就是指向已释放图像的视图。这是这条路径独有的失效方式,
        // 加载/卸载场景本身的引用计数覆盖不到它。
        FEnvironmentMap environmentMap;

        {
            const FImageData furnace = BuildFurnaceEnvironment();

            if (IsRHISuccess(environmentMap.BuildFromEquirect(context, furnace)))
            {
                renderer->SetEnvironmentMap(&environmentMap);
            }
            else
            {
                LIMX_LOG(LogLaunch, Error, "[自检] 环境贴图构建失败");
                LRegistry::Get().Destroy(scene);
                return false;
            }
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

        // 解绑必须早于释放, 且两者之间要等 GPU 空闲 —— 否则正在执行的
        // 帧仍引用着这些图像
        if (context->GetDevice() != nullptr)
        {
            context->GetDevice()->WaitIdle();
        }

        renderer->SetEnvironmentMap(nullptr);
        environmentMap.Release();

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
                               const FRenderer&      renderer,
                               FRenderContext*       context)
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
             "[基准] 状态切换: 材质描述符集绑定 {} 次 | 材质下标变化 {} 次 "
             "| 网格 {} 次",
             sceneStats.MaterialBindCount,
             sceneStats.MaterialSwitchCount, sceneStats.MeshSwitchCount);
    LIMX_LOG(LogLaunch, Log,
             "[基准] 帧耗时: 平均 {} ms | 最差 {} ms | 平均帧率 {} | 总帧数 {}",
             frameStats.AverageFrameMs, frameStats.WorstFrameMs,
             frameStats.AverageFps, frameStats.TotalFrames);

    // ---- CPU 分项 ----
    {
        const FRenderer::FCpuFrameTiming& cpu = renderer.GetCpuFrameTiming();

        LIMX_LOG(LogLaunch, Log,
                 "[基准] CPU 分项 — 取图 {} ms | 更新 {} ms | 录制 {} ms "
                 "| 呈现 {} ms | 合计 {} ms",
                 cpu.AcquireMs, cpu.UpdateMs, cpu.RecordMs,
                 cpu.PresentMs, cpu.TotalMs);
    }

    // ---- 逐 Pass CPU 录制耗时 ----
    {
        FPassManager* passes =
            const_cast<FRenderer&>(renderer).GetPassManager();

        if (passes != nullptr)
        {
            for (SizeType i = 0; i < passes->GetPassCount(); ++i)
            {
                LIMX_LOG(LogLaunch, Log,
                         "[基准] CPU Pass {} — 录制 {} ms",
                         passes->GetPassName(i),
                         passes->GetPassCpuMilliseconds(i));
            }
        }
    }

    // ---- CPU 侧录制耗时 ----
    //
    // 并行录制只能改善"录制"这一段。它占整帧的比例决定了这条路的收益
    // 上限 —— 比例很小的话, 线程数加到多少都不会有明显变化。
    {
        const FParallelRecorder& recorder = renderer.GetRecorder();

        if (recorder.IsInitialized())
        {
            LIMX_LOG(LogLaunch, Log,
                     "[基准] 命令录制: {} ms | {} 段 | {} 线程 "
                     "| 占 CPU 帧时 {}%",
                     recorder.GetRecordMilliseconds(),
                     recorder.GetSegmentCount(),
                     recorder.GetThreadCount(),
                     (frameStats.AverageFrameMs > 0.0)
                         ? (recorder.GetRecordMilliseconds()
                            / frameStats.AverageFrameMs * 100.0)
                         : 0.0);
        }
    }

    // ---- 逐 Pass GPU 计时 ----
    //
    // 这里报的是最后一次成功回读的那一帧, 不是平均值 —— 平均需要跨帧
    // 累加, 而累加器的重置时机与预热帧的边界一旦对不齐, 得到的数字会
    // 混进首帧的管线编译开销。单帧快照口径明确, 且它足以回答"时间花在
    // 哪一个 Pass"这个问题。
    {
        const FGpuProfiler& profiler = renderer.GetGpuProfiler();

        if (!profiler.IsSupported())
        {
            LIMX_LOG(LogLaunch, Log, "[基准] GPU 计时: 硬件不支持");
        }
        else if (profiler.GetResolvedFrameCount() == 0)
        {
            LIMX_LOG(LogLaunch, Warning,
                     "[基准] GPU 计时: 无有效样本 — 查询结果始终未就绪");
        }
        else
        {
            const Float64 frameMs = profiler.GetFrameMilliseconds();
            const Float64 sumMs   = profiler.GetScopeSumMilliseconds();

            for (UInt32 i = 0; i < profiler.GetScopeCount(); ++i)
            {
                const FGpuScopeResult& scope = profiler.GetScope(i);

                LIMX_LOG(LogLaunch, Log,
                         "[基准] GPU Pass {} — {} ms ({}%)",
                         (scope.Name != nullptr) ? scope.Name : "?",
                         scope.Milliseconds,
                         (frameMs > 0.0)
                             ? (scope.Milliseconds / frameMs * 100.0) : 0.0);
            }

            // 未埋点的部分 = 整帧 − 各 Pass 之和。
            //
            // 整帧是独立的一对时间戳, 不是各 Pass 相加, 所以这个差额是
            // 一个真实的测量结果而非恒等式。它偏大就说明有 GPU 工作没被
            // 任何作用域覆盖 —— 那正是"漏埋"的定义。
            const Float64 unaccounted = frameMs - sumMs;

            LIMX_LOG(LogLaunch, Log,
                     "[基准] GPU 整帧 {} ms | 各 Pass 之和 {} ms | "
                     "未埋点 {} ms ({}%) | 已回读 {} 帧",
                     frameMs, sumMs, unaccounted,
                     (frameMs > 0.0) ? (unaccounted / frameMs * 100.0) : 0.0,
                     profiler.GetResolvedFrameCount());
        }
    }

    // 两个口径都报。资产显存是关卡加载/卸载能控制的部分, 设备总量才是
    // 真实占用 —— 渲染目标、阴影贴图、IBL 的立方体贴图、逐帧 UBO 都不
    // 经资源管理器, 只看前者会以为显存已经归零。
    if (context != nullptr)
    {
        const FRenderResourceStats& assetStats =
            context->GetResourceManager().GetStats();

        LIMX_LOG(LogLaunch, Log,
                 "[基准] 资产显存: {} MiB (网格 {} 张 {} MiB | 纹理 {} 张 {} MiB)",
                 assetStats.GetTotalBytes() / (1024 * 1024),
                 assetStats.MeshCount, assetStats.MeshBytes / (1024 * 1024),
                 assetStats.TextureCount,
                 assetStats.TextureBytes / (1024 * 1024));

        if (context->GetDevice() != nullptr)
        {
            const FRHIDeviceMemoryStats deviceStats =
                context->GetDevice()->GetDeviceMemoryStats();

            LIMX_LOG(LogLaunch, Log,
                     "[基准] 设备显存: 占用 {} MiB / 申请 {} MiB | "
                     "分配数 {}/{}",
                     deviceStats.UsedBytes / (1024 * 1024),
                     deviceStats.ReservedBytes / (1024 * 1024),
                     deviceStats.AllocationCount,
                     deviceStats.AllocationLimit);

            // 引擎自身的常驻开销 = 设备总量 - 关卡资产。把它单独报出来,
            // 是因为它才是"加载一个空场景要花多少显存"的答案。
            const UInt64 engineBytes =
                (deviceStats.UsedBytes > assetStats.GetTotalBytes())
                    ? (deviceStats.UsedBytes - assetStats.GetTotalBytes())
                    : 0;

            LIMX_LOG(LogLaunch, Log,
                     "[基准] 其中引擎常驻 (渲染目标/阴影/IBL/UBO): {} MiB",
                     engineBytes / (1024 * 1024));
        }
    }

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
    // 必须在设备创建之前装上 —— 关闭阶段的 Error 要数, 启动阶段的也要
    FErrorCountingSink errorSink;
    FLog::AddSink(&errorSink);

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
    // 录制配置必须在 Initialize 之前 —— 命令池与次级缓冲区在那时一次性
    // 建好, 运行中改线程数意味着重建全部资源并等 GPU 空闲。
    renderer.SetRecordThreadCount(launchOptions.RecordThreads);
    renderer.SetParallelRecording(launchOptions.ParallelRecording);

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
    if (launchOptions.FurnaceCheck)
    {
        const bool   passed               = RunFurnaceSelfTest(&renderContext);
        const UInt32 errorsBeforeShutdown = errorSink.GetCount();

        LRegistry::Get().Destroy(scene);
        FSceneManager::Get().Shutdown();
        renderer.Shutdown();
        renderContext.Shutdown();
        window.Destroy();

        const int code =
            FinalizeSelfCheck(passed, 5, errorSink, errorsBeforeShutdown);

        FLog::RemoveSink(&fileLogSink);
        FLog::RemoveSink(&errorSink);
        fileLogSink.Close();

        return code;
    }

    if (launchOptions.ReloadTest)
    {
        const bool passed =
            RunReloadTest(&renderContext, &renderer, launchOptions);

        const UInt32 errorsBeforeShutdown = errorSink.GetCount();

        LRegistry::Get().Destroy(scene);
        FSceneManager::Get().Shutdown();
        renderer.Shutdown();
        renderContext.Shutdown();
        window.Destroy();

        const int code =
            FinalizeSelfCheck(passed, 4, errorSink, errorsBeforeShutdown);

        FLog::RemoveSink(&fileLogSink);
        FLog::RemoveSink(&errorSink);
        fileLogSink.Close();

        return code;
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
    else if (launchOptions.CornerScene)
    {
        BuildCornerScene(scene, &renderContext, &renderer);
    }
    else if (launchOptions.BloomScene)
    {
        BuildBloomScene(scene, &renderContext, &renderer);
    }
    else if (launchOptions.ShadowScene)
    {
        BuildShadowScene(scene, &renderContext, &renderer,
                         launchOptions.ShadowLights,
                         launchOptions.PointShadow);
    }
    else if (launchOptions.MaterialGrid > 0)
    {
        BuildMaterialGrid(scene, &renderContext, &renderer,
                          launchOptions.MaterialGrid);
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

    // 4d2. 截屏通路 —— 拷贝命令必须录进帧内 (交换链图像只在帧内归应用所有)
    FScreenshotCapture screenshotCapture;

    if (!launchOptions.ScreenshotPath.IsEmpty())
    {
        renderer.SetPostSceneRenderCallback(
            [&screenshotCapture, &renderContext]()
            {
                screenshotCapture.RecordCopy(&renderContext);
            });
    }

    // 4e. 相机朝向覆盖 —— 放在场景构建之后, 免得被场景导入的取景逻辑改回去
    if (launchOptions.OverrideCameraRotation)
    {
        renderer.GetCamera().SetRotation(launchOptions.CameraYaw,
                                         launchOptions.CameraPitch);

        LIMX_LOG(LogLaunch, Log,
                 "[Launch] 相机朝向已固定: yaw={} pitch={}",
                 launchOptions.CameraYaw, launchOptions.CameraPitch);
    }

    if (launchOptions.NoClustered)
    {
        renderer.SetClusteredLighting(false);

        LIMX_LOG(LogLaunch, Display,
                 "[Launch] 分簇光照已关闭 — 暴力遍历全部光源");
    }

    if (launchOptions.Gtao)
    {
        renderer.SetGtaoEnabled(true);

        if (renderer.GetGtaoPass() != nullptr)
        {
            renderer.GetGtaoPass()->SetRadius(launchOptions.AoRadius);
        }

        LIMX_LOG(LogLaunch, Display,
                 "[Launch] 屏幕空间环境光遮蔽已启用 "
                 "(GTAO, 4 方向 x 8 步进, 半径 {})",
                 launchOptions.AoRadius);
    }

    if (launchOptions.Bloom)
    {
        if (renderer.GetBloomPass() != nullptr)
        {
            renderer.GetBloomPass()->SetThreshold(launchOptions.BloomThreshold);
            renderer.GetBloomPass()->SetIntensity(launchOptions.BloomIntensity);
        }

        renderer.SetBloomEnabled(true);

        LIMX_LOG(LogLaunch, Display,
                 "[Launch] 泛光已启用 (阈值 {}, 强度 {}, 6 级链)",
                 launchOptions.BloomThreshold, launchOptions.BloomIntensity);
    }

    if (launchOptions.TemporalAA)
    {
        renderer.SetTaaEnabled(true);

        LIMX_LOG(LogLaunch, Display,
                 "[Launch] 时域抗锯齿已启用 (Halton 2,3 抖动 + 方差裁剪解析)");
    }

    if (launchOptions.GpuDriven)
    {
        FGpuCullPass* const cull = renderer.GetGpuCullPass();

        if (cull == nullptr)
        {
            LIMX_LOG(LogLaunch, Error, "[Launch] GPU 驱动通道不存在");
        }
        else if (!cull->IsSupported())
        {
            // 不支持时明确报出来而不是悄悄退回。
            //
            // 悄悄退回的表现是"开了 --gpu-driven 却一点也没变快", 而那与
            // "GPU 驱动本来就没用"分不开 —— 后者是个完全错误的结论。
            LIMX_LOG(LogLaunch, Error,
                     "[Launch] 本设备不支持 drawIndirectFirstInstance — "
                     "GPU 驱动路径不可用, 仍走逐物体绘制");
        }
        else
        {
            cull->SetEnabled(true);

            LIMX_LOG(LogLaunch, Display,
                     "[Launch] GPU 驱动的剔除与间接绘制已启用 (相机通道)");
        }
    }

    // 4f. 环境贴图 — 必须在渲染器初始化之后 (天空 Pass 的描述符集才存在)
    //
    // 声明在主循环之外: 它拥有立方体贴图的显存, 一旦析构天空 Pass 的
    // 描述符集就会指向已释放的图像。作用域必须覆盖整个渲染期。
    FEnvironmentMap environmentMap;

    if (launchOptions.Furnace)
    {
        // 直接光会叠加在 IBL 之上, 使"物体是否等于背景"不再成立
        FLightManager::Get().ClearAllLights();

        const FImageData furnace = BuildFurnaceEnvironment();

        if (IsRHISuccess(environmentMap.BuildFromEquirect(&renderContext,
                                                          furnace)))
        {
            renderer.SetEnvironmentMap(&environmentMap);
            renderer.SetSkyIntensity(1.0f);
            renderer.SetIblIntensity(1.0f);

            RunFurnaceChecks(environmentMap, &renderContext);

            LIMX_LOG(LogLaunch, Display,
                     "[白炉] 环境就绪 —— 各方向辐射度恒为 1, 直接光已关闭");
        }
        else
        {
            LIMX_LOG(LogLaunch, Error, "[白炉] 合成环境构建失败");
        }
    }
    else if (!launchOptions.HdriPath.IsEmpty())
    {
        LoadEnvironmentMap(environmentMap, &renderContext, &renderer,
                           launchOptions);
    }

    // 光源阵列 —— 叠加在已构建的场景之上, 不进上面那条互斥的分派链
    if (launchOptions.LightGrid > 0)
    {
        BuildLightGrid(launchOptions.LightGrid);
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

    // 自检结果。初值为 true 但只在 --gbuffer-check 打开时才会被真正赋值,
    // 而退出码那里也判了同一个开关 —— 没开自检时它不参与任何判断。
    bool    gbufferCheckPassed = true;
    bool    clusterCheckPassed = true;
    bool    lightCullCheckPassed = true;
    bool    taaCheckPassed = true;
    bool    aoCheckPassed = true;
    bool    bloomCheckPassed = true;
    bool    shadowCheckPassed = true;
    bool    gpuDrivenCheckPassed = true;

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

        // 强制重建交换链 —— 让 OnResize 那条路径被回归覆盖。
        //
        // 放在 RenderFrame 之后而不是之前: 重建内部会 WaitIdle, 此刻上一帧
        // 已经提交, 是最安全的时机。
        if (launchOptions.ResizeEveryFrames > 0 &&
            (loopFrame % launchOptions.ResizeEveryFrames) == 0)
        {
            if (!renderer.ForceRecreateSwapchain())
            {
                LIMX_LOG(LogLaunch, Error,
                         "[Launch] 第 {} 帧的交换链重建失败", loopFrame);
                return 6;
            }
        }

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
            LogBenchmarkReport(launchOptions, renderer, &renderContext);

            if (launchOptions.ClusterCheck)
            {
                clusterCheckPassed = RunClusterChecks(&renderContext, renderer);
            }

            if (launchOptions.LightCullCheck)
            {
                lightCullCheckPassed =
                    RunLightCullChecks(&renderContext, renderer);
            }

            if (launchOptions.AoCheck)
            {
                aoCheckPassed = RunAoChecks(&renderContext, renderer);
            }

            if (launchOptions.BloomCheck)
            {
                bloomCheckPassed = RunBloomChecks(&renderContext, renderer);
            }

            if (launchOptions.GpuDrivenCheck)
            {
                gpuDrivenCheckPassed =
                    RunGpuDrivenChecks(&renderContext, renderer);
            }

            if (launchOptions.ShadowCheck && launchOptions.PointShadow)
            {
                shadowCheckPassed =
                    RunPointShadowChecks(&renderContext, renderer);
            }
            else if (launchOptions.ShadowCheck)
            {
                shadowCheckPassed = RunShadowChecks(
                    &renderContext, renderer,
                    FMath::Min(FMath::Clamp(
                                   launchOptions.ShadowLights, 2u,
                                   static_cast<UInt32>(kShadowTileCount) * 2u),
                               static_cast<UInt32>(kShadowTileCount)));
            }

            if (launchOptions.TaaCheck)
            {
                taaCheckPassed = RunTaaChecks(&renderContext, renderer);
            }

            // G-Buffer 自检: 会自己再渲三帧并改动相机朝向, 所以必须放在
            // 截屏之前 —— 否则截到的是自检最后那一帧的朝向。
            if (launchOptions.GBufferCheck)
            {
                gbufferCheckPassed =
                    RunGBufferChecks(&renderContext, renderer, scene);
            }

            // 截屏: 再渲一帧, 这一帧的命令缓冲区里带上拷贝命令
            if (!launchOptions.ScreenshotPath.IsEmpty() &&
                screenshotCapture.Request(&renderContext))
            {
                scene->Tick(0.0f);
                FSceneManager::Get().SyncScene(scene, 0.0f);
                renderer.RenderFrame();

                screenshotCapture.WriteFile(&renderContext,
                                            launchOptions.ScreenshotPath);
            }

            break;
        }
    }

    // ================================================================
    // 6. 关闭 (逆序: 场景→桥接→渲染器→上下文→窗口)
    // ================================================================

    // 关闭之前记下 Error 计数 —— 之后新增的都算在关闭阶段头上
    const UInt32 errorsBeforeShutdown = errorSink.GetCount();

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

    // 6d. 释放环境贴图 —— 必须在渲染器与上下文关闭之前显式做
    //
    // 靠析构顺序在这里是不够的: environmentMap 是 main 的局部变量, 它的
    // 析构发生在函数末尾, 那时 renderContext.Shutdown() 早已销毁了设备,
    // 释放纹理会踩到已析构的对象。先解绑再释放, 顺序与创建严格相反。
    //
    // WaitIdle 必须在释放之前: 最后几帧的命令缓冲区可能仍在执行, 而它们
    // 引用着这里的采样器与图像。渲染器的 Shutdown 里也有一次 WaitIdle,
    // 但那已经晚了。
    if (renderContext.GetDevice() != nullptr)
    {
        renderContext.GetDevice()->WaitIdle();
    }

    renderer.SetEnvironmentMap(nullptr);
    environmentMap.Release();
    screenshotCapture.Release(renderContext.GetDevice());

    // 6e. 关闭渲染器和 GPU 资源
    renderer.Shutdown();
    renderContext.Shutdown();
    window.Destroy();

    LIMX_LOG(LogLaunch, Log,
        "[Launch] Limx Engine 已关闭");

    // 自检的判定必须放在**全部关闭之后**。
    //
    // 这一点看着无所谓, 其实是本项目栽过的坑: 判定值若在关闭之前就算完并
    // 直接 return, 那么关闭阶段才发现的问题 (显存泄漏、资源未回收) 永远
    // 影响不到退出码, 而它们只在日志里留一行 Error —— CI 看的是退出码。
    int selfCheckCode = 0;

    if (launchOptions.GBufferCheck)
    {
        selfCheckCode = FinalizeSelfCheck(gbufferCheckPassed, 7, errorSink,
                                          errorsBeforeShutdown);
    }

    if (selfCheckCode == 0 && launchOptions.ClusterCheck)
    {
        selfCheckCode = FinalizeSelfCheck(clusterCheckPassed, 9, errorSink,
                                          errorsBeforeShutdown);
    }

    if (selfCheckCode == 0 && launchOptions.LightCullCheck)
    {
        selfCheckCode = FinalizeSelfCheck(lightCullCheckPassed, 10, errorSink,
                                          errorsBeforeShutdown);
    }

    if (selfCheckCode == 0 && launchOptions.TaaCheck)
    {
        selfCheckCode = FinalizeSelfCheck(taaCheckPassed, 11, errorSink,
                                          errorsBeforeShutdown);
    }

    if (selfCheckCode == 0 && launchOptions.AoCheck)
    {
        selfCheckCode = FinalizeSelfCheck(aoCheckPassed, 12, errorSink,
                                          errorsBeforeShutdown);
    }

    if (selfCheckCode == 0 && launchOptions.BloomCheck)
    {
        selfCheckCode = FinalizeSelfCheck(bloomCheckPassed, 13, errorSink,
                                          errorsBeforeShutdown);
    }

    if (selfCheckCode == 0 && launchOptions.ShadowCheck)
    {
        selfCheckCode = FinalizeSelfCheck(shadowCheckPassed, 14, errorSink,
                                          errorsBeforeShutdown);
    }

    if (selfCheckCode == 0 && launchOptions.GpuDrivenCheck)
    {
        selfCheckCode = FinalizeSelfCheck(gpuDrivenCheckPassed, 15, errorSink,
                                          errorsBeforeShutdown);
    }

    // 移除日志 Sink 并关闭
    FLog::RemoveSink(&fileLogSink);
    FLog::RemoveSink(&errorSink);
    fileLogSink.Close();

    return selfCheckCode;
}
