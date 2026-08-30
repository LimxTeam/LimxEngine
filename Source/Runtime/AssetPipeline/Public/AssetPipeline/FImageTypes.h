/*******************************************************************************
 * 文件: FImageTypes.h
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   图像中性数据结构 — 解码后的像素、格式描述与色彩空间提示
 *   所有图像解码器 (PNG/JPEG/DDS) 都产出这一套结构
 *
 * 设计哲学:
 *   格式描述与像素分离 — 像素只是一段字节，如何解释它取决于格式、通道数与
 *   色彩空间。把这三者显式记录，上传层就能直接映射到 Vulkan 格式而不必猜测；
 *   猜错的后果是贴图偏色或整体发暗，且在截图上难以判定。
 *
 *   色彩空间是提示而非事实 — PNG 的 sRGB/gAMA 块、JPEG 的 JFIF 标记都只是
 *   声明，实际数据未必符合。此处如实记录来源的声明，由材质层结合用途决定
 *   最终按 sRGB 还是线性采样（基色贴图按 sRGB，法线与粗糙度贴图按线性）。
 *
 * 技术特性:
 *   - 支持 8 位与 16 位每通道, 1..4 通道
 *   - 行间无填充: 行距恒等于 宽度 × 每像素字节数
 *   - 提供尺寸与像素数的一致性自检
 *
 * 依赖关系:
 *   内部: Core/CoreMinimal.h, Core/Containers/TArray.h, FName.h
 *
 * 注意事项:
 *   不含 mip 链 — mip 生成属于上传阶段的职责
 *   16 位数据按主机字节序存放, 解码器负责从文件的大端转换
 *
 ******************************************************************************/

#pragma once

#include "Core/CoreMinimal.h"
#include "Core/Containers/TArray.h"
#include "Core/Containers/FName.h"
#include "Core/Containers/FString.h"
#include "AssetPipeline/AssetPipelineAPI.h"

namespace Limx
{

// ============================================================================
// EImageFormat — 解码后的像素格式
// ============================================================================

/// 图像像素格式
enum class EImageFormat : UInt8
{
    Unknown = 0,

    /// 单通道 8 位
    R8 = 1,

    /// 双通道 8 位
    RG8 = 2,

    /// 三通道 8 位
    RGB8 = 3,

    /// 四通道 8 位
    RGBA8 = 4,

    /// 单通道 16 位
    R16 = 5,

    /// 双通道 16 位
    RG16 = 6,

    /// 三通道 16 位
    RGB16 = 7,

    /// 四通道 16 位
    RGBA16 = 8,

    /// 三通道 32 位浮点
    RGB32F = 9,

    /// 四通道 32 位浮点
    ///
    /// HDR 环境贴图解码后的格式。用浮点而非归一化整数是必需的 ——
    /// HDR 的意义就在于亮度可以远超 1.0 (太阳能到几千), 归一化格式
    /// 在解码阶段就把这部分截断了, 之后的辐照度卷积也就无从谈起。
    RGBA32F = 10,
};

/// 该格式的通道数
LIMX_NODISCARD inline UInt32 GetImageChannelCount(EImageFormat format)
{
    switch (format)
    {
        case EImageFormat::R8:
        case EImageFormat::R16:    return 1;
        case EImageFormat::RG8:
        case EImageFormat::RG16:   return 2;
        case EImageFormat::RGB8:
        case EImageFormat::RGB16:
        case EImageFormat::RGB32F: return 3;
        case EImageFormat::RGBA8:
        case EImageFormat::RGBA16:
        case EImageFormat::RGBA32F: return 4;
        default:                   return 0;
    }
}

/// 该格式是否为浮点
///
/// 单独判定而非靠"每通道 4 字节"推断: 将来若加入 32 位整数格式,
/// 字节数相同但语义完全不同, 靠字节数推断会静默走错分支。
LIMX_NODISCARD inline bool IsImageFormatFloat(EImageFormat format)
{
    return format == EImageFormat::RGB32F || format == EImageFormat::RGBA32F;
}

/// 该格式每通道的字节数
LIMX_NODISCARD inline UInt32 GetImageBytesPerChannel(EImageFormat format)
{
    switch (format)
    {
        case EImageFormat::R8:
        case EImageFormat::RG8:
        case EImageFormat::RGB8:
        case EImageFormat::RGBA8:  return 1;
        case EImageFormat::R16:
        case EImageFormat::RG16:
        case EImageFormat::RGB16:
        case EImageFormat::RGBA16: return 2;
        case EImageFormat::RGB32F:
        case EImageFormat::RGBA32F: return 4;
        default:                   return 0;
    }
}

/// 由通道数与位深求格式 — 组合非法时返回 Unknown
LIMX_NODISCARD inline EImageFormat MakeImageFormat(UInt32 channelCount,
                                                   UInt32 bytesPerChannel)
{
    if (bytesPerChannel == 1)
    {
        switch (channelCount)
        {
            case 1: return EImageFormat::R8;
            case 2: return EImageFormat::RG8;
            case 3: return EImageFormat::RGB8;
            case 4: return EImageFormat::RGBA8;
            default: break;
        }
    }
    else if (bytesPerChannel == 2)
    {
        switch (channelCount)
        {
            case 1: return EImageFormat::R16;
            case 2: return EImageFormat::RG16;
            case 3: return EImageFormat::RGB16;
            case 4: return EImageFormat::RGBA16;
            default: break;
        }
    }

    return EImageFormat::Unknown;
}

// ============================================================================
// EImageColorSpace — 色彩空间提示
// ============================================================================

/// 色彩空间提示
///
/// 来自文件的声明而非对像素的检测。基色与自发光贴图通常是 sRGB，
/// 法线、粗糙度、金属度、遮蔽贴图必须按线性解释 —— 用途由材质层决定，
/// 此处只如实转达文件说了什么。
enum class EImageColorSpace : UInt8
{
    /// 文件未声明
    Unspecified = 0,

    /// 文件声明为 sRGB
    Srgb = 1,

    /// 文件声明为线性
    Linear = 2,
};

// ============================================================================
// FImageData — 解码后的图像
// ============================================================================

/// 解码后的图像数据
struct FImageData
{
    FName Name;

    /// 像素宽度
    UInt32 Width = 0;

    /// 像素高度
    UInt32 Height = 0;

    /// 像素格式
    EImageFormat Format = EImageFormat::Unknown;

    /// 色彩空间提示
    EImageColorSpace ColorSpace = EImageColorSpace::Unspecified;

    /// 源文件是否含 alpha 通道
    ///
    /// 与格式的通道数不同: 解码器可能把 RGB 扩展为 RGBA 以适配 GPU，
    /// 此时格式有 4 通道但源文件并无 alpha。合批与排序需要区分这两者。
    bool HasSourceAlpha = false;

    /// 像素字节 — 行间无填充, 自上而下
    TArray<UInt8> Pixels;

    LIMX_NODISCARD UInt32 GetChannelCount() const
    {
        return GetImageChannelCount(Format);
    }

    LIMX_NODISCARD UInt32 GetBytesPerChannel() const
    {
        return GetImageBytesPerChannel(Format);
    }

    LIMX_NODISCARD UInt32 GetBytesPerPixel() const
    {
        return GetChannelCount() * GetBytesPerChannel();
    }

    /// 每行字节数 — 无填充
    LIMX_NODISCARD SizeType GetRowPitch() const
    {
        return static_cast<SizeType>(Width) * GetBytesPerPixel();
    }

    /// 像素数据应有的总字节数
    LIMX_NODISCARD SizeType GetExpectedByteSize() const
    {
        return GetRowPitch() * Height;
    }

    /// 是否持有有效且自洽的图像
    LIMX_NODISCARD bool IsValid() const
    {
        return Width > 0 && Height > 0 &&
               Format != EImageFormat::Unknown &&
               Pixels.GetSize() == GetExpectedByteSize();
    }

    /// 清空
    void Reset()
    {
        Name       = FName();
        Width      = 0;
        Height     = 0;
        Format     = EImageFormat::Unknown;
        ColorSpace = EImageColorSpace::Unspecified;
        HasSourceAlpha = false;
        Pixels.Clear();
    }
};

// ============================================================================
// FImageDecodeOptions — 解码选项
// ============================================================================

/// 图像解码选项
struct FImageDecodeOptions
{
    /// 是否把结果统一扩展为四通道
    ///
    /// 默认开启: Vulkan 对三通道格式 (R8G8B8_UNORM) 的支持并不普遍，
    /// 而四通道格式是所有实现都必须支持的。在解码阶段补齐 alpha
    /// 比让上传层为每种通道数分支要简单得多。
    bool ForceFourChannels = true;

    /// 是否把 16 位每通道降为 8 位
    ///
    /// 默认开启: 16 位纹理显存占用翻倍，而绝大多数材质贴图用不到该精度。
    /// 需要保留时 (高度图、位移图) 显式关闭。
    bool ReduceSixteenBitToEight = true;

    /// 是否垂直翻转
    bool FlipVertically = false;
};

// ============================================================================
// FImageDecodeResult — 解码结果
// ============================================================================

/// 解码结果 — 失败时携带原因
struct FImageDecodeResult
{
    bool Succeeded = false;

    FString ErrorMessage;

    /// 出错处的字节偏移, 0 表示不适用
    SizeType ErrorOffset = 0;

    TArray<FString> Warnings;

    LIMX_NODISCARD static FImageDecodeResult Success()
    {
        FImageDecodeResult result;
        result.Succeeded = true;
        return result;
    }

    LIMX_NODISCARD static FImageDecodeResult Failure(const FString& message,
                                                     SizeType offset = 0)
    {
        FImageDecodeResult result;
        result.Succeeded    = false;
        result.ErrorMessage = message;
        result.ErrorOffset  = offset;
        return result;
    }
};

} // namespace Limx
