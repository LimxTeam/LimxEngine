/*******************************************************************************
 * 文件: TEventQueue.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   事件队列 — 延迟分发的类型安全消息队列
 *   将事件缓冲到队列，在 Flush 时统一分发给注册的处理器
 *   用于引擎主循环事件调度、跨帧消息传递、输入系统等场景
 *
 * 设计哲学:
 *   延迟分发 — 事件先入队，Flush 时集中处理，避免重入
 *   类型安全 — 模板参数化事件类型
 *   处理器链 — 支持多个处理器按注册顺序调用
 *
 * 技术特性:
 *   - TEventQueue<EventType>: 事件队列
 *   - Post: 入队事件
 *   - AddHandler: 注册处理器
 *   - RemoveHandler: 注销处理器
 *   - Flush: 分发所有待处理事件
 *   - GetPendingCount: 查询待处理事件数
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Containers/TArray.h,
 *          Core/Templates/TFunction.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Containers/TArray.h"
#include "Core/Templates/TFunction.h"

namespace Limx
{

/// 事件处理器句柄 (用于注销)
struct FEventHandlerHandle
{
    UInt32 Id;

    FEventHandlerHandle() : Id(0) {}
    explicit FEventHandlerHandle(UInt32 id) : Id(id) {}

    LIMX_NODISCARD bool IsValid() const { return Id != 0; }
    void Invalidate() { Id = 0; }

    bool operator==(const FEventHandlerHandle& other) const
    {
        return Id == other.Id;
    }
    bool operator!=(const FEventHandlerHandle& other) const
    {
        return Id != other.Id;
    }
};

/// 延迟分发事件队列
/// @tparam EventType 事件类型
template<typename EventType>
class TEventQueue
{
public:
    using FHandler = TFunction<void(const EventType&)>;

    TEventQueue() : m_NextHandlerId(1) {}

    // ========================================================================
    // 事件发布
    // ========================================================================

    /// 入队事件 (拷贝)
    void Post(const EventType& event)
    {
        m_PendingEvents.Add(event);
    }

    /// 入队事件 (移动)
    void Post(EventType&& event)
    {
        m_PendingEvents.Add(MoveTemp(event));
    }

    // ========================================================================
    // 处理器注册
    // ========================================================================

    /// 注册事件处理器
    /// @return 处理器句柄 (用于注销)
    FEventHandlerHandle AddHandler(FHandler handler)
    {
        FHandlerEntry entry;
        entry.Handle = FEventHandlerHandle(m_NextHandlerId++);
        entry.Handler = MoveTemp(handler);
        m_Handlers.Add(MoveTemp(entry));
        return m_Handlers[m_Handlers.GetSize() - 1].Handle;
    }

    /// 注销处理器
    void RemoveHandler(const FEventHandlerHandle& handle)
    {
        for (SizeType handlerIdx = 0;
             handlerIdx < m_Handlers.GetSize(); ++handlerIdx)
        {
            if (m_Handlers[handlerIdx].Handle == handle)
            {
                m_Handlers.RemoveAt(handlerIdx);
                return;
            }
        }
    }

    // ========================================================================
    // 分发
    // ========================================================================

    /// 分发所有待处理事件
    void Flush()
    {
        // 交换队列以支持 Flush 期间的新 Post
        TArray<EventType> eventsToProcess;
        eventsToProcess = MoveTemp(m_PendingEvents);

        for (SizeType eventIdx = 0;
             eventIdx < eventsToProcess.GetSize(); ++eventIdx)
        {
            const EventType& event =
                eventsToProcess[eventIdx];
            for (SizeType handlerIdx = 0;
                 handlerIdx < m_Handlers.GetSize();
                 ++handlerIdx)
            {
                if (m_Handlers[handlerIdx].Handler)
                {
                    m_Handlers[handlerIdx].Handler(event);
                }
            }
        }
    }

    /// 清空队列 (不触发处理器)
    void Clear()
    {
        m_PendingEvents.Clear();
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 待处理事件数
    LIMX_NODISCARD SizeType GetPendingCount() const
    {
        return m_PendingEvents.GetSize();
    }

    /// 注册的处理器数量
    LIMX_NODISCARD SizeType GetHandlerCount() const
    {
        return m_Handlers.GetSize();
    }

    /// 是否有待处理事件
    LIMX_NODISCARD bool HasPendingEvents() const
    {
        return m_PendingEvents.GetSize() > 0;
    }

private:
    struct FHandlerEntry
    {
        FEventHandlerHandle Handle;
        FHandler            Handler;
    };

    TArray<EventType>    m_PendingEvents;  ///< 待处理事件队列
    TArray<FHandlerEntry> m_Handlers;      ///< 注册的处理器
    UInt32               m_NextHandlerId;  ///< 处理器 ID 计数器
};

} // namespace Limx
