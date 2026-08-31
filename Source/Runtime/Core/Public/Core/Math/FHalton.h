/*******************************************************************************
 * 文件: FHalton.h
 * 创建时间: 2026-08-31
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   Halton 低差异序列 — 时域抗锯齿的亚像素采样点
 *
 * 设计哲学:
 *   低差异而非随机 — TAA 每帧把采样点挪动不到一个像素, 若干帧的结果加权
 *     平均得到超采样的效果。这要求这些点在像素内**均匀铺开**: 随机点会
 *     成团, 成团处的权重叠加, 表现为静止画面上缓慢游走的亮暗斑。
 *
 *     Halton 序列的构造保证了任意前缀都是均匀的 —— 也就是说无论 TAA 的
 *     历史窗口是 4 帧还是 16 帧, 那几帧的采样点都铺得开。这一点随机数
 *     做不到, 而且做不到的时候没有任何报错。
 *
 *   从下标 1 开始 — RadicalInverse(0) 恒为 0, 那是像素的角点而不是内部
 *     的一个采样位置。用 0 号点意味着第一帧完全没有偏移, 而 TAA 的历史
 *     从那一帧起步, 相当于给它一个偏心的起点。
 *
 * 技术特性:
 *   - 基 2 与基 3 组成二维序列 (互质是必要条件, 否则两轴相关)
 *   - 纯整数循环, 不依赖任何浮点状态
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h, Core/Math/FMatrix.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Math/FMatrix.h"
#include "Core/Math/FVector.h"

namespace Limx
{

/// 基 base 下的 radical inverse — Halton 序列的构造块
///
/// 把 index 的 base 进制表示按小数点镜像翻转: index = 5, base = 2 时
/// 5 = 101b, 翻转成 0.101b = 0.625。
///
/// @param index 序列下标 (从 1 开始 —— 0 恒返回 0, 那是角点不是采样点)
/// @param base  进制, 必须 >= 2
LIMX_NODISCARD inline Float32 RadicalInverse(UInt32 index, UInt32 base)
{
    if (base < 2u)
    {
        // 基小于 2 时"翻转"没有定义, 而循环会永不终止 (index /= 1)。
        // 返回 0 而不是死循环 —— 死循环在渲染线程上表现为整个程序卡住,
        // 排查起来比一个偏心的采样点昂贵得多。
        return 0.0f;
    }

    Float32 result   = 0.0f;
    Float32 fraction = 1.0f / static_cast<Float32>(base);

    while (index > 0u)
    {
        result += static_cast<Float32>(index % base) * fraction;

        index /= base;
        fraction /= static_cast<Float32>(base);
    }

    return result;
}

/// 二维 Halton 采样点, 值域 [0,1)
///
/// 基取 2 与 3。两个基必须互质 —— 取 2 与 4 的话两轴完全相关, 采样点会
/// 全部落在一条对角线上, 而那看起来仍然"在动", 只是永远补不满像素。
///
/// @param index 序列下标 (从 1 开始)
/// @param outX  基 2 分量
/// @param outY  基 3 分量
inline void Halton2D(UInt32 index, Float32& outX, Float32& outY)
{
    outX = RadicalInverse(index, 2u);
    outY = RadicalInverse(index, 3u);
}

/// TAA 的亚像素偏移, 值域 (-0.5, 0.5] 像素
///
/// 把 [0,1) 的 Halton 点平移到以像素中心为原点。不平移的话整个采样图案
/// 偏在像素的一侧, 累积结果相对真实几何有半个像素的系统性偏移 —— 表现
/// 为开启 TAA 后画面整体挪了半像素, 而那看起来像是"TAA 让画面变糊了"。
inline void HaltonJitterPixels(UInt32 index, Float32& outX, Float32& outY)
{
    Halton2D(index, outX, outY);

    outX -= 0.5f;
    outY -= 0.5f;
}

/// 把亚像素偏移应用到投影矩阵上
///
/// @param projection 待修改的投影矩阵 (行主序存放, 列向量约定)
/// @param jitterNdc  NDC 单位的偏移 (像素偏移 * 2 / 视口尺寸)
///
/// 改的是第 2 列而不是第 3 列, 这一点是本函数存在的全部理由。
///
/// 裁剪空间的 w 来自矩阵第 3 行, 而 FMatrix::Perspective 里 M[3][2] = -1,
/// 即 w_clip = -z_view。要让 clip.x 增加 jx * w_clip (从而使 NDC 的 x 增加
/// 恒定的 jx, **与深度无关**), 就得让 M[0][2] 减去 jx。
///
/// 常见的错法是写成 M[0][3] += jx —— 那一项乘的是输入向量的 w (恒为 1),
/// 于是 clip.x 增加恒定的 jx, 而 NDC 的 x 增加 jx / w_clip: 近处物体抖得
/// 多、远处几乎不抖。画面看上去仍然"在抖", 只是远处永远补不满, 表现为
/// "TAA 对远景没效果"。两种写法的差别在任何单帧截图上都看不出来。
inline void ApplyJitterToProjection(FMatrix& projection, FVector2 jitterNdc)
{
    projection.M[0][2] -= jitterNdc.X;
    projection.M[1][2] -= jitterNdc.Y;
}

} // namespace Limx
