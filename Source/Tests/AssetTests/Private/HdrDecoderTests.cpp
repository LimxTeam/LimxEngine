/*******************************************************************************
 * 文件: HdrDecoderTests.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   Radiance HDR 解码器单元测试 — RGBE 数值转换、两种 RLE、头部解析、
 *   扫描线方向与损坏数据的处理
 *
 * 设计哲学:
 *   数值必须精确核对而非"接近" — RGBE 的解码是纯粹的整数乘 2 的幂,
 *   结果在浮点里是**精确**可表示的。用容差断言会放过一整类错误:
 *   偏移量差 1 (亮度整体两倍或一半)、通道错位、指数偏置写错 128 而非 136。
 *   这些错误解出来的图看着都"像 HDR", 只有精确比较能抓住。
 *
 *   夹具在测试内按格式手工拼字节 — HDR 的二进制结构简单到可以直接写出来,
 *   而每种排列 (新版游程/老式逐像素/重复标记) 都是独立的失败路径。用真实
 *   文件当夹具只能覆盖写出它的那个工具选的那一种排列。
 *
 *   损坏数据要断言"失败"而非"不崩" — 游程长度越界这类输入在小图上可能
 *   恰好落在已分配内存内, 不崩不代表没越界。断言解码返回失败, 才是把
 *   边界检查本身钉住。
 *
 * 技术特性:
 *   - 覆盖新版自适应 RLE、老式逐像素、(1,1,1,n) 重复标记三条路径
 *   - 覆盖 -Y/+Y 与 +X/-X 四种扫描线方向
 *   - 零长度直传游程 (会被静默跳过的非法输入) 单独设例
 *
 * 依赖关系:
 *   内部: AssetTests/AssetTestsMinimal.h
 *
 ******************************************************************************/

#include "AssetTests/AssetTestsMinimal.h"

using namespace Limx;

namespace
{

// ============================================================================
// 夹具构造
// ============================================================================

/// 逐字节拼一个 HDR 文件
class FHdrBuilder
{
public:
    void AddText(const AnsiChar* text)
    {
        for (SizeType i = 0; text[i] != '\0'; ++i)
        {
            m_Bytes.Add(static_cast<UInt8>(text[i]));
        }
    }

    void AddByte(UInt8 value)
    {
        m_Bytes.Add(value);
    }

    /// 标准头部 + 分辨率行
    void AddHeader(const AnsiChar* resolutionLine)
    {
        AddText("#?RADIANCE\n");
        AddText("FORMAT=32-bit_rle_rgbe\n");
        AddText("\n");
        AddText(resolutionLine);
        AddText("\n");
    }

    /// 一个老式排列的像素
    void AddPixel(UInt8 r, UInt8 g, UInt8 b, UInt8 e)
    {
        AddByte(r);
        AddByte(g);
        AddByte(b);
        AddByte(e);
    }

    /// 新版自适应 RLE 的行首标志
    void AddAdaptiveMarker(UInt32 width)
    {
        AddByte(2);
        AddByte(2);
        AddByte(static_cast<UInt8>((width >> 8) & 0xFF));
        AddByte(static_cast<UInt8>(width & 0xFF));
    }

    /// 一段直传游程
    void AddLiteralRun(const UInt8* values, UInt32 count)
    {
        AddByte(static_cast<UInt8>(count));

        for (UInt32 i = 0; i < count; ++i)
        {
            AddByte(values[i]);
        }
    }

    /// 一段重复游程
    void AddRepeatRun(UInt8 value, UInt32 count)
    {
        AddByte(static_cast<UInt8>(count + 128));
        AddByte(value);
    }

    LIMX_NODISCARD const TArray<UInt8>& GetBytes() const
    {
        return m_Bytes;
    }

    LIMX_NODISCARD FImageDecodeResult Decode(
        FImageData& outImage,
        const FImageDecodeOptions& options = FImageDecodeOptions()) const
    {
        return FHdrDecoder::Decode(m_Bytes.GetData(), m_Bytes.GetSize(),
                                   outImage, options);
    }

private:
    TArray<UInt8> m_Bytes;
};

/// 取指定像素的指定通道 (浮点格式)
Float32 GetFloat(const FImageData& image, UInt32 x, UInt32 y, UInt32 channel)
{
    const SizeType index =
        (static_cast<SizeType>(y) * image.Width + x) * image.GetChannelCount() +
        channel;

    const SizeType byteOffset = index * sizeof(Float32);

    if (byteOffset + sizeof(Float32) > image.Pixels.GetSize())
    {
        return -1.0f;
    }

    return reinterpret_cast<const Float32*>(image.Pixels.GetData())[index];
}

/// 构造一张 8x2 的新版 RLE 图
///
/// 像素 (x, y) 的 RGBE 为 (x*16, y*16+1, 200, 136) —— 指数 136 意味着
/// 缩放恰为 1.0, 因此浮点值就是字节值本身, 便于心算核对。
FHdrBuilder BuildAdaptiveFixture()
{
    constexpr UInt32 kWidth  = 8;
    constexpr UInt32 kHeight = 2;

    FHdrBuilder builder;
    builder.AddHeader("-Y 2 +X 8");

    for (UInt32 y = 0; y < kHeight; ++y)
    {
        builder.AddAdaptiveMarker(kWidth);

        // R: 直传 (每个像素都不同)
        UInt8 red[kWidth] = {};
        for (UInt32 x = 0; x < kWidth; ++x)
        {
            red[x] = static_cast<UInt8>(x * 16);
        }
        builder.AddLiteralRun(red, kWidth);

        // G: 整行同值 —— 走重复游程
        builder.AddRepeatRun(static_cast<UInt8>(y * 16 + 1), kWidth);

        // B: 整行同值
        builder.AddRepeatRun(200, kWidth);

        // E: 整行同值
        builder.AddRepeatRun(136, kWidth);
    }

    return builder;
}

} // namespace

// ============================================================================
// RGBE 数值转换
// ============================================================================

LIMX_TEST(HdrDecoder, ExponentBiasIsExact)
{
    // 尾数 128、指数 136 时缩放为 2^0 = 1, 结果精确等于 128。
    // 偏置若写成 128 (漏掉尾数的 8 位归一化), 这里会得到 32768。
    Float32 r = 0.0f;
    Float32 g = 0.0f;
    Float32 b = 0.0f;

    FHdrDecoder::RgbeToLinear(128, 64, 32, 136, r, g, b);

    LIMX_EXPECT_TRUE(r == 128.0f);
    LIMX_EXPECT_TRUE(g == 64.0f);
    LIMX_EXPECT_TRUE(b == 32.0f);
}

LIMX_TEST(HdrDecoder, StandardExponentGivesNormalizedRange)
{
    // e=128 是"值落在 [0,1)"的常规情形: 缩放 2^-8 = 1/256
    Float32 r = 0.0f;
    Float32 g = 0.0f;
    Float32 b = 0.0f;

    FHdrDecoder::RgbeToLinear(128, 255, 0, 128, r, g, b);

    LIMX_EXPECT_TRUE(r == 0.5f);
    LIMX_EXPECT_TRUE(g == 255.0f / 256.0f);
    LIMX_EXPECT_TRUE(b == 0.0f);
}

LIMX_TEST(HdrDecoder, ZeroExponentIsExactlyBlack)
{
    // 指数 0 是格式约定的纯黑。若照常缩放会得到 2^-136 量级的非规格化数,
    // 数值上等价于零但会污染后续的辐照度积分。
    Float32 r = 1.0f;
    Float32 g = 1.0f;
    Float32 b = 1.0f;

    FHdrDecoder::RgbeToLinear(255, 255, 255, 0, r, g, b);

    LIMX_EXPECT_TRUE(r == 0.0f);
    LIMX_EXPECT_TRUE(g == 0.0f);
    LIMX_EXPECT_TRUE(b == 0.0f);
}

LIMX_TEST(HdrDecoder, ExponentSpansHighDynamicRange)
{
    // HDR 的意义就在这里: 亮度必须能远超 1.0
    Float32 r = 0.0f;
    Float32 g = 0.0f;
    Float32 b = 0.0f;

    // e=150 → 2^14 = 16384; 尾数 128 → 128 * 16384
    FHdrDecoder::RgbeToLinear(128, 128, 128, 150, r, g, b);

    LIMX_EXPECT_TRUE(r == 128.0f * 16384.0f);
    LIMX_EXPECT_TRUE(r > 1.0f);
}

LIMX_TEST(HdrDecoder, PowerTableIsExactAcrossFullExponentRange)
{
    // 逐个指数验证"相邻两级恰好差两倍" —— 查表若在某一项上舍入,
    // 这个比值会偏离 2, 而单看解码结果完全看不出来。
    for (UInt32 e = 2; e < 256; ++e)
    {
        Float32 lowR = 0.0f, lowG = 0.0f, lowB = 0.0f;
        Float32 highR = 0.0f, highG = 0.0f, highB = 0.0f;

        FHdrDecoder::RgbeToLinear(128, 0, 0, static_cast<UInt8>(e - 1),
                                  lowR, lowG, lowB);
        FHdrDecoder::RgbeToLinear(128, 0, 0, static_cast<UInt8>(e),
                                  highR, highG, highB);

        LIMX_EXPECT_TRUE(highR == lowR * 2.0f);
    }
}

// ============================================================================
// 签名识别
// ============================================================================

LIMX_TEST(HdrDecoder, RecognizesBothSignatures)
{
    const AnsiChar* radiance = "#?RADIANCE\n";
    const AnsiChar* rgbe     = "#?RGBE\n";

    LIMX_EXPECT_TRUE(FHdrDecoder::IsHdr(
        reinterpret_cast<const UInt8*>(radiance), 11));
    LIMX_EXPECT_TRUE(FHdrDecoder::IsHdr(
        reinterpret_cast<const UInt8*>(rgbe), 7));
}

LIMX_TEST(HdrDecoder, RejectsNonHdrData)
{
    const UInt8 pngMagic[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    const AnsiChar* text    = "#?SOMETHING\n";

    LIMX_EXPECT_FALSE(FHdrDecoder::IsHdr(pngMagic, 8));
    LIMX_EXPECT_FALSE(FHdrDecoder::IsHdr(
        reinterpret_cast<const UInt8*>(text), 12));
    LIMX_EXPECT_FALSE(FHdrDecoder::IsHdr(nullptr, 0));
    LIMX_EXPECT_FALSE(FHdrDecoder::IsHdr(pngMagic, 2));
}

LIMX_TEST(HdrDecoder, UnifiedEntryPointDispatchesToHdr)
{
    // 统一入口漏掉分派时的表现是"无法识别的图像格式", 与解码器本身无关
    const FHdrBuilder builder = BuildAdaptiveFixture();

    FImageData image;
    const FImageDecodeResult result = FImageDecoder::Decode(
        builder.GetBytes().GetData(), builder.GetBytes().GetSize(), image);

    LIMX_EXPECT_TRUE(result.Succeeded);
    LIMX_EXPECT_EQ(image.Width, 8u);
    LIMX_EXPECT_TRUE(image.Format == EImageFormat::RGBA32F);
}

// ============================================================================
// 新版自适应 RLE
// ============================================================================

LIMX_TEST(HdrDecoder, DecodesAdaptiveRle)
{
    const FHdrBuilder builder = BuildAdaptiveFixture();

    FImageData               image;
    const FImageDecodeResult result = builder.Decode(image);

    LIMX_EXPECT_TRUE(result.Succeeded);
    LIMX_EXPECT_EQ(image.Width, 8u);
    LIMX_EXPECT_EQ(image.Height, 2u);
    LIMX_EXPECT_TRUE(image.IsValid());

    for (UInt32 y = 0; y < 2; ++y)
    {
        for (UInt32 x = 0; x < 8; ++x)
        {
            // 指数 136 → 缩放 1.0, 浮点值就是字节值
            LIMX_EXPECT_TRUE(GetFloat(image, x, y, 0) ==
                             static_cast<Float32>(x * 16));
            LIMX_EXPECT_TRUE(GetFloat(image, x, y, 1) ==
                             static_cast<Float32>(y * 16 + 1));
            LIMX_EXPECT_TRUE(GetFloat(image, x, y, 2) == 200.0f);
            LIMX_EXPECT_TRUE(GetFloat(image, x, y, 3) == 1.0f);
        }
    }
}

LIMX_TEST(HdrDecoder, AdaptiveRleChannelsAreNotInterleaved)
{
    // 新版 RLE 按通道平铺存储 —— 若当成交错像素解, 会得到一张
    // "颜色全错但结构正常"的图。这里让四个通道值互不相同以钉死这一点。
    FHdrBuilder builder;
    builder.AddHeader("-Y 1 +X 8");
    builder.AddAdaptiveMarker(8);
    builder.AddRepeatRun(10, 8);   // R
    builder.AddRepeatRun(20, 8);   // G
    builder.AddRepeatRun(30, 8);   // B
    builder.AddRepeatRun(136, 8);  // E

    FImageData               image;
    const FImageDecodeResult result = builder.Decode(image);

    LIMX_EXPECT_TRUE(result.Succeeded);

    for (UInt32 x = 0; x < 8; ++x)
    {
        LIMX_EXPECT_TRUE(GetFloat(image, x, 0, 0) == 10.0f);
        LIMX_EXPECT_TRUE(GetFloat(image, x, 0, 1) == 20.0f);
        LIMX_EXPECT_TRUE(GetFloat(image, x, 0, 2) == 30.0f);
    }
}

LIMX_TEST(HdrDecoder, MixedRunsWithinOneChannel)
{
    // 一个通道内直传与重复交替 —— 游标推进算错时只有部分列错位
    const UInt8 head[3] = { 1, 2, 3 };
    const UInt8 tail[2] = { 7, 8 };

    FHdrBuilder builder;
    builder.AddHeader("-Y 1 +X 8");
    builder.AddAdaptiveMarker(8);

    builder.AddLiteralRun(head, 3);  // R: 1 2 3
    builder.AddRepeatRun(5, 3);      // R: 5 5 5
    builder.AddLiteralRun(tail, 2);  // R: 7 8

    builder.AddRepeatRun(0, 8);      // G
    builder.AddRepeatRun(0, 8);      // B
    builder.AddRepeatRun(136, 8);    // E

    FImageData               image;
    const FImageDecodeResult result = builder.Decode(image);

    LIMX_EXPECT_TRUE(result.Succeeded);

    const Float32 expected[8] = { 1.0f, 2.0f, 3.0f, 5.0f,
                                  5.0f, 5.0f, 7.0f, 8.0f };

    for (UInt32 x = 0; x < 8; ++x)
    {
        LIMX_EXPECT_TRUE(GetFloat(image, x, 0, 0) == expected[x]);
    }
}

// ============================================================================
// 老式逐像素排列
// ============================================================================

LIMX_TEST(HdrDecoder, DecodesFlatPixels)
{
    // 宽度小于 8 时新版 RLE 不适用, 文件必然是逐像素排列
    FHdrBuilder builder;
    builder.AddHeader("-Y 2 +X 2");
    builder.AddPixel(10, 20, 30, 136);
    builder.AddPixel(40, 50, 60, 136);
    builder.AddPixel(70, 80, 90, 136);
    builder.AddPixel(100, 110, 120, 136);

    FImageData               image;
    const FImageDecodeResult result = builder.Decode(image);

    LIMX_EXPECT_TRUE(result.Succeeded);
    LIMX_EXPECT_EQ(image.Width, 2u);
    LIMX_EXPECT_EQ(image.Height, 2u);

    LIMX_EXPECT_TRUE(GetFloat(image, 0, 0, 0) == 10.0f);
    LIMX_EXPECT_TRUE(GetFloat(image, 1, 0, 1) == 50.0f);
    LIMX_EXPECT_TRUE(GetFloat(image, 0, 1, 2) == 90.0f);
    LIMX_EXPECT_TRUE(GetFloat(image, 1, 1, 0) == 100.0f);
}

LIMX_TEST(HdrDecoder, WideImageWithoutAdaptiveMarkerFallsBackToFlat)
{
    // 宽度够用新版 RLE, 但文件是逐像素写的 —— 首行判定必须能回退,
    // 否则整张图会被当成损坏数据拒掉。
    FHdrBuilder builder;
    builder.AddHeader("-Y 1 +X 8");

    for (UInt32 x = 0; x < 8; ++x)
    {
        builder.AddPixel(static_cast<UInt8>(x + 1), 0, 0, 136);
    }

    FImageData               image;
    const FImageDecodeResult result = builder.Decode(image);

    LIMX_EXPECT_TRUE(result.Succeeded);

    for (UInt32 x = 0; x < 8; ++x)
    {
        LIMX_EXPECT_TRUE(GetFloat(image, x, 0, 0) == static_cast<Float32>(x + 1));
    }
}

LIMX_TEST(HdrDecoder, RepeatMarkerDuplicatesPreviousPixel)
{
    // (1,1,1,n) 表示重复上一像素 n 次
    FHdrBuilder builder;
    builder.AddHeader("-Y 1 +X 5");
    builder.AddPixel(77, 88, 99, 136);
    builder.AddPixel(1, 1, 1, 3);       // 重复 3 次
    builder.AddPixel(11, 22, 33, 136);

    FImageData               image;
    const FImageDecodeResult result = builder.Decode(image);

    LIMX_EXPECT_TRUE(result.Succeeded);

    for (UInt32 x = 0; x < 4; ++x)
    {
        LIMX_EXPECT_TRUE(GetFloat(image, x, 0, 0) == 77.0f);
        LIMX_EXPECT_TRUE(GetFloat(image, x, 0, 1) == 88.0f);
    }

    LIMX_EXPECT_TRUE(GetFloat(image, 4, 0, 0) == 11.0f);
}

LIMX_TEST(HdrDecoder, LeadingRepeatMarkerIsRejected)
{
    // 首像素就是重复标记时没有"上一像素"可复制
    FHdrBuilder builder;
    builder.AddHeader("-Y 1 +X 4");
    builder.AddPixel(1, 1, 1, 2);
    builder.AddPixel(0, 0, 0, 0);
    builder.AddPixel(0, 0, 0, 0);

    FImageData               image;
    const FImageDecodeResult result = builder.Decode(image);

    LIMX_EXPECT_FALSE(result.Succeeded);
}

// ============================================================================
// 扫描线方向
// ============================================================================

LIMX_TEST(HdrDecoder, PositiveYPutsFirstRowAtBottom)
{
    // `+Y h` 表示数据首行是图像**底**行 —— 解错会让整张环境图上下颠倒,
    // 天在脚下。这种错误在天空盒里格外容易被当成"朝向没调对"。
    FHdrBuilder builder;
    builder.AddHeader("+Y 2 +X 2");
    builder.AddPixel(1, 0, 0, 136);   // 数据首行 → 图像底行
    builder.AddPixel(1, 0, 0, 136);
    builder.AddPixel(2, 0, 0, 136);   // 数据次行 → 图像顶行
    builder.AddPixel(2, 0, 0, 136);

    FImageData               image;
    const FImageDecodeResult result = builder.Decode(image);

    LIMX_EXPECT_TRUE(result.Succeeded);
    LIMX_EXPECT_TRUE(GetFloat(image, 0, 0, 0) == 2.0f);
    LIMX_EXPECT_TRUE(GetFloat(image, 0, 1, 0) == 1.0f);
}

LIMX_TEST(HdrDecoder, NegativeXPutsFirstColumnAtRight)
{
    FHdrBuilder builder;
    builder.AddHeader("-Y 1 -X 2");
    builder.AddPixel(1, 0, 0, 136);   // 数据首列 → 图像右列
    builder.AddPixel(2, 0, 0, 136);

    FImageData               image;
    const FImageDecodeResult result = builder.Decode(image);

    LIMX_EXPECT_TRUE(result.Succeeded);
    LIMX_EXPECT_TRUE(GetFloat(image, 0, 0, 0) == 2.0f);
    LIMX_EXPECT_TRUE(GetFloat(image, 1, 0, 0) == 1.0f);
}

LIMX_TEST(HdrDecoder, FlipOptionIsIndependentOfFileOrientation)
{
    // 选项的翻转是调用方的意图, 分辨率行的方向是文件的事实 —— 两者叠加。
    // 若实现把它们合并成一个开关, 二者同时生效时会互相抵消。
    FImageDecodeOptions flipped;
    flipped.FlipVertically = true;

    FHdrBuilder builder;
    builder.AddHeader("+Y 2 +X 2");
    builder.AddPixel(1, 0, 0, 136);
    builder.AddPixel(1, 0, 0, 136);
    builder.AddPixel(2, 0, 0, 136);
    builder.AddPixel(2, 0, 0, 136);

    FImageData               image;
    const FImageDecodeResult result = builder.Decode(image, flipped);

    LIMX_EXPECT_TRUE(result.Succeeded);

    // +Y 先把首行放到底部, 选项再翻回来
    LIMX_EXPECT_TRUE(GetFloat(image, 0, 0, 0) == 1.0f);
    LIMX_EXPECT_TRUE(GetFloat(image, 0, 1, 0) == 2.0f);
}

// ============================================================================
// 通道数选项
// ============================================================================

LIMX_TEST(HdrDecoder, ForceFourChannelsAppendsOpaqueAlpha)
{
    const FHdrBuilder builder = BuildAdaptiveFixture();

    FImageData               image;
    const FImageDecodeResult result = builder.Decode(image);

    LIMX_EXPECT_TRUE(result.Succeeded);
    LIMX_EXPECT_TRUE(image.Format == EImageFormat::RGBA32F);
    LIMX_EXPECT_EQ(image.GetChannelCount(), 4u);
    LIMX_EXPECT_EQ(image.GetBytesPerPixel(), 16u);

    // RGBE 本身没有 alpha —— 补出来的 alpha 不该被当成源数据
    LIMX_EXPECT_FALSE(image.HasSourceAlpha);
    LIMX_EXPECT_TRUE(GetFloat(image, 3, 0, 3) == 1.0f);
}

LIMX_TEST(HdrDecoder, ThreeChannelOutputOmitsAlpha)
{
    FImageDecodeOptions options;
    options.ForceFourChannels = false;

    const FHdrBuilder builder = BuildAdaptiveFixture();

    FImageData               image;
    const FImageDecodeResult result = builder.Decode(image, options);

    LIMX_EXPECT_TRUE(result.Succeeded);
    LIMX_EXPECT_TRUE(image.Format == EImageFormat::RGB32F);
    LIMX_EXPECT_EQ(image.GetChannelCount(), 3u);
    LIMX_EXPECT_EQ(image.GetBytesPerPixel(), 12u);
    LIMX_EXPECT_TRUE(image.IsValid());
}

LIMX_TEST(HdrDecoder, ColorSpaceIsLinear)
{
    // HDR 存的是线性辐射度。标成 sRGB 会让上传层再做一次伽马转换,
    // 结果是整张环境图被额外提亮一次。
    const FHdrBuilder builder = BuildAdaptiveFixture();

    FImageData image;
    LIMX_EXPECT_TRUE(builder.Decode(image).Succeeded);
    LIMX_EXPECT_TRUE(image.ColorSpace == EImageColorSpace::Linear);
}

// ============================================================================
// 头部解析
// ============================================================================

LIMX_TEST(HdrDecoder, AcceptsCrLfLineEndings)
{
    // 经 Windows 工具转存的文件里 CRLF 并不罕见, 而多出来的 CR 会让
    // FORMAT 行的比较失败, 报成"不支持的像素格式"这种误导性错误
    FHdrBuilder builder;
    builder.AddText("#?RADIANCE\r\n");
    builder.AddText("FORMAT=32-bit_rle_rgbe\r\n");
    builder.AddText("\r\n");
    builder.AddText("-Y 1 +X 2\r\n");
    builder.AddPixel(42, 0, 0, 136);
    builder.AddPixel(0, 0, 0, 136);

    FImageData               image;
    const FImageDecodeResult result = builder.Decode(image);

    LIMX_EXPECT_TRUE(result.Succeeded);
    LIMX_EXPECT_EQ(image.Width, 2u);
    LIMX_EXPECT_TRUE(GetFloat(image, 0, 0, 0) == 42.0f);
}

LIMX_TEST(HdrDecoder, SkipsCommentAndMetadataLines)
{
    FHdrBuilder builder;
    builder.AddText("#?RADIANCE\n");
    builder.AddText("# Created by some tool\n");
    builder.AddText("EXPOSURE=1.0\n");
    builder.AddText("SOFTWARE=whatever\n");
    builder.AddText("FORMAT=32-bit_rle_rgbe\n");
    builder.AddText("\n");
    builder.AddText("-Y 1 +X 2\n");
    builder.AddPixel(7, 0, 0, 136);
    builder.AddPixel(0, 0, 0, 136);

    FImageData               image;
    const FImageDecodeResult result = builder.Decode(image);

    LIMX_EXPECT_TRUE(result.Succeeded);
    LIMX_EXPECT_TRUE(GetFloat(image, 0, 0, 0) == 7.0f);
}

LIMX_TEST(HdrDecoder, MissingFormatLineWarnsButDecodes)
{
    FHdrBuilder builder;
    builder.AddText("#?RADIANCE\n");
    builder.AddText("\n");
    builder.AddText("-Y 1 +X 2\n");
    builder.AddPixel(9, 0, 0, 136);
    builder.AddPixel(0, 0, 0, 136);

    FImageData               image;
    const FImageDecodeResult result = builder.Decode(image);

    LIMX_EXPECT_TRUE(result.Succeeded);
    LIMX_EXPECT_GT(result.Warnings.GetSize(), static_cast<SizeType>(0));
    LIMX_EXPECT_TRUE(GetFloat(image, 0, 0, 0) == 9.0f);
}

LIMX_TEST(HdrDecoder, RejectsXyzeColorSpace)
{
    // XYZE 用 CIE XYZ 而非 RGB —— 按 RGBE 解会得到一张色相全错的图,
    // 而且"看起来只是偏色", 不像解码错误
    FHdrBuilder builder;
    builder.AddText("#?RADIANCE\n");
    builder.AddText("FORMAT=32-bit_rle_xyze\n");
    builder.AddText("\n");
    builder.AddText("-Y 1 +X 2\n");
    builder.AddPixel(1, 2, 3, 136);
    builder.AddPixel(1, 2, 3, 136);

    FImageData               image;
    const FImageDecodeResult result = builder.Decode(image);

    LIMX_EXPECT_FALSE(result.Succeeded);
    LIMX_EXPECT_FALSE(result.ErrorMessage.IsEmpty());
}

LIMX_TEST(HdrDecoder, RejectsMalformedResolutionLine)
{
    const AnsiChar* badLines[] = {
        "1024 2048",       // 缺方向标记
        "-Z 2 +X 2",       // 轴名错误
        "-Y +X 2",         // 缺高度
        "-Y 2",            // 缺宽度部分
        "-Y 0 +X 2",       // 零高度
    };

    for (SizeType i = 0; i < sizeof(badLines) / sizeof(badLines[0]); ++i)
    {
        FHdrBuilder builder;
        builder.AddText("#?RADIANCE\n");
        builder.AddText("FORMAT=32-bit_rle_rgbe\n");
        builder.AddText("\n");
        builder.AddText(badLines[i]);
        builder.AddText("\n");
        builder.AddPixel(1, 2, 3, 136);

        FImageData               image;
        const FImageDecodeResult result = builder.Decode(image);

        LIMX_EXPECT_FALSE(result.Succeeded);
    }
}

// ============================================================================
// 损坏数据
// ============================================================================

LIMX_TEST(HdrDecoder, TruncatedPixelDataFails)
{
    // 声称 2x2 却只给了 3 个像素
    FHdrBuilder builder;
    builder.AddHeader("-Y 2 +X 2");
    builder.AddPixel(1, 2, 3, 136);
    builder.AddPixel(1, 2, 3, 136);
    builder.AddPixel(1, 2, 3, 136);

    FImageData               image;
    const FImageDecodeResult result = builder.Decode(image);

    LIMX_EXPECT_FALSE(result.Succeeded);
}

LIMX_TEST(HdrDecoder, TruncatedAdaptiveRunFails)
{
    // 游程声称有 8 个直传字节, 实际只跟了 2 个
    FHdrBuilder builder;
    builder.AddHeader("-Y 1 +X 8");
    builder.AddAdaptiveMarker(8);
    builder.AddByte(8);   // 直传 8 个
    builder.AddByte(1);
    builder.AddByte(2);   // 到此为止

    FImageData               image;
    const FImageDecodeResult result = builder.Decode(image);

    LIMX_EXPECT_FALSE(result.Succeeded);
}

LIMX_TEST(HdrDecoder, ZeroLengthLiteralRunFails)
{
    // 长度为 0 的直传游程在格式里没有定义。危险之处不是死循环 (游标每轮
    // 至少推进一字节), 而是**静默跳过**: 后面的字节照常解出来, 一份已经
    // 错位的文件于是"解码成功"。
    //
    // 因此这个夹具的零长度字节后面跟着一段完整合法的编码 —— 少了判定就会
    // 得到一张成功解出的图, 而不是一次失败。断言"失败"才真正钉住这条检查。
    FHdrBuilder builder;
    builder.AddHeader("-Y 1 +X 8");
    builder.AddAdaptiveMarker(8);
    builder.AddByte(0);           // 零长度直传 —— 非法
    builder.AddRepeatRun(11, 8);  // R
    builder.AddRepeatRun(22, 8);  // G
    builder.AddRepeatRun(33, 8);  // B
    builder.AddRepeatRun(136, 8); // E

    FImageData               image;
    const FImageDecodeResult result = builder.Decode(image);

    LIMX_EXPECT_FALSE(result.Succeeded);
}

LIMX_TEST(HdrDecoder, OverlongRunFails)
{
    // 游程长度超出行宽 —— 少一次检查就是一次堆越界写
    FHdrBuilder builder;
    builder.AddHeader("-Y 1 +X 8");
    builder.AddAdaptiveMarker(8);
    builder.AddRepeatRun(5, 100);  // 声称重复 100 次, 行宽只有 8

    FImageData               image;
    const FImageDecodeResult result = builder.Decode(image);

    LIMX_EXPECT_FALSE(result.Succeeded);
}

LIMX_TEST(HdrDecoder, OverlongRepeatMarkerFails)
{
    FHdrBuilder builder;
    builder.AddHeader("-Y 1 +X 4");
    builder.AddPixel(1, 2, 3, 136);
    builder.AddPixel(1, 1, 1, 200);  // 重复 200 次, 行宽只有 4

    FImageData               image;
    const FImageDecodeResult result = builder.Decode(image);

    LIMX_EXPECT_FALSE(result.Succeeded);
}

LIMX_TEST(HdrDecoder, MismatchedScanlineWidthFails)
{
    // 行首标志声称的宽度与头部不符
    FHdrBuilder builder;
    builder.AddHeader("-Y 1 +X 16");
    builder.AddAdaptiveMarker(8);  // 声称 8, 头部说 16
    builder.AddRepeatRun(1, 8);

    FImageData               image;
    const FImageDecodeResult result = builder.Decode(image);

    LIMX_EXPECT_FALSE(result.Succeeded);
}

LIMX_TEST(HdrDecoder, EmptyAndHeaderOnlyInputFail)
{
    FImageData image;

    LIMX_EXPECT_FALSE(
        FHdrDecoder::Decode(nullptr, 0, image).Succeeded);

    const AnsiChar* headerOnly = "#?RADIANCE\n";
    LIMX_EXPECT_FALSE(FHdrDecoder::Decode(
        reinterpret_cast<const UInt8*>(headerOnly), 11, image).Succeeded);
}

LIMX_TEST(HdrDecoder, FailedDecodeLeavesImageEmpty)
{
    // 失败后 outImage 若残留上一次的内容, 调用方很容易把旧图当新图用
    FImageData image;

    const FHdrBuilder good = BuildAdaptiveFixture();
    LIMX_EXPECT_TRUE(good.Decode(image).Succeeded);
    LIMX_EXPECT_EQ(image.Width, 8u);

    FHdrBuilder bad;
    bad.AddHeader("-Y 2 +X 2");
    bad.AddPixel(1, 2, 3, 136);

    LIMX_EXPECT_FALSE(bad.Decode(image).Succeeded);
    LIMX_EXPECT_EQ(image.Width, 0u);
    LIMX_EXPECT_EQ(image.Pixels.GetSize(), static_cast<SizeType>(0));
    LIMX_EXPECT_FALSE(image.IsValid());
}
