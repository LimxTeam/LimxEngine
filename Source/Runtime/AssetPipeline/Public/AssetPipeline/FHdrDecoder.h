/*******************************************************************************
 * 文件: FHdrDecoder.h
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   Radiance HDR (.hdr / .pic) 解码器 — RGBE 共享指数格式，输出 32 位浮点
 *
 * 设计哲学:
 *   输出浮点而非归一化整数 — HDR 的意义就在于亮度可以远超 1.0（真实天空的
 *   太阳能到几千 nit 的相对值）。在解码阶段截断到 [0,1] 之后，辐照度卷积
 *   与镜面预滤波都失去依据：卷积出来的将是一张被削平的图，金属反射不出
 *   任何高光。
 *
 *   RGBE 是共享指数编码而非真正的浮点 — 三个通道共用一个 8 位指数，
 *   因此单个像素内通道间的动态范围有限（约 8 个数量级里的一段）。
 *   这是格式本身的取舍，解码器忠实还原即可，不做任何"修正"。
 *
 * 技术特性:
 *   - 新版自适应 RLE（逐通道游程）与旧版 RLE（重复上一像素）都支持
 *   - 扫描线方向由分辨率行决定，支持 -Y/+X 与 +Y/+X
 *   - 指数为 0 表示纯黑，不做 ldexp（避免非规格化数）
 *
 * 依赖关系:
 *   内部: AssetPipeline/FImageTypes.h
 *
 * 注意事项:
 *   不支持 XYZE 色彩空间（FORMAT=32-bit_rle_xyze）—— 明确报错而非按 RGBE 解
 *
 ******************************************************************************/

#pragma once

#include "AssetPipeline/FImageTypes.h"

namespace Limx
{

// ============================================================================
// FHdrDecoder — Radiance HDR 解码器
// ============================================================================

class LIMX_ASSETPIPELINE_API FHdrDecoder
{
public:
    FHdrDecoder()                              = delete;
    ~FHdrDecoder()                             = delete;
    FHdrDecoder(const FHdrDecoder&)            = delete;
    FHdrDecoder& operator=(const FHdrDecoder&) = delete;

    /// 是否为 Radiance HDR 文件
    ///
    /// 签名有两种历史写法: `#?RADIANCE` 与更早的 `#?RGBE`。
    LIMX_NODISCARD static bool IsHdr(const UInt8* data, SizeType length);

    /// 解码为 RGBA32F
    ///
    /// alpha 恒为 1.0 —— RGBE 格式本身不带 alpha, 补齐到四通道是为了
    /// 匹配 GPU: 三通道浮点格式在 Vulkan 上的支持并不普遍。
    LIMX_NODISCARD static FImageDecodeResult Decode(
        const UInt8* data, SizeType length,
        FImageData& outImage,
        const FImageDecodeOptions& options = FImageDecodeOptions());

    /// 把一个 RGBE 四元组转为线性浮点 RGB
    ///
    /// 公开出来供测试直接验证 —— 这是整个解码器里唯一的数值转换,
    /// 其余都是容器与游程解析。
    static void RgbeToLinear(UInt8 r, UInt8 g, UInt8 b, UInt8 e,
                             Float32& outR, Float32& outG, Float32& outB);
};

} // namespace Limx
