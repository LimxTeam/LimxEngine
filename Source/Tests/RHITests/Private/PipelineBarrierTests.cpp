/*******************************************************************************
 * 文件: PipelineBarrierTests.cpp
 * 创建时间: 2026-08-30
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   FVulkanCommandBuffer::PipelineBarrier 的批量下发验证 —
 *   一次提交超过内部批量大小的屏障, 断言每一条都真的到达了驱动。
 *
 * 设计哲学:
 *   屏障丢失是"失败落在通过上"的典型: 少下发几个屏障不会崩溃、不会返回
 *   错误码, 连普通验证层都不报 —— 它看到的是一次合法的、只是少了几个
 *   屏障的调用。后果要到运行期才以偶发的数据撕裂显形。
 *
 *   因此本文件的判据刻意避开"跑一遍看输出对不对": 少一个屏障在绝大多数
 *   驱动上照样能跑出正确结果, 那种断言证明不了任何事情。这里用的是两个
 *   与 GPU 调度无关的确定性判据:
 *
 *     图像布局状态机 —— 验证层逐条跟踪每个图像子资源的布局。屏障没下发,
 *       图像就停在 Undefined; 后续以 TransferDst 布局使用它时, 验证层在
 *       提交处必然报错。这是层内的软件状态机, 与驱动、与时序都无关。
 *
 *     同步验证 (syncval) —— 对读写与屏障做符号化建模, 确定性地报出
 *       "这次读没有被前面那次写的屏障覆盖"。缓冲区屏障与全局内存屏障
 *       没有布局可跟踪, 只能靠这一档。设备因此以 enableSyncValidation
 *       创建。
 *
 *   两个判据都通过日志 Sink 统计 —— 验证层的消息经 LogRHI/Error 转发,
 *   用例只需数一数窗口期内有没有错误。
 *
 *   另外每个用例都做真实回读并逐字节比对: 那不是用来抓截断的 (截断版本
 *   也可能读出正确值), 而是用来证明"这条命令流确实在 GPU 上跑过了" ——
 *   否则一个什么都没执行的用例也会显示通过。
 *
 * 依赖关系:
 *   内部: RHITests/RHITestsMinimal.h (含 RHI 设备工厂与命令缓冲区接口)
 *   外部: Vulkan 运行时 + VK_LAYER_KHRONOS_validation; user32 (隐藏窗口)
 *
 * 注意事项:
 *   无 GPU / 无验证层的机器上用例会跳过而非失败 —— 跳过会在报告里单列,
 *   不会被计成通过。变异验证必须在真的建得起设备的机器上做。
 *
 ******************************************************************************/

#include "RHITests/RHITestsMinimal.h"

using namespace Limx;

namespace
{

// ============================================================================
// 常量
// ============================================================================

/// 被测实现一次 vkCmdPipelineBarrier 携带的屏障条数
///
/// 这里刻意与实现保持同一个数字: 用例要构造的是"恰好越过批量边界"的场景。
/// 实现若把批量调大, 本用例的屏障数仍远超它, 覆盖依然成立。
constexpr UInt32 kImplementationBatchSize = 16;

/// 每个用例下发的屏障条数 — 跨越三个批次, 且最后一批不满
constexpr UInt32 kBarrierCount = kImplementationBatchSize * 2 + 7;  // 39

/// 每个缓冲区用例中单个缓冲区的字节数
constexpr UInt64 kBufferBytes = 256;

/// 纹理边长
constexpr UInt32 kTextureSize = 4;

/// 纹理像素字节数 (RGBA8)
constexpr UInt64 kTexturePixelBytes = 4;

// ============================================================================
// FValidationErrorSink — 统计窗口期内验证层报出的错误
// ============================================================================

/// 只数 Error 级别 —— 验证层的警告 (性能提示等) 不参与判定
class FValidationErrorSink final : public ILogSink
{
public:
    void Write(const LogCategory& category,
               LogVerbosity verbosity,
               const AnsiChar* message) override
    {
        static_cast<void>(category);

        if (verbosity != LogVerbosity::Error)
        {
            return;
        }

        ++m_ErrorCount;

        if (m_FirstError.IsEmpty() && message != nullptr)
        {
            m_FirstError = FString(message);
        }
    }

    LIMX_NODISCARD UInt32 GetErrorCount() const { return m_ErrorCount; }

    LIMX_NODISCARD const FString& GetFirstError() const
    {
        return m_FirstError;
    }

private:
    UInt32  m_ErrorCount = 0;
    FString m_FirstError;
};

// ============================================================================
// FBarrierTestDevice — 隐藏窗口 + RHI 设备 + 命令池的 RAII 组合
// ============================================================================

/// 设备初始化链路必经 VkSurfaceKHR, 而 Win32 表面需要一个真实 HWND。
/// 用预定义的 "STATIC" 窗口类可以省掉注册窗口类的一整套代码; 窗口不显示、
/// 不泵消息, 只是为了让 vkCreateWin32SurfaceKHR 有个合法句柄。
class FBarrierTestDevice
{
public:
    FBarrierTestDevice() = default;

    ~FBarrierTestDevice()
    {
        Shutdown();
    }

    LIMX_NON_COPYABLE(FBarrierTestDevice);
    LIMX_NON_MOVABLE(FBarrierTestDevice);

    /// 建窗口 → 建设备 (验证层 + 同步验证) → 建命令池与主命令缓冲区
    /// @return true 表示全部就绪; false 表示本机跑不了 GPU 用例
    bool Initialize()
    {
        m_Window = CreateWindowExW(
            0, L"STATIC", L"LimxRHITests",
            WS_OVERLAPPED,
            0, 0, 16, 16,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

        if (m_Window == nullptr)
        {
            return false;
        }

        m_Device = CreateRHIDevice(m_Window,
                                   /* enableValidation     */ true,
                                   /* enableSyncValidation */ true);
        if (!m_Device)
        {
            return false;
        }

        if (m_Device->CreateCommandPool(EQueueType::Graphics, m_Pool)
            != ERHIResult::Success)
        {
            return false;
        }

        if (m_Device->AllocateCommandBuffer(
                m_Pool, ECommandBufferLevel::Primary, m_CommandBufferHandle)
            != ERHIResult::Success)
        {
            return false;
        }

        m_CommandBuffer = CreateRHICommandBuffer(m_Device.Get(),
                                                 m_CommandBufferHandle);
        if (!m_CommandBuffer)
        {
            return false;
        }

        if (m_Device->CreateFence(false, m_Fence) != ERHIResult::Success)
        {
            return false;
        }

        return true;
    }

    void Shutdown()
    {
        if (m_Device)
        {
            m_Device->WaitIdle();

            m_CommandBuffer.Reset();

            if (m_Fence.IsValid())
            {
                m_Device->DestroyFence(m_Fence);
            }

            if (m_CommandBufferHandle.IsValid())
            {
                m_Device->FreeCommandBuffer(m_CommandBufferHandle);
            }

            if (m_Pool.IsValid())
            {
                m_Device->DestroyCommandPool(m_Pool);
            }

            m_Device.Reset();
        }

        if (m_Window != nullptr)
        {
            DestroyWindow(static_cast<HWND>(m_Window));
            m_Window = nullptr;
        }
    }

    LIMX_NODISCARD IRHIDevice& GetDevice() { return *m_Device; }

    LIMX_NODISCARD IRHICommandBuffer& GetCommandBuffer()
    {
        return *m_CommandBuffer;
    }

    /// 提交已录制的命令并等待执行完毕
    LIMX_NODISCARD bool SubmitAndWait()
    {
        m_Device->ResetFence(m_Fence);

        FRHISubmitInfo submitInfo;
        submitInfo.CommandBuffers     = &m_CommandBufferHandle;
        submitInfo.CommandBufferCount = 1;

        if (m_Device->Submit(EQueueType::Graphics, submitInfo, m_Fence)
            != ERHIResult::Success)
        {
            return false;
        }

        // 超时用有限值而非 UINT64_MAX —— 挂死的用例应当失败, 而不是把
        // 整个测试进程一起卡住。10 秒对这几条拷贝命令绰绰有余。
        constexpr UInt64 kTimeoutNanoseconds = 10ULL * 1000 * 1000 * 1000;

        return m_Device->WaitForFence(m_Fence, kTimeoutNanoseconds)
               == ERHIResult::Success;
    }

private:
    void*                          m_Window = nullptr;
    TUniquePtr<IRHIDevice>         m_Device;
    TUniquePtr<IRHICommandBuffer>  m_CommandBuffer;
    FRHICommandPoolHandle          m_Pool;
    FRHICommandBufferHandle        m_CommandBufferHandle;
    FRHIFenceHandle                m_Fence;
};

// ============================================================================
// 辅助
// ============================================================================

/// 第 index 个纹理/缓冲区使用的填充字节 — 各不相同, 便于定位是哪一条错了
LIMX_NODISCARD UInt8 MakeFillByte(UInt32 index)
{
    return static_cast<UInt8>(1 + (index % 250));
}

/// 构造一个把 Undefined 转到目标布局的图像屏障
LIMX_NODISCARD FRHIImageMemoryBarrier MakeImageBarrier(
    FRHITextureHandle texture,
    EImageLayout oldLayout, EImageLayout newLayout,
    EAccessFlags srcAccess, EAccessFlags dstAccess)
{
    FRHIImageMemoryBarrier barrier;
    barrier.Texture         = texture;
    barrier.OldLayout       = oldLayout;
    barrier.NewLayout       = newLayout;
    barrier.SrcAccessMask   = srcAccess;
    barrier.DstAccessMask   = dstAccess;
    barrier.BaseMipLevel    = 0;
    barrier.MipLevelCount   = 1;
    barrier.BaseArrayLayer  = 0;
    barrier.ArrayLayerCount = 1;
    return barrier;
}

/// 构造一个覆盖整个缓冲区的缓冲区屏障
LIMX_NODISCARD FRHIBufferMemoryBarrier MakeBufferBarrier(
    FRHIBufferHandle buffer, UInt64 size,
    EAccessFlags srcAccess, EAccessFlags dstAccess)
{
    FRHIBufferMemoryBarrier barrier;
    barrier.Buffer        = buffer;
    barrier.Offset        = 0;
    barrier.Size          = size;
    barrier.SrcAccessMask = srcAccess;
    barrier.DstAccessMask = dstAccess;
    return barrier;
}

} // namespace

// ============================================================================
// 图像屏障 — 布局状态机判据
// ============================================================================

LIMX_TEST(PipelineBarrier, ImageBarriersBeyondBatchSizeAllTakeEffect)
{
    FBarrierTestDevice harness;
    if (!harness.Initialize())
    {
        LIMX_TEST_SKIP("本机无法创建带验证层的 Vulkan 设备 — 跳过 GPU 用例");
    }

    IRHIDevice& device = harness.GetDevice();

    // ------------------------------------------------------------------
    // 资源: kBarrierCount 张小纹理 + 一个回读缓冲区
    // ------------------------------------------------------------------

    TArray<FRHITextureHandle> textures;
    textures.SetSize(static_cast<SizeType>(kBarrierCount));

    for (UInt32 i = 0; i < kBarrierCount; ++i)
    {
        FRHITextureDesc desc = FRHITextureDesc::Texture2D(
            kTextureSize, kTextureSize, EPixelFormat::RGBA8_UNORM, 1,
            ETextureUsage::TransferDst | ETextureUsage::TransferSrc);
        desc.MemoryUsage = EMemoryUsage::GpuOnly;
        desc.DebugName   = "BarrierTestTexture";

        LIMX_REQUIRE_EQ(
            static_cast<Int32>(device.CreateTexture(desc, textures[i])),
            static_cast<Int32>(ERHIResult::Success));
    }

    const UInt64 texelsPerTexture =
        static_cast<UInt64>(kTextureSize) * kTextureSize;
    const UInt64 bytesPerTexture = texelsPerTexture * kTexturePixelBytes;
    const UInt64 readbackBytes   = bytesPerTexture * kBarrierCount;

    FRHIBufferDesc readbackDesc;
    readbackDesc.Size        = readbackBytes;
    readbackDesc.Usage       = EBufferUsage::TransferDst;
    readbackDesc.MemoryUsage = EMemoryUsage::GpuToCpu;
    readbackDesc.DebugName   = "BarrierTestReadback";

    FRHIBufferHandle readback;
    LIMX_REQUIRE_EQ(
        static_cast<Int32>(device.CreateBuffer(readbackDesc, readback)),
        static_cast<Int32>(ERHIResult::Success));

    // ------------------------------------------------------------------
    // 录制: 一次下发 kBarrierCount 条图像屏障, 随后逐张清除并回读
    //
    // 若实现在第 16 条之后截断, 第 16..38 张纹理就停在 Undefined 布局,
    // 而后续的 ClearColorImage/CopyTextureToBuffer 声称它们处于
    // TransferDst/TransferSrc —— 验证层的布局状态机必然在提交处报错。
    // ------------------------------------------------------------------

    FValidationErrorSink sink;
    FLog::AddSink(&sink);

    IRHICommandBuffer& commandBuffer = harness.GetCommandBuffer();

    TArray<FRHIImageMemoryBarrier> toTransferDst;
    TArray<FRHIImageMemoryBarrier> toTransferSrc;
    toTransferDst.SetSize(static_cast<SizeType>(kBarrierCount));
    toTransferSrc.SetSize(static_cast<SizeType>(kBarrierCount));

    for (UInt32 i = 0; i < kBarrierCount; ++i)
    {
        toTransferDst[i] = MakeImageBarrier(
            textures[i], EImageLayout::Undefined, EImageLayout::TransferDst,
            EAccessFlags::None, EAccessFlags::TransferWrite);

        toTransferSrc[i] = MakeImageBarrier(
            textures[i], EImageLayout::TransferDst, EImageLayout::TransferSrc,
            EAccessFlags::TransferWrite, EAccessFlags::TransferRead);
    }

    commandBuffer.Begin();

    commandBuffer.PipelineBarrier(
        EPipelineStageFlags::TopOfPipe, EPipelineStageFlags::Transfer,
        nullptr, 0,
        nullptr, 0,
        toTransferDst.GetData(), kBarrierCount);

    for (UInt32 i = 0; i < kBarrierCount; ++i)
    {
        const Float32 channel =
            static_cast<Float32>(MakeFillByte(i)) / 255.0f;

        commandBuffer.ClearColorImage(
            textures[i], EImageLayout::TransferDst,
            FLinearColor(channel, channel, channel, channel));
    }

    commandBuffer.PipelineBarrier(
        EPipelineStageFlags::Transfer, EPipelineStageFlags::Transfer,
        nullptr, 0,
        nullptr, 0,
        toTransferSrc.GetData(), kBarrierCount);

    for (UInt32 i = 0; i < kBarrierCount; ++i)
    {
        FRHIBufferTextureCopyRegion region;
        region.BufferOffset  = bytesPerTexture * i;
        region.MipLevel      = 0;
        region.BaseLayer     = 0;
        region.LayerCount    = 1;
        region.TextureOffset = { 0, 0, 0 };
        region.TextureExtent = { kTextureSize, kTextureSize, 1 };

        commandBuffer.CopyTextureToBuffer(
            textures[i], EImageLayout::TransferSrc, readback, region);
    }

    // 回读缓冲区的写入必须对主机可见 —— 这一条同时也覆盖了缓冲区屏障路径
    const FRHIBufferMemoryBarrier toHost = MakeBufferBarrier(
        readback, readbackBytes,
        EAccessFlags::TransferWrite, EAccessFlags::HostRead);

    commandBuffer.PipelineBarrier(
        EPipelineStageFlags::Transfer, EPipelineStageFlags::Host,
        nullptr, 0,
        &toHost, 1,
        nullptr, 0);

    commandBuffer.End();

    const bool isSubmitted = harness.SubmitAndWait();

    const UInt32 errorCount = sink.GetErrorCount();
    const FString firstError = sink.GetFirstError();
    FLog::RemoveSink(&sink);

    LIMX_EXPECT_TRUE(isSubmitted);

    // 主判据 — 一条验证层错误都不该有
    if (errorCount != 0)
    {
        LIMX_TEST_FAIL(StringFormat(
            "下发 {} 条图像屏障后出现 {} 条验证层错误 (首条: {})",
            kBarrierCount, errorCount, firstError.GetCStr()));
    }
    LIMX_EXPECT_EQ(errorCount, 0u);

    // 佐证 — 命令流确实在 GPU 上跑过, 且每张纹理都被清成了自己的颜色
    void* mapped = nullptr;
    if (device.MapBuffer(readback, &mapped) == ERHIResult::Success
        && mapped != nullptr)
    {
        const UInt8* bytes = static_cast<const UInt8*>(mapped);

        UInt32 mismatchCount = 0;
        for (UInt32 i = 0; i < kBarrierCount; ++i)
        {
            const UInt8 expected = MakeFillByte(i);
            for (UInt64 b = 0; b < bytesPerTexture; ++b)
            {
                if (bytes[bytesPerTexture * i + b] != expected)
                {
                    ++mismatchCount;
                    break;
                }
            }
        }

        LIMX_EXPECT_EQ(mismatchCount, 0u);
        device.UnmapBuffer(readback);
    }
    else
    {
        LIMX_TEST_FAIL("回读缓冲区映射失败 — 无法确认命令是否真的执行过");
    }

    LIMX_TEST_INFO("{} 条图像屏障 (批量 {}) 全部生效, 验证层零错误",
                   kBarrierCount, kImplementationBatchSize);

    device.WaitIdle();
    device.DestroyBuffer(readback);
    for (UInt32 i = 0; i < kBarrierCount; ++i)
    {
        device.DestroyTexture(textures[i]);
    }
}

// ============================================================================
// 缓冲区屏障 — 同步验证判据
// ============================================================================

LIMX_TEST(PipelineBarrier, BufferBarriersBeyondBatchSizeAllTakeEffect)
{
    FBarrierTestDevice harness;
    if (!harness.Initialize())
    {
        LIMX_TEST_SKIP("本机无法创建带验证层的 Vulkan 设备 — 跳过 GPU 用例");
    }

    IRHIDevice& device = harness.GetDevice();

    // ------------------------------------------------------------------
    // 资源: 一个源缓冲区 + kBarrierCount 个中转缓冲区 + 一个回读缓冲区
    // ------------------------------------------------------------------

    FRHIBufferDesc stagingDesc;
    stagingDesc.Size        = kBufferBytes;
    stagingDesc.Usage       = EBufferUsage::TransferSrc;
    stagingDesc.MemoryUsage = EMemoryUsage::CpuToGpu;
    stagingDesc.DebugName   = "BarrierTestStaging";

    FRHIBufferHandle staging;
    LIMX_REQUIRE_EQ(
        static_cast<Int32>(device.CreateBuffer(stagingDesc, staging)),
        static_cast<Int32>(ERHIResult::Success));

    void* stagingMemory = nullptr;
    LIMX_REQUIRE_EQ(
        static_cast<Int32>(device.MapBuffer(staging, &stagingMemory)),
        static_cast<Int32>(ERHIResult::Success));
    LIMX_REQUIRE_NOT_NULL(stagingMemory);

    constexpr UInt8 kSourcePattern = 0xA7;
    Memory::MemSet(stagingMemory, kSourcePattern,
                   static_cast<SizeType>(kBufferBytes));
    device.UnmapBuffer(staging);

    TArray<FRHIBufferHandle> transit;
    transit.SetSize(static_cast<SizeType>(kBarrierCount));

    for (UInt32 i = 0; i < kBarrierCount; ++i)
    {
        FRHIBufferDesc desc;
        desc.Size        = kBufferBytes;
        desc.Usage       = EBufferUsage::TransferSrc | EBufferUsage::TransferDst;
        desc.MemoryUsage = EMemoryUsage::GpuOnly;
        desc.DebugName   = "BarrierTestTransit";

        LIMX_REQUIRE_EQ(
            static_cast<Int32>(device.CreateBuffer(desc, transit[i])),
            static_cast<Int32>(ERHIResult::Success));
    }

    const UInt64 readbackBytes = kBufferBytes * kBarrierCount;

    FRHIBufferDesc readbackDesc;
    readbackDesc.Size        = readbackBytes;
    readbackDesc.Usage       = EBufferUsage::TransferDst;
    readbackDesc.MemoryUsage = EMemoryUsage::GpuToCpu;
    readbackDesc.DebugName   = "BarrierTestReadback";

    FRHIBufferHandle readback;
    LIMX_REQUIRE_EQ(
        static_cast<Int32>(device.CreateBuffer(readbackDesc, readback)),
        static_cast<Int32>(ERHIResult::Success));

    // ------------------------------------------------------------------
    // 录制: 写 → 一次下发 kBarrierCount 条缓冲区屏障 → 读
    //
    // 缓冲区没有布局可跟踪, 判据落在同步验证上: 第 i 个中转缓冲区先被
    // TransferWrite 写、再被 TransferRead 读, 中间那条屏障是唯一能把写
    // 变为可见的东西。屏障被截断时 syncval 必然报 READ_AFTER_WRITE。
    // ------------------------------------------------------------------

    FValidationErrorSink sink;
    FLog::AddSink(&sink);

    IRHICommandBuffer& commandBuffer = harness.GetCommandBuffer();

    TArray<FRHIBufferMemoryBarrier> barriers;
    barriers.SetSize(static_cast<SizeType>(kBarrierCount));

    for (UInt32 i = 0; i < kBarrierCount; ++i)
    {
        barriers[i] = MakeBufferBarrier(
            transit[i], kBufferBytes,
            EAccessFlags::TransferWrite, EAccessFlags::TransferRead);
    }

    commandBuffer.Begin();

    for (UInt32 i = 0; i < kBarrierCount; ++i)
    {
        FRHIBufferCopyRegion region;
        region.SrcOffset = 0;
        region.DstOffset = 0;
        region.Size      = kBufferBytes;

        commandBuffer.CopyBuffer(staging, transit[i], region);
    }

    commandBuffer.PipelineBarrier(
        EPipelineStageFlags::Transfer, EPipelineStageFlags::Transfer,
        nullptr, 0,
        barriers.GetData(), kBarrierCount,
        nullptr, 0);

    for (UInt32 i = 0; i < kBarrierCount; ++i)
    {
        FRHIBufferCopyRegion region;
        region.SrcOffset = 0;
        region.DstOffset = kBufferBytes * i;
        region.Size      = kBufferBytes;

        commandBuffer.CopyBuffer(transit[i], readback, region);
    }

    const FRHIBufferMemoryBarrier toHost = MakeBufferBarrier(
        readback, readbackBytes,
        EAccessFlags::TransferWrite, EAccessFlags::HostRead);

    commandBuffer.PipelineBarrier(
        EPipelineStageFlags::Transfer, EPipelineStageFlags::Host,
        nullptr, 0,
        &toHost, 1,
        nullptr, 0);

    commandBuffer.End();

    const bool isSubmitted = harness.SubmitAndWait();

    const UInt32 errorCount = sink.GetErrorCount();
    const FString firstError = sink.GetFirstError();
    FLog::RemoveSink(&sink);

    LIMX_EXPECT_TRUE(isSubmitted);

    if (errorCount != 0)
    {
        LIMX_TEST_FAIL(StringFormat(
            "下发 {} 条缓冲区屏障后出现 {} 条验证层错误 (首条: {})",
            kBarrierCount, errorCount, firstError.GetCStr()));
    }
    LIMX_EXPECT_EQ(errorCount, 0u);

    void* mapped = nullptr;
    if (device.MapBuffer(readback, &mapped) == ERHIResult::Success
        && mapped != nullptr)
    {
        const UInt8* bytes = static_cast<const UInt8*>(mapped);

        UInt32 mismatchCount = 0;
        for (UInt64 b = 0; b < readbackBytes; ++b)
        {
            if (bytes[b] != kSourcePattern)
            {
                ++mismatchCount;
                break;
            }
        }

        LIMX_EXPECT_EQ(mismatchCount, 0u);
        device.UnmapBuffer(readback);
    }
    else
    {
        LIMX_TEST_FAIL("回读缓冲区映射失败 — 无法确认命令是否真的执行过");
    }

    LIMX_TEST_INFO("{} 条缓冲区屏障 (批量 {}) 全部生效, 同步验证零告警",
                   kBarrierCount, kImplementationBatchSize);

    device.WaitIdle();
    device.DestroyBuffer(readback);
    device.DestroyBuffer(staging);
    for (UInt32 i = 0; i < kBarrierCount; ++i)
    {
        device.DestroyBuffer(transit[i]);
    }
}

// ============================================================================
// 全局内存屏障 — 关键的那一条排在批量边界之外
// ============================================================================

LIMX_TEST(PipelineBarrier, MemoryBarrierBeyondBatchSizeStillTakesEffect)
{
    FBarrierTestDevice harness;
    if (!harness.Initialize())
    {
        LIMX_TEST_SKIP("本机无法创建带验证层的 Vulkan 设备 — 跳过 GPU 用例");
    }

    IRHIDevice& device = harness.GetDevice();

    // ------------------------------------------------------------------
    // 资源: 源 → 中转 → 回读, 各一个
    // ------------------------------------------------------------------

    FRHIBufferDesc stagingDesc;
    stagingDesc.Size        = kBufferBytes;
    stagingDesc.Usage       = EBufferUsage::TransferSrc;
    stagingDesc.MemoryUsage = EMemoryUsage::CpuToGpu;
    stagingDesc.DebugName   = "BarrierTestStaging";

    FRHIBufferHandle staging;
    LIMX_REQUIRE_EQ(
        static_cast<Int32>(device.CreateBuffer(stagingDesc, staging)),
        static_cast<Int32>(ERHIResult::Success));

    void* stagingMemory = nullptr;
    LIMX_REQUIRE_EQ(
        static_cast<Int32>(device.MapBuffer(staging, &stagingMemory)),
        static_cast<Int32>(ERHIResult::Success));
    LIMX_REQUIRE_NOT_NULL(stagingMemory);

    constexpr UInt8 kSourcePattern = 0x5C;
    Memory::MemSet(stagingMemory, kSourcePattern,
                   static_cast<SizeType>(kBufferBytes));
    device.UnmapBuffer(staging);

    FRHIBufferDesc transitDesc;
    transitDesc.Size        = kBufferBytes;
    transitDesc.Usage       = EBufferUsage::TransferSrc | EBufferUsage::TransferDst;
    transitDesc.MemoryUsage = EMemoryUsage::GpuOnly;
    transitDesc.DebugName   = "BarrierTestTransit";

    FRHIBufferHandle transit;
    LIMX_REQUIRE_EQ(
        static_cast<Int32>(device.CreateBuffer(transitDesc, transit)),
        static_cast<Int32>(ERHIResult::Success));

    FRHIBufferDesc readbackDesc;
    readbackDesc.Size        = kBufferBytes;
    readbackDesc.Usage       = EBufferUsage::TransferDst;
    readbackDesc.MemoryUsage = EMemoryUsage::GpuToCpu;
    readbackDesc.DebugName   = "BarrierTestReadback";

    FRHIBufferHandle readback;
    LIMX_REQUIRE_EQ(
        static_cast<Int32>(device.CreateBuffer(readbackDesc, readback)),
        static_cast<Int32>(ERHIResult::Success));

    // ------------------------------------------------------------------
    // 录制
    //
    // 全局内存屏障是幂等的: 任意一条覆盖了 TransferWrite→TransferRead
    // 就把整个冒险解决掉了, 所以"下发了 17 条还是只下发了 16 条"本身并
    // 不可观测。这里把唯一有效的那一条放在下标 kBarrierCount-1 (远超批量
    // 边界), 前面全部填成 TransferRead→TransferRead 的诱饵 —— 诱饵不会
    // 让前一次写变为可用, 因此只有最后那一条真的到达驱动时冒险才消失。
    // ------------------------------------------------------------------

    FValidationErrorSink sink;
    FLog::AddSink(&sink);

    IRHICommandBuffer& commandBuffer = harness.GetCommandBuffer();

    TArray<FRHIMemoryBarrier> memoryBarriers;
    memoryBarriers.SetSize(static_cast<SizeType>(kBarrierCount));

    for (UInt32 i = 0; i < kBarrierCount; ++i)
    {
        memoryBarriers[i].SrcAccessMask = EAccessFlags::TransferRead;
        memoryBarriers[i].DstAccessMask = EAccessFlags::TransferRead;
    }

    memoryBarriers[kBarrierCount - 1].SrcAccessMask =
        EAccessFlags::TransferWrite;
    memoryBarriers[kBarrierCount - 1].DstAccessMask =
        EAccessFlags::TransferRead;

    commandBuffer.Begin();

    FRHIBufferCopyRegion writeRegion;
    writeRegion.SrcOffset = 0;
    writeRegion.DstOffset = 0;
    writeRegion.Size      = kBufferBytes;
    commandBuffer.CopyBuffer(staging, transit, writeRegion);

    commandBuffer.PipelineBarrier(
        EPipelineStageFlags::Transfer, EPipelineStageFlags::Transfer,
        memoryBarriers.GetData(), kBarrierCount,
        nullptr, 0,
        nullptr, 0);

    FRHIBufferCopyRegion readRegion;
    readRegion.SrcOffset = 0;
    readRegion.DstOffset = 0;
    readRegion.Size      = kBufferBytes;
    commandBuffer.CopyBuffer(transit, readback, readRegion);

    const FRHIBufferMemoryBarrier toHost = MakeBufferBarrier(
        readback, kBufferBytes,
        EAccessFlags::TransferWrite, EAccessFlags::HostRead);

    commandBuffer.PipelineBarrier(
        EPipelineStageFlags::Transfer, EPipelineStageFlags::Host,
        nullptr, 0,
        &toHost, 1,
        nullptr, 0);

    commandBuffer.End();

    const bool isSubmitted = harness.SubmitAndWait();

    const UInt32 errorCount = sink.GetErrorCount();
    const FString firstError = sink.GetFirstError();
    FLog::RemoveSink(&sink);

    LIMX_EXPECT_TRUE(isSubmitted);

    if (errorCount != 0)
    {
        LIMX_TEST_FAIL(StringFormat(
            "关键内存屏障位于下标 {} 时出现 {} 条验证层错误 (首条: {})",
            kBarrierCount - 1, errorCount, firstError.GetCStr()));
    }
    LIMX_EXPECT_EQ(errorCount, 0u);

    void* mapped = nullptr;
    if (device.MapBuffer(readback, &mapped) == ERHIResult::Success
        && mapped != nullptr)
    {
        const UInt8* bytes = static_cast<const UInt8*>(mapped);

        UInt32 mismatchCount = 0;
        for (UInt64 b = 0; b < kBufferBytes; ++b)
        {
            if (bytes[b] != kSourcePattern)
            {
                ++mismatchCount;
                break;
            }
        }

        LIMX_EXPECT_EQ(mismatchCount, 0u);
        device.UnmapBuffer(readback);
    }
    else
    {
        LIMX_TEST_FAIL("回读缓冲区映射失败 — 无法确认命令是否真的执行过");
    }

    LIMX_TEST_INFO("关键内存屏障位于下标 {} (批量 {}) 仍然生效",
                   kBarrierCount - 1, kImplementationBatchSize);

    device.WaitIdle();
    device.DestroyBuffer(readback);
    device.DestroyBuffer(transit);
    device.DestroyBuffer(staging);
}

// ============================================================================
// 零屏障 — 纯执行依赖仍须下发
// ============================================================================

LIMX_TEST(PipelineBarrier, PureExecutionBarrierIsStillIssued)
{
    FBarrierTestDevice harness;
    if (!harness.Initialize())
    {
        LIMX_TEST_SKIP("本机无法创建带验证层的 Vulkan 设备 — 跳过 GPU 用例");
    }

    IRHIDevice& device = harness.GetDevice();

    // ------------------------------------------------------------------
    // 三个计数全为 0 的调用不是空操作: 它建立 srcStage → dstStage 的执行
    // 依赖。分批下发的实现很容易把这条语义丢掉 —— 循环条件只看"还有没有
    // 剩余屏障"的话, 零屏障时一次都不会进循环。
    //
    // 判据用写后读之外的另一种冒险: 写后**读**需要内存依赖, 而写后**写**
    // 之前的那种"读完再写"(WAR) 只需要执行依赖。因此下面这段里,
    // 纯执行屏障是唯一能消除冒险的东西, 少下发就必然被同步验证抓到。
    // ------------------------------------------------------------------

    FRHIBufferDesc desc;
    desc.Size        = kBufferBytes;
    desc.Usage       = EBufferUsage::TransferSrc | EBufferUsage::TransferDst;
    desc.MemoryUsage = EMemoryUsage::GpuOnly;
    desc.DebugName   = "BarrierTestWarBuffer";

    FRHIBufferHandle readThenWritten;
    LIMX_REQUIRE_EQ(
        static_cast<Int32>(device.CreateBuffer(desc, readThenWritten)),
        static_cast<Int32>(ERHIResult::Success));

    FRHIBufferHandle destination;
    LIMX_REQUIRE_EQ(
        static_cast<Int32>(device.CreateBuffer(desc, destination)),
        static_cast<Int32>(ERHIResult::Success));

    FRHIBufferHandle source;
    LIMX_REQUIRE_EQ(
        static_cast<Int32>(device.CreateBuffer(desc, source)),
        static_cast<Int32>(ERHIResult::Success));

    FValidationErrorSink sink;
    FLog::AddSink(&sink);

    IRHICommandBuffer& commandBuffer = harness.GetCommandBuffer();

    FRHIBufferCopyRegion region;
    region.SrcOffset = 0;
    region.DstOffset = 0;
    region.Size      = kBufferBytes;

    commandBuffer.Begin();

    // 读 readThenWritten
    commandBuffer.CopyBuffer(readThenWritten, destination, region);

    // 纯执行依赖 — 三个计数全为 0
    commandBuffer.PipelineBarrier(
        EPipelineStageFlags::Transfer, EPipelineStageFlags::Transfer,
        nullptr, 0,
        nullptr, 0,
        nullptr, 0);

    // 写 readThenWritten — 与上面那次读构成 WAR 冒险
    commandBuffer.CopyBuffer(source, readThenWritten, region);

    commandBuffer.End();

    const bool isSubmitted = harness.SubmitAndWait();

    const UInt32 errorCount = sink.GetErrorCount();
    const FString firstError = sink.GetFirstError();
    FLog::RemoveSink(&sink);

    LIMX_EXPECT_TRUE(isSubmitted);

    if (errorCount != 0)
    {
        LIMX_TEST_FAIL(StringFormat(
            "零屏障的纯执行依赖未生效, 出现 {} 条验证层错误 (首条: {})",
            errorCount, firstError.GetCStr()));
    }
    LIMX_EXPECT_EQ(errorCount, 0u);

    LIMX_TEST_INFO("零屏障调用仍建立执行依赖, WAR 冒险已消除");

    device.WaitIdle();
    device.DestroyBuffer(source);
    device.DestroyBuffer(destination);
    device.DestroyBuffer(readThenWritten);
}

// ============================================================================
// 次级命令缓冲区 — 无效句柄必须出声, 不能默默跳过
// ============================================================================

LIMX_TEST(ExecuteCommands, InvalidHandleIsReportedNotSkipped)
{
    FBarrierTestDevice harness;
    if (!harness.Initialize())
    {
        LIMX_TEST_SKIP("本机无法创建带验证层的 Vulkan 设备 — 跳过 GPU 用例");
    }

    // ------------------------------------------------------------------
    // 无效句柄意味着那一段次级缓冲区录下的命令根本不会执行 —— 结果是凭空
    // 少画一批东西, 而 API 层面看不出任何异常。跳过时必须留下记录, 否则
    // 又是一处"失败落在通过上"。
    //
    // 这里三个句柄全部无效, 因此不会有任何 vkCmdExecuteCommands 发出,
    // 验证层也没有可报的东西 —— 计到的三条错误只能来自 RHI 自己。
    // ------------------------------------------------------------------

    constexpr UInt32 kInvalidCount = 3;
    FRHICommandBufferHandle invalidHandles[kInvalidCount];

    FValidationErrorSink sink;
    FLog::AddSink(&sink);

    IRHICommandBuffer& commandBuffer = harness.GetCommandBuffer();

    commandBuffer.Begin();
    commandBuffer.ExecuteCommands(invalidHandles, kInvalidCount);
    commandBuffer.End();

    const UInt32 errorCount = sink.GetErrorCount();
    FLog::RemoveSink(&sink);

    LIMX_EXPECT_EQ(errorCount, kInvalidCount);

    LIMX_TEST_INFO("{} 个无效次级句柄各留下一条错误日志", kInvalidCount);

    harness.GetDevice().WaitIdle();
}
