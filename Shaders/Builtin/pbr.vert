#version 450

// ============================================================
// PBR 顶点着色器 — 输出世界空间位置、法线、颜色、UV
// 与现有 FMeshVertex 布局完全兼容 (location 0-3)
//
// 描述符集规划:
//   set 0, binding 0: ViewProjUBO (View + Proj 矩阵)
//   set 1: 材质参数 + 贴图 (Agent A, 暂不使用)
//   set 2, binding 0: FLightingUBO (光照数据)
//
// Push Constant: 逐物体 Model 矩阵 (64 bytes)
// ============================================================

// ── 顶点属性输入 (从 VBO 读取, 交错布局, 44 bytes/vertex) ──
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

// ── 顶点着色器输出 → 片段着色器输入 ──
layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragWorldNormal;
layout(location = 2) out vec3 fragColor;
layout(location = 3) out vec2 fragTexCoord;

void main()
{
    // 世界空间位置
    vec4 worldPos = pc.model * vec4(inPosition, 1.0);
    fragWorldPos  = worldPos.xyz;

    // 裁剪空间位置
    gl_Position = ubo.proj * ubo.view * worldPos;

    // 世界空间法线 (使用 model 矩阵的逆转置 3x3 子矩阵)
    // 对于等比缩放场景，mat3(model) 等价于法线矩阵
    // 非等比缩放时需要 transpose(inverse(mat3(model)))
    mat3 normalMatrix = transpose(inverse(mat3(pc.model)));
    fragWorldNormal   = normalMatrix * inNormal;

    // 直传顶点颜色和 UV
    fragColor    = inColor;
    fragTexCoord = inTexCoord;
}
