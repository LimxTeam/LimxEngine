#ifndef LIMX_IBL_COMMON_GLSL
#define LIMX_IBL_COMMON_GLSL

// ============================================================
// IBL 共用数学 — 低差异序列、GGX 重要性采样、几何遮蔽
//
// 预滤波与 BRDF 查找表必须用**同一套**采样函数。这不是为了省代码:
// split-sum 近似把一个积分拆成两半分别预计算, 拆开的前提是两半用同一个
// 法线分布与同一套样本分布。两处各写一份, 只要有一处的 a = roughness²
// 写成了 a = roughness, 两半就不再互补 —— 结果是能量对不上, 而表现只是
// "金属看着有点闷", 完全不像是采样函数写错了。
// ============================================================

const float kIblPi = 3.14159265359;

// ── Van der Corput 反位序 ──
//
// 把整数的二进制位左右翻转再当成小数。它生成的序列在 [0,1) 上极其均匀,
// 用它配合 i/N 得到的 Hammersley 点集比伪随机数收敛快得多 —— 同样的
// 样本数下, 预滤波的噪点会低一个量级。
float RadicalInverseVdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);

    return float(bits) * 2.3283064365386963e-10;  // / 2^32
}

vec2 Hammersley(uint i, uint sampleCount)
{
    return vec2(float(i) / float(sampleCount), RadicalInverseVdC(i));
}

// ── GGX 法线分布 ──
//
// a = roughness² 是 Disney/UE 的重参数化: 直接用 roughness 会让感知上的
// 变化集中在很窄的一段里, 平方之后调节曲线才接近线性。
float DistributionGGX(float NdotH, float roughness)
{
    const float a  = roughness * roughness;
    const float a2 = a * a;

    const float denominator = NdotH * NdotH * (a2 - 1.0) + 1.0;

    return a2 / max(kIblPi * denominator * denominator, 1e-7);
}

// ── 按 GGX 分布重要性采样半程向量 ──
//
// 返回世界空间的 H。给定均匀分布的 Xi, 生成的 H 服从 D(H)·cosθ 分布,
// 因此后续的蒙特卡洛估计里这一项被约掉, 收敛快得多。
vec3 ImportanceSampleGGX(vec2 Xi, vec3 N, float roughness)
{
    const float a = roughness * roughness;

    const float phi      = 2.0 * kIblPi * Xi.x;
    const float cosTheta = sqrt((1.0 - Xi.y) /
                                (1.0 + (a * a - 1.0) * Xi.y));
    const float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));

    const vec3 halfTangent = vec3(sinTheta * cos(phi),
                                  sinTheta * sin(phi),
                                  cosTheta);

    // 切空间 → 世界空间。参考向量要避开与 N 平行的情形, 否则叉积为零、
    // 归一化得到 NaN, 而 NaN 会污染整个纹素。
    const vec3 up = (abs(N.z) < 0.999) ? vec3(0.0, 0.0, 1.0)
                                       : vec3(1.0, 0.0, 0.0);

    const vec3 tangentX = normalize(cross(up, N));
    const vec3 tangentY = cross(N, tangentX);

    return normalize(tangentX * halfTangent.x +
                     tangentY * halfTangent.y +
                     N        * halfTangent.z);
}

// ── Smith 几何遮蔽 (IBL 版) ──
//
// k 的取法与直接光**不同**: 直接光用 (roughness+1)²/8, IBL 用 roughness²/2。
// 这不是笔误 —— 两者对应不同的入射分布假设 (点光源 vs 半球均匀)。
// 混用的后果是掠射角处能量偏差可达十几个百分点, 而画面上只表现为
// "边缘的反射有点弱", 看着像菲涅尔没调好。
float GeometrySchlickGGXIbl(float NdotV, float roughness)
{
    const float a = roughness * roughness;
    const float k = a * 0.5;

    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmithIbl(float NdotV, float NdotL, float roughness)
{
    return GeometrySchlickGGXIbl(NdotV, roughness) *
           GeometrySchlickGGXIbl(NdotL, roughness);
}

// ── 立方体面方向 ──
//
// 与 equirect_to_cube.comp / irradiance_convolve.comp 完全一致。
// 三处必须相同: 任何一处不同都会让那张贴图相对其它两张整体旋转,
// 而这在平滑的预滤波结果上几乎看不出来, 只会让反射方向莫名其妙地偏。
//
// uv ∈ [-1, 1], u 向右, v 向下; 层序为 +X, -X, +Y, -Y, +Z, -Z。
vec3 CubeFaceDirection(uint face, vec2 uv)
{
    if (face == 0u) { return vec3(  1.0, -uv.y, -uv.x); }  // +X
    if (face == 1u) { return vec3( -1.0, -uv.y,  uv.x); }  // -X
    if (face == 2u) { return vec3( uv.x,   1.0,  uv.y); }  // +Y
    if (face == 3u) { return vec3( uv.x,  -1.0, -uv.y); }  // -Y
    if (face == 4u) { return vec3( uv.x, -uv.y,   1.0); }  // +Z
    return vec3(-uv.x, -uv.y, -1.0);                       // -Z
}

#endif // LIMX_IBL_COMMON_GLSL
