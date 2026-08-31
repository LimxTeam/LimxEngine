#version 450

// ============================================================
// 深度预 Pass 顶点着色器
//
// 只声明本管线真正消费的属性: 位置用于变换, UV 传给片段着色器做
// Masked 材质的 alpha 测试。声明了却不消费的属性会换来校验层的
// "not consumed" 警告。
//
// Location 编号与前向管线保持一致 (位置 0, 主 UV 3) —— 同一个属性在
// 不同 Pass 里用不同编号是极易看漏的错误来源。步幅仍是完整的
// FMeshVertex (72 字节), 属性按偏移量取值。
// ============================================================

layout(location = 0) in vec3 inPosition;
layout(location = 3) in vec2 inTexCoord0;

// ── 逐物体数据 (set 3, binding 0) ──
//
// 模型矩阵与材质下标从 push constant 搬到了这里, 与 pbr.vert / gbuffer.vert
// 同一份声明。搬家的硬性理由是间接绘制: 一次 DrawIndexedIndirect 覆盖几百个
// 物体, 而 push constant 在整次调用里是常量。
#include "gpu_draw_common.h"

layout(row_major, std430, set = 3, binding = 0) readonly buffer ObjectBuffer {
    DrawObject objects[];
} objectBuffer;

// ── Push Constant: 逐**视图**的视图投影矩阵 ──
//
// 原来这里是逐物体的 model 矩阵, 而 set 0 的 UBO 提供 view/proj。现在反过来:
// model 逐物体所以进了 storage buffer, 而视图矩阵逐**通道**(阴影的每一级级联、
// 阴影图集的每一块) —— 那正是 push constant 最合适的粒度, 一次绘制里它是常量。
//
// 这样安排还消掉了两个东西:
//   阴影通道原本每帧每级一份 UBO 加一个描述符集 (3 帧 × 3 级 = 9 套);
//   阴影图集通道原本把块矩阵**预乘进 model** 再传 push constant —— 那个技巧
//   在 model 搬进 storage buffer 之后就没法用了。
//
// row_major 与 C++ 侧 FMatrix 的行主序一致。
layout(row_major, push_constant) uniform ViewPushConstants {
    mat4 viewProj;
} pc;

layout(location = 0) out vec2 fragTexCoord;

// bindless 材质下标 —— flat, 逐物体恒定
//
// 片段着色器拿不到 gl_InstanceIndex (那是顶点阶段的内建量), 所以由这里传。
layout(location = 1) flat out uint fragMaterialIndex;

void main()
{
    DrawObject obj = objectBuffer.objects[gl_InstanceIndex];

    fragMaterialIndex = obj.drawParams.w;

    gl_Position  = pc.viewProj * obj.model * vec4(inPosition, 1.0);
    fragTexCoord = inTexCoord0;
}
