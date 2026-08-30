/*******************************************************************************
 * 文件: GeometryWindingTests.cpp
 * 创建时间: 2026-08-30
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   内置几何体的三角形缠绕方向与法线一致性测试
 *
 * 设计哲学:
 *   缠绕方向错了不会崩、不会报错、轮廓也一模一样 —— 唯一的后果是背面
 *   剔除把朝向相机的那半边剔掉, 画面上留下的是背对相机的那一半。而
 *   "看到的是远侧表面"这件事, 在有主光源的场景里只表现为"光好像从
 *   另一边来", 极难被认出是缠绕问题。
 *
 *   `GenerateSphere` 就这样带着反向缠绕存在了很久, 直到白炉测试第一次
 *   给出可核对的真值 (法线必须朝向相机) 才暴露。这类错误一旦回归, 同样
 *   不会有任何报错 —— 因此必须由测试钉住。
 *
 *   判据是纯几何的, 不依赖渲染: 对闭合凸体, 顶点顺序按右手定则算出的
 *   面法线必须与该三角形的外向方向同侧。这样测试既不需要 GPU, 也不受
 *   投影矩阵、剔除模式这些约定的影响。
 *
 * 技术特性:
 *   - 逐三角形检查, 不抽样 —— 缠绕错误常常只出现在某一圈或某一面上
 *   - 同时检查顶点法线与几何法线同向, 二者不一致会让光照与轮廓打架
 *
 * 依赖关系:
 *   内部: EngineTests/EngineTestsMinimal.h,
 *         RenderCore/Geometry/FGeometryGenerator.h
 *
 ******************************************************************************/

#include "EngineTests/EngineTestsMinimal.h"
#include "RenderCore/Geometry/FGeometryGenerator.h"

using namespace Limx;

namespace
{

/// 三角形按右手定则算出的面法线 (未归一化)
FVector3 TriangleNormal(const FMeshData& mesh, SizeType triangleIndex)
{
    const UInt32 i0 = mesh.Indices[triangleIndex * 3 + 0];
    const UInt32 i1 = mesh.Indices[triangleIndex * 3 + 1];
    const UInt32 i2 = mesh.Indices[triangleIndex * 3 + 2];

    const FVector3 p0 = mesh.Vertices[i0].Position;
    const FVector3 p1 = mesh.Vertices[i1].Position;
    const FVector3 p2 = mesh.Vertices[i2].Position;

    return FVector3::Cross(p1 - p0, p2 - p0);
}

/// 统计面法线背离几何中心 (即"朝外") 的三角形数量
///
/// 对以原点为中心的闭合凸体, 三角形重心的方向就是它的外法线方向。
void CountOutwardTriangles(const FMeshData& mesh,
                           SizeType& outOutward,
                           SizeType& outInward,
                           SizeType& outDegenerate)
{
    outOutward    = 0;
    outInward     = 0;
    outDegenerate = 0;

    const SizeType triangleCount = mesh.Indices.GetSize() / 3;

    for (SizeType i = 0; i < triangleCount; ++i)
    {
        const FVector3 faceNormal = TriangleNormal(mesh, i);

        if (faceNormal.LengthSquared() < 1.0e-12f)
        {
            ++outDegenerate;
            continue;
        }

        const UInt32 i0 = mesh.Indices[i * 3 + 0];
        const UInt32 i1 = mesh.Indices[i * 3 + 1];
        const UInt32 i2 = mesh.Indices[i * 3 + 2];

        const FVector3 centroid =
            (mesh.Vertices[i0].Position +
             mesh.Vertices[i1].Position +
             mesh.Vertices[i2].Position) * (1.0f / 3.0f);

        if (FVector3::Dot(faceNormal, centroid) > 0.0f)
        {
            ++outOutward;
        }
        else
        {
            ++outInward;
        }
    }
}

} // namespace

// ============================================================================
// 缠绕方向
// ============================================================================

LIMX_TEST(GeometryWinding, SphereWindsOutward)
{
    // 反向缠绕会让背面剔除剔掉朝向相机的半球, 留下背对相机的那一半 ——
    // 法线因此整体反了向, 而轮廓完全不变。
    const FMeshData sphere = FGeometryGenerator::GenerateSphere(1.0f, 16, 8);

    SizeType outward = 0;
    SizeType inward  = 0;
    SizeType degenerate = 0;

    CountOutwardTriangles(sphere, outward, inward, degenerate);

    LIMX_EXPECT_GT(outward, static_cast<SizeType>(0));
    LIMX_EXPECT_EQ(inward, static_cast<SizeType>(0));

    // UV 球在两极处整行顶点坍缩到同一点, 早先的实现照常给每个格子发两个
    // 三角形, 于是每极多出 slices 个零面积三角形。它们画不出任何东西, 却
    // 照样占索引、照样过顶点着色器, 而且任何按面法线做的判断 (背面剔除、
    // 法线核对、切线求解) 都要为它们特判。
    LIMX_EXPECT_EQ(degenerate, static_cast<SizeType>(0));
}

LIMX_TEST(GeometryWinding, SphereHasNoDegenerateTrianglesAtAnyResolution)
{
    // 退化三角形的数量与 slices 成正比、与 stacks 无关 —— 只在两极那两圈
    // 出现。多试几组分辨率, 免得只在某一组恰好为零。
    constexpr UInt32 kSlices[] = { 3, 4, 8, 16, 33 };
    constexpr UInt32 kStacks[] = { 2, 3, 8, 16, 17 };

    for (SizeType s = 0; s < LIMX_ARRAY_COUNT(kSlices); ++s)
    {
        for (SizeType t = 0; t < LIMX_ARRAY_COUNT(kStacks); ++t)
        {
            const FMeshData sphere =
                FGeometryGenerator::GenerateSphere(1.0f, kSlices[s], kStacks[t]);

            SizeType outward    = 0;
            SizeType inward     = 0;
            SizeType degenerate = 0;

            CountOutwardTriangles(sphere, outward, inward, degenerate);

            LIMX_EXPECT_EQ(degenerate, static_cast<SizeType>(0));
            LIMX_EXPECT_EQ(inward, static_cast<SizeType>(0));
            LIMX_EXPECT_GT(outward, static_cast<SizeType>(0));

            // 三角形总数: 两极各是一圈三角形, 中间每圈两个。
            const SizeType expected =
                static_cast<SizeType>(kSlices[s]) *
                (static_cast<SizeType>(kStacks[t]) * 2 - 2);

            LIMX_EXPECT_EQ(sphere.Indices.GetSize() / 3, expected);
        }
    }
}

LIMX_TEST(GeometryWinding, CubeWindsOutward)
{
    const FMeshData cube = FGeometryGenerator::GenerateCube();

    SizeType outward = 0;
    SizeType inward  = 0;
    SizeType degenerate = 0;

    CountOutwardTriangles(cube, outward, inward, degenerate);

    LIMX_EXPECT_EQ(outward, static_cast<SizeType>(12));
    LIMX_EXPECT_EQ(inward, static_cast<SizeType>(0));
    LIMX_EXPECT_EQ(degenerate, static_cast<SizeType>(0));
}

LIMX_TEST(GeometryWinding, SphereFaceNormalAgreesWithVertexNormal)
{
    // 顶点法线用于着色, 面法线决定剔除。两者反向时, 物体会按"正面"着色
    // 却按"背面"被剔除 —— 或者反过来。这是缠绕错误的另一个等价判据,
    // 而且它对非凸网格同样成立。
    const FMeshData sphere = FGeometryGenerator::GenerateSphere(1.0f, 16, 8);

    const SizeType triangleCount = sphere.Indices.GetSize() / 3;

    SizeType agreeing = 0;
    SizeType checked  = 0;

    for (SizeType i = 0; i < triangleCount; ++i)
    {
        const FVector3 faceNormal = TriangleNormal(sphere, i);

        if (faceNormal.LengthSquared() < 1.0e-12f)
        {
            continue;
        }

        const UInt32 i0 = sphere.Indices[i * 3 + 0];

        // 球面上顶点法线就是位置方向, 取任一顶点即可
        const FVector3 vertexNormal = sphere.Vertices[i0].Normal;

        ++checked;

        if (FVector3::Dot(faceNormal, vertexNormal) > 0.0f)
        {
            ++agreeing;
        }
    }

    LIMX_EXPECT_GT(checked, static_cast<SizeType>(0));
    LIMX_EXPECT_EQ(agreeing, checked);
}

LIMX_TEST(GeometryWinding, CubeFaceNormalAgreesWithVertexNormal)
{
    const FMeshData cube = FGeometryGenerator::GenerateCube();

    const SizeType triangleCount = cube.Indices.GetSize() / 3;

    for (SizeType i = 0; i < triangleCount; ++i)
    {
        const FVector3 faceNormal = TriangleNormal(cube, i);
        const UInt32   i0         = cube.Indices[i * 3 + 0];

        LIMX_EXPECT_TRUE(
            FVector3::Dot(faceNormal, cube.Vertices[i0].Normal) > 0.0f);
    }
}

LIMX_TEST(GeometryWinding, PlaneFaceNormalAgreesWithVertexNormal)
{
    // 平面不是闭合体, 没有"朝外"可言 —— 但面法线仍必须与顶点法线同向,
    // 否则从法线所指的一侧看过去它会被剔除, 表现为"地面从某些角度消失"。
    const FMeshData plane = FGeometryGenerator::GeneratePlane(2.0f, 2.0f, 2, 2);

    const SizeType triangleCount = plane.Indices.GetSize() / 3;

    LIMX_EXPECT_GT(triangleCount, static_cast<SizeType>(0));

    for (SizeType i = 0; i < triangleCount; ++i)
    {
        const FVector3 faceNormal = TriangleNormal(plane, i);
        const UInt32   i0         = plane.Indices[i * 3 + 0];

        LIMX_EXPECT_TRUE(
            FVector3::Dot(faceNormal, plane.Vertices[i0].Normal) > 0.0f);
    }
}

LIMX_TEST(GeometryWinding, AllGeneratorsShareOneConvention)
{
    // 三者必须一致 —— 只要有一个反了, 那一类物体就会在所有场景里
    // 悄悄渲染成内侧。这个用例存在的意义是: 将来新增生成器时,
    // "跟现有的保持一致"这件事有一个地方会替你检查。
    const FMeshData meshes[3] =
    {
        FGeometryGenerator::GenerateCube(),
        FGeometryGenerator::GenerateSphere(1.0f, 12, 6),
        FGeometryGenerator::GeneratePlane(2.0f, 2.0f, 1, 1),
    };

    for (SizeType m = 0; m < 3; ++m)
    {
        const SizeType triangleCount = meshes[m].Indices.GetSize() / 3;

        for (SizeType i = 0; i < triangleCount; ++i)
        {
            const FVector3 faceNormal = TriangleNormal(meshes[m], i);

            // 退化三角形没有朝向可言, 跳过。
            //
            // UV 球在两极各有一圈这样的三角形: 那里整条纬线塌成一个点,
            // 于是每个"四边形"退化成一条线。它们不影响渲染 (光栅化阶段
            // 直接丢弃), 只是白白走一遍顶点着色 —— 低细分下能占到
            // 六分之一的三角形。
            if (faceNormal.LengthSquared() < 1.0e-12f)
            {
                continue;
            }

            const UInt32 i0 = meshes[m].Indices[i * 3 + 0];

            LIMX_EXPECT_TRUE(
                FVector3::Dot(faceNormal, meshes[m].Vertices[i0].Normal) > 0.0f);
        }
    }
}
