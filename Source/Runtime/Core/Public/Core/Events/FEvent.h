/*******************************************************************************
 * 文件: FEvent.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   事件基类 — 所有引擎事件的根类型
 *   提供类型 ID、时间戳、来源对象、可取消标志等通用字段
 *   派生结构体无需继承即可使用 (POD 风格)，基类仅为通用字段容器
 *
 * 设计哲学:
 *   轻量值语义 — 事件对象在栈上创建，广播后不存储
 *   不强制继承 — 任意 POD 结构体均可作为事件类型传入 FEventBus
 *   取消语义 — Consume()/IsConsumed() 支持事件冒泡终止
 *
 * 技术特性:
 *   - FEventBase: 通用事件字段 (类型ID/时间戳/来源/已消费)
 *   - LIMX_EVENT: 快速声明事件的辅助宏
 *   - EEventPriority: 分发优先级枚举
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Misc/FTypeId.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Misc/FTypeId.h"

namespace Limx
{

// ============================================================================
// 事件优先级
// ============================================================================

/// 事件分发优先级 — 数值越小越先处理
enum class EEventPriority : UInt8
{
    Critical  = 0,   ///< 系统级 (输入设备、崩溃恢复)
    High      = 64,  ///< 引擎子系统
    Normal    = 128, ///< 普通游戏逻辑 (默认)
    Low       = 192, ///< 延迟处理、UI 刷新
    Deferred  = 255, ///< 最后处理，帧末尾
};

// ============================================================================
// 事件基类
// ============================================================================

/// 通用事件字段 — 可选基类，也可单独使用
struct FEventBase
{
    FTypeId  TypeId;      ///< 事件类型 ID (自动填充)
    void*    Source;      ///< 事件来源对象 (可为 nullptr)
    bool     IsConsumed;  ///< 是否已被消费 (取消冒泡)

    FEventBase()
        : TypeId(FTypeId())
        , Source(nullptr)
        , IsConsumed(false)
    {
    }

    /// 标记事件已被消费 (阻止后续处理器接收)
    void Consume() { IsConsumed = true; }

    /// 是否已被消费
    LIMX_NODISCARD bool GetIsConsumed() const
    {
        return IsConsumed;
    }
};

// ============================================================================
// 事件声明辅助宏
// ============================================================================

/// 声明一个引擎事件结构体
/// 用法:
///   LIMX_EVENT(FWindowResizeEvent)
///   {
///       UInt32 Width;
///       UInt32 Height;
///   };
#define LIMX_EVENT(EventName) \
    struct EventName : public Limx::FEventBase

/// 声明无字段的简单信号事件
#define LIMX_SIGNAL_EVENT(EventName) \
    struct EventName : public Limx::FEventBase {}

} // namespace Limx
