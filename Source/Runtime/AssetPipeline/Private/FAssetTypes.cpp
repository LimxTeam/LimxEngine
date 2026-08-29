/*******************************************************************************
 * 文件: FAssetTypes.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   资产中性数据结构的实现 — 包围盒重算、法线与切线生成、场景层级世界变换
 *
 * 设计哲学:
 *   缺失属性由解析层补齐 — OBJ 常常只有位置，glTF 也允许省略法线与切线。
 *   与其让渲染层为每种缺失情况分支，不如在解析结束时一次性补全，
 *   使上传层拿到的永远是属性完整的顶点。
 *
 *   面积加权的平滑法线 — 累加未归一化的面法线，其模长天然正比于三角形面积，
 *   于是大三角形对顶点法线的贡献更大。这比等权平均更接近真实曲面法线，
 *   且不需要额外计算面积。
 *
 * 技术特性:
 *   - 切线生成采用 Lengyel 方法, 以纹理坐标梯度求解切线基
 *   - 退化三角形 (零面积或 UV 重合) 被跳过而非产生 NaN
 *   - 世界变换沿父链自根向下累乘, 顺序与 FTransform::operator* 语义一致
 *
 * 依赖关系:
 *   内部: AssetPipeline/FAssetTypes.h
 *
 * 注意事项:
 *   GenerateTangents 要求已有法线与纹理坐标, 否则结果无意义
 *
 ******************************************************************************/

#include "AssetPipeline/FAssetTypes.h"

namespace Limx
{

namespace
{

/// 顶点属性判等的容差 — 解析产生的坐标通常来自同一份源数据, 容差可以很紧
constexpr Float32 kVertexEpsilon = 1.0e-6f;

FORCEINLINE bool NearlyEqual(Float32 a, Float32 b)
{
    return FMath::Abs(a - b) <= kVertexEpsilon;
}

FORCEINLINE bool NearlyEqual(const FVector2& a, const FVector2& b)
{
    return NearlyEqual(a.X, b.X) && NearlyEqual(a.Y, b.Y);
}

FORCEINLINE bool NearlyEqual(const FVector3& a, const FVector3& b)
{
    return NearlyEqual(a.X, b.X) && NearlyEqual(a.Y, b.Y) &&
           NearlyEqual(a.Z, b.Z);
}

FORCEINLINE bool NearlyEqual(const FVector4& a, const FVector4& b)
{
    return NearlyEqual(a.X, b.X) && NearlyEqual(a.Y, b.Y) &&
           NearlyEqual(a.Z, b.Z) && NearlyEqual(a.W, b.W);
}

} // namespace

// ============================================================================
// FMeshVertex
// ============================================================================

bool FMeshVertex::operator==(const FMeshVertex& other) const
{
    return NearlyEqual(Position, other.Position) &&
           NearlyEqual(Normal, other.Normal) &&
           NearlyEqual(Tangent, other.Tangent) &&
           NearlyEqual(TexCoord0, other.TexCoord0) &&
           NearlyEqual(TexCoord1, other.TexCoord1) &&
           NearlyEqual(Color, other.Color);
}

// ============================================================================
// FMeshData — 包围盒
// ============================================================================

void FMeshData::RecomputeBounds()
{
    Bounds = FBoundingBox();

    if (Vertices.GetSize() == 0)
    {
        return;
    }

    Bounds = FBoundingBox::FromPoint(Vertices[0].Position);

    for (SizeType i = 1; i < Vertices.GetSize(); ++i)
    {
        Bounds = Bounds.ExpandToInclude(Vertices[i].Position);
    }

    // 各子网格只覆盖自己引用到的顶点
    for (SizeType s = 0; s < SubMeshes.GetSize(); ++s)
    {
        FSubMesh& subMesh = SubMeshes[s];

        if (subMesh.IndexCount == 0)
        {
            subMesh.Bounds = FBoundingBox();
            continue;
        }

        bool initialized = false;

        for (UInt32 i = 0; i < subMesh.IndexCount; ++i)
        {
            const SizeType slot =
                static_cast<SizeType>(subMesh.IndexOffset) + i;

            if (slot >= Indices.GetSize())
            {
                break;
            }

            const UInt32 vertexIndex = Indices[slot];
            if (vertexIndex >= Vertices.GetSize())
            {
                continue;
            }

            const FVector3& position = Vertices[vertexIndex].Position;

            if (!initialized)
            {
                subMesh.Bounds = FBoundingBox::FromPoint(position);
                initialized    = true;
            }
            else
            {
                subMesh.Bounds = subMesh.Bounds.ExpandToInclude(position);
            }
        }
    }
}

// ============================================================================
// FMeshData — 法线生成
// ============================================================================

void FMeshData::GenerateNormals()
{
    if (Vertices.GetSize() == 0 || Indices.GetSize() < 3)
    {
        return;
    }

    for (SizeType i = 0; i < Vertices.GetSize(); ++i)
    {
        Vertices[i].Normal = FVector3(0.0f, 0.0f, 0.0f);
    }

    // ------------------------------------------------------------------
    // 累加未归一化的面法线
    //
    // 叉积的模长等于两倍三角形面积，因此直接累加即为面积加权平均，
    // 无需显式计算面积。大三角形对顶点法线的影响更大，更贴近真实曲面。
    // ------------------------------------------------------------------

    const SizeType triangleCount = Indices.GetSize() / 3;

    for (SizeType t = 0; t < triangleCount; ++t)
    {
        const UInt32 i0 = Indices[t * 3 + 0];
        const UInt32 i1 = Indices[t * 3 + 1];
        const UInt32 i2 = Indices[t * 3 + 2];

        if (i0 >= Vertices.GetSize() || i1 >= Vertices.GetSize() ||
            i2 >= Vertices.GetSize())
        {
            continue;
        }

        const FVector3& p0 = Vertices[i0].Position;
        const FVector3& p1 = Vertices[i1].Position;
        const FVector3& p2 = Vertices[i2].Position;

        const FVector3 faceNormal = FVector3::Cross(p1 - p0, p2 - p0);

        Vertices[i0].Normal = Vertices[i0].Normal + faceNormal;
        Vertices[i1].Normal = Vertices[i1].Normal + faceNormal;
        Vertices[i2].Normal = Vertices[i2].Normal + faceNormal;
    }

    for (SizeType i = 0; i < Vertices.GetSize(); ++i)
    {
        FVector3& normal = Vertices[i].Normal;

        if (normal.IsNearlyZero())
        {
            // 孤立顶点或完全退化的邻接三角形 — 给一个确定的方向而非 NaN
            normal = FVector3(0.0f, 1.0f, 0.0f);
        }
        else
        {
            normal = normal.GetSafeNormal();
        }
    }

    HasNormals = true;
}

// ============================================================================
// FMeshData — 切线生成
// ============================================================================

void FMeshData::GenerateTangents()
{
    if (Vertices.GetSize() == 0 || Indices.GetSize() < 3)
    {
        return;
    }

    // 切线求解依赖纹理坐标梯度; 没有 UV 就无从谈起
    if (!HasTexCoords)
    {
        return;
    }

    if (!HasNormals)
    {
        GenerateNormals();
    }

    TArray<FVector3> tangentAccum;
    TArray<FVector3> bitangentAccum;

    tangentAccum.Reserve(Vertices.GetSize());
    bitangentAccum.Reserve(Vertices.GetSize());

    for (SizeType i = 0; i < Vertices.GetSize(); ++i)
    {
        tangentAccum.Add(FVector3(0.0f, 0.0f, 0.0f));
        bitangentAccum.Add(FVector3(0.0f, 0.0f, 0.0f));
    }

    const SizeType triangleCount = Indices.GetSize() / 3;

    for (SizeType t = 0; t < triangleCount; ++t)
    {
        const UInt32 i0 = Indices[t * 3 + 0];
        const UInt32 i1 = Indices[t * 3 + 1];
        const UInt32 i2 = Indices[t * 3 + 2];

        if (i0 >= Vertices.GetSize() || i1 >= Vertices.GetSize() ||
            i2 >= Vertices.GetSize())
        {
            continue;
        }

        const FVector3& p0 = Vertices[i0].Position;
        const FVector3& p1 = Vertices[i1].Position;
        const FVector3& p2 = Vertices[i2].Position;

        const FVector2& uv0 = Vertices[i0].TexCoord0;
        const FVector2& uv1 = Vertices[i1].TexCoord0;
        const FVector2& uv2 = Vertices[i2].TexCoord0;

        const FVector3 edge1 = p1 - p0;
        const FVector3 edge2 = p2 - p0;

        const Float32 deltaU1 = uv1.X - uv0.X;
        const Float32 deltaV1 = uv1.Y - uv0.Y;
        const Float32 deltaU2 = uv2.X - uv0.X;
        const Float32 deltaV2 = uv2.Y - uv0.Y;

        const Float32 determinant = deltaU1 * deltaV2 - deltaU2 * deltaV1;

        // UV 退化 (三点共线或重合) 时无法求解, 跳过该三角形而非产生 NaN
        if (FMath::Abs(determinant) < 1.0e-12f)
        {
            continue;
        }

        const Float32 inverseDet = 1.0f / determinant;

        const FVector3 tangent(
            inverseDet * (deltaV2 * edge1.X - deltaV1 * edge2.X),
            inverseDet * (deltaV2 * edge1.Y - deltaV1 * edge2.Y),
            inverseDet * (deltaV2 * edge1.Z - deltaV1 * edge2.Z));

        const FVector3 bitangent(
            inverseDet * (-deltaU2 * edge1.X + deltaU1 * edge2.X),
            inverseDet * (-deltaU2 * edge1.Y + deltaU1 * edge2.Y),
            inverseDet * (-deltaU2 * edge1.Z + deltaU1 * edge2.Z));

        tangentAccum[i0] = tangentAccum[i0] + tangent;
        tangentAccum[i1] = tangentAccum[i1] + tangent;
        tangentAccum[i2] = tangentAccum[i2] + tangent;

        bitangentAccum[i0] = bitangentAccum[i0] + bitangent;
        bitangentAccum[i1] = bitangentAccum[i1] + bitangent;
        bitangentAccum[i2] = bitangentAccum[i2] + bitangent;
    }

    // ------------------------------------------------------------------
    // Gram-Schmidt 正交化 + 手性判定
    // ------------------------------------------------------------------

    for (SizeType i = 0; i < Vertices.GetSize(); ++i)
    {
        const FVector3& normal    = Vertices[i].Normal;
        const FVector3& tangent   = tangentAccum[i];
        const FVector3& bitangent = bitangentAccum[i];

        if (tangent.IsNearlyZero())
        {
            // 该顶点未被任何有效三角形覆盖 — 构造一个与法线正交的任意切线
            const FVector3 reference =
                (FMath::Abs(normal.X) < 0.9f) ? FVector3(1.0f, 0.0f, 0.0f)
                                              : FVector3(0.0f, 1.0f, 0.0f);

            const FVector3 fallback =
                FVector3::Cross(normal, reference).GetSafeNormal();

            Vertices[i].Tangent =
                FVector4(fallback.X, fallback.Y, fallback.Z, 1.0f);
            continue;
        }

        // 去掉切线中与法线平行的分量
        const FVector3 orthogonal =
            (tangent - normal * FVector3::Dot(normal, tangent)).GetSafeNormal();

        // w 记录副切线的手性, 供着色器重建 TBN
        const Float32 handedness =
            (FVector3::Dot(FVector3::Cross(normal, tangent), bitangent) < 0.0f)
                ? -1.0f
                : 1.0f;

        Vertices[i].Tangent = FVector4(orthogonal.X, orthogonal.Y,
                                       orthogonal.Z, handedness);
    }

    HasTangents = true;
}

// ============================================================================
// FAssetScene
// ============================================================================

SizeType FAssetScene::GetTotalVertexCount() const
{
    SizeType total = 0;

    for (SizeType i = 0; i < Meshes.GetSize(); ++i)
    {
        total += Meshes[i].GetVertexCount();
    }

    return total;
}

SizeType FAssetScene::GetTotalTriangleCount() const
{
    SizeType total = 0;

    for (SizeType i = 0; i < Meshes.GetSize(); ++i)
    {
        total += Meshes[i].GetTriangleCount();
    }

    return total;
}

SizeType FAssetScene::GetTotalSubMeshCount() const
{
    SizeType total = 0;

    for (SizeType i = 0; i < Meshes.GetSize(); ++i)
    {
        total += Meshes[i].SubMeshes.GetSize();
    }

    return total;
}

FTransform FAssetScene::ComputeWorldTransform(Int32 nodeIndex) const
{
    if (nodeIndex < 0 ||
        static_cast<SizeType>(nodeIndex) >= Nodes.GetSize())
    {
        return FTransform();
    }

    // ------------------------------------------------------------------
    // 先沿父链回溯收集，再自根向下累乘
    //
    // FTransform::operator* 的语义是 result = this * other 表示"先 other
    // 再 this"，因此必须按 根 → 叶 的顺序组合，回溯得到的顺序需要反向使用。
    // ------------------------------------------------------------------

    TArray<Int32> chain;

    Int32 cursor = nodeIndex;
    UInt32 guard = 0;

    while (cursor >= 0 && static_cast<SizeType>(cursor) < Nodes.GetSize())
    {
        chain.Add(cursor);
        cursor = Nodes[cursor].ParentIndex;

        // 父链成环时的保护 — 损坏的资产不应导致死循环
        if (++guard > Nodes.GetSize() + 1)
        {
            break;
        }
    }

    FTransform world;

    for (SizeType i = chain.GetSize(); i > 0; --i)
    {
        const Int32 index = chain[i - 1];
        world = world * Nodes[index].LocalTransform;
    }

    return world;
}

void FAssetScene::RecomputeBounds()
{
    Bounds = FBoundingBox();

    bool initialized = false;

    for (SizeType i = 0; i < Nodes.GetSize(); ++i)
    {
        const FSceneNode& node = Nodes[i];

        if (node.MeshIndex < 0 ||
            static_cast<SizeType>(node.MeshIndex) >= Meshes.GetSize())
        {
            continue;
        }

        const FMeshData& mesh = Meshes[node.MeshIndex];
        if (!mesh.Bounds.IsValid())
        {
            continue;
        }

        const FTransform world = ComputeWorldTransform(static_cast<Int32>(i));

        // 变换包围盒的八个角点后重新求界 —— 直接变换 Min/Max 在存在旋转时会算错
        const FVector3 min = mesh.Bounds.Min;
        const FVector3 max = mesh.Bounds.Max;

        for (Int32 corner = 0; corner < 8; ++corner)
        {
            const FVector3 localCorner(
                (corner & 1) ? max.X : min.X,
                (corner & 2) ? max.Y : min.Y,
                (corner & 4) ? max.Z : min.Z);

            const FVector3 worldCorner = world.TransformPosition(localCorner);

            if (!initialized)
            {
                Bounds      = FBoundingBox::FromPoint(worldCorner);
                initialized = true;
            }
            else
            {
                Bounds = Bounds.ExpandToInclude(worldCorner);
            }
        }
    }
}

void FAssetScene::Reset()
{
    Name = FName();
    BaseDirectory.Clear();

    Meshes.Clear();
    Materials.Clear();
    EmbeddedImages.Clear();
    Nodes.Clear();
    RootNodes.Clear();

    Bounds = FBoundingBox();
}

} // namespace Limx
