/*******************************************************************************
 * 文件: FDdsDecoder.cpp
 * 创建时间: 2026-08-30
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   DDS 容器解析实现 — 头部字段逐项校验、DXGI 格式映射、mip 链对账
 *
 * 设计哲学:
 *   校验顺序即诊断顺序 — 先判魔数, 再判结构版本, 再判"是不是我们能读的
 *   那一类", 最后才算载荷。倒过来的话, 一个根本不是 DDS 的文件会先在
 *   块公式里算出一个天文数字, 报出来的却是"载荷被截断"。
 *
 *   每条错误都带上具体数值 — "DDS 解析失败" 这句话在批量导入的日志里
 *   毫无用处。期望值与实际值都写出来, 才能一眼看出是写出方的问题还是
 *   文件被截断。这一点与写出侧 Programs/lat/src/dds.rs 保持一致。
 *
 *   多出来的尾部字节只告警不拒绝 — 有些工具会在 DDS 尾部塞元数据。
 *   数据本身完整可用, 拒绝加载等于让一个无关的附加块毁掉整张贴图。
 *   载荷不足则相反: 那意味着后面的 mip 层读到的是别的东西, 必须失败。
 *
 * 技术特性:
 *   - 全部多字节字段按小端读取, 不依赖主机字节序
 *   - 头部字段的偏移量以常量命名, 与 dds.rs 里的 push 顺序逐条对应
 *   - 载荷只拷贝按块公式算出的那部分, 尾部附加数据不会混进纹理
 *
 * 依赖关系:
 *   内部: AssetPipeline/FDdsDecoder.h
 *
 * 注意事项:
 *   不做任何解压 — 输出的是原样的块数据
 *
 *   只接受字节流, 不提供"从文件读"的入口 — 与 FPngDecoder / FHdrDecoder
 *   一致。调用方 (FSceneLoader) 本来就要先读出字节才能按魔数分流,
 *   再提供一个读文件的重载只会多出一条没人走、因而也没人测的路径。
 *
 ******************************************************************************/

#include "AssetPipeline/FDdsDecoder.h"

namespace Limx
{

namespace
{

// ============================================================================
// DDS 常量 — 与 Programs/lat/src/dds.rs 逐条对应
// ============================================================================

/// "DDS " 的小端 u32
constexpr UInt32 kDdsMagic = 0x2053'4444u;

/// "DX10" 的小端 u32
constexpr UInt32 kFourCcDx10 = 0x3031'5844u;

constexpr UInt32 kDdsHeaderSize      = 124;
constexpr UInt32 kDdsPixelFormatSize = 32;

/// dwFlags: dwPitchOrLinearSize 存的是第 0 层的总字节数
constexpr UInt32 kDdsdLinearSize = 0x0008'0000u;

/// ddspf.dwFlags: dwFourCC 有效
constexpr UInt32 kDdpfFourCc = 0x0000'0004u;

/// D3D10_RESOURCE_DIMENSION_TEXTURE2D
constexpr UInt32 kResourceDimensionTexture2D = 3;

/// DDS_RESOURCE_MISC_TEXTURECUBE
///
/// 这一位单独拦一次是必需的: 立方体贴图的 arraySize 同样是 1, 载荷却是
/// 六份。只查 arraySize 的话它会一路通过, 而"文件比期望长"只是告警 ——
/// 结果是六个面里只有 +X 被读进来, 且没有任何报错。
constexpr UInt32 kDdsResourceMiscTextureCube = 0x4u;

/// DDS_ALPHA_MODE_PREMULTIPLIED
constexpr UInt32 kDdsAlphaModePremultiplied = 2;

// ---- 头部字段偏移 (自文件起始) ----
constexpr SizeType kOffsetMagic             = 0;
constexpr SizeType kOffsetHeaderSize        = 4;
constexpr SizeType kOffsetFlags             = 8;
constexpr SizeType kOffsetHeight            = 12;
constexpr SizeType kOffsetWidth             = 16;
constexpr SizeType kOffsetLinearSize        = 20;
constexpr SizeType kOffsetMipMapCount       = 28;
constexpr SizeType kOffsetPixelFormatSize   = 76;
constexpr SizeType kOffsetPixelFormatFlags  = 80;
constexpr SizeType kOffsetFourCc            = 84;
constexpr SizeType kOffsetDxgiFormat        = 128;
constexpr SizeType kOffsetResourceDimension = 132;
constexpr SizeType kOffsetMiscFlag          = 136;
constexpr SizeType kOffsetArraySize         = 140;
constexpr SizeType kOffsetMiscFlags2        = 144;

// ---- DXGI_FORMAT (来自 dxgiformat.h) ----
constexpr UInt32 kDxgiFormatBc1Unorm     = 71;
constexpr UInt32 kDxgiFormatBc1UnormSrgb = 72;
constexpr UInt32 kDxgiFormatBc2Unorm     = 74;
constexpr UInt32 kDxgiFormatBc2UnormSrgb = 75;
constexpr UInt32 kDxgiFormatBc3Unorm     = 77;
constexpr UInt32 kDxgiFormatBc3UnormSrgb = 78;
constexpr UInt32 kDxgiFormatBc4Unorm     = 80;
constexpr UInt32 kDxgiFormatBc4Snorm     = 81;
constexpr UInt32 kDxgiFormatBc5Unorm     = 83;
constexpr UInt32 kDxgiFormatBc5Snorm     = 84;
constexpr UInt32 kDxgiFormatBc6HUf16     = 95;
constexpr UInt32 kDxgiFormatBc6HSf16     = 96;
constexpr UInt32 kDxgiFormatBc7Unorm     = 98;
constexpr UInt32 kDxgiFormatBc7UnormSrgb = 99;

// ============================================================================
// 读取辅助
// ============================================================================

/// 从字节流读一个小端 UInt32
///
/// 调用方必须已保证 offset + 4 <= length —— Decode 一开始就核对了
/// 头部长度, 其后所有字段都落在这段之内。
LIMX_NODISCARD UInt32 ReadUInt32LittleEndian(const UInt8* data, SizeType offset)
{
    return static_cast<UInt32>(data[offset]) |
           (static_cast<UInt32>(data[offset + 1]) << 8) |
           (static_cast<UInt32>(data[offset + 2]) << 16) |
           (static_cast<UInt32>(data[offset + 3]) << 24);
}

/// 把 FourCC 转成四个可打印字符 — 不可打印的位置写 '?'
///
/// 错误信息里直接写十六进制没人认得; 写成 'DXT1' 一眼就知道是老式头。
void FourCcToString(UInt32 value, AnsiChar (&outText)[5])
{
    for (UInt32 i = 0; i < 4; ++i)
    {
        const UInt8 byte = static_cast<UInt8>((value >> (i * 8)) & 0xFFu);

        outText[i] = (byte >= 0x20u && byte < 0x7Fu)
                         ? static_cast<AnsiChar>(byte)
                         : '?';
    }

    outText[4] = '\0';
}

/// 下一级 mip 的尺寸 — 不小于 1
LIMX_NODISCARD UInt32 NextMipExtent(UInt32 extent)
{
    return (extent > 1u) ? (extent >> 1) : 1u;
}

/// 完整 mip 链的层数
///
/// 逐级折半直到 1x1, 即 floor(log2(max(w, h))) + 1。这里不复用 RHI 的
/// ComputeMipLevelCount —— 资产管线不依赖 RHI, 而这段循环比一条依赖便宜。
///
/// 前置条件: width > 0 且 height > 0。调用点在零尺寸检查之后, 因此这里
/// 不再重复一次判零 —— 重复的判零会让零尺寸检查变成一条**永远不会失败**
/// 的检查 (两道保险互相遮蔽), 那种检查删掉与否没人能察觉。
LIMX_NODISCARD UInt32 ComputeFullMipChainLength(UInt32 width, UInt32 height)
{
    UInt32 levels = 1;
    UInt32 w      = width;
    UInt32 h      = height;

    while (w > 1u || h > 1u)
    {
        w = NextMipExtent(w);
        h = NextMipExtent(h);
        ++levels;
    }

    return levels;
}

} // namespace

// ============================================================================
// DXGI 格式映射
// ============================================================================

EBlockCompressionFormat FDdsDecoder::MapDxgiFormat(UInt32 dxgiFormat)
{
    switch (dxgiFormat)
    {
        case kDxgiFormatBc1Unorm:     return EBlockCompressionFormat::BC1_UNORM;
        case kDxgiFormatBc1UnormSrgb: return EBlockCompressionFormat::BC1_SRGB;
        case kDxgiFormatBc2Unorm:     return EBlockCompressionFormat::BC2_UNORM;
        case kDxgiFormatBc2UnormSrgb: return EBlockCompressionFormat::BC2_SRGB;
        case kDxgiFormatBc3Unorm:     return EBlockCompressionFormat::BC3_UNORM;
        case kDxgiFormatBc3UnormSrgb: return EBlockCompressionFormat::BC3_SRGB;
        case kDxgiFormatBc4Unorm:     return EBlockCompressionFormat::BC4_UNORM;
        case kDxgiFormatBc4Snorm:     return EBlockCompressionFormat::BC4_SNORM;
        case kDxgiFormatBc5Unorm:     return EBlockCompressionFormat::BC5_UNORM;
        case kDxgiFormatBc5Snorm:     return EBlockCompressionFormat::BC5_SNORM;
        case kDxgiFormatBc6HUf16:     return EBlockCompressionFormat::BC6H_UFLOAT;
        case kDxgiFormatBc6HSf16:     return EBlockCompressionFormat::BC6H_SFLOAT;
        case kDxgiFormatBc7Unorm:     return EBlockCompressionFormat::BC7_UNORM;
        case kDxgiFormatBc7UnormSrgb: return EBlockCompressionFormat::BC7_SRGB;

        // TYPELESS 变体 (70/73/76/79/82/94/97) 与所有非块压缩格式都落到
        // 这里。绝不猜一个默认格式: 块大小相同的两个格式 (例如 BC2 与 BC3)
        // 长度校验完全一致, 猜错的结果是一张颜色全错却毫无报错的贴图。
        default:                      return EBlockCompressionFormat::Unknown;
    }
}

// ============================================================================
// 魔数判定
// ============================================================================

bool FDdsDecoder::IsDds(const UInt8* data, SizeType length)
{
    if (data == nullptr || length < 4)
    {
        return false;
    }

    return ReadUInt32LittleEndian(data, kOffsetMagic) == kDdsMagic;
}

// ============================================================================
// 解析
// ============================================================================

FImageDecodeResult FDdsDecoder::Decode(const UInt8* data, SizeType length,
                                       FCompressedImageData& outImage)
{
    outImage.Reset();

    if (data == nullptr)
    {
        return FImageDecodeResult::Failure(FString("DDS 数据为空"));
    }

    // ---- 1. 头部长度 ----
    if (length < kHeaderByteSize)
    {
        return FImageDecodeResult::Failure(StringFormat(
            "文件太小: {} 字节, DX10 DDS 的头部就要 {} 字节",
            length, kHeaderByteSize), length);
    }

    // ---- 2. 魔数 ----
    const UInt32 magic = ReadUInt32LittleEndian(data, kOffsetMagic);

    if (magic != kDdsMagic)
    {
        return FImageDecodeResult::Failure(StringFormat(
            "魔数不是 'DDS ' (期望 0x{}, 实际 0x{}) — 这不是一个 DDS 文件",
            FHex(kDdsMagic, 8), FHex(magic, 8)), kOffsetMagic);
    }

    // ---- 3. 结构版本 ----
    const UInt32 headerSize = ReadUInt32LittleEndian(data, kOffsetHeaderSize);

    if (headerSize != kDdsHeaderSize)
    {
        return FImageDecodeResult::Failure(StringFormat(
            "DDS_HEADER.dwSize 应为 {}, 实际 {} — 文件结构与规范不符",
            kDdsHeaderSize, headerSize), kOffsetHeaderSize);
    }

    const UInt32 pixelFormatSize =
        ReadUInt32LittleEndian(data, kOffsetPixelFormatSize);

    if (pixelFormatSize != kDdsPixelFormatSize)
    {
        return FImageDecodeResult::Failure(StringFormat(
            "DDS_PIXELFORMAT.dwSize 应为 {}, 实际 {}",
            kDdsPixelFormatSize, pixelFormatSize), kOffsetPixelFormatSize);
    }

    // ---- 4. 必须是 DX10 扩展头 ----
    const UInt32 pixelFormatFlags =
        ReadUInt32LittleEndian(data, kOffsetPixelFormatFlags);

    if ((pixelFormatFlags & kDdpfFourCc) == 0u)
    {
        return FImageDecodeResult::Failure(StringFormat(
            "DDS_PIXELFORMAT.dwFlags = 0x{} 没有 DDPF_FOURCC(0x4), "
            "这是一张未压缩纹理; 引擎的 DDS 路径只处理块压缩纹理",
            FHex(pixelFormatFlags, 8)), kOffsetPixelFormatFlags);
    }

    const UInt32 fourCc = ReadUInt32LittleEndian(data, kOffsetFourCc);

    if (fourCc != kFourCcDx10)
    {
        AnsiChar fourCcText[5];
        FourCcToString(fourCc, fourCcText);

        return FImageDecodeResult::Failure(StringFormat(
            "FourCC 是 '{}' 而不是 'DX10' — 老式 FourCC 头无法表达 sRGB, "
            "引擎不读它; 用 lat 重新烘焙或 texconv 转成 DX10 头",
            fourCcText), kOffsetFourCc);
    }

    // ---- 5. 只支持单张 2D 纹理 ----
    const UInt32 resourceDimension =
        ReadUInt32LittleEndian(data, kOffsetResourceDimension);

    if (resourceDimension != kResourceDimensionTexture2D)
    {
        return FImageDecodeResult::Failure(StringFormat(
            "resourceDimension = {} (期望 {} = TEXTURE2D); "
            "1D/3D 纹理不支持 — 按 2D 读会得到一张尺寸与内容都不对的图",
            resourceDimension, kResourceDimensionTexture2D),
            kOffsetResourceDimension);
    }

    const UInt32 arraySize = ReadUInt32LittleEndian(data, kOffsetArraySize);

    if (arraySize != 1u)
    {
        return FImageDecodeResult::Failure(StringFormat(
            "arraySize = {} (期望 1); 纹理数组不支持 — "
            "静默只读第一片会让显存统计与采样结果同时错掉",
            arraySize), kOffsetArraySize);
    }

    const UInt32 miscFlag = ReadUInt32LittleEndian(data, kOffsetMiscFlag);

    if ((miscFlag & kDdsResourceMiscTextureCube) != 0u)
    {
        return FImageDecodeResult::Failure(StringFormat(
            "miscFlag = 0x{} 声明了立方体贴图; 立方体贴图不支持 — "
            "它的 arraySize 同样是 1 而载荷是六份, 只读第一面不会有任何报错",
            FHex(miscFlag, 8)), kOffsetMiscFlag);
    }

    // ---- 6. 尺寸 ----
    const UInt32 height = ReadUInt32LittleEndian(data, kOffsetHeight);
    const UInt32 width  = ReadUInt32LittleEndian(data, kOffsetWidth);

    if (width == 0u || height == 0u)
    {
        return FImageDecodeResult::Failure(StringFormat(
            "尺寸非法: {}x{}", width, height), kOffsetHeight);
    }

    // ---- 7. DXGI 格式 ----
    const UInt32 dxgiFormat = ReadUInt32LittleEndian(data, kOffsetDxgiFormat);

    const EBlockCompressionFormat format = MapDxgiFormat(dxgiFormat);

    if (format == EBlockCompressionFormat::Unknown)
    {
        return FImageDecodeResult::Failure(StringFormat(
            "dxgiFormat = {} 不是引擎认识的块压缩格式; "
            "非块压缩的 DDS 请改用 PNG/JPEG, 或用 lat 烘成 BC 格式",
            dxgiFormat), kOffsetDxgiFormat);
    }

    // ---- 8. mip 层数 ----
    //
    // dwMipMapCount 为 0 的 DDS 在野外确实存在, 语义是"只有第 0 层"。
    const UInt32 declaredMipCount =
        ReadUInt32LittleEndian(data, kOffsetMipMapCount);

    const UInt32 mipCount = (declaredMipCount > 0u) ? declaredMipCount : 1u;

    const UInt32 maxMipCount = ComputeFullMipChainLength(width, height);

    if (mipCount > maxMipCount)
    {
        return FImageDecodeResult::Failure(StringFormat(
            "dwMipMapCount = {} 超过 {}x{} 的上限 {} 层",
            mipCount, width, height, maxMipCount), kOffsetMipMapCount);
    }

    // ---- 9. 逐层字节数与载荷对账 ----
    //
    // 这一步同时抓住"文件被截断"和"头里的尺寸与载荷不匹配"。少了它,
    // 后面的 mip 层会读到别的东西 —— 而只有远处的 mip 花掉, 极难定位。
    FCompressedImageData image;

    image.Width  = width;
    image.Height = height;
    image.Format = format;
    image.Levels.Reserve(mipCount);

    SizeType payloadBytes = 0;
    UInt32   levelWidth   = width;
    UInt32   levelHeight  = height;

    for (UInt32 level = 0; level < mipCount; ++level)
    {
        FCompressedMipLevel entry;

        entry.Width      = levelWidth;
        entry.Height     = levelHeight;
        entry.ByteOffset = payloadBytes;
        entry.ByteSize   =
            ComputeBlockCompressionLevelSize(format, levelWidth, levelHeight);

        payloadBytes += entry.ByteSize;

        image.Levels.Add(entry);

        levelWidth  = NextMipExtent(levelWidth);
        levelHeight = NextMipExtent(levelHeight);
    }

    const SizeType expectedTotal = kHeaderByteSize + payloadBytes;

    if (length < expectedTotal)
    {
        return FImageDecodeResult::Failure(StringFormat(
            "载荷被截断: 头部声明 {}x{} / {} 层 / {}, 需要 {} 字节, "
            "文件只有 {} 字节 (缺 {})",
            width, height, mipCount, GetBlockCompressionFormatName(format),
            expectedTotal, length, expectedTotal - length), length);
    }

    // ---- 10. dwPitchOrLinearSize 自洽 ----
    //
    // DDSD_LINEARSIZE 语义下它必须等于第 0 层大小。不一致说明写出方的
    // 块公式和这里的不一样 —— 正是要提前发现的那类分歧。
    const UInt32 flags      = ReadUInt32LittleEndian(data, kOffsetFlags);
    const UInt32 linearSize = ReadUInt32LittleEndian(data, kOffsetLinearSize);

    if ((flags & kDdsdLinearSize) != 0u &&
        static_cast<SizeType>(linearSize) != image.Levels[0].ByteSize)
    {
        return FImageDecodeResult::Failure(StringFormat(
            "dwPitchOrLinearSize = {} 与按块公式算出的第 0 层大小 {} 不符",
            linearSize, image.Levels[0].ByteSize), kOffsetLinearSize);
    }

    // ---- 11. 拷贝载荷 ----
    //
    // 只拷贝按块公式算出的那部分。尾部多余数据 (有些工具会塞元数据)
    // 不当作错误, 但也绝不混进纹理。
    image.Data.SetSize(payloadBytes);

    if (payloadBytes > 0)
    {
        Memory::MemCopy(image.Data.GetData(), data + kHeaderByteSize,
                        payloadBytes);
    }

    FImageDecodeResult result = FImageDecodeResult::Success();

    if (length > expectedTotal)
    {
        result.Warnings.Add(StringFormat(
            "文件尾部有 {} 字节多余数据 (期望 {} 字节, 实际 {} 字节), 已忽略",
            length - expectedTotal, expectedTotal, length));
    }

    // ---- 12. alpha 模式 ----
    //
    // 预乘 alpha 的数据需要在着色器里除回去, 引擎当前不做这件事。
    // 不拒绝加载 (颜色只是偏暗而非全错), 但必须让它可见。
    const UInt32 miscFlags2 = ReadUInt32LittleEndian(data, kOffsetMiscFlags2);

    if (miscFlags2 == kDdsAlphaModePremultiplied)
    {
        result.Warnings.Add(FString(
            "miscFlags2 声明为预乘 alpha, 引擎按直通 alpha 采样 — "
            "半透明区域会偏暗"));
    }

    outImage = MoveTemp(image);

    return result;
}

} // namespace Limx
