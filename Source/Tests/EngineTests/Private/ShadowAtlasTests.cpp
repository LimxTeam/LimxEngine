/*******************************************************************************
 * 文件: ShadowAtlasTests.cpp
 * 创建时间: 2026-08-31
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   阴影图集的分块与聚光灯阴影矩阵的用例
 *
 * 设计哲学:
 *   这两件事的错误方式相同: **不崩、不报错、只是阴影落在错误的位置**。而
 *   阴影落错位置在画面上极难与"阴影偏移参数没调好"区分开 —— 后者是人人都
 *   会先怀疑的方向, 于是真正的原因可以藏很久。
 *
 *   判据因此全部落在可计算的量上:
 *     - 分块必须**互不重叠且铺满**。只验"下标在范围内"不够 —— 一个把两盏灯
 *       映射到同一块的分块函数同样满足那一条, 而后果是两盏灯共用一张阴影图。
 *     - 阴影矩阵必须把**光锥边缘恰好映射到 NDC 边界**。视场角算错的表现是
 *       锥边缘漏光或分辨率白白浪费, 两者都不明显。
 *     - 竖直朝下的聚光灯必须给出有限的矩阵。这是最容易漏的一条, 而"灯朝
 *       正下方"恰恰是最常见的摆法。
 *
 ******************************************************************************/

#include "EngineTests/EngineTestsMinimal.h"

#include "RenderCore/Lighting/FShadowAtlas.h"

using namespace Limx;

// ============================================================================
// 分块
// ============================================================================

LIMX_TEST(ShadowAtlas, TilesTileTheAtlasExactly)
{
    // 逐块把它覆盖的纹素标记一遍: 每个纹素必须恰好被覆盖一次。
    //
    // 这一条同时排除了重叠与空隙。只验"矩形在图集范围内"是不够的 —— 一个
    // 把所有块都映射到 (0,0) 的实现同样满足那一条, 而后果是全部光源共用
    // 同一张阴影图。
    //
    // 用计数而非布尔: 布尔只能发现"被覆盖过", 计数能区分"覆盖了两次"。
    constexpr UInt32 kGrid = kShadowAtlasSize / kShadowTileSize;

    UInt32 coverage[kGrid][kGrid] = {};

    for (UInt32 i = 0; i < kShadowTileCount; ++i)
    {
        const FShadowTileRect rect = ShadowTileRect(i);

        LIMX_EXPECT_EQ(rect.Size, kShadowTileSize);

        LIMX_EXPECT_EQ(rect.X % kShadowTileSize, 0u);
        LIMX_EXPECT_EQ(rect.Y % kShadowTileSize, 0u);

        const UInt32 tx = rect.X / kShadowTileSize;
        const UInt32 ty = rect.Y / kShadowTileSize;

        // 先判界再落格。落格前不判界的话, 一个"忘了取模"的分块函数会让这里
        // 写出数组之外 —— 用例确实会失败, 但那是越界写坏了内存导致的失败,
        // 与被测的性质无关, 换个内存布局就可能变成通过。
        LIMX_EXPECT_TRUE(tx < kGrid);
        LIMX_EXPECT_TRUE(ty < kGrid);

        if (tx < kGrid && ty < kGrid)
        {
            ++coverage[ty][tx];
        }
    }

    UInt32 uncovered = 0;
    UInt32 doubled   = 0;

    for (UInt32 y = 0; y < kGrid; ++y)
    {
        for (UInt32 x = 0; x < kGrid; ++x)
        {
            if (coverage[y][x] == 0u)
            {
                ++uncovered;
            }
            else if (coverage[y][x] > 1u)
            {
                ++doubled;
            }
        }
    }

    LIMX_EXPECT_EQ(uncovered, 0u);
    LIMX_EXPECT_EQ(doubled, 0u);
}

LIMX_TEST(ShadowAtlas, TileOrderIsRowMajorAndPinned)
{
    // 顺序要逐值钉死。计算侧写块与着色侧读块必须用同一个约定, 而那没有
    // 编译期保障 —— 不一致的表现是"这盏灯的阴影形状完全不对", 而不是
    // "没有阴影"。
    LIMX_EXPECT_EQ(ShadowTileRect(0).X, 0u);
    LIMX_EXPECT_EQ(ShadowTileRect(0).Y, 0u);

    // 下标 +1 沿 +x 走一块
    LIMX_EXPECT_EQ(ShadowTileRect(1).X, kShadowTileSize);
    LIMX_EXPECT_EQ(ShadowTileRect(1).Y, 0u);

    // 走满一行换行
    LIMX_EXPECT_EQ(ShadowTileRect(kShadowTilesPerRow).X, 0u);
    LIMX_EXPECT_EQ(ShadowTileRect(kShadowTilesPerRow).Y, kShadowTileSize);

    // 最后一块在右下角
    const FShadowTileRect last = ShadowTileRect(kShadowTileCount - 1);

    LIMX_EXPECT_EQ(last.X, kShadowAtlasSize - kShadowTileSize);
    LIMX_EXPECT_EQ(last.Y, kShadowAtlasSize - kShadowTileSize);
}

LIMX_TEST(ShadowAtlas, OutOfRangeTileIsRejected)
{
    // 越界返回 Size = 0 而不是钳到最后一块。钳的话越界的光源会静默地与
    // 最后一块共用阴影图, 而那看起来"有阴影, 只是不对"。
    const FShadowTileRect bad = ShadowTileRect(kShadowTileCount);

    LIMX_EXPECT_EQ(bad.Size, 0u);

    const FShadowTileRect invalid = ShadowTileRect(kInvalidShadowTile);

    LIMX_EXPECT_EQ(invalid.Size, 0u);
}

LIMX_TEST(ShadowAtlas, UvTransformMatchesTileRect)
{
    // UV 变换与纹素矩形必须描述同一件事。两者各写一遍而不一致时, 阴影会
    // 偏移整整一块 —— 而画面上那是"取到了隔壁灯的阴影"。
    for (UInt32 i = 0; i < kShadowTileCount; ++i)
    {
        const FShadowTileRect rect = ShadowTileRect(i);
        const FVector4        uv   = ShadowTileUvTransform(i);

        const Float32 expectedU =
            static_cast<Float32>(rect.X) /
            static_cast<Float32>(kShadowAtlasSize);
        const Float32 expectedV =
            static_cast<Float32>(rect.Y) /
            static_cast<Float32>(kShadowAtlasSize);
        const Float32 expectedScale =
            static_cast<Float32>(rect.Size) /
            static_cast<Float32>(kShadowAtlasSize);

        LIMX_EXPECT_NEAR(uv.X, expectedU, 1.0e-6f);
        LIMX_EXPECT_NEAR(uv.Y, expectedV, 1.0e-6f);
        LIMX_EXPECT_NEAR(uv.Z, expectedScale, 1.0e-6f);
        LIMX_EXPECT_NEAR(uv.W, expectedScale, 1.0e-6f);
    }
}

// ============================================================================
// 上向量
// ============================================================================

LIMX_TEST(ShadowAtlas, StraightDownSpotDoesNotCollapse)
{
    // **最容易漏的一条。** 聚光灯直直朝下时, 用 (0,1,0) 作上向量会让
    // cross(forward, up) 为零, 视图矩阵整个塌掉。而"灯朝正下方"恰恰是最
    // 常见的摆法 (路灯、射灯), 不是边角情形。
    const FVector3 straightDown(0.0f, -1.0f, 0.0f);
    const FVector3 straightUp(0.0f, 1.0f, 0.0f);

    const FVector3 upForDown = ShadowUpVectorFor(straightDown);
    const FVector3 upForUp   = ShadowUpVectorFor(straightUp);

    // 换出来的上向量必须与方向不平行
    LIMX_EXPECT_TRUE(FMath::Abs(FVector3::Dot(upForDown, straightDown)) < 0.1f);
    LIMX_EXPECT_TRUE(FMath::Abs(FVector3::Dot(upForUp, straightUp)) < 0.1f);

    // 矩阵的每一项都必须是有限值
    const FMatrix matrix =
        ComputeSpotShadowMatrix(FVector3(0.0f, 5.0f, 0.0f), straightDown,
                                0.7071f, 20.0f);

    for (Int32 row = 0; row < 4; ++row)
    {
        for (Int32 col = 0; col < 4; ++col)
        {
            const Float32 value = matrix.M[row][col];

            // NaN 与自己不相等; Inf 的绝对值大于任何有限阈值
            LIMX_EXPECT_TRUE(value == value);
            LIMX_EXPECT_TRUE(FMath::Abs(value) < 1.0e30f);
        }
    }
}

LIMX_TEST(ShadowAtlas, TypicalDirectionsKeepTheDefaultUp)
{
    // 只有接近竖直时才换轴。别的方向换轴会让阴影贴图整体旋转, 而旋转本身
    // 无害 —— 但"什么时候旋转"若无规律, 光源缓慢转动时阴影会在某个角度
    // 突然跳一下。
    const FVector3 horizontal(1.0f, 0.0f, 0.0f);
    const FVector3 slanted =
        FVector3(0.5f, -0.5f, 0.7071f).GetSafeNormal();

    LIMX_EXPECT_NEAR(ShadowUpVectorFor(horizontal).Y, 1.0f, 1.0e-6f);
    LIMX_EXPECT_NEAR(ShadowUpVectorFor(slanted).Y, 1.0f, 1.0e-6f);
}

// ============================================================================
// 阴影矩阵
// ============================================================================

LIMX_TEST(ShadowAtlas, ConeEdgeMapsToNdcBoundary)
{
    // 视场角必须恰好是外锥角的两倍 —— 于是光锥的边缘正好落在 NDC 的
    // ±1 上。
    //
    // 小了会在锥边缘漏光 (那一圈没有阴影信息, 采样落到贴图外), 大了则白白
    // 浪费分辨率。两种都不明显, 所以要用数值钉。
    const FVector3 position(0.0f, 5.0f, 0.0f);
    const FVector3 direction(0.0f, -1.0f, 0.0f);

    const Float32 outerAngle = 0.5f;          // 弧度
    const Float32 outerCos   = FMath::Cos(outerAngle);
    const Float32 range      = 20.0f;

    const FMatrix viewProj =
        ComputeSpotShadowMatrix(position, direction, outerCos, range);

    // 取距光源 10 单位、恰好在锥边缘上的一点。
    //
    // 锥轴朝 -y, 边缘与轴成 outerAngle。上向量此时被换成 (0,0,1), 所以
    // 屏幕的"右"是 cross(forward, up) = cross((0,-1,0),(0,0,1)) = (-1,0,0)。
    // 沿 ±x 偏出去的点会落在 NDC 的 x = ∓1 上。
    const Float32 distance = 10.0f;

    const FVector3 edgePoint(
        position.X + distance * FMath::Sin(outerAngle),
        position.Y - distance * FMath::Cos(outerAngle),
        position.Z);

    const FVector4 clip = viewProj.TransformVector4(
        FVector4(edgePoint.X, edgePoint.Y, edgePoint.Z, 1.0f));

    // w 必须为**正** —— 光锥里的点在阴影相机前方。只验 |w| 非零的话, 一个
    // 朝后的阴影相机同样满足, 而它算出的 x/w 依旧是 0 (轴上点的横向偏移
    // 本来就是 0), 于是整条判据形同虚设。
    LIMX_EXPECT_TRUE(clip.W > 1.0e-6f);

    const Float32 ndcX = clip.X / clip.W;

    // 落在边界上, 符号不重要 (取决于上向量的选择)
    LIMX_EXPECT_NEAR(FMath::Abs(ndcX), 1.0f, 1.0e-3f);
}

LIMX_TEST(ShadowAtlas, AxisMapsToNdcCenter)
{
    // 锥轴上的点必须落在 NDC 中心。偏了说明视图矩阵的朝向不对 —— 而那的
    // 表现是阴影整体平移, 极易被当成"深度偏移调大了"。
    const FVector3 position(2.0f, 5.0f, -3.0f);
    const FVector3 direction =
        FVector3(0.3f, -1.0f, 0.2f).GetSafeNormal();

    const FMatrix viewProj =
        ComputeSpotShadowMatrix(position, direction, 0.8f, 20.0f);

    for (Float32 distance = 1.0f; distance <= 15.0f; distance += 2.0f)
    {
        const FVector3 onAxis(position.X + direction.X * distance,
                              position.Y + direction.Y * distance,
                              position.Z + direction.Z * distance);

        const FVector4 clip = viewProj.TransformVector4(
            FVector4(onAxis.X, onAxis.Y, onAxis.Z, 1.0f));

        // 同样要求 w 为正: 轴上点的横向偏移天然是 0, 所以 x/w ≈ 0 这一条对
        // 朝前朝后的阴影相机都成立。区分两者的只有 w 的符号。
        LIMX_EXPECT_TRUE(clip.W > 1.0e-6f);

        LIMX_EXPECT_NEAR(clip.X / clip.W, 0.0f, 1.0e-4f);
        LIMX_EXPECT_NEAR(clip.Y / clip.W, 0.0f, 1.0e-4f);
    }
}

LIMX_TEST(ShadowAtlas, DepthSpansTheNearFarRange)
{
    // 近平面处深度为 0, 远平面处为 1 (Vulkan 的 NDC z 范围)。
    //
    // 反了的话深度比较的方向整个错误 —— 表现是"到处都是阴影"或"完全没有
    // 阴影", 而那两种都会先被归因到比较函数或偏移量上。
    const FVector3 position(0.0f, 0.0f, 0.0f);
    const FVector3 direction(0.0f, 0.0f, -1.0f);

    const Float32 range = 20.0f;

    const FMatrix viewProj =
        ComputeSpotShadowMatrix(position, direction, 0.7071f, range);

    const Float32 nearPlane = 0.05f;

    const FVector4 nearClip = viewProj.TransformVector4(
        FVector4(0.0f, 0.0f, -nearPlane, 1.0f));
    const FVector4 farClip = viewProj.TransformVector4(
        FVector4(0.0f, 0.0f, -range, 1.0f));

    LIMX_EXPECT_NEAR(nearClip.Z / nearClip.W, 0.0f, 1.0e-4f);
    LIMX_EXPECT_NEAR(farClip.Z / farClip.W, 1.0f, 1.0e-4f);
}

LIMX_TEST(ShadowAtlas, DegenerateConeCosineDoesNotProduceNaN)
{
    // 余弦超出 [-1,1] 时 acos 给 NaN, 而 NaN 投影矩阵的表现是那盏灯的阴影
    // 整片消失 —— 没有任何报错。
    const Float32 badCosines[] = { 1.0f, -1.0f, 1.5f, -1.5f, 0.0f };

    for (SizeType i = 0; i < sizeof(badCosines) / sizeof(badCosines[0]); ++i)
    {
        const FMatrix matrix = ComputeSpotShadowMatrix(
            FVector3(0.0f, 1.0f, 0.0f), FVector3(0.0f, -1.0f, 0.0f),
            badCosines[i], 10.0f);

        for (Int32 row = 0; row < 4; ++row)
        {
            for (Int32 col = 0; col < 4; ++col)
            {
                const Float32 value = matrix.M[row][col];

                LIMX_EXPECT_TRUE(value == value);
                LIMX_EXPECT_TRUE(FMath::Abs(value) < 1.0e30f);
            }
        }
    }
}
