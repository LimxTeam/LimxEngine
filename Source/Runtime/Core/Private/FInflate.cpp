/*******************************************************************************
 * 文件: FInflate.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   DEFLATE 解压实现 — 比特流读取、规范 Huffman 解码、三种块类型、
 *   LZ77 反向引用、zlib 容器与 Adler-32 校验
 *
 * 设计哲学:
 *   反向引用必须逐字节复制 — DEFLATE 允许距离小于长度的重叠引用
 *   (例如距离 1、长度 100 表示"把上一字节重复 100 次")，这是它表达
 *   游程的方式。用 memcpy 处理会因源与目标重叠而产生错误结果，
 *   必须逐字节前向复制，让刚写入的字节能立即成为后续读取的源。
 *
 *   码长为零的符号不参与构码 — 规范 Huffman 的构造中，码长 0 表示该符号
 *   未出现。若把它计入长度统计，会让所有码字整体偏移，解出完全错误的符号。
 *
 * 技术特性:
 *   - 比特流按 LSB-first 读取, 与 DEFLATE 规范一致
 *   - Huffman 采用 puff 风格的逐位规范解码, 短小可核对
 *   - 每次读位与每次写出都做边界检查, 截断的流会失败而非越界
 *   - 输出按 2 倍策略增长, 避免频繁重分配
 *
 * 依赖关系:
 *   内部: Core/Misc/FInflate.h
 *
 * 注意事项:
 *   存储块 (BTYPE=00) 的 LEN 与 NLEN 必须互补, 不符即判定流已损坏
 *
 ******************************************************************************/

#include "Core/Misc/FInflate.h"

namespace Limx
{

namespace
{

// ============================================================================
// 静态表 — RFC 1951 第 3.2.5 节
// ============================================================================

/// 长度码 257..285 对应的基础长度
constexpr UInt16 kLengthBase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};

/// 长度码的额外比特数
constexpr UInt8 kLengthExtra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};

/// 距离码 0..29 对应的基础距离
constexpr UInt16 kDistanceBase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577
};

/// 距离码的额外比特数
constexpr UInt8 kDistanceExtra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

/// 码长码的传输顺序 — 规范规定的固定次序
constexpr UInt8 kCodeLengthOrder[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

// ============================================================================
// FBitReader — LSB-first 比特流
// ============================================================================

/// 比特流读取器
///
/// DEFLATE 按最低有效位优先打包，因此从字节的低位开始取。
/// 每次读取都检查剩余字节，截断的流会以失败告终而非读越界。
class FBitReader
{
public:
    FBitReader(const UInt8* data, SizeType length)
        : m_Data(data)
        , m_Length(length)
    {
    }

    /// 读取 count 个比特 — 失败时置位错误标志并返回 0
    UInt32 ReadBits(UInt32 count)
    {
        while (m_BitCount < count)
        {
            if (m_BytePos >= m_Length)
            {
                m_Overrun = true;
                return 0;
            }

            m_BitBuffer |= static_cast<UInt32>(m_Data[m_BytePos]) << m_BitCount;
            ++m_BytePos;
            m_BitCount += 8;
        }

        const UInt32 value = m_BitBuffer & ((1u << count) - 1u);

        m_BitBuffer >>= count;
        m_BitCount   -= count;

        return value;
    }

    /// 读取一个比特
    UInt32 ReadBit()
    {
        return ReadBits(1);
    }

    /// 丢弃当前字节内剩余的比特, 对齐到字节边界
    void AlignToByte()
    {
        const UInt32 discard = m_BitCount % 8;

        m_BitBuffer >>= discard;
        m_BitCount   -= discard;
    }

    /// 直接读取一个字节 — 要求已对齐
    UInt8 ReadAlignedByte()
    {
        if (m_BitCount >= 8)
        {
            const UInt8 value = static_cast<UInt8>(m_BitBuffer & 0xFFu);
            m_BitBuffer >>= 8;
            m_BitCount   -= 8;
            return value;
        }

        if (m_BytePos >= m_Length)
        {
            m_Overrun = true;
            return 0;
        }

        return m_Data[m_BytePos++];
    }

    /// 是否发生过越界读取
    LIMX_NODISCARD bool HasOverrun() const { return m_Overrun; }

    /// 缓冲区中尚未消费的字节数 (不含比特缓存)
    LIMX_NODISCARD SizeType GetRemainingBytes() const
    {
        return (m_BytePos < m_Length) ? (m_Length - m_BytePos) : 0;
    }

private:
    const UInt8* m_Data   = nullptr;
    SizeType     m_Length = 0;
    SizeType     m_BytePos = 0;

    UInt32 m_BitBuffer = 0;
    UInt32 m_BitCount  = 0;

    bool m_Overrun = false;
};

// ============================================================================
// FHuffmanTable — 规范 Huffman 解码表
// ============================================================================

/// 规范 Huffman 表
///
/// 只保存"每种码长有多少个符号"与"符号按码长升序排列后的序列"。
/// 规范 Huffman 的性质保证这两项足以唯一还原全部码字，无需存储码字本身。
struct FHuffmanTable
{
    /// 各码长的符号个数, 下标即码长 (0 号位不使用)
    UInt16 Counts[FInflate::kMaxCodeLength + 1] = {};

    /// 按码长升序、同长内按符号值升序排列的符号表
    TArray<UInt16> Symbols;

    /// 由码长数组构造解码表
    ///
    /// @param lengths     各符号的码长, 0 表示该符号未使用
    /// @param symbolCount 符号总数
    /// @return 码长集合是否构成合法的 Huffman 码
    bool Build(const UInt8* lengths, UInt32 symbolCount)
    {
        for (UInt32 i = 0; i <= FInflate::kMaxCodeLength; ++i)
        {
            Counts[i] = 0;
        }

        // 统计各码长的符号数 —— 码长 0 表示符号未出现, 计入 Counts[0]
        // 但不参与后续构码
        for (UInt32 i = 0; i < symbolCount; ++i)
        {
            ++Counts[lengths[i]];
        }

        // 全部符号都未使用 — 空表在动态块中合法 (例如没有距离码)
        if (Counts[0] == symbolCount)
        {
            return true;
        }

        // ------------------------------------------------------------------
        // 检查码长集合是否过满或不完整
        //
        // 每前进一个码长, 可用码字数翻倍并减去该长度已占用的数量。
        // 剩余为负说明码字冲突 (过满), 流已损坏。
        // ------------------------------------------------------------------

        Int32 remaining = 1;

        for (UInt32 length = 1; length <= FInflate::kMaxCodeLength; ++length)
        {
            remaining <<= 1;
            remaining -= static_cast<Int32>(Counts[length]);

            if (remaining < 0)
            {
                return false;
            }
        }

        // ---- 按码长升序排列符号 ----
        UInt16 offsets[FInflate::kMaxCodeLength + 2] = {};

        offsets[1] = 0;
        for (UInt32 length = 1; length <= FInflate::kMaxCodeLength; ++length)
        {
            offsets[length + 1] =
                static_cast<UInt16>(offsets[length] + Counts[length]);
        }

        Symbols.Clear();
        Symbols.Reserve(symbolCount);

        for (UInt32 i = 0; i < symbolCount; ++i)
        {
            Symbols.Add(0);
        }

        for (UInt16 symbol = 0; symbol < symbolCount; ++symbol)
        {
            const UInt8 length = lengths[symbol];

            if (length != 0)
            {
                Symbols[offsets[length]] = symbol;
                ++offsets[length];
            }
        }

        return true;
    }
};

/// 从比特流解码一个符号
///
/// 逐位累积码字并与各长度的首码字比较：若当前码字落在该长度的区间内，
/// 直接由偏移取出符号。这是 zlib 参考实现 puff 的算法。
Int32 DecodeSymbol(FBitReader& reader, const FHuffmanTable& table)
{
    Int32 code  = 0;
    Int32 first = 0;
    Int32 index = 0;

    for (UInt32 length = 1; length <= FInflate::kMaxCodeLength; ++length)
    {
        code |= static_cast<Int32>(reader.ReadBit());

        if (reader.HasOverrun())
        {
            return -1;
        }

        const Int32 count = static_cast<Int32>(table.Counts[length]);

        if (code - first < count)
        {
            const Int32 slot = index + (code - first);

            if (static_cast<SizeType>(slot) >= table.Symbols.GetSize())
            {
                return -1;
            }

            return static_cast<Int32>(table.Symbols[slot]);
        }

        index += count;
        first  = (first + count) << 1;
        code <<= 1;
    }

    // 15 位内未匹配到任何码字 — 流已损坏
    return -1;
}

/// 构造固定 Huffman 表 (RFC 1951 第 3.2.6 节)
bool BuildFixedTables(FHuffmanTable& outLiteral, FHuffmanTable& outDistance)
{
    UInt8 literalLengths[288];

    for (UInt32 i = 0; i < 144; ++i) { literalLengths[i] = 8; }
    for (UInt32 i = 144; i < 256; ++i) { literalLengths[i] = 9; }
    for (UInt32 i = 256; i < 280; ++i) { literalLengths[i] = 7; }
    for (UInt32 i = 280; i < 288; ++i) { literalLengths[i] = 8; }

    if (!outLiteral.Build(literalLengths, 288))
    {
        return false;
    }

    // 固定距离码全部为 5 位
    UInt8 distanceLengths[30];
    for (UInt32 i = 0; i < 30; ++i) { distanceLengths[i] = 5; }

    return outDistance.Build(distanceLengths, 30);
}

/// 读取动态块的码长定义并构造两张表 (RFC 1951 第 3.2.7 节)
bool BuildDynamicTables(FBitReader& reader, FHuffmanTable& outLiteral,
                        FHuffmanTable& outDistance, FString* outError)
{
    const UInt32 literalCount    = reader.ReadBits(5) + 257;
    const UInt32 distanceCount   = reader.ReadBits(5) + 1;
    const UInt32 codeLengthCount = reader.ReadBits(4) + 4;

    if (reader.HasOverrun())
    {
        if (outError != nullptr) { *outError = FString("动态块头部被截断"); }
        return false;
    }

    if (literalCount > 288 || distanceCount > 30)
    {
        if (outError != nullptr) { *outError = FString("动态块码数超出规范上限"); }
        return false;
    }

    // ---- 码长码表 ----
    UInt8 codeLengthLengths[19] = {};

    for (UInt32 i = 0; i < codeLengthCount; ++i)
    {
        codeLengthLengths[kCodeLengthOrder[i]] =
            static_cast<UInt8>(reader.ReadBits(3));
    }

    if (reader.HasOverrun())
    {
        if (outError != nullptr) { *outError = FString("码长码表被截断"); }
        return false;
    }

    FHuffmanTable codeLengthTable;
    if (!codeLengthTable.Build(codeLengthLengths, 19))
    {
        if (outError != nullptr) { *outError = FString("码长码表不构成合法 Huffman 码"); }
        return false;
    }

    // ---- 用码长码表解出字面/长度码与距离码的码长 ----
    UInt8 lengths[288 + 30] = {};

    const UInt32 totalCount = literalCount + distanceCount;
    UInt32 index = 0;

    while (index < totalCount)
    {
        const Int32 symbol = DecodeSymbol(reader, codeLengthTable);

        if (symbol < 0)
        {
            if (outError != nullptr) { *outError = FString("码长序列解码失败"); }
            return false;
        }

        if (symbol < 16)
        {
            lengths[index++] = static_cast<UInt8>(symbol);
        }
        else if (symbol == 16)
        {
            // 重复上一个码长 3..6 次
            if (index == 0)
            {
                if (outError != nullptr) { *outError = FString("重复码出现在序列开头"); }
                return false;
            }

            const UInt8  previous = lengths[index - 1];
            const UInt32 repeat   = 3 + reader.ReadBits(2);

            for (UInt32 i = 0; i < repeat && index < totalCount; ++i)
            {
                lengths[index++] = previous;
            }
        }
        else if (symbol == 17)
        {
            // 重复零码长 3..10 次
            const UInt32 repeat = 3 + reader.ReadBits(3);

            for (UInt32 i = 0; i < repeat && index < totalCount; ++i)
            {
                lengths[index++] = 0;
            }
        }
        else
        {
            // symbol == 18: 重复零码长 11..138 次
            const UInt32 repeat = 11 + reader.ReadBits(7);

            for (UInt32 i = 0; i < repeat && index < totalCount; ++i)
            {
                lengths[index++] = 0;
            }
        }

        if (reader.HasOverrun())
        {
            if (outError != nullptr) { *outError = FString("码长序列被截断"); }
            return false;
        }
    }

    // 块结束符必须存在, 否则解压将无法终止
    if (lengths[256] == 0)
    {
        if (outError != nullptr) { *outError = FString("动态块缺少块结束码"); }
        return false;
    }

    if (!outLiteral.Build(lengths, literalCount))
    {
        if (outError != nullptr) { *outError = FString("字面/长度码表不合法"); }
        return false;
    }

    if (!outDistance.Build(lengths + literalCount, distanceCount))
    {
        if (outError != nullptr) { *outError = FString("距离码表不合法"); }
        return false;
    }

    return true;
}

} // namespace

// ============================================================================
// FInflate — 裸 DEFLATE
// ============================================================================

bool FInflate::Decompress(const UInt8* source, SizeType sourceSize,
                          TArray<UInt8>& output, FString* outError)
{
    output.Clear();

    if (source == nullptr || sourceSize == 0)
    {
        if (outError != nullptr) { *outError = FString("输入为空"); }
        return false;
    }

    FBitReader reader(source, sourceSize);

    // 输出量通常是输入的数倍, 预留一份减少早期重分配
    output.Reserve(sourceSize * 4);

    bool isFinalBlock = false;

    while (!isFinalBlock)
    {
        isFinalBlock = (reader.ReadBits(1) != 0);

        const UInt32 blockType = reader.ReadBits(2);

        if (reader.HasOverrun())
        {
            if (outError != nullptr) { *outError = FString("块头部被截断"); }
            return false;
        }

        // ---------------------------------------------------------------
        // 类型 0: 存储块 — 字节对齐后直接拷贝
        // ---------------------------------------------------------------
        if (blockType == 0)
        {
            reader.AlignToByte();

            const UInt32 low       = reader.ReadAlignedByte();
            const UInt32 high      = reader.ReadAlignedByte();
            const UInt32 length    = low | (high << 8);

            const UInt32 nLow      = reader.ReadAlignedByte();
            const UInt32 nHigh     = reader.ReadAlignedByte();
            const UInt32 nLength   = nLow | (nHigh << 8);

            if (reader.HasOverrun())
            {
                if (outError != nullptr) { *outError = FString("存储块头部被截断"); }
                return false;
            }

            // LEN 与 NLEN 必须互补 — 不符说明流已损坏
            if ((length ^ 0xFFFFu) != nLength)
            {
                if (outError != nullptr)
                {
                    *outError = FString("存储块的 LEN 与 NLEN 不互补");
                }
                return false;
            }

            for (UInt32 i = 0; i < length; ++i)
            {
                const UInt8 byte = reader.ReadAlignedByte();

                if (reader.HasOverrun())
                {
                    if (outError != nullptr) { *outError = FString("存储块数据被截断"); }
                    return false;
                }

                output.Add(byte);
            }

            continue;
        }

        if (blockType == 3)
        {
            if (outError != nullptr) { *outError = FString("保留的块类型 3"); }
            return false;
        }

        // ---------------------------------------------------------------
        // 类型 1/2: Huffman 编码块
        // ---------------------------------------------------------------
        FHuffmanTable literalTable;
        FHuffmanTable distanceTable;

        if (blockType == 1)
        {
            if (!BuildFixedTables(literalTable, distanceTable))
            {
                if (outError != nullptr) { *outError = FString("固定 Huffman 表构造失败"); }
                return false;
            }
        }
        else
        {
            if (!BuildDynamicTables(reader, literalTable, distanceTable, outError))
            {
                return false;
            }
        }

        // ---------------------------------------------------------------
        // 解码符号流
        // ---------------------------------------------------------------
        while (true)
        {
            const Int32 symbol = DecodeSymbol(reader, literalTable);

            if (symbol < 0)
            {
                if (outError != nullptr) { *outError = FString("字面/长度符号解码失败"); }
                return false;
            }

            // ---- 字面量 ----
            if (symbol < 256)
            {
                output.Add(static_cast<UInt8>(symbol));
                continue;
            }

            // ---- 块结束 ----
            if (symbol == 256)
            {
                break;
            }

            // ---- 反向引用 ----
            const UInt32 lengthIndex = static_cast<UInt32>(symbol - 257);

            if (lengthIndex >= 29)
            {
                if (outError != nullptr) { *outError = FString("长度码超出有效范围"); }
                return false;
            }

            const UInt32 matchLength =
                kLengthBase[lengthIndex] +
                reader.ReadBits(kLengthExtra[lengthIndex]);

            const Int32 distanceSymbol = DecodeSymbol(reader, distanceTable);

            if (distanceSymbol < 0 || distanceSymbol >= 30)
            {
                if (outError != nullptr) { *outError = FString("距离符号解码失败"); }
                return false;
            }

            const UInt32 distance =
                kDistanceBase[distanceSymbol] +
                reader.ReadBits(kDistanceExtra[distanceSymbol]);

            if (reader.HasOverrun())
            {
                if (outError != nullptr) { *outError = FString("反向引用的额外比特被截断"); }
                return false;
            }

            if (distance == 0 || distance > output.GetSize())
            {
                if (outError != nullptr)
                {
                    *outError = FString("反向引用距离超出已解压数据");
                }
                return false;
            }

            // ------------------------------------------------------------
            // 逐字节前向复制
            //
            // DEFLATE 允许距离小于长度的重叠引用 (距离 1、长度 100 即
            // "重复上一字节 100 次")，这是它表达游程的方式。用 memcpy
            // 会因源与目标重叠而出错，必须逐字节复制，让刚写入的字节
            // 能立即成为后续读取的源。
            // ------------------------------------------------------------

            const SizeType copyStart = output.GetSize() - distance;

            for (UInt32 i = 0; i < matchLength; ++i)
            {
                output.Add(output[copyStart + i]);
            }
        }
    }

    return true;
}

// ============================================================================
// FInflate — zlib 容器
// ============================================================================

UInt32 FInflate::ComputeAdler32(const UInt8* data, SizeType length)
{
    // Adler-32 的模数 — 小于 65536 的最大素数
    constexpr UInt32 kModulus = 65521;

    UInt32 low  = 1;
    UInt32 high = 0;

    for (SizeType i = 0; i < length; ++i)
    {
        low  = (low + data[i]) % kModulus;
        high = (high + low) % kModulus;
    }

    return (high << 16) | low;
}

bool FInflate::DecompressZlib(const UInt8* source, SizeType sourceSize,
                              TArray<UInt8>& output, FString* outError)
{
    output.Clear();

    // 2 字节头 + 至少 1 字节数据 + 4 字节校验和
    if (source == nullptr || sourceSize < 6)
    {
        if (outError != nullptr) { *outError = FString("zlib 流过短"); }
        return false;
    }

    const UInt8 cmf = source[0];
    const UInt8 flg = source[1];

    // 压缩方法必须是 8 (DEFLATE)
    if ((cmf & 0x0Fu) != 8)
    {
        if (outError != nullptr)
        {
            *outError = StringFormat("zlib 压缩方法 {} 不是 DEFLATE",
                                     static_cast<UInt32>(cmf & 0x0Fu));
        }
        return false;
    }

    // 头部两字节构成的 16 位大端值必须是 31 的倍数
    if (((static_cast<UInt32>(cmf) << 8) | flg) % 31u != 0)
    {
        if (outError != nullptr) { *outError = FString("zlib 头部校验失败"); }
        return false;
    }

    // 预置字典需要外部提供字典内容, PNG 不会用到
    if ((flg & 0x20u) != 0)
    {
        if (outError != nullptr) { *outError = FString("不支持带预置字典的 zlib 流"); }
        return false;
    }

    const SizeType deflateSize = sourceSize - 2 - 4;

    if (!Decompress(source + 2, deflateSize, output, outError))
    {
        return false;
    }

    // ---- 校验 Adler-32 (大端存放在流尾) ----
    const UInt8* checksumBytes = source + sourceSize - 4;

    const UInt32 expected = (static_cast<UInt32>(checksumBytes[0]) << 24) |
                            (static_cast<UInt32>(checksumBytes[1]) << 16) |
                            (static_cast<UInt32>(checksumBytes[2]) << 8) |
                            static_cast<UInt32>(checksumBytes[3]);

    const UInt32 actual = ComputeAdler32(output.GetData(), output.GetSize());

    if (expected != actual)
    {
        if (outError != nullptr)
        {
            *outError = StringFormat(
                "zlib 校验和不符: 期望 {}, 实际 {}", FHex(expected), FHex(actual));
        }
        return false;
    }

    return true;
}

} // namespace Limx
