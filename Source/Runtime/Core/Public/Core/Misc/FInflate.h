/*******************************************************************************
 * 文件: FInflate.h
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   DEFLATE 解压 — 实现 RFC 1951 的解压侧，以及 RFC 1950 的 zlib 容器包装
 *   PNG 的像素数据即以 zlib 容器封装的 DEFLATE 流存放，本模块是其前置依赖
 *
 * 设计哲学:
 *   与自研压缩分家 — Core 已有的 FCompression 是 LZ4 风格的自研格式，
 *   与 DEFLATE 在码流结构上毫无共同点。强行合并只会让两者都变得难懂，
 *   因此单独成模块，各自服务各自的场景（FCompression 面向引擎自有数据，
 *   FInflate 面向外部标准格式）。
 *
 *   规范 Huffman 逐位解码 — 采用 zlib 参考实现 puff 的算法：按码长统计
 *   计数并计算各长度的首码字，逐位比较即可定位符号。它比查表法慢，
 *   但短小到可以逐行核对正确性，而解压错误产生的是花屏而非崩溃，
 *   极难靠现象定位，正确性因此优先于吞吐。
 *
 *   越界即失败 — 压缩流来自外部文件，可能被截断或篡改。每一次读位、
 *   每一次反向引用都做边界检查，任何越界立即失败并给出原因，
 *   绝不越过缓冲区边界读写。
 *
 * 技术特性:
 *   - 支持三种块类型: 存储 / 固定 Huffman / 动态 Huffman
 *   - LZ77 反向引用支持重叠拷贝 (距离小于长度), 逐字节复制而非 memcpy
 *   - zlib 容器校验 CMF/FLG 与 Adler-32，拒绝预置字典
 *   - 输出缓冲区按需增长, 不要求调用方预知解压尺寸
 *
 * 依赖关系:
 *   内部: Core/CoreMinimal.h, Core/Containers/TArray.h,
 *          Core/Containers/FString.h
 *
 * 注意事项:
 *   仅实现解压 — 压缩侧请用 FCompression 或外部工具
 *   逐位解码未做查表优化, 若纹理加载成为瓶颈可在此处引入快速表
 *
 ******************************************************************************/

#pragma once

#include "Core/CoreMinimal.h"
#include "Core/Containers/TArray.h"
#include "Core/Containers/FString.h"

namespace Limx
{

// ============================================================================
// FInflate — DEFLATE 解压
// ============================================================================

/// DEFLATE / zlib 解压器 — 全静态接口
class LIMX_CORE_API FInflate
{
public:
    /// DEFLATE 的滑动窗口上限
    static constexpr UInt32 kMaxWindowSize = 32768;

    /// Huffman 码字的最大位长
    static constexpr UInt32 kMaxCodeLength = 15;

    FInflate()                           = delete;
    ~FInflate()                          = delete;
    FInflate(const FInflate&)            = delete;
    FInflate& operator=(const FInflate&) = delete;

    /// 解压裸 DEFLATE 流 (RFC 1951)
    /// @param source     压缩数据
    /// @param sourceSize 压缩字节数
    /// @param output     解压结果 (调用前会被清空)
    /// @param outError   失败原因, 可为 nullptr
    /// @return 是否成功
    LIMX_NODISCARD static bool Decompress(const UInt8* source,
                                          SizeType sourceSize,
                                          TArray<UInt8>& output,
                                          FString* outError = nullptr);

    /// 解压 zlib 容器 (RFC 1950) — 校验头部与 Adler-32
    /// @param source     含 2 字节头与 4 字节校验和的完整 zlib 流
    /// @param sourceSize 字节数
    /// @param output     解压结果
    /// @param outError   失败原因, 可为 nullptr
    /// @return 是否成功
    LIMX_NODISCARD static bool DecompressZlib(const UInt8* source,
                                              SizeType sourceSize,
                                              TArray<UInt8>& output,
                                              FString* outError = nullptr);

    /// 计算 Adler-32 校验和
    LIMX_NODISCARD static UInt32 ComputeAdler32(const UInt8* data,
                                                SizeType length);
};

} // namespace Limx
