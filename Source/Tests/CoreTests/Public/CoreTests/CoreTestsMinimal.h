/*******************************************************************************
 * 文件: CoreTestsMinimal.h
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   CoreTests 预编译头 — 聚合测试框架与被测的 Core 公开设施
 *
 * 设计哲学:
 *   测试文件零样板 — 每个测试 .cpp 只需包含本头文件即可直接写 LIMX_TEST
 *
 * 依赖关系:
 *   Testing/TestingMinimal.h + Core 各子系统公开头
 *
 ******************************************************************************/

#pragma once

#include "Testing/TestingMinimal.h"

// 被测的 Core 子系统
#include "Core/Containers/TArray.h"
#include "Core/Containers/FString.h"
#include "Core/Containers/TMap.h"
#include "Core/Containers/TSet.h"
#include "Core/Templates/TUniquePtr.h"
#include "Core/Templates/TSharedPtr.h"
#include "Core/Templates/TOptional.h"
#include "Core/Memory/LinearAllocator.h"
#include "Core/Memory/FStackAllocator.h"
#include "Core/Memory/BlockAllocator.h"
#include "Core/Memory/FPoolAllocator.h"
#include "Core/Math/FMath.h"
#include "Core/Math/FVector.h"
#include "Core/Math/FQuat.h"
#include "Core/Math/FMatrix.h"
#include "Core/Math/FTransform.h"
#include "Core/Misc/FJson.h"
#include "Core/Misc/FInflate.h"
#include "Core/Misc/FBase64.h"
#include "Core/Containers/FStringBuilder.h"
