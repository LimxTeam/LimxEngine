/*******************************************************************************
 * 文件: FTimer.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   定时器管理 — 支持一次性和重复定时回调
 *   由外部驱动 Tick，到期时触发注册的回调函数
 *   用于延迟执行、周期性轮询、冷却计时、动画计时等场景
 *
 * 设计哲学:
 *   句柄管理 — 注册返回句柄，通过句柄取消/查询
 *   外部驱动 — Tick(deltaTime) 由调用者每帧调用
 *   零 STL — 基于 TArray 和 TFunction
 *
 * 技术特性:
 *   - SetTimer: 注册一次性定时器
 *   - SetTimerLoop: 注册循环定时器
 *   - CancelTimer: 取消定时器
 *   - Tick: 推进时间并触发到期回调
 *   - IsTimerActive: 查询定时器是否活跃
 *
 * 依赖关系:
 *   内部: Core/Containers/TArray.h, Core/Templates/TFunction.h
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

/// 定时器句柄
struct FTimerHandle
{
    UInt32 Id;

    constexpr FTimerHandle() : Id(0) {}
    constexpr explicit FTimerHandle(UInt32 id) : Id(id) {}
    LIMX_NODISCARD constexpr bool IsValid() const { return Id != 0; }

    LIMX_NODISCARD constexpr bool operator==(
        const FTimerHandle& other) const
    {
        return Id == other.Id;
    }

    LIMX_NODISCARD constexpr bool operator!=(
        const FTimerHandle& other) const
    {
        return Id != other.Id;
    }
};

/// 定时器管理器
class FTimerManager
{
    struct TimerEntry
    {
        FTimerHandle          Handle;       ///< 句柄
        TFunction<void()>     Callback;     ///< 回调函数
        Float32               Delay;        ///< 延迟 (秒)
        Float32               Remaining;    ///< 剩余时间
        bool                  IsLooping;    ///< 是否循环
        bool                  IsPendingRemoval; ///< 标记待删除
    };

public:
    FTimerManager() : m_NextId(1) {}
    ~FTimerManager() = default;

    // 不可拷贝
    FTimerManager(const FTimerManager&) = delete;
    FTimerManager& operator=(const FTimerManager&) = delete;

    // ========================================================================
    // 注册
    // ========================================================================

    /// 注册一次性定时器
    /// @param delay    延迟时间 (秒)
    /// @param callback 到期回调
    /// @return 定时器句柄
    LIMX_NODISCARD FTimerHandle SetTimer(
        Float32 delay, TFunction<void()> callback)
    {
        TimerEntry entry;
        entry.Handle = FTimerHandle(m_NextId++);
        entry.Callback = MoveTemp(callback);
        entry.Delay = delay;
        entry.Remaining = delay;
        entry.IsLooping = false;
        entry.IsPendingRemoval = false;
        m_Timers.Add(MoveTemp(entry));
        return entry.Handle;
    }

    /// 注册循环定时器
    /// @param interval 间隔时间 (秒)
    /// @param callback 每次到期回调
    /// @return 定时器句柄
    LIMX_NODISCARD FTimerHandle SetTimerLoop(
        Float32 interval, TFunction<void()> callback)
    {
        TimerEntry entry;
        entry.Handle = FTimerHandle(m_NextId++);
        entry.Callback = MoveTemp(callback);
        entry.Delay = interval;
        entry.Remaining = interval;
        entry.IsLooping = true;
        entry.IsPendingRemoval = false;
        m_Timers.Add(MoveTemp(entry));
        return entry.Handle;
    }

    // ========================================================================
    // 取消
    // ========================================================================

    /// 取消定时器
    void CancelTimer(FTimerHandle handle)
    {
        for (SizeType index = 0;
             index < m_Timers.GetSize(); ++index)
        {
            if (m_Timers[index].Handle == handle)
            {
                m_Timers[index].IsPendingRemoval = true;
                return;
            }
        }
    }

    /// 取消所有定时器
    void CancelAll()
    {
        m_Timers.Clear();
    }

    // ========================================================================
    // 驱动
    // ========================================================================

    /// 推进时间 — 触发到期的定时器回调
    /// @param deltaTime 帧间隔 (秒)
    void Tick(Float32 deltaTime)
    {
        // 遍历并更新 (使用索引因为回调可能注册新定时器)
        SizeType count = m_Timers.GetSize();
        for (SizeType index = 0; index < count; ++index)
        {
            TimerEntry& timer = m_Timers[index];
            if (timer.IsPendingRemoval) continue;

            timer.Remaining -= deltaTime;

            if (timer.Remaining <= 0.0f)
            {
                // 触发回调
                timer.Callback();

                if (timer.IsLooping)
                {
                    // 重置剩余时间 (补偿超出部分)
                    timer.Remaining += timer.Delay;
                    if (timer.Remaining < 0.0f)
                    {
                        timer.Remaining = timer.Delay;
                    }
                }
                else
                {
                    timer.IsPendingRemoval = true;
                }
            }
        }

        // 清理已标记的定时器
        RemovePending();
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 定时器是否活跃
    LIMX_NODISCARD bool IsTimerActive(FTimerHandle handle) const
    {
        for (SizeType index = 0;
             index < m_Timers.GetSize(); ++index)
        {
            if (m_Timers[index].Handle == handle &&
                !m_Timers[index].IsPendingRemoval)
            {
                return true;
            }
        }
        return false;
    }

    /// 获取定时器剩余时间 (-1 表示未找到)
    LIMX_NODISCARD Float32 GetRemainingTime(
        FTimerHandle handle) const
    {
        for (SizeType index = 0;
             index < m_Timers.GetSize(); ++index)
        {
            if (m_Timers[index].Handle == handle &&
                !m_Timers[index].IsPendingRemoval)
            {
                return m_Timers[index].Remaining;
            }
        }
        return -1.0f;
    }

    /// 活跃定时器数量
    LIMX_NODISCARD SizeType GetActiveCount() const
    {
        SizeType count = 0;
        for (SizeType index = 0;
             index < m_Timers.GetSize(); ++index)
        {
            if (!m_Timers[index].IsPendingRemoval)
            {
                ++count;
            }
        }
        return count;
    }

private:
    /// 清理标记为待删除的定时器
    void RemovePending()
    {
        SizeType writeIndex = 0;
        for (SizeType readIndex = 0;
             readIndex < m_Timers.GetSize(); ++readIndex)
        {
            if (!m_Timers[readIndex].IsPendingRemoval)
            {
                if (writeIndex != readIndex)
                {
                    m_Timers[writeIndex] =
                        MoveTemp(m_Timers[readIndex]);
                }
                ++writeIndex;
            }
        }

        while (m_Timers.GetSize() > writeIndex)
        {
            m_Timers.RemoveLast();
        }
    }

    TArray<TimerEntry> m_Timers;   ///< 定时器列表
    UInt32             m_NextId;   ///< 下一个句柄 ID
};

} // namespace Limx
