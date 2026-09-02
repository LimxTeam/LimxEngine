// ============================================================
// 文件名称：FDepthPrePass.cpp
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：极简深度渲染 — 深度预 Pass 只关注几何遮挡关系，
//          不执行任何材质/光照计算，以最低 GPU 开销将整个场景
//          的深度信息写入共享深度缓冲区，为 FForwardPass 的 Equal
//          深度测试提供精确的遮挡数据。
// 功能描述：FDepthPrePass 完整实现 — 创建 depth-only RenderPass
//          (无颜色附件)，使用 gbuffer.vert/frag 着色器和共享深度视图
//          创建单一 Framebuffer，渲染所有场景物体写入深度缓冲区。
//          Execute 结束后插入管线屏障，确保深度写入完成前不进入
//          后续 FForwardPass 的 EarlyFragmentTests 阶段。
// 技术特性：depth-only RenderPass: 无颜色附件, 单深度附件 Clear/Store;
//          单一 Framebuffer (共享深度视图);
//          Pipeline: DepthCompareOp=Less, DepthWrite=true, 无颜色混合;
//          Execute 末尾: LateFragmentTests→EarlyFragmentTests 管线屏障。
//
// ── 函数表 ──────────────────────────────────────────────────
// │ 函数名                       │ 描述                            │
// │─────────────────────────────│────────────────────────────────│
// │ Setup()                     │ 创建深度 RenderPass/FB/Shader/管线 │
// │ Execute()                   │ 录制深度渲染 + 管线屏障            │
// │ OnResize()                  │ 重建深度 Framebuffer              │
// │ Shutdown()                  │ 释放所有 GPU 资源                 │
// │ CreateDepthRenderPass()     │ 创建仅含深度附件的渲染通道          │
// │ CreateDepthFramebuffer()    │ 创建 depth-only 帧缓冲             │
// │ DestroyDepthFramebuffer()   │ 销毁帧缓冲                        │
// │ CreateShaders()             │ 加载 gbuffer.vert/frag        │
// │ CreateDepthPipeline()       │ 创建 depth-only 图形管线           │
// │ DestroyPipelineResources()  │ 销毁管线和着色器                   │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M0.5 Early-Z 深度预 Pass) │
// ============================================================

#include "Renderer/RenderPass/FDepthPrePass.h"
#include "RenderCore/Shaders/FShaderManager.h"
#include "Renderer/RenderPass/FGpuCullPass.h"


namespace Limx
{

namespace
{

/// 逐组下 DrawIndexedIndirect
///
/// 深度预通道与前向通道用的是同一段代码。抄两遍的话, 其中一处改了另一处
/// 没改的表现是"深度预通道与前向通道画的东西不一样" —— 而前向通道的深度
/// 测试是 Equal, 那会让整片几何直接消失。
///
/// 返回下发的组数, 供统计。
static UInt32 RecordIndirectGroups(
    IRHICommandBuffer*        commandBuffer,
    const FRenderPassContext& context,
    const FGpuCullPass&       cull,
    FRHIGraphicsPipelineHandle singleSided,
    FRHIGraphicsPipelineHandle doubleSided,
    UInt32                     viewIndex)
{
    const FRHIBufferHandle indirect =
        cull.GetIndirectBuffer(context.FrameIndex);

    if (!indirect.IsValid())
    {
        return 0;
    }

    const TArray<FDrawGroup>& groups = cull.GetGroups();

    FRHIGraphicsPipelineHandle boundPipeline;
    FRHIBufferHandle           boundVertexBuffer;
    FRHIBufferHandle           boundIndexBuffer;
    EIndexType                 boundIndexType = EIndexType::UInt32;

    for (SizeType g = 0; g < groups.GetSize(); ++g)
    {
        const FDrawGroup& group = groups[g];

        if (group.CommandCount == 0)
        {
            continue;
        }

        const FRHIGraphicsPipelineHandle pipeline =
            group.IsDoubleSided ? doubleSided : singleSided;

        if (pipeline.Packed != boundPipeline.Packed)
        {
            commandBuffer->BindGraphicsPipeline(pipeline);
            boundPipeline = pipeline;
        }

        if (group.VertexBuffer.Packed != boundVertexBuffer.Packed)
        {
            commandBuffer->BindVertexBuffer(0, group.VertexBuffer, 0);
            boundVertexBuffer = group.VertexBuffer;
        }

        if (group.IndexBuffer.Packed != boundIndexBuffer.Packed ||
            group.IndexType != boundIndexType)
        {
            commandBuffer->BindIndexBuffer(group.IndexBuffer, 0,
                                           group.IndexType);
            boundIndexBuffer = group.IndexBuffer;
            boundIndexType   = group.IndexType;
        }

        // 一次下发整组。被剔除的那些命令 instanceCount 是 0, 命令处理器
        // 直接跳过 —— 不会走到顶点着色器。
        commandBuffer->DrawIndexedIndirect(
            indirect,
            (static_cast<UInt64>(FGpuCullPass::ViewCommandBase(viewIndex)) +
             group.FirstCommand) *
                sizeof(FDrawIndexedIndirectCommand),
            group.CommandCount,
            static_cast<UInt32>(sizeof(FDrawIndexedIndirectCommand)));
    }

    return static_cast<UInt32>(groups.GetSize());
}

/// 绑 set 3 —— 逐物体数据
///
/// **两条路径都要绑。** 顶点着色器只有一条代码路径, 它总是从这里取模型矩阵
/// 与材质下标; 逐物体绘制那条路径只是把物体下标经 firstInstance 传进去。
static void BindDrawObjectSet(IRHICommandBuffer*        commandBuffer,
                              const FRenderPassContext& context)
{
    if (context.GpuCull == nullptr)
    {
        return;
    }

    const FRHIDescriptorSetHandle set =
        context.GpuCull->GetDrawObjectSet(context.FrameIndex);

    if (!set.IsValid())
    {
        return;
    }

    commandBuffer->BindDescriptorSet(EPipelineBindPoint::Graphics,
                                     context.PipelineLayout, 3, set,
                                     nullptr, 0);
}

} // namespace

LIMX_DECLARE_LOG_CATEGORY(LogRenderer)

// ============================================================================
// Setup — 创建全部 GPU 资源
// ============================================================================

ERHIResult FDepthPrePass::Setup(const FPassSetupDesc& desc)
{
    LIMX_CHECK(desc.Device != nullptr);

    m_PipelineLayout      = desc.PipelineLayout;
    m_SharedDepthTexture  = desc.SharedDepthTexture;
    m_SwapchainExtent     = desc.SwapchainExtent;

    // 1. 创建 depth-only 渲染通道
    // G-Buffer 附件必须先于渲染通道创建 —— 通道要用它们的格式,
    // 帧缓冲要用它们的视图。
    ERHIResult result = CreateGBufferTargets(desc.Device, desc.SwapchainExtent);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    result = CreateDepthRenderPass(desc.Device);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
                 "[DepthPrePass] depth-only RenderPass 创建失败");
        return result;
    }

    // 2. 创建 depth-only Framebuffer (单一，使用共享深度视图)
    result = CreateDepthFramebuffer(desc.Device,
                                     desc.SwapchainExtent,
                                     desc.SharedDepthTextureView);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
                 "[DepthPrePass] depth-only Framebuffer 创建失败");
        return result;
    }

    // 3. 加载 depth_only 着色器模块
    result = CreateShaders(desc.Device);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
                 "[DepthPrePass] 着色器模块创建失败");
        return result;
    }

    // 4. 创建 depth-only 图形管线
    for (SizeType variant = 0;
         variant < kPipelineVariantCount && IsRHISuccess(result); ++variant)
    {
        result = CreateDepthPipeline(desc.Device, variant != 0,
                                     m_DepthPipelines[variant]);
    }
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
                 "[DepthPrePass] depth-only 管线创建失败");
        return result;
    }

    LIMX_LOG(LogRenderer, Log,
             "[DepthPrePass] 初始化完成 — {}x{} (depth-only)",
             desc.SwapchainExtent.Width, desc.SwapchainExtent.Height);

    return ERHIResult::Success;
}

// ============================================================================
// Execute — 录制深度渲染命令 + 插入管线屏障
// ============================================================================

// ============================================================================
// 录制辅助 — 内联路径与并行路径共用
//
// 与前向、阴影两个 Pass 同一模式: 次级命令缓冲区不继承任何绑定状态,
// 公共状态每段各设一次; 绘制代码只有一份, 逐像素比对才检验得了并行本身。
// ============================================================================

void FDepthPrePass::RecordCommonState(IRHICommandBuffer*        commandBuffer,
                                       const FRenderPassContext& context)
{

    // ================================================================
    // 绑定 depth-only 管线 + 设置动态状态
    // ================================================================

    // 管线改为逐物体惰性绑定 —— 单面/双面剔除必须与前向 Pass 对齐

    FRHIViewport viewport = {};
    viewport.X        = 0.0f;
    viewport.Y        = 0.0f;
    viewport.Width    = static_cast<Float32>(context.SwapchainExtent.Width);
    viewport.Height   = static_cast<Float32>(context.SwapchainExtent.Height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    commandBuffer->SetViewport(viewport);

    FRHIScissorRect scissor = {};
    scissor.X      = 0;
    scissor.Y      = 0;
    scissor.Width  = context.SwapchainExtent.Width;
    scissor.Height = context.SwapchainExtent.Height;

    commandBuffer->SetScissor(scissor);

    // ================================================================
    // 绑定描述符集 (set 0 — ViewProj UBO，深度着色器中使用)
    // ================================================================

    commandBuffer->BindDescriptorSet(
        EPipelineBindPoint::Graphics,
        context.PipelineLayout,
        0,
        context.ViewProjDescriptorSet,
        nullptr,
        0
    );

    // set 1 = bindless 材质表 —— Masked 材质的 alpha 测试要读 albedo
    commandBuffer->BindDescriptorSet(
        EPipelineBindPoint::Graphics,
        context.PipelineLayout,
        1,
        context.BindlessDescriptorSet,
        nullptr,
        0);

    // ================================================================
    // 绑定描述符集 (set 3 — 逐物体数据)
    // ================================================================
    //
    // **两条路径都要绑。** 顶点着色器只有一条代码路径, 它总是从这里取模型
    // 矩阵与材质下标; 逐物体绘制那条路径只是把物体下标经 firstInstance 传
    // 进去。不绑的表现是读到未定义内容 —— 整个场景的变换全是垃圾。
    BindDrawObjectSet(commandBuffer, context);
}

void FDepthPrePass::RecordIndirect(IRHICommandBuffer*        commandBuffer,
                                   const FRenderPassContext& context)
{
    if (context.GpuCull == nullptr)
    {
        return;
    }

    RecordIndirectGroups(commandBuffer, context, *context.GpuCull,
                         SelectPipeline(false), SelectPipeline(true),
                         FGpuCullPass::kCameraView);
}

void FDepthPrePass::RecordRange(IRHICommandBuffer*        commandBuffer,
                                 const FRenderPassContext& context,
                                 SizeType                  begin,
                                 SizeType                  end)
{
    if (context.RenderObjects != nullptr)
    {
        // 材质集必须绑定 —— Masked 材质要在这里做与前向 Pass **完全相同**的
        // alpha 测试。不测的话, 深度预 Pass 会为完全透明的纹素写入深度,
        // 把它背后的东西挡掉; 而前向 Pass 又把这些纹素 discard 掉,
        // 结果是植被叶片之间出现挖空的黑洞。
        //
        // 两个 Pass 的裁剪结论必须逐纹素一致, 否则 DepthCompareOp=Equal
        // 的 Early-Z 会在边缘处失配。
        FRHIGraphicsPipelineHandle boundPipeline;
        FRHIDescriptorSetHandle boundMaterial;
        FRHIBufferHandle boundVertexBuffer;
        FRHIBufferHandle boundIndexBuffer;
        EIndexType       boundIndexType = EIndexType::UInt32;

        for (SizeType i = begin; i < end; ++i)
        {
            const FRenderObject& obj = (*context.RenderObjects)[i];

            const FRHIGraphicsPipelineHandle pipeline =
                SelectPipeline(obj.IsDoubleSided);

            if (pipeline.Packed != boundPipeline.Packed)
            {
                commandBuffer->BindGraphicsPipeline(pipeline);
                boundPipeline = pipeline;
            }

            // set 1 已在本段开头绑过一次 (bindless 全局表), 逐 draw 不再绑。
            //
            // 这正是 bindless 的意义: 材质切换从"绑一次描述符集"降级为
            // "push constant 里换一个整数"。GPU 驱动渲染更进一步 ——
            // 间接绘制根本没有逐 draw 绑描述符集这回事。

            if (obj.VertexBuffer.Packed != boundVertexBuffer.Packed)
            {
                commandBuffer->BindVertexBuffer(0, obj.VertexBuffer, 0);
                boundVertexBuffer = obj.VertexBuffer;
            }

            if (obj.IndexBuffer.Packed != boundIndexBuffer.Packed ||
                obj.IndexType != boundIndexType)
            {
                commandBuffer->BindIndexBuffer(obj.IndexBuffer, 0,
                                               obj.IndexType);
                boundIndexBuffer = obj.IndexBuffer;
                boundIndexType   = obj.IndexType;
            }

            // 模型矩阵与材质下标已经在 set 3 的 storage buffer 里了 ——
            // 这里传的是**物体下标**, 经 firstInstance 进到顶点着色器的
            // gl_InstanceIndex。
            //
            // 与间接路径完全同一个机制。所以两条路径读的是同一处数据、走的
            // 是同一份着色器代码, 逐像素比对时比出来的差异只可能来自剔除与
            // 命令下发。
            //
            // 没有逐物体条目的就不画 —— 见 FGpuCullPass 里那段说明。
            //
            // 相机段是缓冲区的**第一段**, 所以它被截断的门槛比后两段高得多
            // (要相机列表自己就超过 kMaxGpuDrawObjects)。但"门槛高"不等于
            // 走不到, 而阴影那三条路径都判了、这里不判的话, 这条路径就是
            // 整个不变式上唯一一个缺口 —— 而缺口在哪, 缺陷就出在哪。
            if (context.GpuCull != nullptr &&
                static_cast<UInt32>(i) >= context.GpuCull->GetCameraCount())
            {
                context.GpuCull->NoteSkippedDraw();
                continue;
            }

            if (context.GpuCull != nullptr)
            {
                context.GpuCull->NoteIssuedDraw(static_cast<UInt32>(i));
            }

            commandBuffer->DrawIndexed(
                obj.IndexCount,
                1,
                obj.IndexOffset,
                0,
                static_cast<UInt32>(i)
            );
        }
    }

}

void FDepthPrePass::Execute(IRHICommandBuffer*        commandBuffer,
                             const FRenderPassContext& context)
{
    commandBuffer->BeginDebugLabel("DepthPrePass", 1.0f, 0.8f, 0.2f);

    // ================================================================
    // 清除深度值 — 最大深度 1.0
    // ================================================================

    FRHIClearDepthStencilValue clearDepth = {};
    clearDepth.Depth   = 1.0f;
    clearDepth.Stencil = 0;

    // 法线清成哨兵值 (2,2) —— 八面体编码**产不出**的值。
    //
    // 八面体编码的值域恰好填满 [-1,1]^2, 所以任何分量绝对值大于 1 的取值
    // 都不可能来自真实几何。这让消费方能够区分"这里没有几何"和"这里有一
    // 个朝向恰好如此的面" —— 二者的正确处理方式完全不同 (天空不该参与
    // GTAO 的遮蔽计算, 而不是被当成一个朝 +Z 的面)。
    //
    // 清成 (0,0) 也能解出合法单位向量, 但那个值 **是** 合法编码 (对应
    // +Z), 于是"没有几何"与"朝向 +Z"永久无法区分。区分不了的后果不是崩,
    // 是天空边缘多出一圈说不清来源的 AO。
    //
    // 解码 (2,2) 得到 (2,2,-3) 归一化后仍是单位向量, 所以即使消费方忘了
    // 判哨兵, 退化行为也是"一个方向", 而不是 NaN。
    //
    // 速度清成 0 —— 天空不动。
    FRHIClearColorValue clearColors[2] = {};
    clearColors[0].R = kGBufferNormalSentinel;
    clearColors[0].G = kGBufferNormalSentinel;
    clearColors[1].R = 0.0f;
    clearColors[1].G = 0.0f;

    // ================================================================
    // 开始深度渲染通道 (仅深度附件，无颜色)
    // ================================================================

    FRHIRenderPassBeginInfo beginInfo = {};
    beginInfo.RenderPass        = m_DepthRenderPass;
    beginInfo.Framebuffer       = m_DepthFramebuffer;
    beginInfo.RenderAreaOffset  = { 0, 0 };
    beginInfo.RenderAreaExtent  = context.SwapchainExtent;
    beginInfo.ClearColors       = clearColors;
    beginInfo.ClearColorCount   = 2;
    beginInfo.ClearDepthStencil = &clearDepth;

    const bool gpuDriven =
        (context.GpuCull != nullptr) && context.GpuCull->IsEnabled();

    // 并行录制时通道内容必须来自次级缓冲区
    const bool useParallel =
        !gpuDriven && (m_Recorder != nullptr) && m_Recorder->IsInitialized();

    beginInfo.UseSecondaryCommandBuffers = useParallel;

    commandBuffer->BeginRenderPass(beginInfo);

    const SizeType objectCount =
        (context.RenderObjects != nullptr) ? context.RenderObjects->GetSize()
                                           : 0;

    if (gpuDriven)
    {
        // 间接路径不分段并行录制 —— 一共十几条命令, 分给四个线程之后每段
        // 只有三四条, 而次级缓冲区的分配与 vkCmdExecuteCommands 本身就要
        // 好几微秒。并行在这里是净亏。
        RecordCommonState(commandBuffer, context);
        RecordIndirect(commandBuffer, context);
    }
    else if (useParallel)
    {
        FRHICommandBufferInheritance inheritance;
        inheritance.RenderPass  = m_DepthRenderPass;
        inheritance.Subpass     = 0;
        inheritance.Framebuffer = m_DepthFramebuffer;

        const FRecorderBatch batch = m_Recorder->RecordSegmented(
            objectCount, inheritance,
            [this, &context](IRHICommandBuffer* segmentBuffer,
                             SizeType begin, SizeType end)
            {
                RecordCommonState(segmentBuffer, context);
                RecordRange(segmentBuffer, context, begin, end);
            });

        m_Recorder->ExecuteInto(commandBuffer, batch);
    }
    else
    {
        RecordCommonState(commandBuffer, context);
        RecordRange(commandBuffer, context, 0, objectCount);
    }

    commandBuffer->EndRenderPass();

    // ================================================================
    // 管线屏障 — 确保深度写入完成后 FForwardPass 才读取深度
    // srcStage: LateFragmentTests (深度写入发生在这里)
    // dstStage: EarlyFragmentTests (ForwardPass 读取深度在这里)
    // ================================================================

    FRHIImageMemoryBarrier depthBarrier = {};
    depthBarrier.SrcAccessMask  = EAccessFlags::DepthStencilAttachmentWrite;
    depthBarrier.DstAccessMask  = EAccessFlags::DepthStencilAttachmentRead;
    depthBarrier.OldLayout      = EImageLayout::DepthStencilAttachment;
    depthBarrier.NewLayout      = EImageLayout::DepthStencilAttachment;
    depthBarrier.Texture        = m_SharedDepthTexture;
    depthBarrier.BaseMipLevel   = 0;
    depthBarrier.MipLevelCount  = 1;
    depthBarrier.BaseArrayLayer = 0;
    depthBarrier.ArrayLayerCount = 1;

    commandBuffer->PipelineBarrier(
        EPipelineStageFlags::LateFragmentTests,
        EPipelineStageFlags::EarlyFragmentTests,
        nullptr, 0,
        nullptr, 0,
        &depthBarrier, 1
    );

    commandBuffer->EndDebugLabel();
}

// ============================================================================
// OnResize — 重建共享深度 Framebuffer
// ============================================================================

ERHIResult FDepthPrePass::OnResize(const FPassResizeDesc& desc)
{
    // 解出局部别名 —— 下面的函数体沿用原来的名字。
    //
    // 这样改动只落在签名上, 函数体一行不动, 便于确认这次重构确实没有
    // 改变行为。
    IRHIDevice* const           device               = desc.Device;
    const FRHISwapchainHandle   swapchain            = desc.Swapchain;
    const FRHIExtent2D          newExtent            = desc.Extent;
    const UInt32                swapchainImageCount  = desc.SwapchainImageCount;
    const FRHITextureHandle     newSharedDepth       = desc.SharedDepth;
    const FRHITextureViewHandle newSharedDepthView   = desc.SharedDepthView;
    const FRHITextureHandle     newSharedColor       = desc.SharedColor;
    const FRHITextureViewHandle newSharedColorView   = desc.SharedColorView;

    (void)device;
    (void)swapchain;
    (void)newExtent;
    (void)swapchainImageCount;
    (void)newSharedDepth;
    (void)newSharedDepthView;
    (void)newSharedColor;
    (void)newSharedColorView;

    m_SwapchainExtent    = newExtent;
    m_SharedDepthTexture = newSharedDepth;

    DestroyDepthFramebuffer(device);

    // G-Buffer 尺寸随交换链变化, 必须一起重建。
    //
    // 顺序: 先销毁旧的帧缓冲 (它引用着旧视图), 再销毁并重建 G-Buffer,
    // 最后建新帧缓冲。反过来会让帧缓冲短暂引用已销毁的视图。
    DestroyGBufferTargets(device);

    ERHIResult gbufferResult = CreateGBufferTargets(device, newExtent);

    if (!IsRHISuccess(gbufferResult))
    {
        return gbufferResult;
    }

    ERHIResult result = CreateDepthFramebuffer(device,
                                                newExtent,
                                                newSharedDepthView);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
                 "[DepthPrePass] Resize: Framebuffer 重建失败");
    }

    return result;
}

// ============================================================================
// ReleaseSwapchainResources — 释放尺寸相关资源
// ============================================================================

void FDepthPrePass::ReleaseSwapchainResources(IRHIDevice* device)
{
    if (device == nullptr)
    {
        return;
    }

    DestroyDepthFramebuffer(device);
}

// ============================================================================
// Shutdown — 释放所有 GPU 资源
// ============================================================================

void FDepthPrePass::Shutdown(IRHIDevice* device)
{
    if (device == nullptr)
    {
        return;
    }

    DestroyPipelineResources(device);
    DestroyDepthFramebuffer(device);
    DestroyGBufferTargets(device);
    device->DestroyRenderPass(m_DepthRenderPass);

    LIMX_LOG(LogRenderer, Log, "[DepthPrePass] 已关闭");
}

// ============================================================================
// CreateDepthRenderPass — 仅含深度附件的渲染通道
// ============================================================================

ERHIResult FDepthPrePass::CreateDepthRenderPass(IRHIDevice* device)
{
    // 三个附件, 顺序是 [0]=法线 [1]=速度 [2]=深度。
    //
    // **深度必须在最后。** BeginRenderPass 填清除值时先按顺序填全部颜色,
    // 再无条件把深度追加到末尾; 顺序不对清除值就整体错位, 而数量仍然对得
    // 上, 验证层不报错。FVulkanDevice::CreateRenderPass 里有一条硬检查会
    // 拒绝错误的顺序 —— 那条检查就是为这里加的。
    FRHIAttachmentDesc attachments[3] = {};

    // [0] 世界空间法线, 八面体编码进两个通道
    attachments[0].Format         = kGBufferNormalFormat;
    attachments[0].Samples        = ESampleCount::Count1;
    attachments[0].LoadOp         = ELoadOp::Clear;
    attachments[0].StoreOp        = EStoreOp::Store;
    attachments[0].StencilLoadOp  = ELoadOp::DontCare;
    attachments[0].StencilStoreOp = EStoreOp::DontCare;
    attachments[0].InitialLayout  = EImageLayout::Undefined;
    attachments[0].FinalLayout    = EImageLayout::ShaderReadOnly;

    // [1] 屏幕空间速度 (当前 NDC − 上一帧 NDC)
    attachments[1] = attachments[0];
    attachments[1].Format = kVelocityFormat;

    // [2] 深度
    attachments[2].Format         = EPixelFormat::D32_SFLOAT;
    attachments[2].Samples        = ESampleCount::Count1;
    attachments[2].LoadOp         = ELoadOp::Clear;
    attachments[2].StoreOp        = EStoreOp::Store;
    attachments[2].StencilLoadOp  = ELoadOp::DontCare;
    attachments[2].StencilStoreOp = EStoreOp::DontCare;
    attachments[2].InitialLayout  = EImageLayout::Undefined;
    attachments[2].FinalLayout    = EImageLayout::DepthStencilAttachment;

    FRHIAttachmentReference colorRefs[2] = {};
    colorRefs[0].AttachmentIndex = 0;
    colorRefs[0].Layout          = EImageLayout::ColorAttachment;
    colorRefs[1].AttachmentIndex = 1;
    colorRefs[1].Layout          = EImageLayout::ColorAttachment;

    FRHIAttachmentReference depthRef = {};
    depthRef.AttachmentIndex = 2;
    depthRef.Layout          = EImageLayout::DepthStencilAttachment;

    FRHISubpassDesc subpass = {};
    subpass.ColorAttachments       = colorRefs;
    subpass.ColorAttachmentCount   = 2;
    subpass.DepthStencilAttachment = &depthRef;

    // 子通道依赖: 外部 → 子通道 0。
    //
    // 现在既写深度也写颜色, 两个阶段都要等 —— 只等 EarlyFragmentTests
    // 会让颜色附件的布局转换与写入之间缺一道同步。
    FRHISubpassDependency dependency = {};
    dependency.SrcSubpass    = 0xFFFFFFFF;
    dependency.DstSubpass    = 0;
    dependency.SrcStageMask  = EPipelineStageFlags::TopOfPipe;
    dependency.DstStageMask  = EPipelineStageFlags::EarlyFragmentTests
                             | EPipelineStageFlags::ColorAttachmentOutput;
    dependency.SrcAccessMask = EAccessFlags::None;
    dependency.DstAccessMask = EAccessFlags::DepthStencilAttachmentWrite
                             | EAccessFlags::ColorAttachmentWrite;

    FRHIRenderPassDesc renderPassDesc = {};
    renderPassDesc.Attachments     = attachments;
    renderPassDesc.AttachmentCount = 3;
    renderPassDesc.Subpasses       = &subpass;
    renderPassDesc.SubpassCount    = 1;
    renderPassDesc.Dependencies    = &dependency;
    renderPassDesc.DependencyCount = 1;
    renderPassDesc.DebugName       = "DepthPrePass_RenderPass";

    return device->CreateRenderPass(renderPassDesc, m_DepthRenderPass);
}

// ============================================================================
// CreateDepthFramebuffer — 单一 depth-only Framebuffer
// ============================================================================

ERHIResult FDepthPrePass::CreateDepthFramebuffer(
    IRHIDevice*           device,
    FRHIExtent2D          extent,
    FRHITextureViewHandle sharedDepthView)
{
    // 视图顺序必须与渲染通道的附件顺序一致 —— [0]=法线 [1]=速度 [2]=深度
    const FRHITextureViewHandle views[3] =
    {
        m_NormalView,
        m_VelocityView,
        sharedDepthView,
    };

    FRHIFramebufferDesc fbDesc = {};
    fbDesc.RenderPass      = m_DepthRenderPass;
    fbDesc.Attachments     = views;
    fbDesc.AttachmentCount = 3;
    fbDesc.Width           = extent.Width;
    fbDesc.Height          = extent.Height;
    fbDesc.Layers          = 1;
    fbDesc.DebugName       = "DepthPrePass_Framebuffer";

    ERHIResult result = device->CreateFramebuffer(fbDesc, m_DepthFramebuffer);

    if (IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Log,
                 "[DepthPrePass] Framebuffer 创建完成 — {}x{} "
                 "(法线 + 速度 + 深度)",
                 extent.Width, extent.Height);
    }

    return result;
}

// ============================================================================
// DestroyDepthFramebuffer
// ============================================================================

void FDepthPrePass::DestroyDepthFramebuffer(IRHIDevice* device)
{
    if (m_DepthFramebuffer.IsValid())
    {
        device->DestroyFramebuffer(m_DepthFramebuffer);
    }
}

// ============================================================================
// CreateShaders — 加载 gbuffer.vert / gbuffer.frag
// ============================================================================

ERHIResult FDepthPrePass::CreateShaders(IRHIDevice* device)
{
    FShaderManager& shaderManager = FShaderManager::Get();
    if (!shaderManager.IsInitialized())
    {
        shaderManager.Initialize();
    }

    ERHIResult result = shaderManager.CreateShaderModule(
        device,
        FString("Builtin/gbuffer.vert"),
        EShaderStage::Vertex,
        m_VertShader);
    if (!IsRHISuccess(result))
    {
        return result;
    }

    result = shaderManager.CreateShaderModule(
        device,
        FString("Builtin/gbuffer.frag"),
        EShaderStage::Fragment,
        m_FragShader);

    return result;
}

// ============================================================================
// CreateDepthPipeline — depth-only 管线 (Less 深度测试, 深度写入, 无颜色混合)
// ============================================================================

ERHIResult FDepthPrePass::CreateDepthPipeline(
    IRHIDevice* device, bool isDoubleSided,
    FRHIGraphicsPipelineHandle& outPipeline)
{
    FRHIGraphicsPipelineDesc pipelineDesc = {};

    // ---- 着色器阶段 ----
    pipelineDesc.ShaderStages[0].Stage      = EShaderStage::Vertex;
    pipelineDesc.ShaderStages[0].Shader     = m_VertShader;
    pipelineDesc.ShaderStages[0].EntryPoint = "main";

    pipelineDesc.ShaderStages[1].Stage      = EShaderStage::Fragment;
    pipelineDesc.ShaderStages[1].Shader     = m_FragShader;
    pipelineDesc.ShaderStages[1].EntryPoint = "main";

    pipelineDesc.ShaderStageCount = 2;

    // ---- 顶点输入 — 与 FMeshVertex 完全兼容 (必须匹配 VBO 步幅) ----
    FRHIVertexInputBinding vertexBinding = {};
    vertexBinding.Binding   = 0;
    vertexBinding.Stride    = sizeof(FMeshVertex);
    vertexBinding.InputRate = EVertexInputRate::PerVertex;

    // 只声明本管线真正读取的属性 —— 声明了却不消费的属性会让校验层报
    // "not consumed by vertex shader"。步幅仍是完整的 FMeshVertex,
    // 属性按偏移量取值, 与缓冲区里其余字段的存在与否无关。
    //
    // 偏移量取自 FMeshVertex 成员而非手写常量: 结构变化时 offsetof 会跟着走,
    // 手写常量只会静默错位。72 字节的布局静态断言在 FAssetTypes.h 中。
    // 位置用于变换, UV 用于 Masked 材质的 alpha 测试。
    // Location 不必连续 —— 保持与 FMeshVertex 在前向管线中的编号一致,
    // 两条管线用同一套 location 号能避免"同一个属性在不同 Pass 里编号不同"
    // 这种极易看漏的错。
    FRHIVertexInputAttribute vertexAttributes[3] = {};

    vertexAttributes[0].Location = 0;
    vertexAttributes[0].Binding  = 0;
    vertexAttributes[0].Format   = EPixelFormat::RGB32_SFLOAT;
    vertexAttributes[0].Offset   = static_cast<UInt32>(
        LIMX_OFFSET_OF(FMeshVertex, Position));

    // location 1 = 法线 —— G-Buffer 要输出它。
    //
    // location 编号必须与前向管线一致 (pbr.vert 用的也是 0/1/2/3/5),
    // 因为两条管线共用同一份顶点数据布局。
    vertexAttributes[1].Location = 1;
    vertexAttributes[1].Binding  = 0;
    vertexAttributes[1].Format   = EPixelFormat::RGB32_SFLOAT;
    vertexAttributes[1].Offset   = static_cast<UInt32>(
        LIMX_OFFSET_OF(FMeshVertex, Normal));

    vertexAttributes[2].Location = 3;
    vertexAttributes[2].Binding  = 0;
    vertexAttributes[2].Format   = EPixelFormat::RG32_SFLOAT;
    vertexAttributes[2].Offset   = static_cast<UInt32>(
        LIMX_OFFSET_OF(FMeshVertex, TexCoord0));

    pipelineDesc.VertexInput.Bindings       = &vertexBinding;
    pipelineDesc.VertexInput.BindingCount   = 1;
    pipelineDesc.VertexInput.Attributes     = vertexAttributes;
    pipelineDesc.VertexInput.AttributeCount = 3;

    // ---- 输入装配 ----
    pipelineDesc.InputAssembly.Topology                  = EPrimitiveTopology::TriangleList;
    pipelineDesc.InputAssembly.IsPrimitiveRestartEnabled = false;

    // ---- 光栅化 ----
    pipelineDesc.Rasterization.PolygonMode                = EPolygonMode::Fill;
    // 必须与 FForwardPass 对同一物体的选择完全一致
    pipelineDesc.Rasterization.CullMode                   =
        isDoubleSided ? ECullMode::None : ECullMode::Back;
    pipelineDesc.Rasterization.FrontFace                  = EFrontFace::CounterClockwise;
    pipelineDesc.Rasterization.LineWidth                  = 1.0f;
    pipelineDesc.Rasterization.IsDepthClampEnabled        = false;
    pipelineDesc.Rasterization.IsRasterizerDiscardEnabled = false;
    pipelineDesc.Rasterization.IsDepthBiasEnabled         = false;

    // ---- 多重采样 ----
    pipelineDesc.Multisample.RasterizationSamples  = ESampleCount::Count1;
    pipelineDesc.Multisample.IsSampleShadingEnabled = false;

    // ---- 深度模板 — Less 测试, 深度写入开启 ----
    pipelineDesc.DepthStencil.IsDepthTestEnabled  = true;
    pipelineDesc.DepthStencil.IsDepthWriteEnabled = true;
    pipelineDesc.DepthStencil.DepthCompareOp      = ECompareOp::Less;

    // ---- 颜色混合 — 两张 G-Buffer 附件, 都不混合 ----
    //
    // AttachmentCount 必须**恰好等于**子通道的 ColorAttachmentCount, 否则
    // vkCreateGraphicsPipelines 直接失败 (这一条验证层会抓, 不是隐患)。
    //
    // 真正要小心的是生命周期: Attachments 存的是指针, 而这个数组是栈上的
    // 局部量 —— 它必须活到 CreateGraphicsPipeline 返回。放在函数作用域里
    // 声明就够, 但绝不能用临时对象。
    FRHIColorBlendAttachmentDesc blendAttachments[2];
    blendAttachments[0] = FRHIColorBlendAttachmentDesc::Opaque();
    blendAttachments[1] = FRHIColorBlendAttachmentDesc::Opaque();

    pipelineDesc.ColorBlend.Attachments      = blendAttachments;
    pipelineDesc.ColorBlend.AttachmentCount  = 2;
    pipelineDesc.ColorBlend.IsLogicOpEnabled = false;

    // ---- 动态状态 ----
    pipelineDesc.DynamicState.EnabledStates =
        EDynamicState::Viewport | EDynamicState::Scissor;

    // ---- 管线布局与渲染通道 ----
    pipelineDesc.PipelineLayout = m_PipelineLayout;
    pipelineDesc.RenderPass     = m_DepthRenderPass;
    pipelineDesc.SubpassIndex   = 0;
    pipelineDesc.DebugName      = "DepthPrePass_Pipeline";

    ERHIResult result =
        device->CreateGraphicsPipeline(pipelineDesc, outPipeline);

    if (IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Log,
                 "[DepthPrePass] depth-only 管线创建完成 (DepthCompareOp=Less, DepthWrite=true)");
    }

    return result;
}

// ============================================================================
// DestroyPipelineResources
// ============================================================================

void FDepthPrePass::DestroyPipelineResources(IRHIDevice* device)
{
    for (SizeType variant = 0; variant < kPipelineVariantCount; ++variant)
    {
        device->DestroyGraphicsPipeline(m_DepthPipelines[variant]);
    }
    device->DestroyShader(m_FragShader);
    device->DestroyShader(m_VertShader);
}

// ============================================================================
// G-Buffer 附件 — 法线与速度
// ============================================================================
//
// 归本 Pass 所有而不是放进 FPassManager 的共享资源: 只有深度预通道**写**
// 它们, 其它 Pass 只读。共享资源那一层管的是"多个 Pass 都要写"的东西
// (深度、HDR 颜色), 把只有一个写者的资源放进去只会让所有权变模糊。

ERHIResult FDepthPrePass::CreateGBufferTargets(IRHIDevice*  device,
                                                FRHIExtent2D extent)
{
    struct FTargetSpec
    {
        EPixelFormat           Format;
        FRHITextureHandle*     Texture;
        FRHITextureViewHandle* View;
        const AnsiChar*        DebugName;
    };

    const FTargetSpec specs[2] =
    {
        { kGBufferNormalFormat, &m_NormalTexture,   &m_NormalView,
          "GBufferNormal" },
        { kVelocityFormat,      &m_VelocityTexture, &m_VelocityView,
          "GBufferVelocity" },
    };

    for (UInt32 i = 0; i < 2; ++i)
    {
        const FTargetSpec& spec = specs[i];

        FRHITextureDesc texDesc = {};
        texDesc.Type        = ETextureType::Texture2D;
        texDesc.Format      = spec.Format;
        texDesc.Extent      = { extent.Width, extent.Height, 1 };
        texDesc.MipLevels   = 1;
        texDesc.ArrayLayers = 1;
        texDesc.Samples     = ESampleCount::Count1;
        // TransferSrc 是常开的, 不挂在某个调试开关下面。
        //
        // 只在自检模式下加这个标志的话, 自检验证的就是一份与实际发布配置
        // 不同的资源 —— 而两者的差异恰恰在同步与布局上, 那正是最容易出错
        // 且最难复现的部分。桌面 GPU 上多这一个标志没有实测代价。
        texDesc.Usage       = static_cast<ETextureUsage>(
            static_cast<UInt32>(ETextureUsage::ColorAttachment) |
            static_cast<UInt32>(ETextureUsage::Sampled) |
            static_cast<UInt32>(ETextureUsage::TransferSrc));
        texDesc.MemoryUsage = EMemoryUsage::GpuOnly;
        texDesc.DebugName   = spec.DebugName;

        ERHIResult result = device->CreateTexture(texDesc, *spec.Texture);

        if (!IsRHISuccess(result))
        {
            LIMX_LOG(LogRenderer, Error,
                     "[DepthPrePass] {} 创建失败 ({}x{})",
                     spec.DebugName, extent.Width, extent.Height);
            return result;
        }

        FRHITextureViewDesc viewDesc = {};
        viewDesc.Texture         = *spec.Texture;
        viewDesc.ViewType        = ETextureType::Texture2D;
        viewDesc.Format          = spec.Format;
        viewDesc.BaseMipLevel    = 0;
        viewDesc.MipLevelCount   = 1;
        viewDesc.BaseArrayLayer  = 0;
        viewDesc.ArrayLayerCount = 1;

        result = device->CreateTextureView(viewDesc, *spec.View);

        if (!IsRHISuccess(result))
        {
            device->DestroyTexture(*spec.Texture);
            return result;
        }
    }

    LIMX_LOG(LogRenderer, Log,
             "[DepthPrePass] G-Buffer 附件已创建 — {}x{} 法线 + 速度 "
             "(各 RG16_SFLOAT, 共 {} KiB)",
             extent.Width, extent.Height,
             (extent.Width * extent.Height * 4 * 2) / 1024);

    return ERHIResult::Success;
}

void FDepthPrePass::DestroyGBufferTargets(IRHIDevice* device)
{
    if (m_NormalView.IsValid())     { device->DestroyTextureView(m_NormalView); }
    if (m_NormalTexture.IsValid())  { device->DestroyTexture(m_NormalTexture); }
    if (m_VelocityView.IsValid())   { device->DestroyTextureView(m_VelocityView); }
    if (m_VelocityTexture.IsValid()) { device->DestroyTexture(m_VelocityTexture); }
}

} // namespace Limx
