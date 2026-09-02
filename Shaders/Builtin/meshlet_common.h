// ============================================================
// 文件名称：meshlet_common.h
// 创建时间：2026-09-02
// 创建者  ：LimxTeam
// 设计哲学：剔除的判据、CPU 的参考实现、着色器里跑的那一份，必须是
//          **同一个公式的同一次书写**。三处各写一遍的话，判据比的是
//          "两个错误一不一样"，而不是"结果对不对"。
//          C++ 侧的参考实现 (--meshlet-cull-check) 是逐字照抄这里的，
//          抄的地方留了注释指回来。
// 功能描述：meshlet 的 GPU 表示，与视锥/背面两条剔除判据。
// ============================================================

#ifndef LIMX_MESHLET_COMMON_H
#define LIMX_MESHLET_COMMON_H

// ============================================================================
// 与 C++ 的 FMeshlet 逐字段一致 (48 字节)
// ============================================================================

struct Meshlet
{
    uint vertexOffset;
    uint vertexCount;
    uint triangleOffset;
    uint triangleCount;

    // xyz = 局部空间球心, w = 半径
    vec4 boundingSphere;

    // xyz = 单位轴, w = 半角余弦; w <= -2 表示无效锥
    vec4 normalCone;
};

// ============================================================================
// 一个实例
//
// 变换存成 3x4 行主序 —— 平移在第四列。用 mat4 的话每个实例多 16 字节,
// 而最后一行恒为 (0,0,0,1)。
// ============================================================================

struct MeshletInstance
{
    vec4 transformRow0;
    vec4 transformRow1;
    vec4 transformRow2;

    // x = meshlet 起点, y = meshlet 个数, z = 源对象下标, w = 保留
    uvec4 meshletRange;
};

/// 法线锥无效的标记 —— 与 C++ 的 kInvalidConeCosine 同值
const float kInvalidConeCosine = -2.0;

// ============================================================================
// 把局部空间的包围球变到世界空间
//
// 半径的缩放取三列长度的最大值。
//
// 这对 TRS 变换 (平移·旋转·缩放) 是**精确**的: 那时矩阵是 R·diag(s),
// 三列分别是 s_i·r_i 而 r_i 正交, 于是奇异值恰好是 |s_i|, 最大列长就是
// 谱范数。含切变的一般矩阵上它是**低估**, 会把球缩小 —— 那时必须换成
// Frobenius 范数 (一定不小于谱范数)。这个引擎的变换全是 TRS。
// ============================================================================

vec4 MeshletWorldSphere(vec4 localSphere, vec4 row0, vec4 row1, vec4 row2)
{
    const vec3 center = vec3(
        dot(row0, vec4(localSphere.xyz, 1.0)),
        dot(row1, vec4(localSphere.xyz, 1.0)),
        dot(row2, vec4(localSphere.xyz, 1.0)));

    const float scaleX = length(vec3(row0.x, row1.x, row2.x));
    const float scaleY = length(vec3(row0.y, row1.y, row2.y));
    const float scaleZ = length(vec3(row0.z, row1.z, row2.z));

    const float scale = max(scaleX, max(scaleY, scaleZ));

    return vec4(center, localSphere.w * scale);
}

/// 三个轴的缩放是不是一致 —— 不一致时法线锥不能直接旋转过去
bool MeshletUniformScale(vec4 row0, vec4 row1, vec4 row2)
{
    const float scaleX = length(vec3(row0.x, row1.x, row2.x));
    const float scaleY = length(vec3(row0.y, row1.y, row2.y));
    const float scaleZ = length(vec3(row0.z, row1.z, row2.z));

    const float maximum = max(scaleX, max(scaleY, scaleZ));
    const float minimum = min(scaleX, min(scaleY, scaleZ));

    // 千分之一的相对差之内算一致。取绝对差的话, 一个尺度 0.001 的物体
    // 会永远被判成一致, 一个尺度 1000 的会永远被判成不一致。
    return (maximum - minimum) <= maximum * 1.0e-3 + 1.0e-9;
}

// ============================================================================
// 视锥剔除 —— 球心到某个平面的有符号距离小于 -半径 时整球在背面
//
// 平面约定与 C++ 的 FPlane 一致: dot(n, p) + d < 0 是背面。
// 与 draw_cull.comp 里那一段是同一条判据 —— 两级剔除必须用同一个,
// 不然实例级留下的东西 meshlet 级可能全剔掉, 反过来也一样。
// ============================================================================

bool MeshletSphereVisible(vec4 worldSphere, vec4 planes[6])
{
    for (int p = 0; p < 6; ++p)
    {
        if (dot(planes[p].xyz, worldSphere.xyz) + planes[p].w
            < -worldSphere.w)
        {
            return false;
        }
    }

    return true;
}

// ============================================================================
// 背面剔除 —— 法线锥
//
// 判据: 从相机看过去, 这个 meshlet 的每一个三角形都背对着相机。
//
//   dot(d, axis) >= cutoff * length(d) + radius
//
// 其中 d 是球心减相机位置。左边是"球心方向与锥轴有多合", 右边把锥的
// 张角 (cutoff) 与球的大小 (radius) 都算进去 —— 球越大, 越可能有某个
// 位置能看到正面, 所以门槛越高。
//
// 两种情况必须**不剔**:
//   锥无效 (w <= -2): 法线散得超过半球, 任何方向都可能看到正面。
//   缩放不一致: 法线不能靠旋转变过去 (要用逆转置), 而那时锥的张角也变了。
//
// 这两条都是"存疑就不剔"。剔错的后果是画面上少一块, 而那与"这一块本来
// 就该被剔"长得完全一样 —— 没有症状的错误是最难查的那种。
//
// ── 曾经有第三条, 删掉了 ──
//
// 第一版还有一条 "相机在包围球内就不剔"。它是**可证明的死代码**:
//
//   dot(d, axis) <= |d| = distance                        (柯西-施瓦茨)
//   相机在球内时 distance <= radius
//   有效锥的 cutoff 恒为正 (见 FMeshlet::NormalCone 的哨兵约定),
//     所以 cutoff * distance + radius >= radius >= distance >= dot(d, axis)
//
// 于是那个 >= 永远不成立, 那一条 early-out 改不了任何结果。
//
// 是变异验证把它逼出来的: 把它删掉, 判据一动不动地绿。**一条不会红的
// 分支就是没有判据的分支** —— 留着它只会让下一个人以为它在起作用。
// 真要防的是"哪天让 cutoff 变成负数", 而那件事由 Day 8 的哨兵判据看着
// (余弦要么是无效标记, 要么严格为正)。
// ============================================================================

bool MeshletBackfaceCull(vec4 localCone, vec4 worldSphere,
                         vec3 cameraPosition, vec4 row0, vec4 row1, vec4 row2)
{
    if (localCone.w <= kInvalidConeCosine)
    {
        return false;
    }

    if (!MeshletUniformScale(row0, row1, row2))
    {
        return false;
    }

    // 均匀缩放下法线只需旋转 —— 把轴按三列方向变过去再归一化
    const vec3 axis = normalize(vec3(
        dot(row0.xyz, localCone.xyz),
        dot(row1.xyz, localCone.xyz),
        dot(row2.xyz, localCone.xyz)));

    const vec3 d = worldSphere.xyz - cameraPosition;

    const float distance = length(d);

    return dot(d, axis) >= localCone.w * distance + worldSphere.w;
}

#endif // LIMX_MESHLET_COMMON_H
