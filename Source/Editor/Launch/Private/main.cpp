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

    /// 启用 TAA 亚像素抖动 (默认关 —— TAA 本身尚未落地)
    bool TemporalJitter = false;

    /// 分簇剔除自检: 回读簇表与 CPU 参照逐簇比对, 以退出码报告
    bool ClusterCheck = false;

    /// 片段着色器走分簇路径
    bool Clustered = false;

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
///   --jitter         启用 TAA 亚像素抖动 (Halton 2,3, 周期 16 帧)
///   --cluster-check  分簇剔除自检: 回读簇表与 CPU 参照比对, 以退出码报告
///   --clustered      片段着色器走分簇路径 (默认走暴力法)
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
        else if (WideEquals(arg, L"--clustered"))
        {
            options.Clustered = true;
        }
        else if (WideEquals(arg, L"--light-cull-check"))
        {
            options.LightCullCheck = true;
        }
        else if (WideEquals(arg, L"--cluster-check"))
        {
            options.ClusterCheck = true;
        }
        else if (WideEquals(arg, L"--jitter"))
        {
            options.TemporalJitter = true;
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
    // 没有这一条时, "--jitter 是个空开关"这种情况会让上面每一项都完美
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

    if (launchOptions.Clustered)
    {
        renderer.SetClusteredLighting(true);

        LIMX_LOG(LogLaunch, Display, "[Launch] 分簇光照已启用");
    }

    if (launchOptions.TemporalJitter)
    {
        renderer.SetTemporalJitterEnabled(true);

        LIMX_LOG(LogLaunch, Display,
                 "[Launch] TAA 亚像素抖动已启用 (Halton 2,3, 周期 16 帧)");
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

    // 移除日志 Sink 并关闭
    FLog::RemoveSink(&fileLogSink);
    FLog::RemoveSink(&errorSink);
    fileLogSink.Close();

    return selfCheckCode;
}
