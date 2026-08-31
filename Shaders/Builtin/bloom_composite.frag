// ============================================================================
// bloom_composite.frag — 原图与泛光相加
//
// 输出仍是线性 HDR —— 色调映射在下一个通道。泛光必须在映射**之前**加,
// 否则加的是已经压缩过的值: 亮部本来就被压平了, 泛光叠上去几乎看不出来,
// 而暗部的泛光又会显得过强。
//
// 强度是线性系数而不是指数或曲线。曲线会让"泛光强度"这个参数与亮度耦合,
// 调起来手感不可预测。
// ============================================================================

#version 450

layout(location = 0) in vec2 fragUV;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D sceneTexture;
layout(set = 0, binding = 1) uniform sampler2D bloomTexture;

layout(push_constant) uniform Params {
    // x = 泛光强度, y/z/w 保留
    vec4 params;
} pc;

void main()
{
    vec3 scene = texture(sceneTexture, fragUV).rgb;
    vec3 bloom = texture(bloomTexture, fragUV).rgb;

    outColor = vec4(scene + bloom * pc.params.x, 1.0);
}
