/*******************************************************************************
 * 文件: FJpegDecoder.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   JPEG 基线解码实现 — 标记扫描、量化表、Huffman 表、熵解码、
 *   反量化与反 zigzag、IDCT、色度上采样、YCbCr 转 RGB
 *
 * 设计哲学:
 *   熵解码器独占字节填充与重启的处理 — JPEG 的熵编码数据里，0xFF 后跟 0x00
 *   表示字面 0xFF，跟 0xD0..0xD7 则是重启标记。把这两件事收敛在比特读取器
 *   内部，上层的 MCU 循环就不必在每次取位时提防标记，逻辑得以保持线性。
 *
 *   DC 是差分而非绝对值 — 每个块的 DC 系数存的是与同组件上一个块的差值，
 *   重启标记处归零。漏掉归零会让重启点之后的整幅图像出现亮度阶跃，
 *   而这类图在小样本上完全正常，只有够大的图才会触发重启。
 *
 * 技术特性:
 *   - Huffman 解码用 (最小码, 最大码, 首索引) 三元组按位长逐级匹配
 *   - IDCT 为可分离的行列两趟浮点变换, 余弦基在首次使用时构建
 *   - 上采样按各组件的采样因子做最近邻复制, 支持任意整数因子
 *   - 组件数为 1 时按灰度处理, 为 3 时按 YCbCr 转 RGB
 *
 * 依赖关系:
 *   内部: AssetPipeline/FJpegDecoder.h, Core/Math/FMath.h
 *
 * 注意事项:
 *   渐进式/算术编码/12 位精度一律明确报错, 不做降级尝试
 *
 ******************************************************************************/

#include "AssetPipeline/FJpegDecoder.h"
#include "Core/Math/FMath.h"

namespace Limx
{

namespace
{

// ============================================================================
// 标记
// ============================================================================

constexpr UInt8 kMarkerPrefix = 0xFF;

constexpr UInt8 kMarkerSoi  = 0xD8;   // 图像开始
constexpr UInt8 kMarkerEoi  = 0xD9;   // 图像结束
constexpr UInt8 kMarkerSof0 = 0xC0;   // 基线顺序 DCT
constexpr UInt8 kMarkerSof1 = 0xC1;   // 扩展顺序 DCT (解码方式同基线)
constexpr UInt8 kMarkerSof2 = 0xC2;   // 渐进式 — 不支持
constexpr UInt8 kMarkerSof3 = 0xC3;   // 无损 — 不支持
constexpr UInt8 kMarkerSof9 = 0xC9;   // 算术编码 — 不支持
constexpr UInt8 kMarkerDht  = 0xC4;   // Huffman 表
constexpr UInt8 kMarkerDqt  = 0xDB;   // 量化表
constexpr UInt8 kMarkerDri  = 0xDD;   // 重启间隔
constexpr UInt8 kMarkerSos  = 0xDA;   // 扫描开始
constexpr UInt8 kMarkerRst0 = 0xD0;   // 重启标记起点
constexpr UInt8 kMarkerRst7 = 0xD7;   // 重启标记终点

/// 组件数上限 — 基线 JPEG 最多 4 个 (CMYK)
constexpr UInt32 kMaxComponents = 4;

/// 尺寸上限, 防止损坏的 SOF 触发天文数字的分配
constexpr UInt32 kMaxDimension = 65535;

/// zigzag 次序: 系数在码流中的顺序 → 8x8 块内的行列位置
constexpr UInt8 kZigZagOrder[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

// ============================================================================
// Huffman 表
// ============================================================================

/// JPEG Huffman 解码表
///
/// JPEG 的码字按位长递增排列，因此只需记录每个位长对应的最小码、最大码
/// 与该长度首个符号在值表中的下标，逐级比较即可解码，无需构造完整码表。
struct FJpegHuffmanTable
{
    /// 各位长 (1..16) 的最小码字
    Int32 MinCode[17] = {};

    /// 各位长的最大码字 — -1 表示该长度没有码字
    Int32 MaxCode[17] = {};

    /// 各位长首个符号在 Values 中的下标
    Int32 ValuePointer[17] = {};

    /// 按位长升序排列的符号值
    UInt8 Values[256] = {};

    bool IsDefined = false;

    /// 由"各位长的码字个数"与"符号值序列"构造
    void Build(const UInt8* countsPerLength, const UInt8* values,
               UInt32 totalValues)
    {
        for (UInt32 i = 0; i < totalValues && i < 256; ++i)
        {
            Values[i] = values[i];
        }

        Int32 code       = 0;
        Int32 valueIndex = 0;

        for (Int32 length = 1; length <= 16; ++length)
        {
            const Int32 count = static_cast<Int32>(countsPerLength[length - 1]);

            ValuePointer[length] = valueIndex;
            MinCode[length]      = code;

            code       += count;
            valueIndex += count;

            // 该长度无码字时置 -1, 使匹配循环直接跳过
            MaxCode[length] = (count > 0) ? (code - 1) : -1;

            code <<= 1;
        }

        IsDefined = true;
    }
};

// ============================================================================
// 组件
// ============================================================================

/// 帧内的一个颜色组件
struct FJpegComponent
{
    UInt8 Id = 0;

    /// 水平/垂直采样因子
    UInt8 HorizontalSampling = 1;
    UInt8 VerticalSampling   = 1;

    /// 量化表编号
    UInt8 QuantizationTable = 0;

    /// 扫描中使用的 DC/AC Huffman 表编号
    UInt8 DcTable = 0;
    UInt8 AcTable = 0;

    /// DC 差分预测值 — 重启标记处归零
    Int32 DcPredictor = 0;

    /// 该组件的采样平面尺寸 (以像素计)
    UInt32 PlaneWidth  = 0;
    UInt32 PlaneHeight = 0;

    /// 采样平面, 行距等于 PlaneWidth
    TArray<UInt8> Plane;
};

// ============================================================================
// 熵解码比特流
// ============================================================================

/// 熵编码数据的比特读取器
///
/// 负责处理两件 JPEG 特有的事：0xFF00 的字节填充要还原为字面 0xFF；
/// 0xFFD0..0xFFD7 的重启标记要被识别而非当作数据。把它们收敛在这里，
/// 上层的 MCU 循环就不必在每次取位时提防标记。
class FEntropyReader
{
public:
    FEntropyReader(const UInt8* data, SizeType length)
        : m_Data(data)
        , m_Length(length)
    {
    }

    /// 读取一个比特 — MSB 优先
    Int32 ReadBit()
    {
        if (m_BitCount == 0)
        {
            if (!FetchByte())
            {
                return -1;
            }
        }

        --m_BitCount;
        return (m_CurrentByte >> m_BitCount) & 1;
    }

    /// 读取 count 个比特构成的无符号值
    Int32 ReadBits(UInt32 count)
    {
        Int32 value = 0;

        for (UInt32 i = 0; i < count; ++i)
        {
            const Int32 bit = ReadBit();

            if (bit < 0)
            {
                return -1;
            }

            value = (value << 1) | bit;
        }

        return value;
    }

    /// 丢弃当前字节内剩余的比特
    void Reset()
    {
        m_BitCount = 0;
        m_SawMarker = false;
    }

    /// 跳过一个重启标记 — 返回是否确实读到了 RSTn
    bool SkipRestartMarker()
    {
        Reset();

        // 标记必须紧邻当前位置
        while (m_BytePos + 1 < m_Length)
        {
            if (m_Data[m_BytePos] == kMarkerPrefix)
            {
                const UInt8 marker = m_Data[m_BytePos + 1];

                if (marker >= kMarkerRst0 && marker <= kMarkerRst7)
                {
                    m_BytePos += 2;
                    return true;
                }

                // 其他标记说明扫描已结束
                return false;
            }

            // 重启点之前可能有填充字节
            ++m_BytePos;
        }

        return false;
    }

    LIMX_NODISCARD bool IsExhausted() const
    {
        return m_BytePos >= m_Length && m_BitCount == 0;
    }

    LIMX_NODISCARD SizeType GetBytePosition() const { return m_BytePos; }

private:
    /// 取下一个数据字节, 处理字节填充与标记
    bool FetchByte()
    {
        if (m_SawMarker || m_BytePos >= m_Length)
        {
            // 数据耗尽后按规范补零位, 使末尾不完整的 MCU 仍能解出
            m_CurrentByte = 0;
            m_BitCount    = 8;
            return true;
        }

        UInt8 byte = m_Data[m_BytePos++];

        if (byte == kMarkerPrefix)
        {
            if (m_BytePos >= m_Length)
            {
                m_SawMarker = true;
                m_CurrentByte = 0;
                m_BitCount    = 8;
                return true;
            }

            const UInt8 next = m_Data[m_BytePos];

            if (next == 0x00)
            {
                // 字节填充: 0xFF00 表示字面 0xFF
                ++m_BytePos;
            }
            else
            {
                // 遇到真正的标记 — 熵数据到此为止, 回退以便上层识别
                --m_BytePos;
                m_SawMarker = true;

                m_CurrentByte = 0;
                m_BitCount    = 8;
                return true;
            }
        }

        m_CurrentByte = byte;
        m_BitCount    = 8;

        return true;
    }

    const UInt8* m_Data   = nullptr;
    SizeType     m_Length = 0;
    SizeType     m_BytePos = 0;

    UInt8  m_CurrentByte = 0;
    UInt32 m_BitCount    = 0;

    bool m_SawMarker = false;
};

/// 解码一个 Huffman 符号
Int32 DecodeHuffmanSymbol(FEntropyReader& reader, const FJpegHuffmanTable& table)
{
    Int32 code = 0;

    for (Int32 length = 1; length <= 16; ++length)
    {
        const Int32 bit = reader.ReadBit();

        if (bit < 0)
        {
            return -1;
        }

        code = (code << 1) | bit;

        if (table.MaxCode[length] >= 0 && code <= table.MaxCode[length])
        {
            const Int32 index =
                table.ValuePointer[length] + (code - table.MinCode[length]);

            if (index < 0 || index >= 256)
            {
                return -1;
            }

            return static_cast<Int32>(table.Values[index]);
        }
    }

    return -1;
}

/// 把 size 位的原始值还原为有符号系数 (JPEG 的扩展规则)
///
/// 最高位为 0 表示负数，需减去 (2^size - 1)。
FORCEINLINE Int32 ExtendSigned(Int32 value, UInt32 size)
{
    if (size == 0)
    {
        return 0;
    }

    const Int32 threshold = 1 << (size - 1);

    return (value < threshold) ? (value - (1 << size) + 1) : value;
}

// ============================================================================
// IDCT
// ============================================================================

/// 8x8 逆离散余弦变换的余弦基
///
/// 表在首次使用时构建。行列两趟可分离变换比直接二维求和快一个数量级，
/// 且精度足够 —— 基线 JPEG 的量化误差本就远大于变换误差。
struct FIdctBasis
{
    Float32 Cosine[8][8] = {};

    FIdctBasis()
    {
        for (Int32 x = 0; x < 8; ++x)
        {
            for (Int32 u = 0; u < 8; ++u)
            {
                // C(u) / 2 * cos((2x+1) u pi / 16)
                const Float32 scale = (u == 0)
                                          ? (0.353553390593f)   // 1/(2*sqrt2)
                                          : 0.5f;

                const Float32 angle =
                    static_cast<Float32>((2 * x + 1) * u) * 3.14159265359f / 16.0f;

                Cosine[x][u] = scale * FMath::Cos(angle);
            }
        }
    }
};

/// 全局余弦基 — 首次使用时构建, 之后只读
const FIdctBasis& GetIdctBasis()
{
    static const FIdctBasis basis;
    return basis;
}

/// 对一个 8x8 系数块做 IDCT 并写入平面
///
/// @param coefficients 反量化后的系数, 已按行列顺序排布
/// @param destination  目标平面
/// @param destStride   目标行距
/// @param destX        块左上角在平面中的 x
/// @param destY        块左上角在平面中的 y
/// @param planeWidth   平面宽度 (用于裁剪边缘块)
/// @param planeHeight  平面高度
void InverseDct(const Float32* coefficients, UInt8* destination,
                SizeType destStride, UInt32 destX, UInt32 destY,
                UInt32 planeWidth, UInt32 planeHeight)
{
    const FIdctBasis& basis = GetIdctBasis();

    // ---- 第一趟: 对每一行做 1D IDCT ----
    Float32 intermediate[64];

    for (Int32 v = 0; v < 8; ++v)
    {
        for (Int32 x = 0; x < 8; ++x)
        {
            Float32 sum = 0.0f;

            for (Int32 u = 0; u < 8; ++u)
            {
                sum += basis.Cosine[x][u] * coefficients[v * 8 + u];
            }

            intermediate[v * 8 + x] = sum;
        }
    }

    // ---- 第二趟: 对每一列做 1D IDCT, 同时电平搬移与裁剪 ----
    for (Int32 x = 0; x < 8; ++x)
    {
        for (Int32 y = 0; y < 8; ++y)
        {
            Float32 sum = 0.0f;

            for (Int32 v = 0; v < 8; ++v)
            {
                sum += basis.Cosine[y][v] * intermediate[v * 8 + x];
            }

            // JPEG 在编码前减去 128, 解码时加回
            Int32 value = static_cast<Int32>(sum + 128.5f);

            if (value < 0)   { value = 0; }
            if (value > 255) { value = 255; }

            const UInt32 pixelX = destX + static_cast<UInt32>(x);
            const UInt32 pixelY = destY + static_cast<UInt32>(y);

            // 边缘的 MCU 会超出平面, 超出部分直接丢弃
            if (pixelX < planeWidth && pixelY < planeHeight)
            {
                destination[pixelY * destStride + pixelX] =
                    static_cast<UInt8>(value);
            }
        }
    }
}

// ============================================================================
// 色彩转换
// ============================================================================

/// YCbCr 转 RGB (JFIF 全范围)
FORCEINLINE void YCbCrToRgb(Int32 y, Int32 cb, Int32 cr,
                            UInt8& outR, UInt8& outG, UInt8& outB)
{
    const Int32 shiftedCb = cb - 128;
    const Int32 shiftedCr = cr - 128;

    // 定点近似, 1024 为缩放因子
    Int32 r = y + ((1436 * shiftedCr) >> 10);
    Int32 g = y - ((352 * shiftedCb + 731 * shiftedCr) >> 10);
    Int32 b = y + ((1815 * shiftedCb) >> 10);

    if (r < 0) { r = 0; } else if (r > 255) { r = 255; }
    if (g < 0) { g = 0; } else if (g > 255) { g = 255; }
    if (b < 0) { b = 0; } else if (b > 255) { b = 255; }

    outR = static_cast<UInt8>(r);
    outG = static_cast<UInt8>(g);
    outB = static_cast<UInt8>(b);
}

} // namespace

// ============================================================================
// FJpegDecoder
// ============================================================================

bool FJpegDecoder::IsJpeg(const UInt8* data, SizeType length)
{
    return data != nullptr && length >= 2 &&
           data[0] == kMarkerPrefix && data[1] == kMarkerSoi;
}

FImageDecodeResult FJpegDecoder::Decode(const UInt8* data, SizeType length,
                                        FImageData& outImage,
                                        const FImageDecodeOptions& options)
{
    outImage.Reset();

    if (!IsJpeg(data, length))
    {
        return FImageDecodeResult::Failure(FString("不是 JPEG 文件"));
    }

    TArray<FString> warnings;

    // ---- 解码状态 ----
    UInt16 quantizationTables[4][64] = {};
    bool   quantizationDefined[4]    = {};

    FJpegHuffmanTable dcTables[4];
    FJpegHuffmanTable acTables[4];

    FJpegComponent components[kMaxComponents];
    UInt32 componentCount = 0;

    UInt32 imageWidth  = 0;
    UInt32 imageHeight = 0;

    UInt32 restartInterval = 0;

    bool sawFrame = false;

    SizeType cursor = 2;   // 跳过 SOI

    // ------------------------------------------------------------------
    // 标记扫描
    // ------------------------------------------------------------------

    while (cursor + 1 < length)
    {
        if (data[cursor] != kMarkerPrefix)
        {
            ++cursor;
            continue;
        }

        const UInt8 marker = data[cursor + 1];
        cursor += 2;

        // 填充字节与独立标记无段长
        if (marker == 0xFF || marker == 0x00)
        {
            continue;
        }

        if (marker == kMarkerEoi)
        {
            break;
        }

        if (marker >= kMarkerRst0 && marker <= kMarkerRst7)
        {
            continue;
        }

        if (cursor + 2 > length)
        {
            return FImageDecodeResult::Failure(FString("标记段长度被截断"), cursor);
        }

        const UInt32 segmentLength =
            (static_cast<UInt32>(data[cursor]) << 8) | data[cursor + 1];

        if (segmentLength < 2 ||
            static_cast<UInt64>(cursor) + segmentLength > length)
        {
            return FImageDecodeResult::Failure(
                StringFormat("标记段越界: 长度 {}", segmentLength), cursor);
        }

        const UInt8*   segment     = data + cursor + 2;
        const SizeType segmentSize = segmentLength - 2;

        // ---------------------------------------------------------------
        // 明确不支持的编码模式
        // ---------------------------------------------------------------
        if (marker == kMarkerSof2)
        {
            return FImageDecodeResult::Failure(
                FString("不支持渐进式 JPEG (SOF2)"), cursor);
        }

        if (marker == kMarkerSof3 || marker == kMarkerSof9)
        {
            return FImageDecodeResult::Failure(
                StringFormat("不支持的编码模式 (标记 {})", FHex(marker)), cursor);
        }

        // ---------------------------------------------------------------
        // DQT — 量化表
        // ---------------------------------------------------------------
        if (marker == kMarkerDqt)
        {
            SizeType offset = 0;

            while (offset < segmentSize)
            {
                const UInt8 precision = segment[offset] >> 4;
                const UInt8 tableId   = segment[offset] & 0x0F;
                ++offset;

                if (tableId >= 4)
                {
                    return FImageDecodeResult::Failure(
                        StringFormat("量化表编号 {} 越界", tableId), cursor);
                }

                const SizeType entryBytes = (precision == 0) ? 1 : 2;

                if (offset + 64 * entryBytes > segmentSize)
                {
                    return FImageDecodeResult::Failure(
                        FString("量化表数据不足"), cursor);
                }

                for (UInt32 i = 0; i < 64; ++i)
                {
                    if (precision == 0)
                    {
                        quantizationTables[tableId][i] = segment[offset + i];
                    }
                    else
                    {
                        quantizationTables[tableId][i] = static_cast<UInt16>(
                            (static_cast<UInt32>(segment[offset + i * 2]) << 8) |
                            segment[offset + i * 2 + 1]);
                    }
                }

                quantizationDefined[tableId] = true;
                offset += 64 * entryBytes;
            }
        }
        // ---------------------------------------------------------------
        // DHT — Huffman 表
        // ---------------------------------------------------------------
        else if (marker == kMarkerDht)
        {
            SizeType offset = 0;

            while (offset + 17 <= segmentSize)
            {
                const UInt8 tableClass = segment[offset] >> 4;   // 0=DC, 1=AC
                const UInt8 tableId    = segment[offset] & 0x0F;
                ++offset;

                if (tableId >= 4 || tableClass > 1)
                {
                    return FImageDecodeResult::Failure(
                        StringFormat("Huffman 表编号 {} 类别 {} 越界",
                                     tableId, tableClass),
                        cursor);
                }

                UInt32 totalValues = 0;
                for (UInt32 i = 0; i < 16; ++i)
                {
                    totalValues += segment[offset + i];
                }

                if (totalValues > 256 || offset + 16 + totalValues > segmentSize)
                {
                    return FImageDecodeResult::Failure(
                        FString("Huffman 表数据不足"), cursor);
                }

                FJpegHuffmanTable& table = (tableClass == 0)
                                               ? dcTables[tableId]
                                               : acTables[tableId];

                table.Build(segment + offset, segment + offset + 16, totalValues);

                offset += 16 + totalValues;
            }
        }
        // ---------------------------------------------------------------
        // DRI — 重启间隔
        // ---------------------------------------------------------------
        else if (marker == kMarkerDri)
        {
            if (segmentSize >= 2)
            {
                restartInterval =
                    (static_cast<UInt32>(segment[0]) << 8) | segment[1];
            }
        }
        // ---------------------------------------------------------------
        // SOF0 / SOF1 — 帧头
        // ---------------------------------------------------------------
        else if (marker == kMarkerSof0 || marker == kMarkerSof1)
        {
            if (segmentSize < 6)
            {
                return FImageDecodeResult::Failure(FString("帧头数据不足"), cursor);
            }

            const UInt8 precision = segment[0];

            if (precision != 8)
            {
                return FImageDecodeResult::Failure(
                    StringFormat("不支持 {} 位精度, 仅支持 8 位", precision),
                    cursor);
            }

            imageHeight = (static_cast<UInt32>(segment[1]) << 8) | segment[2];
            imageWidth  = (static_cast<UInt32>(segment[3]) << 8) | segment[4];

            componentCount = segment[5];

            if (imageWidth == 0 || imageHeight == 0)
            {
                return FImageDecodeResult::Failure(FString("图像尺寸为零"), cursor);
            }

            if (imageWidth > kMaxDimension || imageHeight > kMaxDimension)
            {
                return FImageDecodeResult::Failure(
                    StringFormat("图像尺寸 {}x{} 超出上限",
                                 imageWidth, imageHeight),
                    cursor);
            }

            if (componentCount == 0 || componentCount > kMaxComponents)
            {
                return FImageDecodeResult::Failure(
                    StringFormat("组件数 {} 不受支持", componentCount), cursor);
            }

            if (segmentSize < 6 + componentCount * 3)
            {
                return FImageDecodeResult::Failure(
                    FString("组件描述数据不足"), cursor);
            }

            for (UInt32 i = 0; i < componentCount; ++i)
            {
                const UInt8* entry = segment + 6 + i * 3;

                components[i].Id                 = entry[0];
                components[i].HorizontalSampling = entry[1] >> 4;
                components[i].VerticalSampling   = entry[1] & 0x0F;
                components[i].QuantizationTable  = entry[2];

                if (components[i].HorizontalSampling == 0 ||
                    components[i].VerticalSampling == 0 ||
                    components[i].HorizontalSampling > 4 ||
                    components[i].VerticalSampling > 4)
                {
                    return FImageDecodeResult::Failure(
                        StringFormat("组件 {} 的采样因子 {}x{} 不受支持",
                                     i, components[i].HorizontalSampling,
                                     components[i].VerticalSampling),
                        cursor);
                }
            }

            sawFrame = true;
        }
        // ---------------------------------------------------------------
        // SOS — 扫描开始, 其后是熵编码数据
        // ---------------------------------------------------------------
        else if (marker == kMarkerSos)
        {
            if (!sawFrame)
            {
                return FImageDecodeResult::Failure(
                    FString("SOS 出现在帧头之前"), cursor);
            }

            if (segmentSize < 1)
            {
                return FImageDecodeResult::Failure(FString("扫描头数据不足"), cursor);
            }

            const UInt32 scanComponentCount = segment[0];

            if (scanComponentCount != componentCount)
            {
                return FImageDecodeResult::Failure(
                    FString("不支持非交织扫描 (扫描组件数与帧不符)"), cursor);
            }

            if (segmentSize < 1 + scanComponentCount * 2)
            {
                return FImageDecodeResult::Failure(
                    FString("扫描组件描述不足"), cursor);
            }

            for (UInt32 i = 0; i < scanComponentCount; ++i)
            {
                const UInt8 componentId = segment[1 + i * 2];
                const UInt8 tables      = segment[2 + i * 2];

                // 按 id 找到对应的帧组件
                bool matched = false;

                for (UInt32 c = 0; c < componentCount; ++c)
                {
                    if (components[c].Id == componentId)
                    {
                        components[c].DcTable = tables >> 4;
                        components[c].AcTable = tables & 0x0F;
                        matched = true;
                        break;
                    }
                }

                if (!matched)
                {
                    return FImageDecodeResult::Failure(
                        StringFormat("扫描引用了未知的组件 id {}", componentId),
                        cursor);
                }
            }

            // ------------------------------------------------------------
            // 准备各组件的采样平面
            // ------------------------------------------------------------

            UInt32 maxHorizontal = 1;
            UInt32 maxVertical   = 1;

            for (UInt32 c = 0; c < componentCount; ++c)
            {
                maxHorizontal = FMath::Max<UInt32>(
                    maxHorizontal, components[c].HorizontalSampling);
                maxVertical = FMath::Max<UInt32>(
                    maxVertical, components[c].VerticalSampling);
            }

            // MCU 的像素尺寸
            const UInt32 mcuWidth  = maxHorizontal * 8;
            const UInt32 mcuHeight = maxVertical * 8;

            const UInt32 mcusPerRow    = (imageWidth + mcuWidth - 1) / mcuWidth;
            const UInt32 mcuRows       = (imageHeight + mcuHeight - 1) / mcuHeight;

            for (UInt32 c = 0; c < componentCount; ++c)
            {
                FJpegComponent& component = components[c];

                // 平面按 MCU 对齐分配, 避免边缘块写越界
                component.PlaneWidth =
                    mcusPerRow * component.HorizontalSampling * 8;
                component.PlaneHeight =
                    mcuRows * component.VerticalSampling * 8;

                const SizeType planeBytes =
                    static_cast<SizeType>(component.PlaneWidth) *
                    component.PlaneHeight;

                component.Plane.Clear();
                component.Plane.Reserve(planeBytes);

                for (SizeType i = 0; i < planeBytes; ++i)
                {
                    component.Plane.Add(0);
                }

                component.DcPredictor = 0;
            }

            // ------------------------------------------------------------
            // 熵解码
            // ------------------------------------------------------------

            const SizeType entropyStart = cursor + segmentLength;

            FEntropyReader reader(data + entropyStart, length - entropyStart);

            UInt32 mcusSinceRestart = 0;

            for (UInt32 mcuY = 0; mcuY < mcuRows; ++mcuY)
            {
                for (UInt32 mcuX = 0; mcuX < mcusPerRow; ++mcuX)
                {
                    // ---- 重启标记 ----
                    if (restartInterval > 0 &&
                        mcusSinceRestart == restartInterval)
                    {
                        if (!reader.SkipRestartMarker())
                        {
                            warnings.Add(StringFormat(
                                "在 MCU ({},{}) 处未找到预期的重启标记",
                                mcuX, mcuY));
                        }

                        // DC 是差分值, 重启处必须归零 —— 漏掉会让
                        // 重启点之后的整幅图像出现亮度阶跃
                        for (UInt32 c = 0; c < componentCount; ++c)
                        {
                            components[c].DcPredictor = 0;
                        }

                        mcusSinceRestart = 0;
                    }

                    // ---- 逐组件逐块解码 ----
                    for (UInt32 c = 0; c < componentCount; ++c)
                    {
                        FJpegComponent& component = components[c];

                        const UInt8 quantId = component.QuantizationTable;

                        if (quantId >= 4 || !quantizationDefined[quantId])
                        {
                            return FImageDecodeResult::Failure(
                                StringFormat("组件 {} 引用了未定义的量化表 {}",
                                             c, quantId));
                        }

                        const FJpegHuffmanTable& dcTable =
                            dcTables[component.DcTable & 3];
                        const FJpegHuffmanTable& acTable =
                            acTables[component.AcTable & 3];

                        if (!dcTable.IsDefined || !acTable.IsDefined)
                        {
                            return FImageDecodeResult::Failure(
                                StringFormat("组件 {} 引用了未定义的 Huffman 表", c));
                        }

                        for (UInt32 blockY = 0;
                             blockY < component.VerticalSampling; ++blockY)
                        {
                            for (UInt32 blockX = 0;
                                 blockX < component.HorizontalSampling; ++blockX)
                            {
                                Float32 coefficients[64] = {};

                                // ---- DC ----
                                const Int32 dcSize =
                                    DecodeHuffmanSymbol(reader, dcTable);

                                if (dcSize < 0 || dcSize > 15)
                                {
                                    return FImageDecodeResult::Failure(
                                        StringFormat(
                                            "MCU ({},{}) 组件 {} 的 DC 符号无效",
                                            mcuX, mcuY, c));
                                }

                                Int32 dcDifference = 0;

                                if (dcSize > 0)
                                {
                                    const Int32 raw =
                                        reader.ReadBits(static_cast<UInt32>(dcSize));

                                    if (raw < 0)
                                    {
                                        return FImageDecodeResult::Failure(
                                            FString("DC 系数比特被截断"));
                                    }

                                    dcDifference = ExtendSigned(
                                        raw, static_cast<UInt32>(dcSize));
                                }

                                component.DcPredictor += dcDifference;

                                coefficients[0] =
                                    static_cast<Float32>(component.DcPredictor) *
                                    static_cast<Float32>(
                                        quantizationTables[quantId][0]);

                                // ---- AC ----
                                UInt32 index = 1;

                                while (index < 64)
                                {
                                    const Int32 symbol =
                                        DecodeHuffmanSymbol(reader, acTable);

                                    if (symbol < 0)
                                    {
                                        return FImageDecodeResult::Failure(
                                            StringFormat(
                                                "MCU ({},{}) 组件 {} 的 AC 符号无效",
                                                mcuX, mcuY, c));
                                    }

                                    const UInt32 runLength =
                                        static_cast<UInt32>(symbol >> 4);
                                    const UInt32 size =
                                        static_cast<UInt32>(symbol & 0x0F);

                                    if (size == 0)
                                    {
                                        if (runLength == 15)
                                        {
                                            // ZRL: 跳过 16 个零
                                            index += 16;
                                            continue;
                                        }

                                        // EOB: 本块剩余系数全为零
                                        break;
                                    }

                                    index += runLength;

                                    if (index >= 64)
                                    {
                                        break;
                                    }

                                    const Int32 raw = reader.ReadBits(size);

                                    if (raw < 0)
                                    {
                                        return FImageDecodeResult::Failure(
                                            FString("AC 系数比特被截断"));
                                    }

                                    const Int32 value = ExtendSigned(raw, size);

                                    // 反 zigzag 与反量化一并完成
                                    const UInt8 position = kZigZagOrder[index];

                                    coefficients[position] =
                                        static_cast<Float32>(value) *
                                        static_cast<Float32>(
                                            quantizationTables[quantId][index]);

                                    ++index;
                                }

                                // ---- IDCT 并写入平面 ----
                                const UInt32 destX =
                                    (mcuX * component.HorizontalSampling + blockX) * 8;
                                const UInt32 destY =
                                    (mcuY * component.VerticalSampling + blockY) * 8;

                                InverseDct(coefficients,
                                           component.Plane.GetData(),
                                           component.PlaneWidth,
                                           destX, destY,
                                           component.PlaneWidth,
                                           component.PlaneHeight);
                            }
                        }
                    }

                    ++mcusSinceRestart;
                }
            }

            // ------------------------------------------------------------
            // 组装输出
            // ------------------------------------------------------------

            const bool isGrayscale = (componentCount == 1);

            UInt32 destChannels = isGrayscale ? 1u : 3u;

            if (options.ForceFourChannels)
            {
                destChannels = 4;
            }

            const EImageFormat destFormat = MakeImageFormat(destChannels, 1);

            if (destFormat == EImageFormat::Unknown)
            {
                return FImageDecodeResult::Failure(
                    FString("无法确定目标像素格式"));
            }

            outImage.Width          = imageWidth;
            outImage.Height         = imageHeight;
            outImage.Format         = destFormat;
            outImage.HasSourceAlpha = false;

            // JPEG 的像素数据按惯例是 sRGB
            outImage.ColorSpace = EImageColorSpace::Srgb;

            const SizeType rowPitch =
                static_cast<SizeType>(imageWidth) * destChannels;

            outImage.Pixels.Reserve(rowPitch * imageHeight);

            for (SizeType i = 0; i < rowPitch * imageHeight; ++i)
            {
                outImage.Pixels.Add(0);
            }

            for (UInt32 y = 0; y < imageHeight; ++y)
            {
                for (UInt32 x = 0; x < imageWidth; ++x)
                {
                    UInt8* pixel = outImage.Pixels.GetData() +
                                   y * rowPitch +
                                   static_cast<SizeType>(x) * destChannels;

                    if (isGrayscale)
                    {
                        const FJpegComponent& component = components[0];

                        // 单组件时采样因子必为 1x1, 直接取值
                        const UInt8 value =
                            component.Plane[y * component.PlaneWidth + x];

                        pixel[0] = value;

                        if (destChannels >= 3)
                        {
                            pixel[1] = value;
                            pixel[2] = value;
                        }
                    }
                    else
                    {
                        // 各组件按自身采样因子做最近邻上采样
                        Int32 samples[3] = { 0, 128, 128 };

                        for (UInt32 c = 0; c < 3 && c < componentCount; ++c)
                        {
                            const FJpegComponent& component = components[c];

                            const UInt32 sampleX =
                                (x * component.HorizontalSampling) / maxHorizontal;
                            const UInt32 sampleY =
                                (y * component.VerticalSampling) / maxVertical;

                            const UInt32 clampedX =
                                FMath::Min(sampleX, component.PlaneWidth - 1);
                            const UInt32 clampedY =
                                FMath::Min(sampleY, component.PlaneHeight - 1);

                            samples[c] = static_cast<Int32>(
                                component.Plane[clampedY * component.PlaneWidth +
                                                clampedX]);
                        }

                        YCbCrToRgb(samples[0], samples[1], samples[2],
                                   pixel[0], pixel[1], pixel[2]);
                    }

                    if (destChannels == 4)
                    {
                        pixel[3] = 255;
                    }
                    else if (destChannels == 2)
                    {
                        pixel[1] = 255;
                    }
                }
            }

            // ---- 垂直翻转 ----
            if (options.FlipVertically && imageHeight > 1)
            {
                for (UInt32 y = 0; y < imageHeight / 2; ++y)
                {
                    UInt8* top    = outImage.Pixels.GetData() + y * rowPitch;
                    UInt8* bottom = outImage.Pixels.GetData() +
                                    (imageHeight - 1 - y) * rowPitch;

                    for (SizeType i = 0; i < rowPitch; ++i)
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

        cursor += segmentLength;
    }

    if (!sawFrame)
    {
        return FImageDecodeResult::Failure(FString("缺少帧头 (SOF)"));
    }

    return FImageDecodeResult::Failure(FString("缺少扫描数据 (SOS)"));
}

} // namespace Limx
