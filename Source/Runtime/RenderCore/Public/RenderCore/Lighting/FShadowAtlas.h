/*******************************************************************************
 * 文件: FShadowAtlas.h
 * 创建时间: 2026-08-31
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   阴影图集的分块与聚光灯阴影矩阵 — 纯 CPU, 可脱离 GPU 测
 *
 * 设计哲学:
 *   分块与矩阵这两件事都放在这里, 是因为它们的错误方式相同: **不崩、不报错、
 *     只是阴影落在错误的位置**。而阴影落错位置在画面上极难与"阴影偏移参数
 *     没调好"区分开 —— 后者是个人人都会先怀疑的方向。所以这两件事必须能
 *     用数值断言钉死。
 *
 *   图集用固定分块而非动态打包 — 动态打包 (按光源重要性分配不同尺寸) 能省
 *     显存, 但它引入了一个跨帧的状态: 某个光源这一帧拿到 512、下一帧拿到
 *     256, 阴影分辨率突变。固定分块把这个状态消掉了, 代价是显存按最坏情况
 *     开。
 *
 *   上向量的退化是这里最锋利的一个坑 — 聚光灯直直朝下时, 用 (0,1,0) 作上
 *     向量会让叉积为零, 视图矩阵整个塌掉。而"灯朝正下方"恰恰是最常见的
 *     摆法 (路灯、射灯), 所以这不是边角情形。
 *
 * 依赖关系:
 *   内部: Core/Math/FVector.h, Core/Math/FMatrix.h, Core/Math/FMath.h
 *
 ******************************************************************************/

#pragma once

#include "RenderCore/RenderCoreMinimal.h"

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Math/FVector.h"
#include "Core/Math/FMatrix.h"
#include "Core/Math/FMath.h"

namespace Limx
{

// ============================================================================
// 图集尺寸
// ============================================================================

/// 图集边长 (纹素)
inline constexpr UInt32 kShadowAtlasSize = 4096;

/// 每块的边长 (纹素)
///
/// 512 是在"够清晰"与"够多块"之间的取舍: 4096 的图集能切出 64 块, 而单块
/// 512 对一盏照射半径十几米的聚光灯来说, 每纹素约 3 厘米 —— 接触阴影的
/// 锯齿刚好在可接受的边缘。切成 256 能有 256 块, 但那时锯齿明显可见。
inline constexpr UInt32 kShadowTileSize = 512;

/// 每边的块数
inline constexpr UInt32 kShadowTilesPerRow = kShadowAtlasSize / kShadowTileSize;

/// 块总数
inline constexpr UInt32 kShadowTileCount =
    kShadowTilesPerRow * kShadowTilesPerRow;

/// 无效块下标
inline constexpr UInt32 kInvalidShadowTile = 0xFFFFFFFFu;

// ============================================================================
// 分块
// ============================================================================

/// 图集里一块的矩形 (纹素坐标)
struct FShadowTileRect
{
    UInt32 X    = 0;
    UInt32 Y    = 0;
    UInt32 Size = 0;
};

/// 块下标 → 图集里的矩形
///
/// 行优先: 下标 0 在左上, 沿 +x 走满一行再换行。计算着色器与片段着色器都
/// 按这个约定换算, 两处不一致的表现是阴影取到别的光源那一块 —— 画面上是
/// "这盏灯的阴影形状完全不对", 而不是"没有阴影"。
LIMX_NODISCARD inline FShadowTileRect ShadowTileRect(UInt32 tileIndex)
{
    FShadowTileRect rect;

    if (tileIndex >= kShadowTileCount)
    {
        return rect;
    }

    rect.X    = (tileIndex % kShadowTilesPerRow) * kShadowTileSize;
    rect.Y    = (tileIndex / kShadowTilesPerRow) * kShadowTileSize;
    rect.Size = kShadowTileSize;

    return rect;
}

/// 块下标 → 图集里的 UV 范围 (采样时用)
///
/// 返回 (offsetU, offsetV, scaleU, scaleV): 块内归一化坐标 t 映射到图集的
/// UV 是 offset + t * scale。
LIMX_NODISCARD inline FVector4 ShadowTileUvTransform(UInt32 tileIndex)
{
    if (tileIndex >= kShadowTileCount)
    {
        return FVector4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    const Float32 scale =
        static_cast<Float32>(kShadowTileSize) /
        static_cast<Float32>(kShadowAtlasSize);

    const Float32 offsetU =
        static_cast<Float32>(tileIndex % kShadowTilesPerRow) * scale;
    const Float32 offsetV =
        static_cast<Float32>(tileIndex / kShadowTilesPerRow) * scale;

    return FVector4(offsetU, offsetV, scale, scale);
}

// ============================================================================
// 聚光灯的阴影矩阵
// ============================================================================

/// 给定方向选一个不与之平行的上向量
///
/// **这是本文件最容易出错的一处。** 聚光灯直直朝下 (0,-1,0) 时, 用常见的
/// (0,1,0) 作上向量会让 cross(forward, up) 为零向量, 视图矩阵整个塌掉 ——
/// 而"灯朝正下方"恰恰是最常见的摆法 (路灯、射灯), 不是边角情形。
///
/// 判据用 |dir.y| 而不是 dir 与 up 的点积: 前者只看一个分量, 没有累积误差,
/// 而且阈值的含义直观 (方向与竖直轴的夹角小于约 8 度就换轴)。
LIMX_NODISCARD inline FVector3 ShadowUpVectorFor(const FVector3& direction)
{
    // 0.99 对应约 8 度。取得再接近 1 的话, 接近竖直但没到的方向会算出一个
    // 极短的 right 向量, 归一化之后噪声被放大 —— 阴影会随光源方向的微小
    // 变化而抖动。
    if (FMath::Abs(direction.Y) > 0.99f)
    {
        return FVector3(0.0f, 0.0f, 1.0f);
    }

    return FVector3(0.0f, 1.0f, 0.0f);
}

/// 聚光灯的视图投影矩阵
///
/// @param position     光源世界位置
/// @param direction    光照方向 (单位向量, 从光源指出)
/// @param outerConeCos 外锥角的余弦
/// @param range        衰减距离 (作为远平面)
///
/// 视场角取外锥角的两倍 —— 阴影贴图必须覆盖整个光锥, 少一点就在锥边缘漏光。
/// 不多留余量是因为多留等于降低有效分辨率, 而聚光灯的锥外本来就没有光。
LIMX_NODISCARD inline FMatrix ComputeSpotShadowMatrix(
    const FVector3& position,
    const FVector3& direction,
    Float32         outerConeCos,
    Float32         range)
{
    const FVector3 forward = direction.GetSafeNormal();

    const FVector3 target(position.X + forward.X,
                          position.Y + forward.Y,
                          position.Z + forward.Z);

    const FMatrix view =
        FMatrix::LookAt(position, target, ShadowUpVectorFor(forward));

    // 外锥角的两倍就是所需视场角。余弦钳在 [-1, 1] —— 越界会让 acos 给出
    // NaN, 而 NaN 投影矩阵的表现是那盏灯的阴影整片消失, 没有任何报错。
    const Float32 clampedCos = FMath::Clamp(outerConeCos, -0.9999f, 0.9999f);

    const Float32 fov = 2.0f * FMath::ACos(clampedCos);

    // 近平面取 0.05: 太小会浪费深度精度 (自遮挡加重), 太大则贴着灯的物体
    // 被裁掉、不再投影。
    const Float32 nearPlane = 0.05f;
    const Float32 farPlane  = FMath::Max(range, nearPlane * 2.0f);

    const FMatrix projection =
        FMatrix::Perspective(fov, 1.0f, nearPlane, farPlane);

    return projection * view;
}

// ============================================================================
// 交给 GPU 的每块数据
// ============================================================================

/// 聚光灯阴影的默认深度偏移 —— **世界单位**, 沿光照方向
///
/// 级联那一套的偏移量是加在 NDC 深度上的, 这里不能照抄。原因是透视投影下
/// NDC 深度与距离的关系是 1/d 型的: 近远平面取 0.05 与 30 时,
///
///     d(ndc)/dd = near·far/((far-near)·d²) ≈ 0.05/d²
///
/// 在 d=6 处只有 0.0014 每单位。照搬级联的 0.0015 会把采样点沿光线推开
/// **一整个世界单位**, 而推 0.004 是近三米 —— 阴影整片脱离物体
/// (peter-panning), 看起来却像"这盏灯根本没有阴影"。
///
/// 改成世界单位之后, 这个数与近远平面、与光源 range 都无关: 它就是"把接收
/// 点朝光源挪 3 厘米"。挪多少合适只取决于场景的尺度, 而那是唯一该由人判断
/// 的量。
///
/// 每盏灯的实际偏移随 FSpotShadowData 一起上传, 所以将来按 range 自适应
/// 不需要动着色器。
inline constexpr Float32 kSpotShadowDepthBias = 0.03f;

/// 一块阴影图集对应的 GPU 数据 (std430, 96 字节)
///
/// **UV 变换随矩阵一起上传, 而不是让着色器按块下标现算。**
///
/// 现算意味着着色器要复制一份图集布局的知识 (每行几块、块多大), 而那份
/// 复制品无法被单元测试覆盖 —— 它与 C++ 侧漂移时的表现是"阴影取到了隔壁
/// 灯那一块", 画面上看是形状完全不对的影子。上传的话, 布局只有 C++ 一处
/// 定义, 而那一处已经被 ShadowAtlasTests 逐值钉死。
///
/// 代价是每盏灯多 16 字节。64 块合计 1 KiB。
struct FSpotShadowData
{
    /// 世界 → 该灯阴影贴图的裁剪空间
    FMatrix ViewProj;

    /// 块内归一化坐标 → 图集 UV: offset + t * scale
    Float32 OffsetU = 0.0f;
    Float32 OffsetV = 0.0f;
    Float32 ScaleU  = 0.0f;
    Float32 ScaleV  = 0.0f;

    /// x=深度偏移, y=图集边长 (纹素), z/w=保留
    Float32 DepthBias  = kSpotShadowDepthBias;
    Float32 AtlasSize  = static_cast<Float32>(kShadowAtlasSize);
    Float32 ParamsPad0 = 0.0f;
    Float32 ParamsPad1 = 0.0f;
};

static_assert(sizeof(FSpotShadowData) == 96,
    "FSpotShadowData 必须为 96 字节 (mat4 + 2×vec4, std430) — 与 pbr.frag 的 "
    "SpotShadowData 一致, 由 Scripts/verify-shader-layout.ps1 逐次核对");

/// 填好一块的 GPU 数据
///
/// 单独成函数而非在调用点拼: 分块下标同时决定 UV 变换与图集里的绘制位置,
/// 两处各写一遍时可以不一致, 而不一致的表现是阴影落在别的灯的块上 ——
/// 不崩、不报错。
LIMX_NODISCARD inline FSpotShadowData MakeSpotShadowData(UInt32 tileIndex,
                                                         const FMatrix& viewProj)
{
    FSpotShadowData data;

    data.ViewProj = viewProj;

    const FVector4 uv = ShadowTileUvTransform(tileIndex);

    data.OffsetU = uv.X;
    data.OffsetV = uv.Y;
    data.ScaleU  = uv.Z;
    data.ScaleV  = uv.W;

    data.DepthBias = kSpotShadowDepthBias;
    data.AtlasSize = static_cast<Float32>(kShadowAtlasSize);

    return data;
}

} // namespace Limx
