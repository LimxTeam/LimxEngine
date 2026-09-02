#version 450

// ============================================================================
// meshlet_depth_fallback.frag — 计算展开回退路径的片段着色器
//
// 与 meshlet_depth.frag 的函数体是同一行, 只有输入的声明不同: 那一条是
// 逐图元 (perprimitiveEXT), 这一条是逐顶点 (flat)。
//
// 合不成一个: Vulkan 要求前后阶段的接口匹配, 而 perprimitiveEXT 只能与
// 网格着色器配对。理由写在 meshlet_depth.frag 顶部。
//
// 这一对是判据盯着的地方 —— "两条路径画出的可见性缓冲区逐位相同"。
// ============================================================================

layout(location = 0) flat in uint inVisibility;

layout(location = 0) out uint outVisibility;

void main()
{
    outVisibility = inVisibility;
}
