/*******************************************************************************
 * 文件: FCompression.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   轻量压缩/解压 — 基于 LZ77 变体的快速压缩算法
 *   提供字节数组的压缩与解压缩，优先速度而非压缩率
 *   用于资产打包、网络数据压缩、快照保存等场景
 *
 * 设计哲学:
 *   速度优先 — 牺牲部分压缩率换取极高的压缩/解压速度
 *   无状态 — 压缩和解压均为无状态的静态函数
 *   自描述格式 — 压缩流头部记录原始大小，便于分配解压缓冲区
 *
 * 技术特性:
 *   - Compress: 压缩字节数组
 *   - Decompress: 解压字节数组
 *   - GetMaxCompressedSize: 预计算压缩后的最大大小
 *   - GetDecompressedSize: 从压缩头部读取原始大小
 *   - 格式: [4字节原始大小][压缩数据流]
 *   - 令牌: [literal_length:4|match_length:4][literals][offset:2]
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Memory/MemoryOps.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Memory/MemoryOps.h"

namespace Limx
{

/// 轻量压缩/解压
struct FCompression
{
    /// 压缩头部大小 (4 字节存储原始大小)
    static constexpr SizeType kHeaderSize = 4;

    /// 最大匹配长度 (4 位 = 15, +4 最小匹配 = 19)
    static constexpr SizeType kMinMatchLength = 4;
    static constexpr SizeType kMaxMatchLength = 15 + kMinMatchLength;

    /// 最大字面量长度 (4 位 = 15)
    static constexpr SizeType kMaxLiteralRun = 15;

    /// 滑动窗口大小 (16 位偏移 = 64KB)
    static constexpr SizeType kWindowSize = 65535;

    /// 哈希表大小 (2^14 = 16384)
    static constexpr SizeType kHashTableSize = 1 << 14;
    static constexpr SizeType kHashTableMask = kHashTableSize - 1;

    // ========================================================================
    // 压缩
    // ========================================================================

    /// 预计算压缩后的最大大小
    LIMX_NODISCARD static SizeType GetMaxCompressedSize(
        SizeType sourceSize)
    {
        // 最坏情况: 头部 + 每 15 字节一个令牌字节 + 原始数据
        return kHeaderSize + sourceSize + (sourceSize / 15) + 16;
    }

    /// 压缩数据
    /// @param source     输入数据
    /// @param sourceSize 输入数据大小
    /// @param dest       输出缓冲区 (大小 >= GetMaxCompressedSize)
    /// @return 压缩后的实际大小 (含头部)
    static SizeType Compress(const UInt8* source, SizeType sourceSize,
                               UInt8* dest)
    {
        if (sourceSize == 0)
        {
            WriteUInt32LE(dest, 0);
            return kHeaderSize;
        }

        // 写入头部: 原始大小
        WriteUInt32LE(dest, static_cast<UInt32>(sourceSize));
        SizeType destPos = kHeaderSize;

        // 哈希表 — 存储每个 4 字节序列的最近位置
        // 使用栈上分配 (64KB)，在引擎实际使用中可改为分配器
        SizeType hashTable[kHashTableSize];
        for (SizeType hashIndex = 0;
             hashIndex < kHashTableSize; ++hashIndex)
        {
            hashTable[hashIndex] = static_cast<SizeType>(-1);
        }

        SizeType sourcePos = 0;
        SizeType literalStart = 0;

        while (sourcePos + kMinMatchLength <= sourceSize)
        {
            // 计算当前位置的哈希
            UInt32 hash = Hash4Bytes(source + sourcePos);
            SizeType matchPos = hashTable[hash];
            hashTable[hash] = sourcePos;

            // 尝试匹配
            SizeType matchLength = 0;
            SizeType matchOffset = 0;

            if (matchPos != static_cast<SizeType>(-1) &&
                sourcePos - matchPos <= kWindowSize &&
                matchPos < sourcePos)
            {
                // 计算匹配长度
                matchLength = CalculateMatchLength(
                    source, matchPos, sourcePos, sourceSize);

                if (matchLength >= kMinMatchLength)
                {
                    matchOffset = sourcePos - matchPos;
                }
                else
                {
                    matchLength = 0;
                }
            }

            if (matchLength >= kMinMatchLength)
            {
                // 输出字面量 + 匹配
                SizeType literalLength = sourcePos - literalStart;

                // 分批输出长字面量
                while (literalLength > 0)
                {
                    SizeType runLength = literalLength;
                    if (runLength > kMaxLiteralRun)
                    {
                        runLength = kMaxLiteralRun;
                    }

                    SizeType encodedMatch = matchLength - kMinMatchLength;
                    if (encodedMatch > 15) encodedMatch = 15;

                    if (literalLength <= kMaxLiteralRun)
                    {
                        // 最后一批: 写令牌
                        UInt8 token = static_cast<UInt8>(
                            (runLength << 4) |
                            (encodedMatch & 0x0F));
                        dest[destPos++] = token;

                        // 写字面量
                        Memory::MemCopy(
                            dest + destPos,
                            source + literalStart,
                            runLength);
                        destPos += runLength;
                        literalLength = 0;
                    }
                    else
                    {
                        // 中间批次: 纯字面量令牌 (匹配长度=0)
                        UInt8 token = static_cast<UInt8>(
                            runLength << 4);
                        dest[destPos++] = token;
                        Memory::MemCopy(
                            dest + destPos,
                            source + literalStart,
                            runLength);
                        destPos += runLength;
                        literalStart += runLength;
                        literalLength -= runLength;

                        // 写零偏移 (无匹配)
                        dest[destPos++] = 0;
                        dest[destPos++] = 0;
                    }
                }

                // 写匹配偏移
                dest[destPos++] = static_cast<UInt8>(
                    matchOffset & 0xFF);
                dest[destPos++] = static_cast<UInt8>(
                    (matchOffset >> 8) & 0xFF);

                sourcePos += matchLength;
                literalStart = sourcePos;
            }
            else
            {
                ++sourcePos;
            }
        }

        // 输出剩余字面量
        SizeType remainingLiterals = sourceSize - literalStart;
        if (remainingLiterals > 0)
        {
            while (remainingLiterals > 0)
            {
                SizeType runLength = remainingLiterals;
                if (runLength > kMaxLiteralRun)
                {
                    runLength = kMaxLiteralRun;
                }

                UInt8 token = static_cast<UInt8>(runLength << 4);
                dest[destPos++] = token;
                Memory::MemCopy(
                    dest + destPos,
                    source + literalStart,
                    runLength);
                destPos += runLength;
                literalStart += runLength;
                remainingLiterals -= runLength;

                // 终止匹配偏移 (0 = 无匹配)
                dest[destPos++] = 0;
                dest[destPos++] = 0;
            }
        }

        return destPos;
    }

    // ========================================================================
    // 解压
    // ========================================================================

    /// 从压缩头部读取原始大小
    LIMX_NODISCARD static SizeType GetDecompressedSize(
        const UInt8* compressedData)
    {
        return static_cast<SizeType>(ReadUInt32LE(compressedData));
    }

    /// 解压数据
    /// @param source     压缩数据 (含头部)
    /// @param sourceSize 压缩数据大小
    /// @param dest       输出缓冲区 (大小 >= GetDecompressedSize)
    /// @return 解压后的实际大小，失败返回 0
    static SizeType Decompress(const UInt8* source,
                                 SizeType sourceSize,
                                 UInt8* dest)
    {
        if (sourceSize < kHeaderSize) return 0;

        SizeType originalSize = GetDecompressedSize(source);
        if (originalSize == 0) return 0;

        SizeType sourcePos = kHeaderSize;
        SizeType destPos = 0;

        while (sourcePos < sourceSize && destPos < originalSize)
        {
            // 读令牌
            UInt8 token = source[sourcePos++];
            SizeType literalLength =
                static_cast<SizeType>((token >> 4) & 0x0F);
            SizeType matchLength =
                static_cast<SizeType>(token & 0x0F) +
                kMinMatchLength;

            // 拷贝字面量
            if (literalLength > 0)
            {
                if (sourcePos + literalLength > sourceSize ||
                    destPos + literalLength > originalSize)
                {
                    return 0; // 数据损坏
                }
                Memory::MemCopy(
                    dest + destPos,
                    source + sourcePos,
                    literalLength);
                sourcePos += literalLength;
                destPos += literalLength;
            }

            // 读匹配偏移
            if (sourcePos + 2 > sourceSize) break;
            UInt16 matchOffset = static_cast<UInt16>(
                source[sourcePos] |
                (static_cast<UInt16>(source[sourcePos + 1]) << 8));
            sourcePos += 2;

            // 执行匹配拷贝
            if (matchOffset > 0 && (token & 0x0F) > 0)
            {
                if (matchOffset > destPos) return 0; // 无效偏移
                SizeType matchSource = destPos - matchOffset;

                if (destPos + matchLength > originalSize)
                {
                    matchLength = originalSize - destPos;
                }

                // 逐字节拷贝 (匹配区可能与输出重叠)
                for (SizeType byteIndex = 0;
                     byteIndex < matchLength; ++byteIndex)
                {
                    dest[destPos++] =
                        dest[matchSource + byteIndex];
                }
            }
        }

        return destPos;
    }

private:
    // ========================================================================
    // 内部辅助
    // ========================================================================

    /// 4 字节哈希
    LIMX_NODISCARD static UInt32 Hash4Bytes(const UInt8* ptr)
    {
        UInt32 value =
            static_cast<UInt32>(ptr[0]) |
            (static_cast<UInt32>(ptr[1]) << 8) |
            (static_cast<UInt32>(ptr[2]) << 16) |
            (static_cast<UInt32>(ptr[3]) << 24);
        return (value * 2654435761u) >> 18;
    }

    /// 计算匹配长度
    LIMX_NODISCARD static SizeType CalculateMatchLength(
        const UInt8* source, SizeType matchPos,
        SizeType currentPos, SizeType sourceSize)
    {
        SizeType length = 0;
        SizeType maxLength = sourceSize - currentPos;
        if (maxLength > kMaxMatchLength)
        {
            maxLength = kMaxMatchLength;
        }

        while (length < maxLength &&
               source[matchPos + length] ==
               source[currentPos + length])
        {
            ++length;
        }

        return length;
    }

    /// 小端写 UInt32
    static void WriteUInt32LE(UInt8* dest, UInt32 value)
    {
        dest[0] = static_cast<UInt8>(value & 0xFF);
        dest[1] = static_cast<UInt8>((value >> 8) & 0xFF);
        dest[2] = static_cast<UInt8>((value >> 16) & 0xFF);
        dest[3] = static_cast<UInt8>((value >> 24) & 0xFF);
    }

    /// 小端读 UInt32
    LIMX_NODISCARD static UInt32 ReadUInt32LE(const UInt8* src)
    {
        return static_cast<UInt32>(src[0]) |
               (static_cast<UInt32>(src[1]) << 8) |
               (static_cast<UInt32>(src[2]) << 16) |
               (static_cast<UInt32>(src[3]) << 24);
    }
};

} // namespace Limx
