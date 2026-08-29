/*******************************************************************************
 * 文件: AssetPipelineMinimal.h
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   AssetPipeline 预编译头 — 聚合模块全部公开设施
 *
 * 设计哲学:
 *   一头文件走天下 — 与 Core/CoreMinimal.h 保持一致的聚合约定
 *
 * 包含内容:
 *   AssetPipelineAPI.h  — 导出宏
 *   FAssetTypes.h       — 中性资产数据结构
 *   FObjLoader.h        — Wavefront OBJ / MTL 解析器
 *   FGltfLoader.h       — glTF 2.0 / GLB 解析器
 *
 * 依赖关系:
 *   聚合 AssetPipeline 模块公开头文件
 *
 ******************************************************************************/

#pragma once

#include "Core/CoreMinimal.h"

#include "AssetPipeline/AssetPipelineAPI.h"
#include "AssetPipeline/FAssetTypes.h"
#include "AssetPipeline/FObjLoader.h"
#include "AssetPipeline/FGltfLoader.h"
#include "AssetPipeline/FImageTypes.h"
#include "AssetPipeline/FPngDecoder.h"
#include "AssetPipeline/FJpegDecoder.h"
#include "AssetPipeline/FImageDecoder.h"
#include "AssetPipeline/FAssetRegistry.h"
