// ============================================================================
// gtao.frag — 地平线搜索式环境光遮蔽 (Ground Truth Ambient Occlusion)
//
// 在屏幕空间沿若干个方向搜索"地平线角" —— 从本像素看出去, 每个方向上最高的
// 遮挡物挡到了哪个仰角 —— 再对可见的那段弧做余弦加权积分。
//
// 与更常见的 SSAO (随机采样半球、数有多少采样点被遮挡) 相比, 这里算的是一个
// **积分**而不是一个比例。区别在于余弦加权: 贴着表面掠射方向的遮挡对漫反射
// 的影响远小于正对法线方向的遮挡, 而计数式的 SSAO 把两者一视同仁 —— 表现是
// 浅浅的凹陷被压得过黑。
//
// 深度必须在深度预通道之后、前向通道之前采样。前向通道对深度的 StoreOp 是
// DontCare, 之后内容就是未定义的。
//
// 法线取自 G-Buffer (八面体编码), 而不是从深度差分重建。差分重建出的法线在
// 深度不连续处会给出完全错误的方向, 而那正是 AO 最需要准确的地方 (物体边缘)。
//
// ── 验收 ──
//
// 90 度凹角处余弦加权的可见度**解析值是 0.5** (半个半球被挡住)。但那是
// "搜索半径 → ∞"的极限 —— 有限半径下只看得到墙的一段, 遮挡必然偏小。实测:
//
//     半径 0.8 → 0.679    半径 2 → 0.586    半径 8 → 0.557
//
// 所以 --ao-check 的判据不是"等于 0.5", 而是**随半径增大朝 0.5 单调收敛**。
// 那是这个算法的物理签名, 而写错的实现 (法线没转视空间、角度约定反了、
// 地平线取错方向) 不会有: 它们要么恒为 1, 要么与半径无关, 要么往反方向走。
// ============================================================================

#version 450

#include "gbuffer_common.h"

layout(location = 0) in vec2 fragUV;

layout(location = 0) out float outAO;

layout(set = 0, binding = 0) uniform sampler2D depthBuffer;
layout(set = 0, binding = 1) uniform sampler2D normalBuffer;

// row_major 与 C++ 侧 FMatrix 的行主序存储一致。漏写等于把矩阵整体转置。
layout(row_major, push_constant) uniform Params {
    // 投影矩阵的逆 —— 从 NDC + 深度反算视空间位置
    mat4  inverseProjection;

    // 视矩阵的上 3x3 —— 把 G-Buffer 的世界空间法线送进视空间
    //
    // 存成 mat4 而不是 mat3: std430 里 mat3 每列按 vec4 对齐, 实际占 48 字节
    // 而"看起来"是 36 —— C++ 侧照 36 算偏移就整体错位。用 mat4 让两侧都不必
    // 记住这条规则。
    mat4  view;

    // x = 采样半径 (世界单位), y = 强度, z = 视口宽, w = 视口高
    vec4  params;
} pc;

// 方向数与每方向的步进数。
//
// 4x8 = 32 次采样。方向数少了会出现规则的条纹 (每个方向的地平线角在空间上
// 突变), 步进数少了会漏掉细小的遮挡物。逐像素旋转打散方向, 把条纹换成噪点
// —— 噪点能被 TAA 消掉, 条纹不能。
const int   kDirections = 4;
const int   kSteps      = 8;

const float kPi     = 3.14159265359;
const float kHalfPi = 1.57079632679;

/// 深度 + UV → 视空间位置
vec3 ViewPositionFromDepth(vec2 uv, float depth)
{
    // Vulkan NDC: x,y ∈ [-1,1] 且 y 向下, z ∈ [0,1]
    vec4 clip = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 view = pc.inverseProjection * clip;

    return view.xyz / view.w;
}

/// 采样一点的视空间位置
vec3 SampleViewPosition(vec2 uv)
{
    return ViewPositionFromDepth(uv, texture(depthBuffer, uv).r);
}

/// 每像素的旋转角 —— 交错网格噪声
///
/// 用确定的哈希而不是随机纹理: 随机纹理要多一个绑定和一份显存, 而这个
/// 哈希在空间上足够无关。逐帧不变, 于是静止画面上的噪点也不变 —— TAA
/// 消不掉不变的噪点, 但它本来就是空间噪点而非时域噪点, 不会闪。
float InterleavedGradientNoise(vec2 pixel)
{
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

void main()
{
    float depth = texture(depthBuffer, fragUV).r;

    // 深度为 1 = 天空, 没有几何, 完全不遮挡
    if (depth >= 1.0)
    {
        outAO = 1.0;
        return;
    }

    vec3 viewPos = ViewPositionFromDepth(fragUV, depth);

    // G-Buffer 的法线是世界空间的八面体编码
    vec2 encoded = texture(normalBuffer, fragUV).rg;

    // 哨兵 (见 FDepthPrePass 的清除值): 这里没有几何
    if (abs(encoded.x) > 1.0 || abs(encoded.y) > 1.0)
    {
        outAO = 1.0;
        return;
    }

    vec3 worldNormal = DecodeOctahedralNormal(encoded);
    vec3 viewNormal  = normalize(mat3(pc.view) * worldNormal);

    // 从表面指向相机
    vec3 V = normalize(-viewPos);

    float radius = pc.params.x;

    // 半径投影到屏幕空间。视空间 -z 为前方, 距离越远同样的世界半径占的
    // 像素越少。
    //
    // 投影矩阵的 [0][0] 就是 1/(aspect*tan(fov/2)), 而这里没有投影矩阵本身,
    // 只有它的逆。从逆矩阵反推更绕 —— 直接用"半径在视空间的角尺寸"换算:
    // 屏幕上一个 UV 单位对应视空间深度 |z| 处的宽度是 2*|z|*tanHalfFovX,
    // 而 tanHalfFovX 可以从逆投影矩阵对 NDC (1,0) 的作用取出。
    vec3 rightRay = ViewPositionFromDepth(vec2(1.0, 0.5), depth);
    float halfWidth = abs(rightRay.x - viewPos.x);

    float radiusUV = (halfWidth > 1.0e-6) ? (radius / (2.0 * halfWidth)) : 0.0;

    // 半径小于一个像素时无从搜索
    if (radiusUV * pc.params.z < 1.0)
    {
        outAO = 1.0;
        return;
    }

    vec2 pixel = fragUV * pc.params.zw;

    float rotation = InterleavedGradientNoise(pixel) * kPi;

    float visibility = 0.0;

    for (int d = 0; d < kDirections; ++d)
    {
        float angle = rotation +
                      float(d) * kPi / float(kDirections);

        vec2 dir = vec2(cos(angle), sin(angle));

        // 切片平面: 由视线方向 V 与屏幕方向 dir 张成
        vec3 dir3D = normalize(vec3(dir, 0.0));

        vec3 sliceNormal = normalize(cross(dir3D, V));

        // 法线投影到切片平面上
        vec3 projNormal = viewNormal - sliceNormal * dot(viewNormal, sliceNormal);

        float projLength = length(projNormal);

        if (projLength < 1.0e-4)
        {
            continue;
        }

        vec3 projDir = projNormal / projLength;

        // 投影后的法线相对视线的夹角, 带符号
        float cosN = clamp(dot(projDir, V), -1.0, 1.0);

        // 符号: 法线偏向 +dir 还是 -dir
        float signN = sign(dot(projDir, dir3D));

        // sign() 在恰好为 0 时返回 0, 那会让整个角度塌成 0。
        // 与八面体编码里那个坑同源。
        signN = (signN >= 0.0) ? 1.0 : -1.0;

        float n = signN * acos(cosN);

        // 两侧各搜一遍地平线
        float cosH1 = -1.0;
        float cosH2 = -1.0;

        for (int s = 1; s <= kSteps; ++s)
        {
            float t = float(s) / float(kSteps);

            vec2 offset = dir * radiusUV * t;

            // +dir 一侧
            vec2 uv1 = fragUV + offset;

            if (uv1.x >= 0.0 && uv1.x <= 1.0 && uv1.y >= 0.0 && uv1.y <= 1.0)
            {
                vec3 delta = SampleViewPosition(uv1) - viewPos;

                float len = length(delta);

                if (len > 1.0e-5)
                {
                    // 半径内的遮挡物**全额计入**, 半径外完全不计。
                    //
                    // 这里最初写的是线性衰减 (1 - len/radius), 结果是系统性
                    // 地压低地平线角: 直角凹角处的解析值是 0.5, 而线性衰减
                    // 下实测收敛到 0.735 —— 差了近 50%。原因是遮挡分布在各个
                    // 距离上, 而线性衰减把每一处都按距离打了折。
                    //
                    // 二值判据把半径还原成它本来的含义: 一个**搜索范围**,
                    // 而不是一个权重。改成二值之后实测 R=2 → 0.586,
                    // R=8 → 0.557, 朝 0.5 收敛。
                    //
                    // 代价是遮挡物穿过半径边界时会有跳变。真要软化, 正确的
                    // 做法是衰减**最终的可见度**而不是地平线的余弦 —— 后者
                    // 改变的是几何量本身。
                    float falloff = (len <= radius) ? 1.0 : 0.0;

                    float c = dot(delta / len, V);

                    cosH1 = max(cosH1, mix(-1.0, c, falloff));
                }
            }

            // -dir 一侧
            vec2 uv2 = fragUV - offset;

            if (uv2.x >= 0.0 && uv2.x <= 1.0 && uv2.y >= 0.0 && uv2.y <= 1.0)
            {
                vec3 delta = SampleViewPosition(uv2) - viewPos;

                float len = length(delta);

                if (len > 1.0e-5)
                {
                    // 同上: 二值而非线性衰减
                    float falloff = (len <= radius) ? 1.0 : 0.0;

                    float c = dot(delta / len, V);

                    cosH2 = max(cosH2, mix(-1.0, c, falloff));
                }
            }
        }

        // 地平线角。acos 给出 [0, π], 两侧符号相反。
        float h1 = -acos(clamp(cosH1, -1.0, 1.0));
        float h2 =  acos(clamp(cosH2, -1.0, 1.0));

        // 钳到法线所在的半球内 —— 表面背面的方向本来就看不见
        h1 = n + max(h1 - n, -kHalfPi);
        h2 = n + min(h2 - n,  kHalfPi);

        // 余弦加权的弧积分 (Jimenez et al. 2016)
        //
        // 这一步是 GTAO 与计数式 SSAO 的分界: 它算的是可见弧在余弦权重下的
        // 积分, 而不是"多少个采样点没被挡住"。
        float sinN = sin(n);
        float cosNn = cos(n);

        float a = 0.25 * (-cos(2.0 * h1 - n) + cosNn + 2.0 * h1 * sinN) +
                  0.25 * (-cos(2.0 * h2 - n) + cosNn + 2.0 * h2 * sinN);

        visibility += projLength * a;
    }

    visibility /= float(kDirections);

    // 强度: 1 = 物理值, 大于 1 则加深
    float ao = clamp(visibility, 0.0, 1.0);

    ao = pow(ao, pc.params.y);

    outAO = ao;
}
