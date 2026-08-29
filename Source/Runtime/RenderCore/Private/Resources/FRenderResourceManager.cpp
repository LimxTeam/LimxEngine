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
#include "RenderCore/Renderer/FRenderContext.h"

namespace Limx
{

LIMX_DECLARE_LOG_CATEGORY(LogRenderCore)
LIMX_DEFINE_LOG_CATEGORY(LogRenderCore)

namespace
{

/// 顶点数不超过该值时可用 16 位索引
constexpr UInt32 kMaxUInt16Index = 65535;

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
                                                   FRHITextureHandle& outTexture)
{
    if (pixels == nullptr || byteCount == 0 || width == 0 || height == 0)
    {
        return ERHIResult::ErrorInvalidParameter;
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
    textureDesc.MipLevels     = 1;
    textureDesc.ArrayLayers   = 1;
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

    // ---- 上传并转换布局 ----
    IRHICommandBuffer* commandBuffer = m_Context->BeginSingleTimeCommands();

    if (commandBuffer == nullptr)
    {
        m_Device->DestroyBuffer(staging);
        m_Device->DestroyTexture(outTexture);
        return ERHIResult::ErrorUnknown;
    }

    // 新建纹理处于 Undefined 布局, 先转为传输目标
    commandBuffer->TransitionImageLayout(
        outTexture,
        EImageLayout::Undefined,
        EImageLayout::TransferDst,
        EPipelineStageFlags::TopOfPipe,
        EPipelineStageFlags::Transfer,
        EAccessFlags::None,
        EAccessFlags::TransferWrite);

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

    // 转为着色器只读, 供片元着色器采样
    commandBuffer->TransitionImageLayout(
        outTexture,
        EImageLayout::TransferDst,
        EImageLayout::ShaderReadOnly,
        EPipelineStageFlags::Transfer,
        EPipelineStageFlags::FragmentShader,
        EAccessFlags::TransferWrite,
        EAccessFlags::ShaderRead);

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

    if (!IsRHISuccess(UploadBuffer(meshData.Vertices.GetData(), vertexBytes,
                                   EBufferUsage::VertexBuffer, vertexBuffer)))
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
                                       EBufferUsage::IndexBuffer, indexBuffer)))
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
                                       EBufferUsage::IndexBuffer, indexBuffer)))
        {
            m_Device->DestroyBuffer(vertexBuffer);
            LIMX_LOG(LogRenderCore, Error, "[资源管理器] 索引缓冲区上传失败");
            return handle;
        }
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
    slot.Resource.IndexBufferBytes  = indexBytes;

    slot.Resource.Sections.Clear();
    slot.Resource.Sections.Reserve(meshData.SubMeshes.GetSize());

    for (SizeType i = 0; i < meshData.SubMeshes.GetSize(); ++i)
    {
        const FSubMesh& source = meshData.SubMeshes[i];

        FMeshSection section;
        section.Name         = source.Name;
        section.IndexOffset  = source.IndexOffset;
        section.IndexCount   = source.IndexCount;
        section.MaterialSlot = source.MaterialIndex;
        section.Bounds       = source.Bounds;

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

    if (!IsRHISuccess(UploadTexture2D(image.Pixels.GetData(),
                                      image.Pixels.GetSize(),
                                      image.Width, image.Height,
                                      format, texture)))
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
    viewDesc.MipLevelCount   = 1;
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
    samplerKey.MipLevels     = 1;

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
    slot.Resource.MipLevels   = 1;
    slot.Resource.Format      = format;
    slot.Resource.MemoryBytes = image.Pixels.GetSize();

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
        slot.Resource.IndexBuffer.IsValid())
    {
        FPendingRelease entry;
        entry.VertexBuffer = slot.Resource.VertexBuffer;
        entry.IndexBuffer  = slot.Resource.IndexBuffer;
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
