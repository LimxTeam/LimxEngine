/*******************************************************************************
 * 文件: TStateMachine.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   有限状态机 — 类型安全的状态管理与转换
 *   支持状态进入/退出回调、条件转换、事件驱动
 *   用于 AI 行为、UI 流程、游戏逻辑、动画状态管理等场景
 *
 * 设计哲学:
 *   枚举驱动 — 状态用 enum class 表示，编译时类型安全
 *   回调注册 — OnEnter/OnExit/OnUpdate 回调按状态注册
 *   转换表 — 显式注册合法转换，防止非法状态跳转
 *
 * 技术特性:
 *   - TStateMachine<StateEnum>: 参数化状态机
 *   - AddState: 注册状态及其回调
 *   - AddTransition: 注册合法转换
 *   - TransitionTo: 执行状态转换
 *   - Update: 驱动当前状态更新
 *   - GetCurrentState: 查询当前状态
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Templates/TFunction.h,
 *          Core/Containers/TArray.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Templates/TFunction.h"
#include "Core/Containers/TArray.h"

namespace Limx
{

/// 有限状态机
/// @tparam StateEnum enum class 状态枚举
template<typename StateEnum>
class TStateMachine
{
    /// 状态描述
    struct FStateDesc
    {
        StateEnum                State;     ///< 状态枚举值
        TFunction<void()>        OnEnter;   ///< 进入回调
        TFunction<void()>        OnExit;    ///< 退出回调
        TFunction<void(Float32)> OnUpdate;  ///< 更新回调 (deltaTime)
    };

    /// 转换描述
    struct FTransitionDesc
    {
        StateEnum From;  ///< 源状态
        StateEnum To;    ///< 目标状态
    };

public:
    TStateMachine()
        : m_CurrentState(static_cast<StateEnum>(0))
        , m_IsInitialized(false)
    {
    }

    // ========================================================================
    // 状态注册
    // ========================================================================

    /// 添加状态 (完整回调)
    void AddState(StateEnum state,
                   TFunction<void()> onEnter,
                   TFunction<void()> onExit,
                   TFunction<void(Float32)> onUpdate)
    {
        FStateDesc desc;
        desc.State = state;
        desc.OnEnter = MoveTemp(onEnter);
        desc.OnExit = MoveTemp(onExit);
        desc.OnUpdate = MoveTemp(onUpdate);
        m_States.Add(MoveTemp(desc));
    }

    /// 添加状态 (仅更新回调)
    void AddState(StateEnum state,
                   TFunction<void(Float32)> onUpdate)
    {
        AddState(state,
                  TFunction<void()>(),
                  TFunction<void()>(),
                  MoveTemp(onUpdate));
    }

    /// 注册合法转换
    void AddTransition(StateEnum from, StateEnum to)
    {
        FTransitionDesc trans;
        trans.From = from;
        trans.To = to;
        m_Transitions.Add(trans);
    }

    // ========================================================================
    // 状态控制
    // ========================================================================

    /// 设置初始状态
    void Start(StateEnum initialState)
    {
        m_CurrentState = initialState;
        m_IsInitialized = true;

        FStateDesc* stateDesc = FindState(initialState);
        if (stateDesc && stateDesc->OnEnter)
        {
            stateDesc->OnEnter();
        }
    }

    /// 执行状态转换
    /// @return 是否成功转换 (转换不合法时返回 false)
    bool TransitionTo(StateEnum newState)
    {
        if (!m_IsInitialized) return false;
        if (m_CurrentState == newState) return true;

        // 检查转换是否合法
        if (!IsTransitionValid(m_CurrentState, newState))
        {
            return false;
        }

        // 退出当前状态
        FStateDesc* currentDesc = FindState(m_CurrentState);
        if (currentDesc && currentDesc->OnExit)
        {
            currentDesc->OnExit();
        }

        // 进入新状态
        m_CurrentState = newState;

        FStateDesc* newDesc = FindState(newState);
        if (newDesc && newDesc->OnEnter)
        {
            newDesc->OnEnter();
        }

        return true;
    }

    /// 强制转换 (跳过合法性检查)
    void ForceTransitionTo(StateEnum newState)
    {
        if (m_IsInitialized)
        {
            FStateDesc* currentDesc = FindState(m_CurrentState);
            if (currentDesc && currentDesc->OnExit)
            {
                currentDesc->OnExit();
            }
        }

        m_CurrentState = newState;
        m_IsInitialized = true;

        FStateDesc* newDesc = FindState(newState);
        if (newDesc && newDesc->OnEnter)
        {
            newDesc->OnEnter();
        }
    }

    /// 驱动当前状态更新
    void Update(Float32 deltaTime)
    {
        if (!m_IsInitialized) return;

        FStateDesc* desc = FindState(m_CurrentState);
        if (desc && desc->OnUpdate)
        {
            desc->OnUpdate(deltaTime);
        }
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 获取当前状态
    LIMX_NODISCARD StateEnum GetCurrentState() const
    {
        return m_CurrentState;
    }

    /// 是否已初始化
    LIMX_NODISCARD bool IsInitialized() const
    {
        return m_IsInitialized;
    }

    /// 是否处于指定状态
    LIMX_NODISCARD bool IsInState(StateEnum state) const
    {
        return m_IsInitialized && m_CurrentState == state;
    }

    /// 检查转换是否合法
    LIMX_NODISCARD bool IsTransitionValid(
        StateEnum from, StateEnum to) const
    {
        // 无转换表 = 允许所有转换
        if (m_Transitions.GetCount() == 0) return true;

        for (SizeType index = 0;
             index < m_Transitions.GetCount(); ++index)
        {
            if (m_Transitions[index].From == from &&
                m_Transitions[index].To == to)
            {
                return true;
            }
        }
        return false;
    }

    /// 已注册状态数
    LIMX_NODISCARD SizeType GetStateCount() const
    {
        return m_States.GetCount();
    }

private:
    /// 查找状态描述
    FStateDesc* FindState(StateEnum state)
    {
        for (SizeType index = 0;
             index < m_States.GetCount(); ++index)
        {
            if (m_States[index].State == state)
            {
                return &m_States[index];
            }
        }
        return nullptr;
    }

    TArray<FStateDesc>      m_States;        ///< 状态列表
    TArray<FTransitionDesc> m_Transitions;   ///< 转换表
    StateEnum               m_CurrentState;  ///< 当前状态
    bool                    m_IsInitialized; ///< 是否已初始化
};

} // namespace Limx
