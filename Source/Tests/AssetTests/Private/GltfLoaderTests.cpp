/*******************************************************************************
 * 文件: GltfLoaderTests.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   glTF 2.0 / GLB 解析器单元测试 — 访问器解引用、索引宽度、交错布局、
 *   归一化属性、节点层级、材质映射、GLB 容器与错误处理
 *
 * 设计哲学:
 *   访问器是首要目标 — glTF 的每段数据都要穿过三级间接加两层偏移，
 *   还叠加跨步、组件类型与归一化。用例针对紧密与交错两种布局、三种索引宽度、
 *   归一化与非归一化分别覆盖，因为任何一种处理错误都会产出"看起来像模型
 *   但形状不对"的几何，而这类缺陷肉眼极难判定。
 *
 *   GLB 在代码里构造 — 把近千字节的二进制夹具写成字节数组既不可读也难维护。
 *   测试用辅助函数按规范拼装容器，这样容器布局本身也成了可读的文档。
 *
 *   拒绝要比接受更严格 — 版本不符、魔数错误、访问器越界、压缩扩展，
 *   这些情形若被静默容忍，产出的是空的或扭曲的模型。用例逐一验证它们被拒绝。
 *
 * 技术特性:
 *   - 全部夹具自包含: 缓冲区经 data URI 内嵌, 不依赖外部 .bin
 *   - GLB 容器按规范拼装, 含块长度与 4 字节对齐
 *   - 几何断言精确到顶点坐标与索引值
 *
 * 依赖关系:
 *   内部: AssetTests/AssetTestsMinimal.h
 *
 * 注意事项:
 *   夹具中的 base64 载荷由已知的浮点字节序列生成, 变更时需同步更新偏移量
 *
 ******************************************************************************/

#include "AssetTests/AssetTestsMinimal.h"

using namespace Limx;

namespace
{

constexpr Float32 kTolerance = 1.0e-5f;

// ============================================================================
// 夹具数据
// ============================================================================

/// 紧密排布的三角形缓冲区 (80 字节)
///
/// 布局: POSITION VEC3xF32 @0 (36B) | NORMAL VEC3xF32 @36 (36B)
///       | INDEX USHORTx3 @72 (6B) | 补齐 2B
/// 几何: (0,0,0) (1,0,0) (0,1,0), 法线全为 +Z
constexpr const AnsiChar* kTightBufferBase64 =
    "AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/"
    "AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAABAAIAAAA=";

/// 交错排布的缓冲区 (92 字节), 跨步 28
///
/// 布局: 每个顶点 = POSITION VEC3xF32 (12B) + COLOR_0 VEC4xF32 (16B)
///       索引 USHORTx3 @84
/// 颜色的 G 分量随顶点递增 (0.0 / 0.5 / 1.0), 用于验证跨步取值是否正确
constexpr const AnsiChar* kInterleavedBufferBase64 =
    "AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAACAPwAAgD8AAAAAAAAAAAAAgD8AAAA/"
    "AAAAAAAAgD8AAAAAAACAPwAAAAAAAIA/AACAPwAAAAAAAIA/AAABAAIAAAA=";

/// 使用 UNSIGNED_BYTE 索引的缓冲区 (40 字节)
///
/// 布局: POSITION VEC3xF32 @0 (36B) | INDEX UBYTEx3 @36 (3B) | 补齐 1B
constexpr const AnsiChar* kUByteIndexBufferBase64 =
    "AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAECAA==";

/// 拼出 data URI 前缀 + base64 载荷
FString MakeDataUri(const AnsiChar* base64)
{
    FString uri("data:application/octet-stream;base64,");
    uri.Append(base64);
    return uri;
}

/// 解析 glTF JSON 文本的便捷包装
FAssetLoadResult ParseGltf(const FString& json, FAssetScene& outScene,
                           const FGltfLoadOptions& options = FGltfLoadOptions())
{
    return FGltfLoader::LoadFromJson(json.GetCStr(), json.GetLength(),
                                     FString(), outScene, options);
}

/// 向字节数组追加一个小端 UInt32
void AppendUInt32(TArray<UInt8>& bytes, UInt32 value)
{
    bytes.Add(static_cast<UInt8>(value & 0xFFu));
    bytes.Add(static_cast<UInt8>((value >> 8) & 0xFFu));
    bytes.Add(static_cast<UInt8>((value >> 16) & 0xFFu));
    bytes.Add(static_cast<UInt8>((value >> 24) & 0xFFu));
}

/// 按 glTF 规范拼装一个 GLB 容器
///
/// 结构: [magic|version|totalLength] [jsonLen|"JSON"|json] [binLen|"BIN\0"|bin]
/// JSON 块以空格补齐到 4 字节，二进制块以零补齐 —— 这是规范的明确要求。
TArray<UInt8> BuildGlb(const FString& json, const TArray<UInt8>& binary)
{
    // JSON 块以空格补齐
    TArray<UInt8> jsonChunk;
    for (SizeType i = 0; i < json.GetLength(); ++i)
    {
        jsonChunk.Add(static_cast<UInt8>(json[i]));
    }
    while ((jsonChunk.GetSize() % 4) != 0)
    {
        jsonChunk.Add(static_cast<UInt8>(' '));
    }

    // 二进制块以零补齐
    TArray<UInt8> binChunk;
    for (SizeType i = 0; i < binary.GetSize(); ++i)
    {
        binChunk.Add(binary[i]);
    }
    while ((binChunk.GetSize() % 4) != 0)
    {
        binChunk.Add(0);
    }

    const UInt32 totalLength = static_cast<UInt32>(
        12 + 8 + jsonChunk.GetSize() +
        (binChunk.GetSize() > 0 ? (8 + binChunk.GetSize()) : 0));

    TArray<UInt8> glb;

    AppendUInt32(glb, 0x46546C67u);   // "glTF"
    AppendUInt32(glb, 2u);            // version
    AppendUInt32(glb, totalLength);

    AppendUInt32(glb, static_cast<UInt32>(jsonChunk.GetSize()));
    AppendUInt32(glb, 0x4E4F534Au);   // "JSON"
    for (SizeType i = 0; i < jsonChunk.GetSize(); ++i)
    {
        glb.Add(jsonChunk[i]);
    }

    if (binChunk.GetSize() > 0)
    {
        AppendUInt32(glb, static_cast<UInt32>(binChunk.GetSize()));
        AppendUInt32(glb, 0x004E4942u);   // "BIN\0"
        for (SizeType i = 0; i < binChunk.GetSize(); ++i)
        {
            glb.Add(binChunk[i]);
        }
    }

    return glb;
}

/// 构造一个最小可用的三角形 glTF JSON
///
/// @param bufferUri 缓冲区的 uri; 传空串则省略该字段 (GLB 二进制块的形态)
FString MakeTriangleGltf(const FString& bufferUri)
{
    FStringBuilder builder(2048);

    builder.Append("{\"asset\":{\"version\":\"2.0\",\"generator\":\"LimxTest\"},");
    builder.Append("\"scene\":0,\"scenes\":[{\"nodes\":[0]}],");
    builder.Append("\"nodes\":[{\"mesh\":0,\"name\":\"TriNode\"}],");
    builder.Append("\"meshes\":[{\"name\":\"Tri\",\"primitives\":[");
    builder.Append("{\"attributes\":{\"POSITION\":0,\"NORMAL\":1},");
    builder.Append("\"indices\":2,\"material\":0}]}],");
    builder.Append("\"materials\":[{\"name\":\"TestMat\",");
    builder.Append("\"pbrMetallicRoughness\":{");
    builder.Append("\"baseColorFactor\":[0.8,0.4,0.2,1.0],");
    builder.Append("\"metallicFactor\":0.25,\"roughnessFactor\":0.75},");
    builder.Append("\"emissiveFactor\":[0.1,0.2,0.3],");
    builder.Append("\"doubleSided\":true,");
    builder.Append("\"alphaMode\":\"MASK\",\"alphaCutoff\":0.25}],");
    builder.Append("\"accessors\":[");
    builder.Append("{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},");
    builder.Append("{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},");
    builder.Append("{\"bufferView\":2,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}],");
    builder.Append("\"bufferViews\":[");
    builder.Append("{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},");
    builder.Append("{\"buffer\":0,\"byteOffset\":36,\"byteLength\":36},");
    builder.Append("{\"buffer\":0,\"byteOffset\":72,\"byteLength\":6}],");
    builder.Append("\"buffers\":[{\"byteLength\":80");

    if (!bufferUri.IsEmpty())
    {
        builder.Append(",\"uri\":\"");
        builder.Append(bufferUri);
        builder.Append("\"");
    }

    builder.Append("}]}");

    return builder.ToString();
}


/// 构造一个含 primitiveCount 个图元的 glTF
///
/// 每个图元共用同一份三角形数据 (同样的 accessor), 但各自指向不同的
/// 材质 —— 材质 p 的 baseColorFactor.R = p / 255。这样一来:
///
///   顶点/索引数应为 3 * primitiveCount, 图元 p 的索引应为 {0,1,2} + 3p;
///   子网格 p 的 IndexOffset 应为 3p, MaterialIndex 应为 p。
///
/// 后一条是关键: 装配被并行化之后, 图元完成的先后顺序不再固定, 而渲染
/// 时按子网格顺序绑定材质 —— 顺序一乱, 画面上就是满场的材质错位, 而
/// 顶点数、三角形数这些统计量全都对得上, 看不出任何异常。
FString MakeMultiPrimitiveGltf(SizeType primitiveCount)
{
    FStringBuilder builder(8192);

    builder.Append("{\"asset\":{\"version\":\"2.0\",\"generator\":\"LimxTest\"},");
    builder.Append("\"scene\":0,\"scenes\":[{\"nodes\":[0]}],");
    builder.Append("\"nodes\":[{\"mesh\":0,\"name\":\"MultiNode\"}],");
    builder.Append("\"meshes\":[{\"name\":\"Multi\",\"primitives\":[");

    for (SizeType p = 0; p < primitiveCount; ++p)
    {
        if (p > 0)
        {
            builder.Append(",");
        }

        builder.Append("{\"attributes\":{\"POSITION\":0,\"NORMAL\":1},");
        builder.Append("\"indices\":2,\"material\":");
        builder.AppendInt(static_cast<Int64>(p));
        builder.Append("}");
    }

    builder.Append("]}],\"materials\":[");

    for (SizeType p = 0; p < primitiveCount; ++p)
    {
        if (p > 0)
        {
            builder.Append(",");
        }

        builder.Append("{\"name\":\"Mat");
        builder.AppendInt(static_cast<Int64>(p));
        builder.Append("\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[");
        builder.AppendFloat(static_cast<Float64>(p) / 255.0, 6);
        builder.Append(",0.0,0.0,1.0]}}");
    }

    builder.Append("],\"accessors\":[");
    builder.Append("{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},");
    builder.Append("{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},");
    builder.Append("{\"bufferView\":2,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}],");
    builder.Append("\"bufferViews\":[");
    builder.Append("{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},");
    builder.Append("{\"buffer\":0,\"byteOffset\":36,\"byteLength\":36},");
    builder.Append("{\"buffer\":0,\"byteOffset\":72,\"byteLength\":6}],");
    builder.Append("\"buffers\":[{\"byteLength\":80,\"uri\":\"");
    builder.Append(MakeDataUri(kTightBufferBase64));
    builder.Append("\"}]}");

    return builder.ToString();
}

/// 检查一份多图元解析结果是否处处自洽
///
/// 断言宏引用的是 LIMX_TEST 生成的 limxTestContext 形参, 因此在 helper
/// 里用就得把它显式传进来, 并在函数内保持同名。
void CheckMultiPrimitive(FTestContext& limxTestContext,
                         const FMeshData& mesh, SizeType primitiveCount)
{
    LIMX_EXPECT_EQ(mesh.GetVertexCount(), primitiveCount * 3);
    LIMX_EXPECT_EQ(mesh.GetIndexCount(), primitiveCount * 3);
    LIMX_EXPECT_EQ(mesh.SubMeshes.GetSize(), primitiveCount);

    if (mesh.SubMeshes.GetSize() != primitiveCount)
    {
        return;
    }

    for (SizeType p = 0; p < primitiveCount; ++p)
    {
        const FSubMesh& subMesh = mesh.SubMeshes[p];

        // 子网格必须仍按图元顺序排列
        LIMX_EXPECT_EQ(subMesh.MaterialIndex, static_cast<Int32>(p));
        LIMX_EXPECT_EQ(subMesh.IndexOffset, static_cast<UInt32>(p * 3));
        LIMX_EXPECT_EQ(subMesh.IndexCount, UInt32(3));

        // 索引必须按各自图元的顶点基址整体平移
        for (SizeType i = 0; i < 3; ++i)
        {
            LIMX_EXPECT_EQ(mesh.Indices[p * 3 + i],
                           static_cast<UInt32>(p * 3 + i));
        }

        // 顶点坐标应逐图元重复 —— 读错缓冲区偏移在这里立刻暴露
        LIMX_EXPECT_NEAR(mesh.Vertices[p * 3 + 0].Position.X, 0.0f, kTolerance);
        LIMX_EXPECT_NEAR(mesh.Vertices[p * 3 + 1].Position.X, 1.0f, kTolerance);
        LIMX_EXPECT_NEAR(mesh.Vertices[p * 3 + 2].Position.Y, 1.0f, kTolerance);
    }
}

} // namespace

// ============================================================================
// 基础解析
// ============================================================================

LIMX_TEST(GltfLoader, ParsesTriangleWithDataUri)
{
    const FString json =
        MakeTriangleGltf(MakeDataUri(kTightBufferBase64));

    FAssetScene scene;
    const FAssetLoadResult result = ParseGltf(json, scene);

    LIMX_REQUIRE_TRUE(result.Succeeded);
    LIMX_REQUIRE_EQ(scene.Meshes.GetSize(), SizeType(1));

    const FMeshData& mesh = scene.Meshes[0];

    LIMX_EXPECT_EQ(mesh.GetVertexCount(), SizeType(3));
    LIMX_EXPECT_EQ(mesh.GetIndexCount(), SizeType(3));
    LIMX_EXPECT_EQ(mesh.GetTriangleCount(), SizeType(1));

    // 顶点坐标必须与夹具中的浮点字节完全对应
    LIMX_EXPECT_NEAR(mesh.Vertices[0].Position.X, 0.0f, kTolerance);
    LIMX_EXPECT_NEAR(mesh.Vertices[1].Position.X, 1.0f, kTolerance);
    LIMX_EXPECT_NEAR(mesh.Vertices[2].Position.Y, 1.0f, kTolerance);

    // 法线来自第二个 bufferView — 偏移算错会读到位置数据
    LIMX_EXPECT_TRUE(mesh.HasNormals);
    LIMX_EXPECT_NEAR(mesh.Vertices[0].Normal.Z, 1.0f, kTolerance);
    LIMX_EXPECT_NEAR(mesh.Vertices[1].Normal.Z, 1.0f, kTolerance);
}

LIMX_TEST(GltfLoader, ReadsIndicesInOrder)
{
    const FString json = MakeTriangleGltf(MakeDataUri(kTightBufferBase64));

    FAssetScene scene;
    LIMX_REQUIRE_TRUE(ParseGltf(json, scene).Succeeded);

    const FMeshData& mesh = scene.Meshes[0];

    LIMX_EXPECT_EQ(mesh.Indices[0], UInt32(0));
    LIMX_EXPECT_EQ(mesh.Indices[1], UInt32(1));
    LIMX_EXPECT_EQ(mesh.Indices[2], UInt32(2));
}

LIMX_TEST(GltfLoader, ComputesBounds)
{
    const FString json = MakeTriangleGltf(MakeDataUri(kTightBufferBase64));

    FAssetScene scene;
    LIMX_REQUIRE_TRUE(ParseGltf(json, scene).Succeeded);

    const FBoundingBox& bounds = scene.Meshes[0].Bounds;

    LIMX_REQUIRE_TRUE(bounds.IsValid());
    LIMX_EXPECT_NEAR(bounds.Min.X, 0.0f, kTolerance);
    LIMX_EXPECT_NEAR(bounds.Max.X, 1.0f, kTolerance);
    LIMX_EXPECT_NEAR(bounds.Max.Y, 1.0f, kTolerance);
}

// ============================================================================
// 访问器 — 布局与类型
// ============================================================================

LIMX_TEST(GltfLoader, HandlesInterleavedAttributes)
{
    // 位置与颜色交错存放, 跨步 28 字节 —— 忽略 byteStride 会读出错乱的数据
    FStringBuilder builder(2048);

    builder.Append("{\"asset\":{\"version\":\"2.0\"},");
    builder.Append("\"scene\":0,\"scenes\":[{\"nodes\":[0]}],");
    builder.Append("\"nodes\":[{\"mesh\":0}],");
    builder.Append("\"meshes\":[{\"primitives\":[{\"attributes\":");
    builder.Append("{\"POSITION\":0,\"COLOR_0\":1},\"indices\":2}]}],");
    builder.Append("\"accessors\":[");
    builder.Append("{\"bufferView\":0,\"byteOffset\":0,\"componentType\":5126,"
                   "\"count\":3,\"type\":\"VEC3\"},");
    builder.Append("{\"bufferView\":0,\"byteOffset\":12,\"componentType\":5126,"
                   "\"count\":3,\"type\":\"VEC4\"},");
    builder.Append("{\"bufferView\":1,\"componentType\":5123,\"count\":3,"
                   "\"type\":\"SCALAR\"}],");
    builder.Append("\"bufferViews\":[");
    builder.Append("{\"buffer\":0,\"byteOffset\":0,\"byteLength\":84,"
                   "\"byteStride\":28},");
    builder.Append("{\"buffer\":0,\"byteOffset\":84,\"byteLength\":6}],");
    builder.Append("\"buffers\":[{\"byteLength\":92,\"uri\":\"");
    builder.Append(MakeDataUri(kInterleavedBufferBase64));
    builder.Append("\"}]}");

    FAssetScene scene;
    const FAssetLoadResult result = ParseGltf(builder.ToString(), scene);

    LIMX_REQUIRE_TRUE(result.Succeeded);

    const FMeshData& mesh = scene.Meshes[0];
    LIMX_REQUIRE_EQ(mesh.GetVertexCount(), SizeType(3));

    // 位置按跨步 28 取值
    LIMX_EXPECT_NEAR(mesh.Vertices[0].Position.X, 0.0f, kTolerance);
    LIMX_EXPECT_NEAR(mesh.Vertices[1].Position.X, 1.0f, kTolerance);
    LIMX_EXPECT_NEAR(mesh.Vertices[2].Position.Y, 1.0f, kTolerance);

    // 颜色在每个顶点内偏移 12 字节, G 分量随顶点递增
    LIMX_REQUIRE_TRUE(mesh.HasVertexColors);
    LIMX_EXPECT_NEAR(mesh.Vertices[0].Color.Y, 0.0f, kTolerance);
    LIMX_EXPECT_NEAR(mesh.Vertices[1].Color.Y, 0.5f, kTolerance);
    LIMX_EXPECT_NEAR(mesh.Vertices[2].Color.Y, 1.0f, kTolerance);
}

LIMX_TEST(GltfLoader, HandlesUnsignedByteIndices)
{
    FStringBuilder builder(1024);

    builder.Append("{\"asset\":{\"version\":\"2.0\"},");
    builder.Append("\"scene\":0,\"scenes\":[{\"nodes\":[0]}],");
    builder.Append("\"nodes\":[{\"mesh\":0}],");
    builder.Append("\"meshes\":[{\"primitives\":[{\"attributes\":"
                   "{\"POSITION\":0},\"indices\":1}]}],");
    builder.Append("\"accessors\":[");
    builder.Append("{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                   "\"type\":\"VEC3\"},");
    builder.Append("{\"bufferView\":1,\"componentType\":5121,\"count\":3,"
                   "\"type\":\"SCALAR\"}],");
    builder.Append("\"bufferViews\":[");
    builder.Append("{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},");
    builder.Append("{\"buffer\":0,\"byteOffset\":36,\"byteLength\":3}],");
    builder.Append("\"buffers\":[{\"byteLength\":40,\"uri\":\"");
    builder.Append(MakeDataUri(kUByteIndexBufferBase64));
    builder.Append("\"}]}");

    FAssetScene scene;
    LIMX_REQUIRE_TRUE(ParseGltf(builder.ToString(), scene).Succeeded);

    const FMeshData& mesh = scene.Meshes[0];

    // 单字节索引必须被正确读出, 而非按两字节误读
    LIMX_REQUIRE_EQ(mesh.GetIndexCount(), SizeType(3));
    LIMX_EXPECT_EQ(mesh.Indices[0], UInt32(0));
    LIMX_EXPECT_EQ(mesh.Indices[1], UInt32(1));
    LIMX_EXPECT_EQ(mesh.Indices[2], UInt32(2));
}

LIMX_TEST(GltfLoader, PrimitiveWithoutIndicesBuildsSequentialList)
{
    FStringBuilder builder(1024);

    builder.Append("{\"asset\":{\"version\":\"2.0\"},");
    builder.Append("\"scene\":0,\"scenes\":[{\"nodes\":[0]}],");
    builder.Append("\"nodes\":[{\"mesh\":0}],");
    builder.Append("\"meshes\":[{\"primitives\":[{\"attributes\":"
                   "{\"POSITION\":0}}]}],");
    builder.Append("\"accessors\":[{\"bufferView\":0,\"componentType\":5126,"
                   "\"count\":3,\"type\":\"VEC3\"}],");
    builder.Append("\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,"
                   "\"byteLength\":36}],");
    builder.Append("\"buffers\":[{\"byteLength\":80,\"uri\":\"");
    builder.Append(MakeDataUri(kTightBufferBase64));
    builder.Append("\"}]}");

    FAssetScene scene;
    LIMX_REQUIRE_TRUE(ParseGltf(builder.ToString(), scene).Succeeded);

    const FMeshData& mesh = scene.Meshes[0];

    // 无索引时按顶点顺序构成三角形列表
    LIMX_REQUIRE_EQ(mesh.GetIndexCount(), SizeType(3));
    LIMX_EXPECT_EQ(mesh.Indices[0], UInt32(0));
    LIMX_EXPECT_EQ(mesh.Indices[2], UInt32(2));
}

LIMX_TEST(GltfLoader, GeneratesNormalsWhenAbsent)
{
    FStringBuilder builder(1024);

    builder.Append("{\"asset\":{\"version\":\"2.0\"},");
    builder.Append("\"scene\":0,\"scenes\":[{\"nodes\":[0]}],");
    builder.Append("\"nodes\":[{\"mesh\":0}],");
    builder.Append("\"meshes\":[{\"primitives\":[{\"attributes\":"
                   "{\"POSITION\":0},\"indices\":1}]}],");
    builder.Append("\"accessors\":[");
    builder.Append("{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                   "\"type\":\"VEC3\"},");
    builder.Append("{\"bufferView\":1,\"componentType\":5123,\"count\":3,"
                   "\"type\":\"SCALAR\"}],");
    builder.Append("\"bufferViews\":[");
    builder.Append("{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},");
    builder.Append("{\"buffer\":0,\"byteOffset\":72,\"byteLength\":6}],");
    builder.Append("\"buffers\":[{\"byteLength\":80,\"uri\":\"");
    builder.Append(MakeDataUri(kTightBufferBase64));
    builder.Append("\"}]}");

    FAssetScene scene;
    LIMX_REQUIRE_TRUE(ParseGltf(builder.ToString(), scene).Succeeded);

    const FMeshData& mesh = scene.Meshes[0];

    LIMX_REQUIRE_TRUE(mesh.HasNormals);

    // XY 平面上逆时针环绕的三角形, 生成的法线应指向 +Z
    for (SizeType i = 0; i < mesh.GetVertexCount(); ++i)
    {
        LIMX_EXPECT_NEAR(mesh.Vertices[i].Normal.Z, 1.0f, 1.0e-3f);
    }
}

// ============================================================================
// 材质
// ============================================================================

LIMX_TEST(GltfLoader, ParsesMetallicRoughnessMaterial)
{
    const FString json = MakeTriangleGltf(MakeDataUri(kTightBufferBase64));

    FAssetScene scene;
    LIMX_REQUIRE_TRUE(ParseGltf(json, scene).Succeeded);
    LIMX_REQUIRE_EQ(scene.Materials.GetSize(), SizeType(1));

    const FMaterialData& material = scene.Materials[0];

    LIMX_EXPECT_TRUE(material.Name == "TestMat");

    // glTF 原生即金属粗糙度工作流 — 数值应原样传递, 不做任何换算
    LIMX_EXPECT_NEAR(material.BaseColorFactor.X, 0.8f, kTolerance);
    LIMX_EXPECT_NEAR(material.BaseColorFactor.Y, 0.4f, kTolerance);
    LIMX_EXPECT_NEAR(material.BaseColorFactor.Z, 0.2f, kTolerance);
    LIMX_EXPECT_NEAR(material.MetallicFactor, 0.25f, kTolerance);
    LIMX_EXPECT_NEAR(material.RoughnessFactor, 0.75f, kTolerance);

    LIMX_EXPECT_NEAR(material.EmissiveFactor.X, 0.1f, kTolerance);
    LIMX_EXPECT_NEAR(material.EmissiveFactor.Z, 0.3f, kTolerance);

    LIMX_EXPECT_TRUE(material.DoubleSided);
    LIMX_EXPECT_EQ(static_cast<Int32>(material.AlphaMode),
                   static_cast<Int32>(EAlphaMode::Mask));
    LIMX_EXPECT_NEAR(material.AlphaCutoff, 0.25f, kTolerance);
}

LIMX_TEST(GltfLoader, MaterialDefaultsFollowSpecification)
{
    FStringBuilder builder(1024);

    builder.Append("{\"asset\":{\"version\":\"2.0\"},");
    builder.Append("\"materials\":[{\"name\":\"bare\"}],");
    builder.Append("\"meshes\":[],\"nodes\":[],\"scenes\":[{\"nodes\":[]}],"
                   "\"scene\":0}");

    FAssetScene scene;
    LIMX_REQUIRE_TRUE(ParseGltf(builder.ToString(), scene).Succeeded);
    LIMX_REQUIRE_EQ(scene.Materials.GetSize(), SizeType(1));

    const FMaterialData& material = scene.Materials[0];

    // 规范默认: baseColor 全白, metallic 与 roughness 均为 1
    LIMX_EXPECT_NEAR(material.BaseColorFactor.X, 1.0f, kTolerance);
    LIMX_EXPECT_NEAR(material.BaseColorFactor.W, 1.0f, kTolerance);
    LIMX_EXPECT_NEAR(material.MetallicFactor, 1.0f, kTolerance);
    LIMX_EXPECT_NEAR(material.RoughnessFactor, 1.0f, kTolerance);
    LIMX_EXPECT_FALSE(material.DoubleSided);
    LIMX_EXPECT_EQ(static_cast<Int32>(material.AlphaMode),
                   static_cast<Int32>(EAlphaMode::Opaque));
}

LIMX_TEST(GltfLoader, ResolvesExternalTexturePaths)
{
    FStringBuilder builder(1024);

    builder.Append("{\"asset\":{\"version\":\"2.0\"},");
    builder.Append("\"materials\":[{\"pbrMetallicRoughness\":"
                   "{\"baseColorTexture\":{\"index\":0}},"
                   "\"normalTexture\":{\"index\":1,\"scale\":2.0}}],");
    builder.Append("\"textures\":[{\"source\":0},{\"source\":1}],");
    builder.Append("\"images\":[{\"uri\":\"albedo.png\"},"
                   "{\"uri\":\"normal.png\"}],");
    builder.Append("\"meshes\":[],\"nodes\":[],\"scenes\":[{\"nodes\":[]}],"
                   "\"scene\":0}");

    const FString json = builder.ToString();

    FAssetScene scene;
    const FAssetLoadResult result = FGltfLoader::LoadFromJson(
        json.GetCStr(), json.GetLength(), FString("Models"), scene,
        FGltfLoadOptions());

    LIMX_REQUIRE_TRUE(result.Succeeded);
    LIMX_REQUIRE_EQ(scene.Materials.GetSize(), SizeType(1));

    const FMaterialData& material = scene.Materials[0];

    // texture → image → uri 三级解引用, 且路径要拼上基准目录
    LIMX_EXPECT_STREQ(material.BaseColorTexture.Path.GetCStr(),
                      "Models/albedo.png");
    LIMX_EXPECT_STREQ(material.NormalTexture.Path.GetCStr(),
                      "Models/normal.png");
    LIMX_EXPECT_NEAR(material.NormalScale, 2.0f, kTolerance);
}

// ============================================================================
// 节点层级
// ============================================================================

LIMX_TEST(GltfLoader, BuildsNodeHierarchy)
{
    FStringBuilder builder(1024);

    builder.Append("{\"asset\":{\"version\":\"2.0\"},");
    builder.Append("\"scene\":0,\"scenes\":[{\"nodes\":[0]}],");
    builder.Append("\"nodes\":[");
    builder.Append("{\"name\":\"root\",\"children\":[1,2]},");
    builder.Append("{\"name\":\"childA\"},");
    builder.Append("{\"name\":\"childB\",\"children\":[3]},");
    builder.Append("{\"name\":\"grandchild\"}],");
    builder.Append("\"meshes\":[]}");

    FAssetScene scene;
    LIMX_REQUIRE_TRUE(ParseGltf(builder.ToString(), scene).Succeeded);

    LIMX_REQUIRE_EQ(scene.Nodes.GetSize(), SizeType(4));
    LIMX_REQUIRE_EQ(scene.RootNodes.GetSize(), SizeType(1));
    LIMX_EXPECT_EQ(scene.RootNodes[0], 0);

    // 父子关系双向一致
    LIMX_EXPECT_EQ(scene.Nodes[0].ParentIndex, -1);
    LIMX_EXPECT_EQ(scene.Nodes[0].Children.GetSize(), SizeType(2));

    LIMX_EXPECT_EQ(scene.Nodes[1].ParentIndex, 0);
    LIMX_EXPECT_EQ(scene.Nodes[2].ParentIndex, 0);
    LIMX_EXPECT_EQ(scene.Nodes[3].ParentIndex, 2);

    LIMX_EXPECT_TRUE(scene.Nodes[3].Name == "grandchild");
}

LIMX_TEST(GltfLoader, ParsesTrsTransform)
{
    FStringBuilder builder(1024);

    builder.Append("{\"asset\":{\"version\":\"2.0\"},");
    builder.Append("\"scene\":0,\"scenes\":[{\"nodes\":[0]}],");
    builder.Append("\"nodes\":[{\"translation\":[1.0,2.0,3.0],");
    builder.Append("\"scale\":[2.0,2.0,2.0],");
    builder.Append("\"rotation\":[0.0,0.0,0.0,1.0]}],");
    builder.Append("\"meshes\":[]}");

    FAssetScene scene;
    LIMX_REQUIRE_TRUE(ParseGltf(builder.ToString(), scene).Succeeded);
    LIMX_REQUIRE_EQ(scene.Nodes.GetSize(), SizeType(1));

    const FTransform& transform = scene.Nodes[0].LocalTransform;

    LIMX_EXPECT_NEAR(transform.Translation.X, 1.0f, kTolerance);
    LIMX_EXPECT_NEAR(transform.Translation.Z, 3.0f, kTolerance);
    LIMX_EXPECT_NEAR(transform.Scale3D.X, 2.0f, kTolerance);

    // glTF 四元数按 (x,y,z,w) 排列 — 顺序读错会让单位旋转变成 180 度翻转
    LIMX_EXPECT_NEAR(transform.Rotation.W, 1.0f, 1.0e-3f);
}

LIMX_TEST(GltfLoader, ParsesMatrixTransform)
{
    // 列主序的平移矩阵: 平移分量位于最后一列 (下标 12/13/14)
    FStringBuilder builder(1024);

    builder.Append("{\"asset\":{\"version\":\"2.0\"},");
    builder.Append("\"scene\":0,\"scenes\":[{\"nodes\":[0]}],");
    builder.Append("\"nodes\":[{\"matrix\":[");
    builder.Append("1,0,0,0, 0,1,0,0, 0,0,1,0, 5,6,7,1]}],");
    builder.Append("\"meshes\":[]}");

    FAssetScene scene;
    LIMX_REQUIRE_TRUE(ParseGltf(builder.ToString(), scene).Succeeded);

    const FTransform& transform = scene.Nodes[0].LocalTransform;

    // 列主序读成行主序会把平移读成 (0,0,0)
    LIMX_EXPECT_NEAR(transform.Translation.X, 5.0f, 1.0e-3f);
    LIMX_EXPECT_NEAR(transform.Translation.Y, 6.0f, 1.0e-3f);
    LIMX_EXPECT_NEAR(transform.Translation.Z, 7.0f, 1.0e-3f);
}

LIMX_TEST(GltfLoader, ComposesWorldTransformThroughHierarchy)
{
    FStringBuilder builder(1024);

    builder.Append("{\"asset\":{\"version\":\"2.0\"},");
    builder.Append("\"scene\":0,\"scenes\":[{\"nodes\":[0]}],");
    builder.Append("\"nodes\":[");
    builder.Append("{\"translation\":[10.0,0.0,0.0],\"children\":[1]},");
    builder.Append("{\"translation\":[0.0,5.0,0.0]}],");
    builder.Append("\"meshes\":[]}");

    FAssetScene scene;
    LIMX_REQUIRE_TRUE(ParseGltf(builder.ToString(), scene).Succeeded);

    // 子节点的世界变换应叠加父节点的平移
    const FTransform world = scene.ComputeWorldTransform(1);

    LIMX_EXPECT_NEAR(world.Translation.X, 10.0f, 1.0e-3f);
    LIMX_EXPECT_NEAR(world.Translation.Y, 5.0f, 1.0e-3f);
}

LIMX_TEST(GltfLoader, TreatsParentlessNodesAsRootsWhenSceneOmitsThem)
{
    FStringBuilder builder(1024);

    builder.Append("{\"asset\":{\"version\":\"2.0\"},");
    builder.Append("\"scenes\":[{\"nodes\":[]}],\"scene\":0,");
    builder.Append("\"nodes\":[{\"name\":\"a\"},{\"name\":\"b\"}],");
    builder.Append("\"meshes\":[]}");

    FAssetScene scene;
    LIMX_REQUIRE_TRUE(ParseGltf(builder.ToString(), scene).Succeeded);

    // 场景未声明根节点时, 无父节点者都应被视为根
    LIMX_EXPECT_EQ(scene.RootNodes.GetSize(), SizeType(2));
}

// ============================================================================
// 多图元
// ============================================================================

LIMX_TEST(GltfLoader, MultiplePrimitivesBecomeSubMeshes)
{
    FStringBuilder builder(2048);

    builder.Append("{\"asset\":{\"version\":\"2.0\"},");
    builder.Append("\"scene\":0,\"scenes\":[{\"nodes\":[0]}],");
    builder.Append("\"nodes\":[{\"mesh\":0}],");
    builder.Append("\"meshes\":[{\"primitives\":[");
    builder.Append("{\"attributes\":{\"POSITION\":0},\"indices\":1,"
                   "\"material\":0},");
    builder.Append("{\"attributes\":{\"POSITION\":0},\"indices\":1,"
                   "\"material\":1}]}],");
    builder.Append("\"materials\":[{\"name\":\"m0\"},{\"name\":\"m1\"}],");
    builder.Append("\"accessors\":[");
    builder.Append("{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                   "\"type\":\"VEC3\"},");
    builder.Append("{\"bufferView\":1,\"componentType\":5123,\"count\":3,"
                   "\"type\":\"SCALAR\"}],");
    builder.Append("\"bufferViews\":[");
    builder.Append("{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},");
    builder.Append("{\"buffer\":0,\"byteOffset\":72,\"byteLength\":6}],");
    builder.Append("\"buffers\":[{\"byteLength\":80,\"uri\":\"");
    builder.Append(MakeDataUri(kTightBufferBase64));
    builder.Append("\"}]}");

    FAssetScene scene;
    LIMX_REQUIRE_TRUE(ParseGltf(builder.ToString(), scene).Succeeded);

    const FMeshData& mesh = scene.Meshes[0];

    // 两个图元 → 两个子网格, 各自指向不同材质
    LIMX_REQUIRE_EQ(mesh.SubMeshes.GetSize(), SizeType(2));
    LIMX_EXPECT_EQ(mesh.SubMeshes[0].MaterialIndex, 0);
    LIMX_EXPECT_EQ(mesh.SubMeshes[1].MaterialIndex, 1);

    // 顶点被各图元独立追加, 索引需带上基址偏移
    LIMX_EXPECT_EQ(mesh.GetVertexCount(), SizeType(6));
    LIMX_EXPECT_EQ(mesh.SubMeshes[1].IndexOffset, UInt32(3));

    // 第二个图元的索引应指向后三个顶点
    LIMX_EXPECT_EQ(mesh.Indices[3], UInt32(3));
    LIMX_EXPECT_EQ(mesh.Indices[5], UInt32(5));
}

LIMX_TEST(GltfLoader, SkipsNonTriangleMode)
{
    FStringBuilder builder(1024);

    builder.Append("{\"asset\":{\"version\":\"2.0\"},");
    builder.Append("\"scene\":0,\"scenes\":[{\"nodes\":[0]}],");
    builder.Append("\"nodes\":[{\"mesh\":0}],");
    builder.Append("\"meshes\":[{\"primitives\":[");
    builder.Append("{\"attributes\":{\"POSITION\":0},\"mode\":1}]}],");
    builder.Append("\"accessors\":[{\"bufferView\":0,\"componentType\":5126,"
                   "\"count\":3,\"type\":\"VEC3\"}],");
    builder.Append("\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,"
                   "\"byteLength\":36}],");
    builder.Append("\"buffers\":[{\"byteLength\":80,\"uri\":\"");
    builder.Append(MakeDataUri(kTightBufferBase64));
    builder.Append("\"}]}");

    FAssetScene scene;
    const FAssetLoadResult result = ParseGltf(builder.ToString(), scene);

    // 线段图元被跳过 — 解析仍算成功, 但要留下告警
    LIMX_REQUIRE_TRUE(result.Succeeded);
    LIMX_EXPECT_EQ(scene.Meshes[0].GetVertexCount(), SizeType(0));
    LIMX_EXPECT_GT(result.Warnings.GetSize(), SizeType(0));
}

// ============================================================================
// GLB 容器
// ============================================================================

LIMX_TEST(GlbLoader, ParsesBinaryContainer)
{
    // 缓冲区无 uri — 数据来自 GLB 的二进制块
    const FString json = MakeTriangleGltf(FString());

    TArray<UInt8> binary;
    LIMX_REQUIRE_TRUE(FBase64::Decode(kTightBufferBase64, binary));
    LIMX_REQUIRE_EQ(binary.GetSize(), SizeType(80));

    const TArray<UInt8> glb = BuildGlb(json, binary);

    FAssetScene scene;
    const FAssetLoadResult result = FGltfLoader::LoadFromGlb(
        glb.GetData(), glb.GetSize(), FString(), scene, FGltfLoadOptions());

    LIMX_REQUIRE_TRUE(result.Succeeded);
    LIMX_REQUIRE_EQ(scene.Meshes.GetSize(), SizeType(1));

    const FMeshData& mesh = scene.Meshes[0];

    LIMX_EXPECT_EQ(mesh.GetVertexCount(), SizeType(3));
    LIMX_EXPECT_EQ(mesh.GetTriangleCount(), SizeType(1));
    LIMX_EXPECT_NEAR(mesh.Vertices[1].Position.X, 1.0f, kTolerance);
    LIMX_EXPECT_NEAR(mesh.Vertices[0].Normal.Z, 1.0f, kTolerance);

    // 材质也应从 GLB 内的 JSON 块中解析出来
    LIMX_REQUIRE_EQ(scene.Materials.GetSize(), SizeType(1));
    LIMX_EXPECT_NEAR(scene.Materials[0].MetallicFactor, 0.25f, kTolerance);
}

LIMX_TEST(GlbLoader, RejectsBadMagic)
{
    const FString json = MakeTriangleGltf(FString());

    TArray<UInt8> binary;
    LIMX_REQUIRE_TRUE(FBase64::Decode(kTightBufferBase64, binary));

    TArray<UInt8> glb = BuildGlb(json, binary);
    LIMX_REQUIRE_GT(glb.GetSize(), SizeType(12));

    // 破坏魔数
    glb[0] = 'X';

    FAssetScene scene;
    const FAssetLoadResult result = FGltfLoader::LoadFromGlb(
        glb.GetData(), glb.GetSize(), FString(), scene, FGltfLoadOptions());

    LIMX_EXPECT_FALSE(result.Succeeded);
    LIMX_EXPECT_FALSE(result.ErrorMessage.IsEmpty());
}

LIMX_TEST(GlbLoader, RejectsWrongVersion)
{
    const FString json = MakeTriangleGltf(FString());

    TArray<UInt8> binary;
    LIMX_REQUIRE_TRUE(FBase64::Decode(kTightBufferBase64, binary));

    TArray<UInt8> glb = BuildGlb(json, binary);

    // 版本字段位于偏移 4
    glb[4] = 1;

    FAssetScene scene;
    LIMX_EXPECT_FALSE(FGltfLoader::LoadFromGlb(
        glb.GetData(), glb.GetSize(), FString(), scene,
        FGltfLoadOptions()).Succeeded);
}

LIMX_TEST(GlbLoader, RejectsTruncatedData)
{
    FAssetScene scene;

    const UInt8 tooShort[] = { 0x67, 0x6C, 0x54, 0x46 };

    LIMX_EXPECT_FALSE(FGltfLoader::LoadFromGlb(
        tooShort, sizeof(tooShort), FString(), scene,
        FGltfLoadOptions()).Succeeded);
}

// ============================================================================
// 错误处理
// ============================================================================

LIMX_TEST(GltfLoader, RejectsNonSecondVersion)
{
    FStringBuilder builder(256);
    builder.Append("{\"asset\":{\"version\":\"1.0\"},\"meshes\":[]}");

    FAssetScene scene;
    const FAssetLoadResult result = ParseGltf(builder.ToString(), scene);

    LIMX_EXPECT_FALSE(result.Succeeded);
    LIMX_EXPECT_FALSE(result.ErrorMessage.IsEmpty());
}

LIMX_TEST(GltfLoader, RejectsMalformedJson)
{
    FStringBuilder builder(256);
    builder.Append("{\"asset\":{\"version\":\"2.0\"},");

    FAssetScene scene;
    const FAssetLoadResult result = ParseGltf(builder.ToString(), scene);

    LIMX_EXPECT_FALSE(result.Succeeded);

    // 错误信息应带上 JSON 解析器给出的行号
    LIMX_EXPECT_GT(result.ErrorLocation, UInt32(0));
}

LIMX_TEST(GltfLoader, RejectsAccessorOutOfBounds)
{
    // 访问器声明 100 个元素, 但视图只有 36 字节 (够 3 个 VEC3)
    FStringBuilder builder(1024);

    builder.Append("{\"asset\":{\"version\":\"2.0\"},");
    builder.Append("\"scene\":0,\"scenes\":[{\"nodes\":[0]}],");
    builder.Append("\"nodes\":[{\"mesh\":0}],");
    builder.Append("\"meshes\":[{\"primitives\":[{\"attributes\":"
                   "{\"POSITION\":0}}]}],");
    builder.Append("\"accessors\":[{\"bufferView\":0,\"componentType\":5126,"
                   "\"count\":100,\"type\":\"VEC3\"}],");
    builder.Append("\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,"
                   "\"byteLength\":36}],");
    builder.Append("\"buffers\":[{\"byteLength\":80,\"uri\":\"");
    builder.Append(MakeDataUri(kTightBufferBase64));
    builder.Append("\"}]}");

    FAssetScene scene;
    const FAssetLoadResult result = ParseGltf(builder.ToString(), scene);

    // 越界必须拒绝而非夹紧 —— 夹紧会静默产出扭曲的几何
    LIMX_EXPECT_FALSE(result.Succeeded);
    LIMX_EXPECT_FALSE(result.ErrorMessage.IsEmpty());
}

LIMX_TEST(GltfLoader, RejectsCompressionExtension)
{
    FStringBuilder builder(512);

    builder.Append("{\"asset\":{\"version\":\"2.0\"},");
    builder.Append("\"extensionsRequired\":[\"KHR_draco_mesh_compression\"],");
    builder.Append("\"meshes\":[]}");

    FAssetScene scene;
    const FAssetLoadResult result = ParseGltf(builder.ToString(), scene);

    // 忽略压缩扩展只会产出空几何 — 明确拒绝更诚实
    LIMX_EXPECT_FALSE(result.Succeeded);
}

LIMX_TEST(GltfLoader, RejectsBufferWithoutUriOutsideGlb)
{
    // .gltf 中的缓冲区必须有 uri; 无 uri 只在 GLB 里合法
    const FString json = MakeTriangleGltf(FString());

    FAssetScene scene;
    LIMX_EXPECT_FALSE(ParseGltf(json, scene).Succeeded);
}

LIMX_TEST(GltfLoader, RejectsTruncatedBuffer)
{
    // 声明 80 字节, 实际 data URI 只有 40 字节
    FStringBuilder builder(1024);

    builder.Append("{\"asset\":{\"version\":\"2.0\"},");
    builder.Append("\"meshes\":[],\"nodes\":[],\"scenes\":[{\"nodes\":[]}],"
                   "\"scene\":0,");
    builder.Append("\"buffers\":[{\"byteLength\":80,\"uri\":\"");
    builder.Append(MakeDataUri(kUByteIndexBufferBase64));
    builder.Append("\"}]}");

    FAssetScene scene;
    const FAssetLoadResult result = ParseGltf(builder.ToString(), scene);

    LIMX_EXPECT_FALSE(result.Succeeded);
}

// ============================================================================
// 场景统计
// ============================================================================

LIMX_TEST(GltfLoader, ReportsSceneStatistics)
{
    const FString json = MakeTriangleGltf(MakeDataUri(kTightBufferBase64));

    FAssetScene scene;
    LIMX_REQUIRE_TRUE(ParseGltf(json, scene).Succeeded);

    LIMX_EXPECT_EQ(scene.GetTotalVertexCount(), SizeType(3));
    LIMX_EXPECT_EQ(scene.GetTotalTriangleCount(), SizeType(1));
    LIMX_EXPECT_EQ(scene.GetTotalSubMeshCount(), SizeType(1));
    LIMX_EXPECT_FALSE(scene.IsEmpty());
}

LIMX_TEST(GltfLoader, ResetClearsPreviousScene)
{
    const FString json = MakeTriangleGltf(MakeDataUri(kTightBufferBase64));

    FAssetScene scene;
    LIMX_REQUIRE_TRUE(ParseGltf(json, scene).Succeeded);
    LIMX_REQUIRE_EQ(scene.Meshes.GetSize(), SizeType(1));

    // 二次解析应替换而非追加
    LIMX_REQUIRE_TRUE(ParseGltf(json, scene).Succeeded);
    LIMX_EXPECT_EQ(scene.Meshes.GetSize(), SizeType(1));
    LIMX_EXPECT_EQ(scene.Materials.GetSize(), SizeType(1));
    LIMX_EXPECT_EQ(scene.Nodes.GetSize(), SizeType(1));
}


// ============================================================================
// 多图元装配 — 串行与并行两条分支
//
// FGltfLoader::ParseMeshes 按图元总数选路: 少于 kParallelPrimitiveThreshold
// (32) 走串行, 否则建任务图并行装配。两条分支必须给出完全一致的结果,
// 而线上唯一的 glTF 内容 (Sponza, 103 个图元) 只覆盖得到并行那条。
// ============================================================================

LIMX_TEST(GltfLoader, AssemblesFewPrimitivesSerially)
{
    // 4 个图元 —— 远低于阈值, 必定走串行分支
    const FString json = MakeMultiPrimitiveGltf(4);

    FAssetScene scene;
    const FAssetLoadResult result = ParseGltf(json, scene);

    LIMX_REQUIRE_TRUE(result.Succeeded);
    LIMX_REQUIRE_EQ(scene.Meshes.GetSize(), SizeType(1));

    CheckMultiPrimitive(limxTestContext, scene.Meshes[0], 4);
}

LIMX_TEST(GltfLoader, AssemblesManyPrimitivesInParallel)
{
    // 64 个图元 —— 高于阈值, 必定走并行分支
    const FString json = MakeMultiPrimitiveGltf(64);

    FAssetScene scene;
    const FAssetLoadResult result = ParseGltf(json, scene);

    LIMX_REQUIRE_TRUE(result.Succeeded);
    LIMX_REQUIRE_EQ(scene.Meshes.GetSize(), SizeType(1));

    CheckMultiPrimitive(limxTestContext, scene.Meshes[0], 64);
}

LIMX_TEST(GltfLoader, SerialAndParallelAgreeExactly)
{
    // 跨过阈值的两侧各取一个规模, 逐字节比对两条分支的产物。
    //
    // 上面两个用例各自检查"结果是否自洽", 这一个检查的是"两条分支是否
    // 给出同一个答案" —— 前者能同时被两条分支上同一个错误骗过 (比如两
    // 边都把材质下标算错), 后者不会。
    const SizeType serialCount   = 31;   // 阈值 32 之下一格
    const SizeType parallelCount = 32;   // 恰好触发并行

    FAssetScene serialScene;
    FAssetScene parallelScene;

    LIMX_REQUIRE_TRUE(
        ParseGltf(MakeMultiPrimitiveGltf(serialCount), serialScene).Succeeded);
    LIMX_REQUIRE_TRUE(
        ParseGltf(MakeMultiPrimitiveGltf(parallelCount), parallelScene).Succeeded);

    LIMX_REQUIRE_EQ(serialScene.Meshes.GetSize(), SizeType(1));
    LIMX_REQUIRE_EQ(parallelScene.Meshes.GetSize(), SizeType(1));

    const FMeshData& serialMesh   = serialScene.Meshes[0];
    const FMeshData& parallelMesh = parallelScene.Meshes[0];

    // 先把规模钉死再逐项比对 —— 否则子网格少了一个时下面会越界, 用例
    // 以崩溃收场, 同一个二进制里排在后面的用例就全都跑不到了。
    LIMX_REQUIRE_EQ(serialMesh.SubMeshes.GetSize(), serialCount);
    LIMX_REQUIRE_EQ(parallelMesh.SubMeshes.GetSize(), parallelCount);

    // 并行那份多一个图元, 前 31 个图元的数据必须完全相同
    const SizeType commonVertices = serialCount * 3;

    LIMX_REQUIRE_TRUE(parallelMesh.GetVertexCount() >= commonVertices);

    for (SizeType i = 0; i < commonVertices; ++i)
    {
        LIMX_EXPECT_EQ(serialMesh.Indices[i], parallelMesh.Indices[i]);

        LIMX_EXPECT_NEAR(serialMesh.Vertices[i].Position.X,
                         parallelMesh.Vertices[i].Position.X, kTolerance);
        LIMX_EXPECT_NEAR(serialMesh.Vertices[i].Position.Y,
                         parallelMesh.Vertices[i].Position.Y, kTolerance);
        LIMX_EXPECT_NEAR(serialMesh.Vertices[i].Normal.Z,
                         parallelMesh.Vertices[i].Normal.Z, kTolerance);
    }

    for (SizeType p = 0; p < serialCount; ++p)
    {
        LIMX_EXPECT_EQ(serialMesh.SubMeshes[p].MaterialIndex,
                       parallelMesh.SubMeshes[p].MaterialIndex);
        LIMX_EXPECT_EQ(serialMesh.SubMeshes[p].IndexOffset,
                       parallelMesh.SubMeshes[p].IndexOffset);
    }
}

LIMX_TEST(GltfLoader, ParallelAssemblyIsStableAcrossRuns)
{
    // 并行装配的正确性不能只测一次 —— 顺序错乱是竞态, 而竞态在单次运行
    // 里有很大概率不发作。重复 20 次, 每次都要求完全一致的结果。
    constexpr SizeType kPrimitives = 48;
    constexpr SizeType kRuns       = 20;

    const FString json = MakeMultiPrimitiveGltf(kPrimitives);

    for (SizeType run = 0; run < kRuns; ++run)
    {
        FAssetScene scene;

        LIMX_REQUIRE_TRUE(ParseGltf(json, scene).Succeeded);
        LIMX_REQUIRE_EQ(scene.Meshes.GetSize(), SizeType(1));

        CheckMultiPrimitive(limxTestContext, scene.Meshes[0], kPrimitives);
    }
}
