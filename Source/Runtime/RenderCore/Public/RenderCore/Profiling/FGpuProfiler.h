/*******************************************************************************
 * 文件: FGpuProfiler.h
 * 创建时间: 2026-08-30
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   GPU 逐 Pass 计时 — 基于 Vulkan 时间戳查询
 *   在命令缓冲区里打时间戳, 若干帧之后非阻塞回读, 换算成毫秒
 *   同时记录整帧的起止, 使"各 Pass 之和"与"整帧"可以互相印证
 *
 * 设计哲学:
 *   永不等待 GPU — 回读的是 kFrameSlots 帧之前的那一组结果。传 wait=true
 *     量到的会是"CPU 等 GPU"而不是"GPU 干活", 那个数字毫无意义且会自己
 *     制造出它想测量的停顿。
 *   独立的整帧计时 — 整帧不是各 Pass 之和, 而是单独一对时间戳。两者的
 *     差额就是没有被埋点的部分, 这使"是否漏埋"成为可测量的事实而非假设。
 *   缺失即显式 — 结果未就绪时返回上一次的有效值并标记 IsStale, 而不是
 *     悄悄返回 0。0 毫秒看起来像"这个 Pass 很快", 是最坏的一种谎报。
 *
 * 技术特性:
 *   - 环形帧槽位: 每槽 2 + 2 * kMaxScopes 个时间戳
 *   - 时间戳有效位掩码: 少于 64 位时高位未定义, 必须先掩再减
 *   - tick → 纳秒: 乘 timestampPeriod (各家硬件差别很大, NVIDIA 常为 1.0,
 *     AMD 常为 40 左右, 绝不能假定 tick 即纳秒)
 *   - 作用域嵌套不支持: 逐 Pass 计时是平铺的, 嵌套会让"之和 vs 整帧"
 *     的印证失去意义
 *
 * 依赖关系:
 *   内部: RHI/RHI/IRHIDevice.h, RHI/RHI/IRHICommandBuffer.h,
 *          Core/Containers/TFixedArray.h
 *
 ******************************************************************************/

#pragma once

#include "RenderCore/RenderCoreAPI.h"

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "RHI/RHI/IRHIDevice.h"
#include "RHI/RHI/IRHICommandBuffer.h"

namespace Limx
{

// ============================================================================
// FGpuScopeResult — 单个作用域的计时结果
// ============================================================================

struct FGpuScopeResult
{
    /// 作用域名称 — 指向 Pass 的静态字符串, 不拷贝
    const AnsiChar* Name = nullptr;

    /// GPU 耗时 (毫秒)
    Float64 Milliseconds = 0.0;
};

// ============================================================================
// FGpuProfiler
// ============================================================================

/// GPU 逐 Pass 计时器
///
/// 用法 (每帧):
///   profiler.BeginFrame(commandBuffer, frameIndex);
///     profiler.BeginScope(commandBuffer, "ShadowPass");
///     ... 录制该 Pass 的命令 ...
///     profiler.EndScope(commandBuffer);
///   profiler.EndFrame(commandBuffer);
///
/// 结果在若干帧之后才可用, 通过 GetScopeCount / GetScope / GetFrameMilliseconds
/// 读取。
class LIMX_RENDERCORE_API FGpuProfiler
{
public:
    /// 单帧最多多少个作用域
    static constexpr UInt32 kMaxScopes = 32;

    /// 环形槽位数 — 必须 >= 在飞帧数, 否则会读到尚未完成的那一帧
    ///
    /// 取 4 而非在飞帧数 (通常 2~3): 多一个槽位的成本是几十个 UInt64,
    /// 而少一个槽位的后果是回读到正在写的槽, 得到的数字似是而非。
    static constexpr UInt32 kFrameSlots = 4;

    /// 每槽时间戳数: 整帧一对 + 每作用域一对
    static constexpr UInt32 kTimestampsPerSlot = 2 + 2 * kMaxScopes;

    FGpuProfiler() = default;
    ~FGpuProfiler() = default;

    LIMX_NON_COPYABLE(FGpuProfiler);

    // ========================================================================
    // 生命周期
    // ========================================================================

    /// 创建查询池并取得时间戳换算参数
    ///
    /// 硬件不支持时间戳 (有效位数为 0) 时返回 Success 但整体停用 —— 计时
    /// 缺失不该让引擎起不来。IsSupported() 会如实返回 false。
    ERHIResult Initialize(IRHIDevice* device);

    void Shutdown(IRHIDevice* device);

    /// 硬件是否支持时间戳查询
    LIMX_NODISCARD bool IsSupported() const { return m_Supported; }

    // ========================================================================
    // 逐帧录制
    // ========================================================================

    /// 开始一帧 — 重置本槽位的查询并打下整帧起点
    ///
    /// 必须在任何 RenderPass 之外调用: vkCmdResetQueryPool 不允许出现在
    /// 渲染通道内部。
    void BeginFrame(IRHICommandBuffer* commandBuffer, UInt64 frameNumber);

    /// 开始一个作用域
    ///
    /// @param name 静态字符串 (通常是 IRenderPass::GetName() 的返回值)
    void BeginScope(IRHICommandBuffer* commandBuffer, const AnsiChar* name);

    /// 结束当前作用域
    void EndScope(IRHICommandBuffer* commandBuffer);

    /// 结束一帧 — 打下整帧终点, 并尝试回读最老那一槽
    void EndFrame(IRHICommandBuffer* commandBuffer, IRHIDevice* device);

    // ========================================================================
    // 结果
    // ========================================================================

    /// 最近一次成功回读的作用域个数
    LIMX_NODISCARD UInt32 GetScopeCount() const { return m_ResultCount; }

    /// 第 index 个作用域的结果
    LIMX_NODISCARD const FGpuScopeResult& GetScope(UInt32 index) const
    {
        return m_Results[index < m_ResultCount ? index : 0];
    }

    /// 整帧 GPU 耗时 (毫秒) — 独立测量, 不是各作用域之和
    LIMX_NODISCARD Float64 GetFrameMilliseconds() const
    {
        return m_FrameMilliseconds;
    }

    /// 各作用域之和 (毫秒)
    ///
    /// 与 GetFrameMilliseconds() 的差额即未被埋点的部分。两者相差过大
    /// 说明有 Pass 漏埋, 或者存在 Pass 之外的 GPU 工作。
    LIMX_NODISCARD Float64 GetScopeSumMilliseconds() const;

    /// 结果是否陈旧 (本帧未取到新数据, 沿用上一次)
    ///
    /// 起始几帧必然为 true —— 查询结果还没走完流水线。
    LIMX_NODISCARD bool IsStale() const { return m_Stale; }

    /// 累计成功回读的帧数 — 用于判断数据是否已经稳定
    LIMX_NODISCARD UInt64 GetResolvedFrameCount() const
    {
        return m_ResolvedFrames;
    }

private:
    /// 把 tick 差换算成毫秒
    LIMX_NODISCARD Float64 TicksToMilliseconds(UInt64 begin, UInt64 end) const;

    /// 槽位内第 index 个时间戳的全局查询下标
    LIMX_NODISCARD UInt32 QueryIndex(UInt32 slot, UInt32 index) const
    {
        return slot * kTimestampsPerSlot + index;
    }

    // ------------------------------------------------------------------------
    // 一个槽位的记账
    // ------------------------------------------------------------------------

    struct FSlot
    {
        /// 本槽记录的帧号 (kInvalidFrame 表示尚未使用)
        UInt64 FrameNumber = kInvalidFrame;

        /// 本槽实际写入的作用域个数
        UInt32 ScopeCount = 0;

        /// 各作用域名称
        const AnsiChar* Names[kMaxScopes] = {};
    };

    static constexpr UInt64 kInvalidFrame = ~static_cast<UInt64>(0);

    FRHIQueryPoolHandle m_QueryPool;

    FSlot   m_Slots[kFrameSlots];
    UInt32  m_CurrentSlot   = 0;
    UInt32  m_CurrentScope  = 0;
    Int32   m_OpenScope     = -1;

    bool    m_Supported     = false;
    Float32 m_Period        = 0.0f;
    UInt64  m_ValidMask     = ~static_cast<UInt64>(0);

    FGpuScopeResult m_Results[kMaxScopes];
    UInt32          m_ResultCount      = 0;
    Float64         m_FrameMilliseconds = 0.0;
    bool            m_Stale            = true;
    UInt64          m_ResolvedFrames   = 0;
};

} // namespace Limx
