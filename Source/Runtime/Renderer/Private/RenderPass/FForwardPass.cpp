// ============================================================
// 文件名称：FForwardPass.cpp
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：渐进式封装 — 将现有 FRenderer::RecordCommands() 的单体逻辑
//          迁移为可独立测试的 Pass 对象，配合 PBR 着色器实现
//          Cook-Torrance BRDF 多光源前向渲染。
// 功能描述：FForwardPass 完整实现 — 颜色+深度渲染通道，使用 pbr.vert/frag
//          着色器，配合 FDepthPrePass 的 Early-Z 优化：DepthCompareOp=Equal，
//          禁止深度写入，确保只渲染可见片段。
//          描述符集绑定: set 0 (ViewProj+纹理), set 1 (材质), set 2 (光照)。
// 技术特性：每图像一个 Framebuffer (颜色=交换链视图, 深度=共享深度视图);
//          depth attachment LoadOp=Load (使用 FDepthPrePass 写入的深度值);
//          depth 初始布局=DepthStencilAttachment (prepass 写完后的布局);
//          管线使用 Equal 深度测试, 不写入深度 (避免干扰精确深度值)。
//
// ── 函数表 ──────────────────────────────────────────────────
// │ 函数名                        │ 描述                           │
// │──────────────────────────────│───────────────────────────────│
// │ Setup()                      │ 创建 RenderPass/FB/Shader/管线  │
// │ Execute()                    │ 录制前向渲染完整命令序列          │
// │ OnResize()                   │ 重建 Framebuffer               │
// │ Shutdown()                   │ 释放所有 GPU 资源               │
// │ CreateRenderPass()           │ 创建颜色+深度渲染通道            │
// │ CreateFramebuffers()         │ 为每个交换链图像创建帧缓冲        │
// │ DestroyFramebuffers()        │ 销毁所有帧缓冲                  │
// │ CreateShaders()              │ 加载 pbr.vert/frag              │
// │ CreateGraphicsPipeline()     │ 创建 Equal 深度测试管线          │
// │ DestroyPipelineResources()   │ 销毁管线和着色器                │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M0.5 Pass 抽象层)     │
// │ 2026-04-07  │ LimxTeam  │ M0.5 集成: PBR 着色器+多描述符集  │
// ============================================================

#include "Renderer/RenderPass/FForwardPass.h"
#include "Renderer/RenderPass/FPassManager.h"
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

ERHIResult FForwardPass::Setup(const FPassSetupDesc& desc)
{
    LIMX_CHECK(desc.Device != nullptr);

    m_PipelineLayout  = desc.PipelineLayout;
    m_SwapchainFormat = desc.SwapchainFormat;
    m_SwapchainExtent = desc.SwapchainExtent;
    m_ColorTargetView = desc.SharedColorTextureView;

    // 1. 创建颜色+深度渲染通道
    ERHIResult result = CreateRenderPass(desc.Device, desc.SwapchainFormat);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
                 "[ForwardPass] RenderPass 创建失败");
        return result;
    }

    // 2. 为每个交换链图像创建 Framebuffer
    result = CreateFramebuffers(desc.Device,
                                 desc.Swapchain,
                                 desc.SwapchainExtent,
                                 desc.SwapchainImageCount,
                                 desc.SharedDepthTextureView);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
                 "[ForwardPass] Framebuffer 创建失败");
        return result;
    }

    // 3. 加载着色器模块
    result = CreateShaders(desc.Device);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
                 "[ForwardPass] 着色器模块创建失败");
        return result;
    }

    // 4. 创建图形管线
    for (SizeType variant = 0;
         variant < kPipelineVariantCount && IsRHISuccess(result); ++variant)
    {
        const bool isTranslucent = (variant & 2u) != 0u;
        const bool isDoubleSided = (variant & 1u) != 0u;

        result = CreateGraphicsPipeline(desc.Device, isTranslucent,
                                        isDoubleSided, m_Pipelines[variant]);
    }
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
                 "[ForwardPass] 图形管线创建失败");
        return result;
    }

    LIMX_LOG(LogRenderer, Log,
             "[ForwardPass] 初始化完成 — {}x{}",
             desc.SwapchainExtent.Width, desc.SwapchainExtent.Height);

    return ERHIResult::Success;
}

// ============================================================================
// Execute — 录制前向渲染命令 (等价于 FRenderer::RecordCommands)
// ============================================================================

// ============================================================================
// 录制辅助 — 内联路径与并行路径共用
//
// 三段各自独立, 是因为并行录制要求每个次级命令缓冲区都自成一体: 次级
// 缓冲区不继承主缓冲区的任何绑定状态, 视口、描述符集必须每段各设一次。
//
// 共用同一份代码而不是抄两遍 —— 抄两遍的话, 逐像素比对失败时分不清是
// 并行本身的问题还是某处漏抄了一行。
// ============================================================================

void FForwardPass::RecordCommonState(IRHICommandBuffer*        commandBuffer,
                                      const FRenderPassContext& context)
{

    // ================================================================
    // 设置动态 Viewport/Scissor
    //
    // 管线改为逐物体惰性绑定 —— 单面/双面是两条不同的管线, 在这里固定绑
    // 一条会让另一类物体用错剔除模式。动态状态在管线切换后依然保持,
    // 因此 Viewport/Scissor 仍然只设一次。
    // ================================================================

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
    // 绑定描述符集 (set 0 — ViewProj UBO + 纹理)
    // ================================================================

    commandBuffer->BindDescriptorSet(
        EPipelineBindPoint::Graphics,
        context.PipelineLayout,
        0,
        context.ViewProjDescriptorSet,
        nullptr,
        0
    );

    // ================================================================
    // 绑定描述符集 (set 2 — 光照 UBO, 全物体共享)
    // ================================================================

    commandBuffer->BindDescriptorSet(
        EPipelineBindPoint::Graphics,
        context.PipelineLayout,
        2,
        context.LightingDescriptorSet,
        nullptr,
        0
    );

    // ================================================================
    // 绑定描述符集 (set 1 — bindless 材质表, 全场景共享)
    // ================================================================
    //
    // 每段只绑这一次。逐 draw 的材质切换靠 push constant 里的下标,
    // 不再有描述符集绑定。
    commandBuffer->BindDescriptorSet(
        EPipelineBindPoint::Graphics,
        context.PipelineLayout,
        1,
        context.BindlessDescriptorSet,
        nullptr,
        0
    );

    // ================================================================
    // 绑定描述符集 (set 3 — 逐物体数据)
    // ================================================================
    //
    // **两条路径都要绑。** 顶点着色器只有一条代码路径, 它总是从这里取模型
    // 矩阵与材质下标; 逐物体绘制那条路径只是把物体下标经 firstInstance 传
    // 进去。不绑的表现是读到未定义内容 —— 整个场景的变换全是垃圾。
    BindDrawObjectSet(commandBuffer, context);
}

void FForwardPass::RecordIndirect(IRHICommandBuffer*        commandBuffer,
                                  const FRenderPassContext& context)
{
    if (context.GpuCull == nullptr)
    {
        return;
    }

    RecordIndirectGroups(commandBuffer, context, *context.GpuCull,
                         SelectPipeline(false, false),
                         SelectPipeline(false, true),
                         FGpuCullPass::kCameraView);
}

void FForwardPass::RecordOpaqueRange(IRHICommandBuffer*        commandBuffer,
                                      const FRenderPassContext& context,
                                      SizeType                  begin,
                                      SizeType                  end)
{
    if (context.RenderObjects != nullptr)
    {
        // 记录上一次绑定的状态 —— 批次列表已按材质/网格排序, 相邻批次
        // 大多共享同一套绑定, 逐个重绑等于把排序的收益原地丢掉。
        // 用无效句柄作为初值, 保证第一个批次一定会真正绑定一次。
        FRHIGraphicsPipelineHandle boundPipeline;
        FRHIDescriptorSetHandle boundMaterial;
        FRHIBufferHandle        boundVertexBuffer;
        FRHIBufferHandle        boundIndexBuffer;
        EIndexType              boundIndexType = EIndexType::UInt32;

        for (SizeType i = begin; i < end; ++i)
        {
            const FRenderObject& obj = (*context.RenderObjects)[i];

            // 管线按剔除模式选择 —— 列表已按 IsDoubleSided 聚类,
            // 因此这里最多切换一次。
            const FRHIGraphicsPipelineHandle pipeline =
                SelectPipeline(false, obj.IsDoubleSided);

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

            // 索引宽度由网格顶点数决定 —— 写死 UInt16 会让顶点数超过
            // 65535 的网格读出错位的索引, 表现为随机穿插的三角形。
            // 同一缓冲区换了宽度也必须重绑, 因此宽度参与比较。
            if (obj.IndexBuffer.Packed != boundIndexBuffer.Packed ||
                obj.IndexType != boundIndexType)
            {
                commandBuffer->BindIndexBuffer(obj.IndexBuffer, 0,
                                               obj.IndexType);
                boundIndexBuffer = obj.IndexBuffer;
                boundIndexType   = obj.IndexType;
            }

            // 模型矩阵与材质下标在 set 3 的 storage buffer 里 —— 这里传的
            // 是**物体下标**, 经 firstInstance 进到 gl_InstanceIndex。
            // 与间接路径完全同一个机制。
            commandBuffer->DrawIndexed(
                obj.IndexCount,
                1,
                obj.IndexOffset,
                0,
                static_cast<UInt32>(i)
            );
        }
    }

    // ================================================================
    // 半透明批次 — 同一渲染通道内, 换管线继续绘制
    // ================================================================
    //
    // 放在同一个 RenderPass 里而非另起一个: 半透明要读已经画好的不透明像素
    // 做混合, 另起通道意味着颜色附件要 Store 再 Load 一遍, 在移动端 tile
    // 架构上这是实打实的带宽开销, 在桌面端也白白多两次布局转换。
    //
    // 列表已由 FSceneManager 按到相机的距离由远及近排序 —— 这里只负责按序
    // 提交, 不再做任何重排。
}

void FForwardPass::RecordTranslucent(IRHICommandBuffer*        commandBuffer,
                                      const FRenderPassContext& context)
{
    if (context.TranslucentObjects != nullptr &&
        context.TranslucentObjects->GetSize() > 0)
    {
        // 绑定状态跟踪重新开始 —— 换到半透明是个天然边界, 沿用上一段的
        // 跟踪值只能省下极少几次绑定, 却让"这里到底绑没绑过"变得难以推理。
        FRHIGraphicsPipelineHandle boundPipeline;
        FRHIDescriptorSetHandle boundMaterial;
        FRHIBufferHandle        boundVertexBuffer;
        FRHIBufferHandle        boundIndexBuffer;
        EIndexType              boundIndexType = EIndexType::UInt32;

        for (SizeType i = 0; i < context.TranslucentObjects->GetSize(); ++i)
        {
            const FRenderObject& obj = (*context.TranslucentObjects)[i];

            // 半透明按距离排序, 双面与单面会交替出现, 管线切换次数无法
            // 像不透明那样压到一次 —— 正确的混合顺序优先于状态聚类。
            const FRHIGraphicsPipelineHandle pipeline =
                SelectPipeline(true, obj.IsDoubleSided);

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

            // 半透明的条目接在不透明后面 —— 下标要加上那个基址。
            //
            // 忘了加的话玻璃会读到某个不透明物体的变换, 长在别人的位置上。
            // 而"某块玻璃跑到了奇怪的地方"看着像场景数据错了。
            const UInt32 base =
                (context.GpuCull != nullptr)
                    ? context.GpuCull->GetTranslucentBase() : 0u;

            commandBuffer->DrawIndexed(obj.IndexCount, 1, obj.IndexOffset,
                                       0, base + static_cast<UInt32>(i));
        }
    }

}

void FForwardPass::Execute(IRHICommandBuffer*        commandBuffer,
                            const FRenderPassContext& context)
{
    commandBuffer->BeginDebugLabel("ForwardPass", 0.2f, 0.8f, 1.0f);

    // ================================================================
    // 清除值 — 颜色 (深蓝灰 Limx 品牌色) + 深度 (最大值 1.0)
    // ================================================================

    // 颜色与深度都由更早的 Pass 清过 (天空 Pass 清颜色, 深度预通道清深度),
    // 本 Pass 两个附件都是 LoadOp=Load, 因此不需要任何清除值。
    FRHIClearDepthStencilValue clearDepth = {};
    clearDepth.Depth   = 1.0f;
    clearDepth.Stencil = 0;

    // ================================================================
    // 开始渲染通道
    // ================================================================

    FRHIRenderPassBeginInfo beginInfo = {};
    beginInfo.RenderPass        = m_RenderPass;
    // HDR 目标只有一张, 因此只有一个 Framebuffer —— 与交换链图像下标无关
    beginInfo.Framebuffer       = m_Framebuffers[0];
    beginInfo.RenderAreaOffset  = { 0, 0 };
    beginInfo.RenderAreaExtent  = context.SwapchainExtent;
    beginInfo.ClearColors       = nullptr;
    beginInfo.ClearColorCount   = 0;
    beginInfo.ClearDepthStencil = &clearDepth;

    // 并行录制时通道内容必须来自次级命令缓冲区。
    //
    // 这不是可选优化: 声明为 INLINE 的通道里调用 vkCmdExecuteCommands 是
    // 非法的, 反过来声明为 SECONDARY 的通道里直接录绘制命令也是非法的。
    // 因此一旦走并行路径, 视口与描述符集也必须进次级缓冲区。
    const bool gpuDriven =
        (context.GpuCull != nullptr) && context.GpuCull->IsEnabled();

    const bool useParallel =
        !gpuDriven && (m_Recorder != nullptr) && m_Recorder->IsInitialized();

    beginInfo.UseSecondaryCommandBuffers = useParallel;

    commandBuffer->BeginRenderPass(beginInfo);

    const SizeType opaqueCount =
        (context.RenderObjects != nullptr)
            ? context.RenderObjects->GetSize()
            : 0;

    if (gpuDriven)
    {
        // 间接路径不分段并行录制 —— 一共十几条命令, 分给四个线程之后每段
        // 只剩三四条, 而次级缓冲区的分配与 vkCmdExecuteCommands 本身就要好
        // 几微秒。并行在这里是净亏。
        //
        // 半透明照旧逐物体绘制: 它必须严格由远及近, 而间接命令的顺序表达
        // 不了"按距离"这件事。
        RecordCommonState(commandBuffer, context);
        RecordIndirect(commandBuffer, context);
        RecordTranslucent(commandBuffer, context);
    }
    else if (useParallel)
    {
        FRHICommandBufferInheritance inheritance;
        inheritance.RenderPass  = m_RenderPass;
        inheritance.Subpass     = 0;
        inheritance.Framebuffer = m_Framebuffers[0];

        FRecorderBatch batch = m_Recorder->RecordSegmented(
            opaqueCount, inheritance,
            [this, &context](IRHICommandBuffer* segmentBuffer,
                             SizeType begin, SizeType end)
            {
                RecordCommonState(segmentBuffer, context);
                RecordOpaqueRange(segmentBuffer, context, begin, end);
            });

        // 半透明走串行尾段 —— 它按到相机的距离由远及近绘制, 不切段。
        if (context.TranslucentObjects != nullptr &&
            context.TranslucentObjects->GetSize() > 0)
        {
            m_Recorder->RecordTail(
                inheritance,
                [this, &context](IRHICommandBuffer* tailBuffer)
                {
                    RecordCommonState(tailBuffer, context);
                    RecordTranslucent(tailBuffer, context);
                },
                batch);
        }

        m_Recorder->ExecuteInto(commandBuffer, batch);
    }
    else
    {
        RecordCommonState(commandBuffer, context);
        RecordOpaqueRange(commandBuffer, context, 0, opaqueCount);
        RecordTranslucent(commandBuffer, context);
    }

    commandBuffer->EndRenderPass();
    commandBuffer->EndDebugLabel();
}

// ============================================================================
// OnResize — 重建 Framebuffer
// ============================================================================

ERHIResult FForwardPass::OnResize(const FPassResizeDesc& desc)
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

    m_SwapchainExtent = newExtent;

    // HDR 目标随交换链一同重建, 视图句柄因此换了新的 —— 不更新的话
    // Framebuffer 会挂在已销毁的旧视图上。
    (void)newSharedColor;
    m_ColorTargetView = newSharedColorView;

    DestroyFramebuffers(device);

    ERHIResult result = CreateFramebuffers(device,
                                            swapchain,
                                            newExtent,
                                            swapchainImageCount,
                                            newSharedDepthView);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Error,
                 "[ForwardPass] Resize: Framebuffer 重建失败");
    }

    return result;
}

// ============================================================================
// ReleaseSwapchainResources — 释放尺寸相关资源
// ============================================================================

void FForwardPass::ReleaseSwapchainResources(IRHIDevice* device)
{
    if (device == nullptr)
    {
        return;
    }

    DestroyFramebuffers(device);
}

// ============================================================================
// Shutdown — 释放所有 GPU 资源
// ============================================================================

void FForwardPass::Shutdown(IRHIDevice* device)
{
    if (device == nullptr)
    {
        return;
    }

    DestroyPipelineResources(device);
    DestroyFramebuffers(device);
    device->DestroyRenderPass(m_RenderPass);

    LIMX_LOG(LogRenderer, Log, "[ForwardPass] 已关闭");
}

// ============================================================================
// CreateRenderPass — 颜色附件 + 深度附件 (LoadOp=Load for depth)
// ============================================================================

ERHIResult FForwardPass::CreateRenderPass(IRHIDevice*  device,
                                           EPixelFormat swapchainFormat)
{
    // 附件 0: HDR 颜色附件 — RGBA16_SFLOAT, 清除→存储
    //
    // 不再直接画进交换链: 光照结果的动态范围远超 [0,1], 写进 8 位归一化的
    // 交换链意味着在色调映射之前就把亮部截断了 —— 那样再好的映射曲线也
    // 无从发挥。最终布局转为着色器只读, 供后处理 Pass 采样。
    //
    // swapchainFormat 因此不再参与本 Pass 的附件格式。
    (void)swapchainFormat;

    FRHIAttachmentDesc attachments[2] = {};

    // LoadOp=Load 而非 Clear: 天空 Pass (Order 150) 已经清过屏并画好了
    // 天空。这里再清一次会把天空整片抹掉。
    //
    // 相应地 InitialLayout 必须是 ColorAttachment —— 天空 Pass 把它停在
    // 这个布局上, 声明成 Undefined 等于告诉驱动"内容可以丢弃"。
    attachments[0].Format         = kSharedColorFormat;
    attachments[0].Samples        = ESampleCount::Count1;
    attachments[0].LoadOp         = ELoadOp::Load;
    attachments[0].StoreOp        = EStoreOp::Store;
    attachments[0].StencilLoadOp  = ELoadOp::DontCare;
    attachments[0].StencilStoreOp = EStoreOp::DontCare;
    attachments[0].InitialLayout  = EImageLayout::ColorAttachment;
    attachments[0].FinalLayout    = EImageLayout::ShaderReadOnly;

    // 附件 1: 深度附件 — 使用 FDepthPrePass 写入的深度数据 (LoadOp=Load)
    // InitialLayout=DepthStencilAttachment (prepass 后深度缓冲的布局)
    // FinalLayout=DepthStencilAttachment (保持)
    attachments[1].Format         = kSharedDepthFormat;
    attachments[1].Samples        = ESampleCount::Count1;
    attachments[1].LoadOp         = ELoadOp::Load;
    attachments[1].StoreOp        = EStoreOp::DontCare;
    attachments[1].StencilLoadOp  = ELoadOp::DontCare;
    attachments[1].StencilStoreOp = EStoreOp::DontCare;
    attachments[1].InitialLayout  = EImageLayout::DepthStencilAttachment;
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

    // 子通道依赖 — 确保 FDepthPrePass 的深度写入对本 Pass 可见
    FRHISubpassDependency dependency = {};
    dependency.SrcSubpass    = 0xFFFFFFFF;
    dependency.DstSubpass    = 0;
    dependency.SrcStageMask  = EPipelineStageFlags::EarlyFragmentTests
                             | EPipelineStageFlags::LateFragmentTests;
    dependency.DstStageMask  = EPipelineStageFlags::EarlyFragmentTests
                             | EPipelineStageFlags::ColorAttachmentOutput;
    dependency.SrcAccessMask = EAccessFlags::DepthStencilAttachmentWrite;
    dependency.DstAccessMask = EAccessFlags::DepthStencilAttachmentRead
                             | EAccessFlags::ColorAttachmentWrite;

    FRHIRenderPassDesc renderPassDesc = {};
    renderPassDesc.Attachments     = attachments;
    renderPassDesc.AttachmentCount = 2;
    renderPassDesc.Subpasses       = &subpass;
    renderPassDesc.SubpassCount    = 1;
    renderPassDesc.Dependencies    = &dependency;
    renderPassDesc.DependencyCount = 1;
    renderPassDesc.DebugName       = "ForwardPass_RenderPass";

    return device->CreateRenderPass(renderPassDesc, m_RenderPass);
}

// ============================================================================
// CreateFramebuffers — 每图像一个 Framebuffer [颜色, 共享深度]
// ============================================================================

ERHIResult FForwardPass::CreateFramebuffers(IRHIDevice*           device,
                                              FRHISwapchainHandle   swapchain,
                                              FRHIExtent2D          extent,
                                              UInt32                imageCount,
                                              FRHITextureViewHandle sharedDepthView)
{
    // 只需一个 Framebuffer —— 渲染目标是那一张共享 HDR 纹理, 与交换链
    // 的多缓冲无关。此前每个交换链图像各建一个, 是因为直接画进交换链。
    //
    // 这也意味着相邻帧会写同一张 HDR 纹理: 由帧栅栏保证上一帧已呈现完毕,
    // 与深度缓冲区一直以来的做法相同。
    (void)swapchain;
    (void)imageCount;

    m_Framebuffers.Reserve(1);

    {
        FRHITextureViewHandle fbAttachments[2] =
        {
            m_ColorTargetView,
            sharedDepthView
        };

        FRHIFramebufferDesc fbDesc = {};
        fbDesc.RenderPass      = m_RenderPass;
        fbDesc.Attachments     = fbAttachments;
        fbDesc.AttachmentCount = 2;
        fbDesc.Width           = extent.Width;
        fbDesc.Height          = extent.Height;
        fbDesc.Layers          = 1;
        fbDesc.DebugName       = "ForwardPass_HDRFramebuffer";

        FRHIFramebufferHandle framebuffer;
        ERHIResult result = device->CreateFramebuffer(fbDesc, framebuffer);
        if (!IsRHISuccess(result))
        {
            return result;
        }

        m_Framebuffers.Add(framebuffer);
    }

    LIMX_LOG(LogRenderer, Log,
             "[ForwardPass] Framebuffer 创建完成 — {} 个, {}x{}",
             imageCount, extent.Width, extent.Height);

    return ERHIResult::Success;
}

// ============================================================================
// DestroyFramebuffers
// ============================================================================

void FForwardPass::DestroyFramebuffers(IRHIDevice* device)
{
    for (SizeType i = 0; i < m_Framebuffers.GetSize(); ++i)
    {
        if (m_Framebuffers[i].IsValid())
        {
            device->DestroyFramebuffer(m_Framebuffers[i]);
        }
    }
    m_Framebuffers.Clear();
}

// ============================================================================
// CreateShaders — 加载 triangle.vert / triangle.frag
// ============================================================================

ERHIResult FForwardPass::CreateShaders(IRHIDevice* device)
{
    FShaderManager& shaderManager = FShaderManager::Get();
    if (!shaderManager.IsInitialized())
    {
        shaderManager.Initialize();
    }

    ERHIResult result = shaderManager.CreateShaderModule(
        device,
        FString("Builtin/pbr.vert"),
        EShaderStage::Vertex,
        m_VertShader);
    if (!IsRHISuccess(result))
    {
        return result;
    }

    result = shaderManager.CreateShaderModule(
        device,
        FString("Builtin/pbr.frag"),
        EShaderStage::Fragment,
        m_FragShader);

    return result;
}

// ============================================================================
// CreateGraphicsPipeline — Equal 深度测试, 禁止深度写入
// ============================================================================

ERHIResult FForwardPass::CreateGraphicsPipeline(
    IRHIDevice* device, bool isTranslucent, bool isDoubleSided,
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

    // ---- 顶点输入 — FMeshVertex (44 bytes 交错布局) ----
    FRHIVertexInputBinding vertexBinding = {};
    vertexBinding.Binding   = 0;
    vertexBinding.Stride    = sizeof(FMeshVertex);
    vertexBinding.InputRate = EVertexInputRate::PerVertex;

    // 只声明本管线真正读取的属性 —— 声明了却不消费的属性会让校验层报
    // "not consumed by vertex shader"。TexCoord1 目前无人使用 (光照贴图
    // 尚未实现), 因此不出现在这里; 它仍占据顶点结构中的 8 字节, 步幅不变。
    //
    // 偏移量取自 FMeshVertex 成员而非手写常量: 结构变化时 offsetof 会跟着走,
    // 手写常量只会静默错位。72 字节的布局静态断言在 FAssetTypes.h 中。
    FRHIVertexInputAttribute vertexAttributes[5] = {};

    vertexAttributes[0].Location = 0;
    vertexAttributes[0].Binding  = 0;
    vertexAttributes[0].Format   = EPixelFormat::RGB32_SFLOAT;
    vertexAttributes[0].Offset   = static_cast<UInt32>(
        LIMX_OFFSET_OF(FMeshVertex, Position));

    vertexAttributes[1].Location = 1;
    vertexAttributes[1].Binding  = 0;
    vertexAttributes[1].Format   = EPixelFormat::RGB32_SFLOAT;
    vertexAttributes[1].Offset   = static_cast<UInt32>(
        LIMX_OFFSET_OF(FMeshVertex, Normal));

    vertexAttributes[2].Location = 2;
    vertexAttributes[2].Binding  = 0;
    vertexAttributes[2].Format   = EPixelFormat::RGBA32_SFLOAT;
    vertexAttributes[2].Offset   = static_cast<UInt32>(
        LIMX_OFFSET_OF(FMeshVertex, Tangent));

    vertexAttributes[3].Location = 3;
    vertexAttributes[3].Binding  = 0;
    vertexAttributes[3].Format   = EPixelFormat::RG32_SFLOAT;
    vertexAttributes[3].Offset   = static_cast<UInt32>(
        LIMX_OFFSET_OF(FMeshVertex, TexCoord0));

    vertexAttributes[4].Location = 5;
    vertexAttributes[4].Binding  = 0;
    vertexAttributes[4].Format   = EPixelFormat::RGBA32_SFLOAT;
    vertexAttributes[4].Offset   = static_cast<UInt32>(
        LIMX_OFFSET_OF(FMeshVertex, Color));

    pipelineDesc.VertexInput.Bindings       = &vertexBinding;
    pipelineDesc.VertexInput.BindingCount   = 1;
    pipelineDesc.VertexInput.Attributes     = vertexAttributes;
    pipelineDesc.VertexInput.AttributeCount = 5;

    // ---- 输入装配 ----
    pipelineDesc.InputAssembly.Topology                  = EPrimitiveTopology::TriangleList;
    pipelineDesc.InputAssembly.IsPrimitiveRestartEnabled = false;

    // ---- 光栅化 ----
    pipelineDesc.Rasterization.PolygonMode              = EPolygonMode::Fill;
    // 双面材质关闭剔除 —— 植被与薄片几何只有一层三角形, 剔掉背面等于
    // 让每片叶子只剩朝向相机的那半边。
    pipelineDesc.Rasterization.CullMode                 =
        isDoubleSided ? ECullMode::None : ECullMode::Back;
    pipelineDesc.Rasterization.FrontFace                = EFrontFace::CounterClockwise;
    pipelineDesc.Rasterization.LineWidth                = 1.0f;
    pipelineDesc.Rasterization.IsDepthClampEnabled      = false;
    pipelineDesc.Rasterization.IsRasterizerDiscardEnabled = false;
    pipelineDesc.Rasterization.IsDepthBiasEnabled       = false;

    // ---- 多重采样 ----
    pipelineDesc.Multisample.RasterizationSamples  = ESampleCount::Count1;
    pipelineDesc.Multisample.IsSampleShadingEnabled = false;

    // ---- 深度模板 ----
    //
    // 不透明: Equal —— 深度已由 FDepthPrePass 精确写入, 相等即为可见表面,
    //         这正是 Early-Z 生效的方式。
    // 半透明: LessOrEqual —— 半透明批次**不参与**深度预 Pass (它们不该写深度,
    //         否则会挡住自己身后的同类), 因此深度缓冲区里没有它们的值,
    //         用 Equal 会把它们全部剔除, 表现为半透明物体彻底消失。
    pipelineDesc.DepthStencil.IsDepthTestEnabled  = true;
    pipelineDesc.DepthStencil.IsDepthWriteEnabled = false;
    pipelineDesc.DepthStencil.DepthCompareOp      =
        isTranslucent ? ECompareOp::LessOrEqual : ECompareOp::Equal;

    // ---- 颜色混合 ----
    FRHIColorBlendAttachmentDesc colorBlendAttachment =
        isTranslucent ? FRHIColorBlendAttachmentDesc::AlphaBlend()
                      : FRHIColorBlendAttachmentDesc::Opaque();

    pipelineDesc.ColorBlend.Attachments      = &colorBlendAttachment;
    pipelineDesc.ColorBlend.AttachmentCount  = 1;
    pipelineDesc.ColorBlend.IsLogicOpEnabled = false;

    // ---- 动态状态 ----
    pipelineDesc.DynamicState.EnabledStates =
        EDynamicState::Viewport | EDynamicState::Scissor;

    // ---- 管线布局与渲染通道 ----
    pipelineDesc.PipelineLayout = m_PipelineLayout;
    pipelineDesc.RenderPass     = m_RenderPass;
    pipelineDesc.SubpassIndex   = 0;
    pipelineDesc.DebugName =
        isTranslucent ? (isDoubleSided ? "ForwardPass_Translucent_TwoSided"
                                       : "ForwardPass_Translucent")
                      : (isDoubleSided ? "ForwardPass_Opaque_TwoSided"
                                       : "ForwardPass_Opaque");

    ERHIResult result =
        device->CreateGraphicsPipeline(pipelineDesc, outPipeline);

    if (IsRHISuccess(result))
    {
        LIMX_LOG(LogRenderer, Log,
                 "[ForwardPass] {}{} 管线创建完成 (DepthCompareOp={}, 混合={})",
                 isTranslucent ? "半透明" : "不透明",
                 isDoubleSided ? "/双面" : "/单面",
                 isTranslucent ? "LessOrEqual" : "Equal",
                 isTranslucent ? "AlphaBlend" : "关");
    }

    return result;
}

// ============================================================================
// DestroyPipelineResources
// ============================================================================

void FForwardPass::DestroyPipelineResources(IRHIDevice* device)
{
    for (SizeType variant = 0; variant < kPipelineVariantCount; ++variant)
    {
        device->DestroyGraphicsPipeline(m_Pipelines[variant]);
    }
    device->DestroyShader(m_FragShader);
    device->DestroyShader(m_VertShader);
}

} // namespace Limx
