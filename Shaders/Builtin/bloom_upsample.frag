// ============================================================================
// bloom_upsample.frag — 泛光的升采样链
//
// 3x3 帐篷滤波 (tent filter)。从更小的一级采样, 加回到更大的一级上。
//
// 用帐篷核而不是双线性放大: 双线性只在 2x2 邻域内插值, 放大之后相邻的四个
// 输出像素共用同一组输入 —— 结果是块状的边界。帐篷核跨 3x3, 边界被抹平。
//
// 权重 [1 2 1; 2 4 2; 1 2 4] / 16, 和为 1 —— 与降采样一样, 归一化是能量
// 守恒的前提。
//
// ── 半径的含义 ──
//
// 采样偏移按**目标**纹理的纹素尺寸乘一个半径系数。半径 1 是标准帐篷核;
// 大于 1 会让每一级都糊得更开, 累积起来泛光的拖尾更长。这是一个纯粹的
// 观感参数, 不影响能量守恒 (核仍然归一化)。
// ============================================================================

#version 450

layout(location = 0) in vec2 fragUV;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D sourceTexture;

layout(push_constant) uniform Params {
    // xy = 采样偏移 (目标纹素尺寸 x 半径), z = 半径, w = 保留
    vec4 offset;
} pc;

void main()
{
    vec2 o = pc.offset.xy;

    vec3 result = vec3(0.0);

    result += texture(sourceTexture, fragUV + vec2(-o.x,  o.y)).rgb * 1.0;
    result += texture(sourceTexture, fragUV + vec2( 0.0,  o.y)).rgb * 2.0;
    result += texture(sourceTexture, fragUV + vec2( o.x,  o.y)).rgb * 1.0;

    result += texture(sourceTexture, fragUV + vec2(-o.x,  0.0)).rgb * 2.0;
    result += texture(sourceTexture, fragUV).rgb                    * 4.0;
    result += texture(sourceTexture, fragUV + vec2( o.x,  0.0)).rgb * 2.0;

    result += texture(sourceTexture, fragUV + vec2(-o.x, -o.y)).rgb * 1.0;
    result += texture(sourceTexture, fragUV + vec2( 0.0, -o.y)).rgb * 2.0;
    result += texture(sourceTexture, fragUV + vec2( o.x, -o.y)).rgb * 1.0;

    outColor = vec4(result * (1.0 / 16.0), 1.0);
}
