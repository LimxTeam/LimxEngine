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
 *   - 支持 PNG / JPEG / Radiance HDR; 未知格式明确报错并给出前几字节
 *   - 识别 DDS 但不在此解码 — 它走 FDdsDecoder 的直传路径
 *   - 提供从文件路径加载的便捷入口
 *
 * 依赖关系:
 *   内部: AssetPipeline/FPngDecoder.h, AssetPipeline/FJpegDecoder.h,
 *          AssetPipeline/FHdrDecoder.h, AssetPipeline/FDdsDecoder.h
 *
 * 注意事项:
 *   DDS 块压缩纹理无需 CPU 解码, 产物也不是 FImageData —— 本接口能
 *   *识别* 它并给出指路的错误信息, 但不解码。调用方应先用
 *   FImageDecoder::DetectFormat 分流, DDS 交给 FDdsDecoder。
 *
 *   KTX2 容器尚未支持
 *
 ******************************************************************************/

#pragma once

#include "AssetPipeline/FPngDecoder.h"
#include "AssetPipeline/FHdrDecoder.h"
#include "AssetPipeline/FJpegDecoder.h"
#include "AssetPipeline/FDdsDecoder.h"

namespace Limx
{

/// 图像文件格式
enum class EImageFileFormat : UInt8
{
    Unknown = 0,
    Png     = 1,
    Jpeg    = 2,
    Hdr     = 3,

    /// DDS 块压缩容器 — 由 FDdsDecoder 解析, 本接口只负责识别
    Dds     = 4,
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
