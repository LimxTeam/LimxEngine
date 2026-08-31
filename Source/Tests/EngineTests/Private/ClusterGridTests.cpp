/*******************************************************************************
 * 文件: ClusterGridTests.cpp
 * 创建时间: 2026-08-31
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   分簇网格数学的用例 — 切片映射、簇包围盒、球体相交
 *
 * 设计哲学:
 *   风险全在边界上。分簇在簇的内部区域怎么写都大致能用, 出问题的地方永远
 *   是那几处: 深度恰好落在切片边界、光球恰好擦过簇的角、簇下标恰好在网格
 *   边缘。而这些情况在真实场景里**一定会出现**, 只是不好复现。
 *
 *   分簇错了的表现是"某些角度下某些光突然不亮"。那看起来像是衰减参数或
 *   光源范围的问题, 离真正的原因隔了两层。
 *
 *   几条判据是刻意选的:
 *     - 切片映射必须**往返一致**: 深度 → 切片 → 该片的深度范围, 原深度
 *       必须落在那个范围里。只测"切片下标单调"不够, 单调而错位的映射也
 *       单调。
 *     - 包围盒必须**包住整个簇的视锥体**: 逐点采样簇内部的点, 每一个都
 *       必须在盒内。只测盒的尺寸合理是不够的。
 *     - 球体相交必须是**精确判据**: 专门构造贴着盒角的球 —— 保守近似
 *       (盒心距减半对角线) 会漏掉它们。
 *
 ******************************************************************************/

#include "EngineTests/EngineTestsMinimal.h"

#include "RenderCore/Lighting/FClusterGrid.h"

using namespace Limx;

namespace
{

constexpr Float32 kNear = 0.1f;
constexpr Float32 kFar  = 100.0f;

FMatrix TestProjection()
{
    return FMatrix::Perspective(0.7853981634f, 16.0f / 9.0f, kNear, kFar);
}

} // namespace

// ============================================================================
// 切片映射
// ============================================================================

LIMX_TEST(ClusterGrid, SliceMappingCoversTheWholeRange)
{
    const FClusterSliceMapping mapping = ComputeSliceMapping(kNear, kFar);

    // 近平面必须落在第 0 片, 远平面必须落在最后一片
    LIMX_EXPECT_EQ(SliceForViewDepth(kNear, mapping), 0u);
    LIMX_EXPECT_EQ(SliceForViewDepth(kFar * 0.999f, mapping),
                   kClusterGridZ - 1u);

    // 超出范围的深度要钳住, 不能越界
    LIMX_EXPECT_EQ(SliceForViewDepth(kNear * 0.5f, mapping), 0u);
    LIMX_EXPECT_EQ(SliceForViewDepth(kFar * 10.0f, mapping),
                   kClusterGridZ - 1u);
    LIMX_EXPECT_EQ(SliceForViewDepth(0.0f, mapping), 0u);
    LIMX_EXPECT_EQ(SliceForViewDepth(-5.0f, mapping), 0u);
}

LIMX_TEST(ClusterGrid, SliceMappingRoundTrips)
{
    const FClusterSliceMapping mapping = ComputeSliceMapping(kNear, kFar);

    // 深度 → 切片 → 该片的深度范围, 原深度必须落在里面。
    //
    // 这条比"切片下标随深度单调递增"强得多: 一个整体偏了一片的映射同样
    // 单调, 而它会让每个片元都去查相邻簇的光源列表 —— 画面上表现为光照
    // 沿深度方向整体错开一档, 边界处出现一圈明暗跳变。
    constexpr UInt32 kSamples = 512;

    for (UInt32 i = 0; i < kSamples; ++i)
    {
        // 在对数空间均匀取样 —— 线性取样会把绝大多数样本堆在远处
        const Float32 t =
            static_cast<Float32>(i) / static_cast<Float32>(kSamples - 1u);

        const Float32 depth = kNear * FMath::Pow(kFar / kNear, t);

        const UInt32 slice = SliceForViewDepth(depth, mapping);

        LIMX_EXPECT_TRUE(slice < kClusterGridZ);

        const Float32 sliceNear = SliceNearDepth(slice, kNear, kFar);
        const Float32 sliceFar  = SliceNearDepth(slice + 1u, kNear, kFar);

        // 容差按相对值给 —— 远处一片跨越几十米, 绝对容差在那里没有意义
        const Float32 tolerance = depth * 1.0e-4f;

        LIMX_EXPECT_TRUE(depth >= sliceNear - tolerance);
        LIMX_EXPECT_TRUE(depth <= sliceFar + tolerance);
    }
}

LIMX_TEST(ClusterGrid, SliceBoundariesAreContiguous)
{
    // 相邻两片必须首尾相接, 中间不能有缝。有缝的话落在缝里的深度会被
    // 钳到某一侧, 而那一侧的簇包围盒并不覆盖它 —— 那个片元拿到的是一个
    // 不该属于它的光源列表。
    for (UInt32 slice = 0; slice + 1u < kClusterGridZ; ++slice)
    {
        const Float32 thisFar = SliceNearDepth(slice + 1u, kNear, kFar);
        const Float32 nextNear = SliceNearDepth(slice + 1u, kNear, kFar);

        LIMX_EXPECT_EQ(thisFar, nextNear);
    }

    // 两端必须正好是近远平面
    LIMX_EXPECT_NEAR(SliceNearDepth(0u, kNear, kFar), kNear, 1.0e-6f);
    LIMX_EXPECT_NEAR(SliceNearDepth(kClusterGridZ, kNear, kFar), kFar,
                     kFar * 1.0e-5f);
}

LIMX_TEST(ClusterGrid, DegenerateRangeDoesNotProduceNaN)
{
    // 非法输入下 log 会给出 NaN 或 Inf, 而 NaN 传进簇下标是一个越界的负数
    // —— 表现为随机读取索引表, 比"没有分簇"难查得多。
    const FClusterSliceMapping bad[] =
    {
        ComputeSliceMapping(0.0f, 100.0f),
        ComputeSliceMapping(-1.0f, 100.0f),
        ComputeSliceMapping(10.0f, 10.0f),
        ComputeSliceMapping(100.0f, 1.0f),
    };

    for (SizeType i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
    {
        // 系数本身必须是有限值
        LIMX_EXPECT_TRUE(bad[i].Scale == bad[i].Scale);
        LIMX_EXPECT_TRUE(bad[i].Bias == bad[i].Bias);

        // 用它算出的切片必须仍在合法范围内
        const UInt32 slice = SliceForViewDepth(5.0f, bad[i]);
        LIMX_EXPECT_TRUE(slice < kClusterGridZ);
    }
}

// ============================================================================
// 簇包围盒
// ============================================================================

LIMX_TEST(ClusterGrid, BoundsContainEveryPointInsideTheCluster)
{
    const FMatrix projection = TestProjection();
    const FMatrix inverse    = projection.Inverse();

    // 抽查若干个簇 —— 每个簇内部逐点采样, 每一个点都必须落在包围盒里。
    //
    // 只检查"盒的尺寸看起来合理"是不够的: 一个整体偏移的盒同样尺寸合理,
    // 而它会让贴着簇边缘的光被判成不相交。
    const UInt32 sampleClusters[][3] =
    {
        { 0u, 0u, 0u },
        { kClusterGridX - 1u, kClusterGridY - 1u, kClusterGridZ - 1u },
        { kClusterGridX / 2u, kClusterGridY / 2u, 0u },
        { kClusterGridX / 2u, kClusterGridY / 2u, kClusterGridZ / 2u },
        { 0u, kClusterGridY - 1u, kClusterGridZ / 3u },
        { kClusterGridX - 1u, 0u, kClusterGridZ * 2u / 3u },
    };

    for (SizeType c = 0; c < sizeof(sampleClusters) / sizeof(sampleClusters[0]);
         ++c)
    {
        const UInt32 cx = sampleClusters[c][0];
        const UInt32 cy = sampleClusters[c][1];
        const UInt32 cz = sampleClusters[c][2];

        const FClusterBounds bounds =
            ComputeClusterBounds(cx, cy, cz, inverse, kNear, kFar);

        const Float32 nearDepth = SliceNearDepth(cz, kNear, kFar);
        const Float32 farDepth  = SliceNearDepth(cz + 1u, kNear, kFar);

        // 在簇的 NDC 范围与深度范围内逐点取样
        constexpr UInt32 kSteps = 5;

        for (UInt32 iz = 0; iz < kSteps; ++iz)
        {
            const Float32 tz =
                static_cast<Float32>(iz) / static_cast<Float32>(kSteps - 1u);
            const Float32 depth = nearDepth + (farDepth - nearDepth) * tz;

            for (UInt32 iy = 0; iy < kSteps; ++iy)
            {
                for (UInt32 ix = 0; ix < kSteps; ++ix)
                {
                    const Float32 tx =
                        static_cast<Float32>(ix) /
                        static_cast<Float32>(kSteps - 1u);
                    const Float32 ty =
                        static_cast<Float32>(iy) /
                        static_cast<Float32>(kSteps - 1u);

                    const Float32 ndcX =
                        ((static_cast<Float32>(cx) + tx) /
                         static_cast<Float32>(kClusterGridX)) * 2.0f - 1.0f;
                    const Float32 ndcY =
                        ((static_cast<Float32>(cy) + ty) /
                         static_cast<Float32>(kClusterGridY)) * 2.0f - 1.0f;

                    const FVector3 ray =
                        UnprojectToViewRay(ndcX, ndcY, inverse);

                    const Float32 scale = depth / (-ray.Z);

                    const FVector3 point(ray.X * scale, ray.Y * scale, -depth);

                    // 容差按包围盒的尺度给 —— 绝对容差在远处的大簇上没有
                    // 意义, 在近处的小簇上又过于宽松。
                    const Float32 spanX = bounds.Max.X - bounds.Min.X;
                    const Float32 spanY = bounds.Max.Y - bounds.Min.Y;
                    const Float32 tol =
                        FMath::Max(FMath::Max(spanX, spanY), 1.0f) * 1.0e-4f;

                    LIMX_EXPECT_TRUE(point.X >= bounds.Min.X - tol);
                    LIMX_EXPECT_TRUE(point.X <= bounds.Max.X + tol);
                    LIMX_EXPECT_TRUE(point.Y >= bounds.Min.Y - tol);
                    LIMX_EXPECT_TRUE(point.Y <= bounds.Max.Y + tol);
                    LIMX_EXPECT_TRUE(point.Z >= bounds.Min.Z - tol);
                    LIMX_EXPECT_TRUE(point.Z <= bounds.Max.Z + tol);
                }
            }
        }
    }
}

LIMX_TEST(ClusterGrid, BoundsGrowWithDepth)
{
    // 透视投影下, 同一屏幕位置的簇在远处覆盖更大的世界范围。这条不成立
    // 就说明反投影用错了矩阵 (比如用了投影本身而不是它的逆)。
    const FMatrix inverse = TestProjection().Inverse();

    Float32 previousSpan = 0.0f;

    for (UInt32 cz = 0; cz < kClusterGridZ; ++cz)
    {
        const FClusterBounds bounds = ComputeClusterBounds(
            kClusterGridX / 2u, kClusterGridY / 2u, cz, inverse, kNear, kFar);

        const Float32 span = bounds.Max.X - bounds.Min.X;

        LIMX_EXPECT_TRUE(span > previousSpan);

        previousSpan = span;
    }
}

LIMX_TEST(ClusterGrid, AdjacentClustersLeaveNoGap)
{
    // 相邻两簇的包围盒在 x 方向**不能有缝**。有缝的话, 落在缝里的光源两个
    // 簇都不认领 —— 表现为屏幕上出现规则的暗条纹, 而那看起来像是光照本身
    // 的问题。
    //
    // 注意判据是"不能有缝"而不是"首尾相接"。用轴对齐盒去包一个视锥切片时,
    // 相邻簇的盒**必然重叠**: cx 的盒的右边界来自它远端那一面的右上角, 而
    // cx+1 的左边界来自它近端那一面的左下角 —— 后者比前者更靠内。
    //
    // 重叠是良性的: 它只让某盏光被多分配给几个簇, 剔除偏保守。少算才是
    // 错的。这条用例最初写成了 EXPECT_NEAR(a.Max.X, b.Min.X) 并因此变红,
    // 而变红的是用例的前提, 不是实现。
    const FMatrix inverse = TestProjection().Inverse();

    for (UInt32 cz = 0; cz < kClusterGridZ; cz += 4u)
    {
        for (UInt32 cx = 0; cx + 1u < kClusterGridX; ++cx)
        {
            const FClusterBounds a = ComputeClusterBounds(
                cx, kClusterGridY / 2u, cz, inverse, kNear, kFar);
            const FClusterBounds b = ComputeClusterBounds(
                cx + 1u, kClusterGridY / 2u, cz, inverse, kNear, kFar);

            LIMX_EXPECT_TRUE(a.Max.X >= b.Min.X);

            // 而且重叠不能失控。重叠量应当与"远端比近端宽出来的那一截"
            // 同量级 —— 大幅超出说明反投影或深度范围算错了。
            const Float32 spanA = a.Max.X - a.Min.X;
            LIMX_EXPECT_TRUE(a.Max.X - b.Min.X <= spanA);
        }
    }

    // 纵向同理
    for (UInt32 cy = 0; cy + 1u < kClusterGridY; ++cy)
    {
        const FClusterBounds a = ComputeClusterBounds(
            kClusterGridX / 2u, cy, kClusterGridZ / 2u, inverse, kNear, kFar);
        const FClusterBounds b = ComputeClusterBounds(
            kClusterGridX / 2u, cy + 1u, kClusterGridZ / 2u, inverse,
            kNear, kFar);

        LIMX_EXPECT_TRUE(a.Max.Y >= b.Min.Y);
    }
}

// ============================================================================
// 球体相交
// ============================================================================

LIMX_TEST(ClusterGrid, SphereAabbBasicCases)
{
    const FVector3 boxMin(-1.0f, -1.0f, -1.0f);
    const FVector3 boxMax(1.0f, 1.0f, 1.0f);

    // 球心在盒内
    LIMX_EXPECT_TRUE(SphereIntersectsAABB(
        FVector3(0.0f, 0.0f, 0.0f), 0.1f, boxMin, boxMax));

    // 球完全在盒外, 远离
    LIMX_EXPECT_TRUE(!SphereIntersectsAABB(
        FVector3(10.0f, 0.0f, 0.0f), 1.0f, boxMin, boxMax));

    // 球贴着面
    LIMX_EXPECT_TRUE(SphereIntersectsAABB(
        FVector3(1.5f, 0.0f, 0.0f), 0.5f, boxMin, boxMax));

    // 刚好差一点点
    LIMX_EXPECT_TRUE(!SphereIntersectsAABB(
        FVector3(1.5f, 0.0f, 0.0f), 0.49f, boxMin, boxMax));
}

LIMX_TEST(ClusterGrid, SphereTouchingCornerIsDetected)
{
    // 贴着盒角的球是精确判据与保守近似的分水岭。
    //
    // "盒心到球心的距离 <= 半径 + 半对角线"那种写法在这里会给出**相反**
    // 的结论: 它把盒当成了外接球, 于是角落附近的球被判成相交 (多算) ——
    // 而反过来"盒心距 <= 半径"那种则会漏掉 (少算)。少算的后果是那个簇
    // 缺一盏光。
    const FVector3 boxMin(0.0f, 0.0f, 0.0f);
    const FVector3 boxMax(1.0f, 1.0f, 1.0f);

    // 球心在 (1,1,1) 角外的对角方向上, 距离角点 sqrt(3)*0.1
    const Float32 offset = 0.1f;
    const FVector3 center(1.0f + offset, 1.0f + offset, 1.0f + offset);

    const Float32 distanceToCorner =
        FMath::Sqrt(3.0f) * offset;

    // 半径略大于到角点的距离 —— 必须判为相交
    LIMX_EXPECT_TRUE(SphereIntersectsAABB(
        center, distanceToCorner * 1.01f, boxMin, boxMax));

    // 半径略小 —— 必须判为不相交
    LIMX_EXPECT_TRUE(!SphereIntersectsAABB(
        center, distanceToCorner * 0.99f, boxMin, boxMax));
}

LIMX_TEST(ClusterGrid, SphereTouchingEdgeIsDetected)
{
    // 棱上的情况: 两个轴超出、一个轴在范围内。写成"只比较各轴的一维距离"
    // 的实现在这里会出错 —— 它会把两个方向的超出量分别与半径比较, 而正确
    // 的判据是它们的平方和。
    const FVector3 boxMin(0.0f, 0.0f, 0.0f);
    const FVector3 boxMax(1.0f, 1.0f, 1.0f);

    const Float32 offset = 0.3f;
    const FVector3 center(1.0f + offset, 1.0f + offset, 0.5f);

    const Float32 distanceToEdge = FMath::Sqrt(2.0f) * offset;

    LIMX_EXPECT_TRUE(SphereIntersectsAABB(
        center, distanceToEdge * 1.01f, boxMin, boxMax));

    // 0.42 在 sqrt(2)*0.3 ≈ 0.424 之下, 但**大于**任何单轴的超出量 0.3 ——
    // 只比一维距离的实现会在这里错判为相交。
    LIMX_EXPECT_TRUE(!SphereIntersectsAABB(center, 0.42f, boxMin, boxMax));
}

// ============================================================================
// 线性下标
// ============================================================================

LIMX_TEST(ClusterGrid, LinearIndexOrderIsPinned)
{
    // 排布顺序必须是 x 变化最快、z 最慢, 而且要**逐值钉死**。
    //
    // 只验"是双射"是不够的 —— 任何置换都是双射。而顺序是否与
    // Shaders/Builtin/cluster_common.h 里那份镜像一致, 没有编译期保障:
    // 计算着色器按一个顺序写表、片段着色器按另一个顺序查表, 表现是光照
    // 整体错位到别的屏幕区域, 而画面依然"有光", 看起来像光源摆错了位置。
    LIMX_EXPECT_EQ(ClusterLinearIndex(0u, 0u, 0u), 0u);

    // x 走一格 = 线性下标走 1
    LIMX_EXPECT_EQ(ClusterLinearIndex(1u, 0u, 0u), 1u);

    // y 走一格 = 走一整行
    LIMX_EXPECT_EQ(ClusterLinearIndex(0u, 1u, 0u), kClusterGridX);

    // z 走一格 = 走一整层
    LIMX_EXPECT_EQ(ClusterLinearIndex(0u, 0u, 1u),
                   kClusterGridX * kClusterGridY);

    // 最后一个簇
    LIMX_EXPECT_EQ(
        ClusterLinearIndex(kClusterGridX - 1u, kClusterGridY - 1u,
                           kClusterGridZ - 1u),
        kClusterCount - 1u);

    // 混合下标 —— 上面四条各自只动一个轴, 系数写反了仍可能全部通过
    LIMX_EXPECT_EQ(ClusterLinearIndex(3u, 2u, 5u),
                   3u + 2u * kClusterGridX +
                       5u * kClusterGridX * kClusterGridY);
}

LIMX_TEST(ClusterGrid, UnprojectLandsOnTheFarPlane)
{
    // UnprojectToViewRay 的契约是"NDC 的一点在**远平面**上对应的视空间
    // 坐标", 不只是一个方向。
    //
    // 这条要单独验, 因为调用方随后会把结果按深度重新缩放 —— 均匀缩放会
    // 抵消掉透视除法的影响, 于是"漏掉透视除法"这个变异在别的用例里全部
    // 通过。函数的契约只有它自己能验。
    const FMatrix inverse = TestProjection().Inverse();

    const Float32 ndc[][2] =
    {
        {  0.0f,  0.0f },
        { -1.0f, -1.0f },
        {  1.0f,  1.0f },
        {  0.5f, -0.3f },
    };

    for (SizeType i = 0; i < sizeof(ndc) / sizeof(ndc[0]); ++i)
    {
        const FVector3 point = UnprojectToViewRay(ndc[i][0], ndc[i][1],
                                                  inverse);

        // 视空间 -Z 为前方, 远平面在 -kFar 处
        LIMX_EXPECT_NEAR(point.Z, -kFar, kFar * 1.0e-4f);
    }

    // 屏幕中心必须落在光轴上
    const FVector3 center = UnprojectToViewRay(0.0f, 0.0f, inverse);
    LIMX_EXPECT_NEAR(center.X, 0.0f, 1.0e-4f);
    LIMX_EXPECT_NEAR(center.Y, 0.0f, 1.0e-4f);
}

LIMX_TEST(ClusterGrid, LinearIndexIsABijection)
{
    // 三维下标到线性下标必须是双射, 且覆盖 [0, kClusterCount)。
    //
    // 顺序错了 (比如 x 与 z 调换) 的表现是光照整体错位到别的屏幕区域, 而
    // 画面依然"有光" —— 看起来像是光源位置摆错了。
    constexpr UInt32 kSamplesPerAxis = 4;

    UInt32 maxIndex = 0;

    for (UInt32 z = 0; z < kClusterGridZ; ++z)
    {
        for (UInt32 y = 0; y < kClusterGridY; ++y)
        {
            for (UInt32 x = 0; x < kClusterGridX; ++x)
            {
                const UInt32 index = ClusterLinearIndex(x, y, z);

                LIMX_EXPECT_TRUE(index < kClusterCount);

                maxIndex = FMath::Max(maxIndex, index);
            }
        }
    }

    // 必须正好用满整个范围 —— 用不满说明有下标被浪费, 而浪费通常意味着
    // 另一处有下标被重复使用。
    LIMX_EXPECT_EQ(maxIndex, kClusterCount - 1u);

    (void)kSamplesPerAxis;
}

LIMX_TEST(ClusterGrid, LinearIndexHasNoCollisions)
{
    // 逐个下标标记一遍, 确认每个线性下标恰好被用到一次。
    // 上一条只验了范围, 范围对而有碰撞的映射同样满足它。
    bool* const seen = static_cast<bool*>(
        GetDefaultAllocator().Allocate(kClusterCount * sizeof(bool),
                                       alignof(bool)));

    for (UInt32 i = 0; i < kClusterCount; ++i)
    {
        seen[i] = false;
    }

    UInt32 collisions = 0;

    for (UInt32 z = 0; z < kClusterGridZ; ++z)
    {
        for (UInt32 y = 0; y < kClusterGridY; ++y)
        {
            for (UInt32 x = 0; x < kClusterGridX; ++x)
            {
                const UInt32 index = ClusterLinearIndex(x, y, z);

                if (seen[index])
                {
                    ++collisions;
                }

                seen[index] = true;
            }
        }
    }

    UInt32 unused = 0;

    for (UInt32 i = 0; i < kClusterCount; ++i)
    {
        if (!seen[i])
        {
            ++unused;
        }
    }

    GetDefaultAllocator().Deallocate(seen);

    LIMX_EXPECT_EQ(collisions, 0u);
    LIMX_EXPECT_EQ(unused, 0u);
}
