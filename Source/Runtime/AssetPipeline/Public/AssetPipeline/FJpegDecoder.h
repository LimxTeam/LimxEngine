/*******************************************************************************
 * 文件: FJpegDecoder.h
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   JPEG 基线解码器 — 支持 SOF0 顺序 DCT、灰度与 YCbCr、全部常见色度子采样
 *   与重启标记
 *
 * 设计哲学:
 *   只做基线, 但把基线做全 — 渐进式 (SOF2) 与算术编码在真实资产库中占比极低，
 *   而基线 JPEG 覆盖了几乎全部贴图导出路径。与其把精力摊薄到多种模式上，
 *   不如把基线的子采样、重启标记、字节填充这些容易踩空的细节做扎实。
 *   遇到不支持的模式明确报错，而不是产出一张花屏。
 *
 *   色度上采样按最近邻 — 双线性上采样在视觉上更平滑，但会让相邻块的色度
 *   互相渗透，在法线贴图这类非颜色数据上造成实际误差。贴图用途未知时，
 *   保真优先于平滑。
 *
 * 技术特性:
 *   - 支持 4:4:4 / 4:2:2 / 4:2:0 / 4:4:0 等任意整数采样因子组合
 *   - 熵解码处理 0xFF00 字节填充与 RSTn 重启标记
 *   - IDCT 采用可分离的行列两趟浮点变换, 预计算余弦基
 *   - 输出与 PNG 解码器共用 FImageData, 上层无须区分来源
 *
 * 依赖关系:
 *   内部: AssetPipeline/FImageTypes.h
 *
 * 注意事项:
 *   不支持渐进式 (SOF2)、无损、算术编码与 12 位精度 — 遇到时明确失败
 *   不解析 EXIF 方向标记 — 图像按存储顺序输出
 *
 ******************************************************************************/

#pragma once

#include "AssetPipeline/FImageTypes.h"

namespace Limx
{

// ============================================================================
// FJpegDecoder — JPEG 解码
// ============================================================================

/// JPEG 基线解码器 — 全静态接口
class LIMX_ASSETPIPELINE_API FJpegDecoder
{
public:
    FJpegDecoder()                               = delete;
    ~FJpegDecoder()                              = delete;
    FJpegDecoder(const FJpegDecoder&)            = delete;
    FJpegDecoder& operator=(const FJpegDecoder&) = delete;

    /// 判断字节流是否以 JPEG 的 SOI 标记开头
    LIMX_NODISCARD static bool IsJpeg(const UInt8* data, SizeType length);

    /// 解码 JPEG
    /// @param data     文件字节
    /// @param length   字节数
    /// @param outImage 解码结果 (调用前会被清空)
    /// @param options  解码选项
    LIMX_NODISCARD static FImageDecodeResult Decode(
        const UInt8* data, SizeType length,
        FImageData& outImage,
        const FImageDecodeOptions& options = FImageDecodeOptions());
};

} // namespace Limx
