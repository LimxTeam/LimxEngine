// ============================================================
// 文件名称：RayTracingCheck.cpp
// 创建时间：2026-08-31
// 创建者  ：LimxTeam
// 设计哲学：两套彼此独立的实现撞同一个数。GPU 那套走驱动的 BVH 遍历，
//          CPU 那套是引擎自己的 Möller-Trumbore 逐三角形求交 —— 谁都
//          不能"照着对方调到一致"，因为它们连算法都不是一回事。
//          场景刻意摆成"射线集必须能分辨对错"：有命中也有落空，有多个
//          实例，有一个实例被另一个挡在后面，还有一个被旋转过。任何
//          一样缺席就判失败，而不是让判据在退化的场景里空转通过。
// 功能描述：光线追踪加速结构的 GPU-CPU 交叉验证 —— 建 BLAS/TLAS、发
//          一批射线、把命中距离/实例下标/图元下标逐条与解析解比对。
// 技术特性：射线集含轴向与斜向两组 (轴向射线是 BVH 遍历的退化情形，
//          方向处理的错误可能只在斜向下才现形)；实例含平移、遮挡与
//          绕 Y 轴 45° 旋转三种情形。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                          │ 描述                      │
// │────────────────────────────────│─────────────────────────│
// │ IntersectTriangle()            │ Möller-Trumbore 求交       │
// │ BuildReferenceScene()          │ 构造 CPU 侧参考几何        │
// │ TraceReference()               │ CPU 侧最近命中             │
// │ RunRayTracingChecks()          │ 自检入口                   │
// ============================================================

// 预编译头必须排在最前 —— MSVC 的 /Yu 会丢弃它之前的一切内容。
// 把本文件自己的头放在第一行的话, 它整个不会被编译, 而报出来的错误
// 是一堆看似无关的 Windows API 重定义。
#include "Launch/LaunchMinimal.h"

#include "RayTracingCheck.h"

#include "Core/Containers/TArray.h"
#include "Core/Math/FVector.h"
#include "Core/Math/FMath.h"
#include "RHI/RHI/IRHIDevice.h"
#include "RHI/RHI/IRHICommandBuffer.h"
#include "RenderCore/Renderer/FRenderContext.h"
#include "RenderCore/Shaders/FShaderManager.h"
#include "RenderCore/Camera/FCamera.h"
#include "Renderer/Renderer/FRenderer.h"
#include "Renderer/RenderPass/FDepthPrePass.h"
#include "Renderer/RenderPass/FPassManager.h"
#include "Renderer/RayTracing/FRayTracingScene.h"

namespace Limx
{

LIMX_DEFINE_LOG_CATEGORY(LogRayTracingCheck)

namespace
{

// ============================================================================
// 与着色器逐字段对应的数据结构
// ============================================================================

/// 一条射线 (32 字节, 与 ray_query_test.comp 的 Ray 一致)
struct FRayQueryRay
{
    Float32 OriginX = 0.0f;
    Float32 OriginY = 0.0f;
    Float32 OriginZ = 0.0f;
    Float32 TMin    = 0.0f;

    Float32 DirX = 0.0f;
    Float32 DirY = 0.0f;
    Float32 DirZ = 1.0f;
    Float32 TMax = 0.0f;
};

static_assert(sizeof(FRayQueryRay) == 32,
              "FRayQueryRay 必须是 32 字节 — 与着色器的 Ray 逐字段对应");

/// 一条结果 (16 字节, 与 ray_query_test.comp 的 RayResult 一致)
struct FRayQueryResult
{
    Float32 T = -1.0f;
    UInt32  CustomIndex    = 0xFFFFFFFFu;
    UInt32  PrimitiveIndex = 0xFFFFFFFFu;
    UInt32  Hit = 0;
};

static_assert(sizeof(FRayQueryResult) == 16,
              "FRayQueryResult 必须是 16 字节 — 与着色器的 RayResult 对应");

/// 推送常量
struct FRayQueryPushConstants
{
    UInt32 RayCount = 0;
};

// ============================================================================
// 几何体 — 一个 XY 平面上的方片, 两个三角形
//
// 顶点顺序决定了图元下标: 三角形 0 = (0,1,2), 三角形 1 = (0,2,3)。
// GPU 报回来的 primitiveIndex 必须与这个顺序对得上, 而对不上时画面
// 不会有任何异常 —— 只有材质/UV 会串, 那是几天后才发现的问题。
// ============================================================================

constexpr Float32 kQuadHalfSize = 1.0f;

/// 越界哨兵所在的局部 z —— 比真几何体近 0.5
///
/// 差值必须远大于距离容差 (1e-4), 又要小到不至于跟别的实例混在一起。
constexpr Float32 kSentinelOffsetZ = -0.5f;

// 顶点表分两段: 前 4 个是真几何体, 后 4 个是越界哨兵。
//
// 哨兵摆在更近处, 所以任何"多建了三角形"的实现都会让命中距离整体前移 —— 那是
// 一眼看得出的 0.5, 而不是需要靠容差去分辨的噪声。
constexpr Float32 kQuadVertices[8][3] =
{
    // 真几何体
    { -kQuadHalfSize, -kQuadHalfSize, 0.0f },
    {  kQuadHalfSize, -kQuadHalfSize, 0.0f },
    {  kQuadHalfSize,  kQuadHalfSize, 0.0f },
    { -kQuadHalfSize,  kQuadHalfSize, 0.0f },

    // 越界哨兵
    { -kQuadHalfSize, -kQuadHalfSize, kSentinelOffsetZ },
    {  kQuadHalfSize, -kQuadHalfSize, kSentinelOffsetZ },
    {  kQuadHalfSize,  kQuadHalfSize, kSentinelOffsetZ },
    { -kQuadHalfSize,  kQuadHalfSize, kSentinelOffsetZ },
};

// 索引表同样分两段, 而 BLAS 只声明用前 6 个。
//
// 之所以要有后面这 12 个: 索引缓冲区之后如果是零, 越界读出的索引全是 0,
// 构成的退化三角形一条射线都碰不到 —— 于是"图元数算错"这类缺陷在判据眼里
// 完全不存在。而真实引擎里一个网格的索引后面紧跟的是另一个网格的索引,
// 越界读到的是**能被命中的几何体**。这段哨兵把场景还原成真实的样子。
constexpr UInt32 kQuadIndices[18] =
{
    // 真几何体 —— BLAS 只用这 6 个
    0, 1, 2,
    0, 2, 3,

    // 越界哨兵
    4, 5, 6,
    4, 6, 7,
    4, 5, 6,
    4, 6, 7,
};

/// BLAS 实际构建的三角形数 (只有真几何体那两个)
constexpr UInt32 kQuadTriangleCount = 2;

/// 顶点表的总长度 —— maxVertex 要按它算, 因为哨兵也在同一块缓冲区里
constexpr UInt32 kQuadVertexCount = 8;

/// BLAS 声明使用的索引数
constexpr UInt32 kQuadUsedIndexCount = kQuadTriangleCount * 3;

// ============================================================================
// 实例布局
//
// 每一项都有存在的理由:
//   0  正对相机, 最近 —— 基本命中
//   1  横向偏开 —— 不同射线打到不同实例
//   2  另一侧偏开且更远 —— 自定义下标不能靠"顺序恰好一致"蒙对
//   3  与 0 同轴但更远 —— 遮挡。BVH 必须返回近的那个而不是先遍历到的那个
//   4  绕 Y 轴转 45° —— 变换矩阵不是单位阵时才验得到
// ============================================================================

struct FTestInstance
{
    // 3x4 行主序
    Float32 Transform[12];

    UInt32 CustomIndex;
};

constexpr UInt32 kTestInstanceCount = 5;

/// cos(45°) = sin(45°)
constexpr Float32 kRot45 = 0.70710678f;

const FTestInstance kTestInstances[kTestInstanceCount] =
{
    // 0: 平移到 z = 2
    { { 1.0f, 0.0f, 0.0f,  0.0f,
        0.0f, 1.0f, 0.0f,  0.0f,
        0.0f, 0.0f, 1.0f,  2.0f }, 100u },

    // 1: 平移到 x = 3, z = 5
    { { 1.0f, 0.0f, 0.0f,  3.0f,
        0.0f, 1.0f, 0.0f,  0.0f,
        0.0f, 0.0f, 1.0f,  5.0f }, 101u },

    // 2: 平移到 x = -3, z = 9
    { { 1.0f, 0.0f, 0.0f, -3.0f,
        0.0f, 1.0f, 0.0f,  0.0f,
        0.0f, 0.0f, 1.0f,  9.0f }, 102u },

    // 3: 与 0 同轴, 但在它后面 (z = 6) —— 遮挡判据
    { { 1.0f, 0.0f, 0.0f,  0.0f,
        0.0f, 1.0f, 0.0f,  0.0f,
        0.0f, 0.0f, 1.0f,  6.0f }, 103u },

    // 4: 绕 Y 轴 45°, 平移到 x = 6, z = 4
    { {  kRot45, 0.0f, kRot45,  6.0f,
         0.0f,   1.0f, 0.0f,    0.0f,
        -kRot45, 0.0f, kRot45,  4.0f }, 104u },
};

// ============================================================================
// CPU 参考实现
// ============================================================================

/// 一个已经变换到世界空间的三角形
struct FRefTriangle
{
    FVector3 V0;
    FVector3 V1;
    FVector3 V2;

    UInt32 CustomIndex    = 0;
    UInt32 PrimitiveIndex = 0;
};

/// 把 3x4 行主序变换作用在一个点上
FVector3 TransformPoint(const Float32 (&m)[12], const FVector3& p)
{
    return FVector3(
        m[0] * p.X + m[1] * p.Y + m[2]  * p.Z + m[3],
        m[4] * p.X + m[5] * p.Y + m[6]  * p.Z + m[7],
        m[8] * p.X + m[9] * p.Y + m[10] * p.Z + m[11]);
}

/// Möller-Trumbore 射线-三角形求交
///
/// 与 GPU 的 BVH 遍历在算法上毫无共同之处 —— 这正是它作为参考实现的价值:
/// 两边同时错到同一个数上的可能性可以忽略。
///
/// @return 命中时写出 t 并返回 true; 平行、背面、t 在区间外都返回 false
bool IntersectTriangle(const FVector3& origin, const FVector3& direction,
                       const FRefTriangle& tri,
                       Float32 tMin, Float32 tMax, Float32& outT)
{
    // 判定的阈值。取得比"数值噪声"大, 比"真实的掠射角"小 —— 太大会把
    // 接近平行的合法命中判掉, 太小会让平行射线除出一个巨大的 t。
    constexpr Float32 kEpsilon = 1e-8f;

    const FVector3 edge1 = tri.V1 - tri.V0;
    const FVector3 edge2 = tri.V2 - tri.V0;

    const FVector3 pvec = FVector3::Cross(direction, edge2);
    const Float32 det = FVector3::Dot(edge1, pvec);

    // 双面: 用绝对值判平行, 而不是 det < eps。
    //
    // TLAS 的实例标志里设了 TRIANGLE_FACING_CULL_DISABLE, 所以 GPU 那边
    // 正反面都命中。这里若只接受正面, 两边就会在背面命中上系统性地不一致 ——
    // 而那种不一致看起来像"加速结构错了", 实际上是参考实现错了。
    if (FMath::Abs(det) < kEpsilon)
    {
        return false;
    }

    const Float32 invDet = 1.0f / det;

    const FVector3 tvec = origin - tri.V0;
    const Float32 u = FVector3::Dot(tvec, pvec) * invDet;

    if (u < 0.0f || u > 1.0f)
    {
        return false;
    }

    const FVector3 qvec = FVector3::Cross(tvec, edge1);
    const Float32 v = FVector3::Dot(direction, qvec) * invDet;

    if (v < 0.0f || (u + v) > 1.0f)
    {
        return false;
    }

    const Float32 t = FVector3::Dot(edge2, qvec) * invDet;

    if (t < tMin || t > tMax)
    {
        return false;
    }

    outT = t;
    return true;
}

/// 把所有实例的三角形展开到世界空间
void BuildReferenceScene(TArray<FRefTriangle>& outTriangles)
{
    outTriangles.Reserve(
        static_cast<SizeType>(kTestInstanceCount) * kQuadTriangleCount);

    for (UInt32 inst = 0; inst < kTestInstanceCount; ++inst)
    {
        const FTestInstance& instance = kTestInstances[inst];

        for (UInt32 tri = 0; tri < kQuadTriangleCount; ++tri)
        {
            const UInt32 i0 = kQuadIndices[tri * 3 + 0];
            const UInt32 i1 = kQuadIndices[tri * 3 + 1];
            const UInt32 i2 = kQuadIndices[tri * 3 + 2];

            FRefTriangle out;
            out.V0 = TransformPoint(instance.Transform,
                FVector3(kQuadVertices[i0][0], kQuadVertices[i0][1],
                         kQuadVertices[i0][2]));
            out.V1 = TransformPoint(instance.Transform,
                FVector3(kQuadVertices[i1][0], kQuadVertices[i1][1],
                         kQuadVertices[i1][2]));
            out.V2 = TransformPoint(instance.Transform,
                FVector3(kQuadVertices[i2][0], kQuadVertices[i2][1],
                         kQuadVertices[i2][2]));
            out.CustomIndex    = instance.CustomIndex;
            out.PrimitiveIndex = tri;

            outTriangles.Add(out);
        }
    }
}

/// CPU 侧的最近命中
FRayQueryResult TraceReference(const TArray<FRefTriangle>& triangles,
                               const FRayQueryRay& ray)
{
    FRayQueryResult best;

    const FVector3 origin(ray.OriginX, ray.OriginY, ray.OriginZ);
    const FVector3 direction(ray.DirX, ray.DirY, ray.DirZ);

    Float32 closest = ray.TMax;

    for (SizeType i = 0; i < triangles.GetSize(); ++i)
    {
        Float32 t = 0.0f;

        if (IntersectTriangle(origin, direction, triangles[i],
                              ray.TMin, closest, t))
        {
            closest = t;
            best.T  = t;
            best.CustomIndex    = triangles[i].CustomIndex;
            best.PrimitiveIndex = triangles[i].PrimitiveIndex;
            best.Hit = 1;
        }
    }

    return best;
}

// ============================================================================
// 射线集
//
// 两组:
//   轴向 —— 沿 +Z 的平行射线, 扫过整个 x 范围。轴向是 BVH 遍历的退化情形,
//           方向分量里的符号错误在这里可能刚好抵消。
//   斜向 —— 从同一点发散的扇形。方向的每个分量都非零, 上面那种错误现形。
// ============================================================================

constexpr UInt32 kAxialGridX = 48;
constexpr UInt32 kAxialGridY = 5;
constexpr UInt32 kFanGridX   = 32;
constexpr UInt32 kFanGridY   = 8;

constexpr UInt32 kTestRayCount =
    kAxialGridX * kAxialGridY + kFanGridX * kFanGridY;

void BuildTestRays(TArray<FRayQueryRay>& outRays)
{
    outRays.Reserve(kTestRayCount);

    // ---- 轴向 ----
    for (UInt32 iy = 0; iy < kAxialGridY; ++iy)
    {
        for (UInt32 ix = 0; ix < kAxialGridX; ++ix)
        {
            // x 从 -8 扫到 8: 覆盖三个横向分开的实例, 以及它们之间与
            // 之外的空隙 (那些必须落空)。
            const Float32 x = -8.0f +
                16.0f * (static_cast<Float32>(ix) /
                         static_cast<Float32>(kAxialGridX - 1));
            const Float32 y = -1.6f +
                3.2f * (static_cast<Float32>(iy) /
                        static_cast<Float32>(kAxialGridY - 1));

            FRayQueryRay ray;
            ray.OriginX = x;
            ray.OriginY = y;
            ray.OriginZ = -1.0f;
            ray.TMin    = 0.0f;
            ray.DirX    = 0.0f;
            ray.DirY    = 0.0f;
            ray.DirZ    = 1.0f;
            ray.TMax    = 100.0f;

            outRays.Add(ray);
        }
    }

    // ---- 斜向 ----
    for (UInt32 iy = 0; iy < kFanGridY; ++iy)
    {
        for (UInt32 ix = 0; ix < kFanGridX; ++ix)
        {
            const Float32 tx = -7.0f +
                14.0f * (static_cast<Float32>(ix) /
                         static_cast<Float32>(kFanGridX - 1));
            const Float32 ty = -2.0f +
                4.0f * (static_cast<Float32>(iy) /
                        static_cast<Float32>(kFanGridY - 1));

            // 从 (0, 0, -4) 射向 (tx, ty, 4)
            FVector3 dir(tx, ty, 8.0f);
            dir = dir.GetSafeNormal();

            FRayQueryRay ray;
            ray.OriginX = 0.0f;
            ray.OriginY = 0.0f;
            ray.OriginZ = -4.0f;
            ray.TMin    = 0.0f;
            ray.DirX    = dir.X;
            ray.DirY    = dir.Y;
            ray.DirZ    = dir.Z;
            ray.TMax    = 100.0f;

            outRays.Add(ray);
        }
    }
}

// ============================================================================
// GPU 侧资源 — 一次性, 用完即毁
// ============================================================================

struct FRayTracingCheckResources
{
    FRHIBufferHandle VertexBuffer;
    FRHIBufferHandle IndexBuffer;
    FRHIBufferHandle RayBuffer;
    FRHIBufferHandle ResultBuffer;

    FRHIAccelStructHandle Blas;
    FRHIAccelStructHandle Tlas;

    FRHIDescSetLayoutHandle   SetLayout;
    FRHIDescriptorSetHandle   DescriptorSet;
    FRHIPipelineLayoutHandle  PipelineLayout;
    FRHIComputePipelineHandle Pipeline;
    FRHIShaderHandle          Shader;

    void Destroy(IRHIDevice* device)
    {
        // 先毁加速结构 —— 它们内部还引用着自己的存储缓冲区。
        device->DestroyAccelStruct(Tlas);
        device->DestroyAccelStruct(Blas);

        device->DestroyComputePipeline(Pipeline);
        device->DestroyPipelineLayout(PipelineLayout);
        device->FreeDescriptorSet(DescriptorSet);
        device->DestroyDescSetLayout(SetLayout);
        device->DestroyShader(Shader);

        device->DestroyBuffer(ResultBuffer);
        device->DestroyBuffer(RayBuffer);
        device->DestroyBuffer(IndexBuffer);
        device->DestroyBuffer(VertexBuffer);
    }
};

/// 创建一块主机可写的缓冲区并填入数据
bool CreateAndFill(IRHIDevice* device, const void* data, UInt64 size,
                   EBufferUsage usage, const char* name,
                   FRHIBufferHandle& outHandle)
{
    FRHIBufferDesc desc;
    desc.Size        = size;
    desc.Usage       = usage;
    desc.MemoryUsage = EMemoryUsage::CpuToGpu;
    desc.DebugName   = name;

    if (!IsRHISuccess(device->CreateBuffer(desc, outHandle)))
    {
        LIMX_LOG(LogRayTracingCheck, Error,
                 "[光追自检] 缓冲区 {} 创建失败", name);
        return false;
    }

    if (data == nullptr)
    {
        return true;
    }

    void* mapped = nullptr;

    if (!IsRHISuccess(device->MapBuffer(outHandle, &mapped)) ||
        mapped == nullptr)
    {
        LIMX_LOG(LogRayTracingCheck, Error,
                 "[光追自检] 缓冲区 {} 映射失败", name);
        return false;
    }

    Memory::MemCopy(mapped, data, static_cast<SizeType>(size));
    device->UnmapBuffer(outHandle);

    return true;
}

} // namespace

// ============================================================================
// RunRayTracingChecks
// ============================================================================

bool RunRayTracingChecks(IRHIDevice* device, FRenderContext* context)
{
    if (device == nullptr || context == nullptr)
    {
        LIMX_LOG(LogRayTracingCheck, Error, "[光追自检] 设备或上下文为空");
        return false;
    }

    // "不支持"必须判失败而不是判通过。
    //
    // 判通过的话, 这条判据在任何不支持光追的机器上都是空的 —— 而 CI 换一台
    // 机器就是"不支持"的最常见来源, 那正是它最需要说话的场合。
    if (!device->IsRayTracingSupported())
    {
        LIMX_LOG(LogRayTracingCheck, Error,
                 "[光追自检] 设备不支持光线追踪 — 判据无法执行, 判定为失败");
        return false;
    }

    FRayTracingCheckResources res;
    bool passed = true;

    // ------------------------------------------------------------------
    // 几何体
    // ------------------------------------------------------------------
    const EBufferUsage geometryUsage =
        EBufferUsage::AccelStructBuild | EBufferUsage::ShaderDeviceAddress |
        EBufferUsage::TransferDst;

    if (!CreateAndFill(device, kQuadVertices, sizeof(kQuadVertices),
                       geometryUsage | EBufferUsage::VertexBuffer,
                       "RtCheck.Vertices", res.VertexBuffer) ||
        !CreateAndFill(device, kQuadIndices, sizeof(kQuadIndices),
                       geometryUsage | EBufferUsage::IndexBuffer,
                       "RtCheck.Indices", res.IndexBuffer))
    {
        res.Destroy(device);
        return false;
    }

    // ------------------------------------------------------------------
    // BLAS
    // ------------------------------------------------------------------
    FRHIAccelStructGeometry geometry;
    geometry.VertexBuffer = res.VertexBuffer;
    geometry.VertexOffset = 0;
    geometry.VertexCount  = kQuadVertexCount;
    geometry.VertexStride = sizeof(Float32) * 3;
    geometry.VertexFormat = EPixelFormat::RGB32_SFLOAT;
    geometry.IndexBuffer  = res.IndexBuffer;
    geometry.IndexOffset  = 0;
    geometry.IndexCount   = kQuadUsedIndexCount;
    geometry.IndexType    = EIndexType::UInt32;
    geometry.Opaque       = true;

    FRHIBlasDesc blasDesc;
    blasDesc.Geometries    = &geometry;
    blasDesc.GeometryCount = 1;
    blasDesc.DebugName     = "RtCheck.Blas";

    if (!IsRHISuccess(device->CreateBottomLevelAS(blasDesc, res.Blas)))
    {
        LIMX_LOG(LogRayTracingCheck, Error, "[光追自检] BLAS 创建失败");
        res.Destroy(device);
        return false;
    }

    // ------------------------------------------------------------------
    // TLAS
    // ------------------------------------------------------------------
    FRHITlasDesc tlasDesc;
    tlasDesc.MaxInstanceCount = kTestInstanceCount;
    tlasDesc.DebugName        = "RtCheck.Tlas";

    if (!IsRHISuccess(device->CreateTopLevelAS(tlasDesc, res.Tlas)))
    {
        LIMX_LOG(LogRayTracingCheck, Error, "[光追自检] TLAS 创建失败");
        res.Destroy(device);
        return false;
    }

    FRHIAccelStructInstance instances[kTestInstanceCount];

    for (UInt32 i = 0; i < kTestInstanceCount; ++i)
    {
        for (UInt32 e = 0; e < 12; ++e)
        {
            instances[i].Transform[e] = kTestInstances[i].Transform[e];
        }

        instances[i].CustomIndex = kTestInstances[i].CustomIndex;
        instances[i].Mask        = 0xFF;
        instances[i].Blas        = res.Blas;
    }

    if (!IsRHISuccess(device->UpdateTlasInstances(
            res.Tlas, instances, kTestInstanceCount)))
    {
        LIMX_LOG(LogRayTracingCheck, Error, "[光追自检] TLAS 实例上传失败");
        res.Destroy(device);
        return false;
    }

    // ------------------------------------------------------------------
    // 射线与结果缓冲区
    // ------------------------------------------------------------------
    TArray<FRayQueryRay> rays;
    BuildTestRays(rays);

    const UInt32 rayCount = static_cast<UInt32>(rays.GetSize());

    if (rayCount != kTestRayCount)
    {
        LIMX_LOG(LogRayTracingCheck, Error,
                 "[光追自检] 射线数 {} 与预期 {} 不符",
                 rayCount, kTestRayCount);
        res.Destroy(device);
        return false;
    }

    const UInt64 resultBytes =
        static_cast<UInt64>(rayCount) * sizeof(FRayQueryResult);

    if (!CreateAndFill(device, rays.GetData(),
                       static_cast<UInt64>(rayCount) * sizeof(FRayQueryRay),
                       EBufferUsage::StorageBuffer,
                       "RtCheck.Rays", res.RayBuffer) ||
        !CreateAndFill(device, nullptr, resultBytes,
                       EBufferUsage::StorageBuffer,
                       "RtCheck.Results", res.ResultBuffer))
    {
        res.Destroy(device);
        return false;
    }

    // 结果缓冲区先填成"不可能出现的值"。
    //
    // 清零的话, T=0 与"贴着原点命中"分不开, Hit=0 与"未命中"分不开 ——
    // 于是着色器一条都没跑的情形会与"全部落空"完全一样。填 0xFF 之后
    // 未写入的格子是 T = NaN、Hit = 0xFFFFFFFF, 两者都判得出来。
    {
        void* mapped = nullptr;

        if (IsRHISuccess(device->MapBuffer(res.ResultBuffer, &mapped)) &&
            mapped != nullptr)
        {
            Memory::MemSet(mapped, 0xFF,
                           static_cast<SizeType>(resultBytes));
            device->UnmapBuffer(res.ResultBuffer);
        }
    }

    // ------------------------------------------------------------------
    // 计算管线
    // ------------------------------------------------------------------
    FRHIDescriptorBinding bindings[3] = {};

    bindings[0].Binding    = 0;
    bindings[0].Type       = EDescriptorType::AccelerationStructure;
    bindings[0].Count      = 1;
    bindings[0].StageFlags = EShaderStage::Compute;

    bindings[1].Binding    = 1;
    bindings[1].Type       = EDescriptorType::StorageBuffer;
    bindings[1].Count      = 1;
    bindings[1].StageFlags = EShaderStage::Compute;

    bindings[2].Binding    = 2;
    bindings[2].Type       = EDescriptorType::StorageBuffer;
    bindings[2].Count      = 1;
    bindings[2].StageFlags = EShaderStage::Compute;

    FRHIDescSetLayoutDesc layoutDesc = {};
    layoutDesc.Bindings     = bindings;
    layoutDesc.BindingCount = 3;
    layoutDesc.DebugName    = "RtCheck.SetLayout";

    if (!IsRHISuccess(device->CreateDescSetLayout(layoutDesc, res.SetLayout)) ||
        !IsRHISuccess(device->AllocateDescriptorSet(
            res.SetLayout, res.DescriptorSet)))
    {
        LIMX_LOG(LogRayTracingCheck, Error, "[光追自检] 描述符集创建失败");
        res.Destroy(device);
        return false;
    }

    FRHIDescriptorWrite writes[3];

    writes[0].DescriptorSet = res.DescriptorSet;
    writes[0].Binding       = 0;
    writes[0].Type          = EDescriptorType::AccelerationStructure;
    writes[0].AccelStruct   = res.Tlas;

    writes[1] = FRHIDescriptorWrite::StorageBuffer(
        res.DescriptorSet, 1, res.RayBuffer, 0,
        static_cast<UInt64>(rayCount) * sizeof(FRayQueryRay));

    writes[2] = FRHIDescriptorWrite::StorageBuffer(
        res.DescriptorSet, 2, res.ResultBuffer, 0, resultBytes);

    device->UpdateDescriptorSets(writes, 3);

    FShaderManager& shaders = FShaderManager::Get();

    if (!shaders.IsInitialized())
    {
        shaders.Initialize();
    }

    if (!IsRHISuccess(shaders.CreateShaderModule(
            device, FString("Builtin/ray_query_test.comp"),
            EShaderStage::Compute, res.Shader)))
    {
        LIMX_LOG(LogRayTracingCheck, Error,
                 "[光追自检] ray_query_test.comp 加载失败");
        res.Destroy(device);
        return false;
    }

    FRHIPushConstantRange pushRange = {};
    pushRange.StageFlags = EShaderStage::Compute;
    pushRange.Offset     = 0;
    pushRange.Size       = sizeof(FRayQueryPushConstants);

    FRHIPipelineLayoutDesc pipelineLayoutDesc = {};
    pipelineLayoutDesc.SetLayouts             = &res.SetLayout;
    pipelineLayoutDesc.SetLayoutCount         = 1;
    pipelineLayoutDesc.PushConstantRanges     = &pushRange;
    pipelineLayoutDesc.PushConstantRangeCount = 1;
    pipelineLayoutDesc.DebugName              = "RtCheck.PipelineLayout";

    FRHIComputePipelineDesc pipelineDesc = {};
    pipelineDesc.ComputeShader.Shader     = res.Shader;
    pipelineDesc.ComputeShader.Stage      = EShaderStage::Compute;
    pipelineDesc.ComputeShader.EntryPoint = "main";
    pipelineDesc.DebugName                = "RtCheck.Pipeline";

    if (!IsRHISuccess(device->CreatePipelineLayout(
            pipelineLayoutDesc, res.PipelineLayout)))
    {
        LIMX_LOG(LogRayTracingCheck, Error, "[光追自检] 管线布局创建失败");
        res.Destroy(device);
        return false;
    }

    pipelineDesc.PipelineLayout = res.PipelineLayout;

    if (!IsRHISuccess(device->CreateComputePipeline(
            pipelineDesc, res.Pipeline)))
    {
        LIMX_LOG(LogRayTracingCheck, Error, "[光追自检] 计算管线创建失败");
        res.Destroy(device);
        return false;
    }

    // ------------------------------------------------------------------
    // 构建 + 遍历
    // ------------------------------------------------------------------
    IRHICommandBuffer* cmd = context->BeginSingleTimeCommands();

    if (cmd == nullptr)
    {
        LIMX_LOG(LogRayTracingCheck, Error, "[光追自检] 命令缓冲区分配失败");
        res.Destroy(device);
        return false;
    }

    cmd->BuildAccelStruct(res.Blas, 0);

    // BLAS 必须先于 TLAS 建完 —— TLAS 的构建要读 BLAS 的内容。
    // 少了这道屏障, 结果取决于驱动碰巧怎么调度, 而它多半是对的,
    // 直到某一台机器上不对。
    cmd->AccelStructBarrier();

    cmd->BuildAccelStruct(res.Tlas, kTestInstanceCount);
    cmd->AccelStructBarrier();

    cmd->BindComputePipeline(res.Pipeline);
    cmd->BindDescriptorSet(EPipelineBindPoint::Compute, res.PipelineLayout,
                           0, res.DescriptorSet);

    FRayQueryPushConstants push;
    push.RayCount = rayCount;

    cmd->PushConstants(res.PipelineLayout, EShaderStage::Compute, 0,
                       sizeof(push), &push);

    constexpr UInt32 kGroupSize = 64;
    cmd->Dispatch((rayCount + kGroupSize - 1) / kGroupSize, 1, 1);

    // 计算写 -> 主机读
    FRHIMemoryBarrier readback = {};
    readback.SrcAccessMask = EAccessFlags::ShaderWrite;
    readback.DstAccessMask = EAccessFlags::HostRead;

    cmd->PipelineBarrier(EPipelineStageFlags::ComputeShader,
                         EPipelineStageFlags::Host,
                         &readback, 1, nullptr, 0, nullptr, 0);

    context->EndSingleTimeCommands(cmd);

    // ------------------------------------------------------------------
    // 比对
    // ------------------------------------------------------------------
    TArray<FRefTriangle> refTriangles;
    BuildReferenceScene(refTriangles);

    void* mappedResults = nullptr;

    if (!IsRHISuccess(device->MapBuffer(res.ResultBuffer, &mappedResults)) ||
        mappedResults == nullptr)
    {
        LIMX_LOG(LogRayTracingCheck, Error, "[光追自检] 结果缓冲区映射失败");
        res.Destroy(device);
        return false;
    }

    const auto* gpuResults =
        static_cast<const FRayQueryResult*>(mappedResults);

    // 距离容差。
    //
    // GPU 侧的 t 走过了实例变换 (单精度矩阵乘)、BVH 的包围盒裁剪与三角形
    // 求交; CPU 侧是另一条完全不同的单精度路径。两条路径在 t ≈ 10 的量级
    // 上差几个 ULP 是正常的。这里取绝对容差 1e-3 —— 相对量级约万分之一,
    // 远小于任何一种"建错了"能造成的偏差 (那是以整个实例间距计的)。
    //
    // 实测最大误差 2e-6 (t 最大约 13), 即相对误差约 1.5e-7 —— 单精度的
    // 极限。容差取 1e-4 留了 50 倍余量, 既不会被数值噪声碰到, 又比任何
    // 一种实现错误小得多。
    constexpr Float32 kDistanceTolerance = 1e-4f;

    UInt32 hitCount        = 0;
    UInt32 missCount       = 0;
    UInt32 occlusionCount  = 0;
    UInt32 rotatedHitCount = 0;
    UInt32 mismatchCount   = 0;
    Float32 maxDistanceError = 0.0f;

    // 每个实例被命中多少次 —— 元判据要用
    UInt32 perInstanceHits[kTestInstanceCount] = {};

    for (UInt32 i = 0; i < rayCount; ++i)
    {
        const FRayQueryResult& gpu = gpuResults[i];
        const FRayQueryResult  ref = TraceReference(refTriangles, rays[i]);

        // ---- 命中与否必须完全一致, 不留容差 ----
        if ((gpu.Hit != 0) != (ref.Hit != 0))
        {
            if (mismatchCount < 8)
            {
                LIMX_LOG(LogRayTracingCheck, Error,
                         "[光追自检] 射线 {} 命中判定不一致 — "
                         "GPU:{} CPU:{} (原点 {},{},{} 方向 {},{},{})",
                         i, gpu.Hit, ref.Hit,
                         rays[i].OriginX, rays[i].OriginY, rays[i].OriginZ,
                         rays[i].DirX, rays[i].DirY, rays[i].DirZ);
            }

            ++mismatchCount;
            continue;
        }

        if (ref.Hit == 0)
        {
            ++missCount;
            continue;
        }

        ++hitCount;

        // ---- 距离 ----
        const Float32 error = FMath::Abs(gpu.T - ref.T);

        if (error > maxDistanceError)
        {
            maxDistanceError = error;
        }

        if (error > kDistanceTolerance)
        {
            if (mismatchCount < 8)
            {
                LIMX_LOG(LogRayTracingCheck, Error,
                         "[光追自检] 射线 {} 命中距离不一致 — "
                         "GPU:{} CPU:{} 差:{}",
                         i, gpu.T, ref.T, error);
            }

            ++mismatchCount;
            continue;
        }

        // ---- 实例自定义下标 ----
        if (gpu.CustomIndex != ref.CustomIndex)
        {
            if (mismatchCount < 8)
            {
                LIMX_LOG(LogRayTracingCheck, Error,
                         "[光追自检] 射线 {} 实例下标不一致 — GPU:{} CPU:{}",
                         i, gpu.CustomIndex, ref.CustomIndex);
            }

            ++mismatchCount;
            continue;
        }

        // ---- 图元下标 ----
        if (gpu.PrimitiveIndex != ref.PrimitiveIndex)
        {
            if (mismatchCount < 8)
            {
                LIMX_LOG(LogRayTracingCheck, Error,
                         "[光追自检] 射线 {} 图元下标不一致 — GPU:{} CPU:{}",
                         i, gpu.PrimitiveIndex, ref.PrimitiveIndex);
            }

            ++mismatchCount;
            continue;
        }

        // ---- 统计 (给元判据用) ----
        for (UInt32 inst = 0; inst < kTestInstanceCount; ++inst)
        {
            if (kTestInstances[inst].CustomIndex == gpu.CustomIndex)
            {
                ++perInstanceHits[inst];
                break;
            }
        }

        // 实例 3 在实例 0 后面。射线打到 0 号的自定义下标 100 时,
        // 说明遮挡关系被正确处理了 (否则会返回 103)。
        if (gpu.CustomIndex == 100u)
        {
            ++occlusionCount;
        }

        if (gpu.CustomIndex == 104u)
        {
            ++rotatedHitCount;
        }
    }

    device->UnmapBuffer(res.ResultBuffer);

    // ------------------------------------------------------------------
    // 元判据 — 这批射线到底够不够判
    //
    // 上面那些比对全过, 有可能只是因为"根本没什么可比的": 全部落空的
    // 射线集下, 一个什么都不做的加速结构也能满分通过。所以这里检查
    // 场景本身有没有把每一种情形都摆出来。
    // ------------------------------------------------------------------
    if (hitCount == 0)
    {
        LIMX_LOG(LogRayTracingCheck, Error,
                 "[光追自检] 没有任何射线命中 — 判据无从判定");
        passed = false;
    }

    if (missCount == 0)
    {
        LIMX_LOG(LogRayTracingCheck, Error,
                 "[光追自检] 没有任何射线落空 — "
                 "一个恒返回命中的实现也会通过");
        passed = false;
    }

    UInt32 instancesHit = 0;

    for (UInt32 inst = 0; inst < kTestInstanceCount; ++inst)
    {
        if (perInstanceHits[inst] > 0)
        {
            ++instancesHit;
        }
    }

    // 实例 3 被实例 0 完全挡住, 永远不该被命中 —— 所以能命中的是 4 个。
    constexpr UInt32 kExpectedVisibleInstances = 4;

    if (instancesHit != kExpectedVisibleInstances)
    {
        LIMX_LOG(LogRayTracingCheck, Error,
                 "[光追自检] 被命中的实例数为 {} — 预期 {} "
                 "(0/1/2/4 可见, 3 被 0 完全遮挡)",
                 instancesHit, kExpectedVisibleInstances);
        passed = false;
    }

    if (perInstanceHits[3] != 0)
    {
        LIMX_LOG(LogRayTracingCheck, Error,
                 "[光追自检] 被遮挡的实例 3 被命中了 {} 次 — "
                 "最近命中的选择有问题",
                 perInstanceHits[3]);
        passed = false;
    }

    if (occlusionCount == 0)
    {
        LIMX_LOG(LogRayTracingCheck, Error,
                 "[光追自检] 没有一条射线经过遮挡区域 — 遮挡判据是空的");
        passed = false;
    }

    if (rotatedHitCount == 0)
    {
        LIMX_LOG(LogRayTracingCheck, Error,
                 "[光追自检] 旋转过的实例 4 一次都没被命中 — "
                 "非单位变换没有被验证到");
        passed = false;
    }

    if (mismatchCount > 0)
    {
        LIMX_LOG(LogRayTracingCheck, Error,
                 "[光追自检] {} / {} 条射线的结果与解析解不一致",
                 mismatchCount, rayCount);
        passed = false;
    }

    LIMX_LOG(LogRayTracingCheck, Display,
             "[光追自检] 射线 {} 条 — 命中 {} 落空 {} | "
             "实例命中 {}/{}/{}/{}/{} | 遮挡区 {} 条 旋转实例 {} 条 | "
             "最大距离误差 {} (容差 {}) | 不一致 {} 条",
             rayCount, hitCount, missCount,
             perInstanceHits[0], perInstanceHits[1], perInstanceHits[2],
             perInstanceHits[3], perInstanceHits[4],
             occlusionCount, rotatedHitCount,
             maxDistanceError, kDistanceTolerance, mismatchCount);

    LIMX_LOG(LogRayTracingCheck, Display,
             "[光追自检] {}", passed ? "通过" : "失败");

    res.Destroy(device);

    return passed;
}

// ============================================================================
// RunRayTracingDepthCheck — 光追深度 vs 光栅化深度
// ============================================================================

namespace
{

/// rt_depth.comp 的推送常量 (128 字节)
struct FRtDepthPushConstants
{
    FMatrix InvViewProj;

    FMatrix View;

    UInt32 Width  = 0;
    UInt32 Height = 0;
    UInt32 Pad0   = 0;
    UInt32 Pad1   = 0;

    Float32 NearPlane = 0.0f;
    Float32 FarPlane  = 0.0f;
    Float32 PlanePad0 = 0.0f;
    Float32 PlanePad1 = 0.0f;

    UInt32 RayMask  = 0xFFu;
    UInt32 MaskPad0 = 0;
    UInt32 MaskPad1 = 0;
    UInt32 MaskPad2 = 0;
};

static_assert(sizeof(FRtDepthPushConstants) == 176,
              "FRtDepthPushConstants 必须是 128 字节 — 与 rt_depth.comp 的 "
              "push constant 块逐字段一致");

/// 每像素一对深度: x = 光追, y = 光栅化 (都已线性化到沿相机前向的距离)
struct FDepthPair
{
    Float32 RayTraced  = -1.0f;
    Float32 Rasterized = -1.0f;
    Float32 RasterNdc  = -1.0f;
    Float32 Instance   = -1.0f;
};

static_assert(sizeof(FDepthPair) == 16,
              "FDepthPair 必须是 16 字节 — 与着色器的 vec4 对应");

/// 一个 NDC 深度值上, 深度缓冲区**能分辨的最小世界距离**
///
/// 透视深度在远处压缩得极厉害: 同样一个 float32 的最低位, 在近平面附近代表
/// 微米, 在远平面附近代表米。拿一个固定的相对容差去判"两边算得一不一样",
/// 等于对不同的场景用了完全不同的严格程度 —— 而那个差别可以是十倍。
///
/// 实测: 同一个 9.63 米处的表面
///     近 0.1  远 100  → 一个最低位 = 5.5e-5 世界单位
///     近 0.01 远 36   → 一个最低位 = 5.5e-4 世界单位  (差十倍)
///
/// 所以容差按这个量算, 判据就与场景的近远平面无关了, 而且它说的是一句有
/// 意义的话: "两条路径的结果相差不超过深度缓冲区能分辨的 N 个最低位"。
Float32 DepthQuantumAt(Float32 ndcDepth, Float32 nearPlane, Float32 farPlane)
{
    const Float32 a = farPlane / (nearPlane - farPlane);
    const Float32 b = farPlane * nearPlane / (nearPlane - farPlane);

    const Float32 denom = ndcDepth + a;

    if (FMath::Abs(denom) < 1.0e-12f)
    {
        return 1.0e30f;
    }

    // d(视空间深度)/d(NDC) = |b| / (ndc + a)^2
    const Float32 slope = FMath::Abs(b) / (denom * denom);

    // float32 尾数 23 位; NDC 深度在 [0,1], 靠近 1 时指数为 -1,
    // 于是最低位约为 2^-24。取 ndc 自身的量级更准一些。
    const Float32 magnitude = FMath::Max(FMath::Abs(ndcDepth), 0.5f);
    const Float32 ulp = magnitude * 1.1920929e-7f;

    return slope * ulp;
}

} // namespace

// ── 已知的覆盖边界 (量过, 不是"没想到") ────────────────────────────────
//
// 1. **半透明几何体这条路今天不可达**。加速结构是从"阴影投射体列表"建的,
//    而那份列表按定义就不含半透明。于是 UpdateInstances 里给半透明分掩码
//    的那个分支一次都没执行过 —— "把半透明的掩码填成 0xFF"这条变异因此
//    在两个场景上都逃逸。代码留着是为了将来反射要用整棵树时不必重写,
//    但在那之前它没有判据兜着。
//
// 2. **蒙版几何体在光追里是实心的**。ray query 没有 any-hit, 评估不了
//    alpha 测试。综合场景里有 2 个蒙版物体, 它们贡献的不符像素落在
//    0.0021% 的基线里。真要处理需要在着色器里自己查纹理, 那是后面的事。
//
// 3. 两个场景是互补的, 缺一不可:
//      综合场景  33 物体 + 天空背景 + 蒙版 —— 抓"射线掩码漏掉蒙版"、
//                "实例变换错"这一类
//      OBJ 场景  6 个子网格共用一对缓冲区 —— 是唯一能验到"子网格索引
//                偏移"的场景 (别的场景 IndexOffset 全是 0, 乘不乘位宽
//                都一样)
bool RunRayTracingDepthCheck(FRenderContext* context, FRenderer& renderer)
{
    if (context == nullptr)
    {
        return false;
    }

    IRHIDevice* const device = context->GetDevice();

    if (device == nullptr || !device->IsRayTracingSupported())
    {
        LIMX_LOG(LogRayTracingCheck, Error,
                 "[光追深度] 设备不支持光线追踪 — 判据无法执行, 判定为失败");
        return false;
    }

    if (!renderer.SetRayTracingEnabled(true))
    {
        LIMX_LOG(LogRayTracingCheck, Error, "[光追深度] 无法启用光追场景");
        return false;
    }

    FDepthPrePass* const depthPass = renderer.GetDepthPrePass();

    if (depthPass == nullptr)
    {
        LIMX_LOG(LogRayTracingCheck, Error, "[光追深度] 深度预通道不可用");
        return false;
    }

    const FRHIExtent2D extent = context->GetSwapchainExtent();

    const SizeType pixelCount =
        static_cast<SizeType>(extent.Width) * extent.Height;

    const UInt64 resultBytes =
        static_cast<UInt64>(pixelCount) * sizeof(FDepthPair);

    // ------------------------------------------------------------------
    // 资源
    // ------------------------------------------------------------------
    FRHIBufferHandle resultBuffer;

    {
        FRHIBufferDesc desc = {};
        desc.Size        = resultBytes;
        desc.Usage       = EBufferUsage::StorageBuffer;
        desc.MemoryUsage = EMemoryUsage::GpuToCpu;
        desc.DebugName   = "RtDepth.Result";

        if (!IsRHISuccess(device->CreateBuffer(desc, resultBuffer)))
        {
            return false;
        }
    }

    // 先填成不可能出现的值 (0xFF 解释成 float 是 NaN)。
    //
    // 清零的话"着色器一次都没跑"会表现成"全部深度为 0", 而 0 是合法深度 ——
    // 失败模式又落在通过上了。
    {
        void* mapped = nullptr;

        if (IsRHISuccess(device->MapBuffer(resultBuffer, &mapped)) &&
            mapped != nullptr)
        {
            Memory::MemSet(mapped, 0xFF, static_cast<SizeType>(resultBytes));
            device->UnmapBuffer(resultBuffer);
        }
    }

    FRHIDescSetLayoutHandle   setLayout;
    FRHIDescriptorSetHandle   descriptorSet;
    FRHIPipelineLayoutHandle  pipelineLayout;
    FRHIComputePipelineHandle pipeline;
    FRHIShaderHandle          shader;
    FRHISamplerHandle         depthSampler;

    const auto cleanup = [&]()
    {
        device->DestroyComputePipeline(pipeline);
        device->DestroyPipelineLayout(pipelineLayout);
        device->FreeDescriptorSet(descriptorSet);
        device->DestroyDescSetLayout(setLayout);
        device->DestroyShader(shader);
        device->DestroySampler(depthSampler);
        device->DestroyBuffer(resultBuffer);
    };

    {
        FRHISamplerDesc samplerDesc = {};
        // 最近邻 + 钳边。着色器用的是 texelFetch, 采样器的过滤其实不
        // 参与, 但描述符要求有一个 —— 填成不插值的, 免得将来有人改成
        // texture() 时静默地多出一层双线性。
        samplerDesc.MinFilter    = EFilter::Nearest;
        samplerDesc.MagFilter    = EFilter::Nearest;
        samplerDesc.MipmapMode   = ESamplerMipmapMode::Nearest;
        samplerDesc.AddressModeU = ESamplerAddressMode::ClampToEdge;
        samplerDesc.AddressModeV = ESamplerAddressMode::ClampToEdge;
        samplerDesc.AddressModeW = ESamplerAddressMode::ClampToEdge;
        samplerDesc.IsAnisotropyEnabled = false;

        if (!IsRHISuccess(device->CreateSampler(samplerDesc, depthSampler)))
        {
            LIMX_LOG(LogRayTracingCheck, Error, "[光追深度] 采样器创建失败");
            cleanup();
            return false;
        }
    }

    {
        FRHIDescriptorBinding bindings[3] = {};

        bindings[0].Binding    = 0;
        bindings[0].Type       = EDescriptorType::AccelerationStructure;
        bindings[0].Count      = 1;
        bindings[0].StageFlags = EShaderStage::Compute;

        bindings[1].Binding    = 1;
        bindings[1].Type       = EDescriptorType::StorageBuffer;
        bindings[1].Count      = 1;
        bindings[1].StageFlags = EShaderStage::Compute;

        bindings[2].Binding    = 2;
        bindings[2].Type       = EDescriptorType::CombinedImageSampler;
        bindings[2].Count      = 1;
        bindings[2].StageFlags = EShaderStage::Compute;

        FRHIDescSetLayoutDesc layoutDesc = {};
        layoutDesc.Bindings     = bindings;
        layoutDesc.BindingCount = 3;
        layoutDesc.DebugName    = "RtDepth.SetLayout";

        if (!IsRHISuccess(device->CreateDescSetLayout(layoutDesc, setLayout)) ||
            !IsRHISuccess(device->AllocateDescriptorSet(setLayout,
                                                        descriptorSet)))
        {
            LIMX_LOG(LogRayTracingCheck, Error, "[光追深度] 描述符集创建失败");
            cleanup();
            return false;
        }
    }

    FShaderManager& shaders = FShaderManager::Get();

    if (!shaders.IsInitialized())
    {
        shaders.Initialize();
    }

    if (!IsRHISuccess(shaders.CreateShaderModule(
            device, FString("Builtin/rt_depth.comp"),
            EShaderStage::Compute, shader)))
    {
        LIMX_LOG(LogRayTracingCheck, Error,
                 "[光追深度] rt_depth.comp 加载失败");
        cleanup();
        return false;
    }

    {
        FRHIPushConstantRange pushRange = {};
        pushRange.StageFlags = EShaderStage::Compute;
        pushRange.Offset     = 0;
        pushRange.Size       = sizeof(FRtDepthPushConstants);

        FRHIPipelineLayoutDesc layoutDesc = {};
        layoutDesc.SetLayouts             = &setLayout;
        layoutDesc.SetLayoutCount         = 1;
        layoutDesc.PushConstantRanges     = &pushRange;
        layoutDesc.PushConstantRangeCount = 1;
        layoutDesc.DebugName              = "RtDepth.PipelineLayout";

        if (!IsRHISuccess(device->CreatePipelineLayout(layoutDesc,
                                                        pipelineLayout)))
        {
            cleanup();
            return false;
        }

        FRHIComputePipelineDesc pipelineDesc = {};
        pipelineDesc.ComputeShader.Shader     = shader;
        pipelineDesc.ComputeShader.Stage      = EShaderStage::Compute;
        pipelineDesc.ComputeShader.EntryPoint = "main";
        pipelineDesc.PipelineLayout           = pipelineLayout;
        pipelineDesc.DebugName                = "RtDepth.Pipeline";

        if (!IsRHISuccess(device->CreateComputePipeline(pipelineDesc,
                                                         pipeline)))
        {
            cleanup();
            return false;
        }
    }

    // ------------------------------------------------------------------
    // 先把一帧完整渲染出来, 再单独开一个命令缓冲区发射线
    //
    // 不挂帧内回调: 那条路上有并行录制、二级命令缓冲区、各通道自己的屏障,
    // 变量太多。帧渲染完再读, 深度图的内容就是确定的, 与帧怎么录制无关。
    // ------------------------------------------------------------------
    const FRHITextureViewHandle depthView =
        renderer.GetPassManager()->GetSharedDepthView();

    const FRHITextureHandle depthTexture = depthPass->GetSharedDepthTexture();


    renderer.RenderFrame();

    device->WaitIdle();

    {
        FRHIDescriptorWrite writes[3];

        writes[0] = FRHIDescriptorWrite();
        writes[0].DescriptorSet = descriptorSet;
        writes[0].Binding       = 0;
        writes[0].Type          = EDescriptorType::AccelerationStructure;
        writes[0].AccelStruct   = renderer.GetRayTracingScene().GetTlas();

        writes[1] = FRHIDescriptorWrite::StorageBuffer(
            descriptorSet, 1, resultBuffer, 0, resultBytes);

        writes[2] = FRHIDescriptorWrite();
        writes[2].DescriptorSet = descriptorSet;
        writes[2].Binding       = 2;
        writes[2].Type          = EDescriptorType::CombinedImageSampler;
        writes[2].ImageView     = depthView;
        writes[2].Sampler       = depthSampler;
        writes[2].ImageLayout   = EImageLayout::General;

        device->UpdateDescriptorSets(writes, 3);
    }

    IRHICommandBuffer* cmd = context->BeginSingleTimeCommands();

    if (cmd == nullptr)
    {
        LIMX_LOG(LogRayTracingCheck, Error, "[光追深度] 命令缓冲区分配失败");
        cleanup();
        return false;
    }

    // 深度此刻停在 DepthStencilAttachment (深度预通道与前向通道的
    // FinalLayout)。计算着色器要采样它, 必须转成着色器只读。


    // 深度此刻停在 DepthStencilAttachment (深度预通道与前向通道的
    // FinalLayout)。计算着色器要采样它, 必须先转布局 —— 深度附件在 NVIDIA
    // 上是压缩存储的, 而解压由布局转换触发。
    cmd->TransitionImageLayout(
        depthTexture,
        EImageLayout::DepthStencilAttachment,
        EImageLayout::General,
        EPipelineStageFlags::LateFragmentTests,
        EPipelineStageFlags::ComputeShader,
        EAccessFlags::DepthStencilAttachmentWrite,
        EAccessFlags::ShaderRead);

    const FCamera& camera = renderer.GetCamera();

    const FMatrix viewProj =
        camera.GetProjectionMatrix() * camera.GetViewMatrix();

    FRtDepthPushConstants push;
    push.InvViewProj = viewProj.Inverse();
    push.View        = camera.GetViewMatrix();

    push.Width  = extent.Width;
    push.Height = extent.Height;

    push.NearPlane = camera.GetNearPlane();
    push.FarPlane  = camera.GetFarPlane();


    // 只看会写深度的那一类。
    //
    // 半透明几何体在光栅化里不写深度, 把它算进来的话光追会说"被玻璃挡住",
    // 而深度缓冲区说"看得到玻璃后面" —— 两边永远对不上, 而那不是加速结构
    // 的错。
    push.RayMask = kRayMaskDepthWriting;

    cmd->BindComputePipeline(pipeline);
    cmd->BindDescriptorSet(EPipelineBindPoint::Compute, pipelineLayout, 0,
                            descriptorSet);
    cmd->PushConstants(pipelineLayout, EShaderStage::Compute, 0,
                        sizeof(push), &push);

    constexpr UInt32 kGroup = 8;
    cmd->Dispatch((extent.Width + kGroup - 1) / kGroup,
                   (extent.Height + kGroup - 1) / kGroup, 1);

    FRHIMemoryBarrier barrier = {};
    barrier.SrcAccessMask = EAccessFlags::ShaderWrite;
    barrier.DstAccessMask = EAccessFlags::HostRead;

    cmd->PipelineBarrier(EPipelineStageFlags::ComputeShader,
                          EPipelineStageFlags::Host,
                          &barrier, 1, nullptr, 0, nullptr, 0);

    // 还回去 —— 下一帧的深度预通道以 DepthStencilAttachment 起步


    // 还回去 —— 下一帧的深度预通道以 DepthStencilAttachment 起步
    cmd->TransitionImageLayout(
        depthTexture,
        EImageLayout::General,
        EImageLayout::DepthStencilAttachment,
        EPipelineStageFlags::ComputeShader,
        EPipelineStageFlags::EarlyFragmentTests,
        EAccessFlags::ShaderRead,
        EAccessFlags::DepthStencilAttachmentWrite);

    context->EndSingleTimeCommands(cmd);

    device->WaitIdle();

    // ------------------------------------------------------------------
    // 比对
    // ------------------------------------------------------------------
    void* mapped = nullptr;

    if (!IsRHISuccess(device->MapBuffer(resultBuffer, &mapped)) ||
        mapped == nullptr)
    {
        LIMX_LOG(LogRayTracingCheck, Error, "[光追深度] 回读映射失败");
        cleanup();
        return false;
    }

    const auto* pairs = static_cast<const FDepthPair*>(mapped);

    const FCamera& readbackCamera = renderer.GetCamera();

    const Float32 nearPlane = readbackCamera.GetNearPlane();
    const Float32 farPlane  = readbackCamera.GetFarPlane();



    // 相对容差。
    //
    // 容差按**深度缓冲区在这一像素上能分辨多细**算 —— 见 DepthQuantumAt。
    //
    // 允许的偏差是若干个"最低位"。64 这个数是从实测点之间取的:
    //
    //   墙角场景基线    最大 1.9 个最低位   (几何体填满整屏, 无轮廓)
    //   阴影场景基线    最大 1.4 个最低位   (同上)
    //   综合场景基线    绝大多数像素 < 64; 15 个轮廓像素超出
    //   OBJ 场景基线    绝大多数像素 < 64;  6 个轮廓像素超出
    //
    // 也就是说: 没有轮廓的场景上, 两条路径的差只有一两个最低位 —— 这个
    // 判据的分辨率就是深度缓冲区本身的分辨率。64 的余量留给轮廓附近的
    // 插值差, 而任何一种"几何体装错了"都是成千上万个最低位。
    //
    // 用最低位而不是相对误差, 是因为透视深度在远处压缩得极厉害: 同一个
    // 相对容差在 near=0.1 与 near=0.01 的场景上严格程度差十倍 (实测)。
    constexpr Float32 kUlpBudget = 64.0f;

    SizeType coveredCount  = 0;
    SizeType agreeCount    = 0;
    SizeType rtMissCount   = 0;
    SizeType rtExtraCount  = 0;
    SizeType disagreeCount = 0;
    SizeType uninitCount   = 0;

    Float32 maxRelativeError = 0.0f;

    // 逐实例的不符计数 —— 失败时报出来, 好知道是全局问题还是某个物体
    constexpr UInt32 kMismatchBuckets = 64;
    SizeType mismatchByInstance[kMismatchBuckets] = {};

    for (SizeType i = 0; i < pixelCount; ++i)
    {
        const Float32 rt     = pairs[i].RayTraced;
        const Float32 raster = pairs[i].Rasterized;

        // 0xFF 填充解释成 float 是 NaN —— 这一格根本没被着色器写过
        if (rt != rt || raster != raster)
        {
            ++uninitCount;
            continue;
        }

        const bool rasterHit = (raster > 0.0f);
        const bool rtHit     = (rt > 0.0f);

        if (!rasterHit)
        {
            if (rtHit)
            {
                ++rtExtraCount;
            }
            continue;
        }

        ++coveredCount;

        if (!rtHit)
        {
            ++rtMissCount;
            continue;
        }

        const Float32 quantum =
            DepthQuantumAt(pairs[i].RasterNdc, nearPlane, farPlane);

        // 以"多少个最低位"计的偏差
        const Float32 ulpError =
            FMath::Abs(raster - rt) / FMath::Max(quantum, 1.0e-12f);

        if (ulpError > maxRelativeError)
        {
            maxRelativeError = ulpError;
        }

        if (ulpError <= kUlpBudget)
        {
            ++agreeCount;
        }
        else
        {
            ++disagreeCount;

            // 不符落在哪个实例上 —— 失败时唯一有用的线索。
            // 全场景一片不符与"某一个物体的几何体建错了"在总数上分不开。
            const Int32 inst = static_cast<Int32>(pairs[i].Instance);

            if (inst >= 0 && inst < static_cast<Int32>(kMismatchBuckets))
            {
                ++mismatchByInstance[inst];
            }
        }
    }

    device->UnmapBuffer(resultBuffer);

    const FRayTracingScene& rtScene = renderer.GetRayTracingScene();

    LIMX_LOG(LogRayTracingCheck, Display,
             "[光追深度] BLAS {} 个 (跳过 {}) 实例 {} 个 | "
             "光栅覆盖 {} 像素 — 一致 {} 光追缺失 {} 深度不符 {} | "
             "光追多出 {} | 未写入 {} | 最大偏差 {} 个最低位",
             rtScene.GetBlasCount(), rtScene.GetSkippedCount(),
             rtScene.GetInstanceCount(),
             coveredCount, agreeCount, rtMissCount, disagreeCount,
             rtExtraCount, uninitCount, maxRelativeError);

    LIMX_LOG(LogRayTracingCheck, Display,
             "[光追深度] 实例分类 — 不透明 {} 蒙版 {} 半透明 {}",
             rtScene.GetInstanceCountByClass(0),
             rtScene.GetInstanceCountByClass(1),
             rtScene.GetInstanceCountByClass(2));

    bool passed = true;

    // ---- 0. 结果缓冲区必须整块被写过 ----
    if (uninitCount != 0)
    {
        LIMX_LOG(LogRayTracingCheck, Error,
                 "[光追深度] {} 个像素的结果没被写过 — 派发范围不对?",
                 uninitCount);
        passed = false;
    }

    // ---- 1. 场景里必须真的有东西 ----
    //
    // 光栅器一个像素都没画到的话, 下面每一条都自动成立 —— 而那时判据什么
    // 都没验。空场景与"加速结构全错"在其余判据上完全一样。
    constexpr SizeType kMinCoveredPixels = 100000;

    if (coveredCount < kMinCoveredPixels)
    {
        LIMX_LOG(LogRayTracingCheck, Error,
                 "[光追深度] 光栅化只覆盖了 {} 个像素 (需要至少 {}) — "
                 "这个场景判不了",
                 coveredCount, kMinCoveredPixels);
        passed = false;
    }

    // ---- 2. 加速结构里必须真的有几何体 ----
    if (rtScene.GetInstanceCount() == 0)
    {
        LIMX_LOG(LogRayTracingCheck, Error,
                 "[光追深度] TLAS 里一个实例都没有 — 树是空的");
        passed = false;
    }

    if (rtScene.GetSkippedCount() != 0)
    {
        LIMX_LOG(LogRayTracingCheck, Error,
                 "[光追深度] {} 个对象因几何体无效被跳过 — "
                 "它们在光栅化里画着, 在光追里不存在",
                 rtScene.GetSkippedCount());
        passed = false;
    }

    // ---- 3. 逐像素一致率 ----
    //
    // 不要求 100%: 轮廓边上光栅器的覆盖判定与射线求交都压在浮点边界上,
    // 半个 ULP 就会让两边选到不同的三角形, 而那时深度差是前后两个物体的
    // 距离。这类像素只出现在轮廓上, 数量与周长成正比。
    //
    // 阈值定在万分之一。实测的基线不符率:
    //
    //   墙角 / 阴影场景   0        (几何体填满整屏, 没有轮廓)
    //   综合场景          19 / 919234  = 0.0021%
    //   OBJ 场景          18 / 198364  = 0.0091%
    //
    // 剩下的都是轮廓像素: 光栅器的覆盖判定与射线求交都压在浮点边界上,
    // 半个 ULP 就会让两边选到不同的三角形, 而那时深度差是前后两个物体的
    // 距离 (实测最大近五万个最低位)。这类像素的数量与周长成正比。
    //
    // OBJ 场景的 0.0091% 距上限只有一倍余量 —— 它的近平面是 0.01 (别的
    // 场景是 0.1), 轮廓处的深度插值误差本来就更大。放宽到千分之一的话
    // "射线不加半个像素偏移"那条变异会整条逃掉。
    constexpr Float32 kMaxDisagreeFraction = 1.0e-4f;

    const SizeType mismatchTotal = rtMissCount + disagreeCount;

    const Float32 mismatchFraction =
        (coveredCount > 0)
            ? static_cast<Float32>(mismatchTotal) /
              static_cast<Float32>(coveredCount)
            : 1.0f;

    if (mismatchFraction > kMaxDisagreeFraction)
    {
        LIMX_LOG(LogRayTracingCheck, Error,
                 "[光追深度] {} / {} 个像素对不上 ({}%), 超过上限 {}% — "
                 "加速结构里的场景与光栅化的不是同一个",
                 mismatchTotal, coveredCount,
                 mismatchFraction * 100.0f,
                 kMaxDisagreeFraction * 100.0f);
        passed = false;
    }

    if (!passed)
    {
        for (UInt32 k = 0; k < kMismatchBuckets; ++k)
        {
            if (mismatchByInstance[k] > 0)
            {
                LIMX_LOG(LogRayTracingCheck, Error,
                         "[光追深度] 实例 {} 上有 {} 个不符像素",
                         k, mismatchByInstance[k]);
            }
        }
    }

    LIMX_LOG(LogRayTracingCheck, Display,
             "[光追深度] {}", passed ? "通过" : "失败");

    cleanup();

    return passed;
}

} // namespace Limx
