/*******************************************************************************
 * 文件: FPngDecoder.h
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   PNG 解码器 — 支持全部五种颜色类型、1/2/4/8/16 位深、调色板、
 *   简单透明度 (tRNS) 与 Adam7 隔行
 *
 * 设计哲学:
 *   全格式覆盖而非常见格式覆盖 — 真实资产库里的 PNG 五花八门：
 *   带调色板的 UI 图标、16 位的高度图、隔行的网页遗留资源。
 *   只支持"常见的 RGBA8"意味着总有贴图加载不出来，而缺一张贴图
 *   在渲染结果里表现为一片纯色，排查成本远高于一次性做全。
 *
 *   CRC 只告警不拒绝 — 分块 CRC 不符说明文件曾被损坏，但 PNG 的
 *   分块结构使得单块损坏未必影响可解码性。拒绝加载会让一处磁盘坏道
 *   毁掉整个场景；记为告警并继续，让调用方自行决定。IDAT 的 zlib
 *   校验和另有把关，真正的数据损坏躲不过去。
 *
 * 技术特性:
 *   - 反滤波五种类型 (None/Sub/Up/Average/Paeth) 逐行处理
 *   - 低位深 (1/2/4) 按扫描线解包为每通道一字节
 *   - Adam7 七遍隔行按各遍独立反滤波后回填到最终图像
 *   - 多个 IDAT 分块先拼接再统一解压, 与规范一致
 *
 * 依赖关系:
 *   内部: AssetPipeline/FImageTypes.h, Core/Misc/FInflate.h,
 *          Core/Misc/FCrc32.h
 *
 * 注意事项:
 *   不支持 APNG 动画扩展 — 只解首帧
 *   16 位数据从文件的大端转为主机序; 选项开启时会降为 8 位
 *
 ******************************************************************************/

#pragma once

#include "AssetPipeline/FImageTypes.h"

namespace Limx
{

// ============================================================================
// FPngDecoder — PNG 解码
// ============================================================================

/// PNG 解码器 — 全静态接口
class LIMX_ASSETPIPELINE_API FPngDecoder
{
public:
    FPngDecoder()                              = delete;
    ~FPngDecoder()                             = delete;
    FPngDecoder(const FPngDecoder&)            = delete;
    FPngDecoder& operator=(const FPngDecoder&) = delete;

    /// 判断字节流是否以 PNG 签名开头
    LIMX_NODISCARD static bool IsPng(const UInt8* data, SizeType length);

    /// 解码 PNG
    /// @param data     文件字节
    /// @param length   字节数
    /// @param outImage 解码结果 (调用前会被清空)
    /// @param options  解码选项
    LIMX_NODISCARD static FImageDecodeResult Decode(
        const UInt8* data, SizeType length,
        FImageData& outImage,
        const FImageDecodeOptions& options = FImageDecodeOptions());

    /// 仅读取尺寸与格式, 不解码像素 — 用于资源清单与预算估算
    LIMX_NODISCARD static FImageDecodeResult ReadHeader(
        const UInt8* data, SizeType length,
        UInt32& outWidth, UInt32& outHeight,
        UInt32& outChannelCount, UInt32& outBitDepth);
};

} // namespace Limx
