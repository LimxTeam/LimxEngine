// ============================================================
// 文件名称：FMeshletBuilder.h
// 创建时间：2026-09-02
// 创建者  ：LimxTeam
// 设计哲学：切分必须是**无损**的 —— 展开全部 meshlet 得到的三角形集合，
//          与原始索引数组逐个三角形、连绕序一起，完全相同。
//          这不是"应该如此"，是这个数据结构存在的前提：下游 (剔除、
//          可见性缓冲、材质解析) 全都假定 meshlet 就是原网格的一个划分。
//          少一个三角形表现为"模型上有个洞"，多一个表现为 Z 冲突，
//          而两者都可能只在某个视角下才看得见。
//
//          所以本文件配一条纯组合的判据 (--meshlet-check)：它不看画面、
//          不依赖场景，只问"这是不是同一个三角形集合"。
// 功能描述：把三角形网格切成 meshlet — 每个 meshlet 一组局部顶点、一组
//          3 字节局部索引的三角形、一个包围球、一个法线锥。
// 技术特性：贪心邻接聚类 (每次挑"新增顶点最少"的相邻三角形，同分时挑离
//          当前质心最近的)；顶点/三角形双上限；包围球取 AABB 中心 + 最远
//          距离；法线锥取单位法线均值为轴、全体法线的最小投影为半角余弦，
//          张角超过半球时标记为无效锥 (剔除侧必须当它不可剔)。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                    │ 描述                           │
// │──────────────────────────│───────────────────────────────│
// │ Build()                  │ 由顶点+索引切出 meshlet        │
// │ ComputeStatistics()      │ 统计填充率与顶点复用率          │
//
// ── 结构体表 ──────────────────────────────────────────────────
// │ 结构体名                   │ 描述                           │
// │──────────────────────────│───────────────────────────────│
// │ FMeshlet                 │ 一个 meshlet 的头 (48 字节)     │
// │ FMeshletBuildResult      │ 切分结果 (头 + 两个索引数组)     │
// │ FMeshletStatistics       │ 填充率/复用率/包围体紧致度       │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-09-02  │ LimxTeam  │ 初始创建                        │
// ============================================================

#pragma once

// RenderCoreMinimal 必须先行 —— 理由见 FGeometryGenerator.h 顶部那段。
#include "RenderCore/RenderCoreMinimal.h"

#include "AssetPipeline/FAssetTypes.h"

namespace Limx
{

// meshlet 构建日志类别
LIMX_DECLARE_LOG_CATEGORY(LogMeshletBuilder)

// ============================================================================
// meshlet 的两个上限
//
// 64 顶点 / 124 三角形。
//
// 顶点取 64: 网格着色器一个工作组常取 32 或 64 个调用, 一个调用输出一个
// 顶点时 64 正好一趟做完; 而局部索引因此只需 6 位, 三个凑一起不到 3 字节。
//
// 三角形取 124 而不是 128: Vulkan 的 maxMeshOutputPrimitives 在主流硬件上
// 是 256, 但驱动内部按 128 个图元一批调度, 而每批要留几个槽位放图元剔除的
// 掩码 —— 取 124 让一个 meshlet 正好落进一批。同时 124*3 = 372 字节, 三个
// 字节一组时四字节对齐上也不别扭。
//
// 这两个数是**上限**不是目标: 贪心聚类先撞上哪个就在哪停。真实网格上通常
// 是顶点先满 —— 每个三角形平均只带来 0.5 到 0.8 个新顶点, 124 个三角形要
// 六七十个顶点。
// ============================================================================

inline constexpr UInt32 kMaxMeshletVertices = 64;
inline constexpr UInt32 kMaxMeshletTriangles = 124;

// ============================================================================
// FMeshlet — 一个 meshlet 的头
//
// 48 字节 (四个 UInt32 + 两个 vec4), 与将来 GPU 上的 storage buffer 布局
// 逐字段一致。48 是 16 的倍数, std430 下 vec4 的对齐要求自动满足。
//
// 没有补到 64 去凑一个缓存行: 那要多付 33% 的带宽, 而 meshlet 头是逐个
// 顺序读的 —— 顺序读时跨缓存行不花额外代价, 花代价的是随机访问。剔除
// 那一遍确实是顺序读。
// ============================================================================

struct FMeshlet
{
    /// 本 meshlet 的局部顶点表在 MeshletVertices 里的起点
    UInt32 VertexOffset = 0;

    /// 局部顶点数 (<= kMaxMeshletVertices)
    UInt32 VertexCount = 0;

    /// 本 meshlet 的三角形在 MeshletTriangles 里的起点 —— **以三角形计**
    ///
    /// 不是字节数。字节数要乘 3, 而"这里到底是三角形还是字节"是这种
    /// 数据结构上最容易写错的一处, 所以在名字与注释里都钉死。
    UInt32 TriangleOffset = 0;

    /// 三角形数 (<= kMaxMeshletTriangles)
    UInt32 TriangleCount = 0;

    /// 包围球 —— xyz = 球心, w = 半径 (局部空间)
    FVector4 BoundingSphere = FVector4(0.0f, 0.0f, 0.0f, 0.0f);

    /// 法线锥 —— xyz = 轴 (单位), w = 半角的余弦
    ///
    /// w 为 kInvalidConeCosine 时表示"这个 meshlet 的法线散得超过半球",
    /// 背面剔除对它无效。剔除侧必须把它当作**不可剔**, 而不是当作
    /// "cos = -2 所以永远不满足剔除条件" —— 后者碰巧也对, 但那是巧合,
    /// 换一个比较方向就错了。
    FVector4 NormalCone = FVector4(0.0f, 0.0f, 0.0f, 0.0f);
};

static_assert(sizeof(FMeshlet) == 48,
              "FMeshlet 必须是 48 字节 — 它要按这个布局直接进 storage buffer");

/// 法线锥无效的标记值
///
/// 取 -2 而不是 -1: 余弦的合法范围是 [-1,1], -1 是"张角恰好 180 度"这个
/// 合法值。用 -1 当哨兵的话, 一个真的张满半球的 meshlet 会被误判成无效锥,
/// 而那**恰好也是安全的** —— 于是这个混淆不会有症状, 会一直留着。
inline constexpr Float32 kInvalidConeCosine = -2.0f;

// ============================================================================
// FMeshletBuildResult — 切分结果
// ============================================================================

struct FMeshletBuildResult
{
    /// 全部 meshlet 的头
    TArray<FMeshlet> Meshlets;

    /// 局部顶点 -> 全局顶点下标
    ///
    /// 第 m 个 meshlet 的第 i 个局部顶点是
    ///     MeshletVertices[Meshlets[m].VertexOffset + i]
    TArray<UInt32> MeshletVertices;

    /// 三角形的局部索引 —— 每个三角形三个字节
    ///
    /// 第 m 个 meshlet 的第 t 个三角形的第 k 个角是
    ///     MeshletTriangles[(Meshlets[m].TriangleOffset + t) * 3 + k]
    /// 它是**局部**下标, 还要过一次 MeshletVertices 才是全局顶点。
    TArray<UInt8> MeshletTriangles;

    LIMX_NODISCARD bool IsValid() const { return !Meshlets.IsEmpty(); }
};

// ============================================================================
// FMeshletStatistics — 切分质量
//
// 正确但没用的切分是存在的: 一个三角形一个 meshlet 满足所有正确性判据,
// 而它把顶点数据放大了三倍、把剔除粒度缩到没有意义。所以质量要单独量,
// 并且单独判。
// ============================================================================

struct FMeshletStatistics
{
    UInt32 MeshletCount = 0;

    /// 平均每个 meshlet 的三角形数与顶点数
    Float32 AverageTriangles = 0.0f;
    Float32 AverageVertices = 0.0f;

    /// 顶点复用率 = 三角形数 * 3 / 局部顶点数
    ///
    /// 完全不复用是 1.0 (每个三角形三个独立顶点); 规则网格上理论上限
    /// 接近 6.0 (每个顶点被六个三角形共用)。这个数低就说明聚类没有把
    /// 相邻的三角形放到一起 —— 而那正是 meshlet 存在的理由。
    Float32 VertexReuse = 0.0f;

    /// 包围球半径的平均值 —— 越小剔除越准
    Float32 AverageSphereRadius = 0.0f;

    /// 法线锥有效 (张角未超半球) 的 meshlet 占比
    Float32 ValidConeFraction = 0.0f;
};

// ============================================================================
// FMeshletBuilder — 把三角形网格切成 meshlet
// ============================================================================

class LIMX_RENDERCORE_API FMeshletBuilder
{
public:
    FMeshletBuilder() = delete;

    /// 切分
    ///
    /// @param vertices 顶点数组 (只读位置与法线)
    /// @param indices  三角形索引, 长度必须是 3 的倍数
    /// @return         切分结果; 输入非法时 Meshlets 为空
    ///
    /// 索引数组长度不是 3 的倍数、或有索引越界时**返回空并报错**, 不是
    /// 悄悄截断。截断的后果是模型上少一块, 而那可能几个视角之后才被看到。
    LIMX_NODISCARD static FMeshletBuildResult Build(
        const TArray<FMeshVertex>& vertices, const TArray<UInt32>& indices);

    /// 统计切分质量
    LIMX_NODISCARD static FMeshletStatistics ComputeStatistics(
        const FMeshletBuildResult& result);
};

} // namespace Limx
