/*******************************************************************************
 * 文件: TextureFormatTests.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   纹理格式能力位与 mip 链层数的单元测试
 *
 * 设计哲学:
 *   mip 层数的边界最容易错, 而错了不会报错 — 少算一层, 最小的那级 mip
 *   永远采不到, 画面看不出区别但内存白占; 多算一层, vkCreateImage 直接
 *   失败, 而失败点距离"层数算错"这个原因隔着整个资源创建路径。
 *   这些用例把每个边界钉死。
 *
 *   能力位测试的是"全部包含"语义 — HasFormatFeature 判断的是 required
 *   的每一位都在 value 中, 而不是"有交集"。mip 生成同时需要 BlitSrc、
 *   BlitDst 与线性过滤三项, 写成交集判断会在只支持其中一项的设备上
 *   错误地启用逐级 blit。
 *
 * 依赖关系:
 *   内部: RHITests/RHITestsMinimal.h
 *
 ******************************************************************************/

#include "RHITests/RHITestsMinimal.h"

using namespace Limx;

// ============================================================================
// ComputeMipLevelCount — 层数与边界
// ============================================================================

LIMX_TEST(MipLevelCount, SingleTexelIsOneLevel)
{
    // 1x1 是一层而非零层 —— 它本身就是最小的那级
    LIMX_EXPECT_EQ(ComputeMipLevelCount(1, 1), 1u);
}

LIMX_TEST(MipLevelCount, PowerOfTwoSquares)
{
    LIMX_EXPECT_EQ(ComputeMipLevelCount(2, 2), 2u);
    LIMX_EXPECT_EQ(ComputeMipLevelCount(4, 4), 3u);
    LIMX_EXPECT_EQ(ComputeMipLevelCount(8, 8), 4u);
    LIMX_EXPECT_EQ(ComputeMipLevelCount(64, 64), 7u);
    LIMX_EXPECT_EQ(ComputeMipLevelCount(256, 256), 9u);

    // 1024 是 11 层: 1024→512→256→128→64→32→16→8→4→2→1
    LIMX_EXPECT_EQ(ComputeMipLevelCount(1024, 1024), 11u);
    LIMX_EXPECT_EQ(ComputeMipLevelCount(4096, 4096), 13u);
}

LIMX_TEST(MipLevelCount, TakesLargerDimension)
{
    // 层数由较长边决定 —— 较短边先到 1 之后保持在 1
    LIMX_EXPECT_EQ(ComputeMipLevelCount(1024, 1), 11u);
    LIMX_EXPECT_EQ(ComputeMipLevelCount(1, 1024), 11u);
    LIMX_EXPECT_EQ(ComputeMipLevelCount(256, 16), 9u);
    LIMX_EXPECT_EQ(ComputeMipLevelCount(16, 256), 9u);
}

LIMX_TEST(MipLevelCount, NonPowerOfTwoRoundsDown)
{
    // 非二次幂按 floor(log2(n)) + 1: 逐级向下取整
    // 3 → 1        : 2 层
    // 5 → 2 → 1    : 3 层
    // 100 → 50 → 25 → 12 → 6 → 3 → 1 : 7 层
    LIMX_EXPECT_EQ(ComputeMipLevelCount(3, 3), 2u);
    LIMX_EXPECT_EQ(ComputeMipLevelCount(5, 5), 3u);
    LIMX_EXPECT_EQ(ComputeMipLevelCount(100, 100), 7u);
    LIMX_EXPECT_EQ(ComputeMipLevelCount(1023, 1023), 10u);
    LIMX_EXPECT_EQ(ComputeMipLevelCount(1025, 1025), 11u);
}

LIMX_TEST(MipLevelCount, ZeroExtentYieldsZero)
{
    // 零尺寸不是合法纹理 —— 返回 0 让调用方能判定, 而不是返回 1 装作正常
    LIMX_EXPECT_EQ(ComputeMipLevelCount(0, 0), 0u);
    LIMX_EXPECT_EQ(ComputeMipLevelCount(0, 64), 0u);
    LIMX_EXPECT_EQ(ComputeMipLevelCount(64, 0), 0u);
}

LIMX_TEST(MipLevelCount, MatchesIterativeHalving)
{
    // 与"实际逐级折半"的结果交叉验证 —— 公式与循环必须给出同一个答案。
    // 上传路径里的 blit 循环正是按这个折半规则推进的。
    for (UInt32 size = 1; size <= 2048; ++size)
    {
        UInt32 extent = size;
        UInt32 expected = 1;

        while (extent > 1)
        {
            extent = (extent > 1) ? (extent / 2) : 1;
            ++expected;
        }

        LIMX_EXPECT_EQ(ComputeMipLevelCount(size, 1), expected);
    }
}

// ============================================================================
// EFormatFeature — 位运算与包含判定
// ============================================================================

LIMX_TEST(FormatFeature, EmptyContainsNothing)
{
    LIMX_EXPECT_FALSE(HasFormatFeature(EFormatFeature::None,
                                       EFormatFeature::BlitSrc));
}

LIMX_TEST(FormatFeature, RequiresAllBitsNotAny)
{
    // 只有 BlitSrc 的格式, 不满足 "BlitSrc + BlitDst" 的要求。
    // 若实现写成"有交集即通过", 这一条会失败 —— 而后果是在只支持单向
    // blit 的设备上启用逐级降采样。
    const EFormatFeature onlySrc = EFormatFeature::BlitSrc;

    LIMX_EXPECT_TRUE(HasFormatFeature(onlySrc, EFormatFeature::BlitSrc));
    LIMX_EXPECT_FALSE(HasFormatFeature(
        onlySrc, EFormatFeature::BlitSrc | EFormatFeature::BlitDst));
}

LIMX_TEST(FormatFeature, MipGenerationRequirementIsThreeBits)
{
    const EFormatFeature required = EFormatFeature::BlitSrc |
                                    EFormatFeature::BlitDst |
                                    EFormatFeature::SampledImageLinear;

    // 三项齐全才通过
    const EFormatFeature complete = EFormatFeature::SampledImage |
                                    EFormatFeature::SampledImageLinear |
                                    EFormatFeature::BlitSrc |
                                    EFormatFeature::BlitDst;
    LIMX_EXPECT_TRUE(HasFormatFeature(complete, required));

    // 缺线性过滤 —— 逐级 blit 会退化为最近邻, 生成的 mip 本身就是走样的
    const EFormatFeature noLinear = EFormatFeature::SampledImage |
                                    EFormatFeature::BlitSrc |
                                    EFormatFeature::BlitDst;
    LIMX_EXPECT_FALSE(HasFormatFeature(noLinear, required));

    // 缺 BlitDst —— 写不进目标 mip
    const EFormatFeature noDst = EFormatFeature::SampledImage |
                                 EFormatFeature::SampledImageLinear |
                                 EFormatFeature::BlitSrc;
    LIMX_EXPECT_FALSE(HasFormatFeature(noDst, required));
}

LIMX_TEST(FormatFeature, BitwiseAndExtractsCommonBits)
{
    const EFormatFeature a = EFormatFeature::SampledImage |
                             EFormatFeature::BlitSrc;
    const EFormatFeature b = EFormatFeature::BlitSrc |
                             EFormatFeature::ColorAttachment;

    const EFormatFeature common = a & b;

    LIMX_EXPECT_TRUE(HasFormatFeature(common, EFormatFeature::BlitSrc));
    LIMX_EXPECT_FALSE(HasFormatFeature(common, EFormatFeature::SampledImage));
    LIMX_EXPECT_FALSE(HasFormatFeature(common,
                                       EFormatFeature::ColorAttachment));
}

LIMX_TEST(FormatFeature, BitsAreDistinct)
{
    // 任意两个能力位不得重叠 —— 重叠会让一个查询意外满足另一个要求
    const EFormatFeature all[] = {
        EFormatFeature::SampledImage,
        EFormatFeature::SampledImageLinear,
        EFormatFeature::StorageImage,
        EFormatFeature::ColorAttachment,
        EFormatFeature::ColorAttachmentBlend,
        EFormatFeature::DepthStencilAttachment,
        EFormatFeature::BlitSrc,
        EFormatFeature::BlitDst,
    };

    constexpr SizeType kCount = sizeof(all) / sizeof(all[0]);

    for (SizeType i = 0; i < kCount; ++i)
    {
        for (SizeType j = i + 1; j < kCount; ++j)
        {
            LIMX_EXPECT_TRUE((all[i] & all[j]) == EFormatFeature::None);
        }
    }
}
