/*******************************************************************************
 * 文件: FRHITestDevice.h
 * 创建时间: 2026-08-30
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   GPU 用例的共用脚手架 — 隐藏窗口 + 真实 VkDevice + 命令池 + 栅栏,
 *   外加一个统计验证层错误的日志 Sink。
 *
 * 设计哲学:
 *   RHI 的同步与批量语义只有在真实设备上才谈得上验证, 而"跑一遍看结果对不对"
 *   在这类缺陷上恰恰无效 —— 少下发一个屏障、少提交一个命令缓冲区, 在绝大多数
 *   驱动上照样能跑出正确画面。因此脚手架同时提供两条判据通道:
 *
 *     验证层错误计数 —— 图像布局状态机与同步验证都是层内的软件模型, 结论与
 *       驱动、与时序无关, 是确定性的。设备因此固定以同步验证创建。
 *
 *     真实数据回读   —— 每个用例都把 GPU 写出的字节读回来逐个比对。它抓不住
 *       所有截断, 但能证明"这条命令流确实在 GPU 上跑过了", 避免一个什么都
 *       没执行的用例显示通过。
 *
 *   窗口用预定义的 "STATIC" 类, 省掉注册窗口类的一整套代码; 它不显示、不泵
 *   消息, 只是为了让 vkCreateWin32SurfaceKHR 有个合法句柄 —— 设备初始化链路
 *   必经 VkSurfaceKHR。
 *
 * 依赖关系:
 *   内部: RHITests/RHITestsMinimal.h (RHI 设备工厂与命令缓冲区接口)
 *   外部: Vulkan 运行时 + VK_LAYER_KHRONOS_validation; user32 (隐藏窗口)
 *
 * 注意事项:
 *   无 GPU / 无验证层的机器上 Initialize 返回 false, 用例应当跳过而非失败。
 *   跳过会在报告里单列, 不计入通过 —— 但也意味着那台机器上这组用例没有保护,
 *   变异验证必须在真的建得起设备的机器上做。
 *
 ******************************************************************************/

#pragma once

#include "RHITests/RHITestsMinimal.h"

namespace Limx
{
namespace RHITesting
{

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
// FRHITestDevice — 隐藏窗口 + RHI 设备 + 命令池的 RAII 组合
// ============================================================================

class FRHITestDevice
{
public:
    FRHITestDevice() = default;

    ~FRHITestDevice()
    {
        Shutdown();
    }

    LIMX_NON_COPYABLE(FRHITestDevice);
    LIMX_NON_MOVABLE(FRHITestDevice);

    /// 建窗口 → 建设备 (验证层 + 同步验证) → 建命令池、主命令缓冲区与栅栏
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

    LIMX_NODISCARD FRHICommandPoolHandle GetCommandPool() const
    {
        return m_Pool;
    }

    LIMX_NODISCARD IRHICommandBuffer& GetCommandBuffer()
    {
        return *m_CommandBuffer;
    }

    LIMX_NODISCARD FRHICommandBufferHandle GetCommandBufferHandle() const
    {
        return m_CommandBufferHandle;
    }

    /// 提交任意 FRHISubmitInfo 并等待内置栅栏
    ///
    /// @return Success 提交成功且栅栏在超时前触发;
    ///         Timeout 提交成功但 GPU 没能在超时内完成 —— 通常意味着队列上
    ///                 留着一个永远等不到的提交, 调用方必须自行补救,
    ///                 否则析构里的 vkDeviceWaitIdle 会挂死整个测试进程;
    ///         其它值  提交本身失败。
    LIMX_NODISCARD ERHIResult SubmitAndWaitResult(
        const FRHISubmitInfo& submitInfo)
    {
        m_Device->ResetFence(m_Fence);

        const ERHIResult submitResult =
            m_Device->Submit(EQueueType::Graphics, submitInfo, m_Fence);

        if (submitResult != ERHIResult::Success)
        {
            return submitResult;
        }

        // 超时用有限值而非 UINT64_MAX —— 挂死的用例应当失败, 而不是把整个
        // 测试进程一起卡住。10 秒对这些拷贝命令绰绰有余。
        constexpr UInt64 kTimeoutNanoseconds = 10ULL * 1000 * 1000 * 1000;

        return m_Device->WaitForFence(m_Fence, kTimeoutNanoseconds);
    }

    /// 提交一组命令缓冲区并等待完成
    LIMX_NODISCARD bool SubmitAndWait(const FRHICommandBufferHandle* buffers,
                                      UInt32 count)
    {
        FRHISubmitInfo submitInfo;
        submitInfo.CommandBuffers     = buffers;
        submitInfo.CommandBufferCount = count;

        return SubmitAndWaitResult(submitInfo) == ERHIResult::Success;
    }

    /// 提交内置的那一个命令缓冲区并等待完成
    LIMX_NODISCARD bool SubmitAndWait()
    {
        return SubmitAndWait(&m_CommandBufferHandle, 1);
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
// 共用辅助
// ============================================================================

/// 第 index 个资源使用的填充字节 — 各不相同, 便于定位是哪一条错了
LIMX_NODISCARD inline UInt8 MakeFillByte(UInt32 index)
{
    return static_cast<UInt8>(1 + (index % 250));
}

/// 构造一个图像屏障
LIMX_NODISCARD inline FRHIImageMemoryBarrier MakeImageBarrier(
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
LIMX_NODISCARD inline FRHIBufferMemoryBarrier MakeBufferBarrier(
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

} // namespace RHITesting
} // namespace Limx
