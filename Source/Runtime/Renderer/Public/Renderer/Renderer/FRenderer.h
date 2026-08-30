// ============================================================
// 文件名称：FRenderer.h
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：分层渲染编排 — FRenderer 是渲染管线的顶层编排器，
//          协调 FRenderContext 的帧生命周期与命令录制，
//          将"帧同步"与"渲染逻辑"解耦。
// 功能描述：渲染器主入口 — 管理渲染循环的初始化、每帧执行、关闭。
//          M0.5 集成: Pass 抽象层 (FPassManager + FDepthPrePass + FForwardPass)
//          + 材质系统 (FMaterialManager) + PBR Cook-Torrance BRDF 光照系统
//          (FLightManager) + 3 套描述符集 (set 0 ViewProj+纹理, set 1 材质,
//          set 2 光照) + 程序化几何体 + 棋盘格纹理。
// 技术特性：持有 FRenderContext 和 FWindow 的非拥有指针；
//          RenderFrame() 内部处理 BeginFrame→录制→EndFrame；
//          最小化时自动跳过渲染；交换链重建时自动重建帧缓冲；
//          Pass 系统: FPassManager 管理 FDepthPrePass→FForwardPass 执行顺序；
//          材质系统: FMaterialManager 管理默认材质和描述符集 (set 1)；
//          光照系统: FLightManager 管理光源数组和 UBO (set 2)；
//          管线布局: 3 个描述符集布局 + Push Constant (Model mat4)。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                        │ 描述                           │
// │──────────────────────────────│───────────────────────────────│
// │ Initialize()                 │ 绑定窗口/渲染上下文+初始化全部子系统 │
// │ Shutdown()                   │ 释放全部子系统资源               │
// │ RenderFrame()                │ 执行单帧渲染 (BeginFrame→Pass→EndFrame) │
// │ SetClearColor()              │ 设置清屏颜色                     │
// │ OnSwapchainRecreated()       │ 交换链重建后委托 PassManager 重建  │
// │ CreateUniformBuffers()       │ 创建 Uniform Buffer (ViewProj)   │
// │ CreateDescriptorResources()  │ 创建 set 0 描述符集布局+描述符集   │
// │ CreateTextureResources()     │ 创建棋盘格纹理+采样器+纹理视图    │
// │ CreatePipelineLayout()       │ 创建 3 套描述符集管线布局          │
// │ CreateLightingDescriptorSets()│ 创建 set 2 光照描述符集          │
// │ UpdateUniformBuffer()        │ 每帧更新 ViewProj 矩阵           │
//
// ── 结构体字段表 ──────────────────────────────────────────────
// │ 字段名                      │ 类型                              │ 描述            │
// │────────────────────────────│──────────────────────────────────│────────────────│
// │ m_Context                  │ FRenderContext*                  │ 渲染上下文        │
// │ m_Window                   │ FWindow*                        │ 目标窗口          │
// │ m_ClearColor               │ FLinearColor                    │ 清屏颜色          │
// │ m_PipelineLayout           │ FRHIPipelineLayoutHandle        │ 管线布局 (3 set)   │
// │ m_PassManager              │ TUniquePtr<FPassManager>        │ Pass 管理器       │
// │ m_DepthPrePass             │ TUniquePtr<FDepthPrePass>       │ 深度预 Pass       │
// │ m_ForwardPass              │ TUniquePtr<FForwardPass>        │ 前向渲染 Pass     │
// │ m_Texture                  │ FRHITextureHandle               │ 棋盘格纹理        │
// │ m_TextureView              │ FRHITextureViewHandle           │ 纹理视图          │
// │ m_Sampler                  │ FRHISamplerHandle               │ 纹理采样器        │
// │ m_RenderObjects            │ TArray<FRenderObject>           │ 场景渲染物体      │
// │ m_UniformBuffers           │ TArray<FRHIBufferHandle>        │ ViewProj UBO     │
// │ m_DescSetLayout            │ FRHIDescSetLayoutHandle         │ set 0 描述符布局  │
// │ m_DescriptorSets           │ TArray<FRHIDescriptorSetHandle> │ set 0 描述符集    │
// │ m_LightDescriptorSets      │ TArray<FRHIDescriptorSetHandle> │ set 2 光照描述符集 │
// │ m_DefaultMaterial           │ FMaterial*                     │ 默认 PBR 材质     │
// │ m_Camera                   │ FCamera                         │ 相机             │
// │ m_RotationAngle            │ Float32                         │ 累计旋转角度      │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-06  │ LimxTeam  │ 初始创建 (清屏渲染)              │
// │ 2026-04-07  │ LimxTeam  │ M0.1 三角形渲染                 │
// │ 2026-04-07  │ LimxTeam  │ M0.2 基础: VBO+UBO+MVP 矩阵    │
// │ 2026-04-07  │ LimxTeam  │ M0.2 深度缓冲区: D32_SFLOAT     │
// │ 2026-04-07  │ LimxTeam  │ M0.3 Index Buffer+立方体+法线光照│
// │ 2026-04-07  │ LimxTeam  │ M0.3 纹理采样: UV+棋盘格+sampler │
// │ 2026-04-07  │ LimxTeam  │ M0.5 集成: Pass/材质/光照子系统   │
// │ 2026-04-08  │ LimxTeam  │ M1.1 Launch 集成: 添加公开访问器   │
// ============================================================

#pragma once

#include "Renderer/RendererMinimal.h"
#include "RenderCore/Camera/FCamera.h"
#include "ApplicationCore/Input/FInputManager.h"
#include "RenderCore/Geometry/FGeometryGenerator.h"
#include "RenderCore/Material/FMaterial.h"

namespace Limx
{

// 前向声明
class FWindow;
class FRenderContext;
class FPassManager;
class FForwardPass;
class FShadowPass;
class FDepthPrePass;
class FSkyPass;
class FPostProcessPass;
class FEnvironmentMap;
class FMaterial;

// ============================================================================
// FViewProjUBO — View+Projection Uniform Buffer 数据布局 (128 bytes)
// Model 矩阵通过 Push Constant 逐物体传递
// ============================================================================

struct FViewProjUBO
{
    FMatrix View;
    FMatrix Proj;
};

// ============================================================================
// FModelPushConstant — 逐物体 Model 矩阵 Push Constant 数据 (64 bytes)
// ============================================================================

struct FModelPushConstant
{
    FMatrix Model;
};

// ============================================================================
// FRenderObject — 一次绘制批次 (渲染视图, 非资源所有者)
// ============================================================================

/// 单次 DrawIndexed 所需的全部状态
///
/// 这是"渲染视图"而非资源: 缓冲区句柄由 FRenderResourceManager 拥有,
/// 此处只是本帧的一份只读快照。列表每帧由 FSceneManager 重建，
/// 渲染器不得销毁其中任何 GPU 对象。
///
/// 一个网格按材质切分为若干段, 每段一次绘制调用 —— 因此这里的粒度是
/// "批次"而非"物体"。Sponza 这类单网格多材质的场景没有别的表达方式。
struct FRenderObject
{
    /// GPU 顶点缓冲区 (非拥有)
    FRHIBufferHandle VertexBuffer;
    UInt32           VertexCount = 0;

    /// GPU 索引缓冲区 (非拥有)
    FRHIBufferHandle IndexBuffer;

    /// 本批次在索引缓冲区中的起始位置 (以索引个数计, 非字节)
    UInt32           IndexOffset = 0;

    /// 本批次的索引个数
    UInt32           IndexCount  = 0;

    /// 索引宽度 — 由网格顶点数决定, 绘制时必须与缓冲区实际宽度一致
    EIndexType       IndexType   = EIndexType::UInt32;

    /// 世界空间变换 (Position + Rotation + Scale)
    FTransform       Transform;

    /// 世界空间包围盒 — 供视锥剔除与半透明排序使用
    FBoundingBox     WorldBounds;

    /// 混合模式 — 决定这一批走哪条管线
    ///
    /// 深度预 Pass 需要区分 Opaque 与 Masked: 后者必须做同样的 alpha 测试,
    /// 否则会为完全透明的纹素写入深度, 把它背后的东西挡掉。
    EMaterialBlendMode BlendMode = EMaterialBlendMode::Opaque;

    /// 是否双面渲染 — 与 BlendMode 一起决定管线排列
    ///
    /// 植被与薄片几何必须双面: 单面剔除下每片叶子只剩朝向相机的那半边。
    /// 深度预 Pass 的剔除模式必须与前向 Pass 完全一致, 否则被剔掉的那半边
    /// 在深度缓冲区里没有值, DepthCompareOp=Equal 会把它整片丢掉。
    bool IsDoubleSided = false;

    /// set 1 材质描述符集 — 由 FMaterialManager 分配
    FRHIDescriptorSetHandle MaterialDescriptorSet;

    /// 调试名称
    const AnsiChar*  DebugName      = "Unnamed";
};

// ============================================================================
// FRenderFrameStats — 帧耗时统计
// ============================================================================

/// 滚动窗口内的帧耗时统计
///
/// 单帧耗时抖动极大 (交换链呈现、驱动调度、Windows 合成都会掺进来),
/// 拿单帧数字做比较毫无意义。这里保留一个固定长度的滚动窗口, 同时给出
/// 平均值与最差值 —— 优化的收益要看平均, 卡顿要看最差。
struct FRenderFrameStats
{
    /// 滚动窗口长度 — 60 帧约合 1 秒
    static constexpr UInt32 kWindowSize = 60;

    /// 最近一帧的 CPU 侧耗时 (毫秒)
    Float32 LastFrameMs = 0.0f;

    /// 滚动窗口内的平均耗时 (毫秒)
    Float32 AverageFrameMs = 0.0f;

    /// 滚动窗口内的最差耗时 (毫秒)
    Float32 WorstFrameMs = 0.0f;

    /// 由平均耗时换算的帧率
    Float32 AverageFps = 0.0f;

    /// 自初始化以来渲染的帧数
    UInt64 TotalFrames = 0;

    /// 最近一帧实际提交的绘制批次数
    UInt32 DrawCallCount = 0;
};

// ============================================================================
// FRenderer — 渲染器主入口
// ============================================================================

class FRenderer
{
public:
    LIMX_NON_COPYABLE(FRenderer);

    FRenderer();
    ~FRenderer();

    /// 初始化渲染器
    /// 创建场景 → UBO → 纹理 → 描述符 → 材质 → 光照 → 管线布局 → Pass 系统
    /// @param window   目标窗口 (非拥有)
    /// @param context  渲染上下文 (非拥有，必须已初始化)
    /// @return Success 或错误码
    ERHIResult Initialize(FWindow* window, FRenderContext* context);

    /// 关闭渲染器
    void Shutdown();

    /// 执行单帧渲染
    /// 内部处理: 窗口最小化跳过 → 交换链重建 → BeginFrame → 录制 → EndFrame
    void RenderFrame();

    /// 交换链重建后委托 FPassManager 重建共享深度和 Framebuffer
    ERHIResult OnSwapchainRecreated();

    /// 设置清屏颜色
    void SetClearColor(const FLinearColor& color) { m_ClearColor = color; }

    /// 获取当前清屏颜色
    LIMX_NODISCARD const FLinearColor& GetClearColor() const
    {
        return m_ClearColor;
    }

    /// 获取相机引用 (可修改)
    LIMX_NODISCARD FCamera& GetCamera() { return m_Camera; }

    /// 获取相机引用 (只读)
    LIMX_NODISCARD const FCamera& GetCamera() const { return m_Camera; }

    /// 获取 Pass 管理器 (供外部注册额外 Pass，如 LUIPass)
    LIMX_NODISCARD FPassManager* GetPassManager() { return m_PassManager.Get(); }

    /// 获取渲染上下文 (供外部子系统初始化使用)
    LIMX_NODISCARD FRenderContext* GetRenderContext() { return m_Context; }

    /// 获取默认材质 (供 LMeshTrait 设置材质)
    LIMX_NODISCARD FMaterial* GetDefaultMaterial() { return m_DefaultMaterial; }

    /// 替换本帧的不透明/蒙版批次列表 (由 FSceneManager::SyncScene 每帧调用)
    void SetRenderObjects(const TArray<FRenderObject>& objects) { m_RenderObjects = objects; }

    /// 替换本帧的半透明批次列表
    ///
    /// 与不透明列表分开而非合并后按标志过滤: 两者的绘制顺序要求相反 ——
    /// 不透明按状态聚类以减少绑定, 半透明必须严格由远及近, 合在一起就没有
    /// 哪一种排序能同时满足。
    void SetTranslucentObjects(const TArray<FRenderObject>& objects)
    {
        m_TranslucentObjects = objects;
    }

    /// 获取不透明/蒙版批次列表 (只读)
    LIMX_NODISCARD const TArray<FRenderObject>& GetRenderObjects() const { return m_RenderObjects; }

    /// 获取半透明批次列表 (只读)
    LIMX_NODISCARD const TArray<FRenderObject>& GetTranslucentObjects() const
    {
        return m_TranslucentObjects;
    }

    /// 替换本帧的阴影投射体列表 (未经相机剔除)
    void SetShadowCasterObjects(const TArray<FRenderObject>& objects)
    {
        m_ShadowCasterObjects = objects;
    }

    /// 设置色调映射的线性曝光倍数
    void SetExposure(Float32 exposure);

    LIMX_NODISCARD Float32 GetExposure() const;

    // ====================================================================
    // 环境光照
    // ====================================================================

    /// 绑定环境贴图 — 同时驱动天空盒与 IBL 漫反射项
    ///
    /// 传 nullptr 即解绑, 环境项退回常数环境光。切换关卡时**必须**先解绑
    /// 再销毁贴图 —— 否则描述符集里留着指向已释放图像的视图。
    ///
    /// 接受整个对象而非若干句柄: 天空要环境贴图, 漫反射要辐照度贴图,
    /// 镜面还会再要预滤波贴图与 BRDF 查找表。逐个传句柄的参数表会一直
    /// 膨胀下去, 而且很容易出现"传了三个忘了第四个"的半绑定状态。
    void SetEnvironmentMap(const FEnvironmentMap* environment);

    /// 设置天空强度的线性倍数
    void SetSkyIntensity(Float32 intensity);

    LIMX_NODISCARD Float32 GetSkyIntensity() const;

    /// 设置 IBL 强度的线性倍数 —— 与天空强度分开, 便于单独配平
    void SetIblIntensity(Float32 intensity);

    LIMX_NODISCARD Float32 GetIblIntensity() const { return m_IblIntensity; }

    LIMX_NODISCARD bool HasEnvironmentMap() const;

    /// 设置场景世界包围盒 — 阴影正交视锥据此拟合
    ///
    /// 由 FSceneManager 在收集批次时顺带算出。给得过大浪费阴影贴图精度,
    /// 过小则场景边缘落在贴图之外, 那里会被判为完全不在阴影中 ——
    /// 表现为远处物体突然失去阴影。
    void SetSceneBounds(const FBoundingBox& bounds) { m_SceneBounds = bounds; }

    /// 获取帧耗时统计
    LIMX_NODISCARD const FRenderFrameStats& GetFrameStats() const
    {
        return m_FrameStats;
    }

    /// 清空滚动窗口 — 用于跳过启动阶段的预热帧后重新计时
    void ResetFrameStats();

    /// 设置场景渲染后回调 — 在所有场景 Pass 执行完毕、EndFrame 之前调用
    /// 用于 UI 渲染叠加等需要录制到同一命令缓冲区的操作
    void SetPostSceneRenderCallback(const TFunction<void()>& callback)
    {
        m_PostSceneRenderCallback = callback;
    }

private:
    /// 创建 Uniform Buffer (每帧一个，View+Proj 矩阵)
    ERHIResult CreateUniformBuffers();

    /// 创建 set 0 描述符集布局 + 分配描述符集 + 写入绑定
    ERHIResult CreateDescriptorResources();

    /// 创建棋盘格纹理 + 采样器 + 纹理视图
    ERHIResult CreateTextureResources();

    /// 创建 1x1 黑色立方体贴图 —— 没有环境贴图时的描述符占位
    ///
    /// 着色器里出现的描述符必须在管线绑定时有效, 靠 uniform 分支跳过采样
    /// 并不能免除这一点。占位图取黑色而非白色: 万一开关判断写错, 黑色的
    /// 表现是"环境光没了", 白色则是"整个场景发白" —— 前者更容易定位。
    ERHIResult CreateFallbackCubeMap();

    /// 把辐照度视图写进全部帧的光照描述符集
    void UpdateIrradianceDescriptors(FRHITextureViewHandle irradianceView,
                                     FRHISamplerHandle     sampler);

    /// 销毁纹理资源 (纹理 + 纹理视图 + 采样器)
    void DestroyTextureResources();

    /// 创建管线布局 (set 0 + set 1 材质 + set 2 光照 + Push Constant)
    ERHIResult CreatePipelineLayout();

    /// 创建 set 2 光照描述符集 (每帧一个，绑定 FLightingUBO)
    ERHIResult CreateLightingDescriptorSets();

    /// 销毁 UBO + 描述符资源 (含光照描述符集)
    void DestroyBufferResources();

    /// 每帧更新 View+Proj 矩阵到当前帧的 Uniform Buffer
    void UpdateUniformBuffer(UInt32 frameIndex);

    /// 把一帧的耗时并入滚动窗口并重算统计
    void RecordFrameTime(Float32 frameMilliseconds);

    /// 释放旧尺寸资源 → 重建交换链/帧同步资源 → 重建 Pass 尺寸资源
    ERHIResult RecreateSwapchainResources();

    // ====================================================================
    // 成员
    // ====================================================================

    FRenderContext* m_Context = nullptr;
    FWindow*        m_Window  = nullptr;

    /// 清屏颜色 — 深蓝灰色 (Limx 品牌色调)
    FLinearColor m_ClearColor = FLinearColor(0.01f, 0.01f, 0.02f, 1.0f);

    // ---- 管线布局 (set 0 + set 1 + set 2 + Push Constant) ----
    FRHIPipelineLayoutHandle          m_PipelineLayout;

    // ---- Pass 系统 (通过 TUniquePtr 持有，析构在 .cpp 中完成) ----
    TUniquePtr<FPassManager>          m_PassManager;
    TUniquePtr<FShadowPass>           m_ShadowPass;
    TUniquePtr<FDepthPrePass>         m_DepthPrePass;
    TUniquePtr<FSkyPass>              m_SkyPass;
    TUniquePtr<FForwardPass>          m_ForwardPass;
    TUniquePtr<FPostProcessPass>      m_PostProcessPass;

    /// 场景包围盒 — 阴影视锥的拟合依据, 由 SetSceneBounds 每帧提供
    FBoundingBox                      m_SceneBounds;

    // ---- 纹理资源 (棋盘格) ----
    FRHITextureHandle                 m_Texture;
    FRHITextureViewHandle             m_TextureView;

    // ---- IBL 占位资源 ----
    FRHITextureHandle                 m_FallbackCubeTexture;
    FRHITextureViewHandle             m_FallbackCubeView;
    FRHISamplerHandle                 m_FallbackCubeSampler;

    /// IBL 强度倍数 —— 与天空强度分开记, 两者的合适取值往往不同
    Float32                           m_IblIntensity = 1.0f;
    FRHISamplerHandle                 m_Sampler;

    // ---- 本帧渲染视图 (非拥有 —— GPU 资源归 FRenderResourceManager) ----
    TArray<FRenderObject>             m_RenderObjects;
    TArray<FRenderObject>             m_TranslucentObjects;
    TArray<FRenderObject>             m_ShadowCasterObjects;

    // ---- Uniform Buffer (每帧一个用于 View+Proj 矩阵) ----
    TArray<FRHIBufferHandle>          m_UniformBuffers;

    // ---- set 0 描述符集 (ViewProj UBO + 纹理) ----
    FRHIDescSetLayoutHandle           m_DescSetLayout;
    TArray<FRHIDescriptorSetHandle>   m_DescriptorSets;

    // ---- set 2 光照描述符集 (每帧一个，绑定 FLightingUBO) ----
    TArray<FRHIDescriptorSetHandle>   m_LightDescriptorSets;

    // ---- 默认材质 (非拥有，由 FMaterialManager 管理生命周期) ----
    FMaterial*                        m_DefaultMaterial = nullptr;

    // ---- 相机 ----
    FCamera                           m_Camera;

    // ---- 时间状态 ----
    Float64                           m_LastFrameTime = 0.0;

    // ---- 帧耗时统计 (环形窗口) ----
    FRenderFrameStats                 m_FrameStats;
    Float32                           m_FrameTimeWindow[FRenderFrameStats::kWindowSize] = {};
    UInt32                            m_FrameTimeCursor = 0;
    UInt32                            m_FrameTimeFilled = 0;

    // ---- 场景渲染后回调 (供 UI 叠加渲染等) ----
    TFunction<void()>                 m_PostSceneRenderCallback;
};

} // namespace Limx
