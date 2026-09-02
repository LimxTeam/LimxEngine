/*******************************************************************************
 * 文件: FRenderResourceManager.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   GPU 资源管理器实现 — 缓冲区与纹理上传、采样器共享、代际句柄、
 *   引用计数与延迟回收
 *
 * 设计哲学:
 *   暂存缓冲区必须在提交完成后才销毁 — 拷贝命令是异步录制、同步提交的，
 *   若在 EndSingleTimeCommands 之前释放暂存缓冲区，GPU 读到的是已回收的显存。
 *   实现中严格保持"创建暂存 → 录制 → 提交并等待 → 销毁暂存"的顺序。
 *
 *   销毁必须延迟到 GPU 用完 — 引用计数归零只说明 CPU 侧无人再引用它，
 *   而上一帧提交的命令缓冲区可能仍在读这块显存。退役时把 GPU 对象移出槽位
 *   放进待销毁队列, 等 MaxFramesInFlight 帧后再真正销毁。
 *
 *   索引宽度在上传时收窄 — CPU 侧统一用 UInt32 表达索引以简化解析，
 *   但顶点数不超过 65535 时上传为 UInt16。这一步转换放在上传处而非解析处，
 *   因为只有到了这里才同时知道顶点数与索引数组。
 *
 * 技术特性:
 *   - 上传走 CpuToGpu 暂存缓冲区 + 设备本地目标, 两次布局转换夹住拷贝
 *   - 采样器按配置线性查重, 配置种类极少, 无需哈希表
 *   - 卸载时递增代际号, 使一切旧句柄立即失效
 *
 * 依赖关系:
 *   内部: RenderCore/Resources/FRenderResourceManager.h
 *
 * 注意事项:
 *   上传为同步操作 — 每次调用都会提交并等待 GPU 完成
 *
 ******************************************************************************/

#include "RenderCore/Resources/FRenderResourceManager.h"

#include "RenderCore/Geometry/FMeshletBuilder.h"
#include "RenderCore/Renderer/FRenderContext.h"

namespace Limx
{

LIMX_DECLARE_LOG_CATEGORY(LogRenderCore)
LIMX_DEFINE_LOG_CATEGORY(LogRenderCore)

namespace
{

/// 顶点数不超过该值时可用 16 位索引
constexpr UInt32 kMaxUInt16Index = 65535;

/// 取下一级 mip 的尺寸 —— 不小于 1
LIMX_NODISCARD Int32 NextMipExtent(Int32 extent)
{
    return (extent > 1) ? (extent / 2) : 1;
}

} // namespace

// ============================================================================
// 生命周期
// ============================================================================

FRenderResourceManager::~FRenderResourceManager()
{
    Shutdown();
}

ERHIResult FRenderResourceManager::Initialize(IRHIDevice* device,
                                              FRenderContext* context)
{
    if (device == nullptr || context == nullptr)
    {
        return ERHIResult::ErrorInvalidParameter;
    }

    m_Device  = device;
    m_Context = context;

    LIMX_LOG(LogRenderCore, Log, "[资源管理器] 初始化完成");

    return ERHIResult::Success;
}

void FRenderResourceManager::Shutdown()
{
    if (m_Device == nullptr)
    {
        return;
    }

    LogStats("关闭前");

    for (SizeType i = 0; i < m_Meshes.GetSize(); ++i)
    {
        if (m_Meshes[i].IsActive)
        {
            RetireMeshSlot(m_Meshes[i]);
        }
    }

    for (SizeType i = 0; i < m_Textures.GetSize(); ++i)
    {
        if (m_Textures[i].IsActive)
        {
            RetireTextureSlot(m_Textures[i]);
        }
    }

    // 调用方保证此时 GPU 已空闲 (FRenderContext::Shutdown 先做了 WaitIdle),
    // 因此可以无条件冲刷整个队列。
    FlushPendingReleases();

    for (SizeType i = 0; i < m_Samplers.GetSize(); ++i)
    {
        if (m_Samplers[i].IsValid())
        {
            m_Device->DestroySampler(m_Samplers[i]);
        }
    }

    m_Meshes.Clear();
    m_FreeMeshSlots.Clear();
    m_Textures.Clear();
    m_FreeTextureSlots.Clear();
    m_PendingReleases.Clear();
    m_SamplerKeys.Clear();
    m_Samplers.Clear();

    m_Device  = nullptr;
    m_Context = nullptr;
}

// ============================================================================
// 上传辅助
// ============================================================================

ERHIResult FRenderResourceManager::UploadBuffer(const void* data,
                                                UInt64 byteCount,
                                                EBufferUsage usage,
                                                FRHIBufferHandle& outBuffer)
{
    if (data == nullptr || byteCount == 0)
    {
        return ERHIResult::ErrorInvalidParameter;
    }

    // ---- 暂存缓冲区 (主机可见) ----
    FRHIBufferDesc stagingDesc = {};
    stagingDesc.Size        = byteCount;
    stagingDesc.Usage       = EBufferUsage::TransferSrc;
    stagingDesc.MemoryUsage = EMemoryUsage::CpuToGpu;

    FRHIBufferHandle staging;
    ERHIResult result = m_Device->CreateBuffer(stagingDesc, staging);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    void* mapped = nullptr;
    result = m_Device->MapBuffer(staging, &mapped);

    if (!IsRHISuccess(result) || mapped == nullptr)
    {
        m_Device->DestroyBuffer(staging);
        return ERHIResult::ErrorUnknown;
    }

    Memory::MemCopy(mapped, data, static_cast<SizeType>(byteCount));
    m_Device->UnmapBuffer(staging);

    // ---- 目标缓冲区 (设备本地) ----
    FRHIBufferDesc destDesc = {};
    destDesc.Size        = byteCount;
    destDesc.Usage       = static_cast<EBufferUsage>(
        static_cast<UInt32>(usage) |
        static_cast<UInt32>(EBufferUsage::TransferDst));
    destDesc.MemoryUsage = EMemoryUsage::GpuOnly;

    result = m_Device->CreateBuffer(destDesc, outBuffer);

    if (!IsRHISuccess(result))
    {
        m_Device->DestroyBuffer(staging);
        return result;
    }

    // ---- 拷贝 ----
    //
    // 暂存缓冲区必须活到提交完成之后 —— 拷贝命令由 GPU 异步执行,
    // 提前释放会让 GPU 读到已回收的显存。
    IRHICommandBuffer* commandBuffer = m_Context->BeginSingleTimeCommands();

    if (commandBuffer == nullptr)
    {
        m_Device->DestroyBuffer(staging);
        m_Device->DestroyBuffer(outBuffer);
        return ERHIResult::ErrorUnknown;
    }

    FRHIBufferCopyRegion region = {};
    region.SrcOffset = 0;
    region.DstOffset = 0;
    region.Size      = byteCount;

    commandBuffer->CopyBuffer(staging, outBuffer, region);

    m_Context->EndSingleTimeCommands(commandBuffer);

    m_Device->DestroyBuffer(staging);

    return ERHIResult::Success;
}

ERHIResult FRenderResourceManager::UploadTexture2D(const void* pixels,
                                                   UInt64 byteCount,
                                                   UInt32 width, UInt32 height,
                                                   EPixelFormat format,
                                                   UInt32& outMipLevels,
                                                   FRHITextureHandle& outTexture)
{
    if (pixels == nullptr || byteCount == 0 || width == 0 || height == 0)
    {
        return ERHIResult::ErrorInvalidParameter;
    }

    // ---- mip 层数取决于格式能力, 而非一厢情愿 ----
    //
    // 逐级 blit 要求源格式同时支持 BlitSrc 与线性过滤。缺任一项就退回单层,
    // 并留下日志 —— 静默降级会让"某些机器上远处纹理闪烁"变成无从追查的问题。
    const EFormatFeature features = m_Device->GetFormatFeatures(format);

    const bool canGenerateMips = HasFormatFeature(
        features, EFormatFeature::BlitSrc | EFormatFeature::BlitDst |
                  EFormatFeature::SampledImageLinear);

    const UInt32 mipLevels =
        canGenerateMips ? ComputeMipLevelCount(width, height) : 1;

    outMipLevels = mipLevels;

    if (!canGenerateMips && (width > 1 || height > 1))
    {
        LIMX_LOG(LogRenderCore, Warning,
                 "[资源管理器] 格式 {} 不支持逐级 blit, {}x{} 纹理退回单层 mip",
                 static_cast<UInt32>(format), width, height);
    }

    // ---- 暂存缓冲区 ----
    FRHIBufferDesc stagingDesc = {};
    stagingDesc.Size        = byteCount;
    stagingDesc.Usage       = EBufferUsage::TransferSrc;
    stagingDesc.MemoryUsage = EMemoryUsage::CpuToGpu;

    FRHIBufferHandle staging;
    ERHIResult result = m_Device->CreateBuffer(stagingDesc, staging);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    void* mapped = nullptr;
    result = m_Device->MapBuffer(staging, &mapped);

    if (!IsRHISuccess(result) || mapped == nullptr)
    {
        m_Device->DestroyBuffer(staging);
        return ERHIResult::ErrorUnknown;
    }

    Memory::MemCopy(mapped, pixels, static_cast<SizeType>(byteCount));
    m_Device->UnmapBuffer(staging);

    // ---- 目标纹理 ----
    FRHITextureDesc textureDesc = {};
    textureDesc.Type          = ETextureType::Texture2D;
    textureDesc.Format        = format;
    textureDesc.Extent.Width  = width;
    textureDesc.Extent.Height = height;
    textureDesc.Extent.Depth  = 1;
    textureDesc.MipLevels     = mipLevels;
    textureDesc.ArrayLayers   = 1;
    // TransferSrc 是必需的 —— 生成第 N 级要把第 N-1 级当作 blit 源读回来
    textureDesc.Usage         = static_cast<ETextureUsage>(
        static_cast<UInt32>(ETextureUsage::Sampled) |
        static_cast<UInt32>(ETextureUsage::TransferDst) |
        static_cast<UInt32>(ETextureUsage::TransferSrc));
    textureDesc.MemoryUsage   = EMemoryUsage::GpuOnly;

    result = m_Device->CreateTexture(textureDesc, outTexture);

    if (!IsRHISuccess(result))
    {
        m_Device->DestroyBuffer(staging);
        return result;
    }

    // ---- 上传并转换布局 ----
    IRHICommandBuffer* commandBuffer = m_Context->BeginSingleTimeCommands();

    if (commandBuffer == nullptr)
    {
        m_Device->DestroyBuffer(staging);
        m_Device->DestroyTexture(outTexture);
        return ERHIResult::ErrorUnknown;
    }

    // 全部 mip 层从 Undefined 转为传输目标 —— 新建图像每一层都是 Undefined
    commandBuffer->TransitionImageLayout(
        outTexture,
        EImageLayout::Undefined,
        EImageLayout::TransferDst,
        EPipelineStageFlags::TopOfPipe,
        EPipelineStageFlags::Transfer,
        EAccessFlags::None,
        EAccessFlags::TransferWrite,
        0, mipLevels);

    FRHIBufferTextureCopyRegion region = {};
    region.BufferOffset      = 0;
    region.BufferRowLength   = 0;   // 0 表示紧密排布
    region.BufferImageHeight = 0;
    region.MipLevel          = 0;
    region.BaseLayer         = 0;
    region.LayerCount        = 1;
    region.TextureOffset     = { 0, 0, 0 };
    region.TextureExtent     = { width, height, 1 };

    commandBuffer->CopyBufferToTexture(staging, outTexture,
                                       EImageLayout::TransferDst,
                                       region);

    // ---- 逐级降采样生成 mip 链 ----
    //
    // 每一级都要等上一级写完才能读: 先把第 i-1 级转成 blit 源 (这次转换
    // 同时充当写后读的屏障), blit 到第 i 级, 再把第 i-1 级转成着色器只读。
    // 逐级转换而非整体转换, 是因为同一时刻不同 mip 处于不同布局。
    Int32 mipWidth  = static_cast<Int32>(width);
    Int32 mipHeight = static_cast<Int32>(height);

    for (UInt32 level = 1; level < mipLevels; ++level)
    {
        commandBuffer->TransitionImageLayout(
            outTexture,
            EImageLayout::TransferDst,
            EImageLayout::TransferSrc,
            EPipelineStageFlags::Transfer,
            EPipelineStageFlags::Transfer,
            EAccessFlags::TransferWrite,
            EAccessFlags::TransferRead,
            level - 1, 1);

        const Int32 nextWidth  = NextMipExtent(mipWidth);
        const Int32 nextHeight = NextMipExtent(mipHeight);

        FRHITextureBlitRegion blit = {};
        blit.SrcMipLevel   = level - 1;
        blit.SrcBaseLayer  = 0;
        blit.SrcLayerCount = 1;
        blit.SrcOffsetMin  = { 0, 0, 0 };
        blit.SrcOffsetMax  = { mipWidth, mipHeight, 1 };
        blit.DstMipLevel   = level;
        blit.DstBaseLayer  = 0;
        blit.DstLayerCount = 1;
        blit.DstOffsetMin  = { 0, 0, 0 };
        blit.DstOffsetMax  = { nextWidth, nextHeight, 1 };

        commandBuffer->BlitTexture(outTexture, EImageLayout::TransferSrc,
                                   outTexture, EImageLayout::TransferDst,
                                   blit, EFilter::Linear);

        // 上一级已经用完, 转为着色器只读
        commandBuffer->TransitionImageLayout(
            outTexture,
            EImageLayout::TransferSrc,
            EImageLayout::ShaderReadOnly,
            EPipelineStageFlags::Transfer,
            EPipelineStageFlags::FragmentShader,
            EAccessFlags::TransferRead,
            EAccessFlags::ShaderRead,
            level - 1, 1);

        mipWidth  = nextWidth;
        mipHeight = nextHeight;
    }

    // 最后一级从未作为 blit 源, 仍停在 TransferDst
    commandBuffer->TransitionImageLayout(
        outTexture,
        EImageLayout::TransferDst,
        EImageLayout::ShaderReadOnly,
        EPipelineStageFlags::Transfer,
        EPipelineStageFlags::FragmentShader,
        EAccessFlags::TransferWrite,
        EAccessFlags::ShaderRead,
        mipLevels - 1, 1);

    m_Context->EndSingleTimeCommands(commandBuffer);

    m_Device->DestroyBuffer(staging);

    return ERHIResult::Success;
}

// ============================================================================
// 块压缩纹理的逐 mip 上传
// ============================================================================

ERHIResult FRenderResourceManager::UploadCompressedTexture2D(
    const FCompressedImageData& image,
    EPixelFormat format,
    FRHITextureHandle& outTexture)
{
    if (!image.IsValid() || format == EPixelFormat::Unknown)
    {
        return ERHIResult::ErrorInvalidParameter;
    }

    const UInt32 mipLevels = image.GetMipLevelCount();

    // ---- 格式能力 ----
    //
    // 块压缩格式的采样能力由 textureCompressionBC 特性提供, 桌面 GPU 上
    // 普遍支持但不是规范保证。缺了就必须失败而不是退回未压缩 ——
    // 这里没有像素可退, 我们手上只有块数据。
    const EFormatFeature features = m_Device->GetFormatFeatures(format);

    if (!HasFormatFeature(features, EFormatFeature::SampledImage))
    {
        LIMX_LOG(LogRenderCore, Error,
                 "[资源管理器] 设备不支持采样格式 {} ({}) — "
                 "块压缩纹理无法上传, 也无法退回未压缩: 手上只有块数据",
                 static_cast<UInt32>(format),
                 GetBlockCompressionFormatName(image.Format));
        return ERHIResult::ErrorInvalidParameter;
    }

    const UInt64 payloadBytes = static_cast<UInt64>(image.Data.GetSize());

    // ---- 暂存缓冲区 ----
    //
    // 整条 mip 链一次性搬进暂存区。分层建缓冲区会多出 N 次分配与 N 次
    // 映射, 而 mip 链本来就是一段连续内存。
    FRHIBufferDesc stagingDesc = {};
    stagingDesc.Size        = payloadBytes;
    stagingDesc.Usage       = EBufferUsage::TransferSrc;
    stagingDesc.MemoryUsage = EMemoryUsage::CpuToGpu;

    FRHIBufferHandle staging;
    ERHIResult result = m_Device->CreateBuffer(stagingDesc, staging);

    if (!IsRHISuccess(result))
    {
        return result;
    }

    void* mapped = nullptr;
    result = m_Device->MapBuffer(staging, &mapped);

    if (!IsRHISuccess(result) || mapped == nullptr)
    {
        m_Device->DestroyBuffer(staging);
        return ERHIResult::ErrorUnknown;
    }

    Memory::MemCopy(mapped, image.Data.GetData(),
                    static_cast<SizeType>(payloadBytes));
    m_Device->UnmapBuffer(staging);

    // ---- 目标纹理 ----
    FRHITextureDesc textureDesc = {};
    textureDesc.Type          = ETextureType::Texture2D;
    textureDesc.Format        = format;
    textureDesc.Extent.Width  = image.Width;
    textureDesc.Extent.Height = image.Height;
    textureDesc.Extent.Depth  = 1;
    textureDesc.MipLevels     = mipLevels;
    textureDesc.ArrayLayers   = 1;
    // 不要 TransferSrc —— 这条路径永远不把纹理当 blit 源读回来。
    // 写上它只会让"以后有人加一句 blit"看起来是合法的, 而对压缩格式
    // 那是一条非法调用。
    textureDesc.Usage         = static_cast<ETextureUsage>(
        static_cast<UInt32>(ETextureUsage::Sampled) |
        static_cast<UInt32>(ETextureUsage::TransferDst));
    textureDesc.MemoryUsage   = EMemoryUsage::GpuOnly;

    result = m_Device->CreateTexture(textureDesc, outTexture);

    if (!IsRHISuccess(result))
    {
        m_Device->DestroyBuffer(staging);
        return result;
    }

    // ---- 逐层拷贝 ----
    IRHICommandBuffer* commandBuffer = m_Context->BeginSingleTimeCommands();

    if (commandBuffer == nullptr)
    {
        m_Device->DestroyBuffer(staging);
        m_Device->DestroyTexture(outTexture);
        return ERHIResult::ErrorUnknown;
    }

    commandBuffer->TransitionImageLayout(
        outTexture,
        EImageLayout::Undefined,
        EImageLayout::TransferDst,
        EPipelineStageFlags::TopOfPipe,
        EPipelineStageFlags::Transfer,
        EAccessFlags::None,
        EAccessFlags::TransferWrite,
        0, mipLevels);

    // bufferOffset 的对齐要求 (块字节数的整数倍, 且是 4 的整数倍) 在这里
    // 是**由构造保证**的, 不是靠运行时检查: 上面的 image.IsValid() 已经
    // 核对过每一层的 ByteOffset 是前面各层 ByteSize 的前缀和, 而每个
    // ByteSize 都是 块数 × 每块字节数 (8 或 16), 两者都是 4 的倍数。
    // 在这里再加一道判断只会得到一条永远不会失败的检查 —— 真正把这件事
    // 钉住的是 DdsDecoder 用例里对 ByteOffset % 8 / % 16 的断言。
    for (UInt32 level = 0; level < mipLevels; ++level)
    {
        const FCompressedMipLevel& entry =
            image.Levels[static_cast<SizeType>(level)];

        FRHIBufferTextureCopyRegion region = {};
        region.BufferOffset = static_cast<UInt64>(entry.ByteOffset);

        // 对压缩格式而言这两个字段的单位是**像素**而非块。填 0 让驱动
        // 按"该 mip 尺寸对应的紧凑排列"自己算, 是唯一不会算错的填法 ——
        // 手工填时最常见的错误正是填成块数, 结果每一行都错位四倍。
        region.BufferRowLength   = 0;
        region.BufferImageHeight = 0;

        region.MipLevel      = level;
        region.BaseLayer     = 0;
        region.LayerCount    = 1;
        region.TextureOffset = { 0, 0, 0 };
        region.TextureExtent = { entry.Width, entry.Height, 1 };

        commandBuffer->CopyBufferToTexture(staging, outTexture,
                                           EImageLayout::TransferDst,
                                           region);
    }

    // 全部层一次性转为着色器只读 —— 与未压缩路径不同, 这里不存在
    // "第 i 层要读第 i-1 层"的依赖, 因此不需要逐级转换。
    commandBuffer->TransitionImageLayout(
        outTexture,
        EImageLayout::TransferDst,
        EImageLayout::ShaderReadOnly,
        EPipelineStageFlags::Transfer,
        EPipelineStageFlags::FragmentShader,
        EAccessFlags::TransferWrite,
        EAccessFlags::ShaderRead,
        0, mipLevels);

    m_Context->EndSingleTimeCommands(commandBuffer);

    m_Device->DestroyBuffer(staging);

    return ERHIResult::Success;
}

FRHISamplerHandle FRenderResourceManager::AcquireSampler(const FSamplerKey& key)
{
    // 采样器的配置种类极少 (寻址模式 × 各向异性 × mip 层数),
    // 线性查重比引入哈希表更划算
    for (SizeType i = 0; i < m_SamplerKeys.GetSize(); ++i)
    {
        if (m_SamplerKeys[i] == key)
        {
            return m_Samplers[i];
        }
    }

    FRHISamplerDesc desc = {};
    desc.MinFilter    = EFilter::Linear;
    desc.MagFilter    = EFilter::Linear;
    desc.MipmapMode   = ESamplerMipmapMode::Linear;
    desc.AddressModeU = key.AddressMode;
    desc.AddressModeV = key.AddressMode;
    desc.AddressModeW = key.AddressMode;

    // MaxLod 必须覆盖实际的 mip 层数。缺省值 1000 虽然也能工作, 但把它写实
    // 能让"纹理只有 1 层 mip 却按多层采样"这类不一致在创建时就暴露。
    desc.MinLod = 0.0f;
    desc.MaxLod = static_cast<Float32>(key.MipLevels);

    // 各向异性此前被无条件启用 —— FSamplerKey::UseAnisotropy 参与了查重键,
    // 却从未进入描述符, 于是 FTextureUploadOptions::UseAnisotropy=false
    // 得到的仍是开启各向异性的采样器。
    desc.IsAnisotropyEnabled = key.UseAnisotropy;

    // 上限取设备实际能力 —— 写死 16 在只支持 8 的设备上会被拒绝
    const Float32 deviceMaxAnisotropy = m_Device->GetMaxAnisotropy();
    desc.MaxAnisotropy = (deviceMaxAnisotropy < 16.0f) ? deviceMaxAnisotropy
                                                       : 16.0f;

    FRHISamplerHandle sampler;

    if (!IsRHISuccess(m_Device->CreateSampler(desc, sampler)))
    {
        return FRHISamplerHandle();
    }

    m_SamplerKeys.Add(key);
    m_Samplers.Add(sampler);

    return sampler;
}

EPixelFormat FRenderResourceManager::MapPixelFormat(EImageFormat format,
                                                    bool isSrgb)
{
    switch (format)
    {
        case EImageFormat::R8:
            return EPixelFormat::R8_UNORM;

        case EImageFormat::RG8:
            return EPixelFormat::RG8_UNORM;

        case EImageFormat::RGBA8:
            // sRGB 与线性是同一份字节的两种解释方式 ——
            // 选错会让基色发暗或让法线方向系统性偏移
            return isSrgb ? EPixelFormat::RGBA8_SRGB : EPixelFormat::RGBA8_UNORM;

        case EImageFormat::R16:
            return EPixelFormat::R16_UNORM;

        // 三通道格式在 Vulkan 上支持不普遍, 解码器默认已扩展为四通道;
        // 走到这里说明调用方关闭了扩展, 此处不做隐式转换而是明确失败
        case EImageFormat::RGB8:
        case EImageFormat::RGB16:
        case EImageFormat::RG16:
        case EImageFormat::RGBA16:
        default:
            return EPixelFormat::Unknown;
    }
}

// ============================================================================
// 网格
// ============================================================================

FMeshResourceHandle FRenderResourceManager::CreateMesh(const FMeshData& meshData,
                                                       const FName& name)
{
    FMeshResourceHandle handle;

    if (m_Device == nullptr || meshData.IsEmpty())
    {
        return handle;
    }

    // ---- 顶点缓冲区 ----
    const UInt64 vertexBytes =
        static_cast<UInt64>(meshData.Vertices.GetSize()) * sizeof(FMeshVertex);

    FRHIBufferHandle vertexBuffer;

    // 光追可用时, 网格缓冲区同时也是加速结构的构建输入。
    //
    // 这两个用途标志必须在**创建时**就带上 —— 缓冲区建好之后没法追加,
    // 而漏带的表现不是报错: vkGetBufferDeviceAddress 会安静地返回 0, 于是
    // 加速结构建在空地址上, 成为一棵空树。所有射线都不命中, 而"什么都没
    // 照到"与"场景本来就是空的"在画面上一模一样。
    const EBufferUsage accelUsage =
        (m_Device != nullptr && m_Device->IsRayTracingSupported())
            ? (EBufferUsage::AccelStructBuild |
               EBufferUsage::ShaderDeviceAddress)
            : EBufferUsage::None;

    // TransferSrc 是给虚拟几何路径用的: 光栅化 meshlet 时顶点要从一份
    // 场景级的汇总缓冲区里取, 而各网格的顶点缓冲区是那次拷贝的源。
    const EBufferUsage vertexUsage = static_cast<EBufferUsage>(
        static_cast<UInt32>(EBufferUsage::VertexBuffer) |
        static_cast<UInt32>(EBufferUsage::StorageBuffer) |
        static_cast<UInt32>(EBufferUsage::TransferSrc) |
        static_cast<UInt32>(accelUsage));

    if (!IsRHISuccess(UploadBuffer(meshData.Vertices.GetData(), vertexBytes,
                                   vertexUsage, vertexBuffer)))
    {
        LIMX_LOG(LogRenderCore, Error,
                 "[资源管理器] 顶点缓冲区上传失败: {} 字节", vertexBytes);
        return handle;
    }

    // ---- 索引缓冲区 ----
    //
    // 顶点数不超过 65535 时收窄为 16 位索引, 索引带宽直接减半。
    // 这一步放在上传处而非解析处, 因为只有此时才同时掌握顶点数与索引数组。
    const bool useSixteenBitIndices =
        meshData.Vertices.GetSize() <= kMaxUInt16Index;

    FRHIBufferHandle indexBuffer;
    UInt64 indexBytes = 0;

    if (useSixteenBitIndices)
    {
        TArray<UInt16> narrowIndices;
        narrowIndices.Reserve(meshData.Indices.GetSize());

        for (SizeType i = 0; i < meshData.Indices.GetSize(); ++i)
        {
            narrowIndices.Add(static_cast<UInt16>(meshData.Indices[i]));
        }

        indexBytes = static_cast<UInt64>(narrowIndices.GetSize()) * sizeof(UInt16);

        if (!IsRHISuccess(UploadBuffer(narrowIndices.GetData(), indexBytes,
                                       EBufferUsage::IndexBuffer | accelUsage,
                                       indexBuffer)))
        {
            m_Device->DestroyBuffer(vertexBuffer);
            LIMX_LOG(LogRenderCore, Error, "[资源管理器] 索引缓冲区上传失败");
            return handle;
        }
    }
    else
    {
        indexBytes =
            static_cast<UInt64>(meshData.Indices.GetSize()) * sizeof(UInt32);

        if (!IsRHISuccess(UploadBuffer(meshData.Indices.GetData(), indexBytes,
                                       EBufferUsage::IndexBuffer | accelUsage,
                                       indexBuffer)))
        {
            m_Device->DestroyBuffer(vertexBuffer);
            LIMX_LOG(LogRenderCore, Error, "[资源管理器] 索引缓冲区上传失败");
            return handle;
        }
    }

    // ---- 批次划分 ----
    //
    // 先把批次算出来: meshlet 按批次切, 而批次来自 SubMeshes。
    // 没有 SubMeshes 时补一个覆盖全部索引的批次, 让下面这段无需分支。
    struct FSectionRange
    {
        UInt32 IndexOffset = 0;
        UInt32 IndexCount  = 0;
    };

    TArray<FSectionRange> sectionRanges;

    for (SizeType i = 0; i < meshData.SubMeshes.GetSize(); ++i)
    {
        const FSubMesh& source = meshData.SubMeshes[i];

        if (source.IndexCount == 0)
        {
            continue;
        }

        FSectionRange range;
        range.IndexOffset = source.IndexOffset;
        range.IndexCount  = source.IndexCount;

        sectionRanges.Add(range);
    }

    if (sectionRanges.IsEmpty())
    {
        FSectionRange range;
        range.IndexOffset = 0;
        range.IndexCount  = static_cast<UInt32>(meshData.Indices.GetSize());

        sectionRanges.Add(range);
    }

    // ---- meshlet ----
    //
    // 切分本身在 CPU 上, 与顶点/索引上传同一次调用里完成。切失败就整个
    // 网格创建失败 —— 而不是"这个网格没有 meshlet, 别的有"。后者会让
    // 虚拟几何路径静默地漏画一部分物体。
    //
    // 逐批次切, 结果首尾相接拼成一份。拼接时要把每个 meshlet 的
    // VertexOffset 与 TriangleOffset 加上当前的基址 —— 那两个字段是
    // 数组下标, 而数组换了。
    FRHIBufferHandle meshletBuffer;
    FRHIBufferHandle meshletVertexBuffer;
    FRHIBufferHandle meshletTriangleBuffer;

    UInt64 meshletBytes = 0;

    FMeshletBuildResult meshlets;

    TArray<UInt32>  sectionMeshletOffsets;
    TArray<UInt32>  sectionMeshletCounts;
    TArray<Float32> sectionMaxMeshletRadii;

    for (SizeType s = 0; s < sectionRanges.GetSize(); ++s)
    {
        const FSectionRange& range = sectionRanges[s];

        TArray<UInt32> sectionIndices;
        sectionIndices.Reserve(range.IndexCount);

        for (UInt32 i = 0; i < range.IndexCount; ++i)
        {
            const SizeType source =
                static_cast<SizeType>(range.IndexOffset) + i;

            if (source >= meshData.Indices.GetSize())
            {
                break;
            }

            sectionIndices.Add(meshData.Indices[source]);
        }

        const FMeshletBuildResult part =
            FMeshletBuilder::Build(meshData.Vertices, sectionIndices);

        if (!part.IsValid())
        {
            m_Device->DestroyBuffer(indexBuffer);
            m_Device->DestroyBuffer(vertexBuffer);

            LIMX_LOG(LogRenderCore, Error,
                     "[资源管理器] 批次 {} 的 meshlet 切分失败: "
                     "{} 个顶点 / {} 个索引",
                     s, meshData.Vertices.GetSize(),
                     sectionIndices.GetSize());
            return handle;
        }

        const UInt32 vertexBase =
            static_cast<UInt32>(meshlets.MeshletVertices.GetSize());

        const UInt32 triangleBase =
            static_cast<UInt32>(meshlets.MeshletTriangles.GetSize() / 3);

        sectionMeshletOffsets.Add(
            static_cast<UInt32>(meshlets.Meshlets.GetSize()));

        sectionMeshletCounts.Add(
            static_cast<UInt32>(part.Meshlets.GetSize()));

        Float32 sectionMaxRadius = 0.0f;

        for (SizeType m = 0; m < part.Meshlets.GetSize(); ++m)
        {
            FMeshlet meshlet = part.Meshlets[m];

            meshlet.VertexOffset += vertexBase;
            meshlet.TriangleOffset += triangleBase;

            sectionMaxRadius =
                FMath::Max(sectionMaxRadius, meshlet.BoundingSphere.W);

            meshlets.Meshlets.Add(meshlet);
        }

        sectionMaxMeshletRadii.Add(sectionMaxRadius);

        for (SizeType i = 0; i < part.MeshletVertices.GetSize(); ++i)
        {
            meshlets.MeshletVertices.Add(part.MeshletVertices[i]);
        }

        for (SizeType i = 0; i < part.MeshletTriangles.GetSize(); ++i)
        {
            meshlets.MeshletTriangles.Add(part.MeshletTriangles[i]);
        }
    }

    if (!meshlets.IsValid())
    {
        m_Device->DestroyBuffer(indexBuffer);
        m_Device->DestroyBuffer(vertexBuffer);

        LIMX_LOG(LogRenderCore, Error,
                 "[资源管理器] meshlet 切分之后一个 meshlet 都没有");
        return handle;
    }

    //
    // 切分本身在 CPU 上, 与顶点/索引上传同一次调用里完成。切失败就整个
    // 网格创建失败 —— 而不是"这个网格没有 meshlet, 别的有"。后者会让
    // 虚拟几何路径静默地漏画一部分物体。

    {
        // 三角形的局部索引每个三字节。GLSL 里没有字节寻址的 storage buffer
        // (要 GL_EXT_shader_8bit_storage), 所以按 UInt32 打包上传, 着色器
        // 自己移位取。这一处的代价是四分之一的浪费换一个少一项的扩展依赖。
        TArray<UInt32> packedTriangles;

        const SizeType byteCount = meshlets.MeshletTriangles.GetSize();

        packedTriangles.SetSize((byteCount + 3) / 4, 0u);

        for (SizeType i = 0; i < byteCount; ++i)
        {
            packedTriangles[i / 4] |=
                static_cast<UInt32>(meshlets.MeshletTriangles[i])
                << ((i % 4) * 8);
        }

        const UInt64 headerBytes =
            static_cast<UInt64>(meshlets.Meshlets.GetSize()) *
            sizeof(FMeshlet);

        const UInt64 localVertexBytes =
            static_cast<UInt64>(meshlets.MeshletVertices.GetSize()) *
            sizeof(UInt32);

        const UInt64 packedBytes =
            static_cast<UInt64>(packedTriangles.GetSize()) * sizeof(UInt32);

        // meshlet 头缓冲区要带 TransferSrc: 剔除通道把各网格的 meshlet
        // 拷进一份汇总缓冲区, 而这个缓冲区是那次拷贝的**源**。
        // 漏了的话不会崩, 验证层会报"用途不含 TRANSFER_SRC", 而关掉验证层
        // 就是未定义行为 —— 汇总缓冲区里可能是任意内容。
        // 三份都要带 TransferSrc: 剔除通道把各网格的 meshlet 数据拷进
        // 汇总缓冲区, 而它们是那次拷贝的**源**。
        //
        // 漏了不会崩, 验证层会报"用途不含 TRANSFER_SRC", 而关掉验证层就是
        // 未定义行为 —— 汇总缓冲区里可能是任意内容。
        const EBufferUsage meshletUsage = static_cast<EBufferUsage>(
            static_cast<UInt32>(EBufferUsage::StorageBuffer) |
            static_cast<UInt32>(EBufferUsage::TransferSrc));

        const bool uploaded =
            IsRHISuccess(UploadBuffer(meshlets.Meshlets.GetData(),
                                      headerBytes, meshletUsage,
                                      meshletBuffer)) &&
            IsRHISuccess(UploadBuffer(meshlets.MeshletVertices.GetData(),
                                      localVertexBytes, meshletUsage,
                                      meshletVertexBuffer)) &&
            IsRHISuccess(UploadBuffer(packedTriangles.GetData(), packedBytes,
                                      meshletUsage, meshletTriangleBuffer));

        if (!uploaded)
        {
            m_Device->DestroyBuffer(meshletTriangleBuffer);
            m_Device->DestroyBuffer(meshletVertexBuffer);
            m_Device->DestroyBuffer(meshletBuffer);
            m_Device->DestroyBuffer(indexBuffer);
            m_Device->DestroyBuffer(vertexBuffer);

            LIMX_LOG(LogRenderCore, Error, "[资源管理器] meshlet 缓冲区上传失败");
            return handle;
        }

        meshletBytes = headerBytes + localVertexBytes + packedBytes;
    }

    // ---- 取槽位 ----
    UInt32 slotIndex = 0;

    if (m_FreeMeshSlots.GetSize() > 0)
    {
        slotIndex = m_FreeMeshSlots.Last();
        m_FreeMeshSlots.RemoveAt(m_FreeMeshSlots.GetSize() - 1);
    }
    else
    {
        slotIndex = static_cast<UInt32>(m_Meshes.Add(FMeshSlot()));
    }

    FMeshSlot& slot = m_Meshes[slotIndex];

    slot.Resource.Name = name.IsEmpty() ? meshData.Name : name;
    slot.Resource.VertexBuffer      = vertexBuffer;
    slot.Resource.IndexBuffer       = indexBuffer;
    slot.Resource.VertexCount       = static_cast<UInt32>(meshData.Vertices.GetSize());
    slot.Resource.IndexCount        = static_cast<UInt32>(meshData.Indices.GetSize());
    slot.Resource.IndexType         = useSixteenBitIndices ? EIndexType::UInt16
                                                           : EIndexType::UInt32;
    slot.Resource.Bounds            = meshData.Bounds;
    slot.Resource.VertexBufferBytes = vertexBytes;
    slot.Resource.VertexStride      = sizeof(FMeshVertex);
    slot.Resource.IndexBufferBytes  = indexBytes;

    slot.Resource.MeshletBuffer         = meshletBuffer;
    slot.Resource.MeshletVertexBuffer   = meshletVertexBuffer;
    slot.Resource.MeshletTriangleBuffer = meshletTriangleBuffer;
    slot.Resource.MeshletCount =
        static_cast<UInt32>(meshlets.Meshlets.GetSize());
    slot.Resource.MeshletVertexCount =
        static_cast<UInt32>(meshlets.MeshletVertices.GetSize());
    slot.Resource.MeshletTriangleCount =
        static_cast<UInt32>(meshlets.MeshletTriangles.GetSize() / 3);
    slot.Resource.MeshletBytes = meshletBytes;

    for (SizeType m = 0; m < meshlets.Meshlets.GetSize(); ++m)
    {
        slot.Resource.MaxMeshletRadius =
            FMath::Max(slot.Resource.MaxMeshletRadius,
                       meshlets.Meshlets[m].BoundingSphere.W);
    }

    slot.Resource.Sections.Clear();
    slot.Resource.Sections.Reserve(meshData.SubMeshes.GetSize());

    for (SizeType i = 0; i < meshData.SubMeshes.GetSize(); ++i)
    {
        const FSubMesh& source = meshData.SubMeshes[i];

        if (source.IndexCount == 0)
        {
            continue;
        }

        FMeshSection section;
        section.Name         = source.Name;
        section.IndexOffset  = source.IndexOffset;
        section.IndexCount   = source.IndexCount;
        section.MaterialSlot = source.MaterialIndex;
        section.Bounds       = source.Bounds;

        // 批次与 meshlet 区间是**同一个循环产出的**, 所以下标一一对应。
        // 分两处算的话, 上面那个 IndexCount == 0 的跳过只要有一处漏了,
        // 后面所有批次的 meshlet 区间就整体错位一格 —— 而那正是本周期
        // Day 5 在几何表上踩过的坑。
        const SizeType sectionIndex = slot.Resource.Sections.GetSize();

        if (sectionIndex < sectionMeshletOffsets.GetSize())
        {
            section.MeshletOffset    = sectionMeshletOffsets[sectionIndex];
            section.MeshletCount     = sectionMeshletCounts[sectionIndex];
            section.MaxMeshletRadius = sectionMaxMeshletRadii[sectionIndex];
        }

        slot.Resource.Sections.Add(section);
    }

    // 没有子网格时补一个覆盖全部索引的批次, 使渲染路径无需分支
    if (slot.Resource.Sections.GetSize() == 0)
    {
        FMeshSection section;
        section.Name         = slot.Resource.Name;
        section.IndexOffset  = 0;
        section.IndexCount   = slot.Resource.IndexCount;
        section.MaterialSlot = -1;
        section.Bounds       = meshData.Bounds;

        if (!sectionMeshletOffsets.IsEmpty())
        {
            section.MeshletOffset    = sectionMeshletOffsets[0];
            section.MeshletCount     = sectionMeshletCounts[0];
            section.MaxMeshletRadius = sectionMaxMeshletRadii[0];
        }

        slot.Resource.Sections.Add(section);
    }

    slot.ReferenceCount = 1;
    slot.IsActive       = true;

    handle.Index      = slotIndex;
    handle.Generation = slot.Generation;

    return handle;
}

const FMeshResource* FRenderResourceManager::GetMesh(
    FMeshResourceHandle handle) const
{
    const FMeshSlot* slot = ResolveMesh(handle);
    return (slot != nullptr) ? &slot->Resource : nullptr;
}

// ============================================================================
// 纹理
// ============================================================================

FTextureResourceHandle FRenderResourceManager::CreateTexture(
    const FImageData& image,
    const FTextureUploadOptions& options,
    const FName& name)
{
    FTextureResourceHandle handle;

    if (m_Device == nullptr || !image.IsValid())
    {
        return handle;
    }

    const EPixelFormat format = MapPixelFormat(image.Format, options.IsSrgb);

    if (format == EPixelFormat::Unknown)
    {
        LIMX_LOG(LogRenderCore, Error,
                 "[资源管理器] 不支持的图像格式, 通道数 {} 每通道 {} 字节",
                 image.GetChannelCount(), image.GetBytesPerChannel());
        return handle;
    }

    FRHITextureHandle texture;
    UInt32            mipLevels = 1;

    if (!IsRHISuccess(UploadTexture2D(image.Pixels.GetData(),
                                      image.Pixels.GetSize(),
                                      image.Width, image.Height,
                                      format, mipLevels, texture)))
    {
        LIMX_LOG(LogRenderCore, Error,
                 "[资源管理器] 纹理上传失败: {}x{}", image.Width, image.Height);
        return handle;
    }

    // ---- 视图 ----
    FRHITextureViewDesc viewDesc = {};
    viewDesc.Texture         = texture;
    viewDesc.ViewType        = ETextureType::Texture2D;
    viewDesc.Format          = format;
    viewDesc.BaseMipLevel    = 0;
    // 视图必须覆盖全部 mip —— 只暴露第 0 级等于生成了 mip 链却永远采不到,
    // 而画面表现与完全没有 mip 一模一样。
    viewDesc.MipLevelCount   = mipLevels;
    viewDesc.BaseArrayLayer  = 0;
    viewDesc.ArrayLayerCount = 1;

    FRHITextureViewHandle view;

    if (!IsRHISuccess(m_Device->CreateTextureView(viewDesc, view)))
    {
        m_Device->DestroyTexture(texture);
        LIMX_LOG(LogRenderCore, Error, "[资源管理器] 纹理视图创建失败");
        return handle;
    }

    // ---- 采样器 ----
    FSamplerKey samplerKey;
    samplerKey.AddressMode   = options.AddressMode;
    samplerKey.UseAnisotropy = options.UseAnisotropy;
    samplerKey.MipLevels     = mipLevels;

    const FRHISamplerHandle sampler = AcquireSampler(samplerKey);

    // ---- 取槽位 ----
    UInt32 slotIndex = 0;

    if (m_FreeTextureSlots.GetSize() > 0)
    {
        slotIndex = m_FreeTextureSlots.Last();
        m_FreeTextureSlots.RemoveAt(m_FreeTextureSlots.GetSize() - 1);
    }
    else
    {
        slotIndex = static_cast<UInt32>(m_Textures.Add(FTextureSlot()));
    }

    FTextureSlot& slot = m_Textures[slotIndex];

    slot.Resource.Name        = name.IsEmpty() ? image.Name : name;
    slot.Resource.Texture     = texture;
    slot.Resource.View        = view;
    slot.Resource.Sampler     = sampler;
    slot.Resource.Width       = image.Width;
    slot.Resource.Height      = image.Height;
    slot.Resource.MipLevels   = mipLevels;
    slot.Resource.Format      = format;
    // 完整 mip 链的总面积是基层的 4/3 (等比级数 1 + 1/4 + 1/16 + ...),
    // 逐级向下取整使实际值略小于该上界。统计取上界而非只算基层,
    // 否则显存报表会系统性偏低三成。
    slot.Resource.MemoryBytes =
        (mipLevels > 1) ? (image.Pixels.GetSize() * 4u / 3u)
                        : image.Pixels.GetSize();

    slot.ReferenceCount = 1;
    slot.IsActive       = true;

    handle.Index      = slotIndex;
    handle.Generation = slot.Generation;

    return handle;
}

EPixelFormat FRenderResourceManager::MapCompressedPixelFormat(
    EBlockCompressionFormat format)
{
    // 一一对应, 且没有兜底分支 —— 新增一个块压缩格式时若忘了在这里加,
    // 得到的是"纹理创建失败"这条明确日志, 而不是一张按别的格式解读的图。
    switch (format)
    {
        case EBlockCompressionFormat::BC1_UNORM:   return EPixelFormat::BC1_UNORM;
        case EBlockCompressionFormat::BC1_SRGB:    return EPixelFormat::BC1_SRGB;
        case EBlockCompressionFormat::BC2_UNORM:   return EPixelFormat::BC2_UNORM;
        case EBlockCompressionFormat::BC2_SRGB:    return EPixelFormat::BC2_SRGB;
        case EBlockCompressionFormat::BC3_UNORM:   return EPixelFormat::BC3_UNORM;
        case EBlockCompressionFormat::BC3_SRGB:    return EPixelFormat::BC3_SRGB;
        case EBlockCompressionFormat::BC4_UNORM:   return EPixelFormat::BC4_UNORM;
        case EBlockCompressionFormat::BC4_SNORM:   return EPixelFormat::BC4_SNORM;
        case EBlockCompressionFormat::BC5_UNORM:   return EPixelFormat::BC5_UNORM;
        case EBlockCompressionFormat::BC5_SNORM:   return EPixelFormat::BC5_SNORM;
        case EBlockCompressionFormat::BC6H_UFLOAT: return EPixelFormat::BC6H_UFLOAT;
        case EBlockCompressionFormat::BC6H_SFLOAT: return EPixelFormat::BC6H_SFLOAT;
        case EBlockCompressionFormat::BC7_UNORM:   return EPixelFormat::BC7_UNORM;
        case EBlockCompressionFormat::BC7_SRGB:    return EPixelFormat::BC7_SRGB;

        case EBlockCompressionFormat::Unknown:
        default:                                   return EPixelFormat::Unknown;
    }
}

FTextureResourceHandle FRenderResourceManager::CreateTexture(
    const FCompressedImageData& image,
    const FTextureUploadOptions& options,
    const FName& name)
{
    FTextureResourceHandle handle;

    if (m_Device == nullptr || !image.IsValid())
    {
        LIMX_LOG(LogRenderCore, Error,
                 "[资源管理器] 压缩纹理数据不自洽, 拒绝上传: {}x{} / {} 层",
                 image.Width, image.Height, image.GetMipLevelCount());
        return handle;
    }

    const EPixelFormat format = MapCompressedPixelFormat(image.Format);

    if (format == EPixelFormat::Unknown)
    {
        LIMX_LOG(LogRenderCore, Error,
                 "[资源管理器] 无法映射的块压缩格式: {}",
                 GetBlockCompressionFormatName(image.Format));
        return handle;
    }

    // 色彩空间以文件为准。DDS 的 dxgiFormat 把 sRGB 与否写死了, 那是事实;
    // 调用方按用途猜的 IsSrgb 只是意图。两者不一致时按事实走, 但必须让
    // 这件事可见 —— 不然一张按线性烘出来的 albedo 会安静地偏暗。
    if (options.IsSrgb != image.IsSrgb())
    {
        LIMX_LOG(LogRenderCore, Warning,
                 "[资源管理器] 纹理 '{}' 的色彩空间与调用方预期不符: "
                 "文件是 {}, 调用方按 {} 使用 — 以文件为准",
                 name.GetCStr(),
                 GetBlockCompressionFormatName(image.Format),
                 options.IsSrgb ? "sRGB" : "线性");
    }

    FRHITextureHandle texture;

    if (!IsRHISuccess(UploadCompressedTexture2D(image, format, texture)))
    {
        LIMX_LOG(LogRenderCore, Error,
                 "[资源管理器] 压缩纹理上传失败: {}x{} {}",
                 image.Width, image.Height,
                 GetBlockCompressionFormatName(image.Format));
        return handle;
    }

    const UInt32 mipLevels = image.GetMipLevelCount();

    // ---- 视图 ----
    FRHITextureViewDesc viewDesc = {};
    viewDesc.Texture         = texture;
    viewDesc.ViewType        = ETextureType::Texture2D;
    viewDesc.Format          = format;
    viewDesc.BaseMipLevel    = 0;
    viewDesc.MipLevelCount   = mipLevels;
    viewDesc.BaseArrayLayer  = 0;
    viewDesc.ArrayLayerCount = 1;

    FRHITextureViewHandle view;

    if (!IsRHISuccess(m_Device->CreateTextureView(viewDesc, view)))
    {
        m_Device->DestroyTexture(texture);
        LIMX_LOG(LogRenderCore, Error, "[资源管理器] 压缩纹理视图创建失败");
        return handle;
    }

    // ---- 采样器 ----
    FSamplerKey samplerKey;
    samplerKey.AddressMode   = options.AddressMode;
    samplerKey.UseAnisotropy = options.UseAnisotropy;
    samplerKey.MipLevels     = mipLevels;

    const FRHISamplerHandle sampler = AcquireSampler(samplerKey);

    // ---- 取槽位 ----
    UInt32 slotIndex = 0;

    if (m_FreeTextureSlots.GetSize() > 0)
    {
        slotIndex = m_FreeTextureSlots.Last();
        m_FreeTextureSlots.RemoveAt(m_FreeTextureSlots.GetSize() - 1);
    }
    else
    {
        slotIndex = static_cast<UInt32>(m_Textures.Add(FTextureSlot()));
    }

    FTextureSlot& slot = m_Textures[slotIndex];

    slot.Resource.Name      = name.IsEmpty() ? image.Name : name;
    slot.Resource.Texture   = texture;
    slot.Resource.View      = view;
    slot.Resource.Sampler   = sampler;
    slot.Resource.Width     = image.Width;
    slot.Resource.Height    = image.Height;
    slot.Resource.MipLevels = mipLevels;
    slot.Resource.Format    = format;

    // 压缩纹理不需要按 4/3 估算 —— 载荷里已经是全部 mip 的实际字节数。
    // 未压缩路径要估是因为那边的 mip 是 GPU 现生成的, CPU 侧没有它们。
    slot.Resource.MemoryBytes = static_cast<UInt64>(image.Data.GetSize());

    slot.ReferenceCount = 1;
    slot.IsActive       = true;

    handle.Index      = slotIndex;
    handle.Generation = slot.Generation;

    return handle;
}

FTextureResourceHandle FRenderResourceManager::CreateSolidColorTexture(
    UInt8 red, UInt8 green, UInt8 blue, UInt8 alpha,
    bool isSrgb, const FName& name)
{
    FImageData image;

    image.Name   = name;
    image.Width  = 1;
    image.Height = 1;
    image.Format = EImageFormat::RGBA8;

    image.Pixels.Add(red);
    image.Pixels.Add(green);
    image.Pixels.Add(blue);
    image.Pixels.Add(alpha);

    FTextureUploadOptions options;
    options.IsSrgb        = isSrgb;
    options.AddressMode   = ESamplerAddressMode::Repeat;
    options.UseAnisotropy = false;

    return CreateTexture(image, options, name);
}

const FTextureResource* FRenderResourceManager::GetTexture(
    FTextureResourceHandle handle) const
{
    const FTextureSlot* slot = ResolveTexture(handle);
    return (slot != nullptr) ? &slot->Resource : nullptr;
}

// ============================================================================
// 句柄解析
// ============================================================================

const FRenderResourceManager::FMeshSlot* FRenderResourceManager::ResolveMesh(
    FMeshResourceHandle handle) const
{
    if (!handle.IsValid() || handle.Index >= m_Meshes.GetSize())
    {
        return nullptr;
    }

    const FMeshSlot& slot = m_Meshes[handle.Index];

    // 代际不符说明该槽位已被卸载并复用
    if (!slot.IsActive || slot.Generation != handle.Generation)
    {
        return nullptr;
    }

    return &slot;
}

FRenderResourceManager::FMeshSlot* FRenderResourceManager::ResolveMesh(
    FMeshResourceHandle handle)
{
    const FMeshSlot* slot =
        static_cast<const FRenderResourceManager*>(this)->ResolveMesh(handle);

    return const_cast<FMeshSlot*>(slot);
}

const FRenderResourceManager::FTextureSlot*
FRenderResourceManager::ResolveTexture(FTextureResourceHandle handle) const
{
    if (!handle.IsValid() || handle.Index >= m_Textures.GetSize())
    {
        return nullptr;
    }

    const FTextureSlot& slot = m_Textures[handle.Index];

    if (!slot.IsActive || slot.Generation != handle.Generation)
    {
        return nullptr;
    }

    return &slot;
}

FRenderResourceManager::FTextureSlot* FRenderResourceManager::ResolveTexture(
    FTextureResourceHandle handle)
{
    const FTextureSlot* slot =
        static_cast<const FRenderResourceManager*>(this)->ResolveTexture(handle);

    return const_cast<FTextureSlot*>(slot);
}

// ============================================================================
// 引用计数
// ============================================================================

void FRenderResourceManager::AddMeshReference(FMeshResourceHandle handle)
{
    if (FMeshSlot* slot = ResolveMesh(handle))
    {
        ++slot->ReferenceCount;
    }
}

void FRenderResourceManager::ReleaseMeshReference(FMeshResourceHandle handle)
{
    FMeshSlot* slot = ResolveMesh(handle);

    if (slot != nullptr && slot->ReferenceCount > 0)
    {
        --slot->ReferenceCount;
    }
}

void FRenderResourceManager::AddTextureReference(FTextureResourceHandle handle)
{
    if (FTextureSlot* slot = ResolveTexture(handle))
    {
        ++slot->ReferenceCount;
    }
}

void FRenderResourceManager::ReleaseTextureReference(
    FTextureResourceHandle handle)
{
    FTextureSlot* slot = ResolveTexture(handle);

    if (slot != nullptr && slot->ReferenceCount > 0)
    {
        --slot->ReferenceCount;
    }
}

UInt32 FRenderResourceManager::GetMeshReferenceCount(
    FMeshResourceHandle handle) const
{
    const FMeshSlot* slot = ResolveMesh(handle);
    return (slot != nullptr) ? slot->ReferenceCount : 0;
}

UInt32 FRenderResourceManager::GetTextureReferenceCount(
    FTextureResourceHandle handle) const
{
    const FTextureSlot* slot = ResolveTexture(handle);
    return (slot != nullptr) ? slot->ReferenceCount : 0;
}

// ============================================================================
// 退役与销毁
// ============================================================================

UInt64 FRenderResourceManager::GetCurrentFrame() const
{
    return (m_Context != nullptr) ? m_Context->GetFrameCounter() : 0;
}

void FRenderResourceManager::RetireMeshSlot(FMeshSlot& slot)
{
    // GPU 对象移出槽位而非就地销毁 —— 上一帧的命令缓冲区可能仍在读它们。
    // 移出之后槽位就干净了, 可以立刻复用。
    if (slot.Resource.VertexBuffer.IsValid() ||
        slot.Resource.IndexBuffer.IsValid() ||
        slot.Resource.MeshletBuffer.IsValid())
    {
        FPendingRelease entry;
        entry.VertexBuffer = slot.Resource.VertexBuffer;
        entry.IndexBuffer  = slot.Resource.IndexBuffer;

        entry.MeshletBuffer         = slot.Resource.MeshletBuffer;
        entry.MeshletVertexBuffer   = slot.Resource.MeshletVertexBuffer;
        entry.MeshletTriangleBuffer = slot.Resource.MeshletTriangleBuffer;

        entry.RetireFrame  = GetCurrentFrame();

        m_PendingReleases.Add(entry);
    }

    slot.Resource = FMeshResource();
    slot.IsActive = false;

    // 代际递增使一切指向该槽位的旧句柄立即失效
    ++slot.Generation;
    slot.ReferenceCount = 0;
}

void FRenderResourceManager::RetireTextureSlot(FTextureSlot& slot)
{
    if (slot.Resource.Texture.IsValid() || slot.Resource.View.IsValid())
    {
        FPendingRelease entry;
        entry.Texture     = slot.Resource.Texture;
        entry.View        = slot.Resource.View;
        entry.RetireFrame = GetCurrentFrame();

        m_PendingReleases.Add(entry);
    }

    // 采样器由管理器共享持有, 不随单张纹理销毁

    slot.Resource = FTextureResource();
    slot.IsActive = false;

    ++slot.Generation;
    slot.ReferenceCount = 0;
}

void FRenderResourceManager::DestroyPendingRelease(FPendingRelease& entry)
{
    // 视图先于纹理销毁 —— 反过来是悬垂引用
    if (entry.View.IsValid())
    {
        m_Device->DestroyTextureView(entry.View);
    }

    if (entry.Texture.IsValid())
    {
        m_Device->DestroyTexture(entry.Texture);
    }

    if (entry.IndexBuffer.IsValid())
    {
        m_Device->DestroyBuffer(entry.IndexBuffer);
    }

    if (entry.MeshletTriangleBuffer.IsValid())
    {
        m_Device->DestroyBuffer(entry.MeshletTriangleBuffer);
    }

    if (entry.MeshletVertexBuffer.IsValid())
    {
        m_Device->DestroyBuffer(entry.MeshletVertexBuffer);
    }

    if (entry.MeshletBuffer.IsValid())
    {
        m_Device->DestroyBuffer(entry.MeshletBuffer);
    }

    if (entry.VertexBuffer.IsValid())
    {
        m_Device->DestroyBuffer(entry.VertexBuffer);
    }
}

UInt32 FRenderResourceManager::ProcessPendingReleases()
{
    if (m_Device == nullptr || m_PendingReleases.GetSize() == 0)
    {
        return 0;
    }

    const UInt64 currentFrame = GetCurrentFrame();
    const UInt64 framesInFlight =
        (m_Context != nullptr)
            ? static_cast<UInt64>(m_Context->GetMaxFramesInFlight())
            : 1;

    UInt32 destroyed = 0;

    // 倒序遍历 —— 就地移除时不会跳过元素
    SizeType index = m_PendingReleases.GetSize();

    while (index > 0)
    {
        --index;

        // 退役那一帧的提交是最后一批可能引用这些对象的命令。
        // 走到第 RetireFrame + framesInFlight 帧时, 该帧的栅栏已经通过。
        if (currentFrame < m_PendingReleases[index].RetireFrame + framesInFlight)
        {
            continue;
        }

        DestroyPendingRelease(m_PendingReleases[index]);
        m_PendingReleases.RemoveAt(index);
        ++destroyed;
    }

    return destroyed;
}

UInt32 FRenderResourceManager::FlushPendingReleases()
{
    if (m_Device == nullptr)
    {
        return 0;
    }

    UInt32 destroyed = 0;

    for (SizeType i = 0; i < m_PendingReleases.GetSize(); ++i)
    {
        DestroyPendingRelease(m_PendingReleases[i]);
        ++destroyed;
    }

    m_PendingReleases.Clear();

    return destroyed;
}

// ============================================================================
// 回收
// ============================================================================

UInt32 FRenderResourceManager::CollectUnreferenced()
{
    UInt32 collected = 0;

    for (SizeType i = 0; i < m_Meshes.GetSize(); ++i)
    {
        if (m_Meshes[i].IsActive && m_Meshes[i].ReferenceCount == 0)
        {
            RetireMeshSlot(m_Meshes[i]);
            m_FreeMeshSlots.Add(static_cast<UInt32>(i));
            ++collected;
        }
    }

    for (SizeType i = 0; i < m_Textures.GetSize(); ++i)
    {
        if (m_Textures[i].IsActive && m_Textures[i].ReferenceCount == 0)
        {
            RetireTextureSlot(m_Textures[i]);
            m_FreeTextureSlots.Add(static_cast<UInt32>(i));
            ++collected;
        }
    }

    return collected;
}

void FRenderResourceManager::UnloadMesh(FMeshResourceHandle handle)
{
    FMeshSlot* slot = ResolveMesh(handle);

    if (slot != nullptr)
    {
        RetireMeshSlot(*slot);
        m_FreeMeshSlots.Add(handle.Index);
    }
}

void FRenderResourceManager::UnloadTexture(FTextureResourceHandle handle)
{
    FTextureSlot* slot = ResolveTexture(handle);

    if (slot != nullptr)
    {
        RetireTextureSlot(*slot);
        m_FreeTextureSlots.Add(handle.Index);
    }
}

// ============================================================================
// 统计
// ============================================================================

FRenderResourceStats FRenderResourceManager::GetStats() const
{
    FRenderResourceStats stats;

    for (SizeType i = 0; i < m_Meshes.GetSize(); ++i)
    {
        const FMeshSlot& slot = m_Meshes[i];

        if (!slot.IsActive)
        {
            continue;
        }

        ++stats.MeshCount;

        stats.MeshBytes      += slot.Resource.GetTotalBytes();
        stats.TotalVertices  += slot.Resource.VertexCount;
        stats.TotalTriangles += slot.Resource.IndexCount / 3;
        stats.TotalSections  += static_cast<UInt32>(slot.Resource.Sections.GetSize());

        if (slot.ReferenceCount == 0)
        {
            ++stats.UnreferencedCount;
        }
    }

    for (SizeType i = 0; i < m_Textures.GetSize(); ++i)
    {
        const FTextureSlot& slot = m_Textures[i];

        if (!slot.IsActive)
        {
            continue;
        }

        ++stats.TextureCount;
        stats.TextureBytes += slot.Resource.MemoryBytes;

        if (slot.ReferenceCount == 0)
        {
            ++stats.UnreferencedCount;
        }
    }

    stats.PendingReleaseCount =
        static_cast<UInt32>(m_PendingReleases.GetSize());

    return stats;
}

void FRenderResourceManager::LogStats(const AnsiChar* context) const
{
    const FRenderResourceStats stats = GetStats();

    LIMX_LOG(LogRenderCore, Log,
             "[资源管理器] {} — 网格:{} 纹理:{} 未引用:{} 待销毁:{} | "
             "{} 顶点 / {} 三角形 / {} 批次 | 显存 {} KiB (网格 {} + 纹理 {})",
             context, stats.MeshCount, stats.TextureCount,
             stats.UnreferencedCount, stats.PendingReleaseCount,
             stats.TotalVertices, stats.TotalTriangles, stats.TotalSections,
             stats.GetTotalBytes() / 1024,
             stats.MeshBytes / 1024, stats.TextureBytes / 1024);
}

} // namespace Limx
