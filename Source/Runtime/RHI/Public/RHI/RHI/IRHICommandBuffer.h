/*******************************************************************************
 * 文件名称：IRHICommandBuffer.h
 * 创建时间：2025-07-27
 * 创建者  ：LimxTeam
 * 设计哲学：命令录制接口与执行解耦，所有 GPU 命令通过本接口录制到命令缓冲区，
 *          由 IRHIDevice::Submit 统一提交。接口方法按功能分组：生命周期控制、
 *          渲染通道、管线绑定、资源绑定、绘制调用、计算分派、资源拷贝、
 *          管线屏障、查询、调试标记。
 * 功能描述：RHI 命令缓冲区抽象接口 — 定义 GPU 命令录制的完整契约，
 *          支持图形、计算、传输三类命令录制，覆盖从绑定管线到发出绘制调用
 *          的全部操作。
 * 技术特性：纯虚接口 (无数据成员)，命令缓冲区生命周期由 IRHIDevice 管理；
 *          Begin/End 对称调用保证录制状态正确性；
 *          渲染通道内/外命令分组明确，防止非法命令序列。
 *
 * ── 函数/方法表 ──────────────────────────────────────────────
 * │ 函数名                         │ 描述                        │
 * │───────────────────────────────│───────────────────────────│
 * │ Begin()                       │ 开始命令录制                  │
 * │ End()                         │ 结束命令录制                  │
 * │ Reset()                       │ 重置命令缓冲区                │
 * │ BeginRenderPass()             │ 开始渲染通道                  │
 * │ EndRenderPass()               │ 结束渲染通道                  │
 * │ NextSubpass()                 │ 进入下一子通道                │
 * │ BindGraphicsPipeline()        │ 绑定图形管线                  │
 * │ BindComputePipeline()         │ 绑定计算管线                  │
 * │ BindVertexBuffer()            │ 绑定顶点缓冲区                │
 * │ BindIndexBuffer()             │ 绑定索引缓冲区                │
 * │ BindDescriptorSet()           │ 绑定描述符集                  │
 * │ PushConstants()               │ 推送常量数据                  │
 * │ SetViewport()                 │ 设置视口                     │
 * │ SetScissor()                  │ 设置裁剪矩形                  │
 * │ Draw()                        │ 非索引绘制                   │
 * │ DrawIndexed()                 │ 索引绘制                     │
 * │ DrawIndirect()                │ 间接绘制                     │
 * │ DrawIndexedIndirect()         │ 间接索引绘制                  │
 * │ Dispatch()                    │ 计算着色器分派                │
 * │ DispatchIndirect()            │ 间接计算分派                  │
 * │ CopyBuffer()                  │ 缓冲区到缓冲区拷贝            │
 * │ CopyBufferToTexture()         │ 缓冲区到纹理拷贝              │
 * │ CopyTextureToBuffer()         │ 纹理到缓冲区拷贝              │
 * │ BlitTexture()                 │ 纹理缩放拷贝 (带过滤)          │
 * │ PipelineBarrier()             │ 管线屏障 (内存/执行同步)        │
 * │ TransitionImageLayout()       │ 图像布局转换                  │
 * │ BeginQuery()                  │ 开始查询                     │
 * │ EndQuery()                    │ 结束查询                     │
 * │ ResetQueryPool()              │ 重置查询池                   │
 * │ WriteTimestamp()              │ 写入时间戳                   │
 * │ BeginDebugLabel()             │ 开始调试标记区域              │
 * │ EndDebugLabel()               │ 结束调试标记区域              │
 * │ InsertDebugLabel()            │ 插入调试标记点                │
 *
 * ── 更新历史 ────────────────────────────────────────────────
 * │ 日期         │ 作者       │ 描述                        │
 * │─────────────│──────────│───────────────────────────│
 * │ 2025-07-27  │ LimxTeam  │ 初始创建                     │
 * ============================================================
 ******************************************************************************/

#pragma once

#include "RHI/RHI/IRHIDevice.h"

namespace Limx
{

// ============================================================================
// FRHIRenderPassBeginInfo — 渲染通道开始信息
// ============================================================================

struct FRHIRenderPassBeginInfo
{
    // 渲染通道句柄
    FRHIRenderPassHandle RenderPass;

    // 帧缓冲句柄
    FRHIFramebufferHandle Framebuffer;

    // 渲染区域
    FRHIOffset2D RenderAreaOffset = { 0, 0 };
    FRHIExtent2D RenderAreaExtent = { 0, 0 };

    // 清除值数组 (每个附件一个，与 RenderPass 中附件顺序对应)
    const FRHIClearColorValue*        ClearColors      = nullptr;
    UInt32                            ClearColorCount   = 0;
    const FRHIClearDepthStencilValue* ClearDepthStencil = nullptr;

    /// 本通道的内容是否来自次级命令缓冲区
    ///
    /// 为 true 时通道内**只能**执行次级缓冲区, 不能直接录制绘制命令 ——
    /// 这是 Vulkan 的硬性规定 (VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS)。
    /// 两者混用会被验证层直接拦下。
    bool UseSecondaryCommandBuffers = false;
};

// ============================================================================
// FRHIBufferCopyRegion — 缓冲区拷贝区域
// ============================================================================

struct FRHIBufferCopyRegion
{
    UInt64 SrcOffset = 0;
    UInt64 DstOffset = 0;
    UInt64 Size      = 0;
};

// ============================================================================
// FRHIBufferTextureCopyRegion — 缓冲区与纹理拷贝区域
// ============================================================================

struct FRHIBufferTextureCopyRegion
{
    // 缓冲区偏移 (字节)
    UInt64 BufferOffset = 0;

    // 缓冲区行长度 (像素，0 表示紧密排列)
    UInt32 BufferRowLength = 0;

    // 缓冲区图像高度 (行数，0 表示紧密排列)
    UInt32 BufferImageHeight = 0;

    // 纹理子资源
    UInt32 MipLevel    = 0;
    UInt32 BaseLayer   = 0;
    UInt32 LayerCount  = 1;

    // 纹理偏移
    FRHIOffset3D TextureOffset = { 0, 0, 0 };

    // 拷贝尺寸
    FRHIExtent3D TextureExtent = { 0, 0, 0 };
};

// ============================================================================
// FRHITextureBlitRegion — 纹理缩放拷贝区域
// ============================================================================

struct FRHITextureBlitRegion
{
    // 源子资源
    UInt32 SrcMipLevel   = 0;
    UInt32 SrcBaseLayer  = 0;
    UInt32 SrcLayerCount = 1;
    FRHIOffset3D SrcOffsetMin = { 0, 0, 0 };
    FRHIOffset3D SrcOffsetMax = { 0, 0, 0 };

    // 目标子资源
    UInt32 DstMipLevel   = 0;
    UInt32 DstBaseLayer  = 0;
    UInt32 DstLayerCount = 1;
    FRHIOffset3D DstOffsetMin = { 0, 0, 0 };
    FRHIOffset3D DstOffsetMax = { 0, 0, 0 };
};

// ============================================================================
// FRHIMemoryBarrier — 全局内存屏障
// ============================================================================

struct FRHIMemoryBarrier
{
    EAccessFlags SrcAccessMask = EAccessFlags::None;
    EAccessFlags DstAccessMask = EAccessFlags::None;
};

// ============================================================================
// FRHIImageMemoryBarrier — 图像内存屏障 (含布局转换)
// ============================================================================

struct FRHIImageMemoryBarrier
{
    EAccessFlags SrcAccessMask = EAccessFlags::None;
    EAccessFlags DstAccessMask = EAccessFlags::None;

    EImageLayout OldLayout = EImageLayout::Undefined;
    EImageLayout NewLayout = EImageLayout::General;

    FRHITextureHandle Texture;

    // 子资源范围
    UInt32 BaseMipLevel   = 0;
    UInt32 MipLevelCount  = 1;
    UInt32 BaseArrayLayer = 0;
    UInt32 ArrayLayerCount = 1;
};

// ============================================================================
// FRHIBufferMemoryBarrier — 缓冲区内存屏障
// ============================================================================

struct FRHIBufferMemoryBarrier
{
    EAccessFlags SrcAccessMask = EAccessFlags::None;
    EAccessFlags DstAccessMask = EAccessFlags::None;

    FRHIBufferHandle Buffer;

    UInt64 Offset = 0;
    UInt64 Size   = 0xFFFFFFFFFFFFFFFFULL;
};

// ============================================================================
// IRHICommandBuffer — 命令缓冲区抽象接口
// ============================================================================

class IRHICommandBuffer
{
public:
    virtual ~IRHICommandBuffer() = default;

    // ====================================================================
    // 生命周期
    // ====================================================================

    // 开始命令录制
    virtual ERHIResult Begin() = 0;

    /// 开始录制次级命令缓冲区
    ///
    /// 与 Begin() 的区别在于必须带继承信息, 且会置上
    /// RENDER_PASS_CONTINUE 标志 —— 表示"我在一个已经开始的通道里录制"。
    virtual ERHIResult BeginSecondary(
        const FRHICommandBufferInheritance& inheritance) = 0;

    // 结束命令录制
    virtual ERHIResult End() = 0;

    // 重置命令缓冲区 (清除已录制命令)
    virtual ERHIResult Reset() = 0;

    // ====================================================================
    // 渲染通道
    // ====================================================================

    virtual void BeginRenderPass(const FRHIRenderPassBeginInfo& beginInfo) = 0;
    virtual void EndRenderPass() = 0;

    /// 执行若干次级命令缓冲区
    ///
    /// 只能在主缓冲区上调用, 且当前通道必须是以
    /// UseSecondaryCommandBuffers = true 开始的。
    ///
    /// 执行顺序即数组顺序 —— 这一点对结果的确定性至关重要: 并行录制时
    /// 各线程的完成先后是随机的, 但只要按固定顺序执行, 输出就与单线程
    /// 逐像素相同。若按完成顺序执行, 半透明排序与深度相等的表面会得到
    /// 不确定的结果。
    virtual void ExecuteCommands(const FRHICommandBufferHandle* buffers,
                                  UInt32 count) = 0;
    virtual void NextSubpass() = 0;

    // ====================================================================
    // 管线绑定
    // ====================================================================

    virtual void BindGraphicsPipeline(FRHIGraphicsPipelineHandle pipeline) = 0;
    virtual void BindComputePipeline(FRHIComputePipelineHandle pipeline) = 0;

    // ====================================================================
    // 资源绑定
    // ====================================================================

    virtual void BindVertexBuffer(UInt32 binding,
                                   FRHIBufferHandle buffer,
                                   UInt64 offset = 0) = 0;

    virtual void BindIndexBuffer(FRHIBufferHandle buffer,
                                  UInt64 offset,
                                  EIndexType indexType) = 0;

    virtual void BindDescriptorSet(EPipelineBindPoint bindPoint,
                                    FRHIPipelineLayoutHandle layout,
                                    UInt32 setIndex,
                                    FRHIDescriptorSetHandle descriptorSet,
                                    const UInt32* dynamicOffsets = nullptr,
                                    UInt32 dynamicOffsetCount = 0) = 0;

    virtual void PushConstants(FRHIPipelineLayoutHandle layout,
                                EShaderStage stageFlags,
                                UInt32 offset,
                                UInt32 size,
                                const void* data) = 0;

    // ====================================================================
    // 动态状态
    // ====================================================================

    virtual void SetViewport(const FRHIViewport& viewport) = 0;
    virtual void SetScissor(const FRHIScissorRect& scissor) = 0;
    virtual void SetLineWidth(Float32 lineWidth) = 0;
    virtual void SetDepthBias(Float32 constantFactor,
                               Float32 clamp,
                               Float32 slopeFactor) = 0;
    virtual void SetBlendConstants(const Float32 blendConstants[4]) = 0;
    virtual void SetStencilReference(UInt32 reference) = 0;

    // ====================================================================
    // 绘制命令
    // ====================================================================

    virtual void Draw(UInt32 vertexCount,
                       UInt32 instanceCount = 1,
                       UInt32 firstVertex = 0,
                       UInt32 firstInstance = 0) = 0;

    virtual void DrawIndexed(UInt32 indexCount,
                              UInt32 instanceCount = 1,
                              UInt32 firstIndex = 0,
                              Int32  vertexOffset = 0,
                              UInt32 firstInstance = 0) = 0;

    virtual void DrawIndirect(FRHIBufferHandle buffer,
                               UInt64 offset,
                               UInt32 drawCount,
                               UInt32 stride) = 0;

    virtual void DrawIndexedIndirect(FRHIBufferHandle buffer,
                                      UInt64 offset,
                                      UInt32 drawCount,
                                      UInt32 stride) = 0;

    // ====================================================================
    // 计算分派
    // ====================================================================

    // ------------------------------------------------------------------
    // 加速结构构建
    //
    // 录进命令流, 在 GPU 上执行。构建之后、着色器读取之前必须插一道
    // 屏障 (AccelerationStructureWrite -> AccelerationStructureRead)。
    // ------------------------------------------------------------------

    /// 构建 (或重建) 一个加速结构
    ///
    /// instanceCount 只对 TLAS 有意义: 本次构建实际使用多少个实例。
    /// 对 BLAS 传 0。
    virtual void BuildAccelStruct(FRHIAccelStructHandle handle,
                                  UInt32 instanceCount) = 0;

    /// 加速结构写入 -> 着色器读取 的屏障
    virtual void AccelStructBarrier() = 0;

    virtual void Dispatch(UInt32 groupCountX,
                           UInt32 groupCountY,
                           UInt32 groupCountZ) = 0;

    virtual void DispatchIndirect(FRHIBufferHandle buffer,
                                   UInt64 offset) = 0;

    // ====================================================================
    // 资源拷贝
    // ====================================================================

    virtual void CopyBuffer(FRHIBufferHandle src,
                             FRHIBufferHandle dst,
                             const FRHIBufferCopyRegion& region) = 0;

    virtual void CopyBufferToTexture(FRHIBufferHandle srcBuffer,
                                      FRHITextureHandle dstTexture,
                                      EImageLayout dstLayout,
                                      const FRHIBufferTextureCopyRegion& region) = 0;

    virtual void CopyTextureToBuffer(FRHITextureHandle srcTexture,
                                      EImageLayout srcLayout,
                                      FRHIBufferHandle dstBuffer,
                                      const FRHIBufferTextureCopyRegion& region) = 0;

    virtual void BlitTexture(FRHITextureHandle src,
                              EImageLayout srcLayout,
                              FRHITextureHandle dst,
                              EImageLayout dstLayout,
                              const FRHITextureBlitRegion& region,
                              EFilter filter) = 0;

    // ====================================================================
    // 清除操作
    // ====================================================================

    /// 清除颜色图像 — 在渲染通道外部直接清除纹理
    /// @param texture   目标纹理 (必须处于 TransferDstOptimal 或 General 布局)
    /// @param layout    当前图像布局
    /// @param color     清除颜色 (线性空间 RGBA)
    virtual void ClearColorImage(
        FRHITextureHandle texture,
        EImageLayout layout,
        const FLinearColor& color) = 0;

    // ====================================================================
    // 管线屏障与布局转换
    // ====================================================================

    virtual void PipelineBarrier(
        EPipelineStageFlags srcStageMask,
        EPipelineStageFlags dstStageMask,
        const FRHIMemoryBarrier* memoryBarriers,
        UInt32 memoryBarrierCount,
        const FRHIBufferMemoryBarrier* bufferBarriers,
        UInt32 bufferBarrierCount,
        const FRHIImageMemoryBarrier* imageBarriers,
        UInt32 imageBarrierCount) = 0;

    // 便捷方法: 单个图像布局转换
    virtual void TransitionImageLayout(
        FRHITextureHandle texture,
        EImageLayout oldLayout,
        EImageLayout newLayout,
        EPipelineStageFlags srcStage,
        EPipelineStageFlags dstStage,
        EAccessFlags srcAccess,
        EAccessFlags dstAccess,
        UInt32 baseMipLevel = 0,
        UInt32 mipLevelCount = 1,
        UInt32 baseArrayLayer = 0,
        UInt32 arrayLayerCount = 1) = 0;

    // ====================================================================
    // 查询
    // ====================================================================

    virtual void BeginQuery(FRHIQueryPoolHandle queryPool, UInt32 query) = 0;
    virtual void EndQuery(FRHIQueryPoolHandle queryPool, UInt32 query) = 0;
    virtual void ResetQueryPool(FRHIQueryPoolHandle queryPool,
                                 UInt32 firstQuery,
                                 UInt32 queryCount) = 0;
    virtual void WriteTimestamp(EPipelineStageFlags pipelineStage,
                                 FRHIQueryPoolHandle queryPool,
                                 UInt32 query) = 0;

    // ====================================================================
    // 调试标记 (在 RenderDoc/NSight 等工具中可见)
    // ====================================================================

    virtual void BeginDebugLabel(const char* name,
                                  Float32 r = 1.0f,
                                  Float32 g = 1.0f,
                                  Float32 b = 1.0f,
                                  Float32 a = 1.0f) = 0;
    virtual void EndDebugLabel() = 0;
    virtual void InsertDebugLabel(const char* name,
                                   Float32 r = 1.0f,
                                   Float32 g = 1.0f,
                                   Float32 b = 1.0f,
                                   Float32 a = 1.0f) = 0;

protected:
    IRHICommandBuffer() = default;
    LIMX_NON_COPYABLE(IRHICommandBuffer);
    LIMX_NON_MOVABLE(IRHICommandBuffer);
};

} // namespace Limx
