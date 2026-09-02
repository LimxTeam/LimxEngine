// ============================================================
// 文件名称：FMeshletBuilder.cpp
// 创建时间：2026-09-02
// 创建者  ：LimxTeam
// 功能描述：贪心邻接聚类的 meshlet 切分实现。
// ============================================================

#include "RenderCore/RenderCoreMinimal.h"

#include "RenderCore/Geometry/FMeshletBuilder.h"

#include "Core/Containers/TArray.h"
#include "Core/Math/FMath.h"

namespace Limx
{

LIMX_DEFINE_LOG_CATEGORY(LogMeshletBuilder)

namespace
{

// ============================================================================
// 顶点 -> 三角形的邻接表
//
// 用两个平坦数组而不是"每个顶点一个 TArray": 后者在几十万顶点的网格上是
// 几十万次小分配, 而这里的访问模式是"建一次, 只读地扫很多遍"。
//
// Offsets 长度 = 顶点数 + 1, Offsets[v]..Offsets[v+1] 是顶点 v 的三角形。
// ============================================================================

struct FVertexTriangleAdjacency
{
    TArray<UInt32> Offsets;
    TArray<UInt32> Triangles;
};

FVertexTriangleAdjacency BuildAdjacency(SizeType vertexCount,
                                        const TArray<UInt32>& indices)
{
    FVertexTriangleAdjacency adjacency;

    const SizeType triangleCount = indices.GetSize() / 3;

    adjacency.Offsets.SetSize(vertexCount + 1, 0u);

    for (SizeType i = 0; i < indices.GetSize(); ++i)
    {
        ++adjacency.Offsets[indices[i] + 1];
    }

    for (SizeType v = 0; v < vertexCount; ++v)
    {
        adjacency.Offsets[v + 1] += adjacency.Offsets[v];
    }

    adjacency.Triangles.SetSize(triangleCount * 3, 0u);

    TArray<UInt32> cursor;
    cursor.SetSize(vertexCount, 0u);

    for (SizeType t = 0; t < triangleCount; ++t)
    {
        for (SizeType k = 0; k < 3; ++k)
        {
            const UInt32 v = indices[t * 3 + k];

            adjacency.Triangles[adjacency.Offsets[v] + cursor[v]] =
                static_cast<UInt32>(t);

            ++cursor[v];
        }
    }

    return adjacency;
}

// ============================================================================
// 正在长的那个 meshlet
// ============================================================================

struct FGrowingMeshlet
{
    /// 局部顶点表 —— 全局下标
    UInt32 Vertices[kMaxMeshletVertices] = {};
    UInt32 VertexCount = 0;

    /// 三角形的局部索引
    UInt8 Triangles[kMaxMeshletTriangles * 3] = {};
    UInt32 TriangleCount = 0;

    /// 已收顶点位置之和 —— 用来算质心, 挑下一个三角形时要用
    FVector3 PositionSum = FVector3(0.0f, 0.0f, 0.0f);

    /// 已收顶点的包围盒 —— 退化搜索的距离上限要用它
    FVector3 Minimum = FVector3(1.0e30f, 1.0e30f, 1.0e30f);
    FVector3 Maximum = FVector3(-1.0e30f, -1.0e30f, -1.0e30f);

    void Reset()
    {
        VertexCount = 0;
        TriangleCount = 0;
        PositionSum = FVector3(0.0f, 0.0f, 0.0f);
        Minimum = FVector3(1.0e30f, 1.0e30f, 1.0e30f);
        Maximum = FVector3(-1.0e30f, -1.0e30f, -1.0e30f);

        for (UInt32 i = 0; i < kSlotCount; ++i)
        {
            SlotKeys[i] = 0u;
        }
    }

    /// 当前包围盒对角线的一半 —— 拿它当"这个 meshlet 有多大"
    LIMX_NODISCARD Float32 Extent() const
    {
        if (VertexCount == 0)
        {
            return 0.0f;
        }

        const FVector3 diagonal = Maximum - Minimum;

        return 0.5f * FMath::Sqrt(diagonal.X * diagonal.X +
                                  diagonal.Y * diagonal.Y +
                                  diagonal.Z * diagonal.Z);
    }

    /// 全局顶点 -> 局部下标的开放寻址表
    ///
    /// 这里曾经是对 Vertices 的线性扫描。那看起来无害 (最多 64 个),
    /// 但它在最内层: 每考察一个候选三角形要查三次, 而一个 meshlet 长起来
    /// 要考察几百个候选。Sponza 的导入因此慢了 74 ms, 被导入基准的哨兵
    /// 当场逮到 —— 那条哨兵存在的理由正是"一次性成本不出现在任何逐帧
    /// 数字里"。
    ///
    /// 128 个槽位装最多 64 个顶点, 装载率不超过一半, 线性探测的期望
    /// 步数在 1.5 以内。表在 Reset 里清空 —— 128 次写比一次分配便宜得多。
    static constexpr UInt32 kSlotCount = 128;
    static constexpr UInt32 kSlotMask = kSlotCount - 1;

    /// 槽位里存的是 全局下标 + 1 (0 表示空槽)
    UInt32 SlotKeys[kSlotCount] = {};
    UInt8  SlotValues[kSlotCount] = {};

    LIMX_NODISCARD static UInt32 HashIndex(UInt32 globalIndex)
    {
        // 乘一个奇数再取高位 —— 顶点下标常常是连续的, 直接取低位会让
        // 一整段连续下标挤进相邻槽位, 线性探测退化成线性扫描。
        return ((globalIndex * 2654435761u) >> 24) & kSlotMask;
    }

    /// 这个全局顶点在局部表里的位置; 不在则返回 kMaxMeshletVertices
    LIMX_NODISCARD UInt32 FindLocal(UInt32 globalIndex) const
    {
        const UInt32 key = globalIndex + 1u;

        UInt32 slot = HashIndex(globalIndex);

        for (UInt32 probe = 0; probe < kSlotCount; ++probe)
        {
            if (SlotKeys[slot] == 0u)
            {
                return kMaxMeshletVertices;
            }

            if (SlotKeys[slot] == key)
            {
                return SlotValues[slot];
            }

            slot = (slot + 1u) & kSlotMask;
        }

        return kMaxMeshletVertices;
    }

    void InsertLocal(UInt32 globalIndex, UInt32 localIndex)
    {
        const UInt32 key = globalIndex + 1u;

        UInt32 slot = HashIndex(globalIndex);

        for (UInt32 probe = 0; probe < kSlotCount; ++probe)
        {
            if (SlotKeys[slot] == 0u || SlotKeys[slot] == key)
            {
                SlotKeys[slot]   = key;
                SlotValues[slot] = static_cast<UInt8>(localIndex);
                return;
            }

            slot = (slot + 1u) & kSlotMask;
        }
    }

    /// 加进这个三角形要新增几个顶点 (0..3)
    LIMX_NODISCARD UInt32 NewVertexCount(const UInt32* triangle) const
    {
        UInt32 added = 0;

        for (UInt32 k = 0; k < 3; ++k)
        {
            if (FindLocal(triangle[k]) != kMaxMeshletVertices)
            {
                continue;
            }

            // 三个角里有重复的全局顶点时不能各算一次 —— 退化三角形
            // (两个角同一个顶点) 在真实资产里是存在的。
            bool duplicate = false;

            for (UInt32 j = 0; j < k; ++j)
            {
                if (triangle[j] == triangle[k])
                {
                    duplicate = true;
                }
            }

            if (!duplicate)
            {
                ++added;
            }
        }

        return added;
    }

    LIMX_NODISCARD bool CanFit(const UInt32* triangle) const
    {
        return TriangleCount < kMaxMeshletTriangles &&
               VertexCount + NewVertexCount(triangle) <= kMaxMeshletVertices;
    }

    void AddTriangle(const UInt32* triangle,
                     const TArray<FMeshVertex>& vertices)
    {
        for (UInt32 k = 0; k < 3; ++k)
        {
            UInt32 local = FindLocal(triangle[k]);

            if (local == kMaxMeshletVertices)
            {
                local = VertexCount;

                Vertices[VertexCount] = triangle[k];
                InsertLocal(triangle[k], VertexCount);
                ++VertexCount;

                const FVector3& position = vertices[triangle[k]].Position;

                PositionSum = PositionSum + position;

                Minimum.X = FMath::Min(Minimum.X, position.X);
                Minimum.Y = FMath::Min(Minimum.Y, position.Y);
                Minimum.Z = FMath::Min(Minimum.Z, position.Z);

                Maximum.X = FMath::Max(Maximum.X, position.X);
                Maximum.Y = FMath::Max(Maximum.Y, position.Y);
                Maximum.Z = FMath::Max(Maximum.Z, position.Z);
            }

            Triangles[TriangleCount * 3 + k] = static_cast<UInt8>(local);
        }

        ++TriangleCount;
    }

    LIMX_NODISCARD FVector3 Centroid() const
    {
        if (VertexCount == 0)
        {
            return FVector3(0.0f, 0.0f, 0.0f);
        }

        const Float32 inverse = 1.0f / static_cast<Float32>(VertexCount);

        return FVector3(PositionSum.X * inverse, PositionSum.Y * inverse,
                        PositionSum.Z * inverse);
    }
};

Float32 DistanceSquared(const FVector3& a, const FVector3& b)
{
    const Float32 dx = a.X - b.X;
    const Float32 dy = a.Y - b.Y;
    const Float32 dz = a.Z - b.Z;

    return dx * dx + dy * dy + dz * dz;
}

/// 三角形的几何法线 —— 未归一化时返回零向量
FVector3 TriangleNormal(const FVector3& a, const FVector3& b,
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

    // 退化三角形 (三点共线或重合) 的法线没有定义。返回零向量, 由调用方
    // 把它排除在法线锥之外 —— 硬凑一个方向进去会把锥张开到毫无意义。
    if (length < 1.0e-20f)
    {
        return FVector3(0.0f, 0.0f, 0.0f);
    }

    const Float32 inverse = 1.0f / length;

    return FVector3(cross.X * inverse, cross.Y * inverse, cross.Z * inverse);
}

// ============================================================================
// 把长好的 meshlet 落到结果里, 并算包围球与法线锥
// ============================================================================

void FinalizeMeshlet(const FGrowingMeshlet& growing,
                     const TArray<FMeshVertex>& vertices,
                     FMeshletBuildResult& result)
{
    FMeshlet meshlet;

    meshlet.VertexOffset =
        static_cast<UInt32>(result.MeshletVertices.GetSize());

    meshlet.VertexCount = growing.VertexCount;

    meshlet.TriangleOffset =
        static_cast<UInt32>(result.MeshletTriangles.GetSize() / 3);

    meshlet.TriangleCount = growing.TriangleCount;

    // ---- 包围球: AABB 中心 + 最远顶点距离 ----
    //
    // 不是最小包围球。最小包围球 (Welzl) 会紧一点, 但这里要的是
    // **一定包住** —— 而"中心取哪都行, 半径取到最远点"这一条是恒真的,
    // 不依赖任何几何假设。AABB 中心让它不至于太松。
    FVector3 minimum(1.0e30f, 1.0e30f, 1.0e30f);
    FVector3 maximum(-1.0e30f, -1.0e30f, -1.0e30f);

    for (UInt32 i = 0; i < growing.VertexCount; ++i)
    {
        const FVector3& p = vertices[growing.Vertices[i]].Position;

        minimum.X = FMath::Min(minimum.X, p.X);
        minimum.Y = FMath::Min(minimum.Y, p.Y);
        minimum.Z = FMath::Min(minimum.Z, p.Z);

        maximum.X = FMath::Max(maximum.X, p.X);
        maximum.Y = FMath::Max(maximum.Y, p.Y);
        maximum.Z = FMath::Max(maximum.Z, p.Z);
    }

    const FVector3 center((minimum.X + maximum.X) * 0.5f,
                          (minimum.Y + maximum.Y) * 0.5f,
                          (minimum.Z + maximum.Z) * 0.5f);

    Float32 radiusSquared = 0.0f;

    for (UInt32 i = 0; i < growing.VertexCount; ++i)
    {
        radiusSquared = FMath::Max(
            radiusSquared,
            DistanceSquared(center, vertices[growing.Vertices[i]].Position));
    }

    meshlet.BoundingSphere =
        FVector4(center.X, center.Y, center.Z, FMath::Sqrt(radiusSquared));

    // ---- 法线锥: 单位法线的均值为轴, 全体法线的最小投影为半角余弦 ----
    FVector3 axisSum(0.0f, 0.0f, 0.0f);

    UInt32 usableTriangles = 0;

    for (UInt32 t = 0; t < growing.TriangleCount; ++t)
    {
        const FVector3& a =
            vertices[growing.Vertices[growing.Triangles[t * 3 + 0]]].Position;
        const FVector3& b =
            vertices[growing.Vertices[growing.Triangles[t * 3 + 1]]].Position;
        const FVector3& c =
            vertices[growing.Vertices[growing.Triangles[t * 3 + 2]]].Position;

        const FVector3 normal = TriangleNormal(a, b, c);

        if (normal.X == 0.0f && normal.Y == 0.0f && normal.Z == 0.0f)
        {
            continue;
        }

        axisSum = axisSum + normal;
        ++usableTriangles;
    }

    const Float32 axisLength =
        FMath::Sqrt(axisSum.X * axisSum.X + axisSum.Y * axisSum.Y +
                    axisSum.Z * axisSum.Z);

    // 均值长度趋近零 = 法线互相抵消 = 这个 meshlet 朝向散得没有主方向。
    // 那时没有可用的轴, 直接标记无效。
    if (usableTriangles == 0 || axisLength < 1.0e-6f)
    {
        meshlet.NormalCone =
            FVector4(0.0f, 0.0f, 1.0f, kInvalidConeCosine);
    }
    else
    {
        const Float32 inverse = 1.0f / axisLength;

        const FVector3 axis(axisSum.X * inverse, axisSum.Y * inverse,
                            axisSum.Z * inverse);

        Float32 minimumDot = 1.0f;

        for (UInt32 t = 0; t < growing.TriangleCount; ++t)
        {
            const FVector3& a =
                vertices[growing.Vertices[growing.Triangles[t * 3 + 0]]]
                    .Position;
            const FVector3& b =
                vertices[growing.Vertices[growing.Triangles[t * 3 + 1]]]
                    .Position;
            const FVector3& c =
                vertices[growing.Vertices[growing.Triangles[t * 3 + 2]]]
                    .Position;

            const FVector3 normal = TriangleNormal(a, b, c);

            if (normal.X == 0.0f && normal.Y == 0.0f && normal.Z == 0.0f)
            {
                continue;
            }

            minimumDot = FMath::Min(minimumDot,
                                    axis.X * normal.X + axis.Y * normal.Y +
                                        axis.Z * normal.Z);
        }

        // 半角超过 90 度的锥对背面剔除没有价值 —— 从任何方向看它都可能
        // 有正面。标记无效, 让剔除侧显式地跳过它。
        meshlet.NormalCone =
            FVector4(axis.X, axis.Y, axis.Z,
                     (minimumDot > 0.0f) ? minimumDot : kInvalidConeCosine);
    }

    // ---- 落盘 ----
    for (UInt32 i = 0; i < growing.VertexCount; ++i)
    {
        result.MeshletVertices.Add(growing.Vertices[i]);
    }

    for (UInt32 i = 0; i < growing.TriangleCount * 3; ++i)
    {
        result.MeshletTriangles.Add(growing.Triangles[i]);
    }

    result.Meshlets.Add(meshlet);
}

} // namespace

// ============================================================================
// FMeshletBuilder::Build
//
// 贪心邻接聚类。每一轮:
//   1. 挑一个种子三角形 —— 离上一个 meshlet 质心最近的未用三角形。
//   2. 反复从"与当前 meshlet 共享顶点的未用三角形"里挑一个加进来,
//      挑的依据是**新增顶点最少**, 同分时挑重心离当前质心最近的。
//   3. 装不下 (顶点满或三角形满) 就收尾, 回到 1。
//
// 为什么是"新增顶点最少"而不是"离质心最近": meshlet 的两个上限里,
// 真实网格上先撞到的几乎总是顶点数。优先收不带新顶点的三角形, 等于让
// 同样的 64 个顶点承载尽可能多的三角形 —— 顶点复用率直接决定这件事,
// 而复用率又决定了顶点数据被放大多少倍。
//
// 第二关键字取"离质心近"是为了空间局部性: 只按顶点数挑的话, 网格上
// 一条细长的三角形带会被优先吃掉 (它每步只带一个新顶点), 于是 meshlet
// 变成一条长带 —— 包围球巨大, 剔除完全失效。
// ============================================================================

FMeshletBuildResult FMeshletBuilder::Build(
    const TArray<FMeshVertex>& vertices, const TArray<UInt32>& indices)
{
    FMeshletBuildResult result;

    if (indices.IsEmpty() || vertices.IsEmpty())
    {
        return result;
    }

    if ((indices.GetSize() % 3) != 0)
    {
        LIMX_LOG(LogMeshletBuilder, Error,
                 "[Meshlet] 索引数 {} 不是 3 的倍数 — 拒绝切分",
                 indices.GetSize());
        return result;
    }

    for (SizeType i = 0; i < indices.GetSize(); ++i)
    {
        if (indices[i] >= vertices.GetSize())
        {
            LIMX_LOG(LogMeshletBuilder, Error,
                     "[Meshlet] 索引 {} (第 {} 个) 越界, 顶点数 {} — 拒绝切分",
                     indices[i], i, vertices.GetSize());
            return result;
        }
    }

    const SizeType triangleCount = indices.GetSize() / 3;

    const FVertexTriangleAdjacency adjacency =
        BuildAdjacency(vertices.GetSize(), indices);

    TArray<UInt8> used;
    used.SetSize(triangleCount, 0u);

    // 三角形重心 —— 挑种子与第二关键字都要用, 算一次存下来
    TArray<FVector3> centroids;
    centroids.Reserve(triangleCount);

    for (SizeType t = 0; t < triangleCount; ++t)
    {
        const FVector3& a = vertices[indices[t * 3 + 0]].Position;
        const FVector3& b = vertices[indices[t * 3 + 1]].Position;
        const FVector3& c = vertices[indices[t * 3 + 2]].Position;

        centroids.Add(FVector3((a.X + b.X + c.X) / 3.0f,
                               (a.Y + b.Y + c.Y) / 3.0f,
                               (a.Z + b.Z + c.Z) / 3.0f));
    }

    FGrowingMeshlet growing;

    SizeType remaining = triangleCount;

    // 找种子时从这里往后扫 —— 扫过的前缀一定全被用掉了, 不必回头。
    // 没有它的话最坏情况是 O(三角形数²)。
    SizeType searchStart = 0;

    FVector3 lastCentroid(0.0f, 0.0f, 0.0f);

    bool hasLastCentroid = false;

    while (remaining > 0)
    {
        // ---- 挑种子 ----
        SizeType seed = triangleCount;

        while (searchStart < triangleCount && used[searchStart] != 0)
        {
            ++searchStart;
        }

        if (hasLastCentroid)
        {
            // 从上一个 meshlet 的质心附近开始, 让相邻的 meshlet 在空间上
            // 也相邻。全局最近搜索是 O(n²), 这里只在一个窗口里找 ——
            // 窗口取 1024 个三角形, 那是一次线性扫描的成本, 而效果上
            // 与全局最近几乎没差别 (索引数组本身就有空间局部性)。
            constexpr SizeType kSeedWindow = 1024;

            Float32 best = 1.0e30f;

            const SizeType end =
                FMath::Min(searchStart + kSeedWindow, triangleCount);

            for (SizeType t = searchStart; t < end; ++t)
            {
                if (used[t] != 0)
                {
                    continue;
                }

                const Float32 distance =
                    DistanceSquared(lastCentroid, centroids[t]);

                if (distance < best)
                {
                    best = distance;
                    seed = t;
                }
            }
        }

        if (seed == triangleCount)
        {
            seed = searchStart;
        }

        if (seed >= triangleCount)
        {
            break;
        }

        growing.Reset();
        growing.AddTriangle(&indices[seed * 3], vertices);

        used[seed] = 1;
        --remaining;

        // ---- 长 ----
        for (;;)
        {
            SizeType bestTriangle = triangleCount;

            UInt32 bestNewVertices = 4;

            Float32 bestDistance = 1.0e30f;

            const FVector3 centroid = growing.Centroid();

            // 三角形满了就不必再找候选 —— 原先这一条在 CanFit 里,
            // 合并之后要显式写出来。
            if (growing.TriangleCount >= kMaxMeshletTriangles)
            {
                break;
            }

            // 候选 = 与当前 meshlet 的任一局部顶点相邻的三角形
            for (UInt32 i = 0; i < growing.VertexCount; ++i)
            {
                const UInt32 v = growing.Vertices[i];

                const UInt32 begin = adjacency.Offsets[v];
                const UInt32 end = adjacency.Offsets[v + 1];

                for (UInt32 a = begin; a < end; ++a)
                {
                    const UInt32 t = adjacency.Triangles[a];

                    if (used[t] != 0)
                    {
                        continue;
                    }

                    // CanFit 与 NewVertexCount 问的是同一件事, 算一遍
                    // 就够 —— 这一段在最内层, 每个候选都要走一次。
                    const UInt32 added =
                        growing.NewVertexCount(&indices[t * 3]);

                    if (growing.VertexCount + added > kMaxMeshletVertices)
                    {
                        continue;
                    }

                    const Float32 distance =
                        DistanceSquared(centroid, centroids[t]);

                    if (added < bestNewVertices ||
                        (added == bestNewVertices && distance < bestDistance))
                    {
                        bestNewVertices = added;
                        bestDistance = distance;
                        bestTriangle = t;
                    }
                }
            }

            // ---- 邻接用尽时的退路 ----
            //
            // 没有这一段的话, meshlet 一旦被已用的三角形围住就立刻收尾 ——
            // 而那在两种常见情形下坏得厉害:
            //
            //   立方体这类**每个面顶点独立**的网格, 任意两个面不共享顶点,
            //   于是每个面自成一个 meshlet: 12 个三角形切出 6 个 meshlet,
            //   顶点数据没有任何复用。
            //
            //   球这类连通网格上, 贪心长着长着会把自己围住 —— 实测平均
            //   只填到 62.7 / 124 个三角形。
            //
            // 退路是"窗口内最近且装得下的未用三角形"。距离要有上限:
            // 不设上限的话, 场景另一头的三角形也会被拉进来, 包围球撑满整个
            // 网格, 剔除彻底失效 —— 那是**正确但没用**的切分, 而正确性判据
            // 对它一个字都不会说。
            //
            // 上限取"当前 meshlet 包围盒对角线的一倍", 加一个绝对下限让
            // 只有一个三角形的 meshlet 也够得着邻居。
            //
            // 这个系数是量出来的 (球体 64x48, 6016 个三角形):
            //
            //     系数   meshlet 数   平均三角形   平均包围球半径
            //     关     96           62.67        0.2405
            //     1      94           64.00        0.2449  (+1.8%)
            //     2      90           66.84        0.2597  (+8.0%)
            //     4      81           74.27        0.2942  (+22.3%)
            //     8      73           82.41        0.3370  (+40.1%)
            //
            // 系数越大填得越满, 但包围球也越大 —— 而剔除效率大致随投影
            // 面积走, 半径涨 40% 就是面积涨 96%。
            //
            // 取 1 是因为**这条退路是为病态情形准备的, 不是用来榨填充率的**:
            // 立方体 (六个面互不共享顶点) 在系数 1 时就已经从 6 个 meshlet
            // 并回 1 个, OBJ 测试网格从 30 个并回 7 个 —— 该修的全修好了。
            // 再往上加, 买到的填充率全部由剔除精度付账。
            if (bestTriangle >= triangleCount)
            {
                constexpr SizeType kFallbackWindow = 1024;
                constexpr Float32 kExtentFactor = 1.0f;

                const Float32 limit =
                    FMath::Max(growing.Extent() * kExtentFactor, 1.0e-4f);

                const Float32 limitSquared = limit * limit;

                Float32 best = 1.0e30f;

                const SizeType end =
                    FMath::Min(searchStart + kFallbackWindow, triangleCount);

                for (SizeType t = searchStart; t < end; ++t)
                {
                    if (used[t] != 0 || !growing.CanFit(&indices[t * 3]))
                    {
                        continue;
                    }

                    const Float32 distance =
                        DistanceSquared(centroid, centroids[t]);

                    if (distance <= limitSquared && distance < best)
                    {
                        best = distance;
                        bestTriangle = t;
                    }
                }
            }

            if (bestTriangle >= triangleCount)
            {
                break;
            }

            growing.AddTriangle(&indices[bestTriangle * 3], vertices);

            used[bestTriangle] = 1;
            --remaining;
        }

        lastCentroid = growing.Centroid();
        hasLastCentroid = true;

        FinalizeMeshlet(growing, vertices, result);
    }

    // Log 而不是 Display: 一个网格一行, 而大场景有成百上千个网格。
    LIMX_LOG(LogMeshletBuilder, Log,
             "[Meshlet] {} 个三角形 / {} 个顶点 切成 {} 个 meshlet",
             triangleCount, vertices.GetSize(), result.Meshlets.GetSize());

    return result;
}

// ============================================================================
// FMeshletBuilder::ComputeStatistics
// ============================================================================

FMeshletStatistics FMeshletBuilder::ComputeStatistics(
    const FMeshletBuildResult& result)
{
    FMeshletStatistics statistics;

    statistics.MeshletCount =
        static_cast<UInt32>(result.Meshlets.GetSize());

    if (statistics.MeshletCount == 0)
    {
        return statistics;
    }

    UInt64 totalTriangles = 0;
    UInt64 totalVertices = 0;

    Float64 radiusSum = 0.0;

    UInt32 validCones = 0;

    for (SizeType m = 0; m < result.Meshlets.GetSize(); ++m)
    {
        const FMeshlet& meshlet = result.Meshlets[m];

        totalTriangles += meshlet.TriangleCount;
        totalVertices += meshlet.VertexCount;

        radiusSum += static_cast<Float64>(meshlet.BoundingSphere.W);

        if (meshlet.NormalCone.W > kInvalidConeCosine)
        {
            ++validCones;
        }
    }

    const Float32 count = static_cast<Float32>(statistics.MeshletCount);

    statistics.AverageTriangles =
        static_cast<Float32>(totalTriangles) / count;

    statistics.AverageVertices = static_cast<Float32>(totalVertices) / count;

    statistics.VertexReuse =
        (totalVertices > 0)
            ? (static_cast<Float32>(totalTriangles * 3) /
               static_cast<Float32>(totalVertices))
            : 0.0f;

    statistics.AverageSphereRadius =
        static_cast<Float32>(radiusSum / static_cast<Float64>(count));

    statistics.ValidConeFraction = static_cast<Float32>(validCones) / count;

    return statistics;
}

} // namespace Limx
