// ============================================================
// 文件名称：FRayTracingScene.h
// 创建时间：2026-08-31
// 创建者  ：LimxTeam
// 设计哲学：加速结构与渲染对象列表的对应关系必须是**显式而不是隐含**的。
//          BLAS 逐渲染对象建一个 (一个子网格一棵), TLAS 里第 i 个实例
//          就对应渲染对象列表里第 i 个 —— 于是着色器拿到的
//          instanceCustomIndex 直接就是物体下标，不需要再查一张映射表。
//          隐含的对应关系一旦错位，表现是"反射里的材质串了位"，而那是
//          几天后才会被发现的那种错。
// 功能描述：从渲染对象列表构建并维护光追加速结构 — 逐对象 BLAS、单个
//          TLAS、每帧刷新实例变换。
// 技术特性：BLAS 只在几何体变化时重建 (变换变化只需重建 TLAS)；实例
//          变换由 FTransform 转成 3x4 行主序；跳过没有有效几何体的对象
//          时会**记录并上报**跳过的数量，而不是静默略过。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                          │ 描述                      │
// │────────────────────────────────│─────────────────────────│
// │ Initialize()                   │ 建 TLAS, 定实例数上限      │
// │ Shutdown()                     │ 销毁全部加速结构           │
// │ RebuildGeometry()              │ 按对象列表重建全部 BLAS    │
// │ UpdateInstances()              │ 刷新实例变换 (不重建 BLAS) │
// │ RecordBuild()                  │ 把构建命令录进命令缓冲区   │
// │ GetTlas()                      │ 取 TLAS 句柄              │
// ============================================================

#pragma once

#include "Renderer/RendererMinimal.h"

#include "Core/Containers/TArray.h"
#include "RHI/RHI/RHIResources.h"

namespace Limx
{

class IRHIDevice;
class IRHICommandBuffer;
struct FRenderObject;

// ============================================================================
// FRayTracingScene — 场景的光追加速结构
// ============================================================================

// ============================================================================
// 射线掩码 — 按混合模式给实例分类
//
// 不同用途的射线要看见的东西不一样, 而这个差别**不能**靠"建几棵树"来解决
// (那要多付一份构建开销与显存)。Vulkan 的实例掩码正是为此存在: 一棵树,
// 射线自己声明要看哪几类。
//
// 分类的依据是"这个物体在光栅化里写不写深度":
//   不透明   写。射线与深度缓冲区应当逐像素一致。
//   蒙版     写, 但按 alpha 测试挖了洞。ray query 没有 any-hit, 评估不了
//            alpha, 所以它在光追里是**实心的** —— 这是一个已知的近似,
//            不是 bug。
//   半透明   **不写**。把它按不透明算进遮挡是错的: 玻璃后面的东西在深度
//            缓冲区里是可见的, 在光追里却被挡住。
// ============================================================================

/// 不透明几何体
inline constexpr UInt32 kRayMaskOpaque = 0x01u;

/// 蒙版几何体 (alpha 测试)
inline constexpr UInt32 kRayMaskMasked = 0x02u;

/// 半透明几何体
inline constexpr UInt32 kRayMaskTranslucent = 0x04u;

/// 会写入深度缓冲区的那一类 —— 与光栅化深度比对时用这个
inline constexpr UInt32 kRayMaskDepthWriting =
    kRayMaskOpaque | kRayMaskMasked;

/// 全部
inline constexpr UInt32 kRayMaskAll =
    kRayMaskOpaque | kRayMaskMasked | kRayMaskTranslucent;

class LIMX_RENDERER_API FRayTracingScene
{
public:
    FRayTracingScene() = default;
    ~FRayTracingScene() = default;

    FRayTracingScene(const FRayTracingScene&) = delete;
    FRayTracingScene& operator=(const FRayTracingScene&) = delete;

    /// 实例数上限
    ///
    /// 与 kMaxGpuDrawObjects 取同一个数, 因为它们索引的是同一份对象列表。
    /// 两个数不一致时, 超出的那部分物体在光追里存在而在光栅化里不存在
    /// (或者反过来) —— 而那种不一致只在物体数刚好落在两者之间时才出现。
    static constexpr UInt32 kMaxInstances = 16384;

    /// 建 TLAS。BLAS 要等到第一次 RebuildGeometry
    LIMX_NODISCARD ERHIResult Initialize(IRHIDevice* device);

    /// 销毁 TLAS 与全部 BLAS
    void Shutdown();

    /// 每帧调用一次: 按需重建 BLAS, 并刷新实例变换
    ///
    /// "要不要重建 BLAS"由本类自己按几何签名判定, 不交给调用方。
    ///
    /// 交给调用方的话, 它必须记住"换了网格要重建、只是移动了不用" ——
    /// 而判错的那一半 (该重建却没重建) 的表现是: 光追里的形状还是上一个
    /// 场景的, 位置却是新场景的。画面照常, 只有反射与光追阴影不对。
    LIMX_NODISCARD ERHIResult Update(const TArray<FRenderObject>& objects);

    /// 把构建命令录进命令缓冲区
    ///
    /// 是否连 BLAS 一起重建由上一次 Update 定下, 这里只是消费它。
    void RecordBuild(IRHICommandBuffer* commandBuffer);

    LIMX_NODISCARD FRHIAccelStructHandle GetTlas() const { return m_Tlas; }

    LIMX_NODISCARD bool IsValid() const { return m_Tlas.IsValid(); }

    /// TLAS 里的实例数 —— 等于成功建出 BLAS 的对象数
    LIMX_NODISCARD UInt32 GetInstanceCount() const { return m_InstanceCount; }

    /// 建出来的 BLAS 数
    LIMX_NODISCARD UInt32 GetBlasCount() const
    {
        return static_cast<UInt32>(m_Blas.GetSize());
    }

    /// 各类实例的数量 (下标用 0=不透明 1=蒙版 2=半透明)
    ///
    /// 判据要用: "这个场景里有没有蒙版/半透明"决定了某些比对能不能做。
    /// 没有的话那条判据就是空的, 而空判据必须与通过分得开。
    LIMX_NODISCARD UInt32 GetInstanceCountByClass(UInt32 classIndex) const
    {
        return (classIndex < 3) ? m_ClassCounts[classIndex] : 0;
    }

    /// 因几何体无效而被跳过的对象数
    ///
    /// 这个数必须能被外部看到。跳过本身可能是对的 (点精灵一类没有三角形
    /// 的东西), 但"全部都被跳过了"与"场景是空的"必须分得开 —— 后者会让
    /// 任何光追判据在一棵空树上满分通过。
    LIMX_NODISCARD UInt32 GetSkippedCount() const { return m_SkippedCount; }

private:
    IRHIDevice* m_Device = nullptr;

    FRHIAccelStructHandle m_Tlas;

    /// 逐渲染对象一棵 BLAS。下标与 m_SourceIndices 平行
    TArray<FRHIAccelStructHandle> m_Blas;

    /// m_Blas[i] 来自渲染对象列表里的第几个
    ///
    /// 跳过无效几何体之后两个下标就不再相等了。着色器读到的
    /// instanceCustomIndex 存的是**源下标**, 所以这张表是把 BLAS 顺序
    /// 映回对象顺序的唯一依据。
    TArray<UInt32> m_SourceIndices;

    UInt32 m_InstanceCount = 0;
    UInt32 m_SkippedCount  = 0;

    /// 不透明 / 蒙版 / 半透明 各多少个实例
    UInt32 m_ClassCounts[3] = {};

    /// 上一次建 BLAS 时几何体的签名
    ///
    /// 只覆盖**决定 BLAS 形状**的字段 (缓冲区句柄、索引区间、顶点数与
    /// 跨度), 不含变换 —— 物体移动不需要重建 BLAS。
    UInt64 m_GeometrySignature = 0;

    /// 下一次 RecordBuild 要不要连 BLAS 一起建
    bool m_NeedsBlasBuild = false;

    void DestroyBlas();

    LIMX_NODISCARD ERHIResult RebuildGeometry(
        const TArray<FRenderObject>& objects);

    LIMX_NODISCARD ERHIResult UpdateInstances(
        const TArray<FRenderObject>& objects);

    LIMX_NODISCARD static UInt64 ComputeGeometrySignature(
        const TArray<FRenderObject>& objects);
};

} // namespace Limx
