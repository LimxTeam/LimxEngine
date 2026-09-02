// ============================================================
// 文件名称：meshlet_raster_common.h
// 创建时间：2026-09-02
// 创建者  ：LimxTeam
// 设计哲学：网格着色器路径与计算回退路径必须**逐位**画出同一张图。
//          两条路径的顶点变换、局部索引解包、三角形取顶点, 全部只有
//          这一份实现 —— 各写各的话, 判据比的是"两个实现一不一样",
//          而两个实现可以一起错。
//
//          顶点变换的**运算顺序**也在这里钉死: (viewProj * model) * p,
//          与 depth_only.vert 逐字相同。写成 viewProj * (model * p) 在
//          数学上等价而在浮点上不等价, 于是深度会差一两个最低位 ——
//          那不会崩、不会报错, 只会让"与经典路径逐像素相同"这条判据
//          永远差那么一点点, 而没人说得清是哪里的问题。
// 功能描述：meshlet 的 GPU 端解包与顶点变换。
// ============================================================

#ifndef LIMX_MESHLET_RASTER_COMMON_H
#define LIMX_MESHLET_RASTER_COMMON_H

#include "meshlet_common.h"

// ============================================================================
// 顶点。与 FMeshVertex 逐字段一致 (72 字节)
//
// 只声明本路径要用的前两个字段是不行的 —— 数组步长由结构体大小决定,
// 少声明一个字段就把步长算小了, 于是取到的是别人的数据。
// ============================================================================

struct MeshletVertex
{
    vec3 position;
    vec3 normal;
    vec4 tangent;
    vec2 texCoord0;
    vec2 texCoord1;
    vec4 color;
};

/// 把实例的 3x4 行主序变换补成 mat4
///
/// GLSL 的 mat4 构造函数吃的是**列**向量, 而这三行是行 —— 所以这里是
/// 逐元素填, 不是把三行直接塞进去。塞进去的话得到的是转置, 而单位变换
/// 下转置又是恒等的, 于是只有旋转过的物体才出错。
mat4 MeshletInstanceMatrix(MeshletInstance instance)
{
    return mat4(
        vec4(instance.transformRow0.x, instance.transformRow1.x,
             instance.transformRow2.x, 0.0),
        vec4(instance.transformRow0.y, instance.transformRow1.y,
             instance.transformRow2.y, 0.0),
        vec4(instance.transformRow0.z, instance.transformRow1.z,
             instance.transformRow2.z, 0.0),
        vec4(instance.transformRow0.w, instance.transformRow1.w,
             instance.transformRow2.w, 1.0));
}

/// 局部索引缓冲区按 UInt32 打包, 每个字节一个局部下标
///
/// GLSL 里没有字节寻址的 storage buffer (要 GL_EXT_shader_8bit_storage),
/// 所以 C++ 侧按四个一组打包上传, 这里移位取回。
uint MeshletUnpackIndex(uint packed, uint byteIndex)
{
    return (packed >> ((byteIndex & 3u) * 8u)) & 0xFFu;
}

// ============================================================================
// 可见性缓冲的编号
//
// 一个像素上记的是"哪个可见记录的第几个三角形"。可见记录 (visible slot)
// 已经唯一确定了实例与 meshlet, 所以不必再存它们 —— 存了反而要多一层
// "这两个数一致吗"的问题。
//
// 低 7 位放三角形序号 (上限 124, 7 位够), 高 25 位放可见槽位 (上限
// 262144, 18 位够, 留了余量)。
//
// 0xFFFFFFFF 是"这里没有几何体"。取全一而不是 0: 0 是**合法**的编号
// (第 0 条可见记录的第 0 个三角形), 拿它当空值的话, 画面左上角那个
// 三角形与"没画"永远分不开 —— 而那正是本周期反复遇到的那类混淆。
// ============================================================================

// 存进缓冲区的值是 (编号 + 1), 于是 **0 就是"这里没有几何体"**。
//
// 为什么不直接拿 0xFFFFFFFF 当空值: 清除颜色附件走的是浮点通道
// (FRHIClearColorValue 只有四个 Float32), 而 R32_UINT 附件上只有 0.0f
// 的位模式恰好是整数 0 —— 别的值都要靠位重解释, 那要动 RHI。
//
// 为什么不直接拿 0 当空值而不加偏移: 0 是**合法**的编号 (第 0 条可见记录
// 的第 0 个三角形)。拿它当空值的话, 那一个三角形与"没画"永远分不开 ——
// 而那正是本周期反复遇到的那类混淆 (法线哨兵、法线锥哨兵都是同一件事)。
//
// 低 7 位放三角形序号 (上限 124), 高位放可见槽位 (上限 262144, 18 位)。
// 加一之后最大值是 262143*128 + 123 + 1, 远在 32 位之内。

const uint kVisibilityEmpty = 0u;

const uint kVisibilityTriangleBits = 7u;
const uint kVisibilityTriangleMask = (1u << kVisibilityTriangleBits) - 1u;

uint MeshletEncodeVisibility(uint visibleSlot, uint triangleIndex)
{
    return ((visibleSlot << kVisibilityTriangleBits) |
            (triangleIndex & kVisibilityTriangleMask)) + 1u;
}

bool MeshletVisibilityValid(uint visibility)
{
    return visibility != kVisibilityEmpty;
}

uint MeshletDecodeSlot(uint visibility)
{
    return (visibility - 1u) >> kVisibilityTriangleBits;
}

uint MeshletDecodeTriangle(uint visibility)
{
    return (visibility - 1u) & kVisibilityTriangleMask;
}

#endif // LIMX_MESHLET_RASTER_COMMON_H
