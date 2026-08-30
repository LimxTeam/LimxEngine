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

// ── Uniform Buffer: 视图+投影矩阵 (set 0 binding 0) ──
layout(row_major, set = 0, binding = 0) uniform ViewProjUBO {
    mat4 view;
    mat4 proj;
} ubo;

// ── Push Constant: 逐物体 Model 矩阵 ──
// 与 pbr.vert 逐字段一致 —— push constant 的布局在整条管线上共享,
// 而深度预通道、阴影通道、前向通道用的是同一个 m_PipelineLayout。
layout(row_major, push_constant) uniform PushConstants {
    mat4 model;
    uint materialIndex;
} pc;

layout(location = 0) out vec2 fragTexCoord;

void main()
{
    gl_Position  = ubo.proj * ubo.view * pc.model * vec4(inPosition, 1.0);
    fragTexCoord = inTexCoord0;
}
