#version 450

// ── 片段着色器输入 (来自顶点着色器) ──
layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragTexCoord;

// ── 纹理采样器 (set 0, binding 1) ──
layout(set = 0, binding = 1) uniform sampler2D texSampler;

// ── 片段着色器输出 ──
layout(location = 0) out vec4 outColor;

void main()
{
    // 纹理采样
    vec4 texColor = texture(texSampler, fragTexCoord);

    // 顶点颜色 × 纹理颜色
    vec3 baseColor = fragColor * texColor.rgb;

    // 简单半 Lambert 光照 — 方向光从 (1,1,-1) 方向照射
    vec3 lightDir = normalize(vec3(1.0, 1.0, -1.0));
    vec3 normal   = normalize(fragNormal);
    float ndotl   = dot(normal, lightDir) * 0.5 + 0.5;
    vec3  lit     = baseColor * ndotl;
    outColor      = vec4(lit, 1.0);
}
