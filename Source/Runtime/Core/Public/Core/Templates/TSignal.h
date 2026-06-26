/*******************************************************************************
 * 文件: TSignal.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   信号/槽 — Qt 风格的类型安全信号系统
 *   信号被触发时自动调用所有已连接的槽函数
 *   用于 UI 事件绑定、组件间解耦通信、属性变更通知等场景
 *
 * 设计哲学:
 *   类型安全 — 信号参数类型在编译时检查
 *   句柄管理 — 连接返回句柄，支持断开
 *   无动态分配策略 — 使用 TArray 管理连接列表
 *
 * 技术特性:
 *   - TSignal<Args...>: 可触发信号，支持多参数
 *   - Connect: 连接槽函数，返回连接 ID
 *   - Disconnect: 通过 ID 断开连接
 *   - Emit: 触发信号，调用所有连接的槽
 *   - GetConnectionCount: 当前连接数
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

/// 信号连接 ID
using FConnectionId = UInt32;

/// 无效连接 ID
static constexpr FConnectionId kInvalidConnectionId = 0;

/// 类型安全信号
/// @tparam Args 信号参数类型
template<typename... Args>
class TSignal
{
    /// 连接槽记录
    struct FSlot
    {
        FConnectionId       Id;       ///< 连接 ID
        TFunction<void(Args...)> Callback; ///< 槽函数
        bool                IsActive; ///< 是否活跃
    };

public:
    TSignal()
        : m_NextId(1)
    {
    }

    ~TSignal() = default;

    // 不可拷贝 (信号通常作为成员变量，不应拷贝)
    TSignal(const TSignal&) = delete;
    TSignal& operator=(const TSignal&) = delete;

    // 可移动
    TSignal(TSignal&& other) noexcept
        : m_Slots(MoveTemp(other.m_Slots))
        , m_NextId(other.m_NextId)
    {
        other.m_NextId = 1;
    }

    // ========================================================================
    // 连接与断开
    // ========================================================================

    /// 连接槽函数
    /// @return 连接 ID (用于后续断开)
    FConnectionId Connect(TFunction<void(Args...)> callback)
    {
        FConnectionId id = m_NextId++;

        FSlot slot;
        slot.Id = id;
        slot.Callback = MoveTemp(callback);
        slot.IsActive = true;

        m_Slots.Add(MoveTemp(slot));
        return id;
    }

    /// 断开指定连接
    /// @return 是否成功断开
    bool Disconnect(FConnectionId connectionId)
    {
        for (SizeType index = 0;
             index < m_Slots.GetCount(); ++index)
        {
            if (m_Slots[index].Id == connectionId)
            {
                m_Slots.RemoveAt(index);
                return true;
            }
        }
        return false;
    }

    /// 断开所有连接
    void DisconnectAll()
    {
        m_Slots.Clear();
    }

    // ========================================================================
    // 触发
    // ========================================================================

    /// 触发信号 — 调用所有连接的槽
    void Emit(Args... args) const
    {
        // 拷贝槽列表，防止回调中修改连接
        SizeType slotCount = m_Slots.GetCount();
        for (SizeType index = 0; index < slotCount; ++index)
        {
            if (m_Slots[index].IsActive)
            {
                m_Slots[index].Callback(args...);
            }
        }
    }

    /// 触发信号 — 操作符版本
    void operator()(Args... args) const
    {
        Emit(args...);
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 当前连接数
    LIMX_NODISCARD SizeType GetConnectionCount() const
    {
        return m_Slots.GetCount();
    }

    /// 是否有连接
    LIMX_NODISCARD bool HasConnections() const
    {
        return m_Slots.GetCount() > 0;
    }

private:
    TArray<FSlot>   m_Slots;   ///< 连接槽列表
    FConnectionId   m_NextId;  ///< 下一个连接 ID
};

} // namespace Limx
