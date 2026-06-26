/*******************************************************************************
 * 文件: TObserver.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   观察者模式 — 自动解绑的弱引用订阅机制
 *   Subject 维护弱引用观察者列表，观察者销毁时自动从列表中移除
 *   用于 UI 数据绑定、属性变化通知、事件广播等场景
 *
 * 设计哲学:
 *   自动解绑 — 观察者析构时通过 RAII 自动注销
 *   弱引用模式 — Subject 不持有 Observer 生命周期
 *   类型安全 — 模板化通知数据类型
 *
 * 技术特性:
 *   - TObserver<DataType>: 观察者基类
 *   - TSubject<DataType>: 被观察者
 *   - Subscribe/Unsubscribe: 订阅与注销
 *   - Notify: 通知所有观察者
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

// 前向声明
template<typename DataType> class TSubject;

/// 观察者基类
/// @tparam DataType 通知数据类型
template<typename DataType>
class TObserver
{
    friend class TSubject<DataType>;

public:
    TObserver()
        : m_BoundSubject(nullptr)
    {
    }

    virtual ~TObserver()
    {
        Unsubscribe();
    }

    // 不可拷贝
    TObserver(const TObserver&) = delete;
    TObserver& operator=(const TObserver&) = delete;

    /// 取消订阅
    void Unsubscribe();

    /// 是否已订阅
    LIMX_NODISCARD bool IsSubscribed() const
    {
        return m_BoundSubject != nullptr;
    }

protected:
    /// 子类实现此方法接收通知
    virtual void OnNotify(const DataType& data) = 0;

private:
    TSubject<DataType>* m_BoundSubject;  ///< 当前订阅的 Subject
};

/// 被观察者
/// @tparam DataType 通知数据类型
template<typename DataType>
class TSubject
{
    friend class TObserver<DataType>;

public:
    TSubject() = default;

    ~TSubject()
    {
        // 解绑所有观察者
        for (SizeType observerIdx = 0;
             observerIdx < m_Observers.GetSize();
             ++observerIdx)
        {
            m_Observers[observerIdx]->m_BoundSubject =
                nullptr;
        }
    }

    // 不可拷贝
    TSubject(const TSubject&) = delete;
    TSubject& operator=(const TSubject&) = delete;

    // ========================================================================
    // 订阅管理
    // ========================================================================

    /// 添加观察者
    void Subscribe(TObserver<DataType>* observer)
    {
        LIMX_ASSERT(observer != nullptr);
        if (observer->m_BoundSubject != nullptr)
        {
            observer->Unsubscribe();
        }
        m_Observers.Add(observer);
        observer->m_BoundSubject = this;
    }

    /// 移除观察者
    void Unsubscribe(TObserver<DataType>* observer)
    {
        LIMX_ASSERT(observer != nullptr);
        for (SizeType observerIdx = 0;
             observerIdx < m_Observers.GetSize();
             ++observerIdx)
        {
            if (m_Observers[observerIdx] == observer)
            {
                m_Observers.RemoveAt(observerIdx);
                observer->m_BoundSubject = nullptr;
                return;
            }
        }
    }

    // ========================================================================
    // 通知
    // ========================================================================

    /// 通知所有观察者
    void Notify(const DataType& data)
    {
        for (SizeType observerIdx = 0;
             observerIdx < m_Observers.GetSize();
             ++observerIdx)
        {
            m_Observers[observerIdx]->OnNotify(data);
        }
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 观察者数量
    LIMX_NODISCARD SizeType GetObserverCount() const
    {
        return m_Observers.GetSize();
    }

    /// 是否有观察者
    LIMX_NODISCARD bool HasObservers() const
    {
        return m_Observers.GetSize() > 0;
    }

private:
    TArray<TObserver<DataType>*> m_Observers;  ///< 观察者列表
};

// ============================================================================
// TObserver 内联实现
// ============================================================================

template<typename DataType>
void TObserver<DataType>::Unsubscribe()
{
    if (m_BoundSubject != nullptr)
    {
        m_BoundSubject->Unsubscribe(this);
    }
}

/// 函数式观察者 — 无需继承，通过回调接收通知
/// @tparam DataType 通知数据类型
template<typename DataType>
class TFunctionalObserver final
    : public TObserver<DataType>
{
public:
    using FCallback = TFunction<void(const DataType&)>;

    explicit TFunctionalObserver(FCallback callback)
        : m_Callback(MoveTemp(callback))
    {
    }

protected:
    void OnNotify(const DataType& data) override
    {
        if (m_Callback)
        {
            m_Callback(data);
        }
    }

private:
    FCallback m_Callback;  ///< 通知回调
};

} // namespace Limx
