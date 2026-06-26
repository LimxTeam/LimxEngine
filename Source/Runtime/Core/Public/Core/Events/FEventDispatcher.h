/*******************************************************************************
 * 文件: FEventDispatcher.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   局部事件分发器 — 组件/对象内的类型安全事件路由器
 *   按优先级排序的监听器链，支持事件消费(冒泡终止)
 *   与 FEventBus (全局总线) 配合使用：
 *     FEventDispatcher → 对象内部分发
 *     FEventBus       → 跨系统广播
 *
 * 设计哲学:
 *   优先级有序 — 监听器按 EEventPriority 从小到大排列执行
 *   消费语义 — 监听器可调用 event.Consume() 终止后续分发
 *   句柄管理 — RAII FEventListenerHandle 自动注销
 *
 * 技术特性:
 *   - AddListener<EventT>: 按优先级注册监听器
 *   - RemoveListener: 按句柄注销
 *   - Dispatch<EventT>: 同步按优先级分发
 *   - DispatchDeferred<EventT>: 入队，待 Flush() 批量分发
 *   - Flush: 处理延迟队列中所有事件
 *
 * 依赖关系:
 *   内部: Core/Events/FEvent.h, Core/Containers/TArray.h,
 *          Core/Templates/TFunction.h, Core/Misc/FTypeId.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Events/FEvent.h"
#include "Core/Containers/TArray.h"
#include "Core/Templates/TFunction.h"
#include "Core/Memory/DefaultAllocator.h"

namespace Limx
{

// ============================================================================
// 监听器句柄
// ============================================================================

/// 事件监听器句柄 (用于注销)
struct FEventListenerHandle
{
    UInt64 Id;

    constexpr FEventListenerHandle() : Id(0) {}
    explicit FEventListenerHandle(UInt64 id) : Id(id) {}

    LIMX_NODISCARD bool IsValid() const { return Id != 0; }
    void Invalidate() { Id = 0; }

    bool operator==(const FEventListenerHandle& o) const
    {
        return Id == o.Id;
    }
    bool operator!=(const FEventListenerHandle& o) const
    {
        return Id != o.Id;
    }
};

// ============================================================================
// 局部事件分发器
// ============================================================================

/// 局部事件分发器 (对象内使用)
class FEventDispatcher
{
    /// 类型擦除监听器条目
    struct FListenerEntry
    {
        FEventListenerHandle Handle;
        FTypeId              EventType;
        EEventPriority       Priority;
        void*                CallbackPtr;

        void (*Invoker)(void* cbPtr,
                        FEventBase& eventBase);
        void (*Destroyer)(void* cbPtr);
    };

    /// 延迟事件条目
    struct FDeferredEvent
    {
        FTypeId EventType;
        void*   EventData;   ///< 堆分配事件副本
        void  (*Dispatcher)(FEventDispatcher* self,
                             void* data);
        void  (*Destroyer)(void* data);
    };

public:
    FEventDispatcher() : m_NextHandleId(1) {}

    ~FEventDispatcher()
    {
        for (SizeType i = 0;
             i < m_Listeners.GetSize(); ++i)
        {
            m_Listeners[i].Destroyer(
                m_Listeners[i].CallbackPtr);
        }
        FlushDestroyDeferred();
    }

    FEventDispatcher(const FEventDispatcher&) = delete;
    FEventDispatcher& operator=(
        const FEventDispatcher&) = delete;

    // ========================================================================
    // 注册
    // ========================================================================

    /// 注册事件监听器
    /// @tparam EventT 事件类型 (须继承 FEventBase 或为 FEventBase)
    /// @param callback 监听器回调
    /// @param priority 优先级 (默认 Normal)
    /// @return 句柄 (用于注销)
    template<typename EventT>
    LIMX_NODISCARD FEventListenerHandle AddListener(
        TFunction<void(EventT&)> callback,
        EEventPriority priority = EEventPriority::Normal)
    {
        using CB = TFunction<void(EventT&)>;

        void* mem = GetDefaultAllocator().Allocate(
            sizeof(CB), alignof(CB));
        new (mem) CB(MoveTemp(callback));

        FListenerEntry entry;
        entry.Handle    = FEventListenerHandle(
            m_NextHandleId++);
        entry.EventType = TypeIdOf<EventT>();
        entry.Priority  = priority;
        entry.CallbackPtr = mem;

        entry.Invoker =
            [](void* cbPtr, FEventBase& base)
        {
            EventT& typedEvent =
                static_cast<EventT&>(base);
            (*static_cast<CB*>(cbPtr))(typedEvent);
        };

        entry.Destroyer = [](void* cbPtr)
        {
            static_cast<CB*>(cbPtr)->~CB();
            GetDefaultAllocator().Deallocate(cbPtr);
        };

        // 按优先级插入有序列表
        SizeType insertAt = m_Listeners.GetSize();
        for (SizeType i = 0;
             i < m_Listeners.GetSize(); ++i)
        {
            if (static_cast<UInt8>(
                    m_Listeners[i].Priority) >
                static_cast<UInt8>(priority))
            {
                insertAt = i;
                break;
            }
        }

        m_Listeners.Insert(insertAt, MoveTemp(entry));
        return m_Listeners[insertAt].Handle;
    }

    /// 注销监听器
    void RemoveListener(FEventListenerHandle handle)
    {
        for (SizeType i = 0;
             i < m_Listeners.GetSize(); ++i)
        {
            if (m_Listeners[i].Handle == handle)
            {
                m_Listeners[i].Destroyer(
                    m_Listeners[i].CallbackPtr);
                m_Listeners.RemoveAt(i);
                return;
            }
        }
    }

    // ========================================================================
    // 分发
    // ========================================================================

    /// 同步分发事件 (按优先级，支持消费)
    template<typename EventT>
    void Dispatch(EventT& event)
    {
        FTypeId eventType = TypeIdOf<EventT>();
        FEventBase& base = static_cast<FEventBase&>(event);

        for (SizeType i = 0;
             i < m_Listeners.GetSize(); ++i)
        {
            if (base.IsConsumed) break;
            if (m_Listeners[i].EventType == eventType)
            {
                m_Listeners[i].Invoker(
                    m_Listeners[i].CallbackPtr, base);
            }
        }
    }

    /// 延迟分发 (入队, 待 Flush() 处理)
    template<typename EventT>
    void DispatchDeferred(const EventT& event)
    {
        using EV = EventT;
        void* mem = GetDefaultAllocator().Allocate(
            sizeof(EV), alignof(EV));
        new (mem) EV(event);

        FDeferredEvent deferred;
        deferred.EventType = TypeIdOf<EventT>();
        deferred.EventData = mem;

        deferred.Dispatcher =
            [](FEventDispatcher* self, void* data)
        {
            EV& ev = *static_cast<EV*>(data);
            self->Dispatch(ev);
        };

        deferred.Destroyer = [](void* data)
        {
            static_cast<EV*>(data)->~EV();
            GetDefaultAllocator().Deallocate(data);
        };

        m_DeferredEvents.Add(MoveTemp(deferred));
    }

    /// 处理所有延迟事件
    void Flush()
    {
        // 交换队列，允许 Dispatch 期间再入队
        TArray<FDeferredEvent> toProcess =
            MoveTemp(m_DeferredEvents);

        for (SizeType i = 0;
             i < toProcess.GetSize(); ++i)
        {
            toProcess[i].Dispatcher(
                this, toProcess[i].EventData);
            toProcess[i].Destroyer(
                toProcess[i].EventData);
        }
    }

    // ========================================================================
    // 查询
    // ========================================================================

    LIMX_NODISCARD SizeType GetListenerCount() const
    {
        return m_Listeners.GetSize();
    }

    LIMX_NODISCARD SizeType GetDeferredCount() const
    {
        return m_DeferredEvents.GetSize();
    }

    void ClearAllListeners()
    {
        for (SizeType i = 0;
             i < m_Listeners.GetSize(); ++i)
        {
            m_Listeners[i].Destroyer(
                m_Listeners[i].CallbackPtr);
        }
        m_Listeners.Clear();
    }

private:
    void FlushDestroyDeferred()
    {
        for (SizeType i = 0;
             i < m_DeferredEvents.GetSize(); ++i)
        {
            m_DeferredEvents[i].Destroyer(
                m_DeferredEvents[i].EventData);
        }
        m_DeferredEvents.Clear();
    }

    TArray<FListenerEntry>  m_Listeners;      ///< 有序监听器列表
    TArray<FDeferredEvent>  m_DeferredEvents; ///< 延迟事件队列
    UInt64                  m_NextHandleId;   ///< 句柄 ID 计数器
};

// ============================================================================
// RAII 自动注销句柄
// ============================================================================

/// RAII 包装 — 析构时自动注销监听器
class FAutoEventListener
{
public:
    FAutoEventListener() : m_Dispatcher(nullptr) {}

    FAutoEventListener(FEventDispatcher* dispatcher,
                       FEventListenerHandle handle)
        : m_Dispatcher(dispatcher)
        , m_Handle(handle)
    {
    }

    ~FAutoEventListener()
    {
        Remove();
    }

    FAutoEventListener(const FAutoEventListener&) = delete;
    FAutoEventListener& operator=(
        const FAutoEventListener&) = delete;

    FAutoEventListener(FAutoEventListener&& other)
        : m_Dispatcher(other.m_Dispatcher)
        , m_Handle(other.m_Handle)
    {
        other.m_Dispatcher = nullptr;
        other.m_Handle.Invalidate();
    }

    void Remove()
    {
        if (m_Dispatcher != nullptr &&
            m_Handle.IsValid())
        {
            m_Dispatcher->RemoveListener(m_Handle);
            m_Dispatcher = nullptr;
            m_Handle.Invalidate();
        }
    }

    LIMX_NODISCARD bool IsValid() const
    {
        return m_Dispatcher != nullptr &&
               m_Handle.IsValid();
    }

private:
    FEventDispatcher*    m_Dispatcher;
    FEventListenerHandle m_Handle;
};

} // namespace Limx
