/*******************************************************************************
 * 文件: FImageDecoder.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   图像解码统一入口实现 — 魔数判定与分发
 *
 * 设计哲学:
 *   诊断信息要足够定位 — 未知格式时把前几个字节一并报出。
 *   "无法识别的图像格式"这句话对排查毫无帮助, 而看到实际字节
 *   往往一眼就能认出是文本文件、LFS 指针还是别的容器。
 *
 * 依赖关系:
 *   内部: AssetPipeline/FImageDecoder.h, Core/HAL/FPlatformFile.h
 *
 ******************************************************************************/

#include "AssetPipeline/FImageDecoder.h"
#include "Core/HAL/FPlatformFile.h"

namespace Limx
{

EImageFileFormat FImageDecoder::DetectFormat(const UInt8* data, SizeType length)
{
    if (FPngDecoder::IsPng(data, length))
    {
        return EImageFileFormat::Png;
    }

    if (FJpegDecoder::IsJpeg(data, length))
    {
        return EImageFileFormat::Jpeg;
    }

    if (FHdrDecoder::IsHdr(data, length))
    {
        return EImageFileFormat::Hdr;
    }

    if (FDdsDecoder::IsDds(data, length))
    {
        return EImageFileFormat::Dds;
    }

    return EImageFileFormat::Unknown;
}

FImageDecodeResult FImageDecoder::Decode(const UInt8* data, SizeType length,
                                         FImageData& outImage,
                                         const FImageDecodeOptions& options)
{
    switch (DetectFormat(data, length))
    {
        case EImageFileFormat::Png:
            return FPngDecoder::Decode(data, length, outImage, options);

        case EImageFileFormat::Jpeg:
            return FJpegDecoder::Decode(data, length, outImage, options);

        case EImageFileFormat::Hdr:
            return FHdrDecoder::Decode(data, length, outImage, options);

        case EImageFileFormat::Dds:
            // 块压缩纹理不解压 —— 它的产物是 FCompressedImageData 而非
            // FImageData。这里报一句指路的错, 比让调用方看到
            // "无法识别的图像格式" 要有用得多。
            outImage.Reset();
            return FImageDecodeResult::Failure(FString(
                "这是一张 DDS 块压缩纹理, 不经 CPU 解码 — "
                "请改用 FDdsDecoder::Decode 走逐 mip 直传路径"));

        default:
            break;
    }

    outImage.Reset();

    // 报出前几字节 —— 常能一眼认出是文本、LFS 指针还是别的容器
    if (data != nullptr && length >= 4)
    {
        return FImageDecodeResult::Failure(StringFormat(
            "无法识别的图像格式, 起始字节: {} {} {} {}",
            FHex(data[0], 2), FHex(data[1], 2),
            FHex(data[2], 2), FHex(data[3], 2)));
    }

    return FImageDecodeResult::Failure(FString("图像数据过短或为空"));
}

FImageDecodeResult FImageDecoder::DecodeFile(const FString& path,
                                             FImageData& outImage,
                                             const FImageDecodeOptions& options)
{
    const TArray<UInt8> bytes = FPlatformFile::ReadAllBytes(path);

    if (bytes.GetSize() == 0)
    {
        outImage.Reset();
        return FImageDecodeResult::Failure(
            StringFormat("无法读取图像文件: {}", path.GetCStr()));
    }

    FImageDecodeResult result = Decode(bytes.GetData(), bytes.GetSize(),
                                       outImage, options);

    if (result.Succeeded)
    {
        outImage.Name = FName(path.GetCStr());
    }

    return result;
}

} // namespace Limx
