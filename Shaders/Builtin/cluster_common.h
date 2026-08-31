// ============================================================================
// cluster_common.h — 分簇光照的网格数学 (GLSL 侧)
//
// 与 Source/Runtime/RenderCore/Public/RenderCore/Lighting/FClusterGrid.h
// **逐行对应**。那份 CPU 实现存在的理由是让这套数学的正确性可以脱离 GPU
// 测 —— 分簇错了的表现是"某些角度下某些光突然不亮", 在真实场景里极难复现
// 更难归因, 而风险全在边界上 (恰好落在切片边界的深度、恰好擦过簇角的光球)。
//
// 两份不一致时的表现是: CPU 用例全绿, 而画面上某些簇少算了光。这一点**没有
// 编译期保障** —— 只能靠这条注释、CPU 用例, 以及 --light-cull-check 的
// 逐像素比对 (那一条能真正抓住漂移: 分簇结果与暴力法必须完全一致)。
//
// 网格常量在 C++ 侧是 kClusterGridX/Y/Z, 这里是 CLUSTER_GRID_X/Y/Z。改动
// 任一侧都要同步改另一侧, 且要重新生成 Content/Baselines 的图像基线。
// ============================================================================

#ifndef LIMX_CLUSTER_COMMON_H
#define LIMX_CLUSTER_COMMON_H

// 与 FClusterGrid.h 的 kClusterGridX/Y/Z 一致
#define CLUSTER_GRID_X 32
#define CLUSTER_GRID_Y 18
#define CLUSTER_GRID_Z 32

#define CLUSTER_COUNT (CLUSTER_GRID_X * CLUSTER_GRID_Y * CLUSTER_GRID_Z)

/// 视空间深度 → 切片下标
///
/// 与 FClusterGrid.h 的 SliceForViewDepth 逐行对应, 包括两处钳位。
///
/// 钳位不是防御性编程的装饰: 近平面之前与远平面之后的片元本来就不该被绘制,
/// 但深度预通道的浮点误差会让贴着近平面的片元算出略小于 near 的深度。不钳
/// 的话 log2 给出负数, 转成 uint 后是一个巨大的值 —— 越界读取簇表。
uint SliceForViewDepth(float viewDepth, float sliceScale, float sliceBias)
{
    if (viewDepth <= 0.0)
    {
        return 0u;
    }

    float raw = log2(viewDepth) * sliceScale + sliceBias;

    if (raw <= 0.0)
    {
        return 0u;
    }

    uint slice = uint(raw);

    return min(slice, uint(CLUSTER_GRID_Z - 1));
}

/// 三维簇下标 → 线性下标
///
/// x 变化最快、z 最慢。计算着色器按这个顺序写表、片段着色器按这个顺序查表
/// —— 两边不一致的表现是光照整体错位到别的屏幕区域, 而画面依然"有光",
/// 看起来像是光源位置摆错了。
uint ClusterLinearIndex(uint clusterX, uint clusterY, uint clusterZ)
{
    return clusterX +
           clusterY * uint(CLUSTER_GRID_X) +
           clusterZ * uint(CLUSTER_GRID_X * CLUSTER_GRID_Y);
}

/// 屏幕坐标 + 视空间深度 → 线性簇下标
///
/// @param fragCoordXY gl_FragCoord.xy (像素坐标, 原点在左上)
/// @param screenSize  视口尺寸 (像素)
uint ClusterIndexForFragment(vec2 fragCoordXY, vec2 screenSize,
                             float viewDepth,
                             float sliceScale, float sliceBias)
{
    // 用 min 而不是 clamp 的下界: 像素坐标不会是负数, 而多一次比较在这条
    // 每像素都要走的路径上不是零成本。
    uint tileX = min(uint(fragCoordXY.x / screenSize.x * float(CLUSTER_GRID_X)),
                     uint(CLUSTER_GRID_X - 1));
    uint tileY = min(uint(fragCoordXY.y / screenSize.y * float(CLUSTER_GRID_Y)),
                     uint(CLUSTER_GRID_Y - 1));

    uint slice = SliceForViewDepth(viewDepth, sliceScale, sliceBias);

    return ClusterLinearIndex(tileX, tileY, slice);
}

/// 球与轴对齐包围盒是否相交
///
/// 与 FClusterGrid.h 的 SphereIntersectsAABB 逐行对应。判据是"盒内离球心
/// 最近的点到球心的距离是否不超过半径" —— 精确判据, 不是保守近似。
///
/// 写成"盒心到球心的距离减去半对角线"会漏掉贴着盒角的球, 而漏掉的后果是
/// 那个簇少算一盏光。
bool SphereIntersectsAABB(vec3 center, float radius,
                          vec3 boundsMin, vec3 boundsMax)
{
    vec3 closest = clamp(center, boundsMin, boundsMax);
    vec3 delta   = center - closest;

    return dot(delta, delta) <= radius * radius;
}

#endif // LIMX_CLUSTER_COMMON_H
