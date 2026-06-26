#version 450

// ── 顶点属性输入 (从 VBO 读取, 交错布局) ──
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;
layout(location = 3) in vec2 inTexCoord;

// ── Uniform Buffer: 视图+投影矩阵 (全场景共享, 每帧更新一次) ──
layout(set = 0, binding = 0) uniform ViewProjUBO {
    mat4 view;
    mat4 proj;
} ubo;

// ── Push Constant: 逐物体 Model 矩阵 ──
layout(push_constant) uniform PushConstants {
    mat4 model;
} pc;

// ── 顶点着色器输出 ──
layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec2 fragTexCoord;

void main()
{
    gl_Position = ubo.proj * ubo.view * pc.model * vec4(inPosition, 1.0);
    // 法线变换 (使用 model 矩阵的 3x3 子矩阵, 等比缩放场景下等价于法线矩阵)
    fragNormal  = mat3(pc.model) * inNormal;
    fragColor    = inColor;
    fragTexCoord = inTexCoord;
}
