/*******************************************************************************
 * 文件: RenderPassRefTests.cpp
 * 创建时间: 2026-08-30
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   CreateRenderPass 的附件引用容量验证 — 子通道颜色引用总数超过内联容量时,
 *   每个子通道都必须拿到完整且有效的引用数组。
 *
 * 设计哲学:
 *   这一处原本的形态比"静默截断"更糟, 值得单独立个用例。原实现是:
 *
 *       dst.colorAttachmentCount = src.ColorAttachmentCount;   // 先按调用方写
 *       if (放得下) { dst.pColorAttachments = ...; 填充 }       // 放不下就跳过
 *
 *   放不下时计数已经写进去了, 指针却留在 MemZero 后的 nullptr —— 交给驱动的
 *   是"计数非零 + 空指针"。那不是少几个附件, 而是让驱动去读空指针。
 *
 *   判据是三条并列的: 零验证层错误 + CreateRenderPass 返回成功 + 句柄有效。
 *
 *   实测变异结果值得记下来: 把这处改回原形态后, 进程直接**段错误**
 *   (退出码 139), 验证层一条都没来得及报 —— 它的无状态检查并不会在
 *   colorAttachmentCount 非零时校验 pColorAttachments 是否为空。也就是说
 *   原实现不是"少几个附件"而是一个潜伏的空指针解引用。
 *
 *   这也正是"判定只看退出码"这条纪律的价值: 崩溃、断言失败、验证层报错三种
 *   形态都只落在同一个判据上, 解析输出文本的做法在崩溃时会什么也拿不到。
 *
 *   触发需要**颜色引用总数**超过内联容量 (32), 而不是单个子通道超过
 *   maxColorAttachments (本机为 8)。8 个子通道 × 每个 8 个颜色引用 = 64,
 *   两个限制互不冲突, 因此这个形状是真的能构造出来的。
 *
 * 依赖关系:
 *   内部: RHITests/FRHITestDevice.h (共用 GPU 脚手架)
 *
 ******************************************************************************/

#include "RHITests/RHITestsMinimal.h"
#include "RHITests/FRHITestDevice.h"

using namespace Limx;
using namespace Limx::RHITesting;

namespace
{

/// 被测实现的附件引用内联容量
constexpr UInt32 kInlineRefs = 32;

/// 子通道数 — 与实现的子通道内联容量相同, 不额外考验那一维
constexpr UInt32 kSubpassCount = 8;

/// 每个子通道的颜色附件数 — 取本机 maxColorAttachments 的常见值
constexpr UInt32 kColorPerSubpass = 8;

/// 颜色引用总数 = 64, 是内联容量 32 的两倍
constexpr UInt32 kTotalColorRefs = kSubpassCount * kColorPerSubpass;

} // namespace

// ============================================================================
// 颜色附件引用总数超过内联容量
// ============================================================================

LIMX_TEST(CreateRenderPass, ColorRefsBeyondInlineCapacityStayValid)
{
    static_assert(kTotalColorRefs > kInlineRefs,
                  "用例必须真的越过内联容量, 否则什么都没测到");

    FRHITestDevice harness;
    if (!harness.Initialize())
    {
        LIMX_TEST_SKIP("本机无法创建带验证层的 Vulkan 设备 — 跳过 GPU 用例");
    }

    IRHIDevice& device = harness.GetDevice();

    // ------------------------------------------------------------------
    // 附件: kColorPerSubpass 张颜色附件, 每个子通道都引用全部这些
    // ------------------------------------------------------------------

    TArray<FRHIAttachmentDesc> attachments;
    attachments.SetSize(static_cast<SizeType>(kColorPerSubpass));

    for (UInt32 i = 0; i < kColorPerSubpass; ++i)
    {
        FRHIAttachmentDesc& attachment = attachments[i];
        attachment.Format         = EPixelFormat::RGBA8_UNORM;
        attachment.Samples        = ESampleCount::Count1;
        attachment.LoadOp         = ELoadOp::Clear;
        attachment.StoreOp        = EStoreOp::Store;
        attachment.StencilLoadOp  = ELoadOp::DontCare;
        attachment.StencilStoreOp = EStoreOp::DontCare;
        attachment.InitialLayout  = EImageLayout::Undefined;
        attachment.FinalLayout    = EImageLayout::ColorAttachment;
    }

    // ------------------------------------------------------------------
    // 引用: 每个子通道一段 kColorPerSubpass 个引用, 全部放在一个数组里
    // ------------------------------------------------------------------

    TArray<FRHIAttachmentReference> colorRefs;
    colorRefs.SetSize(static_cast<SizeType>(kTotalColorRefs));

    for (UInt32 s = 0; s < kSubpassCount; ++s)
    {
        for (UInt32 c = 0; c < kColorPerSubpass; ++c)
        {
            FRHIAttachmentReference& ref =
                colorRefs[s * kColorPerSubpass + c];
            ref.AttachmentIndex = c;
            ref.Layout          = EImageLayout::ColorAttachment;
        }
    }

    TArray<FRHISubpassDesc> subpasses;
    subpasses.SetSize(static_cast<SizeType>(kSubpassCount));

    for (UInt32 s = 0; s < kSubpassCount; ++s)
    {
        subpasses[s].ColorAttachments =
            &colorRefs[s * kColorPerSubpass];
        subpasses[s].ColorAttachmentCount = kColorPerSubpass;
    }

    // 串起相邻子通道 —— 每个子通道都写同一批附件, 必须有依赖
    TArray<FRHISubpassDependency> dependencies;
    dependencies.SetSize(static_cast<SizeType>(kSubpassCount - 1));

    for (UInt32 d = 0; d + 1 < kSubpassCount; ++d)
    {
        FRHISubpassDependency& dependency = dependencies[d];
        dependency.SrcSubpass    = d;
        dependency.DstSubpass    = d + 1;
        dependency.SrcStageMask  = EPipelineStageFlags::ColorAttachmentOutput;
        dependency.DstStageMask  = EPipelineStageFlags::ColorAttachmentOutput;
        dependency.SrcAccessMask = EAccessFlags::ColorAttachmentWrite;
        dependency.DstAccessMask = EAccessFlags::ColorAttachmentWrite
                                 | EAccessFlags::ColorAttachmentRead;
    }

    FRHIRenderPassDesc desc;
    desc.Attachments     = attachments.GetData();
    desc.AttachmentCount = kColorPerSubpass;
    desc.Subpasses       = subpasses.GetData();
    desc.SubpassCount    = kSubpassCount;
    desc.Dependencies    = dependencies.GetData();
    desc.DependencyCount = kSubpassCount - 1;
    desc.DebugName       = "RefCapacityTestRenderPass";

    // ------------------------------------------------------------------
    // 创建 — 引用放不下时, 原实现会给出"计数非零 + 空指针"
    // ------------------------------------------------------------------

    FValidationErrorSink sink;
    FLog::AddSink(&sink);

    FRHIRenderPassHandle renderPass;
    const ERHIResult createResult = device.CreateRenderPass(desc, renderPass);

    const UInt32 errorCount = sink.GetErrorCount();
    const FString firstError = sink.GetFirstError();
    FLog::RemoveSink(&sink);

    if (errorCount != 0)
    {
        LIMX_TEST_FAIL(StringFormat(
            "{} 个子通道共 {} 个颜色引用 (内联容量 {}) 触发了 {} 条验证层"
            "错误 (首条: {})",
            kSubpassCount, kTotalColorRefs, kInlineRefs, errorCount,
            firstError.GetCStr()));
    }

    LIMX_EXPECT_EQ(errorCount, 0u);
    LIMX_EXPECT_EQ(static_cast<Int32>(createResult),
                   static_cast<Int32>(ERHIResult::Success));
    LIMX_EXPECT_TRUE(renderPass.IsValid());

    LIMX_TEST_INFO("{} 个子通道 × {} 个颜色引用 = {} 个 (内联容量 {}) 全部有效",
                   kSubpassCount, kColorPerSubpass, kTotalColorRefs,
                   kInlineRefs);

    if (renderPass.IsValid())
    {
        device.DestroyRenderPass(renderPass);
    }

    device.WaitIdle();
}
