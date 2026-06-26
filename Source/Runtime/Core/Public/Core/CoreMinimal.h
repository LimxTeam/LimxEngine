/*******************************************************************************
 * 文件: CoreMinimal.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   引擎最小包含头文件 — 预编译头 (PCH) 的推荐入口
 *   包含 CoreTypes.h (完整类型系统) + ReflectionMacros.h (反射宏)
 *   绝大多数引擎源文件只需 #include "Core/CoreMinimal.h" 即可
 *
 * 设计哲学:
 *   一头文件走天下 — 业务代码的唯一必须包含点
 *   PCH 友好 — 作为预编译头的根文件，加速增量编译
 *   顺序明确 — CoreTypes 先于 ReflectionMacros
 *
 * 包含内容:
 *   CoreTypes.h         — Platform + PlatformTypes + CoreAPI + CoreMacros + TypeTraits
 *   ReflectionMacros.h  — LCLASS/LSTRUCT/LENUM/LPROPERTY/LFUNCTION/LGENERATED_BODY
 *
 * 依赖关系:
 *   聚合 CoreTypes.h 和 ReflectionMacros.h
 *
 ******************************************************************************/

#pragma once

// 完整类型系统基础设施
#include "Core/CoreTypes.h"

// 反射宏定义
#include "Core/Reflection/ReflectionMacros.h"
