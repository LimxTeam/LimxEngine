/*******************************************************************************
 * 文件: CompressedTextureFormatTests.cpp
 * 创建时间: 2026-08-30
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   块压缩格式从资产管线到 RHI 的第二跳映射测试 ——
 *   EBlockCompressionFormat → EPixelFormat
 *
 * 设计哲学:
 *   这一跳是整条 DDS 路径上唯一没有长度校验兜底的地方。
 *
 *   第一跳 (DXGI 码 → EBlockCompressionFormat) 由 AssetTests 覆盖, 而且
 *   映射错了往往会被逐层字节数对账拦下 —— 块大小不同的两个格式长度算出来
 *   就不一样。第二跳没有这层保护: EPixelFormat 只在建纹理时用一次, 映射
 *   错了 GPU 照样能建出一张同样大小的图, 采出来的颜色却是按别的格式解读的。
 *   BC1_UNORM 与 BC1_SRGB 尤其危险 —— 两者字节完全相同, 唯一的差别是硬件
 *   要不要做电光转换, 错了只表现为"这张贴图偏亮/偏暗"。
 *
 *   因此这里逐项核对而不是抽查。表里少一项、错一项, 都会让某一类烘好的
 *   纹理在引擎里变成一张没有任何报错的错图。
 *
 * 技术特性:
 *   - 不需要 RHI 设备: 被测函数是纯静态映射
 *   - 同时核对 sRGB 判定与压缩判定在两侧的一致性
 *
 * 依赖关系:
 *   内部: EngineTests/EngineTestsMinimal.h,
 *          RenderCore/Resources/FRenderResourceManager.h
 *
 ******************************************************************************/

#include "EngineTests/EngineTestsMinimal.h"
#include "RenderCore/Resources/FRenderResourceManager.h"
#include "RenderCore/Material/FMaterial.h"

using namespace Limx;

namespace
{

struct FFormatCase
{
    EBlockCompressionFormat Source;
    EPixelFormat            Expected;
};

/// 全部 14 项 —— 与 EBlockCompressionFormat 的枚举项一一对应
constexpr FFormatCase kCases[14] =
{
    { EBlockCompressionFormat::BC1_UNORM,   EPixelFormat::BC1_UNORM },
    { EBlockCompressionFormat::BC1_SRGB,    EPixelFormat::BC1_SRGB },
    { EBlockCompressionFormat::BC2_UNORM,   EPixelFormat::BC2_UNORM },
    { EBlockCompressionFormat::BC2_SRGB,    EPixelFormat::BC2_SRGB },
    { EBlockCompressionFormat::BC3_UNORM,   EPixelFormat::BC3_UNORM },
    { EBlockCompressionFormat::BC3_SRGB,    EPixelFormat::BC3_SRGB },
    { EBlockCompressionFormat::BC4_UNORM,   EPixelFormat::BC4_UNORM },
    { EBlockCompressionFormat::BC4_SNORM,   EPixelFormat::BC4_SNORM },
    { EBlockCompressionFormat::BC5_UNORM,   EPixelFormat::BC5_UNORM },
    { EBlockCompressionFormat::BC5_SNORM,   EPixelFormat::BC5_SNORM },
    { EBlockCompressionFormat::BC6H_UFLOAT, EPixelFormat::BC6H_UFLOAT },
    { EBlockCompressionFormat::BC6H_SFLOAT, EPixelFormat::BC6H_SFLOAT },
    { EBlockCompressionFormat::BC7_UNORM,   EPixelFormat::BC7_UNORM },
    { EBlockCompressionFormat::BC7_SRGB,    EPixelFormat::BC7_SRGB },
};

} // namespace

LIMX_TEST(CompressedTextureFormat, MapsEveryBlockFormatExactly)
{
    for (UInt32 i = 0; i < 14u; ++i)
    {
        LIMX_EXPECT_EQ(
            FRenderResourceManager::MapCompressedPixelFormat(kCases[i].Source),
            kCases[i].Expected);
    }
}

LIMX_TEST(CompressedTextureFormat, UnknownMapsToUnknown)
{
    // 认不出来必须是 Unknown —— CreateTexture 据此报错并拒绝上传。
    // 兜底成任意一个具体格式的话, 一份认不出的数据会被当成那个格式建图。
    LIMX_EXPECT_EQ(
        FRenderResourceManager::MapCompressedPixelFormat(
            EBlockCompressionFormat::Unknown),
        EPixelFormat::Unknown);
}

LIMX_TEST(CompressedTextureFormat, MappedFormatsAreAllCompressed)
{
    // 映射结果必须落在 RHI 的压缩格式区间内。写成 RGBA8_UNORM 之类的
    // 非压缩格式时, 上传路径会按每像素 4 字节去算尺寸, 而载荷是按块给的。
    for (UInt32 i = 0; i < 14u; ++i)
    {
        LIMX_EXPECT_TRUE(IsCompressedFormat(kCases[i].Expected));
    }
}

LIMX_TEST(CompressedTextureFormat, SrgbAgreesOnBothSides)
{
    // 资产侧的 IsBlockCompressionSrgb 与 RHI 侧的 IsSRGBFormat 必须给出
    // 相同的结论。两边不一致意味着某一处在按错误的传输曲线做决定 ——
    // 而画面上只表现为整体偏亮或偏暗。
    for (UInt32 i = 0; i < 14u; ++i)
    {
        LIMX_EXPECT_EQ(IsBlockCompressionSrgb(kCases[i].Source),
                       IsSRGBFormat(kCases[i].Expected));
    }

    // 对照: 这一组里确实同时存在 sRGB 与非 sRGB, 否则上面那条恒成立
    LIMX_EXPECT_TRUE(IsBlockCompressionSrgb(EBlockCompressionFormat::BC1_SRGB));
    LIMX_EXPECT_FALSE(IsBlockCompressionSrgb(EBlockCompressionFormat::BC1_UNORM));
}

LIMX_TEST(CompressedTextureFormat, NormalTwoChannelFlagDoesNotCollide)
{
    // 这一位与五个槽位标志共用同一个 uint。取到已被占用的位, 表现是
    // "绑了某张贴图之后法线突然按 BC5 解读" —— 没有任何编译期或运行期
    // 报错, 只有画面上说不清的不对劲。
    for (UInt32 slot = 0; slot < kMaterialTextureSlotCount; ++slot)
    {
        LIMX_EXPECT_EQ(kMaterialTexFlagNormalTwoChannel & (1u << slot), 0u);
    }

    // 着色器侧 (pbr.frag 与 material_common.h) 把它写死成 1u << 5。
    // GLSL 没有办法从 C++ 取这个常量, 两边只能靠这条断言对齐 ——
    // 改动这里时必须同步改那两处。
    LIMX_EXPECT_EQ(kMaterialTexFlagNormalTwoChannel, 1u << 5);

    // 槽位标志本身也核对一遍: 法线槽是第 1 位, 双通道标志由它派生
    LIMX_EXPECT_EQ(kMaterialTexFlagNormal, 1u << kMaterialTextureSlotNormal);
}

LIMX_TEST(CompressedTextureFormat, BlockByteSizeAgreesOnBothSides)
{
    // 资产侧按块算字节数, RHI 侧的 GetPixelFormatByteSize 对压缩格式返回的
    // 也是"每块字节数"。两者不一致的话, 上传的偏移与显存统计会各错各的。
    for (UInt32 i = 0; i < 14u; ++i)
    {
        LIMX_EXPECT_EQ(GetBlockCompressionBlockByteSize(kCases[i].Source),
                       GetPixelFormatByteSize(kCases[i].Expected));
    }
}
