// ============================================================
// 文件名称：FMeshLodDag.cpp
// 创建时间：2026-09-02
// 创建者  ：LimxTeam
// 功能描述：FMeshLodDagBuilder 的实现 — 分组、简化、按组重切、逐层向上。
// ============================================================

#include "RenderCore/RenderCoreMinimal.h"
#include "RenderCore/Geometry/FMeshLodDag.h"

#include "Core/Containers/TArray.h"
#include "Core/Math/FMath.h"
#include "RenderCore/Geometry/FMeshSimplifier.h"
#include "RenderCore/Geometry/FMeshletGrouper.h"

namespace Limx
{

LIMX_DEFINE_LOG_CATEGORY(LogMeshLodDag)

namespace
{

/// 含住一组点的球 —— AABB 中心 + 最远距离
///
/// 与 FMeshletBuilder 里那份同一套做法。不求最小包围球: 那要迭代, 而这里
/// 只要"包得住"且不太松, AABB 中心已经够。
FVector4 SphereOfPoints(const TArray<FVector3>& points)
{
    if (points.IsEmpty())
    {
        return FVector4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    FVector3 low  = points[0];
    FVector3 high = points[0];

    for (SizeType i = 1; i < points.GetSize(); ++i)
    {
        low  = FVector3(FMath::Min(low.X, points[i].X),
                        FMath::Min(low.Y, points[i].Y),
                        FMath::Min(low.Z, points[i].Z));
        high = FVector3(FMath::Max(high.X, points[i].X),
                        FMath::Max(high.Y, points[i].Y),
                        FMath::Max(high.Z, points[i].Z));
    }

    const FVector3 center = (low + high) * 0.5f;

    Float32 radius = 0.0f;

    for (SizeType i = 0; i < points.GetSize(); ++i)
    {
        radius = FMath::Max(radius, (points[i] - center).Length());
    }

    return FVector4(center.X, center.Y, center.Z, radius);
}

/// 让 outer 含住 inner —— 中心不动, 半径撑到够
///
/// 含球性是"误差沿边严格增"那个证明的一半 (另一半是误差本身严格增)。
/// 它保证对**任意**相机位置都有 d' <= d, 于是 E = 误差/d 只会更大。
void GrowToContain(FVector4& outer, const FVector4& inner)
{
    const FVector3 outerCenter(outer.X, outer.Y, outer.Z);
    const FVector3 innerCenter(inner.X, inner.Y, inner.Z);

    const Float32 needed = (innerCenter - outerCenter).Length() + inner.W;

    outer.W = FMath::Max(outer.W, needed);
}

} // namespace

// ============================================================================
// FMeshLodDagBuilder::Build
// ============================================================================

FMeshLodDagResult FMeshLodDagBuilder::Build(
    const TArray<FMeshVertex>& vertices, const TArray<UInt32>& indices,
    const FMeshLodDagOptions& options)
{
    FMeshLodDagResult result;

    if (vertices.IsEmpty() || indices.GetSize() < 3 ||
        indices.GetSize() % 3 != 0)
    {
        LIMX_LOG(LogMeshLodDag, Error, "[DAG] 输入不合法 — 顶点 {} 索引 {}",
                 vertices.GetSize(), indices.GetSize());
        return result;
    }

    // ---- 网格尺度 —— 增长地板的绝对量要按它取 ----
    Float32 meshScale = 0.0f;

    {
        FVector3 low  = vertices[0].Position;
        FVector3 high = vertices[0].Position;

        for (SizeType i = 1; i < vertices.GetSize(); ++i)
        {
            const FVector3& p = vertices[i].Position;

            low  = FVector3(FMath::Min(low.X, p.X), FMath::Min(low.Y, p.Y),
                            FMath::Min(low.Z, p.Z));
            high = FVector3(FMath::Max(high.X, p.X), FMath::Max(high.Y, p.Y),
                            FMath::Max(high.Z, p.Z));
        }

        meshScale = (high - low).Length();
    }

    const Float32 absoluteFloor = meshScale * 1.0e-7f;

    // ---- 第 0 层: 原样切分, 一个三角形都不动 ----
    {
        FLodLevel level;
        level.Vertices = vertices;
        level.Indices  = indices;
        level.Meshlets = FMeshletBuilder::Build(vertices, indices);

        if (!level.Meshlets.IsValid())
        {
            LIMX_LOG(LogMeshLodDag, Error, "[DAG] 第 0 层切分失败");
            return result;
        }

        const SizeType count = level.Meshlets.Meshlets.GetSize();

        level.Records.SetSize(count);

        for (SizeType m = 0; m < count; ++m)
        {
            FLodMeshletRecord& record = level.Records[m];

            record.Level       = 0;
            record.SelfError   = 0.0f;
            record.SelfSphere  = level.Meshlets.Meshlets[m].BoundingSphere;
            record.ParentError = kLodInfiniteError;
            record.ParentSphere = record.SelfSphere;
            record.SourceGroup  = kLodInvalidIndex;
            record.TargetGroup  = kLodInvalidIndex;
        }

        result.Levels.Add(static_cast<FLodLevel&&>(level));
    }

    result.StopReason = FMeshLodDagResult::EStopReason::LevelLimit;

    // ---- 逐层向上 ----
    for (UInt32 levelIndex = 0; levelIndex + 1 < options.MaxLevels;
         ++levelIndex)
    {
        FLodLevel& current = result.Levels[levelIndex];

        const SizeType currentMeshlets = current.Meshlets.Meshlets.GetSize();

        if (currentMeshlets <= 1)
        {
            result.StopReason = FMeshLodDagResult::EStopReason::ReachedRoot;
            break;
        }

        // ---- 分组 ----
        FMeshletGroupOptions groupOptions;
        groupOptions.TargetGroupSize = options.TargetGroupSize;
        groupOptions.MaxGroupSize    = options.MaxGroupSize;

        const FMeshletGroupResult groups = FMeshletGrouper::Build(
            current.Meshlets, current.Vertices, groupOptions);

        if (!groups.IsValid())
        {
            LIMX_LOG(LogMeshLodDag, Error, "[DAG] 第 {} 层分组失败",
                     levelIndex);
            break;
        }

        // 只剩一个组就是到根了 —— 再简化一次就是单个 meshlet
        FLodLevel next;

        const UInt32 firstGroupIndex =
            static_cast<UInt32>(result.Groups.GetSize());

        SizeType producedTriangles = 0;

        for (UInt32 g = 0; g < groups.Groups.GetSize(); ++g)
        {
            const FMeshletGroupMesh groupMesh =
                FMeshletGrouper::ExtractGroupMesh(current.Meshlets,
                                                  current.Vertices, groups, g);

            if (groupMesh.Indices.GetSize() < 3)
            {
                continue;
            }

            FLodGroup group;
            group.Level = levelIndex;

            // 组的成员
            const FMeshletGroup& groupRange = groups.Groups[g];

            for (UInt32 i = 0; i < groupRange.MeshletCount; ++i)
            {
                group.ChildMeshlets.Add(
                    groups.GroupMeshlets[groupRange.FirstMeshlet + i]);
            }

            // ---- 组内简化到一半, 组间边界锁死 ----
            FMeshSimplifyOptions simplifyOptions;
            simplifyOptions.TargetTriangleCount =
                static_cast<UInt32>(groupMesh.Indices.GetSize() / 3 / 2);
            simplifyOptions.LockOpenBoundary = true;
            simplifyOptions.LockedVertices   = groupMesh.LockedVertices;

            const FMeshSimplifyResult simplified = FMeshSimplifier::Simplify(
                groupMesh.Vertices, groupMesh.Indices, simplifyOptions);

            // 简化不动的组**不造父层**。
            //
            // 硬造一个"父 = 子的副本"会让父误差等于子误差 (本次误差为 0),
            // 而选择规则"自身 < 阈值且父 >= 阈值"在相等时永不成立 —— 那块
            // 表面在每一个阈值下都不画。
            //
            // 正确的做法是让这个组的成员直接成为根: 它们已经简化到头了。
            if (simplified.Indices.GetSize() < 3 ||
                simplified.Indices.GetSize() >= groupMesh.Indices.GetSize() ||
                simplified.CollapseCount == 0)
            {
                continue;
            }

            // ---- 误差: 累加, 而且严格增 ----
            Float32 childError = 0.0f;

            for (SizeType i = 0; i < group.ChildMeshlets.GetSize(); ++i)
            {
                childError = FMath::Max(
                    childError,
                    current.Records[group.ChildMeshlets[i]].SelfError);
            }

            group.OwnError = simplified.Error;

            group.Error = FMath::Max(childError + simplified.Error,
                                     childError * (1.0f + 1.0f / 1024.0f) +
                                         absoluteFloor);

            // ---- 组的 LOD 球: 含全部成员的自身球, 再撑到不小于误差 ----
            {
                TArray<FVector3> points;

                for (SizeType i = 0; i < groupMesh.Vertices.GetSize(); ++i)
                {
                    points.Add(groupMesh.Vertices[i].Position);
                }

                for (SizeType i = 0; i < simplified.Vertices.GetSize(); ++i)
                {
                    points.Add(simplified.Vertices[i].Position);
                }

                group.Sphere = SphereOfPoints(points);

                for (SizeType i = 0; i < group.ChildMeshlets.GetSize(); ++i)
                {
                    GrowToContain(
                        group.Sphere,
                        current.Records[group.ChildMeshlets[i]].SelfSphere);
                }

                // 半径不小于误差 —— 运行期投影公式保守性的前提
                group.Sphere.W = FMath::Max(group.Sphere.W, group.Error);
            }

            const UInt32 groupIndex =
                static_cast<UInt32>(result.Groups.GetSize());

            // ---- 简化后的几何按**这个组**单独切 meshlet ----
            //
            // 不与别的组合起来重切: 那样一个 meshlet 可能跨两个组, 而
            // "一个 meshlet 只由一个组产出"是选择规则整个证明的前提。
            const FMeshletBuildResult groupMeshlets = FMeshletBuilder::Build(
                simplified.Vertices, simplified.Indices);

            if (!groupMeshlets.IsValid())
            {
                continue;
            }

            // 拼进下一层: 顶点追加, 索引与 meshlet 的偏移跟着挪
            const UInt32 vertexBase =
                static_cast<UInt32>(next.Vertices.GetSize());

            const UInt32 meshletVertexBase =
                static_cast<UInt32>(next.Meshlets.MeshletVertices.GetSize());

            const UInt32 triangleByteBase =
                static_cast<UInt32>(next.Meshlets.MeshletTriangles.GetSize());

            for (SizeType i = 0; i < simplified.Vertices.GetSize(); ++i)
            {
                next.Vertices.Add(simplified.Vertices[i]);
            }

            for (SizeType i = 0; i < simplified.Indices.GetSize(); ++i)
            {
                next.Indices.Add(simplified.Indices[i] + vertexBase);
            }

            for (SizeType i = 0; i < groupMeshlets.MeshletVertices.GetSize();
                 ++i)
            {
                next.Meshlets.MeshletVertices.Add(
                    groupMeshlets.MeshletVertices[i] + vertexBase);
            }

            for (SizeType i = 0; i < groupMeshlets.MeshletTriangles.GetSize();
                 ++i)
            {
                next.Meshlets.MeshletTriangles.Add(
                    groupMeshlets.MeshletTriangles[i]);
            }

            for (SizeType i = 0; i < groupMeshlets.Meshlets.GetSize(); ++i)
            {
                FMeshlet meshlet = groupMeshlets.Meshlets[i];

                meshlet.VertexOffset += meshletVertexBase;
                meshlet.TriangleOffset += triangleByteBase / 3;

                const UInt32 newIndex =
                    static_cast<UInt32>(next.Meshlets.Meshlets.GetSize());

                next.Meshlets.Meshlets.Add(meshlet);

                FLodMeshletRecord record;
                record.Level        = levelIndex + 1;
                record.SelfError    = group.Error;
                record.SelfSphere   = group.Sphere;
                record.ParentError  = kLodInfiniteError;
                record.ParentSphere = group.Sphere;
                record.SourceGroup  = groupIndex;
                record.TargetGroup  = kLodInvalidIndex;

                next.Records.Add(record);

                group.ParentMeshlets.Add(newIndex);

                producedTriangles += meshlet.TriangleCount;
            }

            // ---- 回填这一层成员的父 ----
            for (SizeType i = 0; i < group.ChildMeshlets.GetSize(); ++i)
            {
                FLodMeshletRecord& child =
                    current.Records[group.ChildMeshlets[i]];

                child.ParentError  = group.Error;
                child.ParentSphere = group.Sphere;
                child.TargetGroup  = groupIndex;
            }

            result.Groups.Add(static_cast<FLodGroup&&>(group));
        }

        LIMX_UNUSED(firstGroupIndex);

        if (next.Meshlets.Meshlets.IsEmpty())
        {
            result.StopReason = FMeshLodDagResult::EStopReason::Stagnated;
            break;
        }

        // ---- 停滞: 这一层没比上一层少多少 ----
        SizeType currentTriangles = 0;

        for (SizeType m = 0; m < currentMeshlets; ++m)
        {
            currentTriangles += current.Meshlets.Meshlets[m].TriangleCount;
        }

        if (static_cast<Float32>(producedTriangles) >
            static_cast<Float32>(currentTriangles) * options.StagnationRatio)
        {
            // 停在这里, 而且**不收下**这一层。
            //
            // 收下的话会多出一层误差与上一层几乎相同的 meshlet, 而 LOD 选择
            // 会在两层之间反复横跳 —— 相机微动时几何抖动。
            //
            // 不收下就要把刚才回填的父关系撤掉: 这一层的成员重新变成根。
            for (SizeType g = result.Groups.GetSize(); g > 0; --g)
            {
                const FLodGroup& group = result.Groups[g - 1];

                if (group.Level != levelIndex)
                {
                    break;
                }

                for (SizeType i = 0; i < group.ChildMeshlets.GetSize(); ++i)
                {
                    FLodMeshletRecord& child =
                        current.Records[group.ChildMeshlets[i]];

                    child.ParentError  = kLodInfiniteError;
                    child.ParentSphere = child.SelfSphere;
                    child.TargetGroup  = kLodInvalidIndex;
                }
            }

            while (!result.Groups.IsEmpty() &&
                   result.Groups[result.Groups.GetSize() - 1].Level ==
                       levelIndex)
            {
                result.Groups.RemoveAt(result.Groups.GetSize() - 1);
            }

            result.StopReason = FMeshLodDagResult::EStopReason::Stagnated;
            break;
        }

        result.Levels.Add(static_cast<FLodLevel&&>(next));
    }

    // ---- 汇报 ----
    const AnsiChar* reason = "层数上限";

    switch (result.StopReason)
    {
    case FMeshLodDagResult::EStopReason::ReachedRoot:
        reason = "到根";
        break;
    case FMeshLodDagResult::EStopReason::Stagnated:
        reason = "简化不动了";
        break;
    default:
        break;
    }

    for (SizeType l = 0; l < result.Levels.GetSize(); ++l)
    {
        SizeType triangles = 0;

        for (SizeType m = 0; m < result.Levels[l].Meshlets.Meshlets.GetSize();
             ++m)
        {
            triangles += result.Levels[l].Meshlets.Meshlets[m].TriangleCount;
        }

        LIMX_LOG(LogMeshLodDag, Log,
                 "[DAG] 第 {} 层 — {} 个 meshlet, {} 个三角形", l,
                 result.Levels[l].Meshlets.Meshlets.GetSize(), triangles);
    }

    LIMX_LOG(LogMeshLodDag, Log, "[DAG] {} 层, {} 个组, 停止原因: {}",
             result.Levels.GetSize(), result.Groups.GetSize(), reason);

    return result;
}

} // namespace Limx
