/*******************************************************************************
 * 文件: FDdsDecoder.h
 * 创建时间: 2026-08-30
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   DDS (DirectDraw Surface) 容器读取器 — 只处理 DX10 扩展头的块压缩纹理，
 *   产出带完整 mip 链的、可直接上传的压缩载荷
 *
 * 设计哲学:
 *   不解压 — 块压缩纹理存在的意义就是让 GPU 直接采样它。CPU 侧任何
 *   "解成 RGBA8 再交给上传层"的做法会同时丢掉三样东西: 显存占用的优势、
 *   离线烘出来的高质量 mip 链、以及一次 memcpy 就能上传的路径。因此本
 *   解码器与 FPngDecoder/FHdrDecoder 的产物不同: 它不产 FImageData，
 *   而产 FCompressedImageData —— 一段原样保留的块数据加一张分层索引表。
 *
 *   只认 DX10 头 — 老式 FourCC 头 ('DXT1'/'ATI2') 没有地方表达 sRGB。
 *   同一份 BC1 字节是 UNORM 还是 UNORM_SRGB, 在 FourCC 头里无法区分,
 *   而猜错的后果 (整体偏亮或偏暗) 不会有任何报错。写出侧 (Programs/lat)
 *   固定写 DX10 头, 读取侧就固定只认它 —— 两边的约定必须一样窄。
 *
 *   宁可拒绝也不猜 — 纹理数组、立方体贴图、1D/3D 纹理、认不出来的 DXGI
 *   格式码, 全部明确报错。"按 2D 读第一片"这类静默降级会让显存统计、
 *   mip 层数、采样结果同时错掉, 而画面上只表现为"这张贴图有点怪"。
 *
 * 技术特性:
 *   - 逐层字节数按 ceil(w/4) * ceil(h/4) * 每块字节 计算; 非 4 倍数尺寸下
 *     少算一个块会让其后所有 mip 整体错位, 因此向上取整是硬性的
 *   - 声明的 mip 层数与实际载荷长度逐字节对账, 截断即失败
 *   - 载荷紧凑排布, 第 i 层的偏移天然是块大小 (8/16 字节) 的整数倍,
 *     满足 Vulkan 对 vkCmdCopyBufferToImage 的 bufferOffset 对齐要求
 *   - 解析复杂度 O(mip 层数), 与像素数无关; 拷贝复杂度 O(载荷字节数)
 *
 * 依赖关系:
 *   内部: AssetPipeline/FImageTypes.h (复用 FImageDecodeResult),
 *          Core/HAL/FPlatformFile.h (仅实现中)
 *
 * 注意事项:
 *   产出的格式枚举是与图形 API 无关的 EBlockCompressionFormat ——
 *   到 RHI 的 EPixelFormat 的映射由 RenderCore 完成, 与既有的
 *   EImageFormat → EPixelFormat 走同一条路。AssetPipeline 不依赖 RHI。
 *
 *   不支持体积纹理、纹理数组与立方体贴图 — 遇到时报错而非只读第一片
 *
 ******************************************************************************/

#pragma once

#include "AssetPipeline/FImageTypes.h"

namespace Limx
{

// ============================================================================
// EBlockCompressionFormat — 块压缩像素格式
// ============================================================================

/// 块压缩格式
///
/// 与 RHI 的 EPixelFormat 中的 BC 项一一对应, 但刻意不复用后者:
/// 资产管线是"与图形 API 无关的中性数据"层, 不依赖 RHI。映射由
/// RenderCore 负责, 与既有的 EImageFormat → EPixelFormat 同一路径。
///
/// sRGB 与否是格式的一部分而非旁路的布尔量 —— 同一份 BC1 字节按两种
/// 方式解释得到的亮度完全不同, 把它拆成"格式 + 色彩空间标志"就给了
/// 调用方一个可以填错且不会报错的位置。
enum class EBlockCompressionFormat : UInt8
{
    Unknown = 0,

    /// RGB(+1bit A), 8 字节/块
    BC1_UNORM = 1,
    BC1_SRGB  = 2,

    /// RGBA (显式 4 位 alpha), 16 字节/块
    BC2_UNORM = 3,
    BC2_SRGB  = 4,

    /// RGBA (插值 alpha), 16 字节/块
    BC3_UNORM = 5,
    BC3_SRGB  = 6,

    /// 单通道, 8 字节/块
    BC4_UNORM = 7,
    BC4_SNORM = 8,

    /// 双通道, 16 字节/块 —— 切线空间法线的 XY, Z 由着色器重建
    BC5_UNORM = 9,
    BC5_SNORM = 10,

    /// HDR 三通道, 16 字节/块
    BC6H_UFLOAT = 11,
    BC6H_SFLOAT = 12,

    /// 高质量 RGBA, 16 字节/块
    BC7_UNORM = 13,
    BC7_SRGB  = 14,
};

/// 该格式每个 4x4 块占多少字节 — 未知格式返回 0
///
/// BC1/BC4 是 8 字节 (端点 + 每像素 2/3 bit 索引), 其余都是 16 字节。
/// 这个数值同时决定逐层字节数与缓冲区偏移的对齐粒度, 错一位会让
/// 整条 mip 链错位。
LIMX_NODISCARD inline constexpr UInt32 GetBlockCompressionBlockByteSize(
    EBlockCompressionFormat format)
{
    switch (format)
    {
        case EBlockCompressionFormat::BC1_UNORM:
        case EBlockCompressionFormat::BC1_SRGB:
        case EBlockCompressionFormat::BC4_UNORM:
        case EBlockCompressionFormat::BC4_SNORM:
            return 8;

        case EBlockCompressionFormat::BC2_UNORM:
        case EBlockCompressionFormat::BC2_SRGB:
        case EBlockCompressionFormat::BC3_UNORM:
        case EBlockCompressionFormat::BC3_SRGB:
        case EBlockCompressionFormat::BC5_UNORM:
        case EBlockCompressionFormat::BC5_SNORM:
        case EBlockCompressionFormat::BC6H_UFLOAT:
        case EBlockCompressionFormat::BC6H_SFLOAT:
        case EBlockCompressionFormat::BC7_UNORM:
        case EBlockCompressionFormat::BC7_SRGB:
            return 16;

        case EBlockCompressionFormat::Unknown:
        default:
            return 0;
    }
}

/// 一个 mip 层的字节数
///
/// **必须向上取整**: BC 的最小单位是 4x4 块而非像素, 一张 1x1 的 mip 层
/// 仍然占满一个完整的块。写成 (w / 4) * (h / 4) 在任何非 4 倍数尺寸下都
/// 会少算一整排块, 而其后每一层的偏移都会跟着错位 —— 表现是远处的 mip
/// 变成花屏, 且只在特定尺寸的贴图上出现。
LIMX_NODISCARD inline constexpr SizeType ComputeBlockCompressionLevelSize(
    EBlockCompressionFormat format, UInt32 width, UInt32 height)
{
    const UInt32 blockBytes = GetBlockCompressionBlockByteSize(format);

    if (blockBytes == 0 || width == 0 || height == 0)
    {
        return 0;
    }

    const SizeType blocksX = (static_cast<SizeType>(width) + 3u) / 4u;
    const SizeType blocksY = (static_cast<SizeType>(height) + 3u) / 4u;

    return blocksX * blocksY * static_cast<SizeType>(blockBytes);
}

/// 该格式是否声明为 sRGB
LIMX_NODISCARD inline constexpr bool IsBlockCompressionSrgb(
    EBlockCompressionFormat format)
{
    return format == EBlockCompressionFormat::BC1_SRGB ||
           format == EBlockCompressionFormat::BC2_SRGB ||
           format == EBlockCompressionFormat::BC3_SRGB ||
           format == EBlockCompressionFormat::BC7_SRGB;
}

/// 该格式是否只携带两个通道 (BC5)
///
/// 法线贴图用 BC5 存 XY 时, Z 在文件里根本不存在, 采样得到的第三个分量
/// 恒为 0。着色器必须用 z = sqrt(1 - x² - y²) 重建, 而这件事只能对 BC5
/// 做 —— 普通 RGB 法线图的 Z 是真实存储的, 重建会覆盖掉作者的意图。
/// 判据放在这里, 使"哪些格式需要重建"只有一处定义。
LIMX_NODISCARD inline constexpr bool IsBlockCompressionTwoChannel(
    EBlockCompressionFormat format)
{
    return format == EBlockCompressionFormat::BC5_UNORM ||
           format == EBlockCompressionFormat::BC5_SNORM;
}

/// 格式名 — 用于日志与错误信息
///
/// 名字与 DXGI 的写法一致 (BC1_UNORM_SRGB 而非 BC1_SRGB), 这样引擎日志
/// 里的字符串可以直接拿去和 `lat inspect` 的输出对照。
LIMX_NODISCARD inline constexpr const AnsiChar* GetBlockCompressionFormatName(
    EBlockCompressionFormat format)
{
    switch (format)
    {
        case EBlockCompressionFormat::BC1_UNORM:   return "BC1_UNORM";
        case EBlockCompressionFormat::BC1_SRGB:    return "BC1_UNORM_SRGB";
        case EBlockCompressionFormat::BC2_UNORM:   return "BC2_UNORM";
        case EBlockCompressionFormat::BC2_SRGB:    return "BC2_UNORM_SRGB";
        case EBlockCompressionFormat::BC3_UNORM:   return "BC3_UNORM";
        case EBlockCompressionFormat::BC3_SRGB:    return "BC3_UNORM_SRGB";
        case EBlockCompressionFormat::BC4_UNORM:   return "BC4_UNORM";
        case EBlockCompressionFormat::BC4_SNORM:   return "BC4_SNORM";
        case EBlockCompressionFormat::BC5_UNORM:   return "BC5_UNORM";
        case EBlockCompressionFormat::BC5_SNORM:   return "BC5_SNORM";
        case EBlockCompressionFormat::BC6H_UFLOAT: return "BC6H_UF16";
        case EBlockCompressionFormat::BC6H_SFLOAT: return "BC6H_SF16";
        case EBlockCompressionFormat::BC7_UNORM:   return "BC7_UNORM";
        case EBlockCompressionFormat::BC7_SRGB:    return "BC7_UNORM_SRGB";
        case EBlockCompressionFormat::Unknown:
        default:                                   return "Unknown";
    }
}

// ============================================================================
// FCompressedMipLevel — 压缩纹理的一层
// ============================================================================

/// 压缩纹理 mip 链中的一层
///
/// 偏移与长度都记下来而非只记长度: 上传路径要逐层发一条
/// CopyBufferToTexture, 每条都需要一个绝对偏移。让消费方自己做前缀和
/// 意味着"累加时漏了一层"这类错误要到 GPU 上才暴露。
struct FCompressedMipLevel
{
    /// 该层的像素宽度 (非块数)
    UInt32 Width = 0;

    /// 该层的像素高度 (非块数)
    UInt32 Height = 0;

    /// 该层在载荷中的起始字节偏移
    SizeType ByteOffset = 0;

    /// 该层的字节数
    SizeType ByteSize = 0;
};

// ============================================================================
// FCompressedImageData — 解析后的块压缩纹理
// ============================================================================

/// 块压缩纹理 — 原样保留的块数据 + 分层索引
///
/// 与 FImageData 并列而非继承: 两者的不变量完全不同 (前者按块、含 mip 链、
/// 无"每像素字节数"可言), 混在一个结构里只会让每个字段都要先问一句
/// "这是压缩的还是没压缩的"。
struct FCompressedImageData
{
    FName Name;

    /// 第 0 层的像素宽度
    UInt32 Width = 0;

    /// 第 0 层的像素高度
    UInt32 Height = 0;

    EBlockCompressionFormat Format = EBlockCompressionFormat::Unknown;

    /// 逐层索引, 元素个数即 mip 层数
    TArray<FCompressedMipLevel> Levels;

    /// 全部 mip 层的块数据, 按层紧凑排布
    TArray<UInt8> Data;

    LIMX_NODISCARD UInt32 GetMipLevelCount() const
    {
        return static_cast<UInt32>(Levels.GetSize());
    }

    LIMX_NODISCARD bool IsSrgb() const
    {
        return IsBlockCompressionSrgb(Format);
    }

    /// 是否持有有效且自洽的纹理
    ///
    /// 自洽的含义: 每一层的偏移与长度都落在载荷内、首尾相接、且长度与
    /// 该层尺寸按块公式算出来的值相符。这三条任何一条不成立, 上传出去
    /// 都会是错位的图像而不是报错。
    LIMX_NODISCARD bool IsValid() const
    {
        if (Width == 0 || Height == 0 ||
            Format == EBlockCompressionFormat::Unknown ||
            Levels.GetSize() == 0)
        {
            return false;
        }

        SizeType expectedOffset = 0;

        for (SizeType i = 0; i < Levels.GetSize(); ++i)
        {
            const FCompressedMipLevel& level = Levels[i];

            if (level.ByteOffset != expectedOffset)
            {
                return false;
            }

            if (level.ByteSize !=
                ComputeBlockCompressionLevelSize(Format, level.Width,
                                                 level.Height))
            {
                return false;
            }

            expectedOffset += level.ByteSize;
        }

        return expectedOffset == Data.GetSize();
    }

    void Reset()
    {
        Name   = FName();
        Width  = 0;
        Height = 0;
        Format = EBlockCompressionFormat::Unknown;
        Levels.Clear();
        Data.Clear();
    }
};

// ============================================================================
// FDdsDecoder — DDS 容器读取
// ============================================================================

/// DDS 读取器 — 全静态接口
///
/// 与写出侧 Programs/lat/src/dds.rs 严格对偶: 那边写的每一个字段,
/// 这边都读回来核对一遍。两边任何一侧改了布局, 往返测试立刻变红。
class LIMX_ASSETPIPELINE_API FDdsDecoder
{
public:
    FDdsDecoder()                              = delete;
    ~FDdsDecoder()                             = delete;
    FDdsDecoder(const FDdsDecoder&)            = delete;
    FDdsDecoder& operator=(const FDdsDecoder&) = delete;

    /// DX10 DDS 的头部总字节数: 4 (魔数) + 124 (DDS_HEADER) + 20 (DXT10)
    static constexpr SizeType kHeaderByteSize = 148;

    /// 判断字节流是否以 DDS 魔数开头
    ///
    /// 只看魔数 —— 是不是我们能读的那一类 DDS 由 Decode 判定并给出
    /// 具体原因。若这里就把"老式 FourCC 头"排除掉, 调用方拿到的会是
    /// "无法识别的图像格式", 那句话对排查毫无帮助。
    LIMX_NODISCARD static bool IsDds(const UInt8* data, SizeType length);

    /// 解析 DDS
    ///
    /// @param data     文件字节
    /// @param length   字节数
    /// @param outImage 解析结果 (调用前会被清空)
    ///
    /// 失败时 outImage 保持清空状态, 返回值携带具体原因与出错偏移。
    LIMX_NODISCARD static FImageDecodeResult Decode(
        const UInt8* data, SizeType length,
        FCompressedImageData& outImage);

    /// DXGI 格式码 → 块压缩格式
    ///
    /// 认不出来返回 Unknown, 由调用方报错。**绝不退化成某个默认格式** ——
    /// 把一份 BC7 数据当成 BC1 读, 长度对不上会被截断检查拦住; 但把
    /// BC3 当成 BC2 读长度完全一致, 出来的是一张颜色全错却毫无报错的图。
    LIMX_NODISCARD static EBlockCompressionFormat MapDxgiFormat(
        UInt32 dxgiFormat);
};

} // namespace Limx
