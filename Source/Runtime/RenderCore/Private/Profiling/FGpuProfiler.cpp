/*******************************************************************************
 * 文件: FGpuProfiler.cpp
 * 创建时间: 2026-08-30
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   FGpuProfiler 的实现 — 时间戳查询池的创建、录制与非阻塞回读
 *
 ******************************************************************************/

#include "RenderCore/Profiling/FGpuProfiler.h"

#include "Core/Logging/FLog.h"

namespace Limx
{

LIMX_DECLARE_LOG_CATEGORY(LogGpuProfiler)
LIMX_DEFINE_LOG_CATEGORY(LogGpuProfiler)

// ============================================================================
// 生命周期
// ============================================================================

ERHIResult FGpuProfiler::Initialize(IRHIDevice* device)
{
    if (device == nullptr)
    {
        return ERHIResult::ErrorInvalidParameter;
    }

    const UInt32 validBits = device->GetTimestampValidBits();
    m_Period = device->GetTimestampPeriod();

    // 有效位数为 0 表示该队列族根本不支持时间戳; 周期为 0 同理。
    //
    // 这两种情况下停用计时而非报错 —— 引擎没有计时也能跑, 而因为拿不到
    // 性能数字就起不来是本末倒置。
    if (validBits == 0 || m_Period <= 0.0f)
    {
        m_Supported = false;

        LIMX_LOG(LogGpuProfiler, Warning,
                 "[GPU 计时] 硬件不支持时间戳查询 (有效位 {}, 周期 {}) — 已停用",
                 validBits, m_Period);

        return ERHIResult::Success;
    }

    // 有效位数少于 64 时高位是未定义的, 必须先掩掉再做差。
    //
    // 不掩的后果不是"略有偏差": 跨越回绕点的那一帧, 两个时间戳的差以无
    // 符号解释会变成一个天文数字, 表现为某一帧的某个 Pass 突然显示几百万
    // 毫秒。那种离群值一眼能看出来, 但它同时会污染任何平均值。
    m_ValidMask = MakeTimestampMask(validBits);

    FRHIQueryPoolDesc desc;
    desc.Type       = EQueryType::Timestamp;
    desc.QueryCount = kFrameSlots * kTimestampsPerSlot;

    const ERHIResult result = device->CreateQueryPool(desc, m_QueryPool);

    if (result != ERHIResult::Success)
    {
        m_Supported = false;
        return result;
    }

    for (UInt32 i = 0; i < kFrameSlots; ++i)
    {
        m_Slots[i] = FSlot();
    }

    m_Supported = true;

    LIMX_LOG(LogGpuProfiler, Display,
             "[GPU 计时] 已就绪 — 周期 {} ns/tick, 有效位 {}, 查询池 {} 项",
             m_Period, validBits, desc.QueryCount);

    return ERHIResult::Success;
}

void FGpuProfiler::Shutdown(IRHIDevice* device)
{
    if (device != nullptr && m_QueryPool.IsValid())
    {
        device->DestroyQueryPool(m_QueryPool);
    }

    m_Supported = false;
}

// ============================================================================
// 逐帧录制
// ============================================================================

void FGpuProfiler::BeginFrame(IRHICommandBuffer* commandBuffer,
                              UInt64             frameNumber)
{
    if (!m_Supported || commandBuffer == nullptr)
    {
        return;
    }

    m_CurrentSlot  = static_cast<UInt32>(frameNumber % kFrameSlots);
    m_CurrentScope = 0;
    m_OpenScope    = -1;

    FSlot& slot = m_Slots[m_CurrentSlot];
    slot.FrameNumber = frameNumber;
    slot.ScopeCount  = 0;

    // 重置本槽位的全部查询。
    //
    // 必须在渲染通道之外 —— vkCmdResetQueryPool 不允许出现在 RenderPass
    // 内部, 验证层会直接报错。
    commandBuffer->ResetQueryPool(m_QueryPool,
                                  QueryIndex(m_CurrentSlot, 0),
                                  kTimestampsPerSlot);

    // 整帧起点。用 BottomOfPipe 而非 TopOfPipe: 前者表示"此前提交的工作
    // 全部完成", 语义明确; 后者只表示命令被取走, 与实际执行无关。
    commandBuffer->WriteTimestamp(EPipelineStageFlags::BottomOfPipe,
                                  m_QueryPool,
                                  QueryIndex(m_CurrentSlot, 0));
}

void FGpuProfiler::BeginScope(IRHICommandBuffer* commandBuffer,
                              const AnsiChar*    name)
{
    if (!m_Supported || commandBuffer == nullptr)
    {
        return;
    }

    if (m_CurrentScope >= kMaxScopes)
    {
        // 超出容量就不再记 —— 但要让它可被发现, 而不是悄悄丢弃
        LIMX_LOG(LogGpuProfiler, Warning,
                 "[GPU 计时] 作用域超过上限 {}, '{}' 未被计时",
                 kMaxScopes, (name != nullptr) ? name : "?");
        return;
    }

    LIMX_ASSERT(m_OpenScope < 0);

    m_OpenScope = static_cast<Int32>(m_CurrentScope);

    m_Slots[m_CurrentSlot].Names[m_CurrentScope] = name;

    commandBuffer->WriteTimestamp(
        EPipelineStageFlags::BottomOfPipe,
        m_QueryPool,
        QueryIndex(m_CurrentSlot, 2 + 2 * m_CurrentScope));
}

void FGpuProfiler::EndScope(IRHICommandBuffer* commandBuffer)
{
    if (!m_Supported || commandBuffer == nullptr || m_OpenScope < 0)
    {
        return;
    }

    const UInt32 scope = static_cast<UInt32>(m_OpenScope);

    commandBuffer->WriteTimestamp(
        EPipelineStageFlags::BottomOfPipe,
        m_QueryPool,
        QueryIndex(m_CurrentSlot, 3 + 2 * scope));

    m_OpenScope = -1;
    m_CurrentScope = scope + 1;
    m_Slots[m_CurrentSlot].ScopeCount = m_CurrentScope;
}

void FGpuProfiler::EndFrame(IRHICommandBuffer* commandBuffer,
                            IRHIDevice*        device)
{
    if (!m_Supported || commandBuffer == nullptr || device == nullptr)
    {
        return;
    }

    LIMX_ASSERT(m_OpenScope < 0);

    commandBuffer->WriteTimestamp(EPipelineStageFlags::BottomOfPipe,
                                  m_QueryPool,
                                  QueryIndex(m_CurrentSlot, 1));

    // ---- 回读最老的那一槽 ----
    //
    // 读的是 kFrameSlots-1 帧之前录的那一组, 此刻它极大概率已经完成。
    // 传 wait=false: 未完成就跳过这一帧的更新, 绝不阻塞 —— 一旦阻塞,
    // 量到的就是"CPU 等 GPU", 而那个等待本身正是由测量制造出来的。
    const UInt32 oldestSlot = (m_CurrentSlot + 1) % kFrameSlots;
    FSlot&       slot       = m_Slots[oldestSlot];

    m_Stale = true;

    if (slot.FrameNumber == kInvalidFrame || slot.ScopeCount == 0)
    {
        return;
    }

    UInt64 raw[kTimestampsPerSlot] = {};

    const UInt32 needed = 2 + 2 * slot.ScopeCount;

    const ERHIResult result = device->GetQueryResults(
        m_QueryPool,
        QueryIndex(oldestSlot, 0),
        needed,
        raw,
        false);

    // 注意不能用 IsRHISuccess —— NotReady 也是非负值, 会被判成成功。
    if (result != ERHIResult::Success)
    {
        return;
    }

    m_FrameMilliseconds = TicksToMilliseconds(raw[0], raw[1]);

    m_ResultCount = slot.ScopeCount;

    for (UInt32 i = 0; i < slot.ScopeCount; ++i)
    {
        m_Results[i].Name = slot.Names[i];
        m_Results[i].Milliseconds =
            TicksToMilliseconds(raw[2 + 2 * i], raw[3 + 2 * i]);
    }

    m_Stale = false;
    ++m_ResolvedFrames;
}

// ============================================================================
// 结果
// ============================================================================

Float64 FGpuProfiler::GetScopeSumMilliseconds() const
{
    Float64 sum = 0.0;

    for (UInt32 i = 0; i < m_ResultCount; ++i)
    {
        sum += m_Results[i].Milliseconds;
    }

    return sum;
}

Float64 FGpuProfiler::TicksToMilliseconds(UInt64 begin, UInt64 end) const
{
    const UInt64 delta = ComputeTimestampDelta(begin, end, m_ValidMask);

    // tick → 纳秒 → 毫秒
    return static_cast<Float64>(delta) *
           static_cast<Float64>(m_Period) * 1.0e-6;
}

} // namespace Limx
