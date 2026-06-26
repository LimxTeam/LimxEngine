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

    // 转换清除值 — 颜色附件 + 可选深度模板附件
    constexpr UInt32 kMaxClearValues = 16;
    VkClearValue clearValues[kMaxClearValues];
    UInt32 clearCount = 0;

    // 填充颜色清除值
    for (UInt32 i = 0; i < beginInfo.ClearColorCount
         && clearCount < kMaxClearValues; ++i)
    {
        clearValues[clearCount].color.float32[0] =
            beginInfo.ClearColors[i].R;
        clearValues[clearCount].color.float32[1] =
            beginInfo.ClearColors[i].G;
        clearValues[clearCount].color.float32[2] =
            beginInfo.ClearColors[i].B;
        clearValues[clearCount].color.float32[3] =
            beginInfo.ClearColors[i].A;
        ++clearCount;
    }

    // 填充深度模板清除值 (如果有)
    if (beginInfo.ClearDepthStencil != nullptr
        && clearCount < kMaxClearValues)
    {
        clearValues[clearCount].depthStencil.depth =
            beginInfo.ClearDepthStencil->Depth;
        clearValues[clearCount].depthStencil.stencil =
            beginInfo.ClearDepthStencil->Stencil;
        ++clearCount;
    }

    vkBeginInfo.clearValueCount = clearCount;
    vkBeginInfo.pClearValues    = clearValues;

    vkCmdBeginRenderPass(m_CommandBuffer, &vkBeginInfo,
                          VK_SUBPASS_CONTENTS_INLINE);
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
// 计算
// ============================================================================

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
    // 转换内存屏障
    constexpr UInt32 kMaxBarriers = 16;

    VkMemoryBarrier vkMemBarriers[kMaxBarriers];
    UInt32 memCount = memoryBarrierCount;
    if (memCount > kMaxBarriers)
    {
        memCount = kMaxBarriers;
    }

    for (UInt32 i = 0; i < memCount; ++i)
    {
        MemZero(&vkMemBarriers[i], sizeof(VkMemoryBarrier));
        vkMemBarriers[i].sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        vkMemBarriers[i].srcAccessMask = ToVkAccessFlags(
            memoryBarriers[i].SrcAccessMask);
        vkMemBarriers[i].dstAccessMask = ToVkAccessFlags(
            memoryBarriers[i].DstAccessMask);
    }

    // 转换缓冲区屏障
    VkBufferMemoryBarrier vkBufBarriers[kMaxBarriers];
    UInt32 bufCount = bufferBarrierCount;
    if (bufCount > kMaxBarriers)
    {
        bufCount = kMaxBarriers;
    }

    for (UInt32 i = 0; i < bufCount; ++i)
    {
        MemZero(&vkBufBarriers[i], sizeof(VkBufferMemoryBarrier));
        vkBufBarriers[i].sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        vkBufBarriers[i].srcAccessMask       = ToVkAccessFlags(
            bufferBarriers[i].SrcAccessMask);
        vkBufBarriers[i].dstAccessMask       = ToVkAccessFlags(
            bufferBarriers[i].DstAccessMask);
        vkBufBarriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkBufBarriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkBufBarriers[i].buffer              = m_Device->GetVkBuffer(
            bufferBarriers[i].Buffer);
        vkBufBarriers[i].offset              = bufferBarriers[i].Offset;
        vkBufBarriers[i].size                = bufferBarriers[i].Size;
    }

    // 转换图像屏障
    VkImageMemoryBarrier vkImgBarriers[kMaxBarriers];
    UInt32 imgCount = imageBarrierCount;
    if (imgCount > kMaxBarriers)
    {
        imgCount = kMaxBarriers;
    }

    for (UInt32 i = 0; i < imgCount; ++i)
    {
        MemZero(&vkImgBarriers[i], sizeof(VkImageMemoryBarrier));
        vkImgBarriers[i].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        vkImgBarriers[i].srcAccessMask       = ToVkAccessFlags(
            imageBarriers[i].SrcAccessMask);
        vkImgBarriers[i].dstAccessMask       = ToVkAccessFlags(
            imageBarriers[i].DstAccessMask);
        vkImgBarriers[i].oldLayout           = ToVkImageLayout(
            imageBarriers[i].OldLayout);
        vkImgBarriers[i].newLayout           = ToVkImageLayout(
            imageBarriers[i].NewLayout);
        vkImgBarriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkImgBarriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkImgBarriers[i].image               = m_Device->GetVkImage(
            imageBarriers[i].Texture);

        EPixelFormat imgFormat = m_Device->GetTextureFormat(
            imageBarriers[i].Texture);
        vkImgBarriers[i].subresourceRange.aspectMask =
            GetVkImageAspectFlags(imgFormat);
        vkImgBarriers[i].subresourceRange.baseMipLevel =
            imageBarriers[i].BaseMipLevel;
        vkImgBarriers[i].subresourceRange.levelCount =
            imageBarriers[i].MipLevelCount;
        vkImgBarriers[i].subresourceRange.baseArrayLayer =
            imageBarriers[i].BaseArrayLayer;
        vkImgBarriers[i].subresourceRange.layerCount =
            imageBarriers[i].ArrayLayerCount;
    }

    vkCmdPipelineBarrier(
        m_CommandBuffer,
        ToVkPipelineStageFlags(srcStageMask),
        ToVkPipelineStageFlags(dstStageMask),
        0,
        memCount,   vkMemBarriers,
        bufCount,   vkBufBarriers,
        imgCount,   vkImgBarriers);
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
