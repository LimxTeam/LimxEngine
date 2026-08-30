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
// 程序化几何的顶点描述
//
// 中性顶点 FMeshVertex 有六个属性 (位置/法线/切线/双 UV/颜色)，但程序化
// 几何只关心其中四个。用一个紧凑描述结构承载字面量表，保持表格可读，
// 再统一转换为中性顶点并补齐其余属性 —— 直接以中性顶点写字面量会让
// 每行多出两组占位值，表格将难以核对。
// ============================================================================

namespace
{

/// 字面量表中的顶点描述
struct FProceduralVertex
{
    Float32 Position[3];
    Float32 Normal[3];
    Float32 Color[3];
    Float32 TexCoord[2];
};

/// 转换为中性顶点 — 切线留待 GenerateTangents 统一生成
FMeshVertex ToMeshVertex(const FProceduralVertex& source)
{
    FMeshVertex vertex;

    vertex.Position  = FVector3(source.Position[0], source.Position[1],
                                source.Position[2]);
    vertex.Normal    = FVector3(source.Normal[0], source.Normal[1],
                                source.Normal[2]);
    vertex.TexCoord0 = FVector2(source.TexCoord[0], source.TexCoord[1]);
    vertex.TexCoord1 = FVector2(0.0f, 0.0f);
    vertex.Color     = FVector4(source.Color[0], source.Color[1],
                                source.Color[2], 1.0f);

    return vertex;
}

/// 补齐包围盒与切线, 使程序化几何与解析所得的资产具有同样完整的属性
void FinalizeMesh(FMeshData& mesh, const FName& name)
{
    mesh.Name         = name;
    mesh.HasNormals   = true;
    mesh.HasTexCoords = true;

    if (mesh.SubMeshes.GetSize() == 0)
    {
        FSubMesh section;
        section.Name        = name;
        section.IndexOffset = 0;
        section.IndexCount  = static_cast<UInt32>(mesh.Indices.GetSize());
        mesh.SubMeshes.Add(section);
    }

    mesh.RecomputeBounds();
    mesh.GenerateTangents();
}

} // namespace

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

    FProceduralVertex vertices[] =
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
        mesh.Vertices.Add(ToMeshVertex(vertices[i]));
    }

    mesh.Indices.Reserve(indexCount);
    for (UInt32 i = 0; i < indexCount; ++i)
    {
        mesh.Indices.Add(static_cast<UInt32>(indices[i]));
    }

    FinalizeMesh(mesh, FName("Cube"));

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
                ? FVector4(0.7f, 0.7f, 0.7f, 1.0f)
                : FVector4(0.3f, 0.3f, 0.3f, 1.0f);

            // UV 坐标: [0,1] 映射到平面范围
            vertex.TexCoord0 = FVector2(
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

    FinalizeMesh(mesh, FName("Plane"));

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

    // 两极那两圈各只有一个三角形, 中间每圈两个 —— 见下面索引生成处的说明。
    // stacks == 1 时球退化成两个极点, 一个三角形都发不出来, 这里正好为 0。
    UInt32 indexCount = (stacks >= 2) ? (slices * (stacks * 2 - 2) * 3) : 0;
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
            vertex.Color = FVector4(
                0.5f + 0.5f * FMath::Cos(u * FMath::kPi * 2.0f),
                0.5f + 0.5f * FMath::Cos(v * FMath::kPi),
                0.5f + 0.5f * FMath::Sin(u * FMath::kPi * 2.0f),
                1.0f
            );

            // UV 坐标: 经度→U, 纬度→V
            vertex.TexCoord0 = FVector2(u, v);

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

            // 缠绕方向必须与立方体、平面以及导入的 glTF/OBJ 一致:
            // 从**球外**看去为逆时针 (右手定则的面法线朝外)。
            //
            // 原先写的是 (current, below, next), 叉积指向球心内侧 —— 于是
            // 背面剔除把朝向相机的那半球全部剔掉, 画面上留下的是远侧半球,
            // 法线正好背对相机。
            //
            // 这个错误极难看出来: 球的轮廓一模一样, 只有着色不对。而漫反射
            // 加上一盏主光, 远侧半球看着也像是"光从另一边来"。它是被白炉
            // 测试抓出来的 —— 那是第一次有已知真值可以拿来核对法线。

            // 两极处整行顶点坍缩到同一点 (sinPhi 为零), 因此那两圈各有
            // 一个三角形是零面积的, 必须跳过:
            //
            //   iStack == 0        current 与 next 同为北极点
            //   iStack == stacks-1 below 与 belowNext 同为南极点
            //
            // 零面积三角形画不出任何东西, 但照样占索引、照样过顶点着色器,
            // 而且叉积为零向量 —— 凡是按面法线做判断的地方 (背面剔除的
            // 调试核对、切线求解、法线重建) 都得为它们特判。少发比后面
            // 处处防守划算。
            if (iStack > 0)
            {
                mesh.Indices.Add(current);
                mesh.Indices.Add(next);
                mesh.Indices.Add(below);
            }

            if (iStack + 1 < stacks)
            {
                mesh.Indices.Add(below);
                mesh.Indices.Add(next);
                mesh.Indices.Add(belowNext);
            }
        }
    }

    FinalizeMesh(mesh, FName("Sphere"));

    return mesh;
}

} // namespace Limx
