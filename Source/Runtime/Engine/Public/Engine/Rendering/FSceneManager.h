// ============================================================
// 文件名称：FSceneManager.h
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：单向数据流 — FSceneManager 是 Engine→Luminance 的单向桥梁，
//          每帧从 LScene 收集渲染数据推送到 FRenderer，不持有双向引用，
//          保证渲染层与场景层的单向依赖。
//
//          剔除与排序属于"生产端"而非"消费端" — 渲染器拿到的应当是一份
//          已经可以照单全收的批次列表。把可见性判断留给渲染器，等于要求
//          每个 Pass 各自实现一遍，而 DepthPrePass 与 ForwardPass 一旦
//          剔除结果不一致，Early-Z 的 DepthCompareOp=Equal 就会失效。
// 功能描述：FSceneManager 渲染桥接单例 — 每帧调用 SyncScene() 遍历
//          LScene 所有 LNode，收集 LMeshTrait/LCameraTrait 数据，
//          执行视锥剔除与状态排序，更新 FRenderer 的批次列表和 FCamera。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                    │ 描述                           │
// │─────────────────────────│───────────────────────────────│
// │ Get()                   │ 获取单例                        │
// │ Initialize(renderer)    │ 绑定 FRenderer 实例             │
// │ Shutdown()              │ 解绑并清理                      │
// │ SyncScene(scene, dt)    │ 每帧同步场景数据到渲染器         │
// │ IsInitialized()         │ 是否已初始化                    │
// │ SetCullingEnabled(b)    │ 开关视锥剔除 (用于对照测量)      │
// │ SetSortingEnabled(b)    │ 开关状态排序 (用于对照测量)      │
// │ GetStats()              │ 上一帧的收集统计                │
//
// ── 结构体字段表 ──────────────────────────────────────────────
// │ 字段名                │ 类型                   │ 描述      │
// │──────────────────────│───────────────────────│─────────│
// │ m_Renderer           │ FRenderer*             │ 渲染器引用│
// │ m_SceneRenderObjects │ TArray<FRenderObject>  │ 每帧缓存  │
// │ m_Stats              │ FSceneSyncStats        │ 逐帧统计  │
// │ m_IsCullingEnabled   │ bool                   │ 剔除开关  │
// │ m_IsSortingEnabled   │ bool                   │ 排序开关  │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                             │
// │─────────────│──────────│─────────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M1.0 Engine 渲染桥接)   │
// │ 2026-08-29  │ LimxTeam  │ 批次粒度收集 + 逐帧统计           │
// │ 2026-08-29  │ LimxTeam  │ 视锥剔除 + 状态排序               │
// ============================================================

#pragma once

#include "Engine/EngineAPI.h"
#include "Engine/LScene.h"
#include "Engine/Rendering/LMeshTrait.h"
#include "Engine/Rendering/LCameraTrait.h"
#include "Renderer/Renderer/FRenderer.h"
#include "Core/Math/FFrustum.h"

namespace Limx
{

// ============================================================================
// FSceneSyncStats — 单帧收集统计
// ============================================================================

/// 一次 SyncScene 的产出统计
///
/// 剔除率与状态切换数是渲染吞吐的两个直接指标: 前者决定送进管线的三角形量,
/// 后者决定命令缓冲区里有多少次实际的绑定。没有这两个数, "优化"只能靠猜。
struct FSceneSyncStats
{
    /// 参与收集的网格 Trait 数
    UInt32 MeshTraitCount = 0;

    /// 收集到的绘制批次总数 (剔除前)
    UInt32 BatchCount = 0;

    /// 被视锥剔除掉的批次数
    UInt32 CulledCount = 0;

    /// 实际提交给渲染器的批次数 (不透明 + 半透明)
    UInt32 VisibleCount = 0;

    /// 其中的半透明批次数
    UInt32 TranslucentCount = 0;

    /// 全部批次的世界包围盒 (剔除**之前**)
    ///
    /// 阴影视锥据此拟合, 因此必须用剔除前的完整范围: 相机看不到的物体
    /// 照样会往可见区域投影, 用剔除后的范围会让画面外的物体不投影,
    /// 表现为转动相机时阴影凭空出现或消失。
    FBoundingBox SceneBounds;

    /// 可见批次覆盖的三角形数
    UInt64 VisibleTriangles = 0;

    /// 排序后仍然发生的材质切换次数 — 越接近材质种类数越好
    UInt32 MaterialSwitchCount = 0;

    /// 排序后仍然发生的顶点缓冲区切换次数
    UInt32 MeshSwitchCount = 0;

    /// 剔除率百分比 — 批次为零时返回 0
    LIMX_NODISCARD Float32 GetCullRatio() const
    {
        if (BatchCount == 0)
        {
            return 0.0f;
        }

        return static_cast<Float32>(CulledCount) /
               static_cast<Float32>(BatchCount) * 100.0f;
    }
};

// ============================================================================
// FTranslucentBackToFrontLess — 半透明排序策略
// ============================================================================

/// 半透明批次排序谓词 — 距离相机由远及近
///
/// Alpha 混合不满足交换律: dst = src*a + dst*(1-a) 的结果取决于写入顺序。
/// 近的先画、远的后画时, 远处物体会用已经混过近处颜色的背景再混一次,
/// 表现为"透过玻璃看到的东西颜色发灰"。必须严格由远及近。
///
/// 按包围盒中心到相机的距离平方排序 —— 开方是单调变换, 对排序结果无影响,
/// 而每个批次省一次 sqrt。
///
/// 放在公开头而非实现文件的匿名命名空间里: 这是一条渲染正确性策略,
/// 值得被单独测试, 也值得让读代码的人一眼找到。
struct FTranslucentBackToFrontLess
{
    FVector3 CameraPosition;

    LIMX_NODISCARD bool operator()(const FRenderObject& a,
                                   const FRenderObject& b) const
    {
        const Float32 distanceA =
            (a.WorldBounds.GetCenter() - CameraPosition).LengthSquared();
        const Float32 distanceB =
            (b.WorldBounds.GetCenter() - CameraPosition).LengthSquared();

        // 远的排在前面
        return distanceA > distanceB;
    }
};

// ============================================================================
// FSceneManager — Engine→Luminance 渲染桥接单例
// ============================================================================

class LIMX_ENGINE_API FSceneManager
{
public:
    LIMX_NON_COPYABLE(FSceneManager);

    /// 获取全局单例
    static FSceneManager& Get();

    // ====================================================================
    // 生命周期
    // ====================================================================

    /// 绑定 FRenderer — 必须在 FRenderer::Initialize 之后调用
    void Initialize(FRenderer* renderer);

    /// 解绑并清理缓存
    void Shutdown();

    LIMX_NODISCARD bool IsInitialized() const { return m_Renderer != nullptr; }

    // ====================================================================
    // 每帧同步
    // ====================================================================

    /// 遍历 LScene 所有 LNode，收集渲染数据并推送到 FRenderer
    ///
    /// 顺序是: 解析主相机 → 收集批次 → 视锥剔除 → 状态排序 → 推送。
    /// 相机必须最先解析 —— 剔除用的视锥来自本帧的相机, 用上一帧的会在
    /// 快速转向时把边缘物体错误剔掉一帧, 表现为画面边缘的闪烁。
    void SyncScene(const LScene* scene, Float32 deltaTime);

    // ====================================================================
    // 开关 — 供对照测量使用
    // ====================================================================

    /// 开关视锥剔除
    void SetCullingEnabled(bool enabled) { m_IsCullingEnabled = enabled; }
    LIMX_NODISCARD bool IsCullingEnabled() const { return m_IsCullingEnabled; }

    /// 开关状态排序
    void SetSortingEnabled(bool enabled) { m_IsSortingEnabled = enabled; }
    LIMX_NODISCARD bool IsSortingEnabled() const { return m_IsSortingEnabled; }

    // ====================================================================
    // 统计
    // ====================================================================

    LIMX_NODISCARD const FSceneSyncStats& GetStats() const { return m_Stats; }

private:
    FSceneManager()  = default;
    ~FSceneManager() = default;

    /// 解析主相机并写入渲染器; 返回本帧用于剔除的视锥
    /// @param outFrustum 输出视锥
    void ResolveCamera(const LScene* scene, FFrustum& outFrustum);

    /// 遍历场景收集全部绘制批次
    void CollectBatches(const LScene* scene);

    /// 按世界包围盒剔除不可见批次 — 就地压缩数组
    void CullBatches(const FFrustum& frustum);

    /// 按混合模式把批次拆到两条列表
    ///
    /// 半透明必须与不透明分开: 前者要求严格由远及近, 后者要求按状态聚类,
    /// 同一条列表上没有哪种排序能同时满足两者。
    void PartitionBatches();

    /// 不透明列表按材质与网格排序, 使相同状态的批次相邻
    void SortBatches();

    /// 半透明列表按到相机的距离由远及近排序
    void SortTranslucentBatches();

    /// 统计排序后的状态切换次数与三角形数
    void MeasureBatches();

    FRenderer*            m_Renderer = nullptr;

    /// 本帧收集到的全部批次 —— 剔除与拆分都在这条列表上就地进行
    TArray<FRenderObject> m_SceneRenderObjects;

    /// 拆分后的半透明批次
    TArray<FRenderObject> m_TranslucentObjects;

    FSceneSyncStats       m_Stats;

    bool                  m_IsCullingEnabled = true;
    bool                  m_IsSortingEnabled = true;
};

} // namespace Limx
