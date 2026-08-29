#version 450

// ============================================================
// 深度预 Pass 顶点着色器
//
// 只声明位置属性。顶点步幅由管线的 binding 决定 (完整 FMeshVertex, 72 字节),
// 与着色器声明多少个属性无关 —— 属性按各自偏移量取值。多声明未使用的属性
// 只会换来 "not consumed by vertex shader" 校验警告。
// ============================================================

layout(location = 0) in vec3 inPosition;

// ── Uniform Buffer: 视图+投影矩阵 (set 0 binding 0) ──
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

// 深度预 Pass 无颜色输出 — 仅通过 gl_Position 写入深度缓冲区
void main()
{
    gl_Position = ubo.proj * ubo.view * pc.model * vec4(inPosition, 1.0);
}
