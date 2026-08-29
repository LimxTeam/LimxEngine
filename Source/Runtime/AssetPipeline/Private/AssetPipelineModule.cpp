/*******************************************************************************
 * 文件: AssetPipelineModule.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   LimxAssetPipeline 模块入口 + 编译时布局验证
 *
 * 设计哲学:
 *   编译期钉住顶点布局 — 顶点结构的尺寸直接决定顶点缓冲区的步长,
 *   一旦有人无意间加了个成员, GPU 侧的属性偏移会全部错位且难以察觉。
 *   把预期尺寸固化为 static_assert, 让这种改动在构建阶段就被拦下。
 *
 * 依赖关系:
 *   内部: AssetPipeline/AssetPipelineMinimal.h
 *
 ******************************************************************************/

#include "AssetPipeline/AssetPipelineMinimal.h"

namespace Limx
{

// 资产管线日志类别
LIMX_DEFINE_LOG_CATEGORY(LogAssetPipeline)

// ============================================================================
// 编译时验证
// ============================================================================

// 位置(12) + 法线(12) + 切线(16) + UV0(8) + UV1(8) + 颜色(16) = 72 字节
static_assert(sizeof(FMeshVertex) == 72,
              "FMeshVertex 布局已变更 — 顶点缓冲区步长与着色器属性偏移需同步更新");

// 顶点属性按 4 字节浮点排布, 不应出现编译器插入的填充
static_assert(alignof(FMeshVertex) == 4,
              "FMeshVertex 对齐不应超过 4 字节");

// ============================================================================
// API 宏展开校验
//
// 本模块是静态库, API 宏必须展开为空。若 API 头里 _EXPORTS 的判断排在
// _STATIC 之前 (LBT 对静态库两个宏都定义), 宏会展开为 __declspec(dllexport),
// 使含模板成员的导出类触发 C4251 并在 /WX 下变成编译错误。
// 把这一点固化为编译期断言, 让顺序退化在构建阶段就被拦下。
// ============================================================================

static_assert(sizeof(LIMX_STRINGIFY(LIMX_ASSETPIPELINE_API)) == 1,
              "LIMX_ASSETPIPELINE_API 应展开为空 — 检查该模块 API 头中 _STATIC 与 _EXPORTS 的判断顺序");

} // namespace Limx
