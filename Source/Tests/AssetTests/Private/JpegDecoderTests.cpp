/*******************************************************************************
 * 文件: JpegDecoderTests.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   JPEG 基线解码器单元测试 — 灰度与彩色、色度子采样、DC 差分、
 *   边缘裁剪与不支持模式的拒绝
 *
 * 设计哲学:
 *   夹具用常数块以隔离变量 — 每个 8x8 块编码为单一 DC 系数、AC 全部 EOB，
 *   于是解码结果的期望值可由 DCT 定义精确推导 (常数块的 DC = (P-128)×8)。
 *   这样任何偏差都能直接归因到某个具体环节，而不是淹没在 DCT 的浮点误差里。
 *
 *   DC 差分要跨块验证 — 每块的 DC 存的是与上一块的差值。单块图像永远测不出
 *   差分逻辑的错误，因此夹具用四个取值各异的相邻块，一旦累加有误，
 *   后续块的亮度就会整体偏移。
 *
 *   子采样是独立的失败模式 — 4:2:0 下色度平面只有亮度的四分之一，
 *   上采样坐标算错会让颜色整体错位。夹具让色度保持中性，
 *   使亮度与色度的错误可以分别定位。
 *
 * 技术特性:
 *   - 夹具由脚本按基线 JPEG 规范生成, 量化表全 1 以消除量化误差
 *   - 覆盖 4:4:4 与 4:2:0 两种采样, 以及非 8 倍数尺寸
 *   - IDCT 的浮点特性使结果允许 ±1 的偏差
 *
 * 依赖关系:
 *   内部: AssetTests/AssetTestsMinimal.h
 *
 * 注意事项:
 *   夹具的 AC 系数全为零, 因此不检验 AC 游程解码的复杂路径 ——
 *   该路径由真实照片类 JPEG 覆盖, 属于集成验证范畴
 *
 ******************************************************************************/

#include "AssetTests/AssetTestsMinimal.h"

using namespace Limx;

namespace
{

/// IDCT 为浮点实现, 允许 ±1 的舍入偏差
constexpr Int32 kPixelTolerance = 1;

/// 灰度 8x8 纯色, 像素值 144
constexpr const AnsiChar* kGray8x8Jpeg =
    "/9j/2wBDAAEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEB"
    "AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQH/wAALCAAIAAgBAREA/8QAHwAAAQUBAQEB"
    "AQEAAAAAAAAAAAECAwQFBgcICQoL/8QAFBABAAAAAAAAAAAAAAAAAAAAAP/aAAgB"
    "AQAAPwD6Af/Z";

/// 灰度 16x16, 四个 8x8 块分别为 64/96/160/224 (验证 DC 差分)
constexpr const AnsiChar* kGray16x16Jpeg =
    "/9j/2wBDAAEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEB"
    "AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQH/wAALCAAQABABAREA/8QAHwAAAQUBAQEB"
    "AQEAAAAAAAAAAAECAwQFBgcICQoL/8QAFBABAAAAAAAAAAAAAAAAAAAAAP/aAAgB"
    "AQAAPwD+f9+gD+gB/QA//9k=";

/// YCbCr 4:4:4 8x8, Y=128 Cb=128 Cr=200 (偏红)
constexpr const AnsiChar* kColor444Jpeg =
    "/9j/2wBDAAEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEB"
    "AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQH/wAARCAAIAAgDAREAAhEAAxEA/8QAHwAA"
    "AQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAFBABAAAAAAAAAAAAAAAAAAAA"
    "AP/aAAwDAQACAAMAAD8AA/pAf//Z";

/// YCbCr 4:2:0 16x16, 亮度四块 100/140/180/220, 色度中性
constexpr const AnsiChar* kColor420Jpeg =
    "/9j/2wBDAAEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEB"
    "AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQH/wAARCAAQABADASIAAhEAAxEA/8QAHwAA"
    "AQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAFBABAAAAAAAAAAAAAAAAAAAA"
    "AP/aAAwDAQACAAMAAD8A+H36gP1AfqAA/9k=";

/// 灰度 5x3 纯色 200 (尺寸非 8 的倍数, 验证边缘裁剪)
constexpr const AnsiChar* kGray5x3Jpeg =
    "/9j/2wBDAAEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEB"
    "AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQH/wAALCAADAAUBAREA/8QAHwAAAQUBAQEB"
    "AQEAAAAAAAAAAAECAwQFBgcICQoL/8QAFBABAAAAAAAAAAAAAAAAAAAAAP/aAAgB"
    "AQAAPwD+kB//2Q==";

/// 解码 base64 编码的 JPEG
FImageDecodeResult DecodeJpeg(const AnsiChar* base64, FImageData& outImage,
                              const FImageDecodeOptions& options =
                                  FImageDecodeOptions())
{
    TArray<UInt8> bytes;

    if (!FBase64::Decode(base64, bytes))
    {
        return FImageDecodeResult::Failure(FString("夹具 base64 解码失败"));
    }

    return FJpegDecoder::Decode(bytes.GetData(), bytes.GetSize(),
                                outImage, options);
}

/// 取指定像素的指定通道
UInt8 GetChannel(const FImageData& image, UInt32 x, UInt32 y, UInt32 channel)
{
    const SizeType offset = static_cast<SizeType>(y) * image.GetRowPitch() +
                            static_cast<SizeType>(x) * image.GetBytesPerPixel() +
                            channel;

    return (offset < image.Pixels.GetSize()) ? image.Pixels[offset] : 0;
}

/// 在容差内比较通道值
bool ChannelNear(const FImageData& image, UInt32 x, UInt32 y, UInt32 channel,
                 Int32 expected)
{
    const Int32 actual = static_cast<Int32>(GetChannel(image, x, y, channel));
    const Int32 difference = (actual > expected) ? (actual - expected)
                                                 : (expected - actual);

    return difference <= kPixelTolerance;
}

/// 不做通道扩展的选项
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

LIMX_TEST(JpegDecoder, RecognizesSoiMarker)
{
    TArray<UInt8> bytes;
    LIMX_REQUIRE_TRUE(FBase64::Decode(kGray8x8Jpeg, bytes));

    LIMX_EXPECT_TRUE(FJpegDecoder::IsJpeg(bytes.GetData(), bytes.GetSize()));

    bytes[1] = 0x00;
    LIMX_EXPECT_FALSE(FJpegDecoder::IsJpeg(bytes.GetData(), bytes.GetSize()));

    LIMX_EXPECT_FALSE(FJpegDecoder::IsJpeg(nullptr, 0));
}

// ============================================================================
// 灰度
// ============================================================================

LIMX_TEST(JpegDecoder, DecodesGrayscaleConstantBlock)
{
    FImageData image;
    LIMX_REQUIRE_TRUE(DecodeJpeg(kGray8x8Jpeg, image,
                                 NativeChannelOptions()).Succeeded);

    LIMX_REQUIRE_TRUE(image.IsValid());
    LIMX_EXPECT_EQ(image.Width, UInt32(8));
    LIMX_EXPECT_EQ(image.Height, UInt32(8));

    // 单组件应保持单通道
    LIMX_EXPECT_EQ(static_cast<Int32>(image.Format),
                   static_cast<Int32>(EImageFormat::R8));

    // 常数块的 DC 系数为 (P-128)×8, IDCT 后应还原为 P
    for (UInt32 y = 0; y < 8; ++y)
    {
        for (UInt32 x = 0; x < 8; ++x)
        {
            LIMX_REQUIRE_TRUE(ChannelNear(image, x, y, 0, 144));
        }
    }
}

LIMX_TEST(JpegDecoder, GrayscaleExpandsToFourChannels)
{
    FImageData image;
    LIMX_REQUIRE_TRUE(DecodeJpeg(kGray8x8Jpeg, image).Succeeded);

    LIMX_EXPECT_EQ(static_cast<Int32>(image.Format),
                   static_cast<Int32>(EImageFormat::RGBA8));

    // 灰度值复制到 RGB 三通道, alpha 补满
    LIMX_EXPECT_TRUE(ChannelNear(image, 4, 4, 0, 144));
    LIMX_EXPECT_TRUE(ChannelNear(image, 4, 4, 1, 144));
    LIMX_EXPECT_TRUE(ChannelNear(image, 4, 4, 2, 144));
    LIMX_EXPECT_EQ(GetChannel(image, 4, 4, 3), UInt8(255));

    // JPEG 无 alpha 通道
    LIMX_EXPECT_FALSE(image.HasSourceAlpha);
}

LIMX_TEST(JpegDecoder, TracksDcPredictorAcrossBlocks)
{
    FImageData image;
    LIMX_REQUIRE_TRUE(DecodeJpeg(kGray16x16Jpeg, image,
                                 NativeChannelOptions()).Succeeded);

    LIMX_REQUIRE_TRUE(image.IsValid());
    LIMX_REQUIRE_EQ(image.Width, UInt32(16));
    LIMX_REQUIRE_EQ(image.Height, UInt32(16));

    // 每块的 DC 存的是与上一块的差值。若累加有误, 从第二块起亮度就会偏移,
    // 且误差会一路累积到最后一块。
    const Int32 expected[4] = { 64, 96, 160, 224 };

    for (UInt32 blockY = 0; blockY < 2; ++blockY)
    {
        for (UInt32 blockX = 0; blockX < 2; ++blockX)
        {
            const Int32 value = expected[blockY * 2 + blockX];

            // 取块中心以避开可能的边界效应
            const UInt32 x = blockX * 8 + 4;
            const UInt32 y = blockY * 8 + 4;

            LIMX_REQUIRE_TRUE(ChannelNear(image, x, y, 0, value));
        }
    }
}

LIMX_TEST(JpegDecoder, HandlesNonMultipleOfEightDimensions)
{
    FImageData image;
    LIMX_REQUIRE_TRUE(DecodeJpeg(kGray5x3Jpeg, image,
                                 NativeChannelOptions()).Succeeded);

    // 5x3 会被编码为一个 8x8 的 MCU, 解码时须裁剪到声明尺寸
    LIMX_REQUIRE_TRUE(image.IsValid());
    LIMX_EXPECT_EQ(image.Width, UInt32(5));
    LIMX_EXPECT_EQ(image.Height, UInt32(3));
    LIMX_EXPECT_EQ(image.Pixels.GetSize(), SizeType(15));

    for (UInt32 y = 0; y < 3; ++y)
    {
        for (UInt32 x = 0; x < 5; ++x)
        {
            LIMX_REQUIRE_TRUE(ChannelNear(image, x, y, 0, 200));
        }
    }
}

// ============================================================================
// 彩色
// ============================================================================

LIMX_TEST(JpegDecoder, DecodesYCbCr444)
{
    FImageData image;
    LIMX_REQUIRE_TRUE(DecodeJpeg(kColor444Jpeg, image).Succeeded);

    LIMX_REQUIRE_TRUE(image.IsValid());
    LIMX_EXPECT_EQ(image.Width, UInt32(8));

    // Y=128 Cb=128 Cr=200 经 JFIF 全范围转换后约为 (228, 77, 128)
    for (UInt32 y = 0; y < 8; ++y)
    {
        for (UInt32 x = 0; x < 8; ++x)
        {
            LIMX_REQUIRE_TRUE(ChannelNear(image, x, y, 0, 228));
            LIMX_REQUIRE_TRUE(ChannelNear(image, x, y, 1, 77));
            LIMX_REQUIRE_TRUE(ChannelNear(image, x, y, 2, 128));
        }
    }
}

LIMX_TEST(JpegDecoder, DecodesChromaSubsampled420)
{
    FImageData image;
    LIMX_REQUIRE_TRUE(DecodeJpeg(kColor420Jpeg, image).Succeeded);

    LIMX_REQUIRE_TRUE(image.IsValid());
    LIMX_REQUIRE_EQ(image.Width, UInt32(16));
    LIMX_REQUIRE_EQ(image.Height, UInt32(16));

    // 4:2:0 下色度平面只有亮度的四分之一。色度取中性 (128,128),
    // 因此结果应是纯灰阶 —— 上采样坐标算错会让某些区块染上颜色。
    const Int32 expected[4] = { 100, 140, 180, 220 };

    for (UInt32 blockY = 0; blockY < 2; ++blockY)
    {
        for (UInt32 blockX = 0; blockX < 2; ++blockX)
        {
            const Int32 value = expected[blockY * 2 + blockX];

            const UInt32 x = blockX * 8 + 4;
            const UInt32 y = blockY * 8 + 4;

            // 中性色度下 R/G/B 应彼此相等且等于亮度
            LIMX_REQUIRE_TRUE(ChannelNear(image, x, y, 0, value));
            LIMX_REQUIRE_TRUE(ChannelNear(image, x, y, 1, value));
            LIMX_REQUIRE_TRUE(ChannelNear(image, x, y, 2, value));
        }
    }
}

LIMX_TEST(JpegDecoder, ReportsSrgbColorSpace)
{
    FImageData image;
    LIMX_REQUIRE_TRUE(DecodeJpeg(kColor444Jpeg, image).Succeeded);

    // JPEG 的像素数据按惯例是 sRGB
    LIMX_EXPECT_EQ(static_cast<Int32>(image.ColorSpace),
                   static_cast<Int32>(EImageColorSpace::Srgb));
}

// ============================================================================
// 选项
// ============================================================================

LIMX_TEST(JpegDecoder, FlipsVerticallyWhenRequested)
{
    FImageDecodeOptions options;
    options.ForceFourChannels = false;
    options.FlipVertically    = true;

    FImageData image;
    LIMX_REQUIRE_TRUE(DecodeJpeg(kGray16x16Jpeg, image, options).Succeeded);

    // 翻转后原本在下半部的块 (160/224) 应出现在上半部
    LIMX_EXPECT_TRUE(ChannelNear(image, 4, 4, 0, 160));
    LIMX_EXPECT_TRUE(ChannelNear(image, 12, 4, 0, 224));
    LIMX_EXPECT_TRUE(ChannelNear(image, 4, 12, 0, 64));
}

// ============================================================================
// 拒绝
// ============================================================================

LIMX_TEST(JpegDecoder, RejectsNonJpegData)
{
    const UInt8 garbage[] = { 0x89, 0x50, 0x4E, 0x47, 0x00, 0x00 };

    FImageData image;
    const FImageDecodeResult result =
        FJpegDecoder::Decode(garbage, sizeof(garbage), image);

    LIMX_EXPECT_FALSE(result.Succeeded);
    LIMX_EXPECT_FALSE(result.ErrorMessage.IsEmpty());
}

LIMX_TEST(JpegDecoder, RejectsProgressiveJpeg)
{
    TArray<UInt8> bytes;
    LIMX_REQUIRE_TRUE(FBase64::Decode(kGray8x8Jpeg, bytes));

    // 把 SOF0 (0xC0) 改成 SOF2 (0xC2) 伪装成渐进式
    for (SizeType i = 0; i + 1 < bytes.GetSize(); ++i)
    {
        if (bytes[i] == 0xFF && bytes[i + 1] == 0xC0)
        {
            bytes[i + 1] = 0xC2;
            break;
        }
    }

    FImageData image;
    const FImageDecodeResult result =
        FJpegDecoder::Decode(bytes.GetData(), bytes.GetSize(), image);

    // 渐进式若被当作基线解码会产出花屏 — 必须明确拒绝
    LIMX_EXPECT_FALSE(result.Succeeded);
    LIMX_EXPECT_FALSE(result.ErrorMessage.IsEmpty());
}

LIMX_TEST(JpegDecoder, RejectsTruncatedFile)
{
    TArray<UInt8> bytes;
    LIMX_REQUIRE_TRUE(FBase64::Decode(kGray8x8Jpeg, bytes));

    // 只保留前 20 字节
    TArray<UInt8> truncated;
    for (SizeType i = 0; i < 20 && i < bytes.GetSize(); ++i)
    {
        truncated.Add(bytes[i]);
    }

    FImageData image;
    LIMX_EXPECT_FALSE(FJpegDecoder::Decode(truncated.GetData(),
                                           truncated.GetSize(),
                                           image).Succeeded);
}

LIMX_TEST(JpegDecoder, RejectsMissingScanData)
{
    // 只有 SOI 与 EOI, 没有帧头也没有扫描
    const UInt8 empty[] = { 0xFF, 0xD8, 0xFF, 0xD9 };

    FImageData image;
    const FImageDecodeResult result =
        FJpegDecoder::Decode(empty, sizeof(empty), image);

    LIMX_EXPECT_FALSE(result.Succeeded);
}

// ============================================================================
// 数据一致性
// ============================================================================

LIMX_TEST(JpegDecoder, PixelBufferMatchesDeclaredSize)
{
    FImageData image;
    LIMX_REQUIRE_TRUE(DecodeJpeg(kColor420Jpeg, image).Succeeded);

    LIMX_EXPECT_EQ(image.GetRowPitch(),
                   static_cast<SizeType>(image.Width) * image.GetBytesPerPixel());
    LIMX_EXPECT_EQ(image.Pixels.GetSize(), image.GetExpectedByteSize());
    LIMX_EXPECT_TRUE(image.IsValid());
}

LIMX_TEST(JpegDecoder, DecodeReplacesPreviousImage)
{
    FImageData image;

    LIMX_REQUIRE_TRUE(DecodeJpeg(kColor420Jpeg, image).Succeeded);
    LIMX_REQUIRE_EQ(image.Width, UInt32(16));

    LIMX_REQUIRE_TRUE(DecodeJpeg(kGray8x8Jpeg, image).Succeeded);
    LIMX_EXPECT_EQ(image.Width, UInt32(8));
    LIMX_EXPECT_EQ(image.Height, UInt32(8));
    LIMX_EXPECT_TRUE(image.IsValid());
}
