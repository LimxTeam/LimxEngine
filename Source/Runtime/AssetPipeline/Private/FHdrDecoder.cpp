/*******************************************************************************
 * 文件: FHdrDecoder.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   Radiance HDR 解码实现 — 文本头解析、两种 RLE、RGBE 转线性浮点
 *
 * 设计哲学:
 *   2 的幂用查表而非 powf — 一张 2K 环境图有两百万像素、六百万次通道转换,
 *   每次调一趟 powf 既慢又引入舍入误差。而 2^n 的表可以从 1.0 出发, 靠
 *   反复乘 2 与乘 0.5 逐项推出: 这两个乘法在 IEEE-754 下对 2 的幂是精确的,
 *   一路到非规格化区间的最小值 2^-149 都不丢位。表是精确的, 不是近似的。
 *
 *   游程解析全程带边界检查 — HDR 文件常来自网络或第三方工具, 一条声称
 *   长度 200 的游程完全可能只跟着 3 个字节。少一次检查就是一次越界读,
 *   而且是那种在小图上永远碰不到、换张图才崩的越界读。
 *
 * 依赖关系:
 *   内部: AssetPipeline/FHdrDecoder.h
 *
 ******************************************************************************/

#include "AssetPipeline/FHdrDecoder.h"

namespace Limx
{

namespace
{

// ============================================================================
// 2 的幂查找表
// ============================================================================

/// RGBE 指数字节的取值范围是 [0, 255], 实际缩放是 2^(e - 128 - 8)
constexpr Int32 kRgbeExponentBias = 136;

/// scale[e] = 2^(e - 136)
struct FPowerOfTwoTable
{
    Float32 Values[256] = {};

    constexpr FPowerOfTwoTable()
    {
        // 从 2^0 出发向两端推 —— 乘 2 与乘 0.5 对 2 的幂是精确运算,
        // 向下一路到 2^-149 (最小非规格化数) 都不丢位
        Values[kRgbeExponentBias] = 1.0f;

        for (Int32 e = kRgbeExponentBias + 1; e < 256; ++e)
        {
            Values[e] = Values[e - 1] * 2.0f;
        }

        for (Int32 e = kRgbeExponentBias - 1; e >= 0; --e)
        {
            Values[e] = Values[e + 1] * 0.5f;
        }
    }
};

constexpr FPowerOfTwoTable kPowerOfTwo;

// ============================================================================
// 文本头解析
// ============================================================================

struct FHdrHeader
{
    UInt32 Width  = 0;
    UInt32 Height = 0;

    /// 首行是否为图像的顶行
    ///
    /// `-Y h` 表示 y 向下递减, 即数据的第一行是图像顶行 (最常见)。
    /// `+Y h` 反之。
    bool FirstRowIsTop = true;

    /// 每行的第一个像素是否为左端
    bool FirstColumnIsLeft = true;

    /// 是否为 XYZE 色彩空间 —— 不支持, 用于给出准确的错误信息
    bool IsXyze = false;

    /// 数据段起始偏移
    SizeType DataOffset = 0;
};

/// 从 offset 起读一行文本, 消费掉行尾的换行符
///
/// 同时接受 LF 与 CRLF: Radiance 规范只写 LF, 但经 Windows 工具转存过的
/// 文件里 CRLF 并不罕见, 而多出来的 CR 会让 `FORMAT=32-bit_rle_rgbe` 的
/// 相等比较失败, 表现为"格式不受支持"这种极具误导性的报错。
bool ReadHeaderLine(const UInt8* data, SizeType length, SizeType& offset,
                    char* outLine, SizeType lineCapacity, SizeType& outLength)
{
    outLength = 0;

    if (offset >= length)
    {
        return false;
    }

    while (offset < length)
    {
        const UInt8 byte = data[offset];
        ++offset;

        if (byte == static_cast<UInt8>('\n'))
        {
            // 去掉 CRLF 里的 CR
            if (outLength > 0 && outLine[outLength - 1] == '\r')
            {
                --outLength;
            }

            outLine[outLength] = '\0';
            return true;
        }

        if (outLength + 1 < lineCapacity)
        {
            outLine[outLength] = static_cast<char>(byte);
            ++outLength;
        }
        // 超长行截断而非报错 —— 注释行想写多长写多长, 我们只关心开头
    }

    // 文件在没有换行的情况下结束
    outLine[outLength] = '\0';
    return true;
}

bool LinesMatch(const char* line, SizeType lineLength, const char* prefix)
{
    SizeType i = 0;

    while (prefix[i] != '\0')
    {
        if (i >= lineLength || line[i] != prefix[i])
        {
            return false;
        }
        ++i;
    }

    return true;
}

/// 跳过空白, 解析一个非负十进制整数
bool ParseUnsigned(const char* line, SizeType lineLength, SizeType& cursor,
                   UInt32& outValue)
{
    while (cursor < lineLength && (line[cursor] == ' ' || line[cursor] == '\t'))
    {
        ++cursor;
    }

    if (cursor >= lineLength || line[cursor] < '0' || line[cursor] > '9')
    {
        return false;
    }

    UInt64 value = 0;

    while (cursor < lineLength && line[cursor] >= '0' && line[cursor] <= '9')
    {
        value = value * 10 + static_cast<UInt64>(line[cursor] - '0');

        // 分辨率溢出到 32 位以外的文件一定是坏的
        if (value > 0xFFFFFFFFull)
        {
            return false;
        }

        ++cursor;
    }

    outValue = static_cast<UInt32>(value);
    return true;
}

/// 解析形如 `-Y 1024 +X 2048` 的分辨率行
bool ParseResolutionLine(const char* line, SizeType lineLength,
                         FHdrHeader& header)
{
    SizeType cursor = 0;

    while (cursor < lineLength && (line[cursor] == ' ' || line[cursor] == '\t'))
    {
        ++cursor;
    }

    // 只支持 Y 在前的行主序 —— 列主序 (`+X w -Y h`) 需要转置整张图,
    // 而现实中的 .hdr 文件几乎不用它。遇到时明确失败而非解出一张歪图。
    if (cursor + 1 >= lineLength)
    {
        return false;
    }

    const char ySign = line[cursor];

    if ((ySign != '-' && ySign != '+') || line[cursor + 1] != 'Y')
    {
        return false;
    }

    header.FirstRowIsTop = (ySign == '-');
    cursor += 2;

    if (!ParseUnsigned(line, lineLength, cursor, header.Height))
    {
        return false;
    }

    while (cursor < lineLength && (line[cursor] == ' ' || line[cursor] == '\t'))
    {
        ++cursor;
    }

    if (cursor + 1 >= lineLength)
    {
        return false;
    }

    const char xSign = line[cursor];

    if ((xSign != '-' && xSign != '+') || line[cursor + 1] != 'X')
    {
        return false;
    }

    header.FirstColumnIsLeft = (xSign == '+');
    cursor += 2;

    return ParseUnsigned(line, lineLength, cursor, header.Width);
}

FImageDecodeResult ParseHeader(const UInt8* data, SizeType length,
                               FHdrHeader& header)
{
    constexpr SizeType kLineCapacity = 256;

    char     line[kLineCapacity] = {};
    SizeType lineLength          = 0;
    SizeType offset              = 0;

    if (!ReadHeaderLine(data, length, offset, line, kLineCapacity, lineLength))
    {
        return FImageDecodeResult::Failure(FString("HDR 文件为空"));
    }

    if (!LinesMatch(line, lineLength, "#?"))
    {
        return FImageDecodeResult::Failure(
            FString("HDR 签名缺失, 首行不以 #? 开头"));
    }

    bool sawFormat = false;

    // 头部以空行结束, 紧接着是分辨率行
    for (;;)
    {
        if (!ReadHeaderLine(data, length, offset, line, kLineCapacity,
                            lineLength))
        {
            return FImageDecodeResult::Failure(
                FString("HDR 头部未结束文件就结束了"), offset);
        }

        if (lineLength == 0)
        {
            break;
        }

        if (LinesMatch(line, lineLength, "FORMAT="))
        {
            sawFormat = true;

            if (LinesMatch(line, lineLength, "FORMAT=32-bit_rle_xyze"))
            {
                header.IsXyze = true;
            }
            else if (!LinesMatch(line, lineLength, "FORMAT=32-bit_rle_rgbe"))
            {
                return FImageDecodeResult::Failure(
                    StringFormat("不支持的 HDR 像素格式: {}", line));
            }
        }
    }

    if (header.IsXyze)
    {
        return FImageDecodeResult::Failure(
            FString("HDR 使用 XYZE 色彩空间, 当前只支持 RGBE"));
    }

    if (!ReadHeaderLine(data, length, offset, line, kLineCapacity, lineLength))
    {
        return FImageDecodeResult::Failure(
            FString("HDR 缺少分辨率行"), offset);
    }

    if (!ParseResolutionLine(line, lineLength, header))
    {
        return FImageDecodeResult::Failure(
            StringFormat("无法解析 HDR 分辨率行: {}", line), offset);
    }

    if (header.Width == 0 || header.Height == 0)
    {
        return FImageDecodeResult::Failure(StringFormat(
            "HDR 分辨率非法: {}x{}", header.Width, header.Height));
    }

    header.DataOffset = offset;

    FImageDecodeResult result = FImageDecodeResult::Success();

    if (!sawFormat)
    {
        // 规范要求 FORMAT 必须出现。缺失时按 RGBE 解通常也对, 但没有声明
        // 就意味着文件出自某个不遵守规范的写出器 —— 它在别处也可能不守规范。
        result.Warnings.Add(FString("HDR 头部缺少 FORMAT 行, 按 RGBE 解释"));
    }

    return result;
}

// ============================================================================
// 扫描线解码
// ============================================================================

/// 解一条新版自适应 RLE 扫描线 —— 四个通道各自独立游程
///
/// 输出按通道平铺: outScanline 前 width 字节是 R, 依次类推。
/// 这与文件里的排列一致, 转成交错像素放在调用方。
bool DecodeAdaptiveRleScanline(const UInt8* data, SizeType length,
                               SizeType& offset, UInt32 width,
                               UInt8* outScanline)
{
    for (UInt32 channel = 0; channel < 4; ++channel)
    {
        UInt8* target    = outScanline + static_cast<SizeType>(channel) * width;
        UInt32 decoded   = 0;

        while (decoded < width)
        {
            if (offset >= length)
            {
                return false;
            }

            const UInt8 count = data[offset];
            ++offset;

            if (count > 128)
            {
                // 游程: 同一字节重复 count-128 次
                const UInt32 runLength = static_cast<UInt32>(count) - 128u;

                if (offset >= length || decoded + runLength > width)
                {
                    return false;
                }

                const UInt8 value = data[offset];
                ++offset;

                for (UInt32 i = 0; i < runLength; ++i)
                {
                    target[decoded + i] = value;
                }

                decoded += runLength;
            }
            else
            {
                // 直传: count 个原始字节。count 为 0 在格式里没有定义 ——
                // 不判并不会死循环 (游标每轮至少推进一字节, 数据总会耗尽),
                // 而是更糟: 那个字节被静默跳过, 后面的数据照常解出来,
                // 于是一份已经错位的文件会"解码成功"。
                const UInt32 literalLength = static_cast<UInt32>(count);

                if (literalLength == 0 ||
                    decoded + literalLength > width ||
                    offset + literalLength > length)
                {
                    return false;
                }

                for (UInt32 i = 0; i < literalLength; ++i)
                {
                    target[decoded + i] = data[offset + i];
                }

                offset  += literalLength;
                decoded += literalLength;
            }
        }
    }

    return true;
}

} // namespace

// ============================================================================
// 公开接口
// ============================================================================

void FHdrDecoder::RgbeToLinear(UInt8 r, UInt8 g, UInt8 b, UInt8 e,
                               Float32& outR, Float32& outG, Float32& outB)
{
    // 指数为 0 是格式约定的纯黑。若照常缩放会得到 2^-136 量级的
    // 非规格化数 —— 数值上等价于零, 却会在某些硬件上拖慢后续运算。
    if (e == 0)
    {
        outR = 0.0f;
        outG = 0.0f;
        outB = 0.0f;
        return;
    }

    const Float32 scale = kPowerOfTwo.Values[e];

    outR = static_cast<Float32>(r) * scale;
    outG = static_cast<Float32>(g) * scale;
    outB = static_cast<Float32>(b) * scale;
}

bool FHdrDecoder::IsHdr(const UInt8* data, SizeType length)
{
    if (data == nullptr || length < 6)
    {
        return false;
    }

    if (data[0] != static_cast<UInt8>('#') || data[1] != static_cast<UInt8>('?'))
    {
        return false;
    }

    const char* radiance = "RADIANCE";
    const char* rgbe     = "RGBE";

    bool matchesRadiance = length >= 10;

    for (SizeType i = 0; matchesRadiance && radiance[i] != '\0'; ++i)
    {
        matchesRadiance = data[2 + i] == static_cast<UInt8>(radiance[i]);
    }

    if (matchesRadiance)
    {
        return true;
    }

    bool matchesRgbe = length >= 6;

    for (SizeType i = 0; matchesRgbe && rgbe[i] != '\0'; ++i)
    {
        matchesRgbe = data[2 + i] == static_cast<UInt8>(rgbe[i]);
    }

    return matchesRgbe;
}

FImageDecodeResult FHdrDecoder::Decode(const UInt8* data, SizeType length,
                                       FImageData& outImage,
                                       const FImageDecodeOptions& options)
{
    outImage.Reset();

    if (data == nullptr || length == 0)
    {
        return FImageDecodeResult::Failure(FString("HDR 数据为空"));
    }

    FHdrHeader               header;
    const FImageDecodeResult headerResult = ParseHeader(data, length, header);

    if (!headerResult.Succeeded)
    {
        return headerResult;
    }

    const UInt32   width       = header.Width;
    const UInt32   height      = header.Height;
    const UInt32   channels    = options.ForceFourChannels ? 4u : 3u;
    const SizeType pixelCount  = static_cast<SizeType>(width) * height;

    // 先算总字节数并检查溢出 —— 一个声称 65535x65535 的头会让
    // 这个乘积超出 SizeType, 之后的 Resize 会分配一块远小于所需的内存。
    constexpr SizeType kMaxPixels = static_cast<SizeType>(1) << 30;

    if (pixelCount > kMaxPixels)
    {
        return FImageDecodeResult::Failure(StringFormat(
            "HDR 尺寸过大: {}x{}", width, height));
    }

    TArray<UInt8> scanline;
    scanline.SetSize(static_cast<SizeType>(width) * 4);

    TArray<Float32> pixels;
    pixels.SetSize(pixelCount * channels);

    SizeType offset = header.DataOffset;

    // 新版 RLE 只在这个宽度区间内使用 —— 区间外一定是逐像素排列,
    // 因为标志字节 (2,2,hi,lo) 会与合法的 RGBE 像素混淆。
    const bool mayUseAdaptiveRle = (width >= 8 && width < 32768);

    bool useAdaptiveRle = mayUseAdaptiveRle;

    // 老式数据里"重复上一像素"跨扫描线也成立, 所以前一像素要在行外保存
    UInt8 previous[4] = { 0, 0, 0, 0 };
    bool  hasPrevious = false;

    for (UInt32 row = 0; row < height; ++row)
    {
        bool decodedScanline = false;

        if (useAdaptiveRle)
        {
            if (offset + 4 > length)
            {
                return FImageDecodeResult::Failure(
                    StringFormat("HDR 数据在第 {} 行提前结束", row), offset);
            }

            const bool isAdaptiveMarker =
                data[offset] == 2 && data[offset + 1] == 2 &&
                (data[offset + 2] & 0x80) == 0;

            const UInt32 markerWidth =
                (static_cast<UInt32>(data[offset + 2]) << 8) |
                static_cast<UInt32>(data[offset + 3]);

            if (isAdaptiveMarker && markerWidth == width)
            {
                offset += 4;

                if (!DecodeAdaptiveRleScanline(data, length, offset, width,
                                               scanline.GetData()))
                {
                    return FImageDecodeResult::Failure(
                        StringFormat("HDR 第 {} 行的游程数据损坏", row),
                        offset);
                }

                decodedScanline = true;
            }
            else if (row == 0)
            {
                // 首行就不是新版标志 —— 整个文件都是老式排列
                useAdaptiveRle = false;
            }
            else
            {
                return FImageDecodeResult::Failure(
                    StringFormat("HDR 第 {} 行缺少游程标志", row), offset);
            }
        }

        if (!decodedScanline)
        {
            // 老式排列: 逐像素读 RGBE, (1,1,1,n) 表示重复上一像素 n 次
            UInt32 column      = 0;
            UInt32 repeatShift = 0;

            while (column < width)
            {
                if (offset + 4 > length)
                {
                    return FImageDecodeResult::Failure(
                        StringFormat("HDR 数据在第 {} 行第 {} 列提前结束",
                                     row, column),
                        offset);
                }

                const UInt8 r = data[offset];
                const UInt8 g = data[offset + 1];
                const UInt8 b = data[offset + 2];
                const UInt8 e = data[offset + 3];
                offset += 4;

                if (r == 1 && g == 1 && b == 1)
                {
                    if (!hasPrevious)
                    {
                        return FImageDecodeResult::Failure(
                            FString("HDR 首像素就是重复标记"), offset);
                    }

                    // 连续的重复标记按 8 位一段拼成更长的计数
                    const UInt32 repeatCount =
                        static_cast<UInt32>(e) << repeatShift;

                    if (column + repeatCount > width)
                    {
                        return FImageDecodeResult::Failure(
                            StringFormat("HDR 第 {} 行的重复计数越界", row),
                            offset);
                    }

                    for (UInt32 i = 0; i < repeatCount; ++i)
                    {
                        scanline[column]             = previous[0];
                        scanline[width + column]     = previous[1];
                        scanline[width * 2 + column] = previous[2];
                        scanline[width * 3 + column] = previous[3];
                        ++column;
                    }

                    repeatShift += 8;
                }
                else
                {
                    scanline[column]             = r;
                    scanline[width + column]     = g;
                    scanline[width * 2 + column] = b;
                    scanline[width * 3 + column] = e;

                    previous[0] = r;
                    previous[1] = g;
                    previous[2] = b;
                    previous[3] = e;
                    hasPrevious = true;

                    ++column;
                    repeatShift = 0;
                }
            }
        }
        else
        {
            // 新版 RLE 行解完后也要更新"上一像素", 以防文件混用两种排列
            if (width > 0)
            {
                previous[0] = scanline[width - 1];
                previous[1] = scanline[width * 2 - 1];
                previous[2] = scanline[width * 3 - 1];
                previous[3] = scanline[width * 4 - 1];
                hasPrevious = true;
            }
        }

        // 写入目标行 —— 此处一并处理分辨率行声明的两个方向
        const UInt32 targetRow = header.FirstRowIsTop ? row : (height - 1 - row);

        Float32* destRow =
            pixels.GetData() +
            static_cast<SizeType>(targetRow) * width * channels;

        for (UInt32 column = 0; column < width; ++column)
        {
            const UInt32 targetColumn =
                header.FirstColumnIsLeft ? column : (width - 1 - column);

            Float32* dest = destRow + static_cast<SizeType>(targetColumn) * channels;

            RgbeToLinear(scanline[column],
                         scanline[width + column],
                         scanline[width * 2 + column],
                         scanline[width * 3 + column],
                         dest[0], dest[1], dest[2]);

            if (channels == 4)
            {
                dest[3] = 1.0f;
            }
        }
    }

    // 选项里的垂直翻转独立于分辨率行的方向 —— 前者是调用方的意图,
    // 后者是文件的事实, 两者叠加而非互相覆盖。
    if (options.FlipVertically && height > 1)
    {
        const SizeType rowFloats = static_cast<SizeType>(width) * channels;

        for (UInt32 row = 0; row < height / 2; ++row)
        {
            Float32* top = pixels.GetData() + static_cast<SizeType>(row) * rowFloats;
            Float32* bottom =
                pixels.GetData() +
                static_cast<SizeType>(height - 1 - row) * rowFloats;

            for (SizeType i = 0; i < rowFloats; ++i)
            {
                const Float32 temp = top[i];
                top[i]             = bottom[i];
                bottom[i]          = temp;
            }
        }
    }

    outImage.Width          = width;
    outImage.Height         = height;
    outImage.Format         = (channels == 4) ? EImageFormat::RGBA32F
                                              : EImageFormat::RGB32F;
    outImage.ColorSpace     = EImageColorSpace::Linear;
    outImage.HasSourceAlpha = false;

    // 浮点数组转字节数组 —— 两者的字节布局相同, 逐字节复制即可
    const SizeType byteSize = pixels.GetSize() * sizeof(Float32);
    outImage.Pixels.SetSize(byteSize);

    const UInt8* source = reinterpret_cast<const UInt8*>(pixels.GetData());

    for (SizeType i = 0; i < byteSize; ++i)
    {
        outImage.Pixels[i] = source[i];
    }

    FImageDecodeResult result = FImageDecodeResult::Success();
    result.Warnings           = headerResult.Warnings;

    return result;
}

} // namespace Limx
