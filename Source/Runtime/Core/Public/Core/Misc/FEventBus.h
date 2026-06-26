/*******************************************************************************
 * 文件: FEventBus.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   事件总线 — 类型安全的发布/订阅消息系统
 *   支持按事件类型注册回调，广播时自动分发到所有订阅者
 *   用于引擎子系统间解耦通信、UI 事件通知、游戏逻辑触发等场景
 *
 * 设计哲学:
 *   类型擦除 — 通过 FTypeId 将不同事件类型映射到回调列表
 *   句柄管理 — 订阅返回句柄，通过句柄取消订阅
 *   同步广播 — 广播时立即调用所有回调 (不排队)
 *
 * 技术特性:
 *   - Subscribe<EventT>(callback): 订阅特定类型事件
 *   - Unsubscribe(handle): 取消订阅
 *   - Broadcast(event): 广播事件到所有订阅者
 *   - 基于 TFunction 的回调存储
 *   - 基于 FTypeId 的事件类型区分
 *
 * 依赖关系:
 *   内部: Core/Templates/TFunction.h, Core/Misc/FTypeId.h,
 *          Core/Containers/TArray.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Containers/TArray.h"
#include "Core/Templates/TFunction.h"
#include "Core/Misc/FTypeId.h"
#include "Core/Events/FEvent.h"
#include "Core/Memory/IAllocator.h"
#include "Core/Memory/DefaultAllocator.h"

namespace Limx
{

/// 事件订阅句柄
struct FEventHandle
{
    UInt64 Id;

    constexpr FEventHandle() : Id(0) {}
    constexpr explicit FEventHandle(UInt64 id) : Id(id) {}
    LIMX_NODISCARD constexpr bool IsValid() const { return Id != 0; }

    LIMX_NODISCARD constexpr bool operator==(
        const FEventHandle& other) const
    {
        return Id == other.Id;
    }

    LIMX_NODISCARD constexpr bool operator!=(
        const FEventHandle& other) const
    {
        return Id != other.Id;
    }
};

/// 事件总线 — 类型安全发布/订阅
class FEventBus
{
    /// 类型擦除的回调包装
    struct CallbackEntry
    {
        FEventHandle   Handle;      ///< 订阅句柄
        FTypeId        EventType;   ///< 事件类型 ID
        EEventPriority Priority;    ///< 触发优先级
        void*          CallbackPtr; ///< TFunction 堆分配指针
        void (*Invoker)(void* callbackPtr,
                        const void* eventData);  ///< 调用器
        void (*Destroyer)(void* callbackPtr);    ///< 析构器
    };

    /// 延迟广播的事件条目
    struct DeferredEntry
    {
        FTypeId EventType;
        void*   EventData;
        void  (*Broadcaster)(FEventBus* self, void* data);
        void  (*Destroyer)(void* data);
    };

public:
    FEventBus() : m_NextHandleId(1), m_IsBroadcasting(false) {}

    ~FEventBus()
    {
        // 释放所有回调
        for (SizeType index = 0;
             index < m_Callbacks.GetSize(); ++index)
        {
            m_Callbacks[index].Destroyer(
                m_Callbacks[index].CallbackPtr);
        }
    }

    // 不可拷贝
    FEventBus(const FEventBus&) = delete;
    FEventBus& operator=(const FEventBus&) = delete;

    // ========================================================================
    // 订阅
    // ========================================================================

    /// 订阅事件 — 返回句柄用于取消订阅
    /// @param callback 回调函数
    /// @param priority 优先级 (数值越小越先触发)
    template<typename EventT>
    LIMX_NODISCARD FEventHandle Subscribe(
        TFunction<void(const EventT&)> callback,
        EEventPriority priority = EEventPriority::Normal)
    {
        using CallbackType = TFunction<void(const EventT&)>;

        void* memory = GetDefaultAllocator().Allocate(
            sizeof(CallbackType), alignof(CallbackType));
        new (memory) CallbackType(MoveTemp(callback));

        CallbackEntry entry;
        entry.Handle    = FEventHandle(m_NextHandleId++);
        entry.EventType = TypeIdOf<EventT>();
        entry.Priority  = priority;
        entry.CallbackPtr = memory;

        entry.Invoker = [](void* cbPtr, const void* eventData)
        {
            CallbackType& cb =
                *static_cast<CallbackType*>(cbPtr);
            const EventT& event =
                *static_cast<const EventT*>(eventData);
            cb(event);
        };

        entry.Destroyer = [](void* cbPtr)
        {
            static_cast<CallbackType*>(cbPtr)->
                ~CallbackType();
            GetDefaultAllocator().Deallocate(cbPtr);
        };

        // 按优先级有序插入
        SizeType insertAt = m_Callbacks.GetSize();
        for (SizeType i = 0;
             i < m_Callbacks.GetSize(); ++i)
        {
            if (static_cast<UInt8>(
                    m_Callbacks[i].Priority) >
                static_cast<UInt8>(priority))
            {
                insertAt = i;
                break;
            }
        }
        m_Callbacks.Insert(insertAt, MoveTemp(entry));
        return m_Callbacks[insertAt].Handle;
    }

    // ========================================================================
    // 取消订阅
    // ========================================================================

    /// 通过句柄取消订阅
    void Unsubscribe(FEventHandle handle)
    {
        for (SizeType index = 0;
             index < m_Callbacks.GetSize(); ++index)
        {
            if (m_Callbacks[index].Handle == handle)
            {
                m_Callbacks[index].Destroyer(
                    m_Callbacks[index].CallbackPtr);

                // 与最后一个交换删除
                if (index + 1 < m_Callbacks.GetSize())
                {
                    m_Callbacks[index] = MoveTemp(
                        m_Callbacks[m_Callbacks.GetSize() - 1]);
                }
                m_Callbacks.RemoveLast();
                return;
            }
        }
    }

    // ========================================================================
    // 广播
    // ========================================================================

    /// 同步广播 — 按优先级顺序调用所有匹配回调
    template<typename EventT>
    void Broadcast(const EventT& event)
    {
        FTypeId eventType = TypeIdOf<EventT>();
        const void* eventData = static_cast<const void*>(&event);

        m_IsBroadcasting = true;
        SizeType count = m_Callbacks.GetSize();
        for (SizeType index = 0; index < count; ++index)
        {
            if (m_Callbacks[index].EventType == eventType)
            {
                m_Callbacks[index].Invoker(
                    m_Callbacks[index].CallbackPtr,
                    eventData);
            }
        }
        m_IsBroadcasting = false;
    }

    /// 延迟广播 — 入队，待 FlushDeferred() 时分发
    template<typename EventT>
    void PostDeferred(const EventT& event)
    {
        using EV = EventT;
        void* mem = GetDefaultAllocator().Allocate(
            sizeof(EV), alignof(EV));
        new (mem) EV(event);

        DeferredEntry deferred;
        deferred.EventType = TypeIdOf<EventT>();
        deferred.EventData = mem;

        deferred.Broadcaster =
            [](FEventBus* self, void* data)
        {
            self->Broadcast(*static_cast<EV*>(data));
        };

        deferred.Destroyer = [](void* data)
        {
            static_cast<EV*>(data)->~EV();
            GetDefaultAllocator().Deallocate(data);
        };

        m_DeferredEvents.Add(MoveTemp(deferred));
    }

    /// 处理所有延迟事件 (帧末调用)
    void FlushDeferred()
    {
        TArray<DeferredEntry> toProcess =
            MoveTemp(m_DeferredEvents);

        for (SizeType i = 0;
             i < toProcess.GetSize(); ++i)
        {
            toProcess[i].Broadcaster(
                this, toProcess[i].EventData);
            toProcess[i].Destroyer(
                toProcess[i].EventData);
        }
    }

    /// 是否正在广播中
    LIMX_NODISCARD bool IsBroadcasting() const
    {
        return m_IsBroadcasting;
    }

    /// 延迟队列中待处理事件数
    LIMX_NODISCARD SizeType GetDeferredCount() const
    {
        return m_DeferredEvents.GetSize();
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 总订阅数
    LIMX_NODISCARD SizeType GetSubscriberCount() const
    {
        return m_Callbacks.GetSize();
    }

    /// 指定事件类型的订阅数
    template<typename EventT>
    LIMX_NODISCARD SizeType GetSubscriberCount() const
    {
        FTypeId eventType = TypeIdOf<EventT>();
        SizeType count = 0;
        for (SizeType index = 0;
             index < m_Callbacks.GetSize(); ++index)
        {
            if (m_Callbacks[index].EventType == eventType)
            {
                ++count;
            }
        }
        return count;
    }

    /// 清空所有订阅
    void Clear()
    {
        for (SizeType index = 0;
             index < m_Callbacks.GetSize(); ++index)
        {
            m_Callbacks[index].Destroyer(
                m_Callbacks[index].CallbackPtr);
        }
        m_Callbacks.Clear();
    }

    // ========================================================================
    // 全局实例
    // ========================================================================

    static FEventBus& Get()
    {
        static FEventBus s_Instance;
        return s_Instance;
    }

private:
    TArray<CallbackEntry>  m_Callbacks;      ///< 优先级有序回调列表
    TArray<DeferredEntry>  m_DeferredEvents; ///< 延迟事件队列
    UInt64                 m_NextHandleId;   ///< 下一个句柄 ID
    bool                   m_IsBroadcasting; ///< 广播重入保护标志
};

} // namespace Limx
