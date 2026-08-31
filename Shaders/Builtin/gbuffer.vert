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
layout(row_major, set = 0, binding = 0) uniform ViewProjUBO {
    mat4 view;
    mat4 proj;
} ubo;

// 与 pbr.vert / depth_only.vert 逐字段一致 —— push constant 的布局在整条
// 管线上共享, 而这几个通道用的是同一个 m_PipelineLayout。
layout(row_major, push_constant) uniform PushConstants {
    mat4 model;
    uint materialIndex;
} pc;

layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) out vec3 fragWorldNormal;

void main()
{
    gl_Position  = ubo.proj * ubo.view * pc.model * vec4(inPosition, 1.0);
    fragTexCoord = inTexCoord0;

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
