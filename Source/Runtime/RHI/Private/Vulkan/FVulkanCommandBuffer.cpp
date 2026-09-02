// ============================================================
// 文件名称：FVulkanCommandBuffer.cpp
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：所有命令录制方法直接映射到 vkCmd* 调用，句柄通过
//          FVulkanDevice 访问器转换为原生 Vulkan 对象，零额外开销。
// 功能描述：FVulkanCommandBuffer 完整实现 — 命令缓冲区生命周期管理、
//          渲染通道控制、管线/资源绑定、绘制/计算调用、资源拷贝、
//          管线屏障、动态状态设置、查询操作、调试标记。
// 技术特性：所有方法为 thin wrapper，直接转发到 Vulkan API；
//          管线屏障支持内存/缓冲区/图像三种屏障类型的批量提交；
//          调试标记通过 VK_EXT_debug_utils 扩展实现。
//
// 数量约定：本文件内的 kBarriersPerBatch / kMaxBatch / kInlineClearValues
//          都是**批量大小或内联容量**，不是调用方能提交的数量上限。
//          超出部分一律拆批下发或退到分配器，任何情况下都不丢弃元素 ——
//          静默截断的后果是同步缺失或附件未清除，两者都不会崩溃、不会报错、
//          验证层也看不出异常，只在运行期表现为偶发的数据撕裂与画面残影。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                          │ 描述                      │
// │────────────────────────────────│─────────────────────────│
// │ FVulkanCommandBuffer()         │ 构造                     │
// │ Begin()                        │ vkBeginCommandBuffer     │
// │ End()                          │ vkEndCommandBuffer       │
// │ Reset()                        │ vkResetCommandBuffer     │
// │ BeginRenderPass()              │ vkCmdBeginRenderPass     │
// │ EndRenderPass()                │ vkCmdEndRenderPass       │
// │ NextSubpass()                  │ vkCmdNextSubpass         │
// │ BindGraphicsPipeline()         │ vkCmdBindPipeline        │
// │ BindComputePipeline()          │ vkCmdBindPipeline        │
// │ BindVertexBuffer()             │ vkCmdBindVertexBuffers   │
// │ BindIndexBuffer()              │ vkCmdBindIndexBuffer     │
// │ BindDescriptorSet()            │ vkCmdBindDescriptorSets  │
// │ PushConstants()                │ vkCmdPushConstants       │
// │ Draw()                         │ vkCmdDraw                │
// │ DrawIndexed()                  │ vkCmdDrawIndexed         │
// │ DrawIndirect()                 │ vkCmdDrawIndirect        │
// │ DrawIndexedIndirect()          │ vkCmdDrawIndexedIndirect │
// │ Dispatch()                     │ vkCmdDispatch            │
// │ DispatchIndirect()             │ vkCmdDispatchIndirect    │
// │ CopyBuffer()                   │ vkCmdCopyBuffer          │
// │ CopyBufferToTexture()          │ vkCmdCopyBufferToImage   │
// │ CopyTextureToBuffer()          │ vkCmdCopyImageToBuffer   │
// │ CopyTexture()                  │ vkCmdCopyImage           │
// │ BlitTexture()                  │ vkCmdBlitImage           │
// │ PipelineBarrier()              │ vkCmdPipelineBarrier     │
// │ SetViewport()                  │ vkCmdSetViewport         │
// │ SetScissor()                   │ vkCmdSetScissor          │
// │ SetLineWidth()                 │ vkCmdSetLineWidth        │
// │ SetDepthBias()                 │ vkCmdSetDepthBias        │
// │ SetBlendConstants()            │ vkCmdSetBlendConstants   │
// │ SetStencilReference()          │ vkCmdSetStencilReference │
// │ BeginQuery()                   │ vkCmdBeginQuery          │
// │ EndQuery()                     │ vkCmdEndQuery            │
// │ ResetQueryPool()               │ vkCmdResetQueryPool      │
// │ WriteTimestamp()               │ vkCmdWriteTimestamp      │
// │ BeginDebugLabel()              │ vkCmdBeginDebugUtilsLabel│
// │ EndDebugLabel()                │ vkCmdEndDebugUtilsLabel  │
// │ InsertDebugLabel()             │ vkCmdInsertDebugUtilsLabel│
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-06  │ LimxTeam  │ 初始创建                        │
// │ 2026-08-30  │ LimxTeam  │ 消除三处静默截断: 管线屏障改分批  │
// │             │           │ 下发, 清除值改内联+分配器回退,    │
// │             │           │ 无效次级缓冲区句柄改为出错日志    │
// ============================================================

#include "Vulkan/FVulkanCommandBuffer.h"

namespace Limx
{

// ============================================================================
// 构造
// ============================================================================

FVulkanCommandBuffer::FVulkanCommandBuffer(
    FVulkanDevice* device,
    FRHICommandBufferHandle handle)
    : m_Device(device)
    , m_CommandBuffer(device->GetVkCommandBuffer(handle))
    , m_Handle(handle)
{
}

// ============================================================================
// 生命周期
// ============================================================================

ERHIResult FVulkanCommandBuffer::Begin()
{
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    VkResult vkResult = vkBeginCommandBuffer(m_CommandBuffer, &beginInfo);
    if (vkResult != VK_SUCCESS)
    {
        return ERHIResult::ErrorUnknown;
    }

    return ERHIResult::Success;
}

ERHIResult FVulkanCommandBuffer::BeginSecondary(
    const FRHICommandBufferInheritance& inheritance)
{
    VkCommandBufferInheritanceInfo inheritInfo = {};
    inheritInfo.sType =
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
    inheritInfo.renderPass  = m_Device->GetVkRenderPass(inheritance.RenderPass);
    inheritInfo.subpass     = inheritance.Subpass;
    inheritInfo.framebuffer =
        inheritance.Framebuffer.IsValid()
            ? m_Device->GetVkFramebuffer(inheritance.Framebuffer)
            : VK_NULL_HANDLE;

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    // RENDER_PASS_CONTINUE 是必需的 —— 它告诉驱动这段命令将在一个已经
    // 开始的渲染通道内部执行。少了这个标志, 验证层会在 vkCmdExecuteCommands
    // 时报错。
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
                    | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
    beginInfo.pInheritanceInfo = &inheritInfo;

    const VkResult vkResult =
        vkBeginCommandBuffer(m_CommandBuffer, &beginInfo);

    if (vkResult != VK_SUCCESS)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] vkBeginCommandBuffer (次级) 失败: {}", (Int32)vkResult);
        return ERHIResult::ErrorUnknown;
    }

    return ERHIResult::Success;
}

ERHIResult FVulkanCommandBuffer::End()
{
    VkResult vkResult = vkEndCommandBuffer(m_CommandBuffer);
    if (vkResult != VK_SUCCESS)
    {
        return ERHIResult::ErrorUnknown;
    }

    return ERHIResult::Success;
}

ERHIResult FVulkanCommandBuffer::Reset()
{
    VkResult vkResult = vkResetCommandBuffer(m_CommandBuffer, 0);
    if (vkResult != VK_SUCCESS)
    {
        return ERHIResult::ErrorUnknown;
    }

    return ERHIResult::Success;
}

// ============================================================================
// 渲染通道
// ============================================================================

void FVulkanCommandBuffer::BeginRenderPass(
    const FRHIRenderPassBeginInfo& beginInfo)
{
    VkRenderPass renderPass = m_Device->GetVkRenderPass(
        beginInfo.RenderPass);
    VkFramebuffer framebuffer = m_Device->GetVkFramebuffer(
        beginInfo.Framebuffer);

    VkRenderPassBeginInfo vkBeginInfo = {};
    vkBeginInfo.sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    vkBeginInfo.renderPass  = renderPass;
    vkBeginInfo.framebuffer = framebuffer;

    vkBeginInfo.renderArea.offset.x      = beginInfo.RenderAreaOffset.X;
    vkBeginInfo.renderArea.offset.y      = beginInfo.RenderAreaOffset.Y;
    vkBeginInfo.renderArea.extent.width  = beginInfo.RenderAreaExtent.Width;
    vkBeginInfo.renderArea.extent.height = beginInfo.RenderAreaExtent.Height;

    // ------------------------------------------------------------------
    // 清除值 — 颜色附件 + 可选深度模板附件
    //
    // 附件数量由渲染通道决定, 不受本函数约束, 因此这里不能有上限。
    // 内联容量只是"绝大多数情况下不碰堆"的优化, 超出时退到分配器。
    //
    // 绝不截断: 少一个清除值时 clearValueCount 会小于最大的
    // LOAD_OP_CLEAR 附件下标, 该附件读到的是未初始化内存 —— 表现为
    // 偶发的画面残影, 而调用本身在 API 层面看不出任何异常。
    // ------------------------------------------------------------------

    constexpr SizeType kInlineClearValues = 16;
    TSmallVector<VkClearValue, kInlineClearValues> clearValues;

    UInt32 colorClearCount = beginInfo.ClearColorCount;
    if (beginInfo.ClearColors == nullptr && colorClearCount > 0)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] BeginRenderPass: 清除颜色数组为空但计数为 {}",
            colorClearCount);
        colorClearCount = 0;
    }

    clearValues.Reserve(static_cast<SizeType>(colorClearCount) + 1);

    // 填充颜色清除值
    for (UInt32 i = 0; i < colorClearCount; ++i)
    {
        VkClearValue value = {};
        value.color.float32[0] = beginInfo.ClearColors[i].R;
        value.color.float32[1] = beginInfo.ClearColors[i].G;
        value.color.float32[2] = beginInfo.ClearColors[i].B;
        value.color.float32[3] = beginInfo.ClearColors[i].A;
        clearValues.Add(value);
    }

    // 填充深度模板清除值 (如果有)
    if (beginInfo.ClearDepthStencil != nullptr)
    {
        VkClearValue value = {};
        value.depthStencil.depth   = beginInfo.ClearDepthStencil->Depth;
        value.depthStencil.stencil = beginInfo.ClearDepthStencil->Stencil;
        clearValues.Add(value);
    }

    vkBeginInfo.clearValueCount =
        static_cast<UInt32>(clearValues.GetSize());
    vkBeginInfo.pClearValues    = clearValues.GetData();

    // 通道内容来自次级缓冲区时必须声明 SECONDARY_COMMAND_BUFFERS。
    //
    // 这不是提示而是约束: 声明为 INLINE 的通道里调用 vkCmdExecuteCommands,
    // 或声明为 SECONDARY 的通道里直接录制绘制命令, 两种都是非法的。
    vkCmdBeginRenderPass(
        m_CommandBuffer, &vkBeginInfo,
        beginInfo.UseSecondaryCommandBuffers
            ? VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS
            : VK_SUBPASS_CONTENTS_INLINE);
}

void FVulkanCommandBuffer::ExecuteCommands(
    const FRHICommandBufferHandle* buffers, UInt32 count)
{
    if (count == 0)
    {
        return;
    }

    if (buffers == nullptr)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] ExecuteCommands: 缓冲区数组为空但计数为 {}", count);
        return;
    }

    // kMaxBatch 是一次 vkCmdExecuteCommands 的批量大小, 不是数量上限 ——
    // 满一批就先交出去, 顺序保持不变。
    constexpr UInt32 kMaxBatch = 64;

    VkCommandBuffer native[kMaxBatch];

    UInt32 written = 0;

    for (UInt32 i = 0; i < count; ++i)
    {
        if (written >= kMaxBatch)
        {
            vkCmdExecuteCommands(m_CommandBuffer, written, native);
            written = 0;
        }

        const VkCommandBuffer handle =
            m_Device->GetVkCommandBuffer(buffers[i]);

        if (handle == VK_NULL_HANDLE)
        {
            // 无效句柄意味着这一段次级缓冲区录下的命令**不会执行**。
            // 默默跳过等于凭空少画一批东西, 而 API 层面看不出异常 ——
            // 必须出声。
            LIMX_LOG(LogRHI, Error,
                "[Vulkan] ExecuteCommands: 第 {} 个次级命令缓冲区句柄无效, "
                "该段命令不会执行", i);
            continue;
        }

        native[written] = handle;
        ++written;
    }

    if (written > 0)
    {
        vkCmdExecuteCommands(m_CommandBuffer, written, native);
    }
}

void FVulkanCommandBuffer::EndRenderPass()
{
    vkCmdEndRenderPass(m_CommandBuffer);
}

void FVulkanCommandBuffer::NextSubpass()
{
    vkCmdNextSubpass(m_CommandBuffer, VK_SUBPASS_CONTENTS_INLINE);
}

// ============================================================================
// 管线绑定
// ============================================================================

void FVulkanCommandBuffer::BindGraphicsPipeline(
    FRHIGraphicsPipelineHandle pipeline)
{
    VkPipeline vkPipeline = m_Device->GetVkGraphicsPipeline(pipeline);
    vkCmdBindPipeline(m_CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                       vkPipeline);
}

void FVulkanCommandBuffer::BindComputePipeline(
    FRHIComputePipelineHandle pipeline)
{
    VkPipeline vkPipeline = m_Device->GetVkComputePipeline(pipeline);
    vkCmdBindPipeline(m_CommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                       vkPipeline);
}

// ============================================================================
// 资源绑定
// ============================================================================

void FVulkanCommandBuffer::BindVertexBuffer(
    UInt32 binding, FRHIBufferHandle buffer, UInt64 offset)
{
    VkBuffer vkBuffer = m_Device->GetVkBuffer(buffer);
    VkDeviceSize vkOffset = static_cast<VkDeviceSize>(offset);
    vkCmdBindVertexBuffers(m_CommandBuffer, binding, 1, &vkBuffer,
                            &vkOffset);
}

void FVulkanCommandBuffer::BindIndexBuffer(
    FRHIBufferHandle buffer, UInt64 offset, EIndexType indexType)
{
    VkBuffer vkBuffer = m_Device->GetVkBuffer(buffer);
    vkCmdBindIndexBuffer(m_CommandBuffer, vkBuffer,
                          static_cast<VkDeviceSize>(offset),
                          ToVkIndexType(indexType));
}

void FVulkanCommandBuffer::BindDescriptorSet(
    EPipelineBindPoint bindPoint,
    FRHIPipelineLayoutHandle layout,
    UInt32 setIndex,
    FRHIDescriptorSetHandle descriptorSet,
    const UInt32* dynamicOffsets,
    UInt32 dynamicOffsetCount)
{
    VkPipelineLayout vkLayout = m_Device->GetVkPipelineLayout(layout);
    VkDescriptorSet vkSet     = m_Device->GetVkDescriptorSet(
        descriptorSet);

    vkCmdBindDescriptorSets(
        m_CommandBuffer,
        ToVkPipelineBindPoint(bindPoint),
        vkLayout,
        setIndex,
        1,
        &vkSet,
        dynamicOffsetCount,
        dynamicOffsets);
}

void FVulkanCommandBuffer::PushConstants(
    FRHIPipelineLayoutHandle layout,
    EShaderStage stageFlags,
    UInt32 offset, UInt32 size,
    const void* data)
{
    VkPipelineLayout vkLayout = m_Device->GetVkPipelineLayout(layout);
    vkCmdPushConstants(m_CommandBuffer, vkLayout,
                        ToVkShaderStageFlags(stageFlags),
                        offset, size, data);
}

// ============================================================================
// 绘制
// ============================================================================

void FVulkanCommandBuffer::Draw(
    UInt32 vertexCount, UInt32 instanceCount,
    UInt32 firstVertex, UInt32 firstInstance)
{
    vkCmdDraw(m_CommandBuffer, vertexCount, instanceCount,
               firstVertex, firstInstance);
}

void FVulkanCommandBuffer::DrawIndexed(
    UInt32 indexCount, UInt32 instanceCount,
    UInt32 firstIndex, Int32 vertexOffset,
    UInt32 firstInstance)
{
    vkCmdDrawIndexed(m_CommandBuffer, indexCount, instanceCount,
                      firstIndex, vertexOffset, firstInstance);
}

void FVulkanCommandBuffer::DrawIndirect(
    FRHIBufferHandle buffer, UInt64 offset,
    UInt32 drawCount, UInt32 stride)
{
    VkBuffer vkBuffer = m_Device->GetVkBuffer(buffer);
    vkCmdDrawIndirect(m_CommandBuffer, vkBuffer,
                       static_cast<VkDeviceSize>(offset),
                       drawCount, stride);
}

void FVulkanCommandBuffer::DrawIndexedIndirect(
    FRHIBufferHandle buffer, UInt64 offset,
    UInt32 drawCount, UInt32 stride)
{
    VkBuffer vkBuffer = m_Device->GetVkBuffer(buffer);
    vkCmdDrawIndexedIndirect(m_CommandBuffer, vkBuffer,
                              static_cast<VkDeviceSize>(offset),
                              drawCount, stride);
}

// ============================================================================
// 网格着色器绘制
// ============================================================================

void FVulkanCommandBuffer::DrawMeshTasks(UInt32 groupCountX,
                                         UInt32 groupCountY,
                                         UInt32 groupCountZ)
{
    const auto& functions = m_Device->GetMeshShaderFunctions();

    // 不支持时报错并返回, 而不是静默返回。
    //
    // 静默返回的后果是画面上少一整条渲染路径, 而那与"这条路径没启用"
    // 长得一模一样 —— 又一条失败会落在通过上的路。
    if (functions.DrawMeshTasks == nullptr)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] DrawMeshTasks — 设备不支持网格着色器, 本次绘制被丢弃");
        return;
    }

    functions.DrawMeshTasks(m_CommandBuffer, groupCountX, groupCountY,
                            groupCountZ);
}

void FVulkanCommandBuffer::DrawMeshTasksIndirect(FRHIBufferHandle buffer,
                                                 UInt64 offset,
                                                 UInt32 drawCount,
                                                 UInt32 stride)
{
    const auto& functions = m_Device->GetMeshShaderFunctions();

    if (functions.DrawMeshTasksIndirect == nullptr)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] DrawMeshTasksIndirect — 设备不支持网格着色器, "
            "本次绘制被丢弃");
        return;
    }

    VkBuffer vkBuffer = m_Device->GetVkBuffer(buffer);

    functions.DrawMeshTasksIndirect(m_CommandBuffer, vkBuffer,
                                    static_cast<VkDeviceSize>(offset),
                                    drawCount, stride);
}

// ============================================================================
// 计算
// ============================================================================

// ============================================================================
// 加速结构构建
// ============================================================================

void FVulkanCommandBuffer::BuildAccelStruct(
    FRHIAccelStructHandle handle, UInt32 instanceCount)
{
    const FVulkanAccelStructData* data =
        m_Device->GetAccelStructData(handle);

    const FVulkanDevice::FRayTracingFunctions& rt =
        m_Device->GetRayTracingFunctions();

    if (data == nullptr || data->AccelStruct == VK_NULL_HANDLE ||
        rt.CmdBuild == nullptr)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] BuildAccelStruct 的句柄无效或扩展函数缺失");
        return;
    }

    // 本次构建的图元数。TLAS 是实例数, BLAS 是三角形数。
    //
    // TLAS 传 0 会构建出一棵空树 —— 所有射线都不命中, 而画面上"什么都
    // 没照到"和"场景本来就是空的"看起来完全一样。所以这里把它判掉。
    UInt32 primitiveCount = data->PrimitiveCount;

    if (data->IsTopLevel)
    {
        if (instanceCount == 0)
        {
            LIMX_LOG(LogRHI, Error,
                "[Vulkan] TLAS 构建的实例数为 0 — 这会得到一棵空树");
            return;
        }

        if (instanceCount > data->MaxInstanceCount)
        {
            LIMX_LOG(LogRHI, Error,
                "[Vulkan] TLAS 构建的实例数 {} 超过上限 {}",
                instanceCount, data->MaxInstanceCount);
            return;
        }

        primitiveCount = instanceCount;
    }

    // 几何信息在创建时用过一遍, 这里必须**逐字重建**同一份 —— Vulkan 不
    // 保存它。少填一个字段 (比如 maxVertex) 的后果不是报错, 而是驱动按
    // 一个错的范围去读顶点。
    VkAccelerationStructureGeometryKHR geom = {};
    geom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;

    if (data->IsTopLevel)
    {
        geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        geom.flags        = VK_GEOMETRY_OPAQUE_BIT_KHR;

        VkAccelerationStructureGeometryInstancesDataKHR& inst =
            geom.geometry.instances;
        inst.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        inst.arrayOfPointers    = VK_FALSE;
        inst.data.deviceAddress = data->InstanceAddress;
    }
    else
    {
        geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geom.flags        = data->GeometryFlags;

        VkAccelerationStructureGeometryTrianglesDataKHR& tri =
            geom.geometry.triangles;
        tri.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        tri.vertexFormat             = data->VertexFormat;
        tri.vertexData.deviceAddress = data->VertexAddress;
        tri.vertexStride             = data->VertexStride;
        tri.maxVertex                = data->MaxVertex;
        tri.indexType                = data->IndexType;
        tri.indexData.deviceAddress  = data->IndexAddress;
        tri.transformData.deviceAddress = 0;
    }

    // 暂存地址向上取整到规范要求的对齐。创建时已经为此多要了 (对齐-1) 字节。
    const UInt64 rawScratch =
        m_Device->GetBufferDeviceAddress(data->ScratchBuffer);

    if (rawScratch == 0)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] 暂存缓冲区取不到设备地址 — 构建取消");
        return;
    }

    const UInt64 align = m_Device->GetScratchAlignment();
    const UInt64 scratchAddress = (rawScratch + align - 1) & ~(align - 1);

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo = {};
    buildInfo.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type = data->IsTopLevel
        ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR
        : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    buildInfo.flags = data->BuildFlags;
    buildInfo.mode  = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.dstAccelerationStructure = data->AccelStruct;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries   = &geom;
    buildInfo.scratchData.deviceAddress = scratchAddress;

    VkAccelerationStructureBuildRangeInfoKHR range = {};
    range.primitiveCount  = primitiveCount;
    range.primitiveOffset = 0;
    range.firstVertex     = 0;
    range.transformOffset = 0;

    const VkAccelerationStructureBuildRangeInfoKHR* ranges[] = { &range };

    rt.CmdBuild(m_CommandBuffer, 1, &buildInfo, ranges);
}

void FVulkanCommandBuffer::AccelStructBarrier()
{
    // 构建写 -> 着色器读。
    //
    // 这里必须是全局内存屏障而不是缓冲区屏障: 加速结构的存储在驱动看来
    // 不只是那块存储缓冲区, TLAS 还会去读 BLAS。按缓冲区逐个挡的话,
    // 挡不住的那部分是驱动内部的引用, 而那正是竞争最容易发生的地方。
    VkMemoryBarrier barrier = {};
    barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                            VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(
        m_CommandBuffer,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void FVulkanCommandBuffer::Dispatch(
    UInt32 groupCountX, UInt32 groupCountY, UInt32 groupCountZ)
{
    vkCmdDispatch(m_CommandBuffer, groupCountX, groupCountY,
                   groupCountZ);
}

void FVulkanCommandBuffer::DispatchIndirect(
    FRHIBufferHandle buffer, UInt64 offset)
{
    VkBuffer vkBuffer = m_Device->GetVkBuffer(buffer);
    vkCmdDispatchIndirect(m_CommandBuffer, vkBuffer,
                           static_cast<VkDeviceSize>(offset));
}

// ============================================================================
// 资源拷贝
// ============================================================================

void FVulkanCommandBuffer::CopyBuffer(
    FRHIBufferHandle src, FRHIBufferHandle dst,
    const FRHIBufferCopyRegion& region)
{
    VkBuffer vkSrc = m_Device->GetVkBuffer(src);
    VkBuffer vkDst = m_Device->GetVkBuffer(dst);

    VkBufferCopy vkRegion;
    vkRegion.srcOffset = region.SrcOffset;
    vkRegion.dstOffset = region.DstOffset;
    vkRegion.size      = region.Size;

    vkCmdCopyBuffer(m_CommandBuffer, vkSrc, vkDst, 1, &vkRegion);
}

void FVulkanCommandBuffer::CopyBufferToTexture(
    FRHIBufferHandle srcBuffer,
    FRHITextureHandle dstTexture,
    EImageLayout dstLayout,
    const FRHIBufferTextureCopyRegion& region)
{
    VkBuffer vkBuffer = m_Device->GetVkBuffer(srcBuffer);
    VkImage vkImage   = m_Device->GetVkImage(dstTexture);

    EPixelFormat dstFormat = m_Device->GetTextureFormat(dstTexture);

    VkBufferImageCopy vkRegion = {};
    vkRegion.bufferOffset      = region.BufferOffset;
    vkRegion.bufferRowLength   = region.BufferRowLength;
    vkRegion.bufferImageHeight = region.BufferImageHeight;

    vkRegion.imageSubresource.aspectMask =
        GetVkImageAspectFlags(dstFormat);
    vkRegion.imageSubresource.mipLevel       = region.MipLevel;
    vkRegion.imageSubresource.baseArrayLayer  = region.BaseLayer;
    vkRegion.imageSubresource.layerCount      = region.LayerCount;

    vkRegion.imageOffset.x = region.TextureOffset.X;
    vkRegion.imageOffset.y = region.TextureOffset.Y;
    vkRegion.imageOffset.z = region.TextureOffset.Z;

    vkRegion.imageExtent.width  = region.TextureExtent.Width;
    vkRegion.imageExtent.height = region.TextureExtent.Height;
    vkRegion.imageExtent.depth  = region.TextureExtent.Depth;

    vkCmdCopyBufferToImage(m_CommandBuffer, vkBuffer, vkImage,
                            ToVkImageLayout(dstLayout), 1, &vkRegion);
}

void FVulkanCommandBuffer::CopyTextureToBuffer(
    FRHITextureHandle srcTexture,
    EImageLayout srcLayout,
    FRHIBufferHandle dstBuffer,
    const FRHIBufferTextureCopyRegion& region)
{
    VkImage vkImage   = m_Device->GetVkImage(srcTexture);
    VkBuffer vkBuffer = m_Device->GetVkBuffer(dstBuffer);

    EPixelFormat srcFormat = m_Device->GetTextureFormat(srcTexture);

    VkBufferImageCopy vkRegion = {};
    vkRegion.bufferOffset      = region.BufferOffset;
    vkRegion.bufferRowLength   = region.BufferRowLength;
    vkRegion.bufferImageHeight = region.BufferImageHeight;

    vkRegion.imageSubresource.aspectMask =
        GetVkImageAspectFlags(srcFormat);
    vkRegion.imageSubresource.mipLevel       = region.MipLevel;
    vkRegion.imageSubresource.baseArrayLayer  = region.BaseLayer;
    vkRegion.imageSubresource.layerCount      = region.LayerCount;

    vkRegion.imageOffset.x = region.TextureOffset.X;
    vkRegion.imageOffset.y = region.TextureOffset.Y;
    vkRegion.imageOffset.z = region.TextureOffset.Z;

    vkRegion.imageExtent.width  = region.TextureExtent.Width;
    vkRegion.imageExtent.height = region.TextureExtent.Height;
    vkRegion.imageExtent.depth  = region.TextureExtent.Depth;

    vkCmdCopyImageToBuffer(m_CommandBuffer, vkImage,
                            ToVkImageLayout(srcLayout),
                            vkBuffer, 1, &vkRegion);
}

void FVulkanCommandBuffer::BlitTexture(
    FRHITextureHandle src, EImageLayout srcLayout,
    FRHITextureHandle dst, EImageLayout dstLayout,
    const FRHITextureBlitRegion& region, EFilter filter)
{
    VkImage vkSrc = m_Device->GetVkImage(src);
    VkImage vkDst = m_Device->GetVkImage(dst);

    EPixelFormat srcFormat = m_Device->GetTextureFormat(src);
    EPixelFormat dstFormat = m_Device->GetTextureFormat(dst);

    VkImageBlit vkRegion = {};

    vkRegion.srcSubresource.aspectMask =
        GetVkImageAspectFlags(srcFormat);
    vkRegion.srcSubresource.mipLevel       = region.SrcMipLevel;
    vkRegion.srcSubresource.baseArrayLayer = region.SrcBaseLayer;
    vkRegion.srcSubresource.layerCount     = region.SrcLayerCount;

    vkRegion.srcOffsets[0].x = region.SrcOffsetMin.X;
    vkRegion.srcOffsets[0].y = region.SrcOffsetMin.Y;
    vkRegion.srcOffsets[0].z = region.SrcOffsetMin.Z;
    vkRegion.srcOffsets[1].x = region.SrcOffsetMax.X;
    vkRegion.srcOffsets[1].y = region.SrcOffsetMax.Y;
    vkRegion.srcOffsets[1].z = region.SrcOffsetMax.Z;

    vkRegion.dstSubresource.aspectMask =
        GetVkImageAspectFlags(dstFormat);
    vkRegion.dstSubresource.mipLevel       = region.DstMipLevel;
    vkRegion.dstSubresource.baseArrayLayer = region.DstBaseLayer;
    vkRegion.dstSubresource.layerCount     = region.DstLayerCount;

    vkRegion.dstOffsets[0].x = region.DstOffsetMin.X;
    vkRegion.dstOffsets[0].y = region.DstOffsetMin.Y;
    vkRegion.dstOffsets[0].z = region.DstOffsetMin.Z;
    vkRegion.dstOffsets[1].x = region.DstOffsetMax.X;
    vkRegion.dstOffsets[1].y = region.DstOffsetMax.Y;
    vkRegion.dstOffsets[1].z = region.DstOffsetMax.Z;

    vkCmdBlitImage(m_CommandBuffer, vkSrc, ToVkImageLayout(srcLayout),
                    vkDst, ToVkImageLayout(dstLayout),
                    1, &vkRegion, ToVkFilter(filter));
}

// ============================================================================
// 清除操作
// ============================================================================

void FVulkanCommandBuffer::ClearColorImage(
    FRHITextureHandle texture,
    EImageLayout layout,
    const FLinearColor& color)
{
    VkImage vkImage = m_Device->GetVkImage(texture);

    VkClearColorValue clearColor = {};
    clearColor.float32[0] = color.R;
    clearColor.float32[1] = color.G;
    clearColor.float32[2] = color.B;
    clearColor.float32[3] = color.A;

    VkImageSubresourceRange range = {};
    range.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel   = 0;
    range.levelCount     = 1;
    range.baseArrayLayer = 0;
    range.layerCount     = 1;

    vkCmdClearColorImage(m_CommandBuffer, vkImage,
                          ToVkImageLayout(layout),
                          &clearColor, 1, &range);
}

// ============================================================================
// 管线屏障
// ============================================================================

void FVulkanCommandBuffer::PipelineBarrier(
    EPipelineStageFlags srcStageMask,
    EPipelineStageFlags dstStageMask,
    const FRHIMemoryBarrier* memoryBarriers,
    UInt32 memoryBarrierCount,
    const FRHIBufferMemoryBarrier* bufferBarriers,
    UInt32 bufferBarrierCount,
    const FRHIImageMemoryBarrier* imageBarriers,
    UInt32 imageBarrierCount)
{
    // ------------------------------------------------------------------
    // 分批下发 — 屏障总数不设上限
    //
    // kBarriersPerBatch 是**一次 vkCmdPipelineBarrier 携带的批量大小**,
    // 不是调用方能提交的屏障数量上限。超出批量的部分拆成多次调用下发,
    // 一条都不丢。
    //
    // 为什么不直接把常量调大: 任何固定上限都只是把同一个坑挪远 —— 超限
    // 时丢弃屏障不崩溃、不报错, 验证层看到的是一次合法的、只是少了几个
    // 屏障的调用, 症状要到运行期才以偶发数据撕裂的形式出现。
    //
    // 拆分的语义等价性: 两次调用之间没有录制任何其它命令, 且两次用的是
    // 同一对 srcStageMask/dstStageMask。第一次调用的第二同步作用域覆盖
    // 其后的全部命令, 第二次调用的第一同步作用域覆盖其前的全部命令 ——
    // 于是对任意"之前的命令 P"与"之后的命令 Q", 两种写法建立的依赖完全
    // 相同, 各批次自身的内存依赖也逐条保留。
    // ------------------------------------------------------------------

    constexpr UInt32 kBarriersPerBatch = 16;

    VkMemoryBarrier       vkMemBarriers[kBarriersPerBatch];
    VkBufferMemoryBarrier vkBufBarriers[kBarriersPerBatch];
    VkImageMemoryBarrier  vkImgBarriers[kBarriersPerBatch];

    // 数组为空却给了非零计数是调用方的错误。这里必须出声 —— 静默当作 0
    // 处理正是"屏障没下发, 一切看起来却正常"的那条路径。
    UInt32 memTotal = memoryBarrierCount;
    if (memoryBarriers == nullptr && memTotal > 0)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] PipelineBarrier: 内存屏障数组为空但计数为 {}", memTotal);
        memTotal = 0;
    }

    UInt32 bufTotal = bufferBarrierCount;
    if (bufferBarriers == nullptr && bufTotal > 0)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] PipelineBarrier: 缓冲区屏障数组为空但计数为 {}", bufTotal);
        bufTotal = 0;
    }

    UInt32 imgTotal = imageBarrierCount;
    if (imageBarriers == nullptr && imgTotal > 0)
    {
        LIMX_LOG(LogRHI, Error,
            "[Vulkan] PipelineBarrier: 图像屏障数组为空但计数为 {}", imgTotal);
        imgTotal = 0;
    }

    const VkPipelineStageFlags vkSrcStage =
        ToVkPipelineStageFlags(srcStageMask);
    const VkPipelineStageFlags vkDstStage =
        ToVkPipelineStageFlags(dstStageMask);

    UInt32 memDone = 0;
    UInt32 bufDone = 0;
    UInt32 imgDone = 0;

    // 三个计数全为 0 时仍要下发一次 —— 那是一次纯执行依赖
    // (srcStage → dstStage), 调用方要的正是它。
    bool isFirstBatch = true;

    while (isFirstBatch
           || memDone < memTotal
           || bufDone < bufTotal
           || imgDone < imgTotal)
    {
        isFirstBatch = false;

        // ---------------- 内存屏障 ----------------
        UInt32 memCount = memTotal - memDone;
        if (memCount > kBarriersPerBatch)
        {
            memCount = kBarriersPerBatch;
        }

        for (UInt32 i = 0; i < memCount; ++i)
        {
            const FRHIMemoryBarrier& source = memoryBarriers[memDone + i];

            MemZero(&vkMemBarriers[i], sizeof(VkMemoryBarrier));
            vkMemBarriers[i].sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            vkMemBarriers[i].srcAccessMask =
                ToVkAccessFlags(source.SrcAccessMask);
            vkMemBarriers[i].dstAccessMask =
                ToVkAccessFlags(source.DstAccessMask);
        }

        // ---------------- 缓冲区屏障 ----------------
        UInt32 bufCount = bufTotal - bufDone;
        if (bufCount > kBarriersPerBatch)
        {
            bufCount = kBarriersPerBatch;
        }

        for (UInt32 i = 0; i < bufCount; ++i)
        {
            const FRHIBufferMemoryBarrier& source =
                bufferBarriers[bufDone + i];

            MemZero(&vkBufBarriers[i], sizeof(VkBufferMemoryBarrier));
            vkBufBarriers[i].sType =
                VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            vkBufBarriers[i].srcAccessMask =
                ToVkAccessFlags(source.SrcAccessMask);
            vkBufBarriers[i].dstAccessMask =
                ToVkAccessFlags(source.DstAccessMask);
            vkBufBarriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            vkBufBarriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            vkBufBarriers[i].buffer = m_Device->GetVkBuffer(source.Buffer);
            vkBufBarriers[i].offset = source.Offset;
            vkBufBarriers[i].size   = source.Size;
        }

        // ---------------- 图像屏障 ----------------
        UInt32 imgCount = imgTotal - imgDone;
        if (imgCount > kBarriersPerBatch)
        {
            imgCount = kBarriersPerBatch;
        }

        for (UInt32 i = 0; i < imgCount; ++i)
        {
            const FRHIImageMemoryBarrier& source =
                imageBarriers[imgDone + i];

            MemZero(&vkImgBarriers[i], sizeof(VkImageMemoryBarrier));
            vkImgBarriers[i].sType =
                VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            vkImgBarriers[i].srcAccessMask =
                ToVkAccessFlags(source.SrcAccessMask);
            vkImgBarriers[i].dstAccessMask =
                ToVkAccessFlags(source.DstAccessMask);
            vkImgBarriers[i].oldLayout = ToVkImageLayout(source.OldLayout);
            vkImgBarriers[i].newLayout = ToVkImageLayout(source.NewLayout);
            vkImgBarriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            vkImgBarriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            vkImgBarriers[i].image = m_Device->GetVkImage(source.Texture);

            const EPixelFormat imgFormat =
                m_Device->GetTextureFormat(source.Texture);
            vkImgBarriers[i].subresourceRange.aspectMask =
                GetVkImageAspectFlags(imgFormat);
            vkImgBarriers[i].subresourceRange.baseMipLevel =
                source.BaseMipLevel;
            vkImgBarriers[i].subresourceRange.levelCount =
                source.MipLevelCount;
            vkImgBarriers[i].subresourceRange.baseArrayLayer =
                source.BaseArrayLayer;
            vkImgBarriers[i].subresourceRange.layerCount =
                source.ArrayLayerCount;
        }

        vkCmdPipelineBarrier(
            m_CommandBuffer,
            vkSrcStage,
            vkDstStage,
            0,
            memCount,   vkMemBarriers,
            bufCount,   vkBufBarriers,
            imgCount,   vkImgBarriers);

        memDone += memCount;
        bufDone += bufCount;
        imgDone += imgCount;
    }
}

// ============================================================================
// 动态状态
// ============================================================================

void FVulkanCommandBuffer::SetViewport(const FRHIViewport& viewport)
{
    VkViewport vkViewport;
    vkViewport.x        = viewport.X;
    vkViewport.y        = viewport.Y;
    vkViewport.width    = viewport.Width;
    vkViewport.height   = viewport.Height;
    vkViewport.minDepth = viewport.MinDepth;
    vkViewport.maxDepth = viewport.MaxDepth;

    vkCmdSetViewport(m_CommandBuffer, 0, 1, &vkViewport);
}

void FVulkanCommandBuffer::SetScissor(const FRHIScissorRect& scissor)
{
    VkRect2D vkScissor;
    vkScissor.offset.x      = scissor.X;
    vkScissor.offset.y      = scissor.Y;
    vkScissor.extent.width  = scissor.Width;
    vkScissor.extent.height = scissor.Height;

    vkCmdSetScissor(m_CommandBuffer, 0, 1, &vkScissor);
}

void FVulkanCommandBuffer::SetLineWidth(Float32 lineWidth)
{
    vkCmdSetLineWidth(m_CommandBuffer, lineWidth);
}

void FVulkanCommandBuffer::SetDepthBias(
    Float32 constantFactor, Float32 clamp, Float32 slopeFactor)
{
    vkCmdSetDepthBias(m_CommandBuffer, constantFactor, clamp,
                       slopeFactor);
}

void FVulkanCommandBuffer::SetBlendConstants(
    const Float32 blendConstants[4])
{
    vkCmdSetBlendConstants(m_CommandBuffer, blendConstants);
}

void FVulkanCommandBuffer::SetStencilReference(UInt32 reference)
{
    vkCmdSetStencilReference(m_CommandBuffer,
                              VK_STENCIL_FACE_FRONT_AND_BACK,
                              reference);
}

// ============================================================================
// 布局转换
// ============================================================================

void FVulkanCommandBuffer::TransitionImageLayout(
    FRHITextureHandle texture,
    EImageLayout oldLayout,
    EImageLayout newLayout,
    EPipelineStageFlags srcStage,
    EPipelineStageFlags dstStage,
    EAccessFlags srcAccess,
    EAccessFlags dstAccess,
    UInt32 baseMipLevel,
    UInt32 mipLevelCount,
    UInt32 baseArrayLayer,
    UInt32 arrayLayerCount)
{
    VkImageMemoryBarrier barrier = {};
    barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask       = ToVkAccessFlags(srcAccess);
    barrier.dstAccessMask       = ToVkAccessFlags(dstAccess);
    barrier.oldLayout           = ToVkImageLayout(oldLayout);
    barrier.newLayout           = ToVkImageLayout(newLayout);
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = m_Device->GetVkImage(texture);

    EPixelFormat format = m_Device->GetTextureFormat(texture);
    barrier.subresourceRange.aspectMask     = GetVkImageAspectFlags(format);
    barrier.subresourceRange.baseMipLevel   = baseMipLevel;
    barrier.subresourceRange.levelCount     = mipLevelCount;
    barrier.subresourceRange.baseArrayLayer = baseArrayLayer;
    barrier.subresourceRange.layerCount     = arrayLayerCount;

    vkCmdPipelineBarrier(
        m_CommandBuffer,
        ToVkPipelineStageFlags(srcStage),
        ToVkPipelineStageFlags(dstStage),
        0,
        0, nullptr,
        0, nullptr,
        1, &barrier);
}

// ============================================================================
// 查询
// ============================================================================

void FVulkanCommandBuffer::BeginQuery(
    FRHIQueryPoolHandle queryPool, UInt32 queryIndex)
{
    VkQueryPool pool = m_Device->GetVkQueryPool(queryPool);
    vkCmdBeginQuery(m_CommandBuffer, pool, queryIndex, 0);
}

void FVulkanCommandBuffer::EndQuery(
    FRHIQueryPoolHandle queryPool, UInt32 queryIndex)
{
    VkQueryPool pool = m_Device->GetVkQueryPool(queryPool);
    vkCmdEndQuery(m_CommandBuffer, pool, queryIndex);
}

void FVulkanCommandBuffer::ResetQueryPool(
    FRHIQueryPoolHandle queryPool,
    UInt32 firstQuery, UInt32 queryCount)
{
    VkQueryPool pool = m_Device->GetVkQueryPool(queryPool);
    vkCmdResetQueryPool(m_CommandBuffer, pool, firstQuery, queryCount);
}

void FVulkanCommandBuffer::WriteTimestamp(
    EPipelineStageFlags pipelineStage,
    FRHIQueryPoolHandle queryPool, UInt32 queryIndex)
{
    VkQueryPool pool = m_Device->GetVkQueryPool(queryPool);
    vkCmdWriteTimestamp(
        m_CommandBuffer,
        static_cast<VkPipelineStageFlagBits>(
            ToVkPipelineStageFlags(pipelineStage)),
        pool, queryIndex);
}

// ============================================================================
// 调试标记
// ============================================================================

void FVulkanCommandBuffer::BeginDebugLabel(
    const char* name,
    Float32 r, Float32 g, Float32 b, Float32 a)
{
    auto func = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
        vkGetDeviceProcAddr(m_Device->GetVkDevice(),
            "vkCmdBeginDebugUtilsLabelEXT"));
    if (func == nullptr)
    {
        return;
    }

    VkDebugUtilsLabelEXT label = {};
    label.sType      = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    label.pLabelName = name;
    label.color[0]   = r;
    label.color[1]   = g;
    label.color[2]   = b;
    label.color[3]   = a;

    func(m_CommandBuffer, &label);
}

void FVulkanCommandBuffer::EndDebugLabel()
{
    auto func = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
        vkGetDeviceProcAddr(m_Device->GetVkDevice(),
            "vkCmdEndDebugUtilsLabelEXT"));
    if (func != nullptr)
    {
        func(m_CommandBuffer);
    }
}

void FVulkanCommandBuffer::InsertDebugLabel(
    const char* name,
    Float32 r, Float32 g, Float32 b, Float32 a)
{
    auto func = reinterpret_cast<PFN_vkCmdInsertDebugUtilsLabelEXT>(
        vkGetDeviceProcAddr(m_Device->GetVkDevice(),
            "vkCmdInsertDebugUtilsLabelEXT"));
    if (func == nullptr)
    {
        return;
    }

    VkDebugUtilsLabelEXT label = {};
    label.sType      = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    label.pLabelName = name;
    label.color[0]   = r;
    label.color[1]   = g;
    label.color[2]   = b;
    label.color[3]   = a;

    func(m_CommandBuffer, &label);
}

} // namespace Limx
