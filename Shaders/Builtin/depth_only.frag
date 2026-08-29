#version 450

// ============================================================
// 深度预 Pass 片段着色器 — 只做 alpha 测试, 不输出颜色
//
// 存在的理由: Masked 材质必须在这里做与 pbr.frag **完全相同**的裁剪。
// 不做的话, 本 Pass 会为完全透明的纹素写入深度, 把它背后的东西挡掉;
// 而前向 Pass 又会把这些纹素 discard, 结果是植被叶片之间出现挖空的黑洞。
//
// 两个 Pass 的裁剪结论必须逐纹素一致 —— 前向 Pass 用的是
// DepthCompareOp=Equal, 任何不一致都会在边缘处直接失配。
//
// 阈值与判据来自同一个 set 1 材质 UBO, 因此不存在"两处常量漂移"的可能。
// ============================================================

layout(location = 0) in vec2 fragTexCoord;

// ── 材质参数 (set 1, binding 0) — 必须匹配 FMaterialParams std140 布局 ──
layout(set = 1, binding = 0) uniform MaterialUBO {
    vec4  BaseColor;
    float Metallic;
    float Roughness;
    float AO;
    float NormalScale;
    vec4  EmissiveColor;
    float AlphaCutoff;
    uint  TextureFlags;
    uint  BlendMode;
    float _Padding;
} material;

layout(set = 1, binding = 1) uniform sampler2D materialAlbedoMap;

const uint TEX_ALBEDO   = 1u << 0;
const uint BLEND_MASKED = 1u;

void main()
{
    // 只有 Masked 需要取样 —— 不透明材质在这里读贴图纯属浪费带宽,
    // 而深度预 Pass 覆盖的正是全场景的每一个像素。
    if (material.BlendMode != BLEND_MASKED)
    {
        return;
    }

    float alpha = material.BaseColor.a;

    if ((material.TextureFlags & TEX_ALBEDO) != 0u)
    {
        alpha *= texture(materialAlbedoMap, fragTexCoord).a;
    }

    if (alpha < material.AlphaCutoff)
    {
        discard;
    }
}
