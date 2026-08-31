/*******************************************************************************
 * 文件: FOctahedral.h
 * 创建时间: 2026-08-31
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   八面体法线编解码的 CPU 实现 — 与 Shaders/Builtin/gbuffer_common.h 一致
 *
 * 设计哲学:
 *   与着色器逐行对应 — 这份实现存在的唯一理由是让编解码的正确性可以脱离
 *     GPU 测试。真实硬件上要构造出"编码后再解码"的对照极其麻烦, 而这套
 *     算法的风险恰恰在边界方向 (z 恰好为 0、分量恰好为 0) 上, 那些方向
 *     在真实场景里出现的概率很低但一定会出现。
 *
 *     两份实现必须逐行对应。它们不一致时的表现是: CPU 测试全绿, 而画面上
 *     法线整体偏斜 —— 而偏斜看起来像是光照参数不对。改动任一份都要同步
 *     改另一份, 这一点没有编译期保障, 只能靠这条注释和用例。
 *
 * 技术特性:
 *   - 覆盖整个球面 (含 -Z 半球), 误差分布远比 "存 xy 求 z" 均匀
 *   - sign(0) 在 GLSL 里返回 0, 会让整条向量塌成零 —— 两份实现都必须
 *     显式写成"非负取 +1"
 *
 * 依赖关系:
 *   内部: Core/Math/FVector.h
 *
 ******************************************************************************/

#pragma once

#include "RenderCore/RenderCoreMinimal.h"

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Math/FVector.h"
#include "Core/Math/FMath.h"

namespace Limx
{

/// 按八面体折叠规则翻折 [-1,1]^2 里的点
///
/// 与 gbuffer_common.h 的 OctWrap 逐行对应。
LIMX_NODISCARD inline FVector2 OctWrap(FVector2 v)
{
    // 必须是"非负取 +1"而不是 sign()。
    //
    // GLSL 的 sign(0) 返回 0, 那会让整条向量塌成零向量 —— 而分量恰好为 0
    // 的方向 (比如 (1,0,0) 这类轴向) 在真实场景里到处都是。
    const Float32 signX = (v.X >= 0.0f) ? 1.0f : -1.0f;
    const Float32 signY = (v.Y >= 0.0f) ? 1.0f : -1.0f;

    return FVector2((1.0f - FMath::Abs(v.Y)) * signX,
                    (1.0f - FMath::Abs(v.X)) * signY);
}

/// 单位向量 → [-1,1]^2
LIMX_NODISCARD inline FVector2 EncodeOctahedralNormal(FVector3 n)
{
    const Float32 sum = FMath::Abs(n.X) + FMath::Abs(n.Y) + FMath::Abs(n.Z);

    if (sum < 1.0e-12f)
    {
        // 零向量没有方向可言。返回 (0,0), 解码后是 +Z —— 一个合法方向。
        return FVector2(0.0f, 0.0f);
    }

    n = n * (1.0f / sum);

    const FVector2 xy(n.X, n.Y);

    return (n.Z >= 0.0f) ? xy : OctWrap(xy);
}

/// [-1,1]^2 → 单位向量
LIMX_NODISCARD inline FVector3 DecodeOctahedralNormal(FVector2 e)
{
    FVector3 n(e.X, e.Y, 1.0f - FMath::Abs(e.X) - FMath::Abs(e.Y));

    if (n.Z < 0.0f)
    {
        const FVector2 wrapped = OctWrap(FVector2(n.X, n.Y));
        n.X = wrapped.X;
        n.Y = wrapped.Y;
    }

    const Float32 lengthSq = n.X * n.X + n.Y * n.Y + n.Z * n.Z;

    if (lengthSq < 1.0e-12f)
    {
        return FVector3(0.0f, 0.0f, 1.0f);
    }

    const Float32 inverseLength = 1.0f / FMath::Sqrt(lengthSq);

    return FVector3(n.X * inverseLength,
                    n.Y * inverseLength,
                    n.Z * inverseLength);
}

} // namespace Limx
