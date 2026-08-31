// ============================================================
// gbuffer.frag — 薄 G-Buffer 的片段着色器
//
// 输出两张附件供后续通道采样:
//   [0] 八面体编码的世界法线 (RG16_SFLOAT) — GTAO 要用
//   [1] 屏幕空间速度        (RG16_SFLOAT) — TAA 与运动模糊要用
//
// 深度由固定功能写入, 不在这里输出。
//
// Masked 材质的 alpha 测试与阴影通道共用同一份判据 (见 material_common.h
// 的 ShouldDiscardMasked) —— 两者不一致会让深度图与阴影图在镂空边缘失配。
// ============================================================

#version 450

// 非一致索引 —— 当前材质下标来自 push constant, 一次绘制内是一致的; 等
// GPU 驱动渲染落地之后下标会来自实例数据, 那时它是必需的。现在就写上,
// 免得那时改着色器又要重新验一遍画面。
#extension GL_EXT_nonuniform_qualifier : require

#include "material_common.h"
#include "gbuffer_common.h"

layout(row_major, push_constant) uniform PushConstants {
    mat4 model;
    uint materialIndex;
} pc;

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec3 fragWorldNormal;
layout(location = 2) in vec4 fragCurrentClip;
layout(location = 3) in vec4 fragPrevClip;

// 顺序必须与渲染通道的附件顺序一致: [0]=法线 [1]=速度
layout(location = 0) out vec2 outNormal;
layout(location = 1) out vec2 outVelocity;

void main()
{
    MaterialData mat = materials[pc.materialIndex];

    // alpha 测试放在写输出之前。
    //
    // discard 会阻止整个片段的全部写入 (包括深度与这两张附件), 所以先判
    // 再写是对的; 反过来先写再 discard 也能工作, 但会让人以为那些写入
    // "已经生效了"。
    if (ShouldDiscardMasked(mat, fragTexCoord))
    {
        discard;
    }

    // 法线必须归一化后再编码 —— 插值会让长度偏离 1。
    //
    // 零长度法线要兜底: FMeshVertex::Normal 在源资产缺失时可能是零向量
    // (见 FAssetTypes.h 的说明), 而 normalize(0) 是 NaN。NaN 编码进
    // RG16_SFLOAT 之后, GTAO 读到会算出全黑的 AO, 而那看起来像是"这个
    // 物体的 AO 参数不对"。
    vec3 n = fragWorldNormal;

    float lenSq = dot(n, n);

    // 退化时给 +Z。它是个合法方向, 至少不会污染后续计算。
    n = (lenSq > 1e-12) ? (n * inversesqrt(lenSq)) : vec3(0.0, 0.0, 1.0);

    outNormal = EncodeOctahedralNormal(n);

    // NDC 差值, 不转成 UV 偏移。
    //
    // 两种约定都有人用, 差别是 Y 轴朝向和一个 0.5 的缩放。定成 NDC 是因为
    // 它与 gl_Position 同一个空间, 消费方 (TAA 的重投影) 自己乘 0.5 并翻 Y
    // 即可 —— 反过来在这里就转成 UV, 会让"这张图是什么空间"取决于读注释。
    outVelocity = ComputeVelocity(fragCurrentClip, fragPrevClip);
}
