// ============================================================================
// gbuffer_common.h — 薄 G-Buffer 的编解码
//
// 被深度预通道 (写) 与后续的 GTAO / TAA (读) 共同包含。编码与解码放在同一
// 个文件里, 是为了让"写进去的和读出来的是同一套约定"成为结构上的事实 ——
// 分散在两处的话, 改了一边忘了另一边的表现是法线整体偏斜, 而画面上看着
// 像是光照参数不对。
// ============================================================================

#ifndef LIMX_GBUFFER_COMMON_H
#define LIMX_GBUFFER_COMMON_H

// ============================================================================
// 八面体法线编码
// ============================================================================
//
// 把单位向量映射到 [-1,1]^2。相比直接存 xy 再由 z = sqrt(1-x²-y²) 还原,
// 八面体编码覆盖整个球面 (含 -Z 半球), 且误差分布均匀得多。
//
// 存进 RG16_SFLOAT: 半精度在 [-1,1] 的精度约 5e-4, 对应约 0.03° 的角度
// 误差 —— 对 AO 与反射足够。
//
// 参考: Cigolle et al., "A Survey of Efficient Representations for
// Independent Unit Vectors" (JCGT 2014)。

/// 把 [-1,1]^2 里的点按八面体折叠规则翻折
vec2 OctWrap(vec2 v)
{
    // sign(0) 在 GLSL 里返回 0, 会让整条向量塌成 0。这里必须是"非负取 +1",
    // 所以显式判断而不是用 sign()。
    vec2 signNotZero = vec2(
        (v.x >= 0.0) ? 1.0 : -1.0,
        (v.y >= 0.0) ? 1.0 : -1.0);

    return (1.0 - abs(v.yx)) * signNotZero;
}

/// 单位向量 → [-1,1]^2
vec2 EncodeOctahedralNormal(vec3 n)
{
    // 归一化到八面体表面: 三个分量的绝对值之和为 1
    n /= (abs(n.x) + abs(n.y) + abs(n.z));

    // 下半球折叠到上半球的外围
    n.xy = (n.z >= 0.0) ? n.xy : OctWrap(n.xy);

    return n.xy;
}

/// [-1,1]^2 → 单位向量
vec3 DecodeOctahedralNormal(vec2 e)
{
    vec3 n = vec3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));

    if (n.z < 0.0)
    {
        n.xy = OctWrap(n.xy);
    }

    return normalize(n);
}

// ============================================================================
// 速度矢量
// ============================================================================

/// 由当前与上一帧的齐次裁剪坐标算屏幕空间速度
///
/// 返回 NDC 空间的差值 (范围约 [-2,2], 实际远小于此)。存进 RG16_SFLOAT。
///
/// 除以 w 之前必须确认 w 不为零 —— 位于近裁面上的顶点 w 可以任意小, 而
/// 除出来的 NDC 会飞到天上。那种像素的速度本来就没有意义, 直接给 0。
vec2 ComputeVelocity(vec4 currentClip, vec4 previousClip)
{
    const float kMinW = 1e-6;

    if (abs(currentClip.w) < kMinW || abs(previousClip.w) < kMinW)
    {
        return vec2(0.0);
    }

    vec2 currentNdc  = currentClip.xy  / currentClip.w;
    vec2 previousNdc = previousClip.xy / previousClip.w;

    return currentNdc - previousNdc;
}

#endif // LIMX_GBUFFER_COMMON_H
