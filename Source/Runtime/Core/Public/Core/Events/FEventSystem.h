/*******************************************************************************
 * 文件: FEventSystem.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   事件系统聚合入口 — 一次包含获得完整事件系统基础设施
 *
 *   事件系统层次:
 *   ┌─────────────────────────────────────────────────────┐
 *   │  FEvent.h           — 事件基类 + 优先级枚举 + 宏    │
 *   │  FEventDispatcher.h — 局部分发器 (对象内部通信)     │
 *   │  FEventBus.h        — 全局总线 (跨系统广播)         │
 *   │  TEventQueue.h      — 延迟队列 (帧末批量分发)       │
 *   │  TObserver.h        — 观察者 (RAII 自动解绑)        │
 *   │  TDelegate.h        — 单/多播委托 (点对点回调)      │
 *   └─────────────────────────────────────────────────────┘
 *
 *   使用指南:
 *   - 对象内事件 (输入→角色):  FEventDispatcher
 *   - 引擎系统间广播:          FEventBus::Get()
 *   - 帧末统一处理:            TEventQueue + Flush()
 *   - 一次性或临时订阅:        TObserver / FAutoEventListener
 *   - 直接回调绑定:            TDelegate
 *
 ******************************************************************************/

#pragma once

#include "Core/Events/FEvent.h"
#include "Core/Events/FEventDispatcher.h"
#include "Core/Misc/FEventBus.h"
#include "Core/Templates/TEventQueue.h"
#include "Core/Templates/TObserver.h"
#include "Core/Templates/TDelegate.h"
