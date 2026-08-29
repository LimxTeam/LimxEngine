/*******************************************************************************
 * 文件: FImageDecoder.h
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   图像解码统一入口 — 按文件魔数分发到具体解码器
 *
 * 设计哲学:
 *   按魔数而非扩展名分发 — 资产库里的文件扩展名经常与实际格式不符
 *   (被批量改名的 .png 其实是 JPEG, 从网页保存的 .jpg 其实是 PNG)。
 *   魔数来自文件内容本身, 不会说谎。扩展名只在魔数无法判定时作为线索。
 *
 *   调用方不该关心格式 — 上传层拿到的永远是 FImageData, 无论它来自
 *   PNG 还是 JPEG。新增格式只需在此处加一条分发规则。
 *
 * 技术特性:
 *   - 支持 PNG 与 JPEG; 未知格式明确报错并给出前几字节便于诊断
 *   - 提供从文件路径加载的便捷入口
 *
 * 依赖关系:
 *   内部: AssetPipeline/FPngDecoder.h, AssetPipeline/FJpegDecoder.h
 *
 * 注意事项:
 *   DDS / KTX2 等 GPU 压缩容器尚未支持 — 它们无需 CPU 解码,
 *   应走独立的直传路径而非本接口
 *
 ******************************************************************************/

#pragma once

#include "AssetPipeline/FPngDecoder.h"
#include "AssetPipeline/FJpegDecoder.h"

namespace Limx
{

/// 图像文件格式
enum class EImageFileFormat : UInt8
{
    Unknown = 0,
    Png     = 1,
    Jpeg    = 2,
};

/// 图像解码统一入口 — 全静态接口
class LIMX_ASSETPIPELINE_API FImageDecoder
{
public:
    FImageDecoder()                                = delete;
    ~FImageDecoder()                               = delete;
    FImageDecoder(const FImageDecoder&)            = delete;
    FImageDecoder& operator=(const FImageDecoder&) = delete;

    /// 按魔数判定文件格式
    LIMX_NODISCARD static EImageFileFormat DetectFormat(const UInt8* data,
                                                        SizeType length);

    /// 解码任意受支持格式的图像
    LIMX_NODISCARD static FImageDecodeResult Decode(
        const UInt8* data, SizeType length,
        FImageData& outImage,
        const FImageDecodeOptions& options = FImageDecodeOptions());

    /// 从文件加载并解码
    LIMX_NODISCARD static FImageDecodeResult DecodeFile(
        const FString& path,
        FImageData& outImage,
        const FImageDecodeOptions& options = FImageDecodeOptions());
};

} // namespace Limx
