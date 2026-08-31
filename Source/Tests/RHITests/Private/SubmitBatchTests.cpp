/*******************************************************************************
 * 文件: SubmitBatchTests.cpp
 * 创建时间: 2026-08-30
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   FVulkanDevice::Submit 的数量语义验证 — 一次提交里超过原内联容量的
 *   命令缓冲区与信号量, 断言每一个都真的到达了驱动。
 *
 * 设计哲学:
 *   提交里的静默截断比屏障那处更直接: 第 17 个命令缓冲区被丢掉, 意味着那一
 *   整批录好的命令**根本不会执行**, 而 vkQueueSubmit 照样返回 VK_SUCCESS,
 *   栅栏照样触发, 验证层也没有可报的东西 —— 它看到的是一次合法的、只是少了
 *   几个命令缓冲区的提交。
 *
 *   因此每个用例各用一条与 GPU 调度无关的确定性判据:
 *
 *     命令缓冲区 —— 数据判据。回读缓冲区先由主机填成哨兵字节, 再让每个命令
 *       缓冲区往自己那一段写入各不相同的内容。少执行一个, 对应那一段就仍然
 *       是哨兵。这条判据不依赖验证层, 也不依赖驱动的调度顺序。
 *
 *     触发信号量 —— 前进判据。一次触发 N 个, 再分批 (每批不超内联容量) 等待。
 *       少触发一个, 后一批等待就永远等不到 —— 验证层在提交处直接报"这个信号
 *       量没有任何办法被触发", 驱动层面则是队列停在那里。
 *
 *     等待信号量 —— 状态判据。二值信号量的等待会把它置回未触发态。若等待被
 *       截断, 末尾那个信号量仍处于已触发态, 此时再触发它一次就是重复触发,
 *       验证层必然报错。
 *
 *   触发与等待刻意拆成两个用例, 各自只让一侧超限。合在一起写会互相掩蔽:
 *   触发 11 个只发 8 个、等待 11 个也只等 8 个, 等待照样满足, 谁也看不出来。
 *   这一点是实测出来的 —— 合并写法确实抓不住三处同时截断的变异。
 *
 * 依赖关系:
 *   内部: RHITests/FRHITestDevice.h (共用 GPU 脚手架)
 *
 * 注意事项:
 *   触发端用例在缺陷版本下可能走到"队列上留着一个永远等不到的提交"这一步。
 *   用例必须在超时后补发触发把它放掉 —— 否则析构里的 vkDeviceWaitIdle 会
 *   挂死整个测试进程, 于是"失败"变成"卡住", 连退出码都拿不到。
 *   (在本机的 NVIDIA 驱动上验证层会先一步报错, 队列并未真的停住; 补救路径
 *    是为其它驱动准备的, 未在本机被执行到。)
 *
 ******************************************************************************/

#include "RHITests/RHITestsMinimal.h"
#include "RHITests/FRHITestDevice.h"

using namespace Limx;
using namespace Limx::RHITesting;

namespace
{

// ============================================================================
// 常量
// ============================================================================

/// 被测实现一次提交的命令缓冲区内联容量
constexpr UInt32 kInlineCmdBuffers = 16;

/// 被测实现一次提交的信号量内联容量
constexpr UInt32 kInlineSemaphores = 8;

/// 用例提交的命令缓冲区个数 — 远超内联容量, 且不是它的整数倍
constexpr UInt32 kCmdBufferCount = kInlineCmdBuffers + 9;   // 25

/// 用例使用的信号量个数 — 超过内联容量
constexpr UInt32 kSemaphoreCount = kInlineSemaphores + 3;   // 11

/// 每个命令缓冲区负责的字节数
constexpr UInt64 kSegmentBytes = 256;

/// 回读缓冲区的初始哨兵字节 —— 与任何 MakeFillByte 结果都不同
constexpr UInt8 kSentinelByte = 0xEE;

} // namespace

// ============================================================================
// 命令缓冲区 — 数据判据
// ============================================================================

LIMX_TEST(Submit, CommandBuffersBeyondInlineCapacityAllExecute)
{
    FRHITestDevice harness;
    if (!harness.Initialize())
    {
        LIMX_TEST_SKIP("本机无法创建带验证层的 Vulkan 设备 — 跳过 GPU 用例");
    }

    IRHIDevice& device = harness.GetDevice();

    // ------------------------------------------------------------------
    // 资源: 一个源缓冲区 (主机写入 kCmdBufferCount 段各不相同的内容)
    //       一个回读缓冲区 (主机预填哨兵)
    // ------------------------------------------------------------------

    const UInt64 totalBytes = kSegmentBytes * kCmdBufferCount;

    FRHIBufferDesc stagingDesc;
    stagingDesc.Size        = totalBytes;
    stagingDesc.Usage       = EBufferUsage::TransferSrc;
    stagingDesc.MemoryUsage = EMemoryUsage::CpuToGpu;
    stagingDesc.DebugName   = "SubmitTestStaging";

    FRHIBufferHandle staging;
    LIMX_REQUIRE_EQ(
        static_cast<Int32>(device.CreateBuffer(stagingDesc, staging)),
        static_cast<Int32>(ERHIResult::Success));

    void* stagingMemory = nullptr;
    LIMX_REQUIRE_EQ(
        static_cast<Int32>(device.MapBuffer(staging, &stagingMemory)),
        static_cast<Int32>(ERHIResult::Success));
    LIMX_REQUIRE_NOT_NULL(stagingMemory);

    UInt8* stagingBytes = static_cast<UInt8*>(stagingMemory);
    for (UInt32 i = 0; i < kCmdBufferCount; ++i)
    {
        Memory::MemSet(stagingBytes + kSegmentBytes * i,
                       MakeFillByte(i),
                       static_cast<SizeType>(kSegmentBytes));
    }
    device.UnmapBuffer(staging);

    FRHIBufferDesc readbackDesc;
    readbackDesc.Size        = totalBytes;
    readbackDesc.Usage       = EBufferUsage::TransferDst;
    readbackDesc.MemoryUsage = EMemoryUsage::GpuToCpu;
    readbackDesc.DebugName   = "SubmitTestReadback";

    FRHIBufferHandle readback;
    LIMX_REQUIRE_EQ(
        static_cast<Int32>(device.CreateBuffer(readbackDesc, readback)),
        static_cast<Int32>(ERHIResult::Success));

    // 预填哨兵 —— 这是判据的基线: 没被执行到的段会原样留着哨兵
    void* readbackMemory = nullptr;
    LIMX_REQUIRE_EQ(
        static_cast<Int32>(device.MapBuffer(readback, &readbackMemory)),
        static_cast<Int32>(ERHIResult::Success));
    LIMX_REQUIRE_NOT_NULL(readbackMemory);
    Memory::MemSet(readbackMemory, kSentinelByte,
                   static_cast<SizeType>(totalBytes));
    device.UnmapBuffer(readback);

    // ------------------------------------------------------------------
    // 录制: kCmdBufferCount 个各自独立的命令缓冲区, 每个只拷一段
    // ------------------------------------------------------------------

    TArray<FRHICommandBufferHandle> handles;
    handles.SetSize(static_cast<SizeType>(kCmdBufferCount));

    for (UInt32 i = 0; i < kCmdBufferCount; ++i)
    {
        LIMX_REQUIRE_EQ(
            static_cast<Int32>(device.AllocateCommandBuffer(
                harness.GetCommandPool(), ECommandBufferLevel::Primary,
                handles[i])),
            static_cast<Int32>(ERHIResult::Success));
    }

    FValidationErrorSink sink;
    FLog::AddSink(&sink);

    for (UInt32 i = 0; i < kCmdBufferCount; ++i)
    {
        // 包装器只是持有设备与句柄的薄壳, 用完即弃不会影响已录制的内容
        TUniquePtr<IRHICommandBuffer> commandBuffer =
            CreateRHICommandBuffer(&device, handles[i]);

        commandBuffer->Begin();

        FRHIBufferCopyRegion region;
        region.SrcOffset = kSegmentBytes * i;
        region.DstOffset = kSegmentBytes * i;
        region.Size      = kSegmentBytes;
        commandBuffer->CopyBuffer(staging, readback, region);

        // 最后一个命令缓冲区负责把写入对主机放行 —— 它同时也是"最容易被
        // 截断丢掉"的那一个
        if (i + 1 == kCmdBufferCount)
        {
            const FRHIBufferMemoryBarrier toHost = MakeBufferBarrier(
                readback, totalBytes,
                EAccessFlags::TransferWrite, EAccessFlags::HostRead);

            commandBuffer->PipelineBarrier(
                EPipelineStageFlags::Transfer, EPipelineStageFlags::Host,
                nullptr, 0,
                &toHost, 1,
                nullptr, 0);
        }

        commandBuffer->End();
    }

    // 一次提交, 全部 kCmdBufferCount 个
    const bool isSubmitted =
        harness.SubmitAndWait(handles.GetData(), kCmdBufferCount);

    const UInt32 errorCount = sink.GetErrorCount();
    const FString firstError = sink.GetFirstError();
    FLog::RemoveSink(&sink);

    LIMX_EXPECT_TRUE(isSubmitted);
    LIMX_EXPECT_EQ(errorCount, 0u);

    if (errorCount != 0)
    {
        LIMX_TEST_FAIL(StringFormat(
            "提交 {} 个命令缓冲区后出现 {} 条验证层错误 (首条: {})",
            kCmdBufferCount, errorCount, firstError.GetCStr()));
    }

    // ------------------------------------------------------------------
    // 主判据: 每一段都必须被它自己的命令缓冲区写过
    // ------------------------------------------------------------------

    void* mapped = nullptr;
    if (device.MapBuffer(readback, &mapped) == ERHIResult::Success
        && mapped != nullptr)
    {
        const UInt8* bytes = static_cast<const UInt8*>(mapped);

        UInt32 missingSegments = 0;
        UInt32 wrongSegments   = 0;
        UInt32 firstMissing    = kCmdBufferCount;

        for (UInt32 i = 0; i < kCmdBufferCount; ++i)
        {
            const UInt8 expected = MakeFillByte(i);
            const UInt8 actual   = bytes[kSegmentBytes * i];

            if (actual == kSentinelByte)
            {
                // 哨兵原样还在 —— 这一段的命令缓冲区根本没执行
                ++missingSegments;
                if (firstMissing == kCmdBufferCount)
                {
                    firstMissing = i;
                }
            }
            else if (actual != expected)
            {
                ++wrongSegments;
            }
        }

        if (missingSegments != 0)
        {
            LIMX_TEST_FAIL(StringFormat(
                "{} 个命令缓冲区里有 {} 个未执行 (最早是第 {} 个) —— "
                "对应字段仍是主机预填的哨兵 {}",
                kCmdBufferCount, missingSegments, firstMissing,
                static_cast<UInt32>(kSentinelByte)));
        }

        LIMX_EXPECT_EQ(missingSegments, 0u);
        LIMX_EXPECT_EQ(wrongSegments, 0u);

        device.UnmapBuffer(readback);
    }
    else
    {
        LIMX_TEST_FAIL("回读缓冲区映射失败 — 无法确认命令是否真的执行过");
    }

    LIMX_TEST_INFO("{} 个命令缓冲区 (内联容量 {}) 全部执行",
                   kCmdBufferCount, kInlineCmdBuffers);

    device.WaitIdle();

    for (UInt32 i = 0; i < kCmdBufferCount; ++i)
    {
        device.FreeCommandBuffer(handles[i]);
    }

    device.DestroyBuffer(readback);
    device.DestroyBuffer(staging);
}

// ============================================================================
// 信号量 — 触发与等待分开验证
// ============================================================================
//
// 两个场景刻意各自只让**一侧**超出内联容量, 另一侧全部保持在容量以内。
//
// 理由是掩蔽: 若同一次提交里触发与等待都超限, 两处截断会互相抵消 ——
// 触发 11 个只发出 8 个, 等待 11 个也只等 8 个, 于是等待照样满足, 谁也
// 看不出问题。实测确认过: 三处同时截断的变异用合并写法抓不住信号量那半。
// ============================================================================

/// 触发端超限 — 前进判据
///
/// 一次提交触发全部 kSemaphoreCount 个, 再分两批 (各不超过内联容量) 等待。
/// 触发被截断时, 末尾那几个从未被触发, 第二批等待就永远等不到。
LIMX_TEST(Submit, SignalSemaphoresBeyondInlineCapacityAllTakeEffect)
{
    FRHITestDevice harness;
    if (!harness.Initialize())
    {
        LIMX_TEST_SKIP("本机无法创建带验证层的 Vulkan 设备 — 跳过 GPU 用例");
    }

    IRHIDevice& device = harness.GetDevice();

    TArray<FRHISemaphoreHandle> semaphores;
    semaphores.SetSize(static_cast<SizeType>(kSemaphoreCount));

    for (UInt32 i = 0; i < kSemaphoreCount; ++i)
    {
        LIMX_REQUIRE_EQ(
            static_cast<Int32>(device.CreateSemaphore(semaphores[i])),
            static_cast<Int32>(ERHIResult::Success));
    }

    TArray<EPipelineStageFlags> waitStages;
    waitStages.SetSize(static_cast<SizeType>(kSemaphoreCount));
    for (UInt32 i = 0; i < kSemaphoreCount; ++i)
    {
        waitStages[i] = EPipelineStageFlags::TopOfPipe;
    }

    FValidationErrorSink sink;
    FLog::AddSink(&sink);

    // 一次触发全部 —— 这一侧是被测的超限方
    FRHISubmitInfo signalAll;
    signalAll.SignalSemaphores     = semaphores.GetData();
    signalAll.SignalSemaphoreCount = kSemaphoreCount;

    const ERHIResult signalResult = harness.SubmitAndWaitResult(signalAll);
    LIMX_EXPECT_EQ(static_cast<Int32>(signalResult),
                   static_cast<Int32>(ERHIResult::Success));

    // 分两批等待, 每批都在内联容量以内 —— 等待侧不会被截断
    constexpr UInt32 kFirstChunk  = kInlineSemaphores;
    constexpr UInt32 kSecondChunk = kSemaphoreCount - kInlineSemaphores;

    FRHISubmitInfo waitFirst;
    waitFirst.WaitSemaphores     = semaphores.GetData();
    waitFirst.WaitStages         = waitStages.GetData();
    waitFirst.WaitSemaphoreCount = kFirstChunk;

    const ERHIResult firstResult = harness.SubmitAndWaitResult(waitFirst);
    LIMX_EXPECT_EQ(static_cast<Int32>(firstResult),
                   static_cast<Int32>(ERHIResult::Success));

    FRHISubmitInfo waitSecond;
    waitSecond.WaitSemaphores     = &semaphores[kFirstChunk];
    waitSecond.WaitStages         = waitStages.GetData();
    waitSecond.WaitSemaphoreCount = kSecondChunk;

    const ERHIResult secondResult = harness.SubmitAndWaitResult(waitSecond);

    const UInt32 errorCount = sink.GetErrorCount();
    const FString firstError = sink.GetFirstError();
    FLog::RemoveSink(&sink);

    if (secondResult != ERHIResult::Success)
    {
        LIMX_TEST_FAIL(StringFormat(
            "等待末尾 {} 个信号量的提交没能完成 (结果 {}) —— "
            "说明一次触发 {} 个时后面几个没下发", kSecondChunk,
            static_cast<Int32>(secondResult), kSemaphoreCount));

        // 补救: 逐个补发触发, 让那条卡住的提交前进。不补的话析构里的
        // vkDeviceWaitIdle 会挂死整个进程, "失败"就变成"卡住"。
        for (UInt32 i = kFirstChunk; i < kSemaphoreCount; ++i)
        {
            FRHISubmitInfo rescue;
            rescue.SignalSemaphores     = &semaphores[i];
            rescue.SignalSemaphoreCount = 1;
            static_cast<void>(device.Submit(EQueueType::Graphics, rescue));
        }
    }
    else if (errorCount != 0)
    {
        LIMX_TEST_FAIL(StringFormat(
            "一次触发 {} 个信号量后出现 {} 条验证层错误 (首条: {})",
            kSemaphoreCount, errorCount, firstError.GetCStr()));
    }

    LIMX_EXPECT_EQ(errorCount, 0u);

    LIMX_TEST_INFO("一次触发 {} 个信号量 (内联容量 {}) 全部生效",
                   kSemaphoreCount, kInlineSemaphores);

    device.WaitIdle();

    for (UInt32 i = 0; i < kSemaphoreCount; ++i)
    {
        device.DestroySemaphore(semaphores[i]);
    }
}

/// 等待端超限 — 状态判据
///
/// 分两批 (各不超过内联容量) 触发, 再一次等待全部。二值信号量的等待会把它
/// 置回未触发态, 因此等待若被截断, 末尾那个仍处于已触发态 —— 此时再触发它
/// 一次就是重复触发, 验证层必抓。
LIMX_TEST(Submit, WaitSemaphoresBeyondInlineCapacityAllTakeEffect)
{
    FRHITestDevice harness;
    if (!harness.Initialize())
    {
        LIMX_TEST_SKIP("本机无法创建带验证层的 Vulkan 设备 — 跳过 GPU 用例");
    }

    IRHIDevice& device = harness.GetDevice();

    TArray<FRHISemaphoreHandle> semaphores;
    semaphores.SetSize(static_cast<SizeType>(kSemaphoreCount));

    for (UInt32 i = 0; i < kSemaphoreCount; ++i)
    {
        LIMX_REQUIRE_EQ(
            static_cast<Int32>(device.CreateSemaphore(semaphores[i])),
            static_cast<Int32>(ERHIResult::Success));
    }

    TArray<EPipelineStageFlags> waitStages;
    waitStages.SetSize(static_cast<SizeType>(kSemaphoreCount));
    for (UInt32 i = 0; i < kSemaphoreCount; ++i)
    {
        waitStages[i] = EPipelineStageFlags::TopOfPipe;
    }

    FValidationErrorSink sink;
    FLog::AddSink(&sink);

    // 分两批触发, 每批都在内联容量以内 —— 触发侧不会被截断
    constexpr UInt32 kFirstChunk  = kInlineSemaphores;
    constexpr UInt32 kSecondChunk = kSemaphoreCount - kInlineSemaphores;

    FRHISubmitInfo signalFirst;
    signalFirst.SignalSemaphores     = semaphores.GetData();
    signalFirst.SignalSemaphoreCount = kFirstChunk;
    LIMX_EXPECT_EQ(
        static_cast<Int32>(harness.SubmitAndWaitResult(signalFirst)),
        static_cast<Int32>(ERHIResult::Success));

    FRHISubmitInfo signalSecond;
    signalSecond.SignalSemaphores     = &semaphores[kFirstChunk];
    signalSecond.SignalSemaphoreCount = kSecondChunk;
    LIMX_EXPECT_EQ(
        static_cast<Int32>(harness.SubmitAndWaitResult(signalSecond)),
        static_cast<Int32>(ERHIResult::Success));

    // 一次等待全部 —— 这一侧是被测的超限方
    FRHISubmitInfo waitAll;
    waitAll.WaitSemaphores     = semaphores.GetData();
    waitAll.WaitStages         = waitStages.GetData();
    waitAll.WaitSemaphoreCount = kSemaphoreCount;

    LIMX_EXPECT_EQ(
        static_cast<Int32>(harness.SubmitAndWaitResult(waitAll)),
        static_cast<Int32>(ERHIResult::Success));

    // 再触发一次末尾那个: 等待全部生效时它已是未触发态, 合法;
    // 等待被截断时它仍是已触发态, 重复触发 —— 验证层报错。
    FRHISubmitInfo reSignalLast;
    reSignalLast.SignalSemaphores     = &semaphores[kSemaphoreCount - 1];
    reSignalLast.SignalSemaphoreCount = 1;
    LIMX_EXPECT_EQ(
        static_cast<Int32>(harness.SubmitAndWaitResult(reSignalLast)),
        static_cast<Int32>(ERHIResult::Success));

    // 消费掉刚触发的那个, 收尾不留已触发的信号量
    FRHISubmitInfo waitLast;
    waitLast.WaitSemaphores     = &semaphores[kSemaphoreCount - 1];
    waitLast.WaitStages         = waitStages.GetData();
    waitLast.WaitSemaphoreCount = 1;
    static_cast<void>(harness.SubmitAndWaitResult(waitLast));

    const UInt32 errorCount = sink.GetErrorCount();
    const FString firstError = sink.GetFirstError();
    FLog::RemoveSink(&sink);

    if (errorCount != 0)
    {
        LIMX_TEST_FAIL(StringFormat(
            "一次等待 {} 个信号量后出现 {} 条验证层错误 (首条: {})",
            kSemaphoreCount, errorCount, firstError.GetCStr()));
    }
    LIMX_EXPECT_EQ(errorCount, 0u);

    LIMX_TEST_INFO("一次等待 {} 个信号量 (内联容量 {}) 全部生效",
                   kSemaphoreCount, kInlineSemaphores);

    device.WaitIdle();

    for (UInt32 i = 0; i < kSemaphoreCount; ++i)
    {
        device.DestroySemaphore(semaphores[i]);
    }
}

// ============================================================================
// 无效句柄 — 必须出声, 不能默默跳过
// ============================================================================

LIMX_TEST(Submit, InvalidCommandBufferHandleIsReported)
{
    FRHITestDevice harness;
    if (!harness.Initialize())
    {
        LIMX_TEST_SKIP("本机无法创建带验证层的 Vulkan 设备 — 跳过 GPU 用例");
    }

    IRHIDevice& device = harness.GetDevice();

    // 三个全无效的句柄 —— 于是这次提交不含任何命令缓冲区, 验证层没有可报
    // 的东西, 计到的三条错误只能来自 RHI 自己。
    constexpr UInt32 kInvalidCount = 3;
    FRHICommandBufferHandle invalidHandles[kInvalidCount];

    FValidationErrorSink sink;
    FLog::AddSink(&sink);

    FRHISubmitInfo submitInfo;
    submitInfo.CommandBuffers     = invalidHandles;
    submitInfo.CommandBufferCount = kInvalidCount;

    const ERHIResult result = harness.SubmitAndWaitResult(submitInfo);

    const UInt32 errorCount = sink.GetErrorCount();
    FLog::RemoveSink(&sink);

    LIMX_EXPECT_EQ(static_cast<Int32>(result),
                   static_cast<Int32>(ERHIResult::Success));
    LIMX_EXPECT_EQ(errorCount, kInvalidCount);

    LIMX_TEST_INFO("{} 个无效命令缓冲区句柄各留下一条错误日志",
                   kInvalidCount);

    device.WaitIdle();
}
