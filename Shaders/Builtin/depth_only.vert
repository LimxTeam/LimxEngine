#version 450

// ── 顶点属性输入 (与 FMeshVertex 完全兼容, location 0-3)
// 仅使用 inPosition, 其余属性参与顶点绑定以匹配步幅
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;    // 未使用，但必须声明以匹配 VBO 步幅
layout(location = 2) in vec3 inColor;    // 未使用
layout(location = 3) in vec2 inTexCoord; // 未使用

// ── Uniform Buffer: 视图+投影矩阵 (与 triangle.vert 完全一致, set 0 binding 0) ──
layout(set = 0, binding = 0) uniform ViewProjUBO {
    mat4 view;
    mat4 proj;
} ubo;

// ── Push Constant: 逐物体 Model 矩阵 (与 triangle.vert 完全一致) ──
layout(push_constant) uniform PushConstants {
    mat4 model;
} pc;

// 深度预 Pass 无颜色输出 — 仅通过 gl_Position 写入深度缓冲区
void main()
{
    gl_Position = ubo.proj * ubo.view * pc.model * vec4(inPosition, 1.0);
}
