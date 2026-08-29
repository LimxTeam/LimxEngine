// ============================================================
// 文件名称：FSceneManager.cpp
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：单向同步 — SyncScene 每帧执行 Engine→Luminance 数据同步，
//          只向 FRenderer 写入，不读取渲染状态，保证单向依赖。
//
//          剔除就地压缩而非另建数组 — 批次数在大场景里是万级，每帧多一次
//          全量拷贝就是每帧多几百 KB 的内存带宽。原地覆写把开销压到只剩
//          被保留的那部分元素。
//
//          排序键按"切换代价"降序 — 材质描述符集的切换代价高于顶点缓冲区
//          绑定，因此材质是主键。反过来排会让同一材质被反复重新绑定。
// 功能描述：FSceneManager 完整实现 — 单例初始化、批次收集、视锥剔除、
//          状态排序、统计与推送
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                             │
// │─────────────│──────────│─────────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M1.0 Engine 渲染桥接)   │
// │ 2026-08-29  │ LimxTeam  │ 批次粒度收集 + 逐帧统计           │
// │ 2026-08-29  │ LimxTeam  │ 视锥剔除 + 状态排序               │
// ============================================================

#include "Engine/EngineMinimal.h"
#include "Core/Containers/TSortAlgorithms.h"

namespace Limx
{

LIMX_DECLARE_LOG_CATEGORY(LogEngine)

namespace
{

/// 批次排序谓词 — 材质为主键, 网格为次键
///
/// 目标是让"绑定同一材质描述符集"的批次连续出现, 录制命令时即可跳过重复绑定。
/// 次键取顶点缓冲区, 让同一网格的多个子网格也聚在一起。
struct FBatchStateLess
{
    LIMX_NODISCARD bool operator()(const FRenderObject& a,
                                   const FRenderObject& b) const
    {
        // 剔除模式是主键中的主键 —— 它决定换哪条管线, 而管线切换的代价
        // 高于描述符集切换。单面与双面交替出现会让管线来回重绑。
        if (a.IsDoubleSided != b.IsDoubleSided)
        {
            return static_cast<int>(a.IsDoubleSided) <
                   static_cast<int>(b.IsDoubleSided);
        }

        if (a.MaterialDescriptorSet.Packed != b.MaterialDescriptorSet.Packed)
        {
            return a.MaterialDescriptorSet.Packed <
                   b.MaterialDescriptorSet.Packed;
        }

        if (a.VertexBuffer.Packed != b.VertexBuffer.Packed)
        {
            return a.VertexBuffer.Packed < b.VertexBuffer.Packed;
        }

        if (a.IndexBuffer.Packed != b.IndexBuffer.Packed)
        {
            return a.IndexBuffer.Packed < b.IndexBuffer.Packed;
        }

        // 同一网格内按索引偏移排序 —— 顺序访问索引缓冲区对缓存更友好
        return a.IndexOffset < b.IndexOffset;
    }
};

} // namespace

// ============================================================================
// 单例
// ============================================================================

FSceneManager& FSceneManager::Get()
{
    static FSceneManager s_Instance;
    return s_Instance;
}

// ============================================================================
// 生命周期
// ============================================================================

void FSceneManager::Initialize(FRenderer* renderer)
{
    LIMX_CHECK(renderer != nullptr);
    m_Renderer = renderer;
    LIMX_LOG(LogEngine, Log, "[FSceneManager] 初始化完成，绑定 FRenderer");
}

void FSceneManager::Shutdown()
{
    m_Renderer = nullptr;
    m_Stats    = FSceneSyncStats();
    m_SceneRenderObjects.Clear();
    m_TranslucentObjects.Clear();
    LIMX_LOG(LogEngine, Log, "[FSceneManager] 已关闭");
}

// ============================================================================
// SyncScene — 每帧同步 LScene → FRenderer
// ============================================================================

void FSceneManager::SyncScene(const LScene* scene, Float32 deltaTime)
{
    if (m_Renderer == nullptr || scene == nullptr)
    {
        return;
    }

    (void)deltaTime;

    const UInt32 previousVisible = m_Stats.VisibleCount;

    m_Stats = FSceneSyncStats();

    // 1. 相机先行 —— 剔除视锥必须来自本帧的相机
    FFrustum frustum;
    ResolveCamera(scene, frustum);

    // 2. 收集全部批次
    CollectBatches(scene);

    // 3. 视锥剔除
    if (m_IsCullingEnabled)
    {
        CullBatches(frustum);
    }

    // 4. 按混合模式拆分 —— 必须在排序之前, 两条列表的排序规则不同
    PartitionBatches();

    // 5. 各自排序
    if (m_IsSortingEnabled)
    {
        SortBatches();
    }

    // 半透明的由远及近排序**不受 --no-sort 影响** —— 它不是优化而是正确性
    // 要求, 关掉它得到的不是"慢一点", 而是错误的混合结果。
    SortTranslucentBatches();

    // 6. 统计
    MeasureBatches();

    // 7. 推送
    m_Renderer->SetRenderObjects(m_SceneRenderObjects);
    m_Renderer->SetTranslucentObjects(m_TranslucentObjects);
    m_Renderer->SetSceneBounds(m_Stats.SceneBounds);

    // 只在可见批次数变化时输出 —— 每帧一行会淹没日志, 而"批次数突然变成 0"
    // 恰恰是最需要被看见的事件。
    if (m_Stats.VisibleCount != previousVisible)
    {
        LIMX_LOG(LogEngine, Log,
                 "[FSceneManager] 可见批次 {} → {} (共 {} 个, 剔除 {} 个, "
                 "半透明 {} 个 | 材质切换 {} 次, 网格切换 {} 次)",
                 previousVisible, m_Stats.VisibleCount, m_Stats.BatchCount,
                 m_Stats.CulledCount, m_Stats.TranslucentCount,
                 m_Stats.MaterialSwitchCount, m_Stats.MeshSwitchCount);
    }
}

// ============================================================================
// ResolveCamera — 解析主相机并导出剔除视锥
// ============================================================================

void FSceneManager::ResolveCamera(const LScene* scene, FFrustum& outFrustum)
{
    const TArray<LNode*>& nodes = scene->GetAllNodes();

    bool foundMainCamera = false;

    for (SizeType i = 0; i < nodes.GetSize(); ++i)
    {
        LNode* node = nodes[i];
        if (node == nullptr || node->IsPendingDestroy())
        {
            continue;
        }

        LCameraTrait* camTrait = node->GetTrait<LCameraTrait>();
        if (camTrait != nullptr && camTrait->IsMain() && camTrait->IsEnabled())
        {
            FCamera& cam = m_Renderer->GetCamera();
            camTrait->SetAspectRatio(cam.GetAspectRatio());

            FMatrix view = camTrait->BuildViewMatrix();
            FMatrix proj = camTrait->BuildProjectionMatrix();
            cam.SetExternalMatrices(view, proj,
                                    camTrait->GetCameraWorldPosition());
            foundMainCamera = true;
            break;
        }
    }

    if (!foundMainCamera)
    {
        m_Renderer->GetCamera().ClearExternalMatrices();
    }

    // 视锥统一从渲染器最终采用的矩阵导出 —— 无论矩阵来自相机 Trait 还是
    // 渲染器内置相机, 剔除依据都必须与实际绘制用的矩阵完全一致。
    const FCamera& camera = m_Renderer->GetCamera();

    outFrustum = FFrustum::FromViewProjection(
        camera.GetProjectionMatrix() * camera.GetViewMatrix());
}

// ============================================================================
// CollectBatches — 遍历场景收集绘制批次
// ============================================================================

void FSceneManager::CollectBatches(const LScene* scene)
{
    // 保留上一帧的容量, 稳定场景下不再分配
    m_SceneRenderObjects.Clear();

    const TArray<LNode*>& nodes = scene->GetAllNodes();

    for (SizeType i = 0; i < nodes.GetSize(); ++i)
    {
        LNode* node = nodes[i];
        if (node == nullptr || node->IsPendingDestroy())
        {
            continue;
        }

        LMeshTrait* meshTrait = node->GetTrait<LMeshTrait>();
        if (meshTrait != nullptr && meshTrait->IsEnabled())
        {
            ++m_Stats.MeshTraitCount;
            meshTrait->BuildRenderObjects(m_SceneRenderObjects);
        }
    }

    m_Stats.BatchCount = static_cast<UInt32>(m_SceneRenderObjects.GetSize());

    // 阴影视锥的拟合依据 —— 在剔除之前累积, 见 FSceneSyncStats::SceneBounds
    for (SizeType i = 0; i < m_SceneRenderObjects.GetSize(); ++i)
    {
        const FBoundingBox& bounds = m_SceneRenderObjects[i].WorldBounds;

        if (bounds.IsValid())
        {
            m_Stats.SceneBounds = m_Stats.SceneBounds.Union(bounds);
        }
    }
}

// ============================================================================
// CullBatches — 视锥剔除 (就地压缩)
// ============================================================================

void FSceneManager::CullBatches(const FFrustum& frustum)
{
    SizeType writeIndex = 0;

    for (SizeType readIndex = 0; readIndex < m_SceneRenderObjects.GetSize();
         ++readIndex)
    {
        const FRenderObject& object = m_SceneRenderObjects[readIndex];

        // 包围盒无效说明该批次没有可用的空间信息 —— 保守地保留它。
        // 把"信息缺失"当成"不可见"会静默丢掉物体, 且极难溯源。
        const bool isVisible = !object.WorldBounds.IsValid() ||
                               frustum.IsAABBVisible(object.WorldBounds);

        if (!isVisible)
        {
            continue;
        }

        if (writeIndex != readIndex)
        {
            m_SceneRenderObjects[writeIndex] = m_SceneRenderObjects[readIndex];
        }

        ++writeIndex;
    }

    m_Stats.CulledCount =
        static_cast<UInt32>(m_SceneRenderObjects.GetSize() - writeIndex);

    m_SceneRenderObjects.SetSize(writeIndex);
}

// ============================================================================
// PartitionBatches — 按混合模式拆分
// ============================================================================

void FSceneManager::PartitionBatches()
{
    m_TranslucentObjects.Clear();

    SizeType writeIndex = 0;

    for (SizeType readIndex = 0; readIndex < m_SceneRenderObjects.GetSize();
         ++readIndex)
    {
        const FRenderObject& object = m_SceneRenderObjects[readIndex];

        // 分类规则集中在 IsBlendedMode 一处 —— 见其文档说明。
        // Masked 留在不透明列表: 它靠 discard 实现镂空, 不需要混合,
        // 也照常写深度。
        if (IsBlendedMode(object.BlendMode))
        {
            m_TranslucentObjects.Add(object);
            continue;
        }

        if (writeIndex != readIndex)
        {
            m_SceneRenderObjects[writeIndex] = m_SceneRenderObjects[readIndex];
        }

        ++writeIndex;
    }

    m_SceneRenderObjects.SetSize(writeIndex);

    m_Stats.TranslucentCount =
        static_cast<UInt32>(m_TranslucentObjects.GetSize());
}

// ============================================================================
// SortTranslucentBatches — 由远及近
// ============================================================================

void FSceneManager::SortTranslucentBatches()
{
    if (m_TranslucentObjects.GetSize() < 2)
    {
        return;
    }

    FTranslucentBackToFrontLess predicate;
    predicate.CameraPosition = m_Renderer->GetCamera().GetPosition();

    Sort(m_TranslucentObjects.GetData(), m_TranslucentObjects.GetSize(),
         predicate);
}

// ============================================================================
// SortBatches — 按材质与网格排序
// ============================================================================

void FSceneManager::SortBatches()
{
    if (m_SceneRenderObjects.GetSize() < 2)
    {
        return;
    }

    Sort(m_SceneRenderObjects.GetData(), m_SceneRenderObjects.GetSize(),
         FBatchStateLess());
}

// ============================================================================
// MeasureBatches — 统计状态切换与三角形数
// ============================================================================

void FSceneManager::MeasureBatches()
{
    m_Stats.VisibleCount =
        static_cast<UInt32>(m_SceneRenderObjects.GetSize() +
                            m_TranslucentObjects.GetSize());

    for (SizeType i = 0; i < m_TranslucentObjects.GetSize(); ++i)
    {
        m_Stats.VisibleTriangles += m_TranslucentObjects[i].IndexCount / 3;
    }

    if (m_SceneRenderObjects.GetSize() == 0)
    {
        return;
    }

    // 首个批次必然要绑定一次材质与网格, 因此计数从 1 起
    m_Stats.MaterialSwitchCount = 1;
    m_Stats.MeshSwitchCount     = 1;

    for (SizeType i = 0; i < m_SceneRenderObjects.GetSize(); ++i)
    {
        const FRenderObject& object = m_SceneRenderObjects[i];

        m_Stats.VisibleTriangles += object.IndexCount / 3;

        if (i == 0)
        {
            continue;
        }

        const FRenderObject& previous = m_SceneRenderObjects[i - 1];

        if (object.MaterialDescriptorSet.Packed !=
            previous.MaterialDescriptorSet.Packed)
        {
            ++m_Stats.MaterialSwitchCount;
        }

        if (object.VertexBuffer.Packed != previous.VertexBuffer.Packed)
        {
            ++m_Stats.MeshSwitchCount;
        }
    }
}

} // namespace Limx
