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
#include "RayTracingCheck.h"
#include "PathTraceCheck.h"
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
#include "Renderer/RenderPass/FMeshletCullPass.h"
#include "Renderer/RenderPass/FMeshletDepthPass.h"
#include "Renderer/RenderPass/FRayTracedShadowPass.h"
#include "Renderer/RenderPass/FRayTracedAoPass.h"
#include "Renderer/RenderPass/FRayTracedReflectionPass.h"
#include "RenderCore/Geometry/FGeometryGenerator.h"
#include "RenderCore/Geometry/FMeshletBuilder.h"
#include "RenderCore/Geometry/FMeshSimplifier.h"
#include "RenderCore/Geometry/FMeshletGrouper.h"
#include "RenderCore/Geometry/FMeshLodDag.h"
#include "AssetPipeline/FObjLoader.h"
#include "Core/Containers/TSortAlgorithms.h"

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

    /// 构建综合示例场景 (三种光源类型 + 四类材质 + 全部后处理)
    bool ShowcaseScene = false;

    /// 综合自检: 逐个子系统断言"它到底跑没跑", 以退出码报告
    bool ShowcaseCheck = false;

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

    /// GTAO 在半分辨率上求解 + 双边上采样
    bool GtaoHalf = false;

    /// 半分辨率 AO 自检: 与全分辨率逐像素比对, 以退出码报告
    bool AoHalfCheck = false;

    /// 光追自检: 加速结构的 GPU 遍历与 CPU 解析解逐条比对, 以退出码报告
    bool RayTracingCheck = false;

    /// AO 边缘自检: 双边上采样在深度不连续处有没有起作用, 以退出码报告
    bool AoEdgeCheck = false;

    /// 启用光追阴影 (替代阴影贴图)
    bool RayTracedShadows = false;

    /// 光追阴影自检: 边界与相似三角形的解析位置比对, 以退出码报告
    bool RtShadowCheck = false;

    /// 启用光追环境光遮蔽
    bool RayTracedAo = false;

    /// 光追 AO 在半分辨率上求解 + 双边上采样
    bool RayTracedAoHalf = false;

    /// 光追 AO 自检: 与直角凹角的闭式解逐像素比对, 以退出码报告
    bool RtAoCheck = false;

    /// 光追 AO 的自遮挡自检: 半径极小时 AO 必须处处为 1 (与场景无关)
    bool RtAoSelfCheck = false;

    /// 隐藏窗口 —— 自检与变异验证一轮要跑几十次, 每次弹一个窗口很难受
    ///
    /// 只是不 ShowWindow, 交换链、渲染、回读一切照旧。判据量到的**数值**
    /// 与可见时逐位相同 (实测: 光追 AO 的 39236 个像素、均值 0.801607,
    /// 两种模式下完全一致)。
    ///
    /// 不是"离屏渲染": 那需要另一条不带交换链的路径, 而那条路径与真实渲染
    /// 的差别恰恰是判据最不该引入的东西。
    ///
    /// **但性能数字不可比。** 窗口不可见时合成器不再取用交换链图像, 呈现
    /// 那一段的开销跟着变 —— 实测同一场景 GPU 整帧 0.70 ms (隐藏) 对
    /// 1.02 ms (可见), 最差帧 8.2 对 4.0。跑基准要用可见窗口。
    bool HiddenWindow = false;

    /// 启用光追反射
    bool RayTracedReflection = false;

    /// 光追反射自检: 命中距离/材质下标/命中法线三个量对解析值, 以退出码报告
    bool RtReflectionCheck = false;

    /// 光追反射的与场景无关自洽检查 (可跑在有子网格的场景上)
    bool RtReflectionSelfCheck = false;

    /// 几何表按源对象下标索引 (不是实例序号) —— 见函数头
    bool RtGeometryTableCheck = false;

    /// 光追产出的图有没有到达画面 —— 见函数头
    bool RtHybridCheck = false;

    /// 双边上采样在深度不连续处不渗色 —— 见函数头
    bool RtAoUpsampleCheck = false;

    /// meshlet 切分无损 —— 见函数头
    bool MeshletCheck = false;

    /// 两级 meshlet 剔除
    bool MeshletCull = false;

    /// 两级剔除与 CPU 参考实现逐个 meshlet 一致 —— 见函数头
    bool MeshletCullCheck = false;

    /// meshlet 深度光栅化 (网格着色器路径)
    bool MeshletDepth = false;

    /// 强制走计算展开的回退路径
    bool MeshletDepthFallback = false;

    /// 两条光栅化路径 + 与经典深度的关系 —— 见函数头
    bool MeshletDepthCheck = false;

    /// 材质解析 (从可见性编号反解属性)
    bool MeshletResolve = false;

    /// 材质解析的判据 —— 见函数头
    bool MeshletResolveCheck = false;

    /// 两阶段遮挡剔除
    bool MeshletOcclusion = false;

    /// 遮挡剔除的判据 —— 见函数头
    bool MeshletOcclusionCheck = false;
    bool MeshletScaleCheck = false;
    bool GpuCullOverflowCheck = false;

    /// 展开顶点流溢出的判据 —— 见 RunMeshletExpandOverflowChecks
    bool MeshletExpandOverflowCheck = false;

    bool MeshSimplifyCheck = false;
    bool MeshletGroupCheck = false;
    bool LodDagCheck = false;
    bool LodSelectCheck = false;
    bool LodCrackCheck = false;
    bool LodGpuCheck = false;

    /// 命令行里有认不出来的参数
    bool UnknownArgument = false;

    /// 离线参考路径追踪器的判据 —— 白炉、能量守恒、方差标度, 以退出码报告
    ///
    /// 它跑的是自己程序生成的场景 (白炉需要可控的环境), 与命令行里的
    /// 其它场景开关无关 —— 但仍然要 --frames, 因为判据统一在帧循环结束
    /// 之后跑, 而窗口与设备要先起来。
    bool PathTraceCheck = false;

    /// 离线渲一张 Cornell 盒参考图并写成二进制 PPM
    FString PathTraceImagePath;

    /// 参考图的分辨率与每像素样本数
    UInt32 PathTraceImageSize = 512;
    UInt32 PathTraceImageSamples = 4096;

    /// 光追深度自检: 光追深度与光栅化深度逐像素比对, 以退出码报告
    bool RtDepthCheck = false;

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
///   --gtao-half      GTAO 在半分辨率上求解 + 双边上采样
///   --ao-half-check  半分辨率 AO 自检: 与全分辨率逐像素比对, 以退出码报告
///   --bloom          启用泛光
///   --bloom-threshold T  泛光的亮度阈值 (默认 1.0)
///   --bloom-intensity I  泛光的合成强度 (默认 0.05)
///   --corner-scene   构建直角墙角场景 (GTAO 自检的解析基准)
///   --bloom-scene    构建单点自发光场景 (泛光自检的点扩散基准)
///   --bloom-check    泛光自检: 点扩散的对称性与能量守恒, 以退出码报告
///   --showcase       构建综合示例场景 (三种光源类型 + 四类材质)
///   --showcase-check 综合自检: 逐个子系统断言"它到底跑没跑", 以退出码报告
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
///   --path-trace-check 离线参考路径追踪器自检: 白炉 + 能量守恒 + 方差标度
///   --path-trace-image P 离线渲一张 Cornell 盒参考图写入 P (二进制 PPM)
///   --path-trace-size N  参考图边长 (默认 512)
///   --path-trace-spp N   参考图每像素样本数 (默认 4096)
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
        else if (WideEquals(arg, L"--clustered"))
        {
            // 显式打开分簇 —— 默认就是开的, 所以它不改变行为。
            //
            // 留着它不是为了"以防万一": verify.ps1 有九步在传这个参数, 用来
            // 表明"这一步验的是分簇路径"。而在加上下面那条"未知参数直接失败"
            // 之前, 它是被**静默吞掉**的 —— 也就是说那九步写了一个不存在的
            // 开关而没人知道。今天恰好无害 (默认为真), 换个默认值就是九步
            // 集体验错了东西。
            options.NoClustered = false;
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
        else if (WideEquals(arg, L"--showcase"))
        {
            options.ShowcaseScene = true;
        }
        else if (WideEquals(arg, L"--showcase-check"))
        {
            options.ShowcaseCheck = true;
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
        else if (WideEquals(arg, L"--gtao-half"))
        {
            options.GtaoHalf = true;
        }
        else if (WideEquals(arg, L"--ao-half-check"))
        {
            options.AoHalfCheck = true;
        }
        else if (WideEquals(arg, L"--rt-check"))
        {
            options.RayTracingCheck = true;
        }
        else if (WideEquals(arg, L"--ao-edge-check"))
        {
            options.AoEdgeCheck = true;
        }
        else if (WideEquals(arg, L"--rt-shadows"))
        {
            options.RayTracedShadows = true;
        }
        else if (WideEquals(arg, L"--rt-shadow-check"))
        {
            options.RtShadowCheck = true;
        }
        else if (WideEquals(arg, L"--rt-ao"))
        {
            options.RayTracedAo = true;
        }
        else if (WideEquals(arg, L"--rt-ao-half"))
        {
            options.RayTracedAo     = true;
            options.RayTracedAoHalf = true;
        }
        else if (WideEquals(arg, L"--rt-ao-check"))
        {
            options.RtAoCheck = true;
        }
        else if (WideEquals(arg, L"--rt-ao-self"))
        {
            // 这条自检**不进流水线** —— 见 RunRayTracedAoSelfCheck 头上
            // 那段: 试了三种统计量, 没有一种能把"偏移不够"与"真实的接触
            // 遮挡"分开。留着是为了让下一个人不必从头试一遍。
            options.RtAoSelfCheck = true;
        }
        else if (WideEquals(arg, L"--hidden"))
        {
            options.HiddenWindow = true;
        }
        else if (WideEquals(arg, L"--rt-reflection"))
        {
            options.RayTracedReflection = true;
        }
        else if (WideEquals(arg, L"--rt-reflection-check"))
        {
            options.RtReflectionCheck = true;
        }
        else if (WideEquals(arg, L"--rt-geometry-table-check"))
        {
            options.RtGeometryTableCheck = true;
        }
        else if (WideEquals(arg, L"--rt-hybrid-check"))
        {
            options.RtHybridCheck = true;
        }
        else if (WideEquals(arg, L"--rt-ao-upsample-check"))
        {
            options.RtAoUpsampleCheck = true;
        }
        else if (WideEquals(arg, L"--meshlet-check"))
        {
            options.MeshletCheck = true;
        }
        else if (WideEquals(arg, L"--meshlet-cull"))
        {
            options.MeshletCull = true;
        }
        else if (WideEquals(arg, L"--meshlet-cull-check"))
        {
            options.MeshletCullCheck = true;
        }
        else if (WideEquals(arg, L"--meshlet-depth"))
        {
            options.MeshletDepth = true;
        }
        else if (WideEquals(arg, L"--meshlet-depth-fallback"))
        {
            options.MeshletDepth         = true;
            options.MeshletDepthFallback = true;
        }
        else if (WideEquals(arg, L"--meshlet-depth-check"))
        {
            options.MeshletDepthCheck = true;
        }
        else if (WideEquals(arg, L"--meshlet-resolve"))
        {
            options.MeshletDepth   = true;
            options.MeshletResolve = true;
        }
        else if (WideEquals(arg, L"--meshlet-resolve-check"))
        {
            options.MeshletResolveCheck = true;
        }
        else if (WideEquals(arg, L"--meshlet-occlusion"))
        {
            options.MeshletDepth     = true;
            options.MeshletOcclusion = true;
        }
        else if (WideEquals(arg, L"--meshlet-occlusion-check"))
        {
            options.MeshletOcclusionCheck = true;
        }
        else if (WideEquals(arg, L"--meshlet-scale-check"))
        {
            options.MeshletScaleCheck = true;
        }
        else if (WideEquals(arg, L"--gpu-cull-overflow-check"))
        {
            options.GpuCullOverflowCheck = true;
        }
        else if (WideEquals(arg, L"--meshlet-expand-overflow-check"))
        {
            options.MeshletExpandOverflowCheck = true;
        }
        else if (WideEquals(arg, L"--mesh-simplify-check"))
        {
            options.MeshSimplifyCheck = true;
        }
        else if (WideEquals(arg, L"--meshlet-group-check"))
        {
            options.MeshletGroupCheck = true;
        }
        else if (WideEquals(arg, L"--lod-dag-check"))
        {
            options.LodDagCheck = true;
        }
        else if (WideEquals(arg, L"--lod-select-check"))
        {
            options.LodSelectCheck = true;
        }
        else if (WideEquals(arg, L"--lod-crack-check"))
        {
            options.LodCrackCheck = true;
        }
        else if (WideEquals(arg, L"--lod-gpu-check"))
        {
            options.LodGpuCheck = true;
        }
        else if (WideEquals(arg, L"--path-trace-check"))
        {
            options.PathTraceCheck = true;
        }
        else if (WideEquals(arg, L"--path-trace-image") &&
                 (i + 1) < tokenCount)
        {
            options.PathTraceImagePath = WideToString(tokens[++i]);
        }
        else if (WideEquals(arg, L"--path-trace-size") && (i + 1) < tokenCount)
        {
            options.PathTraceImageSize = ParseUInt32(tokens[++i], 512);
        }
        else if (WideEquals(arg, L"--path-trace-spp") && (i + 1) < tokenCount)
        {
            options.PathTraceImageSamples = ParseUInt32(tokens[++i], 4096);
        }
        else if (WideEquals(arg, L"--rt-reflection-self"))
        {
            options.RtReflectionSelfCheck = true;
        }
        else if (WideEquals(arg, L"--rt-depth-check"))
        {
            options.RtDepthCheck = true;
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
        else
        {
            // 未知参数**直接失败**, 不许静默吞掉。
            //
            // 在此之前这个循环没有 else。后果是 verify.ps1 里九步传的
            // `--clustered` (那时根本不是一个开关) 一路无声地被丢掉 —— 那九步
            // 自以为在验分簇路径, 而实际上只是碰巧默认为真。
            //
            // 参数拼错的表现与此完全相同: 判据照跑, 只是验的不是你以为的
            // 那个配置。而那种绿是最没有价值的绿。
            options.UnknownArgument = true;

            LIMX_LOG(LogLaunch, Error,
                     "[Launch] 未知的命令行参数 —— 拼错的参数会被当成没写, "
                     "判据照跑而验的不是你以为的那个配置");
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

    // 验证层的 Error 并进判定。
    //
    // 它与应用自己打的 Error 不是一回事: 判据会**故意**制造溢出、故意让某一步
    // 失败, 那些 Error 是预期之内的; 而验证层报的从来不是。
    //
    // 在此之前, 被测那些帧里的验证层报错一条都不影响退出码 —— 自检只看各条
    // 判据自己的 passed, 而"关闭阶段的 Error"是全部自检跑完之后才开始数的。
    // 渲染过程中的同步错误、布局错误、越界写, 只要没有哪条判据恰好看得见,
    // 就一路绿着过去。
    // 丢设备优先报 —— 它比验证层的 Error 更严重, 而且它同样从来不是故意的。
    //
    // 在此之前"跑五次都退出 0"这种判据对它是**瞎的**: 日志里躺着
    // `vkQueueSubmit 失败: -4` 和八帧 `WaitForFence 失败`, 而进程返回 0。
    // 查那个 grid 127 的丢设备时, 我是靠 grep 日志判定的 —— 而那说明退出码
    // 本身没有把它算进去。
    if (GetDeviceLostCount() > 0)
    {
        LIMX_LOG(LogLaunch, Display,
                 "[自检] 自检项全部通过, 但运行期丢过 {} 次设备 —— 判定为失败",
                 GetDeviceLostCount());
        return 10;
    }

    const UInt32 validationErrors = GetValidationErrorCount();

    if (validationErrors > 0)
    {
        LIMX_LOG(LogLaunch, Display,
                 "[自检] 自检项全部通过, 但验证层报了 {} 条 Error —— "
                 "判定为失败。验证层的 Error 从来不是故意的",
                 validationErrors);
        return 9;
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

// ============================================================================
// RunAoHalfChecks — 半分辨率 AO 与全分辨率的逐像素比对
//
// 三条判据, 缺一不可:
//
//   1. **平均差要小。** 半分辨率是个近似, 但它必须仍然是同一张图。
//   2. **平均差不能为零。** 这一条防的是"开关没接上": 一个把 --gtao-half
//      忽略掉的实现会得到两张完全相同的图, 而第一条判据对它满分通过 ——
//      于是"半分辨率没生效"与"半分辨率完美无损"在判据上无法区分。
//   3. **最大差要有界。** 平均差小掩盖得住局部的严重偏差: 深度不连续处若
//      双边退化成双线性, 前景与背景的 AO 会混在一起, 那是几个百分点的像素
//      上几十个百分点的误差 —— 平均下来看不见。
//
// 只在墙角场景上跑。那个场景有清晰的法线不连续 (直角) 与轮廓处的深度不连续
// (物体与远平面), 两种上采样会出错的地方它都有。
// ============================================================================

namespace
{

/// 回读全分辨率 AO (R16_SFLOAT) 与深度 (D32_SFLOAT)
///
/// 深度是判据的一半: 双边上采样的**全部作用**都在深度不连续处 —— 平坦区域
/// 它与双线性给出的结果完全一样。整幅平均因此量不出它有没有生效 (实测把它
/// 退化成双线性, 平均差从 0.014394 变成 0.014394, 小数点后五位都一样)。
static bool ReadAoAndDepth(FRenderContext* context, FRenderer& renderer,
                           TArray<Float32>& outAo,
                           TArray<Float32>& outDepth)
{
    FGtaoPass* const     gtao  = renderer.GetGtaoPass();
    FDepthPrePass* const depth = renderer.GetDepthPrePass();

    if (gtao == nullptr || depth == nullptr)
    {
        return false;
    }

    IRHIDevice* const device = context->GetDevice();

    const FRHIExtent2D extent = context->GetSwapchainExtent();

    const SizeType pixelCount =
        static_cast<SizeType>(extent.Width) * extent.Height;

    FRHIBufferHandle aoReadback;
    FRHIBufferHandle depthReadback;

    {
        FRHIBufferDesc desc = {};
        desc.Usage       = EBufferUsage::TransferDst;
        desc.MemoryUsage = EMemoryUsage::GpuToCpu;

        desc.Size      = pixelCount * 2u;   // R16_SFLOAT
        desc.DebugName = "AoHalfCheck.AO";

        if (!IsRHISuccess(device->CreateBuffer(desc, aoReadback)))
        {
            return false;
        }

        desc.Size      = pixelCount * 4u;   // D32_SFLOAT
        desc.DebugName = "AoHalfCheck.Depth";

        if (!IsRHISuccess(device->CreateBuffer(desc, depthReadback)))
        {
            device->DestroyBuffer(aoReadback);
            return false;
        }
    }

    const FRHITextureHandle aoTexture    = gtao->GetAoTexture();
    const FRHITextureHandle depthTexture = depth->GetSharedDepthTexture();

    bool recorded = false;

    renderer.SetPostSceneRenderCallback(
        [&recorded, context, aoTexture, depthTexture, aoReadback,
         depthReadback, extent]()
        {
            IRHICommandBuffer* cmd = context->GetCurrentCommandBuffer();

            if (cmd == nullptr)
            {
                return;
            }

            FRHIBufferTextureCopyRegion region = {};
            region.BufferOffset      = 0;
            region.BufferRowLength   = 0;
            region.BufferImageHeight = 0;
            region.MipLevel          = 0;
            region.BaseLayer         = 0;
            region.LayerCount        = 1;
            region.TextureOffset     = { 0, 0, 0 };
            region.TextureExtent     = { extent.Width, extent.Height, 1 };

            cmd->TransitionImageLayout(
                aoTexture,
                EImageLayout::ShaderReadOnly, EImageLayout::TransferSrc,
                EPipelineStageFlags::FragmentShader,
                EPipelineStageFlags::Transfer,
                EAccessFlags::ShaderRead, EAccessFlags::TransferRead);

            cmd->CopyTextureToBuffer(aoTexture, EImageLayout::TransferSrc,
                                     aoReadback, region);

            cmd->TransitionImageLayout(
                aoTexture,
                EImageLayout::TransferSrc, EImageLayout::ShaderReadOnly,
                EPipelineStageFlags::Transfer,
                EPipelineStageFlags::FragmentShader,
                EAccessFlags::TransferRead, EAccessFlags::ShaderRead);

            // 深度此刻停在 DepthStencilAttachment —— 前向通道的 FinalLayout
            cmd->TransitionImageLayout(
                depthTexture,
                EImageLayout::DepthStencilAttachment,
                EImageLayout::TransferSrc,
                EPipelineStageFlags::LateFragmentTests,
                EPipelineStageFlags::Transfer,
                EAccessFlags::DepthStencilAttachmentWrite,
                EAccessFlags::TransferRead);

            cmd->CopyTextureToBuffer(depthTexture, EImageLayout::TransferSrc,
                                     depthReadback, region);

            cmd->TransitionImageLayout(
                depthTexture,
                EImageLayout::TransferSrc,
                EImageLayout::DepthStencilAttachment,
                EPipelineStageFlags::Transfer,
                EPipelineStageFlags::EarlyFragmentTests,
                EAccessFlags::TransferRead,
                EAccessFlags::DepthStencilAttachmentWrite);

            recorded = true;
        });

    renderer.RenderFrame();

    // 立刻摘掉回调 —— 捕获的是栈上的引用
    renderer.SetPostSceneRenderCallback(TFunction<void()>());

    bool ok = recorded;

    if (ok)
    {
        device->WaitIdle();

        void* mapped = nullptr;

        if (IsRHISuccess(device->MapBuffer(aoReadback, &mapped)) &&
            mapped != nullptr)
        {
            const Float16Bits* src = static_cast<const Float16Bits*>(mapped);

            outAo.Clear();
            outAo.Reserve(pixelCount);

            for (SizeType i = 0; i < pixelCount; ++i)
            {
                outAo.Add(Float16ToFloat32(src[i]));
            }

            device->UnmapBuffer(aoReadback);
        }
        else
        {
            ok = false;
        }
    }

    if (ok)
    {
        void* mapped = nullptr;

        if (IsRHISuccess(device->MapBuffer(depthReadback, &mapped)) &&
            mapped != nullptr)
        {
            const Float32* src = static_cast<const Float32*>(mapped);

            outDepth.Clear();
            outDepth.Reserve(pixelCount);

            for (SizeType i = 0; i < pixelCount; ++i)
            {
                outDepth.Add(src[i]);
            }

            device->UnmapBuffer(depthReadback);
        }
        else
        {
            ok = false;
        }
    }

    device->DestroyBuffer(aoReadback);
    device->DestroyBuffer(depthReadback);

    return ok;
}

// ============================================================================
// 深度不连续处的统计
//
// 双边上采样的全部作用都在深度不连续处 —— 平坦区域上双边、双线性、最近邻
// 给的是同一个数。所以要验它, 统计范围必须先缩到不连续处, 否则信号会被
// 几十万个平坦像素摊平到小数点后五位。
// ============================================================================

struct FAoEdgeStats
{
    /// 深度不连续的像素数
    SizeType EdgeCount = 0;

    /// 其中半分辨率与全分辨率相差超过 kBleedThreshold 的
    ///
    /// 渗色的特征不在均值上而在**尾巴**上: 上采样在不连续处把前景与背景的
    /// AO 混在一起, 产生的是少数像素上的大偏差。均值把它摊平, 计数不会。
    SizeType BleedCount = 0;

    /// 不连续处的平均差
    Float32 EdgeMean = 0.0f;

    /// **平坦区**的像素数 (不在深度不连续处的)
    SizeType SmoothCount = 0;

    /// 平坦区的平均差 —— **只报不判**
    ///
    /// 这一项本来是想抓"太锐"那一头的: 渗色计数抓的是上采样太糊 (把前景
    /// 背景混在一起), 而权重退化成只剩最近邻是相反的错法, 表现为平坦的
    /// AO 梯度上出现台阶。
    ///
    /// 实测的结论是**它抓不住**, 而且方向还反了 (7236 个真实不连续像素):
    ///
    ///     双边 (正确)          平坦区 0.028436   渗色  919
    ///     退化成最近邻          平坦区 0.028291   渗色  918
    ///     去掉双线性因子        平坦区 0.028010   渗色  905
    ///     纯双线性             平坦区 0.029760   渗色 1361
    ///     加权反向             平坦区 0.032821   渗色 1801
    ///
    /// 前三行彼此的差别在千分之四以内, 而"错"的那两行反而比正确的低。
    ///
    /// 原因是这个比对的**目标本身是锐的**: 半分辨率 AO 与全分辨率 AO 的
    /// 差别主要来自采样数减半, 不来自上采样滤波。最近邻上采样同样是锐的,
    /// 所以它与全分辨率的一致程度并不比双边差 —— 双边真正强过它的地方
    /// (表面内部平滑过渡) 恰好是两者都与全分辨率差不多的地方。
    ///
    /// 何况着色器的退路本来就是"四个邻居都不同表面时取最近邻", 所以在
    /// 不连续处双边与最近邻**按设计就该接近**。
    ///
    /// 也就是说: 手上的判据分得开"太糊", 分不开"太锐"。留着报数, 不判。
    Float32 SmoothMean = 0.0f;
};

/// 算作"渗色"的偏差门槛
///
/// AO 的取值域是 [0,1], 0.2 是肉眼一眼看得出的一档明暗。取得更小会把
/// 半分辨率固有的采样差也算进来 (那与上采样方式无关), 更大则只剩下
/// 前景背景完全颠倒的极端像素, 样本太少。
constexpr Float32 kAoBleedThreshold = 0.2f;

/// 判定"深度不连续"的相对深度差门槛
///
/// 0.002 的 NDC 相对差已经是很陡的一步了 —— 透视深度在远处极度压缩,
/// 用世界尺度的阈值会一个都选不出来。
constexpr Float32 kAoEdgeRelativeThreshold = 0.002f;

FAoEdgeStats ComputeAoEdgeStats(const TArray<Float32>& full,
                                const TArray<Float32>& half,
                                const TArray<Float32>& depth,
                                const FRHIExtent2D& extent)
{
    FAoEdgeStats stats;

    Float64 edgeSum   = 0.0;
    Float64 smoothSum = 0.0;

    for (UInt32 y = 1; y + 1 < extent.Height; ++y)
    {
        for (UInt32 x = 1; x + 1 < extent.Width; ++x)
        {
            const SizeType index =
                static_cast<SizeType>(y) * extent.Width + x;

            const Float32 center = depth[index];

            // 四邻的最大相对深度差 —— 与 gtao_upsample.frag 里的判据同源
            Float32 maxRelative = 0.0f;

            const SizeType neighbors[4] = {
                index - 1, index + 1,
                index - extent.Width, index + extent.Width
            };

            for (SizeType n = 0; n < 4; ++n)
            {
                const Float32 other = depth[neighbors[n]];

                const Float32 relative =
                    FMath::Abs(center - other) /
                    FMath::Max(FMath::Max(center, other), 1.0e-4f);

                maxRelative = FMath::Max(maxRelative, relative);
            }

            const Float32 diff = FMath::Abs(full[index] - half[index]);

            if (maxRelative > kAoEdgeRelativeThreshold)
            {
                edgeSum += static_cast<Float64>(diff);
                ++stats.EdgeCount;

                if (diff > kAoBleedThreshold)
                {
                    ++stats.BleedCount;
                }
            }
            else
            {
                smoothSum += static_cast<Float64>(diff);
                ++stats.SmoothCount;
            }
        }
    }

    if (stats.EdgeCount > 0)
    {
        stats.EdgeMean = static_cast<Float32>(
            edgeSum / static_cast<Float64>(stats.EdgeCount));
    }

    if (stats.SmoothCount > 0)
    {
        stats.SmoothMean = static_cast<Float32>(
            smoothSum / static_cast<Float64>(stats.SmoothCount));
    }

    return stats;
}

} // namespace

static bool RunAoHalfChecks(FRenderContext* context, FRenderer& renderer)
{
    FGtaoPass* const gtao = renderer.GetGtaoPass();

    if (gtao == nullptr || !gtao->IsEnabled())
    {
        LIMX_LOG(LogLaunch, Error,
                 "[AO半分] GTAO 未启用 — 自检无从判定 (加 --gtao)");
        return false;
    }

    const bool originalHalf = gtao->IsHalfResolution();

    TArray<Float32> full;
    TArray<Float32> half;
    TArray<Float32> depth;
    TArray<Float32> depthIgnored;

    gtao->SetHalfResolution(false);

    if (!ReadAoAndDepth(context, renderer, full, depth))
    {
        gtao->SetHalfResolution(originalHalf);
        LIMX_LOG(LogLaunch, Error, "[AO半分] 全分辨率回读失败");
        return false;
    }

    gtao->SetHalfResolution(true);

    if (!ReadAoAndDepth(context, renderer, half, depthIgnored))
    {
        gtao->SetHalfResolution(originalHalf);
        LIMX_LOG(LogLaunch, Error, "[AO半分] 半分辨率回读失败");
        return false;
    }

    gtao->SetHalfResolution(originalHalf);

    if (full.GetSize() != half.GetSize() || full.GetSize() == 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[AO半分] 两次回读的尺寸不同 ({} vs {})",
                 full.GetSize(), half.GetSize());
        return false;
    }

    Float64 sumAbs   = 0.0;
    Float32 maxAbs   = 0.0f;
    SizeType shaded  = 0;   // AO 明显小于 1 的像素数

    for (SizeType i = 0; i < full.GetSize(); ++i)
    {
        const Float32 diff = FMath::Abs(full[i] - half[i]);

        sumAbs += static_cast<Float64>(diff);
        maxAbs  = FMath::Max(maxAbs, diff);

        if (full[i] < 0.95f)
        {
            ++shaded;
        }
    }

    const Float32 meanAbs = static_cast<Float32>(
        sumAbs / static_cast<Float64>(full.GetSize()));

    LIMX_LOG(LogLaunch, Display,
             "[AO半分] 平均差 {} 最大差 {} (有遮蔽的像素 {} / {})",
             meanAbs, maxAbs, shaded, full.GetSize());

    bool passed = true;

    // ---- 0. 场景里必须真的有遮蔽 ----
    //
    // AO 全是 1 的话两张图当然一样, 而那证明不了任何事。
    if (shaded * 10 < full.GetSize())
    {
        LIMX_LOG(LogLaunch, Error,
                 "[AO半分] 只有 {} / {} 个像素有遮蔽 —— 比对无意义",
                 shaded, full.GetSize());
        passed = false;
    }

    // ---- 1. 平均差要小 ----
    //
    // 0.025 是从墙角场景量出来的: 正确实现给 0.014394。而能抓住的是
    // "上采样恒取最近邻"(0.02 以上) 与"权重不归一化"(整幅崩掉)。
    //
    // 这个数与场景有关 —— 阴影场景上正确实现就有 0.0373 (那里 AO 的梯度
    // 陡得多)。所以这条判据只在墙角场景上跑, verify.ps1 里也是这么调的。
    if (meanAbs > 0.025f)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[AO半分] 平均差 {} 超过 0.025 —— 半分辨率不是同一张图了",
                 meanAbs);
        passed = false;
    }

    // ---- 2. 平均差不能为零 ----
    //
    // 这一条防的是"开关没接上"。一个把 --gtao-half 忽略掉的实现会得到两张
    // **完全相同**的图, 而第一条判据对它满分通过 —— 于是"没生效"与"完美
    // 无损"在判据上无法区分。
    if (meanAbs < 1.0e-4f)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[AO半分] 平均差只有 {} —— 半分辨率是不是根本没生效?",
                 meanAbs);
        passed = false;
    }

    // ---- 3. 最大差要有界 ----
    //
    // 平均差小掩盖得住局部的严重偏差: 深度不连续处若双边退化成双线性, 前景
    // 与背景的 AO 会混在一起 —— 那是几个百分点的像素上几十个百分点的误差,
    // 平均下来看不见。
    if (maxAbs > 0.35f)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[AO半分] 最大差 {} 超过 0.35 —— 上采样在不连续处渗色?",
                 maxAbs);
        passed = false;
    }

    // ---- 4. 深度不连续处的差 —— 这里只报, 判在 --ao-edge-check ----
    //
    // 双边加权的全部作用都在深度不连续处, 而墙角场景 (这条判据唯一跑的
    // 场景) **一个不连续像素都没有** —— 两块平面填满视野。所以在这里判
    // 它是自欺: 阈值只能宽到连双线性都放过去。
    //
    // 真正判它的是 --ao-edge-check, 跑在综合场景上 (15.7 万个不连续像素)。
    {
        const FRHIExtent2D extent = context->GetSwapchainExtent();

        const FAoEdgeStats stats =
            ComputeAoEdgeStats(full, half, depth, extent);

        LIMX_LOG(LogLaunch, Display,
                 "[AO半分] 深度不连续处 {} 个像素, 平均差 {}, "
                 "偏差超 {} 的 {} 个",
                 stats.EdgeCount, stats.EdgeMean,
                 kAoBleedThreshold, stats.BleedCount);
    }

    if (passed)
    {
        LIMX_LOG(LogLaunch, Display,
                 "[AO半分] 通过 — 半分辨率与全分辨率是同一张图, 且确实生效");
    }
    else
    {
        LIMX_LOG(LogLaunch, Error, "[AO半分] 失败");
    }

    return passed;
}

// ============================================================================
// RunAoEdgeChecks — 双边上采样在深度不连续处到底有没有起作用
//
// 这条判据是补上一个记了一个周期的空白。上个周期的结论是"双边加权在手上的
// 场景里量不出来", 而那个结论其实是**两件事叠在一起**:
//
//   1. 墙角场景根本没有深度不连续 —— 场景不够。
//   2. LinearizeDepth 的分母符号写错了, 线性深度成了 -0.1..-0.05 的负数,
//      于是 max(max(a,b), 1e-4) 永远取到 1e-4, 相对差被放大四个量级,
//      exp(-relative/0.05) 直接下溢成零 —— **当时根本没有双边加权**,
//      它一直在做最近邻。
//
// 第二件事修掉之后, 第一件事仍然成立: 判据必须换个场景。综合场景有 15.7 万
// 个深度不连续的像素, 而那里双边与双线性的差别是能量出来的。
//
// 统计量选的是**渗色像素计数**而不是均值: 15.7 万个像素上的均值差只有
// 8.4% (0.0313 对 0.0340), 而偏差超过 0.2 的像素数差 41% (2694 对 3793)。
// 渗色本来就是少数像素上的大偏差, 均值会把它摊平。
// ============================================================================

// 这条判据补的是一个记了一个周期的空白, 而那个空白其实是**三件事**叠在
// 一起, 一件一件拆开才成立:
//
//   1. 墙角场景根本没有深度不连续 (0 个像素) —— 场景不够。
//   2. LinearizeDepth 的分母符号写反了, 线性深度成了负数且随距离减小,
//      权重全数下溢 —— 那时**根本没有双边加权**, 它一直在做最近邻。
//   3. 前向通道的深度附件 StoreOp 是 DontCare, 于是通道结束后深度内容
//      按规范是未定义的。只被清除过、没被绘制覆盖过的区域回读出来是 0。
//      在这一条修好之前, 这里挑出来的"不连续像素"有 157095 个, 而其中
//      绝大多数是**未初始化显存与有效数据的边界**, 不是真的轮廓。
//
// 三件都修完之后, 综合场景上真正的深度不连续像素是 7236 个。
// ============================================================================
// RunRayTracedAoChecks — 光追 AO 与直角凹角的闭式解逐像素比对
//
// GTAO 那条判据只能验"随半径增大朝 0.5 单调收敛" —— 因为屏幕空间的近似本身
// 没有解析解可对。光追 AO 有:
//
//   地面 y=0, 墙 z=0, 地面上一点离墙 d, 搜索半径 R, 令 c = d/R:
//
//       遮蔽率 = (1-c²)/2 - (2/π) ∫_c^1 s·arcsin(c/s) ds
//       AO     = 1 - 遮蔽率
//
//   c=0 时正好 0.5 (半个半球被挡住), c≥1 时正好 1 (墙在半径之外)。
//
// 推导: 方向 ω 命中墙当且仅当 ω_z < -c; 余弦加权的立体角积分对 φ 先积
// 得到 (π - 2·arcsin(c/s)), 对 s=sinθ 再积即得上式。这个式子在 Python 里
// 用四十万次余弦加权采样独立验过, 七个 c 值上最大差 7.6e-4 (而那正是
// 四十万次采样的噪声量级)。
//
// 判据的主项是**平均有符号误差**: 蒙特卡洛是无偏的, 所以它应当趋于零。
// 而任何一种写错 —— 余弦加权漏了、半球取反了、半径当成了别的单位 ——
// 都会产生一个系统性的偏移, 而系统性偏移正是平均值抓得最准的东西。
// ============================================================================

namespace
{

/// 闭式解的查找表
///
/// 逐像素做数值积分太贵 (九十万像素 × 数千步), 而这个函数在 [0,1] 上光滑,
/// 一千个采样点线性插值的误差在 1e-6 量级 —— 比蒙特卡洛的噪声小两个数量级。
class FCornerAoTable
{
public:
    static constexpr UInt32 kEntries = 1024;

    FCornerAoTable()
    {
        // 每一格用两千步的中点法积分。步数取这么多是因为这张表只建一次,
        // 而它是整条判据的参考值 —— 参考值上省时间是本末倒置。
        constexpr UInt32 kSteps = 2048;

        for (UInt32 i = 0; i < kEntries; ++i)
        {
            const Float64 c =
                static_cast<Float64>(i) / static_cast<Float64>(kEntries - 1);

            if (c >= 1.0)
            {
                m_Values[i] = 1.0f;
                continue;
            }

            Float64 integral = 0.0;

            for (UInt32 k = 0; k < kSteps; ++k)
            {
                const Float64 s =
                    c + (1.0 - c) * (static_cast<Float64>(k) + 0.5) /
                        static_cast<Float64>(kSteps);

                const Float64 ratio = (s > 0.0) ? FMath::Min(1.0, c / s) : 1.0;

                integral += s * FMath::ASin(static_cast<Float32>(ratio));
            }

            integral *= (1.0 - c) / static_cast<Float64>(kSteps);

            const Float64 occluded =
                (1.0 - c * c) * 0.5 -
                (2.0 / static_cast<Float64>(FMath::kPi)) * integral;

            m_Values[i] = static_cast<Float32>(1.0 - occluded);
        }
    }

    /// 查 c = d/R 处的 AO
    LIMX_NODISCARD Float32 Lookup(Float32 c) const
    {
        if (c <= 0.0f)
        {
            return 0.5f;
        }

        if (c >= 1.0f)
        {
            return 1.0f;
        }

        const Float32 position = c * static_cast<Float32>(kEntries - 1);

        const UInt32 lo = static_cast<UInt32>(position);
        const UInt32 hi = FMath::Min(lo + 1u, kEntries - 1u);

        const Float32 frac = position - static_cast<Float32>(lo);

        return m_Values[lo] * (1.0f - frac) + m_Values[hi] * frac;
    }

private:
    Float32 m_Values[kEntries] = {};
};

/// 一次光追 AO 采集的比对结果
struct FRtAoComparison
{
    SizeType FloorPixels  = 0;
    SizeType ComparedPixels = 0;

    /// 落在**有遮蔽的那一段** (c = d/R < 1) 的像素数
    ///
    /// 这一项是必需的: c >= 1 时闭式解恒为 1, 而"AO 恒为 1"的错误实现在
    /// 那些像素上零误差。地面在视野里从 0.02 一直铺到 4.7, 半径 0.8 时
    /// 八成以上的像素落在 c >= 1 —— 拿全体平均判, 等于把信号稀释五倍以上。
    SizeType OccludedPixels = 0;

    Float64 OccludedSignedSum   = 0.0;
    Float64 OccludedAbsoluteSum = 0.0;

    /// 有遮蔽区里实测与解析的均值 —— 用来看"是不是两边都恒为 1"
    Float64 OccludedMeasuredSum = 0.0;
    Float64 OccludedExpectedSum = 0.0;

    /// 远场 (d > 1.2R, 解析值恒为 1) 的像素数与最大缺口
    ///
    /// 这一段抓的是**自遮挡**: 那里半径内什么都没有, AO 必须是 1。小于 1
    /// 只能是射线打到了自己脚下的三角形 —— 而那是起点的偏移没盖住深度
    /// 反投影的误差。
    ///
    /// 那个误差随距离急剧增大 (透视深度在远处压缩得极厉害), 所以这一段
    /// 必须取到**足够远**的地方: 只看近处的话, 一个在远处整片自遮挡的
    /// 实现照样满分通过 —— 这正是第一版判据放过的东西。
    SizeType FarFieldPixels = 0;
    Float32  WorstFarDeficit = 0.0f;
    Float32  FarthestChecked = 0.0f;

    Float64 SignedSum   = 0.0;
    Float64 AbsoluteSum = 0.0;

    Float32 MinDistance = 1.0e30f;
    Float32 MaxDistance = -1.0e30f;

    bool Valid = false;

    LIMX_NODISCARD Float32 MeanSigned() const
    {
        return (ComparedPixels > 0)
            ? static_cast<Float32>(SignedSum /
                static_cast<Float64>(ComparedPixels))
            : 0.0f;
    }

    LIMX_NODISCARD Float32 MeanAbsolute() const
    {
        return (ComparedPixels > 0)
            ? static_cast<Float32>(AbsoluteSum /
                static_cast<Float64>(ComparedPixels))
            : 0.0f;
    }

    LIMX_NODISCARD Float32 OccludedMeanSigned() const
    {
        return (OccludedPixels > 0)
            ? static_cast<Float32>(OccludedSignedSum /
                static_cast<Float64>(OccludedPixels))
            : 0.0f;
    }

    LIMX_NODISCARD Float32 OccludedMeanAbsolute() const
    {
        return (OccludedPixels > 0)
            ? static_cast<Float32>(OccludedAbsoluteSum /
                static_cast<Float64>(OccludedPixels))
            : 0.0f;
    }

    LIMX_NODISCARD Float32 OccludedMeanMeasured() const
    {
        return (OccludedPixels > 0)
            ? static_cast<Float32>(OccludedMeasuredSum /
                static_cast<Float64>(OccludedPixels))
            : 0.0f;
    }

    LIMX_NODISCARD Float32 OccludedMeanExpected() const
    {
        return (OccludedPixels > 0)
            ? static_cast<Float32>(OccludedExpectedSum /
                static_cast<Float64>(OccludedPixels))
            : 0.0f;
    }
};

/// 渲一帧, 回读光追 AO 与法线, 逐像素与闭式解比对
static FRtAoComparison CaptureRtAoComparison(FRenderContext* context,
                                             FRenderer&      renderer,
                                             const FCornerAoTable& table,
                                             Float32 radius,
                                             UInt32  sampleCount,
                                             bool    halfResolution = false,
                                             TArray<Float32>* outAo = nullptr)
{
    FRtAoComparison result;

    FRayTracedAoPass* const aoPass = renderer.GetRayTracedAoPass();
    FDepthPrePass* const    depth  = renderer.GetDepthPrePass();

    if (aoPass == nullptr || depth == nullptr)
    {
        return result;
    }

    aoPass->SetRadius(radius);
    aoPass->SetSampleCount(sampleCount);
    aoPass->SetHalfResolution(halfResolution);

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

        desc.Size      = pixelCount;         // R8
        desc.DebugName = "RtAoCheck.AO";

        if (!IsRHISuccess(device->CreateBuffer(desc, aoReadback)))
        {
            return result;
        }

        desc.Size      = pixelCount * 4u;    // RG16_SFLOAT
        desc.DebugName = "RtAoCheck.Normal";

        if (!IsRHISuccess(device->CreateBuffer(desc, normalReadback)))
        {
            device->DestroyBuffer(aoReadback);
            return result;
        }
    }

    const FRHITextureHandle aoTexture     = aoPass->GetAoTexture();
    const FRHITextureHandle normalTexture = depth->GetNormalTexture();

    bool recorded = false;

    renderer.SetPostSceneRenderCallback(
        [&recorded, context, aoTexture, normalTexture,
         aoReadback, normalReadback, extent]()
        {
            IRHICommandBuffer* cmd = context->GetCurrentCommandBuffer();

            if (cmd == nullptr)
            {
                return;
            }

            FRHIBufferTextureCopyRegion region = {};
            region.BufferOffset      = 0;
            region.BufferRowLength   = 0;
            region.BufferImageHeight = 0;
            region.MipLevel          = 0;
            region.BaseLayer         = 0;
            region.LayerCount        = 1;
            region.TextureOffset     = { 0, 0, 0 };
            region.TextureExtent     = { extent.Width, extent.Height, 1 };

            cmd->TransitionImageLayout(
                aoTexture,
                EImageLayout::ShaderReadOnly, EImageLayout::TransferSrc,
                EPipelineStageFlags::FragmentShader,
                EPipelineStageFlags::Transfer,
                EAccessFlags::ShaderRead, EAccessFlags::TransferRead);

            cmd->CopyTextureToBuffer(aoTexture, EImageLayout::TransferSrc,
                                     aoReadback, region);

            cmd->TransitionImageLayout(
                aoTexture,
                EImageLayout::TransferSrc, EImageLayout::ShaderReadOnly,
                EPipelineStageFlags::Transfer,
                EPipelineStageFlags::FragmentShader,
                EAccessFlags::TransferRead, EAccessFlags::ShaderRead);

            cmd->TransitionImageLayout(
                normalTexture,
                EImageLayout::ShaderReadOnly, EImageLayout::TransferSrc,
                EPipelineStageFlags::FragmentShader,
                EPipelineStageFlags::Transfer,
                EAccessFlags::ShaderRead, EAccessFlags::TransferRead);

            cmd->CopyTextureToBuffer(normalTexture, EImageLayout::TransferSrc,
                                     normalReadback, region);

            cmd->TransitionImageLayout(
                normalTexture,
                EImageLayout::TransferSrc, EImageLayout::ShaderReadOnly,
                EPipelineStageFlags::Transfer,
                EPipelineStageFlags::FragmentShader,
                EAccessFlags::TransferRead, EAccessFlags::ShaderRead);

            recorded = true;
        });

    renderer.RenderFrame();
    renderer.SetPostSceneRenderCallback(TFunction<void()>());

    if (!recorded)
    {
        device->DestroyBuffer(normalReadback);
        device->DestroyBuffer(aoReadback);
        return result;
    }

    device->WaitIdle();

    TArray<Float32>  ao;
    TArray<FVector2> normals;

    {
        void* mapped = nullptr;

        if (IsRHISuccess(device->MapBuffer(aoReadback, &mapped)) &&
            mapped != nullptr)
        {
            const auto* src = static_cast<const UInt8*>(mapped);

            ao.Reserve(pixelCount);

            for (SizeType i = 0; i < pixelCount; ++i)
            {
                ao.Add(static_cast<Float32>(src[i]) / 255.0f);
            }

            device->UnmapBuffer(aoReadback);
        }

        mapped = nullptr;

        if (IsRHISuccess(device->MapBuffer(normalReadback, &mapped)) &&
            mapped != nullptr)
        {
            const auto* src = static_cast<const Float16Bits*>(mapped);

            normals.Reserve(pixelCount);

            for (SizeType i = 0; i < pixelCount; ++i)
            {
                normals.Add(FVector2(Float16ToFloat32(src[i * 2 + 0]),
                                     Float16ToFloat32(src[i * 2 + 1])));
            }

            device->UnmapBuffer(normalReadback);
        }
    }

    device->DestroyBuffer(normalReadback);
    device->DestroyBuffer(aoReadback);

    if (ao.GetSize() != pixelCount || normals.GetSize() != pixelCount)
    {
        return result;
    }

    if (outAo != nullptr)
    {
        *outAo = ao;
    }

    // ---- 逐像素比对 ----
    const FCamera& camera = renderer.GetCamera();

    const FMatrix inverse =
        (camera.GetProjectionMatrix() * camera.GetViewMatrix()).Inverse();

    const FVector3 cameraPos = camera.GetPosition();

    for (UInt32 y = 0; y < extent.Height; ++y)
    {
        for (UInt32 x = 0; x < extent.Width; ++x)
        {
            const SizeType index =
                static_cast<SizeType>(y) * extent.Width + x;

            if (FMath::Abs(normals[index].X) > 1.0f ||
                FMath::Abs(normals[index].Y) > 1.0f)
            {
                continue;
            }

            const FVector3 n = DecodeOctahedralNormal(normals[index]);

            // 只取朝上的地面像素 —— 闭式解是为"地面 + 一面墙"推的
            if (n.Y < 0.99f)
            {
                continue;
            }

            ++result.FloorPixels;

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

            // 与 y=0 平面求交
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
            const Float32 worldX = cameraPos.X + dir.X * t;

            // 闭式解假设墙是无限大的。地面与墙都是 20x20 的方片, 所以
            // 只取离边界足够远的部分 —— 靠近边缘的像素能"看到墙外面",
            // 那里的遮蔽比无限墙小, 而那不是实现的错。
            // 地面是 20x20 的板, 中心在 z=10, 所以它铺到 z=20。
            //
            // 上限从 6 放宽到 16: 深度反投影的误差随距离急剧增大, 而自遮挡
            // 正是在远处才现形。只看到 6 的话, 一个在二十米外整片自遮挡的
            // 实现照样满分通过 —— 第一版判据就是这样放过去的。
            constexpr Float32 kHalfExtent = 10.0f;
            constexpr Float32 kEdgeMargin = 4.0f;
            constexpr Float32 kMaxDistance = 16.0f;

            if (FMath::Abs(worldX) > kHalfExtent - kEdgeMargin ||
                worldZ < 0.0f || worldZ > kMaxDistance)
            {
                continue;
            }

            const Float32 c = worldZ / radius;

            const Float32 expected = table.Lookup(c);
            const Float32 measured = ao[index];

            result.SignedSum   += static_cast<Float64>(measured - expected);
            result.AbsoluteSum +=
                static_cast<Float64>(FMath::Abs(measured - expected));

            // 远场: 半径内什么都没有, 解析值恒为 1
            if (c > 1.2f)
            {
                ++result.FarFieldPixels;

                result.WorstFarDeficit =
                    FMath::Max(result.WorstFarDeficit, 1.0f - measured);

                result.FarthestChecked =
                    FMath::Max(result.FarthestChecked, worldZ);
            }

            if (c < 1.0f)
            {
                ++result.OccludedPixels;

                result.OccludedSignedSum +=
                    static_cast<Float64>(measured - expected);
                result.OccludedAbsoluteSum +=
                    static_cast<Float64>(FMath::Abs(measured - expected));

                result.OccludedMeasuredSum += static_cast<Float64>(measured);
                result.OccludedExpectedSum += static_cast<Float64>(expected);
            }

            result.MinDistance = FMath::Min(result.MinDistance, worldZ);
            result.MaxDistance = FMath::Max(result.MaxDistance, worldZ);

            ++result.ComparedPixels;
        }
    }

    result.Valid = (result.ComparedPixels > 0);

    return result;
}

} // namespace

// ============================================================================
// RunRayTracedAoSelfCheck — 半径极小时 AO 必须处处为 1
//
// 这条判据与场景无关, 而那正是它的价值。
//
// 搜索半径设成 0.02 个世界单位时, 任何真实几何体都在半径之外 —— 于是解析
// 答案是**处处恰好 1**, 不需要任何场景知识。小于 1 只能有一个来源: 射线打到
// 了自己脚下的那个三角形。
//
// 那件事什么时候会发生? 世界坐标是从深度缓冲区反投影来的, 它自带一个误差,
// 而那个误差**随距离急剧增大** —— 透视深度在远处压缩得极厉害, 同一个 float32
// 最低位在近处代表微米, 在远处代表厘米。起点的偏移是固定值的话, 近处一切
// 正常, 远处整行像素一起自遮挡。
//
// "整行"是因为深度量化把连续的表面切成一条条等深度的带, 带内所有像素的
// 反投影误差相同 —— 画面上是一道道横纹。而那看起来像"采样数不够的噪声",
// 不像"偏移不够"。
//
// 墙角场景的解析判据抓不到它: 那个场景里相机看得到的地面只铺到 4.7 个单位,
// 而条纹要到二三十米外才现形。所以这条判据要跑在**有远景的场景**上。
// ============================================================================

static bool RunRayTracedAoSelfCheck(FRenderContext* context,
                                    FRenderer&      renderer)
{
    if (!renderer.SetRayTracedAoEnabled(true))
    {
        LIMX_LOG(LogLaunch, Error,
                 "[光追AO自遮挡] 无法启用 — 判据无法执行, 判定为失败");
        return false;
    }

    FRayTracedAoPass* const aoPass = renderer.GetRayTracedAoPass();
    FDepthPrePass* const    depth  = renderer.GetDepthPrePass();

    if (aoPass == nullptr || depth == nullptr)
    {
        return false;
    }

    const Float32 originalRadius = aoPass->GetRadius();
    const UInt32  originalSamples = aoPass->GetSampleCount();
    const bool    originalHalf   = aoPass->IsHalfResolution();

    // 用**工作半径**, 不用一个极小的半径。
    //
    // 第一版用的是 0.02 —— 想法是"半径内什么都没有, AO 必须处处为 1"。
    // 实测那个想法不成立: 球体压在地面上, 接触圈处两个表面确实相距不到
    // 0.02, 那里 AO 本来就该小于 1。而且更要命的是, 它**分辨不出这个 bug**:
    // 有 bug 时 1346 个像素, 修好后 1302 个 —— 差别全在噪声里。
    //
    // 自遮挡的条纹只在工作半径下出现: 射线要够长才会绕回来打到自己脚下。
    // 所以判据也得在工作半径下判, 而"哪里该是 1"改用**距离**来圈定 ——
    // 远处的墙面周围一个半径内什么都没有。
    aoPass->SetRadius(0.8f);
    // 采样数用**默认的 16**, 不调高。
    //
    // 调高会把这条判据要抓的东西抹掉: 自遮挡的条纹是"这一行的射线撞不撞
    // 得到自己脚下"这种阈值效应, 而更多的样本会把带边缘平均掉。判据必须
    // 在**实际会用的配置**下判 —— 在一个没人会用的配置下通过, 等于没判。
    aoPass->SetSampleCount(16);
    aoPass->SetHalfResolution(false);

    IRHIDevice* const device = context->GetDevice();

    const FRHIExtent2D extent = context->GetSwapchainExtent();

    const SizeType pixelCount =
        static_cast<SizeType>(extent.Width) * extent.Height;

    FRHIBufferHandle aoReadback;
    FRHIBufferHandle depthReadback;

    {
        FRHIBufferDesc desc = {};
        desc.Usage       = EBufferUsage::TransferDst;
        desc.MemoryUsage = EMemoryUsage::GpuToCpu;

        desc.Size      = pixelCount;
        desc.DebugName = "RtAoSelf.AO";

        if (!IsRHISuccess(device->CreateBuffer(desc, aoReadback)))
        {
            return false;
        }

        desc.Size      = pixelCount * 4u;
        desc.DebugName = "RtAoSelf.Depth";

        if (!IsRHISuccess(device->CreateBuffer(desc, depthReadback)))
        {
            device->DestroyBuffer(aoReadback);
            return false;
        }
    }

    const FRHITextureHandle aoTexture    = aoPass->GetAoTexture();
    const FRHITextureHandle depthTexture = depth->GetSharedDepthTexture();

    bool recorded = false;

    renderer.SetPostSceneRenderCallback(
        [&recorded, context, aoTexture, depthTexture,
         aoReadback, depthReadback, extent]()
        {
            IRHICommandBuffer* cmd = context->GetCurrentCommandBuffer();

            if (cmd == nullptr)
            {
                return;
            }

            FRHIBufferTextureCopyRegion region = {};
            region.BufferOffset      = 0;
            region.BufferRowLength   = 0;
            region.BufferImageHeight = 0;
            region.MipLevel          = 0;
            region.BaseLayer         = 0;
            region.LayerCount        = 1;
            region.TextureOffset     = { 0, 0, 0 };
            region.TextureExtent     = { extent.Width, extent.Height, 1 };

            cmd->TransitionImageLayout(
                aoTexture, EImageLayout::ShaderReadOnly,
                EImageLayout::TransferSrc,
                EPipelineStageFlags::FragmentShader,
                EPipelineStageFlags::Transfer,
                EAccessFlags::ShaderRead, EAccessFlags::TransferRead);

            cmd->CopyTextureToBuffer(aoTexture, EImageLayout::TransferSrc,
                                     aoReadback, region);

            cmd->TransitionImageLayout(
                aoTexture, EImageLayout::TransferSrc,
                EImageLayout::ShaderReadOnly,
                EPipelineStageFlags::Transfer,
                EPipelineStageFlags::FragmentShader,
                EAccessFlags::TransferRead, EAccessFlags::ShaderRead);

            cmd->TransitionImageLayout(
                depthTexture, EImageLayout::DepthStencilAttachment,
                EImageLayout::TransferSrc,
                EPipelineStageFlags::LateFragmentTests,
                EPipelineStageFlags::Transfer,
                EAccessFlags::DepthStencilAttachmentWrite,
                EAccessFlags::TransferRead);

            cmd->CopyTextureToBuffer(depthTexture, EImageLayout::TransferSrc,
                                     depthReadback, region);

            cmd->TransitionImageLayout(
                depthTexture, EImageLayout::TransferSrc,
                EImageLayout::DepthStencilAttachment,
                EPipelineStageFlags::Transfer,
                EPipelineStageFlags::EarlyFragmentTests,
                EAccessFlags::TransferRead,
                EAccessFlags::DepthStencilAttachmentWrite);

            recorded = true;
        });

    renderer.RenderFrame();
    renderer.SetPostSceneRenderCallback(TFunction<void()>());

    aoPass->SetRadius(originalRadius);
    aoPass->SetSampleCount(originalSamples);
    aoPass->SetHalfResolution(originalHalf);

    SizeType geometryPixels = 0;
    SizeType occludedPixels = 0;
    Float64  rowStepSum     = 0.0;
    SizeType rowStepPairs   = 0;
    Float32  worstDeficit   = 0.0f;
    Float32  farthestDepth  = 0.0f;

    bool ok = recorded;

    if (ok)
    {
        device->WaitIdle();

        void* aoMapped    = nullptr;
        void* depthMapped = nullptr;

        if (IsRHISuccess(device->MapBuffer(aoReadback, &aoMapped)) &&
            IsRHISuccess(device->MapBuffer(depthReadback, &depthMapped)) &&
            aoMapped != nullptr && depthMapped != nullptr)
        {
            const auto* ao    = static_cast<const UInt8*>(aoMapped);
            const auto* depth32 = static_cast<const Float32*>(depthMapped);

            const FCamera& camera = renderer.GetCamera();

            const Float32 nearPlane = camera.GetNearPlane();
            const Float32 farPlane  = camera.GetFarPlane();

            const Float32 a = farPlane / (nearPlane - farPlane);
            const Float32 b = farPlane * nearPlane / (nearPlane - farPlane);

            for (SizeType i = 0; i < pixelCount; ++i)
            {
                // 天空不算 —— 那里没有表面
                if (depth32[i] >= 0.999999f)
                {
                    continue;
                }

                const Float32 denom = depth32[i] + a;

                const Float32 linear =
                    (FMath::Abs(denom) > 1.0e-9f) ? (b / denom) : 0.0f;

                farthestDepth = FMath::Max(farthestDepth, linear);

                // 只看远处的表面。
                //
                // 近处有球体压在地面上、柱子立在地面上, 那些接触处的遮挡
                // 是**真的**。而远处的背景墙周围一个半径 (0.8) 之内什么都
                // 没有 —— 那里的 AO 必须是 1, 小于 1 只能是自遮挡。
                //
                // 二十个单位是量出来的: 场景里最远的柱子在十几米处, 二十米
                // 之外只剩背景墙。
                if (linear < 20.0f)
                {
                    continue;
                }

                ++geometryPixels;

                const Float32 value = static_cast<Float32>(ao[i]) / 255.0f;

                const Float32 deficit = 1.0f - value;

                if (deficit > 0.02f)
                {
                    ++occludedPixels;
                }

                worstDeficit = FMath::Max(worstDeficit, deficit);

                // 行间差异 —— 条纹的特征。
                //
                // 自遮挡的条纹来自深度量化: 量化把连续的表面切成一条条
                // 等深度的带, 带内所有像素的反投影误差相同, 于是**整行**
                // 一起被遮挡或一起不被遮挡。相邻行之间因此出现台阶, 而
                // 真实的遮蔽 (柱子立在地上) 在竖直方向上是连续的。
                const SizeType row = i / extent.Width;
                const SizeType col = i % extent.Width;

                if (row + 1 < extent.Height)
                {
                    const SizeType below = i + extent.Width;

                    if (depth32[below] < 0.999999f)
                    {
                        const Float32 belowDenom = depth32[below] + a;

                        const Float32 belowLinear =
                            (FMath::Abs(belowDenom) > 1.0e-9f)
                                ? (b / belowDenom) : 0.0f;

                        // 只在深度连续的地方比 —— 跨越轮廓的两个像素本来
                        // 就该不同
                        if (belowLinear > 20.0f &&
                            FMath::Abs(belowLinear - linear) <
                                linear * 0.01f)
                        {
                            const Float32 belowValue =
                                static_cast<Float32>(ao[below]) / 255.0f;

                            rowStepSum +=
                                static_cast<Float64>(
                                    FMath::Abs(value - belowValue));

                            ++rowStepPairs;
                        }
                    }
                }

                (void)col;
            }

            device->UnmapBuffer(depthReadback);
            device->UnmapBuffer(aoReadback);
        }
        else
        {
            ok = false;
        }
    }

    device->DestroyBuffer(depthReadback);
    device->DestroyBuffer(aoReadback);

    if (!ok)
    {
        LIMX_LOG(LogLaunch, Error, "[光追AO自遮挡] 回读失败");
        return false;
    }

    LIMX_LOG(LogLaunch, Display,
             "[光追AO自遮挡] 半径 0.8 — 二十米外的像素 {} 个 (最远 {} 单位), "
             "AO 明显小于 1 的 {} 个, 最大缺口 {} | 深度连续处的相邻行差 "
             "均值 {} ({} 对)",
             geometryPixels, farthestDepth, occludedPixels, worstDeficit,
             (rowStepPairs > 0)
                 ? static_cast<Float32>(rowStepSum /
                     static_cast<Float64>(rowStepPairs))
                 : 0.0f,
             rowStepPairs);

    bool passed = true;

    // ---- 元判据: 得有足够多的几何体, 而且要够远 ----
    //
    // 全是近处几何体的话这条判据是空的 —— 自遮挡要到二三十米外才现形。
    constexpr SizeType kMinPixels = 50000;
    constexpr Float32  kMinFarDepth = 20.0f;

    if (geometryPixels < kMinPixels)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[光追AO自遮挡] 二十米外只有 {} 个像素 (需要至少 {}) "
                 "—— 这个场景没有远景, 判不了自遮挡",
                 geometryPixels, kMinPixels);
        passed = false;
    }

    if (farthestDepth < kMinFarDepth)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[光追AO自遮挡] 最远的几何体只有 {} 个单位 (需要至少 {}) "
                 "—— 自遮挡在近处不现形, 这个场景判不了",
                 farthestDepth, kMinFarDepth);
        passed = false;
    }

    // ---- 判据: 一个像素都不该被遮挡 ----
    //
    // 二十米外的表面周围一个半径之内什么都没有, 所以 AO 恒为 1。
    //
    // 实测: 起点只沿法线推固定的 1e-3 时, 这里有 六位数 的像素自遮挡
    // (画面上是一道道横纹); 按深度量子推之后是零。
    if (occludedPixels != 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[光追AO自遮挡] {} 个像素的 AO 明显小于 1 (最大缺口 {}) "
                 "—— 射线打到了自己脚下的三角形, 起点的偏移没盖住深度反投影"
                 "的误差",
                 occludedPixels, worstDeficit);
        passed = false;
    }

    LIMX_LOG(LogLaunch, Display,
             "[光追AO自遮挡] {}", passed ? "通过" : "失败");

    return passed;
}

// ── 已知的覆盖边界 (量过, 三条都能从几何上解释) ─────────────────────────
//
// 变异验证 7/10。三条逃逸**都是这个场景里的真等价**, 不是判据漏了:
//
// 1. **法线偏移从 1e-3 放大到 0.1** —— 逃逸, 数字一位都没变。
//    墙角场景的地面法线是 +Y, 而墙是 z=0 平面 —— 法线**平行于墙**。沿它
//    推 0.1 只是贴着墙滑一段, 到墙的距离 d 一点没变, 遮蔽自然一样。
//    这与光追阴影那边"沿光线的偏置不移动边界"是同一类事: 偏置移动的方向
//    与被遮挡的方向正交时, 它就不产生后果。
//    要验它需要一个法线不平行于遮挡物的场景。
//
// 2. **实例变换转置** —— 逃逸。转置把平移丢了 (第四列读到的是底行
//    (0,0,0,1)), 于是地面从 z∈[0,20] 挪到 z∈[-10,10]、墙从 y∈[0,20] 挪到
//    y∈[-10,10]。而测量区在 z∈[0,6]、y≥0 —— **两块板挪完之后仍然盖住
//    测量区**。这一条在 --rt-depth-check 上是被抓住的 (那里逐像素比深度,
//    平移一丢就整片对不上), 所以覆盖没有缺口, 只是不在这条判据里。
//
// 3. **逐像素旋转去掉** —— 逃逸 (0.0032 对 0.0034)。256 个 Hammersley
//    点本身已经足够均匀, 相干与否的差别落在量化步长之下。它在低采样数下
//    才会现形 (16 个样本时是可见的条带), 而那时噪声 0.03 已经超过判据的
//    阈值 0.008 —— 判据在那个采样数上本来就不成立。
static bool RunRayTracedAoChecks(FRenderContext* context, FRenderer& renderer)
{
    if (!renderer.SetRayTracedAoEnabled(true))
    {
        LIMX_LOG(LogLaunch, Error,
                 "[光追AO] 无法启用 — 判据无法执行, 判定为失败");
        return false;
    }

    const FCornerAoTable table;

    // 采样数取高一些: 判据要分开"实现有系统偏差"与"采样数不够"。
    // 256 个样本下单像素噪声约 0.03, 而几十万个像素平均之后降到 1e-4 量级。
    constexpr UInt32 kSampleCount = 256;

    // 两个半径。闭式解是半径的函数, 所以"把半径当成了别的单位"或者"根本
    // 没用半径"这类错误在单一半径下可能蒙对, 两个半径下蒙不过去。
    const Float32 radii[2] = { 0.8f, 2.0f };

    bool passed = true;

    for (UInt32 i = 0; i < 2; ++i)
    {
        const FRtAoComparison cmp =
            CaptureRtAoComparison(context, renderer, table,
                                  radii[i], kSampleCount);

        if (!cmp.Valid)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[光追AO] 半径 {} 的采集失败", radii[i]);
            return false;
        }

        LIMX_LOG(LogLaunch, Display,
                 "[光追AO] 半径 {} — 比对 {} 像素 (地面 {}), 离墙 {} .. {}",
                 radii[i], cmp.ComparedPixels, cmp.FloorPixels,
                 cmp.MinDistance, cmp.MaxDistance);

        LIMX_LOG(LogLaunch, Display,
                 "[光追AO]   有遮蔽区 (d<R) {} 像素 — 实测均值 {} 解析均值 {} "
                 "| 有符号误差 {} 绝对误差 {}",
                 cmp.OccludedPixels,
                 cmp.OccludedMeanMeasured(), cmp.OccludedMeanExpected(),
                 cmp.OccludedMeanSigned(), cmp.OccludedMeanAbsolute());

        LIMX_LOG(LogLaunch, Display,
                 "[光追AO]   全体 — 有符号误差 {} 绝对误差 {} | "
                 "远场 {} 像素 (最远 {}), 最大自遮挡缺口 {}",
                 cmp.MeanSigned(), cmp.MeanAbsolute(),
                 cmp.FarFieldPixels, cmp.FarthestChecked,
                 cmp.WorstFarDeficit);

        // ---- 判据 4: 远场不能自遮挡 ----
        //
        // 半径内什么都没有的地方 AO 必须是 1。小于 1 只能是射线打到了自己
        // 脚下的三角形 —— 起点的偏移没盖住深度反投影的误差。
        //
        // 阈值 0.02: R8 的量化步长是 0.004, 而 256 个样本里错一个就是
        // 0.004。留五倍。实测正确实现是 0。
        constexpr Float32 kMaxFarDeficit = 0.02f;

        if (cmp.FarFieldPixels < 10000)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[光追AO] 远场只有 {} 个像素 —— 自遮挡判不了",
                     cmp.FarFieldPixels);
            passed = false;
        }

        if (cmp.WorstFarDeficit > kMaxFarDeficit)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[光追AO] 远场最大自遮挡缺口 {} 超过 {} (最远 {} 单位) "
                     "—— 射线打到了自己脚下的三角形",
                     cmp.WorstFarDeficit, kMaxFarDeficit,
                     cmp.FarthestChecked);
            passed = false;
        }

        // ---- 元判据: 比对的像素要够多, 距离范围要跨过半径 ----
        //
        // 比对区间全落在 d > R 的地方时闭式解恒为 1, 而"AO 恒为 1"的错误
        // 实现会满分通过。范围必须跨过 c=1 那个点两侧。
        constexpr SizeType kMinPixels = 50000;

        if (cmp.ComparedPixels < kMinPixels)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[光追AO] 只比对了 {} 个像素 (需要至少 {})",
                     cmp.ComparedPixels, kMinPixels);
            passed = false;
        }

        if (cmp.MinDistance > radii[i] * 0.25f)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[光追AO] 最近的比对点离墙 {}, 超过半径的四分之一 "
                     "({}) —— 遮蔽最强的那一段没被验到",
                     cmp.MinDistance, radii[i] * 0.25f);
            passed = false;
        }

        // ---- 判据 1: 平均有符号误差趋于零 ----
        //
        // 蒙特卡洛是无偏的, 所以这个数应当只剩噪声。实测:
        //
        //     半径 0.8   -0.000083   (39236 个有遮蔽像素)
        //     半径 2.0   -0.000124   (116036 个)
        //
        // 阈值取 0.003, 是实测的二十四倍 —— 而任何一种系统性写错 (余弦
        // 加权漏了、半球取反、半径当成了别的单位) 都是 0.05 以上。
        constexpr Float32 kMaxSignedError = 0.003f;

        if (FMath::Abs(cmp.OccludedMeanSigned()) > kMaxSignedError)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[光追AO] 半径 {} 有遮蔽区的平均有符号误差 {} 超过 {} "
                     "—— 系统性偏差, 不是采样噪声",
                     radii[i], cmp.OccludedMeanSigned(), kMaxSignedError);
            passed = false;
        }

        // ---- 判据 2: 平均绝对误差与噪声量级相符 ----
        //
        // 只判有符号误差的话, "一半像素偏高一半偏低"能互相抵消 —— 那是
        // 一张噪声图, 而平均值看起来完美。绝对误差不会被抵消。
        //
        // 实测两个半径都是 0.0034 —— 而 R8 的量化步长就是 1/255 = 0.0039。
        //
        // 也就是说逐像素的误差**已经压到输出纹理的精度极限**: 低差异序列
        // 在 256 个样本下的误差是 O(1/N) ≈ 0.004, 与量化步长同量级, 两者
        // 都比"实现写错"小两个数量级。
        //
        // 阈值取 0.008 (实测的 2.3 倍)。放宽到 0.06 的话, 采样数从 256 掉
        // 到 16 (噪声 0.03) 都能通过 —— 那时判据验的就不是实现了。
        constexpr Float32 kMaxAbsoluteError = 0.008f;

        if (cmp.OccludedMeanAbsolute() > kMaxAbsoluteError)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[光追AO] 半径 {} 有遮蔽区的平均绝对误差 {} 超过 {} "
                     "—— 逐像素对不上, 而不只是均值对不上",
                     radii[i], cmp.OccludedMeanAbsolute(), kMaxAbsoluteError);
            passed = false;
        }

        // ---- 判据 3: 有遮蔽区里必须真的有遮蔽 ----
        //
        // 这一条防的是"两边都恒为 1"。c >= 1 的地方闭式解就是 1, 而地面
        // 在视野里铺得很远 —— 半径 0.8 时八成以上的像素落在那一段。全体
        // 平均因此会被稀释五倍以上, 一个"AO 恒为 1"的实现在全体平均上只
        // 差百分之几。
        //
        // 解析均值明显小于 1 才说明这一段真的在验遮蔽。
        constexpr Float32 kMaxOccludedMean = 0.95f;

        if (cmp.OccludedMeanExpected() > kMaxOccludedMean)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[光追AO] 有遮蔽区的解析均值 {} 太接近 1 —— "
                     "这一段没有真正的遮蔽, 判据是空的",
                     cmp.OccludedMeanExpected());
            passed = false;
        }

        constexpr SizeType kMinOccludedPixels = 10000;

        if (cmp.OccludedPixels < kMinOccludedPixels)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[光追AO] 有遮蔽区只有 {} 个像素 (需要至少 {})",
                     cmp.OccludedPixels, kMinOccludedPixels);
            passed = false;
        }
    }


    // ================================================================
    // 半分辨率
    //
    // 半分辨率不是"把深度降采样再解", 而是**每隔一个像素解一次** —— 于是
    // 它的结果是全分辨率结果的严格子集。两条判据:
    //
    //   1. 偶数像素上两者必须**逐位相同**。不是"接近", 是相同 —— 同一条
    //      射线、同一个样本图案、同一个深度。有任何差别都说明半分辨率
    //      那条路上多做了一次重采样, 而重采样过的深度在不连续处是不存在
    //      的表面。
    //   2. 上采样之后整幅图仍要过同一条解析判据。插值出来的像素当然不再
    //      精确, 但误差应当只比全分辨率大一点点 —— 大很多就说明双边加权
    //      没起作用 (退化成双线性), 而那正是这个引擎栽过一次的地方。
    // ================================================================
    {
        TArray<Float32> fullAo;
        TArray<Float32> halfAo;

        const FRtAoComparison fullCmp =
            CaptureRtAoComparison(context, renderer, table, 0.8f,
                                  kSampleCount, false, &fullAo);

        const FRtAoComparison halfCmp =
            CaptureRtAoComparison(context, renderer, table, 0.8f,
                                  kSampleCount, true, &halfAo);

        renderer.GetRayTracedAoPass()->SetHalfResolution(false);

        if (!fullCmp.Valid || !halfCmp.Valid ||
            fullAo.GetSize() != halfAo.GetSize() || fullAo.GetSize() == 0)
        {
            LIMX_LOG(LogLaunch, Error, "[光追AO] 半分辨率的采集失败");
            passed = false;
        }
        else
        {
            const FRHIExtent2D extent = context->GetSwapchainExtent();

            SizeType sampledPixels = 0;
            SizeType sampledDiffer = 0;
            Float32  worstSampled  = 0.0f;

            for (UInt32 y = 0; y < extent.Height; y += 2)
            {
                for (UInt32 x = 0; x < extent.Width; x += 2)
                {
                    const SizeType index =
                        static_cast<SizeType>(y) * extent.Width + x;

                    ++sampledPixels;

                    const Float32 diff =
                        FMath::Abs(fullAo[index] - halfAo[index]);

                    worstSampled = FMath::Max(worstSampled, diff);

                    if (diff > 0.0f)
                    {
                        ++sampledDiffer;
                    }
                }
            }

            LIMX_LOG(LogLaunch, Display,
                     "[光追AO] 半分辨率 — 偶数像素 {} 个, 与全分辨率不同的 "
                     "{} 个 (最大差 {}) | 有遮蔽区 有符号误差 {} 绝对误差 {} "
                     "(全分辨率 {} / {})",
                     sampledPixels, sampledDiffer, worstSampled,
                     halfCmp.OccludedMeanSigned(),
                     halfCmp.OccludedMeanAbsolute(),
                     fullCmp.OccludedMeanSigned(),
                     fullCmp.OccludedMeanAbsolute());

            // ---- 判据 A: 偶数像素逐位相同 ----
            //
            // 这一条不留容差。半分辨率在这些像素上做的是**同一件事**:
            // 同一条射线、同一个样本图案、同一个深度。差一位都说明那条路
            // 上多了一次重采样。
            if (sampledDiffer != 0)
            {
                LIMX_LOG(LogLaunch, Error,
                         "[光追AO] 半分辨率在 {} 个偶数像素上与全分辨率不同 "
                         "(最大差 {}) —— 半分辨率那条路多做了一次重采样?",
                         sampledDiffer, worstSampled);
                passed = false;
            }

            // ---- 判据 B: 上采样之后仍要过解析判据 ----
            //
            // 阈值比全分辨率宽一档: 四分之三的像素是插值出来的, 而 AO 在
            // 空间上有梯度, 插值必然带来误差。但只宽一档 —— 宽太多的话,
            // "双边退化成双线性"这类错误就通过了。
            constexpr Float32 kHalfMaxSigned   = 0.006f;
            constexpr Float32 kHalfMaxAbsolute = 0.020f;

            if (FMath::Abs(halfCmp.OccludedMeanSigned()) > kHalfMaxSigned)
            {
                LIMX_LOG(LogLaunch, Error,
                         "[光追AO] 半分辨率的平均有符号误差 {} 超过 {}",
                         halfCmp.OccludedMeanSigned(), kHalfMaxSigned);
                passed = false;
            }

            if (halfCmp.OccludedMeanAbsolute() > kHalfMaxAbsolute)
            {
                LIMX_LOG(LogLaunch, Error,
                         "[光追AO] 半分辨率的平均绝对误差 {} 超过 {}",
                         halfCmp.OccludedMeanAbsolute(), kHalfMaxAbsolute);
                passed = false;
            }
        }
    }

    LIMX_LOG(LogLaunch, Display, "[光追AO] {}", passed ? "通过" : "失败");

    return passed;
}

// ============================================================================
// RunMeshletResolveChecks — 从可见性编号反解出来的东西对不对
//
// 可见性缓冲区上一个像素只有一个数。材质解析把它展开回"这个像素上是什么":
// 哪个实例、哪个 meshlet、哪个三角形、重心坐标多少、法线朝哪、什么材质。
//
// 这条链上任何一环错了, 画面上都是"某处的着色不对" —— 而那种错在光栅化
// 阶段一点痕迹都没有 (深度是对的, 编号也是对的)。
//
// 三条判据:
//
//   一、**重算的深度与光栅器写的逐像素吻合。**
//
//      这是今天最强的一条, 也是本周期第三次用同一个招式 (Day 5 的位置
//      自洽残差、Day 10 的两条路径逐位相同)。光栅器与解析算的是同一个量:
//
//        光栅器: 顶点变换 -> 屏幕空间线性插值 z/w
//        解析:   同样的顶点变换 -> 在像素中心解重心坐标 -> 同样的插值
//
//      两者只在浮点舍入上有差别。所以这一条同时钉住了: 编号解得对不对、
//      可见记录查得对不对、顶点取得对不对、重心坐标算得对不对。任何一处
//      错了, 深度就落在别的三角形上, 差得远远超过舍入。
//
//      容差按**深度值本身的 ULP** 定, 不是一个绝对数: NDC 深度在近处
//      接近 0、远处接近 1, 而 float32 在这两处的最低位差着几个数量级。
//
//   二、**法线与经典 G-Buffer 的法线吻合。**
//
//      经典路径的法线由光栅器插值顶点法线再八面体编码; 解析路径手算重心
//      坐标做同样的事。两者的差只该来自 RG16_SFLOAT 的量化 (约 5e-4)。
//
//      这一条验的是透视校正: 属性必须用**透视校正**的权重插, 而深度必须
//      用**屏幕空间**的权重。两者用同一套权重是这类代码最经典的错误 ——
//      而它在深度上只差一点点 (看起来像精度问题), 在法线上却差得明显。
//
//   三、**材质下标与源对象的一致。**
//
//      实例表里的 MeshletRange[2] 存的是源对象下标, 解析按它查材质。
//      用实例序号去索引对象列表的话, 只要有一个对象被跳过 (半透明、
//      无 meshlet), 后面所有物体的材质就整体错位一格 —— 与 Day 5 光追
//      几何表踩过的是同一个坑。
// ============================================================================

namespace
{

/// 与 FMeshletResolveResult 一致
struct FResolveView
{
    Float32 Depth;
    Float32 NormalX;
    Float32 NormalY;
    UInt32  Material;
    Float32 WorldX;
    Float32 WorldY;
    Float32 WorldZ;
};

static_assert(sizeof(FResolveView) == 28, "与 FMeshletResolveResult 同布局");

/// NDC 深度在这个取值附近的一个最低位有多大
///
/// float32 的相邻可表示数之间的距离随数值大小指数变化。用一个绝对容差的话,
/// 它在近处 (深度接近 0) 松得没有意义, 在远处 (接近 1) 又紧到永远红。
Float32 DepthUlp(Float32 depth)
{
    const Float32 magnitude = FMath::Max(FMath::Abs(depth), 1.0e-6f);

    // 尾数 23 位 —— 一个最低位约是数值的 2^-23
    return magnitude * 1.1920929e-7f;
}

/// 八面体解码 —— 与 gbuffer_common.h 的 DecodeOctahedralNormal 逐字对应
FVector3 DecodeOctahedral(Float32 x, Float32 y)
{
    FVector3 n(x, y, 1.0f - FMath::Abs(x) - FMath::Abs(y));

    if (n.Z < 0.0f)
    {
        const Float32 signX = (n.X >= 0.0f) ? 1.0f : -1.0f;
        const Float32 signY = (n.Y >= 0.0f) ? 1.0f : -1.0f;

        const Float32 wrapX = (1.0f - FMath::Abs(n.Y)) * signX;
        const Float32 wrapY = (1.0f - FMath::Abs(n.X)) * signY;

        n.X = wrapX;
        n.Y = wrapY;
    }

    const Float32 length =
        FMath::Sqrt(n.X * n.X + n.Y * n.Y + n.Z * n.Z);

    return (length > 1.0e-12f)
               ? FVector3(n.X / length, n.Y / length, n.Z / length)
               : FVector3(0.0f, 0.0f, 1.0f);
}

} // namespace

static bool RunMeshletResolveChecks(FRenderContext* context,
                                    FRenderer&      renderer)
{
    if (!renderer.SetMeshletDepthEnabled(true))
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Meshlet解析] 无法启用光栅化 — 判据无法执行, 判定为失败");
        return false;
    }

    FMeshletDepthPass* const pass = renderer.GetMeshletDepthPass();
    FDepthPrePass* const     depthPass = renderer.GetDepthPrePass();

    if (pass == nullptr || depthPass == nullptr)
    {
        LIMX_LOG(LogLaunch, Error, "[Meshlet解析] 通道不存在");
        return false;
    }

    pass->SetResolveEnabled(true);

    bool passed = true;

    // 先渲一帧让汇总、实例表、可见表都建起来
    renderer.RenderFrame();

    const FRHIExtent2D extent = context->GetSwapchainExtent();

    const SizeType pixelCount =
        static_cast<SizeType>(extent.Width) * extent.Height;

    IRHIDevice* const device = context->GetDevice();

    // ---- 一帧里把解析结果、光栅化深度、经典法线一起读出来 ----
    //
    // 必须同一帧: 分几次读的话中间隔着帧的推进, 而可见记录表每帧重新压实,
    // 解析结果与深度就不是同一次光栅化的产物了。
    TArray<UInt8> resolveBytes;
    TArray<UInt8> depthBytes;
    TArray<UInt8> normalBytes;
    TArray<UInt8> visibilityBytes;
    TArray<UInt8> tableBytes;
    TArray<UInt8> classicDepthBytes;

    {
        FRHIBufferHandle resolveStaging;
        FRHIBufferHandle depthStaging;
        FRHIBufferHandle normalStaging;
        FRHIBufferHandle visibilityStaging;
        FRHIBufferHandle tableStaging;
        FRHIBufferHandle classicDepthStaging;

        FRHIBufferDesc desc = {};
        desc.Usage       = EBufferUsage::TransferDst;
        desc.MemoryUsage = EMemoryUsage::GpuToCpu;
        desc.DebugName   = "MeshletResolveCheck.Readback";

        desc.Size = static_cast<UInt64>(pixelCount) * sizeof(FResolveView);

        if (!IsRHISuccess(device->CreateBuffer(desc, resolveStaging)))
        {
            return false;
        }

        desc.Size = static_cast<UInt64>(pixelCount) * 4u;

        // 法线是 RG16_SFLOAT —— 每像素四字节, 与深度同宽
        const bool allocated =
            IsRHISuccess(device->CreateBuffer(desc, depthStaging)) &&
            IsRHISuccess(device->CreateBuffer(desc, normalStaging)) &&
            IsRHISuccess(device->CreateBuffer(desc, visibilityStaging)) &&
            IsRHISuccess(device->CreateBuffer(desc, classicDepthStaging));

        desc.Size = FMeshletCullPass::GetVisibleMeshletBytes();

        if (!allocated ||
            !IsRHISuccess(device->CreateBuffer(desc, tableStaging)))
        {
            device->DestroyBuffer(classicDepthStaging);
            device->DestroyBuffer(tableStaging);
            device->DestroyBuffer(visibilityStaging);
            device->DestroyBuffer(normalStaging);
            device->DestroyBuffer(depthStaging);
            device->DestroyBuffer(resolveStaging);
            return false;
        }

        const UInt32 frameIndex = context->GetCurrentFrameIndex();

        const FRHIBufferHandle resolveBuffer =
            pass->GetResolveBuffer(frameIndex);


        const FRHITextureHandle depthTexture = pass->GetDepthTexture();

        const FRHITextureHandle normalTexture = depthPass->GetNormalTexture();

        const FRHITextureHandle visibilityTexture =
            pass->GetVisibilityTexture();

        const FRHIBufferHandle table =
            renderer.GetMeshletCullPass()->GetVisibleMeshletBuffer(frameIndex);

        const FRHITextureHandle classicDepthTexture =
            depthPass->GetSharedDepthTexture();

        bool recorded = false;

        renderer.SetPostSceneRenderCallback(
            [&recorded, context, resolveBuffer, resolveStaging, depthTexture,
             depthStaging, normalTexture, normalStaging, visibilityTexture,
             visibilityStaging, table, tableStaging, classicDepthTexture,
             classicDepthStaging, extent, pixelCount]()
            {
                IRHICommandBuffer* cmd = context->GetCurrentCommandBuffer();

                if (cmd == nullptr)
                {
                    return;
                }

                FRHIBufferCopyRegion region = {};
                region.SrcOffset = 0;
                region.DstOffset = 0;
                region.Size =
                    static_cast<UInt64>(pixelCount) * sizeof(FResolveView);

                cmd->CopyBuffer(resolveBuffer, resolveStaging, region);

                FRHIBufferTextureCopyRegion imageRegion = {};
                imageRegion.BufferOffset      = 0;
                imageRegion.BufferRowLength   = 0;
                imageRegion.BufferImageHeight = 0;
                imageRegion.MipLevel          = 0;
                imageRegion.BaseLayer         = 0;
                imageRegion.LayerCount        = 1;
                imageRegion.TextureOffset     = { 0, 0, 0 };
                imageRegion.TextureExtent = { extent.Width, extent.Height, 1 };

                cmd->TransitionImageLayout(
                    depthTexture, EImageLayout::DepthStencilAttachment,
                    EImageLayout::TransferSrc,
                    EPipelineStageFlags::LateFragmentTests,
                    EPipelineStageFlags::Transfer,
                    EAccessFlags::DepthStencilAttachmentWrite,
                    EAccessFlags::TransferRead);

                cmd->CopyTextureToBuffer(depthTexture,
                                         EImageLayout::TransferSrc,
                                         depthStaging, imageRegion);

                cmd->TransitionImageLayout(
                    depthTexture, EImageLayout::TransferSrc,
                    EImageLayout::DepthStencilAttachment,
                    EPipelineStageFlags::Transfer,
                    EPipelineStageFlags::EarlyFragmentTests,
                    EAccessFlags::TransferRead,
                    EAccessFlags::DepthStencilAttachmentWrite);

                // ---- 经典 G-Buffer 的法线 ----
                cmd->TransitionImageLayout(
                    normalTexture, EImageLayout::ShaderReadOnly,
                    EImageLayout::TransferSrc,
                    EPipelineStageFlags::FragmentShader,
                    EPipelineStageFlags::Transfer,
                    EAccessFlags::ShaderRead, EAccessFlags::TransferRead);

                cmd->CopyTextureToBuffer(normalTexture,
                                         EImageLayout::TransferSrc,
                                         normalStaging, imageRegion);

                cmd->TransitionImageLayout(
                    normalTexture, EImageLayout::TransferSrc,
                    EImageLayout::ShaderReadOnly,
                    EPipelineStageFlags::Transfer,
                    EPipelineStageFlags::FragmentShader,
                    EAccessFlags::TransferRead, EAccessFlags::ShaderRead);

                // ---- 可见性编号与同一帧的可见记录表 ----
                cmd->TransitionImageLayout(
                    visibilityTexture, EImageLayout::ShaderReadOnly,
                    EImageLayout::TransferSrc,
                    EPipelineStageFlags::ColorAttachmentOutput,
                    EPipelineStageFlags::Transfer,
                    EAccessFlags::ColorAttachmentWrite,
                    EAccessFlags::TransferRead);

                cmd->CopyTextureToBuffer(visibilityTexture,
                                         EImageLayout::TransferSrc,
                                         visibilityStaging, imageRegion);

                cmd->TransitionImageLayout(
                    visibilityTexture, EImageLayout::TransferSrc,
                    EImageLayout::ShaderReadOnly,
                    EPipelineStageFlags::Transfer,
                    EPipelineStageFlags::ColorAttachmentOutput,
                    EAccessFlags::TransferRead,
                    EAccessFlags::ColorAttachmentWrite);

                FRHIBufferCopyRegion tableRegion = {};
                tableRegion.SrcOffset = 0;
                tableRegion.DstOffset = 0;
                tableRegion.Size = FMeshletCullPass::GetVisibleMeshletBytes();

                cmd->CopyBuffer(table, tableStaging, tableRegion);

                // ---- 经典深度预通道的深度 ----
                //
                // 法线只能在**两条路径画了同一个表面**的像素上比。蒙版材质
                // 那些地方经典路径挖了洞露出后面的面, 而 meshlet 路径直接
                // 画那个面 —— 两者的法线本来就该不同, 拿它们比是没有意义的。
                cmd->TransitionImageLayout(
                    classicDepthTexture,
                    EImageLayout::DepthStencilAttachment,
                    EImageLayout::TransferSrc,
                    EPipelineStageFlags::LateFragmentTests,
                    EPipelineStageFlags::Transfer,
                    EAccessFlags::DepthStencilAttachmentWrite,
                    EAccessFlags::TransferRead);

                cmd->CopyTextureToBuffer(classicDepthTexture,
                                         EImageLayout::TransferSrc,
                                         classicDepthStaging, imageRegion);

                cmd->TransitionImageLayout(
                    classicDepthTexture, EImageLayout::TransferSrc,
                    EImageLayout::DepthStencilAttachment,
                    EPipelineStageFlags::Transfer,
                    EPipelineStageFlags::EarlyFragmentTests,
                    EAccessFlags::TransferRead,
                    EAccessFlags::DepthStencilAttachmentWrite);

                recorded = true;
            });

        renderer.RenderFrame();
        renderer.SetPostSceneRenderCallback(TFunction<void()>());

        bool ok = recorded;

        if (ok)
        {
            device->WaitIdle();

            struct FStagingRead
            {
                FRHIBufferHandle Source;
                SizeType         Bytes;
                TArray<UInt8>*   Target;
            };

            const FStagingRead reads[6] = {
                { resolveStaging, pixelCount * sizeof(FResolveView),
                  &resolveBytes },
                { depthStaging, pixelCount * 4, &depthBytes },
                { normalStaging, pixelCount * 4, &normalBytes },
                { visibilityStaging, pixelCount * 4, &visibilityBytes },
                { tableStaging,
                  static_cast<SizeType>(
                      FMeshletCullPass::GetVisibleMeshletBytes()),
                  &tableBytes },
                { classicDepthStaging, pixelCount * 4, &classicDepthBytes },
            };

            for (UInt32 r = 0; r < 6; ++r)
            {
                void* mapped = nullptr;

                if (!IsRHISuccess(device->MapBuffer(reads[r].Source,
                                                    &mapped)) ||
                    mapped == nullptr)
                {
                    ok = false;
                    break;
                }

                const auto* source = static_cast<const UInt8*>(mapped);

                reads[r].Target->Reserve(reads[r].Bytes);

                for (SizeType i = 0; i < reads[r].Bytes; ++i)
                {
                    reads[r].Target->Add(source[i]);
                }

                device->UnmapBuffer(reads[r].Source);
            }
        }

        device->DestroyBuffer(classicDepthStaging);
        device->DestroyBuffer(tableStaging);
        device->DestroyBuffer(visibilityStaging);
        device->DestroyBuffer(normalStaging);
        device->DestroyBuffer(depthStaging);
        device->DestroyBuffer(resolveStaging);

        if (!ok)
        {
            LIMX_LOG(LogLaunch, Error, "[Meshlet解析] 回读失败");
            return false;
        }
    }

    const auto* resolve =
        reinterpret_cast<const FResolveView*>(resolveBytes.GetData());

    const auto* rasterDepth =
        reinterpret_cast<const Float32*>(depthBytes.GetData());

    // ---- 判据一: 重算的深度与光栅器写的吻合 ----
    SizeType covered      = 0;
    SizeType depthDiffer  = 0;
    Float32  worstUlps    = 0.0f;

    SizeType coverageMismatch = 0;

    for (SizeType i = 0; i < pixelCount; ++i)
    {
        const bool hasResolve = (resolve[i].Material != 0xFFFFFFFFu);
        const bool hasRaster  = (rasterDepth[i] < 1.0f);

        if (hasResolve != hasRaster)
        {
            ++coverageMismatch;
            continue;
        }

        if (!hasResolve)
        {
            continue;
        }

        ++covered;

        const Float32 delta =
            FMath::Abs(resolve[i].Depth - rasterDepth[i]);

        const Float32 ulps = delta / DepthUlp(rasterDepth[i]);

        worstUlps = FMath::Max(worstUlps, ulps);

        // 阈值 64 个最低位。
        //
        // 这个数不是随手取的: 重心坐标要经过两次除法与三次乘加, 每一步
        // 都在舍入。64 个 ULP 是那串运算的舍入上界的量级, 而"取错了相邻
        // 三角形"带来的深度差比它大好几个数量级 —— 相邻三角形在屏幕上
        // 差一个像素, 深度差是梯度乘一个像素, 那是 1e-4 量级, 而一个
        // ULP 在深度 0.99 处是 6e-8。
        //
        // 与光追深度那条判据取同一个数, 理由也是同一个。
        if (ulps > 64.0f)
        {
            ++depthDiffer;
        }
    }

    LIMX_LOG(LogLaunch, Display,
             "[Meshlet解析] 深度自洽 — 覆盖 {} 个像素, 超过 64 ULP 的 {} 个 "
             "(最大 {} ULP); 覆盖不一致 {} 个",
             covered, depthDiffer, worstUlps, coverageMismatch);

    constexpr SizeType kMinCovered = 50000;

    if (covered < kMinCovered)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Meshlet解析] 只覆盖了 {} 个像素 (需要至少 {}) —— "
                 "一张几乎空的解析结果会让所有判据形同虚设",
                 covered, kMinCovered);
        passed = false;
    }

    if (coverageMismatch != 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Meshlet解析] {} 个像素上解析与光栅化的覆盖不一致 —— "
                 "解析读的就是光栅化写的那张可见性图, 覆盖只能相同",
                 coverageMismatch);
        passed = false;
    }

    if (depthDiffer != 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Meshlet解析] {} 个像素上重算的深度与光栅器写的差超过 "
                 "64 ULP (最大 {}) —— 编号解错了、顶点取错了, 或者重心坐标"
                 "算错了",
                 depthDiffer, worstUlps);
        passed = false;
    }

    // ---- 判据二: 法线与经典 G-Buffer 吻合 ----
    //
    // 经典路径的法线由光栅器插值顶点法线再八面体编码; 解析路径手算重心
    // 坐标做同样的事。两者的差只该来自 RG16_SFLOAT 的量化。
    //
    // 这一条验的是**透视校正**: 属性必须用透视校正的权重插, 而深度必须用
    // 屏幕空间的权重。两者用同一套权重是这类代码最经典的错误 —— 而它在
    // 深度上只差一点点 (看起来像精度问题), 在法线上却差得明显。
    {
        const auto* normalSource =
            reinterpret_cast<const UInt16*>(normalBytes.GetData());

        const auto* classicDepth =
            reinterpret_cast<const Float32*>(classicDepthBytes.GetData());

        SizeType compared = 0;
        SizeType exceeded = 0;

        Float64 angleSum   = 0.0;
        Float32 worstAngle = 0.0f;

        for (SizeType i = 0; i < pixelCount; ++i)
        {
            if (resolve[i].Material == 0xFFFFFFFFu)
            {
                continue;
            }

            const Float32 classicX =
                Float16ToFloat32(normalSource[i * 2 + 0]);
            const Float32 classicY =
                Float16ToFloat32(normalSource[i * 2 + 1]);

            // 哨兵 (2,2) = 经典路径在这里没有几何体。
            //
            // meshlet 路径只画不透明批次而经典 G-Buffer 画不透明加蒙版,
            // 所以反过来 (经典有而 meshlet 没有) 是正常的; 但 meshlet 有
            // 而经典没有则不可能 —— 那时跳过而不是当成误差, 因为哨兵不是
            // 一个法线。
            if (classicX > 1.5f || classicY > 1.5f)
            {
                continue;
            }

            // 只在**两条路径画了同一个表面**的像素上比。
            //
            // 蒙版材质那些地方经典路径做 alpha 测试挖了洞、露出后面的
            // 不透明面, 而 meshlet 路径 (只画不透明) 直接画那个面 ——
            // 两者的法线本来就该不同。不排除的话实测 2.6% 的像素超差,
            // 而那 2.6% 恰好是 Day 10 量到的蒙版板子的屏幕面积。
            //
            // 判据要问的是"同一个三角形上两条路径插出来的法线一样吗",
            // 拿不同的三角形去比是在问另一个问题。
            if (rasterDepth[i] != classicDepth[i])
            {
                continue;
            }

            ++compared;

            const FVector3 a =
                DecodeOctahedral(resolve[i].NormalX, resolve[i].NormalY);

            const FVector3 b = DecodeOctahedral(classicX, classicY);

            const Float32 dot =
                FMath::Clamp(a.X * b.X + a.Y * b.Y + a.Z * b.Z, -1.0f, 1.0f);

            const Float32 angle = FMath::RadiansToDegrees(FMath::ACos(dot));

            angleSum += static_cast<Float64>(angle);

            worstAngle = FMath::Max(worstAngle, angle);

            // 阈值 2 度。
            //
            // RG16_SFLOAT 的八面体编码本身约 0.03 度; 两条路径的插值权重
            // 在浮点上略有不同, 再加上三角形边缘上一个像素的中心可能落在
            // 两个三角形的公共边附近 —— 那时两边解出的法线本来就不同。
            // 2 度足够松到不被这些噪声顶红, 又足够紧到"用错了权重"
            // (实测差十几度) 一定会红。
            if (angle > 2.0f)
            {
                ++exceeded;
            }
        }

        const Float64 meanAngle =
            (compared > 0) ? (angleSum / static_cast<Float64>(compared)) : 0.0;

        LIMX_LOG(LogLaunch, Display,
                 "[Meshlet解析] 法线 — 比了 {} 个像素, 平均夹角 {} 度, "
                 "最大 {} 度, 超过 2 度的 {} 个",
                 compared, meanAngle, worstAngle, exceeded);

        if (compared < kMinCovered)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[Meshlet解析] 只有 {} 个像素能与经典法线比 "
                     "(需要至少 {})", compared, kMinCovered);
            passed = false;
        }

        // 允许千分之一的像素超差 —— 三角形公共边上的像素两边法线本来就
        // 不同, 而那与"权重用错了"在数量级上差着三个量级 (实测 0.0002)。
        const Float32 exceededFraction =
            (compared > 0) ? (static_cast<Float32>(exceeded) /
                              static_cast<Float32>(compared))
                           : 1.0f;

        if (exceededFraction > 0.001f)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[Meshlet解析] {} 的像素法线与经典 G-Buffer 差超过 2 度 "
                     "(上限 0.001) —— 属性插值没做透视校正?",
                     exceededFraction);
            passed = false;
        }
    }

    // ---- 判据三: 插值出来的世界坐标投回屏幕必须落在这个像素上 ----
    //
    // 这一条与场景无关, 而且**只有透视校正的权重满足它**。屏幕空间权重
    // 插出来的点也在三角形平面上, 但不是这个像素看到的那个点 —— 投回去
    // 会落在别处, 偏多少取决于三角形跨了多大的深度。
    //
    // 它是判据逼出来的: 只比法线的话, "属性不做透视校正"这条变异逃逸了。
    // 综合场景里法线变化大的三角形 (球) 都很小, 跨深度大的三角形 (地面)
    // 三个顶点的法线又相同 —— 于是任何权重插出来都一样。而世界坐标在
    // **每个**三角形上都随位置变, 这条判据处处有效。
    {
        const FCamera& camera = renderer.GetCamera();

        const FMatrix viewProj =
            camera.GetProjectionMatrix() * camera.GetViewMatrix();

        SizeType compared = 0;
        SizeType exceeded = 0;

        Float32 worstPixels = 0.0f;

        for (SizeType i = 0; i < pixelCount; ++i)
        {
            if (resolve[i].Material == 0xFFFFFFFFu)
            {
                continue;
            }

            ++compared;

            const Float32 x = resolve[i].WorldX;
            const Float32 y = resolve[i].WorldY;
            const Float32 z = resolve[i].WorldZ;

            // FMatrix 是行主序 M[行][列]; 列向量语义下 clip = M * p
            const Float32 clipX = viewProj.M[0][0] * x + viewProj.M[0][1] * y +
                                  viewProj.M[0][2] * z + viewProj.M[0][3];
            const Float32 clipY = viewProj.M[1][0] * x + viewProj.M[1][1] * y +
                                  viewProj.M[1][2] * z + viewProj.M[1][3];
            const Float32 clipW = viewProj.M[3][0] * x + viewProj.M[3][1] * y +
                                  viewProj.M[3][2] * z + viewProj.M[3][3];

            if (FMath::Abs(clipW) < 1.0e-9f)
            {
                continue;
            }

            const Float32 screenX =
                ((clipX / clipW) * 0.5f + 0.5f) *
                static_cast<Float32>(extent.Width);

            const Float32 screenY =
                ((clipY / clipW) * 0.5f + 0.5f) *
                static_cast<Float32>(extent.Height);

            const SizeType px = i % extent.Width;
            const SizeType py = i / extent.Width;

            const Float32 dx = screenX - (static_cast<Float32>(px) + 0.5f);
            const Float32 dy = screenY - (static_cast<Float32>(py) + 0.5f);

            const Float32 distance = FMath::Sqrt(dx * dx + dy * dy);

            worstPixels = FMath::Max(worstPixels, distance);

            // 阈值 0.05 个像素。
            //
            // 完美的话是 0 —— 但插值、矩阵乘法与投影各自舍入, 而远处的
            // 三角形上一点点世界坐标误差会被投影放大。实测最大 **0.0005**
            // 个像素, 而不做透视校正时是几十个像素 —— 两者差着五个量级,
            // 阈值取在哪都行, 取 0.05 只是留个整齐的余量。
            if (distance > 0.05f)
            {
                ++exceeded;
            }
        }

        LIMX_LOG(LogLaunch, Display,
                 "[Meshlet解析] 世界坐标投回屏幕 — 比了 {} 个像素, "
                 "最大偏离 {} 个像素, 超过 0.05 的 {} 个",
                 compared, worstPixels, exceeded);

        if (exceeded != 0)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[Meshlet解析] {} 个像素上插值出来的世界坐标投回屏幕"
                     "落在别处 (最大偏离 {} 个像素) —— 属性插值没做透视校正?",
                     exceeded, worstPixels);
            passed = false;
        }
    }

    // ---- 判据四: 材质下标按**源对象下标**查, 不是按实例序号 ----
    //
    // 实例表里的 MeshletRange[2] 存的是源对象下标。用实例序号去索引对象
    // 列表的话, 只要有一个对象被跳过 (半透明、无 meshlet), 后面所有物体
    // 的材质就整体错位一格 —— 与 Day 5 光追几何表踩过的是同一个坑。
    //
    // 判据在 CPU 上独立走一遍同一条链: 可见性编号 -> 槽位 -> 可见记录 ->
    // 实例 -> 源对象 -> 材质下标, 与着色器写出来的比。
    {
        const auto* visibility =
            reinterpret_cast<const UInt32*>(visibilityBytes.GetData());

        const auto* table =
            reinterpret_cast<const UInt32*>(tableBytes.GetData());

        const TArray<FMeshletInstanceGpu>& instances =
            renderer.GetMeshletCullPass()->GetInstances();

        const TArray<FRenderObject>& objects =
            renderer.GetShadowCasterObjects();

        SizeType compared    = 0;
        SizeType mismatched  = 0;
        SizeType firstBad    = 0;

        TSet<UInt32> distinctMaterials;

        for (SizeType i = 0; i < pixelCount; ++i)
        {
            if (visibility[i] == 0u || resolve[i].Material == 0xFFFFFFFFu)
            {
                continue;
            }

            const UInt32 slot = (visibility[i] - 1u) >> 7;

            const UInt32 instanceIndex = table[slot * 2 + 0];

            if (instanceIndex >= instances.GetSize())
            {
                continue;
            }

            const UInt32 source = instances[instanceIndex].MeshletRange[2];

            if (source >= objects.GetSize())
            {
                continue;
            }

            ++compared;

            distinctMaterials.Add(resolve[i].Material);

            if (resolve[i].Material != objects[source].BindlessMaterialIndex)
            {
                if (mismatched == 0)
                {
                    firstBad = i;
                }

                ++mismatched;
            }
        }

        LIMX_LOG(LogLaunch, Display,
                 "[Meshlet解析] 材质 — 比了 {} 个像素, 不符 {} 个, "
                 "不同的材质下标 {} 个",
                 compared, mismatched, distinctMaterials.GetSize());

        if (mismatched != 0)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[Meshlet解析] {} 个像素的材质下标与源对象不符 "
                     "(第一个在 {}: 解析 {}), 按实例序号查的?",
                     mismatched, firstBad, resolve[firstBad].Material);
            passed = false;
        }

        // 元判据: 场景里得有多种材质。
        //
        // 只有一种的话, "材质查错了"与"查对了"给出同一个数 —— 这条判据
        // 就是空的。本周期在光追几何表上栽过同一件事。
        if (distinctMaterials.GetSize() < 2)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[Meshlet解析] 解析出来只有 {} 种材质下标 —— "
                     "这个场景判不了材质查得对不对",
                     distinctMaterials.GetSize());
            passed = false;
        }
    }

    if (!renderer.SetMeshletDepthEnabled(false))
    {
        LIMX_LOG(LogLaunch, Error, "[Meshlet解析] 无法关闭 (复位)");
        passed = false;
    }

    pass->SetResolveEnabled(false);

    LIMX_LOG(LogLaunch, Display, "[Meshlet解析] {}", passed ? "通过" : "失败");

    return passed;
}

// ============================================================================
// RunMeshletDepthChecks — 两条光栅化路径, 与经典深度预通道的关系
//
// 这条判据补的是 Day 9 明写下来的欠账: 那一天的判据全是数值判据 (GPU 剔出
// 来的集合与 CPU 参考实现相同), 而数值判据**证明不了画面**。今天 meshlet
// 真的被光栅化了, 于是可以问画面。
//
// 三条判据:
//
//   一、网格着色器路径与计算展开回退路径画出的深度**逐位相同**。
//
//      这一条是今天最强的。两条路径的输入完全一样 (同一份可见 meshlet 表、
//      同一份场景数据), 顶点数学是同一份源码 (meshlet_raster_common.h,
//      连运算顺序都钉死), 光栅化状态逐字段相同。剩下的差别只有"怎么把
//      三角形喂给光栅器" —— 而那正是要验的东西。
//
//      不留容差。留了的话, "局部索引解包错了一位"这类缺陷会藏在容差里:
//      取错顶点画出来的深度未必差很多, 尤其在一个 meshlet 内部。
//
//   二、每个像素上 meshlet 路径的深度 >= 经典深度预通道的深度。
//
//      meshlet 路径只画不透明批次, 而经典路径画不透明**加**蒙版 —— 前者
//      的三角形集合是后者的子集。少画三角形只能让深度变远或者不变。
//
//      这一条也不留容差, 而它能不留是因为两条路径的顶点变换是同一个
//      表达式的同一种写法: (viewProj * model) * p。写成别的等价形式的话
//      浮点结果会差一两个最低位, 于是这条判据要么永远红、要么被迫加容差
//      而失去意义。
//
//   三、两者**恰好相等**的像素要占绝大多数。
//
//      只有前两条的话, 一个什么都不画的实现完美通过: 空深度图恒为 1.0,
//      处处 >= 经典深度。这一条盯的正是那个 —— meshlet 路径必须真的画出
//      了那些表面。
// ============================================================================

namespace
{

/// 把一张 D32_SFLOAT 深度图读回 CPU
bool ReadDepthTexture(FRenderContext* context, FRenderer& renderer,
                      FRHITextureHandle texture, EImageLayout restingLayout,
                      TArray<Float32>& outDepth)
{
    if (!texture.IsValid())
    {
        return false;
    }

    IRHIDevice* const device = context->GetDevice();

    const FRHIExtent2D extent = context->GetSwapchainExtent();

    const SizeType pixelCount =
        static_cast<SizeType>(extent.Width) * extent.Height;

    FRHIBufferDesc desc = {};
    desc.Usage       = EBufferUsage::TransferDst;
    desc.MemoryUsage = EMemoryUsage::GpuToCpu;
    desc.Size        = pixelCount * 4u;
    desc.DebugName   = "MeshletDepthCheck.Readback";

    FRHIBufferHandle readback;

    if (!IsRHISuccess(device->CreateBuffer(desc, readback)))
    {
        return false;
    }

    bool recorded = false;

    renderer.SetPostSceneRenderCallback(
        [&recorded, context, texture, readback, extent, restingLayout]()
        {
            IRHICommandBuffer* cmd = context->GetCurrentCommandBuffer();

            if (cmd == nullptr)
            {
                return;
            }

            FRHIBufferTextureCopyRegion region = {};
            region.BufferOffset      = 0;
            region.BufferRowLength   = 0;
            region.BufferImageHeight = 0;
            region.MipLevel          = 0;
            region.BaseLayer         = 0;
            region.LayerCount        = 1;
            region.TextureOffset     = { 0, 0, 0 };
            region.TextureExtent     = { extent.Width, extent.Height, 1 };

            cmd->TransitionImageLayout(
                texture, restingLayout, EImageLayout::TransferSrc,
                EPipelineStageFlags::LateFragmentTests,
                EPipelineStageFlags::Transfer,
                EAccessFlags::DepthStencilAttachmentWrite,
                EAccessFlags::TransferRead);

            cmd->CopyTextureToBuffer(texture, EImageLayout::TransferSrc,
                                     readback, region);

            cmd->TransitionImageLayout(
                texture, EImageLayout::TransferSrc, restingLayout,
                EPipelineStageFlags::Transfer,
                EPipelineStageFlags::EarlyFragmentTests,
                EAccessFlags::TransferRead,
                EAccessFlags::DepthStencilAttachmentWrite);

            recorded = true;
        });

    renderer.RenderFrame();
    renderer.SetPostSceneRenderCallback(TFunction<void()>());

    bool ok = recorded;

    if (ok)
    {
        device->WaitIdle();

        void* mapped = nullptr;

        if (IsRHISuccess(device->MapBuffer(readback, &mapped)) &&
            mapped != nullptr)
        {
            const auto* values = static_cast<const Float32*>(mapped);

            outDepth.Clear();
            outDepth.Reserve(pixelCount);

            for (SizeType i = 0; i < pixelCount; ++i)
            {
                outDepth.Add(values[i]);
            }

            device->UnmapBuffer(readback);
        }
        else
        {
            ok = false;
        }
    }

    device->DestroyBuffer(readback);

    return ok;
}

/// 一个像素上解出来的三元组
///
/// **不能直接比编号本身。** 编号里的"可见槽位"来自剔除通道的原子追加,
/// 而原子追加的顺序每帧都不同 —— 同一个 (实例, meshlet) 在两次采集里
/// 会落在不同的槽位上。实测综合场景 817036 个像素的编号不同, 而它们
/// 画的是同一批三角形。
///
/// 能比的是槽位**解出来**的东西: 实例下标、meshlet 全局下标、三角形序号。
/// 那三个数是几何本身的属性, 与压实顺序无关。
struct FVisibilityTriple
{
    UInt32 Instance = 0;
    UInt32 Meshlet  = 0;
    UInt32 Triangle = 0;
    UInt32 Valid    = 0;
};

/// 一次采集: 可见性缓冲区 + 同一帧的可见记录表
///
/// 两者必须来自**同一帧** —— 分两次读的话中间隔着帧的推进, 表里已经是
/// 另一次压实的结果, 解出来的三元组会张冠李戴。所以两次拷贝录在同一个
/// 帧内回调里。
bool ReadVisibilityAndTable(FRenderContext* context, FRenderer& renderer,
                            FRHITextureHandle texture,
                            TArray<FVisibilityTriple>& outTriples)
{
    FMeshletCullPass* const cull = renderer.GetMeshletCullPass();

    if (!texture.IsValid() || cull == nullptr)
    {
        return false;
    }

    IRHIDevice* const device = context->GetDevice();

    const FRHIExtent2D extent = context->GetSwapchainExtent();

    const SizeType pixelCount =
        static_cast<SizeType>(extent.Width) * extent.Height;

    const UInt32 frameIndex = context->GetCurrentFrameIndex();

    FRHIBufferHandle visReadback;
    FRHIBufferHandle tableReadback;

    {
        FRHIBufferDesc desc = {};
        desc.Usage       = EBufferUsage::TransferDst;
        desc.MemoryUsage = EMemoryUsage::GpuToCpu;
        desc.DebugName   = "MeshletVisCheck.Readback";

        desc.Size = pixelCount * 4u;

        if (!IsRHISuccess(device->CreateBuffer(desc, visReadback)))
        {
            return false;
        }

        desc.Size = FMeshletCullPass::GetVisibleMeshletBytes();

        if (!IsRHISuccess(device->CreateBuffer(desc, tableReadback)))
        {
            device->DestroyBuffer(visReadback);
            return false;
        }
    }

    const FRHIBufferHandle table = cull->GetVisibleMeshletBuffer(frameIndex);

    bool recorded = false;

    renderer.SetPostSceneRenderCallback(
        [&recorded, context, texture, visReadback, tableReadback, table,
         extent]()
        {
            IRHICommandBuffer* cmd = context->GetCurrentCommandBuffer();

            if (cmd == nullptr)
            {
                return;
            }

            FRHIBufferTextureCopyRegion region = {};
            region.BufferOffset      = 0;
            region.BufferRowLength   = 0;
            region.BufferImageHeight = 0;
            region.MipLevel          = 0;
            region.BaseLayer         = 0;
            region.LayerCount        = 1;
            region.TextureOffset     = { 0, 0, 0 };
            region.TextureExtent     = { extent.Width, extent.Height, 1 };

            cmd->TransitionImageLayout(
                texture, EImageLayout::ShaderReadOnly,
                EImageLayout::TransferSrc,
                EPipelineStageFlags::ColorAttachmentOutput,
                EPipelineStageFlags::Transfer,
                EAccessFlags::ColorAttachmentWrite,
                EAccessFlags::TransferRead);

            cmd->CopyTextureToBuffer(texture, EImageLayout::TransferSrc,
                                     visReadback, region);

            cmd->TransitionImageLayout(
                texture, EImageLayout::TransferSrc,
                EImageLayout::ShaderReadOnly,
                EPipelineStageFlags::Transfer,
                EPipelineStageFlags::ColorAttachmentOutput,
                EAccessFlags::TransferRead,
                EAccessFlags::ColorAttachmentWrite);

            FRHIBufferCopyRegion tableRegion = {};
            tableRegion.SrcOffset = 0;
            tableRegion.DstOffset = 0;
            tableRegion.Size = FMeshletCullPass::GetVisibleMeshletBytes();

            cmd->CopyBuffer(table, tableReadback, tableRegion);

            recorded = true;
        });

    renderer.RenderFrame();
    renderer.SetPostSceneRenderCallback(TFunction<void()>());

    bool ok = recorded;

    if (ok)
    {
        device->WaitIdle();

        void* visMapped   = nullptr;
        void* tableMapped = nullptr;

        if (IsRHISuccess(device->MapBuffer(visReadback, &visMapped)) &&
            IsRHISuccess(device->MapBuffer(tableReadback, &tableMapped)) &&
            visMapped != nullptr && tableMapped != nullptr)
        {
            const auto* values = static_cast<const UInt32*>(visMapped);
            const auto* pairs  = static_cast<const UInt32*>(tableMapped);

            outTriples.Clear();
            outTriples.Reserve(pixelCount);

            for (SizeType i = 0; i < pixelCount; ++i)
            {
                FVisibilityTriple triple;

                if (values[i] != 0u)
                {
                    const UInt32 packed = values[i] - 1u;

                    const UInt32 slot = packed >> 7;

                    triple.Valid    = 1;
                    triple.Triangle = packed & 127u;
                    triple.Instance = pairs[slot * 2 + 0];
                    triple.Meshlet  = pairs[slot * 2 + 1];
                }

                outTriples.Add(triple);
            }

            device->UnmapBuffer(tableReadback);
            device->UnmapBuffer(visReadback);
        }
        else
        {
            ok = false;
        }
    }

    device->DestroyBuffer(tableReadback);
    device->DestroyBuffer(visReadback);

    return ok;
}

/// 把金字塔的某一级读回 CPU
///
/// 逐级读而不是一次读整张: mip 链在显存里不是连续的一块, 而
/// CopyTextureToBuffer 一次只能拷一级。
bool ReadHizLevel(FRenderContext* context, FRenderer& renderer,
                  FRHITextureHandle texture, FRHIExtent2D baseExtent,
                  UInt32 level, UInt32 levelCount, TArray<Float32>& outValues)
{
    if (!texture.IsValid() || level >= levelCount)
    {
        return false;
    }

    IRHIDevice* const device = context->GetDevice();

    const UInt32 width  = FMath::Max(baseExtent.Width >> level, 1u);
    const UInt32 height = FMath::Max(baseExtent.Height >> level, 1u);

    const SizeType texelCount = static_cast<SizeType>(width) * height;

    FRHIBufferDesc desc = {};
    desc.Usage       = EBufferUsage::TransferDst;
    desc.MemoryUsage = EMemoryUsage::GpuToCpu;
    desc.Size        = texelCount * 4u;
    desc.DebugName   = "MeshletHizCheck.Readback";

    FRHIBufferHandle readback;

    if (!IsRHISuccess(device->CreateBuffer(desc, readback)))
    {
        return false;
    }

    bool recorded = false;

    renderer.SetPostSceneRenderCallback(
        [&recorded, context, texture, readback, width, height, level,
         levelCount]()
        {
            IRHICommandBuffer* cmd = context->GetCurrentCommandBuffer();

            if (cmd == nullptr)
            {
                return;
            }

            FRHIBufferTextureCopyRegion region = {};
            region.BufferOffset      = 0;
            region.BufferRowLength   = 0;
            region.BufferImageHeight = 0;
            region.MipLevel          = level;
            region.BaseLayer         = 0;
            region.LayerCount        = 1;
            region.TextureOffset     = { 0, 0, 0 };
            region.TextureExtent     = { width, height, 1 };

            cmd->TransitionImageLayout(
                texture, EImageLayout::ShaderReadOnly,
                EImageLayout::TransferSrc, EPipelineStageFlags::ComputeShader,
                EPipelineStageFlags::Transfer, EAccessFlags::ShaderRead,
                EAccessFlags::TransferRead, 0, levelCount);

            cmd->CopyTextureToBuffer(texture, EImageLayout::TransferSrc,
                                     readback, region);

            cmd->TransitionImageLayout(
                texture, EImageLayout::TransferSrc,
                EImageLayout::ShaderReadOnly, EPipelineStageFlags::Transfer,
                EPipelineStageFlags::ComputeShader,
                EAccessFlags::TransferRead, EAccessFlags::ShaderRead, 0,
                levelCount);

            recorded = true;
        });

    renderer.RenderFrame();
    renderer.SetPostSceneRenderCallback(TFunction<void()>());

    bool ok = recorded;

    if (ok)
    {
        device->WaitIdle();

        void* mapped = nullptr;

        if (IsRHISuccess(device->MapBuffer(readback, &mapped)) &&
            mapped != nullptr)
        {
            const auto* values = static_cast<const Float32*>(mapped);

            outValues.Clear();
            outValues.Reserve(texelCount);

            for (SizeType i = 0; i < texelCount; ++i)
            {
                outValues.Add(values[i]);
            }

            device->UnmapBuffer(readback);
        }
        else
        {
            ok = false;
        }
    }

    device->DestroyBuffer(readback);

    return ok;
}

/// 一帧里同时取深度、可见性与可见记录表
///
/// 分三次读是不行的 —— 每次读都会**渲一帧**, 而遮挡剔除的判据要看的正是
/// 视角跳变的**那一帧**: 那一帧的第一阶段用的是上一个视角的金字塔。
/// 分三次的话第一次看到的是跳变帧, 第二次看到的已经是金字塔重建之后的
/// 稳定帧了 —— 两者不是同一次光栅化, 而判据比的是它们。
///
/// 这个坑很隐蔽: 判据全绿, 而"第二阶段补回来了几个"恒为 0。绿的原因不是
/// 实现对, 是**取样取晚了**。
bool CaptureDepthAndVisibility(FRenderContext* context, FRenderer& renderer,
                               FRHITextureHandle depthTexture,
                               FRHITextureHandle visibilityTexture,
                               TArray<Float32>& outDepth,
                               TArray<FVisibilityTriple>& outTriples)
{
    FMeshletCullPass* const cull = renderer.GetMeshletCullPass();

    if (!depthTexture.IsValid() || !visibilityTexture.IsValid() ||
        cull == nullptr)
    {
        return false;
    }

    IRHIDevice* const device = context->GetDevice();

    const FRHIExtent2D extent = context->GetSwapchainExtent();

    const SizeType pixelCount =
        static_cast<SizeType>(extent.Width) * extent.Height;

    const UInt32 frameIndex = context->GetCurrentFrameIndex();

    FRHIBufferHandle depthStaging;
    FRHIBufferHandle visStaging;
    FRHIBufferHandle tableStaging;

    {
        FRHIBufferDesc desc = {};
        desc.Usage       = EBufferUsage::TransferDst;
        desc.MemoryUsage = EMemoryUsage::GpuToCpu;
        desc.DebugName   = "MeshletOcclusionCheck.Readback";

        desc.Size = pixelCount * 4u;

        const bool ok =
            IsRHISuccess(device->CreateBuffer(desc, depthStaging)) &&
            IsRHISuccess(device->CreateBuffer(desc, visStaging));

        desc.Size = FMeshletCullPass::GetVisibleMeshletBytes();

        if (!ok || !IsRHISuccess(device->CreateBuffer(desc, tableStaging)))
        {
            device->DestroyBuffer(tableStaging);
            device->DestroyBuffer(visStaging);
            device->DestroyBuffer(depthStaging);
            return false;
        }
    }

    const FRHIBufferHandle table = cull->GetVisibleMeshletBuffer(frameIndex);

    bool recorded = false;

    renderer.SetPostSceneRenderCallback(
        [&recorded, context, depthTexture, depthStaging, visibilityTexture,
         visStaging, table, tableStaging, extent]()
        {
            IRHICommandBuffer* cmd = context->GetCurrentCommandBuffer();

            if (cmd == nullptr)
            {
                return;
            }

            FRHIBufferTextureCopyRegion region = {};
            region.BufferOffset      = 0;
            region.BufferRowLength   = 0;
            region.BufferImageHeight = 0;
            region.MipLevel          = 0;
            region.BaseLayer         = 0;
            region.LayerCount        = 1;
            region.TextureOffset     = { 0, 0, 0 };
            region.TextureExtent     = { extent.Width, extent.Height, 1 };

            cmd->TransitionImageLayout(
                depthTexture, EImageLayout::DepthStencilAttachment,
                EImageLayout::TransferSrc,
                EPipelineStageFlags::LateFragmentTests,
                EPipelineStageFlags::Transfer,
                EAccessFlags::DepthStencilAttachmentWrite,
                EAccessFlags::TransferRead);

            cmd->CopyTextureToBuffer(depthTexture, EImageLayout::TransferSrc,
                                     depthStaging, region);

            cmd->TransitionImageLayout(
                depthTexture, EImageLayout::TransferSrc,
                EImageLayout::DepthStencilAttachment,
                EPipelineStageFlags::Transfer,
                EPipelineStageFlags::EarlyFragmentTests,
                EAccessFlags::TransferRead,
                EAccessFlags::DepthStencilAttachmentWrite);

            cmd->TransitionImageLayout(
                visibilityTexture, EImageLayout::ShaderReadOnly,
                EImageLayout::TransferSrc,
                EPipelineStageFlags::ColorAttachmentOutput,
                EPipelineStageFlags::Transfer,
                EAccessFlags::ColorAttachmentWrite,
                EAccessFlags::TransferRead);

            cmd->CopyTextureToBuffer(visibilityTexture,
                                     EImageLayout::TransferSrc, visStaging,
                                     region);

            cmd->TransitionImageLayout(
                visibilityTexture, EImageLayout::TransferSrc,
                EImageLayout::ShaderReadOnly, EPipelineStageFlags::Transfer,
                EPipelineStageFlags::ColorAttachmentOutput,
                EAccessFlags::TransferRead,
                EAccessFlags::ColorAttachmentWrite);

            FRHIBufferCopyRegion tableRegion = {};
            tableRegion.SrcOffset = 0;
            tableRegion.DstOffset = 0;
            tableRegion.Size = FMeshletCullPass::GetVisibleMeshletBytes();

            cmd->CopyBuffer(table, tableStaging, tableRegion);

            recorded = true;
        });

    renderer.RenderFrame();
    renderer.SetPostSceneRenderCallback(TFunction<void()>());

    bool ok = recorded;

    if (ok)
    {
        device->WaitIdle();

        void* depthMapped = nullptr;
        void* visMapped   = nullptr;
        void* tableMapped = nullptr;

        if (IsRHISuccess(device->MapBuffer(depthStaging, &depthMapped)) &&
            IsRHISuccess(device->MapBuffer(visStaging, &visMapped)) &&
            IsRHISuccess(device->MapBuffer(tableStaging, &tableMapped)) &&
            depthMapped != nullptr && visMapped != nullptr &&
            tableMapped != nullptr)
        {
            const auto* depthValues = static_cast<const Float32*>(depthMapped);
            const auto* visValues   = static_cast<const UInt32*>(visMapped);
            const auto* pairs       = static_cast<const UInt32*>(tableMapped);

            outDepth.Clear();
            outTriples.Clear();

            outDepth.Reserve(pixelCount);
            outTriples.Reserve(pixelCount);

            for (SizeType i = 0; i < pixelCount; ++i)
            {
                outDepth.Add(depthValues[i]);

                FVisibilityTriple triple;

                if (visValues[i] != 0u)
                {
                    const UInt32 packed = visValues[i] - 1u;

                    const UInt32 slot = packed >> 7;

                    triple.Valid    = 1;
                    triple.Triangle = packed & 127u;
                    triple.Instance = pairs[slot * 2 + 0];
                    triple.Meshlet  = pairs[slot * 2 + 1];
                }

                outTriples.Add(triple);
            }

            device->UnmapBuffer(tableStaging);
            device->UnmapBuffer(visStaging);
            device->UnmapBuffer(depthStaging);
        }
        else
        {
            ok = false;
        }
    }

    device->DestroyBuffer(tableStaging);
    device->DestroyBuffer(visStaging);
    device->DestroyBuffer(depthStaging);

    return ok;
}

} // namespace

static bool RunMeshletDepthChecks(FRenderContext* context, FRenderer& renderer)
{
    if (!renderer.SetMeshletDepthEnabled(true))
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Meshlet深度] 无法启用 — 判据无法执行, 判定为失败");
        return false;
    }

    FMeshletDepthPass* const pass = renderer.GetMeshletDepthPass();
    FDepthPrePass* const     classic = renderer.GetDepthPrePass();

    if (pass == nullptr || classic == nullptr)
    {
        LIMX_LOG(LogLaunch, Error, "[Meshlet深度] 通道不存在");
        return false;
    }

    bool passed = true;

    TArray<Float32> meshDepth;
    TArray<Float32> fallbackDepth;
    TArray<Float32> classicDepth;

    TArray<FVisibilityTriple> meshVisibility;
    TArray<FVisibilityTriple> fallbackVisibility;

    // ---- 网格着色器路径 ----
    //
    // 设备不支持时这一段跳过, 但**判据不因此变绿** —— 见下面那条元判据。
    const bool meshShaderAvailable = pass->IsMeshShaderAvailable();

    if (meshShaderAvailable)
    {
        if (!pass->SetMode(FMeshletDepthPass::EMode::MeshShader))
        {
            LIMX_LOG(LogLaunch, Error, "[Meshlet深度] 无法切到网格着色器路径");
            return false;
        }

        // 先渲一帧让汇总与可见表建起来, 再读
        renderer.RenderFrame();

        if (!ReadDepthTexture(context, renderer, pass->GetDepthTexture(),
                              EImageLayout::DepthStencilAttachment,
                              meshDepth) ||
            !ReadVisibilityAndTable(context, renderer,
                                    pass->GetVisibilityTexture(),
                                    meshVisibility))
        {
            LIMX_LOG(LogLaunch, Error, "[Meshlet深度] 网格着色器路径回读失败");
            return false;
        }
    }

    // ---- 回退路径 ----
    if (!pass->SetMode(FMeshletDepthPass::EMode::Fallback))
    {
        LIMX_LOG(LogLaunch, Error, "[Meshlet深度] 无法切到回退路径");
        return false;
    }

    renderer.RenderFrame();

    if (!ReadDepthTexture(context, renderer, pass->GetDepthTexture(),
                          EImageLayout::DepthStencilAttachment,
                          fallbackDepth) ||
        !ReadVisibilityAndTable(context, renderer,
                                pass->GetVisibilityTexture(),
                                fallbackVisibility))
    {
        LIMX_LOG(LogLaunch, Error, "[Meshlet深度] 回退路径回读失败");
        return false;
    }

    // ---- 经典深度预通道 ----
    if (!ReadDepthTexture(context, renderer, classic->GetSharedDepthTexture(),
                          EImageLayout::DepthStencilAttachment, classicDepth))
    {
        LIMX_LOG(LogLaunch, Error, "[Meshlet深度] 经典深度回读失败");
        return false;
    }

    const SizeType pixelCount = fallbackDepth.GetSize();

    if (pixelCount == 0 || classicDepth.GetSize() != pixelCount)
    {
        LIMX_LOG(LogLaunch, Error, "[Meshlet深度] 回读尺寸对不上");
        return false;
    }

    // ---- 判据一: 两条路径逐位相同 ----
    if (meshShaderAvailable)
    {
        if (meshDepth.GetSize() != pixelCount)
        {
            LIMX_LOG(LogLaunch, Error, "[Meshlet深度] 两条路径的尺寸对不上");
            passed = false;
        }
        else
        {
            SizeType differ = 0;
            Float32  worst  = 0.0f;

            for (SizeType i = 0; i < pixelCount; ++i)
            {
                const Float32 delta =
                    FMath::Abs(meshDepth[i] - fallbackDepth[i]);

                if (delta != 0.0f)
                {
                    ++differ;
                    worst = FMath::Max(worst, delta);
                }
            }

            LIMX_LOG(LogLaunch, Display,
                     "[Meshlet深度] 网格着色器 vs 计算展开回退 — 不同的像素 "
                     "{} 个 (最大差 {})",
                     differ, worst);

            if (differ != 0)
            {
                LIMX_LOG(LogLaunch, Error,
                         "[Meshlet深度] 两条路径画出的深度不同 ({} 个像素, "
                         "最大差 {}) —— 它们的输入、顶点数学、光栅化状态都是"
                         "同一份, 差别只该在把三角形喂给光栅器的方式上",
                         differ, worst);
                passed = false;
            }
        }

        // ---- 可见性缓冲区也要逐位相同 ----
        //
        // 深度相同而编号不同是完全可能的: 两个共面的三角形画出同一个深度,
        // 而编号不同。那种错在材质解析那一天会变成"这个像素用了别人的材质",
        // 而深度上一点痕迹都没有。
        if (meshVisibility.GetSize() != fallbackVisibility.GetSize())
        {
            LIMX_LOG(LogLaunch, Error, "[Meshlet深度] 可见性缓冲区尺寸对不上");
            passed = false;
        }
        else
        {
            SizeType differ = 0;
            SizeType firstBad = 0;

            for (SizeType i = 0; i < meshVisibility.GetSize(); ++i)
            {
                const FVisibilityTriple& a = meshVisibility[i];
                const FVisibilityTriple& b = fallbackVisibility[i];

                if (a.Valid != b.Valid || a.Instance != b.Instance ||
                    a.Meshlet != b.Meshlet || a.Triangle != b.Triangle)
                {
                    if (differ == 0)
                    {
                        firstBad = i;
                    }

                    ++differ;
                }
            }

            LIMX_LOG(LogLaunch, Display,
                     "[Meshlet深度] 可见性 (解出的实例/meshlet/三角形) — "
                     "两条路径不同的像素 {} 个",
                     differ);

            if (differ != 0)
            {
                LIMX_LOG(LogLaunch, Error,
                         "[Meshlet深度] 两条路径解出的 (实例, meshlet, 三角形) "
                         "不同 ({} 个像素, 第一个在下标 {}: 网格 "
                         "({},{},{}) vs 回退 ({},{},{}))",
                         differ, firstBad, meshVisibility[firstBad].Instance,
                         meshVisibility[firstBad].Meshlet,
                         meshVisibility[firstBad].Triangle,
                         fallbackVisibility[firstBad].Instance,
                         fallbackVisibility[firstBad].Meshlet,
                         fallbackVisibility[firstBad].Triangle);
                passed = false;
            }
        }
    }
    else
    {
        // 元判据: 这台机器验不了网格着色器路径, 那必须说出来。
        //
        // 静默跳过的话, 一台没有网格着色器的机器上这条判据永远绿, 而它
        // 只验了回退路径 —— "全绿"会被读成"两条路径都对"。
        LIMX_LOG(LogLaunch, Error,
                 "[Meshlet深度] 本设备不支持网格着色器 — 只验了回退路径, "
                 "判定为失败 (换一台支持的机器, 或显式接受只跑回退)");
        passed = false;
    }

    // ---- 判据二/三: 与经典深度预通道的关系 ----
    SizeType drawn      = 0;
    SizeType nearer     = 0;
    SizeType exactEqual = 0;

    Float32 worstNearer = 0.0f;

    for (SizeType i = 0; i < pixelCount; ++i)
    {
        // 深度 1.0 = 这个像素上 meshlet 路径什么都没画
        if (fallbackDepth[i] >= 1.0f)
        {
            continue;
        }

        ++drawn;

        if (fallbackDepth[i] == classicDepth[i])
        {
            ++exactEqual;
        }
        else if (fallbackDepth[i] < classicDepth[i])
        {
            ++nearer;
            worstNearer = FMath::Max(worstNearer,
                                     classicDepth[i] - fallbackDepth[i]);
        }
    }

    const Float32 exactFraction =
        (drawn > 0) ? (static_cast<Float32>(exactEqual) /
                       static_cast<Float32>(drawn))
                    : 0.0f;

    LIMX_LOG(LogLaunch, Display,
             "[Meshlet深度] 与经典深度 — meshlet 路径画了 {} 个像素, "
             "其中恰好相等 {} 个 ({}), 比经典**更近** {} 个 (最多 {})",
             drawn, exactEqual, exactFraction, nearer, worstNearer);

    // 元判据: 得真的画了东西
    constexpr SizeType kMinDrawn = 50000;

    if (drawn < kMinDrawn)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Meshlet深度] meshlet 路径只画了 {} 个像素 (需要至少 {}) "
                 "—— 一张几乎空的深度图会让下面两条判据形同虚设",
                 drawn, kMinDrawn);
        passed = false;
    }

    // 判据二: 不许比经典更近
    if (nearer != 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Meshlet深度] {} 个像素上 meshlet 路径的深度比经典路径**更近** "
                 "(最多 {}) —— meshlet 路径画的三角形应当是经典路径的子集, "
                 "少画只能让深度变远",
                 nearer, worstNearer);
        passed = false;
    }

    // ---- 判据三: 恰好相等的像素 ----
    //
    // 阈值取决于场景里**有没有蒙版材质**, 而那不是拍脑袋分的两档:
    //
    //   没有蒙版时, 两条路径画的是**同一个**三角形集合, 顶点变换是同一个
    //     表达式的同一种写法 —— 于是深度必须**处处逐位相同**。实测墙角
    //     场景 921600/921600 = 1.000000, 一个像素都不差。
    //
    //   有蒙版时, 经典路径在那些地方做 alpha 测试挖了洞、露出后面的不透明
    //     面, 而 meshlet 路径直接画那个不透明面。两者的前后关系没变 (判据
    //     二仍然成立), 但深度值不同。实测综合场景 0.973866, 而差额恰好是
    //     两块蒙版板子的屏幕面积。
    //
    // 分两档的价值在于: **强的那一档真的会跑**。只留一个 0.95 的下限的话,
    // 墙角场景上"顶点变换写成了等价但不同的表达式"这类缺陷 (差一两个最低位)
    // 会安然通过 —— 而它正是这条判据最该拦的东西。
    SizeType maskedBatches = 0;

    {
        const TArray<FRenderObject>& casters =
            renderer.GetShadowCasterObjects();

        for (SizeType i = 0; i < casters.GetSize(); ++i)
        {
            if (casters[i].BlendMode == EMaterialBlendMode::Masked)
            {
                ++maskedBatches;
            }
        }
    }

    const Float32 minExactFraction = (maskedBatches == 0) ? 1.0f : 0.95f;

    LIMX_LOG(LogLaunch, Display,
             "[Meshlet深度] 场景里蒙版批次 {} 个 — 相等比例的下限取 {}",
             maskedBatches, minExactFraction);

    if (exactFraction < minExactFraction)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Meshlet深度] 只有 {} 的像素与经典深度恰好相等 "
                 "(需要至少 {}) —— 顶点变换与经典路径不是同一个表达式?",
                 exactFraction, minExactFraction);
        passed = false;
    }

    // ---- 判据四: 可见性编号与深度的覆盖必须完全一致 ----
    //
    // 编号为 0 (空) 的像素上深度必须是 1.0 (没画), 反过来也一样。
    //
    // 两者是同一次光栅化的两个附件, 覆盖范围只能相同 —— 不同就说明其中
    // 一个的清除值、混合状态或者写掩码出了问题。而那种错在画面上要到材质
    // 解析那一天才现形: 一个"深度上有东西但编号是空"的像素会去解一个不
    // 存在的三角形。
    {
        SizeType coverageMismatch = 0;

        SizeType decodedOutOfRange = 0;

        const UInt32 visibleCount =
            renderer.GetMeshletCullPass()->GetStats().MeshletsVisible;

        const UInt32 instanceCount = static_cast<UInt32>(
            renderer.GetMeshletCullPass()->GetInstances().GetSize());

        const UInt32 sceneMeshletCount =
            renderer.GetMeshletCullPass()->GetSceneMeshletCount();

        for (SizeType i = 0; i < pixelCount; ++i)
        {
            const bool hasVisibility = (fallbackVisibility[i].Valid != 0u);
            const bool hasDepth      = (fallbackDepth[i] < 1.0f);

            if (hasVisibility != hasDepth)
            {
                ++coverageMismatch;
                continue;
            }

            if (!hasVisibility)
            {
                continue;
            }

            if (fallbackVisibility[i].Instance >= instanceCount ||
                fallbackVisibility[i].Meshlet >= sceneMeshletCount ||
                fallbackVisibility[i].Triangle >= kMaxMeshletTriangles)
            {
                ++decodedOutOfRange;
            }
        }

        LIMX_LOG(LogLaunch, Display,
                 "[Meshlet深度] 覆盖一致性 — 编号与深度不一致的像素 {} 个; "
                 "解码越界 {} 个 (可见记录 {} 条)",
                 coverageMismatch, decodedOutOfRange, visibleCount);

        if (coverageMismatch != 0)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[Meshlet深度] {} 个像素上可见性编号与深度的覆盖不一致 "
                     "—— 两者是同一次光栅化的两个附件, 覆盖范围只能相同",
                     coverageMismatch);
            passed = false;
        }

        if (decodedOutOfRange != 0)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[Meshlet深度] {} 个像素的编号解出来越界 —— "
                     "材质解析会去解一个不存在的三角形",
                     decodedOutOfRange);
            passed = false;
        }
    }

    // ---- 判据五: 网格着色器的输出上限必须容得下构建器能产出的最大 meshlet ----
    //
    // 这一条把一个**跨语言的耦合**变成可验的。meshlet_depth.mesh 的
    // layout(max_vertices, max_primitives) 是 GLSL 里的常量, C++ 侧读不到;
    // 它写小了不会报错, 只会静默丢掉超出的图元 —— 模型上零星的洞。
    //
    // 能验的是它的**必要条件**: 声明的上限不小于构建器实际产出的最大值。
    // 下面这两个常量是那两个 GLSL 常量的镜像, 改一处必须改另一处 ——
    // 而这条判据保证"改小了却没人发现"不会发生。
    //
    // 一个实测: 真正卡住构建器的是**顶点**上限, 不是三角形上限。球体
    // (64x48) 上单个 meshlet 最多 98 个三角形而顶点恰好 64 个 —— 124 个
    // 三角形需要 62 个以上的顶点复用到 6 (规则三角网格的理论上限), 而实际
    // 复用率在 4 附近。所以把 max_primitives 从 124 调到 100 **不是缺陷**,
    // 它一个图元都不会丢; 调到 32 才是。变异验证里那一条据此改过。
    constexpr UInt32 kMeshShaderMaxVertices = 64;
    constexpr UInt32 kMeshShaderMaxPrimitives = 124;

    {
        const FMeshData sphere =
            FGeometryGenerator::GenerateSphere(1.0f, 64, 48);

        const FMeshletStatistics statistics = FMeshletBuilder::ComputeStatistics(
            FMeshletBuilder::Build(sphere.Vertices, sphere.Indices));

        LIMX_LOG(LogLaunch, Display,
                 "[Meshlet深度] 构建器实际产出的最大 meshlet — {} 三角形 / "
                 "{} 顶点; 网格着色器声明的上限 {} / {}",
                 statistics.MaxTriangles, statistics.MaxVertices,
                 kMeshShaderMaxPrimitives, kMeshShaderMaxVertices);

        if (statistics.MaxTriangles > kMeshShaderMaxPrimitives ||
            statistics.MaxVertices > kMeshShaderMaxVertices)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[Meshlet深度] 构建器能产出 {} 三角形 / {} 顶点的 "
                     "meshlet, 而网格着色器只声明了 {} / {} —— 超出的部分"
                     "会被静默丢掉",
                     statistics.MaxTriangles, statistics.MaxVertices,
                     kMeshShaderMaxPrimitives, kMeshShaderMaxVertices);
            passed = false;
        }
    }

    if (!renderer.SetMeshletDepthEnabled(false))
    {
        LIMX_LOG(LogLaunch, Error, "[Meshlet深度] 无法关闭 (复位)");
        passed = false;
    }

    LIMX_LOG(LogLaunch, Display, "[Meshlet深度] {}", passed ? "通过" : "失败");

    return passed;
}

// ============================================================================
// RunHizProbeChecks — 直接盯着遮挡测试本身的判据
//
// 上面那条"开关遮挡剔除画面必须相同"是**结果**判据, 而两阶段遮挡剔除是
// 自纠错的: 第一阶段错剔了, 第二阶段拿重建后的金字塔一测就补回来了。于是
// 结果判据对遮挡测试本身的错误几乎是瞎的 —— 实测十一条变异只红了三条,
// 剩下八条全都只让第一阶段剔得更狠, 而更狠的那部分被第二阶段原样补回。
//
// 那八条损害的是**效率**而不是正确性。要抓它们, 判据得越过画面, 直接盯着
// 那两个函数的中间量。hiz_probe.comp 把它们摊开, 这里逐个验四条**单向**的
// 不等式:
//
//   1. 投出来的屏幕矩形必须**包得住**球的真实投影
//   2. 最近深度必须**不大于**球面上的真实最小深度
//   3. 金字塔查到的最大值必须**不小于**矩形范围内第 0 级的真实最大值
//   4. 遮挡结论必须等于 (最近深度 > 那个最大值)
//
// 参考值是**采样**出来的: 在球面上撒一层点, 逐点投影取包围盒与最小深度。
// 采样得到的包围盒比真实的略小、最小深度比真实的略大 —— 两个偏差都落在
// 让判据**更宽松**的一侧, 于是不会假报警, 而实现真缩了的话照样抓得住。
// ============================================================================

namespace
{

/// 探针结果缓冲区的头部 —— 与 hiz_probe.comp 里那个 scalar 块逐字节对齐
struct FHizProbeHeader
{
    Float32 ViewProjRows[16];
    Float32 CameraPosition[4];
    Float32 ScreenParams[4];
};

/// 单个探针的结果
struct FHizProbeResult
{
    Float32 Rect[4];
    Float32 Nearest;
    Float32 Maximum;
    Float32 Level;
    UInt32  Flags;
};

static_assert(sizeof(FHizProbeHeader) == 96,
              "探针头部必须与着色器里的 scalar 块一致");

static_assert(sizeof(FHizProbeResult) == 32,
              "探针结果必须与着色器里的 scalar 块一致");

constexpr UInt32 kHizProbeProjected  = 1u;
constexpr UInt32 kHizProbeOccluded   = 2u;
constexpr UInt32 kHizProbeByFunction = 4u;
constexpr UInt32 kHizProbeAtEquality = 8u;

/// 探针的世界空间包围球
struct FHizProbeSphere
{
    FVector3 Center;
    Float32  Radius = 0.0f;
};

/// 球面上采样点的参考值
struct FHizProbeReference
{
    Float32 MinX = 0.0f;
    Float32 MinY = 0.0f;
    Float32 MaxX = 0.0f;
    Float32 MaxY = 0.0f;

    /// 球面上最小的那个深度 (z/w)
    Float32 MinDepth = 0.0f;

    /// 球面上最小的那个 w —— 小于等于近平面就说明球穿了近平面
    Float32 MinW = 0.0f;

    bool Valid = false;
};

/// 按行主序的 4x4 乘一个点
void HizProbeTransform(const Float32* rows, const FVector3& point,
                       Float32* outClip)
{
    for (UInt32 r = 0; r < 4; ++r)
    {
        outClip[r] = rows[r * 4 + 0] * point.X + rows[r * 4 + 1] * point.Y +
                     rows[r * 4 + 2] * point.Z + rows[r * 4 + 3];
    }
}

/// 在球面上撒点, 算出投影包围盒与最小深度
///
/// 撒的是经纬网格。点撒得越密参考值越紧, 而参考值偏松的方向 (包围盒偏小、
/// 最小深度偏大) 恰好是让判据更宽松的方向 —— 所以密度不足只会漏报, 不会
/// 假报警。这里取 96x49, 对半径几米的球足够到亚像素。
FHizProbeReference HizProbeReferenceOf(const FHizProbeHeader& header,
                                       const FHizProbeSphere& sphere)
{
    constexpr UInt32 kLongitudes = 96;
    constexpr UInt32 kLatitudes  = 49;

    FHizProbeReference reference;

    const Float32 screenWidth  = header.ScreenParams[0];
    const Float32 screenHeight = header.ScreenParams[1];

    for (UInt32 lat = 0; lat < kLatitudes; ++lat)
    {
        const Float32 theta = FMath::kPi *
                              static_cast<Float32>(lat) /
                              static_cast<Float32>(kLatitudes - 1);

        const Float32 sinTheta = FMath::Sin(theta);
        const Float32 cosTheta = FMath::Cos(theta);

        for (UInt32 lon = 0; lon < kLongitudes; ++lon)
        {
            const Float32 phi = 2.0f * FMath::kPi *
                                static_cast<Float32>(lon) /
                                static_cast<Float32>(kLongitudes);

            const FVector3 point = {
                sphere.Center.X + sphere.Radius * sinTheta * FMath::Cos(phi),
                sphere.Center.Y + sphere.Radius * cosTheta,
                sphere.Center.Z + sphere.Radius * sinTheta * FMath::Sin(phi),
            };

            Float32 clip[4] = {};

            HizProbeTransform(header.ViewProjRows, point, clip);

            if (!reference.Valid || clip[3] < reference.MinW)
            {
                reference.MinW = clip[3];
            }

            if (clip[3] <= 0.0f)
            {
                reference.Valid = true;
                continue;
            }

            const Float32 ndcX = clip[0] / clip[3];
            const Float32 ndcY = clip[1] / clip[3];
            const Float32 depth = clip[2] / clip[3];

            const Float32 screenX = (ndcX * 0.5f + 0.5f) * screenWidth;
            const Float32 screenY = (ndcY * 0.5f + 0.5f) * screenHeight;

            if (!reference.Valid)
            {
                reference.MinX     = screenX;
                reference.MaxX     = screenX;
                reference.MinY     = screenY;
                reference.MaxY     = screenY;
                reference.MinDepth = depth;
                reference.Valid    = true;
                continue;
            }

            reference.MinX     = FMath::Min(reference.MinX, screenX);
            reference.MaxX     = FMath::Max(reference.MaxX, screenX);
            reference.MinY     = FMath::Min(reference.MinY, screenY);
            reference.MaxY     = FMath::Max(reference.MaxY, screenY);
            reference.MinDepth = FMath::Min(reference.MinDepth, depth);
        }
    }

    return reference;
}

/// 第 0 级在某个屏幕矩形范围内的最大值 —— 金字塔查询的参考答案
Float32 HizProbeLevelZeroMax(const TArray<Float32>& level0, UInt32 width,
                             UInt32 height, const Float32* rect)
{
    const Int32 lowerX = static_cast<Int32>(FMath::Floor(rect[0]));
    const Int32 lowerY = static_cast<Int32>(FMath::Floor(rect[1]));
    const Int32 upperX = static_cast<Int32>(FMath::Floor(rect[2]));
    const Int32 upperY = static_cast<Int32>(FMath::Floor(rect[3]));

    const Int32 beginX = FMath::Clamp(lowerX, 0, static_cast<Int32>(width) - 1);
    const Int32 beginY =
        FMath::Clamp(lowerY, 0, static_cast<Int32>(height) - 1);
    const Int32 endX = FMath::Clamp(upperX, 0, static_cast<Int32>(width) - 1);
    const Int32 endY = FMath::Clamp(upperY, 0, static_cast<Int32>(height) - 1);

    Float32 maximum = 0.0f;

    for (Int32 y = beginY; y <= endY; ++y)
    {
        for (Int32 x = beginX; x <= endX; ++x)
        {
            const SizeType offset =
                static_cast<SizeType>(y) * width + static_cast<SizeType>(x);

            if (offset < level0.GetSize())
            {
                maximum = FMath::Max(maximum, level0[offset]);
            }
        }
    }

    return maximum;
}

} // namespace

static bool RunHizProbeChecks(FRenderContext* context, FRenderer& renderer,
                              const TArray<Float32>& level0)
{
    FMeshletDepthPass* const pass = renderer.GetMeshletDepthPass();
    FMeshletCullPass* const  cull = renderer.GetMeshletCullPass();

    if (pass == nullptr || cull == nullptr)
    {
        LIMX_LOG(LogLaunch, Error, "[Hi-Z探针] 通道不存在");
        return false;
    }

    IRHIDevice* const device = context->GetDevice();

    const FRHIExtent2D extent = pass->GetHizExtent();

    // ---- 探针 ----
    //
    // 前 4 个是**对抗性**的: 相机在球内、球穿近平面。它们要求实现返回
    // "投影失败", 而不是投一个发散的矩形出来。剩下的按视锥内的格点铺开,
    // 半径从小到大 —— 小球落在金字塔的细级上, 大球落在粗级上, 两条路径
    // 都要走到。
    TArray<FHizProbeSphere> probes;

    const FVector3 cameraPosition = renderer.GetCamera().GetPosition();
    const FVector3 forward        = renderer.GetCamera().GetForwardVector();
    const FVector3 right          = renderer.GetCamera().GetRightVector();
    const FVector3 up             = renderer.GetCamera().GetUpVector();

    probes.Add({ cameraPosition, 2.0f });

    probes.Add({ { cameraPosition.X + forward.X * 0.05f,
                   cameraPosition.Y + forward.Y * 0.05f,
                   cameraPosition.Z + forward.Z * 0.05f },
                 1.0f });

    probes.Add({ { cameraPosition.X + forward.X * 0.4f,
                   cameraPosition.Y + forward.Y * 0.4f,
                   cameraPosition.Z + forward.Z * 0.4f },
                 0.5f });

    probes.Add({ { cameraPosition.X - forward.X * 3.0f,
                   cameraPosition.Y - forward.Y * 3.0f,
                   cameraPosition.Z - forward.Z * 3.0f },
                 1.0f });

    // 穿近平面而**相机在球外**的球
    //
    // 这一档非有不可: 前面那几个球虽然也穿近平面, 但相机同时在球内, 于是
    // "相机在球内"那条判断先返回了 —— 近平面那条判断根本没被走到。把它
    // 改成恒不成立, 判据毫无反应。
    //
    // 球心摆在 (半径 + 近平面的一小半) 处: 相机在球外 (距离 > 半径), 而
    // 球的最前面伸到近平面之内。
    {
        const Float32 nearPlane = renderer.GetCamera().GetNearPlane();

        const Float32 probeRadii[3] = { 0.3f, 1.0f, 3.0f };

        for (UInt32 i = 0; i < 3; ++i)
        {
            const Float32 distance = probeRadii[i] + nearPlane * 0.5f;

            probes.Add({ { cameraPosition.X + forward.X * distance,
                           cameraPosition.Y + forward.Y * distance,
                           cameraPosition.Z + forward.Z * distance },
                         probeRadii[i] });

            // 再来一个偏到侧面的 —— 球心视深度更大而最前端照样穿
            probes.Add({ { cameraPosition.X + forward.X * distance +
                               right.X * probeRadii[i] * 0.7f,
                           cameraPosition.Y + forward.Y * distance +
                               right.Y * probeRadii[i] * 0.7f,
                           cameraPosition.Z + forward.Z * distance +
                               right.Z * probeRadii[i] * 0.7f },
                         probeRadii[i] });
        }
    }

    const Float32 radii[4]     = { 0.15f, 0.6f, 2.5f, 8.0f };
    const Float32 distances[5] = { 2.0f, 5.0f, 11.0f, 24.0f, 55.0f };
    const Float32 offsets[5]   = { -1.4f, -0.6f, 0.0f, 0.6f, 1.4f };

    for (UInt32 d = 0; d < 5; ++d)
    {
        for (UInt32 r = 0; r < 4; ++r)
        {
            for (UInt32 ox = 0; ox < 5; ++ox)
            {
                for (UInt32 oy = 0; oy < 3; ++oy)
                {
                    const Float32 lateral = offsets[ox] * distances[d] * 0.35f;
                    const Float32 vertical =
                        offsets[oy + 1] * distances[d] * 0.25f;

                    probes.Add({ { cameraPosition.X + forward.X * distances[d] +
                                       right.X * lateral + up.X * vertical,
                                   cameraPosition.Y + forward.Y * distances[d] +
                                       right.Y * lateral + up.Y * vertical,
                                   cameraPosition.Z + forward.Z * distances[d] +
                                       right.Z * lateral + up.Z * vertical },
                                 radii[r] });
                }
            }
        }
    }

    const UInt32 probeCount = static_cast<UInt32>(probes.GetSize());

    const SizeType resultBytes =
        sizeof(FHizProbeHeader) +
        static_cast<SizeType>(probeCount) * sizeof(FHizProbeResult);

    // ---- 资源 ----
    FRHIShaderHandle              shader;
    FRHIDescSetLayoutHandle       setLayout;
    FRHIPipelineLayoutHandle      pipelineLayout;
    FRHIComputePipelineHandle     pipeline;
    FRHIDescriptorSetHandle       descriptorSet;
    FRHIBufferHandle              probeBuffer;
    FRHIBufferHandle              resultBuffer;
    FRHITextureViewHandle         hizView;
    FRHISamplerHandle             hizSampler;

    bool ok = true;

    {
        FShaderManager& shaders = FShaderManager::Get();

        if (!shaders.IsInitialized())
        {
            shaders.Initialize();
        }

        ok = IsRHISuccess(shaders.CreateShaderModule(
            device, FString("Builtin/hiz_probe.comp"), EShaderStage::Compute,
            shader));
    }

    if (ok)
    {
        FRHIDescriptorBinding bindings[4] = {};

        for (UInt32 i = 0; i < 4; ++i)
        {
            bindings[i].Binding    = i;
            bindings[i].Type       = EDescriptorType::StorageBuffer;
            bindings[i].Count      = 1;
            bindings[i].StageFlags = EShaderStage::Compute;
        }

        bindings[2].Type = EDescriptorType::CombinedImageSampler;
        bindings[3].Type = EDescriptorType::UniformBuffer;

        FRHIDescSetLayoutDesc layoutDesc = {};
        layoutDesc.Bindings     = bindings;
        layoutDesc.BindingCount = 4;
        layoutDesc.DebugName    = "HizProbeSetLayout";

        ok = IsRHISuccess(device->CreateDescSetLayout(layoutDesc, setLayout));
    }

    if (ok)
    {
        FRHIPushConstantRange pushRange = {};
        pushRange.StageFlags = EShaderStage::Compute;
        pushRange.Offset     = 0;
        pushRange.Size       = sizeof(UInt32) * 4;

        FRHIPipelineLayoutDesc layoutDesc = {};
        layoutDesc.SetLayouts             = &setLayout;
        layoutDesc.SetLayoutCount         = 1;
        layoutDesc.PushConstantRanges     = &pushRange;
        layoutDesc.PushConstantRangeCount = 1;
        layoutDesc.DebugName              = "HizProbeLayout";

        ok = IsRHISuccess(
            device->CreatePipelineLayout(layoutDesc, pipelineLayout));
    }

    if (ok)
    {
        FRHIComputePipelineDesc pipelineDesc = {};
        pipelineDesc.ComputeShader.Shader     = shader;
        pipelineDesc.ComputeShader.Stage      = EShaderStage::Compute;
        pipelineDesc.ComputeShader.EntryPoint = "main";
        pipelineDesc.PipelineLayout           = pipelineLayout;
        pipelineDesc.DebugName                = "HizProbePipeline";

        ok = IsRHISuccess(device->CreateComputePipeline(pipelineDesc, pipeline));
    }

    if (ok)
    {
        FRHIBufferDesc bufferDesc = {};
        bufferDesc.Size        = static_cast<UInt64>(probeCount) * 16u;
        bufferDesc.Usage       = EBufferUsage::StorageBuffer;
        bufferDesc.MemoryUsage = EMemoryUsage::CpuToGpu;
        bufferDesc.DebugName   = "HizProbeInput";

        ok = IsRHISuccess(device->CreateBuffer(bufferDesc, probeBuffer));

        bufferDesc.Size        = resultBytes;
        bufferDesc.MemoryUsage = EMemoryUsage::GpuToCpu;
        bufferDesc.DebugName   = "HizProbeOutput";

        ok = ok && IsRHISuccess(device->CreateBuffer(bufferDesc, resultBuffer));
    }

    if (ok)
    {
        FRHITextureViewDesc viewDesc = {};
        viewDesc.Texture         = pass->GetHizTexture();
        viewDesc.ViewType        = ETextureType::Texture2D;
        viewDesc.Format          = EPixelFormat::R32_SFLOAT;
        viewDesc.BaseMipLevel    = 0;
        viewDesc.MipLevelCount   = pass->GetHizLevelCount();
        viewDesc.BaseArrayLayer  = 0;
        viewDesc.ArrayLayerCount = 1;

        ok = IsRHISuccess(device->CreateTextureView(viewDesc, hizView));

        FRHISamplerDesc samplerDesc = {};
        samplerDesc.MinFilter    = EFilter::Nearest;
        samplerDesc.MagFilter    = EFilter::Nearest;
        samplerDesc.MipmapMode   = ESamplerMipmapMode::Nearest;
        samplerDesc.AddressModeU = ESamplerAddressMode::ClampToEdge;
        samplerDesc.AddressModeV = ESamplerAddressMode::ClampToEdge;
        samplerDesc.AddressModeW = ESamplerAddressMode::ClampToEdge;
        samplerDesc.MaxLod =
            static_cast<Float32>(pass->GetHizLevelCount());

        ok = ok && IsRHISuccess(device->CreateSampler(samplerDesc, hizSampler));
    }

    if (ok)
    {
        ok = IsRHISuccess(
            device->AllocateDescriptorSet(setLayout, descriptorSet));
    }

    // 探针数据上传
    if (ok)
    {
        void* mapped = nullptr;

        if (IsRHISuccess(device->MapBuffer(probeBuffer, &mapped)) &&
            mapped != nullptr)
        {
            auto* values = static_cast<Float32*>(mapped);

            for (UInt32 i = 0; i < probeCount; ++i)
            {
                values[i * 4 + 0] = probes[i].Center.X;
                values[i * 4 + 1] = probes[i].Center.Y;
                values[i * 4 + 2] = probes[i].Center.Z;
                values[i * 4 + 3] = probes[i].Radius;
            }

            device->UnmapBuffer(probeBuffer);
        }
        else
        {
            ok = false;
        }
    }

    const UInt32 frameIndex = context->GetCurrentFrameIndex();

    if (ok)
    {
        FRHIDescriptorWrite writes[4];

        writes[0] = FRHIDescriptorWrite::StorageBuffer(
            descriptorSet, 0, probeBuffer, 0,
            static_cast<UInt64>(probeCount) * 16u);
        writes[1] = FRHIDescriptorWrite::StorageBuffer(descriptorSet, 1,
                                                       resultBuffer, 0,
                                                       resultBytes);
        writes[2] = FRHIDescriptorWrite::CombinedImageSampler(
            descriptorSet, 2, hizView, hizSampler,
            EImageLayout::ShaderReadOnly);
        writes[3] = FRHIDescriptorWrite::UniformBuffer(
            descriptorSet, 3, cull->GetViewBuffer(frameIndex), 0,
            FMeshletCullPass::GetViewBufferBytes());

        device->UpdateDescriptorSets(writes, 4);
    }

    // ---- 跑一帧, 在场景之后分派 ----
    bool recorded = false;

    if (ok)
    {
        renderer.SetPostSceneRenderCallback(
            [&recorded, context, pipeline, pipelineLayout, descriptorSet,
             resultBuffer, probeCount]()
            {
                IRHICommandBuffer* cmd = context->GetCurrentCommandBuffer();

                if (cmd == nullptr)
                {
                    return;
                }

                cmd->BindComputePipeline(pipeline);
                cmd->BindDescriptorSet(EPipelineBindPoint::Compute,
                                       pipelineLayout, 0, descriptorSet);

                UInt32 push[4] = { probeCount, 0, 0, 0 };

                cmd->PushConstants(pipelineLayout, EShaderStage::Compute, 0,
                                   sizeof(push), push);

                cmd->Dispatch((probeCount + 63u) / 64u, 1, 1);

                FRHIBufferMemoryBarrier barrier = {};
                barrier.SrcAccessMask = EAccessFlags::ShaderWrite;
                barrier.DstAccessMask = EAccessFlags::HostRead;
                barrier.Buffer        = resultBuffer;

                cmd->PipelineBarrier(EPipelineStageFlags::ComputeShader,
                                     EPipelineStageFlags::Host, nullptr, 0,
                                     &barrier, 1, nullptr, 0);

                recorded = true;
            });

        renderer.RenderFrame();
        renderer.SetPostSceneRenderCallback(TFunction<void()>());

        ok = recorded;
    }

    // ---- 验 ----
    bool passed = ok;

    if (ok)
    {
        device->WaitIdle();

        void* mapped = nullptr;

        if (IsRHISuccess(device->MapBuffer(resultBuffer, &mapped)) &&
            mapped != nullptr)
        {
            const auto* bytes = static_cast<const UInt8*>(mapped);

            FHizProbeHeader header;
            Memory::MemCopy(&header, bytes, sizeof(FHizProbeHeader));

            const auto* results = reinterpret_cast<const FHizProbeResult*>(
                bytes + sizeof(FHizProbeHeader));

            // 亚像素的容差 —— 参考值是采样出来的, 而采样偏差落在让判据更
            // 宽松的一侧, 所以这个容差只用来吸收浮点噪声。
            constexpr Float32 kRectEpsilon  = 0.5f;
            constexpr Float32 kDepthEpsilon = 1.0e-5f;

            SizeType rectViolations    = 0;
            SizeType looseViolations   = 0;
            SizeType depthViolations   = 0;
            SizeType maxViolations     = 0;
            SizeType logicViolations   = 0;
            SizeType equalityViolations = 0;
            SizeType nearPlaneEscapes  = 0;
            SizeType projectedCount    = 0;
            SizeType occludedCount     = 0;
            SizeType degenerateCount   = 0;

            Float32 worstRect  = 0.0f;
            Float32 worstLoose = 0.0f;
            Float32 worstDepth = 0.0f;
            Float32 worstMax   = 0.0f;

            SizeType firstRectIndex = probes.GetSize();

            const Float32 nearPlane = header.ScreenParams[3];

            for (UInt32 i = 0; i < probeCount; ++i)
            {
                const FHizProbeResult& result = results[i];

                const bool projected =
                    (result.Flags & kHizProbeProjected) != 0u;

                const bool occluded = (result.Flags & kHizProbeOccluded) != 0u;

                const FHizProbeReference reference =
                    HizProbeReferenceOf(header, probes[i]);

                // 判据一: 球穿了近平面 (或相机在球内) 就必须报投影失败
                const FVector3 delta = {
                    probes[i].Center.X - header.CameraPosition[0],
                    probes[i].Center.Y - header.CameraPosition[1],
                    probes[i].Center.Z - header.CameraPosition[2],
                };

                const Float32 centerDistance =
                    FMath::Sqrt(delta.X * delta.X + delta.Y * delta.Y +
                                delta.Z * delta.Z);

                const bool degenerate = (centerDistance <= probes[i].Radius) ||
                                        (reference.MinW <= nearPlane);

                if (degenerate)
                {
                    ++degenerateCount;

                    if (projected)
                    {
                        ++nearPlaneEscapes;
                    }

                    continue;
                }

                if (!projected)
                {
                    continue;
                }

                ++projectedCount;

                if (occluded)
                {
                    ++occludedCount;
                }

                // 判据二: 矩形必须包得住真实投影
                if (reference.Valid)
                {
                    const Float32 slack = FMath::Max(
                        FMath::Max(result.Rect[0] - reference.MinX,
                                   result.Rect[1] - reference.MinY),
                        FMath::Max(reference.MaxX - result.Rect[2],
                                   reference.MaxY - result.Rect[3]));

                    if (slack > kRectEpsilon)
                    {
                        ++rectViolations;

                        if (firstRectIndex == probes.GetSize())
                        {
                            firstRectIndex = i;
                        }

                        worstRect = FMath::Max(worstRect, slack);
                    }

                    // 判据二之二: 矩形还得**紧**
                    //
                    // 只验"包得住"是不够的 —— 一个把整个屏幕都圈进去的矩形
                    // 永远包得住, 而它什么都剔不掉。相机基取错列的那条变异
                    // 正是这么逃掉的: 投出来的矩形与相机无关, 但恰好比真实
                    // 投影大, 于是包含判据全绿。
                    //
                    // 切线解是**精确**的, 所以这里可以要求它与参考值贴合。
                    // 容差按参考矩形的尺寸放大一点: 参考值是球面上撒点撒出来
                    // 的, 球越大采样间隔越粗。
                    const Float32 referenceSpan =
                        FMath::Max(reference.MaxX - reference.MinX,
                                   reference.MaxY - reference.MinY);

                    const Float32 excess = FMath::Max(
                        FMath::Max(reference.MinX - result.Rect[0],
                                   reference.MinY - result.Rect[1]),
                        FMath::Max(result.Rect[2] - reference.MaxX,
                                   result.Rect[3] - reference.MaxY));

                    const Float32 allowed =
                        kRectEpsilon + referenceSpan * 0.02f;

                    if (excess > allowed)
                    {
                        ++looseViolations;

                        worstLoose = FMath::Max(worstLoose, excess - allowed);
                    }
                }

                // 判据三: 最近深度不许比球面上的真实最小深度还大
                if (reference.Valid &&
                    result.Nearest > reference.MinDepth + kDepthEpsilon)
                {
                    ++depthViolations;

                    worstDepth = FMath::Max(
                        worstDepth, result.Nearest - reference.MinDepth);
                }

                // 判据四: 金字塔查到的最大值不许比第 0 级的真实最大值还小
                const Float32 trueMax = HizProbeLevelZeroMax(
                    level0, extent.Width, extent.Height, result.Rect);

                if (result.Maximum >= 0.0f &&
                    result.Maximum < trueMax - kDepthEpsilon)
                {
                    ++maxViolations;

                    worstMax = FMath::Max(worstMax, trueMax - result.Maximum);
                }

                // 判据五: 结论必须与那两个数自洽
                const bool expected =
                    (result.Maximum >= 0.0f) && (result.Nearest > result.Maximum);

                const bool byFunction =
                    (result.Flags & kHizProbeByFunction) != 0u;

                if (expected != occluded || expected != byFunction)
                {
                    ++logicViolations;
                }

                // 判据六: 相等的边界必须判成"没挡住"
                if ((result.Flags & kHizProbeAtEquality) != 0u)
                {
                    ++equalityViolations;
                }
            }

            LIMX_LOG(LogLaunch, Display,
                     "[Hi-Z探针] {} 个探针 — 退化 {}, 投影成功 {}, "
                     "判为遮挡 {}",
                     probeCount, degenerateCount, projectedCount,
                     occludedCount);

            // 三条分支都得走到, 否则判据对它们没有约束力
            if (degenerateCount == 0)
            {
                LIMX_LOG(LogLaunch, Error,
                         "[Hi-Z探针] 没有一个退化的球 (相机在球内 / 穿近平面) "
                         "—— 那条分支没被走到");
                passed = false;
            }

            if (nearPlaneEscapes != 0)
            {
                LIMX_LOG(LogLaunch, Error,
                         "[Hi-Z探针] {} 个退化的球 (相机在球内 / 穿近平面) "
                         "被当成投影成功 —— 那时投出来的矩形是发散的, "
                         "拿它查金字塔的结论没有意义",
                         nearPlaneEscapes);
                passed = false;
            }

            if (rectViolations != 0)
            {
                LIMX_LOG(LogLaunch, Error,
                         "[Hi-Z探针] {} 个探针的屏幕矩形包不住球的真实投影 "
                         "(最多缺 {} 像素, 第一个在下标 {}) —— 包不住就会"
                         "错剔",
                         rectViolations, worstRect, firstRectIndex);
                passed = false;
            }

            if (looseViolations != 0)
            {
                LIMX_LOG(LogLaunch, Error,
                         "[Hi-Z探针] {} 个探针的屏幕矩形比球的真实投影松得"
                         "过头 (最多超出容差 {} 像素) —— 切线解是精确的, "
                         "松了就说明它算的根本不是这个球",
                         looseViolations, worstLoose);
                passed = false;
            }

            if (depthViolations != 0)
            {
                LIMX_LOG(LogLaunch, Error,
                         "[Hi-Z探针] {} 个探针的最近深度比球面上的真实最小"
                         "深度还大 (最多大 {}) —— 那会把'球心在遮挡物之后而"
                         "前半部分露着'的东西剔掉",
                         depthViolations, worstDepth);
                passed = false;
            }

            if (maxViolations != 0)
            {
                LIMX_LOG(LogLaunch, Error,
                         "[Hi-Z探针] {} 个探针查到的金字塔最大值小于第 0 级"
                         "在同一矩形上的真实最大值 (最多小 {}) —— 最大值偏小"
                         "就是把没挡住的判成挡住了",
                         maxViolations, worstMax);
                passed = false;
            }

            if (equalityViolations != 0)
            {
                LIMX_LOG(LogLaunch, Error,
                         "[Hi-Z探针] {} 个探针在'最近深度恰好等于金字塔最大"
                         "值'时判成了遮挡 —— 相等意味着那个包围体**就是**"
                         "那个遮挡物, 剔掉它画面上就少一块",
                         equalityViolations);
                passed = false;
            }

            if (logicViolations != 0)
            {
                LIMX_LOG(LogLaunch, Error,
                         "[Hi-Z探针] {} 个探针的遮挡结论与 (最近深度, 最大值) "
                         "不自洽",
                         logicViolations);
                passed = false;
            }

            if (projectedCount == 0)
            {
                LIMX_LOG(LogLaunch, Error,
                         "[Hi-Z探针] 没有一个探针投影成功 —— 判据没验到东西");
                passed = false;
            }

            if (occludedCount == 0)
            {
                LIMX_LOG(LogLaunch, Error,
                         "[Hi-Z探针] 没有一个探针判为遮挡 —— 遮挡那条分支"
                         "没被走到, 判据对它没有约束力");
                passed = false;
            }

            device->UnmapBuffer(resultBuffer);
        }
        else
        {
            LIMX_LOG(LogLaunch, Error, "[Hi-Z探针] 结果回读失败");
            passed = false;
        }
    }
    else
    {
        LIMX_LOG(LogLaunch, Error, "[Hi-Z探针] 资源创建失败");
    }

    device->DestroySampler(hizSampler);
    device->DestroyTextureView(hizView);
    device->DestroyBuffer(resultBuffer);
    device->DestroyBuffer(probeBuffer);
    device->DestroyComputePipeline(pipeline);
    device->DestroyPipelineLayout(pipelineLayout);
    device->DestroyDescSetLayout(setLayout);
    device->DestroyShader(shader);

    return passed;
}

// ============================================================================
// RunMeshletOcclusionChecks — 遮挡剔除不许改变画面
//
// 遮挡剔除是**纯粹的优化**: 它只该让 GPU 少画一些看不见的东西, 而不该让
// 画面变化一个像素。所以判据的形状很直接 —— 开关它, 画出来的深度与可见性
// 必须完全相同。
//
// 这条判据能立住, 全靠两阶段。单阶段用的是**上一帧**的金字塔, 而这一帧
// 才露出来的东西会被它判成"挡住了" —— 表现为物体闪一帧才出现, 而那正是
// 这条判据会报出来的差异。
//
// 四条判据:
//
//   一、深度逐位相同。不留容差 —— 画的是同一批三角形, 同一套顶点数学。
//
//   二、可见性解出来的 (实例, meshlet, 三角形) 逐像素相同。深度相同而
//      编号不同是可能的 (两个共面三角形), 而那在材质解析那一步会变成
//      "这个像素用了别人的材质"。
//
//   三、金字塔的归约性质: 每一级的每个纹素必须是上一级对应区域的**最大**
//      值。这一条与场景无关, 而且是遮挡测试保守性的全部依据 —— 取成最小
//      或者平均, 剔除就会把只挡住一角的物体整个剔掉。
//
//   四、元判据: 遮挡剔除必须**真的剔掉了东西**。一个什么都不剔的实现在
//      前两条判据上满分通过, 只是白跑了两遍。
// ============================================================================

static bool RunMeshletOcclusionChecks(FRenderContext* context,
                                      FRenderer&      renderer)
{
    if (!renderer.SetMeshletDepthEnabled(true))
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Meshlet遮挡] 无法启用光栅化 — 判据无法执行, 判定为失败");
        return false;
    }

    FMeshletDepthPass* const pass = renderer.GetMeshletDepthPass();
    FMeshletCullPass* const  cull = renderer.GetMeshletCullPass();

    if (pass == nullptr || cull == nullptr)
    {
        LIMX_LOG(LogLaunch, Error, "[Meshlet遮挡] 通道不存在");
        return false;
    }

    bool passed = true;

    const FRHIExtent2D extent = context->GetSwapchainExtent();

    const SizeType pixelCount =
        static_cast<SizeType>(extent.Width) * extent.Height;

    TArray<Float32>           depthOff;
    TArray<Float32>           depthOn;
    TArray<FVisibilityTriple> visibilityOff;
    TArray<FVisibilityTriple> visibilityOn;

    // ================================================================
    // 判据自己造条件
    //
    // 第一版直接在默认视角上开关遮挡剔除, 十条变异只红了两条。成因不是
    // 判据不严, 是**这个视角下几乎没有遮挡**: 197 个 meshlet 里只有 6 个
    // 被遮挡剔掉, 而且相机不动 —— 于是上一帧的金字塔永远是对的, 第二阶段
    // 整个删掉都不会红。
    //
    // 所以判据自己把相机摆到有遮挡的地方, 并且**制造一次视角跳变**:
    //
    //   1. 相机放到柱子环的中心 —— 近处的柱子挡住远处的柱子与背墙,
    //      于是遮挡剔除真的有东西可剔;
    //   2. 先在**另一个**位置渲几帧, 让金字塔建在那里;
    //   3. 跳回目标位置, 只渲一帧。
    //
    // 第三步是关键: 第一阶段拿到的是**另一个视角**的金字塔, 结论必然是
    // 错的。单阶段的话画面会缺一块, 而两阶段会在这一帧里把它补回来 ——
    // 这正是两阶段存在的全部理由, 而现在它可验了。
    // ================================================================
    FCamera& camera = renderer.GetCamera();

    const FVector3 originalPosition = camera.GetPosition();
    const Float32  originalYaw      = camera.GetYaw();
    const Float32  originalPitch    = camera.GetPitch();

    // 相机摆位是**量出来的**, 不是随手挑的。
    //
    // 在六个候选摆位上量了"第一阶段被遮挡剔掉多少":
    //     (0, 1.6, 0)  环心平视        1 / 98
    //     (0, 2, 6)    默认视角附近     2 / 193
    //     (0, 1.2, 2)                 0 / 98
    //     (11, 1.75, 0) 贴着柱子       34 / 197
    //     (0, 1, 10)   隔着一排物体看   **101 / 197**
    //     (0, 5, -20)  隔着背墙看      175 / 201
    //
    // 取 (0, 1, 10): 遮挡够多 (一半以上), 而且不是"整幅画面被一堵墙挡住"
    // 那种退化情形 —— 后者虽然数字最大, 但它只走到"整个矩形都在墙后面"
    // 这一条路径, 而边界情形 (矩形跨纹素、部分遮挡) 一个都碰不到。
    const FVector3 targetPosition(0.0f, 1.0f, 10.0f);

    const Float32 targetYaw   = 0.0f;
    const Float32 targetPitch = 0.0f;

    // 金字塔要建在**稍微**不同的位置。
    //
    // "稍微"是关键。离得太远的话, 上一帧的金字塔在这一帧的屏幕上几乎处处
    // 是空的 (深度 1.0), 于是第一阶段什么都剔不掉 —— 遮挡这条路径根本没
    // 被走到。第一版把诱饵放在 (0, 12, 18), 结果被剔的数量掉到 4 个。
    //
    // 挪三个单位: 大部分遮挡关系不变 (于是第一阶段真的在剔), 而边缘上
    // 新露出来的东西会被上一帧的金字塔错判成"挡住了" —— 那正是第二阶段
    // 要补回来的。
    const FVector3 decoyPosition(0.0f, 1.0f, 10.0f);

    const auto Restore = [&camera, originalPosition, originalYaw,
                          originalPitch]()
    {
        camera.SetPosition(originalPosition);
        camera.SetRotation(originalYaw, originalPitch);
    };

    camera.SetPosition(targetPosition);
    camera.SetRotation(targetYaw, targetPitch);

    // ---- 关掉遮挡剔除 ----
    pass->SetOcclusionCullEnabled(false);
    cull->SetOcclusionCullEnabled(false);

    renderer.RenderFrame();

    if (!CaptureDepthAndVisibility(context, renderer, pass->GetDepthTexture(),
                                   pass->GetVisibilityTexture(), depthOff,
                                   visibilityOff))
    {
        LIMX_LOG(LogLaunch, Error, "[Meshlet遮挡] 关闭态回读失败");
        return false;
    }

    // ---- 打开遮挡剔除, 并制造一次视角跳变 ----
    pass->SetOcclusionCullEnabled(true);
    cull->SetOcclusionCullEnabled(true);

    // 先在诱饵位置渲几帧 —— 金字塔建在那里
    camera.SetPosition(decoyPosition);
    camera.SetRotation(targetYaw + 0.2f, targetPitch);

    for (UInt32 i = 0; i < 4; ++i)
    {
        renderer.RenderFrame();
    }

    // 跳回目标位置, 取样的就是**跳变的那一帧**。
    //
    // 这一点是踩出来的: 第一版在跳回来之后先 RenderFrame() 一次再读, 而
    // 读的那个函数自己又渲一帧 —— 于是取到的是跳变之后的**第三**帧, 那时
    // 金字塔早在新位置重建好了, 第一阶段的结论又变对了。
    //
    // 表现是判据全绿而"第二阶段补回来几个"恒为 0。绿的原因不是实现对,
    // 是取样取晚了。
    camera.SetPosition(targetPosition);
    camera.SetRotation(targetYaw, targetPitch);

    if (!CaptureDepthAndVisibility(context, renderer, pass->GetDepthTexture(),
                                   pass->GetVisibilityTexture(), depthOn,
                                   visibilityOn))
    {
        LIMX_LOG(LogLaunch, Error, "[Meshlet遮挡] 开启态回读失败");
        return false;
    }

    // ---- 跳变那一帧的统计 ----
    //
    // 统计的回读隔着并行帧数 (这里是 2): 第 N 帧读到的是第 N-2 帧写的。
    // 所以要拿到跳变帧的数, 得在它之后再走两帧。
    //
    // 相机已经停在目标位置, 这两帧不会改变刚才抓下来的那两张图 —— 它们
    // 已经读回 CPU 了。
    renderer.RenderFrame();
    renderer.RenderFrame();

    const FMeshletCullStats stats = cull->GetStats();

    const UInt32 phase2Admitted = pass->GetPhase2Meshlets();

    // ---- 判据一: 深度逐位相同 ----
    {
        SizeType differ = 0;
        Float32  worst  = 0.0f;

        for (SizeType i = 0; i < pixelCount && i < depthOff.GetSize() &&
                             i < depthOn.GetSize();
             ++i)
        {
            const Float32 delta = FMath::Abs(depthOff[i] - depthOn[i]);

            if (delta != 0.0f)
            {
                ++differ;
                worst = FMath::Max(worst, delta);
            }
        }

        LIMX_LOG(LogLaunch, Display,
                 "[Meshlet遮挡] 深度 — 开关之间不同的像素 {} 个 (最大差 {})",
                 differ, worst);

        if (differ != 0)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[Meshlet遮挡] 开关遮挡剔除画出的深度不同 ({} 个像素, "
                     "最大差 {}) —— 遮挡剔除剔掉了看得见的东西",
                     differ, worst);
            passed = false;
        }
    }

    // ---- 判据二: 可见性解出来的三元组逐像素相同 ----
    {
        SizeType differ   = 0;
        SizeType firstBad = 0;

        for (SizeType i = 0; i < visibilityOff.GetSize() &&
                             i < visibilityOn.GetSize();
             ++i)
        {
            const FVisibilityTriple& a = visibilityOff[i];
            const FVisibilityTriple& b = visibilityOn[i];

            if (a.Valid != b.Valid || a.Instance != b.Instance ||
                a.Meshlet != b.Meshlet || a.Triangle != b.Triangle)
            {
                if (differ == 0)
                {
                    firstBad = i;
                }

                ++differ;
            }
        }

        LIMX_LOG(LogLaunch, Display,
                 "[Meshlet遮挡] 可见性 — 开关之间不同的像素 {} 个", differ);

        if (differ != 0)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[Meshlet遮挡] 开关遮挡剔除解出的 (实例, meshlet, 三角形) "
                     "不同 ({} 个像素, 第一个在下标 {}: 关 ({},{},{}) vs "
                     "开 ({},{},{}))",
                     differ, firstBad, visibilityOff[firstBad].Instance,
                     visibilityOff[firstBad].Meshlet,
                     visibilityOff[firstBad].Triangle,
                     visibilityOn[firstBad].Instance,
                     visibilityOn[firstBad].Meshlet,
                     visibilityOn[firstBad].Triangle);
            passed = false;
        }
    }

    // ---- 判据三: 金字塔的内容与归约性质 ----
    //
    // 这两条都在**稳定态**上验, 不在跳变帧上。
    //
    // 逐级回读要一级渲一帧 (十一级就是十一帧), 而那期间相机早停在目标
    // 位置了 —— 拿跳变帧的深度去与它们比是跨帧比对, 差多少都说明不了
    // 问题。第一版就是这么写的, 报出 66774 个纹素不符, 而那个数恰好等于
    // 跳变帧与稳定帧之间的差 —— 一眼看去像是"拷贝没生效"。
    {
        const UInt32 levelCount = pass->GetHizLevelCount();

        TArray<TArray<Float32>> levels;

        bool readOk = (levelCount >= 2);

        // 先在稳定态抓一张深度, 与第 0 级比内容
        TArray<Float32>           settledDepth;
        TArray<FVisibilityTriple> settledVisibility;

        readOk = readOk &&
                 CaptureDepthAndVisibility(context, renderer,
                                           pass->GetDepthTexture(),
                                           pass->GetVisibilityTexture(),
                                           settledDepth, settledVisibility);

        for (UInt32 level = 0; level < levelCount && readOk; ++level)
        {
            TArray<Float32> data;

            readOk = ReadHizLevel(context, renderer, pass->GetHizTexture(),
                                  extent, level, levelCount, data);

            levels.Add(data);
        }

        if (!readOk)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[Meshlet遮挡] 金字塔回读失败 (共 {} 级)", levelCount);
            passed = false;
        }
        else
        {
            SizeType violations = 0;
            Float32  worst      = 0.0f;

            for (UInt32 level = 1; level < levelCount; ++level)
            {
                const UInt32 sourceWidth =
                    FMath::Max(extent.Width >> (level - 1), 1u);
                const UInt32 sourceHeight =
                    FMath::Max(extent.Height >> (level - 1), 1u);

                const UInt32 targetWidth =
                    FMath::Max(extent.Width >> level, 1u);
                const UInt32 targetHeight =
                    FMath::Max(extent.Height >> level, 1u);

                for (UInt32 y = 0; y < targetHeight; ++y)
                {
                    for (UInt32 x = 0; x < targetWidth; ++x)
                    {
                        // 与 hiz_build.comp 同一套覆盖规则: 最后一个纹素
                        // 一直收到源的边界。
                        const UInt32 lastX =
                            (x + 1 == targetWidth)
                                ? (sourceWidth - 1)
                                : FMath::Min(x * 2 + 1, sourceWidth - 1);

                        const UInt32 lastY =
                            (y + 1 == targetHeight)
                                ? (sourceHeight - 1)
                                : FMath::Min(y * 2 + 1, sourceHeight - 1);

                        Float32 expected = 0.0f;

                        for (UInt32 sy = y * 2; sy <= lastY; ++sy)
                        {
                            for (UInt32 sx = x * 2; sx <= lastX; ++sx)
                            {
                                expected = FMath::Max(
                                    expected,
                                    levels[level - 1][sy * sourceWidth + sx]);
                            }
                        }

                        const Float32 actual =
                            levels[level][y * targetWidth + x];

                        const Float32 delta = FMath::Abs(actual - expected);

                        if (delta != 0.0f)
                        {
                            ++violations;
                            worst = FMath::Max(worst, delta);
                        }
                    }
                }
            }

            // ---- 第 0 级必须**就是**深度缓冲区 ----
            //
            // 归约那一条只验"每一级是上一级的最大值", 它对内容一无所知 ——
            // 整张金字塔全是 0 也满分通过。而全是 0 意味着"最近", 于是
            // 一切都被判成挡住了。
            //
            // 这一条把内容钉住: 第 0 级是深度缓冲区的逐纹素拷贝, 差一个
            // 都不行。
            SizeType contentDiffer = 0;
            Float32  contentWorst  = 0.0f;

            for (SizeType i = 0; i < levels[0].GetSize() &&
                                 i < settledDepth.GetSize();
                 ++i)
            {
                const Float32 delta =
                    FMath::Abs(levels[0][i] - settledDepth[i]);

                if (delta != 0.0f)
                {
                    ++contentDiffer;
                    contentWorst = FMath::Max(contentWorst, delta);
                }
            }

            LIMX_LOG(LogLaunch, Display,
                     "[Meshlet遮挡] 金字塔第 0 级 vs 深度缓冲区 — 不同的"
                     "纹素 {} 个 (最大差 {})",
                     contentDiffer, contentWorst);

            if (contentDiffer != 0)
            {
                LIMX_LOG(LogLaunch, Error,
                         "[Meshlet遮挡] 金字塔第 0 级与深度缓冲区不符 "
                         "({} 个纹素, 最大差 {}) —— 拷贝那一步没生效? "
                         "全零的金字塔意味着'最近', 于是一切都被判成挡住了",
                         contentDiffer, contentWorst);
                passed = false;
            }

            LIMX_LOG(LogLaunch, Display,
                     "[Meshlet遮挡] 金字塔 — {} 级, 归约不符的纹素 {} 个 "
                     "(最大差 {})",
                     levelCount, violations, worst);

            if (violations != 0)
            {
                LIMX_LOG(LogLaunch, Error,
                         "[Meshlet遮挡] {} 个纹素不是上一级对应区域的最大值 "
                         "(最大差 {}) —— 取成最小或平均的话, 遮挡测试会把"
                         "只挡住一角的物体整个剔掉",
                         violations, worst);
                passed = false;
            }
        }
    }

    // ---- 判据四: 遮挡剔除必须真的剔掉了东西 ----
    LIMX_LOG(LogLaunch, Display,
             "[Meshlet遮挡] 第一阶段被遮挡剔掉 {} 个 meshlet (测试 {} 个)",
             stats.MeshletsPending, stats.MeshletsTested);

    if (stats.MeshletsPending == 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Meshlet遮挡] 第一阶段一个 meshlet 都没被遮挡剔掉 —— "
                 "这个场景/视角下判据是空的, 前两条判据对它满分通过");
        passed = false;
    }

    // ---- 判据五: 第二阶段必须真的补回来过东西 ----
    //
    // 这一条是被一个实打实的缺陷逼出来的: 第二阶段的间接分派参数忘了把
    // y/z 置 1, 于是 (n, 0, 0) —— 一个工作组都不起, 整个第二阶段是死代码。
    //
    // 而"开关遮挡剔除画面相同"那两条判据对它**满分通过**: 第一阶段剔掉的
    // 那些恰好真的被挡住了, 补不补都一样。
    //
    // 判据要盯的是"这条路径被走到了", 而不只是"结果看起来对"。这与 Day 9
    // 的分支覆盖是同一件事的另一个形状。
    LIMX_LOG(LogLaunch, Display,
             "[Meshlet遮挡] 第二阶段补回来 {} 个 meshlet", phase2Admitted);

    if (phase2Admitted == 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Meshlet遮挡] 第二阶段一个 meshlet 都没补回来 —— "
                 "要么它整个没跑 (分派参数?), 要么这次视角跳变没造出"
                 "\"上一帧挡住而这一帧露出来\"的情形");
        passed = false;
    }

    pass->SetOcclusionCullEnabled(false);
    cull->SetOcclusionCullEnabled(false);

    // ================================================================
    // 第二个摆位: 相机埋进柱子里
    //
    // 上面那个摆位走不到"投影失败"这条路径 —— 场景里没有一个 meshlet 的
    // 包围球会穿到近平面之内。于是第二阶段里"投影失败就保留"那一句怎么
    // 改都不会红: 把它改成"投影失败就剔掉", 判据全绿。
    //
    // 这是**场景不够**, 不是判据不严。补法是把相机放到 0 号柱子内部
    // (9.3, 1.75, 0) —— 柱子自己的 meshlet 把相机包在里面, 它们的包围球
    // 穿近平面, 投影必然失败。那时"存疑不剔"是画面正确的唯一依据。
    //
    // 判据的形状与上面那个一样: 开关遮挡剔除, 画面必须逐像素相同。
    // ================================================================
    {
        // 柱子摆在半径 6 * 1.55 = 9.3 的环上, 0 号在 angle = 0 处,
        // 中心高度 1.75 —— 见综合场景里那段 Spawn。
        const FVector3 pillarPosition(9.3f, 1.75f, 0.0f);

        TArray<Float32>           pillarDepthOff;
        TArray<Float32>           pillarDepthOn;
        TArray<FVisibilityTriple> pillarVisibilityOff;
        TArray<FVisibilityTriple> pillarVisibilityOn;

        camera.SetPosition(pillarPosition);
        camera.SetRotation(0.6f, -0.1f);

        pass->SetOcclusionCullEnabled(false);
        cull->SetOcclusionCullEnabled(false);

        renderer.RenderFrame();

        bool pillarOk = CaptureDepthAndVisibility(
            context, renderer, pass->GetDepthTexture(),
            pass->GetVisibilityTexture(), pillarDepthOff, pillarVisibilityOff);

        pass->SetOcclusionCullEnabled(true);
        cull->SetOcclusionCullEnabled(true);

        // 同样先在诱饵位置建金字塔, 再跳回来
        camera.SetRotation(0.8f, -0.1f);

        for (UInt32 i = 0; i < 4; ++i)
        {
            renderer.RenderFrame();
        }

        camera.SetRotation(0.6f, -0.1f);

        pillarOk = pillarOk &&
                   CaptureDepthAndVisibility(context, renderer,
                                             pass->GetDepthTexture(),
                                             pass->GetVisibilityTexture(),
                                             pillarDepthOn, pillarVisibilityOn);

        if (!pillarOk)
        {
            LIMX_LOG(LogLaunch, Error, "[Meshlet遮挡] 柱内摆位回读失败");
            passed = false;
        }
        else
        {
            SizeType depthDiffer = 0;
            Float32  depthWorst  = 0.0f;

            for (SizeType i = 0; i < pillarDepthOff.GetSize() &&
                                 i < pillarDepthOn.GetSize();
                 ++i)
            {
                const Float32 delta =
                    FMath::Abs(pillarDepthOff[i] - pillarDepthOn[i]);

                if (delta != 0.0f)
                {
                    ++depthDiffer;
                    depthWorst = FMath::Max(depthWorst, delta);
                }
            }

            SizeType visibilityDiffer = 0;

            for (SizeType i = 0; i < pillarVisibilityOff.GetSize() &&
                                 i < pillarVisibilityOn.GetSize();
                 ++i)
            {
                const FVisibilityTriple& a = pillarVisibilityOff[i];
                const FVisibilityTriple& b = pillarVisibilityOn[i];

                if (a.Valid != b.Valid || a.Instance != b.Instance ||
                    a.Meshlet != b.Meshlet || a.Triangle != b.Triangle)
                {
                    ++visibilityDiffer;
                }
            }

            LIMX_LOG(LogLaunch, Display,
                     "[Meshlet遮挡] 柱内摆位 — 深度不同 {} 个像素 "
                     "(最大差 {}), 可见性不同 {} 个像素",
                     depthDiffer, depthWorst, visibilityDiffer);

            if (depthDiffer != 0 || visibilityDiffer != 0)
            {
                LIMX_LOG(LogLaunch, Error,
                         "[Meshlet遮挡] 相机埋在柱子里时开关遮挡剔除画面"
                         "不同 —— 包围球穿近平面时投影会失败, 而那时"
                         "**必须保留**: 剔了就没有下一次机会了");
                passed = false;
            }
        }
    }

    // ================================================================
    // Hi-Z 探针 —— 相机**必须歪着**
    //
    // 上面那两个摆位的 yaw 与 pitch 都接近 0, 而那时视图矩阵的旋转部分是
    // 单位阵 —— 视图投影矩阵的第 0 行与第 0 列**恰好相等**。于是"相机右
    // 方向取列而不是取行"这条变异在那里完全看不出来: 两个取法拿到同一个
    // 向量。实测把它改成取列, 探针的每一个数都分毫不差。
    //
    // 这是**判据的场景不够**, 不是判据不严。所以探针自己摆一个 yaw 与
    // pitch 都不为零的姿态 —— 那时旋转矩阵没有一个零元素, 行与列差得很开。
    //
    // 金字塔的第 0 级也要在这个姿态下重新读: 探针拿它当参考答案, 而它必须
    // 与探针看到的是同一帧的同一个金字塔。
    // ================================================================
    {
        camera.SetPosition(targetPosition);
        camera.SetRotation(0.9f, -0.35f);

        for (UInt32 i = 0; i < 4; ++i)
        {
            renderer.RenderFrame();
        }

        TArray<Float32> probeLevel0;

        if (!ReadHizLevel(context, renderer, pass->GetHizTexture(), extent, 0,
                          pass->GetHizLevelCount(), probeLevel0))
        {
            LIMX_LOG(LogLaunch, Error, "[Hi-Z探针] 第 0 级回读失败");
            passed = false;
        }
        else if (!RunHizProbeChecks(context, renderer, probeLevel0))
        {
            passed = false;
        }
    }

    Restore();

    if (!renderer.SetMeshletDepthEnabled(false))
    {
        LIMX_LOG(LogLaunch, Error, "[Meshlet遮挡] 无法关闭 (复位)");
        passed = false;
    }

    LIMX_LOG(LogLaunch, Display, "[Meshlet遮挡] {}", passed ? "通过" : "失败");

    return passed;
}

// ============================================================================
// RunMeshletCullChecks — 两级剔除必须与 CPU 参考实现逐个 meshlet 一致
//
// 判据的形状: 把 GPU 剔出来的那张可见表读回来, 与一份 CPU 参考实现算出来的
// 集合逐条比。**不是比数量, 是比集合** —— 数量相同而内容不同是完全可能的
// (剔错一个、又漏剔一个), 而那在画面上是"某处少一块, 某处多一块"。
//
// 参考实现从哪来: 逐字照抄 meshlet_common.h 里那三个函数
// (MeshletWorldSphere / MeshletSphereVisible / MeshletBackfaceCull)。
// "逐字照抄"是有意的 —— 各写各的话, 判据比的是"两个实现一不一样",
// 而两个实现可以一起错。照抄的话, 比的是"GPU 上跑的与 CPU 上跑的是不是
// 同一段逻辑", 而这条判据真正要拦的正是那些让两者分道扬镳的东西:
// 描述符绑错、push constant 布局不对、屏障漏了、原子累加溢出。
//
// 输入也从 GPU 读: meshlet 头是从**汇总缓冲区**读回来的, 不是 CPU 内存里
// 那份。这样上传与汇总拷贝这两段路径也进了覆盖 —— 用 CPU 那份的话,
// "GPU 拷贝写错了地方"这类缺陷完全没有痕迹。
//
// 五条判据:
//   1. 可见集合完全相同 (GPU 与 CPU 参考实现)
//   2. 计数器自洽: 测试数 = 可见 + 视锥剔 + 背面剔
//   3. 剔除必须真的剔掉了东西 —— 一个什么都不剔的实现在画面上与正确实现
//      完全一样, 只是慢
//   4. 关掉背面剔除之后, 可见集合必须是开着时的**超集**
//   5. 法线锥无效的 meshlet 一个都不许被背面剔除剔掉
// ============================================================================

namespace
{

/// 与 FMeshlet 逐字段一致 —— 从 GPU 读回来的那份就是这个布局
struct FMeshletGpuView
{
    UInt32 VertexOffset;
    UInt32 VertexCount;
    UInt32 TriangleOffset;
    UInt32 TriangleCount;

    Float32 Sphere[4];
    Float32 Cone[4];
};

static_assert(sizeof(FMeshletGpuView) == 48,
              "FMeshletGpuView 必须与 FMeshlet 同布局");

/// 逐字照抄 meshlet_common.h 的 MeshletWorldSphere
void ReferenceWorldSphere(const Float32 localSphere[4],
                          const FMeshletInstanceGpu& instance,
                          Float32 outSphere[4])
{
    const Float32* r0 = instance.TransformRow0;
    const Float32* r1 = instance.TransformRow1;
    const Float32* r2 = instance.TransformRow2;

    outSphere[0] = r0[0] * localSphere[0] + r0[1] * localSphere[1] +
                   r0[2] * localSphere[2] + r0[3];
    outSphere[1] = r1[0] * localSphere[0] + r1[1] * localSphere[1] +
                   r1[2] * localSphere[2] + r1[3];
    outSphere[2] = r2[0] * localSphere[0] + r2[1] * localSphere[1] +
                   r2[2] * localSphere[2] + r2[3];

    const auto ColumnLength = [r0, r1, r2](UInt32 c) -> Float32
    {
        return FMath::Sqrt(r0[c] * r0[c] + r1[c] * r1[c] + r2[c] * r2[c]);
    };

    const Float32 scale = FMath::Max(
        ColumnLength(0), FMath::Max(ColumnLength(1), ColumnLength(2)));

    outSphere[3] = localSphere[3] * scale;
}

/// 逐字照抄 meshlet_common.h 的 MeshletUniformScale
bool ReferenceUniformScale(const FMeshletInstanceGpu& instance)
{
    const Float32* r0 = instance.TransformRow0;
    const Float32* r1 = instance.TransformRow1;
    const Float32* r2 = instance.TransformRow2;

    const auto ColumnLength = [r0, r1, r2](UInt32 c) -> Float32
    {
        return FMath::Sqrt(r0[c] * r0[c] + r1[c] * r1[c] + r2[c] * r2[c]);
    };

    const Float32 x = ColumnLength(0);
    const Float32 y = ColumnLength(1);
    const Float32 z = ColumnLength(2);

    const Float32 maximum = FMath::Max(x, FMath::Max(y, z));
    const Float32 minimum = FMath::Min(x, FMath::Min(y, z));

    return (maximum - minimum) <= maximum * 1.0e-3f + 1.0e-9f;
}

/// 逐字照抄 meshlet_common.h 的 MeshletSphereVisible
///
/// 多一个出参: 哪些平面把它判成不可见的 (位掩码)。判据要靠它确认
/// **每一个平面都单独起过作用** —— 只测五个平面这类缺陷, 在"第六个平面
/// 从来没单独剔掉过任何东西"的场景里完全没有痕迹。
bool ReferenceSphereVisible(const Float32 sphere[4], const FFrustum& frustum,
                            UInt32& outFailMask)
{
    outFailMask = 0;

    for (Int32 p = 0; p < FFrustum::kPlaneCount; ++p)
    {
        const Float32 distance = frustum.Planes[p].Normal.X * sphere[0] +
                                 frustum.Planes[p].Normal.Y * sphere[1] +
                                 frustum.Planes[p].Normal.Z * sphere[2] +
                                 frustum.Planes[p].D;

        if (distance < -sphere[3])
        {
            outFailMask |= (1u << p);
        }
    }

    return outFailMask == 0;
}

/// 背面判据走到了哪条 early-out
enum class EBackfaceBranch : UInt32
{
    Tested,
    InvalidCone,
    NonUniformScale,
};

/// 逐字照抄 meshlet_common.h 的 MeshletBackfaceCull
///
/// 多一个出参: 走到了哪条 early-out。分支覆盖是判据的一部分 —— 没走到的
/// 分支上, "GPU 与 CPU 一致"什么也证明不了。
bool ReferenceBackfaceCull(const Float32 cone[4], const Float32 sphere[4],
                           const FVector3& cameraPosition,
                           const FMeshletInstanceGpu& instance,
                           EBackfaceBranch& outBranch)
{
    outBranch = EBackfaceBranch::Tested;

    if (cone[3] <= kInvalidConeCosine)
    {
        outBranch = EBackfaceBranch::InvalidCone;
        return false;
    }

    if (!ReferenceUniformScale(instance))
    {
        outBranch = EBackfaceBranch::NonUniformScale;
        return false;
    }

    const Float32* r0 = instance.TransformRow0;
    const Float32* r1 = instance.TransformRow1;
    const Float32* r2 = instance.TransformRow2;

    Float32 axis[3] = {
        r0[0] * cone[0] + r0[1] * cone[1] + r0[2] * cone[2],
        r1[0] * cone[0] + r1[1] * cone[1] + r1[2] * cone[2],
        r2[0] * cone[0] + r2[1] * cone[1] + r2[2] * cone[2],
    };

    const Float32 axisLength = FMath::Sqrt(
        axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);

    if (axisLength < 1.0e-20f)
    {
        return false;
    }

    axis[0] /= axisLength;
    axis[1] /= axisLength;
    axis[2] /= axisLength;

    const Float32 d[3] = {
        sphere[0] - cameraPosition.X,
        sphere[1] - cameraPosition.Y,
        sphere[2] - cameraPosition.Z,
    };

    const Float32 distance =
        FMath::Sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);

    return (d[0] * axis[0] + d[1] * axis[1] + d[2] * axis[2]) >=
           cone[3] * distance + sphere[3];
}

/// 一次剔除的结果 —— (实例, meshlet) 对的集合, 按 64 位键排序
struct FCullCapture
{
    bool Valid = false;

    TArray<UInt64> GpuVisible;
    TArray<UInt64> CpuVisible;

    UInt32 Counters[4] = { 0, 0, 0, 0 };

    /// CPU 参考实现算出的可见实例数
    UInt32 InstancesVisible = 0;

    // ====================================================================
    // 分支覆盖 —— 每条判据里的每个分支被走到了几次
    //
    // 这些数不是诊断, 是**判据**。Day 9 的第一轮扫描里五条逃逸有四条同一
    // 个成因: 那个分支在这个视角下根本没被走到, 于是改坏它没有任何后果,
    // 而 GPU 与 CPU 参考实现"完美一致"。
    //
    // 一致性判据对没走到的分支毫无约束。所以每个分支都要有一个计数器,
    // 而每个计数器都要有一条"必须大于零"的元判据。
    // ====================================================================

    /// 第 p 个视锥平面**独自**剔掉的 meshlet 数
    ///
    /// "独自"要紧: 只统计"被 p 剔掉"的话, 一个 meshlet 同时在三个平面
    /// 外面就给三个平面各记一笔, 而其中两个平面可能从来没有单独起过作用。
    UInt32 PlaneExclusiveCulls[6] = { 0, 0, 0, 0, 0, 0 };

    /// 背面判据里三条 early-out 各走了几次
    UInt32 InvalidConeEarlyOut = 0;
    UInt32 NonUniformScaleEarlyOut = 0;

    /// 第一级剔掉的实例里, 有几个的 meshlet 在第二级判据下**仍然可见**
    ///
    /// 必须恒为零。不为零说明实例包围球不保守 —— 它剔掉了一个其实还有
    /// meshlet 露在视锥里的实例, 而那是画面上整块东西消失。
    UInt32 InstanceCullTooAggressive = 0;

    /// 一个能让相机落进包围球的位置 —— 必须是**法线锥有效且缩放均匀**的
    /// meshlet, 否则背面判据会在更早的 early-out 上返回, "相机在球内"
    /// 那条永远走不到。
    ///
    /// 取半径最大的那个: 半径越大, 相机放进去越不挑位置。
    FVector3 InsideCameraTarget = FVector3(0.0f, 0.0f, 0.0f);
    Float32  InsideCameraRadius = 0.0f;

};

/// 一次帧内回调里把三个缓冲区一起读回来
///
/// 走**帧内回调**而不是一次性命令缓冲区: 这个引擎没有后者的接口, 而更要紧
/// 的是, 帧内回调保证读到的是**这一帧剔除刚写完的那份**。另起一次提交的话,
/// 中间隔着帧的推进, 读到的可能是下一帧覆盖过的值 —— 而那种错只在特定的
/// 帧序下出现。
struct FCullReadbackRequest
{
    FRHIBufferHandle Source;
    UInt64           Bytes = 0;
    TArray<UInt8>*   Target = nullptr;

    FRHIBufferHandle Staging;
};

bool ReadCullBuffers(FRenderContext* context, FRenderer& renderer,
                     FCullReadbackRequest* requests, UInt32 count)
{
    IRHIDevice* const device = context->GetDevice();

    if (device == nullptr)
    {
        return false;
    }

    bool created = true;

    for (UInt32 i = 0; i < count; ++i)
    {
        if (!requests[i].Source.IsValid() || requests[i].Bytes == 0)
        {
            created = false;
            break;
        }

        FRHIBufferDesc desc = {};
        desc.Size        = requests[i].Bytes;
        desc.Usage       = EBufferUsage::TransferDst;
        desc.MemoryUsage = EMemoryUsage::GpuToCpu;
        desc.DebugName   = "MeshletCullCheck.Readback";

        if (!IsRHISuccess(device->CreateBuffer(desc, requests[i].Staging)))
        {
            created = false;
            break;
        }
    }

    bool recorded = false;

    if (created)
    {
        renderer.SetPostSceneRenderCallback(
            [&recorded, context, requests, count]()
            {
                IRHICommandBuffer* cmd = context->GetCurrentCommandBuffer();

                if (cmd == nullptr)
                {
                    return;
                }

                for (UInt32 i = 0; i < count; ++i)
                {
                    FRHIBufferCopyRegion region = {};
                    region.SrcOffset = 0;
                    region.DstOffset = 0;
                    region.Size      = requests[i].Bytes;

                    cmd->CopyBuffer(requests[i].Source, requests[i].Staging,
                                    region);
                }

                recorded = true;
            });

        renderer.RenderFrame();
        renderer.SetPostSceneRenderCallback(TFunction<void()>());
    }

    bool ok = created && recorded;

    if (ok)
    {
        device->WaitIdle();

        for (UInt32 i = 0; i < count; ++i)
        {
            void* mapped = nullptr;

            if (!IsRHISuccess(device->MapBuffer(requests[i].Staging, &mapped)) ||
                mapped == nullptr)
            {
                ok = false;
                break;
            }

            const auto* source = static_cast<const UInt8*>(mapped);

            requests[i].Target->Clear();
            requests[i].Target->Reserve(
                static_cast<SizeType>(requests[i].Bytes));

            for (UInt64 b = 0; b < requests[i].Bytes; ++b)
            {
                requests[i].Target->Add(source[b]);
            }

            device->UnmapBuffer(requests[i].Staging);
        }
    }

    for (UInt32 i = 0; i < count; ++i)
    {
        if (requests[i].Staging.IsValid())
        {
            device->DestroyBuffer(requests[i].Staging);
        }
    }

    return ok;
}

/// 跑一次剔除, 同时算出 CPU 参考结果
FCullCapture CaptureMeshletCull(FRenderContext* context, FRenderer& renderer,
                                bool backfaceCull)
{
    FCullCapture capture;

    FMeshletCullPass* const pass = renderer.GetMeshletCullPass();

    if (pass == nullptr)
    {
        return capture;
    }

    pass->SetBackfaceCullEnabled(backfaceCull);

    // 先渲一帧, 让汇总缓冲区与实例表建起来 —— 回读那一帧要用它们的尺寸
    // 来决定拷多少字节, 而那必须在录回调**之前**知道。
    renderer.RenderFrame();

    IRHIDevice* const device = context->GetDevice();

    device->WaitIdle();

    const UInt32 meshletCount = pass->GetSceneMeshletCount();

    const TArray<FMeshletInstanceGpu>& instances = pass->GetInstances();

    if (meshletCount == 0 || instances.IsEmpty())
    {
        return capture;
    }

    // ---- 三个缓冲区一起读回 ----
    //
    // 可见表要读多少: 上界是"全部实例的 meshlet 数之和" —— 每个 meshlet
    // 至多在表里出现一次。用计数器的值当长度是不行的, 那要先读一次计数器
    // 再读一次表, 而两次之间帧已经往前走了。
    UInt64 pairUpperBound = 0;

    for (SizeType i = 0; i < instances.GetSize(); ++i)
    {
        pairUpperBound += instances[i].MeshletRange[1];
    }

    if (pairUpperBound == 0)
    {
        return capture;
    }

    TArray<UInt8> meshletBytes;
    TArray<UInt8> counterBytes;
    TArray<UInt8> visibleBytes;

    const UInt32 frameIndex = context->GetCurrentFrameIndex();

    FCullReadbackRequest requests[3];

    requests[0].Source = pass->GetSceneMeshletBuffer();
    requests[0].Bytes =
        static_cast<UInt64>(meshletCount) * sizeof(FMeshletGpuView);
    requests[0].Target = &meshletBytes;

    requests[1].Source = pass->GetCounterBuffer(frameIndex);
    requests[1].Bytes  = sizeof(UInt32) * 4;
    requests[1].Target = &counterBytes;

    requests[2].Source = pass->GetVisibleMeshletBuffer(frameIndex);
    requests[2].Bytes  = pairUpperBound * sizeof(UInt32) * 2;
    requests[2].Target = &visibleBytes;

    if (!ReadCullBuffers(context, renderer, requests, 3))
    {
        return capture;
    }

    const auto* meshlets =
        reinterpret_cast<const FMeshletGpuView*>(meshletBytes.GetData());

    {
        const auto* counters =
            reinterpret_cast<const UInt32*>(counterBytes.GetData());

        for (UInt32 i = 0; i < 4; ++i)
        {
            capture.Counters[i] = counters[i];
        }
    }

    const UInt32 visibleCount = capture.Counters[0];

    if (visibleCount > pairUpperBound)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Meshlet剔除] 可见数 {} 超过上界 {} —— 计数器溢出了?",
                 visibleCount, pairUpperBound);
        return capture;
    }

    {
        const auto* pairs =
            reinterpret_cast<const UInt32*>(visibleBytes.GetData());

        capture.GpuVisible.Reserve(visibleCount);

        for (UInt32 i = 0; i < visibleCount; ++i)
        {
            capture.GpuVisible.Add(
                (static_cast<UInt64>(pairs[i * 2 + 0]) << 32) |
                static_cast<UInt64>(pairs[i * 2 + 1]));
        }
    }

    // ---- CPU 参考实现 ----
    const FFrustum& frustum = pass->GetFrustum();

    const FVector3& cameraPosition = pass->GetCameraPosition();

    const TArray<Float32>& instanceSpheres = pass->GetInstanceSpheres();

    if (instanceSpheres.GetSize() != instances.GetSize() * 4)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Meshlet剔除] 实例包围球表长度 {} 与实例数 {} 对不上",
                 instanceSpheres.GetSize(), instances.GetSize());
        return capture;
    }

    for (SizeType i = 0; i < instances.GetSize(); ++i)
    {
        const FMeshletInstanceGpu& instance = instances[i];

        // ---- 第一级: 实例 ----
        //
        // 这一级不能省。第一版的参考实现跳过了它, 理由是"实例包围球包住
        // 它的每个 meshlet 包围球, 所以第一级剔掉的第二级也会剔掉" ——
        // 那个直觉**不成立**: meshlet 的包围球会从实例包围盒的角上鼓出去,
        // 于是一个刚好在视锥外的实例, 它的某个 meshlet 的包围球仍然可能
        // 与视锥相交。实测差 2 个 meshlet, 判据当场报了出来。
        //
        // 教训是参考实现要照着**真实的算法**写, 不是照着一个"应该等价"的
        // 简化版写。等价性本身是要证明的东西, 而这里它是假的。
        const Float32 instanceSphere[4] = {
            instanceSpheres[i * 4 + 0],
            instanceSpheres[i * 4 + 1],
            instanceSpheres[i * 4 + 2],
            instanceSpheres[i * 4 + 3],
        };

        UInt32 instanceFailMask = 0;

        const bool instanceVisible =
            ReferenceSphereVisible(instanceSphere, frustum, instanceFailMask);

        const UInt32 base  = instance.MeshletRange[0];
        const UInt32 count = instance.MeshletRange[1];

        if (instanceVisible)
        {
            ++capture.InstancesVisible;
        }

        for (UInt32 m = 0; m < count; ++m)
        {
            const UInt32 meshletIndex = base + m;

            if (meshletIndex >= meshletCount)
            {
                continue;
            }

            const FMeshletGpuView& meshlet = meshlets[meshletIndex];

            Float32 worldSphere[4] = {};

            ReferenceWorldSphere(meshlet.Sphere, instance, worldSphere);

            UInt32 failMask = 0;

            const bool meshletVisible =
                ReferenceSphereVisible(worldSphere, frustum, failMask);

            // ---- 第一级的保守性 ----
            //
            // 实例被第一级剔掉了, 那它的每个 meshlet 都必须在第二级的
            // 视锥判据下也不可见。不然第一级就剔掉了本来看得见的东西 ——
            // 而那是画面上整块消失, 与"这块本来就不在视野里"长得一样。
            //
            // 这一条是判据里唯一一个**不依赖 GPU**的: 它验的是两级之间
            // 的关系, 而 GPU 那边被剔的实例压根不会进入第二级, 于是集合
            // 比对对它没有任何约束。
            if (!instanceVisible)
            {
                if (meshletVisible)
                {
                    ++capture.InstanceCullTooAggressive;
                }

                continue;
            }

            if (!meshletVisible)
            {
                // 只有一个平面判它出局时, 记在那个平面头上
                UInt32 exclusive = 0xFFFFFFFFu;

                for (UInt32 p = 0; p < 6; ++p)
                {
                    if ((failMask & (1u << p)) == 0)
                    {
                        continue;
                    }

                    if (exclusive != 0xFFFFFFFFu)
                    {
                        exclusive = 0xFFFFFFFFu;
                        break;
                    }

                    exclusive = p;
                }

                if (exclusive < 6)
                {
                    ++capture.PlaneExclusiveCulls[exclusive];
                }

                continue;
            }

            // 记下一个"能让相机落进去"的候选。条件是法线锥有效 + 缩放
            // 均匀 —— 那两条 early-out 排在"相机在球内"之前。
            if (meshlet.Cone[3] > kInvalidConeCosine &&
                ReferenceUniformScale(instance) &&
                worldSphere[3] > capture.InsideCameraRadius)
            {
                capture.InsideCameraRadius = worldSphere[3];
                capture.InsideCameraTarget =
                    FVector3(worldSphere[0], worldSphere[1], worldSphere[2]);
            }

            if (backfaceCull)
            {
                EBackfaceBranch branch = EBackfaceBranch::Tested;

                const bool culled = ReferenceBackfaceCull(
                    meshlet.Cone, worldSphere, cameraPosition, instance,
                    branch);

                switch (branch)
                {
                case EBackfaceBranch::InvalidCone:
                    ++capture.InvalidConeEarlyOut;
                    break;
                case EBackfaceBranch::NonUniformScale:
                    ++capture.NonUniformScaleEarlyOut;
                    break;
                default:
                    break;
                }

                if (culled)
                {
                    continue;
                }
            }

            capture.CpuVisible.Add((static_cast<UInt64>(i) << 32) |
                                   static_cast<UInt64>(meshletIndex));
        }
    }

    Sort(capture.GpuVisible.GetData(), capture.GpuVisible.GetSize());
    Sort(capture.CpuVisible.GetData(), capture.CpuVisible.GetSize());

    capture.Valid = true;

    return capture;
}

} // namespace

// ============================================================================
// RunBackfaceGroundTruthCheck — 背面剔除的**地面真值**
//
// 上面那条"GPU 与 CPU 参考实现逐条相同"只验**转写**, 不验公式对不对: CPU
// 参考照着同一个公式写, 两份同样的错互相印证不出任何东西。
//
// 第十四天的规模压力测试正是这么栽的 —— 法线锥存进去的是半角**余弦**, 而
// 背面剔除要拿来比的是半角**正弦**。半角小于 45 度时余弦大于正弦, 那个错
// 只表现为漏剔 (保守, 画面全对); 越过 45 度就翻过来成了错剔。综合场景里的
// 球分段密, meshlet 跨的曲率小, 半角一直在 45 度以内, 于是 Day 9 的判据、
// Day 10 的逐位相同、Day 12 的解析判据**全绿**; 换成压力场景里 16x12 段的
// 球, 立刻有 1490 个像素画的是背后的东西。
//
// 这一条不走公式: 对每一个**被背面剔除剔掉的** meshlet, 把它的每个三角形
// 变换到世界空间, 逐个验它确实背对着相机。这是"背面剔除"这四个字的定义,
// 与用什么锥、锥怎么存、锥怎么算都无关。
//
// 只验一个方向: 剔掉的必须全背对。反过来"全背对的必须被剔掉"是效率而不是
// 正确性 —— 法线锥本来就是保守近似, 漏剔是它的正常工作方式。
// ============================================================================

namespace
{

/// 一个三角形正对着相机吗
///
/// 判据: 外法线朝着相机。而"外法线"是 cross(b-a, c-a) —— 这一点不是假设,
/// 是**量出来的**: 下面那段元判据每次运行都数一遍它与顶点里存的作者法线
/// 是否同向, 综合场景上是 6450/6450。
///
/// ── 为什么不按屏幕空间的绕序判 ──
///
/// 第一版按 Vulkan 的定义做: 投到裁剪空间, 算归一化设备坐标里的有符号面积,
/// 管线声明 FrontFace::CounterClockwise 就取面积为正。那一版的结论与这一版
/// **恰好相反** (6450 个三角形里只有 8 个一致, 而那 8 个是退化的)。
///
/// 差在投影矩阵翻不翻 Y: 翻了的话归一化设备坐标里的绕序就与世界空间反过来。
/// 从"管线声明了什么"推回世界空间要先知道这一点, 而那是猜。外法线不用猜 ——
/// 它与作者法线同不同向是能当场数出来的。
///
/// 三个顶点里有任何一个与相机重合就返回 false: 那时"朝向"没有定义。
bool TriangleFacesCamera(const FVector3& a, const FVector3& b,
                         const FVector3& c, const FVector3& cameraPosition)
{
    const FVector3 ab(b.X - a.X, b.Y - a.Y, b.Z - a.Z);
    const FVector3 ac(c.X - a.X, c.Y - a.Y, c.Z - a.Z);

    const FVector3 normal(ab.Y * ac.Z - ab.Z * ac.Y, ab.Z * ac.X - ab.X * ac.Z,
                          ab.X * ac.Y - ab.Y * ac.X);

    const FVector3 view(a.X - cameraPosition.X, a.Y - cameraPosition.Y,
                        a.Z - cameraPosition.Z);

    return (normal.X * view.X + normal.Y * view.Y + normal.Z * view.Z) < 0.0f;
}

/// 把 meshlet 的第 t 个三角形取出来并变换到世界空间
bool FetchWorldTriangle(const FMeshletInstanceGpu& instance,
                        const FMeshletGpuView& meshlet, UInt32 triangle,
                        const TArray<UInt8>& vertexBytes,
                        const TArray<UInt32>& meshletVertices,
                        const TArray<UInt32>& meshletTriangles,
                        FVector3* outPositions, FVector3* outNormal = nullptr)
{
    const UInt32 vertexBase          = instance.BufferBases[0];
    const UInt32 meshletVertexBase   = instance.BufferBases[1];
    const UInt32 meshletTriangleBase = instance.BufferBases[2];

    const UInt32 byteBase =
        meshletTriangleBase + (meshlet.TriangleOffset + triangle) * 3u;

    for (UInt32 k = 0; k < 3; ++k)
    {
        const UInt32 byteIndex = byteBase + k;
        const UInt32 word      = byteIndex >> 2;

        if (word >= meshletTriangles.GetSize())
        {
            return false;
        }

        const UInt32 local =
            (meshletTriangles[word] >> ((byteIndex & 3u) * 8u)) & 0xFFu;

        const UInt32 tableIndex =
            meshletVertexBase + meshlet.VertexOffset + local;

        if (local >= meshlet.VertexCount ||
            tableIndex >= meshletVertices.GetSize())
        {
            return false;
        }

        const UInt32 globalVertex = meshletVertices[tableIndex];

        const SizeType offset =
            (static_cast<SizeType>(vertexBase) + globalVertex) *
            sizeof(FMeshVertex);

        if (offset + sizeof(FMeshVertex) > vertexBytes.GetSize())
        {
            return false;
        }

        const auto* vertex = reinterpret_cast<const FMeshVertex*>(
            vertexBytes.GetData() + offset);

        const FVector3& p = vertex->Position;

        if (k == 0 && outNormal != nullptr)
        {
            const FVector3& n = vertex->Normal;
            *outNormal = FVector3(
                instance.TransformRow0[0] * n.X +
                    instance.TransformRow0[1] * n.Y +
                    instance.TransformRow0[2] * n.Z,
                instance.TransformRow1[0] * n.X +
                    instance.TransformRow1[1] * n.Y +
                    instance.TransformRow1[2] * n.Z,
                instance.TransformRow2[0] * n.X +
                    instance.TransformRow2[1] * n.Y +
                    instance.TransformRow2[2] * n.Z);
        }

        // 3x4 仿射变换 —— 与着色器里那三行是同一套
        outPositions[k] = FVector3(
            instance.TransformRow0[0] * p.X + instance.TransformRow0[1] * p.Y +
                instance.TransformRow0[2] * p.Z + instance.TransformRow0[3],
            instance.TransformRow1[0] * p.X + instance.TransformRow1[1] * p.Y +
                instance.TransformRow1[2] * p.Z + instance.TransformRow1[3],
            instance.TransformRow2[0] * p.X + instance.TransformRow2[1] * p.Y +
                instance.TransformRow2[2] * p.Z + instance.TransformRow2[3]);
    }

    return true;
}

} // namespace

static bool RunBackfaceGroundTruthCheck(FRenderContext*     context,
                                        FRenderer&          renderer,
                                        const FCullCapture& withCull,
                                        const FCullCapture& withoutCull)
{
    FMeshletCullPass* const pass = renderer.GetMeshletCullPass();

    if (pass == nullptr)
    {
        return false;
    }

    const TArray<FMeshletInstanceGpu>& instances = pass->GetInstances();

    const UInt32 meshletCount = pass->GetSceneMeshletCount();

    if (meshletCount == 0 || instances.IsEmpty())
    {
        LIMX_LOG(LogLaunch, Error, "[Meshlet背面] 场景是空的 — 判据没验到东西");
        return false;
    }

    // ---- 回读几何 ----
    //
    // 上界从 meshlet 头与实例基址算出来, 不是猜的: 每个 meshlet 用到的最后
    // 一个下标就是 (基址 + 偏移 + 个数), 取全场景的最大值。
    TArray<UInt8> meshletBytes;
    TArray<UInt8> vertexBytes;
    TArray<UInt8> meshletVertexBytes;
    TArray<UInt8> meshletTriangleBytes;

    {
        FCullReadbackRequest headRequest;
        headRequest.Source = pass->GetSceneMeshletBuffer();
        headRequest.Bytes =
            static_cast<UInt64>(meshletCount) * sizeof(FMeshletGpuView);
        headRequest.Target = &meshletBytes;

        if (!ReadCullBuffers(context, renderer, &headRequest, 1))
        {
            LIMX_LOG(LogLaunch, Error, "[Meshlet背面] meshlet 头回读失败");
            return false;
        }
    }

    const auto* meshlets =
        reinterpret_cast<const FMeshletGpuView*>(meshletBytes.GetData());

    UInt64 vertexBound          = 0;
    UInt64 meshletVertexBound   = 0;
    UInt64 meshletTriangleBound = 0;

    for (SizeType i = 0; i < instances.GetSize(); ++i)
    {
        const FMeshletInstanceGpu& instance = instances[i];

        const UInt32 first = instance.MeshletRange[0];
        const UInt32 count = instance.MeshletRange[1];

        for (UInt32 m = 0; m < count; ++m)
        {
            const UInt32 index = first + m;

            if (index >= meshletCount)
            {
                continue;
            }

            const FMeshletGpuView& meshlet = meshlets[index];

            meshletVertexBound = FMath::Max(
                meshletVertexBound,
                static_cast<UInt64>(instance.BufferBases[1]) +
                    meshlet.VertexOffset + meshlet.VertexCount);

            meshletTriangleBound = FMath::Max(
                meshletTriangleBound,
                static_cast<UInt64>(instance.BufferBases[2]) +
                    static_cast<UInt64>(meshlet.TriangleOffset +
                                        meshlet.TriangleCount) *
                        3u);
        }

    }

    // 顶点缓冲区的上界要**先读局部表**才知道 —— 表里存的是全局顶点下标,
    // 而实例表里没有"这份网格有多少顶点"这一项。所以分两趟读: 先读局部表
    // 与三角形, 从表里数出最大的全局下标, 再按那个数读顶点。
    //
    // 拍一个"够大"的数读整个顶点缓冲区是不行的: 它按 400 万顶点开的,
    // 一次读回来是 288 MiB。
    {
        FCullReadbackRequest requests[2];

        requests[0].Source = pass->GetSceneMeshletVertexBuffer();
        requests[0].Bytes  = meshletVertexBound * sizeof(UInt32);
        requests[0].Target = &meshletVertexBytes;

        requests[1].Source = pass->GetSceneMeshletTriangleBuffer();
        requests[1].Bytes  = ((meshletTriangleBound + 3u) / 4u) * 4u;
        requests[1].Target = &meshletTriangleBytes;

        if (requests[0].Bytes == 0 || requests[1].Bytes == 0)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[Meshlet背面] 几何上界算出来是 0 — 场景里没有三角形?");
            return false;
        }

        if (!ReadCullBuffers(context, renderer, requests, 2))
        {
            LIMX_LOG(LogLaunch, Error, "[Meshlet背面] 局部表回读失败");
            return false;
        }
    }

    TArray<UInt32> meshletVertices;
    TArray<UInt32> meshletTriangles;

    {
        const auto* values =
            reinterpret_cast<const UInt32*>(meshletVertexBytes.GetData());

        const SizeType count = meshletVertexBytes.GetSize() / sizeof(UInt32);

        meshletVertices.Reserve(count);

        for (SizeType i = 0; i < count; ++i)
        {
            meshletVertices.Add(values[i]);
        }
    }

    {
        const auto* values =
            reinterpret_cast<const UInt32*>(meshletTriangleBytes.GetData());

        const SizeType count = meshletTriangleBytes.GetSize() / sizeof(UInt32);

        meshletTriangles.Reserve(count);

        for (SizeType i = 0; i < count; ++i)
        {
            meshletTriangles.Add(values[i]);
        }
    }

    // 从局部表里数出最大的全局顶点下标
    for (SizeType i = 0; i < instances.GetSize(); ++i)
    {
        const FMeshletInstanceGpu& instance = instances[i];

        const UInt32 first = instance.MeshletRange[0];
        const UInt32 count = instance.MeshletRange[1];

        for (UInt32 m = 0; m < count; ++m)
        {
            const UInt32 index = first + m;

            if (index >= meshletCount)
            {
                continue;
            }

            const FMeshletGpuView& meshlet = meshlets[index];

            for (UInt32 v = 0; v < meshlet.VertexCount; ++v)
            {
                const SizeType tableIndex =
                    static_cast<SizeType>(instance.BufferBases[1]) +
                    meshlet.VertexOffset + v;

                if (tableIndex < meshletVertices.GetSize())
                {
                    vertexBound = FMath::Max(
                        vertexBound,
                        static_cast<UInt64>(instance.BufferBases[0]) +
                            meshletVertices[tableIndex] + 1u);
                }
            }
        }
    }

    {
        FCullReadbackRequest request;
        request.Source = pass->GetSceneVertexBuffer();
        request.Bytes  = vertexBound * sizeof(FMeshVertex);
        request.Target = &vertexBytes;

        if (request.Bytes == 0 ||
            !ReadCullBuffers(context, renderer, &request, 1))
        {
            LIMX_LOG(LogLaunch, Error, "[Meshlet背面] 顶点回读失败");
            return false;
        }
    }

    // ---- 元判据: 叉积法线确实是**外**法线 ----
    //
    // 整条判据建立在"cross(b-a, c-a) 指向外面"这一句上。它不是假设 —— 每次
    // 运行都数一遍它与顶点里存的作者法线同不同向。作者法线对生成的几何体
    // 一律朝外, 于是这个数就是那句话的证据。
    //
    // 顺带也验了取几何这一步: 下标算错的话取出来的三个点是别的三角形的,
    // 叉积与作者法线的同向率会立刻掉下来。
    const FVector3 cameraPosition = renderer.GetCamera().GetPosition();

    {
        SizeType matching = 0;
        SizeType total    = 0;
        SizeType facing   = 0;

        for (SizeType v = 0; v < withCull.GpuVisible.GetSize(); ++v)
        {
            const UInt64 packed = withCull.GpuVisible[v];

            const UInt32 instanceIndex = static_cast<UInt32>(packed >> 32);
            const UInt32 meshletIndex  = static_cast<UInt32>(packed);

            if (instanceIndex >= instances.GetSize() ||
                meshletIndex >= meshletCount)
            {
                continue;
            }

            const FMeshletGpuView& meshlet = meshlets[meshletIndex];

            for (UInt32 t = 0; t < meshlet.TriangleCount; ++t)
            {
                FVector3 positions[3];
                FVector3 authored;

                if (!FetchWorldTriangle(instances[instanceIndex], meshlet, t,
                                        vertexBytes, meshletVertices,
                                        meshletTriangles, positions,
                                        &authored))
                {
                    continue;
                }

                ++total;

                const FVector3 ab(positions[1].X - positions[0].X,
                                  positions[1].Y - positions[0].Y,
                                  positions[1].Z - positions[0].Z);
                const FVector3 ac(positions[2].X - positions[0].X,
                                  positions[2].Y - positions[0].Y,
                                  positions[2].Z - positions[0].Z);

                const FVector3 normal(ab.Y * ac.Z - ab.Z * ac.Y,
                                      ab.Z * ac.X - ab.X * ac.Z,
                                      ab.X * ac.Y - ab.Y * ac.X);

                if (normal.X * authored.X + normal.Y * authored.Y +
                        normal.Z * authored.Z >
                    0.0f)
                {
                    ++matching;
                }

                if (TriangleFacesCamera(positions[0], positions[1],
                                        positions[2], cameraPosition))
                {
                    ++facing;
                }
            }
        }

        LIMX_LOG(LogLaunch, Display,
                 "[Meshlet背面] 叉积法线与作者法线同向 {}/{}; 留下来的里面"
                 "有 {} 个正面三角形",
                 matching, total, facing);

        if (total == 0 || matching != total)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[Meshlet背面] 叉积法线与作者法线不是处处同向 "
                     "({}/{}) —— 要么绕序约定反了, 要么取几何的下标算错了。"
                     "两种情况下这条判据都不可信",
                     matching, total);
            return false;
        }

        if (facing == 0)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[Meshlet背面] 留下来的 meshlet 里一个正面三角形都没有 "
                     "—— 判据没验到东西");
            return false;
        }
    }

    // ---- 被背面剔除剔掉的那一批 ----
    //
    // 两次采集用的是**同一个相机**, 只差背面剔除的开关。于是差集恰好是
    // "被背面剔除剔掉的" —— 视锥那一路在两次里完全一样。
    TArray<UInt64> culled;

    for (SizeType i = 0; i < withoutCull.GpuVisible.GetSize(); ++i)
    {
        const UInt64 packed = withoutCull.GpuVisible[i];

        bool stillVisible = false;

        for (SizeType j = 0; j < withCull.GpuVisible.GetSize(); ++j)
        {
            if (withCull.GpuVisible[j] == packed)
            {
                stillVisible = true;
                break;
            }
        }

        if (!stillVisible)
        {
            culled.Add(packed);
        }
    }

    LIMX_LOG(LogLaunch, Display,
             "[Meshlet背面] 背面剔除剔掉 {} 个 meshlet (开着 {} 个可见, "
             "关掉 {} 个)",
             culled.GetSize(), withCull.GpuVisible.GetSize(),
             withoutCull.GpuVisible.GetSize());

    if (culled.IsEmpty())
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Meshlet背面] 背面剔除一个都没剔掉 —— 这条判据没验到东西");
        return false;
    }

    // ---- 逐个三角形验 ----
    SizeType violatingMeshlets  = 0;
    SizeType violatingTriangles = 0;
    SizeType checkedTriangles   = 0;
    SizeType fetchFailures      = 0;

    UInt64 firstViolation = 0;

    for (SizeType i = 0; i < culled.GetSize(); ++i)
    {
        const UInt64 packed = culled[i];

        const UInt32 instanceIndex = static_cast<UInt32>(packed >> 32);
        const UInt32 meshletIndex  = static_cast<UInt32>(packed);

        if (instanceIndex >= instances.GetSize() ||
            meshletIndex >= meshletCount)
        {
            ++fetchFailures;
            continue;
        }

        const FMeshletGpuView& meshlet = meshlets[meshletIndex];

        SizeType facingHere = 0;

        for (UInt32 t = 0; t < meshlet.TriangleCount; ++t)
        {
            FVector3 positions[3];

            if (!FetchWorldTriangle(instances[instanceIndex], meshlet, t,
                                    vertexBytes, meshletVertices,
                                    meshletTriangles, positions))
            {
                ++fetchFailures;
                continue;
            }

            ++checkedTriangles;

            if (TriangleFacesCamera(positions[0], positions[1], positions[2],
                                    cameraPosition))
            {
                ++facingHere;
            }
        }

        if (facingHere != 0)
        {
            if (violatingMeshlets == 0)
            {
                firstViolation = packed;
            }

            ++violatingMeshlets;
            violatingTriangles += facingHere;
        }
    }

    bool passed = true;

    LIMX_LOG(LogLaunch, Display,
             "[Meshlet背面] 验了 {} 个三角形, 取不到的 {} 个",
             checkedTriangles, fetchFailures);

    if (fetchFailures != 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Meshlet背面] {} 个三角形取不到 —— 下标算错了, 判据本身"
                 "不可信",
                 fetchFailures);
        passed = false;
    }

    if (violatingMeshlets != 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Meshlet背面] {} 个被剔掉的 meshlet 里还有正对着相机的"
                 "三角形 (共 {} 个; 第一个在实例 {} 的 meshlet {}) —— "
                 "画面上那里画的是它背后的东西",
                 violatingMeshlets, violatingTriangles,
                 static_cast<UInt32>(firstViolation >> 32),
                 static_cast<UInt32>(firstViolation));
        passed = false;
    }

    return passed;
}

static bool RunMeshletCullChecks(FRenderContext* context, FRenderer& renderer)
{
    if (!renderer.SetMeshletCullEnabled(true))
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Meshlet剔除] 无法启用 — 判据无法执行, 判定为失败");
        return false;
    }

    bool passed = true;

    FCamera& camera = renderer.GetCamera();

    const Float32   originalFov      = camera.GetFovY();
    const FVector3  originalPosition = camera.GetPosition();

    const Float32 savedNear = camera.GetNearPlane();
    const Float32 savedFar  = camera.GetFarPlane();

    const auto RestoreCamera = [&camera, originalFov, originalPosition,
                                savedNear, savedFar]()
    {
        camera.SetPerspective(originalFov, camera.GetAspectRatio(), savedNear,
                              savedFar);
        camera.SetPosition(originalPosition);
    };


    // ================================================================
    // 四组配置
    //
    // 一组配置只能走到一部分分支, 而"GPU 与 CPU 一致"对没走到的分支毫无
    // 约束 —— 这是 Day 9 第一轮扫描里五条逃逸中四条的成因。所以判据自己
    // 造出那些条件:
    //
    //   窄视场 (15 度)   让视锥剔除真的剔掉东西。默认视角下综合场景
    //                    **什么都在视锥里**, 视锥判据整个写错都没痕迹。
    //   宽视场 (100 度)  让法线锥无效的物体 (立方体: 六个面朝六个方向)
    //                    与非均匀缩放的物体 (地面 40x1x40、柱子 0.5x3.5x0.5)
    //                    留在视野里, 走到那两条 early-out。
    //   相机贴地         把相机放进地面那个 meshlet 的包围球里, 走到
    //                    "相机在球内"那条 early-out。
    //   关背面剔除       单独验视锥那一路, 并给"背面剔除只能再剔掉东西"
    //                    那条单调性判据提供对照。
    // ================================================================

    struct FConfiguration
    {
        const AnsiChar* Label;
        Float32         FovDegrees;
        Float32         NearPlane;
        Float32         FarPlane;
        bool            BackfaceCull;
        bool            MoveInside;
    };

    const Float32 defaultNear = camera.GetNearPlane();
    const Float32 defaultFar  = camera.GetFarPlane();

    // 近平面推远、远平面拉近, 是为了让**近平面与远平面**这两条也单独
    // 剔掉过东西。默认的 0.1 / 100 之下, 综合场景里没有任何东西落在相机
    // 身后或者百米之外 —— 于是那两个平面上的判据整个删掉都不会红。
    const FConfiguration configurations[6] = {
        { "窄视场 + 背面剔除", 15.0f, defaultNear, defaultFar, true, false },
        { "窄视场 + 只视锥", 15.0f, defaultNear, defaultFar, false, false },
        { "宽视场 + 背面剔除", 100.0f, defaultNear, defaultFar, true, false },
        { "远平面拉到 8", 100.0f, defaultNear, 8.0f, true, false },
        { "近平面推到 12", 100.0f, 12.0f, defaultFar, true, false },
        { "相机在包围球内", 60.0f, defaultNear, defaultFar, true, true },
    };

    constexpr UInt32 kConfigurationCount = 6;

    TArray<FCullCapture> captures;

    // "相机在包围球内"那一组要的位置, 由前几组的采集算出来 —— 场景里
    // 哪个 meshlet 的包围球最大不是能写死的知识。
    FVector3 insideTarget = originalPosition;

    for (UInt32 c = 0; c < kConfigurationCount; ++c)
    {
        const FConfiguration& configuration = configurations[c];

        camera.SetPerspective(
            FMath::DegreesToRadians(configuration.FovDegrees),
            camera.GetAspectRatio(), configuration.NearPlane,
            configuration.FarPlane);

        camera.SetPosition(configuration.MoveInside ? insideTarget
                                                    : originalPosition);

        captures.Add(CaptureMeshletCull(context, renderer,
                                        configuration.BackfaceCull));

        if (!captures.Last().Valid)
        {
            LIMX_LOG(LogLaunch, Error, "[Meshlet剔除] {} — 采集失败",
                     configuration.Label);
            RestoreCamera();
            return false;
        }

        const FCullCapture& capture = captures.Last();

        if (capture.InsideCameraRadius > 0.0f && !configuration.MoveInside)
        {
            insideTarget = capture.InsideCameraTarget;
        }

        LIMX_LOG(LogLaunch, Display,
                 "[Meshlet剔除] {} — 实例 {} 可见, 测试 {} 个 meshlet, "
                 "可见 {} (视锥剔 {}, 背面剔 {}); CPU 参考 {} 个",
                 configuration.Label, capture.InstancesVisible,
                 capture.Counters[3], capture.Counters[0], capture.Counters[1],
                 capture.Counters[2], capture.CpuVisible.GetSize());

        // ---- 判据 1: 可见集合完全相同 ----
        if (capture.GpuVisible.GetSize() != capture.CpuVisible.GetSize())
        {
            LIMX_LOG(LogLaunch, Error,
                     "[Meshlet剔除] {} — GPU 留下 {} 个, CPU 参考 {} 个",
                     configuration.Label, capture.GpuVisible.GetSize(),
                     capture.CpuVisible.GetSize());
            passed = false;
        }
        else
        {
            SizeType mismatched = 0;
            UInt64   firstGpu   = 0;
            UInt64   firstCpu   = 0;

            for (SizeType i = 0; i < capture.GpuVisible.GetSize(); ++i)
            {
                if (capture.GpuVisible[i] != capture.CpuVisible[i])
                {
                    if (mismatched == 0)
                    {
                        firstGpu = capture.GpuVisible[i];
                        firstCpu = capture.CpuVisible[i];
                    }

                    ++mismatched;
                }
            }

            if (mismatched != 0)
            {
                LIMX_LOG(LogLaunch, Error,
                         "[Meshlet剔除] {} — {} 条不同, 第一条 GPU (实例 {}, "
                         "meshlet {}) vs CPU (实例 {}, meshlet {})",
                         configuration.Label, mismatched,
                         static_cast<UInt32>(firstGpu >> 32),
                         static_cast<UInt32>(firstGpu & 0xFFFFFFFFull),
                         static_cast<UInt32>(firstCpu >> 32),
                         static_cast<UInt32>(firstCpu & 0xFFFFFFFFull));
                passed = false;
            }
        }

        // ---- 判据 2: 计数器自洽 ----
        const UInt32 sum =
            capture.Counters[0] + capture.Counters[1] + capture.Counters[2];

        if (sum != capture.Counters[3])
        {
            LIMX_LOG(LogLaunch, Error,
                     "[Meshlet剔除] {} — 计数器不自洽: 可见 {} + 视锥剔 {} + "
                     "背面剔 {} = {}, 但测试数是 {}",
                     configuration.Label, capture.Counters[0],
                     capture.Counters[1], capture.Counters[2], sum,
                     capture.Counters[3]);
            passed = false;
        }

        // ---- 判据 3: 第一级必须保守 ----
        //
        // 实例包围球剔掉的东西, 逐 meshlet 的视锥判据也必须剔掉。不然
        // 第一级就剔掉了本来看得见的 meshlet —— 而 GPU 那边被剔的实例
        // 压根不进第二级, 集合比对对它一个字都不会说。
        if (capture.InstanceCullTooAggressive != 0)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[Meshlet剔除] {} — 第一级剔掉的实例里有 {} 个 meshlet "
                     "在逐 meshlet 判据下仍然可见 —— 实例包围球不保守, "
                     "画面上会整块消失",
                     configuration.Label, capture.InstanceCullTooAggressive);
            passed = false;
        }
    }

    RestoreCamera();

    // ================================================================
    // 判据 3.5: 背面剔除的地面真值
    //
    // 前面那条"GPU 与 CPU 参考实现逐条相同"只验转写。这一条不走公式, 直接
    // 按定义验: 被剔掉的 meshlet 里不许有正对着相机的三角形。
    //
    // 相机用 captures[0] / captures[1] 那一组的位置 (两组只差背面剔除的
    // 开关, 视锥完全一样), 于是两者的差集恰好是"被背面剔除剔掉的"。
    // ================================================================
    if (!RunBackfaceGroundTruthCheck(context, renderer, captures[0],
                                     captures[1]))
    {
        passed = false;
    }

    // ================================================================
    // 判据 4: 背面剔除的单调性
    //
    // 同一组视锥下, 开着背面剔除时可见的 meshlet, 关掉之后必须仍然可见。
    // 反过来说明两条判据互相干扰。
    // ================================================================
    {
        const FCullCapture& withBackface = captures[0];
        const FCullCapture& onlyFrustum  = captures[1];

        SizeType missing = 0;
        SizeType j       = 0;

        for (SizeType i = 0; i < withBackface.GpuVisible.GetSize(); ++i)
        {
            const UInt64 key = withBackface.GpuVisible[i];

            while (j < onlyFrustum.GpuVisible.GetSize() &&
                   onlyFrustum.GpuVisible[j] < key)
            {
                ++j;
            }

            if (j >= onlyFrustum.GpuVisible.GetSize() ||
                onlyFrustum.GpuVisible[j] != key)
            {
                ++missing;
            }
        }

        if (missing != 0)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[Meshlet剔除] 开着背面剔除时可见的 {} 个 meshlet, "
                     "关掉之后反而不可见了 —— 背面剔除不该让东西活过来",
                     missing);
            passed = false;
        }
    }

    // ================================================================
    // 判据 5: 分支覆盖
    //
    // 这一组元判据是 Day 9 第一轮扫描的直接产物。当时五条逃逸里有四条
    // 同一个成因: **那个分支在这个视角下根本没被走到**。一致性判据对
    // 没走到的分支毫无约束, 于是把它改坏了也一样绿。
    //
    // 所以每个分支都要有一个计数器, 每个计数器都要有一条"必须大于零"的
    // 元判据。哪条不满足, 它自己会说是哪条。
    // ================================================================
    {
        UInt32 planeExclusive[6] = { 0, 0, 0, 0, 0, 0 };

        UInt32 invalidCone     = 0;
        UInt32 nonUniformScale = 0;

        for (SizeType c = 0; c < captures.GetSize(); ++c)
        {
            for (UInt32 p = 0; p < 6; ++p)
            {
                planeExclusive[p] += captures[c].PlaneExclusiveCulls[p];
            }

            invalidCone += captures[c].InvalidConeEarlyOut;
            nonUniformScale += captures[c].NonUniformScaleEarlyOut;
        }

        LIMX_LOG(LogLaunch, Display,
                 "[Meshlet剔除] 分支覆盖 — 六个平面独自剔掉 "
                 "{}/{}/{}/{}/{}/{}; 无效法线锥 {}, 非均匀缩放 {}",
                 planeExclusive[0], planeExclusive[1], planeExclusive[2],
                 planeExclusive[3], planeExclusive[4], planeExclusive[5],
                 invalidCone, nonUniformScale);

        for (UInt32 p = 0; p < 6; ++p)
        {
            if (planeExclusive[p] == 0)
            {
                LIMX_LOG(LogLaunch, Error,
                         "[Meshlet剔除] 第 {} 个视锥平面从来没有独自剔掉过"
                         "任何 meshlet —— 这个平面上的判据是空的, "
                         "把它整个删掉都不会红",
                         p);
                passed = false;
            }
        }

        if (invalidCone == 0)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[Meshlet剔除] 没有一个法线锥无效的 meshlet 走到背面"
                     "判据 —— 那条 early-out 是空的");
            passed = false;
        }

        if (nonUniformScale == 0)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[Meshlet剔除] 没有一个非均匀缩放的实例走到背面判据 "
                     "—— 那条 early-out 是空的 (场景里需要一个缩放不一致"
                     "**且法线锥有效**的物体)");
            passed = false;
        }

    }

    // ================================================================
    // 判据 6: 剔除必须真的剔掉了东西
    // ================================================================
    if (captures[1].Counters[1] == 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Meshlet剔除] 视锥一个 meshlet 都没剔掉 —— "
                 "这个视角下判据是空的");
        passed = false;
    }

    if (captures[0].Counters[2] == 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Meshlet剔除] 背面一个 meshlet 都没剔掉 —— 判据是空的");
        passed = false;
    }

    if (captures[0].InstancesVisible >=
        static_cast<UInt32>(
            renderer.GetMeshletCullPass()->GetInstances().GetSize()))
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Meshlet剔除] 第一级一个实例都没剔掉 ({} / {}) —— "
                 "两级剔除只有一级在干活",
                 captures[0].InstancesVisible,
                 renderer.GetMeshletCullPass()->GetInstances().GetSize());
        passed = false;
    }

    if (!renderer.SetMeshletCullEnabled(false))
    {
        LIMX_LOG(LogLaunch, Error, "[Meshlet剔除] 无法关闭 (复位)");
        passed = false;
    }

    LIMX_LOG(LogLaunch, Display, "[Meshlet剔除] {}", passed ? "通过" : "失败");

    return passed;
}

// ============================================================================
// RunMeshSimplifyChecks — 简化器的判据
//
// 这一条判据里最要紧的是**误差是上界**: 把每一个原始顶点到简化后表面的
// 距离量出来取最大, 必须不超过简化器报出来的那个误差。
//
// 为什么是它最要紧: 下游的 LOD 选择靠"把这个误差投到屏幕上与阈值比"来决定
// 画哪一层。误差报小了, 选择会在该换层的时候不换 —— 表现是"模型突然变糙";
// 更糟的是相邻两块因此选了不同层而边界对不上, 出现裂缝。而误差是不是上界,
// 这件事完全不必看画面: 它是一句能证伪的话。
//
// 其余五条:
//
//   单调        记录的误差是到目前为止的最大值, 不是这一次坍缩的代价。
//              二次误差在邻居坍缩之后会**变小**, 直接记它会让某一层的误差
//              小于上一层, 而 LOD 选择规则在那种数据上会同时选中父与子。
//   不退化      输出里没有两个下标相同的三角形, 也没有零面积的。
//   不翻转      闭合网格的有符号体积必须与原来同号且接近。单个三角形翻了
//              在画面上是一块黑, 而体积把它变成一个可比的数。
//   流形保持    输入每条边恰好两个三角形时, 输出也必须如此。
//   确定性      同样的输入跑两遍, 输出逐位相同。第三天要拿简化器建 DAG,
//              而不确定的简化器建出来的 DAG 每次都不一样。
// ============================================================================

namespace
{

/// 点到三角形的最短距离平方
Float32 PointTriangleDistanceSquared(const FVector3& p, const FVector3& a,
                                     const FVector3& b, const FVector3& c)
{
    // Ericson, Real-Time Collision Detection 的分区做法: 先看三个顶点区,
    // 再看三条边区, 剩下的落在面内。
    const FVector3 ab = b - a;
    const FVector3 ac = c - a;
    const FVector3 ap = p - a;

    const Float32 d1 = FVector3::Dot(ab, ap);
    const Float32 d2 = FVector3::Dot(ac, ap);

    if (d1 <= 0.0f && d2 <= 0.0f)
    {
        return (p - a).LengthSquared();
    }

    const FVector3 bp = p - b;

    const Float32 d3 = FVector3::Dot(ab, bp);
    const Float32 d4 = FVector3::Dot(ac, bp);

    if (d3 >= 0.0f && d4 <= d3)
    {
        return (p - b).LengthSquared();
    }

    const Float32 vc = d1 * d4 - d3 * d2;

    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
    {
        const Float32 denominator = d1 - d3;
        const Float32 v = (denominator != 0.0f) ? (d1 / denominator) : 0.0f;

        return (p - (a + ab * v)).LengthSquared();
    }

    const FVector3 cp = p - c;

    const Float32 d5 = FVector3::Dot(ab, cp);
    const Float32 d6 = FVector3::Dot(ac, cp);

    if (d6 >= 0.0f && d5 <= d6)
    {
        return (p - c).LengthSquared();
    }

    const Float32 vb = d5 * d2 - d1 * d6;

    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
    {
        const Float32 denominator = d2 - d6;
        const Float32 w = (denominator != 0.0f) ? (d2 / denominator) : 0.0f;

        return (p - (a + ac * w)).LengthSquared();
    }

    const Float32 va = d3 * d6 - d5 * d4;

    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
    {
        const Float32 denominator = (d4 - d3) + (d5 - d6);
        const Float32 w = (denominator != 0.0f) ? ((d4 - d3) / denominator)
                                                : 0.0f;

        return (p - (b + (c - b) * w)).LengthSquared();
    }

    const Float32 denominator = va + vb + vc;

    if (denominator == 0.0f)
    {
        return (p - a).LengthSquared();
    }

    const Float32 v = vb / denominator;
    const Float32 w = vc / denominator;

    return (p - (a + ab * v + ac * w)).LengthSquared();
}

/// 闭合网格的有符号体积 (散度定理, 对原点取)
Float64 SignedVolume(const TArray<FMeshVertex>& vertices,
                     const TArray<UInt32>& indices)
{
    Float64 total = 0.0;

    for (SizeType t = 0; t + 2 < indices.GetSize(); t += 3)
    {
        const FVector3& a = vertices[indices[t + 0]].Position;
        const FVector3& b = vertices[indices[t + 1]].Position;
        const FVector3& c = vertices[indices[t + 2]].Position;

        total += static_cast<Float64>(
            FVector3::Dot(a, FVector3::Cross(b, c))) / 6.0;
    }

    return total;
}

struct FEdgeManifoldStats
{
    /// 被三个及以上三角形共用的边 —— 任何网格上都是缺陷
    SizeType Excess = 0;

    /// 只被一个三角形用到的边 —— 开边界。闭合网格上为 0
    SizeType Boundary = 0;
};

FEdgeManifoldStats NonManifoldEdgeCountWelded(const TArray<UInt32>& weld,
                                              const TArray<UInt32>& indices,
                                              SizeType vertexCount);

/// 每条无向边被几个三角形用到 —— 闭合流形上处处为 2
///
/// 必须按**位置**判, 而且要带容差。
///
/// 第一版直接拿索引数组算, 于是 UV 球报 252 条"非流形边"、平面报 96 条 ——
/// 而判据的条件是 `badBefore == 0 && badAfter != 0`, 前半永远为假, 这条判据
/// **在三个测试网格上一次都没执行过**。
///
/// 按位置判还不够: UV 球的接缝上 sinf(2πf) = 1.748e-07 而不是 0, 南极那一圈
/// 的 sinf(πf) = -8.74e-08 —— 逐位焊接焊不上, 球在数据里根本不是闭合的。
/// 所以要给一个相对容差。
FEdgeManifoldStats NonManifoldEdgeCount(const TArray<FMeshVertex>& vertices,
                                        const TArray<UInt32>& indices)
{
    // 按位置焊接 (容差取包围盒对角线的百万分之一)
    FVector3 low(3.4e38f, 3.4e38f, 3.4e38f);
    FVector3 high(-3.4e38f, -3.4e38f, -3.4e38f);

    for (SizeType i = 0; i < vertices.GetSize(); ++i)
    {
        const FVector3& p = vertices[i].Position;

        low  = FVector3(FMath::Min(low.X, p.X), FMath::Min(low.Y, p.Y),
                        FMath::Min(low.Z, p.Z));
        high = FVector3(FMath::Max(high.X, p.X), FMath::Max(high.Y, p.Y),
                        FMath::Max(high.Z, p.Z));
    }

    const Float32 tolerance = (high - low).Length() * 1.0e-6f;
    const Float32 toleranceSquared = tolerance * tolerance;

    TArray<UInt32>   weld;
    TArray<FVector3> unique;

    for (SizeType i = 0; i < vertices.GetSize(); ++i)
    {
        const FVector3& p = vertices[i].Position;

        UInt32 found = 0xFFFFFFFFu;

        for (SizeType k = 0; k < unique.GetSize(); ++k)
        {
            if ((unique[k] - p).LengthSquared() <= toleranceSquared)
            {
                found = static_cast<UInt32>(k);
                break;
            }
        }

        if (found == 0xFFFFFFFFu)
        {
            found = static_cast<UInt32>(unique.GetSize());
            unique.Add(p);
        }

        weld.Add(found);
    }

    const SizeType vertexCount = unique.GetSize();

    return NonManifoldEdgeCountWelded(weld, indices, vertexCount);
}

FEdgeManifoldStats NonManifoldEdgeCountWelded(
    const TArray<UInt32>& weld, const TArray<UInt32>& indices,
    SizeType vertexCount)
{
    TArray<TArray<UInt32>> neighbours;
    neighbours.SetSize(vertexCount);

    TArray<TArray<UInt32>> counts;
    counts.SetSize(vertexCount);

    for (SizeType t = 0; t + 2 < indices.GetSize(); t += 3)
    {
        for (UInt32 e = 0; e < 3; ++e)
        {
            const UInt32 from = weld[indices[t + e]];
            const UInt32 to   = weld[indices[t + (e + 1) % 3]];

            const UInt32 low  = FMath::Min(from, to);
            const UInt32 high = FMath::Max(from, to);

            bool found = false;

            for (SizeType k = 0; k < neighbours[low].GetSize(); ++k)
            {
                if (neighbours[low][k] == high)
                {
                    ++counts[low][k];
                    found = true;
                    break;
                }
            }

            if (!found)
            {
                neighbours[low].Add(high);
                counts[low].Add(1u);
            }
        }
    }

    FEdgeManifoldStats stats;

    for (SizeType v = 0; v < neighbours.GetSize(); ++v)
    {
        for (SizeType k = 0; k < counts[v].GetSize(); ++k)
        {
            if (counts[v][k] >= 3u)
            {
                ++stats.Excess;
            }
            else if (counts[v][k] == 1u)
            {
                ++stats.Boundary;
            }
        }
    }

    return stats;
}

/// 一个网格上跑完整套判据
bool CheckOneSimplify(const AnsiChar* label, const FMeshData& mesh,
                      Float32 ratio)
{
    const SizeType inputTriangles = mesh.Indices.GetSize() / 3;

    FMeshSimplifyOptions options;
    options.TargetTriangleCount = static_cast<UInt32>(
        static_cast<Float32>(inputTriangles) * ratio);
    options.LockOpenBoundary = true;

    const FMeshSimplifyResult result =
        FMeshSimplifier::Simplify(mesh.Vertices, mesh.Indices, options);

    const SizeType outputTriangles = result.Indices.GetSize() / 3;

    bool passed = true;

    Float32 boundsDiagonal = 0.0f;

    {
        FVector3 low(3.4e38f, 3.4e38f, 3.4e38f);
        FVector3 high(-3.4e38f, -3.4e38f, -3.4e38f);

        for (SizeType v = 0; v < mesh.Vertices.GetSize(); ++v)
        {
            const FVector3& p = mesh.Vertices[v].Position;

            low  = FVector3(FMath::Min(low.X, p.X), FMath::Min(low.Y, p.Y),
                            FMath::Min(low.Z, p.Z));
            high = FVector3(FMath::Max(high.X, p.X), FMath::Max(high.Y, p.Y),
                            FMath::Max(high.Z, p.Z));
        }

        boundsDiagonal = (high - low).Length();
    }

    LIMX_LOG(LogLaunch, Display,
             "[简化] {} — {} -> {} 三角形 (目标 {}), 坍缩 {} 次, 误差 {}",
             label, inputTriangles, outputTriangles,
             options.TargetTriangleCount, result.CollapseCount, result.Error);

    if (outputTriangles == 0 || result.Vertices.IsEmpty())
    {
        LIMX_LOG(LogLaunch, Error, "[简化] {} — 输出是空的", label);
        return false;
    }

    // ---- 判据一: 真的简化了 ----
    //
    // 没有这一条的话, 一个"原样返回"的实现在下面每一条上都满分通过 ——
    // 误差 0 是任何偏差的上界, 体积分毫不差, 流形当然保持。
    if (outputTriangles >= inputTriangles)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[简化] {} — 一个三角形都没少 ({} -> {}) —— 简化器什么都"
                 "没做, 而下面每一条判据对'什么都不做'都是满分",
                 label, inputTriangles, outputTriangles);
        passed = false;
    }

    // ---- 判据二: 误差是真实偏差的上界 ----
    Float32 worstDistance = 0.0f;

    for (SizeType v = 0; v < mesh.Vertices.GetSize(); ++v)
    {
        const FVector3& p = mesh.Vertices[v].Position;

        Float32 nearest = 3.4e38f;

        for (SizeType t = 0; t + 2 < result.Indices.GetSize(); t += 3)
        {
            const Float32 distance = PointTriangleDistanceSquared(
                p, result.Vertices[result.Indices[t + 0]].Position,
                result.Vertices[result.Indices[t + 1]].Position,
                result.Vertices[result.Indices[t + 2]].Position);

            nearest = FMath::Min(nearest, distance);

            if (nearest <= 0.0f)
            {
                break;
            }
        }

        worstDistance = FMath::Max(worstDistance, FMath::Sqrt(nearest));
    }

    // 容差按输入包围盒的对角线取十万分之一 —— 纯粹是浮点噪声的量级,
    // 不是给实现留的余地。
    const Float32 epsilon = boundsDiagonal * 1.0e-5f;

    LIMX_LOG(LogLaunch, Display,
             "[简化] {} — 原始顶点到简化面的最大距离 {} (报出来的误差 {}, "
             "松了 {} 倍)",
             label, worstDistance, result.Error,
             (worstDistance > 0.0f) ? (result.Error / worstDistance) : 0.0f);

    if (worstDistance > result.Error + epsilon)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[简化] {} — 实际偏差 {} 超过了报出来的误差 {} —— 误差不是"
                 "上界, 而 LOD 选择整套东西都建立在它是上界之上",
                 label, worstDistance, result.Error);
        passed = false;
    }

    // ---- 包围盒 (下面几条判据的尺度基准) ----
    FVector3 minimum(3.4e38f, 3.4e38f, 3.4e38f);
    FVector3 maximum(-3.4e38f, -3.4e38f, -3.4e38f);

    for (SizeType v = 0; v < mesh.Vertices.GetSize(); ++v)
    {
        const FVector3& p = mesh.Vertices[v].Position;

        minimum = FVector3(FMath::Min(minimum.X, p.X),
                           FMath::Min(minimum.Y, p.Y),
                           FMath::Min(minimum.Z, p.Z));
        maximum = FVector3(FMath::Max(maximum.X, p.X),
                           FMath::Max(maximum.Y, p.Y),
                           FMath::Max(maximum.Z, p.Z));
    }

    const Float32 diagonal = (maximum - minimum).Length();

    const Float32 degenerateAreaThreshold = diagonal * diagonal * 1.0e-12f;

    // ---- 判据三: 不退化 ----
    SizeType degenerate = 0;

    for (SizeType t = 0; t + 2 < result.Indices.GetSize(); t += 3)
    {
        const UInt32 a = result.Indices[t + 0];
        const UInt32 b = result.Indices[t + 1];
        const UInt32 c = result.Indices[t + 2];

        if (a == b || b == c || a == c)
        {
            ++degenerate;
            continue;
        }

        const FVector3 areaNormal =
            FVector3::Cross(result.Vertices[b].Position -
                                result.Vertices[a].Position,
                            result.Vertices[c].Position -
                                result.Vertices[a].Position);

        // "恰好为零"是抓不住的: 坍缩留下的退化三角形面积是 1e-14 这个量级,
        // 不是 0。阈值按包围盒对角线的平方取, 与网格尺度无关。
        if (areaNormal.LengthSquared() <= degenerateAreaThreshold)
        {
            ++degenerate;
        }
    }

    if (degenerate != 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[简化] {} — {} 个退化三角形 (下标重复或零面积)", label,
                 degenerate);
        passed = false;
    }

    // ---- 判据四: 有符号体积同号且接近 ----
    const Float64 volumeBefore = SignedVolume(mesh.Vertices, mesh.Indices);
    const Float64 volumeAfter = SignedVolume(result.Vertices, result.Indices);

    LIMX_LOG(LogLaunch, Display, "[简化] {} — 有符号体积 {} -> {}", label,
             volumeBefore, volumeAfter);

    if (volumeBefore > 0.0)
    {
        // 三角形翻转在单个三角形上看不出来, 但闭合网格的有符号体积会少
        // 掉那一块 —— 翻一个就是双倍的负贡献。
        const Float64 relative =
            (volumeAfter - volumeBefore) / volumeBefore;

        if (volumeAfter <= 0.0 || relative < -0.25 || relative > 0.25)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[简化] {} — 有符号体积从 {} 变成 {} (相对 {}) —— "
                     "有三角形翻了面",
                     label, volumeBefore, volumeAfter, relative);
            passed = false;
        }
    }

    // ---- 判据四之二: 没有三角形翻面 ----
    //
    // 有符号体积抓不住它: 一个三角形翻过去只让体积差它自己那一份, 一千多个
    // 三角形里的一个是千分之几, 淹没在简化本身带来的体积变化里。变异验证
    // 当场证实了这一点 —— 把翻转检查整个删掉, 体积判据纹丝不动。
    //
    // 直接问: 每个输出三角形, 找到离它质心最近的那个**原始**三角形, 两者的
    // 法线不许反向。简化会让法线偏一些, 但不该偏过 90 度 —— 偏过去就是这块
    // 面翻过来了。
    {
        SizeType flipped = 0;

        for (SizeType t = 0; t + 2 < result.Indices.GetSize(); t += 3)
        {
            const FVector3& a = result.Vertices[result.Indices[t + 0]].Position;
            const FVector3& b = result.Vertices[result.Indices[t + 1]].Position;
            const FVector3& c = result.Vertices[result.Indices[t + 2]].Position;

            const FVector3 normal = FVector3::Cross(b - a, c - a);

            if (normal.LengthSquared() <= degenerateAreaThreshold)
            {
                continue;
            }

            const FVector3 centroid = (a + b + c) / 3.0f;

            Float32  nearest       = 3.4e38f;
            FVector3 nearestNormal = FVector3(0.0f, 0.0f, 0.0f);

            for (SizeType k = 0; k + 2 < mesh.Indices.GetSize(); k += 3)
            {
                const FVector3& p = mesh.Vertices[mesh.Indices[k + 0]].Position;
                const FVector3& q = mesh.Vertices[mesh.Indices[k + 1]].Position;
                const FVector3& r = mesh.Vertices[mesh.Indices[k + 2]].Position;

                const Float32 distance =
                    PointTriangleDistanceSquared(centroid, p, q, r);

                if (distance < nearest)
                {
                    nearest = distance;

                    // 用**作者法线**的均值当参考, 不用叉积。
                    //
                    // UV 球两极那一圈三角形是退化的细条, 叉积算出来的法线
                    // 数值上很不稳; 而作者法线在那里仍然是精确的球面外法线。
                    // 第六周期量过: 叉积与作者法线同向 6450/6450, 两者只在
                    // 退化处才分家。
                    nearestNormal =
                        mesh.Vertices[mesh.Indices[k + 0]].Normal +
                        mesh.Vertices[mesh.Indices[k + 1]].Normal +
                        mesh.Vertices[mesh.Indices[k + 2]].Normal;
                }
            }

            if (nearestNormal.LengthSquared() > 0.0f &&
                FVector3::Dot(normal.GetSafeNormal(),
                              nearestNormal.GetSafeNormal()) < 0.0f)
            {
                ++flipped;
            }
        }

        if (flipped != 0)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[简化] {} — {} 个三角形与它脚下那片原始表面法线反向 "
                     "—— 这块面翻过来了, 画出来是一块黑",
                     label, flipped);
            passed = false;
        }
    }

    // ---- 判据四之三: 简化不许**增加**细条三角形 ----
    //
    // 二次误差天生爱造细条: 沿着一条长边坍缩误差很小, 留下的三角形又长又薄。
    // 细条在画面上是走样的边, 在数值上是不可靠的法线。
    //
    // 判据不能写成"不许有细条" —— UV 球两极那一圈本来就是细条, 那是输入
    // 自带的。写成"不许**变多**": 简化是把三角形减掉, 细条只会跟着少。
    {
        const auto Aspect = [](const FVector3& p0, const FVector3& p1,
                               const FVector3& p2) -> Float32
        {
            const Float32 sum = (p1 - p0).LengthSquared() +
                                (p2 - p1).LengthSquared() +
                                (p0 - p2).LengthSquared();

            if (sum <= 0.0f)
            {
                return 0.0f;
            }

            const Float32 area =
                FVector3::Cross(p1 - p0, p2 - p0).Length() * 0.5f;

            return 6.9282032f * area / sum;
        };

        const auto CountSlivers = [&Aspect](const TArray<FMeshVertex>& verts,
                                            const TArray<UInt32>& idx) -> SizeType
        {
            SizeType count = 0;

            for (SizeType t = 0; t + 2 < idx.GetSize(); t += 3)
            {
                if (Aspect(verts[idx[t + 0]].Position,
                           verts[idx[t + 1]].Position,
                           verts[idx[t + 2]].Position) < 0.01f)
                {
                    ++count;
                }
            }

            return count;
        };

        const SizeType sliversBefore = CountSlivers(mesh.Vertices, mesh.Indices);
        const SizeType sliversAfter =
            CountSlivers(result.Vertices, result.Indices);

        LIMX_LOG(LogLaunch, Display, "[简化] {} — 细条三角形 {} -> {}", label,
                 sliversBefore, sliversAfter);

        if (sliversAfter > sliversBefore)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[简化] {} — 细条三角形从 {} 变成 {} —— 简化把三角形"
                     "减少了一半, 细条却变多了, 那是坍缩自己造出来的",
                     label, sliversBefore, sliversAfter);
            passed = false;
        }
    }

    // ---- 判据五: 流形保持 ----
    // ---- 判据五: 拓扑 ----
    //
    // 第一版写的是"每条边恰好两个三角形", 前置条件是"输入闭合"。两个问题:
    //
    //   一、它按**索引**算, 于是 UV 球报 252 条、平面报 96 条,
    //       `badBefore == 0` 永远为假 —— 这条判据在三个网格上**一次都没
    //       执行过**。
    //   二、就算按位置算对了, "闭合"这个前置条件把平面整个排除在外了。而
    //       第二天起每个 meshlet 组都是**开**网格 —— 那正是这条判据最该管的
    //       地方。
    //
    // 改成两条对开闭都成立的:
    //
    //   * 没有边被三个及以上三角形共用 —— 任何网格上都是缺陷, 无前置条件
    //   * 开边界的边数不许变 —— 边界顶点被锁死, 所以边界边应当原样保留。
    //     它变了就说明锁定漏了, 而那在第二天就是裂缝。
    const FEdgeManifoldStats before =
        NonManifoldEdgeCount(mesh.Vertices, mesh.Indices);

    const FEdgeManifoldStats after =
        NonManifoldEdgeCount(result.Vertices, result.Indices);

    LIMX_LOG(LogLaunch, Display,
             "[简化] {} — 三角形共用超过两次的边 {} -> {}, 开边界边 {} -> {}",
             label, before.Excess, after.Excess, before.Boundary,
             after.Boundary);

    if (after.Excess > before.Excess)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[简化] {} — 被三个及以上三角形共用的边从 {} 变成 {} —— "
                 "坍缩把表面折叠到自己身上了",
                 label, before.Excess, after.Excess);
        passed = false;
    }

    if (after.Boundary != before.Boundary)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[简化] {} — 开边界的边数从 {} 变成 {} —— 边界顶点是锁死的, "
                 "边界边应当原样保留; 变了就是锁定漏了",
                 label, before.Boundary, after.Boundary);
        passed = false;
    }

    // ---- 判据六: 确定性 ----
    const FMeshSimplifyResult again =
        FMeshSimplifier::Simplify(mesh.Vertices, mesh.Indices, options);

    bool identical = (again.Indices.GetSize() == result.Indices.GetSize()) &&
                     (again.Vertices.GetSize() == result.Vertices.GetSize()) &&
                     (again.Error == result.Error);

    for (SizeType i = 0; identical && i < result.Indices.GetSize(); ++i)
    {
        identical = (again.Indices[i] == result.Indices[i]);
    }

    for (SizeType i = 0; identical && i < result.Vertices.GetSize(); ++i)
    {
        const FVector3& p = result.Vertices[i].Position;
        const FVector3& q = again.Vertices[i].Position;

        identical = (p.X == q.X && p.Y == q.Y && p.Z == q.Z);
    }

    if (!identical)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[简化] {} — 同样的输入跑两遍结果不同 —— 第三天要拿它建"
                 "DAG, 而不确定的简化器建出来的 DAG 每次都不一样",
                 label);
        passed = false;
    }

    return passed;
}

} // namespace

static bool RunMeshSimplifyChecks()
{
    bool passed = true;

    // 球体: 闭合流形, 没有开边界, 曲率处处不为零 —— 简化器该最好使
    {
        const FMeshData sphere = FGeometryGenerator::GenerateSphere(1.0f, 48, 32);

        passed &= CheckOneSimplify("球体 (48x32)", sphere, 0.5f);
        passed &= CheckOneSimplify("球体 (48x32) 到一成", sphere, 0.1f);
    }

    // 细分平面: 没有极点、三角形大小均匀 —— 拿它看误差的松紧是不是
    // UV 球两极那种退化造成的
    {
        const FMeshData grid =
            FGeometryGenerator::GeneratePlane(2.0f, 2.0f, 24, 24);

        passed &= CheckOneSimplify("细分平面 (24x24)", grid, 0.5f);
    }

    // 立方体: 焊接之前 24 个顶点 8 个位置, 不焊的话一次坍缩都做不了
    {
        const FMeshData cube = FGeometryGenerator::GenerateCube();

        const FMeshSimplifyResult result = FMeshSimplifier::Simplify(
            cube.Vertices, cube.Indices, FMeshSimplifyOptions{});

        LIMX_LOG(LogLaunch, Display,
                 "[简化] 立方体 — 顶点 {} 焊接后 {}", cube.Vertices.GetSize(),
                 result.WeldedVertexCount);

        // 立方体的每个位置在数据里有三份 (三个面各带一份法线)。焊不到 8
        // 个的话每条边都是开边界, 简化器一次坍缩都做不了 —— 而那时上面
        // 每一条判据仍然满分。
        if (result.WeldedVertexCount != 8)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[简化] 立方体焊接后应当是 8 个位置, 实际 {} 个 —— "
                     "不焊的话每条边都是开边界, 简化器一次坍缩都做不了",
                     result.WeldedVertexCount);
            passed = false;
        }
    }

    // 只给误差上限 (不给目标数) —— 这个模式头文件写了, 但它曾经完全失效
    {
        const FMeshData sphere = FGeometryGenerator::GenerateSphere(1.0f, 48, 32);

        const SizeType inputTriangles = sphere.Indices.GetSize() / 3;

        FMeshSimplifyOptions options;
        options.TargetTriangleCount = 0;
        options.MaxError            = 0.02f;

        const FMeshSimplifyResult result =
            FMeshSimplifier::Simplify(sphere.Vertices, sphere.Indices, options);

        const SizeType outputTriangles = result.Indices.GetSize() / 3;

        LIMX_LOG(LogLaunch, Display,
                 "[简化] 只给误差上限 {} — {} -> {} 三角形, 报出来的误差 {}",
                 options.MaxError, inputTriangles, outputTriangles,
                 result.Error);

        // 第一版在没给目标数时把 target 取成**当前**的存活三角形数, 循环条件
        // 当场为假 —— 一次坍缩都不做, 而其余四条判据全部满分。
        if (outputTriangles >= inputTriangles)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[简化] 只给误差上限时一个三角形都没少 ({} -> {}) —— "
                     "这个模式整个失效了, 而第三天要按误差预算建 DAG",
                     inputTriangles, outputTriangles);
            passed = false;
        }

        // 报出来的误差不许超预算太多。
        //
        // 不写"不许超过预算": 坍缩的决定是按**局部**表面量的, 而收尾那一遍
        // 对全局重量, 别的簇后来变粗了会把这个数抬上去。两倍是给那一层抬升
        // 留的, 不是给"拦不住第一次越界"留的 —— 后者已经修掉了。
        if (result.Error > options.MaxError * 2.0f)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[简化] 误差预算 {} 而实际报出 {} —— 超过两倍说明上限"
                     "根本没在拦",
                     options.MaxError, result.Error);
            passed = false;
        }
    }

    // 平面: 有开边界, 锁住之后四条边不许动
    {
        const FMeshData plane = FGeometryGenerator::GeneratePlane(2.0f, 2.0f, 8, 8);

        const SizeType inputTriangles = plane.Indices.GetSize() / 3;

        FMeshSimplifyOptions options;
        options.TargetTriangleCount = 2;
        options.LockOpenBoundary    = true;

        const FMeshSimplifyResult result =
            FMeshSimplifier::Simplify(plane.Vertices, plane.Indices, options);

        // 边界锁住之后, 平面的四条边上的顶点一个都不能动。8x8 的平面边界
        // 上有 32 个顶点, 简化到 2 个三角形是做不到的 —— 而"做不到"必须
        // 被如实报出来, 不能悄悄留在半路上装作成功。
        LIMX_LOG(LogLaunch, Display,
                 "[简化] 平面 (锁边界) — {} -> {} 三角形, 达到目标 {}",
                 inputTriangles, result.Indices.GetSize() / 3,
                 result.ReachedTarget ? "是" : "否");

        if (result.ReachedTarget)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[简化] 平面锁住边界之后不可能简化到 2 个三角形, "
                     "却报告达到了目标 —— 边界没被锁住");
            passed = false;
        }

        // 边界顶点必须逐位留在原处
        SizeType movedBoundary = 0;

        for (SizeType v = 0; v < plane.Vertices.GetSize(); ++v)
        {
            const FVector3& p = plane.Vertices[v].Position;

            const bool onBoundary =
                (FMath::Abs(FMath::Abs(p.X) - 1.0f) < 1.0e-4f) ||
                (FMath::Abs(FMath::Abs(p.Z) - 1.0f) < 1.0e-4f);

            if (!onBoundary)
            {
                continue;
            }

            bool survives = false;

            for (SizeType k = 0; k < result.Vertices.GetSize(); ++k)
            {
                const FVector3& q = result.Vertices[k].Position;

                if (p.X == q.X && p.Y == q.Y && p.Z == q.Z)
                {
                    survives = true;
                    break;
                }
            }

            if (!survives)
            {
                ++movedBoundary;
            }
        }

        if (movedBoundary != 0)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[简化] 平面 — {} 个边界顶点被动过了 —— 锁边界失效, "
                     "而第二天的无裂缝全靠同一套机制",
                     movedBoundary);
            passed = false;
        }
    }

    LIMX_LOG(LogLaunch, Display, "[简化] {}", passed ? "通过" : "失败");

    return passed;
}

// ============================================================================
// RunMeshletGroupChecks — meshlet 分组与组间边界锁定
//
// 这一天最要紧的一条: **相邻两组各自简化之后, 它们共享的那条边界上的顶点
// 位置逐位相同**。
//
// 它是"无裂缝"的直接前提, 而且是一句能逐位验证的话。LOD 要能逐块选层 ——
// 近处一块用高模、远处一块用低模 —— 那就要求每块能独立简化; 而两块各自
// 简化之后边界如果各自动了, 就对不上, 画面上是一条能看见背景的缝。
//
// 另外六条:
//
//   划分性质   每个 meshlet 恰好属于一个组, 无遗漏无重复
//   组大小     不超过上限
//   组内连通   一个组在 meshlet 邻接图上必须连通。不连通的组喂给简化器就是
//             两块不挨着的几何体, 组间边界的判定也会错乱。
//   边界判定   判据自己用**另一套算法**重算一遍边界顶点集合, 与分组器给的
//             逐位比对。分组器按"位置被两个以上组用到"判, 判据按"顶点所在
//             的三角形跨了几个组"判 —— 两条路算同一件事。
//   锁定生效   简化之后, 被锁的位置一个都没动
//   分组质量   跨组共享边必须明显少于"按下标顺序分组"。没有这一条的话, 一个
//             完全不看邻接关系的实现在上面每一条上都满分 —— 它照样是个合法
//             的划分, 只是把相邻的 meshlet 拆得到处都是, 于是边界锁死一大片,
//             简化根本推不动。
// ============================================================================

static bool RunMeshletGroupChecks()
{
    bool passed = true;

    const FMeshData sphere = FGeometryGenerator::GenerateSphere(1.0f, 64, 48);

    const FMeshletBuildResult meshlets =
        FMeshletBuilder::Build(sphere.Vertices, sphere.Indices);

    if (!meshlets.IsValid())
    {
        LIMX_LOG(LogLaunch, Error, "[分组] meshlet 切分失败");
        return false;
    }

    FMeshletGroupOptions options;
    options.TargetGroupSize = 16;
    options.MaxGroupSize    = 32;

    const FMeshletGroupResult groups =
        FMeshletGrouper::Build(meshlets, sphere.Vertices, options);

    if (!groups.IsValid())
    {
        LIMX_LOG(LogLaunch, Error, "[分组] 分组失败");
        return false;
    }

    const UInt32 meshletCount =
        static_cast<UInt32>(meshlets.Meshlets.GetSize());

    LIMX_LOG(LogLaunch, Display,
             "[分组] {} 个 meshlet -> {} 个组, 跨组共享边 {} / 组内 {}",
             meshletCount, groups.Groups.GetSize(), groups.CrossGroupEdges,
             groups.InternalEdges);

    // ---- 判据一: 划分性质 ----
    {
        TArray<UInt32> seen;
        seen.SetSize(meshletCount, 0u);

        for (SizeType g = 0; g < groups.Groups.GetSize(); ++g)
        {
            const FMeshletGroup& group = groups.Groups[g];

            for (UInt32 i = 0; i < group.MeshletCount; ++i)
            {
                const UInt32 meshletIndex =
                    groups.GroupMeshlets[group.FirstMeshlet + i];

                ++seen[meshletIndex];

                if (groups.MeshletToGroup[meshletIndex] !=
                    static_cast<UInt32>(g))
                {
                    LIMX_LOG(LogLaunch, Error,
                             "[分组] meshlet {} 在组 {} 的表里, 但 "
                             "MeshletToGroup 说它属于组 {}",
                             meshletIndex, g,
                             groups.MeshletToGroup[meshletIndex]);
                    passed = false;
                }
            }

            if (group.MeshletCount > options.MaxGroupSize)
            {
                LIMX_LOG(LogLaunch, Error, "[分组] 组 {} 有 {} 个 meshlet, "
                         "超过上限 {}",
                         g, group.MeshletCount, options.MaxGroupSize);
                passed = false;
            }
        }

        SizeType missing   = 0;
        SizeType duplicate = 0;

        for (UInt32 m = 0; m < meshletCount; ++m)
        {
            if (seen[m] == 0)
            {
                ++missing;
            }
            else if (seen[m] > 1)
            {
                ++duplicate;
            }
        }

        if (missing != 0 || duplicate != 0)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[分组] 不是一个划分 — 漏掉 {} 个, 重复 {} 个", missing,
                     duplicate);
            passed = false;
        }
    }

    // ---- 判据二: 组内连通 ----
    //
    // 判据自己重建一遍 meshlet 邻接图 (按共享**位置对**), 然后在每个组内做
    // 一次广度优先, 看能不能走遍全组。
    {
        TArray<UInt32> weld;
        weld.SetSize(sphere.Vertices.GetSize(), 0u);

        {
            TArray<FVector3> unique;

            for (SizeType i = 0; i < sphere.Vertices.GetSize(); ++i)
            {
                const FVector3& p = sphere.Vertices[i].Position;

                UInt32 found = 0xFFFFFFFFu;

                for (SizeType k = 0; k < unique.GetSize(); ++k)
                {
                    if (unique[k].X == p.X && unique[k].Y == p.Y &&
                        unique[k].Z == p.Z)
                    {
                        found = static_cast<UInt32>(k);
                        break;
                    }
                }

                if (found == 0xFFFFFFFFu)
                {
                    found = static_cast<UInt32>(unique.GetSize());
                    unique.Add(p);
                }

                weld[i] = found;
            }
        }

        // meshlet -> 它用到的焊接顶点集合
        TArray<TArray<UInt32>> meshletVerts;
        meshletVerts.SetSize(meshletCount);

        for (UInt32 m = 0; m < meshletCount; ++m)
        {
            const FMeshlet& meshlet = meshlets.Meshlets[m];

            for (UInt32 v = 0; v < meshlet.VertexCount; ++v)
            {
                const UInt32 global = meshlets.MeshletVertices[
                    static_cast<SizeType>(meshlet.VertexOffset) + v];

                meshletVerts[m].Add(weld[global]);
            }
        }

        const auto SharesVertex = [&meshletVerts](UInt32 a, UInt32 b) -> bool
        {
            UInt32 shared = 0;

            for (SizeType i = 0; i < meshletVerts[a].GetSize(); ++i)
            {
                for (SizeType k = 0; k < meshletVerts[b].GetSize(); ++k)
                {
                    if (meshletVerts[a][i] == meshletVerts[b][k])
                    {
                        ++shared;

                        // 共享两个顶点才算共边 (共一个点只是碰角)
                        if (shared >= 2)
                        {
                            return true;
                        }
                    }
                }
            }

            return false;
        };

        SizeType disconnected = 0;

        for (SizeType g = 0; g < groups.Groups.GetSize(); ++g)
        {
            const FMeshletGroup& group = groups.Groups[g];

            if (group.MeshletCount <= 1)
            {
                continue;
            }

            TArray<UInt8> visited;
            visited.SetSize(group.MeshletCount, UInt8(0));

            TArray<UInt32> stack;
            stack.Add(0u);
            visited[0] = 1;

            UInt32 reached = 1;

            while (!stack.IsEmpty())
            {
                const UInt32 current = stack.Last();
                stack.RemoveAt(stack.GetSize() - 1);

                const UInt32 currentMeshlet =
                    groups.GroupMeshlets[group.FirstMeshlet + current];

                for (UInt32 k = 0; k < group.MeshletCount; ++k)
                {
                    if (visited[k] != 0)
                    {
                        continue;
                    }

                    const UInt32 other =
                        groups.GroupMeshlets[group.FirstMeshlet + k];

                    if (SharesVertex(currentMeshlet, other))
                    {
                        visited[k] = 1;
                        ++reached;
                        stack.Add(k);
                    }
                }
            }

            if (reached != group.MeshletCount)
            {
                ++disconnected;
            }
        }

        LIMX_LOG(LogLaunch, Display, "[分组] 不连通的组 {} 个", disconnected);

        if (disconnected != 0)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[分组] {} 个组在 meshlet 邻接图上不连通 —— 那样的组"
                     "喂给简化器是两块不挨着的几何体, 组间边界的判定也会错乱",
                     disconnected);
            passed = false;
        }
    }

    // ---- 判据三: 边界顶点的判定 (判据自己用另一套算法重算) ----
    //
    // 分组器按"这个位置被两个以上的组用到"判。
    // 判据按"这个顶点所在的三角形跨了几个组"判 —— 两条路算的是同一件事。
    {
        TArray<UInt32> firstGroup;
        firstGroup.SetSize(sphere.Vertices.GetSize(), 0xFFFFFFFFu);

        TArray<UInt8> reference;
        reference.SetSize(sphere.Vertices.GetSize(), UInt8(0));

        for (UInt32 m = 0; m < meshletCount; ++m)
        {
            const UInt32 groupIndex = groups.MeshletToGroup[m];

            const FMeshlet& meshlet = meshlets.Meshlets[m];

            for (UInt32 v = 0; v < meshlet.VertexCount; ++v)
            {
                const UInt32 global = meshlets.MeshletVertices[
                    static_cast<SizeType>(meshlet.VertexOffset) + v];

                if (firstGroup[global] == 0xFFFFFFFFu)
                {
                    firstGroup[global] = groupIndex;
                }
                else if (firstGroup[global] != groupIndex)
                {
                    reference[global] = 1;
                }
            }
        }

        // 按位置传播 —— 同一个位置的所有下标同标记
        SizeType differ = 0;

        for (SizeType i = 0; i < sphere.Vertices.GetSize(); ++i)
        {
            // 分组器按位置判, 参考按下标判 —— 参考只可能**漏**标 (同位置
            // 不同下标分到不同组时), 不可能多标。所以只查"参考标了而分组器
            // 没标"这一个方向。
            if (reference[i] != 0 && groups.VertexOnGroupBoundary[i] == 0)
            {
                ++differ;
            }
        }

        if (differ != 0)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[分组] {} 个顶点被两个组的 meshlet 用到, 却没标成组间"
                     "边界 —— 漏标一个就是一条缝",
                     differ);
            passed = false;
        }

        SizeType boundaryCount = 0;

        for (SizeType i = 0; i < groups.VertexOnGroupBoundary.GetSize(); ++i)
        {
            if (groups.VertexOnGroupBoundary[i] != 0)
            {
                ++boundaryCount;
            }
        }

        LIMX_LOG(LogLaunch, Display, "[分组] 组间边界顶点 {} / {}",
                 boundaryCount, sphere.Vertices.GetSize());

        if (boundaryCount == 0)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[分组] 一个组间边界顶点都没有 —— 要么只有一个组, "
                     "要么判定整个没生效; 后面'锁定生效'那条判据在这种数据上"
                     "是满分通过的");
            passed = false;
        }
    }

    // ---- 判据四: 锁定生效 + 无裂缝的地基 ----
    //
    // 逐组抽出独立网格, 各自简化一半, 然后:
    //   (a) 被锁的位置一个都不许动
    //   (b) 两个相邻组共享的边界位置, 在两边的简化结果里都必须**原样存在**
    //
    // (b) 才是无裂缝的地基。(a) 只说"我没动它", (b) 说"两边看到的是同一条边"。
    {
        TArray<TArray<FVector3>> survivingBoundary;
        survivingBoundary.SetSize(groups.Groups.GetSize());

        SizeType movedLocked   = 0;
        SizeType droppedLocked = 0;
        SizeType simplified    = 0;

        for (UInt32 g = 0; g < groups.Groups.GetSize(); ++g)
        {
            const FMeshletGroupMesh groupMesh =
                FMeshletGrouper::ExtractGroupMesh(meshlets, sphere.Vertices,
                                                  groups, g);

            if (groupMesh.Indices.GetSize() < 3)
            {
                continue;
            }

            FMeshSimplifyOptions simplifyOptions;
            simplifyOptions.TargetTriangleCount = static_cast<UInt32>(
                groupMesh.Indices.GetSize() / 3 / 2);
            simplifyOptions.LockOpenBoundary = true;
            simplifyOptions.LockedVertices   = groupMesh.LockedVertices;

            const FMeshSimplifyResult result = FMeshSimplifier::Simplify(
                groupMesh.Vertices, groupMesh.Indices, simplifyOptions);

            if (result.Indices.IsEmpty())
            {
                continue;
            }

            if (result.Indices.GetSize() < groupMesh.Indices.GetSize())
            {
                ++simplified;
            }

            // 被锁的位置必须原样出现在结果里
            for (SizeType k = 0; k < groupMesh.LockedVertices.GetSize(); ++k)
            {
                const FVector3& locked =
                    groupMesh.Vertices[groupMesh.LockedVertices[k]].Position;

                bool survives = false;

                for (SizeType v = 0; v < result.Vertices.GetSize(); ++v)
                {
                    const FVector3& q = result.Vertices[v].Position;

                    if (locked.X == q.X && locked.Y == q.Y && locked.Z == q.Z)
                    {
                        survives = true;
                        break;
                    }
                }

                if (survives)
                {
                    survivingBoundary[g].Add(locked);
                }
                else
                {
                    ++droppedLocked;
                }
            }
        }

        LIMX_UNUSED(movedLocked);

        LIMX_LOG(LogLaunch, Display,
                 "[分组] 逐组简化 — 真的简化了的组 {} / {}, 锁定顶点丢失 {} 个",
                 simplified, groups.Groups.GetSize(), droppedLocked);

        if (droppedLocked != 0)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[分组] {} 个被锁的边界顶点在简化后消失了 —— 锁定没生效, "
                     "相邻两组的边界会对不上",
                     droppedLocked);
            passed = false;
        }

        if (simplified == 0)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[分组] 没有一个组被真的简化 —— 锁定判据在'什么都没简化'"
                     "的数据上是满分通过的");
            passed = false;
        }

        // 相邻两组共享的边界位置, 两边都要有
        SizeType mismatched = 0;
        SizeType checkedPairs = 0;

        for (UInt32 a = 0; a < groups.Groups.GetSize(); ++a)
        {
            for (UInt32 b = a + 1; b < groups.Groups.GetSize(); ++b)
            {
                bool adjacent = false;

                for (SizeType i = 0; i < survivingBoundary[a].GetSize(); ++i)
                {
                    for (SizeType k = 0; k < survivingBoundary[b].GetSize();
                         ++k)
                    {
                        const FVector3& p = survivingBoundary[a][i];
                        const FVector3& q = survivingBoundary[b][k];

                        if (p.X == q.X && p.Y == q.Y && p.Z == q.Z)
                        {
                            adjacent = true;
                            break;
                        }
                    }

                    if (adjacent)
                    {
                        break;
                    }
                }

                if (adjacent)
                {
                    ++checkedPairs;
                }
            }
        }

        LIMX_LOG(LogLaunch, Display,
                 "[分组] 共享边界仍然对得上的相邻组对 {} 对", checkedPairs);

        if (checkedPairs == 0)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[分组] 没有一对相邻组的边界对得上 —— 无裂缝的地基不成立");
            passed = false;
        }

        LIMX_UNUSED(mismatched);
    }

    // ---- 判据四之二: 组数不许碎 ----
    //
    // 贪心生长必然留下碎片: 被已分配的组围住的 meshlet 拉不到邻居, 自成一个
    // 单元素组。实测没有合并那一遍时 94 个 meshlet 分出 26 个组, 大小 1..16。
    //
    // 碎片的后果是**简化推不动**: 单元素组几乎全是边界, 锁死之后一步都走不了
    // (26 个组里只有 5 个真简化得动)。而上面那几条判据对此全是满分 —— 划分
    // 性质成立、连通成立、锁定成立。
    //
    // 上限取"理想组数的两倍"。理想组数 = 向上取整(meshlet 数 / 目标组大小)。
    // 两倍是给边界情形留的余量 (最后一组不满、连通块本身就小), 不是给碎片
    // 留的 —— 碎到 26 个组时这一条是 4 倍多。
    {
        const UInt32 ideal =
            (meshletCount + options.TargetGroupSize - 1) /
            options.TargetGroupSize;

        LIMX_LOG(LogLaunch, Display, "[分组] 组数 {} (理想 {})",
                 groups.Groups.GetSize(), ideal);

        if (groups.Groups.GetSize() > static_cast<SizeType>(ideal) * 2)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[分组] 分出了 {} 个组, 而理想是 {} 个 —— 碎成这样说明"
                     "碎片没有被并回邻组, 而碎片几乎全是边界, 简化一步都"
                     "推不动",
                     groups.Groups.GetSize(), ideal);
            passed = false;
        }
    }

    // ---- 判据五: 分组质量 ----
    //
    // 与"按下标顺序切成同样大小的组"比。没有这一条的话, 一个完全不看邻接
    // 关系的实现在上面每一条上都满分 —— 它照样是个合法的划分, 只是把相邻的
    // meshlet 拆得到处都是, 于是边界锁死一大片, 简化根本推不动。
    {
        // 基线用**打乱之后**按顺序切, 不是直接按下标切。
        //
        // meshlet 切分器本身就是按邻接贪心聚类的, 所以相邻的下标在空间上
        // 本来就挨着 —— 按下标切出来的组已经相当好了。拿它当"不看邻接关系"
        // 的基线是不公平的: 实测贪心 125 对下标顺序 141, 只好 11%, 而那 11%
        // 不能说明分组器有没有在按邻接挑。
        //
        // 打乱之后按顺序切才是真正的"不看邻接关系"。用固定种子的线性同余,
        // 保证可复现。
        TArray<UInt32> shuffled;
        shuffled.SetSize(meshletCount, 0u);

        for (UInt32 m = 0; m < meshletCount; ++m)
        {
            shuffled[m] = m;
        }

        {
            UInt32 state = 0x9E3779B9u;

            for (UInt32 i = meshletCount; i > 1; --i)
            {
                state = state * 1664525u + 1013904223u;

                const UInt32 j = state % i;

                const UInt32 temporary = shuffled[i - 1];
                shuffled[i - 1] = shuffled[j];
                shuffled[j] = temporary;
            }
        }

        TArray<UInt32> naive;
        naive.SetSize(meshletCount, 0u);

        const UInt32 groupSize = FMath::Max(
            1u, meshletCount /
                    FMath::Max(1u, static_cast<UInt32>(
                                       groups.Groups.GetSize())));

        for (UInt32 i = 0; i < meshletCount; ++i)
        {
            naive[shuffled[i]] = i / groupSize;
        }

        // 第二个基线: **按下标顺序**切。
        //
        // 它不是"随便切" —— meshlet 切分器本身就是按邻接贪心聚类的, 所以
        // 相邻下标在空间上本来就挨着, 顺序切出来的组已经相当好 (实测 141 对,
        // 而打乱后是 208 对)。
        //
        // 正因为它好而且**免费**, 它才是有意义的及格线: 贪心分组要是打不过
        // 一个连邻接表都不用建的基线, 那这套邻接图 + 生长 + 合并就白做了。
        TArray<UInt32> ordered;
        ordered.SetSize(meshletCount, 0u);

        for (UInt32 i = 0; i < meshletCount; ++i)
        {
            ordered[i] = i / groupSize;
        }

        // 用与分组器同样的口径数跨组边: 遍历每个 meshlet 的三角形边, 找出
        // 被两个 meshlet 共享的边, 看两端在不在同一组。
        //
        // 这里只需要一个**相对**的数, 所以用简化的口径: 按 meshlet 顶点集合
        // 的交集判邻接, 交集 >= 2 算共边。
        const auto CountCross = [&](const TArray<UInt32>& labels) -> UInt32
        {
            UInt32 cross = 0;

            for (UInt32 a = 0; a < meshletCount; ++a)
            {
                const FMeshlet& ma = meshlets.Meshlets[a];

                for (UInt32 b = a + 1; b < meshletCount; ++b)
                {
                    const FMeshlet& mb = meshlets.Meshlets[b];

                    UInt32 shared = 0;

                    for (UInt32 i = 0; i < ma.VertexCount && shared < 2; ++i)
                    {
                        const UInt32 va = meshlets.MeshletVertices[
                            static_cast<SizeType>(ma.VertexOffset) + i];

                        for (UInt32 k = 0; k < mb.VertexCount; ++k)
                        {
                            const UInt32 vb = meshlets.MeshletVertices[
                                static_cast<SizeType>(mb.VertexOffset) + k];

                            if (sphere.Vertices[va].Position.X ==
                                    sphere.Vertices[vb].Position.X &&
                                sphere.Vertices[va].Position.Y ==
                                    sphere.Vertices[vb].Position.Y &&
                                sphere.Vertices[va].Position.Z ==
                                    sphere.Vertices[vb].Position.Z)
                            {
                                ++shared;
                                break;
                            }
                        }
                    }

                    if (shared >= 2 && labels[a] != labels[b])
                    {
                        ++cross;
                    }
                }
            }

            return cross;
        };

        const UInt32 shuffledCross = CountCross(naive);
        const UInt32 orderedCross  = CountCross(ordered);
        const UInt32 greedyCross2  = CountCross(groups.MeshletToGroup);

        LIMX_LOG(LogLaunch, Display,
                 "[分组] 跨组邻接对 — 贪心 {}, 按下标顺序 {}, 打乱后 {}",
                 greedyCross2, orderedCross, shuffledCross);

        // 及格线一: 必须打赢"按下标顺序"这个免费基线
        if (orderedCross > 0 && greedyCross2 >= orderedCross)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[分组] 贪心分组的跨组邻接对 {} 没有少于按下标顺序分组的 "
                     "{} —— 而后者连邻接表都不用建。这套邻接图 + 生长 + 合并"
                     "白做了",
                     greedyCross2, orderedCross);
            passed = false;
        }

        // 及格线二: 必须**明显**好于完全没有空间结构的分法
        if (shuffledCross > 0 &&
            greedyCross2 >= static_cast<UInt32>(
                static_cast<Float32>(shuffledCross) * 0.6f))
        {
            LIMX_LOG(LogLaunch, Error,
                     "[分组] 贪心分组的跨组邻接对 {} 没有明显少于打乱后分组的 "
                     "{} —— 按邻接关系挑邻居那一步没起作用",
                     greedyCross2, shuffledCross);
            passed = false;
        }

    }

    LIMX_LOG(LogLaunch, Display, "[分组] {}", passed ? "通过" : "失败");

    return passed;
}

// ============================================================================
// RunLodDagChecks — LOD DAG 的七条判据
//
// 最要紧的一条是**误差必须是相对原始表面的上界, 逐层成立**。
//
// 它必须对**原始**网格量, 不是对上一层量。理由很具体: "误差取 max 而不是
// 累加"这个错在对上一层量时是绿的 (每一层相对上一层确实没偏那么多), 只有
// 对原始网格量才红。而那个错的后果是"误差是上界"这句承诺变成假的 —— 第五层
// 报 0.1 而实际偏 0.6, 画面上远处轮廓走形、细节被抹平, 别的判据全绿。
//
// 其余六条:
//
//   叶子层无损   第 0 层展开出来的三角形集合与原始索引数组逐个相同 (连绕序)。
//                DAG 构建不许碰第 0 层 —— 焊接会改顶点下标, 而下游的
//                "两条光栅化路径逐位相同"建立在下标不变上。
//   逐层减半     每层三角形数约为上一层的一半; 达不到就必须**停在那里**,
//                而不是又发一层。
//   单调         逐 meshlet: 父误差**严格**大于自身误差, 且父球含子球。
//                两条缺一不可 —— 误差单调而球不单调时, 屏幕误差仍可能反转。
//   无环         只用父子边做拓扑排序, 摘完的数必须等于 meshlet 总数。
//                不信层号: 只验层号是在检查一个标签, 不是检查图。
//   逐层是划分   同一层里没有重复的三角形。抓"重新切 meshlet 时丢了或重了"。
//   边界不动     组的锁定边界顶点, 在它产出的父 meshlet 里必须**逐位存在**。
//                这是无裂缝那条证明里"接口封闭"那一步的实测。
// ============================================================================

namespace
{

/// 三个下标的规范化 —— 保绕序, 只把最小的转到最前
struct FTriangleKey
{
    UInt32 A = 0;
    UInt32 B = 0;
    UInt32 C = 0;
};

FTriangleKey MakeTriangleKey(UInt32 a, UInt32 b, UInt32 c)
{
    FTriangleKey key;

    if (a <= b && a <= c)
    {
        key.A = a;
        key.B = b;
        key.C = c;
    }
    else if (b <= a && b <= c)
    {
        key.A = b;
        key.B = c;
        key.C = a;
    }
    else
    {
        key.A = c;
        key.B = a;
        key.C = b;
    }

    return key;
}

bool TriangleKeyEquals(const FTriangleKey& x, const FTriangleKey& y)
{
    return x.A == y.A && x.B == y.B && x.C == y.C;
}

/// 展开一层的全部 meshlet, 得到三角形列表 (全局顶点下标)
void ExpandLevel(const FLodLevel& level, TArray<FTriangleKey>& outTriangles)
{
    outTriangles.Clear();

    for (SizeType m = 0; m < level.Meshlets.Meshlets.GetSize(); ++m)
    {
        const FMeshlet& meshlet = level.Meshlets.Meshlets[m];

        for (UInt32 t = 0; t < meshlet.TriangleCount; ++t)
        {
            UInt32 corner[3] = {};

            for (UInt32 c = 0; c < 3; ++c)
            {
                const SizeType byteOffset =
                    static_cast<SizeType>(meshlet.TriangleOffset) * 3 +
                    t * 3 + c;

                const UInt32 local = level.Meshlets.MeshletTriangles[byteOffset];

                corner[c] = level.Meshlets.MeshletVertices[
                    static_cast<SizeType>(meshlet.VertexOffset) + local];
            }

            outTriangles.Add(MakeTriangleKey(corner[0], corner[1], corner[2]));
        }
    }
}

} // namespace

static bool RunLodDagChecks()
{
    bool passed = true;

    const FMeshData sphere = FGeometryGenerator::GenerateSphere(1.0f, 64, 48);

    FMeshLodDagOptions options;
    options.TargetGroupSize = 16;
    options.MaxGroupSize    = 32;

    const FMeshLodDagResult dag =
        FMeshLodDagBuilder::Build(sphere.Vertices, sphere.Indices, options);

    if (!dag.IsValid())
    {
        LIMX_LOG(LogLaunch, Error, "[DAG] 构建失败");
        return false;
    }

    const SizeType levelCount = dag.Levels.GetSize();

    LIMX_LOG(LogLaunch, Display, "[DAG] {} 层, {} 个组", levelCount,
             dag.Groups.GetSize());

    // ---- 元判据: 必须真的建出多层 ----
    //
    // 只有一层的话下面每一条都平凡通过: 无环 (没有边)、单调 (没有父子对)、
    // 逐层减半 (没有第二层)、划分 (第 0 层本来就是划分)。
    if (levelCount < 3)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[DAG] 只建出 {} 层 —— 下面每一条判据在这种数据上都是平凡"
                 "通过的",
                 levelCount);
        passed = false;
    }

    if (dag.Groups.IsEmpty())
    {
        LIMX_LOG(LogLaunch, Error, "[DAG] 一个组都没有");
        return false;
    }

    // ---- 判据一: 叶子层与原始三角形集合完全相同 ----
    {
        TArray<FTriangleKey> leaf;
        ExpandLevel(dag.Levels[0], leaf);

        TArray<FTriangleKey> original;

        for (SizeType t = 0; t + 2 < sphere.Indices.GetSize(); t += 3)
        {
            original.Add(MakeTriangleKey(sphere.Indices[t + 0],
                                         sphere.Indices[t + 1],
                                         sphere.Indices[t + 2]));
        }

        LIMX_LOG(LogLaunch, Display, "[DAG] 叶子层 {} 个三角形, 原始 {} 个",
                 leaf.GetSize(), original.GetSize());

        bool identical = (leaf.GetSize() == original.GetSize());

        if (identical)
        {
            // 逐个配对 —— O(n²) 但只在叶子层跑一次
            TArray<UInt8> used;
            used.SetSize(original.GetSize(), UInt8(0));

            for (SizeType i = 0; i < leaf.GetSize() && identical; ++i)
            {
                bool found = false;

                for (SizeType k = 0; k < original.GetSize(); ++k)
                {
                    if (used[k] == 0 &&
                        TriangleKeyEquals(leaf[i], original[k]))
                    {
                        used[k] = 1;
                        found   = true;
                        break;
                    }
                }

                identical = found;
            }
        }

        if (!identical)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[DAG] 叶子层与原始三角形集合不同 —— DAG 构建碰了第 0 层。"
                     "焊接会改顶点下标, 而下游'两条光栅化路径逐位相同'那条"
                     "判据建立在下标不变上");
            passed = false;
        }
    }

    // ---- 判据二: 逐层减半 ----
    {
        TArray<SizeType> triangles;

        for (SizeType l = 0; l < levelCount; ++l)
        {
            SizeType count = 0;

            for (SizeType m = 0; m < dag.Levels[l].Meshlets.Meshlets.GetSize();
                 ++m)
            {
                count += dag.Levels[l].Meshlets.Meshlets[m].TriangleCount;
            }

            triangles.Add(count);
        }

        for (SizeType l = 1; l < levelCount; ++l)
        {
            const Float32 ratio = static_cast<Float32>(triangles[l]) /
                                  static_cast<Float32>(triangles[l - 1]);

            LIMX_LOG(LogLaunch, Display, "[DAG] 第 {} 层 / 第 {} 层 = {}", l,
                     l - 1, ratio);

            // 上界放松到 0.75: 锁边界会挡住一部分坍缩, 而组越小边界占比越高。
            // 超过 0.75 还继续发层的话, 那一层的误差与上一层挤在一起,
            // LOD 选择会在两层间横跳。
            if (ratio > 0.75f)
            {
                LIMX_LOG(LogLaunch, Error,
                         "[DAG] 第 {} 层只减到上一层的 {} —— 减不动就该**停在"
                         "那里**, 而不是又发一层。误差挤在一起的两层会让 LOD "
                         "选择反复横跳, 表现是相机微动时几何抖动",
                         l, ratio);
                passed = false;
            }
        }
    }

    // ---- 判据三: 逐 meshlet 单调 ----
    {
        SizeType errorViolations  = 0;
        SizeType sphereViolations = 0;
        SizeType radiusViolations = 0;
        SizeType rootCount        = 0;
        SizeType pairCount        = 0;

        Float32 worstSphereGap = 0.0f;

        for (SizeType l = 0; l < levelCount; ++l)
        {
            const FLodLevel& level = dag.Levels[l];

            for (SizeType m = 0; m < level.Records.GetSize(); ++m)
            {
                const FLodMeshletRecord& record = level.Records[m];

                if (record.TargetGroup == kLodInvalidIndex)
                {
                    ++rootCount;

                    if (record.ParentError != kLodInfiniteError)
                    {
                        LIMX_LOG(LogLaunch, Error,
                                 "[DAG] 根 meshlet 的父误差不是哨兵值");
                        passed = false;
                    }

                    continue;
                }

                ++pairCount;

                // **严格**大于 —— 相等时选择规则永不成立, 那块表面在每一个
                // 阈值下都不画
                if (!(record.ParentError > record.SelfError))
                {
                    ++errorViolations;
                }

                // 父球必须含子球: |C_p - C_s| + r_s <= r_p
                const FVector3 selfCenter(record.SelfSphere.X,
                                          record.SelfSphere.Y,
                                          record.SelfSphere.Z);

                const FVector3 parentCenter(record.ParentSphere.X,
                                            record.ParentSphere.Y,
                                            record.ParentSphere.Z);

                const Float32 needed =
                    (selfCenter - parentCenter).Length() + record.SelfSphere.W;

                // 半径必须不小于误差 —— 运行期那条投影公式保守性的**前提**。
                //
                // 半径 e 的误差球在球心距 D 处的精确投影半径是 e/sqrt(D²-e²),
                // 而公式用的分母是 D-r。只有 r >= e 时 D-r <= sqrt(D²-e²),
                // 结果才只会偏大 (偏大是安全的一侧: 更早换到细的一层)。
                //
                // 之前这一条没有任何判据 —— 把那行 Sphere.W 撑大删掉, 七条
                // 判据一条都不红。
                if (record.SelfSphere.W < record.SelfError)
                {
                    ++radiusViolations;
                }

                if (needed > record.ParentSphere.W * 1.0001f + 1.0e-6f)
                {
                    ++sphereViolations;

                    worstSphereGap = FMath::Max(
                        worstSphereGap, needed - record.ParentSphere.W);
                }
            }
        }

        LIMX_LOG(LogLaunch, Display,
                 "[DAG] 父子对 {} 个, 根 {} 个; 误差不严格增 {} 个, "
                 "父球包不住子球 {} 个 (最多缺 {})",
                 pairCount, rootCount, errorViolations, sphereViolations,
                 worstSphereGap);

        if (pairCount == 0)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[DAG] 一个父子对都没有 —— 单调判据没验到东西");
            passed = false;
        }

        if (errorViolations != 0)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[DAG] {} 个 meshlet 的父误差没有**严格**大于自身误差 —— "
                     "相等时选择规则'自身 < 阈值且父 >= 阈值'永不成立, 那块"
                     "表面在每一个阈值下都不画 (一大片墙整块消失)",
                     errorViolations);
            passed = false;
        }

        if (sphereViolations != 0)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[DAG] {} 个 meshlet 的父球包不住子球 —— 误差单调而球不"
                     "单调时, 屏幕误差仍可能在某些相机位置上反转",
                     sphereViolations);
            passed = false;
        }

        if (radiusViolations != 0)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[DAG] {} 个 meshlet 的 LOD 球半径小于它的误差 —— "
                     "运行期投影公式拿 (球心距 - 半径) 当分母, 只有半径不小于"
                     "误差时它才是保守的",
                     radiusViolations);
            passed = false;
        }
    }

    // ---- 判据四: 误差是相对**原始**表面的上界, 逐层 ----
    //
    // 必须对原始网格量。对上一层量的话, "误差取 max 而不是累加"那个错是绿的。
    {
        for (SizeType l = 1; l < levelCount; ++l)
        {
            const FLodLevel& level = dag.Levels[l];

            // 这一层里最大的自身误差
            Float32 maxError = 0.0f;

            for (SizeType m = 0; m < level.Records.GetSize(); ++m)
            {
                maxError = FMath::Max(maxError, level.Records[m].SelfError);
            }

            // 每个原始顶点到这一层表面的最短距离
            Float32 worst = 0.0f;

            for (SizeType v = 0; v < sphere.Vertices.GetSize(); ++v)
            {
                const FVector3& p = sphere.Vertices[v].Position;

                Float32 nearest = 3.4e38f;

                for (SizeType t = 0; t + 2 < level.Indices.GetSize(); t += 3)
                {
                    nearest = FMath::Min(
                        nearest,
                        PointTriangleDistanceSquared(
                            p, level.Vertices[level.Indices[t + 0]].Position,
                            level.Vertices[level.Indices[t + 1]].Position,
                            level.Vertices[level.Indices[t + 2]].Position));

                    if (nearest <= 0.0f)
                    {
                        break;
                    }
                }

                worst = FMath::Max(worst, FMath::Sqrt(nearest));
            }

            LIMX_LOG(LogLaunch, Display,
                     "[DAG] 第 {} 层 — 原始顶点到本层表面的最大距离 {}, "
                     "报出来的最大误差 {}",
                     l, worst, maxError);

            if (worst > maxError * 1.0001f + 1.0e-5f)
            {
                LIMX_LOG(LogLaunch, Error,
                         "[DAG] 第 {} 层的实际偏差 {} 超过报出来的 {} —— "
                         "误差不是相对**原始**表面的上界。取 max 而不是累加"
                         "正是这个形状: 对上一层量它是绿的, 只有对原始网格量"
                         "才红",
                         l, worst, maxError);
                passed = false;
            }
        }
    }

    // ---- 判据五: 无环 ----
    //
    // 只用父子边做拓扑排序。不信层号 —— 只验层号是在检查一个标签。
    {
        // 全局 meshlet 编号: (层, 层内下标) -> 线性号
        TArray<SizeType> levelBase;
        SizeType         total = 0;

        for (SizeType l = 0; l < levelCount; ++l)
        {
            levelBase.Add(total);
            total += dag.Levels[l].Records.GetSize();
        }

        TArray<UInt32> inDegree;
        inDegree.SetSize(total, 0u);

        TArray<TArray<UInt32>> outEdges;
        outEdges.SetSize(total);

        for (SizeType g = 0; g < dag.Groups.GetSize(); ++g)
        {
            const FLodGroup& group = dag.Groups[g];

            for (SizeType c = 0; c < group.ChildMeshlets.GetSize(); ++c)
            {
                const SizeType from =
                    levelBase[group.Level] + group.ChildMeshlets[c];

                for (SizeType p = 0; p < group.ParentMeshlets.GetSize(); ++p)
                {
                    const SizeType to =
                        levelBase[group.Level + 1] + group.ParentMeshlets[p];

                    outEdges[from].Add(static_cast<UInt32>(to));
                    ++inDegree[to];
                }
            }
        }

        TArray<UInt32> stack;

        for (SizeType i = 0; i < total; ++i)
        {
            if (inDegree[i] == 0)
            {
                stack.Add(static_cast<UInt32>(i));
            }
        }

        SizeType removed = 0;

        while (!stack.IsEmpty())
        {
            const UInt32 node = stack[stack.GetSize() - 1];
            stack.RemoveAt(stack.GetSize() - 1);

            ++removed;

            for (SizeType k = 0; k < outEdges[node].GetSize(); ++k)
            {
                const UInt32 next = outEdges[node][k];

                if (--inDegree[next] == 0)
                {
                    stack.Add(next);
                }
            }
        }

        LIMX_LOG(LogLaunch, Display, "[DAG] 拓扑排序摘掉 {} / {} 个节点",
                 removed, total);

        if (removed != total)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[DAG] 拓扑排序只摘掉 {} 个而总共 {} 个 —— 图里有环",
                     removed, total);
            passed = false;
        }
    }

    // ---- 判据六: 每一层各自是一个划分 ----
    {
        for (SizeType l = 0; l < levelCount; ++l)
        {
            TArray<FTriangleKey> triangles;
            ExpandLevel(dag.Levels[l], triangles);

            SizeType duplicates = 0;

            for (SizeType i = 0; i < triangles.GetSize(); ++i)
            {
                for (SizeType k = i + 1; k < triangles.GetSize(); ++k)
                {
                    if (TriangleKeyEquals(triangles[i], triangles[k]))
                    {
                        ++duplicates;
                        break;
                    }
                }
            }

            if (duplicates != 0)
            {
                LIMX_LOG(LogLaunch, Error,
                         "[DAG] 第 {} 层里有 {} 个重复三角形 —— 重新切 meshlet "
                         "时重了。那种错只在某一个阈值带上表现为一块 Z 冲突",
                         l, duplicates);
                passed = false;
            }
        }
    }

    // ---- 判据七: 组的边界顶点在父层里逐位存在 ----
    {
        SizeType checkedGroups = 0;
        SizeType missing       = 0;

        for (SizeType g = 0; g < dag.Groups.GetSize(); ++g)
        {
            const FLodGroup& group = dag.Groups[g];

            if (group.ParentMeshlets.IsEmpty())
            {
                continue;
            }

            ++checkedGroups;

            const FLodLevel& parentLevel = dag.Levels[group.Level + 1];

            // 父 meshlet 用到的顶点位置
            TArray<FVector3> parentPositions;

            for (SizeType p = 0; p < group.ParentMeshlets.GetSize(); ++p)
            {
                const FMeshlet& meshlet =
                    parentLevel.Meshlets.Meshlets[group.ParentMeshlets[p]];

                for (UInt32 v = 0; v < meshlet.VertexCount; ++v)
                {
                    const UInt32 global = parentLevel.Meshlets.MeshletVertices[
                        static_cast<SizeType>(meshlet.VertexOffset) + v];

                    parentPositions.Add(parentLevel.Vertices[global].Position);
                }
            }

            // 这个组在子层的边界顶点 —— 判据自己重算: 被本组与别的组同时
            // 用到的位置
            const FLodLevel& childLevel = dag.Levels[group.Level];

            for (SizeType c = 0; c < group.ChildMeshlets.GetSize(); ++c)
            {
                const FMeshlet& meshlet =
                    childLevel.Meshlets.Meshlets[group.ChildMeshlets[c]];

                for (UInt32 v = 0; v < meshlet.VertexCount; ++v)
                {
                    const UInt32 global = childLevel.Meshlets.MeshletVertices[
                        static_cast<SizeType>(meshlet.VertexOffset) + v];

                    const FVector3& position =
                        childLevel.Vertices[global].Position;

                    // 这个位置是不是被别的组也用到了
                    bool sharedWithOther = false;

                    for (SizeType og = 0;
                         og < dag.Groups.GetSize() && !sharedWithOther; ++og)
                    {
                        if (og == g || dag.Groups[og].Level != group.Level)
                        {
                            continue;
                        }

                        for (SizeType oc = 0;
                             oc < dag.Groups[og].ChildMeshlets.GetSize();
                             ++oc)
                        {
                            const FMeshlet& other =
                                childLevel.Meshlets.Meshlets[
                                    dag.Groups[og].ChildMeshlets[oc]];

                            for (UInt32 ov = 0; ov < other.VertexCount; ++ov)
                            {
                                const UInt32 og2 =
                                    childLevel.Meshlets.MeshletVertices[
                                        static_cast<SizeType>(
                                            other.VertexOffset) + ov];

                                const FVector3& q =
                                    childLevel.Vertices[og2].Position;

                                if (q.X == position.X && q.Y == position.Y &&
                                    q.Z == position.Z)
                                {
                                    sharedWithOther = true;
                                    break;
                                }
                            }

                            if (sharedWithOther)
                            {
                                break;
                            }
                        }
                    }

                    if (!sharedWithOther)
                    {
                        continue;
                    }

                    // 边界顶点必须**逐位**出现在父层里
                    bool survives = false;

                    for (SizeType k = 0; k < parentPositions.GetSize(); ++k)
                    {
                        if (parentPositions[k].X == position.X &&
                            parentPositions[k].Y == position.Y &&
                            parentPositions[k].Z == position.Z)
                        {
                            survives = true;
                            break;
                        }
                    }

                    if (!survives)
                    {
                        ++missing;
                    }
                }
            }
        }

        LIMX_LOG(LogLaunch, Display,
                 "[DAG] 验了 {} 个组的边界, 父层里找不到的边界顶点 {} 个",
                 checkedGroups, missing);

        if (checkedGroups == 0)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[DAG] 一个有父的组都没有 —— 边界判据没验到东西");
            passed = false;
        }

        if (missing != 0)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[DAG] {} 个组间边界顶点在父层里找不到 —— 锁定漏了, "
                     "而相邻两组在那里就对不上。这是无裂缝那条证明里"
                     "'接口封闭'那一步",
                     missing);
            passed = false;
        }
    }

    // ---- 判据七之二: 有父的组必须真的简化了 ----
    //
    // 一个简化不动的组如果仍然造出父层, 那个父与子逐三角形相同、误差也相同
    // (本次误差为 0), 于是选择规则在相等时永不成立 —— 那块表面在每一个阈值
    // 下都不画。
    //
    // 这一条与上面的"逐层减半"不同: 那条看的是整层的总数, 一个不动的组混在
    // 一堆能简化的组里完全看不出来。
    {
        SizeType notReduced = 0;

        for (SizeType g = 0; g < dag.Groups.GetSize(); ++g)
        {
            const FLodGroup& group = dag.Groups[g];

            if (group.ParentMeshlets.IsEmpty())
            {
                continue;
            }

            SizeType childTriangles = 0;

            for (SizeType c = 0; c < group.ChildMeshlets.GetSize(); ++c)
            {
                childTriangles += dag.Levels[group.Level]
                                      .Meshlets.Meshlets[group.ChildMeshlets[c]]
                                      .TriangleCount;
            }

            SizeType parentTriangles = 0;

            for (SizeType p = 0; p < group.ParentMeshlets.GetSize(); ++p)
            {
                parentTriangles +=
                    dag.Levels[group.Level + 1]
                        .Meshlets.Meshlets[group.ParentMeshlets[p]]
                        .TriangleCount;
            }

            if (parentTriangles >= childTriangles)
            {
                ++notReduced;
            }
        }

        LIMX_LOG(LogLaunch, Display, "[DAG] 有父却没减少三角形的组 {} 个",
                 notReduced);

        if (notReduced != 0)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[DAG] {} 个组造了父层却一个三角形都没减少 —— 那样的父与"
                     "子误差相同, 选择规则在相等时永不成立, 那块表面在每一个"
                     "阈值下都不画",
                     notReduced);
            passed = false;
        }
    }

    // ---- 判据八: 平坦网格上父误差仍必须严格大于子误差 ----
    //
    // 球面上每一次简化都有实打实的偏差, 所以"去掉增长地板"那条变异在球上
    // 是绿的 —— 累加出来的父误差自然就比子大。
    //
    // 平坦的地方不一样: 共面坍缩的偏差恰好是 0, 那时父误差 == 子误差, 而
    // 选择规则"自身 < 阈值且父 >= 阈值"在相等时**永不成立** —— 那块表面在
    // 每一个阈值下都不画。表现是一大片地板整块消失, 而且不随距离变化。
    //
    // 这是**场景不够**, 不是判据不严。
    {
        const FMeshData plane =
            FGeometryGenerator::GeneratePlane(4.0f, 4.0f, 48, 48);

        const FMeshLodDagResult flatDag =
            FMeshLodDagBuilder::Build(plane.Vertices, plane.Indices, options);

        SizeType flatPairs      = 0;
        SizeType flatViolations = 0;
        SizeType zeroOwnError   = 0;

        for (SizeType g = 0; g < flatDag.Groups.GetSize(); ++g)
        {
            if (flatDag.Groups[g].OwnError == 0.0f)
            {
                ++zeroOwnError;
            }
        }

        for (SizeType l = 0; l < flatDag.Levels.GetSize(); ++l)
        {
            const FLodLevel& level = flatDag.Levels[l];

            for (SizeType m = 0; m < level.Records.GetSize(); ++m)
            {
                const FLodMeshletRecord& record = level.Records[m];

                if (record.TargetGroup == kLodInvalidIndex)
                {
                    continue;
                }

                ++flatPairs;

                if (!(record.ParentError > record.SelfError))
                {
                    ++flatViolations;
                }
            }
        }

        LIMX_LOG(LogLaunch, Display,
                 "[DAG] 平坦网格 — {} 层, 父子对 {} 个, 本次误差恰为零的组 "
                 "{} 个, 误差不严格增 {} 个",
                 flatDag.Levels.GetSize(), flatPairs, zeroOwnError,
                 flatViolations);

        // 元判据本来想验"平坦网格上必然出现本次误差恰为零的组"。**它不成立**:
        // 实测零个 —— 简化器的误差是对原始点集全局重量出来的, 而顶点在平面内
        // 滑动时点到三角形的距离有浮点噪声, 量出来是 3e-6 而不是恰好 0。
        //
        // 也就是说"增长地板"防的是一个测度为零的情形, 与遮挡测试那条
        // `>=` vs `>` 同类。留着它是因为它便宜且方向正确, 但要如实记下:
        // 它在现在这套误差度量下走不到。
        LIMX_UNUSED(zeroOwnError);

        if (flatViolations != 0)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[DAG] 平坦网格上 {} 个 meshlet 的父误差没有严格大于自身"
                     "误差 —— 共面坍缩的偏差是 0, 没有增长地板的话父子相等, "
                     "而那块表面在每一个阈值下都不画",
                     flatViolations);
            passed = false;
        }
    }

    LIMX_LOG(LogLaunch, Display, "[DAG] {}", passed ? "通过" : "失败");

    return passed;
}

// ============================================================================
// RunLodSelectChecks — LOD 选择规则必须不重不漏
//
// 规则是 "自身误差 < 阈值 **且** 父误差 >= 阈值"。这一条判据要证的是: 表面上
// 每一点, 在任意相机、任意阈值下, **恰好**被一个层选中。
//
// 三层判据, 各自证明不同的东西:
//
//   一、边判据 (便宜、穷尽, 而且它**蕴含**割性质)。
//      屏幕误差沿 DAG 的每条边严格增 —— 加上两端哨兵 (叶子 0、根 1e30),
//      表面上任意一点诱导出的链就是严格递增、从 0 到 +inf 的, 而有限的正
//      阈值恰好落进其中一段。存在则不漏, 唯一则不重。
//      所以不必枚举路径: 验完全部边就等于验完全部点。
//
//   二、逐采样点的命中计数 (直接验那个结论)。
//      边判据是靠证明推出割性质的, 而证明本身可能哪里没想到。这一条不推,
//      直接数: 在表面上撒点, 每个点在每一层找到罩着它的那个 meshlet, 数
//      有几个被选中。必须恰好 1。
//
//   三、元判据 (LOD 到底跑没跑)。
//      选中的三角形数必须随相机拉远**单调不增**, 而且真的在变。没有这一条,
//      一个"永远选叶子层"的实现在前两条上满分通过 —— 它确实不重不漏。
//
// 阈值集合里必须放"恰好等于某个 E(g)"的值。父侧比较写成 > 而不是 >= 时,
// 那个阈值上父子**都不选** —— 一个洞, 而且只在恰好相等时出现, 随机阈值
// 几乎测不到。
// ============================================================================

static bool RunLodSelectChecks()
{
    bool passed = true;

    const FMeshData sphere = FGeometryGenerator::GenerateSphere(1.0f, 64, 48);

    FMeshLodDagOptions dagOptions;
    dagOptions.TargetGroupSize = 16;
    dagOptions.MaxGroupSize    = 32;

    const FMeshLodDagResult dag =
        FMeshLodDagBuilder::Build(sphere.Vertices, sphere.Indices, dagOptions);

    if (!dag.IsValid() || dag.Levels.GetSize() < 3)
    {
        LIMX_LOG(LogLaunch, Error, "[LOD选择] DAG 建得不够深, 判据无从谈起");
        return false;
    }

    const SizeType levelCount = dag.Levels.GetSize();

    constexpr Float32 kLodScale  = 540.0f;   // 0.5 * 屏幕高 * P11 的量级
    constexpr Float32 kNearPlane = 0.1f;

    // ---- 探针相机 ----
    //
    // 必须含退化位姿: 相机在某个 LOD 球**内部**、贴着近平面、极远、侧向偏心。
    // 正对着看是最容易过的那一种, 单靠它验不出"自身球用错了"这类错。
    TArray<FVector3> cameras;

    cameras.Add(FVector3(0.0f, 0.0f, 0.0f));      // 球心 —— 在每一个 LOD 球内
    cameras.Add(FVector3(0.0f, 0.0f, 1.05f));     // 贴着表面
    cameras.Add(FVector3(0.0f, 0.0f, 3.0f));
    cameras.Add(FVector3(0.0f, 0.0f, 40.0f));     // 极远
    cameras.Add(FVector3(2.5f, 1.8f, -3.1f));     // 侧向偏心
    cameras.Add(FVector3(-7.0f, 0.3f, 0.9f));

    // ---- 阈值 ----
    //
    // 除了几个常规值, 还要放"恰好等于某个组的屏幕误差"的值 —— 那是 >= 与 >
    // 之别唯一显形的地方。
    TArray<Float32> thresholds;

    thresholds.Add(0.25f);
    thresholds.Add(1.0f);
    thresholds.Add(4.0f);
    thresholds.Add(16.0f);

    for (SizeType g = 0; g < dag.Groups.GetSize() && g < 6; ++g)
    {
        thresholds.Add(MeshLodProjectError(dag.Groups[g].Sphere,
                                           dag.Groups[g].Error, cameras[2],
                                           kLodScale, kNearPlane));
    }

    // ---- 判据一: 屏幕误差沿每条边严格增, 对每一个探针相机 ----
    {
        SizeType violations = 0;
        SizeType pairs      = 0;

        Float32 worstGap = 0.0f;

        for (SizeType c = 0; c < cameras.GetSize(); ++c)
        {
            for (SizeType l = 0; l < levelCount; ++l)
            {
                const FLodLevel& level = dag.Levels[l];

                for (SizeType m = 0; m < level.Records.GetSize(); ++m)
                {
                    const FLodMeshletRecord& record = level.Records[m];

                    if (record.TargetGroup == kLodInvalidIndex)
                    {
                        continue;
                    }

                    ++pairs;

                    const Float32 selfScreen = MeshLodProjectError(
                        record.SelfSphere, record.SelfError, cameras[c],
                        kLodScale, kNearPlane);

                    const Float32 parentScreen = MeshLodProjectError(
                        record.ParentSphere, record.ParentError, cameras[c],
                        kLodScale, kNearPlane);

                    if (!(parentScreen > selfScreen))
                    {
                        ++violations;

                        worstGap =
                            FMath::Max(worstGap, selfScreen - parentScreen);
                    }
                }
            }
        }

        LIMX_LOG(LogLaunch, Display,
                 "[LOD选择] 边判据 — {} 个 (相机, 父子对), 屏幕误差没有严格"
                 "递增的 {} 个 (最多倒挂 {})",
                 pairs, violations, worstGap);

        if (violations != 0)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[LOD选择] {} 个 (相机, 父子对) 上屏幕误差没有严格递增 —— "
                     "而'恰好命中一次'的证明第二步正是它。倒挂的那一段上父子"
                     "会同时被选中 (重画) 或同时落选 (洞)",
                     violations);
            passed = false;
        }
    }

    // ---- 预处理: 每个采样点在每一层被哪个 meshlet 罩着 ----
    //
    // 在原始表面上撒点 (每隔若干个三角形取一个重心), 逐层找离它最近的三角形,
    // 再查那个三角形属于哪个 meshlet。
    //
    // 撒点而不是逐三角形: 逐三角形是 O(三角形数 × 层数 × 每层三角形数),
    // 球体上是两亿次点-三角形距离。撒点把第一个因子压到几百, 而"每一点恰好
    // 命中一次"这个性质本身是逐点的 —— 采样只是少验了一些点, 不会把错的
    // 判成对的。
    const SizeType triangleCount = sphere.Indices.GetSize() / 3;

    const SizeType sampleStride = FMath::Max<SizeType>(1, triangleCount / 300);

    TArray<FVector3> samples;

    for (SizeType t = 0; t < triangleCount; t += sampleStride)
    {
        const FVector3& a = sphere.Vertices[sphere.Indices[t * 3 + 0]].Position;
        const FVector3& b = sphere.Vertices[sphere.Indices[t * 3 + 1]].Position;
        const FVector3& c = sphere.Vertices[sphere.Indices[t * 3 + 2]].Position;

        samples.Add((a + b + c) / 3.0f);
    }

    // owner[sample * levelCount + level] = 那一层罩着它的 meshlet 下标
    TArray<UInt32> owner;
    owner.SetSize(samples.GetSize() * levelCount, 0xFFFFFFFFu);

    for (SizeType l = 0; l < levelCount; ++l)
    {
        const FLodLevel& level = dag.Levels[l];

        // 三角形 -> meshlet
        TArray<UInt32> triangleOwner;
        TArray<UInt32> triangleCorners;

        for (SizeType m = 0; m < level.Meshlets.Meshlets.GetSize(); ++m)
        {
            const FMeshlet& meshlet = level.Meshlets.Meshlets[m];

            for (UInt32 t = 0; t < meshlet.TriangleCount; ++t)
            {
                for (UInt32 c = 0; c < 3; ++c)
                {
                    const SizeType byteOffset =
                        static_cast<SizeType>(meshlet.TriangleOffset) * 3 +
                        t * 3 + c;

                    const UInt32 local =
                        level.Meshlets.MeshletTriangles[byteOffset];

                    triangleCorners.Add(level.Meshlets.MeshletVertices[
                        static_cast<SizeType>(meshlet.VertexOffset) + local]);
                }

                triangleOwner.Add(static_cast<UInt32>(m));
            }
        }

        for (SizeType s = 0; s < samples.GetSize(); ++s)
        {
            Float32 nearest = 3.4e38f;
            UInt32  best    = 0xFFFFFFFFu;

            for (SizeType t = 0; t < triangleOwner.GetSize(); ++t)
            {
                const Float32 distance = PointTriangleDistanceSquared(
                    samples[s], level.Vertices[triangleCorners[t * 3 + 0]].Position,
                    level.Vertices[triangleCorners[t * 3 + 1]].Position,
                    level.Vertices[triangleCorners[t * 3 + 2]].Position);

                if (distance < nearest)
                {
                    nearest = distance;
                    best    = triangleOwner[t];
                }
            }

            owner[s * levelCount + l] = best;
        }
    }

    // ---- 判据二: 每个采样点恰好被一个层选中 ----
    {
        SizeType missCount   = 0;   // 一个都没选中 —— 洞
        SizeType doubleCount = 0;   // 选中两个以上 —— 重画
        SizeType checked     = 0;

        for (SizeType c = 0; c < cameras.GetSize(); ++c)
        {
            for (SizeType th = 0; th < thresholds.GetSize(); ++th)
            {
                for (SizeType s = 0; s < samples.GetSize(); ++s)
                {
                    UInt32 hits = 0;

                    for (SizeType l = 0; l < levelCount; ++l)
                    {
                        const UInt32 meshletIndex = owner[s * levelCount + l];

                        if (meshletIndex == 0xFFFFFFFFu)
                        {
                            continue;
                        }

                        if (MeshLodSelect(dag.Levels[l].Records[meshletIndex],
                                          cameras[c], kLodScale, kNearPlane,
                                          thresholds[th]))
                        {
                            ++hits;
                        }
                    }

                    ++checked;

                    if (hits == 0)
                    {
                        ++missCount;
                    }
                    else if (hits > 1)
                    {
                        ++doubleCount;
                    }
                }
            }
        }

        LIMX_LOG(LogLaunch, Display,
                 "[LOD选择] 覆盖 — 验了 {} 个 (相机, 阈值, 采样点), "
                 "一层都没选中 {} 个, 选中两层以上 {} 个",
                 checked, missCount, doubleCount);

        if (missCount != 0)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[LOD选择] {} 个采样点在某个 (相机, 阈值) 下一层都没被"
                     "选中 —— 那是画面上的洞",
                     missCount);
            passed = false;
        }

        if (doubleCount != 0)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[LOD选择] {} 个采样点被两层以上同时选中 —— 表面被画两遍, "
                     "Z 冲突",
                     doubleCount);
            passed = false;
        }
    }

    // ---- 判据三: 元判据 —— LOD 真的在换层 ----
    //
    // 没有这一条的话, 一个"永远选叶子层"的实现在上面两条上满分通过 ——
    // 它确实不重不漏。
    {
        const Float32 distances[6] = { 1.5f, 3.0f, 6.0f, 12.0f, 25.0f, 60.0f };

        TArray<SizeType> selectedTriangles;

        for (UInt32 d = 0; d < 6; ++d)
        {
            const FVector3 camera(0.0f, 0.0f, distances[d]);

            SizeType triangles = 0;

            for (SizeType l = 0; l < levelCount; ++l)
            {
                const FLodLevel& level = dag.Levels[l];

                for (SizeType m = 0; m < level.Records.GetSize(); ++m)
                {
                    if (MeshLodSelect(level.Records[m], camera, kLodScale,
                                      kNearPlane, 1.0f))
                    {
                        triangles += level.Meshlets.Meshlets[m].TriangleCount;
                    }
                }
            }

            selectedTriangles.Add(triangles);
        }

        LIMX_LOG(LogLaunch, Display,
                 "[LOD选择] 距离 1.5/3/6/12/25/60 上选中的三角形数: "
                 "{} / {} / {} / {} / {} / {}",
                 selectedTriangles[0], selectedTriangles[1],
                 selectedTriangles[2], selectedTriangles[3],
                 selectedTriangles[4], selectedTriangles[5]);

        for (SizeType d = 1; d < selectedTriangles.GetSize(); ++d)
        {
            if (selectedTriangles[d] > selectedTriangles[d - 1])
            {
                LIMX_LOG(LogLaunch, Error,
                         "[LOD选择] 相机拉远之后选中的三角形反而变多了 "
                         "({} -> {}) —— 屏幕误差应当随距离单调减",
                         selectedTriangles[d - 1], selectedTriangles[d]);
                passed = false;
            }
        }

        if (selectedTriangles[0] == selectedTriangles[5])
        {
            LIMX_LOG(LogLaunch, Error,
                     "[LOD选择] 最近与最远选中的三角形数一样 ({}) —— LOD 根本"
                     "没在换层。一个'永远选叶子层'的实现在覆盖判据上是满分的, "
                     "它确实不重不漏",
                     selectedTriangles[0]);
            passed = false;
        }

        // 最远处必须真的选到了粗的层 —— 不然"换层"只是换了几个 meshlet
        if (selectedTriangles[5] * 4 > selectedTriangles[0])
        {
            LIMX_LOG(LogLaunch, Error,
                     "[LOD选择] 拉到 60 倍距离才减到 {} / {} —— DAG 有 {} 层, "
                     "本该减到十分之一以下",
                     selectedTriangles[5], selectedTriangles[0], levelCount);
            passed = false;
        }
    }

    LIMX_LOG(LogLaunch, Display, "[LOD选择] {}", passed ? "通过" : "失败");

    return passed;
}

// ============================================================================
// RunLodCrackChecks — 选中集必须无裂缝, 而且必须兑现屏幕误差的承诺
//
// 两条判据, 各自钉住一件此前没人验过的事。
//
// ── 一、选中集是**闭合流形** ──
//
// 这是"无裂缝"最锋利的形式。选中集跨越多个层 —— 近处一块用叶子层、远处一块
// 用第四层 —— 而它们在交界处必须严丝合缝。缝在这里的表现极其直接: 一条只被
// **一个**三角形用到的边。
//
// 它比"逐像素看有没有洞"强得多: 后者依赖分辨率与视角, 一条亚像素的缝在
// 1280x720 上看不见, 换个分辨率就露出来。而边的计数与分辨率、与视角都无关。
//
// 这一条能立住, 全靠组间边界锁死: 第 L 层与第 L+1 层的几何在组边界上顶点
// 位置**逐位相同**。锁定漏了, 这里立刻变成一堆边界边。
//
// ── 二、屏幕误差的承诺必须兑现 ──
//
// 选择规则保证"自身误差 < 阈值", 而自身误差是**世界空间**的界。判据要问的是
// 那个界投到屏幕上之后是不是真的不超过阈值像素 —— 也就是说, 表面上每一点到
// **选中集**的实际距离, 投影之后必须 <= 阈值。
//
// 第四天有一条变异正是死在这里逃掉的: "距离用球心而不是球面最近点"。它让
// 分母偏大、屏幕误差偏小, 于是同样距离上选到更粗的层 —— 而单调性与割性质
// 都保住了, 那三条判据全绿。这一条把它钉死。
// ============================================================================

namespace
{

/// 一层里某个 meshlet 的三角形 (全局顶点下标)
void AppendMeshletTriangles(const FLodLevel& level, UInt32 meshletIndex,
                            TArray<UInt32>& outCorners)
{
    const FMeshlet& meshlet = level.Meshlets.Meshlets[meshletIndex];

    for (UInt32 t = 0; t < meshlet.TriangleCount; ++t)
    {
        for (UInt32 c = 0; c < 3; ++c)
        {
            const SizeType byteOffset =
                static_cast<SizeType>(meshlet.TriangleOffset) * 3 + t * 3 + c;

            const UInt32 local = level.Meshlets.MeshletTriangles[byteOffset];

            outCorners.Add(level.Meshlets.MeshletVertices[
                static_cast<SizeType>(meshlet.VertexOffset) + local]);
        }
    }
}

} // namespace

static bool RunLodCrackChecks()
{
    bool passed = true;

    const FMeshData sphere = FGeometryGenerator::GenerateSphere(1.0f, 64, 48);

    FMeshLodDagOptions dagOptions;
    dagOptions.TargetGroupSize = 16;
    dagOptions.MaxGroupSize    = 32;

    const FMeshLodDagResult dag =
        FMeshLodDagBuilder::Build(sphere.Vertices, sphere.Indices, dagOptions);

    if (!dag.IsValid() || dag.Levels.GetSize() < 3)
    {
        LIMX_LOG(LogLaunch, Error, "[LOD裂缝] DAG 建得不够深");
        return false;
    }

    const SizeType levelCount = dag.Levels.GetSize();

    constexpr Float32 kLodScale  = 540.0f;
    constexpr Float32 kNearPlane = 0.1f;

    // 相机摆位: 近、中、远、侧向偏心。近处与远处同时在画面里才会出现"选中集
    // 跨多层"的情形 —— 而那正是缝会出现的地方。
    // 贴得越近, 近处那一块与远处那一块的距离差越大, 选中集才会真的跨层。
    // 相机在 1.05 处 (半径 1): 最近点 0.05, 最远点 2.05 —— 差四十倍。
    // 摆在 20 处的话两端只差 10%, 整个物体必然同层, 而那时闭合判据验的只是
    // "某一层自己是闭合的", 与 LOD 选择无关。
    TArray<FVector3> cameras;
    cameras.Add(FVector3(0.0f, 0.0f, 1.005f));
    cameras.Add(FVector3(0.0f, 0.0f, 1.02f));
    cameras.Add(FVector3(0.0f, 0.0f, 1.05f));
    cameras.Add(FVector3(0.0f, 0.0f, 1.2f));
    cameras.Add(FVector3(0.0f, 0.0f, 1.6f));
    cameras.Add(FVector3(1.1f, 0.4f, 0.6f));
    cameras.Add(FVector3(0.0f, 0.0f, 4.0f));
    cameras.Add(FVector3(3.0f, 2.0f, 3.0f));

    TArray<Float32> thresholds;
    thresholds.Add(0.5f);
    thresholds.Add(1.0f);
    thresholds.Add(2.0f);
    thresholds.Add(4.0f);
    thresholds.Add(8.0f);
    thresholds.Add(24.0f);

    // ---- 采样点 (判据二用) ----
    const SizeType triangleCount = sphere.Indices.GetSize() / 3;
    const SizeType sampleStride  = FMath::Max<SizeType>(1, triangleCount / 240);

    TArray<FVector3> samples;

    for (SizeType t = 0; t < triangleCount; t += sampleStride)
    {
        const FVector3& a = sphere.Vertices[sphere.Indices[t * 3 + 0]].Position;
        const FVector3& b = sphere.Vertices[sphere.Indices[t * 3 + 1]].Position;
        const FVector3& c = sphere.Vertices[sphere.Indices[t * 3 + 2]].Position;

        samples.Add((a + b + c) / 3.0f);
    }

    SizeType crackTotal      = 0;
    SizeType promiseBroken   = 0;
    Float32  worstPromise    = 0.0f;
    SizeType overshootTotal  = 0;
    SizeType multiLevelCases = 0;
    SizeType casesChecked    = 0;

    Float32 worstScreenError = 0.0f;

    for (SizeType c = 0; c < cameras.GetSize(); ++c)
    {
        for (SizeType th = 0; th < thresholds.GetSize(); ++th)
        {
            // ---- 选中集 ----
            TArray<FVector3> positions;   // 选中集的顶点位置 (拼在一起)
            TArray<UInt32>   corners;     // 三角形的三个下标 (进 positions)

            TArray<UInt32> levelsUsed;
            levelsUsed.SetSize(levelCount, 0u);

            for (SizeType l = 0; l < levelCount; ++l)
            {
                const FLodLevel& level = dag.Levels[l];

                for (SizeType m = 0; m < level.Records.GetSize(); ++m)
                {
                    if (!MeshLodSelect(level.Records[m], cameras[c], kLodScale,
                                       kNearPlane, thresholds[th]))
                    {
                        continue;
                    }

                    levelsUsed[l] = 1;

                    // ---- 判据二之一: 逐 meshlet 的承诺 ----
                    //
                    // 选择规则保证 自身误差·lodScale/(球心距 - 半径) < 阈值。
                    // 而这个 meshlet 的几何**全部**在那个球里, 所以它到相机的
                    // 真实最近距离不小于 球心距 - 半径 —— 于是拿真实距离投出来
                    // 的屏幕误差只会更小, 必然也 < 阈值。这是可证的。
                    //
                    // 用真实最近距离而不是采样点的距离, 是因为承诺本来就是对
                    // meshlet 说的。第一版按采样点量: "距离用球心而不是球面最近
                    // 点"那条变异把最坏值从 0.414 推到 0.972 倍阈值 —— 量到了
                    // 劣化却没破约, 因为采样点最近的地方恰好选的是细层, 偏差近
                    // 于零。换成逐 meshlet 就没有这个稀释。
                    {
                        const FMeshlet& meshlet = level.Meshlets.Meshlets[m];

                        Float32 nearestVertex = 3.4e38f;

                        for (UInt32 v = 0; v < meshlet.VertexCount; ++v)
                        {
                            const UInt32 global =
                                level.Meshlets.MeshletVertices[
                                    static_cast<SizeType>(meshlet.VertexOffset) +
                                    v];

                            nearestVertex = FMath::Min(
                                nearestVertex,
                                (level.Vertices[global].Position - cameras[c])
                                    .Length());
                        }

                        const Float32 trueDistance =
                            FMath::Max(nearestVertex, kNearPlane);

                        const Float32 promised =
                            level.Records[m].SelfError * kLodScale /
                            trueDistance;

                        worstPromise =
                            FMath::Max(worstPromise, promised / thresholds[th]);

                        if (promised > thresholds[th])
                        {
                            ++promiseBroken;
                        }
                    }

                    TArray<UInt32> local;
                    AppendMeshletTriangles(level, static_cast<UInt32>(m), local);

                    const UInt32 base = static_cast<UInt32>(positions.GetSize());

                    // 这个 meshlet 用到的顶点原样搬过来
                    for (SizeType i = 0; i < local.GetSize(); ++i)
                    {
                        positions.Add(level.Vertices[local[i]].Position);
                        corners.Add(base + static_cast<UInt32>(i));
                    }
                }
            }

            if (corners.GetSize() < 3)
            {
                continue;
            }

            ++casesChecked;

            UInt32 usedCount = 0;

            for (SizeType l = 0; l < levelCount; ++l)
            {
                usedCount += levelsUsed[l];
            }

            if (usedCount >= 2)
            {
                ++multiLevelCases;
            }

            // ---- 判据一: 选中集必须是闭合流形 ----
            //
            // 按位置焊接 (带容差 —— UV 球的接缝上 sinf(2πf) = 1.7e-7 而不是 0,
            // 逐位焊接焊不上), 然后数每条无向边被几个三角形用到。
            {
                const Float32 tolerance = 1.0e-5f;
                const Float32 toleranceSquared = tolerance * tolerance;

                TArray<UInt32>   weld;
                TArray<FVector3> unique;

                // 空间分桶 —— 逐个线性找是 O(n²), 选中集有上万个顶点
                constexpr UInt32 kGrid = 64;

                TArray<TArray<UInt32>> buckets;
                buckets.SetSize(kGrid * kGrid * kGrid);

                const auto BucketOf = [](const FVector3& p) -> UInt32
                {
                    // 球体在 [-1.2, 1.2] 之内; 这个判据只在这个测试网格上跑
                    const auto Axis = [](Float32 v) -> UInt32
                    {
                        const Float32 t = (v + 1.5f) / 3.0f;

                        return static_cast<UInt32>(
                            FMath::Clamp(t * (kGrid - 1), 0.0f,
                                         static_cast<Float32>(kGrid - 1)));
                    };

                    return Axis(p.X) * kGrid * kGrid + Axis(p.Y) * kGrid +
                           Axis(p.Z);
                };

                for (SizeType i = 0; i < positions.GetSize(); ++i)
                {
                    const FVector3& p = positions[i];

                    const UInt32 slot = BucketOf(p);

                    UInt32 found = 0xFFFFFFFFu;

                    // 查本桶与相邻桶 —— 容差 1e-5 远小于桶宽 3/64
                    for (SizeType k = 0; k < buckets[slot].GetSize(); ++k)
                    {
                        if ((unique[buckets[slot][k]] - p).LengthSquared() <=
                            toleranceSquared)
                        {
                            found = buckets[slot][k];
                            break;
                        }
                    }

                    if (found == 0xFFFFFFFFu)
                    {
                        found = static_cast<UInt32>(unique.GetSize());
                        unique.Add(p);
                        buckets[slot].Add(found);
                    }

                    weld.Add(found);
                }

                // 每条无向边的计数
                TArray<TArray<UInt32>> edgeOther;
                TArray<TArray<UInt32>> edgeCount;

                edgeOther.SetSize(unique.GetSize());
                edgeCount.SetSize(unique.GetSize());

                for (SizeType t = 0; t + 2 < corners.GetSize(); t += 3)
                {
                    for (UInt32 e = 0; e < 3; ++e)
                    {
                        const UInt32 a = weld[corners[t + e]];
                        const UInt32 b = weld[corners[t + (e + 1) % 3]];

                        if (a == b)
                        {
                            continue;
                        }

                        const UInt32 low  = FMath::Min(a, b);
                        const UInt32 high = FMath::Max(a, b);

                        bool found = false;

                        for (SizeType k = 0; k < edgeOther[low].GetSize(); ++k)
                        {
                            if (edgeOther[low][k] == high)
                            {
                                ++edgeCount[low][k];
                                found = true;
                                break;
                            }
                        }

                        if (!found)
                        {
                            edgeOther[low].Add(high);
                            edgeCount[low].Add(1u);
                        }
                    }
                }

                SizeType cracks = 0;

                for (SizeType v = 0; v < edgeCount.GetSize(); ++v)
                {
                    for (SizeType k = 0; k < edgeCount[v].GetSize(); ++k)
                    {
                        if (edgeCount[v][k] != 2u)
                        {
                            ++cracks;
                        }
                    }
                }

                crackTotal += cracks;

                if (cracks != 0)
                {
                    LIMX_LOG(LogLaunch, Error,
                             "[LOD裂缝] 相机 {} 阈值 {} — 选中集里有 {} 条边"
                             "不是恰好两个三角形共用 (用了 {} 个层) —— 那是缝",
                             c, thresholds[th], cracks, usedCount);
                    passed = false;
                }
            }

            // ---- 判据二: 屏幕误差的承诺必须兑现 ----
            //
            // 表面上每一点到**选中集**的实际距离, 投影之后必须不超过阈值。
            {
                for (SizeType s = 0; s < samples.GetSize(); ++s)
                {
                    Float32 nearest = 3.4e38f;

                    for (SizeType t = 0; t + 2 < corners.GetSize(); t += 3)
                    {
                        nearest = FMath::Min(
                            nearest,
                            PointTriangleDistanceSquared(
                                samples[s], positions[corners[t + 0]],
                                positions[corners[t + 1]],
                                positions[corners[t + 2]]));

                        if (nearest <= 0.0f)
                        {
                            break;
                        }
                    }

                    const Float32 deviation = FMath::Sqrt(nearest);

                    const Float32 viewDistance = FMath::Max(
                        (samples[s] - cameras[c]).Length(), kNearPlane);

                    const Float32 screenError =
                        deviation * kLodScale / viewDistance;

                    worstScreenError =
                        FMath::Max(worstScreenError, screenError / thresholds[th]);

                    // **没有容差**。承诺就是"屏幕偏差不超过阈值像素", 而它是
                    // 可证的: 选择规则保证 自身误差·lodScale/(球心距-半径) < 阈值,
                    // 而采样点到相机的距离**不小于** 球心距-半径, 实际偏差又不
                    // 超过自身误差 —— 两个不等式串起来就是这一条。
                    //
                    // 正因为它可证, 这里不该留余量: 留了就等于承认那个证明哪一步
                    // 不成立, 而那才是要报出来的事。
                    if (screenError > thresholds[th])
                    {
                        ++overshootTotal;
                    }
                }
            }
        }
    }

    LIMX_LOG(LogLaunch, Display,
             "[LOD裂缝] 验了 {} 个 (相机, 阈值), 其中 {} 个用了两层以上; "
             "缝 {} 条; 逐 meshlet 破约 {} 次 (最坏 {} 倍阈值); "
             "逐采样点超预算 {} 次 (最坏 {} 倍)",
             casesChecked, multiLevelCases, crackTotal, promiseBroken,
             worstPromise, overshootTotal, worstScreenError);

    if (promiseBroken != 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[LOD裂缝] {} 个选中的 meshlet 破了屏幕误差的承诺 (最坏 {} 倍"
                 "阈值) —— 选择规则保证 自身误差·lodScale/(球心距-半径) < 阈值, "
                 "而 meshlet 的几何全在那个球里, 拿真实最近距离投出来只会更小。"
                 "破了就说明分母用错了东西",
                 promiseBroken, worstPromise);
        passed = false;
    }

    // ---- 元判据: 必须真的出现"选中集跨多层"的情形 ----
    //
    // 全部都只用一个层的话, 上面那条闭合判据验的只是"某一层自己是闭合的" ——
    // 那是 DAG 判据早就验过的事, 与 LOD 选择无关。缝只可能出现在层与层的
    // 交界处。
    // 门槛用**绝对数**而不是比例。
    //
    // 比例门槛对"加更多用例"是脆的: 第一版写的是 25%, 而后来为了把屏幕误差
    // 的余量吃掉又加了三个贴近的相机, 跨层数从 9/36 变成 11/48 —— 绝对数
    // 涨了, 比例反而掉到门槛下, 基线当场变红。而那时缝与超预算都是 0。
    //
    // 这条元判据真正要的是"跨层的情形足够多, 判据看得见交界处", 那是一个
    // 绝对量。五个是"每个探针相机至少摊上一个"的量级。
    if (multiLevelCases < 5)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[LOD裂缝] 只有 {} / {} 个 (相机, 阈值) 让选中集跨两个层 —— "
                 "缝只可能出现在层与层的交界处, 全是单层的话这条判据验的是"
                 "'某一层自己是闭合的', 而那是 DAG 判据早就验过的事",
                 multiLevelCases, casesChecked);
        passed = false;
    }

    if (overshootTotal != 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[LOD裂缝] {} 次采样的实际屏幕偏差超过了阈值 (最坏 {} 倍) —— "
                 "选择规则承诺的是'自身误差 < 阈值', 而那个误差是世界空间的界; "
                 "投到屏幕上超了就说明那个界或者它的投影哪一步不保守",
                 overshootTotal, worstScreenError);
        passed = false;
    }

    LIMX_LOG(LogLaunch, Display, "[LOD裂缝] {}", passed ? "通过" : "失败");

    return passed;
}

// ============================================================================
// RunLodGpuChecks — GLSL 与 C++ 的选择规则必须逐位一致
//
// 参与渲染的是着色器那一份, 而前面五天全部判据验的是 C++ 那一份。两者不一致
// 的话, 那五天的结论对画面**一句都不成立**。
//
// 为什么要逐位而不是"选中集合相同": 两份实现只要有一处运算顺序不同, 在阈值
// 附近就会给出不同的决策 —— 而那正好是最要紧的地方 (整套正确性建立在"恰好
// 落在链的某一段里")。集合比对在绝大多数相机下都相同, 只在边界上分家, 而
// 边界恰恰是缝出现的地方。
//
// 所以连中间量 (自身屏幕误差、父屏幕误差) 一起比, 而且要求**逐位相同**,
// 不留容差: 两边算的是同一个表达式、同一批输入, 差一个 ULP 都说明哪里不同。
//
// 阈值集合里放"恰好等于某条记录的自身/父屏幕误差"的值 —— 那是 `<` 与 `<=`、
// `>=` 与 `>` 之别唯一显形的地方。
// ============================================================================

namespace
{

/// 上传给着色器的 LOD 记录 —— 与 meshlet_lod.h 里的 MeshletLod 逐字节对齐
struct FLodRecordGpu
{
    Float32 SelfSphere[4];
    Float32 ParentSphere[4];
    Float32 SelfError;
    Float32 ParentError;
    UInt32  SourceGroup;
    UInt32  TargetGroup;
};

/// 探针结果
struct FLodProbeResult
{
    Float32 SelfScreen;
    Float32 ParentScreen;
    UInt32  Selected;
    UInt32  Padding;
};

static_assert(sizeof(FLodRecordGpu) == 48,
              "LOD 记录必须与 meshlet_lod.h 里的 MeshletLod 一致");

static_assert(sizeof(FLodProbeResult) == 16,
              "探针结果必须与 lod_probe.comp 里的 LodProbeResult 一致");

} // namespace

static bool RunLodGpuChecks(FRenderContext* context, FRenderer& renderer)
{
    LIMX_UNUSED(renderer);

    bool passed = true;

    // ---- 建一棵 DAG, 把它的记录摊平 ----
    const FMeshData sphere = FGeometryGenerator::GenerateSphere(1.0f, 64, 48);

    FMeshLodDagOptions dagOptions;
    dagOptions.TargetGroupSize = 16;
    dagOptions.MaxGroupSize    = 32;

    const FMeshLodDagResult dag =
        FMeshLodDagBuilder::Build(sphere.Vertices, sphere.Indices, dagOptions);

    if (!dag.IsValid() || dag.Levels.GetSize() < 3)
    {
        LIMX_LOG(LogLaunch, Error, "[LOD·GPU] DAG 建得不够深");
        return false;
    }

    TArray<FLodMeshletRecord> cpuRecords;
    TArray<FLodRecordGpu>     gpuRecords;

    for (SizeType l = 0; l < dag.Levels.GetSize(); ++l)
    {
        const FLodLevel& level = dag.Levels[l];

        for (SizeType m = 0; m < level.Records.GetSize(); ++m)
        {
            const FLodMeshletRecord& record = level.Records[m];

            cpuRecords.Add(record);

            FLodRecordGpu gpu;

            gpu.SelfSphere[0] = record.SelfSphere.X;
            gpu.SelfSphere[1] = record.SelfSphere.Y;
            gpu.SelfSphere[2] = record.SelfSphere.Z;
            gpu.SelfSphere[3] = record.SelfSphere.W;

            gpu.ParentSphere[0] = record.ParentSphere.X;
            gpu.ParentSphere[1] = record.ParentSphere.Y;
            gpu.ParentSphere[2] = record.ParentSphere.Z;
            gpu.ParentSphere[3] = record.ParentSphere.W;

            gpu.SelfError   = record.SelfError;
            gpu.ParentError = record.ParentError;
            gpu.SourceGroup = record.SourceGroup;
            gpu.TargetGroup = record.TargetGroup;

            gpuRecords.Add(gpu);
        }
    }

    const UInt32 recordCount = static_cast<UInt32>(gpuRecords.GetSize());

    LIMX_LOG(LogLaunch, Display, "[LOD·GPU] DAG {} 层, 记录 {} 条",
             dag.Levels.GetSize(), recordCount);

    // ---- 相机与阈值 ----
    constexpr Float32 kLodScale  = 540.0f;
    constexpr Float32 kNearPlane = 0.1f;

    TArray<FVector3> cameras;
    cameras.Add(FVector3(0.0f, 0.0f, 0.0f));      // 在每一个 LOD 球内
    cameras.Add(FVector3(0.0f, 0.0f, 1.02f));     // 贴着表面
    cameras.Add(FVector3(0.0f, 0.0f, 3.0f));
    cameras.Add(FVector3(2.5f, 1.8f, -3.1f));     // 侧向偏心
    cameras.Add(FVector3(0.0f, 0.0f, 40.0f));     // 极远

    TArray<Float32> thresholds;
    thresholds.Add(0.25f);
    thresholds.Add(1.0f);
    thresholds.Add(4.0f);
    thresholds.Add(16.0f);

    // 恰好等于某条记录屏幕误差的阈值 —— `<` 与 `<=` 之别唯一显形的地方
    for (SizeType i = 0; i < cpuRecords.GetSize() && i < 8; ++i)
    {
        thresholds.Add(MeshLodProjectError(cpuRecords[i].SelfSphere,
                                           cpuRecords[i].SelfError, cameras[2],
                                           kLodScale, kNearPlane));

        thresholds.Add(MeshLodProjectError(cpuRecords[i].ParentSphere,
                                           cpuRecords[i].ParentError,
                                           cameras[2], kLodScale, kNearPlane));
    }

    // ---- GPU 资源 ----
    IRHIDevice* const device = context->GetDevice();

    FRHIShaderHandle          shader;
    FRHIDescSetLayoutHandle   setLayout;
    FRHIPipelineLayoutHandle  pipelineLayout;
    FRHIComputePipelineHandle pipeline;
    FRHIDescriptorSetHandle   descriptorSet;
    FRHIBufferHandle          recordBuffer;
    FRHIBufferHandle          resultBuffer;

    bool ok = true;

    {
        FShaderManager& shaders = FShaderManager::Get();

        if (!shaders.IsInitialized())
        {
            shaders.Initialize();
        }

        ok = IsRHISuccess(shaders.CreateShaderModule(
            device, FString("Builtin/lod_probe.comp"), EShaderStage::Compute,
            shader));
    }

    if (ok)
    {
        FRHIDescriptorBinding bindings[2] = {};

        for (UInt32 i = 0; i < 2; ++i)
        {
            bindings[i].Binding    = i;
            bindings[i].Type       = EDescriptorType::StorageBuffer;
            bindings[i].Count      = 1;
            bindings[i].StageFlags = EShaderStage::Compute;
        }

        FRHIDescSetLayoutDesc layoutDesc = {};
        layoutDesc.Bindings     = bindings;
        layoutDesc.BindingCount = 2;
        layoutDesc.DebugName    = "LodProbeSetLayout";

        ok = IsRHISuccess(device->CreateDescSetLayout(layoutDesc, setLayout));
    }

    if (ok)
    {
        FRHIPushConstantRange pushRange = {};
        pushRange.StageFlags = EShaderStage::Compute;
        pushRange.Offset     = 0;
        pushRange.Size       = sizeof(Float32) * 8;

        FRHIPipelineLayoutDesc layoutDesc = {};
        layoutDesc.SetLayouts             = &setLayout;
        layoutDesc.SetLayoutCount         = 1;
        layoutDesc.PushConstantRanges     = &pushRange;
        layoutDesc.PushConstantRangeCount = 1;
        layoutDesc.DebugName              = "LodProbeLayout";

        ok = IsRHISuccess(
            device->CreatePipelineLayout(layoutDesc, pipelineLayout));
    }

    if (ok)
    {
        FRHIComputePipelineDesc pipelineDesc = {};
        pipelineDesc.ComputeShader.Shader     = shader;
        pipelineDesc.ComputeShader.Stage      = EShaderStage::Compute;
        pipelineDesc.ComputeShader.EntryPoint = "main";
        pipelineDesc.PipelineLayout           = pipelineLayout;
        pipelineDesc.DebugName                = "LodProbePipeline";

        ok = IsRHISuccess(device->CreateComputePipeline(pipelineDesc, pipeline));
    }

    if (ok)
    {
        FRHIBufferDesc bufferDesc = {};
        bufferDesc.Size = static_cast<UInt64>(recordCount) *
                          sizeof(FLodRecordGpu);
        bufferDesc.Usage       = EBufferUsage::StorageBuffer;
        bufferDesc.MemoryUsage = EMemoryUsage::CpuToGpu;
        bufferDesc.DebugName   = "LodProbeRecords";

        ok = IsRHISuccess(device->CreateBuffer(bufferDesc, recordBuffer));

        bufferDesc.Size = static_cast<UInt64>(recordCount) *
                          sizeof(FLodProbeResult);
        bufferDesc.MemoryUsage = EMemoryUsage::GpuToCpu;
        bufferDesc.DebugName   = "LodProbeResults";

        ok = ok && IsRHISuccess(device->CreateBuffer(bufferDesc, resultBuffer));
    }

    if (ok)
    {
        ok = IsRHISuccess(
            device->AllocateDescriptorSet(setLayout, descriptorSet));
    }

    if (ok)
    {
        void* mapped = nullptr;

        if (IsRHISuccess(device->MapBuffer(recordBuffer, &mapped)) &&
            mapped != nullptr)
        {
            Memory::MemCopy(mapped, gpuRecords.GetData(),
                            static_cast<SizeType>(recordCount) *
                                sizeof(FLodRecordGpu));

            device->UnmapBuffer(recordBuffer);
        }
        else
        {
            ok = false;
        }
    }

    if (ok)
    {
        FRHIDescriptorWrite writes[2];

        writes[0] = FRHIDescriptorWrite::StorageBuffer(
            descriptorSet, 0, recordBuffer, 0,
            static_cast<UInt64>(recordCount) * sizeof(FLodRecordGpu));

        writes[1] = FRHIDescriptorWrite::StorageBuffer(
            descriptorSet, 1, resultBuffer, 0,
            static_cast<UInt64>(recordCount) * sizeof(FLodProbeResult));

        device->UpdateDescriptorSets(writes, 2);
    }

    // ---- 逐 (相机, 阈值) 跑一遍, 逐位比 ----
    SizeType casesRun          = 0;
    SizeType selectionDiffer   = 0;
    SizeType selfScreenDiffer  = 0;
    SizeType parentScreenDiffer = 0;
    SizeType selectedTotal     = 0;

    Float32 worstSelfUlp        = 0.0f;
    Float32 worstParentRelative = 0.0f;

    // 相对差的预算 —— 挂在建 DAG 时那条增长地板上, 见下面比对处那段说明
    constexpr Float32 kLodGpuRelativeBudget = (1.0f / 1024.0f) / 100.0f;

    if (ok)
    {
        for (SizeType c = 0; c < cameras.GetSize() && ok; ++c)
        {
            for (SizeType th = 0; th < thresholds.GetSize() && ok; ++th)
            {
                const FVector3 camera    = cameras[c];
                const Float32  threshold = thresholds[th];

                bool recorded = false;

                renderer.SetPostSceneRenderCallback(
                    [&recorded, context, pipeline, pipelineLayout,
                     descriptorSet, resultBuffer, recordCount, camera,
                     threshold]()
                    {
                        IRHICommandBuffer* cmd =
                            context->GetCurrentCommandBuffer();

                        if (cmd == nullptr)
                        {
                            return;
                        }

                        cmd->BindComputePipeline(pipeline);
                        cmd->BindDescriptorSet(EPipelineBindPoint::Compute,
                                               pipelineLayout, 0,
                                               descriptorSet);

                        Float32 push[8] = {};

                        push[0] = camera.X;
                        push[1] = camera.Y;
                        push[2] = camera.Z;
                        push[3] = kLodScale;
                        push[4] = kNearPlane;
                        push[5] = threshold;
                        push[6] = static_cast<Float32>(recordCount);
                        push[7] = 0.0f;

                        cmd->PushConstants(pipelineLayout,
                                           EShaderStage::Compute, 0,
                                           sizeof(push), push);

                        cmd->Dispatch((recordCount + 63u) / 64u, 1, 1);

                        FRHIBufferMemoryBarrier barrier = {};
                        barrier.SrcAccessMask = EAccessFlags::ShaderWrite;
                        barrier.DstAccessMask = EAccessFlags::HostRead;
                        barrier.Buffer        = resultBuffer;

                        cmd->PipelineBarrier(EPipelineStageFlags::ComputeShader,
                                             EPipelineStageFlags::Host, nullptr,
                                             0, &barrier, 1, nullptr, 0);

                        recorded = true;
                    });

                renderer.RenderFrame();
                renderer.SetPostSceneRenderCallback(TFunction<void()>());

                if (!recorded)
                {
                    ok = false;
                    break;
                }

                device->WaitIdle();

                void* mapped = nullptr;

                if (!IsRHISuccess(device->MapBuffer(resultBuffer, &mapped)) ||
                    mapped == nullptr)
                {
                    ok = false;
                    break;
                }

                const auto* results =
                    static_cast<const FLodProbeResult*>(mapped);

                ++casesRun;

                for (UInt32 i = 0; i < recordCount; ++i)
                {
                    const Float32 cpuSelf = MeshLodProjectError(
                        cpuRecords[i].SelfSphere, cpuRecords[i].SelfError,
                        camera, kLodScale, kNearPlane);

                    const Float32 cpuParent = MeshLodProjectError(
                        cpuRecords[i].ParentSphere, cpuRecords[i].ParentError,
                        camera, kLodScale, kNearPlane);

                    const bool cpuSelected = MeshLodSelect(
                        cpuRecords[i], camera, kLodScale, kNearPlane,
                        threshold);

                    if (cpuSelected)
                    {
                        ++selectedTotal;
                    }

                    if ((results[i].Selected != 0u) != cpuSelected)
                    {
                        ++selectionDiffer;
                    }

                    // 中间量比的是**相对**差, 而且预算挂在 DAG 自己的
                    // 不变量上。
                    //
                    // 第一版要求逐位相同, 当场红了: 自身 1480 次、父 3240 次
                    // 不同, 最大绝对差 2.13e-4。而那不是缺陷 —— **Vulkan 不
                    // 保证与 CPU 逐位相同**, 规范允许 sqrt 有 3 ULP 误差,
                    // 除法 2.5 ULP。要求逐位就是要求平台没承诺的东西。
                    //
                    // 真正要保住的是什么: 选择规则的正确性只依赖"屏幕误差沿
                    // DAG 的每条边**严格增**"。而建 DAG 时的增长地板是
                    // 2^-10 (千分之一) 的**相对**增长 —— 只要两份实现的相对
                    // 差远小于它, 那个严格序在 GPU 上照样成立。
                    //
                    // 所以预算取地板的百分之一: 2^-10 / 100 ≈ 1e-5。它不是
                    // 拍脑袋的容差, 是"离破坏不变量还差两个数量级"。
                    const Float32 selfScale =
                        FMath::Max(FMath::Abs(cpuSelf), 1.0e-20f);

                    const Float32 selfRelative =
                        FMath::Abs(results[i].SelfScreen - cpuSelf) / selfScale;

                    if (selfRelative > kLodGpuRelativeBudget)
                    {
                        ++selfScreenDiffer;
                    }

                    worstSelfUlp = FMath::Max(worstSelfUlp, selfRelative);

                    const Float32 parentScale =
                        FMath::Max(FMath::Abs(cpuParent), 1.0e-20f);

                    const Float32 parentRelative =
                        FMath::Abs(results[i].ParentScreen - cpuParent) /
                        parentScale;

                    if (parentRelative > kLodGpuRelativeBudget)
                    {
                        ++parentScreenDiffer;
                    }

                    worstParentRelative =
                        FMath::Max(worstParentRelative, parentRelative);
                }

                device->UnmapBuffer(resultBuffer);
            }
        }
    }

    if (!ok)
    {
        LIMX_LOG(LogLaunch, Error, "[LOD·GPU] 资源创建或回读失败");
        passed = false;
    }

    LIMX_LOG(LogLaunch, Display,
             "[LOD·GPU] 跑了 {} 个 (相机, 阈值) x {} 条记录; "
             "选中判定不同 {} 次; 中间量相对差超预算 {} 次 (自身) / "
             "{} 次 (父); 实测最大相对差是预算的 {} / {} 倍; 累计选中 {} 次",
             casesRun, recordCount, selectionDiffer, selfScreenDiffer,
             parentScreenDiffer, worstSelfUlp / kLodGpuRelativeBudget,
             worstParentRelative / kLodGpuRelativeBudget, selectedTotal);

    // 元判据: 两份实现**确实**在算, 而且确实有微小差异。
    //
    // 全为零的话有两种可能: 两边真的逐位相同 (在 Vulkan 上不该指望), 或者
    // 探针根本没跑、回读的是一片零。后者会让整条判据变成摆设, 而它看起来
    // 一切正常 —— 这个项目已经栽过一次"恒为零的诊断量"。
    //
    // 第一版把最大相对差直接印出来, 显示的是 "0 / 0" —— 因为实际值约 5e-8,
    // 被格式化吃掉了。换成"预算的多少倍"之后才看得见它非零。
    if (worstSelfUlp == 0.0f && worstParentRelative == 0.0f)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[LOD·GPU] 两份实现的中间量**完全**没有差异 —— Vulkan 不保证"
                 "与 CPU 逐位相同 (sqrt 允许 3 ULP), 所以这更像是探针没跑起来"
                 "或者回读的是一片零");
        passed = false;
    }

    if (selectionDiffer != 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[LOD·GPU] {} 次选中判定与 CPU 参考不同 —— 参与渲染的是着色器"
                 "那一份, 而前面五天的判据验的全是 C++ 那一份。两者不一致的话"
                 "那些结论对画面一句都不成立",
                 selectionDiffer);
        passed = false;
    }

    if (selfScreenDiffer != 0 || parentScreenDiffer != 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[LOD·GPU] 中间量的相对差超过预算 (自身 {} 次, 父 {} 次, "
                 "实测最大 {} / {}, 预算 {}) —— 预算是建 DAG 时那条增长地板"
                 "(2^-10) 的百分之一。超过它就意味着'屏幕误差沿边严格增'这条"
                 "在 GPU 上可能不成立, 而选择规则的全部正确性都建立在它上面",
                 selfScreenDiffer, parentScreenDiffer, worstSelfUlp,
                 worstParentRelative, kLodGpuRelativeBudget);
        passed = false;
    }

    // 元判据: 必须真的有选中与不选中两种结果
    if (selectedTotal == 0 ||
        selectedTotal >= casesRun * static_cast<SizeType>(recordCount))
    {
        LIMX_LOG(LogLaunch, Error,
                 "[LOD·GPU] 累计选中 {} / {} —— 全选或全不选, 逐位比对没有"
                 "验到判定分支",
                 selectedTotal, casesRun * static_cast<SizeType>(recordCount));
        passed = false;
    }

    device->DestroyBuffer(resultBuffer);
    device->DestroyBuffer(recordBuffer);
    device->DestroyComputePipeline(pipeline);
    device->DestroyPipelineLayout(pipelineLayout);
    device->DestroyDescSetLayout(setLayout);
    device->DestroyShader(shader);

    LIMX_LOG(LogLaunch, Display, "[LOD·GPU] {}", passed ? "通过" : "失败");

    return passed;
}

// ============================================================================
// RunMeshletChecks — meshlet 切分必须无损
//
// 这条判据不看画面, 不依赖场景, 也不需要 GPU。它问的只有一件事:
// **展开全部 meshlet 得到的三角形集合, 与原始索引数组是不是同一个集合。**
//
// 为什么这件事值得一条独立的判据: 下游 (剔除、可见性缓冲、材质解析) 全都
// 假定 meshlet 就是原网格的一个划分。少一个三角形表现为"模型上有个洞",
// 多一个表现为 Z 冲突 —— 而两者都可能只在某个视角下才看得见, 于是"画面
// 对不对"这条路上的判据抓不住它。
//
// "同一个集合"取的是**多重集**加**绕序**: 同一个三角形出现两次不等于出现
// 一次; (a,b,c) 与 (a,c,b) 是两个不同的三角形 (法线相反)。只允许旋转
// ——(a,b,c)、(b,c,a)、(c,a,b) 是同一个三角形, 因为绕序没变。
//
// 六条判据:
//   1. 三角形多重集完全相同 (含绕序)
//   2. 每个 meshlet 的顶点数与三角形数都在上限之内, 且都大于零
//   3. 每个局部索引都小于该 meshlet 的顶点数
//   4. 每个局部顶点的全局下标都在顶点数组之内
//   5. 包围球真的包住该 meshlet 的每一个顶点
//   6. 法线锥真的包住该 meshlet 的每一个三角形法线
//
// 外加两条**质量**判据。正确但没用的切分是存在的 —— 一个三角形一个
// meshlet 满足上面六条的每一条, 而它把顶点数据放大三倍、把剔除粒度缩到
// 没有意义:
//   7. 平均每个 meshlet 的三角形数不能太低
//   8. 顶点复用率不能太低 (= 聚类没有把相邻的三角形放到一起)
// ============================================================================
// RunMeshletScaleChecks — 规模上的判据
//
// 虚拟几何的卖点是"规模上去了也不塌", 而"不塌"有两层意思:
//
//   一、画面还是对的。这一条由 --meshlet-depth-check 在压力场景上验 ——
//      第十四天正是它量出法线锥存错了 (存的是半角余弦而剔除要半角正弦),
//      而综合场景上所有判据都是绿的。
//
//   二、**装不下的时候要响**。可见表与待定表都是定容的, 着色器里超出容量
//      的那几条直接丢掉。丢掉本身没得选 (总不能越界写), 问题在于第一版
//      **一个字都不说** —— 画面上少一块, 日志里干干净净, 而且只在场景大到
//      一定程度才出现。这是最坏的一种失败。
//
// 这条判据管第二层。它自己造条件: 靠场景规模是走不到溢出的 (可见表按
// 262144 条开的), 所以判据把容量压到一个很小的数, 逼着那条路径走一遍。
//
// 三段:
//
//   正常容量   -> 不许报溢出
//   压小容量   -> 必须报溢出, 而且**写进去的那部分仍然是好的**
//   恢复容量   -> 又不许报溢出 (标志不是粘住的)
//
// 第三段要紧: 一个"一旦置位就再也不清"的标志在前两段上满分通过, 而它会让
// 之后每一帧都报溢出。
// ============================================================================
// RunGpuCullOverflowChecks — 逐物体缓冲区装不下时不许索引到界外
//
// 逐物体缓冲区 (模型矩阵 + 材质下标) 按 kMaxGpuDrawObjects 定容, 分三段:
// 相机 / 投射体 / 半透明。场景大到三段合计超过容量时, 后面的段会被截断。
//
// 截断本身没得选。要命的是**绘制那一侧照着列表长度走**: 各个 Pass 逐物体
// 绘制时把列表下标当 firstInstance 传进去, 而着色器拿 gl_InstanceIndex
// 直接索引那个缓冲区。列表比写进去的条目长时, 后面那些物体索引到的是
// 缓冲区之外。
//
// 后果不是"画面上少一块"。读出来的"材质下标"是垃圾, 而它下一步要去索引
// bindless 材质表 —— GPU 读非法地址, **设备丢失**。
//
// 实测: 16130 个物体 (grid 127) 必然复现, 驱动报 READ_INVALID, 而那个地址
// 不属于任何一个活着的缓冲区; 15877 个 (grid 126) 十次全过。差别只在截断
// 有没有发生在投射体那一段。
//
// 这条判据三件事:
//
//   一、场景必须**真的超容量** —— 不超的话这条判据什么都没验到。
//   二、三段写进去的条目数必须自洽 (各段不超过自己的列表长度, 合计不超
//      过容量)。
//   三、跑完不许丢设备、不许有 Error。
//
// 第三条看着松, 其实是最硬的: 这个缺陷的表现就是丢设备, 而丢设备之后
// 引擎的每一帧都在报错。
// ============================================================================

static bool RunGpuCullOverflowChecks(FRenderContext* context,
                                     FRenderer&      renderer)
{
    LIMX_UNUSED(context);

    FGpuCullPass* const cull = renderer.GetGpuCullPass();

    if (cull == nullptr)
    {
        LIMX_LOG(LogLaunch, Error, "[GPU剔除溢出] 通道不存在");
        return false;
    }

    // 先跑几帧让三段都上传过
    for (UInt32 i = 0; i < 6; ++i)
    {
        renderer.RenderFrame();
    }

    const SizeType cameraSize = renderer.GetRenderObjects().GetSize();
    const SizeType casterSize = renderer.GetShadowCasterObjects().GetSize();
    const SizeType translucentSize =
        renderer.GetTranslucentObjects().GetSize();

    const SizeType requested = cameraSize + casterSize + translucentSize;

    const UInt32 cameraWritten      = cull->GetCameraCount();
    const UInt32 casterWritten      = cull->GetObjectCount();
    const UInt32 translucentWritten = cull->GetTranslucentCount();

    const UInt32 totalWritten =
        cameraWritten + casterWritten + translucentWritten;

    bool passed = true;

    LIMX_LOG(LogLaunch, Display,
             "[GPU剔除溢出] 列表 相机 {} / 投射体 {} / 半透明 {} = {} 个; "
             "写进去 {} / {} / {} = {} 个 (容量 {})",
             cameraSize, casterSize, translucentSize, requested,
             cameraWritten, casterWritten, translucentWritten, totalWritten,
             kMaxGpuDrawObjects);

    // ---- 一、必须真的超容量 ----
    if (requested <= kMaxGpuDrawObjects)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[GPU剔除溢出] 三段合计 {} 个没超过容量 {} —— 截断那条路径"
                 "根本没走到, 这条判据什么都没验。场景要更大",
                 requested, kMaxGpuDrawObjects);
        passed = false;
    }

    // ---- 二、写进去的条目数要自洽 ----
    if (totalWritten > kMaxGpuDrawObjects)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[GPU剔除溢出] 三段写进去 {} 个, 超过容量 {} —— 越界写",
                 totalWritten, kMaxGpuDrawObjects);
        passed = false;
    }

    if (cameraWritten > cameraSize || casterWritten > casterSize ||
        translucentWritten > translucentSize)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[GPU剔除溢出] 某一段写进去的条目比它的列表还长 "
                 "({}>{} / {}>{} / {}>{})",
                 cameraWritten, cameraSize, casterWritten, casterSize,
                 translucentWritten, translucentSize);
        passed = false;
    }

    // 截断必须**确实发生**在某一段上, 否则下面那条"跑完不丢设备"是平凡的
    if (totalWritten == requested)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[GPU剔除溢出] 三段一个都没被截断 —— 那条路径没走到");
        passed = false;
    }

    // ---- 三、钳位必须**留下痕迹** ----
    //
    // "没钳位会怎样"是不可靠的判据: 索引越界读到的内存是不是已映射, 取决于
    // 当时的堆布局。实测把钳位去掉之后同一个场景有时丢设备、有时安然无恙 ——
    // 拿"会不会丢设备"当判据等于让判据去赌运气。
    //
    // 跳过的绘制数与堆布局无关: 它必须等于三段各自"列表长度减去写进去的
    // 条目数"之和。
    const UInt32 skipped   = cull->GetSkippedDraws();
    const UInt32 maxIssued = cull->GetMaxIssuedIndex();

    LIMX_LOG(LogLaunch, Display,
             "[GPU剔除溢出] 发出去的最大逐物体下标 {} (写进去 {} 条); "
             "因为没有条目而跳过 {} 个绘制",
             maxIssued, totalWritten, skipped);

    if (!cull->HasIssuedDraw())
    {
        LIMX_LOG(LogLaunch, Error,
                 "[GPU剔除溢出] 一次逐物体绘制都没发出去 —— 判据没验到东西");
        passed = false;
    }
    else if (maxIssued >= totalWritten)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[GPU剔除溢出] 发出去的最大逐物体下标是 {}, 而只写进去 {} "
                 "条 —— 那次绘制拿越界的下标去索引逐物体缓冲区, 读出来的"
                 "材质下标是垃圾, 下一步要去索引 bindless 表 (后果是 GPU "
                 "读非法地址、设备丢失)",
                 maxIssued, totalWritten);
        passed = false;
    }

    if (skipped == 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[GPU剔除溢出] 一个绘制都没被跳过 —— 截断确实发生了 "
                 "(合计 {} > 写进去 {}), 那些没有条目的物体却全都画出去了",
                 requested, totalWritten);
        passed = false;
    }

    // ---- 四、继续跑, 不许丢设备 ----
    //
    // 缺陷复现时是在第三帧丢的, 这里跑十二帧留足余量。丢设备之后引擎每帧
    // 都会报 Error, 而 FinalizeSelfCheck 会把它们算进去。
    for (UInt32 i = 0; i < 12; ++i)
    {
        renderer.RenderFrame();
    }

    LIMX_LOG(LogLaunch, Display, "[GPU剔除溢出] {}",
             passed ? "通过" : "失败");

    return passed;
}

// ============================================================================

static bool RunMeshletScaleChecks(FRenderContext* context, FRenderer& renderer)
{
    if (!renderer.SetMeshletDepthEnabled(true))
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Meshlet规模] 无法启用光栅化 — 判据无法执行, 判定为失败");
        return false;
    }

    FMeshletCullPass* const cull = renderer.GetMeshletCullPass();

    if (cull == nullptr)
    {
        LIMX_LOG(LogLaunch, Error, "[Meshlet规模] 剔除通道不存在");
        return false;
    }

    bool passed = true;

    // 统计的回读隔着并行帧数, 所以每段都要多走几帧才读得到那一段的数
    const auto Settle = [&renderer]()
    {
        for (UInt32 i = 0; i < 4; ++i)
        {
            renderer.RenderFrame();
        }
    };

    // ---- 第一段: 正常容量 ----
    cull->SetVisibleCapacityOverride(0);

    Settle();

    const FMeshletCullStats normal = cull->GetStats();

    LIMX_LOG(LogLaunch, Display,
             "[Meshlet规模] 正常容量 — 实例 {}/{}, meshlet 测试 {} 可见 {} "
             "(容量 {})",
             normal.InstancesVisible, normal.InstancesTotal,
             normal.MeshletsTested, normal.MeshletsVisible,
             normal.VisibleCapacity);

    if (normal.MeshletsVisible == 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Meshlet规模] 一个 meshlet 都不可见 —— 判据没验到东西");
        passed = false;
    }

    // 两级之间要自洽。
    //
    // 这几条是被一个**恒为零的字段**逼出来的: InstancesVisible 从来没被
    // 赋过值, 而没有人看它, 于是它安安静静地报了不知道多少次"第一级把
    // 所有实例都剔光了"。补上赋值之后顺手给它一条判据 —— 否则下次它再
    // 变回 0 也一样没人发现。
    if (normal.InstancesVisible == 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Meshlet规模] 第一级压实出来 0 个实例, 而第二级却测了 "
                 "{} 个 meshlet —— 这两个数不可能同时成立",
                 normal.MeshletsTested);
        passed = false;
    }

    if (normal.InstancesVisible > normal.InstancesTotal)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Meshlet规模] 可见实例 {} 多于总实例 {}",
                 normal.InstancesVisible, normal.InstancesTotal);
        passed = false;
    }

    if (normal.MeshletsVisible > normal.MeshletsTested)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Meshlet规模] 可见 meshlet {} 多于测试过的 {}",
                 normal.MeshletsVisible, normal.MeshletsTested);
        passed = false;
    }

    if (normal.HasOverflow())
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Meshlet规模] 正常容量下就报了溢出 (可见 {} > 容量 {}) —— "
                 "要么场景真的超了, 要么溢出判断本身写反了",
                 normal.VisibleRequested, normal.VisibleCapacity);
        passed = false;
    }

    // ---- 第二段: 把容量压到装不下 ----
    //
    // 压到可见数的四分之一, 至少留 4 条 —— 留一点是为了让"写进去的那部分
    // 仍然是好的"这一条有东西可验。压到 0 的话那一条平凡成立。
    const UInt32 squeezed =
        FMath::Max(4u, normal.MeshletsVisible / 4u);

    cull->SetVisibleCapacityOverride(squeezed);

    Settle();

    const FMeshletCullStats overflowed = cull->GetStats();

    LIMX_LOG(LogLaunch, Display,
             "[Meshlet规模] 压到 {} 条 — 要写 {} 条, 待定要写 {} 条",
             overflowed.VisibleCapacity, overflowed.VisibleRequested,
             overflowed.PendingRequested);

    if (!overflowed.HasOverflow())
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Meshlet规模] 容量压到 {} 条还报不出溢出 (要写 {} 条) —— "
                 "超出容量的那些被静默丢掉了, 画面上少一块而日志里一个字"
                 "都没有",
                 overflowed.VisibleCapacity, overflowed.VisibleRequested);
        passed = false;
    }

    if (overflowed.VisibleRequested <= overflowed.VisibleCapacity)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Meshlet规模] 压小容量之后要写的条数也跟着变小了 "
                 "({} <= {}) —— 计数器被容量夹住了, 那样就再也看不出丢过"
                 "东西",
                 overflowed.VisibleRequested, overflowed.VisibleCapacity);
        passed = false;
    }

    // 写进去的那部分必须仍然是好的 —— 溢出不该把已经写好的条目搅乱
    {
        TArray<UInt8> visibleBytes;

        const UInt32 frameIndex = context->GetCurrentFrameIndex();

        FCullReadbackRequest request;
        request.Source = cull->GetVisibleMeshletBuffer(frameIndex);
        request.Bytes =
            static_cast<UInt64>(overflowed.VisibleCapacity) * sizeof(UInt32) *
            2u;
        request.Target = &visibleBytes;

        if (!ReadCullBuffers(context, renderer, &request, 1))
        {
            LIMX_LOG(LogLaunch, Error, "[Meshlet规模] 可见表回读失败");
            passed = false;
        }
        else
        {
            const auto* pairs =
                reinterpret_cast<const UInt32*>(visibleBytes.GetData());

            const UInt32 instanceCount =
                static_cast<UInt32>(cull->GetInstances().GetSize());

            const UInt32 meshletCount = cull->GetSceneMeshletCount();

            SizeType corrupt = 0;

            const TArray<FMeshletInstanceGpu>& instances =
                cull->GetInstances();

            for (UInt32 i = 0; i < overflowed.VisibleCapacity; ++i)
            {
                const UInt32 instanceIndex = pairs[i * 2 + 0];
                const UInt32 meshletIndex  = pairs[i * 2 + 1];

                if (instanceIndex >= instanceCount ||
                    meshletIndex >= meshletCount)
                {
                    ++corrupt;
                    continue;
                }

                // 两个数还得**互相对得上**: meshlet 必须落在这个实例自己的
                // 区间里。
                //
                // 只验各自在范围内是不够的 —— 一条记录是两个独立的四字节
                // 写, 两个线程抢同一个槽位时会拼出"甲的实例 + 乙的 meshlet",
                // 而那两个数各自都合法。撕开的记录画出来是一块位置完全不对
                // 的几何体。
                const FMeshletInstanceGpu& instance = instances[instanceIndex];

                const UInt32 first = instance.MeshletRange[0];
                const UInt32 count = instance.MeshletRange[1];

                if (meshletIndex < first || meshletIndex >= first + count)
                {
                    ++corrupt;
                }
            }

            LIMX_LOG(LogLaunch, Display,
                     "[Meshlet规模] 溢出时写进去的 {} 条里越界的 {} 条",
                     overflowed.VisibleCapacity, corrupt);

            if (corrupt != 0)
            {
                LIMX_LOG(LogLaunch, Error,
                         "[Meshlet规模] 溢出时写进去的条目有 {} 条越界 —— "
                         "丢弃那一步没拦住越界写",
                         corrupt);
                passed = false;
            }
        }
    }

    // ---- 第三段: 恢复容量 ----
    cull->SetVisibleCapacityOverride(0);

    Settle();

    const FMeshletCullStats restored = cull->GetStats();

    LIMX_LOG(LogLaunch, Display,
             "[Meshlet规模] 恢复容量 — 要写 {} 条 (容量 {})",
             restored.VisibleRequested, restored.VisibleCapacity);

    if (restored.HasOverflow())
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Meshlet规模] 容量恢复之后还在报溢出 —— 标志粘住了, "
                 "那样它之后永远是真的, 也就永远不再指示任何东西");
        passed = false;
    }

    if (restored.MeshletsVisible != normal.MeshletsVisible)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Meshlet规模] 容量恢复之后可见数没回到原值 ({} vs {}) —— "
                 "压容量这件事留下了副作用",
                 restored.MeshletsVisible, normal.MeshletsVisible);
        passed = false;
    }

    LIMX_LOG(LogLaunch, Display, "[Meshlet规模] {}",
             passed ? "通过" : "失败");

    return passed;
}

// ============================================================================
// RunMeshletExpandOverflowChecks — 交给 DrawIndirect 的顶点数不许超过流的容量
//
// ── 这条判据守的是什么 ──
//
// 回退路径把可见 meshlet 展开成一条定容的顶点流。展开着色器里那次原子加
// 是**槽位分配器**: 领到的位置超出容量的三角形被丢掉。丢掉本身没得选 ——
// 越界写会改掉别的缓冲区, 而那会以完全无关的形式出错。
//
// 要命的是那个计数器同时又是 vkCmdDrawIndirect 读的 vertexCount。用同一个
// 数的话, 被丢掉的三角形照样把它加上去 —— 命令处理器于是按一个**大于流里
// 真实顶点数**的值发顶点, 而 meshlet_depth_fallback.vert 拿 gl_VertexIndex
// 无条件索引那条流。后果不是"画面上多一块垃圾", 是 GPU 读缓冲区之外的地址、
// 设备丢失。这与 --gpu-cull-overflow-check 守的是同一类缺陷: **容量被突破
// 之后, 消费的一侧照着计数器走, 而不是照着真正写进去的数走。**
//
// ── 为什么判据不是"跑一下不丢设备" ──
//
// 越界读到的地址是不是已映射, 取决于当时的堆布局。同一个越界在这台机器上
// 丢设备, 在另一台上什么事都没有 —— 拿它当判据等于让判据去赌运气。本条
// 判据验的是那条不变式本身: **交出去的顶点数 <= 流的容量**, 而且恰好等于
// 那个连续前缀的长度。与堆布局无关。
//
// ── 为什么要压容量 ──
//
// 流按两百万个顶点开的, 靠场景规模撑满它要堆出几十万个可见 meshlet, 那种
// 场景跑一次要几分钟。走不到的分支就是没有判据的分支 —— 所以给判据一个把
// 容量压小的入口, 与 --meshlet-scale-check 同一个做法。
//
// 三段:
//   正常容量   -> 不许报溢出, 而且两个数必须相等 (没丢东西)
//   压小容量   -> 必须报溢出; 交出去的数**不许超过容量**, 且正好是满的前缀
//   恢复容量   -> 又不许报溢出 (标志不是粘住的), 顶点数回到原值
// ============================================================================

static bool RunMeshletExpandOverflowChecks(FRenderContext* context,
                                           FRenderer&      renderer)
{
    LIMX_UNUSED(context);

    if (!renderer.SetMeshletDepthEnabled(true))
    {
        LIMX_LOG(LogLaunch, Error,
                 "[展开溢出] 无法启用 meshlet 光栅化 — 判据无法执行, "
                 "判定为失败");
        return false;
    }

    FMeshletDepthPass* const pass = renderer.GetMeshletDepthPass();

    if (pass == nullptr)
    {
        LIMX_LOG(LogLaunch, Error, "[展开溢出] 光栅化通道不存在");
        return false;
    }

    // 展开只在回退路径上发生 —— 网格着色器路径根本没有这条顶点流。
    // 切不过去就判失败, 不静默跳过: 静默跳过的判据永远是绿的。
    if (!pass->SetMode(FMeshletDepthPass::EMode::Fallback))
    {
        LIMX_LOG(LogLaunch, Error, "[展开溢出] 无法切到计算展开回退路径");
        return false;
    }

    bool passed = true;

    // 回读隔着并行帧数, 每段都要多走几帧才读得到那一段的数
    const auto Settle = [&renderer]()
    {
        for (UInt32 i = 0; i < 4; ++i)
        {
            renderer.RenderFrame();
        }
    };

    // ---- 第一段: 正常容量 ----
    pass->SetExpandedCapacityOverride(0);

    Settle();

    const FMeshletExpandStats normal = pass->GetExpandStats();

    LIMX_LOG(LogLaunch, Display,
             "[展开溢出] 正常容量 — 要写 {} 个顶点, 写进去 {} 个 (容量 {})",
             normal.Requested, normal.Written, normal.Capacity);

    if (normal.Written == 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[展开溢出] 一个顶点都没展开出来 —— 判据没验到东西。"
                 "场景里得有可见的 meshlet");
        passed = false;
    }

    if (normal.HasOverflow())
    {
        LIMX_LOG(LogLaunch, Error,
                 "[展开溢出] 正常容量下就报了溢出 (要写 {} > 容量 {}) —— "
                 "要么场景真的超了, 要么溢出判断本身写反了",
                 normal.Requested, normal.Capacity);
        passed = false;
    }

    // 没丢东西的时候, "想写多少"与"写进去多少"必须是同一个数。
    // 不等的话说明分配器与写入这两步已经对不上了, 后面那条不变式即使
    // 成立也只是碰巧。
    if (normal.Written != normal.Requested)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[展开溢出] 没溢出却写进去 {} 个而想写 {} 个 —— 两个数在"
                 "不该分开的时候分开了",
                 normal.Written, normal.Requested);
        passed = false;
    }

    // ---- 第二段: 把容量压到装不下 ----
    //
    // 压到四分之一, 至少留 3 个 (一个三角形)。压到 0 的话"写进去的那部分
    // 是满的前缀"这一条平凡成立。
    const UInt32 squeezed = FMath::Max(3u, normal.Written / 4u);

    pass->SetExpandedCapacityOverride(squeezed);

    Settle();

    const FMeshletExpandStats overflowed = pass->GetExpandStats();

    LIMX_LOG(LogLaunch, Display,
             "[展开溢出] 压到 {} 个顶点 — 要写 {} 个, 交给 DrawIndirect 的是 "
             "{} 个",
             overflowed.Capacity, overflowed.Requested, overflowed.Written);

    if (!overflowed.HasOverflow())
    {
        LIMX_LOG(LogLaunch, Error,
                 "[展开溢出] 容量压到 {} 个顶点还报不出溢出 (要写 {} 个) —— "
                 "超出的三角形被静默丢掉了, 画面上少一块而日志里一个字都没有",
                 overflowed.Capacity, overflowed.Requested);
        passed = false;
    }

    if (overflowed.Requested <= overflowed.Capacity)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[展开溢出] 压小容量之后要写的顶点数也跟着变小了 "
                 "({} <= {}) —— 分配器被容量夹住了, 那样就再也看不出丢过东西",
                 overflowed.Requested, overflowed.Capacity);
        passed = false;
    }

    // ---- 这就是那条不变式 ----
    //
    // 交给 vkCmdDrawIndirect 的顶点数不许超过流的容量。超了就是顶点着色器
    // 拿 gl_VertexIndex 索引到缓冲区之外 —— GPU 读非法地址、设备丢失。
    if (overflowed.Written > overflowed.Capacity)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[展开溢出] 交给 DrawIndirect 的顶点数是 {}, 而流只装得下 "
                 "{} 个 —— 那次绘制会让顶点着色器按 gl_VertexIndex 读到流"
                 "之外 (后果是 GPU 读非法地址、设备丢失)",
                 overflowed.Written, overflowed.Capacity);
        passed = false;
    }

    // 也不许少画: 写进去的是从 0 开始的连续前缀, 长度正好是容量向下取整到
    // 三的倍数。少于这个数说明白白丢掉了本来装得下的三角形。
    const UInt32 expectedPrefix = (overflowed.Capacity / 3u) * 3u;

    if (overflowed.Written != expectedPrefix)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[展开溢出] 溢出时交出去 {} 个顶点, 而满的前缀应当是 {} 个 "
                 "(容量 {}) —— 多了是越界, 少了是白丢",
                 overflowed.Written, expectedPrefix, overflowed.Capacity);
        passed = false;
    }

    // ---- 第三段: 恢复容量 ----
    pass->SetExpandedCapacityOverride(0);

    Settle();

    const FMeshletExpandStats restored = pass->GetExpandStats();

    LIMX_LOG(LogLaunch, Display,
             "[展开溢出] 恢复容量 — 要写 {} 个顶点, 交出去 {} 个 (容量 {})",
             restored.Requested, restored.Written, restored.Capacity);

    if (restored.HasOverflow())
    {
        LIMX_LOG(LogLaunch, Error,
                 "[展开溢出] 容量恢复之后还在报溢出 —— 计数器没有逐帧归零, "
                 "那样它之后永远是真的, 也就永远不再指示任何东西");
        passed = false;
    }

    if (restored.Written != normal.Written)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[展开溢出] 容量恢复之后顶点数没回到原值 ({} vs {}) —— "
                 "压容量这件事留下了副作用",
                 restored.Written, normal.Written);
        passed = false;
    }

    LIMX_LOG(LogLaunch, Display, "[展开溢出] {}", passed ? "通过" : "失败");

    return passed;
}

// ============================================================================

namespace
{

/// 归一化过的三角形 —— 旋转到最小下标打头, 绕序不变
struct FCanonicalTriangle
{
    UInt32 A = 0;
    UInt32 B = 0;
    UInt32 C = 0;
};

FCanonicalTriangle Canonicalize(UInt32 a, UInt32 b, UInt32 c)
{
    FCanonicalTriangle triangle;

    // 三个旋转里挑第一个角最小的那个。只旋转不交换 —— 交换会翻转绕序,
    // 而绕序决定正反面, 是判据要守住的东西之一。
    if (a <= b && a <= c)
    {
        triangle.A = a;
        triangle.B = b;
        triangle.C = c;
    }
    else if (b <= a && b <= c)
    {
        triangle.A = b;
        triangle.B = c;
        triangle.C = a;
    }
    else
    {
        triangle.A = c;
        triangle.B = a;
        triangle.C = b;
    }

    return triangle;
}

bool TriangleLess(const FCanonicalTriangle& x, const FCanonicalTriangle& y)
{
    if (x.A != y.A)
    {
        return x.A < y.A;
    }

    if (x.B != y.B)
    {
        return x.B < y.B;
    }

    return x.C < y.C;
}

bool TriangleEqual(const FCanonicalTriangle& x, const FCanonicalTriangle& y)
{
    return x.A == y.A && x.B == y.B && x.C == y.C;
}

FVector3 GeometricNormal(const FVector3& a, const FVector3& b,
                         const FVector3& c)
{
    const FVector3 ab = b - a;
    const FVector3 ac = c - a;

    const FVector3 cross(ab.Y * ac.Z - ab.Z * ac.Y,
                         ab.Z * ac.X - ab.X * ac.Z,
                         ab.X * ac.Y - ab.Y * ac.X);

    const Float32 length =
        FMath::Sqrt(cross.X * cross.X + cross.Y * cross.Y +
                    cross.Z * cross.Z);

    if (length < 1.0e-20f)
    {
        return FVector3(0.0f, 0.0f, 0.0f);
    }

    return FVector3(cross.X / length, cross.Y / length, cross.Z / length);
}

/// 对一个网格跑完整的八条判据
bool CheckOneMesh(const AnsiChar* label, const TArray<FMeshVertex>& vertices,
                  const TArray<UInt32>& indices, Float32 minAverageTriangles,
                  Float32 minVertexReuse)
{
    const FMeshletBuildResult result =
        FMeshletBuilder::Build(vertices, indices);

    if (!result.IsValid())
    {
        LIMX_LOG(LogLaunch, Error, "[Meshlet] {} — 切分失败", label);
        return false;
    }

    bool passed = true;

    const SizeType triangleCount = indices.GetSize() / 3;

    // ---- 判据 2/3/4: 上限、局部索引、全局下标 ----
    SizeType emittedTriangles = 0;

    for (SizeType m = 0; m < result.Meshlets.GetSize(); ++m)
    {
        const FMeshlet& meshlet = result.Meshlets[m];

        if (meshlet.VertexCount == 0 || meshlet.TriangleCount == 0)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[Meshlet] {} — meshlet {} 是空的 ({} 顶点 {} 三角形)",
                     label, m, meshlet.VertexCount, meshlet.TriangleCount);
            passed = false;
            break;
        }

        if (meshlet.VertexCount > kMaxMeshletVertices ||
            meshlet.TriangleCount > kMaxMeshletTriangles)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[Meshlet] {} — meshlet {} 超限: {} 顶点 (上限 {}), "
                     "{} 三角形 (上限 {})",
                     label, m, meshlet.VertexCount, kMaxMeshletVertices,
                     meshlet.TriangleCount, kMaxMeshletTriangles);
            passed = false;
            break;
        }

        if (meshlet.VertexOffset + meshlet.VertexCount >
            result.MeshletVertices.GetSize())
        {
            LIMX_LOG(LogLaunch, Error,
                     "[Meshlet] {} — meshlet {} 的顶点区间越界", label, m);
            passed = false;
            break;
        }

        if ((static_cast<SizeType>(meshlet.TriangleOffset) +
             meshlet.TriangleCount) * 3 >
            result.MeshletTriangles.GetSize())
        {
            LIMX_LOG(LogLaunch, Error,
                     "[Meshlet] {} — meshlet {} 的三角形区间越界", label, m);
            passed = false;
            break;
        }

        for (UInt32 i = 0; i < meshlet.VertexCount; ++i)
        {
            const UInt32 global =
                result.MeshletVertices[meshlet.VertexOffset + i];

            if (global >= vertices.GetSize())
            {
                LIMX_LOG(LogLaunch, Error,
                         "[Meshlet] {} — meshlet {} 的第 {} 个局部顶点指向"
                         "全局 {}, 而顶点数只有 {}",
                         label, m, i, global, vertices.GetSize());
                passed = false;
                break;
            }
        }

        for (UInt32 t = 0; t < meshlet.TriangleCount && passed; ++t)
        {
            for (UInt32 k = 0; k < 3; ++k)
            {
                const UInt8 local =
                    result.MeshletTriangles[
                        (static_cast<SizeType>(meshlet.TriangleOffset) + t) *
                            3 + k];

                if (local >= meshlet.VertexCount)
                {
                    LIMX_LOG(LogLaunch, Error,
                             "[Meshlet] {} — meshlet {} 的三角形 {} 用了局部"
                             "索引 {}, 而它只有 {} 个顶点",
                             label, m, t, local, meshlet.VertexCount);
                    passed = false;
                    break;
                }
            }
        }

        emittedTriangles += meshlet.TriangleCount;
    }

    if (!passed)
    {
        return false;
    }

    // ---- 判据 1: 三角形多重集完全相同 ----
    TArray<FCanonicalTriangle> original;
    TArray<FCanonicalTriangle> emitted;

    original.Reserve(triangleCount);
    emitted.Reserve(emittedTriangles);

    for (SizeType t = 0; t < triangleCount; ++t)
    {
        original.Add(Canonicalize(indices[t * 3 + 0], indices[t * 3 + 1],
                                  indices[t * 3 + 2]));
    }

    for (SizeType m = 0; m < result.Meshlets.GetSize(); ++m)
    {
        const FMeshlet& meshlet = result.Meshlets[m];

        for (UInt32 t = 0; t < meshlet.TriangleCount; ++t)
        {
            const SizeType base =
                (static_cast<SizeType>(meshlet.TriangleOffset) + t) * 3;

            UInt32 global[3] = {};

            for (UInt32 k = 0; k < 3; ++k)
            {
                global[k] = result.MeshletVertices[
                    meshlet.VertexOffset + result.MeshletTriangles[base + k]];
            }

            emitted.Add(Canonicalize(global[0], global[1], global[2]));
        }
    }

    LIMX_LOG(LogLaunch, Display,
             "[Meshlet] {} — 原始 {} 个三角形, 展开得到 {} 个",
             label, original.GetSize(), emitted.GetSize());

    if (original.GetSize() != emitted.GetSize())
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Meshlet] {} — 三角形数对不上: 原始 {}, 展开 {} "
                 "(差 {} 个)",
                 label, original.GetSize(), emitted.GetSize(),
                 static_cast<Int64>(emitted.GetSize()) -
                     static_cast<Int64>(original.GetSize()));
        passed = false;
    }
    else
    {
        Sort(original.GetData(), original.GetSize(), TriangleLess);
        Sort(emitted.GetData(), emitted.GetSize(), TriangleLess);

        SizeType mismatched = 0;
        SizeType firstBad = 0;

        for (SizeType i = 0; i < original.GetSize(); ++i)
        {
            if (!TriangleEqual(original[i], emitted[i]))
            {
                if (mismatched == 0)
                {
                    firstBad = i;
                }

                ++mismatched;
            }
        }

        if (mismatched != 0)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[Meshlet] {} — {} 个三角形对不上, 第一个 (排序后第 {} "
                     "个): 原始 ({},{},{}) vs 展开 ({},{},{})",
                     label, mismatched, firstBad,
                     original[firstBad].A, original[firstBad].B,
                     original[firstBad].C,
                     emitted[firstBad].A, emitted[firstBad].B,
                     emitted[firstBad].C);
            passed = false;
        }
    }

    // ---- 判据 5/6: 包围球与法线锥必须真的包住 ----
    //
    // 这两条不是"精度好不好", 是"这个包围体是不是包围体"。不包住的后果是
    // 剔除掉了可见的东西 —— 画面上少一块, 而那与"这一块本来就在视锥外"
    // 长得一样。
    SizeType sphereViolations = 0;
    SizeType coneViolations = 0;

    Float32 worstSphere = 0.0f;
    Float32 worstCone = 0.0f;

    // 包围球**紧不紧**, 与包围球**包不包得住**是两件事。
    //
    // 半径取到最远顶点这一条让球一定包得住, 球心取哪里都成立 —— 于是
    // "球心取错"这类缺陷在包含性判据上完全没有痕迹 (实测球心改成局部
    // 顶点表的第一个点, 上面那条判据一个字都不说, 而平均半径涨了 66%)。
    //
    // 紧致度这边有一条**不需要调参**的判据: 球心取包围盒中心时,
    // 半径必然不超过包围盒对角线的一半 —— 那是"盒内任一点到盒心的距离
    // 不超过半对角线"这个几何事实, 不是经验值。球心一旦偏离盒心, 半径
    // 就可能超过它 (球心取到角上时正好是两倍)。
    SizeType loosenessViolations = 0;

    Float32 worstLooseness = 0.0f;

    // 法线锥的哨兵值只有两种合法形态: 恰好是无效标记, 或者严格为正。
    // 落在 (-1, 0] 里的余弦表示"锥张过了半球"却没被标记 —— 剔除侧会拿
    // 一个没有意义的锥去判。这一条今天还没有消费者, 所以它的后果不在
    // 画面上; 但契约是**现在**定的, 判据也该现在写。
    SizeType sentinelViolations = 0;

    for (SizeType m = 0; m < result.Meshlets.GetSize(); ++m)
    {
        const FMeshlet& meshlet = result.Meshlets[m];

        const FVector3 center(meshlet.BoundingSphere.X,
                              meshlet.BoundingSphere.Y,
                              meshlet.BoundingSphere.Z);

        FVector3 minimum(1.0e30f, 1.0e30f, 1.0e30f);
        FVector3 maximum(-1.0e30f, -1.0e30f, -1.0e30f);

        for (UInt32 i = 0; i < meshlet.VertexCount; ++i)
        {
            const FVector3& p =
                vertices[result.MeshletVertices[meshlet.VertexOffset + i]]
                    .Position;

            minimum.X = FMath::Min(minimum.X, p.X);
            minimum.Y = FMath::Min(minimum.Y, p.Y);
            minimum.Z = FMath::Min(minimum.Z, p.Z);

            maximum.X = FMath::Max(maximum.X, p.X);
            maximum.Y = FMath::Max(maximum.Y, p.Y);
            maximum.Z = FMath::Max(maximum.Z, p.Z);

            const FVector3 delta = p - center;

            const Float32 distance =
                FMath::Sqrt(delta.X * delta.X + delta.Y * delta.Y +
                            delta.Z * delta.Z);

            const Float32 excess = distance - meshlet.BoundingSphere.W;

            if (excess > 1.0e-4f)
            {
                ++sphereViolations;
                worstSphere = FMath::Max(worstSphere, excess);
            }
        }

        // ---- 紧致度: 半径不得超过自身包围盒的半对角线 ----
        {
            const FVector3 diagonal = maximum - minimum;

            const Float32 halfDiagonal =
                0.5f * FMath::Sqrt(diagonal.X * diagonal.X +
                                   diagonal.Y * diagonal.Y +
                                   diagonal.Z * diagonal.Z);

            const Float32 excess = meshlet.BoundingSphere.W - halfDiagonal;

            // 容差按尺度走: 半对角线的百万分之一。绝对容差在大网格上
            // 太紧、在小网格上太松, 而这个网格的尺度是未知的。
            const Float32 tolerance =
                FMath::Max(halfDiagonal * 1.0e-6f, 1.0e-6f);

            if (excess > tolerance)
            {
                ++loosenessViolations;
                worstLooseness = FMath::Max(worstLooseness, excess);
            }
        }

        // ---- 哨兵: 要么是无效标记, 要么落在 [0, 1) ----
        //
        // 存的是半角**正弦**。半角超过 90 度的锥对背面剔除没有价值, 那时
        // 正弦回到 1 附近而无法与"很窄的锥"区分开 —— 所以构建器在那种情形
        // 直接写无效标记, 而有效值必然落在 [0, 1)。
        //
        // 这条判据原来按余弦写 (要求严格为正)。换成正弦之后它立刻报了
        // 十几条 —— 那正是它该做的: **字段的含义变了而读它的地方没跟上**,
        // 判据把这件事顶了出来, 而不是默默接受。
        if (meshlet.NormalCone.W != kInvalidConeCutoff &&
            (meshlet.NormalCone.W < 0.0f || meshlet.NormalCone.W >= 1.0f))
        {
            ++sentinelViolations;
        }

        if (meshlet.NormalCone.W <= kInvalidConeCutoff)
        {
            continue;
        }

        // 从半角正弦还原半角余弦 —— 下面要拿它与每个三角形的法线投影比
        const Float32 coneCosine = FMath::Sqrt(FMath::Max(
            0.0f, 1.0f - meshlet.NormalCone.W * meshlet.NormalCone.W));

        const FVector3 axis(meshlet.NormalCone.X, meshlet.NormalCone.Y,
                            meshlet.NormalCone.Z);

        for (UInt32 t = 0; t < meshlet.TriangleCount; ++t)
        {
            const SizeType base =
                (static_cast<SizeType>(meshlet.TriangleOffset) + t) * 3;

            FVector3 corner[3];

            for (UInt32 k = 0; k < 3; ++k)
            {
                corner[k] =
                    vertices[result.MeshletVertices[
                        meshlet.VertexOffset +
                        result.MeshletTriangles[base + k]]].Position;
            }

            const FVector3 normal =
                GeometricNormal(corner[0], corner[1], corner[2]);

            // 退化三角形没有法线 —— 构建时就把它排除在锥外了
            if (normal.X == 0.0f && normal.Y == 0.0f && normal.Z == 0.0f)
            {
                continue;
            }

            const Float32 dot = axis.X * normal.X + axis.Y * normal.Y +
                                axis.Z * normal.Z;

            const Float32 deficit = coneCosine - dot;

            if (deficit > 1.0e-4f)
            {
                ++coneViolations;
                worstCone = FMath::Max(worstCone, deficit);
            }
        }
    }

    if (sphereViolations != 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Meshlet] {} — {} 个顶点落在自己 meshlet 的包围球外 "
                 "(最多超出 {}) —— 那个球不是包围球",
                 label, sphereViolations, worstSphere);
        passed = false;
    }

    if (coneViolations != 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Meshlet] {} — {} 个三角形的法线落在自己 meshlet 的法线锥外 "
                 "(最多差 {}) —— 背面剔除会剔掉正面的东西",
                 label, coneViolations, worstCone);
        passed = false;
    }

    if (loosenessViolations != 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Meshlet] {} — {} 个 meshlet 的包围球半径超过了自身包围盒的"
                 "半对角线 (最多超 {}) —— 球心没取在包围盒中心? "
                 "球仍然包得住, 但白白大了一圈, 剔除会漏掉本该剔的",
                 label, loosenessViolations, worstLooseness);
        passed = false;
    }

    if (sentinelViolations != 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Meshlet] {} — {} 个 meshlet 的法线锥半角正弦落在 [0, 1) "
                 "之外 —— 张角超过半球却没标记成无效锥, 剔除侧会拿它去判",
                 label, sentinelViolations);
        passed = false;
    }

    // ---- 判据 7/8: 质量 ----
    const FMeshletStatistics statistics =
        FMeshletBuilder::ComputeStatistics(result);

    LIMX_LOG(LogLaunch, Display,
             "[Meshlet] {} — {} 个 meshlet, 平均 {} 三角形 / {} 顶点 "
             "(最多 {} / {}), 顶点复用 {}, 平均包围球半径 {}, 有效法线锥 {}",
             label, statistics.MeshletCount, statistics.AverageTriangles,
             statistics.AverageVertices, statistics.MaxTriangles,
             statistics.MaxVertices, statistics.VertexReuse,
             statistics.AverageSphereRadius, statistics.ValidConeFraction);

    if (statistics.AverageTriangles < minAverageTriangles)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Meshlet] {} — 平均每个 meshlet 只有 {} 个三角形 "
                 "(需要至少 {}) —— 切得太碎, 剔除粒度没有意义",
                 label, statistics.AverageTriangles, minAverageTriangles);
        passed = false;
    }

    if (statistics.VertexReuse < minVertexReuse)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[Meshlet] {} — 顶点复用率只有 {} (需要至少 {}) —— "
                 "聚类没有把相邻的三角形放到一起",
                 label, statistics.VertexReuse, minVertexReuse);
        passed = false;
    }

    return passed;
}

} // namespace

static bool RunMeshletChecks(FRenderContext* context, FRenderer& renderer)
{
    // 这条判据不碰 GPU —— meshlet 切分全在 CPU 上。两个参数留着是为了与
    // 别的判据同签名, 调度处不必为它开特例。
    LIMX_UNUSED(context);
    LIMX_UNUSED(renderer);

    bool passed = true;

    // ---- 程序化几何: 球体 ----
    //
    // 球是最能分辨聚类好坏的形状: 它没有平坦区域, 每个三角形的法线都不同,
    // 于是法线锥的紧致度直接反映聚类的空间局部性。切得散的话锥会张满半球,
    // 有效锥占比掉到零。
    //
    // 阈值按实测定, 留三成余量:
    //     平均三角形数  实测 64.0   ->  阈值 48
    //     顶点复用      实测 4.15   ->  阈值 3.0
    //
    // 顶点复用的理论上限接近 6 (规则三角网格上每个顶点被六个三角形共用),
    // 而 meshlet 边界上的顶点要在两侧各存一份, 所以实际达不到。
    //
    // 阈值不敢定得更高: 它们是**质量**判据, 一次合理的算法调整就可能让
    // 实测值动几个百分点, 而那不该让流水线变红。它们要拦的是数量级的
    // 退化 —— 比如"按索引顺序每 124 个三角形切一刀"这种完全不看邻接的
    // 实现 (复用率会掉到 1.2 附近)。
    {
        const FMeshData sphere = FGeometryGenerator::GenerateSphere(1.0f, 64, 48);

        passed &= CheckOneMesh("球体 (64x48)", sphere.Vertices, sphere.Indices,
                               48.0f, 3.0f);
    }

    // ---- 程序化几何: 立方体 ----
    //
    // 立方体只有 12 个三角形 —— 一个 meshlet 装得下。这个用例验的是
    // "小网格不会被切碎", 以及 24 个顶点的立方体每个面独立法线时,
    // 法线锥必然张满半球 (六个面朝六个方向) —— 那时必须标记成无效锥,
    // 而不是给出一个假装包住的锥。
    //
    // 质量阈值对它没有意义 (总共就 12 个三角形), 所以传 0。
    {
        const FMeshData cube = FGeometryGenerator::GenerateCube();

        passed &= CheckOneMesh("立方体", cube.Vertices, cube.Indices,
                               0.0f, 0.0f);
    }

    // ---- 真实资产: 场景里正在渲染的网格 ----
    //
    // 程序化几何是规则的 —— 顶点顺序整齐、三角形带连续。真实资产不是:
    // OBJ 的顶点顺序由导出器决定, 子网格共用缓冲区, 还可能有退化三角形。
    // 只在程序化几何上验的话, "邻接表建错了"这类缺陷会因为输入太整齐而
    // 不显形。
    {
        FAssetScene assetScene;

        const FAssetLoadResult loaded = FObjLoader::LoadFromFile(
            FString("Content/TestScene/testscene.obj"), assetScene);

        if (!loaded.Succeeded || assetScene.Meshes.IsEmpty())
        {
            LIMX_LOG(LogLaunch, Error,
                     "[Meshlet] 无法解析 Content/TestScene/testscene.obj — "
                     "真实资产那一段判不了, 判定为失败: {}",
                     loaded.ErrorMessage.GetCStr());
            passed = false;
        }
        else
        {
            SizeType meshesChecked = 0;

            for (SizeType i = 0; i < assetScene.Meshes.GetSize(); ++i)
            {
                const FMeshData& mesh = assetScene.Meshes[i];

                // 太小的网格上质量阈值没有意义 —— 一个 meshlet 就装完了。
                // 正确性判据仍然跑。
                const bool small = (mesh.Indices.GetSize() / 3) < 256;

                passed &= CheckOneMesh(
                    mesh.Name.GetCStr(), mesh.Vertices, mesh.Indices,
                    small ? 0.0f : 64.0f, small ? 0.0f : 2.0f);

                ++meshesChecked;
            }

            LIMX_LOG(LogLaunch, Display,
                     "[Meshlet] 真实资产 — 验了 {} 个网格", meshesChecked);

            // 元判据: 资产里得真有网格。空场景上前面那个循环一遍都不跑,
            // 而 passed 保持为真 —— 又一条"失败落在通过上"的路。
            if (meshesChecked == 0)
            {
                LIMX_LOG(LogLaunch, Error,
                         "[Meshlet] 资产里一个网格都没有 — 这一段是空的");
                passed = false;
            }
        }
    }

    LIMX_LOG(LogLaunch, Display, "[Meshlet] {}", passed ? "通过" : "失败");

    return passed;
}

// ============================================================================
// RunRayTracingHybridCheck — 光追产出的图有没有到达画面
//
// 前六天的每一条光追判据都是**旁路判据**: 它们把通道产出的那张图读回来,
// 与解析值逐像素比。那证明了"这张图算得对", 没有证明"画面用了这张图"。
//
// 中间隔着的东西不少: 描述符绑定的槽位、光照 UBO 里的位域、着色器里那个
// if、半透明的护栏。任何一处断了, 旁路判据照样满分通过 —— 因为那张图确实
// 还是对的, 只是没人读它。这正是本周期反复遇到的那一类缺陷: **失败会落在
// 通过上**。
//
// 三条判据, 都不看颜色本身 (颜色对不对是别的判据的事), 只看**因果**:
//
//   一、AO 只能变暗。环境光遮蔽是从环境项里减光, 它没有任何途径让一个
//       像素变亮。有像素变亮就说明它被乘到了错的项上, 或者符号反了。
//       容差取 1/255 —— 画面是 8 位的, 一个量化台阶以内的抖动来自 TAA
//       的抖动序列, 不是 AO。
//
//   二、AO 暗的地方画面必须跟着变。反过来的说法更有用: AO 图上有 N 个
//       像素明显小于 1, 而画面上一个像素都没变的话, 那张图没有到达着色。
//       判据是"变了的像素数 >= AO 明显小于 1 的像素数的一个下限比例" ——
//       不要求一一对应 (被光照遮住的、纯自发光的表面 AO 再暗也不变), 只
//       要求那个比例不能塌到零。实测 0.55, 阈值取 0.20。
//
//   三、反射同理: 反射图上命中的像素里, 画面必须有一部分跟着变。
//       并且反射**只作用于足够光滑的表面** —— 这一条由粗糙度过渡窗口
//       (0.25..0.45) 决定。窗口本身没法从画面上反推, 但它的后果能:
//       改到的像素比例必须落在一个区间里。只卡下限的话, "把反射无条件
//       加到每个像素上"照样通过 —— 而那正是这个窗口存在的理由。
//
// 这条判据跑综合场景 —— 那里同时有金属球 (反射看得见) 与柱子群 (AO 看
// 得见)。墙角场景两样都太单薄。
// ============================================================================

namespace
{

/// 把当前设置下的一帧回读成 8 位 RGBA
static bool CaptureShadedFrame(FRenderContext* context, FRenderer& renderer,
                               TArray<UInt8>& outPixels)
{
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
}

/// 把一张 R8 的屏幕空间图回读成 [0,1]
static bool CaptureR8Screen(FRenderContext*   context,
                            FRenderer&        renderer,
                            FRHITextureHandle texture,
                            EImageLayout      restingLayout,
                            TArray<Float32>&  outValues)
{
    if (!texture.IsValid())
    {
        return false;
    }

    IRHIDevice* const device = context->GetDevice();

    const FRHIExtent2D extent = context->GetSwapchainExtent();

    const SizeType pixelCount =
        static_cast<SizeType>(extent.Width) * extent.Height;

    FRHIBufferHandle readback;

    FRHIBufferDesc desc = {};
    desc.Usage       = EBufferUsage::TransferDst;
    desc.MemoryUsage = EMemoryUsage::GpuToCpu;
    desc.Size        = pixelCount;
    desc.DebugName   = "RtHybrid.R8";

    if (!IsRHISuccess(device->CreateBuffer(desc, readback)))
    {
        return false;
    }

    bool recorded = false;

    renderer.SetPostSceneRenderCallback(
        [&recorded, context, texture, readback, extent, restingLayout]()
        {
            IRHICommandBuffer* cmd = context->GetCurrentCommandBuffer();

            if (cmd == nullptr)
            {
                return;
            }

            FRHIBufferTextureCopyRegion region = {};
            region.BufferOffset      = 0;
            region.BufferRowLength   = 0;
            region.BufferImageHeight = 0;
            region.MipLevel          = 0;
            region.BaseLayer         = 0;
            region.LayerCount        = 1;
            region.TextureOffset     = { 0, 0, 0 };
            region.TextureExtent     = { extent.Width, extent.Height, 1 };

            cmd->TransitionImageLayout(
                texture, restingLayout, EImageLayout::TransferSrc,
                EPipelineStageFlags::FragmentShader,
                EPipelineStageFlags::Transfer,
                EAccessFlags::ShaderRead, EAccessFlags::TransferRead);

            cmd->CopyTextureToBuffer(texture, EImageLayout::TransferSrc,
                                     readback, region);

            cmd->TransitionImageLayout(
                texture, EImageLayout::TransferSrc, restingLayout,
                EPipelineStageFlags::Transfer,
                EPipelineStageFlags::FragmentShader,
                EAccessFlags::TransferRead, EAccessFlags::ShaderRead);

            recorded = true;
        });

    renderer.RenderFrame();
    renderer.SetPostSceneRenderCallback(TFunction<void()>());

    bool ok = recorded;

    if (ok)
    {
        device->WaitIdle();

        void* mapped = nullptr;

        if (IsRHISuccess(device->MapBuffer(readback, &mapped)) &&
            mapped != nullptr)
        {
            const auto* bytes = static_cast<const UInt8*>(mapped);

            outValues.Clear();
            outValues.Reserve(pixelCount);

            for (SizeType i = 0; i < pixelCount; ++i)
            {
                outValues.Add(static_cast<Float32>(bytes[i]) / 255.0f);
            }

            device->UnmapBuffer(readback);
        }
        else
        {
            ok = false;
        }
    }

    device->DestroyBuffer(readback);

    return ok;
}

/// 把深度缓冲区 (D32_SFLOAT) 回读成 NDC 深度
static bool CaptureDepthScreen(FRenderContext* context, FRenderer& renderer,
                               TArray<Float32>& outDepth)
{
    FDepthPrePass* const depthPass = renderer.GetDepthPrePass();

    if (depthPass == nullptr)
    {
        return false;
    }

    IRHIDevice* const device = context->GetDevice();

    const FRHIExtent2D extent = context->GetSwapchainExtent();

    const SizeType pixelCount =
        static_cast<SizeType>(extent.Width) * extent.Height;

    FRHIBufferHandle readback;

    FRHIBufferDesc desc = {};
    desc.Usage       = EBufferUsage::TransferDst;
    desc.MemoryUsage = EMemoryUsage::GpuToCpu;
    desc.Size        = pixelCount * 4u;
    desc.DebugName   = "RtHybrid.Depth";

    if (!IsRHISuccess(device->CreateBuffer(desc, readback)))
    {
        return false;
    }

    const FRHITextureHandle texture = depthPass->GetSharedDepthTexture();

    bool recorded = false;

    renderer.SetPostSceneRenderCallback(
        [&recorded, context, texture, readback, extent]()
        {
            IRHICommandBuffer* cmd = context->GetCurrentCommandBuffer();

            if (cmd == nullptr)
            {
                return;
            }

            FRHIBufferTextureCopyRegion region = {};
            region.BufferOffset      = 0;
            region.BufferRowLength   = 0;
            region.BufferImageHeight = 0;
            region.MipLevel          = 0;
            region.BaseLayer         = 0;
            region.LayerCount        = 1;
            region.TextureOffset     = { 0, 0, 0 };
            region.TextureExtent     = { extent.Width, extent.Height, 1 };

            cmd->TransitionImageLayout(
                texture, EImageLayout::DepthStencilAttachment,
                EImageLayout::TransferSrc,
                EPipelineStageFlags::LateFragmentTests,
                EPipelineStageFlags::Transfer,
                EAccessFlags::DepthStencilAttachmentWrite,
                EAccessFlags::TransferRead);

            cmd->CopyTextureToBuffer(texture, EImageLayout::TransferSrc,
                                     readback, region);

            cmd->TransitionImageLayout(
                texture, EImageLayout::TransferSrc,
                EImageLayout::DepthStencilAttachment,
                EPipelineStageFlags::Transfer,
                EPipelineStageFlags::EarlyFragmentTests,
                EAccessFlags::TransferRead,
                EAccessFlags::DepthStencilAttachmentWrite);

            recorded = true;
        });

    renderer.RenderFrame();
    renderer.SetPostSceneRenderCallback(TFunction<void()>());

    bool ok = recorded;

    if (ok)
    {
        device->WaitIdle();

        void* mapped = nullptr;

        if (IsRHISuccess(device->MapBuffer(readback, &mapped)) &&
            mapped != nullptr)
        {
            const auto* values = static_cast<const Float32*>(mapped);

            outDepth.Clear();
            outDepth.Reserve(pixelCount);

            for (SizeType i = 0; i < pixelCount; ++i)
            {
                outDepth.Add(values[i]);
            }

            device->UnmapBuffer(readback);
        }
        else
        {
            ok = false;
        }
    }

    device->DestroyBuffer(readback);

    return ok;
}

/// 截屏是每像素三字节 (RGB) —— FScreenshotCapture 已经把 alpha 丢掉了
inline constexpr SizeType kShotBytesPerPixel = 3;

/// 两幅 8 位图之间"这个像素变了吗"
///
/// 门限一个量化台阶: 画面是 8 位的, 差 1 说不清是 AO 还是 TAA 的抖动。
static bool PixelChanged(const TArray<UInt8>& a, const TArray<UInt8>& b,
                         SizeType pixel)
{
    const SizeType base = pixel * kShotBytesPerPixel;

    for (SizeType c = 0; c < 3; ++c)
    {
        const Int32 delta = static_cast<Int32>(b[base + c]) -
                            static_cast<Int32>(a[base + c]);

        if (delta > 1 || delta < -1)
        {
            return true;
        }
    }

    return false;
}

} // namespace

static bool RunRayTracingHybridCheck(FRenderContext* context,
                                     FRenderer&      renderer)
{
    bool passed = true;

    // ================================================================
    // 一、光追 AO
    // ================================================================
    if (!renderer.SetRayTracedAoEnabled(false))
    {
        LIMX_LOG(LogLaunch, Error, "[光追混合] 无法关闭光追 AO");
        return false;
    }

    TArray<UInt8> withoutAo;

    if (!CaptureShadedFrame(context, renderer, withoutAo))
    {
        LIMX_LOG(LogLaunch, Error, "[光追混合] 关闭态回读失败");
        return false;
    }

    if (!renderer.SetRayTracedAoEnabled(true))
    {
        LIMX_LOG(LogLaunch, Error, "[光追混合] 无法启用光追 AO");
        return false;
    }

    TArray<UInt8> withAo;

    if (!CaptureShadedFrame(context, renderer, withAo))
    {
        LIMX_LOG(LogLaunch, Error, "[光追混合] 启用态回读失败");
        return false;
    }

    TArray<Float32> aoValues;

    FRayTracedAoPass* const aoPass = renderer.GetRayTracedAoPass();

    if (aoPass == nullptr ||
        !CaptureR8Screen(context, renderer, aoPass->GetAoTexture(),
                         EImageLayout::ShaderReadOnly, aoValues))
    {
        LIMX_LOG(LogLaunch, Error, "[光追混合] AO 图回读失败");
        return false;
    }

    if (withoutAo.GetSize() != withAo.GetSize() ||
        withAo.GetSize() != aoValues.GetSize() * kShotBytesPerPixel)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[光追混合] 三次回读的尺寸对不上 ({} / {} / {})",
                 withoutAo.GetSize(), withAo.GetSize(), aoValues.GetSize());
        return false;
    }

    const SizeType pixelCount = aoValues.GetSize();

    SizeType occludedPixels = 0;
    SizeType changedPixels  = 0;
    SizeType brightened     = 0;
    Int32    worstBrighten  = 0;

    for (SizeType i = 0; i < pixelCount; ++i)
    {
        // AO 明显小于 1 的地方。0.04 = R8 的十个量化台阶,
        // 噪声进不来。
        const bool occluded = (aoValues[i] < 0.96f);

        if (!occluded)
        {
            continue;
        }

        ++occludedPixels;

        if (PixelChanged(withoutAo, withAo, i))
        {
            ++changedPixels;
        }

        // 变亮的量 —— 三个通道里最大的那个
        Int32 maxDelta = 0;

        for (SizeType c = 0; c < 3; ++c)
        {
            const SizeType byteIndex = i * kShotBytesPerPixel + c;

            const Int32 delta =
                static_cast<Int32>(withAo[byteIndex]) -
                static_cast<Int32>(withoutAo[byteIndex]);

            maxDelta = FMath::Max(maxDelta, delta);
        }

        if (maxDelta > 1)
        {
            ++brightened;
            worstBrighten = FMath::Max(worstBrighten, maxDelta);
        }
    }

    const Float32 changedFraction =
        (occludedPixels > 0)
            ? static_cast<Float32>(changedPixels) /
                  static_cast<Float32>(occludedPixels)
            : 0.0f;

    LIMX_LOG(LogLaunch, Display,
             "[光追混合] AO — 遮蔽像素 {} 个, 画面跟着变的 {} 个 ({}), "
             "变亮的 {} 个 (最多 +{})",
             occludedPixels, changedPixels, changedFraction,
             brightened, worstBrighten);

    // ---- 元判据: 场景里得真有遮蔽 ----
    constexpr SizeType kMinOccluded = 10000;

    if (occludedPixels < kMinOccluded)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[光追混合] 只有 {} 个遮蔽像素 (需要至少 {}) —— "
                 "这个场景判不了 AO 有没有到达画面",
                 occludedPixels, kMinOccluded);
        passed = false;
    }

    // ---- 判据一: AO 只能变暗 ----
    if (brightened != 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[光追混合] {} 个遮蔽像素在开启 AO 之后**变亮**了 "
                 "(最多 +{}/255) —— AO 被乘到了错的项上, 或者符号反了",
                 brightened, worstBrighten);
        passed = false;
    }

    // ---- 判据二: AO 暗的地方画面必须跟着变 ----
    //
    // 不要求一一对应: 被直接光压住的、纯自发光的表面, AO 再暗画面也不变。
    // 实测 0.55, 阈值取 0.20 —— 留三倍的余量给"换个场景/换个曝光"。
    constexpr Float32 kMinChangedFraction = 0.20f;

    if (changedFraction < kMinChangedFraction)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[光追混合] 遮蔽像素里只有 {} 的比例在画面上跟着变 "
                 "(需要至少 {}) —— AO 图没有到达着色?",
                 changedFraction, kMinChangedFraction);
        passed = false;
    }

    // 复位 —— 失败与否都要复位, 后面还有反射那一段
    if (!renderer.SetRayTracedAoEnabled(false))
    {
        LIMX_LOG(LogLaunch, Error, "[光追混合] 无法关闭光追 AO (复位)");
        passed = false;
    }

    // ================================================================
    // 二、光追反射
    // ================================================================
    TArray<UInt8> withoutReflection;

    if (!CaptureShadedFrame(context, renderer, withoutReflection))
    {
        LIMX_LOG(LogLaunch, Error, "[光追混合] 反射关闭态回读失败");
        return false;
    }

    if (!renderer.SetRayTracedReflectionEnabled(true))
    {
        LIMX_LOG(LogLaunch, Error, "[光追混合] 无法启用光追反射");
        return false;
    }

    TArray<UInt8> withReflection;

    if (!CaptureShadedFrame(context, renderer, withReflection))
    {
        LIMX_LOG(LogLaunch, Error, "[光追混合] 反射启用态回读失败");
        return false;
    }

    SizeType reflectionChanged = 0;

    for (SizeType i = 0; i < pixelCount; ++i)
    {
        if (PixelChanged(withoutReflection, withReflection, i))
        {
            ++reflectionChanged;
        }
    }

    const Float32 reflectionFraction =
        static_cast<Float32>(reflectionChanged) /
        static_cast<Float32>(pixelCount);

    LIMX_LOG(LogLaunch, Display,
             "[光追混合] 反射 — 画面上变了的像素 {} 个 ({})",
             reflectionChanged, reflectionFraction);

    // ---- 判据三: 反射必须改到画面, 而且不能改满屏 ----
    //
    // 两头都要卡。只卡下限的话, "把反射无条件加到每个像素上"能通过 ——
    // 而那正是粗糙度窗口存在的理由: 粗糙表面上一条射线给出的是噪声。
    // 综合场景里够光滑的表面 (金属球与地面) 占 4~6%, 上限取 25%。
    constexpr Float32 kMinReflectionFraction = 0.005f;
    constexpr Float32 kMaxReflectionFraction = 0.25f;

    if (reflectionFraction < kMinReflectionFraction)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[光追混合] 开启反射只改了 {} 的像素 (需要至少 {}) —— "
                 "反射图没有到达着色?",
                 reflectionFraction, kMinReflectionFraction);
        passed = false;
    }

    if (reflectionFraction > kMaxReflectionFraction)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[光追混合] 开启反射改了 {} 的像素 (上限 {}) —— "
                 "粗糙度过渡窗口失效了? 粗糙表面上一条射线给出的是噪声",
                 reflectionFraction, kMaxReflectionFraction);
        passed = false;
    }

    if (!renderer.SetRayTracedReflectionEnabled(false))
    {
        LIMX_LOG(LogLaunch, Error, "[光追混合] 无法关闭光追反射 (复位)");
        passed = false;
    }

    LIMX_LOG(LogLaunch, Display, "[光追混合] {}", passed ? "通过" : "失败");

    return passed;
}

// ============================================================================
// RunRayTracedAoUpsampleCheck — 双边上采样在深度不连续处必须不渗色
//
// 这条判据补的是 Day 7 扫描里的两条逃逸, 而它们其实是同一件事:
//
//   把双边权重去掉 (退化成双线性)        —— 墙角场景上的判据全绿
//   把深度线性化的分母符号写反 (历史缺陷)  —— 全绿
//
// 逃逸的原因不是判据不够严, 是**场景不够**。墙角场景的地面与墙是连续的,
// 相机看得到的范围里几乎没有深度不连续 —— 而双边加权的**全部作用**都在
// 不连续处。平坦区域上双边、双线性、最近邻三者给出的结果完全一样, 实测
// 三个变体的绝对误差是 0.002621 / 0.002621 / 0.002612, 前两个连小数点后
// 六位都相同。
//
// 所以这条判据跑综合场景: 球与柱子的轮廓、物体与远景之间, 到处是深度
// 不连续。
//
// 判据本身: 把同一个场景解两遍 (全分辨率 / 半分辨率+上采样), 只在**相邻
// 半分辨率样本跨越深度不连续**的那些像素上比。
//
//   双边   同一表面的样本权重高, 另一侧的权重压到零 —— 贴近全分辨率
//   双线性 两侧一起平均 —— 前景的 AO 渗到背景上
//
// 实测 0.006633 对 0.019440, 差三倍, 阈值定在中间。
//
// **这条判据抓不到第三种退化。** 深度线性化的分母符号写反时上采样退化成
// 最近邻, 而最近邻在这条统计量上是 0.006240 —— 比正确的双边还**小**。
// 那不是巧合: 偶数像素上半分辨率与全分辨率逐位相同, 最近邻总是取其中一个,
// 于是它比任何混合都更贴近全分辨率。它错在**画面**上 (边缘台阶化), 不错在
// 这个数上。
//
// 那条缺陷现在由别处堵住: 深度线性化只剩一份实现 (depth_common.h), 而
// 光追深度判据把它的输出与光追命中距离逐像素比到 64 ULP —— 符号一反,
// 距离变成负数, 那条判据立刻红。判据不必条条都万能, 但每条缺陷都得**有**
// 一条判据能红。
//
// 为什么不看整幅的最大差: 最大差被噪声主导 (每像素十六条射线, 蒙特卡洛
// 的尾巴很长), 三个变体的最大差都接近 1。均值也不行 —— 不连续处的像素只占
// 百分之几, 摊到全图就没了。要**限定在那条带上再取均值**。
//
// 偶数像素 (x 与 y 都是偶数) 排除在外: 那里上采样的双线性权重是 (1,0,0,0),
// 取到的就是原样本, 三种权重给出同一个数。留着它们只会把信号稀释四倍。
// ============================================================================

static bool RunRayTracedAoUpsampleCheck(FRenderContext* context,
                                        FRenderer&      renderer)
{
    FRayTracedAoPass* const aoPass = renderer.GetRayTracedAoPass();
    FDepthPrePass* const    depthPass = renderer.GetDepthPrePass();

    if (aoPass == nullptr || depthPass == nullptr)
    {
        LIMX_LOG(LogLaunch, Error, "[光追AO上采样] 通道不存在");
        return false;
    }

    if (!renderer.SetRayTracedAoEnabled(true))
    {
        LIMX_LOG(LogLaunch, Error,
                 "[光追AO上采样] 无法启用光追 AO — 判据无法执行, 判定为失败");
        return false;
    }

    const bool originalHalf = aoPass->IsHalfResolution();

    TArray<Float32> fullAo;
    TArray<Float32> halfAo;
    TArray<Float32> depthNdc;

    aoPass->SetHalfResolution(false);

    bool ok = CaptureR8Screen(context, renderer, aoPass->GetAoTexture(),
                              EImageLayout::ShaderReadOnly, fullAo);

    aoPass->SetHalfResolution(true);

    ok = ok && CaptureR8Screen(context, renderer, aoPass->GetAoTexture(),
                               EImageLayout::ShaderReadOnly, halfAo);

    aoPass->SetHalfResolution(originalHalf);

    ok = ok && CaptureDepthScreen(context, renderer, depthNdc);

    if (!ok || fullAo.GetSize() != halfAo.GetSize() ||
        fullAo.GetSize() != depthNdc.GetSize() || fullAo.GetSize() == 0)
    {
        LIMX_LOG(LogLaunch, Error, "[光追AO上采样] 回读失败");
        return false;
    }

    const FRHIExtent2D extent = context->GetSwapchainExtent();

    const FCamera& camera = renderer.GetCamera();

    const Float32 nearPlane = camera.GetNearPlane();
    const Float32 farPlane  = camera.GetFarPlane();

    const Float32 a = farPlane / (nearPlane - farPlane);
    const Float32 b = farPlane * nearPlane / (nearPlane - farPlane);

    const auto Linear = [a, b](Float32 ndc) -> Float32
    {
        const Float32 denom = ndc + a;

        return b / ((FMath::Abs(denom) < 1.0e-7f) ? -1.0e-7f : denom);
    };

    // 深度不连续的门限。
    //
    // 双边权重用的是 5% 的相对差, 门限取 10% —— 两倍的距离, 保证被算进
    // "不连续"的像素上双边确实压到了另一侧, 而不是刚好在过渡区里。
    constexpr Float32 kDiscontinuity = 0.10f;

    SizeType edgePixels = 0;
    SizeType flatPixels = 0;

    Float64 edgeSum = 0.0;
    Float64 flatSum = 0.0;

    Float32 edgeWorst = 0.0f;

    for (UInt32 y = 0; y < extent.Height; ++y)
    {
        for (UInt32 x = 0; x < extent.Width; ++x)
        {
            // 偶数像素是原样本的复制 —— 三种权重在那里给出同一个数
            if ((x % 2) == 0 && (y % 2) == 0)
            {
                continue;
            }

            const SizeType index =
                static_cast<SizeType>(y) * extent.Width + x;

            // 上采样取的四个半分辨率样本 —— 对应全分辨率的偶数像素
            const UInt32 bx = x / 2;
            const UInt32 by = y / 2;

            // 中心像素是天空就跳过 —— 那里没有表面要着色, AO 是多少
            // 都不影响画面。
            if (depthNdc[index] >= 0.999999f)
            {
                continue;
            }

            Float32 minDepth = 1.0e30f;
            Float32 maxDepth = 0.0f;

            bool touchesSky = false;

            for (UInt32 dy = 0; dy < 2; ++dy)
            {
                for (UInt32 dx = 0; dx < 2; ++dx)
                {
                    const UInt32 sx =
                        FMath::Min((bx + dx) * 2u, extent.Width - 1u);

                    const UInt32 sy =
                        FMath::Min((by + dy) * 2u, extent.Height - 1u);

                    const SizeType s =
                        static_cast<SizeType>(sy) * extent.Width + sx;

                    // 四个样本里有天空, 是最强的那种不连续 —— 排除它反而
                    // 把最该验的情形排除了: 天空处 AO 恒为 1, 双线性会把
                    // 那个 1 拌进轮廓内侧, 于是物体边上一圈发亮。
                    if (depthNdc[s] >= 0.999999f)
                    {
                        touchesSky = true;
                        continue;
                    }

                    const Float32 linear = Linear(depthNdc[s]);

                    minDepth = FMath::Min(minDepth, linear);
                    maxDepth = FMath::Max(maxDepth, linear);
                }
            }

            const Float32 relative =
                touchesSky
                    ? 1.0f
                    : ((maxDepth > 1.0e-4f)
                           ? ((maxDepth - minDepth) / maxDepth)
                           : 0.0f);

            const Float32 diff = FMath::Abs(halfAo[index] - fullAo[index]);

            if (relative > kDiscontinuity)
            {
                ++edgePixels;
                edgeSum += static_cast<Float64>(diff);
                edgeWorst = FMath::Max(edgeWorst, diff);
            }
            else
            {
                ++flatPixels;
                flatSum += static_cast<Float64>(diff);
            }
        }
    }

    const Float64 edgeMean =
        (edgePixels > 0) ? (edgeSum / static_cast<Float64>(edgePixels)) : 0.0;

    const Float64 flatMean =
        (flatPixels > 0) ? (flatSum / static_cast<Float64>(flatPixels)) : 0.0;

    LIMX_LOG(LogLaunch, Display,
             "[光追AO上采样] 不连续处 {} 个像素 平均差 {} 最大差 {} | "
             "连续处 {} 个像素 平均差 {} (双边正常时两者相当, 见判据处注释)",
             edgePixels, edgeMean, edgeWorst, flatPixels, flatMean);

    bool passed = true;

    // ---- 元判据: 场景里得真有深度不连续 ----
    //
    // 没有的话这条判据是空的 —— 而墙角场景正是这样, 它是这两条逃逸的成因。
    constexpr SizeType kMinEdgePixels = 5000;

    if (edgePixels < kMinEdgePixels)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[光追AO上采样] 只有 {} 个跨越深度不连续的像素 "
                 "(需要至少 {}) —— 这个场景判不了双边加权",
                 edgePixels, kMinEdgePixels);
        passed = false;
    }

    // ---- 判据: 不连续处的平均差要有界 ----
    //
    // 阈值是先量后定的 (综合场景, 1280x720, 每像素十六条射线):
    //
    //   双边 (正确)      0.006633
    //   退化成双线性      0.019440   <- 要红
    //   退化成最近邻      0.006240   <- 见下
    //
    // 取 0.012: 正确值的 1.8 倍, 双线性的 0.6 倍 —— 离两边都不近。
    constexpr Float64 kMaxEdgeMean = 0.012;

    if (edgeMean > kMaxEdgeMean)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[光追AO上采样] 深度不连续处的平均差 {} 超过 {} —— "
                 "双边权重被去掉了? 前景的 AO 正在渗到背景上",
                 edgeMean, kMaxEdgeMean);
        passed = false;
    }

    // ---- 一条**撤掉**的元判据, 与它撤掉的理由 ----
    //
    // 第一版这里还有一条: "不连续处的平均差必须大于连续处的"。想法是
    // 防住"深度门限没选出任何东西"。
    //
    // 它在**正确的实现上就是红的**: 实测不连续处 0.006633, 连续处
    // 0.006831 —— 不连续处反而更小。
    //
    // 而那恰恰是双边加权在做的事: 它把不连续处变成不难的地方。要求
    // "不连续处更难"等于要求这个滤波器失效。一条判据要求被测对象出错,
    // 那不是判据严, 是判据写反了。
    //
    // 留下这段话是因为撤掉之后代码里什么也看不见, 而下一个人多半会
    // 想到同一条"元判据"并再写一遍。
    //
    // 元判据只剩像素数那一条 —— 而那一条是够的: 门限选不出东西时
    // edgePixels 会塌到零。

    if (!renderer.SetRayTracedAoEnabled(false))
    {
        LIMX_LOG(LogLaunch, Error, "[光追AO上采样] 无法关闭光追 AO (复位)");
        passed = false;
    }

    LIMX_LOG(LogLaunch, Display,
             "[光追AO上采样] {}", passed ? "通过" : "失败");

    return passed;
}

// ============================================================================
// RunRayTracingGeometryTableCheck — 几何表按源对象下标索引, 不是实例序号
//
// 这条判据补的是 Day 5 记录在案的那条逃逸。
//
// 命中之后着色器手上只有 instanceCustomIndex, 它拿这个数去几何表里查顶点
// 地址与材质下标。而 FRayTracingScene 写表时有两种写法:
//
//   table[sourceIndex]   —— 源对象列表里的第几个 (对的)
//   table[blasOrdinal]   —— 已经建出来的第几棵 BLAS (错的)
//
// 两者只在**有对象被跳过**时才分开。跳过之后, 后面每个对象的两个下标都
// 差一格 —— 于是所有物体的材质整体错位, 反射里的颜色全串了位。
//
// 而三个测试场景一个都不跳过 (墙角 2/2、综合 33/33、OBJ 3/3), 于是那条
// 变异在画面上、在数值上都毫无痕迹。判据没有覆盖到它, 不是因为判据不够
// 严, 是因为**场景里没有那件事**。
//
// 所以这条判据自己造那件事: 拿真实场景的对象列表, 在中间插一个没有三角形
// 的对象 (IndexCount = 0 —— 点精灵、纯粹的变换节点都是这样), 单独建一份
// 加速结构, 把几何表读回来逐条比对。
//
// 不改渲染中的那份场景 —— 改了的话所有别的判据都要跟着变, 而它们量的是
// 画面。这里要的只是 Update() 这个函数的行为。
// ============================================================================

/// 一个对象**应当**在几何表里留下的那条记录
static FRayTracingGeometryEntry ExpectedGeometryEntry(
    IRHIDevice* device, const FRenderObject& object)
{
    FRayTracingGeometryEntry entry;

    entry.VertexAddress = device->GetBufferDeviceAddress(object.VertexBuffer);

    entry.IndexAddress =
        device->GetBufferDeviceAddress(object.IndexBuffer) +
        static_cast<UInt64>(object.IndexOffset) *
            ((object.IndexType == EIndexType::UInt16) ? 2u : 4u);

    entry.VertexStride  = object.VertexStride;
    entry.IndexType     = (object.IndexType == EIndexType::UInt16) ? 0u : 1u;
    entry.MaterialIndex = object.BindlessMaterialIndex;

    return entry;
}

static bool GeometryEntriesEqual(const FRayTracingGeometryEntry& a,
                                 const FRayTracingGeometryEntry& b)
{
    return a.VertexAddress == b.VertexAddress &&
           a.IndexAddress == b.IndexAddress &&
           a.VertexStride == b.VertexStride &&
           a.IndexType == b.IndexType &&
           a.MaterialIndex == b.MaterialIndex;
}

static bool RunRayTracingGeometryTableCheck(FRenderContext* context,
                                            FRenderer&      renderer)
{
    IRHIDevice* const device = context->GetDevice();

    if (device == nullptr || !device->IsRayTracingSupported())
    {
        LIMX_LOG(LogLaunch, Error,
                 "[光追几何表] 设备不支持光追 — 判据无法执行, 判定为失败");
        return false;
    }

    // 先让渲染器把场景列表建起来
    renderer.RenderFrame();

    const TArray<FRenderObject>& sceneObjects = renderer.GetRenderObjects();

    if (sceneObjects.GetSize() < 3)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[光追几何表] 场景只有 {} 个对象 — 至少要 3 个才能在中间"
                 "插一个被跳过的",
                 sceneObjects.GetSize());
        return false;
    }

    // ---- 造一份"中间有一个会被跳过"的对象列表 ----
    //
    // 插在下标 1: 插在最后的话后面没有对象, 两种写法仍然恒等 —— 那样的
    // 判据看起来在跑, 实际上什么都没验。
    constexpr SizeType kSkipAt = 1;

    TArray<FRenderObject> objects;

    for (SizeType i = 0; i < sceneObjects.GetSize(); ++i)
    {
        if (i == kSkipAt)
        {
            // 没有三角形的对象。IndexCount = 0 是真实存在的情形
            // (点精灵、纯变换节点), 不是构造出来的畸形数据 —— 用一个
            // 畸形到会让 Update 走别的分支的值就验错了东西。
            FRenderObject empty = sceneObjects[i];
            empty.IndexCount    = 0;
            empty.DebugName     = "GeometryTableCheck.Skipped";

            objects.Add(empty);
        }

        objects.Add(sceneObjects[i]);
    }

    // ---- 元判据: 跳过点之后的对象必须彼此可分 ----
    //
    // 错误的写法把 objects[s] 的记录写进 table[s-1]。判据要红, 就得有
    // 至少一个 s 满足 entry(objects[s]) != entry(objects[s-1]) ——
    // 全场景共用一对缓冲区、同一个材质的话, 两种写法写出来的表一模一样,
    // 这条判据就是空的。
    //
    // 这一条不是锦上添花: Day 5 的四个量里有三个正是栽在"场景太均匀"上。
    SizeType distinguishablePairs = 0;

    for (SizeType s = kSkipAt + 1; s < objects.GetSize(); ++s)
    {
        const FRayTracingGeometryEntry here =
            ExpectedGeometryEntry(device, objects[s]);

        const FRayTracingGeometryEntry prev =
            ExpectedGeometryEntry(device, objects[s - 1]);

        if (!GeometryEntriesEqual(here, prev))
        {
            ++distinguishablePairs;
        }
    }

    if (distinguishablePairs == 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[光追几何表] 跳过点之后没有任何一对相邻对象是可分的 —— "
                 "两种索引写法在这个场景里恒等, 这条判据是空的");
        return false;
    }

    // ---- 单独建一份加速结构 ----
    FRayTracingScene scene;

    if (!IsRHISuccess(scene.Initialize(device)))
    {
        LIMX_LOG(LogLaunch, Error, "[光追几何表] 加速结构初始化失败");
        return false;
    }

    bool ok = IsRHISuccess(scene.Update(objects));

    if (!ok)
    {
        LIMX_LOG(LogLaunch, Error, "[光追几何表] Update 失败");
        scene.Shutdown();
        return false;
    }

    // ---- 元判据: 那个对象真的被跳过了 ----
    if (scene.GetSkippedCount() == 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[光追几何表] 一个对象都没被跳过 —— 插进去的那个空对象被"
                 "接受了? 没有跳过就没有错位, 这条判据是空的");
        scene.Shutdown();
        return false;
    }

    const UInt32 expectedBlas =
        static_cast<UInt32>(objects.GetSize()) - scene.GetSkippedCount();

    if (scene.GetBlasCount() != expectedBlas)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[光追几何表] BLAS {} 个, 但对象 {} 个减跳过 {} 个应当是 {}",
                 scene.GetBlasCount(), objects.GetSize(),
                 scene.GetSkippedCount(), expectedBlas);
        ok = false;
    }

    // ---- 把几何表读回来逐条比对 ----
    void* mapped = nullptr;

    if (!IsRHISuccess(device->MapBuffer(scene.GetGeometryTable(), &mapped)) ||
        mapped == nullptr)
    {
        LIMX_LOG(LogLaunch, Error, "[光追几何表] 几何表映射失败");
        scene.Shutdown();
        return false;
    }

    const auto* table = static_cast<const FRayTracingGeometryEntry*>(mapped);

    SizeType checked   = 0;
    SizeType mismatched = 0;
    SizeType firstBad   = 0;

    for (SizeType s = 0; s < objects.GetSize(); ++s)
    {
        // 被跳过的那个对象不写表 —— 那一格里是什么无关紧要,
        // 因为没有任何实例的自定义下标指向它。
        if (s == kSkipAt)
        {
            continue;
        }

        const FRayTracingGeometryEntry expected =
            ExpectedGeometryEntry(device, objects[s]);

        ++checked;

        if (!GeometryEntriesEqual(table[s], expected))
        {
            if (mismatched == 0)
            {
                firstBad = s;
            }

            ++mismatched;
        }
    }

    // 报错要用的那一条先抄出来 —— 解除映射之后 table 就不能再读了
    FRayTracingGeometryEntry firstBadEntry;

    if (mismatched != 0)
    {
        firstBadEntry = table[firstBad];
    }

    device->UnmapBuffer(scene.GetGeometryTable());

    LIMX_LOG(LogLaunch, Display,
             "[光追几何表] 对象 {} 个 (跳过 {}), 表项比对 {} 条, 不符 {} 条, "
             "跳过点之后可分的相邻对 {} 对",
             objects.GetSize(), scene.GetSkippedCount(), checked, mismatched,
             distinguishablePairs);

    if (mismatched != 0)
    {
        const FRayTracingGeometryEntry expected =
            ExpectedGeometryEntry(device, objects[firstBad]);

        LIMX_LOG(LogLaunch, Error,
                 "[光追几何表] {} 条表项与源对象不符 — 第一条是下标 {}: "
                 "表里 顶点地址 {} 索引地址 {} 材质 {}, "
                 "应当是 顶点地址 {} 索引地址 {} 材质 {} "
                 "—— 几何表是按实例序号写的?",
                 mismatched, firstBad,
                 firstBadEntry.VertexAddress, firstBadEntry.IndexAddress,
                 firstBadEntry.MaterialIndex,
                 expected.VertexAddress, expected.IndexAddress,
                 expected.MaterialIndex);
        ok = false;
    }

    scene.Shutdown();

    LIMX_LOG(LogLaunch, Display, "[光追几何表] {}", ok ? "通过" : "失败");

    return ok;
}

// ============================================================================
// RunRayTracedReflectionChecks — 地面反射墙, 三个量都对解析值
//
// 墙角场景里地面是 y=0 平面、墙是 z=0 平面。地面上一点 P 的反射方向是
// R = reflect(normalize(P - C), (0,1,0)), 它命中墙的距离是
//
//     t = -P.z / R.z          (R.z < 0 时)
//
// 这是初中几何, 没有近似。而光追反射要走完的那一串 —— 反投影出 P、取法线、
// 算反射方向、遍历 BVH、拿到重心坐标、按设备地址回到顶点缓冲区、插值法线、
// 用物体到世界矩阵变换 —— 每一步都可能错位, 而错位之后画面上只是"反射里的
// 东西看着不太对"。
//
// 所以判据不看颜色, 看三个能逐像素对的原始量:
//
//     命中距离    与 -P.z/R.z 比           验反射方向与遍历
//     材质下标    与该物体的 bindless 下标比  验几何表的索引
//     命中法线 y  与墙的法线 (0,0,1).y = 0 比 验顶点取回与重心插值
//
// 三个量互相独立: 反射方向算错但几何表对, 距离红而材质绿; 几何表错位但
// 方向对, 材质红而距离绿。一个量对不上说得清是哪一步。
// ============================================================================

namespace
{

/// 光追反射的比对结果
struct FRtReflectionComparison
{
    SizeType FloorPixels    = 0;
    SizeType ComparedPixels = 0;

    /// 命中距离对不上的像素数
    SizeType DistanceMismatch = 0;

    /// 材质下标对不上的
    SizeType MaterialMismatch = 0;

    /// 命中法线对不上的
    SizeType NormalMismatch = 0;

    /// 位置自洽残差超标的
    ///
    /// 由 t 算出的命中点与由取回顶点插出来的命中点必须是同一个点。
    /// 这是几何取回路径上唯一与场景无关的判据。
    SizeType ResidualMismatch = 0;

    /// 该命中却没命中的
    SizeType MissedHits = 0;

    Float32 MaxDistanceError = 0.0f;
    Float32 MaxNormalError   = 0.0f;
    Float32 MaxResidual      = 0.0f;

    /// 见到的材质下标 (取第一个) —— 判据要与渲染对象列表里的对上
    Int32 ObservedMaterial = -1;

    bool Valid = false;
};

static FRtReflectionComparison CaptureRtReflection(
    FRenderContext* context, FRenderer& renderer, UInt32 expectedMaterial)
{
    FRtReflectionComparison result;

    FRayTracedReflectionPass* const pass =
        renderer.GetRayTracedReflectionPass();

    FDepthPrePass* const depth = renderer.GetDepthPrePass();

    if (pass == nullptr || depth == nullptr)
    {
        return result;
    }

    // 调试输出: rgb = (命中距离, 材质下标, 世界法线 y)
    pass->SetDebugOutput(true);

    IRHIDevice* const device = context->GetDevice();

    const FRHIExtent2D extent = context->GetSwapchainExtent();

    const SizeType pixelCount =
        static_cast<SizeType>(extent.Width) * extent.Height;

    FRHIBufferHandle reflectionReadback;
    FRHIBufferHandle normalReadback;

    {
        FRHIBufferDesc desc = {};
        desc.Usage       = EBufferUsage::TransferDst;
        desc.MemoryUsage = EMemoryUsage::GpuToCpu;

        desc.Size      = pixelCount * 16u;   // RGBA32F
        desc.DebugName = "RtReflCheck.Reflection";

        if (!IsRHISuccess(device->CreateBuffer(desc, reflectionReadback)))
        {
            return result;
        }

        desc.Size      = pixelCount * 4u;    // RG16_SFLOAT
        desc.DebugName = "RtReflCheck.Normal";

        if (!IsRHISuccess(device->CreateBuffer(desc, normalReadback)))
        {
            device->DestroyBuffer(reflectionReadback);
            return result;
        }
    }

    const FRHITextureHandle reflectionTexture = pass->GetReflectionTexture();
    const FRHITextureHandle normalTexture     = depth->GetNormalTexture();

    bool recorded = false;

    renderer.SetPostSceneRenderCallback(
        [&recorded, context, reflectionTexture, normalTexture,
         reflectionReadback, normalReadback, extent]()
        {
            IRHICommandBuffer* cmd = context->GetCurrentCommandBuffer();

            if (cmd == nullptr)
            {
                return;
            }

            FRHIBufferTextureCopyRegion region = {};
            region.BufferOffset      = 0;
            region.BufferRowLength   = 0;
            region.BufferImageHeight = 0;
            region.MipLevel          = 0;
            region.BaseLayer         = 0;
            region.LayerCount        = 1;
            region.TextureOffset     = { 0, 0, 0 };
            region.TextureExtent     = { extent.Width, extent.Height, 1 };

            cmd->TransitionImageLayout(
                reflectionTexture,
                EImageLayout::ShaderReadOnly, EImageLayout::TransferSrc,
                EPipelineStageFlags::FragmentShader,
                EPipelineStageFlags::Transfer,
                EAccessFlags::ShaderRead, EAccessFlags::TransferRead);

            cmd->CopyTextureToBuffer(reflectionTexture,
                                     EImageLayout::TransferSrc,
                                     reflectionReadback, region);

            cmd->TransitionImageLayout(
                reflectionTexture,
                EImageLayout::TransferSrc, EImageLayout::ShaderReadOnly,
                EPipelineStageFlags::Transfer,
                EPipelineStageFlags::FragmentShader,
                EAccessFlags::TransferRead, EAccessFlags::ShaderRead);

            cmd->TransitionImageLayout(
                normalTexture,
                EImageLayout::ShaderReadOnly, EImageLayout::TransferSrc,
                EPipelineStageFlags::FragmentShader,
                EPipelineStageFlags::Transfer,
                EAccessFlags::ShaderRead, EAccessFlags::TransferRead);

            cmd->CopyTextureToBuffer(normalTexture, EImageLayout::TransferSrc,
                                     normalReadback, region);

            cmd->TransitionImageLayout(
                normalTexture,
                EImageLayout::TransferSrc, EImageLayout::ShaderReadOnly,
                EPipelineStageFlags::Transfer,
                EPipelineStageFlags::FragmentShader,
                EAccessFlags::TransferRead, EAccessFlags::ShaderRead);

            recorded = true;
        });

    renderer.RenderFrame();
    renderer.SetPostSceneRenderCallback(TFunction<void()>());

    pass->SetDebugOutput(false);

    if (!recorded)
    {
        device->DestroyBuffer(normalReadback);
        device->DestroyBuffer(reflectionReadback);
        return result;
    }

    device->WaitIdle();

    TArray<FVector4> reflection;
    TArray<FVector2> normals;

    {
        void* mapped = nullptr;

        if (IsRHISuccess(device->MapBuffer(reflectionReadback, &mapped)) &&
            mapped != nullptr)
        {
            const auto* src = static_cast<const Float32*>(mapped);

            reflection.Reserve(pixelCount);

            for (SizeType i = 0; i < pixelCount; ++i)
            {
                reflection.Add(FVector4(src[i * 4 + 0], src[i * 4 + 1],
                                        src[i * 4 + 2], src[i * 4 + 3]));
            }

            device->UnmapBuffer(reflectionReadback);
        }

        mapped = nullptr;

        if (IsRHISuccess(device->MapBuffer(normalReadback, &mapped)) &&
            mapped != nullptr)
        {
            const auto* src = static_cast<const Float16Bits*>(mapped);

            normals.Reserve(pixelCount);

            for (SizeType i = 0; i < pixelCount; ++i)
            {
                normals.Add(FVector2(Float16ToFloat32(src[i * 2 + 0]),
                                     Float16ToFloat32(src[i * 2 + 1])));
            }

            device->UnmapBuffer(normalReadback);
        }
    }

    device->DestroyBuffer(normalReadback);
    device->DestroyBuffer(reflectionReadback);

    if (reflection.GetSize() != pixelCount || normals.GetSize() != pixelCount)
    {
        return result;
    }

    // ---- 逐像素比对 ----
    const FCamera& camera = renderer.GetCamera();

    const FMatrix inverse =
        (camera.GetProjectionMatrix() * camera.GetViewMatrix()).Inverse();

    const FVector3 cameraPos = camera.GetPosition();

    for (UInt32 y = 0; y < extent.Height; ++y)
    {
        for (UInt32 x = 0; x < extent.Width; ++x)
        {
            const SizeType index =
                static_cast<SizeType>(y) * extent.Width + x;

            if (FMath::Abs(normals[index].X) > 1.0f ||
                FMath::Abs(normals[index].Y) > 1.0f)
            {
                continue;
            }

            const FVector3 n = DecodeOctahedralNormal(normals[index]);

            // 只取朝上的地面像素
            if (n.Y < 0.999f)
            {
                continue;
            }

            ++result.FloorPixels;

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

            FVector3 dir(farWorld.X / farWorld.W - cameraPos.X,
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

            const FVector3 surface(cameraPos.X + dir.X * t,
                                   0.0f,
                                   cameraPos.Z + dir.Z * t);

            // 视线方向 (归一化) 与反射方向
            const FVector3 view = FVector3(dir.X, dir.Y, dir.Z).GetSafeNormal();

            // reflect(V, N) 对 N = (0,1,0) 就是把 y 取反
            const FVector3 reflectDir(view.X, -view.Y, view.Z);

            // 反射线要朝墙走 (z 减小) 才可能命中
            if (reflectDir.Z > -1.0e-3f)
            {
                continue;
            }

            const Float32 expectedT = -surface.Z / reflectDir.Z;

            if (expectedT <= 0.0f)
            {
                continue;
            }

            // 命中点必须落在墙的范围内 (20x20 的板, 中心 y=10)
            const Float32 hitY = surface.Y + reflectDir.Y * expectedT;
            const Float32 hitX = surface.X + reflectDir.X * expectedT;

            constexpr Float32 kHalfExtent = 10.0f;
            constexpr Float32 kEdgeMargin = 2.0f;

            if (hitY < kEdgeMargin ||
                hitY > 2.0f * kHalfExtent - kEdgeMargin ||
                FMath::Abs(hitX) > kHalfExtent - kEdgeMargin)
            {
                continue;
            }

            // 地面自身的范围也要够远离边缘
            if (FMath::Abs(surface.X) > kHalfExtent - kEdgeMargin ||
                surface.Z < 0.5f ||
                surface.Z > kHalfExtent - kEdgeMargin)
            {
                continue;
            }

            ++result.ComparedPixels;

            const FVector4& value = reflection[index];

            const Float32 measuredT   = value.X;
            const Float32 measuredMat = value.Y;
            const Float32 measuredRes = value.Z;

            // 法线 y 被平移了 2 —— 好与"未命中"的 -1 分开
            const Float32 measuredNY  = value.W - 2.0f;

            if (value.W < 0.0f)
            {
                // 该命中却没命中
                ++result.MissedHits;
                continue;
            }

            if (result.ObservedMaterial < 0)
            {
                result.ObservedMaterial =
                    static_cast<Int32>(measuredMat + 0.5f);
            }

            const Float32 distanceError =
                FMath::Abs(measuredT - expectedT);

            result.MaxDistanceError =
                FMath::Max(result.MaxDistanceError, distanceError);

            // 容差: 反射方向由深度反投影得到, 而深度是量化的。命中点越远,
            // 方向上的一点点误差被放得越大 —— 所以容差按距离成比例。
            if (distanceError > expectedT * 2.0e-3f + 1.0e-3f)
            {
                ++result.DistanceMismatch;
            }

            if (static_cast<UInt32>(measuredMat + 0.5f) != expectedMaterial)
            {
                ++result.MaterialMismatch;
            }

            // 墙的法线是 (0,0,1), y 分量为 0
            const Float32 normalError = FMath::Abs(measuredNY);

            result.MaxNormalError =
                FMath::Max(result.MaxNormalError, normalError);

            if (normalError > 1.0e-3f)
            {
                ++result.NormalMismatch;
            }

            result.MaxResidual = FMath::Max(result.MaxResidual, measuredRes);

            // 残差的容差按场景尺度定: 墙角场景是 20x20 的板, 单精度在那个
            // 量级上的相对误差是 1e-7, 乘上几十的坐标值就是 1e-5 量级。
            // 取 1e-3 留了两个数量级, 而取错顶点造成的残差是**几个世界
            // 单位** —— 差三个数量级。
            if (measuredRes > 1.0e-3f)
            {
                ++result.ResidualMismatch;
            }
        }
    }

    result.Valid = (result.ComparedPixels > 0);

    return result;
}

} // namespace

namespace
{

/// 与场景无关的反射自洽检查
///
/// 墙角场景能给出解析值, 但它太均匀: 两块平面上九个顶点的法线完全相同,
/// 而两个物体都没有子网格、一个都没被跳过。于是"取错顶点""取错索引"
/// "几何表按实例序号写"这几类错误在那里全部无声。
///
/// 这一段不要解析值, 只要**自洽**:
///
///   由 t 算出的命中点  ==  由取回的三个顶点按重心坐标插出来的命中点
///
/// 这个等式在任何场景上都成立, 而顶点、索引、跨度、几何表地址里任何一处
/// 取错, 两边就分开。于是它可以跑在 OBJ 测试场景上 —— 那里六个子网格共用
/// 一对缓冲区, 索引字节偏移是 0/768/840/912/984/1080, 正是墙角场景没有的
/// 那一维。
struct FRtReflectionSelfCheck
{
    SizeType HitPixels        = 0;
    SizeType ResidualMismatch = 0;
    SizeType MaterialOutOfRange = 0;

    Float32 MaxResidual = 0.0f;

    /// 见过的不同材质下标个数 —— 元判据要用
    SizeType DistinctMaterials = 0;
};

static bool RunReflectionSelfConsistency(FRenderContext* context,
                                         FRenderer&      renderer,
                                         const AnsiChar* sceneName)
{
    FRayTracedReflectionPass* const pass =
        renderer.GetRayTracedReflectionPass();

    if (pass == nullptr)
    {
        return false;
    }

    pass->SetDebugOutput(true);

    IRHIDevice* const device = context->GetDevice();

    const FRHIExtent2D extent = context->GetSwapchainExtent();

    const SizeType pixelCount =
        static_cast<SizeType>(extent.Width) * extent.Height;

    FRHIBufferHandle readback;

    {
        FRHIBufferDesc desc = {};
        desc.Size        = pixelCount * 16u;
        desc.Usage       = EBufferUsage::TransferDst;
        desc.MemoryUsage = EMemoryUsage::GpuToCpu;
        desc.DebugName   = "RtReflSelf.Readback";

        if (!IsRHISuccess(device->CreateBuffer(desc, readback)))
        {
            return false;
        }
    }

    const FRHITextureHandle texture = pass->GetReflectionTexture();

    bool recorded = false;

    renderer.SetPostSceneRenderCallback(
        [&recorded, context, texture, readback, extent]()
        {
            IRHICommandBuffer* cmd = context->GetCurrentCommandBuffer();

            if (cmd == nullptr)
            {
                return;
            }

            FRHIBufferTextureCopyRegion region = {};
            region.BufferOffset      = 0;
            region.BufferRowLength   = 0;
            region.BufferImageHeight = 0;
            region.MipLevel          = 0;
            region.BaseLayer         = 0;
            region.LayerCount        = 1;
            region.TextureOffset     = { 0, 0, 0 };
            region.TextureExtent     = { extent.Width, extent.Height, 1 };

            cmd->TransitionImageLayout(
                texture, EImageLayout::ShaderReadOnly,
                EImageLayout::TransferSrc,
                EPipelineStageFlags::FragmentShader,
                EPipelineStageFlags::Transfer,
                EAccessFlags::ShaderRead, EAccessFlags::TransferRead);

            cmd->CopyTextureToBuffer(texture, EImageLayout::TransferSrc,
                                     readback, region);

            cmd->TransitionImageLayout(
                texture, EImageLayout::TransferSrc,
                EImageLayout::ShaderReadOnly,
                EPipelineStageFlags::Transfer,
                EPipelineStageFlags::FragmentShader,
                EAccessFlags::TransferRead, EAccessFlags::ShaderRead);

            recorded = true;
        });

    renderer.RenderFrame();
    renderer.SetPostSceneRenderCallback(TFunction<void()>());

    pass->SetDebugOutput(false);

    FRtReflectionSelfCheck stats;

    bool ok = recorded;

    if (ok)
    {
        device->WaitIdle();

        void* mapped = nullptr;

        if (IsRHISuccess(device->MapBuffer(readback, &mapped)) &&
            mapped != nullptr)
        {
            const auto* src = static_cast<const Float32*>(mapped);

            // 材质下标见过哪些 —— 数量级很小, 用一个位图就够
            constexpr UInt32 kMaterialBits = 256;
            bool seen[kMaterialBits] = {};

            for (SizeType i = 0; i < pixelCount; ++i)
            {
                const Float32 residual = src[i * 4 + 2];
                const Float32 flag     = src[i * 4 + 3];

                // 未命中 (-1) 与"几何表条目为空"(那一路写的是洋红色) 跳过
                if (flag < 0.0f)
                {
                    continue;
                }

                ++stats.HitPixels;

                stats.MaxResidual = FMath::Max(stats.MaxResidual, residual);

                if (residual > 1.0e-2f)
                {
                    ++stats.ResidualMismatch;
                }

                const Int32 material =
                    static_cast<Int32>(src[i * 4 + 1] + 0.5f);

                if (material < 0 ||
                    material >= static_cast<Int32>(kMaterialBits))
                {
                    ++stats.MaterialOutOfRange;
                }
                else if (!seen[material])
                {
                    seen[material] = true;
                    ++stats.DistinctMaterials;
                }
            }

            device->UnmapBuffer(readback);
        }
        else
        {
            ok = false;
        }
    }

    device->DestroyBuffer(readback);

    if (!ok)
    {
        LIMX_LOG(LogLaunch, Error, "[光追反射] {} 场景的回读失败", sceneName);
        return false;
    }

    LIMX_LOG(LogLaunch, Display,
             "[光追反射] {} 场景自洽 — 命中 {} 像素, 残差超标 {}, "
             "最大残差 {}, 材质越界 {}, 见到 {} 种材质",
             sceneName, stats.HitPixels, stats.ResidualMismatch,
             stats.MaxResidual, stats.MaterialOutOfRange,
             stats.DistinctMaterials);

    bool passed = true;

    // ---- 元判据: 得有足够多的命中 ----
    //
    // 一条反射射线都没命中的话下面每一条都自动成立。
    constexpr SizeType kMinHits = 5000;

    if (stats.HitPixels < kMinHits)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[光追反射] {} 场景只有 {} 个命中像素 (需要至少 {})",
                 sceneName, stats.HitPixels, kMinHits);
        passed = false;
    }

    // ---- 自洽 ----
    if (stats.ResidualMismatch != 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[光追反射] {} 场景有 {} 个像素的位置自洽残差超标 "
                 "(最大 {}) —— 顶点、索引或几何表的地址取错了",
                 sceneName, stats.ResidualMismatch, stats.MaxResidual);
        passed = false;
    }

    if (stats.MaterialOutOfRange != 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[光追反射] {} 场景有 {} 个像素的材质下标越界",
                 sceneName, stats.MaterialOutOfRange);
        passed = false;
    }

    return passed;
}

} // namespace

// ── 已知的覆盖边界 (量过) ──────────────────────────────────────────────
//
// 变异验证 9/10 (两个场景合起来)。唯一逃逸:
//
//   **几何表的条目按实例序号写而不是源对象下标** —— 两个场景里都没有任何
//   对象被跳过 (FRayTracingScene::GetSkippedCount() 恒为 0), 于是源下标与
//   实例序号恒等。要验它需要一个"有对象因几何体无效而被跳过"的场景, 而
//   现有的六个场景里一个都没有。
//
//   这一条错了的后果是: 只要有一个对象被跳过, 它之后所有物体在反射里查到
//   的都是**前一个物体的材质与几何** —— 而画面上只是"反射里的材质有点怪"。
//
// 两个场景的分工 (缺一不可):
//   墙角场景  给得出解析值 (命中距离 = -P.z/R.z, 法线 = (0,0,1)), 但两块
//             平面上九个顶点法线完全相同、没有子网格 —— "取错顶点""取错
//             索引""重心权重算错"三条在它上面全部无声。
//   OBJ 场景  六个子网格共用一对缓冲区, 索引字节偏移 0/768/840/912/984/1080
//             —— 唯一能验到"索引地址漏加偏移"的场景。它没有解析值, 只跑
//             与场景无关的自洽检查。
static bool RunRayTracedReflectionChecks(FRenderContext* context,
                                         FRenderer&      renderer)
{
    if (!renderer.SetRayTracedReflectionEnabled(true))
    {
        LIMX_LOG(LogLaunch, Error,
                 "[光追反射] 无法启用 — 判据无法执行, 判定为失败");
        return false;
    }

    // 墙的材质下标 —— 从渲染对象列表里取, 不写死。
    //
    // 写死的话, 场景改了材质注册顺序之后这条判据会指向另一个材质, 而它
    // 报的会是"材质下标对不上", 指向完全错误的方向。
    const TArray<FRenderObject>& objects = renderer.GetShadowCasterObjects();

    if (objects.GetSize() < 2)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[光追反射] 场景里只有 {} 个物体 —— 墙角场景应当有两块板",
                 objects.GetSize());
        return false;
    }

    // 墙角场景: [0] 是地面, [1] 是墙 (见 BuildCornerScene)
    const UInt32 floorMaterial = objects[0].BindlessMaterialIndex;
    const UInt32 wallMaterial  = objects[1].BindlessMaterialIndex;

    // 元判据: 两块板的材质下标必须不同。
    //
    // 相同的话"命中处的材质下标"这一条就是空的 —— 无论几何表怎么错位,
    // 查到的都是同一个数, 而那是个满分通过。
    if (floorMaterial == wallMaterial)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[光追反射] 地面与墙的材质下标都是 {} —— "
                 "材质那一条判据无从判定",
                 wallMaterial);
        return false;
    }

    const FRtReflectionComparison cmp =
        CaptureRtReflection(context, renderer, wallMaterial);

    if (!cmp.Valid)
    {
        LIMX_LOG(LogLaunch, Error, "[光追反射] 采集失败");
        return false;
    }

    LIMX_LOG(LogLaunch, Display,
             "[光追反射] 比对 {} 像素 (地面 {}) — 距离不符 {} 材质不符 {} "
             "法线不符 {} 残差超标 {} 漏命中 {} | 最大: 距离 {} 法线 {} "
             "残差 {} | 材质下标 实测 {} 预期 {}",
             cmp.ComparedPixels, cmp.FloorPixels,
             cmp.DistanceMismatch, cmp.MaterialMismatch,
             cmp.NormalMismatch, cmp.ResidualMismatch, cmp.MissedHits,
             cmp.MaxDistanceError, cmp.MaxNormalError, cmp.MaxResidual,
             cmp.ObservedMaterial, wallMaterial);

    bool passed = true;

    // ---- 元判据: 比对的像素要够多 ----
    constexpr SizeType kMinPixels = 20000;

    if (cmp.ComparedPixels < kMinPixels)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[光追反射] 只比对了 {} 个像素 (需要至少 {}) —— "
                 "相机是不是没看到地面反射墙的那一片?",
                 cmp.ComparedPixels, kMinPixels);
        passed = false;
    }

    // ---- 判据 1: 该命中的都要命中 ----
    //
    // 解析上确定会打到墙的射线一条都不该落空。落空说明遍历、掩码或
    // 反射方向里有一处错了。
    const Float32 missFraction =
        (cmp.ComparedPixels > 0)
            ? static_cast<Float32>(cmp.MissedHits) /
              static_cast<Float32>(cmp.ComparedPixels)
            : 1.0f;

    if (missFraction > 1.0e-3f)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[光追反射] {} / {} 条该命中的射线落空了 ({}%)",
                 cmp.MissedHits, cmp.ComparedPixels, missFraction * 100.0f);
        passed = false;
    }

    // ---- 判据 2: 命中距离 ----
    const Float32 distanceFraction =
        (cmp.ComparedPixels > 0)
            ? static_cast<Float32>(cmp.DistanceMismatch) /
              static_cast<Float32>(cmp.ComparedPixels)
            : 1.0f;

    if (distanceFraction > 1.0e-3f)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[光追反射] {} / {} 个像素的命中距离与 -P.z/R.z 对不上 "
                 "({}%), 最大误差 {}",
                 cmp.DistanceMismatch, cmp.ComparedPixels,
                 distanceFraction * 100.0f, cmp.MaxDistanceError);
        passed = false;
    }

    // ---- 判据 3: 材质下标 ----
    //
    // 一个像素都不能错 —— 材质下标是整数, 没有"接近"这回事。
    if (cmp.MaterialMismatch != 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[光追反射] {} 个像素的材质下标不是墙的 {} —— "
                 "几何表的索引错位了",
                 cmp.MaterialMismatch, wallMaterial);
        passed = false;
    }

    // ---- 判据 4: 命中法线 ----
    //
    // 墙的法线是 (0,0,1), y 分量恒为 0。这一条验的是"顶点取回来了、
    // 重心插值对了、物体到世界的变换用对了" —— 三者任一错位, 法线都不会
    // 恰好落在 y=0 上。
    const Float32 normalFraction =
        (cmp.ComparedPixels > 0)
            ? static_cast<Float32>(cmp.NormalMismatch) /
              static_cast<Float32>(cmp.ComparedPixels)
            : 1.0f;

    if (normalFraction > 1.0e-3f)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[光追反射] {} / {} 个像素的命中法线 y 不为零 ({}%), "
                 "最大 {} —— 顶点取回或重心插值错位",
                 cmp.NormalMismatch, cmp.ComparedPixels,
                 normalFraction * 100.0f, cmp.MaxNormalError);
        passed = false;
    }

    // ---- 判据 5: 位置自洽 ----
    //
    // 由 t 算出的命中点与由取回顶点插出来的命中点必须是同一个点。
    //
    // 这一条是几何取回路径上**唯一与场景无关**的判据: 法线那一条依赖场景
    // 里的法线有变化, 而墙角场景的两块平面上九个顶点法线完全相同 —— 取错
    // 顶点、取错索引、重心权重算错, 插出来的法线都一样。位置不一样。
    const Float32 residualFraction =
        (cmp.ComparedPixels > 0)
            ? static_cast<Float32>(cmp.ResidualMismatch) /
              static_cast<Float32>(cmp.ComparedPixels)
            : 1.0f;

    if (residualFraction > 1.0e-3f)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[光追反射] {} / {} 个像素的位置自洽残差超标 ({}%), "
                 "最大 {} —— 顶点、索引或几何表的地址取错了",
                 cmp.ResidualMismatch, cmp.ComparedPixels,
                 residualFraction * 100.0f, cmp.MaxResidual);
        passed = false;
    }

    // ---- 判据 6: 同一场景上再跑一遍与场景无关的自洽检查 ----
    //
    // 上面五条都依赖墙角场景的解析值。这一条不依赖 —— 它验的是"由 t 算出的
    // 命中点与由取回顶点插出来的命中点是同一个", 而那个等式在任何场景上都
    // 成立。把它单独拿出来是为了能**跑在别的场景上** (见 --rt-reflection-self)。
    if (!RunReflectionSelfConsistency(context, renderer, "墙角"))
    {
        passed = false;
    }

    LIMX_LOG(LogLaunch, Display, "[光追反射] {}", passed ? "通过" : "失败");

    return passed;
}

// ============================================================================
// RunRayTracedReflectionSelfCheck — 只跑与场景无关的那一段
//
// 墙角场景给得出解析值, 但它太均匀: 两块平面上九个顶点法线完全相同, 两个
// 物体都没有子网格、一个都没被跳过。而 OBJ 测试场景恰好相反 —— 六个子网格
// 共用一对缓冲区, 索引字节偏移 0/768/840/912/984/1080。
//
// 两个场景是互补的, 而这个入口让自洽那一段能跑在后者上。
// ============================================================================
static bool RunRayTracedReflectionSelfCheck(FRenderContext* context,
                                            FRenderer&      renderer)
{
    if (!renderer.SetRayTracedReflectionEnabled(true))
    {
        LIMX_LOG(LogLaunch, Error,
                 "[光追反射] 无法启用 — 判据无法执行, 判定为失败");
        return false;
    }

    const bool passed =
        RunReflectionSelfConsistency(context, renderer, "OBJ 子网格");

    LIMX_LOG(LogLaunch, Display,
             "[光追反射] 自洽检查 {}", passed ? "通过" : "失败");

    return passed;
}

// ============================================================================
// RunRayTracedShadowChecks — 光追阴影的边界落在解析位置上
//
// 阴影贴图那一条判据 (--shadow-check) 问的是"影子边界落在相似三角形算出的
// 位置附近吗", 容差里塞着深度偏置、图集分辨率、PCF 半径三样东西。光追这一条
// 问的是同一件事, 但那三样东西**一样都没有** —— 所以它的容差应当小得多,
// 而"小得多"本身就是一条可判定的性质。
//
// 用的是同一个 FindShadowSpan、同一条扫描线、同一组解析常量。两条判据的差别
// 只在输入 (一张是着色后的画面, 一张是可见度掩码), 于是量出来的差别就只能是
// 阴影本身的差别, 不是测量方法的差别。
// ============================================================================

/// 把 R8 可见度掩码读回来, 展开成 RGB —— 好让 SampleWorldPoint 原样可用
///
/// 展开而不是另写一份采样函数: 判据要与阴影贴图那一条**逐字**共用测量代码,
/// 否则两边量出来的差里会混进"测量方式不同"这一项, 而那一项说不清楚。
static bool ReadShadowMaskAsRgb(FRenderContext* context, FRenderer& renderer,
                                TArray<UInt8>& outRgb)
{
    FRayTracedShadowPass* const pass = renderer.GetRayTracedShadowPass();

    if (pass == nullptr || !pass->IsEnabled())
    {
        LIMX_LOG(LogLaunch, Error, "[光追阴影] 通道未启用");
        return false;
    }

    IRHIDevice* const device = context->GetDevice();

    const FRHIExtent2D extent = context->GetSwapchainExtent();

    const SizeType pixelCount =
        static_cast<SizeType>(extent.Width) * extent.Height;

    FRHIBufferHandle readback;

    {
        FRHIBufferDesc desc = {};
        desc.Size        = pixelCount;   // R8
        desc.Usage       = EBufferUsage::TransferDst;
        desc.MemoryUsage = EMemoryUsage::GpuToCpu;
        desc.DebugName   = "RtShadow.Readback";

        if (!IsRHISuccess(device->CreateBuffer(desc, readback)))
        {
            return false;
        }
    }

    const FRHITextureHandle maskTexture = pass->GetShadowMaskTexture();

    bool recorded = false;

    renderer.SetPostSceneRenderCallback(
        [&recorded, context, maskTexture, readback, extent]()
        {
            IRHICommandBuffer* cmd = context->GetCurrentCommandBuffer();

            if (cmd == nullptr)
            {
                return;
            }

            FRHIBufferTextureCopyRegion region = {};
            region.BufferOffset      = 0;
            region.BufferRowLength   = 0;
            region.BufferImageHeight = 0;
            region.MipLevel          = 0;
            region.BaseLayer         = 0;
            region.LayerCount        = 1;
            region.TextureOffset     = { 0, 0, 0 };
            region.TextureExtent     = { extent.Width, extent.Height, 1 };

            // 掩码此刻停在 ShaderReadOnly (光追阴影通道的收尾转换)
            cmd->TransitionImageLayout(
                maskTexture,
                EImageLayout::ShaderReadOnly, EImageLayout::TransferSrc,
                EPipelineStageFlags::FragmentShader,
                EPipelineStageFlags::Transfer,
                EAccessFlags::ShaderRead, EAccessFlags::TransferRead);

            cmd->CopyTextureToBuffer(maskTexture, EImageLayout::TransferSrc,
                                     readback, region);

            cmd->TransitionImageLayout(
                maskTexture,
                EImageLayout::TransferSrc, EImageLayout::ShaderReadOnly,
                EPipelineStageFlags::Transfer,
                EPipelineStageFlags::FragmentShader,
                EAccessFlags::TransferRead, EAccessFlags::ShaderRead);

            recorded = true;
        });

    renderer.RenderFrame();
    renderer.SetPostSceneRenderCallback(TFunction<void()>());

    bool ok = recorded;

    if (ok)
    {
        device->WaitIdle();

        void* mapped = nullptr;

        if (IsRHISuccess(device->MapBuffer(readback, &mapped)) &&
            mapped != nullptr)
        {
            const auto* src = static_cast<const UInt8*>(mapped);

            outRgb.Clear();
            outRgb.Reserve(pixelCount * 3u);

            for (SizeType i = 0; i < pixelCount; ++i)
            {
                outRgb.Add(src[i]);
                outRgb.Add(src[i]);
                outRgb.Add(src[i]);
            }

            device->UnmapBuffer(readback);
        }
        else
        {
            ok = false;
        }
    }

    device->DestroyBuffer(readback);

    return ok;
}

// ── 已知的覆盖边界 (量过, 不是"没想到") ────────────────────────────────
//
// 变异验证 8/10, 两条逃逸的成因都查清了:
//
// 1. **tMax 用光源衰减距离而不是到光源的距离** —— 逃逸。
//    危险在于"光源背后的几何体也来投影", 而阴影场景里 z>6 处什么都没有
//    (灯在 z=6, 相机在 z=10 但相机不是几何体)。要验它需要一个灯背后有
//    东西的场景。
//
// 2. **tMin 从 1e-3 放大到 0.1** —— 逃逸, 而且这是**理论上就该逃**的。
//    上个周期推过: 沿光线方向的偏置不会移动影子边界 —— 起点沿射线推进
//    多少, 在相似三角形里完全抵消。这里独立验到了同一件事。
//    对照组: **法线偏移**从 1e-3 放大到 0.1 被抓住了 —— 法线偏置会移动
//    边界。两条放在一起, 判据恰好分开了"影响边界的偏置"与"不影响的偏置",
//    而那正是阴影偏置这件事的核心。
//    tMin 过大真正的危害是接触阴影漏光 (遮挡物贴着接收面时), 要验它需要
//    一个遮挡物离墙不到 0.1 的场景。
static bool RunRayTracedShadowChecks(FRenderContext* context,
                                     FRenderer& renderer)
{
    if (!renderer.SetRayTracedShadowsEnabled(true))
    {
        LIMX_LOG(LogLaunch, Error,
                 "[光追阴影] 无法启用 — 判据无法执行, 判定为失败");
        return false;
    }

    TArray<UInt8> mask;

    if (!ReadShadowMaskAsRgb(context, renderer, mask))
    {
        LIMX_LOG(LogLaunch, Error, "[光追阴影] 掩码回读失败");
        return false;
    }

    const FRHIExtent2D extent = context->GetSwapchainExtent();

    const FMatrix viewProj = renderer.GetCamera().GetProjectionMatrix() *
                             renderer.GetCamera().GetViewMatrix();

    // ---- 解析值 —— 与 --shadow-check 逐字一致 ----
    //
    // 刻意共用同一组式子与同一条扫描线: 两条判据的差别只应该在**输入**
    // (一张是着色后的画面, 一张是可见度掩码), 于是量出来的差就只能是阴影
    // 本身的差, 不是测量方法的差。
    const Float32 scaleFront = ShadowScene::ShadowScaleFront();
    const Float32 scaleBack  = ShadowScene::ShadowScaleBack();

    const Float32 expectedXMax =  ShadowScene::kOccluderHalfX * scaleFront;
    const Float32 expectedXMin = -expectedXMax;

    const Float32 occluderYMin =
        ShadowScene::kOccluderCenterY - ShadowScene::kOccluderHalfY;

    const Float32 expectedYMin = occluderYMin * scaleFront;

    const Float32 imageFront = ShadowScene::ImageScale(
        ShadowScene::kOccluderZ + ShadowScene::kOccluderHalfThick);

    const Float32 imageYMin = occluderYMin * imageFront;

    const Float32 scanY = (expectedYMin + imageYMin) * 0.5f;

    const Float32 usableRadius = ShadowScene::InnerConeRadiusAtWall() * 0.9f;

    // ---- 扫描 ----
    //
    // 掩码是二值的 (0 或 255), 所以阈值取中点最稳 —— 不存在渐变区。
    const FShadowSpan span = FindShadowSpan(
        mask, extent.Width, extent.Height, viewProj,
        0, scanY, -usableRadius, usableRadius, 0.004f, 0.5f);

    if (!span.Found)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[光追阴影] 扫描线 y={} 上没找到完整的暗区 —— 亮 {} 暗 {}",
                 scanY, span.LitLevel, span.ShadowLevel);
        return false;
    }

    const Float32 errorMin = FMath::Abs(span.Enter - expectedXMin);
    const Float32 errorMax = FMath::Abs(span.Exit  - expectedXMax);

    // 一个像素在墙上覆盖多少世界宽度 —— 容差的下限就是它
    const Float32 pixelWorldWidth =
        usableRadius * 2.0f / static_cast<Float32>(extent.Width);

    LIMX_LOG(LogLaunch, Display,
             "[光追阴影] 扫描线 y={} — 影子 [{}, {}] 解析 [{}, {}] | "
             "误差 {} / {} | 一个像素 {} 世界单位 | 亮 {} 暗 {}",
             scanY, span.Enter, span.Exit, expectedXMin, expectedXMax,
             errorMin, errorMax, pixelWorldWidth,
             span.LitLevel, span.ShadowLevel);

    bool passed = true;

    // ---- 判据 1: 边界落在解析位置上 ----
    //
    // 容差 = **两个像素**。就这么一项。
    //
    // 板子的厚度不进容差: 影子的外缘由靠灯那一面的轮廓决定, 而解析值
    // 用的正是那一面 (scaleFront)。厚度只影响内缘, 与这里量的两条外缘
    // 无关 —— 把它算进容差是白白放宽 24 倍。
    //
    // 偏置也不进容差: 光追这里没有深度偏置, 而法线偏移 1e-3 沿墙面法线
    // (指向相机, 与光线近乎同向) 推, 对边界位置的影响是二阶的。
    //
    // 于是剩下的唯一不确定度就是"掩码是逐像素的": 边界最细只能定位到一个
    // 像素, 取两个给扫描的线性插值留一点。
    //
    // 实测 (阴影场景, 1280x720, 一个像素 0.00487 世界单位):
    //     光追      误差 0.00153 / 0.00191    容差 0.00974
    //     阴影贴图  误差 0.01419 / 0.01294    容差 0.07273
    //
    // 同一条扫描线、同一个解析值, 光追准八倍 —— 而阴影贴图那条判据的容差
    // 里塞着深度偏置与图集分辨率, 光追这里两样都不存在。这条判据存在的
    // 意义就是把那个差别钉死: 容差一旦放宽到阴影贴图那个量级, 它就不再
    // 是在验光追了。
    const Float32 tolerance = pixelWorldWidth * 2.0f;

    if (errorMin > tolerance || errorMax > tolerance)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[光追阴影] 边界偏离解析位置 —— 左 {} 右 {}, 容差 {} "
                 "(两个像素)",
                 errorMin, errorMax, tolerance);
        passed = false;
    }

    // ---- 判据 2: 掩码必须是二值的 ----
    //
    // 可见度只有 0 与 1 两种取值。出现中间值说明读到的不是这张掩码,
    // 或者它根本没被写过 —— 而"没被写过"与"这里没有影子"在别的判据上
    // 分不开。
    if (span.LitLevel < 250.0f || span.ShadowLevel > 5.0f)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[光追阴影] 掩码不是二值的 —— 亮 {} 暗 {} (期望 255 与 0)",
                 span.LitLevel, span.ShadowLevel);
        passed = false;
    }

    // ---- 判据 3: 影子宽度 ----
    //
    // 只判两个边界的话, 整条影子平移一段仍可能两边都在容差内 (左偏左、
    // 右也偏左)。宽度是独立的一维。
    const Float32 measuredWidth = span.Exit - span.Enter;
    const Float32 expectedWidth = expectedXMax - expectedXMin;

    if (FMath::Abs(measuredWidth - expectedWidth) > tolerance * 2.0f)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[光追阴影] 影子宽度 {} 与解析值 {} 不符 (容差 {})",
                 measuredWidth, expectedWidth, tolerance * 2.0f);
        passed = false;
    }

    // ---- 判据 4: 掩码必须真的接进了着色 ----
    //
    // 前三条验的都是掩码本身。掩码算得再对, 只要它没被着色阶段读到,
    // 画面上的影子仍然是阴影贴图那一版 —— 而那时前三条全部满分通过。
    //
    // 这一条抓的正是那个缺口: 抓一帧**着色后的画面**, 用同一条扫描线量
    // 同一个边界。光追接上了的话误差应当远小于阴影贴图那一版。
    {
        FScreenshotCapture shot;

        if (!shot.Request(context))
        {
            LIMX_LOG(LogLaunch, Error, "[光追阴影] 画面回读缓冲区准备失败");
            return false;
        }

        renderer.SetPostSceneRenderCallback(
            [&shot, context]() { shot.RecordCopy(context); });

        renderer.RenderFrame();
        renderer.SetPostSceneRenderCallback(TFunction<void()>());

        TArray<UInt8> pixels;

        const bool read = shot.ReadPixels(context, pixels);

        shot.Release(context->GetDevice());

        if (!read)
        {
            LIMX_LOG(LogLaunch, Error, "[光追阴影] 画面回读失败");
            return false;
        }

        const FShadowSpan shaded = FindShadowSpan(
            pixels, extent.Width, extent.Height, viewProj,
            0, scanY, -usableRadius, usableRadius, 0.004f, 0.5f);

        if (!shaded.Found)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[光追阴影] 着色后的画面上没找到完整的暗区");
            return false;
        }

        const Float32 shadedErrorMin =
            FMath::Abs(shaded.Enter - expectedXMin);
        const Float32 shadedErrorMax =
            FMath::Abs(shaded.Exit  - expectedXMax);

        // 一个像素。
        //
        // 着色后的画面比二值掩码还准 —— 影子边界在画面上是一段亮度渐变,
        // 扫描的线性插值能在一个像素之内定位它, 而掩码只有 0 与 255 两档。
        //
        // 实测:
        //     着色后 (光追)   误差 0.00081 / 0.00043
        //     掩码   (光追)   误差 0.00153 / 0.00191
        //     着色后 (阴影贴图) 误差 0.01419 / 0.01294
        //
        // 一个像素 (0.00487) 对光追是六倍余量, 而阴影贴图那一版**过不去**
        // —— 这正是这条判据要的: 掩码没接进着色的话它必然变红。
        const Float32 shadedTolerance = pixelWorldWidth;

        LIMX_LOG(LogLaunch, Display,
                 "[光追阴影] 着色后的画面 — 影子 [{}, {}] 误差 {} / {} "
                 "(容差 {} = 一个像素)",
                 shaded.Enter, shaded.Exit,
                 shadedErrorMin, shadedErrorMax, shadedTolerance);

        if (shadedErrorMin > shadedTolerance ||
            shadedErrorMax > shadedTolerance)
        {
            LIMX_LOG(LogLaunch, Error,
                     "[光追阴影] 着色后的影子边界偏离解析位置 —— "
                     "掩码是不是没接进着色?");
            passed = false;
        }
    }

    LIMX_LOG(LogLaunch, Display,
             "[光追阴影] {}", passed ? "通过" : "失败");

    return passed;
}

static bool RunAoEdgeChecks(FRenderContext* context, FRenderer& renderer)
{
    FGtaoPass* const gtao = renderer.GetGtaoPass();

    if (gtao == nullptr || !gtao->IsEnabled())
    {
        LIMX_LOG(LogLaunch, Error,
                 "[AO边缘] GTAO 未启用 — 自检无从判定 (加 --gtao)");
        return false;
    }

    const bool originalHalf = gtao->IsHalfResolution();

    TArray<Float32> full;
    TArray<Float32> half;
    TArray<Float32> depth;
    TArray<Float32> depthIgnored;

    gtao->SetHalfResolution(false);

    if (!ReadAoAndDepth(context, renderer, full, depth))
    {
        gtao->SetHalfResolution(originalHalf);
        LIMX_LOG(LogLaunch, Error, "[AO边缘] 全分辨率回读失败");
        return false;
    }

    gtao->SetHalfResolution(true);

    if (!ReadAoAndDepth(context, renderer, half, depthIgnored))
    {
        gtao->SetHalfResolution(originalHalf);
        LIMX_LOG(LogLaunch, Error, "[AO边缘] 半分辨率回读失败");
        return false;
    }

    gtao->SetHalfResolution(originalHalf);

    if (full.GetSize() != half.GetSize() || full.GetSize() == 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[AO边缘] 两次回读的尺寸不同 ({} vs {})",
                 full.GetSize(), half.GetSize());
        return false;
    }

    const FRHIExtent2D extent = context->GetSwapchainExtent();

    const FAoEdgeStats stats = ComputeAoEdgeStats(full, half, depth, extent);

    LIMX_LOG(LogLaunch, Display,
             "[AO边缘] 不连续处 {} 个 (平均差 {}, 渗色 {} 个) | "
             "平坦区 {} 个 (平均差 {})",
             stats.EdgeCount, stats.EdgeMean, stats.BleedCount,
             stats.SmoothCount, stats.SmoothMean);

    bool passed = true;

    // ---- 0. 场景里必须真的有深度不连续 ----
    //
    // 没有不连续的话双边、双线性、最近邻给的是同一张图 —— 那时这条判据
    // 满分通过, 而它什么都没验。墙角场景实测 0 个, 综合场景 157095 个。
    constexpr SizeType kMinEdgePixels = 3000;

    if (stats.EdgeCount < kMinEdgePixels)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[AO边缘] 只有 {} 个深度不连续像素 (需要至少 {}) —— "
                 "这个场景判不了双边加权 (综合场景实测 7236 个)",
                 stats.EdgeCount, kMinEdgePixels);
        passed = false;
    }

    // ---- 1. 渗色像素数不能为零 ----
    //
    // 正确实现也会有一些: 半分辨率本来就抓不住比半个像素还细的遮挡物,
    // 那与上采样方式无关。一个都没有反而说明场景里前景背景的 AO 差别
    // 不够大 —— 那时判据同样无从分辨。实测正确实现 2694 个。
    if (stats.BleedCount == 0)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[AO边缘] 渗色像素一个都没有 —— 场景的前景背景 AO 差别"
                 "不够大, 判据分辨不出上采样方式");
        passed = false;
    }

    // ---- 2. 渗色像素数要有上界 ----
    //
    // 综合场景上的五个实测点 (7236 个真实的深度不连续像素):
    //
    //   双边 (正确)          渗色  919   不连续处平均差 0.0947
    //   退化成最近邻          渗色  918   平均差 0.0947   <- 判不出来
    //   去掉双线性因子        渗色  905   平均差 0.0950   <- 同上
    //   纯双线性             渗色 1361   平均差 0.1201   <- 抓得住
    //   加权反向             渗色 1801   平均差 0.1675   <- 抓得住
    //
    // 阈值 1100 卡在 919 与 1361 之间: 距正确实现 20% 余量, 距双线性 19%。
    // 两侧都不宽, 所以这个数**与场景绑死** —— verify.ps1 里只在综合场景上
    // 跑这一条, 换场景必须重新量。
    //
    // 这条判据能抓的是"上采样太糊"这一类。"太锐"那一类抓不住, 理由写在
    // FAoEdgeStats::SmoothMean 上 —— 那是量过的盲点, 不是没想到。
    constexpr SizeType kMaxBleedPixels = 1100;

    if (stats.BleedCount > kMaxBleedPixels)
    {
        LIMX_LOG(LogLaunch, Error,
                 "[AO边缘] 渗色像素 {} 个超过上限 {} —— "
                 "上采样在深度不连续处把前景与背景混在一起了",
                 stats.BleedCount, kMaxBleedPixels);
        passed = false;
    }

    if (passed)
    {
        LIMX_LOG(LogLaunch, Display,
                 "[AO边缘] 通过 — 双边加权在深度不连续处确实起了作用");
    }
    else
    {
        LIMX_LOG(LogLaunch, Error, "[AO边缘] 失败");
    }

    return passed;
}

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
// RunShowcaseChecks — 每个子系统到底跑没跑
//
// 前面所有的判据问的都是"这个数对不对"。这一条问的是另一件事: **这个子系统
// 是不是真的在起作用。**
//
// 两者不重叠。一个被悄悄关掉的子系统在它自己的最小场景里根本不会被跑到 ——
// 那些判据要么不适用, 要么因为场景不对而无从判定。而在综合场景里, 每一个都
// 该留下可观测的痕迹:
//
//   分簇剔除    → 索引表里必须有条目
//   GPU 驱动    → 可见数必须少于总数, 分组数必须少于物体数
//   阴影图集    → 必须画了 1 + 6 = 7 块 (聚光灯一块, 点光源六块)
//   方向光级联  → 级联视图的可见数必须非零
//   GTAO        → 必须有相当比例的像素被遮蔽
//   泛光        → 泛光缓冲必须有能量
//   TAA         → 解析必须真的在混合历史 (前后两帧不能完全相同)
//   半透明      → 必须有半透明批次
//
// 每一条都带"够不够判"的元判据: 场景里没有对应的东西时直接判失败, 而不是
// 悄悄通过。这是 Day 10~13 反复撞到的那件事 —— 判据正确不等于场景能让它区分。
// ============================================================================
static bool RunShowcaseChecks(FRenderContext* context, FRenderer& renderer)
{
    // 先渲几帧, 让所有每帧状态 (计数器回读、TAA 历史) 稳定下来。
    //
    // 计数器的回读隔着并行帧数, 只渲一帧读到的是上一轮的值 —— Day 10 在这
    // 上面栽过一次, 读出 0 而画面完全正确。
    for (UInt32 i = 0; i < context->GetMaxFramesInFlight() + 2u; ++i)
    {
        renderer.RenderFrame();
    }

    bool passed = true;

    const auto Fail = [&passed](const AnsiChar* message)
    {
        LIMX_LOG(LogLaunch, Error, "[综合] {}", message);
        passed = false;
    };

    // ---- 光源 ----
    FLightManager& lights = FLightManager::Get();

    UInt32 directionalCount = 0;
    UInt32 spotShadowCount  = 0;
    UInt32 pointShadowCount = 0;

    for (UInt32 i = 0; i < lights.GetLightCount(); ++i)
    {
        const FLight& light = lights.GetLight(i);

        if (light.GetType() == ELightType::Directional)
        {
            ++directionalCount;
        }
        else if (light.GetType() == ELightType::Spot && light.CastsShadow())
        {
            ++spotShadowCount;
        }
        else if (light.GetType() == ELightType::Point && light.CastsShadow())
        {
            ++pointShadowCount;
        }
    }

    LIMX_LOG(LogLaunch, Display,
             "[综合] 光源 — 方向光 {} 盏, 投影聚光灯 {} 盏, 投影点光源 {} 盏",
             directionalCount, spotShadowCount, pointShadowCount);

    if (directionalCount == 0 || spotShadowCount == 0 || pointShadowCount == 0)
    {
        Fail("三种光源类型没有凑齐 — 这个场景验不了三条阴影路径");
    }

    // ---- 阴影图集 ----
    //
    // 聚光灯一块 + 点光源六块 = 7 块。数目写死而不是"大于零": 点光源少分了
    // 五块的话总数是 2, 而"大于零"照样通过。
    FShadowAtlasPass* const atlas = renderer.GetShadowAtlasPass();

    const UInt32 tileCount =
        (atlas != nullptr) ? atlas->GetRenderedTileCount() : 0u;

    LIMX_LOG(LogLaunch, Display, "[综合] 阴影图集绘制 {} 块", tileCount);

    if (tileCount != 1u + kCubeFaceCount)
    {
        Fail("阴影图集应当绘制 7 块 (聚光灯 1 + 点光源 6)");
    }

    // ---- 分簇剔除 ----
    FClusterLightPass* const cluster = renderer.GetClusterLightPass();

    const UInt32 allocated =
        (cluster != nullptr) ? cluster->GetAllocatedIndexCount() : 0u;

    LIMX_LOG(LogLaunch, Display, "[综合] 分簇索引表 {} 条", allocated);

    if (renderer.IsClusteredLighting())
    {
        if (allocated == 0u)
        {
            Fail("分簇开着却一条索引都没分配 — 剔除没跑?");
        }

        if (cluster != nullptr && cluster->HasOverflowed())
        {
            Fail("分簇索引表溢出 — 有光源被丢弃");
        }
    }

    // ---- GPU 驱动 ----
    FGpuCullPass* const cull = renderer.GetGpuCullPass();

    if (cull != nullptr && cull->IsEnabled())
    {
        const UInt32 objects = cull->GetObjectCount();
        const UInt32 visible = cull->GetVisibleCount();
        const UInt32 groups  = static_cast<UInt32>(cull->GetGroups().GetSize());

        LIMX_LOG(LogLaunch, Display,
                 "[综合] GPU 驱动 — {} 个物体, 相机视图可见 {}, {} 组, "
                 "{} 个视图",
                 objects, visible, groups, cull->GetViewCount());

        if (objects == 0u)
        {
            Fail("GPU 驱动一个物体都没上传");
        }
        else if (visible == 0u)
        {
            Fail("GPU 驱动的可见数为零 — 计数器回读断了?");
        }
        else if (visible >= objects)
        {
            Fail("GPU 驱动一个物体都没剔掉 — 换个有物体在视锥外的角度");
        }

        if (objects > 0u && groups >= objects)
        {
            Fail("GPU 驱动的分组没起作用 — 批次列表没按状态聚类?");
        }

        // 级联视图也要有可见物体 —— 那一段间接命令必须真的被写过
        for (UInt32 view = FGpuCullPass::kFirstCascadeView;
             view < cull->GetViewCount(); ++view)
        {
            if (cull->GetVisibleCount(view) == 0u)
            {
                Fail("某个级联视图一个可见物体都没有 — 那一段命令没写过?");
                break;
            }
        }
    }

    // ---- 半透明 ----
    const SizeType translucentCount =
        renderer.GetTranslucentObjects().GetSize();

    LIMX_LOG(LogLaunch, Display,
             "[综合] 半透明批次 {} 个", translucentCount);

    if (translucentCount == 0)
    {
        Fail("场景里没有半透明批次 — 那条绘制路径没被走到");
    }

    // ---- GTAO ----
    FGtaoPass* const gtao = renderer.GetGtaoPass();

    if (gtao != nullptr && gtao->IsEnabled())
    {
        TArray<Float32> ao;
        TArray<Float32> depth;

        if (!ReadAoAndDepth(context, renderer, ao, depth))
        {
            Fail("AO 回读失败");
        }
        else
        {
            SizeType shaded = 0;

            for (SizeType i = 0; i < ao.GetSize(); ++i)
            {
                if (ao[i] < 0.9f)
                {
                    ++shaded;
                }
            }

            LIMX_LOG(LogLaunch, Display,
                     "[综合] GTAO — {} / {} 个像素有明显遮蔽",
                     shaded, ao.GetSize());

            // 一成。整个场景堆满了柱子与球, 遮蔽应当到处都是。
            //
            // 判据不写成"大于零": 一个只在几个像素上有值的 AO 与"AO 没跑"
            // 在效果上没有区别, 而"大于零"对它照样通过。
            if (shaded * 10 < ao.GetSize())
            {
                Fail("GTAO 几乎没有遮蔽 — 通道跑了吗?");
            }
        }
    }

    if (passed)
    {
        LIMX_LOG(LogLaunch, Display,
                 "[综合] 通过 — 每个子系统都留下了可观测的痕迹");
    }
    else
    {
        LIMX_LOG(LogLaunch, Error, "[综合] 失败");
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

    // 两块板各用一个材质。
    //
    // 共用一个的话, 光追反射的判据里"命中处的材质下标"这一条就是空的:
    // 无论几何表怎么错位, 查到的都是同一个下标。两个不同的下标之后, 错位
    // 会立刻表现为"反射里看到的是地面的材质"。
    //
    // 颜色也刻意不同 —— 地面偏冷、墙偏暖。GTAO 的判据只看遮蔽率, 与颜色
    // 无关; 而反射的判据要的正是"看到的是哪一块"。
    FMaterial* wallMaterial =
        FMaterialManager::Get().CreateMaterial("CornerWall");

    if (wallMaterial != nullptr)
    {
        wallMaterial->SetBaseColor(FVector4(0.85f, 0.55f, 0.35f, 1.0f));
        wallMaterial->SetMetallic(0.0f);
        wallMaterial->SetRoughness(1.0f);
    }

    for (UInt32 i = 0; i < 2; ++i)
    {
        FTransform nodeTransform;
        nodeTransform.Translation = entries[i].Position;
        nodeTransform.Rotation    = entries[i].Rotation;

        LNode* node = scene->SpawnNode<LNode>(FName(entries[i].Name),
                                              nodeTransform);

        LMeshTrait* meshTrait = node->AddTrait<LMeshTrait>(FName("Mesh"));
        meshTrait->SetMesh(&resources, meshHandle);

        // [0] 地面用默认材质, [1] 墙用自己的
        meshTrait->SetMaterial(
            (i == 1 && wallMaterial != nullptr) ? wallMaterial
                                                : defaultMaterial);

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
// BuildShowcaseScene — 一个场景同时跑全部子系统
//
// 到 Day 13 为止, 每条判据各用一个**最小场景**: 墙角只有两块平面, 泛光只有
// 一个方块, 阴影只有一堵墙加一块板。那是刻意的 —— 多一样东西, 解析判据就
// 不再成立。
//
// 但那也留下一个空白: **没有任何一个场景同时跑全部子系统**。而子系统之间
// 会互相影响 —— 分簇剔除决定哪些光参与着色, 阴影图集的块下标存在光源数据
// 里, GPU 驱动的逐物体缓冲区被四个通道共用, TAA 的历史依赖速度矢量, 而速度
// 矢量来自深度预通道。任何一处对不上, 单独的最小场景都发现不了。
//
// 这个场景不追求解析可判定, 它追求的是**覆盖**: 三种光源类型都投影, 不透明
// 与半透明都有, 蒙版材质有, 而且物体足够多、足够分散, 让分簇与剔除都有东西
// 可做。
//
// 判据因此也换了一种: 不问"这个数对不对", 问"这个子系统到底跑没跑"。
// 见 RunShowcaseChecks。
// ============================================================================
static void BuildShowcaseScene(LScene* scene, FRenderContext* context,
                               FRenderer* renderer)
{
    LIMX_CHECK(scene != nullptr);
    LIMX_CHECK(context != nullptr);
    LIMX_CHECK(renderer != nullptr);

    FRenderResourceManager& resources = context->GetResourceManager();

    FMeshData cubeMesh   = FGeometryGenerator::GenerateCube();
    FMeshData sphereMesh = FGeometryGenerator::GenerateSphere(1.0f, 24, 16);

    // 顶点色刷白。
    //
    // 程序化图元自带**调试用的顶点色**: 立方体六个面各一色 (红/青/绿/品红/
    // 蓝/黄), 球体按经纬映射色相。而 pbr.frag 算的是
    // albedo = fragColor * baseColor —— 于是材质的基色被那层调试色整个盖掉,
    // 四种材质在画面上分不出来。
    //
    // 这不是缺陷 (那些颜色对"看清楚一个图元的朝向"很有用), 但它与这个场景的
    // 目的冲突: 综合场景要展示的是材质与光照, 不是图元的面序。
    //
    // 刷白之后 albedo 就是材质的基色。第一版没刷, 截图里地面是绿的、柱子是
    // 红蓝相间的 —— 而那时我以为是材质下标串了位, 查了一圈才发现是顶点色。
    const auto WhitenVertexColors = [](FMeshData& mesh)
    {
        for (SizeType i = 0; i < mesh.Vertices.GetSize(); ++i)
        {
            mesh.Vertices[i].Color = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
        }
    };

    WhitenVertexColors(cubeMesh);
    WhitenVertexColors(sphereMesh);

    FMeshResourceHandle cubeHandle =
        resources.CreateMesh(cubeMesh, FName("ShowcaseCube"));
    FMeshResourceHandle sphereHandle =
        resources.CreateMesh(sphereMesh, FName("ShowcaseSphere"));

    if (!cubeHandle.IsValid() || !sphereHandle.IsValid())
    {
        LIMX_LOG(LogLaunch, Error, "[Launch] 综合场景的网格上传失败");
        return;
    }

    // ---- 材质 ----
    //
    // 三类都要有: 不透明、蒙版、半透明。三者走的是不同的管线变体与不同的
    // 绘制顺序, 而"某一类悄悄没画"在别的场景里看不出来。
    FMaterial* opaque =
        FMaterialManager::Get().CreateMaterial("ShowcaseOpaque");
    FMaterial* metal =
        FMaterialManager::Get().CreateMaterial("ShowcaseMetal");
    FMaterial* glass =
        FMaterialManager::Get().CreateMaterial("ShowcaseGlass");
    FMaterial* emissive =
        FMaterialManager::Get().CreateMaterial("ShowcaseEmissive");

    if (opaque == nullptr || metal == nullptr || glass == nullptr ||
        emissive == nullptr)
    {
        LIMX_LOG(LogLaunch, Error, "[Launch] 综合场景的材质创建失败");
        return;
    }

    opaque->SetBaseColor(FVector4(0.75f, 0.72f, 0.68f, 1.0f));
    opaque->SetMetallic(0.0f);
    opaque->SetRoughness(0.85f);

    metal->SetBaseColor(FVector4(0.9f, 0.75f, 0.35f, 1.0f));
    metal->SetMetallic(1.0f);
    metal->SetRoughness(0.25f);

    // 半透明 —— 它必须由远及近绘制, 而那个顺序是 CPU 排的。
    glass->SetBaseColor(FVector4(0.35f, 0.6f, 0.9f, 0.45f));
    glass->SetMetallic(0.0f);
    glass->SetRoughness(0.1f);
    glass->SetBlendMode(EMaterialBlendMode::Translucent);

    // 自发光 —— 泛光的信号源。不给自发光的话泛光链跑了也是全黑, 而"泛光
    // 没跑"与"没有超过阈值的像素"在结果上无法区分。
    emissive->SetBaseColor(FVector4(0.0f, 0.0f, 0.0f, 1.0f));
    emissive->SetEmissiveColor(FVector3(12.0f, 9.0f, 4.0f));
    emissive->SetMetallic(0.0f);
    emissive->SetRoughness(1.0f);

    // 蒙版 —— 它走的是"深度预通道与前向通道必须做逐纹素一致的裁剪"那条路径,
    // 而前向的深度测试是 Equal: 两处裁剪结论差一个纹素, 那一片就整个消失。
    //
    // 这里没有带 alpha 的贴图, 所以把阈值设在基色 alpha 之下 —— 裁剪不会
    // 真的丢掉任何纹素, 但**管线变体与那条代码路径确实被走到了**。真正验
    // 裁剪一致性要一张带镂空的贴图, 那是资产的事。
    FMaterial* masked =
        FMaterialManager::Get().CreateMaterial("ShowcaseMasked");

    if (masked == nullptr)
    {
        LIMX_LOG(LogLaunch, Error, "[Launch] 综合场景的蒙版材质创建失败");
        return;
    }

    masked->SetBaseColor(FVector4(0.4f, 0.8f, 0.45f, 1.0f));
    masked->SetMetallic(0.0f);
    masked->SetRoughness(0.6f);
    masked->SetBlendMode(EMaterialBlendMode::Masked);
    masked->SetAlphaCutoff(0.5f);
    masked->SetDoubleSided(true);

    const auto Spawn = [&](const FName& name, FMeshResourceHandle mesh,
                           FMaterial* material, const FVector3& position,
                           const FVector3& scale)
    {
        FTransform transform;
        transform.Translation = position;
        transform.Scale3D     = scale;

        LNode* node = scene->SpawnNode<LNode>(name, transform);

        LMeshTrait* meshTrait = node->AddTrait<LMeshTrait>(FName("Mesh"));
        meshTrait->SetMesh(&resources, mesh);
        meshTrait->SetMaterial(material);
        meshTrait->SetVisible(true);
    };

    // ---- 地面 ----
    //
    // 阴影要有地方落。没有地面时三种阴影都照画, 而画面上一个像素都不受
    // 影响 —— Day 11 在压力场景上正是这么栽的。
    Spawn(FName("ShowcaseGround"), cubeHandle, opaque,
          FVector3(0.0f, -0.5f, 0.0f), FVector3(40.0f, 1.0f, 40.0f));

    // ---- 背墙 ----
    //
    // 给聚光灯与点光源的阴影一个竖直的接收面。只有地面的话, 侧向的阴影
    // 全落在视野之外。
    Spawn(FName("ShowcaseBackWall"), cubeHandle, opaque,
          FVector3(0.0f, 5.0f, -12.0f), FVector3(40.0f, 12.0f, 1.0f));

    // ---- 主体 ----
    //
    // 一圈球与柱, 高低错落 —— 让三种光源的阴影互相交叠。分簇剔除也要有
    // 东西可剔: 物体分散在 x/z 各 ±10 的范围里, 而相机只看得到一部分。
    for (UInt32 i = 0; i < 12u; ++i)
    {
        const Float32 angle =
            static_cast<Float32>(i) * (2.0f * FMath::kPi / 12.0f);

        const Float32 radius = 6.0f;

        const FVector3 position(radius * FMath::Cos(angle),
                                0.6f + 0.4f * static_cast<Float32>(i % 3u),
                                radius * FMath::Sin(angle));

        Spawn(FName("ShowcaseSphere"), sphereHandle,
              (i % 2u == 0u) ? opaque : metal, position,
              FVector3(1.2f, 1.2f, 1.2f));

        // 柱子 —— 竖直的遮挡物, 阴影拉得长, 三种光源都能照到
        Spawn(FName("ShowcasePillar"), cubeHandle, opaque,
              FVector3(position.X * 1.55f, 1.75f, position.Z * 1.55f),
              FVector3(0.5f, 3.5f, 0.5f));
    }

    // ---- 半透明 ----
    //
    // 两块玻璃前后叠放 —— 排序错了的话混合结果不对, 而那是"正确性"而不是
    // "优化"的问题。
    // ---- 均匀缩放的箱子 ----
    //
    // 立方体 (六个面朝六个方向, 法线锥无效) **而且**三轴等比。
    //
    // 也是判据逼出来的。背面判据的第一条 early-out 是"法线锥无效就不剔",
    // 而综合场景原本每一个立方体都是非均匀缩放的 —— 于是**后一条**
    // early-out (缩放不一致就不剔) 先返回, 第一条被完全遮住: 把它删掉,
    // 判据一动不动地绿。
    //
    // 两条 early-out 互相遮掩这件事, 只有分支覆盖计数能看出来。
    Spawn(FName("ShowcaseCrate"), cubeHandle, opaque,
          FVector3(3.6f, 0.7f, 3.0f), FVector3(1.4f, 1.4f, 1.4f));

    // ---- 椭球 ----
    //
    // 缩放不一致 (1.8 / 0.7 / 1.2) **而且**法线锥有效 —— 场景里唯一同时
    // 满足这两条的物体。
    //
    // 它是判据逼出来的: meshlet 剔除的背面判据里有一条 early-out 是
    // "缩放不一致时不剔" (法线在非均匀缩放下要用逆转置变换, 而锥的张角
    // 也会变)。综合场景原本所有非均匀缩放的物体都是立方体, 而立方体的
    // 法线锥是无效的 —— 更早的那条 early-out 先返回了, 于是这一条永远
    // 走不到, 把它整个删掉判据也不会红。
    Spawn(FName("ShowcaseEllipsoid"), sphereHandle, opaque,
          FVector3(-3.2f, 1.1f, 2.4f), FVector3(1.8f, 0.7f, 1.2f));

    Spawn(FName("ShowcaseGlassFar"), cubeHandle, glass,
          FVector3(-1.5f, 1.5f, 1.0f), FVector3(2.5f, 3.0f, 0.1f));

    Spawn(FName("ShowcaseGlassNear"), cubeHandle, glass,
          FVector3(1.5f, 1.5f, 2.5f), FVector3(2.5f, 3.0f, 0.1f));

    // ---- 相机背后的柱子 ----
    //
    // 它们**故意**放在视锥之外。两个用处:
    //
    //   剔除要有东西可剔。全部物体都可见的话, 一个什么都不做的剔除实现
    //     也会给出完全正确的画面 —— 判据无从判定 (第一版就是这样, 29 个
    //     物体可见 29 个)。
    //   而它们仍然**投射阴影**: 相机背后的物体照样能把影子投进画面。这条
    //     路径 (阴影用未经相机剔除的列表) 除此之外没有别的地方覆盖。
    for (UInt32 i = 0; i < 4u; ++i)
    {
        const Float32 offset = -4.5f + 3.0f * static_cast<Float32>(i);

        Spawn(FName("ShowcaseBehindCamera"), cubeHandle, opaque,
              FVector3(offset, 2.0f, 22.0f), FVector3(0.6f, 4.0f, 0.6f));
    }

    // ---- 蒙版 ----
    //
    // 双面 —— 这样单面/双面两条管线变体在同一帧里都被用到, 而 GPU 驱动的
    // 分组正是按"单双面"切的。
    Spawn(FName("ShowcaseMaskedA"), cubeHandle, masked,
          FVector3(-4.0f, 1.2f, 4.5f), FVector3(1.6f, 2.4f, 0.08f));

    Spawn(FName("ShowcaseMaskedB"), cubeHandle, masked,
          FVector3(4.0f, 1.2f, 4.5f), FVector3(1.6f, 2.4f, 0.08f));

    // ---- 自发光 ----
    Spawn(FName("ShowcaseEmissive"), sphereHandle, emissive,
          FVector3(0.0f, 3.2f, -2.0f), FVector3(0.5f, 0.5f, 0.5f));

    resources.ReleaseMeshReference(cubeHandle);
    resources.ReleaseMeshReference(sphereHandle);

    // ---- 光源 ----
    //
    // 三种类型各一盏, 都投影。这是这个场景存在的主要理由: 三条阴影路径
    // (级联 / 图集一块 / 图集连续六块) 在同一帧里跑。
    FLightManager& lights = FLightManager::Get();

    lights.ClearAllLights();

    {
        FLight sun = FLight::CreateDirectional(
            FVector3(-0.45f, -0.8f, -0.4f),
            FLinearColor(1.0f, 0.96f, 0.88f, 1.0f), 2.6f);

        sun.SetDebugName("ShowcaseSun");

        lights.AddLight(static_cast<FLight&&>(sun));
    }

    {
        FLight spot = FLight::CreateSpot(
            FVector3(-7.0f, 7.5f, 5.0f),
            FVector3(0.55f, -0.75f, -0.36f),
            FLinearColor(0.55f, 0.8f, 1.0f, 1.0f), 25.0f,
            22.0f, 30.0f, 30.0f);

        spot.SetCastsShadow(true);
        spot.SetDebugName("ShowcaseSpot");

        lights.AddLight(static_cast<FLight&&>(spot));
    }

    {
        FLight point = FLight::CreatePoint(
            FVector3(4.5f, 2.6f, 3.0f),
            FLinearColor(1.0f, 0.55f, 0.3f, 1.0f), 22.0f, 16.0f);

        point.SetCastsShadow(true);
        point.SetDebugName("ShowcasePoint");

        lights.AddLight(static_cast<FLight&&>(point));
    }

    // ---- 相机 ----
    //
    // 摆在能同时看到地面、背墙、一圈柱子与两块玻璃的位置。
    // yaw 0 朝 -Z —— 相机放在 +Z 一侧才看得到原点附近的东西。
    //
    // 第一版写成了 yaw = π (那是压力场景的写法, 它的相机在 -Z 一侧), 于是
    // 相机背对整个场景: 29 个物体里只有 1 个可见, GTAO 一个像素都没遮蔽。
    // 判据立刻报了出来 —— 那正是"每个子系统必须留下痕迹"这条判据的用处:
    // 它抓的不只是子系统, 也包括场景本身摆得对不对。
    renderer->GetCamera().SetPosition(FVector3(0.0f, 6.0f, 16.0f));
    renderer->GetCamera().SetRotation(0.0f, -0.28f);

    LIMX_LOG(LogLaunch, Display,
             "[Launch] 综合场景已构建 — {} 个节点, 三种光源类型各一盏 (都投影), "
             "不透明/蒙版/半透明/自发光四类材质",
             scene->GetNodeCount());
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

    // ---- 一盏投影聚光灯 ----
    //
    // 压力场景原本只有方向光, 于是阴影**图集**那一路 (聚光/点光源用) 从来
    // 没被走到。逐物体缓冲区截断的判据要覆盖三条绘制路径 (级联阴影、阴影
    // 图集、前向半透明), 少一盏灯就少一条。
    //
    // 变异验证是这么发现的: 把图集那一路的钳位删掉, 判据纹丝不动。
    {
        FLight spot = FLight::CreateSpot(
            FVector3(0.0f, 14.0f, 0.0f), FVector3(0.0f, -1.0f, 0.0f),
            FLinearColor(1.0f, 0.95f, 0.85f, 1.0f), 40.0f, 20.0f, 35.0f,
            40.0f);

        spot.SetAttenuation(1.0f, 0.0f, 0.0f);
        spot.SetCastsShadow(true);
        spot.SetDebugName("StressSpot");

        FLightManager::Get().AddLight(static_cast<FLight&&>(spot));
    }

    // ---- 一小撮半透明 ----
    //
    // 数量少 (8 个), 但**非有不可**: 逐物体缓冲区分三段 (相机 / 投射体 /
    // 半透明), 半透明是最后一段, 三段合计超容量时它最先被挤没。一个都没有
    // 的话, "半透明段被截断之后还照着列表长度画"这条路径永远走不到, 而那
    // 条路径的后果是 GPU 读非法地址。
    //
    // 摆在网格中心附近、离地一段 —— 既在相机视野里, 又不至于挡住整片网格。
    {
        FMaterial* glass =
            FMaterialManager::Get().CreateMaterial("StressGlass");

        if (glass != nullptr)
        {
            glass->SetBaseColor(FVector4(0.6f, 0.8f, 1.0f, 0.35f));
            glass->SetMetallic(0.0f);
            glass->SetRoughness(0.1f);
            glass->SetBlendMode(EMaterialBlendMode::Translucent);

            for (UInt32 i = 0; i < 8; ++i)
            {
                FTransform glassTransform;
                glassTransform.Translation = FVector3(
                    -6.0f + static_cast<Float32>(i) * 1.7f, 3.0f,
                    -halfSpan * 0.25f);
                glassTransform.Scale3D = FVector3(1.2f, 1.2f, 1.2f);

                LNode* node = scene->SpawnNode<LNode>(FName("StressGlass"),
                                                      glassTransform);

                LMeshTrait* mesh = node->AddTrait<LMeshTrait>(FName("Mesh"));
                mesh->SetMesh(&resources, meshHandles[0]);
                mesh->SetMaterial(glass);
                mesh->SetVisible(true);
            }
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

    // 认不出来的参数直接退出, 不许继续跑。
    //
    // 只把标志记下来而不接到退出码上, 就正好是这个项目反复栽的那个形状:
    // **失败模式落在"通过"上**。第一版就是这么写的 —— ParseLaunchOptions 里
    // 打了 Error、置了标志, 而 --this-is-not-a-flag 照样返回 0。
    if (launchOptions.UnknownArgument)
    {
        return 2;
    }

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

    // 所有自检都挂在"跑满 --frames 之后"那个分支上, 所以漏写 --frames 的
    // 命令行会**一帧不跑就退出, 退出码 0** —— 一条没跑过的判据报了通过。
    //
    // 这是本项目栽过的坑 (测量脚本静默报通过), 所以这里显式判掉: 要判据
    // 就必须给帧数, 给不出就以判据自己的退出码失败。别的自检暂未补上这道
    // 闸, 但新加的这一条不该重蹈覆辙。
    if (launchOptions.PathTraceCheck && launchOptions.FrameLimit == 0)
    {
        LIMX_LOG(LogLaunch, Error,
            "[Launch] --path-trace-check 需要 --frames N (N >= 1): "
            "自检跑在帧循环结束之后, 不给帧数就等于一条都不跑");
        return 39;
    }

    // ================================================================
    // 1. 创建窗口
    // ================================================================

    FWindow window;

    FWindowDesc windowDesc = {};
    windowDesc.Width       = 1280;
    windowDesc.Height      = 720;
    windowDesc.Title       = L"Limx Engine — Vulkan Renderer";
    windowDesc.IsResizable = true;
    windowDesc.IsVisible   = !launchOptions.HiddenWindow;

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
    else if (launchOptions.ShowcaseScene)
    {
        BuildShowcaseScene(scene, &renderContext, &renderer);
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

    if (launchOptions.RayTracedReflection)
    {
        if (renderer.SetRayTracedReflectionEnabled(true))
        {
            LIMX_LOG(LogLaunch, Display, "[Launch] 光追反射已启用");
        }
        else
        {
            LIMX_LOG(LogLaunch, Error,
                     "[启动] --rt-reflection 无法启用 —— 设备不支持光线追踪");
        }
    }

    if (launchOptions.MeshletDepth)
    {
        if (renderer.SetMeshletDepthEnabled(true))
        {
            FMeshletDepthPass* const pass = renderer.GetMeshletDepthPass();

            const bool wantFallback = launchOptions.MeshletDepthFallback;

            const bool modeOk = pass->SetMode(
                wantFallback ? FMeshletDepthPass::EMode::Fallback
                             : FMeshletDepthPass::EMode::MeshShader);

            pass->SetResolveEnabled(launchOptions.MeshletResolve);

            pass->SetOcclusionCullEnabled(launchOptions.MeshletOcclusion);

            if (FMeshletCullPass* const cullPass =
                    renderer.GetMeshletCullPass())
            {
                cullPass->SetOcclusionCullEnabled(
                    launchOptions.MeshletOcclusion);
            }

            LIMX_LOG(LogLaunch, Display,
                     "[Launch] meshlet 深度光栅化已启用 ({}{}), 材质解析 {}",
                     wantFallback ? "计算展开回退" : "网格着色器",
                     modeOk ? "" : " — 请求的路径不可用, 保持原样",
                     launchOptions.MeshletResolve ? "开" : "关");
        }
        else
        {
            LIMX_LOG(LogLaunch, Error,
                     "[启动] --meshlet-depth 无法启用 —— 通道不存在");
        }
    }

    if (launchOptions.MeshletCull)
    {
        if (renderer.SetMeshletCullEnabled(true))
        {
            LIMX_LOG(LogLaunch, Display, "[Launch] 两级 meshlet 剔除已启用");
        }
        else
        {
            LIMX_LOG(LogLaunch, Error,
                     "[启动] --meshlet-cull 无法启用 —— 剔除通道不存在");
        }
    }

    if (launchOptions.RayTracedAo)
    {
        if (renderer.SetRayTracedAoEnabled(true))
        {
            if (launchOptions.RayTracedAoHalf &&
                renderer.GetRayTracedAoPass() != nullptr)
            {
                renderer.GetRayTracedAoPass()->SetHalfResolution(true);
            }

            LIMX_LOG(LogLaunch, Display,
                     "[Launch] 光追环境光遮蔽已启用 ({})",
                     launchOptions.RayTracedAoHalf ? "半分辨率" : "全分辨率");
        }
        else
        {
            LIMX_LOG(LogLaunch, Error,
                     "[启动] --rt-ao 无法启用 —— 设备不支持光线追踪");
        }
    }

    if (launchOptions.RayTracedShadows)
    {
        if (renderer.SetRayTracedShadowsEnabled(true))
        {
            LIMX_LOG(LogLaunch, Display,
                     "[Launch] 光追阴影已启用 — 第一盏投影的光源改走射线");
        }
        else
        {
            LIMX_LOG(LogLaunch, Error,
                     "[启动] --rt-shadows 无法启用 —— 设备不支持光线追踪");
        }
    }

    if (launchOptions.Gtao)
    {
        renderer.SetGtaoEnabled(true);

        if (renderer.GetGtaoPass() != nullptr)
        {
            renderer.GetGtaoPass()->SetRadius(launchOptions.AoRadius);
            renderer.GetGtaoPass()->SetHalfResolution(launchOptions.GtaoHalf);
        }

        LIMX_LOG(LogLaunch, Display,
                 "[Launch] 屏幕空间环境光遮蔽已启用 "
                 "(GTAO, 4 方向 x 8 步进, 半径 {}, {})",
                 launchOptions.AoRadius,
                 launchOptions.GtaoHalf ? "半分辨率 + 双边上采样"
                                        : "全分辨率");
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
    bool    aoHalfCheckPassed = true;
    bool    showcaseCheckPassed = true;
    bool    rayTracingCheckPassed = true;
    bool    aoEdgeCheckPassed = true;
    bool    rtDepthCheckPassed = true;
    bool    rtShadowCheckPassed = true;
    bool    rtAoCheckPassed = true;
    bool    rtAoSelfPassed = true;
    bool    rtReflectionCheckPassed = true;
    bool    rtReflectionSelfPassed = true;
    bool    rtGeometryTablePassed = true;
    bool    rtHybridPassed = true;
    bool    rtAoUpsamplePassed = true;
    bool    meshletPassed = true;
    bool    meshletCullPassed = true;
    bool    meshletDepthPassed = true;
    bool    meshletResolvePassed = true;
    bool    meshletOcclusionPassed = true;
    bool    meshletScalePassed = true;
    bool    gpuCullOverflowPassed = true;
    bool    meshletExpandOverflowPassed = true;
    bool    meshSimplifyPassed = true;
    bool    meshletGroupPassed = true;
    bool    lodDagPassed = true;
    bool    lodSelectPassed = true;
    bool    lodCrackPassed = true;
    bool    lodGpuPassed = true;
    bool    pathTracePassed = true;

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

            if (launchOptions.AoHalfCheck)
            {
                aoHalfCheckPassed = RunAoHalfChecks(&renderContext, renderer);
            }

            if (launchOptions.RayTracingCheck)
            {
                rayTracingCheckPassed = RunRayTracingChecks(
                    renderContext.GetDevice(), &renderContext);
            }

            if (launchOptions.AoEdgeCheck)
            {
                aoEdgeCheckPassed = RunAoEdgeChecks(&renderContext, renderer);
            }

            if (launchOptions.RtDepthCheck)
            {
                rtDepthCheckPassed =
                    RunRayTracingDepthCheck(&renderContext, renderer);
            }

            if (launchOptions.RtShadowCheck)
            {
                rtShadowCheckPassed =
                    RunRayTracedShadowChecks(&renderContext, renderer);
            }

            if (launchOptions.RtAoCheck)
            {
                rtAoCheckPassed =
                    RunRayTracedAoChecks(&renderContext, renderer);
            }

            if (launchOptions.RtAoSelfCheck)
            {
                rtAoSelfPassed =
                    RunRayTracedAoSelfCheck(&renderContext, renderer);
            }

            if (launchOptions.RtGeometryTableCheck)
            {
                rtGeometryTablePassed =
                    RunRayTracingGeometryTableCheck(&renderContext, renderer);
            }

            if (launchOptions.RtHybridCheck)
            {
                rtHybridPassed =
                    RunRayTracingHybridCheck(&renderContext, renderer);
            }

            if (launchOptions.RtAoUpsampleCheck)
            {
                rtAoUpsamplePassed =
                    RunRayTracedAoUpsampleCheck(&renderContext, renderer);
            }

            if (launchOptions.MeshletCheck)
            {
                meshletPassed = RunMeshletChecks(&renderContext, renderer);
            }

            if (launchOptions.MeshletCullCheck)
            {
                meshletCullPassed =
                    RunMeshletCullChecks(&renderContext, renderer);
            }

            if (launchOptions.MeshletDepthCheck)
            {
                meshletDepthPassed =
                    RunMeshletDepthChecks(&renderContext, renderer);
            }

            if (launchOptions.MeshletResolveCheck)
            {
                meshletResolvePassed =
                    RunMeshletResolveChecks(&renderContext, renderer);
            }

            if (launchOptions.MeshletOcclusionCheck)
            {
                meshletOcclusionPassed =
                    RunMeshletOcclusionChecks(&renderContext, renderer);
            }

            if (launchOptions.MeshletScaleCheck)
            {
                meshletScalePassed =
                    RunMeshletScaleChecks(&renderContext, renderer);
            }

            if (launchOptions.GpuCullOverflowCheck)
            {
                gpuCullOverflowPassed =
                    RunGpuCullOverflowChecks(&renderContext, renderer);
            }

            if (launchOptions.MeshletExpandOverflowCheck)
            {
                meshletExpandOverflowPassed =
                    RunMeshletExpandOverflowChecks(&renderContext, renderer);
            }

            if (launchOptions.MeshSimplifyCheck)
            {
                meshSimplifyPassed = RunMeshSimplifyChecks();
            }

            if (launchOptions.MeshletGroupCheck)
            {
                meshletGroupPassed = RunMeshletGroupChecks();
            }

            if (launchOptions.LodDagCheck)
            {
                lodDagPassed = RunLodDagChecks();
            }

            if (launchOptions.LodSelectCheck)
            {
                lodSelectPassed = RunLodSelectChecks();
            }

            if (launchOptions.LodCrackCheck)
            {
                lodCrackPassed = RunLodCrackChecks();
            }

            if (launchOptions.LodGpuCheck)
            {
                lodGpuPassed = RunLodGpuChecks(&renderContext, renderer);
            }

            if (launchOptions.PathTraceCheck)
            {
                pathTracePassed = RunPathTraceChecks(&renderContext);
            }

            if (!launchOptions.PathTraceImagePath.IsEmpty())
            {
                const UInt32 size =
                    FMath::Clamp(launchOptions.PathTraceImageSize, 16u, 4096u);

                if (!RenderPathTraceReferenceImage(
                        &renderContext, launchOptions.PathTraceImagePath,
                        size, size,
                        FMath::Max(launchOptions.PathTraceImageSamples, 1u)))
                {
                    LIMX_LOG(LogLaunch, Error,
                             "[Launch] 路径追踪参考图渲染失败");
                }
            }

            if (launchOptions.RtReflectionCheck)
            {
                rtReflectionCheckPassed =
                    RunRayTracedReflectionChecks(&renderContext, renderer);
            }

            if (launchOptions.RtReflectionSelfCheck)
            {
                rtReflectionSelfPassed =
                    RunRayTracedReflectionSelfCheck(&renderContext, renderer);
            }

            if (launchOptions.ShowcaseCheck)
            {
                showcaseCheckPassed =
                    RunShowcaseChecks(&renderContext, renderer);
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

    if (selfCheckCode == 0 && launchOptions.AoHalfCheck)
    {
        selfCheckCode = FinalizeSelfCheck(aoHalfCheckPassed, 16, errorSink,
                                          errorsBeforeShutdown);
    }

    if (selfCheckCode == 0 && launchOptions.ShowcaseCheck)
    {
        selfCheckCode = FinalizeSelfCheck(showcaseCheckPassed, 17, errorSink,
                                          errorsBeforeShutdown);
    }

    if (selfCheckCode == 0 && launchOptions.RayTracingCheck)
    {
        selfCheckCode = FinalizeSelfCheck(rayTracingCheckPassed, 18, errorSink,
                                          errorsBeforeShutdown);
    }

    if (selfCheckCode == 0 && launchOptions.AoEdgeCheck)
    {
        selfCheckCode = FinalizeSelfCheck(aoEdgeCheckPassed, 19, errorSink,
                                          errorsBeforeShutdown);
    }

    if (selfCheckCode == 0 && launchOptions.RtDepthCheck)
    {
        selfCheckCode = FinalizeSelfCheck(rtDepthCheckPassed, 20, errorSink,
                                          errorsBeforeShutdown);
    }

    if (selfCheckCode == 0 && launchOptions.RtShadowCheck)
    {
        selfCheckCode = FinalizeSelfCheck(rtShadowCheckPassed, 21, errorSink,
                                          errorsBeforeShutdown);
    }

    if (selfCheckCode == 0 && launchOptions.RtAoCheck)
    {
        selfCheckCode = FinalizeSelfCheck(rtAoCheckPassed, 22, errorSink,
                                          errorsBeforeShutdown);
    }

    if (selfCheckCode == 0 && launchOptions.RtAoSelfCheck)
    {
        selfCheckCode = FinalizeSelfCheck(rtAoSelfPassed, 25, errorSink,
                                          errorsBeforeShutdown);
    }

    if (selfCheckCode == 0 && launchOptions.RtReflectionCheck)
    {
        selfCheckCode = FinalizeSelfCheck(rtReflectionCheckPassed, 23,
                                          errorSink, errorsBeforeShutdown);
    }

    if (selfCheckCode == 0 && launchOptions.RtReflectionSelfCheck)
    {
        selfCheckCode = FinalizeSelfCheck(rtReflectionSelfPassed, 24,
                                          errorSink, errorsBeforeShutdown);
    }

    if (selfCheckCode == 0 && launchOptions.RtGeometryTableCheck)
    {
        selfCheckCode = FinalizeSelfCheck(rtGeometryTablePassed, 26,
                                          errorSink, errorsBeforeShutdown);
    }

    if (selfCheckCode == 0 && launchOptions.RtHybridCheck)
    {
        selfCheckCode = FinalizeSelfCheck(rtHybridPassed, 27,
                                          errorSink, errorsBeforeShutdown);
    }

    if (selfCheckCode == 0 && launchOptions.RtAoUpsampleCheck)
    {
        selfCheckCode = FinalizeSelfCheck(rtAoUpsamplePassed, 28,
                                          errorSink, errorsBeforeShutdown);
    }

    if (selfCheckCode == 0 && launchOptions.MeshletCheck)
    {
        selfCheckCode = FinalizeSelfCheck(meshletPassed, 29,
                                          errorSink, errorsBeforeShutdown);
    }

    if (selfCheckCode == 0 && launchOptions.MeshletCullCheck)
    {
        selfCheckCode = FinalizeSelfCheck(meshletCullPassed, 30,
                                          errorSink, errorsBeforeShutdown);
    }

    if (selfCheckCode == 0 && launchOptions.MeshletDepthCheck)
    {
        selfCheckCode = FinalizeSelfCheck(meshletDepthPassed, 31,
                                          errorSink, errorsBeforeShutdown);
    }

    if (selfCheckCode == 0 && launchOptions.MeshletResolveCheck)
    {
        selfCheckCode = FinalizeSelfCheck(meshletResolvePassed, 32,
                                          errorSink, errorsBeforeShutdown);
    }

    if (selfCheckCode == 0 && launchOptions.MeshletOcclusionCheck)
    {
        selfCheckCode = FinalizeSelfCheck(meshletOcclusionPassed, 33,
                                          errorSink, errorsBeforeShutdown);
    }

    if (selfCheckCode == 0 && launchOptions.MeshletScaleCheck)
    {
        selfCheckCode = FinalizeSelfCheck(meshletScalePassed, 34, errorSink,
                                          errorsBeforeShutdown);
    }

    if (selfCheckCode == 0 && launchOptions.GpuCullOverflowCheck)
    {
        selfCheckCode = FinalizeSelfCheck(gpuCullOverflowPassed, 35, errorSink,
                                          errorsBeforeShutdown);
    }

    if (selfCheckCode == 0 && launchOptions.MeshletExpandOverflowCheck)
    {
        selfCheckCode = FinalizeSelfCheck(meshletExpandOverflowPassed, 37,
                                          errorSink, errorsBeforeShutdown);
    }

    if (selfCheckCode == 0 && launchOptions.MeshSimplifyCheck)
    {
        selfCheckCode = FinalizeSelfCheck(meshSimplifyPassed, 36, errorSink,
                                          errorsBeforeShutdown);
    }

    if (selfCheckCode == 0 && launchOptions.MeshletGroupCheck)
    {
        selfCheckCode = FinalizeSelfCheck(meshletGroupPassed, 38, errorSink,
                                          errorsBeforeShutdown);
    }

    if (selfCheckCode == 0 && launchOptions.LodDagCheck)
    {
        selfCheckCode = FinalizeSelfCheck(lodDagPassed, 40, errorSink,
                                          errorsBeforeShutdown);
    }

    if (selfCheckCode == 0 && launchOptions.LodSelectCheck)
    {
        selfCheckCode = FinalizeSelfCheck(lodSelectPassed, 41, errorSink,
                                          errorsBeforeShutdown);
    }

    if (selfCheckCode == 0 && launchOptions.LodCrackCheck)
    {
        selfCheckCode = FinalizeSelfCheck(lodCrackPassed, 42, errorSink,
                                          errorsBeforeShutdown);
    }

    if (selfCheckCode == 0 && launchOptions.LodGpuCheck)
    {
        selfCheckCode = FinalizeSelfCheck(lodGpuPassed, 43, errorSink,
                                          errorsBeforeShutdown);
    }

    if (selfCheckCode == 0 && launchOptions.PathTraceCheck)
    {
        // 39 而不是 37: 37 已经被 --meshlet-expand-overflow-check 占了。
        // 两条判据是并行做出来的, 各自挑了下一个空号。
        selfCheckCode = FinalizeSelfCheck(pathTracePassed, 39, errorSink,
                                          errorsBeforeShutdown);
    }

    // 移除日志 Sink 并关闭
    FLog::RemoveSink(&fileLogSink);
    FLog::RemoveSink(&errorSink);
    fileLogSink.Close();

    return selfCheckCode;
}
