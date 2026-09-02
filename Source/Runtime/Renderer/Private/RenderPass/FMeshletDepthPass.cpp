/*******************************************************************************
 * 文件: FMeshletDepthPass.cpp
 * 创建时间: 2026-09-02
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   meshlet 深度光栅化 — 网格着色器路径与计算展开回退路径。
 *
 ******************************************************************************/

#include "Renderer/RendererMinimal.h"

#include "Renderer/RenderPass/FMeshletDepthPass.h"
#include "Renderer/RenderPass/FMeshletCullPass.h"
#include "Renderer/Renderer/FRenderer.h"

#include "RenderCore/Geometry/FMeshletBuilder.h"
#include "RenderCore/Shaders/FShaderManager.h"

namespace Limx
{

LIMX_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{

/// 与 meshlet_depth.mesh / meshlet_depth_fallback.vert 的 push constant 一致
struct FMeshletRasterPushConstants
{
    Float32 ViewProj[16] = {};

    /// x = 可见 meshlet 数, y/z/w = 保留
    UInt32 Params[4] = { 0, 0, 0, 0 };
};

static_assert(sizeof(FMeshletRasterPushConstants) == 80,
              "FMeshletRasterPushConstants 必须是 80 字节 — 与两个光栅化"
              "着色器的 push constant 块逐字段一致");

/// 与 meshlet_expand.comp 的 push constant 一致
struct FMeshletExpandPushConstants
{
    /// x = 可见 meshlet 数, y = 顶点流容量, z/w = 保留
    UInt32 Params[4] = { 0, 0, 0, 0 };
};

/// 与 VkDrawIndirectCommand 逐字段一致
struct FDrawIndirectCommand
{
    UInt32 VertexCount = 0;
    UInt32 InstanceCount = 1;
    UInt32 FirstVertex = 0;
    UInt32 FirstInstance = 0;
};

constexpr UInt64 kExpandedBytes =
    static_cast<UInt64>(sizeof(UInt32)) * 2 * kMaxExpandedVertices;

/// 间接绘制参数块 —— 前 16 字节与 VkDrawIndirectCommand 逐字段一致,
/// 后 16 字节是诊断量 (槽位分配器发出去的顶点数 + 三个保留字)
///
/// 诊断量接在后面而不是另开一个缓冲区: vkCmdDrawIndirect 从偏移 0 读,
/// 后面的字节它一个都不碰。另开一个的话就多一次归零、一次屏障、一次拷贝,
/// 而这两个数是同一个着色器在同一次原子操作前后写的 —— 分开放反而给了
/// "两处不同步"的机会。
constexpr UInt64 kDrawArgsBytes =
    sizeof(FDrawIndirectCommand) + sizeof(UInt32) * 4;

/// 间接分派参数的模板 (0, 1, 1, 0)
constexpr UInt64 kDispatchPatternBytes = sizeof(UInt32) * 4;

/// 归零源里分派模板的偏移 —— 绘制参数在前, 分派模板紧随其后
constexpr UInt64 kResetDispatchOffset = kDrawArgsBytes;

constexpr UInt64 kResetSourceBytes = kDrawArgsBytes + kDispatchPatternBytes;

/// 间接绘制参数块里诊断量的字下标
constexpr UInt32 kDrawArgsWrittenWord   = 0;   // vertexCount
constexpr UInt32 kDrawArgsRequestedWord = 4;   // requestedVertices

/// 与 meshlet_resolve.comp 的 push constant 一致
struct FMeshletResolvePushConstants
{
    Float32 ViewProj[16] = {};

    /// x = 宽, y = 高, z/w = 保留
    UInt32 Params[4] = { 0, 0, 0, 0 };
};

static_assert(sizeof(FMeshletResolvePushConstants) == 80,
              "FMeshletResolvePushConstants 必须是 80 字节 — 与 "
              "meshlet_resolve.comp 的 push constant 块逐字段一致");

constexpr UInt32 kResolveWorkgroupSize = 8;

/// 与 hiz_copy.comp / hiz_build.comp 的 push constant 一致
struct FHizPushConstants
{
    /// copy: x = 宽, y = 高; build: x/y = 上一级, z/w = 本级
    UInt32 Params[4] = { 0, 0, 0, 0 };
};

/// 与 meshlet_cull_phase2.comp 的 push constant 一致
struct FPhase2PushConstants
{
    /// x = 待定数, y = 输出表容量, z/w = 保留
    UInt32 Params[4] = { 0, 0, 0, 0 };
};

constexpr UInt32 kHizWorkgroupSize = 8;

/// 金字塔级数 —— **必须与 Vulkan 的 mip 约定一致**
///
/// Vulkan 的第 i 级尺寸是 max(1, floor(base / 2^i)), 而 mipLevels 的上限是
/// floor(log2(max(w,h))) + 1。第一版按"向上取整地逐级减半"算, 1280x720 得到
/// 12 级, 而 Vulkan 只允许 11 —— vkCreateImage 当场拒绝。
///
/// 向下取整会丢掉奇数尺寸那一行/列, 而那一行的遮挡信息缺失会让上一级的
/// 最大值**偏小** —— 偏小意味着更容易判成"挡住了", 那是**错剔**的方向。
/// 所以归约着色器在最后一个纹素上要多收一行/列, 见 hiz_build.comp。
UInt32 ComputeHizLevels(UInt32 width, UInt32 height)
{
    UInt32 levels = 1;

    UInt32 size = FMath::Max(width, height);

    while (size > 1)
    {
        size /= 2u;
        ++levels;
    }

    return levels;
}

/// 第 level 级的尺寸 —— Vulkan 的约定
FRHIExtent2D HizLevelExtent(FRHIExtent2D base, UInt32 level)
{
    FRHIExtent2D extent = base;

    for (UInt32 i = 0; i < level; ++i)
    {
        extent.Width  = extent.Width / 2u;
        extent.Height = extent.Height / 2u;
    }

    extent.Width  = FMath::Max(extent.Width, 1u);
    extent.Height = FMath::Max(extent.Height, 1u);

    return extent;
}

} // namespace

// ============================================================================
// Setup
// ============================================================================

ERHIResult FMeshletDepthPass::Setup(const FPassSetupDesc& desc)
{
    LIMX_CHECK(desc.Device != nullptr);

    m_Device     = desc.Device;
    m_FrameCount = desc.MaxFramesInFlight;
    m_Extent     = desc.SwapchainExtent;

    m_MeshShaderAvailable = desc.Device->IsMeshShaderSupported();

    // 设备不支持时**默认就是回退路径**, 而不是"默认网格着色器然后运行时
    // 悄悄退回"。后者会让日志里写着"网格着色器路径"而实际跑的是别的东西。
    if (!m_MeshShaderAvailable)
    {
        m_Mode = EMode::Fallback;
    }

    ERHIResult result = CreateDepthTarget(desc.Device, desc.SwapchainExtent);

    if (IsRHISuccess(result))
    {
        result = CreateRenderPass(desc.Device);
    }

    if (IsRHISuccess(result))
    {
        // 展开顶点流与间接绘制参数 —— 逐并行帧一套
        for (UInt32 i = 0; i < m_FrameCount; ++i)
        {
            FRHIBufferDesc bufferDesc = {};
            bufferDesc.Size        = kExpandedBytes;
            bufferDesc.Usage       = EBufferUsage::StorageBuffer;
            bufferDesc.MemoryUsage = EMemoryUsage::GpuOnly;
            bufferDesc.DebugName   = "MeshletExpandedVertices";

            FRHIBufferHandle expanded;

            result = desc.Device->CreateBuffer(bufferDesc, expanded);

            if (!IsRHISuccess(result))
            {
                return result;
            }

            m_ExpandedBuffers.Add(expanded);

            bufferDesc.Size  = kDrawArgsBytes;
            bufferDesc.Usage = static_cast<EBufferUsage>(
                static_cast<UInt32>(EBufferUsage::StorageBuffer) |
                static_cast<UInt32>(EBufferUsage::IndirectBuffer) |
                static_cast<UInt32>(EBufferUsage::TransferDst) |
                static_cast<UInt32>(EBufferUsage::TransferSrc));
            bufferDesc.DebugName = "MeshletExpandDrawArgs";

            FRHIBufferHandle drawArgs;

            result = desc.Device->CreateBuffer(bufferDesc, drawArgs);

            if (!IsRHISuccess(result))
            {
                return result;
            }

            m_DrawArgsBuffers.Add(drawArgs);

            // 间接绘制参数的回读 —— 判据要拿"交出去的顶点数"与"想写的
            // 顶点数"比。没有这两个数的话, 溢出之后交出去的到底是哪个数
            // 在外面完全看不见, 而那正是这个缺陷藏身的地方。
            {
                FRHIBufferDesc readbackDesc = {};
                readbackDesc.Size        = kDrawArgsBytes;
                readbackDesc.Usage       = EBufferUsage::TransferDst;
                readbackDesc.MemoryUsage = EMemoryUsage::GpuToCpu;
                readbackDesc.DebugName   = "MeshletExpandDrawArgsReadback";

                FRHIBufferHandle drawArgsReadback;

                const ERHIResult readbackResult =
                    desc.Device->CreateBuffer(readbackDesc, drawArgsReadback);

                if (!IsRHISuccess(readbackResult))
                {
                    return readbackResult;
                }

                // 先清零。回读缓冲区在第一次拷进来之前装的是未初始化的
                // 主机内存 —— 读它会报出一个不存在的溢出, 而"判据在第一帧
                // 报假警"比不报还坏: 下一个人会去关掉那条判据。
                void* zero = nullptr;

                if (IsRHISuccess(
                        desc.Device->MapBuffer(drawArgsReadback, &zero)) &&
                    zero != nullptr)
                {
                    Memory::MemZero(zero, kDrawArgsBytes);
                    desc.Device->UnmapBuffer(drawArgsReadback);
                }

                m_DrawArgsReadbacks.Add(drawArgsReadback);

                // 第一次回读之前那一轮根本没展开过 —— 容量记成默认值,
                // 于是读到的 (0, 0) 与它比出来是"没溢出"。
                m_ExpandedCapacityInFlight.Add(kMaxExpandedVertices);
            }

            // 第二阶段的间接分派参数 + 第二次绘制的间接参数 +
            // 第一阶段结束时的可见数
            bufferDesc.Size  = sizeof(UInt32) * 4;
            bufferDesc.Usage = static_cast<EBufferUsage>(
                static_cast<UInt32>(EBufferUsage::StorageBuffer) |
                static_cast<UInt32>(EBufferUsage::IndirectBuffer) |
                static_cast<UInt32>(EBufferUsage::TransferDst) |
                static_cast<UInt32>(EBufferUsage::TransferSrc));

            FRHIBufferHandle phase2Dispatch;
            FRHIBufferHandle phase2RasterArgs;
            FRHIBufferHandle phase1Count;

            bufferDesc.DebugName = "MeshletPhase2Dispatch";

            result = desc.Device->CreateBuffer(bufferDesc, phase2Dispatch);

            if (IsRHISuccess(result))
            {
                bufferDesc.DebugName = "MeshletPhase2RasterArgs";
                result =
                    desc.Device->CreateBuffer(bufferDesc, phase2RasterArgs);
            }

            if (IsRHISuccess(result))
            {
                bufferDesc.DebugName = "MeshletPhase1Count";
                result = desc.Device->CreateBuffer(bufferDesc, phase1Count);
            }

            if (!IsRHISuccess(result))
            {
                return result;
            }

            // 第一阶段可见数的回读 —— 判据要拿它算"第二阶段补回来了多少"
            FRHIBufferHandle phase1Readback;

            {
                FRHIBufferDesc readbackDesc = {};
                readbackDesc.Size        = sizeof(UInt32) * 4;
                readbackDesc.Usage       = EBufferUsage::TransferDst;
                readbackDesc.MemoryUsage = EMemoryUsage::GpuToCpu;
                readbackDesc.DebugName   = "MeshletPhase1Readback";

                const ERHIResult readbackResult =
                    desc.Device->CreateBuffer(readbackDesc, phase1Readback);

                if (!IsRHISuccess(readbackResult))
                {
                    return readbackResult;
                }
            }

            FRHIBufferHandle finalReadback;

            {
                FRHIBufferDesc readbackDesc = {};
                readbackDesc.Size        = sizeof(UInt32) * 4;
                readbackDesc.Usage       = EBufferUsage::TransferDst;
                readbackDesc.MemoryUsage = EMemoryUsage::GpuToCpu;
                readbackDesc.DebugName   = "MeshletPhase2FinalReadback";

                const ERHIResult readbackResult =
                    desc.Device->CreateBuffer(readbackDesc, finalReadback);

                if (!IsRHISuccess(readbackResult))
                {
                    return readbackResult;
                }
            }

            m_Phase2FinalReadbacks.Add(finalReadback);
            m_Phase1Readbacks.Add(phase1Readback);
            m_Phase2DispatchBuffers.Add(phase2Dispatch);
            m_Phase2RasterArgsBuffers.Add(phase2RasterArgs);
            m_Phase1CountBuffers.Add(phase1Count);
        }
    }

    // 间接绘制参数的归零源
    //
    // vertexCount 归零 (计算着色器原子累加), instanceCount 必须是 1 ——
    // 全零的话一个实例都不画, 而那与"什么都不可见"分不开。
    if (IsRHISuccess(result))
    {
        // 两段: [0..31] 是间接绘制参数块 (0,1,0,0 + 四个诊断字全零),
        //       [32..47] 是间接分派参数 (0,1,1,0)。
        //
        // 诊断字也要归零 —— requestedVertices 是逐帧累加的分配器, 不清的话
        // 第二帧起它恒大于容量, "有没有溢出"这个判断就永远是真。一个恒为真
        // 的诊断量与一个恒为零的一样糟。
        //
        // 分派参数的 y/z **必须是 1**。第一版没给它们初值 —— 于是第二阶段
        // 的分派是 (n, 0, 0), 一个工作组都不起, 整个第二阶段是死代码。
        // 而"开关遮挡剔除画面相同"那条判据对此**满分通过**: 第一阶段剔掉
        // 的那些恰好真的被挡住了, 于是补不补都一样。
        //
        // 抓到它的是一条新加的元判据: 第二阶段必须真的补回来过东西。
        FRHIBufferDesc bufferDesc = {};
        bufferDesc.Size        = kResetSourceBytes;
        bufferDesc.Usage       = EBufferUsage::TransferSrc;
        bufferDesc.MemoryUsage = EMemoryUsage::CpuToGpu;
        bufferDesc.DebugName   = "MeshletExpandReset";

        result = desc.Device->CreateBuffer(bufferDesc, m_ResetSource);

        if (IsRHISuccess(result))
        {
            void* mapped = nullptr;

            if (IsRHISuccess(desc.Device->MapBuffer(m_ResetSource, &mapped)) &&
                mapped != nullptr)
            {
                auto* values = static_cast<UInt32*>(mapped);

                Memory::MemZero(values, kResetSourceBytes);

                // 间接绘制: (vertexCount, instanceCount, first, first)
                values[0] = 0;
                values[1] = 1;
                values[2] = 0;
                values[3] = 0;

                // 诊断字 (requestedVertices + 三个保留) 全零 —— 上面
                // MemZero 已经写过, 这里不再重复。

                // 间接分派: (x, y, z, 保留) —— y/z 必须是 1
                values[kResetDispatchOffset / sizeof(UInt32) + 0] = 0;
                values[kResetDispatchOffset / sizeof(UInt32) + 1] = 1;
                values[kResetDispatchOffset / sizeof(UInt32) + 2] = 1;
                values[kResetDispatchOffset / sizeof(UInt32) + 3] = 0;

                desc.Device->UnmapBuffer(m_ResetSource);
            }
        }
    }

    // ---- 材质解析的缓冲区 ----
    if (IsRHISuccess(result))
    {
        const SizeType pixelCount =
            static_cast<SizeType>(desc.SwapchainExtent.Width) *
            desc.SwapchainExtent.Height;

        for (UInt32 i = 0; i < m_FrameCount; ++i)
        {
            FRHIBufferDesc bufferDesc = {};
            bufferDesc.Size = static_cast<UInt64>(pixelCount) *
                              sizeof(FMeshletResolveResult);
            bufferDesc.Usage = static_cast<EBufferUsage>(
                static_cast<UInt32>(EBufferUsage::StorageBuffer) |
                static_cast<UInt32>(EBufferUsage::TransferSrc));
            bufferDesc.MemoryUsage = EMemoryUsage::GpuOnly;
            bufferDesc.DebugName   = "MeshletResolveResults";

            FRHIBufferHandle resolve;

            result = desc.Device->CreateBuffer(bufferDesc, resolve);

            if (!IsRHISuccess(result))
            {
                return result;
            }

            m_ResolveBuffers.Add(resolve);

            bufferDesc.Size =
                static_cast<UInt64>(sizeof(UInt32)) * kMaxMeshletInstances;
            bufferDesc.Usage       = EBufferUsage::StorageBuffer;
            bufferDesc.MemoryUsage = EMemoryUsage::CpuToGpu;
            bufferDesc.DebugName   = "MeshletInstanceMaterials";

            FRHIBufferHandle materials;

            result = desc.Device->CreateBuffer(bufferDesc, materials);

            if (!IsRHISuccess(result))
            {
                return result;
            }

            m_MaterialBuffers.Add(materials);
        }
    }

    // ---- 层次深度金字塔 ----
    //
    // 必须在描述符之前 —— 描述符集里要写金字塔的逐级视图。顺序反了的话
    // 那个数组还是空的, 而"取空数组的第 0 个"是当场崩, 不是报错。
    if (IsRHISuccess(result))
    {
        result = CreateHizResources(desc.Device, desc.SwapchainExtent);
    }

    if (IsRHISuccess(result))
    {
        result = CreateDescriptors(desc.Device, m_FrameCount);
    }

    if (IsRHISuccess(result))
    {
        result = CreatePipelines(desc.Device);
    }

    if (!IsRHISuccess(result))
    {
        return result;
    }

    LIMX_LOG(LogRenderer, Log,
             "[MeshletDepth] 初始化完成 — 网格着色器 {}, 默认路径 {}",
             m_MeshShaderAvailable ? "可用" : "**不可用**",
             (m_Mode == EMode::MeshShader) ? "网格着色器" : "计算展开回退");

    return ERHIResult::Success;
}

// ============================================================================
// SetMode
// ============================================================================

bool FMeshletDepthPass::SetMode(EMode mode)
{
    if (mode == EMode::MeshShader && !m_MeshShaderAvailable)
    {
        LIMX_LOG(LogRenderer, Error,
                 "[MeshletDepth] 请求网格着色器路径, 但设备不支持 — 保持原样");
        return false;
    }

    m_Mode = mode;

    return true;
}

// ============================================================================
// 深度目标
// ============================================================================

ERHIResult FMeshletDepthPass::CreateDepthTarget(IRHIDevice* device,
                                                FRHIExtent2D extent)
{
    FRHITextureDesc texDesc = {};
    texDesc.Type        = ETextureType::Texture2D;
    texDesc.Format      = EPixelFormat::D32_SFLOAT;
    texDesc.Extent      = { extent.Width, extent.Height, 1 };
    texDesc.MipLevels   = 1;
    texDesc.ArrayLayers = 1;
    texDesc.Samples     = ESampleCount::Count1;
    texDesc.Usage       = static_cast<ETextureUsage>(
        static_cast<UInt32>(ETextureUsage::DepthStencilAttachment) |
        static_cast<UInt32>(ETextureUsage::Sampled) |
        static_cast<UInt32>(ETextureUsage::TransferSrc));
    texDesc.MemoryUsage = EMemoryUsage::GpuOnly;
    texDesc.DebugName   = "MeshletDepth";

    ERHIResult result = device->CreateTexture(texDesc, m_DepthTexture);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    FRHITextureViewDesc viewDesc = {};
    viewDesc.Texture         = m_DepthTexture;
    viewDesc.ViewType        = ETextureType::Texture2D;
    viewDesc.Format          = EPixelFormat::D32_SFLOAT;
    viewDesc.BaseMipLevel    = 0;
    viewDesc.MipLevelCount   = 1;
    viewDesc.BaseArrayLayer  = 0;
    viewDesc.ArrayLayerCount = 1;

    m_Extent = extent;

    result = device->CreateTextureView(viewDesc, m_DepthView);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    // ---- 可见性缓冲区 ----
    texDesc.Format    = EPixelFormat::R32_UINT;
    // Storage 是给材质解析用的 —— 它按整数坐标原样读这张图。
    // 走采样器那条路会经过归一化与过滤, 而插值出来的整数编号毫无意义,
    // 且不会有任何报错。
    texDesc.Usage     = static_cast<ETextureUsage>(
        static_cast<UInt32>(ETextureUsage::ColorAttachment) |
        static_cast<UInt32>(ETextureUsage::Sampled) |
        static_cast<UInt32>(ETextureUsage::Storage) |
        static_cast<UInt32>(ETextureUsage::TransferSrc));
    texDesc.DebugName = "MeshletVisibility";

    result = device->CreateTexture(texDesc, m_VisibilityTexture);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    viewDesc.Texture = m_VisibilityTexture;
    viewDesc.Format  = EPixelFormat::R32_UINT;

    return device->CreateTextureView(viewDesc, m_VisibilityView);
}

void FMeshletDepthPass::DestroyDepthTarget(IRHIDevice* device)
{
    if (m_Framebuffer.IsValid())
    {
        device->DestroyFramebuffer(m_Framebuffer);
        m_Framebuffer = FRHIFramebufferHandle();
    }

    if (m_LoadFramebuffer.IsValid())
    {
        device->DestroyFramebuffer(m_LoadFramebuffer);
        m_LoadFramebuffer = FRHIFramebufferHandle();
    }

    if (m_VisibilityView.IsValid())
    {
        device->DestroyTextureView(m_VisibilityView);
        m_VisibilityView = FRHITextureViewHandle();
    }

    if (m_VisibilityTexture.IsValid())
    {
        device->DestroyTexture(m_VisibilityTexture);
        m_VisibilityTexture = FRHITextureHandle();
    }

    if (m_DepthView.IsValid())
    {
        device->DestroyTextureView(m_DepthView);
        m_DepthView = FRHITextureViewHandle();
    }

    if (m_DepthTexture.IsValid())
    {
        device->DestroyTexture(m_DepthTexture);
        m_DepthTexture = FRHITextureHandle();
    }
}

// ============================================================================
// 渲染通道 + 帧缓冲
// ============================================================================

ERHIResult FMeshletDepthPass::CreateRenderPass(IRHIDevice* device)
{
    // 附件顺序: [0] = 可见性 (颜色), [1] = 深度。
    //
    // **深度必须在最后。** BeginRenderPass 填清除值时先按顺序填全部颜色,
    // 再无条件把深度追加到末尾 —— 顺序不对清除值就整体错位, 而数量仍然
    // 对得上, 验证层不报错。FVulkanDevice::CreateRenderPass 里有一条硬检查
    // 就是为这件事加的。
    FRHIAttachmentDesc attachments[2] = {};

    attachments[0].Format         = EPixelFormat::R32_UINT;
    attachments[0].Samples        = ESampleCount::Count1;
    attachments[0].LoadOp         = ELoadOp::Clear;
    attachments[0].StoreOp        = EStoreOp::Store;
    attachments[0].StencilLoadOp  = ELoadOp::DontCare;
    attachments[0].StencilStoreOp = EStoreOp::DontCare;
    attachments[0].InitialLayout  = EImageLayout::Undefined;
    attachments[0].FinalLayout    = EImageLayout::ShaderReadOnly;

    attachments[1].Format         = EPixelFormat::D32_SFLOAT;
    attachments[1].Samples        = ESampleCount::Count1;
    attachments[1].LoadOp         = ELoadOp::Clear;
    attachments[1].StoreOp        = EStoreOp::Store;
    attachments[1].StencilLoadOp  = ELoadOp::DontCare;
    attachments[1].StencilStoreOp = EStoreOp::DontCare;
    attachments[1].InitialLayout  = EImageLayout::Undefined;
    attachments[1].FinalLayout    = EImageLayout::DepthStencilAttachment;

    FRHIAttachmentReference colorRef = {};
    colorRef.AttachmentIndex = 0;
    colorRef.Layout          = EImageLayout::ColorAttachment;

    FRHIAttachmentReference depthRef = {};
    depthRef.AttachmentIndex = 1;
    depthRef.Layout          = EImageLayout::DepthStencilAttachment;

    FRHISubpassDesc subpass = {};
    subpass.ColorAttachments       = &colorRef;
    subpass.ColorAttachmentCount   = 1;
    subpass.DepthStencilAttachment = &depthRef;

    FRHISubpassDependency dependency = {};
    dependency.SrcSubpass    = 0xFFFFFFFF;
    dependency.DstSubpass    = 0;
    dependency.SrcStageMask  = EPipelineStageFlags::TopOfPipe;
    dependency.DstStageMask  = EPipelineStageFlags::EarlyFragmentTests |
                               EPipelineStageFlags::ColorAttachmentOutput;
    dependency.SrcAccessMask = EAccessFlags::None;
    dependency.DstAccessMask = EAccessFlags::DepthStencilAttachmentWrite |
                               EAccessFlags::ColorAttachmentWrite;

    FRHIRenderPassDesc renderPassDesc = {};
    renderPassDesc.Attachments     = attachments;
    renderPassDesc.AttachmentCount = 2;
    renderPassDesc.Subpasses       = &subpass;
    renderPassDesc.SubpassCount    = 1;
    renderPassDesc.Dependencies    = &dependency;
    renderPassDesc.DependencyCount = 1;
    renderPassDesc.DebugName       = "MeshletDepth_RenderPass";

    ERHIResult result = device->CreateRenderPass(renderPassDesc, m_RenderPass);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    // 视图顺序必须与附件顺序一致 —— [0] 可见性, [1] 深度
    const FRHITextureViewHandle views[2] = { m_VisibilityView, m_DepthView };

    FRHIFramebufferDesc fbDesc = {};
    fbDesc.RenderPass      = m_RenderPass;
    fbDesc.Attachments     = views;
    fbDesc.AttachmentCount = 2;
    fbDesc.Width           = m_Extent.Width;
    fbDesc.Height          = m_Extent.Height;
    fbDesc.Layers          = 1;
    fbDesc.DebugName       = "MeshletDepth_Framebuffer";

    result = device->CreateFramebuffer(fbDesc, m_Framebuffer);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    // ---- 第二遍: 加载而不是清除 ----
    //
    // 两阶段遮挡剔除的第二阶段要把补上来的 meshlet 画进**同一对附件**。
    // 用第一遍那个渲染通道的话附件会被清掉, 第一阶段画的东西就没了。
    attachments[0].LoadOp        = ELoadOp::Load;
    attachments[0].InitialLayout = EImageLayout::ShaderReadOnly;

    attachments[1].LoadOp        = ELoadOp::Load;
    attachments[1].InitialLayout = EImageLayout::DepthStencilAttachment;

    renderPassDesc.DebugName = "MeshletDepth_LoadRenderPass";

    result = device->CreateRenderPass(renderPassDesc, m_LoadRenderPass);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    fbDesc.RenderPass = m_LoadRenderPass;
    fbDesc.DebugName  = "MeshletDepth_LoadFramebuffer";

    return device->CreateFramebuffer(fbDesc, m_LoadFramebuffer);
}

// ============================================================================
// 描述符
// ============================================================================

ERHIResult FMeshletDepthPass::CreateDescriptors(IRHIDevice* device,
                                                UInt32 frameCount)
{
    const auto MakeLayout = [device](UInt32 count, EShaderStage stages,
                                     const AnsiChar* name,
                                     FRHIDescSetLayoutHandle& out)
    {
        FRHIDescriptorBinding bindings[7] = {};

        for (UInt32 i = 0; i < count; ++i)
        {
            bindings[i].Binding    = i;
            bindings[i].Type       = EDescriptorType::StorageBuffer;
            bindings[i].Count      = 1;
            bindings[i].StageFlags = stages;
        }

        FRHIDescSetLayoutDesc layoutDesc = {};
        layoutDesc.Bindings     = bindings;
        layoutDesc.BindingCount = count;
        layoutDesc.DebugName    = name;

        return device->CreateDescSetLayout(layoutDesc, out);
    };

    ERHIResult result = MakeLayout(6, EShaderStage::Mesh,
                                   "MeshletDepthMeshSetLayout",
                                   m_MeshSetLayout);

    if (IsRHISuccess(result))
    {
        result = MakeLayout(7, EShaderStage::Compute,
                            "MeshletExpandSetLayout", m_ExpandSetLayout);
    }

    if (IsRHISuccess(result))
    {
        result = MakeLayout(4, EShaderStage::Vertex,
                            "MeshletFallbackSetLayout", m_FallbackSetLayout);
    }

    if (!IsRHISuccess(result))
    {
        return result;
    }

    // 解析那一套的第 0 个绑定是**存储图像**而不是缓冲区 —— 可见性缓冲区
    // 是一张 R32_UINT 纹理, 而这里要按整数原样读它。当采样纹理读的话会
    // 走归一化/过滤那条路, 而整数编号被插值出来的值毫无意义。
    {
        FRHIDescriptorBinding bindings[9] = {};

        bindings[0].Binding    = 0;
        bindings[0].Type       = EDescriptorType::StorageImage;
        bindings[0].Count      = 1;
        bindings[0].StageFlags = EShaderStage::Compute;

        for (UInt32 i = 1; i < 9; ++i)
        {
            bindings[i].Binding    = i;
            bindings[i].Type       = EDescriptorType::StorageBuffer;
            bindings[i].Count      = 1;
            bindings[i].StageFlags = EShaderStage::Compute;
        }

        FRHIDescSetLayoutDesc layoutDesc = {};
        layoutDesc.Bindings     = bindings;
        layoutDesc.BindingCount = 9;
        layoutDesc.DebugName    = "MeshletResolveSetLayout";

        result = device->CreateDescSetLayout(layoutDesc, m_ResolveSetLayout);

        if (!IsRHISuccess(result))
        {
            return result;
        }
    }

    // 可见性缓冲区的**存储图像**视图。
    //
    // 与颜色附件那个视图是同一张纹理的两个视图 —— 用途不同, Vulkan 要求
    // 分开声明。共用一个的话创建时就会被拒绝, 而那条错误信息不会说是
    // 用途的问题。
    {
        FRHITextureViewDesc viewDesc = {};
        viewDesc.Texture         = m_VisibilityTexture;
        viewDesc.ViewType        = ETextureType::Texture2D;
        viewDesc.Format          = EPixelFormat::R32_UINT;
        viewDesc.BaseMipLevel    = 0;
        viewDesc.MipLevelCount   = 1;
        viewDesc.BaseArrayLayer  = 0;
        viewDesc.ArrayLayerCount = 1;

        result = device->CreateTextureView(viewDesc, m_VisibilityStorageView);

        if (!IsRHISuccess(result))
        {
            return result;
        }
    }

    // ---- Hi-Z 的两套布局 ----
    //
    // 拷贝那一套: 0 = 深度 (采样), 1 = 第 0 级 (存储图像)
    // 归约那一套: 0 = 上一级 (存储图像), 1 = 本级 (存储图像)
    {
        FRHIDescriptorBinding bindings[2] = {};

        bindings[0].Binding    = 0;
        bindings[0].Type       = EDescriptorType::CombinedImageSampler;
        bindings[0].Count      = 1;
        bindings[0].StageFlags = EShaderStage::Compute;

        bindings[1].Binding    = 1;
        bindings[1].Type       = EDescriptorType::StorageImage;
        bindings[1].Count      = 1;
        bindings[1].StageFlags = EShaderStage::Compute;

        FRHIDescSetLayoutDesc layoutDesc = {};
        layoutDesc.Bindings     = bindings;
        layoutDesc.BindingCount = 2;
        layoutDesc.DebugName    = "MeshletHizCopySetLayout";

        result = device->CreateDescSetLayout(layoutDesc, m_HizCopySetLayout);

        if (!IsRHISuccess(result))
        {
            return result;
        }

        bindings[0].Type = EDescriptorType::StorageImage;

        layoutDesc.DebugName = "MeshletHizBuildSetLayout";

        result = device->CreateDescSetLayout(layoutDesc, m_HizBuildSetLayout);

        if (!IsRHISuccess(result))
        {
            return result;
        }
    }

    // ---- 第二阶段的布局 ----
    //
    // 0..4 是存储缓冲区, 5 是金字塔 (采样), 6 是逐视图 UBO
    {
        FRHIDescriptorBinding bindings[8] = {};

        for (UInt32 i = 0; i < 8; ++i)
        {
            bindings[i].Binding    = i;
            bindings[i].Type       = EDescriptorType::StorageBuffer;
            bindings[i].Count      = 1;
            bindings[i].StageFlags = EShaderStage::Compute;
        }

        bindings[5].Type = EDescriptorType::CombinedImageSampler;
        bindings[6].Type = EDescriptorType::UniformBuffer;

        FRHIDescSetLayoutDesc layoutDesc = {};
        layoutDesc.Bindings     = bindings;
        layoutDesc.BindingCount = 8;
        layoutDesc.DebugName    = "MeshletPhase2SetLayout";

        result = device->CreateDescSetLayout(layoutDesc, m_Phase2SetLayout);

        if (!IsRHISuccess(result))
        {
            return result;
        }
    }

    // ---- Hi-Z 的描述符集 (与并行帧无关: 金字塔只有一份) ----
    {
        result = device->AllocateDescriptorSet(m_HizCopySetLayout,
                                               m_HizCopySet);

        if (!IsRHISuccess(result))
        {
            return result;
        }

        FRHIDescriptorWrite writes[2];

        writes[0] = FRHIDescriptorWrite::CombinedImageSampler(
            m_HizCopySet, 0, m_DepthView, m_DepthSampler,
            EImageLayout::ShaderReadOnly);

        writes[1] = FRHIDescriptorWrite::StorageImage(m_HizCopySet, 1,
                                                      m_HizLevelViews[0]);

        device->UpdateDescriptorSets(writes, 2);

        for (UInt32 level = 1; level < m_HizLevels; ++level)
        {
            FRHIDescriptorSetHandle buildSet;

            result = device->AllocateDescriptorSet(m_HizBuildSetLayout,
                                                   buildSet);

            if (!IsRHISuccess(result))
            {
                return result;
            }

            FRHIDescriptorWrite buildWrites[2];

            buildWrites[0] = FRHIDescriptorWrite::StorageImage(
                buildSet, 0, m_HizLevelViews[level - 1]);

            buildWrites[1] = FRHIDescriptorWrite::StorageImage(
                buildSet, 1, m_HizLevelViews[level]);

            device->UpdateDescriptorSets(buildWrites, 2);

            m_HizBuildSets.Add(buildSet);
        }
    }

    for (UInt32 i = 0; i < frameCount; ++i)
    {
        FRHIDescriptorSetHandle meshSet;
        FRHIDescriptorSetHandle expandSet;
        FRHIDescriptorSetHandle fallbackSet;
        FRHIDescriptorSetHandle resolveSet;

        result = device->AllocateDescriptorSet(m_MeshSetLayout, meshSet);

        if (IsRHISuccess(result))
        {
            result =
                device->AllocateDescriptorSet(m_ExpandSetLayout, expandSet);
        }

        if (IsRHISuccess(result))
        {
            result = device->AllocateDescriptorSet(m_FallbackSetLayout,
                                                   fallbackSet);
        }

        if (!IsRHISuccess(result))
        {
            return result;
        }

        result = device->AllocateDescriptorSet(m_ResolveSetLayout, resolveSet);

        if (!IsRHISuccess(result))
        {
            return result;
        }

        FRHIDescriptorSetHandle phase2Set;

        result = device->AllocateDescriptorSet(m_Phase2SetLayout, phase2Set);

        if (!IsRHISuccess(result))
        {
            return result;
        }

        m_MeshSets.Add(meshSet);
        m_BoundVertexBuffers.Add(FRHIBufferHandle());
        m_ExpandSets.Add(expandSet);
        m_FallbackSets.Add(fallbackSet);
        m_ResolveSets.Add(resolveSet);
        m_Phase2Sets.Add(phase2Set);
    }

    return ERHIResult::Success;
}

// ============================================================================
// 管线
// ============================================================================

ERHIResult FMeshletDepthPass::CreatePipelines(IRHIDevice* device)
{
    FShaderManager& shaders = FShaderManager::Get();

    if (!shaders.IsInitialized())
    {
        shaders.Initialize();
    }

    ERHIResult result = shaders.CreateShaderModule(
        device, FString("Builtin/meshlet_depth.frag"), EShaderStage::Fragment,
        m_FragmentShader);

    if (IsRHISuccess(result))
    {
        result = shaders.CreateShaderModule(
            device, FString("Builtin/meshlet_expand.comp"),
            EShaderStage::Compute, m_ExpandShader);
    }

    if (IsRHISuccess(result))
    {
        result = shaders.CreateShaderModule(
            device, FString("Builtin/meshlet_depth_fallback.vert"),
            EShaderStage::Vertex, m_FallbackVertexShader);
    }

    if (IsRHISuccess(result))
    {
        result = shaders.CreateShaderModule(
            device, FString("Builtin/meshlet_depth_fallback.frag"),
            EShaderStage::Fragment, m_FallbackFragmentShader);
    }

    // 网格着色器模块只在设备支持时创建 —— 不支持时创建会被驱动拒绝,
    // 而那条错误看起来像"着色器文件坏了"。
    if (IsRHISuccess(result) && m_MeshShaderAvailable)
    {
        result = shaders.CreateShaderModule(
            device, FString("Builtin/meshlet_depth.mesh"), EShaderStage::Mesh,
            m_MeshShader);
    }

    if (!IsRHISuccess(result))
    {
        return result;
    }

    // ---- 管线布局 ----
    const auto MakeLayout = [device](FRHIDescSetLayoutHandle setLayout,
                                     EShaderStage stages, UInt32 pushSize,
                                     const AnsiChar* name,
                                     FRHIPipelineLayoutHandle& out)
    {
        FRHIPushConstantRange pushRange = {};
        pushRange.StageFlags = stages;
        pushRange.Offset     = 0;
        pushRange.Size       = pushSize;

        FRHIPipelineLayoutDesc layoutDesc = {};
        layoutDesc.SetLayouts             = &setLayout;
        layoutDesc.SetLayoutCount         = 1;
        layoutDesc.PushConstantRanges     = &pushRange;
        layoutDesc.PushConstantRangeCount = 1;
        layoutDesc.DebugName              = name;

        return device->CreatePipelineLayout(layoutDesc, out);
    };

    result = MakeLayout(m_MeshSetLayout, EShaderStage::Mesh,
                        sizeof(FMeshletRasterPushConstants),
                        "MeshletDepthMeshLayout", m_MeshPipelineLayout);

    if (IsRHISuccess(result))
    {
        result = MakeLayout(m_ExpandSetLayout, EShaderStage::Compute,
                            sizeof(FMeshletExpandPushConstants),
                            "MeshletExpandLayout", m_ExpandPipelineLayout);
    }

    if (IsRHISuccess(result))
    {
        result = MakeLayout(m_FallbackSetLayout, EShaderStage::Vertex,
                            sizeof(FMeshletRasterPushConstants),
                            "MeshletFallbackLayout", m_FallbackPipelineLayout);
    }

    if (!IsRHISuccess(result))
    {
        return result;
    }

    // ---- Hi-Z 的两条计算管线 ----
    {
        result = shaders.CreateShaderModule(
            device, FString("Builtin/hiz_copy.comp"), EShaderStage::Compute,
            m_HizCopyShader);

        if (IsRHISuccess(result))
        {
            result = shaders.CreateShaderModule(
                device, FString("Builtin/hiz_build.comp"),
                EShaderStage::Compute, m_HizBuildShader);
        }

        if (IsRHISuccess(result))
        {
            result = shaders.CreateShaderModule(
                device, FString("Builtin/meshlet_cull_phase2.comp"),
                EShaderStage::Compute, m_Phase2Shader);
        }

        if (!IsRHISuccess(result))
        {
            return result;
        }

        result = MakeLayout(m_HizCopySetLayout, EShaderStage::Compute,
                            sizeof(FHizPushConstants), "MeshletHizCopyLayout",
                            m_HizCopyLayout);

        if (IsRHISuccess(result))
        {
            result = MakeLayout(m_HizBuildSetLayout, EShaderStage::Compute,
                                sizeof(FHizPushConstants),
                                "MeshletHizBuildLayout", m_HizBuildLayout);
        }

        if (IsRHISuccess(result))
        {
            result = MakeLayout(m_Phase2SetLayout, EShaderStage::Compute,
                                sizeof(FPhase2PushConstants),
                                "MeshletPhase2Layout", m_Phase2Layout);
        }

        if (!IsRHISuccess(result))
        {
            return result;
        }

        FRHIComputePipelineDesc pipelineDesc = {};
        pipelineDesc.ComputeShader.Stage      = EShaderStage::Compute;
        pipelineDesc.ComputeShader.EntryPoint = "main";

        pipelineDesc.ComputeShader.Shader = m_HizCopyShader;
        pipelineDesc.PipelineLayout       = m_HizCopyLayout;
        pipelineDesc.DebugName            = "MeshletHizCopyPipeline";

        result = device->CreateComputePipeline(pipelineDesc, m_HizCopyPipeline);

        if (IsRHISuccess(result))
        {
            pipelineDesc.ComputeShader.Shader = m_HizBuildShader;
            pipelineDesc.PipelineLayout       = m_HizBuildLayout;
            pipelineDesc.DebugName            = "MeshletHizBuildPipeline";

            result =
                device->CreateComputePipeline(pipelineDesc, m_HizBuildPipeline);
        }

        if (IsRHISuccess(result))
        {
            pipelineDesc.ComputeShader.Shader = m_Phase2Shader;
            pipelineDesc.PipelineLayout       = m_Phase2Layout;
            pipelineDesc.DebugName            = "MeshletPhase2Pipeline";

            result =
                device->CreateComputePipeline(pipelineDesc, m_Phase2Pipeline);
        }

        if (!IsRHISuccess(result))
        {
            return result;
        }
    }

    // ---- 解析的计算管线 ----
    {
        result = shaders.CreateShaderModule(
            device, FString("Builtin/meshlet_resolve.comp"),
            EShaderStage::Compute, m_ResolveShader);

        if (!IsRHISuccess(result))
        {
            return result;
        }

        result = MakeLayout(m_ResolveSetLayout, EShaderStage::Compute,
                            sizeof(FMeshletResolvePushConstants),
                            "MeshletResolveLayout", m_ResolvePipelineLayout);

        if (!IsRHISuccess(result))
        {
            return result;
        }

        FRHIComputePipelineDesc pipelineDesc = {};
        pipelineDesc.ComputeShader.Shader     = m_ResolveShader;
        pipelineDesc.ComputeShader.Stage      = EShaderStage::Compute;
        pipelineDesc.ComputeShader.EntryPoint = "main";
        pipelineDesc.PipelineLayout           = m_ResolvePipelineLayout;
        pipelineDesc.DebugName                = "MeshletResolvePipeline";

        result = device->CreateComputePipeline(pipelineDesc, m_ResolvePipeline);

        if (!IsRHISuccess(result))
        {
            return result;
        }
    }

    // ---- 展开的计算管线 ----
    {
        FRHIComputePipelineDesc pipelineDesc = {};
        pipelineDesc.ComputeShader.Shader     = m_ExpandShader;
        pipelineDesc.ComputeShader.Stage      = EShaderStage::Compute;
        pipelineDesc.ComputeShader.EntryPoint = "main";
        pipelineDesc.PipelineLayout           = m_ExpandPipelineLayout;
        pipelineDesc.DebugName                = "MeshletExpandPipeline";

        result = device->CreateComputePipeline(pipelineDesc, m_ExpandPipeline);

        if (!IsRHISuccess(result))
        {
            return result;
        }
    }

    // ---- 图形管线 (两条路径共用同一份状态, 只有着色器阶段不同) ----
    //
    // 状态必须逐字相同 —— 剔除模式、深度比较、视口约定, 任何一处不同都会
    // 让两条路径画出不同的深度, 而判据要求它们逐位相同。
    const auto MakeGraphics = [&](FRHIShaderHandle firstStage,
                                  EShaderStage firstStageKind,
                                  FRHIShaderHandle fragmentStage,
                                  FRHIPipelineLayoutHandle layout,
                                  const AnsiChar* name,
                                  FRHIGraphicsPipelineHandle& out)
    {
        FRHIGraphicsPipelineDesc pipelineDesc = {};

        pipelineDesc.ShaderStages[0].Shader     = firstStage;
        pipelineDesc.ShaderStages[0].Stage      = firstStageKind;
        pipelineDesc.ShaderStages[0].EntryPoint = "main";

        pipelineDesc.ShaderStages[1].Shader     = fragmentStage;
        pipelineDesc.ShaderStages[1].Stage      = EShaderStage::Fragment;
        pipelineDesc.ShaderStages[1].EntryPoint = "main";

        pipelineDesc.ShaderStageCount = 2;

        // 顶点输入为空。网格着色器路径下 Vulkan 直接忽略这一段; 回退路径
        // 的顶点数据来自 storage buffer, 也不经过顶点输入。
        pipelineDesc.VertexInput.BindingCount   = 0;
        pipelineDesc.VertexInput.AttributeCount = 0;

        pipelineDesc.InputAssembly.Topology = EPrimitiveTopology::TriangleList;

        pipelineDesc.Rasterization.PolygonMode = EPolygonMode::Fill;
        pipelineDesc.Rasterization.CullMode    = ECullMode::Back;
        pipelineDesc.Rasterization.FrontFace   = EFrontFace::CounterClockwise;
        pipelineDesc.Rasterization.LineWidth   = 1.0f;

        pipelineDesc.Multisample.RasterizationSamples = ESampleCount::Count1;

        FRHIColorBlendAttachmentDesc blendAttachment = {};
        blendAttachment.IsBlendEnabled = false;
        blendAttachment.ColorWriteMask = EColorWriteMask::All;

        pipelineDesc.DepthStencil.IsDepthTestEnabled  = true;
        pipelineDesc.DepthStencil.IsDepthWriteEnabled = true;
        pipelineDesc.DepthStencil.DepthCompareOp      = ECompareOp::Less;

        // 一个颜色附件, 不混合。可见性编号是整数, 混合毫无意义 ——
        // 而 R32_UINT 上开混合会被验证层直接拒绝。
        pipelineDesc.ColorBlend.Attachments     = &blendAttachment;
        pipelineDesc.ColorBlend.AttachmentCount = 1;

        pipelineDesc.DynamicState.EnabledStates =
            EDynamicState::Viewport | EDynamicState::Scissor;

        pipelineDesc.PipelineLayout = layout;
        pipelineDesc.RenderPass =
            m_UseLoadRenderPass ? m_LoadRenderPass : m_RenderPass;
        pipelineDesc.SubpassIndex   = 0;
        pipelineDesc.DebugName      = name;

        return device->CreateGraphicsPipeline(pipelineDesc, out);
    };

    if (m_MeshShaderAvailable)
    {
        result = MakeGraphics(m_MeshShader, EShaderStage::Mesh,
                              m_FragmentShader, m_MeshPipelineLayout,
                              "MeshletDepthMeshPipeline", m_MeshPipeline);

        if (!IsRHISuccess(result))
        {
            return result;
        }
    }

    result = MakeGraphics(m_FallbackVertexShader, EShaderStage::Vertex,
                          m_FallbackFragmentShader, m_FallbackPipelineLayout,
                          "MeshletDepthFallbackPipeline", m_FallbackPipeline);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    // ---- 第二遍用的两条管线 ----
    //
    // 与上面两条只差渲染通道。Vulkan 的"渲染通道兼容"不看 LoadOp, 所以
    // 理论上可以共用 —— 但那是靠一条兼容性规则恰好允许, 而规则里没写
    // "以后也一定允许"。分开建两条, 每条明确属于一个渲染通道。
    m_UseLoadRenderPass = true;

    if (m_MeshShaderAvailable)
    {
        result = MakeGraphics(m_MeshShader, EShaderStage::Mesh,
                              m_FragmentShader, m_MeshPipelineLayout,
                              "MeshletDepthMeshPipelineLoad",
                              m_MeshPipelineLoad);

        if (!IsRHISuccess(result))
        {
            return result;
        }
    }

    result = MakeGraphics(m_FallbackVertexShader, EShaderStage::Vertex,
                          m_FallbackFragmentShader, m_FallbackPipelineLayout,
                          "MeshletDepthFallbackPipelineLoad",
                          m_FallbackPipelineLoad);

    m_UseLoadRenderPass = false;

    return result;
}

// ============================================================================
// Execute
// ============================================================================

void FMeshletDepthPass::Execute(IRHICommandBuffer*        commandBuffer,
                                const FRenderPassContext& context)
{
    if (!m_Enabled || m_Device == nullptr)
    {
        return;
    }

    FMeshletCullPass* const cull = context.MeshletCull;

    if (cull == nullptr || !cull->IsEnabled())
    {
        return;
    }

    const UInt32 frameIndex = context.FrameIndex;

    const UInt32 instanceCount =
        static_cast<UInt32>(cull->GetInstances().GetSize());

    if (instanceCount == 0 || cull->GetSceneMeshletCount() == 0)
    {
        return;
    }

    commandBuffer->BeginDebugLabel("MeshletDepthPass", 0.5f, 0.9f, 0.5f);

    // ---- 描述符指向本帧的场景缓冲区 ----
    //
    // 场景缓冲区只在几何签名变化时重建, 而描述符要跟着走。每帧无条件重写
    // 也行 (几个 write 而已), 但那会掩盖"缓冲区换了而描述符没换"这类错误 ——
    // 现在只在真的换了时才写, 于是那件事有痕迹。
    //
    // ── 只改**当前帧**那一份 ──
    //
    // 第一版一次把所有帧下标的集都改了, 而别的帧下标的集可能正被在飞的
    // 命令缓冲区用着 —— vkUpdateDescriptorSets 不允许改在用的集。
    //
    // 这个错在小场景上不出现: 缓冲区是在预热阶段建好的, 那时还没有帧在飞。
    // 规模一上去 (grid 128, 一万六千个实例) 场景同步慢下来, 重建落到了有
    // 帧在飞的时刻, 验证层立刻报出来, 紧接着 vkQueueSubmit 返回 DEVICE_LOST,
    // 六十帧一帧都没跑完。
    //
    // 所以每个帧下标各记一份"已经指到哪个缓冲区了"。
    if (frameIndex < m_BoundVertexBuffers.GetSize() &&
        m_BoundVertexBuffers[frameIndex] != cull->GetSceneVertexBuffer())
    {
        {
            const UInt32 i = frameIndex;

            FRHIDescriptorWrite meshWrites[6];

            meshWrites[0] = FRHIDescriptorWrite::StorageBuffer(
                m_MeshSets[i], 0, cull->GetInstanceBuffer(i), 0,
                cull->GetInstanceBufferBytes());
            meshWrites[1] = FRHIDescriptorWrite::StorageBuffer(
                m_MeshSets[i], 1, cull->GetSceneMeshletBuffer(), 0,
                cull->GetSceneMeshletBytes());
            meshWrites[2] = FRHIDescriptorWrite::StorageBuffer(
                m_MeshSets[i], 2, cull->GetVisibleMeshletBuffer(i), 0,
                cull->GetVisibleMeshletBytes());
            meshWrites[3] = FRHIDescriptorWrite::StorageBuffer(
                m_MeshSets[i], 3, cull->GetSceneVertexBuffer(), 0,
                cull->GetSceneVertexBytes());
            meshWrites[4] = FRHIDescriptorWrite::StorageBuffer(
                m_MeshSets[i], 4, cull->GetSceneMeshletVertexBuffer(), 0,
                cull->GetSceneMeshletVertexBytes());
            meshWrites[5] = FRHIDescriptorWrite::StorageBuffer(
                m_MeshSets[i], 5, cull->GetSceneMeshletTriangleBuffer(), 0,
                cull->GetSceneMeshletTriangleBytes());

            m_Device->UpdateDescriptorSets(meshWrites, 6);

            FRHIDescriptorWrite expandWrites[7];

            expandWrites[0] = FRHIDescriptorWrite::StorageBuffer(
                m_ExpandSets[i], 0, cull->GetSceneMeshletBuffer(), 0,
                cull->GetSceneMeshletBytes());
            expandWrites[1] = FRHIDescriptorWrite::StorageBuffer(
                m_ExpandSets[i], 1, cull->GetVisibleMeshletBuffer(i), 0,
                cull->GetVisibleMeshletBytes());
            expandWrites[2] = FRHIDescriptorWrite::StorageBuffer(
                m_ExpandSets[i], 2, cull->GetSceneMeshletVertexBuffer(), 0,
                cull->GetSceneMeshletVertexBytes());
            expandWrites[3] = FRHIDescriptorWrite::StorageBuffer(
                m_ExpandSets[i], 3, cull->GetSceneMeshletTriangleBuffer(), 0,
                cull->GetSceneMeshletTriangleBytes());
            expandWrites[4] = FRHIDescriptorWrite::StorageBuffer(
                m_ExpandSets[i], 4, m_ExpandedBuffers[i], 0, kExpandedBytes);
            expandWrites[5] = FRHIDescriptorWrite::StorageBuffer(
                m_ExpandSets[i], 5, m_DrawArgsBuffers[i], 0, kDrawArgsBytes);
            expandWrites[6] = FRHIDescriptorWrite::StorageBuffer(
                m_ExpandSets[i], 6, cull->GetInstanceBuffer(i), 0,
                cull->GetInstanceBufferBytes());

            m_Device->UpdateDescriptorSets(expandWrites, 7);

            FRHIDescriptorWrite fallbackWrites[4];

            fallbackWrites[0] = FRHIDescriptorWrite::StorageBuffer(
                m_FallbackSets[i], 0, cull->GetInstanceBuffer(i), 0,
                cull->GetInstanceBufferBytes());
            fallbackWrites[1] = FRHIDescriptorWrite::StorageBuffer(
                m_FallbackSets[i], 1, cull->GetSceneVertexBuffer(), 0,
                cull->GetSceneVertexBytes());
            fallbackWrites[2] = FRHIDescriptorWrite::StorageBuffer(
                m_FallbackSets[i], 2, m_ExpandedBuffers[i], 0, kExpandedBytes);
            fallbackWrites[3] = FRHIDescriptorWrite::StorageBuffer(
                m_FallbackSets[i], 3, cull->GetVisibleMeshletBuffer(i), 0,
                cull->GetVisibleMeshletBytes());

            m_Device->UpdateDescriptorSets(fallbackWrites, 4);

            FRHIDescriptorWrite resolveWrites[9];

            resolveWrites[0] = FRHIDescriptorWrite::StorageImage(
                m_ResolveSets[i], 0, m_VisibilityStorageView,
                EImageLayout::General);
            resolveWrites[1] = FRHIDescriptorWrite::StorageBuffer(
                m_ResolveSets[i], 1, cull->GetInstanceBuffer(i), 0,
                cull->GetInstanceBufferBytes());
            resolveWrites[2] = FRHIDescriptorWrite::StorageBuffer(
                m_ResolveSets[i], 2, cull->GetSceneMeshletBuffer(), 0,
                cull->GetSceneMeshletBytes());
            resolveWrites[3] = FRHIDescriptorWrite::StorageBuffer(
                m_ResolveSets[i], 3, cull->GetVisibleMeshletBuffer(i), 0,
                cull->GetVisibleMeshletBytes());
            resolveWrites[4] = FRHIDescriptorWrite::StorageBuffer(
                m_ResolveSets[i], 4, cull->GetSceneVertexBuffer(), 0,
                cull->GetSceneVertexBytes());
            resolveWrites[5] = FRHIDescriptorWrite::StorageBuffer(
                m_ResolveSets[i], 5, cull->GetSceneMeshletVertexBuffer(), 0,
                cull->GetSceneMeshletVertexBytes());
            resolveWrites[6] = FRHIDescriptorWrite::StorageBuffer(
                m_ResolveSets[i], 6, cull->GetSceneMeshletTriangleBuffer(), 0,
                cull->GetSceneMeshletTriangleBytes());
            resolveWrites[7] = FRHIDescriptorWrite::StorageBuffer(
                m_ResolveSets[i], 7, m_MaterialBuffers[i], 0,
                static_cast<UInt64>(sizeof(UInt32)) * kMaxMeshletInstances);
            resolveWrites[8] = FRHIDescriptorWrite::StorageBuffer(
                m_ResolveSets[i], 8, m_ResolveBuffers[i], 0,
                static_cast<UInt64>(m_Extent.Width) * m_Extent.Height *
                    sizeof(FMeshletResolveResult));

            m_Device->UpdateDescriptorSets(resolveWrites, 9);

            FRHIDescriptorWrite phase2Writes[8];

            phase2Writes[0] = FRHIDescriptorWrite::StorageBuffer(
                m_Phase2Sets[i], 0, cull->GetInstanceBuffer(i), 0,
                cull->GetInstanceBufferBytes());
            phase2Writes[1] = FRHIDescriptorWrite::StorageBuffer(
                m_Phase2Sets[i], 1, cull->GetSceneMeshletBuffer(), 0,
                cull->GetSceneMeshletBytes());
            phase2Writes[2] = FRHIDescriptorWrite::StorageBuffer(
                m_Phase2Sets[i], 2, cull->GetPendingBuffer(i), 0,
                FMeshletCullPass::GetPendingBufferBytes());
            phase2Writes[3] = FRHIDescriptorWrite::StorageBuffer(
                m_Phase2Sets[i], 3, cull->GetVisibleMeshletBuffer(i), 0,
                cull->GetVisibleMeshletBytes());
            phase2Writes[4] = FRHIDescriptorWrite::StorageBuffer(
                m_Phase2Sets[i], 4, cull->GetCounterBuffer(i), 0,
                sizeof(UInt32) * 4);
            phase2Writes[5] = FRHIDescriptorWrite::CombinedImageSampler(
                m_Phase2Sets[i], 5, m_HizFullView, m_HizSampler,
                EImageLayout::ShaderReadOnly);
            phase2Writes[6] = FRHIDescriptorWrite::UniformBuffer(
                m_Phase2Sets[i], 6, cull->GetViewBuffer(i), 0,
                FMeshletCullPass::GetViewBufferBytes());
            phase2Writes[7] = FRHIDescriptorWrite::StorageBuffer(
                m_Phase2Sets[i], 7, cull->GetPendingCounterBuffer(i), 0,
                sizeof(UInt32) * 4);

            m_Device->UpdateDescriptorSets(phase2Writes, 8);
        }

        m_BoundVertexBuffers[frameIndex] = cull->GetSceneVertexBuffer();
    }

    m_DrawnMeshlets = cull->GetStats().MeshletsVisible;

    // 第二阶段补回来了多少 —— 回读隔着并行帧数, 与别的计数同理
    {
        void* phase1Mapped = nullptr;
        void* finalMapped  = nullptr;

        if (frameIndex < m_Phase1Readbacks.GetSize() &&
            IsRHISuccess(m_Device->MapBuffer(m_Phase1Readbacks[frameIndex],
                                             &phase1Mapped)) &&
            IsRHISuccess(m_Device->MapBuffer(
                m_Phase2FinalReadbacks[frameIndex], &finalMapped)) &&
            phase1Mapped != nullptr && finalMapped != nullptr)
        {
            const UInt32 phase1 =
                static_cast<const UInt32*>(phase1Mapped)[0];

            const UInt32 finalCount =
                static_cast<const UInt32*>(finalMapped)[0];

            m_Phase2Meshlets =
                (finalCount > phase1) ? (finalCount - phase1) : 0;

            m_Device->UnmapBuffer(m_Phase2FinalReadbacks[frameIndex]);
            m_Device->UnmapBuffer(m_Phase1Readbacks[frameIndex]);
        }
    }

    // ---- 展开顶点流的两个数 ----
    //
    // 回读同样隔着并行帧数 —— 读的是同一个帧下标上一轮写下的值。
    //
    // 交给 vkCmdDrawIndirect 的顶点数**不许超过流的容量**。超了的话顶点
    // 着色器会拿 gl_VertexIndex 去索引流之外的地址, 而那不是"画面上多一块
    // 垃圾", 是 GPU 读非法地址、设备丢失。这条不变式与堆布局无关, 所以它
    // 是能拿来当判据的那个量; "会不会丢设备"不是。
    //
    // 只有回退路径才展开 —— 网格着色器路径下那份缓冲区从来没被写过, 读它
    // 得到的是未初始化的内容, 而那会报出不存在的溢出。
    if (m_Mode == EMode::Fallback)
    {
        void* mapped = nullptr;

        if (frameIndex < m_DrawArgsReadbacks.GetSize() &&
            IsRHISuccess(m_Device->MapBuffer(m_DrawArgsReadbacks[frameIndex],
                                             &mapped)) &&
            mapped != nullptr)
        {
            const auto* const words = static_cast<const UInt32*>(mapped);

            // 容量取**那一轮**生效的那个, 不是现在这个。见成员声明处。
            m_ExpandStats.Capacity  = m_ExpandedCapacityInFlight[frameIndex];
            m_ExpandStats.Written   = words[kDrawArgsWrittenWord];
            m_ExpandStats.Requested = words[kDrawArgsRequestedWord];

            if (m_ExpandStats.HasOverflow())
            {
                LIMX_LOG(LogRenderer, Error,
                         "[MeshletDepth] 展开顶点流溢出 — 要写 {} 个顶点 "
                         "(容量 {}), 实际写进去 {} 个。超出的三角形被丢掉"
                         "了, 画面上会少东西",
                         m_ExpandStats.Requested, m_ExpandStats.Capacity,
                         m_ExpandStats.Written);
            }

            if (m_ExpandStats.Written > m_ExpandStats.Capacity)
            {
                LIMX_LOG(LogRenderer, Error,
                         "[MeshletDepth] 交给 DrawIndirect 的顶点数 {} 超过"
                         "流的容量 {} —— 顶点着色器会读到缓冲区之外",
                         m_ExpandStats.Written, m_ExpandStats.Capacity);
            }

            m_Device->UnmapBuffer(m_DrawArgsReadbacks[frameIndex]);
        }
    }

    // 工作组数**来自 GPU** —— 剔除通道把可见数拷进了一份间接参数。
    //
    // 第一版拿"场景 meshlet 总数"当工作组数, 那是错的: 可见表里是
    // (实例, meshlet) 对, 同一个 meshlet 会因为多个实例出现多次, 于是
    // 表长远大于场景 meshlet 数。实测综合场景 14 个 meshlet 对应九十多条
    // 可见记录 —— 两条光栅化路径各画了任意的 14 条 (原子追加的顺序每帧
    // 都不同), 判据报出 28334 个像素不同。
    //
    // 用上一帧回读的数也不行: 物体一动那个数就不对, 多了就去读表里上一帧
    // 留下的记录, 那是一块位置完全不对的几何体。
    //
    // 着色器里那条越界判断留着, 参数给的是表的容量 —— 间接参数已经是准确
    // 的了, 那条判断防的是"间接参数本身错了"。
    const UInt32 boundsLimit = kMaxSceneMeshlets;

    const FRHIBufferHandle rasterArgs =
        cull->GetRasterArgsBuffer(frameIndex);

    if (!rasterArgs.IsValid())
    {
        commandBuffer->EndDebugLabel();
        return;
    }

    FMeshletRasterPushConstants rasterPush;

    // 视锥/视图矩阵与剔除用的是同一个 —— 剔除按 A 剔、光栅化按 B 画的话,
    // 画出来的东西会缺一块或多一块, 而两者都在"边界附近"。
    {
        const FMatrix viewProj =
            context.Camera != nullptr
                ? (context.Camera->GetProjectionMatrix() *
                   context.Camera->GetViewMatrix())
                : FMatrix::kIdentity;

        for (UInt32 row = 0; row < 4; ++row)
        {
            for (UInt32 col = 0; col < 4; ++col)
            {
                rasterPush.ViewProj[row * 4 + col] = viewProj.M[row][col];
            }
        }
    }

    rasterPush.Params[0] = boundsLimit;
    rasterPush.Params[1] = 0;

    // ---- 回退路径: 先展开 ----
    if (m_Mode == EMode::Fallback)
    {
        FRHIBufferCopyRegion region = {};
        region.SrcOffset = 0;
        region.DstOffset = 0;
        region.Size      = kDrawArgsBytes;

        commandBuffer->CopyBuffer(m_ResetSource, m_DrawArgsBuffers[frameIndex],
                                  region);

        FRHIBufferMemoryBarrier barrier = {};
        barrier.SrcAccessMask = EAccessFlags::TransferWrite;
        barrier.DstAccessMask =
            EAccessFlags::ShaderRead | EAccessFlags::ShaderWrite;
        barrier.Buffer = m_DrawArgsBuffers[frameIndex];

        commandBuffer->PipelineBarrier(EPipelineStageFlags::Transfer,
                                       EPipelineStageFlags::ComputeShader,
                                       nullptr, 0, &barrier, 1, nullptr, 0);

        commandBuffer->BindComputePipeline(m_ExpandPipeline);
        commandBuffer->BindDescriptorSet(EPipelineBindPoint::Compute,
                                         m_ExpandPipelineLayout, 0,
                                         m_ExpandSets[frameIndex]);

        // 记下这一轮生效的容量 —— 下一次转到这个帧下标时, 回读到的两个数
        // 属于这一轮, 要和这个容量比。上面那段回读已经读过旧值了, 所以
        // 这里覆盖它是安全的。
        if (frameIndex < m_ExpandedCapacityInFlight.GetSize())
        {
            m_ExpandedCapacityInFlight[frameIndex] = GetExpandedCapacity();
        }

        FMeshletExpandPushConstants expandPush;
        expandPush.Params[0] = boundsLimit;
        expandPush.Params[1] = GetExpandedCapacity();
        expandPush.Params[2] = 0;

        commandBuffer->PushConstants(m_ExpandPipelineLayout,
                                     EShaderStage::Compute, 0,
                                     sizeof(expandPush), &expandPush);

        commandBuffer->DispatchIndirect(rasterArgs, 0);

        FRHIBufferMemoryBarrier after[2] = {};

        after[0].SrcAccessMask = EAccessFlags::ShaderWrite;
        after[0].DstAccessMask = EAccessFlags::ShaderRead;
        after[0].Buffer        = m_ExpandedBuffers[frameIndex];

        after[1].SrcAccessMask = EAccessFlags::ShaderWrite;
        after[1].DstAccessMask = EAccessFlags::IndirectCommandRead;
        after[1].Buffer        = m_DrawArgsBuffers[frameIndex];

        commandBuffer->PipelineBarrier(
            EPipelineStageFlags::ComputeShader,
            EPipelineStageFlags::VertexShader |
                EPipelineStageFlags::DrawIndirect,
            nullptr, 0, after, 2, nullptr, 0);
    }

    // ---- 光栅化 ----
    FRHIClearDepthStencilValue clearDepth = {};
    clearDepth.Depth   = 1.0f;
    clearDepth.Stencil = 0;

    FRHIRenderPassBeginInfo beginInfo = {};
    beginInfo.RenderPass        = m_RenderPass;
    beginInfo.Framebuffer       = m_Framebuffer;
    beginInfo.RenderAreaOffset  = { 0, 0 };
    beginInfo.RenderAreaExtent  = m_Extent;
    // 可见性缓冲区清成 0 = "这里没有几何体"。
    //
    // 清除值走的是浮点通道 (FRHIClearColorValue 只有四个 Float32), 而
    // R32_UINT 附件上只有 0.0f 的位模式恰好是整数 0 —— 这正是编号里
    // 加一个 +1 偏移的原因: 让 0 空出来当空值。
    FRHIClearColorValue clearVisibility = {};
    clearVisibility.R = 0.0f;
    clearVisibility.G = 0.0f;
    clearVisibility.B = 0.0f;
    clearVisibility.A = 0.0f;

    beginInfo.ClearColors       = &clearVisibility;
    beginInfo.ClearColorCount   = 1;
    beginInfo.ClearDepthStencil = &clearDepth;

    commandBuffer->BeginRenderPass(beginInfo);

    FRHIViewport viewport = {};
    viewport.X        = 0.0f;
    viewport.Y        = 0.0f;
    viewport.Width    = static_cast<Float32>(m_Extent.Width);
    viewport.Height   = static_cast<Float32>(m_Extent.Height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    commandBuffer->SetViewport(viewport);

    FRHIScissorRect scissor = {};
    scissor.X      = 0;
    scissor.Y      = 0;
    scissor.Width  = m_Extent.Width;
    scissor.Height = m_Extent.Height;

    commandBuffer->SetScissor(scissor);

    if (m_Mode == EMode::MeshShader)
    {
        commandBuffer->BindGraphicsPipeline(m_MeshPipeline);
        commandBuffer->BindDescriptorSet(EPipelineBindPoint::Graphics,
                                         m_MeshPipelineLayout, 0,
                                         m_MeshSets[frameIndex]);

        commandBuffer->PushConstants(m_MeshPipelineLayout, EShaderStage::Mesh,
                                     0, sizeof(rasterPush), &rasterPush);

        commandBuffer->DrawMeshTasksIndirect(rasterArgs, 0, 1,
                                             sizeof(UInt32) * 4);
    }
    else
    {
        commandBuffer->BindGraphicsPipeline(m_FallbackPipeline);
        commandBuffer->BindDescriptorSet(EPipelineBindPoint::Graphics,
                                         m_FallbackPipelineLayout, 0,
                                         m_FallbackSets[frameIndex]);

        commandBuffer->PushConstants(m_FallbackPipelineLayout,
                                     EShaderStage::Vertex, 0,
                                     sizeof(rasterPush), &rasterPush);

        commandBuffer->DrawIndirect(m_DrawArgsBuffers[frameIndex], 0, 1,
                                    sizeof(FDrawIndirectCommand));
    }

    commandBuffer->EndRenderPass();

    // ================================================================
    // 两阶段遮挡剔除的后半段
    //
    //   1. 用这一帧刚画出来的深度建金字塔
    //   2. 把第一阶段被遮挡剔掉的重测一遍 (追加到可见表末尾)
    //   3. 把补上来的那一段画进同一对附件 (不清)
    //
    // 为什么这样就不再有近似: 一个物体如果真的被挡住, 那些遮挡物一定在
    // 第一阶段就画出来了 (它们自己没被挡住, 否则递归下去总有一层是没被
    // 挡的) —— 所以新金字塔里有它们, 结论正确。一个物体如果没被挡住,
    // 新金字塔里那片区域的深度就不会比它近, 它活下来。
    // ================================================================
    if (m_OcclusionCull)
    {
        BuildHiz(commandBuffer, frameIndex);

        // 把这一帧的金字塔交给剔除通道 —— 下一帧的第一阶段用它
        cull->SetHizPyramid(m_HizFullView, m_HizSampler, m_Extent.Width,
                            m_Extent.Height, m_HizLevels);

        // ---- 记下第一阶段结束时的可见数 ----
        //
        // 第二次绘制要从这里往后画。记之前必须挡一道 —— 第一阶段的
        // 光栅化还在读那个数 (间接参数), 而第二阶段马上要改它。
        {
            FRHIBufferCopyRegion region = {};
            region.SrcOffset = 0;
            region.DstOffset = 0;
            region.Size      = sizeof(UInt32);

            commandBuffer->CopyBuffer(cull->GetCounterBuffer(frameIndex),
                                      m_Phase1CountBuffers[frameIndex],
                                      region);

            // 第二阶段的分派参数: 先把 (0,1,1,0) 拷进去, 再用待定数
            // 覆盖 x。
            //
            // 顺序不能反 —— 反了的话 x 会被 0 盖掉。
            FRHIBufferCopyRegion patternRegion = {};
            patternRegion.SrcOffset = kResetDispatchOffset;
            patternRegion.DstOffset = 0;
            patternRegion.Size      = kDispatchPatternBytes;

            commandBuffer->CopyBuffer(m_ResetSource,
                                      m_Phase2DispatchBuffers[frameIndex],
                                      patternRegion);

            {
                FRHIBufferMemoryBarrier patternBarrier = {};
                patternBarrier.SrcAccessMask = EAccessFlags::TransferWrite;
                patternBarrier.DstAccessMask = EAccessFlags::TransferWrite;
                patternBarrier.Buffer = m_Phase2DispatchBuffers[frameIndex];

                commandBuffer->PipelineBarrier(
                    EPipelineStageFlags::Transfer,
                    EPipelineStageFlags::Transfer, nullptr, 0,
                    &patternBarrier, 1, nullptr, 0);
            }

            commandBuffer->CopyBuffer(
                cull->GetPendingCounterBuffer(frameIndex),
                m_Phase2DispatchBuffers[frameIndex], region);

            FRHIBufferMemoryBarrier barriers[2] = {};

            barriers[0].SrcAccessMask = EAccessFlags::TransferWrite;
            barriers[0].DstAccessMask = EAccessFlags::ShaderRead;
            barriers[0].Buffer        = m_Phase1CountBuffers[frameIndex];

            barriers[1].SrcAccessMask = EAccessFlags::TransferWrite;
            barriers[1].DstAccessMask = EAccessFlags::IndirectCommandRead;
            barriers[1].Buffer        = m_Phase2DispatchBuffers[frameIndex];

            commandBuffer->PipelineBarrier(
                EPipelineStageFlags::Transfer,
                EPipelineStageFlags::ComputeShader |
                    EPipelineStageFlags::DrawIndirect,
                nullptr, 0, barriers, 2, nullptr, 0);
        }

        // ---- 第二阶段的剔除 ----
        //
        // 间接分派: 工作组数来自待定计数器, 而那个数只有 GPU 知道。
        //
        // params 里的两个数都只是**上界** (待定表的容量), 真正的越界判断
        // 在着色器里用待定计数器本身做。拿上界当边界是不行的: 越界的线程
        // 会去读待定表里上一帧留下的记录, 而那是一块位置完全不对的几何体,
        // 会被当成"这一帧新露出来的"补画出来。
        {
            commandBuffer->BindComputePipeline(m_Phase2Pipeline);
            commandBuffer->BindDescriptorSet(EPipelineBindPoint::Compute,
                                             m_Phase2Layout, 0,
                                             m_Phase2Sets[frameIndex]);

            FPhase2PushConstants push;
            push.Params[0] = kMaxSceneMeshlets;
            push.Params[1] = kMaxSceneMeshlets;

            commandBuffer->PushConstants(m_Phase2Layout,
                                         EShaderStage::Compute, 0,
                                         sizeof(push), &push);

            commandBuffer->DispatchIndirect(
                m_Phase2DispatchBuffers[frameIndex], 0);
        }

        // ---- 补画 ----
        {
            // 两份都要挡, 而且**可见表那一份是必须的**:
            //
            //   * 计数器 —— 下面要拷进绘制的间接参数 (Transfer 读)
            //   * 可见表 —— 补画的网格着色器 (或回退路径的展开着色器)
            //     要读第二阶段刚追加进去的那一段
            //
            // 少了可见表那一条的后果很隐蔽: 第二阶段的统计一切正常 (它读
            // 的是计数器), 而画面纹丝不动 —— 补回来的 46 个 meshlet 一个
            // 都没画上去。开关遮挡剔除的画面差恰好一个像素不变, 前后两次
            // 都是 66774 个, 这个"完全没变"才是线索。
            FRHIBufferMemoryBarrier barriers[2] = {};

            barriers[0].SrcAccessMask = EAccessFlags::ShaderWrite;
            barriers[0].DstAccessMask = EAccessFlags::TransferRead;
            barriers[0].Buffer        = cull->GetCounterBuffer(frameIndex);

            barriers[1].SrcAccessMask = EAccessFlags::ShaderWrite;
            barriers[1].DstAccessMask = EAccessFlags::ShaderRead;
            barriers[1].Buffer = cull->GetVisibleMeshletBuffer(frameIndex);

            commandBuffer->PipelineBarrier(
                EPipelineStageFlags::ComputeShader,
                EPipelineStageFlags::Transfer |
                    EPipelineStageFlags::ComputeShader |
                    EPipelineStageFlags::VertexShader |
                    EPipelineStageFlags::MeshShader,
                nullptr, 0, barriers, 2, nullptr, 0);

            // 第二次绘制的工作组数 = 新的可见数 (着色器自己从起始槽位
            // 开始, 越界就退出)。
            //
            // 与分派参数**一模一样的坑**: 先拷 (0,1,1,0) 的模板, 再用可见
            // 数覆盖 x。y/z 是 0 的话 vkCmdDrawMeshTasksIndirectEXT 一个
            // 工作组都不起 —— 第二次绘制一个像素都画不出来。
            //
            // 这一个藏得比分派参数那个还深: "第二阶段补回来 N 个"那个统计
            // 读的是**计算着色器**写的计数器, 它一路正常, 而画面纹丝不动。
            // 把第二阶段改成无条件补回全部 113 个, 画面差还是分毫不差的
            // 66774 个像素 —— 三次差值完全相同, 才说明补画那一步整个是空的。
            FRHIBufferCopyRegion region = {};
            region.SrcOffset = 0;
            region.DstOffset = 0;
            region.Size      = sizeof(UInt32);

            FRHIBufferCopyRegion patternRegion = {};
            patternRegion.SrcOffset = kResetDispatchOffset;
            patternRegion.DstOffset = 0;
            patternRegion.Size      = kDispatchPatternBytes;

            commandBuffer->CopyBuffer(m_ResetSource,
                                      m_Phase2RasterArgsBuffers[frameIndex],
                                      patternRegion);

            {
                FRHIBufferMemoryBarrier patternBarrier = {};
                patternBarrier.SrcAccessMask = EAccessFlags::TransferWrite;
                patternBarrier.DstAccessMask = EAccessFlags::TransferWrite;
                patternBarrier.Buffer =
                    m_Phase2RasterArgsBuffers[frameIndex];

                commandBuffer->PipelineBarrier(
                    EPipelineStageFlags::Transfer,
                    EPipelineStageFlags::Transfer, nullptr, 0,
                    &patternBarrier, 1, nullptr, 0);
            }

            commandBuffer->CopyBuffer(cull->GetCounterBuffer(frameIndex),
                                      m_Phase2RasterArgsBuffers[frameIndex],
                                      region);

            FRHIBufferMemoryBarrier argsBarrier = {};
            argsBarrier.SrcAccessMask = EAccessFlags::TransferWrite;
            argsBarrier.DstAccessMask = EAccessFlags::IndirectCommandRead;
            argsBarrier.Buffer = m_Phase2RasterArgsBuffers[frameIndex];

            commandBuffer->PipelineBarrier(EPipelineStageFlags::Transfer,
                                           EPipelineStageFlags::DrawIndirect,
                                           nullptr, 0, &argsBarrier, 1,
                                           nullptr, 0);
        }

        // 第二遍渲染通道 —— 附件**加载**而不是清除
        FRHIRenderPassBeginInfo secondPass = beginInfo;
        secondPass.RenderPass  = m_LoadRenderPass;
        secondPass.Framebuffer = m_LoadFramebuffer;

        commandBuffer->BeginRenderPass(secondPass);

        commandBuffer->SetViewport(viewport);
        commandBuffer->SetScissor(scissor);

        // 起始槽位由第一阶段的可见数给出 —— 但那个数在 GPU 上。
        //
        // 网格着色器读不到间接参数以外的 GPU 数据来做偏移, 所以这里退一步:
        // 从 **0** 开始画整张可见表。第一阶段那一段会被再画一遍, 而深度
        // 测试与可见性写入都是幂等的 (同样的三角形、同样的深度、同样的
        // 编号), 画面完全一样。
        //
        // 代价是多一遍第一阶段的光栅化。真正的省法是把起始槽位也放进
        // 间接参数 —— 那要给绘制命令加一个"first workgroup"的概念, 而
        // Vulkan 的 DrawMeshTasksIndirect 没有。留在这里说清楚, 不假装
        // 它已经解决了。
        rasterPush.Params[1] = 0;

        if (m_Mode == EMode::MeshShader)
        {
            commandBuffer->BindGraphicsPipeline(m_MeshPipelineLoad);
            commandBuffer->BindDescriptorSet(EPipelineBindPoint::Graphics,
                                             m_MeshPipelineLayout, 0,
                                             m_MeshSets[frameIndex]);

            commandBuffer->PushConstants(m_MeshPipelineLayout,
                                         EShaderStage::Mesh, 0,
                                         sizeof(rasterPush), &rasterPush);

            commandBuffer->DrawMeshTasksIndirect(
                m_Phase2RasterArgsBuffers[frameIndex], 0, 1,
                sizeof(UInt32) * 4);
        }
        else
        {
            // 回退路径要先把补上来的那一段展开
            commandBuffer->EndRenderPass();

            {
                commandBuffer->BindComputePipeline(m_ExpandPipeline);
                commandBuffer->BindDescriptorSet(EPipelineBindPoint::Compute,
                                                 m_ExpandPipelineLayout, 0,
                                                 m_ExpandSets[frameIndex]);

                FMeshletExpandPushConstants expandPush;
                expandPush.Params[0] = boundsLimit;
                expandPush.Params[1] = GetExpandedCapacity();
                expandPush.Params[2] = 0;

                commandBuffer->PushConstants(m_ExpandPipelineLayout,
                                             EShaderStage::Compute, 0,
                                             sizeof(expandPush), &expandPush);

                commandBuffer->DispatchIndirect(
                    m_Phase2RasterArgsBuffers[frameIndex], 0);

                FRHIBufferMemoryBarrier after[2] = {};

                after[0].SrcAccessMask = EAccessFlags::ShaderWrite;
                after[0].DstAccessMask = EAccessFlags::ShaderRead;
                after[0].Buffer        = m_ExpandedBuffers[frameIndex];

                after[1].SrcAccessMask = EAccessFlags::ShaderWrite;
                after[1].DstAccessMask = EAccessFlags::IndirectCommandRead;
                after[1].Buffer        = m_DrawArgsBuffers[frameIndex];

                commandBuffer->PipelineBarrier(
                    EPipelineStageFlags::ComputeShader,
                    EPipelineStageFlags::VertexShader |
                        EPipelineStageFlags::DrawIndirect,
                    nullptr, 0, after, 2, nullptr, 0);
            }

            commandBuffer->BeginRenderPass(secondPass);

            commandBuffer->SetViewport(viewport);
            commandBuffer->SetScissor(scissor);

            commandBuffer->BindGraphicsPipeline(m_FallbackPipelineLoad);
            commandBuffer->BindDescriptorSet(EPipelineBindPoint::Graphics,
                                             m_FallbackPipelineLayout, 0,
                                             m_FallbackSets[frameIndex]);

            commandBuffer->PushConstants(m_FallbackPipelineLayout,
                                         EShaderStage::Vertex, 0,
                                         sizeof(rasterPush), &rasterPush);

            commandBuffer->DrawIndirect(m_DrawArgsBuffers[frameIndex], 0, 1,
                                        sizeof(FDrawIndirectCommand));
        }

        commandBuffer->EndRenderPass();

        // ---- 两个数的回读: 第一阶段可见数, 与第二阶段之后的最终数 ----
        //
        // 判据要拿它们的差判"第二阶段是不是真的补回来过东西"。
        //
        // **最终数必须自己读**, 不能用剔除通道的统计: 那份统计是剔除通道
        // 在**自己的** Execute 末尾拷的, 而那时第二阶段还没跑 —— 于是它
        // 永远等于第一阶段的数, 两者相减恒为零。
        //
        // 第一版正是这么写的, 结果是"第二阶段补回来 0 个"恒成立, 而那个
        // 恒成立掩盖了另一个缺陷 (分派参数的 y/z 没置 1)。**一个恒为零的
        // 诊断量比没有诊断量更糟**。
        {
            FRHIBufferCopyRegion region = {};
            region.SrcOffset = 0;
            region.DstOffset = 0;
            region.Size      = sizeof(UInt32);

            commandBuffer->CopyBuffer(m_Phase1CountBuffers[frameIndex],
                                      m_Phase1Readbacks[frameIndex], region);

            commandBuffer->CopyBuffer(cull->GetCounterBuffer(frameIndex),
                                      m_Phase2FinalReadbacks[frameIndex],
                                      region);
        }
    }

    // ================================================================
    // 材质解析
    // ================================================================
    if (m_ResolveEnabled)
    {
        // 逐实例的材质下标 —— 每帧上传, 因为实例表每帧重建
        {
            void* mapped = nullptr;

            if (IsRHISuccess(m_Device->MapBuffer(m_MaterialBuffers[frameIndex],
                                                 &mapped)) &&
                mapped != nullptr)
            {
                auto* target = static_cast<UInt32*>(mapped);

                const TArray<FMeshletInstanceGpu>& instances =
                    cull->GetInstances();

                const TArray<FRenderObject>& objects =
                    *context.ShadowCasterObjects;

                for (SizeType i = 0; i < instances.GetSize(); ++i)
                {
                    // MeshletRange[2] 存的是**源对象下标** —— 那是实例表
                    // 与对象列表之间唯一的对应关系。用实例序号去索引对象
                    // 列表的话, 只要有一个对象被跳过 (半透明、无 meshlet),
                    // 后面所有物体的材质就整体错位一格。
                    //
                    // 这与 Day 5 光追几何表踩过的是同一个坑。
                    const UInt32 source = instances[i].MeshletRange[2];

                    target[i] = (source < objects.GetSize())
                                    ? objects[source].BindlessMaterialIndex
                                    : 0u;
                }

                m_Device->UnmapBuffer(m_MaterialBuffers[frameIndex]);
            }
        }

        // 可见性纹理从颜色附件转成通用布局 —— 计算着色器要按存储图像读它
        commandBuffer->TransitionImageLayout(
            m_VisibilityTexture, EImageLayout::ShaderReadOnly,
            EImageLayout::General,
            EPipelineStageFlags::ColorAttachmentOutput,
            EPipelineStageFlags::ComputeShader,
            EAccessFlags::ColorAttachmentWrite, EAccessFlags::ShaderRead);

        commandBuffer->BindComputePipeline(m_ResolvePipeline);
        commandBuffer->BindDescriptorSet(EPipelineBindPoint::Compute,
                                         m_ResolvePipelineLayout, 0,
                                         m_ResolveSets[frameIndex]);

        FMeshletResolvePushConstants resolvePush;

        for (UInt32 i = 0; i < 16; ++i)
        {
            resolvePush.ViewProj[i] = rasterPush.ViewProj[i];
        }

        resolvePush.Params[0] = m_Extent.Width;
        resolvePush.Params[1] = m_Extent.Height;

        commandBuffer->PushConstants(m_ResolvePipelineLayout,
                                     EShaderStage::Compute, 0,
                                     sizeof(resolvePush), &resolvePush);

        const UInt32 groupsX =
            (m_Extent.Width + kResolveWorkgroupSize - 1u) /
            kResolveWorkgroupSize;

        const UInt32 groupsY =
            (m_Extent.Height + kResolveWorkgroupSize - 1u) /
            kResolveWorkgroupSize;

        commandBuffer->Dispatch(groupsX, groupsY, 1);

        // 转回来 —— 描述符里声明的静止布局是 ShaderReadOnly, 而 Vulkan
        // 在提交时检查声明与实际是否一致, 与有没有人读无关。
        commandBuffer->TransitionImageLayout(
            m_VisibilityTexture, EImageLayout::General,
            EImageLayout::ShaderReadOnly,
            EPipelineStageFlags::ComputeShader,
            EPipelineStageFlags::ColorAttachmentOutput,
            EAccessFlags::ShaderRead, EAccessFlags::ColorAttachmentWrite);

        FRHIBufferMemoryBarrier barrier = {};
        barrier.SrcAccessMask = EAccessFlags::ShaderWrite;
        barrier.DstAccessMask = EAccessFlags::TransferRead;
        barrier.Buffer        = m_ResolveBuffers[frameIndex];

        commandBuffer->PipelineBarrier(EPipelineStageFlags::ComputeShader,
                                       EPipelineStageFlags::Transfer, nullptr,
                                       0, &barrier, 1, nullptr, 0);
    }

    // ================================================================
    // 展开顶点流的两个数拷去回读缓冲区
    //
    // 放在最末尾 —— 两阶段遮挡剔除下展开跑两次, 第二次接着往同一条流里
    // 追加。中途拷的话读到的是第一次的数, 而溢出恰恰是第二次才发生的那种
    // 情形会整个漏掉。
    //
    // 源阶段是**顶点着色器**而不是计算着色器: 最后碰这块缓冲区的是
    // vkCmdDrawIndirect 那次读, 而计算着色器的写在它之前已经被上面那道
    // 屏障发布过了。
    // ================================================================
    if (m_Mode == EMode::Fallback && frameIndex < m_DrawArgsReadbacks.GetSize())
    {
        FRHIBufferMemoryBarrier barrier = {};
        barrier.SrcAccessMask = EAccessFlags::ShaderWrite;
        barrier.DstAccessMask = EAccessFlags::TransferRead;
        barrier.Buffer        = m_DrawArgsBuffers[frameIndex];

        commandBuffer->PipelineBarrier(EPipelineStageFlags::ComputeShader,
                                       EPipelineStageFlags::Transfer, nullptr,
                                       0, &barrier, 1, nullptr, 0);

        FRHIBufferCopyRegion region = {};
        region.SrcOffset = 0;
        region.DstOffset = 0;
        region.Size      = kDrawArgsBytes;

        commandBuffer->CopyBuffer(m_DrawArgsBuffers[frameIndex],
                                  m_DrawArgsReadbacks[frameIndex], region);
    }

    commandBuffer->EndDebugLabel();
}

FRHIBufferHandle FMeshletDepthPass::GetResolveBuffer(UInt32 frameIndex) const
{
    return (frameIndex < m_ResolveBuffers.GetSize())
               ? m_ResolveBuffers[frameIndex]
               : FRHIBufferHandle();
}

// ============================================================================
// 其余接口
// ============================================================================

ERHIResult FMeshletDepthPass::OnResize(const FPassResizeDesc& desc)
{
    if (desc.Device == nullptr)
    {
        return ERHIResult::Success;
    }

    DestroyDepthTarget(desc.Device);

    ERHIResult result = CreateDepthTarget(desc.Device, desc.Extent);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    const FRHITextureViewHandle views[2] = { m_VisibilityView, m_DepthView };

    FRHIFramebufferDesc fbDesc = {};
    fbDesc.RenderPass      = m_RenderPass;
    fbDesc.Attachments     = views;
    fbDesc.AttachmentCount = 2;
    fbDesc.Width           = m_Extent.Width;
    fbDesc.Height          = m_Extent.Height;
    fbDesc.Layers          = 1;
    fbDesc.DebugName       = "MeshletDepth_Framebuffer";

    return desc.Device->CreateFramebuffer(fbDesc, m_Framebuffer);
}

void FMeshletDepthPass::ReleaseSwapchainResources(IRHIDevice* device)
{
    if (device != nullptr)
    {
        DestroyDepthTarget(device);
    }
}

void FMeshletDepthPass::Shutdown(IRHIDevice* device)
{
    if (device == nullptr)
    {
        return;
    }

    DestroyDepthTarget(device);

    if (m_RenderPass.IsValid())
    {
        device->DestroyRenderPass(m_RenderPass);
    }

    if (m_LoadRenderPass.IsValid())
    {
        device->DestroyRenderPass(m_LoadRenderPass);
    }

    if (m_MeshPipelineLoad.IsValid())
    {
        device->DestroyGraphicsPipeline(m_MeshPipelineLoad);
    }

    if (m_FallbackPipelineLoad.IsValid())
    {
        device->DestroyGraphicsPipeline(m_FallbackPipelineLoad);
    }

    if (m_MeshPipeline.IsValid())
    {
        device->DestroyGraphicsPipeline(m_MeshPipeline);
    }

    if (m_FallbackPipeline.IsValid())
    {
        device->DestroyGraphicsPipeline(m_FallbackPipeline);
    }

    if (m_ExpandPipeline.IsValid())
    {
        device->DestroyComputePipeline(m_ExpandPipeline);
    }

    if (m_ResolvePipeline.IsValid())
    {
        device->DestroyComputePipeline(m_ResolvePipeline);
    }

    // ---- Hi-Z 与第二阶段 ----
    const auto DestroyComputePipeline = [device](
                                            FRHIComputePipelineHandle& handle)
    {
        if (handle.IsValid())
        {
            device->DestroyComputePipeline(handle);
        }
    };

    DestroyComputePipeline(m_HizCopyPipeline);
    DestroyComputePipeline(m_HizBuildPipeline);
    DestroyComputePipeline(m_Phase2Pipeline);

    for (SizeType i = 0; i < m_HizLevelViews.GetSize(); ++i)
    {
        if (m_HizLevelViews[i].IsValid())
        {
            device->DestroyTextureView(m_HizLevelViews[i]);
        }
    }

    m_HizLevelViews.Clear();

    if (m_HizFullView.IsValid())
    {
        device->DestroyTextureView(m_HizFullView);
    }

    if (m_HizTexture.IsValid())
    {
        device->DestroyTexture(m_HizTexture);
    }

    if (m_HizSampler.IsValid())
    {
        device->DestroySampler(m_HizSampler);
    }

    if (m_DepthSampler.IsValid())
    {
        device->DestroySampler(m_DepthSampler);
    }

    m_HizBuildSets.Clear();
    m_Phase2Sets.Clear();

    if (m_VisibilityStorageView.IsValid())
    {
        device->DestroyTextureView(m_VisibilityStorageView);
    }

    const auto DestroyLayout = [device](FRHIPipelineLayoutHandle& handle)
    {
        if (handle.IsValid())
        {
            device->DestroyPipelineLayout(handle);
        }
    };

    DestroyLayout(m_MeshPipelineLayout);
    DestroyLayout(m_ExpandPipelineLayout);
    DestroyLayout(m_FallbackPipelineLayout);
    DestroyLayout(m_ResolvePipelineLayout);
    DestroyLayout(m_HizCopyLayout);
    DestroyLayout(m_HizBuildLayout);
    DestroyLayout(m_Phase2Layout);

    const auto DestroySetLayout = [device](FRHIDescSetLayoutHandle& handle)
    {
        if (handle.IsValid())
        {
            device->DestroyDescSetLayout(handle);
        }
    };

    DestroySetLayout(m_MeshSetLayout);
    DestroySetLayout(m_ExpandSetLayout);
    DestroySetLayout(m_FallbackSetLayout);
    DestroySetLayout(m_ResolveSetLayout);
    DestroySetLayout(m_HizCopySetLayout);
    DestroySetLayout(m_HizBuildSetLayout);
    DestroySetLayout(m_Phase2SetLayout);

    const auto DestroyShader = [device](FRHIShaderHandle& handle)
    {
        if (handle.IsValid())
        {
            device->DestroyShader(handle);
        }
    };

    DestroyShader(m_MeshShader);
    DestroyShader(m_FragmentShader);
    DestroyShader(m_FallbackFragmentShader);
    DestroyShader(m_ExpandShader);
    DestroyShader(m_FallbackVertexShader);
    DestroyShader(m_ResolveShader);
    DestroyShader(m_HizCopyShader);
    DestroyShader(m_HizBuildShader);
    DestroyShader(m_Phase2Shader);

    const auto DestroyBuffers = [device](TArray<FRHIBufferHandle>& buffers)
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

    DestroyBuffers(m_ExpandedBuffers);
    DestroyBuffers(m_DrawArgsBuffers);
    DestroyBuffers(m_ResolveBuffers);
    DestroyBuffers(m_MaterialBuffers);
    DestroyBuffers(m_Phase2DispatchBuffers);
    DestroyBuffers(m_Phase2RasterArgsBuffers);
    DestroyBuffers(m_Phase1CountBuffers);
    DestroyBuffers(m_Phase1Readbacks);
    DestroyBuffers(m_Phase2FinalReadbacks);
    DestroyBuffers(m_DrawArgsReadbacks);

    m_ExpandedCapacityInFlight.Clear();

    if (m_ResetSource.IsValid())
    {
        device->DestroyBuffer(m_ResetSource);
    }

    m_MeshSets.Clear();
    m_ExpandSets.Clear();
    m_FallbackSets.Clear();
    m_ResolveSets.Clear();

    m_Device = nullptr;
}

// ============================================================================
// CreateHizResources — 金字塔纹理、逐级视图、采样器
//
// 尺寸不对齐到二的幂。对齐要么放大 (浪费显存与带宽), 要么缩小 (丢掉边上
// 的遮挡信息 —— 那一条边上的物体会被判成"没有遮挡物"而不剔, 保守但白白
// 损失剔除率)。逐级减半**向上取整**, 边界处用钳边取样。
// ============================================================================

ERHIResult FMeshletDepthPass::CreateHizResources(IRHIDevice* device,
                                                 FRHIExtent2D extent)
{
    m_HizLevels = ComputeHizLevels(extent.Width, extent.Height);

    FRHITextureDesc texDesc = {};
    texDesc.Type        = ETextureType::Texture2D;
    texDesc.Format      = EPixelFormat::R32_SFLOAT;
    texDesc.Extent      = { extent.Width, extent.Height, 1 };
    texDesc.MipLevels   = m_HizLevels;
    texDesc.ArrayLayers = 1;
    texDesc.Samples     = ESampleCount::Count1;
    texDesc.Usage       = static_cast<ETextureUsage>(
        static_cast<UInt32>(ETextureUsage::Sampled) |
        static_cast<UInt32>(ETextureUsage::Storage) |
        static_cast<UInt32>(ETextureUsage::TransferSrc));
    texDesc.MemoryUsage = EMemoryUsage::GpuOnly;
    texDesc.DebugName   = "MeshletHiz";

    ERHIResult result = device->CreateTexture(texDesc, m_HizTexture);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    // 逐级视图 (存储图像只能绑单级) + 一个覆盖全部级的视图 (采样用)
    for (UInt32 level = 0; level < m_HizLevels; ++level)
    {
        FRHITextureViewDesc viewDesc = {};
        viewDesc.Texture         = m_HizTexture;
        viewDesc.ViewType        = ETextureType::Texture2D;
        viewDesc.Format          = EPixelFormat::R32_SFLOAT;
        viewDesc.BaseMipLevel    = level;
        viewDesc.MipLevelCount   = 1;
        viewDesc.BaseArrayLayer  = 0;
        viewDesc.ArrayLayerCount = 1;

        FRHITextureViewHandle view;

        result = device->CreateTextureView(viewDesc, view);

        if (!IsRHISuccess(result))
        {
            return result;
        }

        m_HizLevelViews.Add(view);
    }

    {
        FRHITextureViewDesc viewDesc = {};
        viewDesc.Texture         = m_HizTexture;
        viewDesc.ViewType        = ETextureType::Texture2D;
        viewDesc.Format          = EPixelFormat::R32_SFLOAT;
        viewDesc.BaseMipLevel    = 0;
        viewDesc.MipLevelCount   = m_HizLevels;
        viewDesc.BaseArrayLayer  = 0;
        viewDesc.ArrayLayerCount = 1;

        result = device->CreateTextureView(viewDesc, m_HizFullView);

        if (!IsRHISuccess(result))
        {
            return result;
        }
    }

    // 采样器: 最近邻 + 钳边。
    //
    // 线性过滤是错的 —— 插值出来的"最大深度"不是任何一片区域的最大深度,
    // 而遮挡测试的保守性正建立在"那个数确实是最大值"上。
    FRHISamplerDesc samplerDesc = {};
    samplerDesc.MinFilter    = EFilter::Nearest;
    samplerDesc.MagFilter    = EFilter::Nearest;
    samplerDesc.MipmapMode   = ESamplerMipmapMode::Nearest;
    samplerDesc.AddressModeU = ESamplerAddressMode::ClampToEdge;
    samplerDesc.AddressModeV = ESamplerAddressMode::ClampToEdge;
    samplerDesc.AddressModeW = ESamplerAddressMode::ClampToEdge;
    samplerDesc.MaxLod       = static_cast<Float32>(m_HizLevels);

    result = device->CreateSampler(samplerDesc, m_HizSampler);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    // 深度图的采样器 —— 第 0 级拷贝要用
    return device->CreateSampler(samplerDesc, m_DepthSampler);
}

// ============================================================================
// BuildHiz — 把这一帧的深度收成金字塔
// ============================================================================

void FMeshletDepthPass::BuildHiz(IRHICommandBuffer* commandBuffer,
                                 UInt32 frameIndex)
{
    LIMX_UNUSED(frameIndex);

    commandBuffer->BeginDebugLabel("MeshletHiz", 0.3f, 0.6f, 0.9f);

    // 深度图从附件转成可采样
    commandBuffer->TransitionImageLayout(
        m_DepthTexture, EImageLayout::DepthStencilAttachment,
        EImageLayout::ShaderReadOnly, EPipelineStageFlags::LateFragmentTests,
        EPipelineStageFlags::ComputeShader,
        EAccessFlags::DepthStencilAttachmentWrite, EAccessFlags::ShaderRead);

    // 整张金字塔转成通用布局 (存储图像写)
    //
    // **必须带上全部 mip 级。** 默认参数只转一级 —— 而金字塔有十一级,
    // 漏掉的那十级会停在上一次的布局上。验证层会逐级报"布局与描述符声明
    // 的不符", 而关掉验证层就是未定义行为。
    commandBuffer->TransitionImageLayout(
        m_HizTexture, EImageLayout::Undefined, EImageLayout::General,
        EPipelineStageFlags::TopOfPipe, EPipelineStageFlags::ComputeShader,
        EAccessFlags::None, EAccessFlags::ShaderWrite, 0, m_HizLevels);

    // ---- 第 0 级: 拷贝 ----
    {
        commandBuffer->BindComputePipeline(m_HizCopyPipeline);
        commandBuffer->BindDescriptorSet(EPipelineBindPoint::Compute,
                                         m_HizCopyLayout, 0, m_HizCopySet);

        FHizPushConstants push;
        push.Params[0] = m_Extent.Width;
        push.Params[1] = m_Extent.Height;

        commandBuffer->PushConstants(m_HizCopyLayout, EShaderStage::Compute, 0,
                                     sizeof(push), &push);

        commandBuffer->Dispatch(
            (m_Extent.Width + kHizWorkgroupSize - 1u) / kHizWorkgroupSize,
            (m_Extent.Height + kHizWorkgroupSize - 1u) / kHizWorkgroupSize, 1);
    }

    // ---- 逐级归约 ----
    //
    // 每一级都要等上一级写完 —— 少一道屏障的表现是金字塔高层混进了
    // 未写完的数据, 而那是**随机**的遮挡结论: 物体随机消失一帧。
    commandBuffer->BindComputePipeline(m_HizBuildPipeline);

    for (UInt32 level = 1; level < m_HizLevels; ++level)
    {
        FRHIImageMemoryBarrier barrier = {};
        barrier.SrcAccessMask = EAccessFlags::ShaderWrite;
        barrier.DstAccessMask = EAccessFlags::ShaderRead;
        barrier.OldLayout     = EImageLayout::General;
        barrier.NewLayout     = EImageLayout::General;
        barrier.Texture       = m_HizTexture;
        barrier.BaseMipLevel  = 0;
        barrier.MipLevelCount = m_HizLevels;

        commandBuffer->PipelineBarrier(EPipelineStageFlags::ComputeShader,
                                       EPipelineStageFlags::ComputeShader,
                                       nullptr, 0, nullptr, 0, &barrier, 1);

        commandBuffer->BindDescriptorSet(EPipelineBindPoint::Compute,
                                         m_HizBuildLayout, 0,
                                         m_HizBuildSets[level - 1]);

        const FRHIExtent2D source = HizLevelExtent(m_Extent, level - 1);
        const FRHIExtent2D target = HizLevelExtent(m_Extent, level);

        FHizPushConstants push;
        push.Params[0] = source.Width;
        push.Params[1] = source.Height;
        push.Params[2] = target.Width;
        push.Params[3] = target.Height;

        commandBuffer->PushConstants(m_HizBuildLayout, EShaderStage::Compute,
                                     0, sizeof(push), &push);

        commandBuffer->Dispatch(
            (target.Width + kHizWorkgroupSize - 1u) / kHizWorkgroupSize,
            (target.Height + kHizWorkgroupSize - 1u) / kHizWorkgroupSize, 1);
    }

    // 转成可采样 —— 剔除通道下一帧要读它。同样要带上全部 mip 级。
    commandBuffer->TransitionImageLayout(
        m_HizTexture, EImageLayout::General, EImageLayout::ShaderReadOnly,
        EPipelineStageFlags::ComputeShader, EPipelineStageFlags::ComputeShader,
        EAccessFlags::ShaderWrite, EAccessFlags::ShaderRead, 0, m_HizLevels);

    // 深度图转回附件 —— 第二次绘制还要写它
    commandBuffer->TransitionImageLayout(
        m_DepthTexture, EImageLayout::ShaderReadOnly,
        EImageLayout::DepthStencilAttachment,
        EPipelineStageFlags::ComputeShader,
        EPipelineStageFlags::EarlyFragmentTests, EAccessFlags::ShaderRead,
        EAccessFlags::DepthStencilAttachmentWrite);

    commandBuffer->EndDebugLabel();
}

void FMeshletDepthPass::SetOcclusionCullEnabled(bool enabled)
{
    m_OcclusionCull = enabled;
}

} // namespace Limx
