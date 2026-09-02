/*******************************************************************************
 * 文件: FMeshletCullPass.cpp
 * 创建时间: 2026-09-02
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   两级 GPU 剔除的实现 — 实例级压实 + meshlet 级间接分派。
 *
 ******************************************************************************/

#include "Renderer/RendererMinimal.h"

#include "Renderer/RenderPass/FMeshletCullPass.h"
#include "Renderer/Renderer/FRenderer.h"

#include "RenderCore/Geometry/FMeshletBuilder.h"
#include "RenderCore/Shaders/FShaderManager.h"

#include "Core/Math/FFrustum.h"

namespace Limx
{

LIMX_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{

constexpr UInt32 kCullWorkgroupSize = 64;

/// 三个剔除着色器共用的 push constant 形状
///
/// 逐视图的东西 (平面、矩阵、相机、金字塔参数) 全在 UBO 里 —— 见
/// FMeshletCullViewGpu 上面那段。留在 push constant 里的只有这一帧的
/// 计数与开关, 十六个字节。
struct FCullPushConstants
{
    UInt32 Params[4] = { 0, 0, 0, 0 };
};

static_assert(sizeof(FCullPushConstants) == 16,
              "FCullPushConstants 必须是 16 字节 — 与三个剔除着色器的 "
              "push constant 块逐字段一致");

/// 汇总缓冲区的字节数
constexpr UInt64 kSceneMeshletBytes =
    static_cast<UInt64>(sizeof(FMeshlet)) * kMaxSceneMeshlets;

/// 场景顶点的上限。
///
/// meshlet 的局部顶点表指向它, 而每个 meshlet 最多 64 个局部顶点 ——
/// 但那是**去重后**的, 场景顶点数与 meshlet 数不成固定比例。取 400 万个
/// (每个 72 字节, 288 MiB) 覆盖到十万级三角形的场景。
///
/// 超出时**丢掉整个网格并报错** —— 不是截断。截断的后果是那个网格的
/// 一部分 meshlet 指向别人的顶点, 画面上是一团乱七八糟的三角形。
constexpr UInt32 kMaxSceneVertices = 4000000;

constexpr UInt64 kSceneVertexBytes =
    static_cast<UInt64>(sizeof(FMeshVertex)) * kMaxSceneVertices;

/// meshlet 局部顶点表的上限 —— 每个 meshlet 最多 64 个
constexpr UInt32 kMaxSceneMeshletVertices = kMaxSceneMeshlets * 64;

constexpr UInt64 kSceneMeshletVertexBytes =
    static_cast<UInt64>(sizeof(UInt32)) * kMaxSceneMeshletVertices;

/// meshlet 三角形索引的上限 —— 每个 meshlet 最多 124 个三角形, 每个三字节,
/// 按 UInt32 打包 (四字节一组)
constexpr UInt32 kMaxSceneMeshletTriangleWords =
    (kMaxSceneMeshlets * kMaxMeshletTriangles * 3u + 3u) / 4u;

constexpr UInt64 kSceneMeshletTriangleBytes =
    static_cast<UInt64>(sizeof(UInt32)) * kMaxSceneMeshletTriangleWords;

constexpr UInt64 kInstanceBytes =
    static_cast<UInt64>(sizeof(FMeshletInstanceGpu)) * kMaxMeshletInstances;

constexpr UInt64 kInstanceSphereBytes =
    static_cast<UInt64>(sizeof(Float32)) * 4 * kMaxMeshletInstances;

constexpr UInt64 kVisibleInstanceBytes =
    static_cast<UInt64>(sizeof(UInt32)) * kMaxMeshletInstances;

/// 可见 meshlet 表 —— uvec2 一条
constexpr UInt64 kVisibleMeshletBytes =
    static_cast<UInt64>(sizeof(UInt32)) * 2 * kMaxSceneMeshlets;

constexpr UInt64 kDispatchBytes = sizeof(UInt32) * 4;
constexpr UInt64 kCounterBytes = sizeof(UInt32) * 4;

/// 光栅化间接参数 —— (groupCountX, groupCountY, groupCountZ, 保留)
constexpr UInt64 kRasterArgsBytes = sizeof(UInt32) * 4;

/// 待定表 —— 与可见表同样大 (最坏情况下每个 meshlet 都被遮挡剔掉)
constexpr UInt64 kPendingBytes = kVisibleMeshletBytes;

constexpr UInt64 kPendingCounterBytes = sizeof(UInt32) * 4;

constexpr UInt64 kViewBytes = sizeof(FMeshletCullViewGpu);

} // namespace

// ============================================================================
// Setup
// ============================================================================

ERHIResult FMeshletCullPass::Setup(const FPassSetupDesc& desc)
{
    LIMX_CHECK(desc.Device != nullptr);
    LIMX_CHECK(desc.MaxFramesInFlight > 0);

    m_Device = desc.Device;

    IRHIDevice* const device = desc.Device;

    const UInt32 frameCount = desc.MaxFramesInFlight;

    // ---- 汇总后的场景数据 (四份) ----
    {
        // TransferSrc 是给判据用的 —— 它要把上传上去的那份读回来比对。
        // 没有它的话判据只能比 CPU 内存里那份与自己, 上传路径整个不在
        // 覆盖里。
        const EBufferUsage usage = static_cast<EBufferUsage>(
            static_cast<UInt32>(EBufferUsage::StorageBuffer) |
            static_cast<UInt32>(EBufferUsage::TransferDst) |
            static_cast<UInt32>(EBufferUsage::TransferSrc));

        struct FSceneBuffer
        {
            UInt64            Size;
            const AnsiChar*   Name;
            FRHIBufferHandle* Target;
        };

        const FSceneBuffer sceneBuffers[4] = {
            { kSceneMeshletBytes, "SceneMeshlets", &m_SceneMeshlets },
            { kSceneVertexBytes, "SceneVertices", &m_SceneVertices },
            { kSceneMeshletVertexBytes, "SceneMeshletVertices",
              &m_SceneMeshletVertices },
            { kSceneMeshletTriangleBytes, "SceneMeshletTriangles",
              &m_SceneMeshletTriangles },
        };

        for (UInt32 i = 0; i < 4; ++i)
        {
            FRHIBufferDesc bufferDesc = {};
            bufferDesc.Size        = sceneBuffers[i].Size;
            bufferDesc.Usage       = usage;
            bufferDesc.MemoryUsage = EMemoryUsage::GpuOnly;
            bufferDesc.DebugName   = sceneBuffers[i].Name;

            const ERHIResult result =
                device->CreateBuffer(bufferDesc, *sceneBuffers[i].Target);

            if (!IsRHISuccess(result))
            {
                return result;
            }
        }
    }

    // ---- 逐并行帧 ----
    for (UInt32 i = 0; i < frameCount; ++i)
    {
        const auto Create = [device](UInt64 size, EBufferUsage usage,
                                     EMemoryUsage memory,
                                     const AnsiChar* name,
                                     FRHIBufferHandle& out) -> ERHIResult
        {
            FRHIBufferDesc bufferDesc = {};
            bufferDesc.Size        = size;
            bufferDesc.Usage       = usage;
            bufferDesc.MemoryUsage = memory;
            bufferDesc.DebugName   = name;

            return device->CreateBuffer(bufferDesc, out);
        };

        const EBufferUsage storageAndTransfer = static_cast<EBufferUsage>(
            static_cast<UInt32>(EBufferUsage::StorageBuffer) |
            static_cast<UInt32>(EBufferUsage::TransferDst) |
            static_cast<UInt32>(EBufferUsage::TransferSrc));

        FRHIBufferHandle instances;
        FRHIBufferHandle spheres;
        FRHIBufferHandle visibleInstances;
        FRHIBufferHandle dispatchArgs;
        FRHIBufferHandle visibleMeshlets;
        FRHIBufferHandle counters;
        FRHIBufferHandle readback;

        ERHIResult result =
            Create(kInstanceBytes, EBufferUsage::StorageBuffer,
                   EMemoryUsage::CpuToGpu, "MeshletInstances", instances);

        if (IsRHISuccess(result))
        {
            result = Create(kInstanceSphereBytes, EBufferUsage::StorageBuffer,
                            EMemoryUsage::CpuToGpu, "MeshletInstanceSpheres",
                            spheres);
        }

        if (IsRHISuccess(result))
        {
            result = Create(kVisibleInstanceBytes,
                            EBufferUsage::StorageBuffer,
                            EMemoryUsage::GpuOnly, "VisibleMeshletInstances",
                            visibleInstances);
        }

        if (IsRHISuccess(result))
        {
            // 分派参数缓冲区既要被计算着色器原子累加, 又要当
            // vkCmdDispatchIndirect 的源 —— 两个用途缺一不可。
            result = Create(kDispatchBytes,
                            static_cast<EBufferUsage>(
                                static_cast<UInt32>(
                                    EBufferUsage::StorageBuffer) |
                                static_cast<UInt32>(
                                    EBufferUsage::IndirectBuffer) |
                                static_cast<UInt32>(
                                    EBufferUsage::TransferDst)),
                            EMemoryUsage::GpuOnly, "MeshletDispatchArgs",
                            dispatchArgs);
        }

        if (IsRHISuccess(result))
        {
            result = Create(kVisibleMeshletBytes, storageAndTransfer,
                            EMemoryUsage::GpuOnly, "VisibleMeshlets",
                            visibleMeshlets);
        }

        if (IsRHISuccess(result))
        {
            result = Create(kCounterBytes, storageAndTransfer,
                            EMemoryUsage::GpuOnly, "MeshletCullCounters",
                            counters);
        }

        if (IsRHISuccess(result))
        {
            result = Create(kCounterBytes, EBufferUsage::TransferDst,
                            EMemoryUsage::GpuToCpu,
                            "MeshletCullCountersReadback", readback);
        }

        if (!IsRHISuccess(result))
        {
            return result;
        }

        m_InstanceBuffers.Add(instances);
        m_InstanceSphereBuffers.Add(spheres);
        m_VisibleInstanceBuffers.Add(visibleInstances);
        m_DispatchBuffers.Add(dispatchArgs);
        m_VisibleMeshletBuffers.Add(visibleMeshlets);
        m_CounterBuffers.Add(counters);
        m_CounterReadbacks.Add(readback);

        // 光栅化的间接参数 —— (可见数, 1, 1, 0)
        //
        // 与计数器分开一份而不是让光栅化直接读计数器: 计数器的布局是
        // (可见, 视锥剔, 背面剔, 测试数), 而间接分派要的是 (x, y, z)。
        // 让两者共用一份就得把诊断计数挪到别的位置, 那会让"这个数是什么"
        // 完全取决于谁在读它。
        FRHIBufferHandle rasterArgs;

        {
            const ERHIResult argsResult =
                Create(kRasterArgsBytes,
                       static_cast<EBufferUsage>(
                           static_cast<UInt32>(EBufferUsage::StorageBuffer) |
                           static_cast<UInt32>(EBufferUsage::IndirectBuffer) |
                           static_cast<UInt32>(EBufferUsage::TransferDst)),
                       EMemoryUsage::GpuOnly, "MeshletRasterArgs",
                       rasterArgs);

            if (!IsRHISuccess(argsResult))
            {
                return argsResult;
            }
        }

        m_RasterArgsBuffers.Add(rasterArgs);

        // 待定表 + 它的计数器 + 逐视图 UBO
        {
            FRHIBufferHandle pending;
            FRHIBufferHandle pendingCounter;
            FRHIBufferHandle view;

            ERHIResult extra =
                Create(kPendingBytes, storageAndTransfer,
                       EMemoryUsage::GpuOnly, "MeshletPending", pending);

            if (IsRHISuccess(extra))
            {
                extra = Create(kPendingCounterBytes, storageAndTransfer,
                               EMemoryUsage::GpuOnly, "MeshletPendingCounter",
                               pendingCounter);
            }

            if (IsRHISuccess(extra))
            {
                extra = Create(kViewBytes, EBufferUsage::UniformBuffer,
                               EMemoryUsage::CpuToGpu, "MeshletCullView",
                               view);
            }

            if (!IsRHISuccess(extra))
            {
                return extra;
            }

            m_PendingBuffers.Add(pending);
            FRHIBufferHandle pendingReadback;

            {
                const ERHIResult readbackResult =
                    Create(kPendingCounterBytes, EBufferUsage::TransferDst,
                           EMemoryUsage::GpuToCpu, "MeshletPendingReadback",
                           pendingReadback);

                if (!IsRHISuccess(readbackResult))
                {
                    return readbackResult;
                }
            }

            m_PendingReadbacks.Add(pendingReadback);
            m_PendingCounterBuffers.Add(pendingCounter);
            m_ViewBuffers.Add(view);
        }
    }

    // ---- 归零用的拷贝源 ----
    {
        FRHIBufferDesc bufferDesc = {};
        // 多四个字节放 1.0f —— 兜底金字塔的清除值。
        //
        // 不能拿前面那些 0 去清: 0.0f 是"最近", 拿它当遮挡深度的话**一切
        // 都被判成挡住了**, 整个场景消失。兜底值必须落在安全的那一侧。
        bufferDesc.Size =
            kCounterBytes + kDispatchBytes + kRasterArgsBytes + sizeof(UInt32);
        bufferDesc.Usage       = EBufferUsage::TransferSrc;
        bufferDesc.MemoryUsage = EMemoryUsage::CpuToGpu;
        bufferDesc.DebugName   = "MeshletCullResetSource";

        const ERHIResult result =
            device->CreateBuffer(bufferDesc, m_ResetSource);

        if (!IsRHISuccess(result))
        {
            return result;
        }

        void* mapped = nullptr;

        if (IsRHISuccess(device->MapBuffer(m_ResetSource, &mapped)) &&
            mapped != nullptr)
        {
            UInt32* values = static_cast<UInt32*>(mapped);

            // 计数器四个全零
            values[0] = 0;
            values[1] = 0;
            values[2] = 0;
            values[3] = 0;

            // 分派参数: x 由着色器原子累加, y/z 必须是 1
            values[4] = 0;
            values[5] = 1;
            values[6] = 1;
            values[7] = 1;

            // 光栅化间接参数: x 由可见数拷进来, y/z 恒为 1
            values[8]  = 0;
            values[9]  = 1;
            values[10] = 1;
            values[11] = 0;

            // 兜底金字塔的清除值: 1.0f 的位模式 = 最远 = 什么都挡不住
            values[12] = 0x3F800000u;

            device->UnmapBuffer(m_ResetSource);
        }
    }

    // ---- 1x1 的"没有遮挡物"纹理 ----
    //
    // 见头文件里那段: 描述符集里不能有没写过的绑定。
    {
        FRHITextureDesc texDesc = {};
        texDesc.Type        = ETextureType::Texture2D;
        texDesc.Format      = EPixelFormat::R32_SFLOAT;
        texDesc.Extent      = { 1, 1, 1 };
        texDesc.MipLevels   = 1;
        texDesc.ArrayLayers = 1;
        texDesc.Samples     = ESampleCount::Count1;
        texDesc.Usage       = static_cast<ETextureUsage>(
            static_cast<UInt32>(ETextureUsage::Sampled) |
            static_cast<UInt32>(ETextureUsage::TransferDst));
        texDesc.MemoryUsage = EMemoryUsage::GpuOnly;
        texDesc.DebugName   = "MeshletHizDummy";

        ERHIResult result = device->CreateTexture(texDesc, m_DummyHizTexture);

        if (!IsRHISuccess(result))
        {
            return result;
        }

        FRHITextureViewDesc viewDesc = {};
        viewDesc.Texture         = m_DummyHizTexture;
        viewDesc.ViewType        = ETextureType::Texture2D;
        viewDesc.Format          = EPixelFormat::R32_SFLOAT;
        viewDesc.BaseMipLevel    = 0;
        viewDesc.MipLevelCount   = 1;
        viewDesc.BaseArrayLayer  = 0;
        viewDesc.ArrayLayerCount = 1;

        result = device->CreateTextureView(viewDesc, m_DummyHizView);

        if (!IsRHISuccess(result))
        {
            return result;
        }

        // 金字塔的采样器: 最近邻 + 钳边。
        //
        // 线性过滤是错的 —— 插值出来的"最大深度"不是任何一片区域的最大
        // 深度, 而遮挡测试的保守性正建立在"那个数确实是最大值"上。
        FRHISamplerDesc samplerDesc = {};
        samplerDesc.MinFilter    = EFilter::Nearest;
        samplerDesc.MagFilter    = EFilter::Nearest;
        samplerDesc.MipmapMode   = ESamplerMipmapMode::Nearest;
        samplerDesc.AddressModeU = ESamplerAddressMode::ClampToEdge;
        samplerDesc.AddressModeV = ESamplerAddressMode::ClampToEdge;
        samplerDesc.AddressModeW = ESamplerAddressMode::ClampToEdge;


        result = device->CreateSampler(samplerDesc, m_HizDefaultSampler);

        if (!IsRHISuccess(result))
        {
            return result;
        }

        m_HizSampler = m_HizDefaultSampler;
    }

    // ---- 描述符 ----
    {
        FRHIDescriptorBinding bindings[9] = {};

        for (UInt32 i = 0; i < 9; ++i)
        {
            bindings[i].Binding    = i;
            bindings[i].Type       = EDescriptorType::StorageBuffer;
            bindings[i].Count      = 1;
            bindings[i].StageFlags = EShaderStage::Compute;
        }

        // 第一级: 0..3 是存储缓冲区, 4 是逐视图 UBO
        bindings[4].Type = EDescriptorType::UniformBuffer;

        FRHIDescSetLayoutDesc layoutDesc = {};
        layoutDesc.Bindings     = bindings;
        layoutDesc.BindingCount = 5;
        layoutDesc.DebugName    = "MeshletInstanceCullSetLayout";

        ERHIResult result =
            device->CreateDescSetLayout(layoutDesc, m_InstanceCullSetLayout);

        if (!IsRHISuccess(result))
        {
            return result;
        }

        // 第二级: 0..6 是存储缓冲区, 7 是金字塔 (采样器), 8 是逐视图 UBO
        bindings[4].Type = EDescriptorType::StorageBuffer;
        bindings[7].Type = EDescriptorType::CombinedImageSampler;
        bindings[8].Type = EDescriptorType::UniformBuffer;

        layoutDesc.BindingCount = 9;
        layoutDesc.DebugName    = "MeshletCullSetLayout";

        result = device->CreateDescSetLayout(layoutDesc, m_MeshletCullSetLayout);

        if (!IsRHISuccess(result))
        {
            return result;
        }

        for (UInt32 i = 0; i < frameCount; ++i)
        {
            FRHIDescriptorSetHandle instanceSet;

            result = device->AllocateDescriptorSet(m_InstanceCullSetLayout,
                                                   instanceSet);

            if (!IsRHISuccess(result))
            {
                return result;
            }

            FRHIDescriptorWrite instanceWrites[5];

            instanceWrites[0] = FRHIDescriptorWrite::StorageBuffer(
                instanceSet, 0, m_InstanceBuffers[i], 0, kInstanceBytes);
            instanceWrites[1] = FRHIDescriptorWrite::StorageBuffer(
                instanceSet, 1, m_InstanceSphereBuffers[i], 0,
                kInstanceSphereBytes);
            instanceWrites[2] = FRHIDescriptorWrite::StorageBuffer(
                instanceSet, 2, m_VisibleInstanceBuffers[i], 0,
                kVisibleInstanceBytes);
            instanceWrites[3] = FRHIDescriptorWrite::StorageBuffer(
                instanceSet, 3, m_DispatchBuffers[i], 0, kDispatchBytes);
            instanceWrites[4] = FRHIDescriptorWrite::UniformBuffer(
                instanceSet, 4, m_ViewBuffers[i], 0, kViewBytes);

            device->UpdateDescriptorSets(instanceWrites, 5);

            m_InstanceCullSets.Add(instanceSet);

            FRHIDescriptorSetHandle meshletSet;

            result = device->AllocateDescriptorSet(m_MeshletCullSetLayout,
                                                   meshletSet);

            if (!IsRHISuccess(result))
            {
                return result;
            }

            // 金字塔那一格 (binding 7) 在这里先不写 —— 它要等光栅化
            // 通道把纹理建好。Execute 里看到金字塔换了就补写。
            //
            // 描述符集里有未写的绑定时**不能**提交给使用它的管线, 所以
            // 遮挡剔除在金字塔就绪之前必须是关的 —— C++ 侧那个开关就是
            // 这么用的, 而不是"传个 0 让着色器自己跳过"。
            FRHIDescriptorWrite meshletWrites[8];

            meshletWrites[0] = FRHIDescriptorWrite::StorageBuffer(
                meshletSet, 0, m_InstanceBuffers[i], 0, kInstanceBytes);
            meshletWrites[1] = FRHIDescriptorWrite::StorageBuffer(
                meshletSet, 1, m_SceneMeshlets, 0, kSceneMeshletBytes);
            meshletWrites[2] = FRHIDescriptorWrite::StorageBuffer(
                meshletSet, 2, m_VisibleInstanceBuffers[i], 0,
                kVisibleInstanceBytes);
            meshletWrites[3] = FRHIDescriptorWrite::StorageBuffer(
                meshletSet, 3, m_VisibleMeshletBuffers[i], 0,
                kVisibleMeshletBytes);
            meshletWrites[4] = FRHIDescriptorWrite::StorageBuffer(
                meshletSet, 4, m_CounterBuffers[i], 0, kCounterBytes);
            meshletWrites[5] = FRHIDescriptorWrite::StorageBuffer(
                meshletSet, 5, m_PendingBuffers[i], 0, kPendingBytes);
            meshletWrites[6] = FRHIDescriptorWrite::StorageBuffer(
                meshletSet, 6, m_PendingCounterBuffers[i], 0,
                kPendingCounterBytes);
            meshletWrites[7] = FRHIDescriptorWrite::UniformBuffer(
                meshletSet, 8, m_ViewBuffers[i], 0, kViewBytes);

            device->UpdateDescriptorSets(meshletWrites, 8);

            // 金字塔那一格先指向 1x1 的兜底图。真的金字塔就绪之后
            // Execute 会补写。
            const FRHIDescriptorWrite hizWrite =
                FRHIDescriptorWrite::CombinedImageSampler(
                    meshletSet, 7, m_DummyHizView, m_HizDefaultSampler,
                    EImageLayout::ShaderReadOnly);

            device->UpdateDescriptorSets(&hizWrite, 1);

            m_MeshletCullSets.Add(meshletSet);
        }
    }

    // ---- 管线 ----
    {
        FShaderManager& shaders = FShaderManager::Get();

        if (!shaders.IsInitialized())
        {
            shaders.Initialize();
        }

        ERHIResult result = shaders.CreateShaderModule(
            device, FString("Builtin/meshlet_cull_instance.comp"),
            EShaderStage::Compute, m_InstanceCullShader);

        if (!IsRHISuccess(result))
        {
            return result;
        }

        result = shaders.CreateShaderModule(
            device, FString("Builtin/meshlet_cull.comp"),
            EShaderStage::Compute, m_MeshletCullShader);

        if (!IsRHISuccess(result))
        {
            return result;
        }

        FRHIPushConstantRange pushRange = {};
        pushRange.StageFlags = EShaderStage::Compute;
        pushRange.Offset     = 0;
        pushRange.Size       = sizeof(FCullPushConstants);

        FRHIPipelineLayoutDesc layoutDesc = {};
        layoutDesc.SetLayouts             = &m_InstanceCullSetLayout;
        layoutDesc.SetLayoutCount         = 1;
        layoutDesc.PushConstantRanges     = &pushRange;
        layoutDesc.PushConstantRangeCount = 1;
        layoutDesc.DebugName              = "MeshletInstanceCullLayout";

        result =
            device->CreatePipelineLayout(layoutDesc, m_InstanceCullLayout);

        if (!IsRHISuccess(result))
        {
            return result;
        }

        layoutDesc.SetLayouts     = &m_MeshletCullSetLayout;
        layoutDesc.DebugName      = "MeshletCullLayout";

        result = device->CreatePipelineLayout(layoutDesc, m_MeshletCullLayout);

        if (!IsRHISuccess(result))
        {
            return result;
        }

        FRHIComputePipelineDesc pipelineDesc = {};
        pipelineDesc.ComputeShader.Shader     = m_InstanceCullShader;
        pipelineDesc.ComputeShader.Stage      = EShaderStage::Compute;
        pipelineDesc.ComputeShader.EntryPoint = "main";
        pipelineDesc.PipelineLayout           = m_InstanceCullLayout;
        pipelineDesc.DebugName                = "MeshletInstanceCullPipeline";

        result = device->CreateComputePipeline(pipelineDesc,
                                               m_InstanceCullPipeline);

        if (!IsRHISuccess(result))
        {
            return result;
        }

        pipelineDesc.ComputeShader.Shader = m_MeshletCullShader;
        pipelineDesc.PipelineLayout       = m_MeshletCullLayout;
        pipelineDesc.DebugName            = "MeshletCullPipeline";

        result = device->CreateComputePipeline(pipelineDesc,
                                               m_MeshletCullPipeline);

        if (!IsRHISuccess(result))
        {
            return result;
        }
    }

    LIMX_LOG(LogRenderer, Log,
             "[MeshletCull] 初始化完成 — 场景 meshlet 上限 {}, 实例上限 {}",
             kMaxSceneMeshlets, kMaxMeshletInstances);

    return ERHIResult::Success;
}

// ============================================================================
// RebuildSceneMeshlets — 把场景里所有网格的 meshlet 汇总成一份连续缓冲区
//
// 每个网格自带一份 meshlet 缓冲区 (FRenderResourceManager 在建网格时一起
// 建的), 而剔除要一次分派处理整个场景 —— 那需要一份连续的数组。
//
// 用 GPU 到 GPU 的拷贝而不是让 CPU 重新上传: meshlet 数据已经在显存里了,
// 拷贝是显存内部的事, 不经过 PCIe。
//
// 只在几何签名变化时重做。签名只看"有哪些 meshlet 缓冲区", **不看变换** ——
// 物体移动不改变 meshlet。每帧重做的话是几十次拷贝加一次全屏障。
// ============================================================================

bool FMeshletCullPass::RebuildSceneMeshlets(
    IRHIDevice* device, IRHICommandBuffer* commandBuffer,
    const TArray<FRenderObject>& objects)
{
    // FNV-1a over (缓冲区下标, 代际, meshlet 区间)
    UInt64 signature = 1469598103934665603ull;

    const auto Mix = [&signature](UInt64 value)
    {
        for (UInt32 byte = 0; byte < 8; ++byte)
        {
            signature ^= (value >> (byte * 8)) & 0xFFull;
            signature *= 1099511628211ull;
        }
    };

    Mix(objects.GetSize());

    for (SizeType i = 0; i < objects.GetSize(); ++i)
    {
        Mix(objects[i].MeshletBuffer.GetHash());
        Mix(objects[i].MeshletOffset);
        Mix(objects[i].MeshletCount);
    }

    if (signature == m_GeometrySignature && m_SceneMeshletCount > 0)
    {
        return true;
    }

    m_SourceBuffers.Clear();
    m_SourceBases.Clear();
    m_SourceVertexBases.Clear();
    m_SourceMeshletVertexBases.Clear();
    m_SourceMeshletTriangleBases.Clear();

    UInt32 base = 0;
    UInt32 vertexBase = 0;
    UInt32 meshletVertexBase = 0;
    UInt32 meshletTriangleByteBase = 0;

    for (SizeType i = 0; i < objects.GetSize(); ++i)
    {
        const FRenderObject& object = objects[i];

        if (!object.MeshletBuffer.IsValid() || object.MeshletCount == 0)
        {
            continue;
        }

        // 同一个网格的多个批次共用一份缓冲区 —— 只拷一次
        bool seen = false;

        for (SizeType s = 0; s < m_SourceBuffers.GetSize(); ++s)
        {
            if (m_SourceBuffers[s] == object.MeshletBuffer)
            {
                seen = true;
                break;
            }
        }

        if (seen)
        {
            continue;
        }

        // 这个网格的 meshlet 总数不知道 (对象只知道自己批次那一段), 所以
        // 按"见过的最大 (偏移 + 个数)"来定。同一网格的批次都会走到这里,
        // 但只有第一个会被记下 —— 所以先扫一遍求出总数。
        UInt32 total = 0;

        for (SizeType j = 0; j < objects.GetSize(); ++j)
        {
            if (objects[j].MeshletBuffer != object.MeshletBuffer)
            {
                continue;
            }

            total = FMath::Max(total, objects[j].MeshletOffset +
                                          objects[j].MeshletCount);
        }

        // meshlet 三角形按四字节一组打包上传, 所以字节数要向上取整到 4。
        // 不取整的话下一个网格的起点落在字上不对齐的位置, 而着色器是按
        // uint 数组读的 —— 取到的是两个相邻字节拼出来的垃圾。
        const UInt32 triangleBytes =
            ((object.MeshletTriangleTotal * 3u) + 3u) & ~3u;

        if (base + total > kMaxSceneMeshlets ||
            vertexBase + object.VertexCount > kMaxSceneVertices ||
            meshletVertexBase + object.MeshletVertexTotal >
                kMaxSceneMeshletVertices ||
            (meshletTriangleByteBase + triangleBytes) / 4u >
                kMaxSceneMeshletTriangleWords)
        {
            LIMX_LOG(LogRenderer, Error,
                     "[MeshletCull] 场景数据超过汇总缓冲区上限 — "
                     "多出的网格不会被剔除也不会被画 "
                     "(meshlet {}/{}, 顶点 {}/{})",
                     base + total, kMaxSceneMeshlets,
                     vertexBase + object.VertexCount, kMaxSceneVertices);
            break;
        }

        // 四次拷贝, 四个独立的基址。
        //
        // 基址记在实例表里而不是改写数据里的偏移量: 改写要在 GPU 上再跑
        // 一遍重定位 (或者把数据搬回 CPU), 而基址只是每个实例多三个 uint,
        // 汇总本身仍然是纯粹的缓冲区拷贝。
        const auto Copy = [commandBuffer](FRHIBufferHandle source,
                                          FRHIBufferHandle target,
                                          UInt64 dstOffset, UInt64 size)
        {
            if (size == 0 || !source.IsValid())
            {
                return;
            }

            FRHIBufferCopyRegion region = {};
            region.SrcOffset = 0;
            region.DstOffset = dstOffset;
            region.Size      = size;

            commandBuffer->CopyBuffer(source, target, region);
        };

        Copy(object.MeshletBuffer, m_SceneMeshlets,
             static_cast<UInt64>(base) * sizeof(FMeshlet),
             static_cast<UInt64>(total) * sizeof(FMeshlet));

        Copy(object.VertexBuffer, m_SceneVertices,
             static_cast<UInt64>(vertexBase) * sizeof(FMeshVertex),
             static_cast<UInt64>(object.VertexCount) * sizeof(FMeshVertex));

        Copy(object.MeshletVertexBuffer, m_SceneMeshletVertices,
             static_cast<UInt64>(meshletVertexBase) * sizeof(UInt32),
             static_cast<UInt64>(object.MeshletVertexTotal) * sizeof(UInt32));

        Copy(object.MeshletTriangleBuffer, m_SceneMeshletTriangles,
             static_cast<UInt64>(meshletTriangleByteBase), triangleBytes);

        m_SourceBuffers.Add(object.MeshletBuffer);
        m_SourceBases.Add(base);
        m_SourceVertexBases.Add(vertexBase);
        m_SourceMeshletVertexBases.Add(meshletVertexBase);
        m_SourceMeshletTriangleBases.Add(meshletTriangleByteBase);

        base += total;
        vertexBase += object.VertexCount;
        meshletVertexBase += object.MeshletVertexTotal;
        meshletTriangleByteBase += triangleBytes;
    }

    m_SceneMeshletCount = base;
    m_GeometrySignature = signature;

    // 拷完要挡一次: 下面的计算着色器要读它
    {
        FRHIBufferMemoryBarrier barriers[4] = {};

        const FRHIBufferHandle targets[4] = {
            m_SceneMeshlets, m_SceneVertices, m_SceneMeshletVertices,
            m_SceneMeshletTriangles,
        };

        for (UInt32 i = 0; i < 4; ++i)
        {
            barriers[i].SrcAccessMask = EAccessFlags::TransferWrite;
            barriers[i].DstAccessMask = EAccessFlags::ShaderRead;
            barriers[i].Buffer        = targets[i];
        }

        // 目标阶段要把顶点着色器与网格着色器也算上 —— 光栅化路径在同一帧
        // 里读这几份数据。只写 ComputeShader 的话, 验证层会报, 而关掉验证
        // 层就是竞态: 光栅化读到的可能是拷贝完成之前的内容。
        commandBuffer->PipelineBarrier(
            EPipelineStageFlags::Transfer,
            EPipelineStageFlags::ComputeShader |
                EPipelineStageFlags::VertexShader |
                EPipelineStageFlags::MeshShader,
            nullptr, 0, barriers, 4, nullptr, 0);
    }

    LIMX_LOG(LogRenderer, Log,
             "[MeshletCull] 场景 meshlet 汇总完成 — {} 个网格, {} 个 meshlet",
             m_SourceBuffers.GetSize(), m_SceneMeshletCount);

    LIMX_UNUSED(device);

    return true;
}

// ============================================================================
// BuildInstances
// ============================================================================

void FMeshletCullPass::BuildInstances(const TArray<FRenderObject>& objects)
{
    m_Instances.Clear();
    m_InstanceSpheres.Clear();

    for (SizeType i = 0; i < objects.GetSize(); ++i)
    {
        const FRenderObject& object = objects[i];

        if (!object.MeshletBuffer.IsValid() || object.MeshletCount == 0)
        {
            continue;
        }

        // 只收**不透明**批次。
        //
        // 蒙版材质要 alpha 测试, 而 meshlet 的光栅化路径现在没有地方放它
        // (材质解析是后面的事)。收进来的话画出的深度会比经典路径**近** ——
        // 挖掉的那些洞被画成了实心。
        //
        // 排除之后本路径的三角形集合是经典深度预通道的**子集**, 于是每个
        // 像素上本路径的深度只能等于或者远于经典路径的 —— 而那是一条不需要
        // 任何容差的判据。
        if (object.BlendMode != EMaterialBlendMode::Opaque)
        {
            continue;
        }

        if (m_Instances.GetSize() >= kMaxMeshletInstances)
        {
            LIMX_LOG(LogRenderer, Error,
                     "[MeshletCull] 实例数超过上限 {}", kMaxMeshletInstances);
            break;
        }

        // 这个网格在四份汇总缓冲区里的起点
        UInt32 meshBase = 0;
        UInt32 vertexBase = 0;
        UInt32 meshletVertexBase = 0;
        UInt32 meshletTriangleByteBase = 0;

        bool found = false;

        for (SizeType s = 0; s < m_SourceBuffers.GetSize(); ++s)
        {
            if (m_SourceBuffers[s] == object.MeshletBuffer)
            {
                meshBase                = m_SourceBases[s];
                vertexBase              = m_SourceVertexBases[s];
                meshletVertexBase       = m_SourceMeshletVertexBases[s];
                meshletTriangleByteBase = m_SourceMeshletTriangleBases[s];
                found                   = true;
                break;
            }
        }

        // 汇总时被上限截断掉的网格 —— 跳过而不是指向 0。
        // 指向 0 的话它会拿别人的 meshlet 来画。
        if (!found)
        {
            continue;
        }

        FMeshletInstanceGpu instance;

        const FMatrix model = object.Transform.ToMatrix();

        // FMatrix 是行主序 M[行][列], 着色器那边的三行也是行主序 ——
        // 直接逐元素搬, 不转置。与 FRayTracingScene 里那一段同理:
        // 转置了的话物体会绕原点乱转, 而单位变换下转置又是恒等的,
        // 于是只有旋转过的物体才出错。
        for (UInt32 row = 0; row < 3; ++row)
        {
            Float32* target = (row == 0)   ? instance.TransformRow0
                              : (row == 1) ? instance.TransformRow1
                                           : instance.TransformRow2;

            for (UInt32 col = 0; col < 4; ++col)
            {
                target[col] = model.M[row][col];
            }
        }

        instance.MeshletRange[0] = meshBase + object.MeshletOffset;
        instance.MeshletRange[1] = object.MeshletCount;
        instance.MeshletRange[2] = static_cast<UInt32>(i);
        instance.MeshletRange[3] = 0;

        instance.BufferBases[0] = vertexBase;
        instance.BufferBases[1] = meshletVertexBase;
        instance.BufferBases[2] = meshletTriangleByteBase;
        instance.BufferBases[3] = 0;

        m_Instances.Add(instance);

        // 实例的世界包围球 —— 由世界包围盒外接, 再加上最大的 meshlet 半径
        //
        // 外接而不是内切: 内切球会漏掉盒角上的几何体, 那是"画面上少一块"。
        //
        // 加最大 meshlet 半径这一项, 是判据逼出来的。第一版只外接包围盒,
        // 而**那不够**: meshlet 的包围球会从包围盒的角上鼓出去, 于是一个
        // 刚好被第一级剔掉的实例, 它的某个 meshlet 其实还与视锥相交。
        // 实测综合场景里有 2 个 meshlet 就这样被误剔 —— 而第一级剔掉的
        // 实例根本不进第二级, "GPU 与 CPU 参考实现一致"这条判据对它一个字
        // 都不会说。
        //
        // 加上之后是**可证明**的包含: 每个 meshlet 的球心在包围盒内 (距
        // 盒心不超过半对角线), 球最多再向外伸出自己的半径, 而这里取的是
        // 全部 meshlet 半径的上界。
        const FVector3 center = object.WorldBounds.GetCenter();
        const FVector3 extent = object.WorldBounds.GetExtent();

        const FVector3 scale = object.Transform.Scale3D;

        const Float32 maxScale = FMath::Max(
            FMath::Abs(scale.X),
            FMath::Max(FMath::Abs(scale.Y), FMath::Abs(scale.Z)));

        const Float32 radius =
            FMath::Sqrt(extent.X * extent.X + extent.Y * extent.Y +
                        extent.Z * extent.Z) +
            object.MaxMeshletRadius * maxScale;

        m_InstanceSpheres.Add(center.X);
        m_InstanceSpheres.Add(center.Y);
        m_InstanceSpheres.Add(center.Z);
        m_InstanceSpheres.Add(radius);
    }
}

// ============================================================================
// Execute
// ============================================================================

void FMeshletCullPass::Execute(IRHICommandBuffer*        commandBuffer,
                               const FRenderPassContext& context)
{
    if (m_Device == nullptr || context.ShadowCasterObjects == nullptr)
    {
        return;
    }

    // 统计的回读隔着并行帧数 —— 读的是同一个下标上一轮写的值。
    // 立刻读的话拿到的是这一帧还没写完的内容。
    {
        void* mapped = nullptr;

        if (IsRHISuccess(m_Device->MapBuffer(
                m_CounterReadbacks[context.FrameIndex], &mapped)) &&
            mapped != nullptr)
        {
            const auto* counters = static_cast<const UInt32*>(mapped);

            m_Stats.MeshletsVisible          = counters[0];
            m_Stats.MeshletsCulledByFrustum  = counters[1];
            m_Stats.MeshletsCulledByBackface = counters[2];
            m_Stats.MeshletsTested           = counters[3];

            // 待定数在另一份缓冲区里, 见下面那段。
            m_Stats.MeshletsPending = m_LastPendingCount;

            m_Device->UnmapBuffer(m_CounterReadbacks[context.FrameIndex]);
        }

        void* pendingMapped = nullptr;

        if (IsRHISuccess(m_Device->MapBuffer(
                m_PendingReadbacks[context.FrameIndex], &pendingMapped)) &&
            pendingMapped != nullptr)
        {
            m_LastPendingCount =
                static_cast<const UInt32*>(pendingMapped)[0];

            m_Device->UnmapBuffer(m_PendingReadbacks[context.FrameIndex]);
        }
    }

    if (!m_Enabled)
    {
        return;
    }

    commandBuffer->BeginDebugLabel("MeshletCullPass", 0.9f, 0.6f, 0.3f);

    // 首帧把兜底金字塔清成 1.0 (最远)。
    //
    // Setup 里没有命令缓冲区可用, 所以只能挪到这里。不清的话它是未初始化
    // 的显存 —— 虽然开关关着时着色器不会去采它, 但"靠一个分支不被走到来
    // 保证正确"是本周期反复否定过的那类推理。
    if (!m_DummyHizInitialized)
    {
        commandBuffer->TransitionImageLayout(
            m_DummyHizTexture, EImageLayout::Undefined,
            EImageLayout::TransferDst, EPipelineStageFlags::TopOfPipe,
            EPipelineStageFlags::Transfer, EAccessFlags::None,
            EAccessFlags::TransferWrite);

        FRHIBufferTextureCopyRegion region = {};
        region.BufferOffset =
            kCounterBytes + kDispatchBytes + kRasterArgsBytes;
        region.BufferRowLength   = 0;
        region.BufferImageHeight = 0;
        region.MipLevel          = 0;
        region.BaseLayer         = 0;
        region.LayerCount        = 1;
        region.TextureOffset     = { 0, 0, 0 };
        region.TextureExtent     = { 1, 1, 1 };

        commandBuffer->CopyBufferToTexture(m_ResetSource, m_DummyHizTexture,
                                           EImageLayout::TransferDst, region);

        commandBuffer->TransitionImageLayout(
            m_DummyHizTexture, EImageLayout::TransferDst,
            EImageLayout::ShaderReadOnly, EPipelineStageFlags::Transfer,
            EPipelineStageFlags::ComputeShader, EAccessFlags::TransferWrite,
            EAccessFlags::ShaderRead);

        m_DummyHizInitialized = true;
    }

    const UInt32 frameIndex = context.FrameIndex;

    // 剔除的输入取**未经相机剔除**的那份列表。
    //
    // 用剔除后的列表的话, GPU 剔除永远剔不掉任何实例 —— 一个什么都不做的
    // 实现会得到完全正确的结果, 判据也就无从判定。这一条与 FGpuCullPass
    // 同理, 是那一天写下来的。
    const TArray<FRenderObject>& objects = *context.ShadowCasterObjects;

    RebuildSceneMeshlets(m_Device, commandBuffer, objects);

    BuildInstances(objects);

    m_Stats.InstancesTotal = static_cast<UInt32>(m_Instances.GetSize());

    if (m_Instances.IsEmpty())
    {
        commandBuffer->EndDebugLabel();
        return;
    }

    // ---- 上传实例表 ----
    {
        void* mapped = nullptr;

        if (IsRHISuccess(
                m_Device->MapBuffer(m_InstanceBuffers[frameIndex], &mapped)) &&
            mapped != nullptr)
        {
            auto* target = static_cast<FMeshletInstanceGpu*>(mapped);

            for (SizeType i = 0; i < m_Instances.GetSize(); ++i)
            {
                target[i] = m_Instances[i];
            }

            m_Device->UnmapBuffer(m_InstanceBuffers[frameIndex]);
        }

        mapped = nullptr;

        if (IsRHISuccess(m_Device->MapBuffer(
                m_InstanceSphereBuffers[frameIndex], &mapped)) &&
            mapped != nullptr)
        {
            auto* target = static_cast<Float32*>(mapped);

            for (SizeType i = 0; i < m_InstanceSpheres.GetSize(); ++i)
            {
                target[i] = m_InstanceSpheres[i];
            }

            m_Device->UnmapBuffer(m_InstanceSphereBuffers[frameIndex]);
        }
    }

    // ---- 计数器与分派参数归零 ----
    //
    // 分派参数的 y/z 必须是 1 —— 它是 vkCmdDispatchIndirect 的三个维度。
    // 全写 0 的话第二级一个工作组都不起, 而那与"什么都不可见"分不开。
    {
        FRHIBufferCopyRegion counterRegion = {};
        counterRegion.SrcOffset = 0;
        counterRegion.DstOffset = 0;
        counterRegion.Size      = kCounterBytes;

        commandBuffer->CopyBuffer(m_ResetSource,
                                  m_CounterBuffers[frameIndex],
                                  counterRegion);

        FRHIBufferCopyRegion dispatchRegion = {};
        dispatchRegion.SrcOffset = kCounterBytes;
        dispatchRegion.DstOffset = 0;
        dispatchRegion.Size      = kDispatchBytes;

        commandBuffer->CopyBuffer(m_ResetSource,
                                  m_DispatchBuffers[frameIndex],
                                  dispatchRegion);

        FRHIBufferCopyRegion rasterRegion = {};
        rasterRegion.SrcOffset = kCounterBytes + kDispatchBytes;
        rasterRegion.DstOffset = 0;
        rasterRegion.Size      = kRasterArgsBytes;

        commandBuffer->CopyBuffer(m_ResetSource,
                                  m_RasterArgsBuffers[frameIndex],
                                  rasterRegion);

        // 待定表的计数器也要归零 —— 它与可见计数器同布局, 拷同一段。
        FRHIBufferCopyRegion pendingRegion = {};
        pendingRegion.SrcOffset = 0;
        pendingRegion.DstOffset = 0;
        pendingRegion.Size      = kPendingCounterBytes;

        commandBuffer->CopyBuffer(m_ResetSource,
                                  m_PendingCounterBuffers[frameIndex],
                                  pendingRegion);

        FRHIBufferMemoryBarrier barriers[3] = {};

        for (UInt32 i = 0; i < 3; ++i)
        {
            barriers[i].SrcAccessMask = EAccessFlags::TransferWrite;
            barriers[i].DstAccessMask =
                EAccessFlags::ShaderRead | EAccessFlags::ShaderWrite;
        }

        barriers[0].Buffer = m_CounterBuffers[frameIndex];
        barriers[1].Buffer = m_DispatchBuffers[frameIndex];
        barriers[2].Buffer = m_PendingCounterBuffers[frameIndex];

        commandBuffer->PipelineBarrier(EPipelineStageFlags::Transfer,
                                       EPipelineStageFlags::ComputeShader |
                                           EPipelineStageFlags::Transfer,
                                       nullptr, 0, barriers, 3, nullptr, 0);
    }

    const FFrustum& frustum = m_Frustum;

    // ---- 逐视图 UBO ----
    //
    // 三个剔除着色器读同一份。分开传的话, 实例级与 meshlet 级用了不同的
    // 视锥这种事会悄悄发生 —— 而它只在物体压着视锥边界时现形。
    {
        void* mapped = nullptr;

        if (IsRHISuccess(m_Device->MapBuffer(m_ViewBuffers[frameIndex],
                                             &mapped)) &&
            mapped != nullptr)
        {
            auto* view = static_cast<FMeshletCullViewGpu*>(mapped);

            for (Int32 p = 0; p < FFrustum::kPlaneCount; ++p)
            {
                view->Planes[p][0] = frustum.Planes[p].Normal.X;
                view->Planes[p][1] = frustum.Planes[p].Normal.Y;
                view->Planes[p][2] = frustum.Planes[p].Normal.Z;
                view->Planes[p][3] = frustum.Planes[p].D;
            }

            const FMatrix viewProj =
                (context.Camera != nullptr)
                    ? (context.Camera->GetProjectionMatrix() *
                       context.Camera->GetViewMatrix())
                    : FMatrix::kIdentity;

            for (UInt32 row = 0; row < 4; ++row)
            {
                for (UInt32 col = 0; col < 4; ++col)
                {
                    view->ViewProj[row * 4 + col] = viewProj.M[row][col];
                }
            }

            view->CameraPosition[0] = m_CameraPosition.X;
            view->CameraPosition[1] = m_CameraPosition.Y;
            view->CameraPosition[2] = m_CameraPosition.Z;

            view->HizParams[0] = static_cast<Float32>(m_HizWidth);
            view->HizParams[1] = static_cast<Float32>(m_HizHeight);
            view->HizParams[2] =
                (m_HizLevels > 0) ? static_cast<Float32>(m_HizLevels - 1) : 0.0f;
            view->HizParams[3] = (context.Camera != nullptr)
                                     ? context.Camera->GetNearPlane()
                                     : 0.1f;

            m_Device->UnmapBuffer(m_ViewBuffers[frameIndex]);
        }
    }

    // ---- 金字塔换了才补写描述符 ----
    //
    // 只改**当前帧**那一个集。别的帧下标的集可能正被在飞的命令缓冲区用着,
    // 而 vkUpdateDescriptorSets 不允许改在用的集 —— 验证层会明确报出来。
    //
    // 每帧无条件重写也行, 但那会掩盖"金字塔换了而描述符没换"这类错误。
    if (m_BoundHizViews.GetSize() < m_MeshletCullSets.GetSize())
    {
        m_BoundHizViews.SetSize(m_MeshletCullSets.GetSize(),
                                FRHITextureViewHandle());
    }

    if (m_HizView.IsValid() && frameIndex < m_BoundHizViews.GetSize() &&
        m_BoundHizViews[frameIndex] != m_HizView)
    {
        const FRHIDescriptorWrite write =
            FRHIDescriptorWrite::CombinedImageSampler(
                m_MeshletCullSets[frameIndex], 7, m_HizView, m_HizSampler,
                EImageLayout::ShaderReadOnly);

        m_Device->UpdateDescriptorSets(&write, 1);

        m_BoundHizViews[frameIndex] = m_HizView;
    }

    // ---- 第一级: 实例 ----
    {
        commandBuffer->BindComputePipeline(m_InstanceCullPipeline);
        commandBuffer->BindDescriptorSet(EPipelineBindPoint::Compute,
                                         m_InstanceCullLayout, 0,
                                         m_InstanceCullSets[frameIndex]);

        FCullPushConstants push;

        push.Params[0] = static_cast<UInt32>(m_Instances.GetSize());
        push.Params[1] = kMaxMeshletInstances;

        commandBuffer->PushConstants(m_InstanceCullLayout,
                                     EShaderStage::Compute, 0, sizeof(push),
                                     &push);

        const UInt32 groupCount =
            (static_cast<UInt32>(m_Instances.GetSize()) + kCullWorkgroupSize -
             1u) /
            kCullWorkgroupSize;

        commandBuffer->Dispatch(groupCount, 1, 1);
    }

    // ---- 屏障: 第二级要读第一级写的可见实例表与分派参数 ----
    //
    // 分派参数的目标阶段是 DrawIndirect 而不是 ComputeShader —— 它被
    // 命令处理器当分派参数读, 那是另一个阶段。写成 ComputeShader 的话
    // 验证层会报, 而关掉验证层就是竞态: 分派用的可能是归零后的值。
    {
        FRHIBufferMemoryBarrier barrier = {};
        barrier.SrcAccessMask = EAccessFlags::ShaderWrite;
        barrier.DstAccessMask = EAccessFlags::ShaderRead;
        barrier.Buffer        = m_VisibleInstanceBuffers[frameIndex];

        FRHIBufferMemoryBarrier indirectBarrier = {};
        indirectBarrier.SrcAccessMask = EAccessFlags::ShaderWrite;
        indirectBarrier.DstAccessMask = EAccessFlags::IndirectCommandRead |
                                        EAccessFlags::ShaderRead;
        indirectBarrier.Buffer = m_DispatchBuffers[frameIndex];

        FRHIBufferMemoryBarrier barriers[2] = { barrier, indirectBarrier };

        commandBuffer->PipelineBarrier(
            EPipelineStageFlags::ComputeShader,
            EPipelineStageFlags::ComputeShader |
                EPipelineStageFlags::DrawIndirect,
            nullptr, 0, barriers, 2, nullptr, 0);
    }

    // ---- 第二级: meshlet ----
    {
        commandBuffer->BindComputePipeline(m_MeshletCullPipeline);
        commandBuffer->BindDescriptorSet(EPipelineBindPoint::Compute,
                                         m_MeshletCullLayout, 0,
                                         m_MeshletCullSets[frameIndex]);

        FCullPushConstants push;

        // x 由 DispatchIndirect 从缓冲区取, 这里给的是着色器自己用来
        // 判越界的那个数 —— 两者必须是同一个值, 而着色器读不到分派参数。
        // 所以这里给上限, 让越界判断退化成"不越界"; 真正的边界由
        // 可见实例表的容量保证 (第一级已经卡过一次)。
        push.Params[0] = kMaxMeshletInstances;
        push.Params[1] = kMaxSceneMeshlets;
        push.Params[2] = m_BackfaceCull ? 1u : 0u;

        // 遮挡剔除要金字塔就绪才开。
        //
        // 没就绪时传 0 而不是"让着色器判一下": 描述符集里那一格还没写过,
        // 而 Vulkan 不允许提交带未写绑定的描述符集 —— 着色器里判不判都
        // 已经晚了。
        push.Params[3] =
            (m_OcclusionCull && m_HizView.IsValid()) ? 1u : 0u;

        commandBuffer->PushConstants(m_MeshletCullLayout,
                                     EShaderStage::Compute, 0, sizeof(push),
                                     &push);

        commandBuffer->DispatchIndirect(m_DispatchBuffers[frameIndex], 0);
    }

    // ---- 待定数回读 ----
    //
    // 与可见计数器同一条路: 拷进一份 GpuToCpu 的缓冲区, 下一轮同一个帧
    // 下标再读。判据要拿它判"遮挡剔除是不是真的剔掉了东西" —— 没有这个数
    // 的话, 一个什么都不剔的实现在"开关画面相同"那两条判据上满分通过。
    {
        FRHIBufferMemoryBarrier barrier = {};
        barrier.SrcAccessMask = EAccessFlags::ShaderWrite;
        barrier.DstAccessMask = EAccessFlags::TransferRead;
        barrier.Buffer        = m_PendingCounterBuffers[frameIndex];

        commandBuffer->PipelineBarrier(EPipelineStageFlags::ComputeShader,
                                       EPipelineStageFlags::Transfer, nullptr,
                                       0, &barrier, 1, nullptr, 0);

        FRHIBufferCopyRegion region = {};
        region.SrcOffset = 0;
        region.DstOffset = 0;
        region.Size      = kPendingCounterBytes;

        commandBuffer->CopyBuffer(m_PendingCounterBuffers[frameIndex],
                                  m_PendingReadbacks[frameIndex], region);
    }

    // ---- 可见数 -> 光栅化的间接参数, 以及计数器回读 ----
    {
        FRHIBufferMemoryBarrier barrier = {};
        barrier.SrcAccessMask = EAccessFlags::ShaderWrite;
        barrier.DstAccessMask = EAccessFlags::TransferRead;
        barrier.Buffer        = m_CounterBuffers[frameIndex];

        commandBuffer->PipelineBarrier(EPipelineStageFlags::ComputeShader,
                                       EPipelineStageFlags::Transfer, nullptr,
                                       0, &barrier, 1, nullptr, 0);

        // 只拷第一个 uint (可见数) —— 后面的 y/z 是复位时写进去的 1。
        //
        // 整个拷过去的话 y/z 会被诊断计数 (视锥剔了多少、背面剔了多少)
        // 覆盖, 于是间接分派的 Y 维变成几十, 同一批 meshlet 被画几十遍。
        FRHIBufferCopyRegion argsRegion = {};
        argsRegion.SrcOffset = 0;
        argsRegion.DstOffset = 0;
        argsRegion.Size      = sizeof(UInt32);

        commandBuffer->CopyBuffer(m_CounterBuffers[frameIndex],
                                  m_RasterArgsBuffers[frameIndex], argsRegion);

        FRHIBufferMemoryBarrier argsBarrier = {};
        argsBarrier.SrcAccessMask = EAccessFlags::TransferWrite;
        argsBarrier.DstAccessMask = EAccessFlags::IndirectCommandRead;
        argsBarrier.Buffer        = m_RasterArgsBuffers[frameIndex];

        commandBuffer->PipelineBarrier(EPipelineStageFlags::Transfer,
                                       EPipelineStageFlags::DrawIndirect,
                                       nullptr, 0, &argsBarrier, 1, nullptr,
                                       0);

        FRHIBufferCopyRegion region = {};
        region.SrcOffset = 0;
        region.DstOffset = 0;
        region.Size      = kCounterBytes;

        commandBuffer->CopyBuffer(m_CounterBuffers[frameIndex],
                                  m_CounterReadbacks[frameIndex], region);
    }

    commandBuffer->EndDebugLabel();
}

// ============================================================================
// 其余接口
// ============================================================================

ERHIResult FMeshletCullPass::OnResize(const FPassResizeDesc& desc)
{
    LIMX_UNUSED(desc);

    // meshlet 与分辨率无关
    return ERHIResult::Success;
}

void FMeshletCullPass::ReleaseSwapchainResources(IRHIDevice* device)
{
    LIMX_UNUSED(device);
}

void FMeshletCullPass::Shutdown(IRHIDevice* device)
{
    if (device == nullptr)
    {
        return;
    }

    if (m_MeshletCullShader.IsValid())
    {
        device->DestroyShader(m_MeshletCullShader);
    }

    if (m_InstanceCullShader.IsValid())
    {
        device->DestroyShader(m_InstanceCullShader);
    }

    if (m_MeshletCullPipeline.IsValid())
    {
        device->DestroyComputePipeline(m_MeshletCullPipeline);
    }

    if (m_InstanceCullPipeline.IsValid())
    {
        device->DestroyComputePipeline(m_InstanceCullPipeline);
    }

    if (m_MeshletCullLayout.IsValid())
    {
        device->DestroyPipelineLayout(m_MeshletCullLayout);
    }

    if (m_InstanceCullLayout.IsValid())
    {
        device->DestroyPipelineLayout(m_InstanceCullLayout);
    }

    if (m_MeshletCullSetLayout.IsValid())
    {
        device->DestroyDescSetLayout(m_MeshletCullSetLayout);
    }

    if (m_InstanceCullSetLayout.IsValid())
    {
        device->DestroyDescSetLayout(m_InstanceCullSetLayout);
    }

    const auto DestroyAll = [device](TArray<FRHIBufferHandle>& buffers)
    {
        for (SizeType i = 0; i < buffers.GetSize(); ++i)
        {
            if (buffers[i].IsValid())
            {
                device->DestroyBuffer(buffers[i]);
            }
        }

        buffers.Clear();
    };

    DestroyAll(m_RasterArgsBuffers);
    DestroyAll(m_PendingBuffers);
    DestroyAll(m_PendingCounterBuffers);
    DestroyAll(m_PendingReadbacks);
    DestroyAll(m_ViewBuffers);
    DestroyAll(m_CounterReadbacks);
    DestroyAll(m_CounterBuffers);
    DestroyAll(m_VisibleMeshletBuffers);
    DestroyAll(m_DispatchBuffers);
    DestroyAll(m_VisibleInstanceBuffers);
    DestroyAll(m_InstanceSphereBuffers);
    DestroyAll(m_InstanceBuffers);

    FRHIBufferHandle* const sceneBuffers[4] = {
        &m_SceneMeshlets, &m_SceneVertices, &m_SceneMeshletVertices,
        &m_SceneMeshletTriangles,
    };

    for (UInt32 i = 0; i < 4; ++i)
    {
        if (sceneBuffers[i]->IsValid())
        {
            device->DestroyBuffer(*sceneBuffers[i]);
        }
    }

    if (m_ResetSource.IsValid())
    {
        device->DestroyBuffer(m_ResetSource);
    }

    if (m_HizDefaultSampler.IsValid())
    {
        device->DestroySampler(m_HizDefaultSampler);
    }

    if (m_DummyHizView.IsValid())
    {
        device->DestroyTextureView(m_DummyHizView);
    }

    if (m_DummyHizTexture.IsValid())
    {
        device->DestroyTexture(m_DummyHizTexture);
    }

    m_InstanceCullSets.Clear();
    m_MeshletCullSets.Clear();

    m_Device = nullptr;
}

FRHIBufferHandle FMeshletCullPass::GetVisibleMeshletBuffer(
    UInt32 frameIndex) const
{
    return (frameIndex < m_VisibleMeshletBuffers.GetSize())
               ? m_VisibleMeshletBuffers[frameIndex]
               : FRHIBufferHandle();
}

FRHIBufferHandle FMeshletCullPass::GetCounterBuffer(UInt32 frameIndex) const
{
    return (frameIndex < m_CounterBuffers.GetSize())
               ? m_CounterBuffers[frameIndex]
               : FRHIBufferHandle();
}

FRHIBufferHandle FMeshletCullPass::GetRasterArgsBuffer(
    UInt32 frameIndex) const
{
    return (frameIndex < m_RasterArgsBuffers.GetSize())
               ? m_RasterArgsBuffers[frameIndex]
               : FRHIBufferHandle();
}

FRHIBufferHandle FMeshletCullPass::GetInstanceBuffer(UInt32 frameIndex) const
{
    return (frameIndex < m_InstanceBuffers.GetSize())
               ? m_InstanceBuffers[frameIndex]
               : FRHIBufferHandle();
}

void FMeshletCullPass::SetHizPyramid(FRHITextureViewHandle view,
                                     FRHISamplerHandle sampler, UInt32 width,
                                     UInt32 height, UInt32 levelCount)
{
    m_HizView    = view;
    m_HizSampler = sampler;
    m_HizWidth   = width;
    m_HizHeight  = height;
    m_HizLevels  = levelCount;
}

FRHIBufferHandle FMeshletCullPass::GetPendingBuffer(UInt32 frameIndex) const
{
    return (frameIndex < m_PendingBuffers.GetSize())
               ? m_PendingBuffers[frameIndex]
               : FRHIBufferHandle();
}

FRHIBufferHandle FMeshletCullPass::GetPendingCounterBuffer(
    UInt32 frameIndex) const
{
    return (frameIndex < m_PendingCounterBuffers.GetSize())
               ? m_PendingCounterBuffers[frameIndex]
               : FRHIBufferHandle();
}

FRHIBufferHandle FMeshletCullPass::GetViewBuffer(UInt32 frameIndex) const
{
    return (frameIndex < m_ViewBuffers.GetSize()) ? m_ViewBuffers[frameIndex]
                                                  : FRHIBufferHandle();
}

UInt64 FMeshletCullPass::GetPendingBufferBytes() { return kPendingBytes; }

UInt64 FMeshletCullPass::GetViewBufferBytes() { return kViewBytes; }

UInt64 FMeshletCullPass::GetInstanceBufferBytes() { return kInstanceBytes; }

UInt64 FMeshletCullPass::GetSceneMeshletBytes() { return kSceneMeshletBytes; }

UInt64 FMeshletCullPass::GetVisibleMeshletBytes()
{
    return kVisibleMeshletBytes;
}

UInt64 FMeshletCullPass::GetSceneVertexBytes() { return kSceneVertexBytes; }

UInt64 FMeshletCullPass::GetSceneMeshletVertexBytes()
{
    return kSceneMeshletVertexBytes;
}

UInt64 FMeshletCullPass::GetSceneMeshletTriangleBytes()
{
    return kSceneMeshletTriangleBytes;
}

} // namespace Limx
