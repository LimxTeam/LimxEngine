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
// │ CreateScene()                │ 创建场景物体 (VBO+IBO)           │
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

namespace Limx
{

// 前向声明
class FWindow;
class FRenderContext;
class FPassManager;
class FForwardPass;
class FDepthPrePass;
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
// FRenderObject — 可渲染物体 (VBO + IBO + 变换)
// ============================================================================

struct FRenderObject
{
    /// GPU 顶点缓冲区
    FRHIBufferHandle VertexBuffer;
    UInt32           VertexCount = 0;

    /// GPU 索引缓冲区
    FRHIBufferHandle IndexBuffer;
    UInt32           IndexCount  = 0;

    /// 世界空间变换 (Position + Rotation + Scale)
    FTransform       Transform;

    /// 是否启用旋转动画
    bool             IsAnimated     = false;

    /// 旋转速度 (弧度/秒)
    Float32          RotationSpeed  = 0.0f;

    /// set 1 材质描述符集 — 由 FMaterialManager 分配
    FRHIDescriptorSetHandle MaterialDescriptorSet;

    /// 调试名称
    const AnsiChar*  DebugName      = "Unnamed";
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

    /// 替换本帧渲染对象列表 (由 FSceneManager::SyncScene 每帧调用)
    void SetRenderObjects(const TArray<FRenderObject>& objects) { m_RenderObjects = objects; }

    /// 获取渲染对象列表 (只读)
    LIMX_NODISCARD const TArray<FRenderObject>& GetRenderObjects() const { return m_RenderObjects; }

    /// 设置场景渲染后回调 — 在所有场景 Pass 执行完毕、EndFrame 之前调用
    /// 用于 UI 渲染叠加等需要录制到同一命令缓冲区的操作
    void SetPostSceneRenderCallback(const TFunction<void()>& callback)
    {
        m_PostSceneRenderCallback = callback;
    }

private:
    /// 创建场景物体 (立方体 + 球体 + 地面)
    ERHIResult CreateScene();

    /// 销毁场景物体 (VBO/IBO)
    void DestroyScene();

    /// 从 FMeshData 创建单个渲染物体的 GPU 缓冲区
    ERHIResult CreateRenderObjectBuffers(
        const FMeshData& meshData, FRenderObject& outObject);

    /// 创建 Uniform Buffer (每帧一个，View+Proj 矩阵)
    ERHIResult CreateUniformBuffers();

    /// 创建 set 0 描述符集布局 + 分配描述符集 + 写入绑定
    ERHIResult CreateDescriptorResources();

    /// 创建棋盘格纹理 + 采样器 + 纹理视图
    ERHIResult CreateTextureResources();

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
    TUniquePtr<FDepthPrePass>         m_DepthPrePass;
    TUniquePtr<FForwardPass>          m_ForwardPass;

    // ---- 纹理资源 (棋盘格) ----
    FRHITextureHandle                 m_Texture;
    FRHITextureViewHandle             m_TextureView;
    FRHISamplerHandle                 m_Sampler;

    // ---- 场景物体 ----
    TArray<FRenderObject>             m_RenderObjects;

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

    // ---- 动画/时间状态 ----
    Float32                           m_RotationAngle = 0.0f;
    Float64                           m_LastFrameTime = 0.0;

    // ---- 场景渲染后回调 (供 UI 叠加渲染等) ----
    TFunction<void()>                 m_PostSceneRenderCallback;
};

} // namespace Limx
