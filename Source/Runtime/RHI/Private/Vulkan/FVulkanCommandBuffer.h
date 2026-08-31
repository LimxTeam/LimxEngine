// ============================================================
// 文件名称：FVulkanCommandBuffer.h
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：命令缓冲区作为轻量包装器持有 VkCommandBuffer 和
//          FVulkanDevice 反向引用，所有命令录制直接调用 Vulkan API，
//          句柄到原生对象的转换通过 FVulkanDevice 访问器完成。
// 功能描述：IRHICommandBuffer 的 Vulkan 实现 — 封装 VkCommandBuffer
//          的完整命令录制接口，包括渲染通道、管线绑定、资源绑定、
//          绘制调用、计算分派、资源拷贝、管线屏障、查询、调试标记。
// 技术特性：持有 FVulkanDevice* 反向引用用于句柄→原生对象转换；
//          所有方法直接转发到 vkCmd* 函数，零额外开销。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                          │ 描述                      │
// │────────────────────────────────│─────────────────────────│
// │ FVulkanCommandBuffer()         │ 构造 (绑定设备和句柄)      │
// │ Begin()                        │ 开始录制                  │
// │ End()                          │ 结束录制                  │
// │ Reset()                        │ 重置命令缓冲区             │
// │ (全部 IRHICommandBuffer 方法)   │ 见 IRHICommandBuffer.h   │
//
// ── 字段表 ──────────────────────────────────────────────────
// │ 字段名              │ 类型                     │ 描述         │
// │────────────────────│────────────────────────│────────────│
// │ m_Device           │ FVulkanDevice*          │ 设备反向引用  │
// │ m_CommandBuffer    │ VkCommandBuffer         │ 原生句柄     │
// │ m_Handle           │ FRHICommandBufferHandle │ RHI 句柄    │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-06  │ LimxTeam  │ 初始创建                        │
// ============================================================

#pragma once

#include "Vulkan/FVulkanDevice.h"

namespace Limx
{

class FVulkanCommandBuffer final : public IRHICommandBuffer
{
public:
    /// 构造 — 绑定到指定设备和命令缓冲区句柄
    FVulkanCommandBuffer(FVulkanDevice* device,
                          FRHICommandBufferHandle handle);
    ~FVulkanCommandBuffer() override = default;

    // ====================================================================
    // 生命周期
    // ====================================================================

    ERHIResult Begin() override;
    ERHIResult BeginSecondary(
        const FRHICommandBufferInheritance& inheritance) override;
    ERHIResult End() override;
    ERHIResult Reset() override;

    // ====================================================================
    // 渲染通道
    // ====================================================================

    void BeginRenderPass(const FRHIRenderPassBeginInfo& beginInfo) override;
    void EndRenderPass() override;
    void ExecuteCommands(const FRHICommandBufferHandle* buffers,
                          UInt32 count) override;
    void NextSubpass() override;

    // ====================================================================
    // 管线绑定
    // ====================================================================

    void BindGraphicsPipeline(FRHIGraphicsPipelineHandle pipeline) override;
    void BindComputePipeline(FRHIComputePipelineHandle pipeline) override;

    // ====================================================================
    // 资源绑定
    // ====================================================================

    void BindVertexBuffer(UInt32 binding, FRHIBufferHandle buffer,
                           UInt64 offset) override;
    void BindIndexBuffer(FRHIBufferHandle buffer, UInt64 offset,
                          EIndexType indexType) override;
    void BindDescriptorSet(EPipelineBindPoint bindPoint,
                            FRHIPipelineLayoutHandle layout,
                            UInt32 setIndex,
                            FRHIDescriptorSetHandle descriptorSet,
                            const UInt32* dynamicOffsets,
                            UInt32 dynamicOffsetCount) override;
    void PushConstants(FRHIPipelineLayoutHandle layout,
                        EShaderStage stageFlags,
                        UInt32 offset, UInt32 size,
                        const void* data) override;

    // ====================================================================
    // 绘制
    // ====================================================================

    void Draw(UInt32 vertexCount, UInt32 instanceCount,
               UInt32 firstVertex, UInt32 firstInstance) override;
    void DrawIndexed(UInt32 indexCount, UInt32 instanceCount,
                      UInt32 firstIndex, Int32 vertexOffset,
                      UInt32 firstInstance) override;
    void DrawIndirect(FRHIBufferHandle buffer, UInt64 offset,
                       UInt32 drawCount, UInt32 stride) override;
    void DrawIndexedIndirect(FRHIBufferHandle buffer, UInt64 offset,
                              UInt32 drawCount,
                              UInt32 stride) override;

    // ====================================================================
    // 计算
    // ====================================================================

    void BuildAccelStruct(FRHIAccelStructHandle handle,
                          UInt32 instanceCount) override;
    void AccelStructBarrier() override;

    void Dispatch(UInt32 groupCountX, UInt32 groupCountY,
                   UInt32 groupCountZ) override;
    void DispatchIndirect(FRHIBufferHandle buffer,
                           UInt64 offset) override;

    // ====================================================================
    // 资源拷贝
    // ====================================================================

    void CopyBuffer(FRHIBufferHandle src, FRHIBufferHandle dst,
                     const FRHIBufferCopyRegion& region) override;
    void CopyBufferToTexture(FRHIBufferHandle srcBuffer,
                              FRHITextureHandle dstTexture,
                              EImageLayout dstLayout,
                              const FRHIBufferTextureCopyRegion& region) override;
    void CopyTextureToBuffer(FRHITextureHandle srcTexture,
                              EImageLayout srcLayout,
                              FRHIBufferHandle dstBuffer,
                              const FRHIBufferTextureCopyRegion& region) override;
    void BlitTexture(FRHITextureHandle src, EImageLayout srcLayout,
                      FRHITextureHandle dst, EImageLayout dstLayout,
                      const FRHITextureBlitRegion& region,
                      EFilter filter) override;

    void ClearColorImage(FRHITextureHandle texture,
                          EImageLayout layout,
                          const FLinearColor& color) override;

    // ====================================================================
    // 管线屏障
    // ====================================================================

    void PipelineBarrier(
        EPipelineStageFlags srcStageMask,
        EPipelineStageFlags dstStageMask,
        const FRHIMemoryBarrier* memoryBarriers,
        UInt32 memoryBarrierCount,
        const FRHIBufferMemoryBarrier* bufferBarriers,
        UInt32 bufferBarrierCount,
        const FRHIImageMemoryBarrier* imageBarriers,
        UInt32 imageBarrierCount) override;

    // ====================================================================
    // 动态状态
    // ====================================================================

    void SetViewport(const FRHIViewport& viewport) override;
    void SetScissor(const FRHIScissorRect& scissor) override;
    void SetLineWidth(Float32 lineWidth) override;
    void SetDepthBias(Float32 constantFactor, Float32 clamp,
                       Float32 slopeFactor) override;
    void SetBlendConstants(const Float32 blendConstants[4]) override;
    void SetStencilReference(UInt32 reference) override;

    // ====================================================================
    // 布局转换
    // ====================================================================

    void TransitionImageLayout(
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
        UInt32 arrayLayerCount) override;

    // ====================================================================
    // 查询
    // ====================================================================

    void BeginQuery(FRHIQueryPoolHandle queryPool,
                     UInt32 queryIndex) override;
    void EndQuery(FRHIQueryPoolHandle queryPool,
                   UInt32 queryIndex) override;
    void ResetQueryPool(FRHIQueryPoolHandle queryPool,
                         UInt32 firstQuery,
                         UInt32 queryCount) override;
    void WriteTimestamp(EPipelineStageFlags pipelineStage,
                         FRHIQueryPoolHandle queryPool,
                         UInt32 queryIndex) override;

    // ====================================================================
    // 调试标记
    // ====================================================================

    void BeginDebugLabel(const char* name,
                          Float32 r, Float32 g, Float32 b,
                          Float32 a) override;
    void EndDebugLabel() override;
    void InsertDebugLabel(const char* name,
                           Float32 r, Float32 g, Float32 b,
                           Float32 a) override;

private:
    FVulkanDevice*           m_Device        = nullptr;
    VkCommandBuffer          m_CommandBuffer = VK_NULL_HANDLE;
    FRHICommandBufferHandle  m_Handle;
};

} // namespace Limx
