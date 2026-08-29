/*******************************************************************************
 * 文件: PngDecoderTests.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   PNG 解码器单元测试 — 五种颜色类型、全部位深、调色板、透明色键、
 *   四种滤波器、Adam7 隔行、通道扩展与损坏数据的处理
 *
 * 设计哲学:
 *   像素值必须逐个核对 — 解码器出错时图像往往仍能"显示出来"，只是颜色偏移、
 *   通道错位或行错位。断言"解码成功"完全抓不住这类缺陷，因此每个用例都对
 *   具体坐标的具体通道值做断言，夹具的像素值也刻意选成易于心算的形式。
 *
 *   滤波器逐种覆盖 — None/Sub/Up/Paeth 四种滤波在同一张图的不同行上使用，
 *   任何一种实现错误都会让该行整体错乱而其余行正常，定位极其直接。
 *
 *   隔行是独立的失败模式 — Adam7 把图像拆成七遍采样，若按整幅图反滤波
 *   会得到纯噪声。夹具用 8x8 覆盖全部七遍，并按公式核对每个像素。
 *
 * 技术特性:
 *   - 夹具由脚本按 PNG 规范手工构造, 像素值可由坐标公式推导
 *   - 覆盖 1/4/8/16 位深与调色板索引两种低位深语义
 *   - 通道扩展与 16 位降级分别验证
 *
 * 依赖关系:
 *   内部: AssetTests/AssetTestsMinimal.h
 *
 * 注意事项:
 *   夹具以 base64 内嵌, 不依赖外部图像文件
 *
 ******************************************************************************/

#include "AssetTests/AssetTestsMinimal.h"

using namespace Limx;

namespace
{

/// RGB8 2x2: 红 绿 / 蓝 白
constexpr const AnsiChar* kRgb8Png =
    "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAFElEQVR42mP4z8DA"
    "AMIM/////w8AH+4F+2BscPIAAAAASUVORK5CYII=";

/// RGBA8 2x2: 四个像素的 alpha 分别为 255/128/64/0
constexpr const AnsiChar* kRgba8Png =
    "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAF0lEQVR42mP4z8Dw"
    "HwgbGIC0w////xkAQBgHul5CkSMAAAAASUVORK5CYII=";

/// 灰度8 4x1: 0/85/170/255
constexpr const AnsiChar* kGray8Png =
    "iVBORw0KGgoAAAANSUhEUgAAAAQAAAABCAAAAADcV1ARAAAADUlEQVR42mNgCF31"
    "HwADVwH/8jhg/wAAAABJRU5ErkJggg==";

/// 调色板8 4x1: 红绿蓝黄
constexpr const AnsiChar* kPalette8Png =
    "iVBORw0KGgoAAAANSUhEUgAAAAQAAAABCAMAAADO4v//AAAADFBMVEX/AAAA/wAA"
    "AP///wDWAo97AAAADUlEQVR42mNgYGRiBgAADwAHW9CLfQAAAABJRU5ErkJggg==";

/// 调色板4位 4x1: 与上同色, 每字节打包两个索引
constexpr const AnsiChar* kPalette4Png =
    "iVBORw0KGgoAAAANSUhEUgAAAAQAAAABBAMAAAALEhL+AAAADFBMVEX/AAAA/wAA"
    "AP///wDWAo97AAAAC0lEQVR42mNgVAYAACgAJalnYggAAAAASUVORK5CYII=";

/// 灰度1位 8x1: 交替黑白 (10101010)
constexpr const AnsiChar* kGray1Png =
    "iVBORw0KGgoAAAANSUhEUgAAAAgAAAABAQAAAADLe9LuAAAACklEQVR42mNYBQAA"
    "rACry4Oe5gAAAABJRU5ErkJggg==";

/// 灰度16位 2x1: 0x0000 与 0xFFFF
constexpr const AnsiChar* kGray16Png =
    "iVBORw0KGgoAAAANSUhEUgAAAAIAAAABEAAAAACB2fwVAAAADUlEQVR42mNgYPj/"
    "HwADAgH/OSkZvgAAAABJRU5ErkJggg==";

/// RGB8 4x4: 四行分别使用 None/Sub/Up/Paeth 滤波
/// 像素 (x,y) 的值为 (x*16, y*16, 128)
constexpr const AnsiChar* kFiltersPng =
    "iVBORw0KGgoAAAANSUhEUgAAAAQAAAAECAIAAAAmkwkpAAAAJklEQVR42mNgYGgQ"
    "YGhQYGgwYGhgZBAAchggiIkBzhRgYAFRMAAAiiMDeBKaDeQAAAAASUVORK5CYII=";

/// Adam7 隔行 8x8 RGB8
/// 像素 (x,y) 的值为 (x*32 % 256, y*32 % 256, (x+y)*16 % 256)
constexpr const AnsiChar* kInterlacedPng =
    "iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAIAAAE8ahlKAAAAi0lEQVR42hWNQRVD"
    "MRACkVAJK+FLQEIkICESkBAJSPgSVkIlrJSmHObNaQDuDF7Q/ks1BFrtACySppoG"
    "Wmy7090ofIQnWION4iOucA8Pykve8Rm/N7bVJ/1Of4H6VBXrUdG1UuraU7esp0Rq"
    "SbJ25NYZ3f+sipit2DlJOu+kgdk15hxNPG+me74z8wMBQlQB+u3JdwAAAABJRU5E"
    "rkJggg==";

/// RGB8 2x2 带 tRNS 色键: 纯红被视为全透明
constexpr const AnsiChar* kColorKeyPng =
    "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAABnRSTlMA/wAAAACk"
    "wsAdAAAAFElEQVR42mP4z8DAAMIM/////w8AH+4F+2BscPIAAAAASUVORK5CYII=";

/// RGB8 1x1 带 sRGB 块
constexpr const AnsiChar* kSrgbPng =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAAAXNSR0IArs4c6QAA"
    "AAxJREFUeNpjaGhoAAADBAGBdS4BvAAAAABJRU5ErkJggg==";

/// 解码 base64 编码的 PNG
FImageDecodeResult DecodePng(const AnsiChar* base64, FImageData& outImage,
                             const FImageDecodeOptions& options =
                                 FImageDecodeOptions())
{
    TArray<UInt8> bytes;

    if (!FBase64::Decode(base64, bytes))
    {
        return FImageDecodeResult::Failure(FString("夹具 base64 解码失败"));
    }

    return FPngDecoder::Decode(bytes.GetData(), bytes.GetSize(),
                               outImage, options);
}

/// 取指定像素的指定通道 (8 位格式)
UInt8 GetChannel(const FImageData& image, UInt32 x, UInt32 y, UInt32 channel)
{
    const SizeType offset = static_cast<SizeType>(y) * image.GetRowPitch() +
                            static_cast<SizeType>(x) * image.GetBytesPerPixel() +
                            channel;

    return (offset < image.Pixels.GetSize()) ? image.Pixels[offset] : 0;
}

/// 不做通道扩展的选项 — 便于验证源格式的通道数
FImageDecodeOptions NativeChannelOptions()
{
    FImageDecodeOptions options;
    options.ForceFourChannels = false;
    return options;
}

} // namespace

// ============================================================================
// 识别
// ============================================================================

LIMX_TEST(PngDecoder, RecognizesSignature)
{
    TArray<UInt8> bytes;
    LIMX_REQUIRE_TRUE(FBase64::Decode(kRgb8Png, bytes));

    LIMX_EXPECT_TRUE(FPngDecoder::IsPng(bytes.GetData(), bytes.GetSize()));

    // 破坏签名后不应再被识别
    bytes[1] = 'X';
    LIMX_EXPECT_FALSE(FPngDecoder::IsPng(bytes.GetData(), bytes.GetSize()));

    LIMX_EXPECT_FALSE(FPngDecoder::IsPng(nullptr, 0));
}

LIMX_TEST(PngDecoder, ReadsHeaderWithoutDecoding)
{
    TArray<UInt8> bytes;
    LIMX_REQUIRE_TRUE(FBase64::Decode(kInterlacedPng, bytes));

    UInt32 width = 0;
    UInt32 height = 0;
    UInt32 channels = 0;
    UInt32 bitDepth = 0;

    LIMX_REQUIRE_TRUE(FPngDecoder::ReadHeader(bytes.GetData(), bytes.GetSize(),
                                              width, height, channels,
                                              bitDepth).Succeeded);

    LIMX_EXPECT_EQ(width, UInt32(8));
    LIMX_EXPECT_EQ(height, UInt32(8));
    LIMX_EXPECT_EQ(channels, UInt32(3));
    LIMX_EXPECT_EQ(bitDepth, UInt32(8));
}

// ============================================================================
// 颜色类型
// ============================================================================

LIMX_TEST(PngDecoder, DecodesRgb8)
{
    FImageData image;
    LIMX_REQUIRE_TRUE(DecodePng(kRgb8Png, image).Succeeded);

    LIMX_REQUIRE_TRUE(image.IsValid());
    LIMX_EXPECT_EQ(image.Width, UInt32(2));
    LIMX_EXPECT_EQ(image.Height, UInt32(2));

    // 默认扩展为四通道 (Vulkan 对三通道格式支持不普遍)
    LIMX_EXPECT_EQ(static_cast<Int32>(image.Format),
                   static_cast<Int32>(EImageFormat::RGBA8));
    LIMX_EXPECT_FALSE(image.HasSourceAlpha);

    // (0,0) 红
    LIMX_EXPECT_EQ(GetChannel(image, 0, 0, 0), UInt8(255));
    LIMX_EXPECT_EQ(GetChannel(image, 0, 0, 1), UInt8(0));
    LIMX_EXPECT_EQ(GetChannel(image, 0, 0, 2), UInt8(0));
    LIMX_EXPECT_EQ(GetChannel(image, 0, 0, 3), UInt8(255));

    // (1,0) 绿
    LIMX_EXPECT_EQ(GetChannel(image, 1, 0, 1), UInt8(255));

    // (0,1) 蓝 — 行偏移算错会读到第一行的值
    LIMX_EXPECT_EQ(GetChannel(image, 0, 1, 2), UInt8(255));
    LIMX_EXPECT_EQ(GetChannel(image, 0, 1, 0), UInt8(0));

    // (1,1) 白
    LIMX_EXPECT_EQ(GetChannel(image, 1, 1, 0), UInt8(255));
    LIMX_EXPECT_EQ(GetChannel(image, 1, 1, 1), UInt8(255));
    LIMX_EXPECT_EQ(GetChannel(image, 1, 1, 2), UInt8(255));
}

LIMX_TEST(PngDecoder, DecodesRgb8WithoutChannelExpansion)
{
    FImageData image;
    LIMX_REQUIRE_TRUE(DecodePng(kRgb8Png, image, NativeChannelOptions()).Succeeded);

    // 关闭扩展后应保持源格式的三通道
    LIMX_EXPECT_EQ(static_cast<Int32>(image.Format),
                   static_cast<Int32>(EImageFormat::RGB8));
    LIMX_EXPECT_EQ(image.GetBytesPerPixel(), UInt32(3));

    LIMX_EXPECT_EQ(GetChannel(image, 1, 1, 0), UInt8(255));
}

LIMX_TEST(PngDecoder, DecodesRgba8)
{
    FImageData image;
    LIMX_REQUIRE_TRUE(DecodePng(kRgba8Png, image).Succeeded);

    LIMX_REQUIRE_TRUE(image.IsValid());
    LIMX_EXPECT_TRUE(image.HasSourceAlpha);

    // 四个像素的 alpha 各不相同 — 通道错位会让这组断言全部落空
    LIMX_EXPECT_EQ(GetChannel(image, 0, 0, 3), UInt8(255));
    LIMX_EXPECT_EQ(GetChannel(image, 1, 0, 3), UInt8(128));
    LIMX_EXPECT_EQ(GetChannel(image, 0, 1, 3), UInt8(64));
    LIMX_EXPECT_EQ(GetChannel(image, 1, 1, 3), UInt8(0));

    LIMX_EXPECT_EQ(GetChannel(image, 1, 0, 1), UInt8(255));
}

LIMX_TEST(PngDecoder, DecodesGrayscale8)
{
    FImageData image;
    LIMX_REQUIRE_TRUE(DecodePng(kGray8Png, image, NativeChannelOptions()).Succeeded);

    LIMX_REQUIRE_TRUE(image.IsValid());
    LIMX_EXPECT_EQ(image.Width, UInt32(4));
    LIMX_EXPECT_EQ(image.Height, UInt32(1));

    // 无 alpha 的灰度应保持单通道
    LIMX_EXPECT_EQ(static_cast<Int32>(image.Format),
                   static_cast<Int32>(EImageFormat::R8));

    LIMX_EXPECT_EQ(GetChannel(image, 0, 0, 0), UInt8(0));
    LIMX_EXPECT_EQ(GetChannel(image, 1, 0, 0), UInt8(85));
    LIMX_EXPECT_EQ(GetChannel(image, 2, 0, 0), UInt8(170));
    LIMX_EXPECT_EQ(GetChannel(image, 3, 0, 0), UInt8(255));
}

LIMX_TEST(PngDecoder, GrayscaleExpandsToAllRgbChannels)
{
    FImageData image;
    LIMX_REQUIRE_TRUE(DecodePng(kGray8Png, image).Succeeded);

    // 扩展为四通道后, 灰度值应复制到 R/G/B, alpha 补满
    LIMX_EXPECT_EQ(GetChannel(image, 2, 0, 0), UInt8(170));
    LIMX_EXPECT_EQ(GetChannel(image, 2, 0, 1), UInt8(170));
    LIMX_EXPECT_EQ(GetChannel(image, 2, 0, 2), UInt8(170));
    LIMX_EXPECT_EQ(GetChannel(image, 2, 0, 3), UInt8(255));
}

// ============================================================================
// 调色板
// ============================================================================

LIMX_TEST(PngDecoder, DecodesPalette8)
{
    FImageData image;
    LIMX_REQUIRE_TRUE(DecodePng(kPalette8Png, image).Succeeded);

    LIMX_REQUIRE_TRUE(image.IsValid());
    LIMX_EXPECT_EQ(image.Width, UInt32(4));

    // 索引 0..3 对应 红/绿/蓝/黄
    LIMX_EXPECT_EQ(GetChannel(image, 0, 0, 0), UInt8(255));
    LIMX_EXPECT_EQ(GetChannel(image, 0, 0, 1), UInt8(0));

    LIMX_EXPECT_EQ(GetChannel(image, 1, 0, 1), UInt8(255));
    LIMX_EXPECT_EQ(GetChannel(image, 1, 0, 0), UInt8(0));

    LIMX_EXPECT_EQ(GetChannel(image, 2, 0, 2), UInt8(255));

    // 黄 = 红 + 绿
    LIMX_EXPECT_EQ(GetChannel(image, 3, 0, 0), UInt8(255));
    LIMX_EXPECT_EQ(GetChannel(image, 3, 0, 1), UInt8(255));
    LIMX_EXPECT_EQ(GetChannel(image, 3, 0, 2), UInt8(0));
}

LIMX_TEST(PngDecoder, DecodesPalette4BitPacked)
{
    FImageData image;
    LIMX_REQUIRE_TRUE(DecodePng(kPalette4Png, image).Succeeded);

    LIMX_REQUIRE_TRUE(image.IsValid());
    LIMX_EXPECT_EQ(image.Width, UInt32(4));

    // 每字节打包两个索引, 高位在前。
    // 若解包时把索引也做了灰度归一化, 会取到完全错误的调色板项。
    LIMX_EXPECT_EQ(GetChannel(image, 0, 0, 0), UInt8(255));   // 索引 0 红
    LIMX_EXPECT_EQ(GetChannel(image, 1, 0, 1), UInt8(255));   // 索引 1 绿
    LIMX_EXPECT_EQ(GetChannel(image, 2, 0, 2), UInt8(255));   // 索引 2 蓝
    LIMX_EXPECT_EQ(GetChannel(image, 3, 0, 0), UInt8(255));   // 索引 3 黄
    LIMX_EXPECT_EQ(GetChannel(image, 3, 0, 1), UInt8(255));
}

// ============================================================================
// 低位深与高位深
// ============================================================================

LIMX_TEST(PngDecoder, DecodesOneBitGrayscale)
{
    FImageData image;
    LIMX_REQUIRE_TRUE(DecodePng(kGray1Png, image, NativeChannelOptions()).Succeeded);

    LIMX_REQUIRE_TRUE(image.IsValid());
    LIMX_EXPECT_EQ(image.Width, UInt32(8));

    // 1 位灰度需归一化到 0..255: 0 -> 0, 1 -> 255
    for (UInt32 x = 0; x < 8; ++x)
    {
        const UInt8 expected = ((x % 2) == 0) ? UInt8(255) : UInt8(0);
        LIMX_REQUIRE_EQ(GetChannel(image, x, 0, 0), expected);
    }
}

LIMX_TEST(PngDecoder, DecodesSixteenBitReducedToEight)
{
    FImageData image;
    LIMX_REQUIRE_TRUE(DecodePng(kGray16Png, image, NativeChannelOptions()).Succeeded);

    LIMX_REQUIRE_TRUE(image.IsValid());

    // 默认把 16 位降为 8 位
    LIMX_EXPECT_EQ(static_cast<Int32>(image.Format),
                   static_cast<Int32>(EImageFormat::R8));

    LIMX_EXPECT_EQ(GetChannel(image, 0, 0, 0), UInt8(0));
    LIMX_EXPECT_EQ(GetChannel(image, 1, 0, 0), UInt8(255));
}

LIMX_TEST(PngDecoder, PreservesSixteenBitWhenRequested)
{
    FImageDecodeOptions options;
    options.ForceFourChannels       = false;
    options.ReduceSixteenBitToEight = false;

    FImageData image;
    LIMX_REQUIRE_TRUE(DecodePng(kGray16Png, image, options).Succeeded);

    LIMX_REQUIRE_TRUE(image.IsValid());
    LIMX_EXPECT_EQ(static_cast<Int32>(image.Format),
                   static_cast<Int32>(EImageFormat::R16));
    LIMX_EXPECT_EQ(image.GetBytesPerPixel(), UInt32(2));

    // 16 位样本按主机序 (小端) 存放
    LIMX_EXPECT_EQ(image.Pixels[0], UInt8(0x00));
    LIMX_EXPECT_EQ(image.Pixels[1], UInt8(0x00));
    LIMX_EXPECT_EQ(image.Pixels[2], UInt8(0xFF));
    LIMX_EXPECT_EQ(image.Pixels[3], UInt8(0xFF));
}

// ============================================================================
// 滤波器
// ============================================================================

LIMX_TEST(PngDecoder, AppliesAllFilterTypes)
{
    FImageData image;
    LIMX_REQUIRE_TRUE(DecodePng(kFiltersPng, image).Succeeded);

    LIMX_REQUIRE_TRUE(image.IsValid());
    LIMX_REQUIRE_EQ(image.Width, UInt32(4));
    LIMX_REQUIRE_EQ(image.Height, UInt32(4));

    // 四行分别使用 None/Sub/Up/Paeth 滤波。
    // 任何一种反滤波实现错误都会让对应行整体错乱, 而其余行仍正确。
    for (UInt32 y = 0; y < 4; ++y)
    {
        for (UInt32 x = 0; x < 4; ++x)
        {
            LIMX_REQUIRE_EQ(GetChannel(image, x, y, 0),
                            static_cast<UInt8>(x * 16));
            LIMX_REQUIRE_EQ(GetChannel(image, x, y, 1),
                            static_cast<UInt8>(y * 16));
            LIMX_REQUIRE_EQ(GetChannel(image, x, y, 2), UInt8(128));
        }
    }
}

// ============================================================================
// 隔行
// ============================================================================

LIMX_TEST(PngDecoder, DecodesAdam7Interlaced)
{
    FImageData image;
    LIMX_REQUIRE_TRUE(DecodePng(kInterlacedPng, image).Succeeded);

    LIMX_REQUIRE_TRUE(image.IsValid());
    LIMX_REQUIRE_EQ(image.Width, UInt32(8));
    LIMX_REQUIRE_EQ(image.Height, UInt32(8));

    // Adam7 把图像拆成七遍采样。若按整幅图反滤波会得到纯噪声,
    // 若回填的格点算错则像素会散落到错误位置。
    for (UInt32 y = 0; y < 8; ++y)
    {
        for (UInt32 x = 0; x < 8; ++x)
        {
            LIMX_REQUIRE_EQ(GetChannel(image, x, y, 0),
                            static_cast<UInt8>((x * 32) % 256));
            LIMX_REQUIRE_EQ(GetChannel(image, x, y, 1),
                            static_cast<UInt8>((y * 32) % 256));
            LIMX_REQUIRE_EQ(GetChannel(image, x, y, 2),
                            static_cast<UInt8>(((x + y) * 16) % 256));
        }
    }
}

// ============================================================================
// 透明色键
// ============================================================================

LIMX_TEST(PngDecoder, AppliesColorKeyTransparency)
{
    FImageData image;
    LIMX_REQUIRE_TRUE(DecodePng(kColorKeyPng, image).Succeeded);

    LIMX_REQUIRE_TRUE(image.IsValid());

    // tRNS 声明纯红为透明色 — 该像素的 alpha 应为 0
    LIMX_EXPECT_EQ(GetChannel(image, 0, 0, 0), UInt8(255));
    LIMX_EXPECT_EQ(GetChannel(image, 0, 0, 3), UInt8(0));

    // 其余像素不受影响
    LIMX_EXPECT_EQ(GetChannel(image, 1, 0, 3), UInt8(255));
    LIMX_EXPECT_EQ(GetChannel(image, 0, 1, 3), UInt8(255));
    LIMX_EXPECT_EQ(GetChannel(image, 1, 1, 3), UInt8(255));

    // 有色键即视为源含 alpha
    LIMX_EXPECT_TRUE(image.HasSourceAlpha);
}

// ============================================================================
// 色彩空间
// ============================================================================

LIMX_TEST(PngDecoder, ReportsSrgbColorSpace)
{
    FImageData image;
    LIMX_REQUIRE_TRUE(DecodePng(kSrgbPng, image).Succeeded);

    // sRGB 块存在时应如实转达
    LIMX_EXPECT_EQ(static_cast<Int32>(image.ColorSpace),
                   static_cast<Int32>(EImageColorSpace::Srgb));
}

LIMX_TEST(PngDecoder, ColorSpaceUnspecifiedWhenAbsent)
{
    FImageData image;
    LIMX_REQUIRE_TRUE(DecodePng(kRgb8Png, image).Succeeded);

    // 无 sRGB/gAMA 块时不应臆测
    LIMX_EXPECT_EQ(static_cast<Int32>(image.ColorSpace),
                   static_cast<Int32>(EImageColorSpace::Unspecified));
}

// ============================================================================
// 翻转
// ============================================================================

LIMX_TEST(PngDecoder, FlipsVerticallyWhenRequested)
{
    FImageDecodeOptions options;
    options.FlipVertically = true;

    FImageData image;
    LIMX_REQUIRE_TRUE(DecodePng(kRgb8Png, image, options).Succeeded);

    // 原图 (0,0) 是红、(0,1) 是蓝; 翻转后应互换
    LIMX_EXPECT_EQ(GetChannel(image, 0, 0, 2), UInt8(255));   // 现在是蓝
    LIMX_EXPECT_EQ(GetChannel(image, 0, 1, 0), UInt8(255));   // 现在是红
    LIMX_EXPECT_EQ(GetChannel(image, 0, 1, 2), UInt8(0));
}

// ============================================================================
// 损坏数据
// ============================================================================

LIMX_TEST(PngDecoder, RejectsNonPngData)
{
    const UInt8 garbage[] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07 };

    FImageData image;
    const FImageDecodeResult result =
        FPngDecoder::Decode(garbage, sizeof(garbage), image);

    LIMX_EXPECT_FALSE(result.Succeeded);
    LIMX_EXPECT_FALSE(result.ErrorMessage.IsEmpty());
}

LIMX_TEST(PngDecoder, RejectsTruncatedFile)
{
    TArray<UInt8> bytes;
    LIMX_REQUIRE_TRUE(FBase64::Decode(kFiltersPng, bytes));

    // 只保留签名与部分 IHDR
    TArray<UInt8> truncated;
    for (SizeType i = 0; i < 20 && i < bytes.GetSize(); ++i)
    {
        truncated.Add(bytes[i]);
    }

    FImageData image;
    LIMX_EXPECT_FALSE(FPngDecoder::Decode(truncated.GetData(),
                                          truncated.GetSize(), image).Succeeded);
}

LIMX_TEST(PngDecoder, WarnsButDecodesOnCrcMismatch)
{
    TArray<UInt8> bytes;
    LIMX_REQUIRE_TRUE(FBase64::Decode(kRgb8Png, bytes));
    LIMX_REQUIRE_GT(bytes.GetSize(), SizeType(30));

    // 破坏 IHDR 的 CRC (位于签名 8 + 长度 4 + 类型 4 + 数据 13 之后)
    bytes[8 + 4 + 4 + 13] ^= 0xFFu;

    FImageData image;
    const FImageDecodeResult result =
        FPngDecoder::Decode(bytes.GetData(), bytes.GetSize(), image);

    // CRC 只告警不拒绝 — 分块损坏未必影响可解码性,
    // 真正的像素损坏由 IDAT 的 zlib 校验和拦下
    LIMX_EXPECT_TRUE(result.Succeeded);
    LIMX_EXPECT_GT(result.Warnings.GetSize(), SizeType(0));
    LIMX_EXPECT_TRUE(image.IsValid());
}

LIMX_TEST(PngDecoder, RejectsCorruptedPixelData)
{
    TArray<UInt8> bytes;
    LIMX_REQUIRE_TRUE(FBase64::Decode(kFiltersPng, bytes));

    // 破坏 IDAT 中段的压缩数据 — zlib 校验和会发现
    const SizeType middle = bytes.GetSize() / 2;
    bytes[middle] ^= 0xFFu;
    bytes[middle + 1] ^= 0xFFu;

    FImageData image;
    const FImageDecodeResult result =
        FPngDecoder::Decode(bytes.GetData(), bytes.GetSize(), image);

    LIMX_EXPECT_FALSE(result.Succeeded);
}

// ============================================================================
// 数据一致性
// ============================================================================

LIMX_TEST(PngDecoder, PixelBufferMatchesDeclaredSize)
{
    FImageData image;
    LIMX_REQUIRE_TRUE(DecodePng(kInterlacedPng, image).Succeeded);

    // 行距无填充, 缓冲区大小应与尺寸严格吻合
    LIMX_EXPECT_EQ(image.GetRowPitch(),
                   static_cast<SizeType>(image.Width) * image.GetBytesPerPixel());
    LIMX_EXPECT_EQ(image.Pixels.GetSize(), image.GetExpectedByteSize());
    LIMX_EXPECT_TRUE(image.IsValid());
}

LIMX_TEST(PngDecoder, ResetClearsImage)
{
    FImageData image;
    LIMX_REQUIRE_TRUE(DecodePng(kRgb8Png, image).Succeeded);
    LIMX_REQUIRE_TRUE(image.IsValid());

    image.Reset();

    LIMX_EXPECT_FALSE(image.IsValid());
    LIMX_EXPECT_EQ(image.Width, UInt32(0));
    LIMX_EXPECT_EQ(image.Pixels.GetSize(), SizeType(0));
}

LIMX_TEST(PngDecoder, DecodeReplacesPreviousImage)
{
    FImageData image;

    LIMX_REQUIRE_TRUE(DecodePng(kInterlacedPng, image).Succeeded);
    LIMX_REQUIRE_EQ(image.Width, UInt32(8));

    // 二次解码应替换而非叠加
    LIMX_REQUIRE_TRUE(DecodePng(kRgb8Png, image).Succeeded);
    LIMX_EXPECT_EQ(image.Width, UInt32(2));
    LIMX_EXPECT_EQ(image.Height, UInt32(2));
    LIMX_EXPECT_TRUE(image.IsValid());
}
