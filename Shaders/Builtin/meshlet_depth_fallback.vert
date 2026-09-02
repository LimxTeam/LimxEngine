#version 450

// ============================================================================
// meshlet_depth_fallback.vert — 回退路径的顶点着色器
//
// 没有顶点输入布局: 顶点数据由 meshlet_expand.comp 展开成一条 storage
// buffer 里的流, 这里按 gl_VertexIndex 去读。
//
// 这样安排是因为经典管线的顶点输入只能绑一份缓冲区加一个步长, 而 meshlet
// 的顶点散落在全场景的顶点缓冲区里、还各自属于不同的实例。真要用顶点输入
// 的话就得每个 meshlet 一次绘制调用 —— 那正是虚拟几何要消掉的东西。
//
// 顶点变换与 meshlet_depth.mesh **逐字相同** (同一个共享头, 同一个运算
// 顺序)。两条路径要画出逐位相同的深度, 这一条是前提。
// ============================================================================

#extension GL_EXT_scalar_block_layout : require

#include "meshlet_raster_common.h"

layout(std430, set = 0, binding = 0) readonly buffer InstanceBuffer {
    MeshletInstance instances[];
} instanceBuffer;

layout(scalar, set = 0, binding = 1) readonly buffer VertexBuffer {
    MeshletVertex vertices[];
} vertexBuffer;

layout(std430, set = 0, binding = 2) readonly buffer ExpandedBuffer {
    uvec2 vertices[];
} expandedBuffer;

// 可见表 —— 由编号里的槽位查实例
layout(std430, set = 0, binding = 3) readonly buffer VisibleMeshletBuffer {
    uvec2 visible[];
} visibleBuffer;

layout(location = 0) flat out uint outVisibility;

layout(row_major, push_constant) uniform Params {
    mat4 viewProj;

    uvec4 params;
} pc;

void main()
{
    const uvec2 entry = expandedBuffer.vertices[gl_VertexIndex];

    outVisibility = entry.y;

    const uint slot = MeshletDecodeSlot(entry.y);

    const uint instanceIndex = visibleBuffer.visible[slot].x;

    const MeshletInstance instance =
        instanceBuffer.instances[instanceIndex];

    const mat4 modelViewProj = pc.viewProj * MeshletInstanceMatrix(instance);

    // entry.x 已经是**场景级**的顶点下标 —— 展开时折算过了。
    // 这里再加一次基址的话就加了两遍。
    gl_Position =
        modelViewProj * vec4(vertexBuffer.vertices[entry.x].position, 1.0);
}
