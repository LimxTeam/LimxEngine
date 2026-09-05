// ============================================================
// 文件名称：PathTraceCheck.h
// 创建时间：2026-09-02
// 创建者  ：LimxTeam
// 设计哲学：路径追踪器是后面所有实时 GI 的参考答案，所以验它的判据不能是
//          "看起来对"。这里的三条判据一条都不看画面：
//            白炉 —— 纯白环境 + 纯白材质，收敛后必须恰好是 1.0。它对
//                    "好看"完全无感，只对能量对不对有感；
//            能量守恒 —— 封闭自发光盒里 N 次弹射的解析值是
//                    (1-a^(N+1))/(1-a)，逐个 N 对；
//            方差 ∝ 1/N —— 对数坐标上回归，斜率必须接近 -1。它验的是
//                    "样本之间真的独立"，而那是前两条都验不到的。
//          误差预算一律由实测的单样本标准差 σ 推出 σ/√N，不拍脑袋。
// 功能描述：离线参考路径追踪器的判据入口，以及一张 Cornell 盒参考图的
//          离线渲染入口。
// 技术特性：场景全部程序生成（白炉需要可控的环境，导入的资产给不了）；
//          每条判据都带元判据 —— 射线真的打到几何体了、场景真的是封闭的、
//          方差真的不是零，避免退化的场景让错的实现照样通过。
// ============================================================

#pragma once

#include "Core/CoreMinimal.h"

namespace Limx
{

class FRenderContext;

/// 跑离线参考路径追踪器的三条判据
///
/// @return 全部通过为 true。设备不支持光追时返回 false 并记录 Error ——
///         "跳过"绝不能表现为"通过"。
LIMX_NODISCARD bool RunPathTraceChecks(FRenderContext* context);

/// 时域累积: N 帧 x 1 spp 必须等价于 1 帧 x N spp
///
/// 实时 GI 的形状是"每帧 1 spp, 跨帧累积", 而它成立靠的是"跨帧样本真的独立"
/// —— 那不是自动成立的。种子在帧间卡住的话每一帧逐位相同, 画面看起来反而更
/// **稳**, 而稳正是时域累积想要的效果。所以这条判据不看画面, 看方差。
LIMX_NODISCARD bool RunGiAccumulationChecks(FRenderContext* context);

/// 离线渲染一张 Cornell 盒参考图并写成二进制 PPM
///
/// 判据用不到它 —— 它的用途是让"这个路径追踪器到底在算什么"有一张能看的
/// 证据, 以及给后面的实时 GI 一张可以逐像素对的参考图。
LIMX_NODISCARD bool RenderPathTraceReferenceImage(FRenderContext* context,
                                                  const FString& outputPath,
                                                  UInt32 width,
                                                  UInt32 height,
                                                  UInt32 samplesPerPixel);

} // namespace Limx
