/*******************************************************************************
 * 文件: FPngDecoder.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   PNG 解码实现 — 分块遍历、IHDR 解析、IDAT 解压、五种反滤波、
 *   低位深解包、调色板展开、tRNS 透明度、Adam7 隔行重组
 *
 * 设计哲学:
 *   反滤波按字节而非按像素 — PNG 的滤波器作用于"当前字节"与"左侧一个
 *   像素处的同位置字节"，而非通道或像素整体。以 bpp (每像素字节数，
 *   低位深时向上取整为 1) 为步长逐字节还原，这样五种滤波器都只需一份实现，
 *   不必为每种颜色类型分支。
 *
 *   隔行按遍独立还原 — Adam7 的七遍各有自己的宽高与扫描线，滤波也在
 *   各遍内部独立进行。若把它当成一整幅图去反滤波，得到的是彻底的噪声。
 *   实现上让每遍走同一套反滤波代码，再按各遍的采样格点回填最终图像。
 *
 * 技术特性:
 *   - 多个 IDAT 分块先拼接后统一解压 (规范允许在任意字节处切分)
 *   - 低位深按扫描线解包, 每通道扩展为一字节并按最大值归一化
 *   - tRNS 对调色板与真彩色两种形态分别处理
 *   - 通道扩展与位深降级在同一趟像素转换中完成, 不做多次遍历
 *
 * 依赖关系:
 *   内部: AssetPipeline/FPngDecoder.h, Core/Misc/FInflate.h, Core/Misc/FCrc32.h
 *
 * 注意事项:
 *   分块 CRC 不符只记告警; 真正的数据损坏由 IDAT 的 zlib 校验和拦截
 *
 ******************************************************************************/

#include "AssetPipeline/FPngDecoder.h"
#include "Core/Misc/FInflate.h"
#include "Core/Misc/FCrc32.h"

namespace Limx
{

namespace
{

// ============================================================================
// 常量
// ============================================================================

/// PNG 文件签名
constexpr UInt8 kPngSignature[8] = { 0x89, 0x50, 0x4E, 0x47,
                                     0x0D, 0x0A, 0x1A, 0x0A };

/// 颜色类型
constexpr UInt8 kColorTypeGray      = 0;
constexpr UInt8 kColorTypeRgb       = 2;
constexpr UInt8 kColorTypePalette   = 3;
constexpr UInt8 kColorTypeGrayAlpha = 4;
constexpr UInt8 kColorTypeRgba      = 6;

/// 滤波器类型
constexpr UInt8 kFilterNone    = 0;
constexpr UInt8 kFilterSub     = 1;
constexpr UInt8 kFilterUp      = 2;
constexpr UInt8 kFilterAverage = 3;
constexpr UInt8 kFilterPaeth   = 4;

/// 尺寸上限 — 防止损坏的 IHDR 触发天文数字的分配
constexpr UInt32 kMaxDimension = 65535;

/// Adam7 各遍的起点与步长
constexpr UInt32 kAdam7StartX[7] = { 0, 4, 0, 2, 0, 1, 0 };
constexpr UInt32 kAdam7StartY[7] = { 0, 0, 4, 0, 2, 0, 1 };
constexpr UInt32 kAdam7StepX[7]  = { 8, 8, 4, 4, 2, 2, 1 };
constexpr UInt32 kAdam7StepY[7]  = { 8, 8, 8, 4, 4, 2, 2 };

// ============================================================================
// 字节读取
// ============================================================================

/// 读取大端 UInt32 — PNG 的所有多字节整数都是大端
FORCEINLINE UInt32 ReadBigEndianUInt32(const UInt8* data)
{
    return (static_cast<UInt32>(data[0]) << 24) |
           (static_cast<UInt32>(data[1]) << 16) |
           (static_cast<UInt32>(data[2]) << 8) |
           static_cast<UInt32>(data[3]);
}

/// 判断四字节块类型是否与给定字面量相同
FORCEINLINE bool IsChunkType(const UInt8* data, const AnsiChar* type)
{
    return data[0] == static_cast<UInt8>(type[0]) &&
           data[1] == static_cast<UInt8>(type[1]) &&
           data[2] == static_cast<UInt8>(type[2]) &&
           data[3] == static_cast<UInt8>(type[3]);
}

// ============================================================================
// IHDR
// ============================================================================

/// 图像头信息
struct FPngHeader
{
    UInt32 Width       = 0;
    UInt32 Height      = 0;
    UInt8  BitDepth    = 0;
    UInt8  ColorType   = 0;
    UInt8  Compression = 0;
    UInt8  Filter      = 0;
    UInt8  Interlace   = 0;

    /// 该颜色类型的通道数
    LIMX_NODISCARD UInt32 GetChannelCount() const
    {
        switch (ColorType)
        {
            case kColorTypeGray:      return 1;
            case kColorTypeRgb:       return 3;
            case kColorTypePalette:   return 1;   // 索引, 展开后为 3 或 4
            case kColorTypeGrayAlpha: return 2;
            case kColorTypeRgba:      return 4;
            default:                  return 0;
        }
    }

    /// 每像素比特数 (调色板时为索引位数)
    LIMX_NODISCARD UInt32 GetBitsPerPixel() const
    {
        return GetChannelCount() * BitDepth;
    }

    /// 反滤波使用的字节步长 — 每像素字节数, 不足一字节时取 1
    LIMX_NODISCARD UInt32 GetFilterStride() const
    {
        const UInt32 bits = GetBitsPerPixel();
        return (bits + 7) / 8;
    }

    /// 给定宽度的一行原始字节数 (不含滤波器字节)
    LIMX_NODISCARD SizeType GetRawRowBytes(UInt32 rowWidth) const
    {
        const UInt64 bits =
            static_cast<UInt64>(rowWidth) * GetBitsPerPixel();
        return static_cast<SizeType>((bits + 7) / 8);
    }
};

/// 校验位深与颜色类型的组合是否合法 (规范表 11.2.2)
bool IsValidBitDepthForColorType(UInt8 colorType, UInt8 bitDepth)
{
    switch (colorType)
    {
        case kColorTypeGray:
            return bitDepth == 1 || bitDepth == 2 || bitDepth == 4 ||
                   bitDepth == 8 || bitDepth == 16;

        case kColorTypePalette:
            return bitDepth == 1 || bitDepth == 2 || bitDepth == 4 ||
                   bitDepth == 8;

        case kColorTypeRgb:
        case kColorTypeGrayAlpha:
        case kColorTypeRgba:
            return bitDepth == 8 || bitDepth == 16;

        default:
            return false;
    }
}

// ============================================================================
// 反滤波
// ============================================================================

/// Paeth 预测器 (规范 9.4)
///
/// 在左、上、左上三者中选出与线性预测 a+b-c 最接近的那个。
/// 全程使用整数运算, 与规范给出的参考实现逐步对应。
FORCEINLINE UInt8 PaethPredictor(UInt8 left, UInt8 above, UInt8 upperLeft)
{
    const Int32 a = static_cast<Int32>(left);
    const Int32 b = static_cast<Int32>(above);
    const Int32 c = static_cast<Int32>(upperLeft);

    const Int32 prediction = a + b - c;

    const Int32 distanceA = (prediction > a) ? (prediction - a) : (a - prediction);
    const Int32 distanceB = (prediction > b) ? (prediction - b) : (b - prediction);
    const Int32 distanceC = (prediction > c) ? (prediction - c) : (c - prediction);

    if (distanceA <= distanceB && distanceA <= distanceC)
    {
        return left;
    }

    if (distanceB <= distanceC)
    {
        return above;
    }

    return upperLeft;
}

/// 对一条扫描线做反滤波
///
/// @param filterType  该行的滤波器类型
/// @param current     当前行 (原地还原)
/// @param previous    上一行 (已还原); 首行传 nullptr
/// @param rowBytes    行字节数
/// @param stride      每像素字节数, 低位深时为 1
/// @return 滤波器类型是否合法
bool UnfilterScanline(UInt8 filterType, UInt8* current, const UInt8* previous,
                      SizeType rowBytes, UInt32 stride)
{
    switch (filterType)
    {
        case kFilterNone:
            return true;

        case kFilterSub:
        {
            // 左侧不足一个像素时视为 0
            for (SizeType i = stride; i < rowBytes; ++i)
            {
                current[i] = static_cast<UInt8>(current[i] + current[i - stride]);
            }
            return true;
        }

        case kFilterUp:
        {
            if (previous == nullptr)
            {
                // 首行的上方视为全 0, 等价于不滤波
                return true;
            }

            for (SizeType i = 0; i < rowBytes; ++i)
            {
                current[i] = static_cast<UInt8>(current[i] + previous[i]);
            }
            return true;
        }

        case kFilterAverage:
        {
            for (SizeType i = 0; i < rowBytes; ++i)
            {
                const UInt32 left =
                    (i >= stride) ? static_cast<UInt32>(current[i - stride]) : 0u;
                const UInt32 above =
                    (previous != nullptr) ? static_cast<UInt32>(previous[i]) : 0u;

                // 规范规定取下整平均
                current[i] = static_cast<UInt8>(current[i] + ((left + above) / 2));
            }
            return true;
        }

        case kFilterPaeth:
        {
            for (SizeType i = 0; i < rowBytes; ++i)
            {
                const UInt8 left =
                    (i >= stride) ? current[i - stride] : 0u;
                const UInt8 above =
                    (previous != nullptr) ? previous[i] : 0u;
                const UInt8 upperLeft =
                    (previous != nullptr && i >= stride) ? previous[i - stride] : 0u;

                current[i] = static_cast<UInt8>(
                    current[i] + PaethPredictor(left, above, upperLeft));
            }
            return true;
        }

        default:
            return false;
    }
}

// ============================================================================
// 低位深解包
// ============================================================================

/// 把 1/2/4 位的样本解包为每样本一字节
///
/// @param packed       打包的扫描线
/// @param sampleCount  该行的样本总数 (宽度 × 通道数)
/// @param bitDepth     1、2 或 4
/// @param scaleToByte  是否把值缩放到 0..255 (灰度需要, 调色板索引不需要)
/// @param output       输出, 每样本一字节
void UnpackLowBitDepth(const UInt8* packed, UInt32 sampleCount, UInt8 bitDepth,
                       bool scaleToByte, UInt8* output)
{
    const UInt32 maxValue    = (1u << bitDepth) - 1u;
    const UInt32 perByte     = 8u / bitDepth;

    for (UInt32 i = 0; i < sampleCount; ++i)
    {
        const UInt32 byteIndex = i / perByte;
        const UInt32 slot      = i % perByte;

        // 高位在前
        const UInt32 shift = 8u - bitDepth * (slot + 1u);
        const UInt32 value = (packed[byteIndex] >> shift) & maxValue;

        // 灰度需要映射到完整的 0..255 区间; 调色板索引必须保持原值
        output[i] = scaleToByte
                        ? static_cast<UInt8>((value * 255u) / maxValue)
                        : static_cast<UInt8>(value);
    }
}

// ============================================================================
// 解码上下文
// ============================================================================

/// 解码过程中的共享状态
struct FDecodeContext
{
    FPngHeader Header;

    /// 调色板 (RGB 三元组)
    TArray<UInt8> Palette;

    /// 调色板各项的 alpha (tRNS)
    TArray<UInt8> PaletteAlpha;

    /// 真彩色/灰度的透明色键 (tRNS) — 按 16 位样本值存放
    bool   HasColorKey = false;
    UInt16 ColorKey[3] = {};

    EImageColorSpace ColorSpace = EImageColorSpace::Unspecified;

    TArray<FString>* Warnings = nullptr;
};

/// 把一行已反滤波的原始样本转换为目标像素格式
///
/// @param context       解码上下文
/// @param rawRow        已反滤波的原始行 (低位深已解包为每样本一字节)
/// @param rowWidth      该行的像素数
/// @param destination   目标像素起点
/// @param destChannels  目标通道数
/// @param destBytes     目标每通道字节数
void ConvertRow(const FDecodeContext& context, const UInt8* rawRow,
                UInt32 rowWidth, UInt8* destination,
                UInt32 destChannels, UInt32 destBytes)
{
    const FPngHeader& header = context.Header;

    const UInt32 sourceChannels = header.GetChannelCount();
    const bool   sourceIs16Bit  = (header.BitDepth == 16);

    for (UInt32 x = 0; x < rowWidth; ++x)
    {
        // 先把源像素统一取为最多四个 16 位样本
        UInt16 samples[4] = { 0, 0, 0, 65535 };

        if (header.ColorType == kColorTypePalette)
        {
            // 索引在解包阶段已扩展为一字节
            const UInt32 index = rawRow[x];

            const SizeType paletteOffset = static_cast<SizeType>(index) * 3;

            if (paletteOffset + 2 < context.Palette.GetSize())
            {
                // 调色板项是 8 位, 扩展到 16 位区间
                samples[0] = static_cast<UInt16>(
                    context.Palette[paletteOffset + 0] * 257u);
                samples[1] = static_cast<UInt16>(
                    context.Palette[paletteOffset + 1] * 257u);
                samples[2] = static_cast<UInt16>(
                    context.Palette[paletteOffset + 2] * 257u);
            }

            samples[3] = (index < context.PaletteAlpha.GetSize())
                             ? static_cast<UInt16>(
                                   context.PaletteAlpha[index] * 257u)
                             : 65535;
        }
        else
        {
            for (UInt32 c = 0; c < sourceChannels; ++c)
            {
                if (sourceIs16Bit)
                {
                    const SizeType offset =
                        (static_cast<SizeType>(x) * sourceChannels + c) * 2;

                    // 文件是大端
                    samples[c] = static_cast<UInt16>(
                        (static_cast<UInt32>(rawRow[offset]) << 8) |
                        rawRow[offset + 1]);
                }
                else
                {
                    const SizeType offset =
                        static_cast<SizeType>(x) * sourceChannels + c;

                    // 8 位扩展到 16 位区间, 乘 257 使 255 映射到 65535
                    samples[c] = static_cast<UInt16>(rawRow[offset] * 257u);
                }
            }

            // ---- 灰度与灰度+Alpha: 复制到 RGB 三通道 ----
            if (header.ColorType == kColorTypeGray)
            {
                samples[1] = samples[0];
                samples[2] = samples[0];
            }
            else if (header.ColorType == kColorTypeGrayAlpha)
            {
                samples[3] = samples[1];
                samples[1] = samples[0];
                samples[2] = samples[0];
            }

            // ---- 透明色键 ----
            if (context.HasColorKey)
            {
                bool matches = false;

                if (header.ColorType == kColorTypeGray)
                {
                    // 比较未经通道复制前的原始样本值
                    const UInt16 original = sourceIs16Bit
                                                ? samples[0]
                                                : static_cast<UInt16>(samples[0] / 257u);
                    matches = (original == context.ColorKey[0]);
                }
                else if (header.ColorType == kColorTypeRgb)
                {
                    const UInt16 r = sourceIs16Bit ? samples[0]
                                                   : static_cast<UInt16>(samples[0] / 257u);
                    const UInt16 g = sourceIs16Bit ? samples[1]
                                                   : static_cast<UInt16>(samples[1] / 257u);
                    const UInt16 b = sourceIs16Bit ? samples[2]
                                                   : static_cast<UInt16>(samples[2] / 257u);

                    matches = (r == context.ColorKey[0]) &&
                              (g == context.ColorKey[1]) &&
                              (b == context.ColorKey[2]);
                }

                if (matches)
                {
                    samples[3] = 0;
                }
            }
        }

        // ---- 写出到目标格式 ----
        UInt8* pixel = destination +
                       static_cast<SizeType>(x) * destChannels * destBytes;

        for (UInt32 c = 0; c < destChannels; ++c)
        {
            const UInt16 value = samples[c];

            if (destBytes == 2)
            {
                // 主机序写出
                pixel[c * 2 + 0] = static_cast<UInt8>(value & 0xFFu);
                pixel[c * 2 + 1] = static_cast<UInt8>((value >> 8) & 0xFFu);
            }
            else
            {
                // 取高字节即为 8 位近似 — 等价于除以 257 的舍入
                pixel[c] = static_cast<UInt8>(value >> 8);
            }
        }
    }
}

} // namespace

// ============================================================================
// FPngDecoder
// ============================================================================

bool FPngDecoder::IsPng(const UInt8* data, SizeType length)
{
    if (data == nullptr || length < 8)
    {
        return false;
    }

    for (SizeType i = 0; i < 8; ++i)
    {
        if (data[i] != kPngSignature[i])
        {
            return false;
        }
    }

    return true;
}

FImageDecodeResult FPngDecoder::ReadHeader(const UInt8* data, SizeType length,
                                           UInt32& outWidth, UInt32& outHeight,
                                           UInt32& outChannelCount,
                                           UInt32& outBitDepth)
{
    outWidth        = 0;
    outHeight       = 0;
    outChannelCount = 0;
    outBitDepth     = 0;

    if (!IsPng(data, length))
    {
        return FImageDecodeResult::Failure(FString("不是 PNG 文件"));
    }

    // 签名 8 + 块长 4 + 块类型 4 + IHDR 13 字节
    if (length < 8 + 8 + 13)
    {
        return FImageDecodeResult::Failure(FString("文件过短, 不足 IHDR"), 8);
    }

    if (!IsChunkType(data + 12, "IHDR"))
    {
        return FImageDecodeResult::Failure(FString("首块不是 IHDR"), 12);
    }

    const UInt8* ihdr = data + 16;

    outWidth    = ReadBigEndianUInt32(ihdr);
    outHeight   = ReadBigEndianUInt32(ihdr + 4);
    outBitDepth = ihdr[8];

    FPngHeader header;
    header.ColorType = ihdr[9];
    header.BitDepth  = ihdr[8];

    outChannelCount = header.GetChannelCount();

    // 调色板展开后是 RGB 或 RGBA
    if (header.ColorType == kColorTypePalette)
    {
        outChannelCount = 3;
    }

    return FImageDecodeResult::Success();
}

FImageDecodeResult FPngDecoder::Decode(const UInt8* data, SizeType length,
                                       FImageData& outImage,
                                       const FImageDecodeOptions& options)
{
    outImage.Reset();

    if (!IsPng(data, length))
    {
        return FImageDecodeResult::Failure(FString("不是 PNG 文件"));
    }

    TArray<FString> warnings;

    FDecodeContext context;
    context.Warnings = &warnings;

    TArray<UInt8> compressedData;

    bool sawHeader = false;
    bool sawEnd    = false;

    // ------------------------------------------------------------------
    // 分块遍历
    // ------------------------------------------------------------------

    SizeType cursor = 8;

    while (cursor + 8 <= length)
    {
        const UInt32 chunkLength = ReadBigEndianUInt32(data + cursor);
        const UInt8* chunkType   = data + cursor + 4;

        // 块长 + 类型 4 + 数据 + CRC 4
        if (static_cast<UInt64>(cursor) + 12 + chunkLength > length)
        {
            return FImageDecodeResult::Failure(
                StringFormat("块越界: 长度 {} 超出文件剩余部分", chunkLength),
                cursor);
        }

        const UInt8* chunkData = data + cursor + 8;

        // ---- CRC 校验 ----
        {
            const UInt32 storedCrc =
                ReadBigEndianUInt32(data + cursor + 8 + chunkLength);

            // CRC 覆盖块类型与块数据, 不含长度字段
            const UInt32 computedCrc =
                FCrc32::Compute(chunkType, 4 + static_cast<SizeType>(chunkLength));

            if (storedCrc != computedCrc)
            {
                // 只告警不拒绝: PNG 的分块结构使单块损坏未必影响可解码性,
                // 而真正的像素数据损坏会被 IDAT 的 zlib 校验和拦下
                warnings.Add(StringFormat(
                    "块 CRC 不符 (偏移 {}): 文件记录 {}, 实际 {}",
                    static_cast<UInt64>(cursor), FHex(storedCrc),
                    FHex(computedCrc)));
            }
        }

        // ---------------------------------------------------------------
        // IHDR
        // ---------------------------------------------------------------
        if (IsChunkType(chunkType, "IHDR"))
        {
            if (chunkLength != 13)
            {
                return FImageDecodeResult::Failure(
                    StringFormat("IHDR 长度应为 13, 实际 {}", chunkLength),
                    cursor);
            }

            context.Header.Width       = ReadBigEndianUInt32(chunkData);
            context.Header.Height      = ReadBigEndianUInt32(chunkData + 4);
            context.Header.BitDepth    = chunkData[8];
            context.Header.ColorType   = chunkData[9];
            context.Header.Compression = chunkData[10];
            context.Header.Filter      = chunkData[11];
            context.Header.Interlace   = chunkData[12];

            const FPngHeader& header = context.Header;

            if (header.Width == 0 || header.Height == 0)
            {
                return FImageDecodeResult::Failure(FString("图像尺寸为零"), cursor);
            }

            if (header.Width > kMaxDimension || header.Height > kMaxDimension)
            {
                return FImageDecodeResult::Failure(
                    StringFormat("图像尺寸 {}x{} 超出上限 {}",
                                 header.Width, header.Height, kMaxDimension),
                    cursor);
            }

            if (!IsValidBitDepthForColorType(header.ColorType, header.BitDepth))
            {
                return FImageDecodeResult::Failure(
                    StringFormat("颜色类型 {} 不支持位深 {}",
                                 header.ColorType, header.BitDepth),
                    cursor);
            }

            if (header.Compression != 0)
            {
                return FImageDecodeResult::Failure(
                    StringFormat("未知的压缩方法 {}", header.Compression), cursor);
            }

            if (header.Filter != 0)
            {
                return FImageDecodeResult::Failure(
                    StringFormat("未知的滤波方法 {}", header.Filter), cursor);
            }

            if (header.Interlace > 1)
            {
                return FImageDecodeResult::Failure(
                    StringFormat("未知的隔行方法 {}", header.Interlace), cursor);
            }

            sawHeader = true;
        }
        // ---------------------------------------------------------------
        // PLTE
        // ---------------------------------------------------------------
        else if (IsChunkType(chunkType, "PLTE"))
        {
            if ((chunkLength % 3) != 0)
            {
                return FImageDecodeResult::Failure(
                    StringFormat("PLTE 长度 {} 不是 3 的倍数", chunkLength),
                    cursor);
            }

            context.Palette.Clear();
            context.Palette.Reserve(chunkLength);

            for (UInt32 i = 0; i < chunkLength; ++i)
            {
                context.Palette.Add(chunkData[i]);
            }
        }
        // ---------------------------------------------------------------
        // tRNS
        // ---------------------------------------------------------------
        else if (IsChunkType(chunkType, "tRNS"))
        {
            if (context.Header.ColorType == kColorTypePalette)
            {
                context.PaletteAlpha.Clear();
                context.PaletteAlpha.Reserve(chunkLength);

                for (UInt32 i = 0; i < chunkLength; ++i)
                {
                    context.PaletteAlpha.Add(chunkData[i]);
                }
            }
            else if (context.Header.ColorType == kColorTypeGray &&
                     chunkLength >= 2)
            {
                context.HasColorKey = true;
                context.ColorKey[0] = static_cast<UInt16>(
                    (static_cast<UInt32>(chunkData[0]) << 8) | chunkData[1]);
            }
            else if (context.Header.ColorType == kColorTypeRgb &&
                     chunkLength >= 6)
            {
                context.HasColorKey = true;

                for (UInt32 c = 0; c < 3; ++c)
                {
                    context.ColorKey[c] = static_cast<UInt16>(
                        (static_cast<UInt32>(chunkData[c * 2]) << 8) |
                        chunkData[c * 2 + 1]);
                }
            }
        }
        // ---------------------------------------------------------------
        // sRGB / gAMA — 色彩空间提示
        // ---------------------------------------------------------------
        else if (IsChunkType(chunkType, "sRGB"))
        {
            context.ColorSpace = EImageColorSpace::Srgb;
        }
        else if (IsChunkType(chunkType, "gAMA") && chunkLength >= 4)
        {
            // gAMA 记录的是 100000 倍的伽马值; 45455 对应 1/2.2, 即 sRGB 近似
            const UInt32 gamma = ReadBigEndianUInt32(chunkData);

            if (context.ColorSpace == EImageColorSpace::Unspecified)
            {
                context.ColorSpace = (gamma > 90000)
                                         ? EImageColorSpace::Linear
                                         : EImageColorSpace::Srgb;
            }
        }
        // ---------------------------------------------------------------
        // IDAT — 可能分多块, 规范允许在任意字节处切分
        // ---------------------------------------------------------------
        else if (IsChunkType(chunkType, "IDAT"))
        {
            compressedData.Reserve(compressedData.GetSize() + chunkLength);

            for (UInt32 i = 0; i < chunkLength; ++i)
            {
                compressedData.Add(chunkData[i]);
            }
        }
        // ---------------------------------------------------------------
        // IEND
        // ---------------------------------------------------------------
        else if (IsChunkType(chunkType, "IEND"))
        {
            sawEnd = true;
            break;
        }

        cursor += 12 + chunkLength;
    }

    if (!sawHeader)
    {
        return FImageDecodeResult::Failure(FString("缺少 IHDR 块"));
    }

    if (compressedData.GetSize() == 0)
    {
        return FImageDecodeResult::Failure(FString("缺少 IDAT 数据"));
    }

    if (!sawEnd)
    {
        warnings.Add(FString("缺少 IEND 块, 文件可能被截断"));
    }

    const FPngHeader& header = context.Header;

    if (header.ColorType == kColorTypePalette && context.Palette.GetSize() == 0)
    {
        return FImageDecodeResult::Failure(
            FString("调色板图像缺少 PLTE 块"));
    }

    // ------------------------------------------------------------------
    // 解压 IDAT
    // ------------------------------------------------------------------

    TArray<UInt8> rawData;
    FString inflateError;

    if (!FInflate::DecompressZlib(compressedData.GetData(),
                                  compressedData.GetSize(),
                                  rawData, &inflateError))
    {
        return FImageDecodeResult::Failure(
            StringFormat("IDAT 解压失败: {}", inflateError.GetCStr()));
    }

    // ------------------------------------------------------------------
    // 目标格式
    // ------------------------------------------------------------------

    const bool sourceHasAlpha =
        (header.ColorType == kColorTypeGrayAlpha) ||
        (header.ColorType == kColorTypeRgba) ||
        (header.ColorType == kColorTypePalette &&
         context.PaletteAlpha.GetSize() > 0) ||
        context.HasColorKey;

    // 灰度保持单通道; 其余一律按 RGB(A) 处理
    UInt32 destChannels = 0;

    if (header.ColorType == kColorTypeGray)
    {
        destChannels = context.HasColorKey ? 2u : 1u;
    }
    else if (header.ColorType == kColorTypeGrayAlpha)
    {
        destChannels = 2;
    }
    else
    {
        destChannels = sourceHasAlpha ? 4u : 3u;
    }

    if (options.ForceFourChannels)
    {
        destChannels = 4;
    }

    const UInt32 destBytes =
        (header.BitDepth == 16 && !options.ReduceSixteenBitToEight) ? 2u : 1u;

    // 灰度在 ConvertRow 中会复制到 RGB, 若目标是单/双通道则只取前 1/2 个分量,
    // 语义上仍是灰度与灰度+Alpha, 与源一致
    const EImageFormat destFormat = MakeImageFormat(destChannels, destBytes);

    if (destFormat == EImageFormat::Unknown)
    {
        return FImageDecodeResult::Failure(
            StringFormat("无法确定目标格式: {} 通道 {} 字节",
                         destChannels, destBytes));
    }

    outImage.Width          = header.Width;
    outImage.Height         = header.Height;
    outImage.Format         = destFormat;
    outImage.ColorSpace     = context.ColorSpace;
    outImage.HasSourceAlpha = sourceHasAlpha;

    const SizeType destPixelBytes = static_cast<SizeType>(destChannels) * destBytes;
    const SizeType destRowBytes   = destPixelBytes * header.Width;
    const SizeType destTotalBytes = destRowBytes * header.Height;

    outImage.Pixels.Reserve(destTotalBytes);
    for (SizeType i = 0; i < destTotalBytes; ++i)
    {
        outImage.Pixels.Add(0);
    }

    // ------------------------------------------------------------------
    // 逐遍反滤波并转换
    //
    // 非隔行图像视为只有一遍、覆盖全图的特殊情形，从而与 Adam7 共用
    // 同一套反滤波与转换代码。
    // ------------------------------------------------------------------

    const UInt32 filterStride = header.GetFilterStride();
    const UInt32 passCount    = (header.Interlace == 1) ? 7u : 1u;

    SizeType rawCursor = 0;

    // 解包缓冲 — 低位深时把样本展开为每样本一字节
    TArray<UInt8> unpackedRow;

    // 上一行的已还原字节 — 反滤波需要
    TArray<UInt8> previousRow;
    TArray<UInt8> currentRow;

    for (UInt32 pass = 0; pass < passCount; ++pass)
    {
        UInt32 passStartX = 0;
        UInt32 passStartY = 0;
        UInt32 passStepX  = 1;
        UInt32 passStepY  = 1;

        if (header.Interlace == 1)
        {
            passStartX = kAdam7StartX[pass];
            passStartY = kAdam7StartY[pass];
            passStepX  = kAdam7StepX[pass];
            passStepY  = kAdam7StepY[pass];
        }

        // 该遍覆盖的像素数
        const UInt32 passWidth =
            (header.Width > passStartX)
                ? ((header.Width - passStartX + passStepX - 1) / passStepX)
                : 0;
        const UInt32 passHeight =
            (header.Height > passStartY)
                ? ((header.Height - passStartY + passStepY - 1) / passStepY)
                : 0;

        if (passWidth == 0 || passHeight == 0)
        {
            continue;
        }

        const SizeType passRowBytes = header.GetRawRowBytes(passWidth);

        previousRow.Clear();
        currentRow.Clear();

        for (SizeType i = 0; i < passRowBytes; ++i)
        {
            previousRow.Add(0);
            currentRow.Add(0);
        }

        for (UInt32 row = 0; row < passHeight; ++row)
        {
            // 每行前有一个滤波器类型字节
            if (rawCursor + 1 + passRowBytes > rawData.GetSize())
            {
                return FImageDecodeResult::Failure(
                    StringFormat("解压数据不足: 第 {} 遍第 {} 行需要 {} 字节",
                                 pass, row, static_cast<UInt64>(passRowBytes)));
            }

            const UInt8 filterType = rawData[rawCursor];
            ++rawCursor;

            for (SizeType i = 0; i < passRowBytes; ++i)
            {
                currentRow[i] = rawData[rawCursor + i];
            }

            rawCursor += passRowBytes;

            const UInt8* previous = (row == 0) ? nullptr : previousRow.GetData();

            if (!UnfilterScanline(filterType, currentRow.GetData(), previous,
                                  passRowBytes, filterStride))
            {
                return FImageDecodeResult::Failure(
                    StringFormat("第 {} 遍第 {} 行的滤波器类型 {} 非法",
                                 pass, row, filterType));
            }

            // ---- 低位深解包 ----
            const UInt8* sampleRow = currentRow.GetData();

            if (header.BitDepth < 8)
            {
                const UInt32 sampleCount = passWidth * header.GetChannelCount();

                unpackedRow.Clear();
                unpackedRow.Reserve(sampleCount);

                for (UInt32 i = 0; i < sampleCount; ++i)
                {
                    unpackedRow.Add(0);
                }

                // 调色板索引必须保持原值, 灰度则需归一化到 0..255
                const bool scaleToByte =
                    (header.ColorType != kColorTypePalette);

                UnpackLowBitDepth(currentRow.GetData(), sampleCount,
                                  header.BitDepth, scaleToByte,
                                  unpackedRow.GetData());

                sampleRow = unpackedRow.GetData();
            }

            // ---- 转换并回填 ----
            const UInt32 destY = passStartY + row * passStepY;

            if (destY >= header.Height)
            {
                continue;
            }

            if (header.Interlace == 0)
            {
                // 非隔行: 整行连续写出
                ConvertRow(context, sampleRow, passWidth,
                           outImage.Pixels.GetData() + destY * destRowBytes,
                           destChannels, destBytes);
            }
            else
            {
                // 隔行: 该遍的像素按步长散布在目标行上, 需逐像素回填。
                // 先转换到临时行, 再按格点写入。
                TArray<UInt8> convertedRow;
                convertedRow.Reserve(passWidth * destPixelBytes);

                for (SizeType i = 0; i < passWidth * destPixelBytes; ++i)
                {
                    convertedRow.Add(0);
                }

                ConvertRow(context, sampleRow, passWidth,
                           convertedRow.GetData(), destChannels, destBytes);

                for (UInt32 x = 0; x < passWidth; ++x)
                {
                    const UInt32 destX = passStartX + x * passStepX;

                    if (destX >= header.Width)
                    {
                        break;
                    }

                    UInt8* destination = outImage.Pixels.GetData() +
                                         destY * destRowBytes +
                                         static_cast<SizeType>(destX) * destPixelBytes;

                    const UInt8* source = convertedRow.GetData() +
                                          static_cast<SizeType>(x) * destPixelBytes;

                    for (SizeType b = 0; b < destPixelBytes; ++b)
                    {
                        destination[b] = source[b];
                    }
                }
            }

            // 当前行成为下一行的参照
            for (SizeType i = 0; i < passRowBytes; ++i)
            {
                previousRow[i] = currentRow[i];
            }
        }
    }

    // ------------------------------------------------------------------
    // 垂直翻转
    // ------------------------------------------------------------------

    if (options.FlipVertically && header.Height > 1)
    {
        for (UInt32 y = 0; y < header.Height / 2; ++y)
        {
            UInt8* top    = outImage.Pixels.GetData() + y * destRowBytes;
            UInt8* bottom = outImage.Pixels.GetData() +
                            (header.Height - 1 - y) * destRowBytes;

            for (SizeType i = 0; i < destRowBytes; ++i)
            {
                const UInt8 temporary = top[i];
                top[i]    = bottom[i];
                bottom[i] = temporary;
            }
        }
    }

    FImageDecodeResult result = FImageDecodeResult::Success();
    result.Warnings = static_cast<TArray<FString>&&>(warnings);

    return result;
}

} // namespace Limx
