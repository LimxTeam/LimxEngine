/*******************************************************************************
 * 文件: AssetPipelineAPI.h
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   LIMX_ASSETPIPELINE_API 宏定义
 *
 * 设计哲学:
 *   DLL 边界控制 — 静态库判断优先, 与 LBT 同时定义 _STATIC 与 _EXPORTS 的
 *   行为保持一致 (顺序反了会让静态库被按 dllexport 编译)
 *
 * 依赖关系:
 *   无
 *
 ******************************************************************************/

#pragma once

#if defined(LIMX_ASSETPIPELINE_STATIC)
    #define LIMX_ASSETPIPELINE_API
#elif defined(LIMX_ASSETPIPELINE_EXPORTS)
    #define LIMX_ASSETPIPELINE_API __declspec(dllexport)
#else
    #define LIMX_ASSETPIPELINE_API __declspec(dllimport)
#endif
