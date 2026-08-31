#version 450

// ============================================================================
// gtao_upsample.frag — 半分辨率 AO 的双边上采样
//
// GTAO 在半分辨率上算 (四分之一的像素), 再上采样回全分辨率。实测 GTAO 占
// 整帧 GPU 的 64.6% (1.128 ms / 1.745 ms), 是唯一值得动的那一项。
//
// ── 为什么必须是双边而不是双线性 ──
//
// 双线性会在**深度不连续处**把前景与背景的 AO 混在一起。表现是物体边缘外面
// 一圈发暗 (背景吃到了前景的遮蔽), 或者边缘里面一圈发亮 —— 那种一圈光晕看
// 起来像"AO 半径调大了", 而调半径根本治不了它。
//
// 双边的做法: 四个半分辨率邻居各按"深度与本像素有多接近"加权。深度差得远的
// 邻居权重趋近于零, 于是不连续处只取同一侧的样本。
//
// ── 权重用视空间深度的相对差 ──
//
// 直接用 NDC 深度差不行: 透视投影下 NDC 深度是 1/z 型的, 同样的世界距离在
// 近处对应巨大的 NDC 差、在远处对应极小的差。用它作权重, 近处会退化成"只取
// 最近的一个邻居"(锯齿), 远处会退化成双线性(渗色)。
//
// 用相对差 |z1-z2| / max(z1,z2) 则与距离无关: 它问的是"这两个样本在不在同
// 一个表面上", 而那正是判据本身。
// ============================================================================

layout(location = 0) in vec2 fragUV;

layout(location = 0) out float outAo;

// 半分辨率的 AO
layout(set = 0, binding = 0) uniform sampler2D halfAo;

// 全分辨率深度
layout(set = 0, binding = 1) uniform sampler2D sceneDepth;

layout(push_constant) uniform Params {
    // x = 半分辨率宽, y = 半分辨率高, z = 近平面, w = 远平面
    vec4 params;
} pc;

/// NDC 深度 → 视空间线性深度 (正值)
///
/// Vulkan 的 NDC z 是 [0,1], 而透视投影下 z_ndc = A + B/z_view。反解得到
/// z_view = B / (z_ndc - A), 其中 A = far/(near-far), B = far*near/(near-far)。
///
/// 直接用 near/far 而不是传逆投影矩阵进来: 上采样只需要"两个样本在不在同一
/// 个表面上", 而那只跟线性深度的比值有关, 与横向位置无关。
float LinearizeDepth(float ndcDepth)
{
    float nearPlane = pc.params.z;
    float farPlane  = pc.params.w;

    float a = farPlane / (nearPlane - farPlane);
    float b = farPlane * nearPlane / (nearPlane - farPlane);

    // z_ndc 等于 a 时分母为零 —— 那对应无穷远。钳一个极小量, 让它给出一个
    // 极大的深度而不是 inf: inf 会让下面的相对差变成 NaN, 而 NaN 参与比较
    // 时永远为假, 于是四个权重全零, 结果退化成除以零。
    float denom = ndcDepth - a;

    return b / ((abs(denom) < 1.0e-7) ? 1.0e-7 : denom);
}

void main()
{
    vec2 halfSize = pc.params.xy;

    // 本像素在半分辨率纹理里的连续坐标, 减半个纹素回到纹素中心的坐标系
    vec2 halfCoord = fragUV * halfSize - 0.5;

    vec2 baseCoord = floor(halfCoord);
    vec2 frac      = halfCoord - baseCoord;

    float centerDepth = LinearizeDepth(texture(sceneDepth, fragUV).r);

    // 四个邻居的双线性权重
    float bilinear[4];
    bilinear[0] = (1.0 - frac.x) * (1.0 - frac.y);
    bilinear[1] = frac.x * (1.0 - frac.y);
    bilinear[2] = (1.0 - frac.x) * frac.y;
    bilinear[3] = frac.x * frac.y;

    ivec2 offsets[4];
    offsets[0] = ivec2(0, 0);
    offsets[1] = ivec2(1, 0);
    offsets[2] = ivec2(0, 1);
    offsets[3] = ivec2(1, 1);

    float sum       = 0.0;
    float weightSum = 0.0;

    for (int i = 0; i < 4; ++i)
    {
        vec2 neighborCoord = baseCoord + vec2(offsets[i]) + 0.5;
        vec2 neighborUV    = neighborCoord / halfSize;

        float ao = texture(halfAo, neighborUV).r;

        // 邻居所在的全分辨率深度。半分辨率的 AO 是在半分辨率的像素中心算
        // 的, 而那个中心对应全分辨率的一个具体位置 —— 取那里的深度。
        float neighborDepth = LinearizeDepth(texture(sceneDepth, neighborUV).r);

        // 相对深度差 —— 与距离无关, 问的是"在不在同一个表面上"
        float relative = abs(centerDepth - neighborDepth) /
                         max(max(centerDepth, neighborDepth), 1.0e-4);

        // 0.05 的相对差 (5%) 之内算同一个表面。
        //
        // 取得太紧 (比如 0.01) 会让斜面上的相邻像素也被判成不同表面, 权重
        // 退化成只剩最近的一个 —— 那就是最近邻上采样, 边缘出现台阶。
        // 取得太松就退化回双线性, 不连续处渗色。
        float depthWeight = exp(-relative / 0.05);

        float weight = bilinear[i] * depthWeight;

        sum       += ao * weight;
        weightSum += weight;
    }

    // 四个邻居全都与本像素不在同一个表面上 (薄物体、一像素宽的缝) 时,
    // weightSum 会趋近于零。那时退回最近邻 —— 取双线性权重最大的那个。
    //
    // 不退回的话结果是 0/0。而 AO 是个乘性因子, NaN 会让那个像素整个变黑,
    // 表现为画面上零星的黑点, 看着像噪声而不像上采样的问题。
    if (weightSum < 1.0e-5)
    {
        int best = 0;
        float bestWeight = bilinear[0];

        for (int i = 1; i < 4; ++i)
        {
            if (bilinear[i] > bestWeight)
            {
                bestWeight = bilinear[i];
                best       = i;
            }
        }

        vec2 neighborUV = (baseCoord + vec2(offsets[best]) + 0.5) / halfSize;

        outAo = texture(halfAo, neighborUV).r;
        return;
    }

    outAo = sum / weightSum;
}
