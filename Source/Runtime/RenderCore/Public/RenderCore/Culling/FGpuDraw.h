/*******************************************************************************
 * 文件: FGpuDraw.h
 * 创建时间: 2026-08-31
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   GPU 驱动绘制的两个数据结构 — 逐物体数据与间接绘制命令
 *
 * 设计哲学:
 *   Day 1 那次逐 Pass 计时给出的结论一直没变: **整条渲染是 CPU 受限的**。
 *     压力场景实测 CPU 3.41 ms 对 GPU 0.275 ms, 差了十二倍, 而其中 2.57 ms
 *     花在命令录制上。录制的内容是 576 个物体 × 每个一次视锥测试、几次句柄
 *     比较、一次 68 字节的 push constant 上传、一次 DrawIndexed。
 *
 *   GPU 驱动把这一整段搬到显卡上: CPU 每帧只写一次逐物体数据 (模型矩阵 +
 *     包围球 + 绘制参数), 计算着色器做剔除并写出间接命令, 图形通道按
 *     "同一对顶点/索引缓冲区"分组, 每组一次 DrawIndexedIndirect。576 次绘制
 *     降到十几次。
 *
 *   包围球而非包围盒 — 不是为了省指令, 是为了**保证 GPU 剔除掉的是 CPU 也
 *     会剔除掉的子集**。外接球包住整个 AABB, 所以"球与视锥相交"是"盒与视锥
 *     相交"的必要条件的放宽: GPU 保留的物体一定是 CPU 保留的超集。多画的
 *     那些本来就在视锥外, 会被裁剪掉, 一个像素都不会变。
 *
 *     反过来 (GPU 比 CPU 更严) 就是画面上少东西, 而那与"这个物体没加载"
 *     长得一样。两条路径要能逐像素比对, 方向就必须是这一个。
 *
 *   firstInstance 携带物体下标 — 顶点着色器靠 gl_InstanceIndex 去 storage
 *     buffer 里取自己的模型矩阵。这需要设备特性 drawIndirectFirstInstance;
 *     不支持时 firstInstance 恒为 0, **整个场景会挤在同一个变换上**, 而那
 *     不是崩溃、只有开着验证层才报。所以那一条必须查询, 不支持就退回逐物体
 *     绘制。
 *
 * 依赖关系:
 *   内部: Core/Math/FMatrix.h, Core/Math/FBoundingBox.h
 *
 ******************************************************************************/

#pragma once

#include "RenderCore/RenderCoreMinimal.h"

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Math/FVector.h"
#include "Core/Math/FMatrix.h"
#include "Core/Math/FMath.h"
#include "Core/Math/FBoundingBox.h"

namespace Limx
{

// ============================================================================
// 上限
// ============================================================================

/// 一帧最多参与 GPU 驱动绘制的物体数
///
/// 65536 × 96 字节 = 6 MiB, 每并行帧一份。固定上限而非动态扩容: 扩容意味着
/// 缓冲区换了句柄, 而描述符集指向的是旧句柄 —— 要么每帧重写描述符, 要么在
/// 某一帧读到已释放的显存。固定上限把这个状态整个消掉了。
///
/// 超过上限的物体退回逐物体绘制而不是丢弃 —— 丢弃的表现是"场景里少了一部分
/// 东西", 而那与资源加载失败长得一样。
inline constexpr UInt32 kMaxGpuDrawObjects = 65536;

// ============================================================================
// 间接绘制命令
// ============================================================================

/// 与 VkDrawIndexedIndirectCommand 逐字段一致 (20 字节)
///
/// 自己声明而不是包 vulkan.h: RenderCore 不该依赖某一个图形 API 的头文件。
/// 代价是这个结构与 Vulkan 的定义之间没有编译期保障 —— 所以字段顺序在下面
/// 逐个注明, 而 sizeof 有 static_assert 兜住。
///
/// 顺序写错的表现是"绘制数量与索引数量互换": 画出来的三角形数目荒谬, 或者
/// 干脆什么都不画。不会有任何报错。
struct FDrawIndexedIndirectCommand
{
    UInt32 IndexCount    = 0;   // vkCmdDrawIndexed 的 indexCount
    UInt32 InstanceCount = 0;   // 0 = 被剔除
    UInt32 FirstIndex    = 0;
    Int32  VertexOffset  = 0;   // 有符号 —— Vulkan 就是这么定义的
    UInt32 FirstInstance = 0;   // 物体下标, 顶点着色器靠它索引
};

static_assert(sizeof(FDrawIndexedIndirectCommand) == 20,
    "FDrawIndexedIndirectCommand 必须为 20 字节 — 与 "
    "VkDrawIndexedIndirectCommand 一致");

// ============================================================================
// 逐物体数据
// ============================================================================

/// 交给 GPU 的单个物体 (std430, 96 字节)
///
/// 模型矩阵与绘制参数放在一起, 是因为它们的消费者不同却同源: 矩阵给顶点
/// 着色器, 绘制参数给剔除的计算着色器写间接命令。拆成两个缓冲区意味着两处
/// 下标必须一致, 而不一致的表现是"这个物体用了另一个物体的变换"。
struct FGpuDrawObject
{
    /// 世界变换 (行主序 — 着色器侧必须写 row_major)
    FMatrix Model;

    /// 世界空间包围球: xyz = 球心, w = 半径
    Float32 BoundsCenterX = 0.0f;
    Float32 BoundsCenterY = 0.0f;
    Float32 BoundsCenterZ = 0.0f;
    Float32 BoundsRadius  = 0.0f;

    /// 绘制参数: 索引数 / 首索引 / 顶点偏移 / bindless 材质下标
    UInt32 IndexCount    = 0;
    UInt32 FirstIndex    = 0;
    UInt32 VertexOffset  = 0;
    UInt32 MaterialIndex = 0;
};

static_assert(sizeof(FGpuDrawObject) == 96,
    "FGpuDrawObject 必须为 96 字节 (mat4 + 2×vec4, std430) — 与 "
    "gpu_draw_common.h 的 DrawObject 一致, 由 "
    "Scripts/verify-shader-layout.ps1 逐次核对");

// ============================================================================
// 包围盒 → 外接球
// ============================================================================

/// AABB 的外接球
///
/// **必须外接而不是内切。** 内切球被 AABB 包住, 于是"球在视锥外"不能推出
/// "盒在视锥外" —— GPU 会剔掉 CPU 保留的物体, 画面上少东西。外接球反过来,
/// GPU 保留的一定是 CPU 保留的超集, 多画的那些在视锥外、会被裁掉。
///
/// 这个方向不能反。反了的症状是"某些角度下某个物体突然消失", 而那看起来像
/// 是可见性判断的问题 —— 离真正的原因 (球取小了) 隔着一层。
LIMX_NODISCARD inline FVector4 BoundingSphereFromBox(const FBoundingBox& box)
{
    if (!box.IsValid())
    {
        // 无效包围盒 → 半径无穷大的球, 也就是"永远保留"。
        //
        // 与 CPU 侧的剔除同一原则: 把"信息缺失"当成"不可见"会静默丢掉物体。
        return FVector4(0.0f, 0.0f, 0.0f, 3.4e38f);
    }

    const FVector3 center((box.Min.X + box.Max.X) * 0.5f,
                          (box.Min.Y + box.Max.Y) * 0.5f,
                          (box.Min.Z + box.Max.Z) * 0.5f);

    const Float32 extentX = (box.Max.X - box.Min.X) * 0.5f;
    const Float32 extentY = (box.Max.Y - box.Min.Y) * 0.5f;
    const Float32 extentZ = (box.Max.Z - box.Min.Z) * 0.5f;

    const Float32 radius = FMath::Sqrt(extentX * extentX +
                                       extentY * extentY +
                                       extentZ * extentZ);

    return FVector4(center.X, center.Y, center.Z, radius);
}

} // namespace Limx
