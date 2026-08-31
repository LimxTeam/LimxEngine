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

#include "gpu_draw_common.h"

// ── 逐物体数据 (set 3, binding 0) ──
//
// 模型矩阵与材质下标从 push constant 搬到了这里。搬的理由不是"更现代",
// 是**间接绘制根本没有逐 draw 推送 push constant 这回事** —— 一次
// DrawIndexedIndirect 覆盖几百个物体, 而 push constant 在整次调用里是常量。
//
// 定位靠 gl_InstanceIndex: 每条间接命令的 firstInstance 写的是物体下标,
// 而 gl_InstanceIndex 是 firstInstance + 实例号 (这里实例数恒为 1)。
// 逐物体绘制那条路径同样把物体下标传给 firstInstance, 所以**两条路径读的
// 是同一处数据、走的是同一份代码** —— 逐像素比对时比出来的差异只可能来自
// 剔除与命令下发, 不会混进"两份着色器本来就不一样"这个因素。
//
// row_major 与 C++ 侧 FMatrix 一致。漏掉的话矩阵整体转置, 物体会出现在
// 完全无关的位置。
layout(row_major, std430, set = 3, binding = 0) readonly buffer ObjectBuffer {
    DrawObject objects[];
} objectBuffer;

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

// bindless 材质下标 —— flat, 逐物体恒定
layout(location = 4) flat out uint fragMaterialIndex;

void main()
{
    DrawObject obj = objectBuffer.objects[gl_InstanceIndex];

    fragMaterialIndex = obj.drawParams.w;

    // 与 pbr.vert 逐字相同 —— 前向通道的深度测试是 Equal, 两处算出的 z
    // 差一个 ulp 就会让整个物体被剔除。同一个 obj.model, 同一个 ubo.viewProj。
    vec4 worldPos = obj.model * vec4(inPosition, 1.0);

    gl_Position  = ubo.viewProj * worldPos;
    fragTexCoord = inTexCoord0;

    // 速度这一对必须**都**用无抖动矩阵, 而不是 gl_Position。
    // 用 gl_Position 的话本帧含抖动、上一帧不含, 差值里就多出一个
    // 每帧变化的亚像素偏移 —— 那是假运动。
    fragCurrentClip = ubo.viewProjNoJitter * worldPos;

    // 上一帧位置用的是**同一个** obj.model —— 前提是物体在帧间不动。
    //
    // 这个前提目前成立 (引擎里没有任何地方逐帧改变换), 但它是前提而不是
    // 定理。逐物体变换已经搬进 storage buffer 了 (原先在 push constant 里,
    // 那时再加一个 mat4 就是 132 字节, 超过 Vulkan 保证的 128 下限), 所以
    // 等有了骨骼动画或移动物体, 给 DrawObject 加一个 prevModel 就够 ——
    // 不必再去压缩 model 矩阵的表示。
    fragPrevClip = ubo.prevViewProjNoJitter * worldPos;

    // 法线矩阵与 pbr.vert 逐字一致。
    //
    // 不能图省事写成 mat3(model): 非等比缩放的物体在两条路径上会算出不同
    // 的法线, 而 GTAO 用 G-Buffer 法线、前向光照用自己算的法线 —— 同一个
    // 表面两套法线, 表现为 AO 与光照的边界错位。那个错位很小, 容易被当成
    // "GTAO 参数没调好"。
    //
    // 代价是每个顶点一次 3x3 求逆。若实测显著吃掉 Early-Z 的收益, 正确的
    // 做法是把法线矩阵预算好放进 DrawObject, 而不是换一个不等价的公式。
    mat3 normalMatrix = transpose(inverse(mat3(obj.model)));
    fragWorldNormal   = normalMatrix * inNormal;
}
