/*******************************************************************************
 * 文件: ObjLoaderTests.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   OBJ / MTL 解析器单元测试 — 几何解析、顶点去重、索引解析、三角化、
 *   材质切分、Phong 到 PBR 换算、属性补齐与畸形输入容错
 *
 * 设计哲学:
 *   顶点去重必须被量化验证 — 一个立方体用 8 个位置和 6 个法线描述，
 *   若去重键只用位置会得到 8 个顶点（法线错乱），完全不去重会得到 24 个。
 *   用例断言确切的顶点数，任何一种错误都会立即体现为数字不符。
 *
 *   索引语义是最易错处 — OBJ 索引从 1 起始且支持负数相对寻址，
 *   两者都容易写成差一错误。用例对正索引、负索引、越界索引分别覆盖。
 *
 *   容错要验证"跳过了什么" — 断言解析成功不足以说明容错正确，
 *   还需断言畸形行确实被跳过（几何数量正确）且产生了告警（问题可见）。
 *
 * 技术特性:
 *   - 全部测试数据内嵌为字符串字面量, 不依赖外部资产文件
 *   - 几何断言精确到顶点数、索引数、三角形数与具体坐标
 *   - 材质换算验证 Ns 到粗糙度的映射与 PBR 扩展字段的优先级
 *
 * 依赖关系:
 *   内部: AssetTests/AssetTestsMinimal.h
 *
 * 注意事项:
 *   涉及 mtllib 的用例不测试文件读取路径 — 材质库单独用
 *   ParseMaterialLibrary 直接从内存解析
 *
 ******************************************************************************/

#include "AssetTests/AssetTestsMinimal.h"

using namespace Limx;

namespace
{

constexpr Float32 kTolerance = 1.0e-5f;

/// 从内存解析 OBJ 的便捷包装
FAssetLoadResult ParseObj(const AnsiChar* text, FAssetScene& outScene,
                          const FObjLoadOptions& options = FObjLoadOptions())
{
    SizeType length = 0;
    while (text[length] != '\0')
    {
        ++length;
    }

    return FObjLoader::LoadFromMemory(text, length, FString(),
                                      outScene, options);
}

/// 不翻转 V 轴的选项 — 便于用例直接比对源文件中的 UV 值
FObjLoadOptions RawUvOptions()
{
    FObjLoadOptions options;
    options.FlipTexCoordV = false;
    return options;
}

} // namespace

// ============================================================================
// 基础几何
// ============================================================================

LIMX_TEST(ObjLoader, ParsesSingleTriangle)
{
    const AnsiChar* source =
        "v 0.0 0.0 0.0\n"
        "v 1.0 0.0 0.0\n"
        "v 0.0 1.0 0.0\n"
        "f 1 2 3\n";

    FAssetScene scene;
    const FAssetLoadResult result = ParseObj(source, scene);

    LIMX_REQUIRE_TRUE(result.Succeeded);
    LIMX_REQUIRE_EQ(scene.Meshes.GetSize(), SizeType(1));

    const FMeshData& mesh = scene.Meshes[0];

    LIMX_EXPECT_EQ(mesh.GetVertexCount(), SizeType(3));
    LIMX_EXPECT_EQ(mesh.GetIndexCount(), SizeType(3));
    LIMX_EXPECT_EQ(mesh.GetTriangleCount(), SizeType(1));

    LIMX_EXPECT_NEAR(mesh.Vertices[0].Position.X, 0.0f, kTolerance);
    LIMX_EXPECT_NEAR(mesh.Vertices[1].Position.X, 1.0f, kTolerance);
    LIMX_EXPECT_NEAR(mesh.Vertices[2].Position.Y, 1.0f, kTolerance);
}

LIMX_TEST(ObjLoader, EmptyInputFails)
{
    FAssetScene scene;

    const FAssetLoadResult result = ParseObj("", scene);
    LIMX_EXPECT_FALSE(result.Succeeded);
    LIMX_EXPECT_FALSE(result.ErrorMessage.IsEmpty());
}

LIMX_TEST(ObjLoader, GeometryWithoutFacesFails)
{
    // 只有顶点没有面 — 不构成可渲染的几何
    const AnsiChar* source =
        "v 0.0 0.0 0.0\n"
        "v 1.0 0.0 0.0\n";

    FAssetScene scene;
    LIMX_EXPECT_FALSE(ParseObj(source, scene).Succeeded);
}

LIMX_TEST(ObjLoader, SkipsCommentsAndBlankLines)
{
    const AnsiChar* source =
        "# 这是注释\n"
        "\n"
        "v 0.0 0.0 0.0\n"
        "   \n"
        "# 又一条注释\n"
        "v 1.0 0.0 0.0\n"
        "v 0.0 1.0 0.0\n"
        "f 1 2 3\n";

    FAssetScene scene;
    LIMX_REQUIRE_TRUE(ParseObj(source, scene).Succeeded);
    LIMX_EXPECT_EQ(scene.Meshes[0].GetVertexCount(), SizeType(3));
}

// ============================================================================
// 索引语义
// ============================================================================

LIMX_TEST(ObjLoader, ResolvesOneBasedIndices)
{
    const AnsiChar* source =
        "v 10.0 0.0 0.0\n"
        "v 20.0 0.0 0.0\n"
        "v 30.0 0.0 0.0\n"
        "f 1 2 3\n";

    FAssetScene scene;
    LIMX_REQUIRE_TRUE(ParseObj(source, scene).Succeeded);

    const FMeshData& mesh = scene.Meshes[0];

    // 索引 1 应对应第一个顶点 — 差一错误会让这里读到 20.0
    LIMX_EXPECT_NEAR(mesh.Vertices[mesh.Indices[0]].Position.X, 10.0f, kTolerance);
    LIMX_EXPECT_NEAR(mesh.Vertices[mesh.Indices[2]].Position.X, 30.0f, kTolerance);
}

LIMX_TEST(ObjLoader, ResolvesNegativeIndices)
{
    const AnsiChar* source =
        "v 10.0 0.0 0.0\n"
        "v 20.0 0.0 0.0\n"
        "v 30.0 0.0 0.0\n"
        "f -3 -2 -1\n";

    FAssetScene scene;
    LIMX_REQUIRE_TRUE(ParseObj(source, scene).Succeeded);

    const FMeshData& mesh = scene.Meshes[0];
    LIMX_REQUIRE_EQ(mesh.GetIndexCount(), SizeType(3));

    // -1 指向最后一个顶点, -3 指向倒数第三个
    LIMX_EXPECT_NEAR(mesh.Vertices[mesh.Indices[0]].Position.X, 10.0f, kTolerance);
    LIMX_EXPECT_NEAR(mesh.Vertices[mesh.Indices[2]].Position.X, 30.0f, kTolerance);
}

LIMX_TEST(ObjLoader, SkipsOutOfRangeIndices)
{
    const AnsiChar* source =
        "v 0.0 0.0 0.0\n"
        "v 1.0 0.0 0.0\n"
        "v 0.0 1.0 0.0\n"
        "f 1 2 99\n"       // 越界 — 整个面因不足三顶点被跳过
        "f 1 2 3\n";

    FAssetScene scene;
    const FAssetLoadResult result = ParseObj(source, scene);

    LIMX_REQUIRE_TRUE(result.Succeeded);

    // 只有合法的那个面被保留
    LIMX_EXPECT_EQ(scene.Meshes[0].GetTriangleCount(), SizeType(1));

    // 越界必须留下告警而非被静默吞掉
    LIMX_EXPECT_GT(result.Warnings.GetSize(), SizeType(0));
}

LIMX_TEST(ObjLoader, ParsesFaceWithoutTexCoord)
{
    // v//vn 形式: 有法线无 UV
    const AnsiChar* source =
        "v 0.0 0.0 0.0\n"
        "v 1.0 0.0 0.0\n"
        "v 0.0 1.0 0.0\n"
        "vn 0.0 0.0 1.0\n"
        "f 1//1 2//1 3//1\n";

    FAssetScene scene;
    LIMX_REQUIRE_TRUE(ParseObj(source, scene).Succeeded);

    const FMeshData& mesh = scene.Meshes[0];

    LIMX_EXPECT_TRUE(mesh.HasNormals);
    LIMX_EXPECT_FALSE(mesh.HasTexCoords);
    LIMX_EXPECT_NEAR(mesh.Vertices[0].Normal.Z, 1.0f, kTolerance);
}

LIMX_TEST(ObjLoader, ParsesFullVertexTriples)
{
    const AnsiChar* source =
        "v 0.0 0.0 0.0\n"
        "v 1.0 0.0 0.0\n"
        "v 0.0 1.0 0.0\n"
        "vt 0.0 0.0\n"
        "vt 1.0 0.0\n"
        "vt 0.0 1.0\n"
        "vn 0.0 0.0 1.0\n"
        "f 1/1/1 2/2/1 3/3/1\n";

    FAssetScene scene;
    LIMX_REQUIRE_TRUE(ParseObj(source, scene, RawUvOptions()).Succeeded);

    const FMeshData& mesh = scene.Meshes[0];

    LIMX_EXPECT_TRUE(mesh.HasNormals);
    LIMX_EXPECT_TRUE(mesh.HasTexCoords);
    LIMX_EXPECT_NEAR(mesh.Vertices[1].TexCoord0.X, 1.0f, kTolerance);
    LIMX_EXPECT_NEAR(mesh.Vertices[2].TexCoord0.Y, 1.0f, kTolerance);
}

// ============================================================================
// 顶点去重
// ============================================================================

LIMX_TEST(ObjLoader, DeduplicatesIdenticalVertexTriples)
{
    // 两个三角形共享一条边 — 共享的两个顶点属性完全相同, 应折叠
    const AnsiChar* source =
        "v 0.0 0.0 0.0\n"
        "v 1.0 0.0 0.0\n"
        "v 1.0 1.0 0.0\n"
        "v 0.0 1.0 0.0\n"
        "f 1 2 3\n"
        "f 1 3 4\n";

    FAssetScene scene;
    LIMX_REQUIRE_TRUE(ParseObj(source, scene).Succeeded);

    const FMeshData& mesh = scene.Meshes[0];

    // 四个位置各出现一次 — 不去重会得到 6 个顶点
    LIMX_EXPECT_EQ(mesh.GetVertexCount(), SizeType(4));
    LIMX_EXPECT_EQ(mesh.GetIndexCount(), SizeType(6));
    LIMX_EXPECT_EQ(mesh.GetTriangleCount(), SizeType(2));
}

LIMX_TEST(ObjLoader, KeepsSamePositionWithDifferentNormals)
{
    // 同一位置配不同法线 — 必须保留为两个顶点, 否则法线会互相覆盖
    const AnsiChar* source =
        "v 0.0 0.0 0.0\n"
        "v 1.0 0.0 0.0\n"
        "v 0.0 1.0 0.0\n"
        "vn 0.0 0.0 1.0\n"
        "vn 1.0 0.0 0.0\n"
        "f 1//1 2//1 3//1\n"
        "f 1//2 2//2 3//2\n";

    FAssetScene scene;
    LIMX_REQUIRE_TRUE(ParseObj(source, scene).Succeeded);

    const FMeshData& mesh = scene.Meshes[0];

    // 3 个位置 × 2 组法线 = 6 个唯一顶点
    LIMX_EXPECT_EQ(mesh.GetVertexCount(), SizeType(6));
    LIMX_EXPECT_EQ(mesh.GetTriangleCount(), SizeType(2));
}

LIMX_TEST(ObjLoader, CubeProducesExpectedVertexCount)
{
    // 立方体: 8 个位置, 6 个面法线; 每个角点被 3 个面共享且法线各异,
    // 因此唯一顶点数为 8 × 3 = 24
    const AnsiChar* source =
        "v -1 -1 -1\nv  1 -1 -1\nv  1  1 -1\nv -1  1 -1\n"
        "v -1 -1  1\nv  1 -1  1\nv  1  1  1\nv -1  1  1\n"
        "vn  0  0 -1\nvn  0  0  1\nvn -1  0  0\n"
        "vn  1  0  0\nvn  0 -1  0\nvn  0  1  0\n"
        "f 1//1 3//1 2//1\nf 1//1 4//1 3//1\n"
        "f 5//2 6//2 7//2\nf 5//2 7//2 8//2\n"
        "f 1//3 5//3 8//3\nf 1//3 8//3 4//3\n"
        "f 2//4 3//4 7//4\nf 2//4 7//4 6//4\n"
        "f 1//5 2//5 6//5\nf 1//5 6//5 5//5\n"
        "f 4//6 8//6 7//6\nf 4//6 7//6 3//6\n";

    FAssetScene scene;
    LIMX_REQUIRE_TRUE(ParseObj(source, scene).Succeeded);

    const FMeshData& mesh = scene.Meshes[0];

    LIMX_EXPECT_EQ(mesh.GetVertexCount(), SizeType(24));
    LIMX_EXPECT_EQ(mesh.GetTriangleCount(), SizeType(12));
    LIMX_EXPECT_EQ(mesh.GetIndexCount(), SizeType(36));
}

// ============================================================================
// 三角化
// ============================================================================

LIMX_TEST(ObjLoader, TriangulatesQuad)
{
    const AnsiChar* source =
        "v 0.0 0.0 0.0\n"
        "v 1.0 0.0 0.0\n"
        "v 1.0 1.0 0.0\n"
        "v 0.0 1.0 0.0\n"
        "f 1 2 3 4\n";

    FAssetScene scene;
    LIMX_REQUIRE_TRUE(ParseObj(source, scene).Succeeded);

    const FMeshData& mesh = scene.Meshes[0];

    // 四边形扇形三角化产出 2 个三角形
    LIMX_EXPECT_EQ(mesh.GetVertexCount(), SizeType(4));
    LIMX_EXPECT_EQ(mesh.GetTriangleCount(), SizeType(2));
}

LIMX_TEST(ObjLoader, TriangulatesHexagon)
{
    const AnsiChar* source =
        "v 0.0 0.0 0.0\n"
        "v 1.0 0.0 0.0\n"
        "v 1.5 1.0 0.0\n"
        "v 1.0 2.0 0.0\n"
        "v 0.0 2.0 0.0\n"
        "v -0.5 1.0 0.0\n"
        "f 1 2 3 4 5 6\n";

    FAssetScene scene;
    LIMX_REQUIRE_TRUE(ParseObj(source, scene).Succeeded);

    // n 边形产出 n-2 个三角形
    LIMX_EXPECT_EQ(scene.Meshes[0].GetTriangleCount(), SizeType(4));
}

LIMX_TEST(ObjLoader, SkipsDegenerateFace)
{
    const AnsiChar* source =
        "v 0.0 0.0 0.0\n"
        "v 1.0 0.0 0.0\n"
        "v 0.0 1.0 0.0\n"
        "f 1 2\n"          // 只有两个顶点, 构不成三角形
        "f 1 2 3\n";

    FAssetScene scene;
    const FAssetLoadResult result = ParseObj(source, scene);

    LIMX_REQUIRE_TRUE(result.Succeeded);
    LIMX_EXPECT_EQ(scene.Meshes[0].GetTriangleCount(), SizeType(1));
    LIMX_EXPECT_GT(result.Warnings.GetSize(), SizeType(0));
}

// ============================================================================
// 纹理坐标
// ============================================================================

LIMX_TEST(ObjLoader, FlipsTexCoordVByDefault)
{
    const AnsiChar* source =
        "v 0.0 0.0 0.0\nv 1.0 0.0 0.0\nv 0.0 1.0 0.0\n"
        "vt 0.25 0.75\n"
        "f 1/1 2/1 3/1\n";

    FAssetScene flipped;
    LIMX_REQUIRE_TRUE(ParseObj(source, flipped).Succeeded);

    // OBJ 的 UV 原点在左下, Vulkan 图像原点在左上 — 默认翻转 V
    LIMX_EXPECT_NEAR(flipped.Meshes[0].Vertices[0].TexCoord0.X, 0.25f, kTolerance);
    LIMX_EXPECT_NEAR(flipped.Meshes[0].Vertices[0].TexCoord0.Y, 0.25f, kTolerance);

    FAssetScene raw;
    LIMX_REQUIRE_TRUE(ParseObj(source, raw, RawUvOptions()).Succeeded);

    LIMX_EXPECT_NEAR(raw.Meshes[0].Vertices[0].TexCoord0.Y, 0.75f, kTolerance);
}

LIMX_TEST(ObjLoader, AcceptsTexCoordWithoutVComponent)
{
    // 一维纹理坐标 — v 分量可省略
    const AnsiChar* source =
        "v 0.0 0.0 0.0\nv 1.0 0.0 0.0\nv 0.0 1.0 0.0\n"
        "vt 0.5\n"
        "f 1/1 2/1 3/1\n";

    FAssetScene scene;
    LIMX_REQUIRE_TRUE(ParseObj(source, scene, RawUvOptions()).Succeeded);

    LIMX_EXPECT_NEAR(scene.Meshes[0].Vertices[0].TexCoord0.X, 0.5f, kTolerance);
    LIMX_EXPECT_NEAR(scene.Meshes[0].Vertices[0].TexCoord0.Y, 0.0f, kTolerance);
}

// ============================================================================
// 属性补齐
// ============================================================================

LIMX_TEST(ObjLoader, GeneratesNormalsWhenMissing)
{
    // XY 平面上的三角形, 逆时针环绕 — 生成的法线应指向 +Z
    const AnsiChar* source =
        "v 0.0 0.0 0.0\n"
        "v 1.0 0.0 0.0\n"
        "v 0.0 1.0 0.0\n"
        "f 1 2 3\n";

    FAssetScene scene;
    LIMX_REQUIRE_TRUE(ParseObj(source, scene).Succeeded);

    const FMeshData& mesh = scene.Meshes[0];

    // 源数据无法线, 但补齐后每个顶点都应有单位法线
    LIMX_EXPECT_TRUE(mesh.HasNormals);

    for (SizeType i = 0; i < mesh.GetVertexCount(); ++i)
    {
        LIMX_EXPECT_NEAR(mesh.Vertices[i].Normal.Length(), 1.0f, 1.0e-3f);
        LIMX_EXPECT_NEAR(mesh.Vertices[i].Normal.Z, 1.0f, 1.0e-3f);
    }
}

LIMX_TEST(ObjLoader, DoesNotGenerateNormalsWhenDisabled)
{
    const AnsiChar* source =
        "v 0.0 0.0 0.0\nv 1.0 0.0 0.0\nv 0.0 1.0 0.0\n"
        "f 1 2 3\n";

    FObjLoadOptions options;
    options.GenerateMissingNormals = false;

    FAssetScene scene;
    LIMX_REQUIRE_TRUE(ParseObj(source, scene, options).Succeeded);

    LIMX_EXPECT_FALSE(scene.Meshes[0].HasNormals);
}

LIMX_TEST(ObjLoader, GeneratesTangentsWhenTexCoordsPresent)
{
    const AnsiChar* source =
        "v 0.0 0.0 0.0\nv 1.0 0.0 0.0\nv 0.0 1.0 0.0\n"
        "vt 0.0 0.0\nvt 1.0 0.0\nvt 0.0 1.0\n"
        "vn 0.0 0.0 1.0\n"
        "f 1/1/1 2/2/1 3/3/1\n";

    FAssetScene scene;
    LIMX_REQUIRE_TRUE(ParseObj(source, scene, RawUvOptions()).Succeeded);

    const FMeshData& mesh = scene.Meshes[0];
    LIMX_REQUIRE_TRUE(mesh.HasTangents);

    for (SizeType i = 0; i < mesh.GetVertexCount(); ++i)
    {
        const FVector4& tangent = mesh.Vertices[i].Tangent;

        // 切线应为单位向量且与法线正交
        const FVector3 tangentXyz(tangent.X, tangent.Y, tangent.Z);
        LIMX_EXPECT_NEAR(tangentXyz.Length(), 1.0f, 1.0e-3f);
        LIMX_EXPECT_NEAR(FVector3::Dot(tangentXyz, mesh.Vertices[i].Normal),
                         0.0f, 1.0e-3f);

        // 手性必须是 ±1
        LIMX_EXPECT_NEAR(FMath::Abs(tangent.W), 1.0f, 1.0e-3f);
    }
}

LIMX_TEST(ObjLoader, ComputesBounds)
{
    const AnsiChar* source =
        "v -2.0 -3.0 -4.0\n"
        "v  5.0  0.0  0.0\n"
        "v  0.0  6.0  7.0\n"
        "f 1 2 3\n";

    FAssetScene scene;
    LIMX_REQUIRE_TRUE(ParseObj(source, scene).Succeeded);

    const FBoundingBox& bounds = scene.Meshes[0].Bounds;

    LIMX_REQUIRE_TRUE(bounds.IsValid());
    LIMX_EXPECT_NEAR(bounds.Min.X, -2.0f, kTolerance);
    LIMX_EXPECT_NEAR(bounds.Min.Y, -3.0f, kTolerance);
    LIMX_EXPECT_NEAR(bounds.Min.Z, -4.0f, kTolerance);
    LIMX_EXPECT_NEAR(bounds.Max.X, 5.0f, kTolerance);
    LIMX_EXPECT_NEAR(bounds.Max.Y, 6.0f, kTolerance);
    LIMX_EXPECT_NEAR(bounds.Max.Z, 7.0f, kTolerance);

    // 场景包围盒应与单网格一致 (根节点为单位变换)
    LIMX_EXPECT_NEAR(scene.Bounds.Max.Z, 7.0f, kTolerance);
}

// ============================================================================
// MTL 材质
// ============================================================================

LIMX_TEST(MtlLoader, ParsesBasicMaterial)
{
    const AnsiChar* source =
        "newmtl brick\n"
        "Kd 0.8 0.2 0.1\n"
        "Ke 0.0 0.0 0.5\n"
        "d 1.0\n";

    FAssetScene scene;
    TArray<FString> warnings;

    SizeType length = 0;
    while (source[length] != '\0') { ++length; }

    const FAssetLoadResult result = FObjLoader::ParseMaterialLibrary(
        source, length, FString(), scene, warnings);

    LIMX_REQUIRE_TRUE(result.Succeeded);
    LIMX_REQUIRE_EQ(scene.Materials.GetSize(), SizeType(1));

    const FMaterialData& material = scene.Materials[0];

    LIMX_EXPECT_TRUE(material.Name == "brick");
    LIMX_EXPECT_NEAR(material.BaseColorFactor.X, 0.8f, kTolerance);
    LIMX_EXPECT_NEAR(material.BaseColorFactor.Y, 0.2f, kTolerance);
    LIMX_EXPECT_NEAR(material.BaseColorFactor.Z, 0.1f, kTolerance);
    LIMX_EXPECT_NEAR(material.EmissiveFactor.Z, 0.5f, kTolerance);
    LIMX_EXPECT_EQ(static_cast<Int32>(material.AlphaMode),
                   static_cast<Int32>(EAlphaMode::Opaque));
}

LIMX_TEST(MtlLoader, ConvertsSpecularExponentToRoughness)
{
    const AnsiChar* source =
        "newmtl rough\n"
        "Ns 0.0\n"
        "newmtl smooth\n"
        "Ns 1000.0\n";

    FAssetScene scene;
    TArray<FString> warnings;

    SizeType length = 0;
    while (source[length] != '\0') { ++length; }

    LIMX_REQUIRE_TRUE(FObjLoader::ParseMaterialLibrary(
        source, length, FString(), scene, warnings).Succeeded);
    LIMX_REQUIRE_EQ(scene.Materials.GetSize(), SizeType(2));

    // roughness = sqrt(2 / (Ns + 2)); Ns=0 得 1.0
    LIMX_EXPECT_NEAR(scene.Materials[0].RoughnessFactor, 1.0f, 1.0e-3f);

    // Ns=1000 得约 0.0447
    LIMX_EXPECT_LT(scene.Materials[1].RoughnessFactor, 0.1f);
    LIMX_EXPECT_GT(scene.Materials[1].RoughnessFactor, 0.0f);
}

LIMX_TEST(MtlLoader, PbrExtensionOverridesApproximation)
{
    // Pr 是 MTL 的 PBR 扩展 — 出现时不应再用 Ns 的近似式
    const AnsiChar* source =
        "newmtl metal\n"
        "Pr 0.25\n"
        "Ns 500.0\n"
        "Pm 1.0\n";

    FAssetScene scene;
    TArray<FString> warnings;

    SizeType length = 0;
    while (source[length] != '\0') { ++length; }

    LIMX_REQUIRE_TRUE(FObjLoader::ParseMaterialLibrary(
        source, length, FString(), scene, warnings).Succeeded);
    LIMX_REQUIRE_EQ(scene.Materials.GetSize(), SizeType(1));

    LIMX_EXPECT_NEAR(scene.Materials[0].RoughnessFactor, 0.25f, kTolerance);
    LIMX_EXPECT_NEAR(scene.Materials[0].MetallicFactor, 1.0f, kTolerance);
}

LIMX_TEST(MtlLoader, DefaultsToNonMetal)
{
    // OBJ 没有金属度概念 — 从 Ks 反推极不可靠, 默认必须是非金属
    const AnsiChar* source =
        "newmtl plain\n"
        "Kd 0.5 0.5 0.5\n";

    FAssetScene scene;
    TArray<FString> warnings;

    SizeType length = 0;
    while (source[length] != '\0') { ++length; }

    LIMX_REQUIRE_TRUE(FObjLoader::ParseMaterialLibrary(
        source, length, FString(), scene, warnings).Succeeded);

    LIMX_EXPECT_NEAR(scene.Materials[0].MetallicFactor, 0.0f, kTolerance);
}

LIMX_TEST(MtlLoader, TransparencyMapsToBlendMode)
{
    const AnsiChar* source =
        "newmtl glass\n"
        "d 0.4\n"
        "newmtl fromTr\n"
        "Tr 0.7\n";

    FAssetScene scene;
    TArray<FString> warnings;

    SizeType length = 0;
    while (source[length] != '\0') { ++length; }

    LIMX_REQUIRE_TRUE(FObjLoader::ParseMaterialLibrary(
        source, length, FString(), scene, warnings).Succeeded);
    LIMX_REQUIRE_EQ(scene.Materials.GetSize(), SizeType(2));

    LIMX_EXPECT_NEAR(scene.Materials[0].BaseColorFactor.W, 0.4f, kTolerance);
    LIMX_EXPECT_EQ(static_cast<Int32>(scene.Materials[0].AlphaMode),
                   static_cast<Int32>(EAlphaMode::Blend));

    // Tr 是 d 的补数 — 0.7 的透明度等于 0.3 的不透明度
    LIMX_EXPECT_NEAR(scene.Materials[1].BaseColorFactor.W, 0.3f, kTolerance);
}

LIMX_TEST(MtlLoader, ParsesTexturePathsSkippingOptions)
{
    const AnsiChar* source =
        "newmtl textured\n"
        "map_Kd brick_albedo.png\n"
        "map_Bump -bm 1.5 brick_normal.png\n"
        "map_Ke -o 1 1 -s 2 2 glow.jpg\n";

    FAssetScene scene;
    TArray<FString> warnings;

    SizeType length = 0;
    while (source[length] != '\0') { ++length; }

    LIMX_REQUIRE_TRUE(FObjLoader::ParseMaterialLibrary(
        source, length, FString("Textures"), scene, warnings).Succeeded);

    const FMaterialData& material = scene.Materials[0];

    LIMX_EXPECT_STREQ(material.BaseColorTexture.Path.GetCStr(),
                      "Textures/brick_albedo.png");

    // 选项参数必须被跳过, 只取文件名
    LIMX_EXPECT_STREQ(material.NormalTexture.Path.GetCStr(),
                      "Textures/brick_normal.png");
    LIMX_EXPECT_STREQ(material.EmissiveTexture.Path.GetCStr(),
                      "Textures/glow.jpg");
}

LIMX_TEST(MtlLoader, NormalizesBackslashPaths)
{
    // Windows 导出的 MTL 常用反斜杠
    const AnsiChar* source =
        "newmtl win\n"
        "map_Kd textures\\wall\\albedo.png\n";

    FAssetScene scene;
    TArray<FString> warnings;

    SizeType length = 0;
    while (source[length] != '\0') { ++length; }

    LIMX_REQUIRE_TRUE(FObjLoader::ParseMaterialLibrary(
        source, length, FString("Assets"), scene, warnings).Succeeded);

    LIMX_EXPECT_STREQ(scene.Materials[0].BaseColorTexture.Path.GetCStr(),
                      "Assets/textures/wall/albedo.png");
}

LIMX_TEST(MtlLoader, WarnsOnPropertyBeforeNewmtl)
{
    const AnsiChar* source =
        "Kd 1.0 0.0 0.0\n"
        "newmtl valid\n";

    FAssetScene scene;
    TArray<FString> warnings;

    SizeType length = 0;
    while (source[length] != '\0') { ++length; }

    LIMX_REQUIRE_TRUE(FObjLoader::ParseMaterialLibrary(
        source, length, FString(), scene, warnings).Succeeded);

    LIMX_EXPECT_EQ(scene.Materials.GetSize(), SizeType(1));
    LIMX_EXPECT_GT(warnings.GetSize(), SizeType(0));
}

// ============================================================================
// 材质与子网格切分
// ============================================================================

LIMX_TEST(ObjLoader, SplitsSubMeshesByMaterial)
{
    const AnsiChar* source =
        "v 0.0 0.0 0.0\nv 1.0 0.0 0.0\nv 0.0 1.0 0.0\n"
        "v 2.0 0.0 0.0\nv 3.0 0.0 0.0\nv 2.0 1.0 0.0\n"
        "usemtl red\n"
        "f 1 2 3\n"
        "usemtl blue\n"
        "f 4 5 6\n";

    FAssetScene scene;

    // 先注入两个材质, 使 usemtl 能匹配上
    FMaterialData red;
    red.Name = FName("red");
    scene.Materials.Add(red);

    FMaterialData blue;
    blue.Name = FName("blue");
    scene.Materials.Add(blue);

    // LoadFromMemory 会清空场景, 因此改为解析后再核对切分结构
    const AnsiChar* withMaterials =
        "v 0.0 0.0 0.0\nv 1.0 0.0 0.0\nv 0.0 1.0 0.0\n"
        "v 2.0 0.0 0.0\nv 3.0 0.0 0.0\nv 2.0 1.0 0.0\n"
        "usemtl red\n"
        "f 1 2 3\n"
        "usemtl blue\n"
        "f 4 5 6\n";

    FAssetScene parsed;
    const FAssetLoadResult result = ParseObj(withMaterials, parsed);

    LIMX_REQUIRE_TRUE(result.Succeeded);

    const FMeshData& mesh = parsed.Meshes[0];

    // 两次 usemtl 切换 — 应产生两个子网格
    LIMX_REQUIRE_EQ(mesh.SubMeshes.GetSize(), SizeType(2));

    LIMX_EXPECT_EQ(mesh.SubMeshes[0].IndexOffset, UInt32(0));
    LIMX_EXPECT_EQ(mesh.SubMeshes[0].IndexCount, UInt32(3));
    LIMX_EXPECT_EQ(mesh.SubMeshes[1].IndexOffset, UInt32(3));
    LIMX_EXPECT_EQ(mesh.SubMeshes[1].IndexCount, UInt32(3));

    // 两段索引区间应覆盖全部索引且不重叠
    LIMX_EXPECT_EQ(mesh.SubMeshes[0].IndexCount + mesh.SubMeshes[1].IndexCount,
                   static_cast<UInt32>(mesh.GetIndexCount()));

    LIMX_UNUSED(scene);
}

LIMX_TEST(ObjLoader, SingleMaterialProducesSingleSubMesh)
{
    const AnsiChar* source =
        "v 0.0 0.0 0.0\nv 1.0 0.0 0.0\nv 0.0 1.0 0.0\n"
        "f 1 2 3\n";

    FAssetScene scene;
    LIMX_REQUIRE_TRUE(ParseObj(source, scene).Succeeded);

    LIMX_REQUIRE_EQ(scene.Meshes[0].SubMeshes.GetSize(), SizeType(1));
    LIMX_EXPECT_EQ(scene.Meshes[0].SubMeshes[0].IndexCount, UInt32(3));
    LIMX_EXPECT_EQ(scene.Meshes[0].SubMeshes[0].MaterialIndex, -1);
}

LIMX_TEST(ObjLoader, SubMeshBoundsCoverOnlyOwnVertices)
{
    const AnsiChar* source =
        "v 0.0 0.0 0.0\nv 1.0 0.0 0.0\nv 0.0 1.0 0.0\n"
        "v 100.0 0.0 0.0\nv 101.0 0.0 0.0\nv 100.0 1.0 0.0\n"
        "usemtl a\n"
        "f 1 2 3\n"
        "usemtl b\n"
        "f 4 5 6\n";

    FAssetScene scene;
    LIMX_REQUIRE_TRUE(ParseObj(source, scene).Succeeded);

    const FMeshData& mesh = scene.Meshes[0];
    LIMX_REQUIRE_EQ(mesh.SubMeshes.GetSize(), SizeType(2));

    // 第一个子网格不应包含远处的第二组顶点
    LIMX_EXPECT_LT(mesh.SubMeshes[0].Bounds.Max.X, 50.0f);
    LIMX_EXPECT_GT(mesh.SubMeshes[1].Bounds.Min.X, 50.0f);

    // 整体包围盒覆盖两者
    LIMX_EXPECT_NEAR(mesh.Bounds.Max.X, 101.0f, kTolerance);
}

// ============================================================================
// 场景结构
// ============================================================================

LIMX_TEST(ObjLoader, ProducesSingleRootNode)
{
    const AnsiChar* source =
        "v 0.0 0.0 0.0\nv 1.0 0.0 0.0\nv 0.0 1.0 0.0\n"
        "f 1 2 3\n";

    FAssetScene scene;
    LIMX_REQUIRE_TRUE(ParseObj(source, scene).Succeeded);

    LIMX_REQUIRE_EQ(scene.Nodes.GetSize(), SizeType(1));
    LIMX_REQUIRE_EQ(scene.RootNodes.GetSize(), SizeType(1));

    LIMX_EXPECT_EQ(scene.RootNodes[0], 0);
    LIMX_EXPECT_EQ(scene.Nodes[0].MeshIndex, 0);
    LIMX_EXPECT_EQ(scene.Nodes[0].ParentIndex, -1);
}

LIMX_TEST(ObjLoader, ReportsSceneStatistics)
{
    const AnsiChar* source =
        "v 0.0 0.0 0.0\nv 1.0 0.0 0.0\nv 1.0 1.0 0.0\nv 0.0 1.0 0.0\n"
        "f 1 2 3\n"
        "f 1 3 4\n";

    FAssetScene scene;
    LIMX_REQUIRE_TRUE(ParseObj(source, scene).Succeeded);

    LIMX_EXPECT_EQ(scene.GetTotalVertexCount(), SizeType(4));
    LIMX_EXPECT_EQ(scene.GetTotalTriangleCount(), SizeType(2));
    LIMX_EXPECT_EQ(scene.GetTotalSubMeshCount(), SizeType(1));
    LIMX_EXPECT_FALSE(scene.IsEmpty());
}

// ============================================================================
// 数值解析
// ============================================================================

LIMX_TEST(ObjLoader, ParsesVariousNumberFormats)
{
    const AnsiChar* source =
        "v -1.5e2 .5 3.\n"
        "v +2.0 -0.25 1e-3\n"
        "v 0 0 0\n"
        "f 1 2 3\n";

    FAssetScene scene;
    LIMX_REQUIRE_TRUE(ParseObj(source, scene).Succeeded);

    const FMeshData& mesh = scene.Meshes[0];
    LIMX_REQUIRE_EQ(mesh.GetVertexCount(), SizeType(3));

    // 科学计数法、省略整数部分、省略小数部分都应被接受
    LIMX_EXPECT_NEAR(mesh.Vertices[0].Position.X, -150.0f, 1.0e-3f);
    LIMX_EXPECT_NEAR(mesh.Vertices[0].Position.Y, 0.5f, kTolerance);
    LIMX_EXPECT_NEAR(mesh.Vertices[0].Position.Z, 3.0f, kTolerance);

    LIMX_EXPECT_NEAR(mesh.Vertices[1].Position.X, 2.0f, kTolerance);
    LIMX_EXPECT_NEAR(mesh.Vertices[1].Position.Y, -0.25f, kTolerance);
    LIMX_EXPECT_NEAR(mesh.Vertices[1].Position.Z, 0.001f, 1.0e-6f);
}

LIMX_TEST(ObjLoader, WarnsOnIncompleteVertexLine)
{
    const AnsiChar* source =
        "v 1.0 2.0\n"        // 缺少 Z
        "v 0.0 0.0 0.0\nv 1.0 0.0 0.0\nv 0.0 1.0 0.0\n"
        "f 1 2 3\n";

    FAssetScene scene;
    const FAssetLoadResult result = ParseObj(source, scene);

    LIMX_REQUIRE_TRUE(result.Succeeded);

    // 畸形行被跳过 — 只有后三个顶点进入数组
    LIMX_EXPECT_EQ(scene.Meshes[0].GetVertexCount(), SizeType(3));
    LIMX_EXPECT_GT(result.Warnings.GetSize(), SizeType(0));
}

LIMX_TEST(ObjLoader, HandlesCrLfLineEndings)
{
    const AnsiChar* source =
        "v 0.0 0.0 0.0\r\n"
        "v 1.0 0.0 0.0\r\n"
        "v 0.0 1.0 0.0\r\n"
        "f 1 2 3\r\n";

    FAssetScene scene;
    LIMX_REQUIRE_TRUE(ParseObj(source, scene).Succeeded);

    LIMX_EXPECT_EQ(scene.Meshes[0].GetVertexCount(), SizeType(3));
    LIMX_EXPECT_EQ(scene.Meshes[0].GetTriangleCount(), SizeType(1));
}

LIMX_TEST(ObjLoader, DropsUnreferencedVertices)
{
    // 第四个顶点没有出现在任何面里 — 去重按面引用建表, 它不应进入输出
    const AnsiChar* source =
        "v 0.0 0.0 0.0\n"
        "v 1.0 0.0 0.0\n"
        "v 0.0 1.0 0.0\n"
        "v 99.0 99.0 99.0\n"
        "f 1 2 3\n";

    FAssetScene scene;
    LIMX_REQUIRE_TRUE(ParseObj(source, scene).Succeeded);

    const FMeshData& mesh = scene.Meshes[0];

    LIMX_EXPECT_EQ(mesh.GetVertexCount(), SizeType(3));

    // 孤立顶点也不应影响包围盒
    LIMX_EXPECT_LT(mesh.Bounds.Max.X, 50.0f);
}

LIMX_TEST(ObjLoader, HandlesLargeMesh)
{
    // 构造一条足够长的三角形带, 验证解析器在规模上的行为
    FStringBuilder builder(256 * 1024);

    const Int32 kStripLength = 2000;

    for (Int32 i = 0; i < kStripLength; ++i)
    {
        builder.Append("v ");
        builder.AppendFloat(static_cast<Float64>(i), 1);
        builder.Append(" 0.0 0.0\n");

        builder.Append("v ");
        builder.AppendFloat(static_cast<Float64>(i), 1);
        builder.Append(" 1.0 0.0\n");
    }

    for (Int32 i = 0; i + 1 < kStripLength; ++i)
    {
        const Int32 base = i * 2 + 1;

        builder.Append("f ");
        builder.AppendInt(base);
        builder.Append(" ");
        builder.AppendInt(base + 1);
        builder.Append(" ");
        builder.AppendInt(base + 2);
        builder.Append("\n");
    }

    const FString source = builder.ToString();

    FAssetScene scene;
    const FAssetLoadResult result = FObjLoader::LoadFromMemory(
        source.GetCStr(), source.GetLength(), FString(), scene,
        FObjLoadOptions());

    LIMX_REQUIRE_TRUE(result.Succeeded);

    const FMeshData& mesh = scene.Meshes[0];

    // 三角形带的最后一个顶点未被任何面引用。顶点去重只登记面实际用到的
    // 属性组合，孤立顶点因此不会进入输出 —— GPU 侧也不需要它们。
    LIMX_EXPECT_EQ(mesh.GetVertexCount(),
                   static_cast<SizeType>(kStripLength * 2 - 1));
    LIMX_EXPECT_EQ(mesh.GetTriangleCount(),
                   static_cast<SizeType>(kStripLength - 1));

    LIMX_TEST_INFO("大网格: {} 顶点 / {} 三角形, 包围盒 X 跨度 {}",
                   mesh.GetVertexCount(), mesh.GetTriangleCount(),
                   mesh.Bounds.Max.X - mesh.Bounds.Min.X);
}
