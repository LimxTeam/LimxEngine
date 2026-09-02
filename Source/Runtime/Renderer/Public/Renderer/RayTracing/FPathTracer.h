// ============================================================
// 文件名称：FPathTracer.h
// 创建时间：2026-09-02
// 创建者  ：LimxTeam
// 设计哲学：这个类的产出是**别的东西的参考答案**，所以它交出来的不能只是
//          一张图。判据要判的是"这个数对不对"，而"对不对"必须连着"这个数
//          本身有多大的不确定度"一起给出 —— 否则容差只能拍脑袋。
//          因此逐像素回读四样东西：辐射度之和、辐射度平方之和、因深度截断
//          而终止的样本数、首次命中的样本数。
//            平方和 → 样本方差 → 标准误 σ/√N，误差预算从这里推。
//            截断计数 → 反照率为 1 时截断偏差恰好等于它，白炉判据因此在
//                       任何弹射次数下都是精确的（L + 截断比例 ≡ 1）。
//            首次命中 → 元判据：射线真的打到几何体了。没有这一项的话，
//                       一个"加速结构是空的"的实现会得到满屏环境光，而
//                       白炉测试对满屏 1.0 恰恰是判通过的。
//          场景接口刻意做成三角形汤而不是"渲染对象列表"：oracle 的价值在于
//          它与被验的那条路径**没有共享代码**，共享得越少越好。
// 功能描述：离线参考路径追踪器 —— 建单 BLAS/TLAS、分块派发计算着色器、
//          把逐像素统计量回读到 CPU。
// 技术特性：漫反射 BRDF + 余弦加权半球采样 + 俄罗斯轮盘；spp 分块提交
//          （避开 Windows 的 GPU 超时重置）；种子是 (像素, 样本号, 试验号)
//          的确定性哈希 —— 同样的参数永远给出同样的数。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                          │ 描述                      │
// │────────────────────────────────│─────────────────────────│
// │ Initialize()                   │ 建管线与描述符             │
// │ Shutdown()                     │ 释放全部 GPU 资源          │
// │ SetScene()                     │ 上传三角形汤并建加速结构   │
// │ Render()                       │ 跑满 spp 并回读逐像素统计  │
// │ GetTriangleCount()             │ 当前场景的三角形数         │
// ============================================================

#pragma once

#include "Renderer/RendererMinimal.h"

#include "Core/Containers/TArray.h"
#include "Core/Math/FVector.h"
#include "RHI/RHI/RHIResources.h"

namespace Limx
{

class IRHIDevice;
class FRenderContext;

// ============================================================================
// FPathTraceMaterial — 一种材质
//
// 只有漫反射反照率与自发光。参考实现不做金属度/粗糙度 —— 多一个参数就多
// 一条没有解析解可对的路径, 而 oracle 的每一条路径都必须有解析解可对。
// 与着色器的 Material 逐字段一致 (32 字节)。
// ============================================================================

struct FPathTraceMaterial
{
    Float32 AlbedoR = 0.0f;
    Float32 AlbedoG = 0.0f;
    Float32 AlbedoB = 0.0f;
    Float32 AlbedoPad = 0.0f;

    Float32 EmissionR = 0.0f;
    Float32 EmissionG = 0.0f;
    Float32 EmissionB = 0.0f;
    Float32 EmissionPad = 0.0f;
};

static_assert(sizeof(FPathTraceMaterial) == 32,
              "FPathTraceMaterial 必须是 32 字节 — 与着色器的 Material 逐字段"
              "一致");

// ============================================================================
// FPathTraceScene — 三角形汤
//
// 顶点是纯位置 (3 个 float 一组), 法线由着色器从三角形叉积算 —— 参考实现
// 不接受插值法线, 那是为了画面好看引入的近似。
// ============================================================================

struct FPathTraceScene
{
    /// 顶点位置, 每个顶点 3 个 Float32
    const Float32* Positions = nullptr;
    UInt32 VertexCount = 0;

    /// 三角形索引, 每 3 个一组
    const UInt32* Indices = nullptr;
    UInt32 IndexCount = 0;

    /// 逐三角形的材质下标 (长度 = IndexCount / 3)
    const UInt32* TriangleMaterials = nullptr;

    /// 材质表
    const FPathTraceMaterial* Materials = nullptr;
    UInt32 MaterialCount = 0;
};

// ============================================================================
// FPathTraceCamera — 针孔相机
// ============================================================================

struct FPathTraceCamera
{
    FVector3 Position = FVector3(0.0f, 0.0f, 0.0f);

    /// 前向 / 右向 / 上向 —— 必须两两正交且已归一化
    FVector3 Forward = FVector3(0.0f, 0.0f, -1.0f);
    FVector3 Right   = FVector3(1.0f, 0.0f, 0.0f);
    FVector3 Up      = FVector3(0.0f, 1.0f, 0.0f);

    /// 垂直视场角 (弧度)
    Float32 FovY = 1.0471976f;
};

// ============================================================================
// FPathTraceSettings — 一次渲染的全部参数
// ============================================================================

struct FPathTraceSettings
{
    UInt32 Width  = 128;
    UInt32 Height = 128;

    /// 每像素样本数
    UInt32 SamplesPerPixel = 256;

    /// 最大弹射次数 —— 0 表示只取首次命中的自发光, 不散射
    UInt32 MaxBounce = 8;

    /// 从第几次散射开始开轮盘 (>= MaxBounce 时轮盘永不触发)
    UInt32 RussianRouletteStartDepth = 1;

    /// 试验号 —— 只改这一个数就得到一组统计独立、但同样可复现的样本
    UInt32 TrialIndex = 0;

    /// 逃逸方向上的入射辐射度  L_env(ω) = EnvironmentRadiance
    ///                                 + EnvironmentGradientY · ω.y
    ///
    /// 各向同性 (梯度 0) 时是白炉测试的环境; 带梯度时半球上的余弦加权
    /// 平均与均匀加权平均分开, 于是"采样分布与 pdf 不匹配"变得可判定。
    Float32 EnvironmentRadiance  = 0.0f;
    Float32 EnvironmentGradientY = 0.0f;

    Float32 RayTMin = 0.0f;

    /// 二次射线起点沿法线的偏移 —— 必须盖住命中点重建的浮点误差
    Float32 NormalOffset = 1.0e-4f;

    Float32 MaxRayDistance = 1.0e6f;
};

// ============================================================================
// FPathTracePixel — 一个像素上回读的统计量
//
// 注意这里回读的是**和**而不是平均: 平均要除以样本数, 而样本数在分块派发
// 下是分几次凑齐的。让 CPU 拿到和自己去除, 分块与不分块的结果才逐位相同。
// ============================================================================

struct FPathTracePixel
{
    /// Σ L
    Float32 SumR = 0.0f;
    Float32 SumG = 0.0f;
    Float32 SumB = 0.0f;

    /// 因达到最大弹射次数而终止的样本数
    Float32 SumTruncated = 0.0f;

    /// Σ L²
    Float32 SumSqR = 0.0f;
    Float32 SumSqG = 0.0f;
    Float32 SumSqB = 0.0f;

    /// 首次命中几何体的样本数
    Float32 SumPrimaryHit = 0.0f;
};

static_assert(sizeof(FPathTracePixel) == 32,
              "FPathTracePixel 必须是 32 字节 — 与着色器写出的两个 vec4 对应");

// ============================================================================
// FPathTracer — 离线参考路径追踪器
// ============================================================================

class LIMX_RENDERER_API FPathTracer
{
public:
    FPathTracer() = default;
    ~FPathTracer() = default;

    FPathTracer(const FPathTracer&) = delete;
    FPathTracer& operator=(const FPathTracer&) = delete;

    /// 建计算管线、描述符布局与描述符集
    ///
    /// 设备不支持光追时返回失败 —— "跳过"绝不能表现为"通过"。
    LIMX_NODISCARD ERHIResult Initialize(IRHIDevice* device,
                                         FRenderContext* context);

    void Shutdown();

    /// 上传三角形汤并重建加速结构
    ///
    /// 可以反复调用 —— 每次都会先释放上一份场景的缓冲区与加速结构。
    LIMX_NODISCARD ERHIResult SetScene(const FPathTraceScene& scene);

    /// 跑满 spp 并把逐像素统计量回读到 outPixels
    ///
    /// outPixels 的长度是 Width * Height, 行优先。
    LIMX_NODISCARD ERHIResult Render(const FPathTraceCamera& camera,
                                     const FPathTraceSettings& settings,
                                     TArray<FPathTracePixel>& outPixels);

    LIMX_NODISCARD UInt32 GetTriangleCount() const { return m_TriangleCount; }

    /// 上一次 Render 实际提交了多少次分派 —— 分块逻辑的可观测出口
    LIMX_NODISCARD UInt32 GetLastDispatchCount() const
    {
        return m_LastDispatchCount;
    }

private:
    void ReleaseScene();
    void ReleaseAccumulation();

    /// 保证累加缓冲区至少能装下 pixelCount 个像素, 并清零
    LIMX_NODISCARD ERHIResult PrepareAccumulation(UInt32 pixelCount);

    IRHIDevice*     m_Device  = nullptr;
    FRenderContext* m_Context = nullptr;

    // ── 场景 ──
    FRHIBufferHandle m_PositionBuffer;
    FRHIBufferHandle m_IndexBuffer;
    FRHIBufferHandle m_TriangleMaterialBuffer;
    FRHIBufferHandle m_MaterialBuffer;

    FRHIAccelStructHandle m_Blas;
    FRHIAccelStructHandle m_Tlas;

    UInt32 m_TriangleCount = 0;
    UInt32 m_VertexCount   = 0;
    UInt32 m_MaterialCount = 0;

    // ── 累加与回读 ──
    FRHIBufferHandle m_AccumBuffer;
    FRHIBufferHandle m_ZeroBuffer;
    FRHIBufferHandle m_ReadbackBuffer;

    UInt32 m_AccumPixelCapacity = 0;

    // ── 管线 ──
    FRHIDescSetLayoutHandle   m_SetLayout;
    FRHIDescriptorSetHandle   m_DescriptorSet;
    FRHIPipelineLayoutHandle  m_PipelineLayout;
    FRHIComputePipelineHandle m_Pipeline;
    FRHIShaderHandle          m_Shader;

    UInt32 m_LastDispatchCount = 0;
};

} // namespace Limx
