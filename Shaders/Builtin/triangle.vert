#version 450

// ── 顶点属性输入 (从 VBO 读取, 交错布局) ──
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;
layout(location = 3) in vec2 inTexCoord;

// ── Uniform Buffer: 视图+投影矩阵 (全场景共享, 每帧更新一次) ──
// ── 矩阵存储序 ────────────────────────────────────────────────
// 引擎的 FMatrix 是行主序 (M[行][列], 平移在最后一列)。GLSL 默认按列主序
// 解读 uniform 中的 mat4, 不加 row_major 就等于把矩阵整体转置 —— 顶点会被
// 变换到裁剪体之外, 表现为"什么都不显示"而没有任何报错。
// FMatrix.h 的注释即以"与着色器 row_major 一致"为前提。
layout(row_major, set = 0, binding = 0) uniform ViewProjUBO {
    mat4 view;
    mat4 proj;
} ubo;

// ── Push Constant: 逐物体 Model 矩阵 ──
layout(row_major, push_constant) uniform PushConstants {
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
