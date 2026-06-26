/*******************************************************************************
 * 文件: TFuture.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   异步未来值 — 轻量异步结果封装
 *   TPromise 写入结果，TFuture 读取结果，配合任务系统使用
 *   用于任务返回值、IO 异步结果、GPU 栅栏结果等场景
 *
 * 设计哲学:
 *   共享状态 — TPromise 和 TFuture 共享一个控制块
 *   无锁查询 — IsReady() 以原子变量标识完成
 *   值语义 — 结果通过移动返回，只能 Get() 一次
 *
 * 技术特性:
 *   - TFuture<T>: 只读异步结果句柄
 *   - TPromise<T>: 只写结果生产者
 *   - IsReady: 非阻塞完成检测
 *   - Get: 阻塞等待并移动结果
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h,
 *          Core/Threading/FAtomic.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Threading/FAtomic.h"
#include "Core/Memory/DefaultAllocator.h"
#include "Core/Memory/MemoryOps.h"

namespace Limx
{

// 前向声明
template<typename T> class TPromise;

/// 异步未来值 — 只读结果句柄
/// @tparam T 结果类型
template<typename T>
class TFuture
{
    friend class TPromise<T>;

    struct FSharedState
    {
        TAtomic<bool>  IsReady;
        alignas(T)    UInt8 Storage[sizeof(T)];
        TAtomic<Int32> RefCount;

        FSharedState()
            : IsReady(false)
            , RefCount(2)  // Promise + Future 各持一份
        {
        }

        void AddRef() { RefCount.FetchAdd(1); }

        void Release()
        {
            if (RefCount.FetchSub(1) == 1)
            {
                if (IsReady.Load())
                {
                    GetValue().~T();
                }
                this->~FSharedState();
                GetDefaultAllocator().Deallocate(this);
            }
        }

        T& GetValue()
        {
            return *reinterpret_cast<T*>(Storage);
        }

        const T& GetValue() const
        {
            return *reinterpret_cast<const T*>(Storage);
        }
    };

public:
    TFuture() : m_State(nullptr) {}

    TFuture(const TFuture& other)
        : m_State(other.m_State)
    {
        if (m_State != nullptr)
        {
            m_State->AddRef();
        }
    }

    TFuture(TFuture&& other)
        : m_State(other.m_State)
    {
        other.m_State = nullptr;
    }

    ~TFuture()
    {
        if (m_State != nullptr)
        {
            m_State->Release();
        }
    }

    TFuture& operator=(const TFuture& other)
    {
        if (this != &other)
        {
            if (m_State != nullptr) m_State->Release();
            m_State = other.m_State;
            if (m_State != nullptr) m_State->AddRef();
        }
        return *this;
    }

    TFuture& operator=(TFuture&& other)
    {
        if (this != &other)
        {
            if (m_State != nullptr) m_State->Release();
            m_State = other.m_State;
            other.m_State = nullptr;
        }
        return *this;
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 是否有效 (是否关联了 Promise)
    LIMX_NODISCARD bool IsValid() const
    {
        return m_State != nullptr;
    }

    /// 非阻塞检查是否完成
    LIMX_NODISCARD bool IsReady() const
    {
        return m_State != nullptr &&
               m_State->IsReady.Load();
    }

    // ========================================================================
    // 获取结果
    // ========================================================================

    /// 获取结果 (仅在 IsReady() 后调用)
    LIMX_NODISCARD const T& Get() const
    {
        LIMX_ASSERT(IsReady());
        return m_State->GetValue();
    }

    /// 移动获取结果
    LIMX_NODISCARD T Take()
    {
        LIMX_ASSERT(IsReady());
        return MoveTemp(m_State->GetValue());
    }

private:
    explicit TFuture(FSharedState* state)
        : m_State(state)
    {
    }

    FSharedState* m_State;
};

/// 异步未来值 — 只写结果生产者
/// @tparam T 结果类型
template<typename T>
class TPromise
{
public:
    TPromise()
    {
        m_State = static_cast<
            typename TFuture<T>::FSharedState*>(
            GetDefaultAllocator().Allocate(
                sizeof(typename TFuture<T>::FSharedState),
                alignof(typename TFuture<T>::FSharedState)));
        new (m_State)
            typename TFuture<T>::FSharedState();
    }

    TPromise(const TPromise&) = delete;
    TPromise& operator=(const TPromise&) = delete;

    TPromise(TPromise&& other)
        : m_State(other.m_State)
    {
        other.m_State = nullptr;
    }

    ~TPromise()
    {
        if (m_State != nullptr)
        {
            m_State->Release();
        }
    }

    // ========================================================================
    // 获取 Future
    // ========================================================================

    LIMX_NODISCARD TFuture<T> GetFuture()
    {
        LIMX_ASSERT(m_State != nullptr);
        m_State->AddRef();
        return TFuture<T>(m_State);
    }

    // ========================================================================
    // 设置结果
    // ========================================================================

    void SetValue(const T& value)
    {
        LIMX_ASSERT(m_State != nullptr);
        LIMX_ASSERT(!m_State->IsReady.Load());
        new (m_State->Storage) T(value);
        m_State->IsReady.Store(true);
    }

    void SetValue(T&& value)
    {
        LIMX_ASSERT(m_State != nullptr);
        LIMX_ASSERT(!m_State->IsReady.Load());
        new (m_State->Storage) T(MoveTemp(value));
        m_State->IsReady.Store(true);
    }

    LIMX_NODISCARD bool IsValid() const
    {
        return m_State != nullptr;
    }

private:
    typename TFuture<T>::FSharedState* m_State;
};

} // namespace Limx
