// ============================================================
// 文件名称：FMeshletGrouper.h
// 创建时间：2026-09-02
// 创建者  ：LimxTeam
// 设计哲学：**组与组之间的边界顶点必须一动不动。**
//
//          这是虚拟几何"无裂缝"的全部依据，而且它是一条能逐位验证的话，
//          不是一句设计意图。
//
//          为什么非要分组再简化，而不是整张网格一起简化：LOD 要能**逐块**
//          选层 —— 近处的一块用高模、远处的一块用低模。那就要求每一块能
//          独立简化。而两块各自简化之后，它们共享的那条边界如果各自动了，
//          就对不上 —— 画面上是一条能看见背景的缝。
//
//          锁住边界的代价是那里简化不动，于是边界会随着层数升高越积越多。
//          Nanite 的解法是**每一层重新分组**，让这一层的边界落在上一层的
//          组内部，于是它在下一层就能被简化掉。第三天建 DAG 时用的正是
//          这一点，而本文件是它的前提。
//
// 功能描述：把 meshlet 按邻接关系分成若干组，并标出组间边界顶点。
// 技术特性：按共享边建 meshlet 邻接图；从度数最小的 meshlet 起贪心生长，
//          每次并入"与本组共享边最多"的邻居；组间边界顶点 = 被两个以上
//          组的三角形用到的顶点（按位置焊接之后判定）。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                    │ 描述                           │
// │──────────────────────────│───────────────────────────────│
// │ Build()                  │ 由 meshlet 切分结果分组         │
// │ ExtractGroupMesh()       │ 取出一个组的独立网格 + 锁定表   │
//
// ── 结构体表 ──────────────────────────────────────────────────
// │ 结构体名                   │ 描述                           │
// │──────────────────────────│───────────────────────────────│
// │ FMeshletGroupOptions     │ 组大小的目标与上下限            │
// │ FMeshletGroup            │ 一个组占 GroupMeshlets 的区间   │
// │ FMeshletGroupResult      │ 分组 + 组间边界顶点标记         │
// │ FMeshletGroupMesh        │ 一个组抽出来的独立网格          │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-09-02  │ LimxTeam  │ 初始创建                        │
// ============================================================

#pragma once

// RenderCoreMinimal 必须先行 —— 理由见 FGeometryGenerator.h 顶部那段。
#include "RenderCore/RenderCoreMinimal.h"

#include "AssetPipeline/FAssetTypes.h"
#include "RenderCore/Geometry/FMeshletBuilder.h"

namespace Limx
{

// meshlet 分组日志类别
LIMX_DECLARE_LOG_CATEGORY(LogMeshletGrouper)

// ============================================================================
// FMeshletGroupOptions
// ============================================================================

/// 分组参数
struct FMeshletGroupOptions
{
    /// 目标每组多少个 meshlet
    ///
    /// 取 16 是个折中: 组太小则组间边界占比高 (锁死的顶点多, 简化不动),
    /// 组太大则 LOD 的粒度粗 (一整块只能整体换层, 近处远处被迫同一层)。
    UInt32 TargetGroupSize = 16;

    /// 每组至多多少个 —— 生长到这个数就收
    UInt32 MaxGroupSize = 32;
};

// ============================================================================
// FMeshletGroup
// ============================================================================

/// 一个组 —— 它在 FMeshletGroupResult::GroupMeshlets 里占的区间
struct FMeshletGroup
{
    UInt32 FirstMeshlet = 0;
    UInt32 MeshletCount = 0;
};

// ============================================================================
// FMeshletGroupResult
// ============================================================================

/// 分组结果
struct FMeshletGroupResult
{
    TArray<FMeshletGroup> Groups;

    /// 按组连续排列的 meshlet 下标
    TArray<UInt32> GroupMeshlets;

    /// 每个 meshlet 属于哪个组 —— 与 Groups/GroupMeshlets 是同一件事的两种查法
    TArray<UInt32> MeshletToGroup;

    /// 每个**输入顶点**是不是组间边界顶点
    ///
    /// 判定按**位置**做: 焊接之后同一个位置只要被两个以上的组用到, 那个位置
    /// 上的全部输入顶点都标为边界。按下标判会漏 —— UV 接缝处同一个位置有
    /// 两个下标, 它们可能分到不同的组, 而那条缝在几何上是同一条。
    TArray<UInt8> VertexOnGroupBoundary;

    /// 跨组的共享边数 —— 分组质量的可观测量
    ///
    /// 判据要拿它与"按下标顺序分组"比: 一个不看邻接关系的实现在"划分性质"
    /// 那几条上全是满分, 只有这个数会暴露它。
    UInt32 CrossGroupEdges = 0;

    /// 组内共享边数 —— 与上面那个一起看才有意义
    UInt32 InternalEdges = 0;

    LIMX_NODISCARD bool IsValid() const { return !Groups.IsEmpty(); }
};

// ============================================================================
// FMeshletGroupMesh
// ============================================================================

/// 一个组抽出来的独立网格 —— 直接喂给简化器
struct FMeshletGroupMesh
{
    TArray<FMeshVertex> Vertices;
    TArray<UInt32>      Indices;

    /// 要锁死的顶点 (按本网格的顶点下标)
    TArray<UInt32> LockedVertices;

    /// 本网格顶点 -> 原网格顶点下标
    TArray<UInt32> SourceVertices;
};

// ============================================================================
// FMeshletGrouper
// ============================================================================

/// meshlet 分组器
class FMeshletGrouper
{
public:
    /// 把 meshlet 分组
    LIMX_NODISCARD static FMeshletGroupResult Build(
        const FMeshletBuildResult& meshlets,
        const TArray<FMeshVertex>& vertices,
        const FMeshletGroupOptions& options);

    /// 取出一个组的独立网格
    ///
    /// 顶点按组内实际用到的重新编号, 索引跟着改写; LockedVertices 里装的是
    /// 组间边界顶点在**新编号**下的下标 —— 可以直接放进
    /// FMeshSimplifyOptions::LockedVertices。
    LIMX_NODISCARD static FMeshletGroupMesh ExtractGroupMesh(
        const FMeshletBuildResult& meshlets,
        const TArray<FMeshVertex>& vertices,
        const FMeshletGroupResult& groups, UInt32 groupIndex);
};

} // namespace Limx
