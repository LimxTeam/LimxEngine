#version 450

// ============================================================
// UI 顶点着色器 — 屏幕像素坐标 → NDC
//
// 顶点输入 (FUIVertex, 40 bytes):
//   location 0: vec2 inPos      — 屏幕像素位置
//   location 1: vec2 inUV       — UV 坐标
//   location 2: vec4 inColor    — RGBA 颜色
//
// UBO set=0, binding=0: 正交矩阵 (OrthoMatrix, 64 bytes)
// ============================================================

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;

layout(set = 0, binding = 0) uniform OrthoUBO {
    mat4 orthoMatrix;
} ubo;

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec4 fragColor;

void main()
{
    gl_Position = ubo.orthoMatrix * vec4(inPos, 0.0, 1.0);
    fragUV      = inUV;
    fragColor   = inColor;
}
