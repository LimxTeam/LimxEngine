/*******************************************************************************
 * 文件: AssetTestsMinimal.h
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   AssetTests 预编译头 — 聚合测试框架与资产管线公开设施
 *
 * 设计哲学:
 *   测试数据内嵌 — 用例中的 OBJ/MTL/glTF 文本以字符串字面量给出,
 *   不依赖任何外部资产文件, 使测试在任何检出环境下都可复现。
 *
 * 依赖关系:
 *   Testing/TestingMinimal.h + AssetPipeline/AssetPipelineMinimal.h
 *
 ******************************************************************************/

#pragma once

#include "Testing/TestingMinimal.h"
#include "AssetPipeline/AssetPipelineMinimal.h"
#include "Core/Containers/FStringBuilder.h"
#include "Core/Misc/FBase64.h"
