// ============================================================================
// view_common.h — 相机矩阵 UBO 的唯一声明 (set 0, binding 0)
//
// 五个顶点着色器都读这个块。此前每个文件各自声明一遍, 而 C++ 侧的
// FViewProjUBO 一旦加字段, 五处都要同步 —— **漏改一个的表现是那条通道里
// 矩阵整体错位、画面全黑, 而且没有任何报错**。std140 布局不会因为着色器
// 少声明一个字段就报错, 它只是按自己声明的偏移去读。
//
// 收成一份之后, 漂移在结构上不可能发生。
//
// 与 C++ 侧 Source/Runtime/Renderer/Public/Renderer/Renderer/FRenderer.h 的
// FViewProjUBO 逐字段对应, 那边有 static_assert 钉住总大小。
// ============================================================================

#ifndef LIMX_VIEW_COMMON_H
#define LIMX_VIEW_COMMON_H

layout(row_major, set = 0, binding = 0) uniform ViewProjUBO {
    mat4 view;
    mat4 proj;

    /// proj * view, CPU 侧算好的合成矩阵
    ///
    /// 存在的理由有两条, 都不是"省一次乘法":
    ///
    /// 1. **前向 Pass 用 Equal 做深度测试**, 也就是说 pbr.vert 算出的
    ///    gl_Position.z 必须与深度预通道 (gbuffer.vert) 算出的**逐位相同**,
    ///    差一个 ulp 就整片像素被剔除、物体消失。两个着色器各写一遍
    ///    `proj * view * world` 时, 编译器对两处的重结合不保证一致。用同一
    ///    个预乘矩阵、同一条 `viewProj * world`, 这个隐患从根上不存在。
    ///
    /// 2. 速度矢量要拿本帧与上一帧的裁剪空间坐标相减。上一帧那侧只能是
    ///    单个矩阵 (prevViewProj), 本帧那侧若走 proj*(view*world) 的两步
    ///    路径, 相机静止时两者也差一个 ulp —— 速度就不是精确的零, 而是
    ///    一片 1e-7 量级的噪声。
    mat4 viewProj;

    /// 本帧的 proj * view, **不含抖动** —— 只给速度矢量用
    ///
    /// 速度必须是无抖动的。抖动是每帧变化的亚像素偏移, 把它算进速度就等于
    /// 告诉 TAA "画面每帧都在抖" —— 而那正是 TAA 要消掉的东西。结果是静止
    /// 画面上持续的重投影噪声, 看起来像 TAA 的钳制参数没调好。
    ///
    /// 于是有了这条不变量: **相机与物体都静止时, 速度缓冲精确为零,
    /// 无论抖动是否开启**。--gbuffer-check 直接断言它。
    mat4 viewProjNoJitter;

    /// 上一帧的 proj * view, 同样不含抖动
    mat4 prevViewProjNoJitter;
} ubo;

#endif // LIMX_VIEW_COMMON_H
