// ============================================================
// 文件名称：FMeshletGrouper.cpp
// 创建时间：2026-09-02
// 创建者  ：LimxTeam
// 功能描述：FMeshletGrouper 的实现 — 按共享边分组, 标出组间边界。
// ============================================================

#include "RenderCore/RenderCoreMinimal.h"
#include "RenderCore/Geometry/FMeshletGrouper.h"

#include "Core/Containers/TArray.h"
#include "Core/Math/FMath.h"

namespace Limx
{

LIMX_DEFINE_LOG_CATEGORY(LogMeshletGrouper)

namespace
{

constexpr UInt32 kInvalid = 0xFFFFFFFFu;

/// 三个浮点的哈希
UInt32 HashPosition(const FVector3& position)
{
    const auto* bits = reinterpret_cast<const UInt32*>(&position);

    UInt32 hash = 2166136261u;

    for (UInt32 i = 0; i < 3; ++i)
    {
        hash ^= bits[i];
        hash *= 16777619u;
    }

    return hash;
}

/// 按位置焊接 —— 与简化器里那份是同一套判定
///
/// 邻接关系必须按**位置**算, 不能按顶点下标: UV 接缝处同一个位置有两个
/// 下标, 按下标算的话接缝两侧的 meshlet 是"不相邻的", 于是分组会沿着接缝
/// 把它们劈开 —— 而那条缝在几何上是连着的。
void WeldByPosition(const TArray<FMeshVertex>& vertices,
                    TArray<UInt32>& outRemap, UInt32& outCount)
{
    constexpr UInt32 kSlotCount = 2048;

    outRemap.Clear();
    outRemap.Reserve(vertices.GetSize());

    TArray<TArray<UInt32>> buckets;
    buckets.SetSize(static_cast<SizeType>(kSlotCount));

    TArray<FVector3> unique;

    for (SizeType i = 0; i < vertices.GetSize(); ++i)
    {
        const FVector3& position = vertices[i].Position;

        const UInt32 slot = HashPosition(position) & (kSlotCount - 1u);

        UInt32 found = kInvalid;

        for (SizeType k = 0; k < buckets[slot].GetSize(); ++k)
        {
            const UInt32 candidate = buckets[slot][k];

            if (unique[candidate].X == position.X &&
                unique[candidate].Y == position.Y &&
                unique[candidate].Z == position.Z)
            {
                found = candidate;
                break;
            }
        }

        if (found == kInvalid)
        {
            found = static_cast<UInt32>(unique.GetSize());
            unique.Add(position);
            buckets[slot].Add(found);
        }

        outRemap.Add(found);
    }

    outCount = static_cast<UInt32>(unique.GetSize());
}

/// meshlet 的第 triangle 个三角形的三个**全局**顶点下标
void MeshletTriangle(const FMeshletBuildResult& meshlets, UInt32 meshletIndex,
                     UInt32 triangle, UInt32* outIndices)
{
    const FMeshlet& meshlet = meshlets.Meshlets[meshletIndex];

    for (UInt32 c = 0; c < 3; ++c)
    {
        const SizeType byteOffset =
            static_cast<SizeType>(meshlet.TriangleOffset) * 3 + triangle * 3 + c;

        const UInt32 local = meshlets.MeshletTriangles[byteOffset];

        outIndices[c] = meshlets.MeshletVertices[
            static_cast<SizeType>(meshlet.VertexOffset) + local];
    }
}

/// 无向边的键 —— 两个焊接顶点下标, 小的在前
struct FEdgeKey
{
    UInt32 Low  = 0;
    UInt32 High = 0;
};

/// 邻接表: 每个 meshlet 的邻居 + 与该邻居共享的边数
struct FNeighbour
{
    UInt32 Meshlet    = 0;
    UInt32 SharedEdges = 0;
};

} // namespace

// ============================================================================
// FMeshletGrouper::Build
// ============================================================================

FMeshletGroupResult FMeshletGrouper::Build(
    const FMeshletBuildResult& meshlets, const TArray<FMeshVertex>& vertices,
    const FMeshletGroupOptions& options)
{
    FMeshletGroupResult result;

    if (!meshlets.IsValid() || vertices.IsEmpty())
    {
        LIMX_LOG(LogMeshletGrouper, Error, "[分组] 输入不合法");
        return result;
    }

    const UInt32 meshletCount = static_cast<UInt32>(meshlets.Meshlets.GetSize());

    // ---- 焊接 ----
    TArray<UInt32> weld;
    UInt32         weldedCount = 0;

    WeldByPosition(vertices, weld, weldedCount);

    // ---- 每条边被哪些 meshlet 用到 ----
    //
    // 用"每个焊接顶点挂一张小表"而不是全局哈希: 顶点的邻接度是个位数, 线性
    // 查找比哈希快而且不必处理冲突。
    TArray<TArray<UInt32>> edgeOther;   // 边的另一端
    TArray<TArray<UInt32>> edgeOwnerA;  // 用到这条边的第一个 meshlet
    TArray<TArray<UInt32>> edgeOwnerB;  // 第二个 (再多就是非流形, 只记两个)

    edgeOther.SetSize(weldedCount);
    edgeOwnerA.SetSize(weldedCount);
    edgeOwnerB.SetSize(weldedCount);

    for (UInt32 m = 0; m < meshletCount; ++m)
    {
        const FMeshlet& meshlet = meshlets.Meshlets[m];

        for (UInt32 t = 0; t < meshlet.TriangleCount; ++t)
        {
            UInt32 triangle[3] = {};
            MeshletTriangle(meshlets, m, t, triangle);

            for (UInt32 e = 0; e < 3; ++e)
            {
                const UInt32 a = weld[triangle[e]];
                const UInt32 b = weld[triangle[(e + 1) % 3]];

                if (a == b)
                {
                    continue;
                }

                const UInt32 low  = FMath::Min(a, b);
                const UInt32 high = FMath::Max(a, b);

                SizeType slot = edgeOther[low].GetSize();

                for (SizeType k = 0; k < edgeOther[low].GetSize(); ++k)
                {
                    if (edgeOther[low][k] == high)
                    {
                        slot = k;
                        break;
                    }
                }

                if (slot == edgeOther[low].GetSize())
                {
                    edgeOther[low].Add(high);
                    edgeOwnerA[low].Add(m);
                    edgeOwnerB[low].Add(kInvalid);
                    continue;
                }

                if (edgeOwnerA[low][slot] != m &&
                    edgeOwnerB[low][slot] == kInvalid)
                {
                    edgeOwnerB[low][slot] = m;
                }
            }
        }
    }

    // ---- meshlet 邻接图 ----
    TArray<TArray<FNeighbour>> adjacency;
    adjacency.SetSize(meshletCount);

    for (UInt32 v = 0; v < weldedCount; ++v)
    {
        for (SizeType k = 0; k < edgeOther[v].GetSize(); ++k)
        {
            const UInt32 ownerA = edgeOwnerA[v][k];
            const UInt32 ownerB = edgeOwnerB[v][k];

            if (ownerB == kInvalid || ownerA == ownerB)
            {
                continue;
            }

            const UInt32 pair[2] = { ownerA, ownerB };

            for (UInt32 side = 0; side < 2; ++side)
            {
                const UInt32 from = pair[side];
                const UInt32 to   = pair[1 - side];

                bool found = false;

                for (SizeType n = 0; n < adjacency[from].GetSize(); ++n)
                {
                    if (adjacency[from][n].Meshlet == to)
                    {
                        ++adjacency[from][n].SharedEdges;
                        found = true;
                        break;
                    }
                }

                if (!found)
                {
                    FNeighbour neighbour;
                    neighbour.Meshlet     = to;
                    neighbour.SharedEdges = 1;

                    adjacency[from].Add(neighbour);
                }
            }
        }
    }

    // ---- 贪心生长 ----
    //
    // 从**度数最小**的未分配 meshlet 起头。挑度数最小的是有讲究的: 那些是
    // 网格的角落, 先分它们能避免最后剩下一堆孤立的碎片凑成一个不连通的组;
    // 从度数最大的起头则相反 —— 中间那块被先吃光, 边角凑不成整组。
    result.MeshletToGroup.SetSize(meshletCount, kInvalid);

    TArray<UInt32> assigned;

    UInt32 remaining  = meshletCount;
    UInt32 groupCount = 0;

    while (remaining > 0)
    {
        // 挑种子: **未分配**邻居最少的那个 —— 先把口袋填掉
        //
        // 第一版挑的是静态度数最小的。那样会碎: 前几个组各吃满上限, 剩下的
        // meshlet 被已分配的组隔成一个个孤立口袋, 每个口袋只够凑一两个。
        // 实测 94 个 meshlet 分出 21 个组, 大小从 1 到 32 —— 而 1 个 meshlet
        // 的组几乎全是边界, 简化根本推不动 (21 个组只有 3 个真简化得动)。
        //
        // 改成动态的"未分配邻居最少"就先吃口袋: 一个被围住的 meshlet 未分配
        // 邻居为 0, 它会被立刻挑走并与周围凑成一组。
        UInt32   seed       = kInvalid;
        SizeType bestDegree = 0;

        for (UInt32 m = 0; m < meshletCount; ++m)
        {
            if (result.MeshletToGroup[m] != kInvalid)
            {
                continue;
            }

            SizeType degree = 0;

            for (SizeType n = 0; n < adjacency[m].GetSize(); ++n)
            {
                if (result.MeshletToGroup[adjacency[m][n].Meshlet] == kInvalid)
                {
                    ++degree;
                }
            }

            if (seed == kInvalid || degree < bestDegree)
            {
                seed       = m;
                bestDegree = degree;
            }
        }

        if (seed == kInvalid)
        {
            break;
        }

        const UInt32 groupIndex = groupCount++;

        UInt32 groupSize = 0;

        assigned.Clear();

        result.MeshletToGroup[seed] = groupIndex;
        assigned.Add(seed);
        ++groupSize;
        --remaining;

        // 生长: 每次并入"与本组共享边最多"的那个邻居
        // 长到**目标**大小就收, MaxGroupSize 只是硬顶。
        //
        // 一直长到硬顶的话前几个组会把中间那块吃光, 剩下的凑不成整组 ——
        // 那正是上面那段碎片化的另一半原因。
        const UInt32 growLimit =
            FMath::Min(options.TargetGroupSize, options.MaxGroupSize);

        while (groupSize < growLimit && remaining > 0)
        {
            UInt32 best      = kInvalid;
            UInt32 bestShared = 0;

            for (SizeType a = 0; a < assigned.GetSize(); ++a)
            {
                const UInt32 inside = assigned[a];

                for (SizeType n = 0; n < adjacency[inside].GetSize(); ++n)
                {
                    const FNeighbour& neighbour = adjacency[inside][n];

                    if (result.MeshletToGroup[neighbour.Meshlet] != kInvalid)
                    {
                        continue;
                    }

                    // 与**整个组**共享的边数, 不是与某一个成员
                    UInt32 shared = 0;

                    for (SizeType b = 0; b < assigned.GetSize(); ++b)
                    {
                        for (SizeType k = 0;
                             k < adjacency[assigned[b]].GetSize(); ++k)
                        {
                            if (adjacency[assigned[b]][k].Meshlet ==
                                neighbour.Meshlet)
                            {
                                shared += adjacency[assigned[b]][k].SharedEdges;
                            }
                        }
                    }

                    // 同分时挑下标小的 —— 确定性要紧, 第三天要拿分组建 DAG
                    if (best == kInvalid || shared > bestShared ||
                        (shared == bestShared && neighbour.Meshlet < best))
                    {
                        best       = neighbour.Meshlet;
                        bestShared = shared;
                    }
                }
            }

            if (best == kInvalid)
            {
                // 这个组已经吃光了它所在的连通块。**停在这里**, 不去抓一个
                // 不相邻的 meshlet 凑数 —— 组内不连通的话, 简化器在这个组
                // 上看到的是两块不挨着的几何体, 组间边界的判定也会错乱。
                break;
            }

            result.MeshletToGroup[best] = groupIndex;
            assigned.Add(best);
            ++groupSize;
            --remaining;
        }
    }

    // ---- 合并: 把碎片并进邻组 ----
    //
    // 贪心生长必然留下碎片: 一个被已分配的组围住的 meshlet, 它的未分配邻居
    // 是 0, 于是被挑成种子之后一个邻居都拉不到, 自成一个单元素组。实测 94
    // 个 meshlet 分出 26 个组, 大小从 1 到 16 —— 而单元素组几乎全是边界,
    // 简化一步都推不动 (26 个组只有 5 个真简化得动)。
    //
    // 修法不是换种子规则 (换成"未分配邻居最多"会让碎片留到最后, 一样碎),
    // 而是**生长完之后再合并一遍**: 小于目标一半的组, 并进与它共享边最多的
    // 那个邻组, 只要并完不超过硬顶。
    {
        TArray<UInt32> groupSizes;
        groupSizes.SetSize(groupCount, 0u);

        for (UInt32 m = 0; m < meshletCount; ++m)
        {
            ++groupSizes[result.MeshletToGroup[m]];
        }

        const UInt32 smallThreshold = FMath::Max(1u, options.TargetGroupSize / 2u);

        bool merged = true;

        while (merged)
        {
            merged = false;

            for (UInt32 g = 0; g < groupCount; ++g)
            {
                if (groupSizes[g] == 0 || groupSizes[g] >= smallThreshold)
                {
                    continue;
                }

                // 找与它共享边最多的邻组
                UInt32 bestGroup  = kInvalid;
                UInt32 bestShared = 0;

                for (UInt32 m = 0; m < meshletCount; ++m)
                {
                    if (result.MeshletToGroup[m] != g)
                    {
                        continue;
                    }

                    for (SizeType n = 0; n < adjacency[m].GetSize(); ++n)
                    {
                        const UInt32 other =
                            result.MeshletToGroup[adjacency[m][n].Meshlet];

                        if (other == g)
                        {
                            continue;
                        }

                        if (groupSizes[other] + groupSizes[g] >
                            options.MaxGroupSize)
                        {
                            continue;
                        }

                        // 与整个邻组共享的边数
                        UInt32 shared = 0;

                        for (UInt32 k = 0; k < meshletCount; ++k)
                        {
                            if (result.MeshletToGroup[k] != g)
                            {
                                continue;
                            }

                            for (SizeType q = 0; q < adjacency[k].GetSize();
                                 ++q)
                            {
                                if (result.MeshletToGroup[
                                        adjacency[k][q].Meshlet] == other)
                                {
                                    shared += adjacency[k][q].SharedEdges;
                                }
                            }
                        }

                        // 同分挑下标小的 —— 确定性要紧
                        if (bestGroup == kInvalid || shared > bestShared ||
                            (shared == bestShared && other < bestGroup))
                        {
                            bestGroup  = other;
                            bestShared = shared;
                        }
                    }
                }

                if (bestGroup == kInvalid)
                {
                    continue;
                }

                for (UInt32 m = 0; m < meshletCount; ++m)
                {
                    if (result.MeshletToGroup[m] == g)
                    {
                        result.MeshletToGroup[m] = bestGroup;
                    }
                }

                groupSizes[bestGroup] += groupSizes[g];
                groupSizes[g] = 0;

                merged = true;
            }
        }

        // ---- 压实标号并重建组表 ----
        TArray<UInt32> compact;
        compact.SetSize(groupCount, kInvalid);

        UInt32 nextLabel = 0;

        for (UInt32 g = 0; g < groupCount; ++g)
        {
            if (groupSizes[g] != 0)
            {
                compact[g] = nextLabel++;
            }
        }

        for (UInt32 m = 0; m < meshletCount; ++m)
        {
            result.MeshletToGroup[m] = compact[result.MeshletToGroup[m]];
        }

        result.Groups.SetSize(nextLabel);

        for (UInt32 label = 0; label < nextLabel; ++label)
        {
            result.Groups[label].FirstMeshlet =
                static_cast<UInt32>(result.GroupMeshlets.GetSize());
            result.Groups[label].MeshletCount = 0;

            for (UInt32 m = 0; m < meshletCount; ++m)
            {
                if (result.MeshletToGroup[m] == label)
                {
                    result.GroupMeshlets.Add(m);
                    ++result.Groups[label].MeshletCount;
                }
            }
        }
    }

    // ---- 组间边界顶点 ----
    //
    // 一个位置只要被两个以上的组的三角形用到, 它就是组间边界。
    TArray<UInt32> firstGroupOfVertex;
    firstGroupOfVertex.SetSize(weldedCount, kInvalid);

    TArray<UInt8> weldedOnBoundary;
    weldedOnBoundary.SetSize(weldedCount, UInt8(0));

    for (UInt32 m = 0; m < meshletCount; ++m)
    {
        const UInt32 groupIndex = result.MeshletToGroup[m];

        const FMeshlet& meshlet = meshlets.Meshlets[m];

        for (UInt32 t = 0; t < meshlet.TriangleCount; ++t)
        {
            UInt32 triangle[3] = {};
            MeshletTriangle(meshlets, m, t, triangle);

            for (UInt32 c = 0; c < 3; ++c)
            {
                const UInt32 welded = weld[triangle[c]];

                if (firstGroupOfVertex[welded] == kInvalid)
                {
                    firstGroupOfVertex[welded] = groupIndex;
                }
                else if (firstGroupOfVertex[welded] != groupIndex)
                {
                    weldedOnBoundary[welded] = 1;
                }
            }
        }
    }

    result.VertexOnGroupBoundary.SetSize(vertices.GetSize(), UInt8(0));

    for (SizeType i = 0; i < vertices.GetSize(); ++i)
    {
        result.VertexOnGroupBoundary[i] = weldedOnBoundary[weld[i]];
    }

    // ---- 分组质量 ----
    for (UInt32 v = 0; v < weldedCount; ++v)
    {
        for (SizeType k = 0; k < edgeOther[v].GetSize(); ++k)
        {
            const UInt32 ownerA = edgeOwnerA[v][k];
            const UInt32 ownerB = edgeOwnerB[v][k];

            if (ownerB == kInvalid)
            {
                continue;
            }

            if (result.MeshletToGroup[ownerA] == result.MeshletToGroup[ownerB])
            {
                ++result.InternalEdges;
            }
            else
            {
                ++result.CrossGroupEdges;
            }
        }
    }

    LIMX_LOG(LogMeshletGrouper, Log,
             "[分组] {} 个 meshlet -> {} 个组 (目标每组 {}), "
             "跨组共享边 {} / 组内 {}",
             meshletCount, result.Groups.GetSize(), options.TargetGroupSize,
             result.CrossGroupEdges, result.InternalEdges);

    return result;
}

// ============================================================================
// FMeshletGrouper::ExtractGroupMesh
// ============================================================================

FMeshletGroupMesh FMeshletGrouper::ExtractGroupMesh(
    const FMeshletBuildResult& meshlets, const TArray<FMeshVertex>& vertices,
    const FMeshletGroupResult& groups, UInt32 groupIndex)
{
    FMeshletGroupMesh mesh;

    if (groupIndex >= groups.Groups.GetSize())
    {
        return mesh;
    }

    const FMeshletGroup& group = groups.Groups[groupIndex];

    // 原顶点下标 -> 本网格下标
    TArray<UInt32> localOf;
    localOf.SetSize(vertices.GetSize(), kInvalid);

    for (UInt32 i = 0; i < group.MeshletCount; ++i)
    {
        const UInt32 meshletIndex =
            groups.GroupMeshlets[group.FirstMeshlet + i];

        const FMeshlet& meshlet = meshlets.Meshlets[meshletIndex];

        for (UInt32 t = 0; t < meshlet.TriangleCount; ++t)
        {
            UInt32 triangle[3] = {};
            MeshletTriangle(meshlets, meshletIndex, t, triangle);

            for (UInt32 c = 0; c < 3; ++c)
            {
                const UInt32 source = triangle[c];

                if (localOf[source] == kInvalid)
                {
                    localOf[source] = static_cast<UInt32>(
                        mesh.Vertices.GetSize());

                    mesh.Vertices.Add(vertices[source]);
                    mesh.SourceVertices.Add(source);

                    if (groups.VertexOnGroupBoundary[source] != 0)
                    {
                        mesh.LockedVertices.Add(localOf[source]);
                    }
                }

                mesh.Indices.Add(localOf[source]);
            }
        }
    }

    return mesh;
}

} // namespace Limx
