#version 450

// ============================================================
// 色调映射 — 曝光 → ACES → sRGB
//
// 从 pbr.frag 中独立出来的理由不是复用, 而是**正确性与可扩展性**:
//   1. 写在 PBR 里意味着每个片段各自映射一次, 而色调映射按定义是对
//      最终图像的操作。有了 Bloom、TAA 之后, 它们需要的是**线性 HDR**
//      输入 —— 若颜色在进入它们之前已被映射, 亮部信息已经丢失, 再怎么
//      处理也找不回来。
//   2. 曝光是全局参数, 逐材质着色器无处安放。
//
// ACES 用的是 Stephen Hill 拟合的 RRT+ODT 近似 (fit)。相比 Reinhard,
// 它在高光处保留更多色相 —— Reinhard 把所有通道独立压缩, 强光下三个
// 通道先后饱和, 结果是高光一律偏白; ACES 的矩阵变换让高光沿着更接近
// 胶片的路径滚降。
// ============================================================

layout(location = 0) in vec2 fragUV;

layout(set = 0, binding = 0) uniform sampler2D hdrColor;

layout(push_constant) uniform TonemapParams {
    float exposure;      // 线性曝光倍数
    float _pad0;
    float _pad1;
    float _pad2;
} params;

layout(location = 0) out vec4 outColor;

// ── ACES 输入/输出矩阵 (sRGB 原色 ↔ ACES 色域) ──
const mat3 kACESInputMatrix = mat3(
    0.59719, 0.07600, 0.02840,
    0.35458, 0.90834, 0.13383,
    0.04823, 0.01566, 0.83777
);

const mat3 kACESOutputMatrix = mat3(
     1.60475, -0.10208, -0.00327,
    -0.53108,  1.10813, -0.07276,
    -0.07367, -0.00605,  1.07602
);

// RRT 与 ODT 合并后的有理式拟合
vec3 RRTAndODTFit(vec3 v)
{
    vec3 a = v * (v + 0.0245786) - 0.000090537;
    vec3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return a / b;
}

vec3 ACESFitted(vec3 color)
{
    color = kACESInputMatrix * color;
    color = RRTAndODTFit(color);
    color = kACESOutputMatrix * color;
    return clamp(color, 0.0, 1.0);
}

// 线性 → sRGB 传递函数
//
// 用分段精确式而非 pow(x, 1/2.2): 后者在暗部与真实 sRGB 曲线偏差可达
// 数个色阶, 在渐变与阴影过渡处会显出色带。
vec3 LinearToSRGB(vec3 linearColor)
{
    vec3 lo = linearColor * 12.92;
    vec3 hi = 1.055 * pow(linearColor, vec3(1.0 / 2.4)) - 0.055;
    return mix(hi, lo, step(linearColor, vec3(0.0031308)));
}

void main()
{
    vec3 hdr = texture(hdrColor, fragUV).rgb;

    hdr *= max(params.exposure, 0.0);

    vec3 mapped = ACESFitted(hdr);

    outColor = vec4(LinearToSRGB(mapped), 1.0);
}
