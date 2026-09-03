// ============================================================
// 文件名称：FMeshLodDag.h
// 创建时间：2026-09-02
// 创建者  ：LimxTeam
// 设计哲学：**DAG 的边是"组"级的，不是 meshlet 级的。**
//
//          一个组被当作一个整体简化再切分，所以：第 L+1 层某个 meshlet 的
//          子，是产出它的那个组的**全部**成员；第 L 层某个 meshlet 的父，
//          是它所属组产出的**全部** meshlet。
//
//          为什么不做更细的父子（逐 meshlet 甚至逐三角形）：边坍缩会把一片
//          区域揉成任意形状，重新切 meshlet 又会把三角形跨边界重排 —— 更细
//          的"父"是编出来的，而 LOD 选择规则会真的去依赖它。
//
//          代价很具体：若把父记成"重叠最大的那一个 meshlet"，同一组的两个
//          兄弟就拿到不同的父误差，于是在同一个相机下一个换层一个不换。
//          它们之间的边界**在组内部**，没有被锁定 —— 那是真裂缝。画面上是
//          一像素宽的背景色发丝，沿着组内部的旧 meshlet 边界，随相机移动
//          闪烁；静态截图里几乎发现不了，一动就明显。
//
//          第二条同样要紧：误差必须**累加**而不是取最大，而且父必须
//          **严格**大于子。理由见 FLodGroup::Error 那段。
//
// 功能描述：反复"分组 → 组内简化到一半 → 按组重新切 meshlet"，建出一棵
//          自底向上的 LOD DAG。
// 技术特性：逐组独立简化（组间边界锁死）；每层的 meshlet 按组分别切分，
//          保证"一个 meshlet 只由一个组产出"；误差沿边严格单调；停止条件
//          是到根 / 简化不动 / 层数上限，**不按误差停**。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                    │ 描述                           │
// │──────────────────────────│───────────────────────────────│
// │ Build()                  │ 由顶点+索引建出整棵 DAG        │
//
// ── 结构体表 ──────────────────────────────────────────────────
// │ 结构体名                   │ 描述                           │
// │──────────────────────────│───────────────────────────────│
// │ FMeshLodDagOptions       │ 组大小、层数上限、停滞阈值      │
// │ FLodMeshletRecord        │ 一个 meshlet 的 LOD 记录        │
// │ FLodGroup                │ 一个组：子、父、球、误差        │
// │ FLodLevel                │ 一层：顶点、索引、meshlet、记录 │
// │ FMeshLodDagResult        │ 全部层 + 全部组                 │
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
#include "RenderCore/Geometry/FMeshletBuilder.h"

namespace Limx
{

// LOD DAG 日志类别
LIMX_DECLARE_LOG_CATEGORY(LogMeshLodDag)

/// 无效下标 —— 叶子的产出组、根的消费组
constexpr UInt32 kLodInvalidIndex = 0xFFFFFFFFu;

/// 根 meshlet 的父误差 —— 一个巨大的有限数
///
/// 不用 FLT_MAX: 运行期还要乘实例缩放。留八个数量级的余量, 乘完仍然是有限数,
/// 而"有限的巨大数 >= 任何阈值"与 +inf 一样成立, 却不会在与别的数比较时冒出
/// NaN。这与法线锥的 -2 哨兵、可见性编号的 +1 是同一个思路: 让退化情形落在
/// 合法值上、落在安全的一侧, 而不是靠一个分支。
constexpr Float32 kLodInfiniteError = 1.0e30f;

// ============================================================================
// FMeshLodDagOptions
// ============================================================================

/// 建 DAG 的参数
struct FMeshLodDagOptions
{
    /// 目标每组多少个 meshlet
    UInt32 TargetGroupSize = 16;

    /// 每组至多多少个
    UInt32 MaxGroupSize = 32;

    /// 层数上限
    ///
    /// 2^16 = 65536 倍缩减, 任何八百万三角形以下的网格早就到根了。它的作用
    /// 是在别的两个停止条件都写错时仍然终止, 而不是"跑到显存爆"。
    UInt32 MaxLevels = 16;

    /// 停滞阈值 —— 这一层的三角形数超过上一层的这个比例就停
    ///
    /// 锁边界会在很粗的层上占住大部分顶点, 再简化不动。继续建只会产生一串
    /// 误差几乎相同的层, 而那些层的误差挤在一起, LOD 选择会在它们之间反复
    /// 横跳 —— 表现是相机微动时几何抖动。
    Float32 StagnationRatio = 0.95f;
};

// ============================================================================
// FLodMeshletRecord
// ============================================================================

/// 一个 meshlet 的 LOD 记录
struct FLodMeshletRecord
{
    /// 产出它的那个组的 LOD 球 (叶子层是 meshlet 自己的包围球)
    FVector4 SelfSphere = FVector4(0.0f, 0.0f, 0.0f, 0.0f);

    /// 将把它简化掉的那个组的 LOD 球 (根等于 SelfSphere)
    FVector4 ParentSphere = FVector4(0.0f, 0.0f, 0.0f, 0.0f);

    /// 相对**原始**表面的偏差上界 (叶子层是 0)
    Float32 SelfError = 0.0f;

    /// 同上; 根是 kLodInfiniteError
    Float32 ParentError = kLodInfiniteError;

    /// 产出它的组 (叶子层是 kLodInvalidIndex)
    UInt32 SourceGroup = kLodInvalidIndex;

    /// 将把它简化掉的组 (根是 kLodInvalidIndex)
    UInt32 TargetGroup = kLodInvalidIndex;

    /// 它在哪一层
    UInt32 Level = 0;
};

// ============================================================================
// FLodGroup
// ============================================================================

/// DAG 的一条边 —— 一个组
struct FLodGroup
{
    /// 组里的 meshlet 在哪一层
    UInt32 Level = 0;

    /// 第 Level 层里的成员 (下标进 FLodLevel::Meshlets)
    TArray<UInt32> ChildMeshlets;

    /// 第 Level+1 层里由它简化出来的 meshlet
    TArray<UInt32> ParentMeshlets;

    /// 组的 LOD 球 —— 含全部成员的自身球, 且半径不小于 Error
    ///
    /// "半径不小于 Error"不是保险, 是运行期那条投影公式保守性的**前提**:
    /// 半径 e 的误差球在球心距 D 处的精确投影半径是 e/sqrt(D²-e²), 而公式
    /// 用的分母是 D-r。只有 r >= e 时 D-r <= sqrt(D²-e²), 结果才只会偏大。
    FVector4 Sphere = FVector4(0.0f, 0.0f, 0.0f, 0.0f);

    /// 组的误差 —— 相对**原始**表面的偏差上界, 沿 DAG 的边严格递增
    ///
    /// 算法: max(子误差 + 本次简化误差, 子误差 * (1 + 2^-10) + 绝对地板)
    ///
    /// ── 为什么是累加而不是取最大 ──
    ///
    /// Simplify 量的是"第 L+1 层相对第 L 层"的偏差, 不是相对**原始**表面的。
    /// 由三角不等式, 相对原始的界是 子误差 + 本次误差。
    ///
    /// 取最大也能保住单调, 画面上不裂、不漏 —— 只是"误差是上界"这句话变成
    /// 假的: 第五层报 0.1 而实际偏 0.6。表现是远处轮廓明显走形、地形起伏被
    /// 抹平, 而**所有几何判据全绿**。这是最难查的那一类。
    ///
    /// ── 为什么必须严格增, 而且是 float32 意义上的严格 ──
    ///
    /// 平坦区域上本次误差可以恰好是 0 (共面坍缩), 那时父误差 == 子误差,
    /// 而选择规则"自身 < 阈值且父 >= 阈值"在两者相等时**永不成立** —— 那块
    /// 表面在**每一个**阈值下都不画。表现是一大片墙或地板整块消失, 而且不随
    /// 距离变化, 很容易被误判成"这块几何没导进来"。
    ///
    /// 地板取 2^-10 的相对增长: 它远大于 float32 的 2^-23, 所以乘出来的两个
    /// 数**必然是不同的浮点数**。只加一个绝对小量是不够的 —— 误差大起来之后
    /// 那个小量会被舍进去。
    Float32 Error = 0.0f;

    /// 本次简化自己报的误差 (诊断用, 不参与选择)
    Float32 OwnError = 0.0f;
};

// ============================================================================
// FLodLevel
// ============================================================================

/// DAG 的一层
struct FLodLevel
{
    TArray<FMeshVertex> Vertices;
    TArray<UInt32>      Indices;

    /// 这一层的 meshlet —— **按组分别切分之后拼起来**的
    ///
    /// 不是把整层的几何合起来重切: 那样一个 meshlet 可能跨两个组的几何,
    /// "一个 meshlet 只由一个组产出"这条就不成立了, 而选择规则的整个证明
    /// 都建立在它上面。
    FMeshletBuildResult Meshlets;

    /// 与 Meshlets.Meshlets 同下标
    TArray<FLodMeshletRecord> Records;
};

// ============================================================================
// FMeshLodDagResult
// ============================================================================

/// 建出来的 DAG
struct FMeshLodDagResult
{
    TArray<FLodLevel> Levels;
    TArray<FLodGroup> Groups;

    /// 为什么停的 —— 判据要拿它区分"到根了"与"简化不动了"
    enum class EStopReason : UInt8
    {
        ReachedRoot,
        Stagnated,
        LevelLimit,
        Failed,
    };

    EStopReason StopReason = EStopReason::Failed;

    LIMX_NODISCARD bool IsValid() const { return Levels.GetSize() >= 1; }
};

// ============================================================================
// FMeshLodDagBuilder
// ============================================================================

/// LOD DAG 构建器
class FMeshLodDagBuilder
{
public:
    /// 建出整棵 DAG
    ///
    /// 第 0 层就是输入的 meshlet 切分, **一个三角形都不动** —— 判据会拿它与
    /// 原始索引数组做多重集比较 (连绕序)。焊接会改顶点下标, 所以哪怕表面
    /// 一模一样也不许在第 0 层跑一遍简化。
    LIMX_NODISCARD static FMeshLodDagResult Build(
        const TArray<FMeshVertex>& vertices, const TArray<UInt32>& indices,
        const FMeshLodDagOptions& options);
};

} // namespace Limx
