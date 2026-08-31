/*******************************************************************************
 * 文件: DdsDecoderTests.cpp
 * 创建时间: 2026-08-30
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   DDS (DX10) 块压缩纹理读取器单元测试 — 头部字段校验、DXGI 格式映射、
 *   块尺寸算术、mip 链对账, 以及与 Programs/lat 写出侧的真实往返
 *
 * 设计哲学:
 *   拒绝路径必须逐条设例 — 这个解码器的价值有一大半在于"该失败的时候
 *   真的失败"。纹理数组被当成 2D 读、载荷被截断却读出半张图、认不出的
 *   DXGI 码退化成默认格式 —— 这三类错误都不会崩、不会报警, 只会让画面
 *   微妙地不对。因此每一条 ensure 都从一份**合法**字节流出发只改一个
 *   字段, 若哪天有人把检查删掉, 对应用例立刻变红。
 *
 *   块尺寸必须用非 4 倍数尺寸验证 — (w/4)*(h/4) 与 ceil 在 4 的倍数上
 *   结果完全一致, 只用 256x256 之类的尺寸做断言等于没有区分能力。
 *   13x7 与 300x173 这类形状下, 少算一整排块会让其后每一层的偏移都错位。
 *
 *   真实产物做往返 — 合成的字节流只能证明"解析器与我对头部布局的理解
 *   一致", 证明不了"与 lat 一致"。因此另有两份由 lat 实际烘出来的 DDS
 *   以 base64 内嵌: 一份 BC1/sRGB 的 16x16, 一份 BC5/线性的 13x7。
 *   写出侧改了布局, 这两条会立刻失败。
 *
 * 技术特性:
 *   - 合成夹具按 DX10 DDS 规范逐字段拼装, 字段偏移与 dds.rs 的写出顺序对应
 *   - 每层载荷填不同的字节, 使"层偏移算错"能被内容比对直接抓住
 *   - DXGI 映射表逐码核对, 并显式核对相邻的 TYPELESS 码必须被拒绝
 *
 * 依赖关系:
 *   内部: AssetTests/AssetTestsMinimal.h
 *
 * 注意事项:
 *   两份 base64 夹具由 lat 0.x 烘出, 对应源图见 make_fixtures 的描述:
 *   16x16 彩色棋盘 与 13x7 的 R/G 渐变。重新烘焙时须同步更新逐层断言。
 *
 ******************************************************************************/

#include "AssetTests/AssetTestsMinimal.h"

using namespace Limx;

namespace
{

// ============================================================================
// 头部字段偏移 — 与 FDdsDecoder.cpp / dds.rs 一致
// ============================================================================

constexpr SizeType kOffsetMagic             = 0;
constexpr SizeType kOffsetHeaderSize        = 4;
constexpr SizeType kOffsetFlags             = 8;
constexpr SizeType kOffsetHeight            = 12;
constexpr SizeType kOffsetWidth             = 16;
constexpr SizeType kOffsetLinearSize        = 20;
constexpr SizeType kOffsetMipMapCount       = 28;
constexpr SizeType kOffsetPixelFormatSize   = 76;
constexpr SizeType kOffsetPixelFormatFlags  = 80;
constexpr SizeType kOffsetFourCc            = 84;
constexpr SizeType kOffsetDxgiFormat        = 128;
constexpr SizeType kOffsetResourceDimension = 132;
constexpr SizeType kOffsetMiscFlag          = 136;
constexpr SizeType kOffsetArraySize         = 140;

constexpr SizeType kHeaderBytes = FDdsDecoder::kHeaderByteSize;

constexpr UInt32 kDxgiBc1Unorm     = 71;
constexpr UInt32 kDxgiBc1UnormSrgb = 72;
constexpr UInt32 kDxgiBc3UnormSrgb = 78;
constexpr UInt32 kDxgiBc5Unorm     = 83;

// ============================================================================
// 合成夹具
// ============================================================================

void WriteUInt32(TArray<UInt8>& bytes, SizeType offset, UInt32 value)
{
    bytes[offset]     = static_cast<UInt8>(value & 0xFFu);
    bytes[offset + 1] = static_cast<UInt8>((value >> 8) & 0xFFu);
    bytes[offset + 2] = static_cast<UInt8>((value >> 16) & 0xFFu);
    bytes[offset + 3] = static_cast<UInt8>((value >> 24) & 0xFFu);
}

UInt32 NextExtent(UInt32 extent)
{
    return (extent > 1u) ? (extent >> 1) : 1u;
}

/// 一层的字节数 — 测试自己算一遍, 不复用被测实现
///
/// 复用 ComputeBlockCompressionLevelSize 的话, 把它改成向下取整会让
/// 期望值跟着一起错, 用例仍然通过 —— 那就等于没有测。
SizeType ExpectedLevelBytes(UInt32 width, UInt32 height, UInt32 blockBytes)
{
    const SizeType blocksX = (static_cast<SizeType>(width) + 3u) / 4u;
    const SizeType blocksY = (static_cast<SizeType>(height) + 3u) / 4u;

    return blocksX * blocksY * static_cast<SizeType>(blockBytes);
}

/// 拼一份合法的 DX10 DDS
///
/// 第 i 层的载荷全部填 (0xA0 + i), 这样"层偏移算错"能被内容比对抓住 ——
/// 只比长度的话, 相邻两层长度相同时错位是看不出来的。
TArray<UInt8> BuildDds(UInt32 width, UInt32 height, UInt32 dxgiFormat,
                       UInt32 blockBytes, UInt32 mipCount)
{
    TArray<UInt8> bytes;
    bytes.SetSize(kHeaderBytes, static_cast<UInt8>(0));

    // DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_LINEARSIZE
    UInt32 flags = 0x1u | 0x2u | 0x4u | 0x1000u | 0x80000u;
    UInt32 caps  = 0x1000u; // DDSCAPS_TEXTURE

    if (mipCount > 1u)
    {
        flags |= 0x20000u;         // DDSD_MIPMAPCOUNT
        caps  |= 0x400000u | 0x8u; // DDSCAPS_MIPMAP | DDSCAPS_COMPLEX
    }

    const SizeType level0Bytes = ExpectedLevelBytes(width, height, blockBytes);

    WriteUInt32(bytes, kOffsetMagic, 0x20534444u);  // "DDS "
    WriteUInt32(bytes, kOffsetHeaderSize, 124u);
    WriteUInt32(bytes, kOffsetFlags, flags);
    WriteUInt32(bytes, kOffsetHeight, height);
    WriteUInt32(bytes, kOffsetWidth, width);
    WriteUInt32(bytes, kOffsetLinearSize, static_cast<UInt32>(level0Bytes));
    WriteUInt32(bytes, SizeType(24), 0u);           // dwDepth
    WriteUInt32(bytes, kOffsetMipMapCount, mipCount);
    WriteUInt32(bytes, kOffsetPixelFormatSize, 32u);
    WriteUInt32(bytes, kOffsetPixelFormatFlags, 0x4u);  // DDPF_FOURCC
    WriteUInt32(bytes, kOffsetFourCc, 0x30315844u);     // "DX10"
    WriteUInt32(bytes, SizeType(108), caps);            // dwCaps
    WriteUInt32(bytes, kOffsetDxgiFormat, dxgiFormat);
    WriteUInt32(bytes, kOffsetResourceDimension, 3u);   // TEXTURE2D
    WriteUInt32(bytes, kOffsetMiscFlag, 0u);
    WriteUInt32(bytes, kOffsetArraySize, 1u);
    WriteUInt32(bytes, SizeType(144), 0u);              // miscFlags2

    UInt32 levelWidth  = width;
    UInt32 levelHeight = height;

    for (UInt32 level = 0; level < mipCount; ++level)
    {
        const SizeType levelBytes =
            ExpectedLevelBytes(levelWidth, levelHeight, blockBytes);

        const UInt8 fill = static_cast<UInt8>(0xA0u + level);

        for (SizeType i = 0; i < levelBytes; ++i)
        {
            bytes.Add(fill);
        }

        levelWidth  = NextExtent(levelWidth);
        levelHeight = NextExtent(levelHeight);
    }

    return bytes;
}

/// 对照组: 256x256 BC1/sRGB 完整 9 层
TArray<UInt8> ValidBytes()
{
    return BuildDds(256, 256, kDxgiBc1UnormSrgb, 8, 9);
}

FImageDecodeResult DecodeBytes(const TArray<UInt8>& bytes,
                               FCompressedImageData& outImage)
{
    return FDdsDecoder::Decode(bytes.GetData(), bytes.GetSize(), outImage);
}

/// 解码一份 base64 夹具
FImageDecodeResult DecodeBase64(const AnsiChar* base64,
                                FCompressedImageData& outImage,
                                SizeType& outByteCount)
{
    TArray<UInt8> bytes;

    if (!FBase64::Decode(base64, bytes))
    {
        outByteCount = 0;
        return FImageDecodeResult::Failure(FString("base64 夹具本身解码失败"));
    }

    outByteCount = bytes.GetSize();

    return FDdsDecoder::Decode(bytes.GetData(), bytes.GetSize(), outImage);
}

// ============================================================================
// 真实 lat 产物 (base64)
// ============================================================================

/// lat bake src_color_16.png --color-space srgb --format bc1
/// 源图: 16x16 的 4x4 彩色棋盘。332 字节 = 148 头 + 184 载荷
constexpr const AnsiChar* kLatBc1Srgb16x16 =
    "RERTIHwAAAAHEAoAEAAAABAAAACAAAAAAAAAAAUAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACAAAAAEAAAARFgxMAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAACBBAAAAAAAAAAAAAAAAAAAAAAABIAAAAAwAAAAAAAAABAAAA"
    "AAAAAAL5AvkAAAAAnxGfEQAAAAAC+QL5AAAAAJ8RnxEAAAAAnxGfEQAAAAAC+QL5"
    "AAAAAJ8RnxEAAAAAAvkC+QAAAAAC+QL5AAAAAJ8RnxEAAAAAAvkC+QAAAACfEZ8R"
    "AAAAAJ8RnxEAAAAAAvkC+QAAAACfEZ8RAAAAAAL5AvkAAAAAAvmfEVBQBQUC+Z8R"
    "UFAFBQL5nxFQUAUFAvmfEVBQBQUC+Z8RRBFEEVe5V7kAAAAAV7lXuQAAAAA=";

/// lat bake src_normal_13x7.png --color-space linear --format bc5
/// 源图: 13x7 的 R/G 渐变 —— 两个维度都不是 4 的倍数。
/// 340 字节 = 148 头 + 192 载荷
constexpr const AnsiChar* kLatBc5Linear13x7 =
    "RERTIHwAAAAHEAoABwAAAA0AAACAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACAAAAAEAAAARFgxMAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAACBBAAAAAAAAAAAAAAAAAAAAAAABTAAAAAwAAAAAAAAABAAAA"
    "AAAAAJuA8RAP8RAPgE0AsG22nSS/pPEQD/EQD4BNALBttp0k48jxEA/xEA+ATQCw"
    "bbadJOzsSZIkSZIkgE0AsG22nSSbgPEQD/EQDzwaAECSSZIkv6TxEA/xEA88GgBA"
    "kkmSJOPI8RAP8RAPPBoAQJJJkiTs7EmSJEmSJDwaAECSSZIkv4nxEA/xEA9vKwBA"
    "kkmSJOPRARAAARAAbysAQJJJkiTakiEQAiEQAk1NSZIkSZIktrZJkiRJkiRNTUmS"
    "JEmSJA==";

} // namespace

// ============================================================================
// 对照组
// ============================================================================

LIMX_TEST(DdsDecoder, BaselineIsActuallyValid)
{
    // 对照组的对照组: 这条失败的话, 下面所有"必须失败"的用例都失去意义 ——
    // 它们可能是因为别的原因才失败的。
    FCompressedImageData image;
    const FImageDecodeResult result = DecodeBytes(ValidBytes(), image);

    LIMX_REQUIRE_TRUE(result.Succeeded);
    LIMX_EXPECT_TRUE(image.IsValid());
    LIMX_EXPECT_EQ(image.Width, 256u);
    LIMX_EXPECT_EQ(image.Height, 256u);
    LIMX_EXPECT_EQ(image.Format, EBlockCompressionFormat::BC1_SRGB);
    LIMX_EXPECT_EQ(image.GetMipLevelCount(), 9u);
    LIMX_EXPECT_TRUE(image.IsSrgb());
    LIMX_EXPECT_EQ(result.Warnings.GetSize(), SizeType(0));
}

LIMX_TEST(DdsDecoder, IsDdsChecksMagicOnly)
{
    const TArray<UInt8> bytes = ValidBytes();

    LIMX_EXPECT_TRUE(FDdsDecoder::IsDds(bytes.GetData(), bytes.GetSize()));

    // 老式 FourCC 头也是 DDS —— 识别归识别, 拒绝归拒绝。
    // 若 IsDds 顺手把它排除掉, 调用方拿到的会是"无法识别的图像格式",
    // 那句话对排查毫无帮助。
    TArray<UInt8> legacy = bytes;
    WriteUInt32(legacy, kOffsetFourCc, 0x31545844u); // "DXT1"
    LIMX_EXPECT_TRUE(FDdsDecoder::IsDds(legacy.GetData(), legacy.GetSize()));

    const UInt8 notDds[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    LIMX_EXPECT_FALSE(FDdsDecoder::IsDds(notDds, 8));
    LIMX_EXPECT_FALSE(FDdsDecoder::IsDds(nullptr, 0));
    LIMX_EXPECT_FALSE(FDdsDecoder::IsDds(bytes.GetData(), SizeType(3)));
}

// ============================================================================
// 块尺寸算术 — 向上取整
// ============================================================================

LIMX_TEST(DdsDecoder, LevelSizeRoundsBlocksUp)
{
    // 300x173: 173 / 4 = 43.25, 必须抬成 44 块。写成 (h/4) 会得到 43,
    // 少一整排块 —— 而其后每一层的偏移都跟着错位。
    LIMX_EXPECT_EQ(
        ComputeBlockCompressionLevelSize(EBlockCompressionFormat::BC1_UNORM,
                                         300, 173),
        SizeType(75) * 44 * 8);
    LIMX_EXPECT_EQ(
        ComputeBlockCompressionLevelSize(EBlockCompressionFormat::BC5_UNORM,
                                         300, 173),
        SizeType(75) * 44 * 16);

    // 这两个值必须不同, 否则上面的断言根本没有区分能力
    LIMX_EXPECT_NE(SizeType(75) * 44 * 8, SizeType(75) * 43 * 8);

    // 1 像素宽的那一维要被抬成 1 个块而不是 0 个
    LIMX_EXPECT_EQ(
        ComputeBlockCompressionLevelSize(EBlockCompressionFormat::BC3_UNORM,
                                         1, 1000),
        SizeType(1) * 250 * 16);
    LIMX_EXPECT_EQ(
        ComputeBlockCompressionLevelSize(EBlockCompressionFormat::BC4_UNORM,
                                         1000, 1),
        SizeType(250) * 1 * 8);
}

LIMX_TEST(DdsDecoder, SubBlockLevelsStillOccupyOneFullBlock)
{
    // 4x4 以下的 mip 层仍然占满一个块 —— 一条 1024 的 mip 链末端有
    // 三层都是单块。少算它们会让最后几层全部读到别的数据。
    const UInt32 dims[6][2] = { {4,4}, {3,3}, {2,2}, {1,1}, {1,4}, {4,1} };

    for (UInt32 i = 0; i < 6; ++i)
    {
        LIMX_EXPECT_EQ(
            ComputeBlockCompressionLevelSize(EBlockCompressionFormat::BC1_UNORM,
                                             dims[i][0], dims[i][1]),
            SizeType(8));
        LIMX_EXPECT_EQ(
            ComputeBlockCompressionLevelSize(EBlockCompressionFormat::BC5_UNORM,
                                             dims[i][0], dims[i][1]),
            SizeType(16));
    }
}

LIMX_TEST(DdsDecoder, BlockByteSizeMatchesFormatFamily)
{
    LIMX_EXPECT_EQ(
        GetBlockCompressionBlockByteSize(EBlockCompressionFormat::BC1_UNORM), 8u);
    LIMX_EXPECT_EQ(
        GetBlockCompressionBlockByteSize(EBlockCompressionFormat::BC1_SRGB), 8u);
    LIMX_EXPECT_EQ(
        GetBlockCompressionBlockByteSize(EBlockCompressionFormat::BC4_UNORM), 8u);
    LIMX_EXPECT_EQ(
        GetBlockCompressionBlockByteSize(EBlockCompressionFormat::BC4_SNORM), 8u);

    const EBlockCompressionFormat sixteen[10] =
    {
        EBlockCompressionFormat::BC2_UNORM, EBlockCompressionFormat::BC2_SRGB,
        EBlockCompressionFormat::BC3_UNORM, EBlockCompressionFormat::BC3_SRGB,
        EBlockCompressionFormat::BC5_UNORM, EBlockCompressionFormat::BC5_SNORM,
        EBlockCompressionFormat::BC6H_UFLOAT,
        EBlockCompressionFormat::BC6H_SFLOAT,
        EBlockCompressionFormat::BC7_UNORM, EBlockCompressionFormat::BC7_SRGB,
    };

    for (UInt32 i = 0; i < 10; ++i)
    {
        LIMX_EXPECT_EQ(GetBlockCompressionBlockByteSize(sixteen[i]), 16u);
    }

    LIMX_EXPECT_EQ(
        GetBlockCompressionBlockByteSize(EBlockCompressionFormat::Unknown), 0u);
    LIMX_EXPECT_EQ(
        ComputeBlockCompressionLevelSize(EBlockCompressionFormat::Unknown,
                                         64, 64),
        SizeType(0));
}

// ============================================================================
// DXGI 格式映射
// ============================================================================

LIMX_TEST(DdsDecoder, DxgiFormatMappingIsExact)
{
    struct FCase
    {
        UInt32                  Dxgi;
        EBlockCompressionFormat Expected;
    };

    // 逐码核对。打乱其中任意两项 (例如把 77/78 对调) 都会让这条失败 ——
    // 而在画面上, BC3_UNORM 与 BC3_UNORM_SRGB 搞反只表现为"这张贴图
    // 有点发暗", 长度校验完全抓不住。
    const FCase cases[14] =
    {
        { 71, EBlockCompressionFormat::BC1_UNORM },
        { 72, EBlockCompressionFormat::BC1_SRGB },
        { 74, EBlockCompressionFormat::BC2_UNORM },
        { 75, EBlockCompressionFormat::BC2_SRGB },
        { 77, EBlockCompressionFormat::BC3_UNORM },
        { 78, EBlockCompressionFormat::BC3_SRGB },
        { 80, EBlockCompressionFormat::BC4_UNORM },
        { 81, EBlockCompressionFormat::BC4_SNORM },
        { 83, EBlockCompressionFormat::BC5_UNORM },
        { 84, EBlockCompressionFormat::BC5_SNORM },
        { 95, EBlockCompressionFormat::BC6H_UFLOAT },
        { 96, EBlockCompressionFormat::BC6H_SFLOAT },
        { 98, EBlockCompressionFormat::BC7_UNORM },
        { 99, EBlockCompressionFormat::BC7_SRGB },
    };

    for (UInt32 i = 0; i < 14; ++i)
    {
        LIMX_EXPECT_EQ(FDdsDecoder::MapDxgiFormat(cases[i].Dxgi),
                       cases[i].Expected);
    }

    // sRGB 与否是格式的一部分, 两者绝不能相等
    LIMX_EXPECT_NE(FDdsDecoder::MapDxgiFormat(71),
                   FDdsDecoder::MapDxgiFormat(72));
    LIMX_EXPECT_TRUE(IsBlockCompressionSrgb(FDdsDecoder::MapDxgiFormat(72)));
    LIMX_EXPECT_FALSE(IsBlockCompressionSrgb(FDdsDecoder::MapDxgiFormat(71)));

    // BC5 是唯一需要着色器重建 Z 的一族
    LIMX_EXPECT_TRUE(
        IsBlockCompressionTwoChannel(EBlockCompressionFormat::BC5_UNORM));
    LIMX_EXPECT_TRUE(
        IsBlockCompressionTwoChannel(EBlockCompressionFormat::BC5_SNORM));
    LIMX_EXPECT_FALSE(
        IsBlockCompressionTwoChannel(EBlockCompressionFormat::BC1_UNORM));
    LIMX_EXPECT_FALSE(
        IsBlockCompressionTwoChannel(EBlockCompressionFormat::BC3_UNORM));
}

LIMX_TEST(DdsDecoder, DxgiFormatMappingRejectsEverythingElse)
{
    // TYPELESS 变体 (70/73/76/79/82/94/97) 没有确定的解释方式;
    // 非块压缩格式 (28 = R8G8B8A8_UNORM) 根本不该走这条路。
    // 任何一个被"顺手"映射成某个默认格式, 都会产出一张毫无报错的错图。
    const UInt32 rejected[12] =
    { 0, 28, 70, 73, 76, 79, 82, 87, 94, 97, 100, 0xFFFFFFFFu };

    for (UInt32 i = 0; i < 12; ++i)
    {
        LIMX_EXPECT_EQ(FDdsDecoder::MapDxgiFormat(rejected[i]),
                       EBlockCompressionFormat::Unknown);
    }
}

// ============================================================================
// mip 链结构
// ============================================================================

LIMX_TEST(DdsDecoder, MipChainOffsetsArePrefixSums)
{
    FCompressedImageData image;
    LIMX_REQUIRE_TRUE(DecodeBytes(ValidBytes(), image).Succeeded);
    LIMX_REQUIRE_EQ(image.GetMipLevelCount(), 9u);

    SizeType runningOffset = 0;
    UInt32   levelWidth    = 256;

    for (UInt32 level = 0; level < 9u; ++level)
    {
        const FCompressedMipLevel& entry =
            image.Levels[static_cast<SizeType>(level)];

        LIMX_EXPECT_EQ(entry.Width, levelWidth);
        LIMX_EXPECT_EQ(entry.Height, levelWidth);
        LIMX_EXPECT_EQ(entry.ByteOffset, runningOffset);
        LIMX_EXPECT_EQ(entry.ByteSize,
                       ExpectedLevelBytes(levelWidth, levelWidth, 8));

        // 每一层的偏移都必须满足 vkCmdCopyBufferToImage 的对齐要求:
        // 块字节数的整数倍, 且是 4 的整数倍。
        LIMX_EXPECT_EQ(entry.ByteOffset % SizeType(8), SizeType(0));
        LIMX_EXPECT_EQ(entry.ByteOffset % SizeType(4), SizeType(0));

        runningOffset += entry.ByteSize;
        levelWidth = NextExtent(levelWidth);
    }

    LIMX_EXPECT_EQ(image.Data.GetSize(), runningOffset);
    LIMX_EXPECT_EQ(runningOffset, SizeType(43704));
}

LIMX_TEST(DdsDecoder, PerLevelPayloadLandsAtItsOwnOffset)
{
    // 夹具把第 i 层整层填成 (0xA0 + i)。只比长度的话, 相邻层长度相同时
    // 错位是看不出来的; 比内容才能钉住"每一层都落在自己的偏移上"。
    FCompressedImageData image;
    LIMX_REQUIRE_TRUE(DecodeBytes(ValidBytes(), image).Succeeded);

    for (UInt32 level = 0; level < image.GetMipLevelCount(); ++level)
    {
        const FCompressedMipLevel& entry =
            image.Levels[static_cast<SizeType>(level)];

        const UInt8 expected = static_cast<UInt8>(0xA0u + level);

        LIMX_REQUIRE_LE(entry.ByteOffset + entry.ByteSize,
                        image.Data.GetSize());

        bool allMatch = true;

        for (SizeType i = 0; i < entry.ByteSize; ++i)
        {
            if (image.Data[entry.ByteOffset + i] != expected)
            {
                allMatch = false;
                break;
            }
        }

        LIMX_EXPECT_TRUE(allMatch);
    }
}

LIMX_TEST(DdsDecoder, NonPowerOfTwoChainRoundsEveryLevelUp)
{
    // 300x173 的每一层都要各自向上取整, 而不是对基层取整之后逐级折半。
    FCompressedImageData image;
    LIMX_REQUIRE_TRUE(
        DecodeBytes(BuildDds(300, 173, kDxgiBc5Unorm, 16, 9), image).Succeeded);

    LIMX_EXPECT_EQ(image.GetMipLevelCount(), 9u);
    LIMX_EXPECT_EQ(image.Levels[0].Width, 300u);
    LIMX_EXPECT_EQ(image.Levels[0].Height, 173u);
    LIMX_EXPECT_EQ(image.Levels[0].ByteSize, SizeType(75) * 44 * 16);
    LIMX_EXPECT_EQ(image.Levels[8].Width, 1u);
    LIMX_EXPECT_EQ(image.Levels[8].Height, 1u);
    LIMX_EXPECT_EQ(image.Levels[8].ByteSize, SizeType(16));

    // 逐层核对: 150x86 -> 38x22 块, 75x43 -> 19x11 块 ...
    const UInt32 expectedDims[9][2] =
    {
        {300,173}, {150,86}, {75,43}, {37,21}, {18,10},
        {9,5}, {4,2}, {2,1}, {1,1}
    };

    for (UInt32 level = 0; level < 9u; ++level)
    {
        const FCompressedMipLevel& entry =
            image.Levels[static_cast<SizeType>(level)];

        LIMX_EXPECT_EQ(entry.Width, expectedDims[level][0]);
        LIMX_EXPECT_EQ(entry.Height, expectedDims[level][1]);
        LIMX_EXPECT_EQ(entry.ByteSize,
                       ExpectedLevelBytes(expectedDims[level][0],
                                          expectedDims[level][1], 16));
        LIMX_EXPECT_EQ(entry.ByteOffset % SizeType(16), SizeType(0));
    }

    LIMX_EXPECT_TRUE(image.IsValid());
}

LIMX_TEST(DdsDecoder, MipCountZeroMeansSingleLevel)
{
    // dwMipMapCount = 0 的 DDS 在野外确实存在, 语义是"只有第 0 层"。
    TArray<UInt8> bytes = BuildDds(64, 64, kDxgiBc1Unorm, 8, 1);
    WriteUInt32(bytes, kOffsetMipMapCount, 0u);

    FCompressedImageData image;
    LIMX_REQUIRE_TRUE(DecodeBytes(bytes, image).Succeeded);
    LIMX_EXPECT_EQ(image.GetMipLevelCount(), 1u);
    LIMX_EXPECT_EQ(image.Data.GetSize(), SizeType(16) * 16 * 8);
}

LIMX_TEST(DdsDecoder, PartialMipChainIsAccepted)
{
    // 只写前 3 层是合法的 DDS (lat --no-mipmaps 就走这条路)。
    FCompressedImageData image;
    LIMX_REQUIRE_TRUE(
        DecodeBytes(BuildDds(256, 256, kDxgiBc3UnormSrgb, 16, 3), image)
            .Succeeded);

    LIMX_EXPECT_EQ(image.GetMipLevelCount(), 3u);
    LIMX_EXPECT_EQ(image.Format, EBlockCompressionFormat::BC3_SRGB);
    LIMX_EXPECT_EQ(image.Data.GetSize(),
                   SizeType(64) * 64 * 16 + SizeType(32) * 32 * 16 +
                   SizeType(16) * 16 * 16);
}

LIMX_TEST(DdsDecoder, TrailingBytesAreWarnedNotRejected)
{
    // 有些工具在 DDS 尾部塞元数据。数据本身完整可用, 拒绝加载等于让一个
    // 无关的附加块毁掉整张贴图 —— 但也绝不能把它混进纹理。
    TArray<UInt8> bytes = ValidBytes();
    const SizeType payloadBytes = bytes.GetSize() - kHeaderBytes;

    for (UInt32 i = 0; i < 32u; ++i)
    {
        bytes.Add(static_cast<UInt8>(0xEE));
    }

    FCompressedImageData image;
    const FImageDecodeResult result = DecodeBytes(bytes, image);

    LIMX_REQUIRE_TRUE(result.Succeeded);
    LIMX_EXPECT_EQ(result.Warnings.GetSize(), SizeType(1));
    LIMX_EXPECT_EQ(image.Data.GetSize(), payloadBytes);
    LIMX_EXPECT_TRUE(image.IsValid());
}

LIMX_TEST(DdsDecoder, PremultipliedAlphaIsWarnedNotRejected)
{
    // miscFlags2 = 2 (DDS_ALPHA_MODE_PREMULTIPLIED) 表示颜色已经乘过 alpha,
    // 采样后要除回去 —— 引擎当前不做这一步。数据本身是合法的, 拒绝加载
    // 太重; 但静默按直通 alpha 采样会让半透明区域偏暗, 而且没有任何线索。
    TArray<UInt8> bytes = BuildDds(64, 64, kDxgiBc3UnormSrgb, 16, 7);
    WriteUInt32(bytes, SizeType(144), 2u);

    FCompressedImageData image;
    const FImageDecodeResult result = DecodeBytes(bytes, image);

    LIMX_REQUIRE_TRUE(result.Succeeded);
    LIMX_EXPECT_EQ(result.Warnings.GetSize(), SizeType(1));
    LIMX_EXPECT_TRUE(image.IsValid());

    // 对照: 直通 alpha (lat 写出的值) 不该产生任何告警
    TArray<UInt8> straight = BuildDds(64, 64, kDxgiBc3UnormSrgb, 16, 7);
    WriteUInt32(straight, SizeType(144), 1u);

    const FImageDecodeResult straightResult = DecodeBytes(straight, image);

    LIMX_REQUIRE_TRUE(straightResult.Succeeded);
    LIMX_EXPECT_EQ(straightResult.Warnings.GetSize(), SizeType(0));
}

// ============================================================================
// 拒绝路径 — 每条都从一份合法字节流出发, 只改一个字段
// ============================================================================

LIMX_TEST(DdsDecoder, RejectsBadMagic)
{
    TArray<UInt8> bytes = ValidBytes();
    bytes[0] = static_cast<UInt8>('X');

    FCompressedImageData image;
    LIMX_EXPECT_FALSE(DecodeBytes(bytes, image).Succeeded);
    LIMX_EXPECT_FALSE(image.IsValid());
}

LIMX_TEST(DdsDecoder, RejectsTooSmallForHeader)
{
    // 头部就要 148 字节。少一个字节都不能开始读字段, 否则后面每一次
    // 取值都是越界读。
    TArray<UInt8> bytes = ValidBytes();
    bytes.SetSize(kHeaderBytes - 1);

    FCompressedImageData image;
    LIMX_EXPECT_FALSE(DecodeBytes(bytes, image).Succeeded);

    LIMX_EXPECT_FALSE(FDdsDecoder::Decode(nullptr, 0, image).Succeeded);
}

LIMX_TEST(DdsDecoder, RejectsWrongHeaderSize)
{
    TArray<UInt8> bytes = ValidBytes();
    WriteUInt32(bytes, kOffsetHeaderSize, 125u);

    FCompressedImageData image;
    LIMX_EXPECT_FALSE(DecodeBytes(bytes, image).Succeeded);
}

LIMX_TEST(DdsDecoder, RejectsWrongPixelFormatSize)
{
    TArray<UInt8> bytes = ValidBytes();
    WriteUInt32(bytes, kOffsetPixelFormatSize, 24u);

    FCompressedImageData image;
    LIMX_EXPECT_FALSE(DecodeBytes(bytes, image).Succeeded);
}

LIMX_TEST(DdsDecoder, RejectsMissingFourCcFlag)
{
    // 没有 DDPF_FOURCC 说明这是一张未压缩纹理, 后面的 dwFourCC 字段
    // 根本无效 —— 照读会把 RGB 位掩码当成格式码。
    TArray<UInt8> bytes = ValidBytes();
    WriteUInt32(bytes, kOffsetPixelFormatFlags, 0x41u); // DDPF_RGB|ALPHAPIXELS

    FCompressedImageData image;
    LIMX_EXPECT_FALSE(DecodeBytes(bytes, image).Succeeded);
}

LIMX_TEST(DdsDecoder, RejectsLegacyFourCc)
{
    // 老式 FourCC 头没有地方表达 sRGB。同一份 BC1 数据按 UNORM 还是
    // UNORM_SRGB 读, 亮度完全不同, 而猜错不会有任何报错。
    const UInt32 legacy[4] =
    {
        0x31545844u, // "DXT1"
        0x35545844u, // "DXT5"
        0x31495441u, // "ATI1"
        0x32495441u, // "ATI2"
    };

    for (UInt32 i = 0; i < 4; ++i)
    {
        TArray<UInt8> bytes = ValidBytes();
        WriteUInt32(bytes, kOffsetFourCc, legacy[i]);

        FCompressedImageData image;
        LIMX_EXPECT_FALSE(DecodeBytes(bytes, image).Succeeded);
    }
}

LIMX_TEST(DdsDecoder, RejectsNonTexture2DResourceDimension)
{
    // D3D10_RESOURCE_DIMENSION: 0=UNKNOWN 1=BUFFER 2=TEXTURE1D
    // 3=TEXTURE2D 4=TEXTURE3D。除 3 以外一律拒绝 —— 按 2D 读一张 3D
    // 纹理会得到尺寸与内容都不对的图, 且不会有任何报错。
    const UInt32 wrongDimensions[4] = { 0, 1, 2, 4 };

    for (UInt32 i = 0; i < 4; ++i)
    {
        TArray<UInt8> bytes = ValidBytes();
        WriteUInt32(bytes, kOffsetResourceDimension, wrongDimensions[i]);

        FCompressedImageData image;
        LIMX_EXPECT_FALSE(DecodeBytes(bytes, image).Succeeded);
        LIMX_EXPECT_FALSE(image.IsValid());
    }
}

LIMX_TEST(DdsDecoder, RejectsTextureArray)
{
    // 静默只读第一片会让显存统计与采样结果同时错掉。
    const UInt32 arraySizes[3] = { 0, 2, 6 };

    for (UInt32 i = 0; i < 3; ++i)
    {
        TArray<UInt8> bytes = ValidBytes();
        WriteUInt32(bytes, kOffsetArraySize, arraySizes[i]);

        FCompressedImageData image;
        LIMX_EXPECT_FALSE(DecodeBytes(bytes, image).Succeeded);
    }
}

LIMX_TEST(DdsDecoder, RejectsCubemap)
{
    // 立方体贴图的 arraySize 同样是 1, 载荷却是六份 —— 只查 arraySize
    // 的话它会一路通过, 而"文件比期望长"只是告警。
    TArray<UInt8> bytes = ValidBytes();
    WriteUInt32(bytes, kOffsetMiscFlag, 0x4u);

    // 把载荷补成六面, 使"截断检查"不会先一步拦住它 ——
    // 这样这条用例测的确实是立方体贴图检查本身。
    const SizeType oneFace = bytes.GetSize() - kHeaderBytes;

    for (UInt32 face = 1; face < 6u; ++face)
    {
        for (SizeType i = 0; i < oneFace; ++i)
        {
            bytes.Add(static_cast<UInt8>(0x55));
        }
    }

    FCompressedImageData image;
    LIMX_EXPECT_FALSE(DecodeBytes(bytes, image).Succeeded);
}

LIMX_TEST(DdsDecoder, RejectsUnknownDxgiFormat)
{
    // 28 = R8G8B8A8_UNORM (非块压缩), 70 = BC1_TYPELESS (无确定解释)。
    // 退化成某个默认格式的话: 块大小相同的两个格式长度校验完全一致,
    // 出来的是一张颜色全错却毫无报错的图。
    const UInt32 unmappable[4] = { 0, 28, 70, 87 };

    for (UInt32 i = 0; i < 4; ++i)
    {
        TArray<UInt8> bytes = ValidBytes();
        WriteUInt32(bytes, kOffsetDxgiFormat, unmappable[i]);

        FCompressedImageData image;
        LIMX_EXPECT_FALSE(DecodeBytes(bytes, image).Succeeded);
        LIMX_EXPECT_EQ(image.Format, EBlockCompressionFormat::Unknown);
    }

    // 上面那一轮里 dwPitchOrLinearSize 的一致性检查会替格式检查挡一道 ——
    // 认不出来的格式块大小为 0, 算出的第 0 层大小也是 0, 与头里声明的
    // 32768 不符。清掉 DDSD_LINEARSIZE 把那道保险拿开, 剩下的就只有
    // 格式检查本身: 它一旦被删, 一张 R8G8B8A8 的 DDS 会被"成功"读成
    // 一张零字节的纹理。
    for (UInt32 i = 0; i < 4; ++i)
    {
        TArray<UInt8> bytes = ValidBytes();
        WriteUInt32(bytes, kOffsetDxgiFormat, unmappable[i]);

        // DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT
        // | DDSD_MIPMAPCOUNT — 唯独不置 DDSD_LINEARSIZE
        WriteUInt32(bytes, kOffsetFlags,
                    0x1u | 0x2u | 0x4u | 0x1000u | 0x20000u);

        FCompressedImageData image;
        LIMX_EXPECT_FALSE(DecodeBytes(bytes, image).Succeeded);
    }
}

LIMX_TEST(DdsDecoder, RejectsTruncatedPayload)
{
    // 少一个字节也必须失败: 后面的 mip 层会读到别的东西, 而只有远处的
    // mip 花掉, 极难定位。
    TArray<UInt8> bytes = ValidBytes();
    const SizeType full = bytes.GetSize();

    bytes.SetSize(full - 1);

    FCompressedImageData image;
    LIMX_EXPECT_FALSE(DecodeBytes(bytes, image).Succeeded);
    LIMX_EXPECT_FALSE(image.IsValid());
    LIMX_EXPECT_EQ(image.Data.GetSize(), SizeType(0));

    // 只有头、没有载荷
    TArray<UInt8> headerOnly = ValidBytes();
    headerOnly.SetSize(kHeaderBytes);
    LIMX_EXPECT_FALSE(DecodeBytes(headerOnly, image).Succeeded);

    // 少了最后一层 (8 字节)
    TArray<UInt8> missingLastLevel = ValidBytes();
    missingLastLevel.SetSize(full - 8);
    LIMX_EXPECT_FALSE(DecodeBytes(missingLastLevel, image).Succeeded);
}

LIMX_TEST(DdsDecoder, RejectsTruncatedPayloadOnNonPowerOfTwo)
{
    // 非 4 倍数尺寸下, 少算一排块的表现正是"文件看起来短了一截"。
    // 这条与向上取整互为对照: 若块公式改成向下取整, 期望长度会变小,
    // 这个被截断的文件反而会被接受。
    TArray<UInt8> bytes = BuildDds(300, 173, kDxgiBc5Unorm, 16, 9);
    bytes.SetSize(bytes.GetSize() - 16);

    FCompressedImageData image;
    LIMX_EXPECT_FALSE(DecodeBytes(bytes, image).Succeeded);
}

LIMX_TEST(DdsDecoder, RejectsLinearSizeMismatch)
{
    // dwPitchOrLinearSize 与按块公式算出的第 0 层大小不符, 说明写出方
    // 的块公式和这里的不一样 —— 正是要提前发现的那类分歧。
    TArray<UInt8> bytes = ValidBytes();
    WriteUInt32(bytes, kOffsetLinearSize, 12345u);

    FCompressedImageData image;
    LIMX_EXPECT_FALSE(DecodeBytes(bytes, image).Succeeded);

    // 差一倍也要抓住 (典型症状: 一侧漏乘了每块字节数)
    TArray<UInt8> halved = ValidBytes();
    WriteUInt32(halved, kOffsetLinearSize, 64u * 64u * 8u / 2u);
    LIMX_EXPECT_FALSE(DecodeBytes(halved, image).Succeeded);
}

LIMX_TEST(DdsDecoder, RejectsMipCountOverLimit)
{
    // 256x256 的上限是 9 层。第 10 层的 1x1 块大小与第 9 层相同,
    // 逐层尺寸检查抓不住它 —— 必须有独立的层数上限检查。
    TArray<UInt8> bytes = ValidBytes();
    WriteUInt32(bytes, kOffsetMipMapCount, 10u);

    for (UInt32 i = 0; i < 8u; ++i)
    {
        bytes.Add(static_cast<UInt8>(0xBB));
    }

    FCompressedImageData image;
    LIMX_EXPECT_FALSE(DecodeBytes(bytes, image).Succeeded);
}

LIMX_TEST(DdsDecoder, RejectsZeroExtent)
{
    FCompressedImageData image;

    TArray<UInt8> zeroWidth = ValidBytes();
    WriteUInt32(zeroWidth, kOffsetWidth, 0u);
    LIMX_EXPECT_FALSE(DecodeBytes(zeroWidth, image).Succeeded);

    TArray<UInt8> zeroHeight = ValidBytes();
    WriteUInt32(zeroHeight, kOffsetHeight, 0u);
    LIMX_EXPECT_FALSE(DecodeBytes(zeroHeight, image).Succeeded);

    // 上面两条会被 mip 层数上限检查顺手挡住 (零尺寸的 mip 链长度是 0,
    // 而声明的 9 层超过了它), 因此它们证明不了"零尺寸检查本身还在"。
    // 下面这份输入把其余每一道保险都调成自洽的:
    //   mipMapCount = 1、dwPitchOrLinearSize = 0、载荷 0 字节。
    // 此时唯一能拦住它的就只剩零尺寸检查。
    TArray<UInt8> degenerate = BuildDds(1, 1, kDxgiBc1Unorm, 8, 1);
    WriteUInt32(degenerate, kOffsetWidth, 0u);
    WriteUInt32(degenerate, kOffsetLinearSize, 0u);
    degenerate.SetSize(kHeaderBytes);

    LIMX_EXPECT_FALSE(DecodeBytes(degenerate, image).Succeeded);
    LIMX_EXPECT_FALSE(image.IsValid());
}

LIMX_TEST(DdsDecoder, FailedDecodeLeavesOutputCleared)
{
    // 失败后 outImage 必须回到干净状态 —— 留着上一次的内容会让调用方
    // 在忽略返回值时上传一张陈旧的贴图。
    FCompressedImageData image;
    LIMX_REQUIRE_TRUE(DecodeBytes(ValidBytes(), image).Succeeded);
    LIMX_REQUIRE_TRUE(image.IsValid());

    TArray<UInt8> broken = ValidBytes();
    broken[0] = static_cast<UInt8>('X');

    LIMX_EXPECT_FALSE(DecodeBytes(broken, image).Succeeded);
    LIMX_EXPECT_EQ(image.Width, 0u);
    LIMX_EXPECT_EQ(image.Height, 0u);
    LIMX_EXPECT_EQ(image.Format, EBlockCompressionFormat::Unknown);
    LIMX_EXPECT_EQ(image.GetMipLevelCount(), 0u);
    LIMX_EXPECT_EQ(image.Data.GetSize(), SizeType(0));
}

// ============================================================================
// 与 Programs/lat 的真实往返
// ============================================================================

LIMX_TEST(DdsDecoder, LatBakedBc1SrgbRoundTrip)
{
    FCompressedImageData image;
    SizeType             fileBytes = 0;

    const FImageDecodeResult result =
        DecodeBase64(kLatBc1Srgb16x16, image, fileBytes);

    LIMX_REQUIRE_TRUE(result.Succeeded);
    LIMX_EXPECT_EQ(result.Warnings.GetSize(), SizeType(0));

    // 与 `lat inspect` 打印的数字逐条对照
    LIMX_EXPECT_EQ(fileBytes, SizeType(332));
    LIMX_EXPECT_EQ(image.Width, 16u);
    LIMX_EXPECT_EQ(image.Height, 16u);
    LIMX_EXPECT_EQ(image.Format, EBlockCompressionFormat::BC1_SRGB);
    LIMX_EXPECT_TRUE(image.IsSrgb());
    LIMX_EXPECT_FALSE(IsBlockCompressionTwoChannel(image.Format));
    LIMX_REQUIRE_EQ(image.GetMipLevelCount(), 5u);
    LIMX_EXPECT_EQ(image.Data.GetSize(), SizeType(184));
    LIMX_EXPECT_EQ(fileBytes, kHeaderBytes + image.Data.GetSize());

    const UInt32   dims[5]    = { 16, 8, 4, 2, 1 };
    const SizeType sizes[5]   = { 128, 32, 8, 8, 8 };
    const SizeType offsets[5] = { 0, 128, 160, 168, 176 };

    for (UInt32 level = 0; level < 5u; ++level)
    {
        const FCompressedMipLevel& entry =
            image.Levels[static_cast<SizeType>(level)];

        LIMX_EXPECT_EQ(entry.Width, dims[level]);
        LIMX_EXPECT_EQ(entry.Height, dims[level]);
        LIMX_EXPECT_EQ(entry.ByteSize, sizes[level]);
        LIMX_EXPECT_EQ(entry.ByteOffset, offsets[level]);
    }

    LIMX_EXPECT_TRUE(image.IsValid());
}

LIMX_TEST(DdsDecoder, LatBakedBc5NonPowerOfTwoRoundTrip)
{
    // 13x7: 两个维度都不是 4 的倍数, 且中间层出现 6x3 与 3x1 ——
    // 向下取整会把 6x3 算成 (6/4)*(3/4) = 1*0 = 0 字节, 整条链塌掉。
    FCompressedImageData image;
    SizeType             fileBytes = 0;

    const FImageDecodeResult result =
        DecodeBase64(kLatBc5Linear13x7, image, fileBytes);

    LIMX_REQUIRE_TRUE(result.Succeeded);

    LIMX_EXPECT_EQ(fileBytes, SizeType(340));
    LIMX_EXPECT_EQ(image.Width, 13u);
    LIMX_EXPECT_EQ(image.Height, 7u);
    LIMX_EXPECT_EQ(image.Format, EBlockCompressionFormat::BC5_UNORM);
    LIMX_EXPECT_FALSE(image.IsSrgb());
    LIMX_EXPECT_TRUE(IsBlockCompressionTwoChannel(image.Format));
    LIMX_REQUIRE_EQ(image.GetMipLevelCount(), 4u);
    LIMX_EXPECT_EQ(image.Data.GetSize(), SizeType(192));

    const UInt32   widths[4]  = { 13, 6, 3, 1 };
    const UInt32   heights[4] = { 7, 3, 1, 1 };
    const SizeType sizes[4]   = { 128, 32, 16, 16 };
    const SizeType offsets[4] = { 0, 128, 160, 176 };

    for (UInt32 level = 0; level < 4u; ++level)
    {
        const FCompressedMipLevel& entry =
            image.Levels[static_cast<SizeType>(level)];

        LIMX_EXPECT_EQ(entry.Width, widths[level]);
        LIMX_EXPECT_EQ(entry.Height, heights[level]);
        LIMX_EXPECT_EQ(entry.ByteSize, sizes[level]);
        LIMX_EXPECT_EQ(entry.ByteOffset, offsets[level]);
        LIMX_EXPECT_EQ(entry.ByteOffset % SizeType(16), SizeType(0));
    }

    LIMX_EXPECT_TRUE(image.IsValid());
}

LIMX_TEST(DdsDecoder, LatBakedPayloadIsNotAllZero)
{
    // 一份全 0 的载荷同样能通过所有长度校验, 却是一张纯黑贴图。
    // 这条确认夹具里装的确实是压缩数据, 上面两条才有意义。
    FCompressedImageData image;
    SizeType             fileBytes = 0;

    LIMX_REQUIRE_TRUE(
        DecodeBase64(kLatBc5Linear13x7, image, fileBytes).Succeeded);

    UInt32 nonZero = 0;

    for (SizeType i = 0; i < image.Data.GetSize(); ++i)
    {
        if (image.Data[i] != 0)
        {
            ++nonZero;
        }
    }

    LIMX_EXPECT_GT(nonZero, 100u);
}

LIMX_TEST(DdsDecoder, LatBakedFileSurvivesSingleByteTruncation)
{
    // 真实文件同样受截断检查保护 —— 合成夹具证明不了这一点,
    // 因为它的长度是测试自己算出来的。
    TArray<UInt8> bytes;
    LIMX_REQUIRE_TRUE(FBase64::Decode(kLatBc1Srgb16x16, bytes));
    LIMX_REQUIRE_EQ(bytes.GetSize(), SizeType(332));

    bytes.SetSize(bytes.GetSize() - 1);

    FCompressedImageData image;
    LIMX_EXPECT_FALSE(
        FDdsDecoder::Decode(bytes.GetData(), bytes.GetSize(), image).Succeeded);
}

// ============================================================================
// 与统一解码入口的衔接
// ============================================================================

LIMX_TEST(DdsDecoder, ImageDecoderDetectsDdsAndRefusesToDecodeIt)
{
    TArray<UInt8> bytes;
    LIMX_REQUIRE_TRUE(FBase64::Decode(kLatBc1Srgb16x16, bytes));

    LIMX_EXPECT_EQ(FImageDecoder::DetectFormat(bytes.GetData(),
                                               bytes.GetSize()),
                   EImageFileFormat::Dds);

    // 块压缩纹理的产物不是 FImageData。这里必须失败并指路,
    // 而不是产出一张空图让调用方以为解码成功了。
    FImageData image;
    const FImageDecodeResult result =
        FImageDecoder::Decode(bytes.GetData(), bytes.GetSize(), image);

    LIMX_EXPECT_FALSE(result.Succeeded);
    LIMX_EXPECT_FALSE(image.IsValid());
}
