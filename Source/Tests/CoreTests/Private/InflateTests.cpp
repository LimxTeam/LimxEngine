/*******************************************************************************
 * 文件: InflateTests.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   FInflate 单元测试 — 三种块类型、反向引用、重叠拷贝、zlib 容器校验
 *   与损坏数据的拒绝
 *
 * 设计哲学:
 *   Adler-32 就是最强的断言 — zlib 容器在流尾带有整个解压结果的校验和，
 *   本实现会核对它。因此 DecompressZlib 返回 true 已经等价于"解压结果
 *   逐字节正确"，用例只需再确认长度与关键字节，无须逐字节比对海量数据。
 *
 *   重叠拷贝必须单独覆盖 — DEFLATE 用"距离 1、长度 N"表达游程，
 *   源与目标区间重叠。若实现里用了 memcpy，短测试数据可能侥幸通过，
 *   而长游程会产生错误结果。用例专门构造 1000 字节的单字符游程。
 *
 * 技术特性:
 *   - 测试向量由标准 zlib 生成, 覆盖固定/动态 Huffman 与存储块
 *   - 损坏用例逐项破坏头部、校验和与数据体, 验证各自被拒绝
 *
 * 依赖关系:
 *   内部: CoreTests/CoreTestsMinimal.h
 *
 * 注意事项:
 *   向量以 base64 内嵌, 不依赖外部文件
 *
 ******************************************************************************/

#include "CoreTests/CoreTestsMinimal.h"
#include "Core/Misc/FInflate.h"
#include "Core/Misc/FBase64.h"

using namespace Limx;

namespace
{

/// 由标准 zlib 生成的测试向量 (level 6, 除注明外)

/// "Hello, Limx Engine!" — 19 字节
constexpr const AnsiChar* kShortZlib = "eJzzSM3JyddR8MnMrVBwzUvPzEtVBABA2gZy";

/// "ABCD" 重复 500 次 — 2000 字节, 触发长距离反向引用
constexpr const AnsiChar* kRepeatZlib =
    "eJxzdHJ2cRzFo3gUj+JRPIpHsctQxgDkAgen";

/// 'X' 重复 1000 次 — 触发距离 1 的重叠拷贝
constexpr const AnsiChar* kRunLengthZlib = "eJyLiBgFo2AURAxzAAA6KFfQ";

/// "NoCompression" 重复 20 次, level 0 — 存储块
constexpr const AnsiChar* kStoredZlib =
    "eAEBBAH7/k5vQ29tcHJlc3Npb25Ob0NvbXByZXNzaW9uTm9Db21wcmVzc2lvbk5v"
    "Q29tcHJlc3Npb25Ob0NvbXByZXNzaW9uTm9Db21wcmVzc2lvbk5vQ29tcHJlc3Np"
    "b25Ob0NvbXByZXNzaW9uTm9Db21wcmVzc2lvbk5vQ29tcHJlc3Npb25Ob0NvbXBy"
    "ZXNzaW9uTm9Db21wcmVzc2lvbk5vQ29tcHJlc3Npb25Ob0NvbXByZXNzaW9uTm9D"
    "b21wcmVzc2lvbk5vQ29tcHJlc3Npb25Ob0NvbXByZXNzaW9uTm9Db21wcmVzc2lv"
    "bk5vQ29tcHJlc3Npb25Ob0NvbXByZXNzaW9uB7hqLQ==";

/// 100 行结构化文本 — 典型动态 Huffman
constexpr const AnsiChar* kTextZlib =
    "eJyd2Es2BDEYgNG5VWQJ8k7sBl1oShdNa6zeYQfuOOcb5Z48/nV/WMLlVXh/WMLraX/7"
    "FG6O2/kQ7rbP8Hh6fnkL28dy/Fter7+/wm67v1h/mwhNgiZDU6Cp0DRoOjQDmil7ShBE"
    "QhQKUSxEwRBFQxQOUTxEARFFRBIRic4GEZFERBIRSUQkEZFERBIRSURkEZFFRKbrQkRk"
    "EZFFRBYRWURkEZFFRBERRUQUEVHoBSEiiogoIqKIiCIiioioIqKKiCoiqoio9KgUEVVE"
    "VBFRRUQVEU1ENBHRREQTEU1ENPpniIgmIpqIaCKii4guIrqI6CKii4guIjp9PUVEFxFd"
    "RAwRMUTEEBFDRAwRMUTEEBGDphEiYoiIKSKmiJgiYoqIKSKmiJgiYoqISQOqf4r4AXsb"
    "PgI=";

/// 解码 base64 并解压 zlib
bool InflateBase64(const AnsiChar* base64, TArray<UInt8>& output,
                   FString& outError)
{
    TArray<UInt8> compressed;

    if (!FBase64::Decode(base64, compressed))
    {
        outError = FString("base64 解码失败");
        return false;
    }

    return FInflate::DecompressZlib(compressed.GetData(), compressed.GetSize(),
                                    output, &outError);
}

/// 比对解压结果与期望字节
bool MatchesBytes(const TArray<UInt8>& actual, const AnsiChar* expected,
                  SizeType expectedLength)
{
    if (actual.GetSize() != expectedLength)
    {
        return false;
    }

    for (SizeType i = 0; i < expectedLength; ++i)
    {
        if (actual[i] != static_cast<UInt8>(expected[i]))
        {
            return false;
        }
    }

    return true;
}

} // namespace

// ============================================================================
// 基本解压
// ============================================================================

LIMX_TEST(Inflate, DecompressesShortText)
{
    TArray<UInt8> output;
    FString error;

    LIMX_REQUIRE_TRUE(InflateBase64(kShortZlib, output, error));

    const AnsiChar* expected = "Hello, Limx Engine!";
    LIMX_EXPECT_EQ(output.GetSize(), SizeType(19));
    LIMX_EXPECT_TRUE(MatchesBytes(output, expected, 19));
}

LIMX_TEST(Inflate, DecompressesLongBackReferences)
{
    TArray<UInt8> output;
    FString error;

    // "ABCD" 重复 500 次 — 压缩后仅 27 字节, 全靠反向引用还原
    LIMX_REQUIRE_TRUE(InflateBase64(kRepeatZlib, output, error));

    LIMX_REQUIRE_EQ(output.GetSize(), SizeType(2000));

    for (SizeType i = 0; i < 2000; ++i)
    {
        const AnsiChar expected = "ABCD"[i % 4];
        LIMX_REQUIRE_EQ(output[i], static_cast<UInt8>(expected));
    }
}

LIMX_TEST(Inflate, HandlesOverlappingCopy)
{
    TArray<UInt8> output;
    FString error;

    // 1000 个 'X' — DEFLATE 用"距离 1、长度 N"表达游程, 源与目标重叠。
    // 用 memcpy 实现反向引用会在这里产出错误结果。
    LIMX_REQUIRE_TRUE(InflateBase64(kRunLengthZlib, output, error));

    LIMX_REQUIRE_EQ(output.GetSize(), SizeType(1000));

    for (SizeType i = 0; i < 1000; ++i)
    {
        LIMX_REQUIRE_EQ(output[i], static_cast<UInt8>('X'));
    }
}

LIMX_TEST(Inflate, DecompressesStoredBlock)
{
    TArray<UInt8> output;
    FString error;

    // level 0 产出存储块 — 走的是与 Huffman 完全不同的代码路径
    LIMX_REQUIRE_TRUE(InflateBase64(kStoredZlib, output, error));

    LIMX_REQUIRE_EQ(output.GetSize(), SizeType(260));

    // 逐段比对 "NoCompression"
    const AnsiChar* unit = "NoCompression";

    for (SizeType block = 0; block < 20; ++block)
    {
        for (SizeType i = 0; i < 13; ++i)
        {
            LIMX_REQUIRE_EQ(output[block * 13 + i],
                            static_cast<UInt8>(unit[i]));
        }
    }
}

LIMX_TEST(Inflate, DecompressesDynamicHuffmanText)
{
    TArray<UInt8> output;
    FString error;

    // 结构化文本会让 zlib 选用动态 Huffman, 需要解出码长码表
    LIMX_REQUIRE_TRUE(InflateBase64(kTextZlib, output, error));

    LIMX_REQUIRE_EQ(output.GetSize(), SizeType(5290));

    // 首行应为 "line 0: the quick brown fox jumps over the lazy dog"
    const AnsiChar* firstLine = "line 0: the quick brown fox jumps over the lazy dog";

    SizeType lineLength = 0;
    while (firstLine[lineLength] != '\0')
    {
        LIMX_REQUIRE_EQ(output[lineLength],
                        static_cast<UInt8>(firstLine[lineLength]));
        ++lineLength;
    }

    // 紧随首行之后应是换行符 — 由行长推导而非硬编码位置
    LIMX_EXPECT_EQ(output[lineLength], static_cast<UInt8>('\n'));
}

// ============================================================================
// Adler-32
// ============================================================================

LIMX_TEST(Inflate, ComputesAdler32)
{
    // RFC 1950 的定义: 空输入的 Adler-32 为 1
    LIMX_EXPECT_EQ(FInflate::ComputeAdler32(nullptr, 0), UInt32(1));

    const UInt8 wikipedia[] = { 'W', 'i', 'k', 'i', 'p', 'e', 'd', 'i', 'a' };

    // "Wikipedia" 的 Adler-32 是维基百科条目中的标准示例值
    LIMX_EXPECT_EQ(FInflate::ComputeAdler32(wikipedia, sizeof(wikipedia)),
                   UInt32(0x11E60398));
}

LIMX_TEST(Inflate, ChecksumMismatchIsRejected)
{
    TArray<UInt8> compressed;
    LIMX_REQUIRE_TRUE(FBase64::Decode(kShortZlib, compressed));
    LIMX_REQUIRE_GT(compressed.GetSize(), SizeType(6));

    // 破坏尾部的 Adler-32
    compressed[compressed.GetSize() - 1] ^= 0xFFu;

    TArray<UInt8> output;
    FString error;

    LIMX_EXPECT_FALSE(FInflate::DecompressZlib(
        compressed.GetData(), compressed.GetSize(), output, &error));

    LIMX_EXPECT_FALSE(error.IsEmpty());
}

// ============================================================================
// 损坏数据的拒绝
// ============================================================================

LIMX_TEST(Inflate, RejectsEmptyInput)
{
    TArray<UInt8> output;
    FString error;

    LIMX_EXPECT_FALSE(FInflate::Decompress(nullptr, 0, output, &error));
    LIMX_EXPECT_FALSE(FInflate::DecompressZlib(nullptr, 0, output, &error));
}

LIMX_TEST(Inflate, RejectsTooShortZlibStream)
{
    const UInt8 tooShort[] = { 0x78, 0x9C, 0x00 };

    TArray<UInt8> output;
    FString error;

    LIMX_EXPECT_FALSE(FInflate::DecompressZlib(tooShort, sizeof(tooShort),
                                               output, &error));
}

LIMX_TEST(Inflate, RejectsWrongCompressionMethod)
{
    TArray<UInt8> compressed;
    LIMX_REQUIRE_TRUE(FBase64::Decode(kShortZlib, compressed));

    // CMF 低四位是压缩方法, 必须为 8
    compressed[0] = 0x79;

    TArray<UInt8> output;
    FString error;

    LIMX_EXPECT_FALSE(FInflate::DecompressZlib(
        compressed.GetData(), compressed.GetSize(), output, &error));
}

LIMX_TEST(Inflate, RejectsBadHeaderChecksum)
{
    TArray<UInt8> compressed;
    LIMX_REQUIRE_TRUE(FBase64::Decode(kShortZlib, compressed));

    // 头两字节构成的 16 位大端值必须是 31 的倍数
    compressed[1] = 0x00;

    TArray<UInt8> output;
    FString error;

    LIMX_EXPECT_FALSE(FInflate::DecompressZlib(
        compressed.GetData(), compressed.GetSize(), output, &error));
}

LIMX_TEST(Inflate, RejectsPresetDictionary)
{
    TArray<UInt8> compressed;
    LIMX_REQUIRE_TRUE(FBase64::Decode(kShortZlib, compressed));

    // FDICT 位置位表示需要外部字典 — 本实现不支持
    // 置位后需同时修正头部校验以确保拒绝原因是字典而非校验
    compressed[1] |= 0x20u;

    for (UInt32 candidate = 0; candidate < 256; ++candidate)
    {
        const UInt8 flg = static_cast<UInt8>((compressed[1] & 0xE0u) | (candidate & 0x1Fu));

        if ((((static_cast<UInt32>(compressed[0]) << 8) | flg) % 31u) == 0)
        {
            compressed[1] = flg;
            break;
        }
    }

    TArray<UInt8> output;
    FString error;

    LIMX_EXPECT_FALSE(FInflate::DecompressZlib(
        compressed.GetData(), compressed.GetSize(), output, &error));
}

LIMX_TEST(Inflate, RejectsTruncatedStream)
{
    TArray<UInt8> compressed;
    LIMX_REQUIRE_TRUE(FBase64::Decode(kRepeatZlib, compressed));
    LIMX_REQUIRE_GT(compressed.GetSize(), SizeType(10));

    // 截去后半段 — 解压过程中会读到流末尾
    TArray<UInt8> truncated;
    for (SizeType i = 0; i < compressed.GetSize() / 2; ++i)
    {
        truncated.Add(compressed[i]);
    }

    TArray<UInt8> output;
    FString error;

    LIMX_EXPECT_FALSE(FInflate::Decompress(truncated.GetData() + 2,
                                           truncated.GetSize() - 2,
                                           output, &error));
    LIMX_EXPECT_FALSE(error.IsEmpty());
}

LIMX_TEST(Inflate, RejectsReservedBlockType)
{
    // 单字节 0x07: BFINAL=1, BTYPE=11 (保留)
    const UInt8 reserved[] = { 0x07 };

    TArray<UInt8> output;
    FString error;

    LIMX_EXPECT_FALSE(FInflate::Decompress(reserved, sizeof(reserved),
                                           output, &error));
}

LIMX_TEST(Inflate, RejectsCorruptedStoredBlockLength)
{
    TArray<UInt8> compressed;
    LIMX_REQUIRE_TRUE(FBase64::Decode(kStoredZlib, compressed));
    LIMX_REQUIRE_GT(compressed.GetSize(), SizeType(10));

    // 存储块的 LEN 与 NLEN 必须互补 — 破坏 LEN 的低字节
    // (zlib 头 2 字节 + 块头 1 字节之后是 LEN)
    compressed[3] ^= 0xFFu;

    TArray<UInt8> output;
    FString error;

    LIMX_EXPECT_FALSE(FInflate::DecompressZlib(
        compressed.GetData(), compressed.GetSize(), output, &error));
}

// ============================================================================
// 规模
// ============================================================================

LIMX_TEST(Inflate, HandlesRepeatedDecompression)
{
    // 复用同一个输出数组多次解压 — 验证 Clear 后状态干净
    TArray<UInt8> output;
    FString error;

    for (Int32 round = 0; round < 10; ++round)
    {
        LIMX_REQUIRE_TRUE(InflateBase64(kRepeatZlib, output, error));
        LIMX_REQUIRE_EQ(output.GetSize(), SizeType(2000));
    }

    LIMX_TEST_INFO("重复解压 10 轮, 每轮 2000 字节, 结果一致");
}
