#version 450

// ============================================================
// 天空盒顶点着色器 — 全屏三角形 + 视线方向重建
//
// 不画立方体网格, 而是画一个全屏三角形并为每个像素重建世界空间视线:
//   立方体网格要处理"相机在盒内"、近平面裁掉盒面、盒子随相机平移等一系列
//   琐碎问题, 而这些问题的每一个都只在特定视角下暴露。全屏三角形没有几何,
//   自然也没有这些问题。
//
// 深度写在**远平面** (z = w → 深度 1.0):
//   配合 LessOrEqual 测试与关闭的深度写入, 天空只填充深度仍为 1.0 的像素,
//   也就是没有任何几何体覆盖的地方。若写成 0.0 (近平面), 天空会盖住整个
//   画面; 若开启深度写入, 后续的半透明物体会被天空的深度挡掉。
//
// 视线重建不需要求逆矩阵:
//   投影矩阵的对角元素直接给出 NDC 到视空间的比例, 而视图矩阵的旋转部分
//   是正交阵, 其逆就是转置。两者组合即可, 比在着色器里做 4x4 求逆便宜
//   得多, 也没有数值稳定性问题。
// ============================================================

// 与顶点着色器共享 set 0 —— 天空只需要 view 与 proj, 不额外占描述符集
layout(row_major, set = 0, binding = 0) uniform ViewProjUBO {
    mat4 view;
    mat4 proj;
} ubo;

layout(location = 0) out vec3 fragViewDirection;

void main()
{
    // 与 fullscreen.vert 相同的三角形: 索引 0/1/2 → (-1,-1) (3,-1) (-1,3)
    const vec2 uv  = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    const vec2 ndc = uv * 2.0 - 1.0;

    // NDC → 视空间方向
    //
    // 视空间为右手系, -Z 为前方 (见 FMatrix::Perspective)。
    // proj[0][0] = 1/(aspect*tan(fov/2)), proj[1][1] = -1/tan(fov/2);
    // 后者的负号正是 Vulkan 的 y 轴向下, 除以它即可还原视空间的 y。
    const vec3 viewRay = vec3(ndc.x / ubo.proj[0][0],
                              ndc.y / ubo.proj[1][1],
                              -1.0);

    // 视空间 → 世界空间: 只取旋转部分, 转置即为其逆
    fragViewDirection = transpose(mat3(ubo.view)) * viewRay;

    // z = w = 1 → 深度 1.0 (远平面)
    gl_Position = vec4(ndc, 1.0, 1.0);
}
