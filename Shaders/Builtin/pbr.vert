#version 450

// ============================================================
// PBR 顶点着色器 — 输出世界空间位置、法线、切线基、颜色、UV
// 顶点布局与 FMeshVertex 一致 (72 bytes/vertex, location 0-5)
//
// 描述符集规划:
//   set 0, binding 0: ViewProjUBO (View + Proj 矩阵)
//   set 1: 材质参数 + 贴图
//   set 2, binding 0: FLightingUBO (光照数据)
//
// Push Constant: 逐物体 Model 矩阵 (64 bytes)
// ============================================================

// ── 顶点属性输入 (VBO 交错布局 72 bytes/vertex, 此处只取用到的属性) ──
//
// location 4 (TexCoord1) 有意缺席 —— 光照贴图尚未实现, 声明了不用会换来
// 校验层警告。顶点步幅由管线 binding 决定, 与声明数量无关。
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;    // xyz = 切线, w = 手性 (±1)
layout(location = 3) in vec2 inTexCoord0;
layout(location = 5) in vec4 inColor;

// ── Uniform Buffer: 视图+投影矩阵 (全场景共享, 每帧更新一次) ──
// ── 矩阵存储序 ────────────────────────────────────────────────
// 引擎的 FMatrix 是行主序 (M[行][列], 平移在最后一列)。GLSL 默认按列主序
// 解读 uniform 中的 mat4, 不加 row_major 就等于把矩阵整体转置 —— 顶点会被
// 变换到裁剪体之外, 表现为"什么都不显示"而没有任何报错。
// FMatrix.h 的注释即以"与着色器 row_major 一致"为前提。
#include "view_common.h"

// ── Push Constant: 逐物体 Model 矩阵 ──
// push constant 里多了材质下标。
//
// 顶点着色器用不到它, 但 push constant 的布局在整条管线上是共享的 ——
// 片段着色器要读, 这里就必须声明出同样的布局, 否则偏移对不上。
layout(row_major, push_constant) uniform PushConstants {
    mat4 model;
    uint materialIndex;
} pc;

// ── 顶点着色器输出 → 片段着色器输入 ──
layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragWorldNormal;
layout(location = 2) out vec3 fragColor;
layout(location = 3) out vec2 fragTexCoord;
layout(location = 4) out vec4 fragWorldTangent;

void main()
{
    // 世界空间位置
    vec4 worldPos = pc.model * vec4(inPosition, 1.0);
    fragWorldPos  = worldPos.xyz;

    // 裁剪空间位置
    // 必须与 gbuffer.vert 写法逐字相同 —— 前向 Pass 的深度测试是 Equal,
    // 两处算出的 z 差一个 ulp 就会让整个物体被剔除。
    gl_Position = ubo.viewProj * worldPos;

    // 世界空间法线 (使用 model 矩阵的逆转置 3x3 子矩阵)
    // 对于等比缩放场景，mat3(model) 等价于法线矩阵
    // 非等比缩放时需要 transpose(inverse(mat3(model)))
    mat3 normalMatrix = transpose(inverse(mat3(pc.model)));
    fragWorldNormal   = normalMatrix * inNormal;

    // 切线按 model 变换而非法线矩阵 —— 切线是切空间向量, 随表面一同拉伸,
    // 用逆转置会在非等比缩放下把它推离表面。手性 w 原样传递。
    fragWorldTangent = vec4(mat3(pc.model) * inTangent.xyz, inTangent.w);

    // 直传顶点颜色和 UV
    fragColor    = inColor.rgb;
    fragTexCoord = inTexCoord0;
}
