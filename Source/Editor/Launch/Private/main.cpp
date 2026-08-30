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
#include "AssetPipeline/FImageDecoder.h"

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
///   --furnace        白炉测试: 合成均匀环境 + 关闭直接光
///   --furnace-check  白炉自检: 断言 IBL 各级预计算, 以退出码报告
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
        else if (WideEquals(arg, L"--furnace"))
        {
            options.Furnace = true;
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
             "[基准] 状态切换: 材质 {} 次 | 网格 {} 次",
             sceneStats.MaterialSwitchCount, sceneStats.MeshSwitchCount);
    LIMX_LOG(LogLaunch, Log,
             "[基准] 帧耗时: 平均 {} ms | 最差 {} ms | 平均帧率 {} | 总帧数 {}",
             frameStats.AverageFrameMs, frameStats.WorstFrameMs,
             frameStats.AverageFps, frameStats.TotalFrames);

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
    if (launchOptions.FurnaceCheck)
    {
        const bool passed = RunFurnaceSelfTest(&renderContext);

        LRegistry::Get().Destroy(scene);
        FSceneManager::Get().Shutdown();
        renderer.Shutdown();
        renderContext.Shutdown();
        window.Destroy();

        FLog::RemoveSink(&fileLogSink);
        fileLogSink.Close();

        return passed ? 0 : 5;
    }

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
            LogBenchmarkReport(launchOptions, renderer, &renderContext);

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

    // 移除文件日志 Sink 并关闭
    FLog::RemoveSink(&fileLogSink);
    fileLogSink.Close();

    return 0;
}
