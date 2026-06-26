// ============================================================
// 文件名称：FGeometryGenerator.cpp
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：纯数据生成器 — 输出 CPU 端顶点/索引数组。
// 功能描述：程序化基础几何体生成实现 — 立方体、平面、UV 球体。
// 技术特性：全部 static 方法，无状态；
//          顶点布局: 位置(vec3) + 法线(vec3) + 颜色(vec3) + UV(vec2)；
//          索引使用 UInt16；FMath 数学函数。
//
// ── 函数表 ──────────────────────────────────────────────────
// │ 函数名                      │ 描述                           │
// │──────────────────────────────│───────────────────────────────│
// │ GenerateCube()              │ 单位立方体 24顶点 36索引         │
// │ GeneratePlane()             │ XZ 平面 (subdivX+1)*(subdivZ+1) │
// │ GenerateSphere()            │ UV 球体 经纬细分                │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建                        │
// │ 2026-04-07  │ LimxTeam  │ M0.3 添加 UV 坐标支持纹理采样   │
// ============================================================

#include "RenderCore/Geometry/FGeometryGenerator.h"

namespace Limx
{

// ============================================================================
// GenerateCube — 单位立方体 (中心原点, 边长 1.0)
// ============================================================================

FMeshData FGeometryGenerator::GenerateCube()
{
    FMeshData mesh;

    // 半边长
    constexpr Float32 h = 0.5f;

    // 每面 4 顶点, 6 面 = 24 顶点
    // 每面独立法线, 颜色按面区分
    // 顺序: +X, -X, +Y, -Y, +Z, -Z

    FMeshVertex vertices[] =
    {
        // +X 面 (右) — 红色           pos              normal          color                   uv
        { { h, -h, -h}, { 1,  0,  0}, {0.9f, 0.2f, 0.2f}, {0.0f, 1.0f} },
        { { h,  h, -h}, { 1,  0,  0}, {0.9f, 0.2f, 0.2f}, {0.0f, 0.0f} },
        { { h,  h,  h}, { 1,  0,  0}, {0.9f, 0.2f, 0.2f}, {1.0f, 0.0f} },
        { { h, -h,  h}, { 1,  0,  0}, {0.9f, 0.2f, 0.2f}, {1.0f, 1.0f} },

        // -X 面 (左) — 青色
        { {-h, -h,  h}, {-1,  0,  0}, {0.2f, 0.8f, 0.8f}, {0.0f, 1.0f} },
        { {-h,  h,  h}, {-1,  0,  0}, {0.2f, 0.8f, 0.8f}, {0.0f, 0.0f} },
        { {-h,  h, -h}, {-1,  0,  0}, {0.2f, 0.8f, 0.8f}, {1.0f, 0.0f} },
        { {-h, -h, -h}, {-1,  0,  0}, {0.2f, 0.8f, 0.8f}, {1.0f, 1.0f} },

        // +Y 面 (上) — 绿色
        { {-h,  h, -h}, { 0,  1,  0}, {0.2f, 0.9f, 0.3f}, {0.0f, 1.0f} },
        { {-h,  h,  h}, { 0,  1,  0}, {0.2f, 0.9f, 0.3f}, {0.0f, 0.0f} },
        { { h,  h,  h}, { 0,  1,  0}, {0.2f, 0.9f, 0.3f}, {1.0f, 0.0f} },
        { { h,  h, -h}, { 0,  1,  0}, {0.2f, 0.9f, 0.3f}, {1.0f, 1.0f} },

        // -Y 面 (下) — 品红色
        { {-h, -h,  h}, { 0, -1,  0}, {0.8f, 0.2f, 0.8f}, {0.0f, 1.0f} },
        { {-h, -h, -h}, { 0, -1,  0}, {0.8f, 0.2f, 0.8f}, {0.0f, 0.0f} },
        { { h, -h, -h}, { 0, -1,  0}, {0.8f, 0.2f, 0.8f}, {1.0f, 0.0f} },
        { { h, -h,  h}, { 0, -1,  0}, {0.8f, 0.2f, 0.8f}, {1.0f, 1.0f} },

        // +Z 面 (前) — 蓝色
        { {-h, -h,  h}, { 0,  0,  1}, {0.2f, 0.3f, 0.9f}, {0.0f, 1.0f} },
        { { h, -h,  h}, { 0,  0,  1}, {0.2f, 0.3f, 0.9f}, {1.0f, 1.0f} },
        { { h,  h,  h}, { 0,  0,  1}, {0.2f, 0.3f, 0.9f}, {1.0f, 0.0f} },
        { {-h,  h,  h}, { 0,  0,  1}, {0.2f, 0.3f, 0.9f}, {0.0f, 0.0f} },

        // -Z 面 (后) — 黄色
        { { h, -h, -h}, { 0,  0, -1}, {0.9f, 0.9f, 0.2f}, {0.0f, 1.0f} },
        { {-h, -h, -h}, { 0,  0, -1}, {0.9f, 0.9f, 0.2f}, {1.0f, 1.0f} },
        { {-h,  h, -h}, { 0,  0, -1}, {0.9f, 0.9f, 0.2f}, {1.0f, 0.0f} },
        { { h,  h, -h}, { 0,  0, -1}, {0.9f, 0.9f, 0.2f}, {0.0f, 0.0f} },
    };

    // 索引: 每面 2 个三角形, 6 面 = 36 索引
    UInt16 indices[] =
    {
         0,  1,  2,   0,  2,  3,   // +X
         4,  5,  6,   4,  6,  7,   // -X
         8,  9, 10,   8, 10, 11,   // +Y
        12, 13, 14,  12, 14, 15,   // -Y
        16, 17, 18,  16, 18, 19,   // +Z
        20, 21, 22,  20, 22, 23,   // -Z
    };

    constexpr UInt32 vertexCount = static_cast<UInt32>(
        LIMX_ARRAY_COUNT(vertices));
    constexpr UInt32 indexCount  = static_cast<UInt32>(
        LIMX_ARRAY_COUNT(indices));

    mesh.Vertices.Reserve(vertexCount);
    for (UInt32 i = 0; i < vertexCount; ++i)
    {
        mesh.Vertices.Add(vertices[i]);
    }

    mesh.Indices.Reserve(indexCount);
    for (UInt32 i = 0; i < indexCount; ++i)
    {
        mesh.Indices.Add(indices[i]);
    }

    return mesh;
}

// ============================================================================
// GeneratePlane — XZ 平面 (中心原点, Y=0)
// ============================================================================

FMeshData FGeometryGenerator::GeneratePlane(
    Float32 width, Float32 depth,
    UInt32 subdivX, UInt32 subdivZ)
{
    FMeshData mesh;

    UInt32 vertsX = subdivX + 1;
    UInt32 vertsZ = subdivZ + 1;
    UInt32 vertexCount = vertsX * vertsZ;
    UInt32 indexCount  = subdivX * subdivZ * 6;

    mesh.Vertices.Reserve(vertexCount);
    mesh.Indices.Reserve(indexCount);

    Float32 halfW = width * 0.5f;
    Float32 halfD = depth * 0.5f;
    Float32 stepX = width / static_cast<Float32>(subdivX);
    Float32 stepZ = depth / static_cast<Float32>(subdivZ);

    // 生成顶点
    for (UInt32 iz = 0; iz < vertsZ; ++iz)
    {
        Float32 z = -halfD + static_cast<Float32>(iz) * stepZ;
        for (UInt32 ix = 0; ix < vertsX; ++ix)
        {
            Float32 x = -halfW + static_cast<Float32>(ix) * stepX;

            FMeshVertex vertex;
            vertex.Position = FVector3(x, 0.0f, z);
            vertex.Normal   = FVector3(0.0f, 1.0f, 0.0f);
            // 棋盘格颜色
            bool isEven = ((ix + iz) % 2) == 0;
            vertex.Color = isEven
                ? FVector3(0.7f, 0.7f, 0.7f)
                : FVector3(0.3f, 0.3f, 0.3f);

            // UV 坐标: [0,1] 映射到平面范围
            vertex.TexCoord = FVector2(
                static_cast<Float32>(ix) / static_cast<Float32>(subdivX),
                static_cast<Float32>(iz) / static_cast<Float32>(subdivZ));

            mesh.Vertices.Add(vertex);
        }
    }

    // 生成索引
    for (UInt32 iz = 0; iz < subdivZ; ++iz)
    {
        for (UInt32 ix = 0; ix < subdivX; ++ix)
        {
            UInt16 topLeft     = static_cast<UInt16>(iz * vertsX + ix);
            UInt16 topRight    = static_cast<UInt16>(topLeft + 1);
            UInt16 bottomLeft  = static_cast<UInt16>((iz + 1) * vertsX + ix);
            UInt16 bottomRight = static_cast<UInt16>(bottomLeft + 1);

            // 三角形 1
            mesh.Indices.Add(topLeft);
            mesh.Indices.Add(bottomLeft);
            mesh.Indices.Add(topRight);

            // 三角形 2
            mesh.Indices.Add(topRight);
            mesh.Indices.Add(bottomLeft);
            mesh.Indices.Add(bottomRight);
        }
    }

    return mesh;
}

// ============================================================================
// GenerateSphere — UV 球体 (中心原点)
// ============================================================================

FMeshData FGeometryGenerator::GenerateSphere(
    Float32 radius, UInt32 slices, UInt32 stacks)
{
    FMeshData mesh;

    UInt32 vertexCount = (slices + 1) * (stacks + 1);
    UInt32 indexCount   = slices * stacks * 6;
    mesh.Vertices.Reserve(vertexCount);
    mesh.Indices.Reserve(indexCount);

    // 生成顶点
    for (UInt32 iStack = 0; iStack <= stacks; ++iStack)
    {
        Float32 phi = FMath::kPi
                    * static_cast<Float32>(iStack)
                    / static_cast<Float32>(stacks);
        Float32 sinPhi, cosPhi;
        FMath::SinCos(phi, sinPhi, cosPhi);

        for (UInt32 iSlice = 0; iSlice <= slices; ++iSlice)
        {
            Float32 theta = 2.0f * FMath::kPi
                          * static_cast<Float32>(iSlice)
                          / static_cast<Float32>(slices);
            Float32 sinTheta, cosTheta;
            FMath::SinCos(theta, sinTheta, cosTheta);

            FVector3 normal(
                sinPhi * cosTheta,
                cosPhi,
                sinPhi * sinTheta
            );

            FMeshVertex vertex;
            vertex.Position = normal * radius;
            vertex.Normal   = normal;
            // 颜色: 经纬度映射到色相
            Float32 u = static_cast<Float32>(iSlice)
                      / static_cast<Float32>(slices);
            Float32 v = static_cast<Float32>(iStack)
                      / static_cast<Float32>(stacks);
            vertex.Color = FVector3(
                0.5f + 0.5f * FMath::Cos(u * FMath::kPi * 2.0f),
                0.5f + 0.5f * FMath::Cos(v * FMath::kPi),
                0.5f + 0.5f * FMath::Sin(u * FMath::kPi * 2.0f)
            );

            // UV 坐标: 经度→U, 纬度→V
            vertex.TexCoord = FVector2(u, v);

            mesh.Vertices.Add(vertex);
        }
    }

    // 生成索引
    for (UInt32 iStack = 0; iStack < stacks; ++iStack)
    {
        for (UInt32 iSlice = 0; iSlice < slices; ++iSlice)
        {
            UInt16 current = static_cast<UInt16>(
                iStack * (slices + 1) + iSlice);
            UInt16 next    = static_cast<UInt16>(current + 1);
            UInt16 below   = static_cast<UInt16>(
                (iStack + 1) * (slices + 1) + iSlice);
            UInt16 belowNext = static_cast<UInt16>(below + 1);

            mesh.Indices.Add(current);
            mesh.Indices.Add(below);
            mesh.Indices.Add(next);

            mesh.Indices.Add(next);
            mesh.Indices.Add(below);
            mesh.Indices.Add(belowNext);
        }
    }

    return mesh;
}

} // namespace Limx
