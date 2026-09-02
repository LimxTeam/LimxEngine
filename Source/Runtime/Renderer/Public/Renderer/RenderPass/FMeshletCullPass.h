/*******************************************************************************
 * 文件: FMeshletCullPass.h
 * 创建时间: 2026-09-02
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   两级 GPU 剔除 — 第一级按实例, 第二级按 meshlet。
 *
 * 设计哲学:
 *   **两级不是"做两遍同样的事"。** 第一级的输入是实例数 (几十到几千),
 *     第二级的输入是 meshlet 数 (几万到几百万)。第一级每剔掉一个实例,
 *     第二级就少几百次测试 —— 省下的不是一次测试, 是一整棵子树。
 *
 *   **两级必须用同一条视锥判据。** 不同的话会出现"实例级留下但 meshlet 级
 *     全剔掉"或者反过来, 而两者都是画面上少东西, 且只在特定视角出现。
 *     判据写在 meshlet_common.h 里, 两个着色器都包含它, C++ 的参考实现
 *     逐字照抄同一份。
 *
 *   **存疑就不剔。** 背面剔除有三种情形必须放行: 法线锥无效 (张角超半球)、
 *     相机在包围球内、实例缩放不一致 (法线要用逆转置变换, 锥的张角也会变)。
 *     剔错的后果是画面上少一块, 而那与"这一块本来就该被剔"长得完全一样。
 *
 *   **计数器不是可选的。** 一个什么都不剔的实现在画面上与正确实现完全一样,
 *     只是慢。没有"测试了多少 / 剔掉了多少"这几个数, 判据无从判定剔除到底
 *     跑没跑 —— 那正是本周期反复遇到的"失败落在通过上"。
 *
 * 技术特性:
 *   - Order 11: 紧跟 FGpuCullPass (10) 之后, 所有图形通道之前
 *   - 场景的 meshlet 汇总成一份连续缓冲区 (逐网格 GPU 到 GPU 拷贝)
 *   - 第一级压实出可见实例表, 第二级按"一个工作组一个可见实例"间接分派
 *   - 输出 (实例, meshlet) 对的紧凑表 + 四个计数器
 *
 * 依赖关系:
 *   内部: Renderer/RenderPass/IRenderPass.h, RenderCore/Geometry/FMeshletBuilder.h
 *
 * 注意事项:
 *   本通道**目前没有图形消费者** —— 网格着色器路径是下一天的事。所以今天
 *   它的判据全是数值判据 (与 CPU 参考实现逐个 meshlet 对齐), 而数值判据
 *   证明不了画面。这一点是明写的欠账: 端到端的判据是下一天的义务。
 *
 ******************************************************************************/

#pragma once

#include "Renderer/RenderPass/IRenderPass.h"

#include "Core/Containers/TArray.h"
#include "Core/Math/FFrustum.h"

namespace Limx
{

// ============================================================================
// 上限
//
// 与 kMaxGpuDrawObjects 一样, 这些数决定了缓冲区多大, 而超出时的行为是
// **丢掉并报数** —— 不是越界写。越界写会以完全无关的形式出错。
// ============================================================================

/// 场景里 meshlet 的总数上限
inline constexpr UInt32 kMaxSceneMeshlets = 262144;

/// 参与剔除的实例数上限
inline constexpr UInt32 kMaxMeshletInstances = 16384;

// ============================================================================
// FMeshletInstance — 与 meshlet_common.h 的 MeshletInstance 逐字段一致
// ============================================================================

struct FMeshletInstanceGpu
{
    /// 世界变换的前三行 (3x4 行主序), 平移在第四列
    Float32 TransformRow0[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
    Float32 TransformRow1[4] = { 0.0f, 1.0f, 0.0f, 0.0f };
    Float32 TransformRow2[4] = { 0.0f, 0.0f, 1.0f, 0.0f };

    /// x = meshlet 起点 (全局), y = meshlet 个数, z = 源对象下标, w = 保留
    UInt32 MeshletRange[4] = { 0, 0, 0, 0 };
};

static_assert(sizeof(FMeshletInstanceGpu) == 64,
              "FMeshletInstanceGpu 必须是 64 字节 — 与 meshlet_common.h 的 "
              "MeshletInstance 逐字段一致");

// ============================================================================
// FMeshletCullStats — 一次剔除的结果
// ============================================================================

struct FMeshletCullStats
{
    UInt32 InstancesTotal = 0;
    UInt32 InstancesVisible = 0;

    UInt32 MeshletsTested = 0;
    UInt32 MeshletsVisible = 0;
    UInt32 MeshletsCulledByFrustum = 0;
    UInt32 MeshletsCulledByBackface = 0;
};

// ============================================================================
// FMeshletCullPass
// ============================================================================

class FMeshletCullPass final : public IRenderPass
{
public:
    FMeshletCullPass()           = default;
    ~FMeshletCullPass() override = default;

    LIMX_NODISCARD const AnsiChar* GetName() const override
    {
        return "MeshletCullPass";
    }

    /// 11 — 紧跟 FGpuCullPass (10), 早于每一个图形通道
    ///
    /// "早于每一个"这句是 FGpuCullPass 用一次阴影贴图里画出未初始化显存
    /// 换来的。生产者必须排在**全部**消费者之前, 不是排在当时想得起来的
    /// 那几个之前。
    LIMX_NODISCARD UInt32 GetOrder() const override { return 11; }

    ERHIResult Setup(const FPassSetupDesc& desc) override;

    void Execute(IRHICommandBuffer*        commandBuffer,
                 const FRenderPassContext& context) override;

    ERHIResult OnResize(const FPassResizeDesc& desc) override;

    void ReleaseSwapchainResources(IRHIDevice* device) override;

    void Shutdown(IRHIDevice* device) override;

    // ====================================================================
    // 开关
    // ====================================================================

    void SetEnabled(bool enabled) { m_Enabled = enabled; }

    LIMX_NODISCARD bool IsEnabled() const { return m_Enabled; }

    /// 相机视锥 —— 由 FRenderer 每帧设置
    ///
    /// 与 FGpuCullPass 一样, 用**不含抖动**的投影矩阵算。抖动每帧改变
    /// 亚像素偏移, 用它算出的视锥边界会逐帧漂移 —— 漂移本身无害, 但会让
    /// "GPU 与 CPU 参考实现一致"这条判据变成逐帧不同, 无法比对。
    void SetFrustum(const FFrustum& frustum) { m_Frustum = frustum; }

    LIMX_NODISCARD const FFrustum& GetFrustum() const { return m_Frustum; }

    /// 相机世界位置 —— 背面剔除要用
    void SetCameraPosition(const FVector3& position)
    {
        m_CameraPosition = position;
    }

    LIMX_NODISCARD const FVector3& GetCameraPosition() const
    {
        return m_CameraPosition;
    }

    /// 背面剔除 (法线锥) 开关 —— 独立于视锥剔除
    ///
    /// 分开是为了让判据能单独验它。合在一起的话, "背面剔除完全没生效"
    /// 与"视锥剔除多剔了一点"在总数上分不开。
    void SetBackfaceCullEnabled(bool enabled) { m_BackfaceCull = enabled; }

    LIMX_NODISCARD bool IsBackfaceCullEnabled() const
    {
        return m_BackfaceCull;
    }

    // ====================================================================
    // 结果
    // ====================================================================

    /// 上一次执行的统计 —— 隔着并行帧数, 见实现里那段说明
    LIMX_NODISCARD const FMeshletCullStats& GetStats() const
    {
        return m_Stats;
    }

    /// 汇总后的场景 meshlet 缓冲区
    LIMX_NODISCARD FRHIBufferHandle GetSceneMeshletBuffer() const
    {
        return m_SceneMeshlets;
    }

    LIMX_NODISCARD UInt32 GetSceneMeshletCount() const
    {
        return m_SceneMeshletCount;
    }

    /// 本帧的实例表 (CPU 侧那一份) —— 判据要用它做参考实现
    LIMX_NODISCARD const TArray<FMeshletInstanceGpu>& GetInstances() const
    {
        return m_Instances;
    }

    /// 本帧的实例世界包围球 (每个四个 Float32) —— 参考实现要用
    ///
    /// 判据必须照着**两级**来。只做第二级的话, 被第一级剔掉的实例仍然会
    /// 被参考实现算成可见 —— 而"实例包围球包住它的每个 meshlet 包围球"
    /// 这个直觉**不成立**: meshlet 的包围球会从实例包围盒的角上鼓出去。
    /// 实测差 2 个 meshlet。
    LIMX_NODISCARD const TArray<Float32>& GetInstanceSpheres() const
    {
        return m_InstanceSpheres;
    }

    /// 可见 meshlet 表 (uvec2: 实例下标, meshlet 全局下标)
    LIMX_NODISCARD FRHIBufferHandle GetVisibleMeshletBuffer(
        UInt32 frameIndex) const;

    /// 计数器缓冲区 (uvec4)
    LIMX_NODISCARD FRHIBufferHandle GetCounterBuffer(UInt32 frameIndex) const;

private:
    /// 把场景里所有网格的 meshlet 汇总成一份连续缓冲区
    ///
    /// 只在几何签名变化时重做 —— 每帧重做的话是几十次 GPU 拷贝。
    /// 签名只看"有哪些 meshlet 缓冲区", 不看变换: 物体移动不改变 meshlet。
    bool RebuildSceneMeshlets(IRHIDevice* device,
                              IRHICommandBuffer* commandBuffer,
                              const TArray<struct FRenderObject>& objects);

    /// 按本帧的对象列表填实例表
    void BuildInstances(const TArray<struct FRenderObject>& objects);

    bool m_Enabled = false;
    bool m_BackfaceCull = true;

    FFrustum m_Frustum;
    FVector3 m_CameraPosition = FVector3(0.0f, 0.0f, 0.0f);

    /// 归零用的拷贝源 —— 前四个 uint 是计数器 (全 0),
    /// 后四个是分派参数 (0, 1, 1, 1)
    ///
    /// 分派参数的 y/z 必须是 1: 它们是 vkCmdDispatchIndirect 的三个维度,
    /// 全写 0 的话一个工作组都不起, 而那与"什么都不可见"分不开。
    FRHIBufferHandle m_ResetSource;

    IRHIDevice* m_Device = nullptr;

    // ---- 汇总后的场景 meshlet ----
    FRHIBufferHandle m_SceneMeshlets;
    UInt32           m_SceneMeshletCount = 0;

    /// 每个网格的 meshlet 缓冲区在汇总缓冲区里的起点 (以 meshlet 计)
    TArray<FRHIBufferHandle> m_SourceBuffers;
    TArray<UInt32>           m_SourceBases;

    /// 上一次汇总时的几何签名
    UInt64 m_GeometrySignature = 0;

    // ---- 逐并行帧的资源 ----
    TArray<FRHIBufferHandle> m_InstanceBuffers;
    TArray<FRHIBufferHandle> m_InstanceSphereBuffers;
    TArray<FRHIBufferHandle> m_VisibleInstanceBuffers;
    TArray<FRHIBufferHandle> m_DispatchBuffers;
    TArray<FRHIBufferHandle> m_VisibleMeshletBuffers;
    TArray<FRHIBufferHandle> m_CounterBuffers;
    TArray<FRHIBufferHandle> m_CounterReadbacks;

    TArray<FRHIDescriptorSetHandle> m_InstanceCullSets;
    TArray<FRHIDescriptorSetHandle> m_MeshletCullSets;

    FRHIDescSetLayoutHandle m_InstanceCullSetLayout;
    FRHIDescSetLayoutHandle m_MeshletCullSetLayout;

    FRHIPipelineLayoutHandle  m_InstanceCullLayout;
    FRHIPipelineLayoutHandle  m_MeshletCullLayout;
    FRHIComputePipelineHandle m_InstanceCullPipeline;
    FRHIComputePipelineHandle m_MeshletCullPipeline;
    FRHIShaderHandle          m_InstanceCullShader;
    FRHIShaderHandle          m_MeshletCullShader;

    /// CPU 侧的实例表 —— 判据的参考实现要用
    TArray<FMeshletInstanceGpu> m_Instances;
    TArray<Float32>             m_InstanceSpheres;

    FMeshletCullStats m_Stats;
};

} // namespace Limx
