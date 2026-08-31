// ============================================================
// 文件名称：RayTracingCheck.h
// 创建时间：2026-08-31
// 创建者  ：LimxTeam
// 设计哲学：判据的价值不在它有多严，而在它失败时能不能被看见。加速结构
//          建错了不会崩、不会报错，只是所有射线都不命中或命中错地方 ——
//          而那在画面上与"这里本来就没东西"完全一样。所以这里不看画面，
//          直接把 GPU 遍历出的命中距离与 CPU 的解析解逐条比对。
// 功能描述：光线追踪加速结构的 GPU-CPU 交叉验证自检入口。
// 技术特性：CPU 侧用 Möller-Trumbore 逐三角形求交作为独立参考实现；
//          带"这批射线够不够判"的元判据 —— 命中/未命中/多实例/遮挡
//          任一缺失即判失败，避免退化的射线集让错误实现照样通过。
// ============================================================

#pragma once

#include "Core/CoreMinimal.h"

namespace Limx
{

class IRHIDevice;
class FRenderContext;

/// 运行光追加速结构的 GPU-CPU 交叉验证
///
/// @return 全部判据通过为 true。设备不支持光追时返回 false 并记录 Error ——
///         "跳过"绝不能表现为"通过", 否则这条判据在任何不支持的机器上
///         都是空的, 而那正是它最需要说话的场合。
LIMX_NODISCARD bool RunRayTracingChecks(IRHIDevice* device,
                                        FRenderContext* context);

} // namespace Limx
