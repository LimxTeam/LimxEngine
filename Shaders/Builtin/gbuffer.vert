// ============================================================
// gbuffer.vert — 薄 G-Buffer 的顶点着色器
//
// 与 depth_only.vert 分开而不是共用一份, 是因为两者的顶点输入不同:
// 本着色器要读法线, 而阴影通道不需要。共用的话阴影管线也得声明法线属性
// —— 那是三级级联乘全部投射体的取数带宽, 白白多取 12 字节每顶点。
//
// (共用还有一个硬性障碍: Vulkan 要求顶点着色器声明的每个 Location 都必须
//  出现在管线的顶点属性描述里, 否则 vkCreateGraphicsPipelines 直接失败。)
//
// 输出: 世界法线 (供片段着色器编码) + UV (供 Masked 的 alpha 测试)
// ============================================================

#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 3) in vec2 inTexCoord0;

// location 编号与 pbr.vert 一致 —— 两条管线读的是同一份顶点数据布局
#include "view_common.h"

// 与 pbr.vert / depth_only.vert 逐字段一致 —— push constant 的布局在整条
// 管线上共享, 而这几个通道用的是同一个 m_PipelineLayout。
layout(row_major, push_constant) uniform PushConstants {
    mat4 model;
    uint materialIndex;
} pc;

layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) out vec3 fragWorldNormal;

// 裁剪空间位置传两份, 透视除法留到片段着色器里做。
//
// 不能在这里就除掉 w 再传 NDC: 插值器对 varying 做的是透视校正插值, 而
// NDC 已经是除过的量, 对它再插值得到的不是"该像素的 NDC"。表现是三角形
// 内部的速度矢量偏斜, 靠近顶点处正确、中心处最错 —— 很容易被当成 TAA
// 的"重投影精度问题"。
layout(location = 2) out vec4 fragCurrentClip;
layout(location = 3) out vec4 fragPrevClip;

void main()
{
    vec4 worldPos = pc.model * vec4(inPosition, 1.0);

    gl_Position  = ubo.viewProj * worldPos;
    fragTexCoord = inTexCoord0;

    // 速度这一对必须**都**用无抖动矩阵, 而不是 gl_Position。
    // 用 gl_Position 的话本帧含抖动、上一帧不含, 差值里就多出一个
    // 每帧变化的亚像素偏移 —— 那是假运动。
    fragCurrentClip = ubo.viewProjNoJitter * worldPos;

    // 上一帧位置用的是**同一个** pc.model —— 前提是物体在帧间不动。
    //
    // 这个前提目前成立 (引擎里没有任何地方逐帧改变换), 但它是前提而不是
    // 定理。等有了骨骼动画或移动物体, 这里必须换成上一帧的 model 矩阵,
    // 而那放不进 push constant: 现在已用 68 字节, 再加一个 mat4 就是 132,
    // 超过 Vulkan 保证的 128 字节下限。届时的正确做法是把逐物体变换搬进
    // storage buffer, 而不是把 model 矩阵压缩成不等价的形式。
    fragPrevClip = ubo.prevViewProjNoJitter * worldPos;

    // 法线矩阵与 pbr.vert 逐字一致。
    //
    // 不能图省事写成 mat3(model): 非等比缩放的物体在两条路径上会算出不同
    // 的法线, 而 GTAO 用 G-Buffer 法线、前向光照用自己算的法线 —— 同一个
    // 表面两套法线, 表现为 AO 与光照的边界错位。那个错位很小, 容易被当成
    // "GTAO 参数没调好"。
    //
    // 代价是每个顶点一次 3x3 求逆。若实测显著吃掉 Early-Z 的收益, 正确的
    // 做法是把法线矩阵预算好放进 push constant (还剩 60 字节), 而不是换一
    // 个不等价的公式。
    mat3 normalMatrix = transpose(inverse(mat3(pc.model)));
    fragWorldNormal   = normalMatrix * inNormal;
}
