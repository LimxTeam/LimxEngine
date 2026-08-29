/*******************************************************************************
 * 文件: JsonTests.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   FJson 单元测试 — 标量与容器解析、嵌套结构、转义解码、数字精度、
 *   错误定位、以及对非法输入的拒绝
 *
 * 设计哲学:
 *   嵌套是首要目标 — 解析器的子节点采用"索引池 + scratch 栈"两段式收集，
 *   因为嵌套容器会把自身子节点追加到节点池尾部，父容器的子节点在池中并不相邻。
 *   用例刻意构造 [1,[2],3] 这类交错结构，若索引管理有误会立即读出错误元素。
 *
 *   拒绝也是契约 — 一个接受尾随逗号、注释、前导零的"JSON 解析器"会让上游
 *   格式错误悄悄溜过，直到渲染出错才被发现。因此非法输入的拒绝与合法输入的
 *   接受同等重要，用例两侧都覆盖。
 *
 * 技术特性:
 *   - 覆盖 RFC 8259 全部六种值类型与全部转义序列
 *   - UTF-16 代理对解码校验输出的 UTF-8 字节序列
 *   - 以真实 glTF 片段验证深层链式取值
 *
 * 依赖关系:
 *   内部: CoreTests/CoreTestsMinimal.h
 *
 * 注意事项:
 *   解析器为只读模型 — 不测试序列化输出 (未实现)
 *
 ******************************************************************************/

#include "CoreTests/CoreTestsMinimal.h"
#include "Core/Misc/FJson.h"

using namespace Limx;

// ============================================================================
// 标量
// ============================================================================

LIMX_TEST(Json, ParsesLiterals)
{
    FJsonDocument document;

    LIMX_REQUIRE_TRUE(document.Parse("null"));
    LIMX_EXPECT_TRUE(document.GetRoot().IsNull());

    LIMX_REQUIRE_TRUE(document.Parse("true"));
    LIMX_EXPECT_TRUE(document.GetRoot().IsBool());
    LIMX_EXPECT_TRUE(document.GetRoot().AsBool());

    LIMX_REQUIRE_TRUE(document.Parse("false"));
    LIMX_EXPECT_FALSE(document.GetRoot().AsBool(true));
}

LIMX_TEST(Json, ParsesIntegers)
{
    FJsonDocument document;

    LIMX_REQUIRE_TRUE(document.Parse("42"));
    LIMX_EXPECT_EQ(document.GetRoot().AsInt32(), 42);

    LIMX_REQUIRE_TRUE(document.Parse("-17"));
    LIMX_EXPECT_EQ(document.GetRoot().AsInt32(), -17);

    LIMX_REQUIRE_TRUE(document.Parse("0"));
    LIMX_EXPECT_EQ(document.GetRoot().AsInt32(), 0);
}

LIMX_TEST(Json, ParsesFloatingPoint)
{
    FJsonDocument document;

    LIMX_REQUIRE_TRUE(document.Parse("3.14159"));
    LIMX_EXPECT_NEAR(document.GetRoot().AsDouble(), 3.14159, 1.0e-12);

    LIMX_REQUIRE_TRUE(document.Parse("-0.5"));
    LIMX_EXPECT_NEAR(document.GetRoot().AsDouble(), -0.5, 1.0e-15);

    LIMX_REQUIRE_TRUE(document.Parse("1.0e3"));
    LIMX_EXPECT_NEAR(document.GetRoot().AsDouble(), 1000.0, 1.0e-9);

    LIMX_REQUIRE_TRUE(document.Parse("2.5E-3"));
    LIMX_EXPECT_NEAR(document.GetRoot().AsDouble(), 0.0025, 1.0e-15);

    LIMX_REQUIRE_TRUE(document.Parse("1e+2"));
    LIMX_EXPECT_NEAR(document.GetRoot().AsDouble(), 100.0, 1.0e-9);
}

LIMX_TEST(Json, PreservesDoublePrecision)
{
    FJsonDocument document;

    // 逐位累加再除以 10^k 的实现会在这类值上明显失真
    LIMX_REQUIRE_TRUE(document.Parse("0.1"));
    LIMX_EXPECT_NEAR(document.GetRoot().AsDouble(), 0.1, 1.0e-16);

    LIMX_REQUIRE_TRUE(document.Parse("123456789.123456789"));
    LIMX_EXPECT_NEAR(document.GetRoot().AsDouble(), 123456789.123456789, 1.0e-6);

    // 典型的顶点坐标精度
    LIMX_REQUIRE_TRUE(document.Parse("-0.0078125"));
    LIMX_EXPECT_NEAR(document.GetRoot().AsDouble(), -0.0078125, 1.0e-17);
}

LIMX_TEST(Json, ParsesStrings)
{
    FJsonDocument document;

    LIMX_REQUIRE_TRUE(document.Parse("\"limx\""));
    LIMX_EXPECT_TRUE(document.GetRoot().IsString());
    LIMX_EXPECT_STREQ(document.GetRoot().AsString(), "limx");
    LIMX_EXPECT_EQ(document.GetRoot().GetStringLength(), SizeType(4));

    LIMX_REQUIRE_TRUE(document.Parse("\"\""));
    LIMX_EXPECT_STREQ(document.GetRoot().AsString(), "");
    LIMX_EXPECT_EQ(document.GetRoot().GetStringLength(), SizeType(0));
}

// ============================================================================
// 转义
// ============================================================================

LIMX_TEST(Json, DecodesSimpleEscapes)
{
    FJsonDocument document;

    LIMX_REQUIRE_TRUE(document.Parse("\"a\\nb\\tc\""));
    LIMX_EXPECT_STREQ(document.GetRoot().AsString(), "a\nb\tc");

    LIMX_REQUIRE_TRUE(document.Parse("\"\\\"quoted\\\"\""));
    LIMX_EXPECT_STREQ(document.GetRoot().AsString(), "\"quoted\"");

    LIMX_REQUIRE_TRUE(document.Parse("\"back\\\\slash\""));
    LIMX_EXPECT_STREQ(document.GetRoot().AsString(), "back\\slash");

    LIMX_REQUIRE_TRUE(document.Parse("\"for\\/ward\""));
    LIMX_EXPECT_STREQ(document.GetRoot().AsString(), "for/ward");
}

LIMX_TEST(Json, DecodesUnicodeEscapeToUtf8)
{
    FJsonDocument document;

    // U+00E9 (é) 应编码为两字节 0xC3 0xA9
    LIMX_REQUIRE_TRUE(document.Parse("\"\\u00e9\""));

    const AnsiChar* text = document.GetRoot().AsString();
    LIMX_REQUIRE_EQ(document.GetRoot().GetStringLength(), SizeType(2));
    LIMX_EXPECT_EQ(static_cast<UInt8>(text[0]), UInt8(0xC3));
    LIMX_EXPECT_EQ(static_cast<UInt8>(text[1]), UInt8(0xA9));

    // U+4E2D (中) 应编码为三字节 0xE4 0xB8 0xAD
    LIMX_REQUIRE_TRUE(document.Parse("\"\\u4e2d\""));
    text = document.GetRoot().AsString();
    LIMX_REQUIRE_EQ(document.GetRoot().GetStringLength(), SizeType(3));
    LIMX_EXPECT_EQ(static_cast<UInt8>(text[0]), UInt8(0xE4));
    LIMX_EXPECT_EQ(static_cast<UInt8>(text[1]), UInt8(0xB8));
    LIMX_EXPECT_EQ(static_cast<UInt8>(text[2]), UInt8(0xAD));
}

LIMX_TEST(Json, DecodesSurrogatePair)
{
    FJsonDocument document;

    // U+1F600 由代理对 D83D DE00 表示, UTF-8 为四字节 F0 9F 98 80
    LIMX_REQUIRE_TRUE(document.Parse("\"\\ud83d\\ude00\""));

    const AnsiChar* text = document.GetRoot().AsString();
    LIMX_REQUIRE_EQ(document.GetRoot().GetStringLength(), SizeType(4));
    LIMX_EXPECT_EQ(static_cast<UInt8>(text[0]), UInt8(0xF0));
    LIMX_EXPECT_EQ(static_cast<UInt8>(text[1]), UInt8(0x9F));
    LIMX_EXPECT_EQ(static_cast<UInt8>(text[2]), UInt8(0x98));
    LIMX_EXPECT_EQ(static_cast<UInt8>(text[3]), UInt8(0x80));
}

LIMX_TEST(Json, RejectsUnpairedSurrogate)
{
    FJsonDocument document;

    // 孤立的高位代理
    LIMX_EXPECT_FALSE(document.Parse("\"\\ud83d\""));

    // 孤立的低位代理
    LIMX_EXPECT_FALSE(document.Parse("\"\\ude00\""));
}

// ============================================================================
// 数组
// ============================================================================

LIMX_TEST(Json, ParsesFlatArray)
{
    FJsonDocument document;
    LIMX_REQUIRE_TRUE(document.Parse("[1, 2, 3]"));

    FJsonValue root = document.GetRoot();
    LIMX_REQUIRE_TRUE(root.IsArray());
    LIMX_REQUIRE_EQ(root.GetArraySize(), SizeType(3));

    LIMX_EXPECT_EQ(root[SizeType(0)].AsInt32(), 1);
    LIMX_EXPECT_EQ(root[SizeType(1)].AsInt32(), 2);
    LIMX_EXPECT_EQ(root[SizeType(2)].AsInt32(), 3);
}

LIMX_TEST(Json, ParsesEmptyArray)
{
    FJsonDocument document;
    LIMX_REQUIRE_TRUE(document.Parse("[]"));

    LIMX_EXPECT_TRUE(document.GetRoot().IsArray());
    LIMX_EXPECT_EQ(document.GetRoot().GetArraySize(), SizeType(0));
}

LIMX_TEST(Json, ParsesInterleavedNestedArray)
{
    FJsonDocument document;

    // 这是子索引池设计的关键用例: 内层数组会把自身元素追加到节点池尾部,
    // 使外层三个元素在节点池中并不相邻 (分别是节点 1、2、4)
    LIMX_REQUIRE_TRUE(document.Parse("[1, [2], 3]"));

    FJsonValue root = document.GetRoot();
    LIMX_REQUIRE_EQ(root.GetArraySize(), SizeType(3));

    LIMX_EXPECT_EQ(root[SizeType(0)].AsInt32(), 1);
    LIMX_EXPECT_EQ(root[SizeType(2)].AsInt32(), 3);

    FJsonValue inner = root[SizeType(1)];
    LIMX_REQUIRE_TRUE(inner.IsArray());
    LIMX_REQUIRE_EQ(inner.GetArraySize(), SizeType(1));
    LIMX_EXPECT_EQ(inner[SizeType(0)].AsInt32(), 2);
}

LIMX_TEST(Json, ParsesDeeplyInterleavedStructure)
{
    FJsonDocument document;

    LIMX_REQUIRE_TRUE(document.Parse(
        "[10, [20, [30, [40]], 21], 11, [12]]"));

    FJsonValue root = document.GetRoot();
    LIMX_REQUIRE_EQ(root.GetArraySize(), SizeType(4));

    LIMX_EXPECT_EQ(root[SizeType(0)].AsInt32(), 10);
    LIMX_EXPECT_EQ(root[SizeType(2)].AsInt32(), 11);

    FJsonValue level1 = root[SizeType(1)];
    LIMX_REQUIRE_EQ(level1.GetArraySize(), SizeType(3));
    LIMX_EXPECT_EQ(level1[SizeType(0)].AsInt32(), 20);
    LIMX_EXPECT_EQ(level1[SizeType(2)].AsInt32(), 21);

    FJsonValue level2 = level1[SizeType(1)];
    LIMX_REQUIRE_EQ(level2.GetArraySize(), SizeType(2));
    LIMX_EXPECT_EQ(level2[SizeType(0)].AsInt32(), 30);

    FJsonValue level3 = level2[SizeType(1)];
    LIMX_REQUIRE_EQ(level3.GetArraySize(), SizeType(1));
    LIMX_EXPECT_EQ(level3[SizeType(0)].AsInt32(), 40);

    FJsonValue last = root[SizeType(3)];
    LIMX_REQUIRE_EQ(last.GetArraySize(), SizeType(1));
    LIMX_EXPECT_EQ(last[SizeType(0)].AsInt32(), 12);
}

LIMX_TEST(Json, ArrayIndexOutOfRangeReturnsInvalid)
{
    FJsonDocument document;
    LIMX_REQUIRE_TRUE(document.Parse("[1]"));

    LIMX_EXPECT_FALSE(document.GetRoot()[SizeType(5)].IsValid());
    LIMX_EXPECT_EQ(document.GetRoot()[SizeType(5)].AsInt32(-1), -1);
}

// ============================================================================
// 对象
// ============================================================================

LIMX_TEST(Json, ParsesFlatObject)
{
    FJsonDocument document;
    LIMX_REQUIRE_TRUE(document.Parse(
        "{\"name\": \"limx\", \"version\": 2, \"active\": true}"));

    FJsonValue root = document.GetRoot();
    LIMX_REQUIRE_TRUE(root.IsObject());
    LIMX_REQUIRE_EQ(root.GetMemberCount(), SizeType(3));

    LIMX_EXPECT_STREQ(root.GetStringField("name"), "limx");
    LIMX_EXPECT_EQ(root.GetInt32Field("version"), 2);
    LIMX_EXPECT_TRUE(root.GetBoolField("active"));
}

LIMX_TEST(Json, ParsesEmptyObject)
{
    FJsonDocument document;
    LIMX_REQUIRE_TRUE(document.Parse("{}"));

    LIMX_EXPECT_TRUE(document.GetRoot().IsObject());
    LIMX_EXPECT_EQ(document.GetRoot().GetMemberCount(), SizeType(0));
}

LIMX_TEST(Json, MissingMemberReturnsInvalid)
{
    FJsonDocument document;
    LIMX_REQUIRE_TRUE(document.Parse("{\"a\": 1}"));

    FJsonValue root = document.GetRoot();

    LIMX_EXPECT_TRUE(root.HasMember("a"));
    LIMX_EXPECT_FALSE(root.HasMember("b"));
    LIMX_EXPECT_FALSE(root["b"].IsValid());

    // 缺失字段应返回调用方给的默认值
    LIMX_EXPECT_EQ(root.GetInt32Field("b", 99), 99);
    LIMX_EXPECT_STREQ(root.GetStringField("b", "fallback"), "fallback");
}

LIMX_TEST(Json, EnumeratesMembersByIndex)
{
    FJsonDocument document;
    LIMX_REQUIRE_TRUE(document.Parse("{\"x\": 1, \"y\": 2}"));

    FJsonValue root = document.GetRoot();
    LIMX_REQUIRE_EQ(root.GetMemberCount(), SizeType(2));

    // 成员顺序应与源文本一致
    LIMX_EXPECT_STREQ(root.GetMemberName(0), "x");
    LIMX_EXPECT_EQ(root.GetMemberValue(0).AsInt32(), 1);
    LIMX_EXPECT_STREQ(root.GetMemberName(1), "y");
    LIMX_EXPECT_EQ(root.GetMemberValue(1).AsInt32(), 2);
}

LIMX_TEST(Json, ParsesNestedObjectsAndArrays)
{
    FJsonDocument document;
    LIMX_REQUIRE_TRUE(document.Parse(
        "{"
        "  \"mesh\": {"
        "    \"name\": \"cube\","
        "    \"vertices\": [0.0, 1.0, 2.0],"
        "    \"submeshes\": ["
        "      {\"material\": 0, \"count\": 36},"
        "      {\"material\": 1, \"count\": 12}"
        "    ]"
        "  }"
        "}"));

    FJsonValue mesh = document.GetRoot()["mesh"];
    LIMX_REQUIRE_TRUE(mesh.IsObject());
    LIMX_EXPECT_STREQ(mesh.GetStringField("name"), "cube");

    FJsonValue vertices = mesh["vertices"];
    LIMX_REQUIRE_EQ(vertices.GetArraySize(), SizeType(3));
    LIMX_EXPECT_NEAR(vertices[SizeType(2)].AsFloat(), 2.0f, 1.0e-6f);

    FJsonValue submeshes = mesh["submeshes"];
    LIMX_REQUIRE_EQ(submeshes.GetArraySize(), SizeType(2));
    LIMX_EXPECT_EQ(submeshes[SizeType(0)].GetInt32Field("count"), 36);
    LIMX_EXPECT_EQ(submeshes[SizeType(1)].GetInt32Field("material"), 1);
}

// ============================================================================
// 类型不匹配的容错
// ============================================================================

LIMX_TEST(Json, TypeMismatchReturnsDefault)
{
    FJsonDocument document;
    LIMX_REQUIRE_TRUE(document.Parse("{\"text\": \"hello\", \"num\": 5}"));

    FJsonValue root = document.GetRoot();

    // 把字符串当数字读应返回默认值而非崩溃
    LIMX_EXPECT_EQ(root.GetInt32Field("text", -1), -1);

    // 把数字当字符串读同理
    LIMX_EXPECT_STREQ(root.GetStringField("num", "none"), "none");

    // 对标量做容器操作应安全返回空
    LIMX_EXPECT_EQ(root["num"].GetArraySize(), SizeType(0));
    LIMX_EXPECT_EQ(root["num"].GetMemberCount(), SizeType(0));
}

LIMX_TEST(Json, InvalidValueIsSafeToChain)
{
    FJsonDocument document;
    LIMX_REQUIRE_TRUE(document.Parse("{\"a\": 1}"));

    // 对不存在的路径继续链式取值不得崩溃
    FJsonValue deep = document.GetRoot()["missing"]["also"][SizeType(3)]["gone"];

    LIMX_EXPECT_FALSE(deep.IsValid());
    LIMX_EXPECT_EQ(deep.AsInt32(7), 7);
}

// ============================================================================
// 非法输入的拒绝
// ============================================================================

LIMX_TEST(Json, RejectsTrailingComma)
{
    FJsonDocument document;

    LIMX_EXPECT_FALSE(document.Parse("[1, 2, ]"));
    LIMX_EXPECT_FALSE(document.Parse("{\"a\": 1, }"));
}

LIMX_TEST(Json, RejectsComments)
{
    FJsonDocument document;

    // JSON 不含注释 — 接受它会让 JSON5 内容悄悄溜过
    LIMX_EXPECT_FALSE(document.Parse("{\"a\": 1} // comment"));
    LIMX_EXPECT_FALSE(document.Parse("/* c */ {}"));
}

LIMX_TEST(Json, RejectsLeadingZero)
{
    FJsonDocument document;

    LIMX_EXPECT_FALSE(document.Parse("012"));
    LIMX_EXPECT_FALSE(document.Parse("-01"));

    // 但 0 与 0.5 合法
    LIMX_EXPECT_TRUE(document.Parse("0"));
    LIMX_EXPECT_TRUE(document.Parse("0.5"));
}

LIMX_TEST(Json, RejectsMalformedNumbers)
{
    FJsonDocument document;

    LIMX_EXPECT_FALSE(document.Parse("1."));
    LIMX_EXPECT_FALSE(document.Parse(".5"));
    LIMX_EXPECT_FALSE(document.Parse("1e"));
    LIMX_EXPECT_FALSE(document.Parse("1e+"));
    LIMX_EXPECT_FALSE(document.Parse("-"));
}

LIMX_TEST(Json, RejectsUnterminatedConstructs)
{
    FJsonDocument document;

    LIMX_EXPECT_FALSE(document.Parse("\"unterminated"));
    LIMX_EXPECT_FALSE(document.Parse("[1, 2"));
    LIMX_EXPECT_FALSE(document.Parse("{\"a\": 1"));
    LIMX_EXPECT_FALSE(document.Parse("{\"a\""));
}

LIMX_TEST(Json, RejectsTrailingContent)
{
    FJsonDocument document;

    LIMX_EXPECT_FALSE(document.Parse("{} extra"));
    LIMX_EXPECT_FALSE(document.Parse("1 2"));

    // 但尾随空白合法
    LIMX_EXPECT_TRUE(document.Parse("{}   \n\t "));
}

LIMX_TEST(Json, RejectsRawControlCharacterInString)
{
    FJsonDocument document;

    // 字符串内的裸换行必须被拒绝
    const AnsiChar source[] = { '"', 'a', '\n', 'b', '"', '\0' };
    LIMX_EXPECT_FALSE(document.Parse(source));
}

LIMX_TEST(Json, RejectsEmptyInput)
{
    FJsonDocument document;

    LIMX_EXPECT_FALSE(document.Parse(""));
    LIMX_EXPECT_FALSE(document.Parse("   "));
}

LIMX_TEST(Json, RejectsExcessiveNesting)
{
    FJsonDocument document;

    // 构造超过深度上限的嵌套数组
    FString source;
    for (UInt32 i = 0; i < FJsonDocument::kMaxDepth + 10; ++i)
    {
        source.AppendChar('[');
    }
    for (UInt32 i = 0; i < FJsonDocument::kMaxDepth + 10; ++i)
    {
        source.AppendChar(']');
    }

    LIMX_EXPECT_FALSE(document.Parse(source.GetCStr()));
}

// ============================================================================
// 错误定位
// ============================================================================

LIMX_TEST(Json, ReportsErrorLocation)
{
    FJsonDocument document;

    LIMX_REQUIRE_FALSE(document.Parse("{\n  \"a\": 1,\n  \"b\": @\n}"));

    // 错误应定位到第 3 行的非法字符
    LIMX_EXPECT_EQ(document.GetErrorLine(), UInt32(3));
    LIMX_EXPECT_GT(document.GetErrorColumn(), UInt32(1));
    LIMX_EXPECT_FALSE(document.GetErrorMessage().IsEmpty());
}

LIMX_TEST(Json, SuccessfulParseHasNoError)
{
    FJsonDocument document;

    LIMX_REQUIRE_TRUE(document.Parse("{\"ok\": true}"));

    LIMX_EXPECT_TRUE(document.GetErrorMessage().IsEmpty());
    LIMX_EXPECT_EQ(document.GetErrorLine(), UInt32(0));
}

// ============================================================================
// 实际使用形态
// ============================================================================

LIMX_TEST(Json, SkipsUtf8Bom)
{
    FJsonDocument document;

    // 不少工具会在 JSON 文件开头写入 BOM
    const AnsiChar source[] = {
        static_cast<AnsiChar>(0xEF), static_cast<AnsiChar>(0xBB),
        static_cast<AnsiChar>(0xBF), '{', '"', 'a', '"', ':', '1', '}', '\0'
    };

    LIMX_REQUIRE_TRUE(document.Parse(source));
    LIMX_EXPECT_EQ(document.GetRoot().GetInt32Field("a"), 1);
}

LIMX_TEST(Json, ParsesGltfLikeDocument)
{
    FJsonDocument document;

    LIMX_REQUIRE_TRUE(document.Parse(
        "{"
        "  \"asset\": {\"version\": \"2.0\", \"generator\": \"Limx\"},"
        "  \"scene\": 0,"
        "  \"scenes\": [{\"nodes\": [0, 1]}],"
        "  \"nodes\": ["
        "    {\"mesh\": 0, \"translation\": [1.0, 2.0, 3.0]},"
        "    {\"mesh\": 1, \"rotation\": [0.0, 0.0, 0.0, 1.0]}"
        "  ],"
        "  \"meshes\": ["
        "    {\"primitives\": [{\"attributes\": {\"POSITION\": 0, \"NORMAL\": 1},"
        "                      \"indices\": 2, \"material\": 0}]}"
        "  ],"
        "  \"accessors\": ["
        "    {\"bufferView\": 0, \"componentType\": 5126, \"count\": 24,"
        "     \"type\": \"VEC3\"}"
        "  ]"
        "}"));

    FJsonValue root = document.GetRoot();

    LIMX_EXPECT_STREQ(root["asset"].GetStringField("version"), "2.0");
    LIMX_EXPECT_EQ(root.GetInt32Field("scene"), 0);

    // 深层链式取值 — glTF 解析的典型访问形态
    FJsonValue primitive =
        root["meshes"][SizeType(0)]["primitives"][SizeType(0)];

    LIMX_REQUIRE_TRUE(primitive.IsObject());
    LIMX_EXPECT_EQ(primitive["attributes"].GetInt32Field("POSITION"), 0);
    LIMX_EXPECT_EQ(primitive["attributes"].GetInt32Field("NORMAL"), 1);
    LIMX_EXPECT_EQ(primitive.GetInt32Field("indices"), 2);

    FJsonValue accessor = root["accessors"][SizeType(0)];
    LIMX_EXPECT_EQ(accessor.GetInt32Field("componentType"), 5126);
    LIMX_EXPECT_STREQ(accessor.GetStringField("type"), "VEC3");

    FJsonValue translation = root["nodes"][SizeType(0)]["translation"];
    LIMX_REQUIRE_EQ(translation.GetArraySize(), SizeType(3));
    LIMX_EXPECT_NEAR(translation[SizeType(1)].AsFloat(), 2.0f, 1.0e-6f);
}

LIMX_TEST(Json, ResetClearsDocument)
{
    FJsonDocument document;
    LIMX_REQUIRE_TRUE(document.Parse("{\"a\": 1}"));
    LIMX_REQUIRE_TRUE(document.IsValid());

    document.Reset();

    LIMX_EXPECT_FALSE(document.IsValid());
    LIMX_EXPECT_FALSE(document.GetRoot().IsValid());
    LIMX_EXPECT_EQ(document.GetNodeCount(), SizeType(0));
}

LIMX_TEST(Json, ReparseReplacesPreviousContent)
{
    FJsonDocument document;

    LIMX_REQUIRE_TRUE(document.Parse("{\"first\": 1}"));
    LIMX_REQUIRE_TRUE(document.Parse("{\"second\": 2}"));

    LIMX_EXPECT_FALSE(document.GetRoot().HasMember("first"));
    LIMX_EXPECT_TRUE(document.GetRoot().HasMember("second"));
    LIMX_EXPECT_EQ(document.GetRoot().GetInt32Field("second"), 2);
}

LIMX_TEST(Json, HandlesLargeArrayOfNumbers)
{
    FJsonDocument document;

    // 模拟 glTF 内联的顶点数据规模
    FStringBuilder builder(64 * 1024);
    builder.Append("[");

    const Int32 kCount = 4096;
    for (Int32 i = 0; i < kCount; ++i)
    {
        if (i > 0)
        {
            builder.Append(",");
        }

        builder.AppendFloat(static_cast<Float64>(i) * 0.25, 4);
    }

    builder.Append("]");

    const FString source = builder.ToString();
    LIMX_REQUIRE_TRUE(document.Parse(source.GetCStr(), source.GetLength()));

    FJsonValue root = document.GetRoot();
    LIMX_REQUIRE_EQ(root.GetArraySize(), static_cast<SizeType>(kCount));

    LIMX_EXPECT_NEAR(root[SizeType(0)].AsDouble(), 0.0, 1.0e-9);
    LIMX_EXPECT_NEAR(root[SizeType(100)].AsDouble(), 25.0, 1.0e-9);
    LIMX_EXPECT_NEAR(root[SizeType(kCount - 1)].AsDouble(),
                     static_cast<Float64>(kCount - 1) * 0.25, 1.0e-9);
}
