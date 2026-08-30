// ============================================================
// 文件名称：FMaterialManager.cpp
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：初始化时建立基础设施 — 默认纹理和描述符集布局在 Initialize()
//          一次性创建，后续所有材质共享这些基础资源，避免重复创建开销。
//          工厂方法封装初始化细节，调用者只需持有非拥有指针。
//          Shutdown() 按依赖逆序销毁：实例 → 材质 → 纹理 → 布局，
//          确保 GPU 资源无泄漏、无悬挂引用。
// 功能描述：FMaterialManager 完整实现 — 初始化默认纹理/采样器/描述符集布局，
//          提供 CreateMaterial/CreateMaterialInstance 工厂接口，
//          实现 Shutdown 完整资源释放和 UploadDirtyMaterials 批量上传。
// 技术特性：默认 1×1 白色纹理通过 Staging Buffer + BeginSingleTimeCommands 上传；
//          set 1 描述符集布局固定 6 个绑定 (1 UBO + 5 Sampler)；
//          CreateMaterial/CreateMaterialInstance 使用 TUniquePtr 管理所有权；
//          DestroyMaterial 通过指针比较在 TArray 中定位并移除。
//
// ── 函数表 ──────────────────────────────────────────────────
// │ 函数名                       │ 描述                           │
// │─────────────────────────────│───────────────────────────────│
// │ Initialize()                │ 创建默认纹理+采样器+描述符集布局   │
// │ Shutdown()                  │ 按逆序销毁全部资源               │
// │ CreateMaterial()            │ 工厂: 创建并注册 FMaterial        │
// │ CreateMaterialInstance()    │ 工厂: 创建并注册 FMaterialInstance│
// │ DestroyMaterial()           │ 从注册表移除并销毁               │
// │ DestroyMaterialInstance()   │ 同上，针对实例                   │
// │ CreateDefaultMaterial()     │ 创建预设灰色 PBR 材质            │
// │ UploadDirtyMaterials()      │ 遍历所有材质/实例调用 Flush()     │
// │ CreateDefaultTexture()      │ 创建 1×1 白色纹理并上传到 GPU     │
// │ CreateDescSetLayout()       │ 创建 set 1 描述符集布局           │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M0.5 材质系统 Agent A)  │
// ============================================================

#include "RenderCore/Material/FMaterialManager.h"
#include "RenderCore/Material/FBindlessTable.h"

namespace Limx
{

// 引入 Core 内存操作
using Memory::MemCopy;

// 日志类别定义
LIMX_DEFINE_LOG_CATEGORY(LogMaterialManager)

// ============================================================================
// Initialize — 创建默认纹理、采样器和描述符集布局
// ============================================================================

ERHIResult FMaterialManager::Initialize(
    IRHIDevice*    device,
    FRenderContext* context)
{
    LIMX_CHECK(device != nullptr);
    LIMX_CHECK(context != nullptr);
    LIMX_CHECK(!m_IsInitialized);

    m_Device  = device;
    m_Context = context;

    // 1. 创建默认 1×1 白色纹理
    ERHIResult result = CreateDefaultTexture();
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogMaterialManager, Error,
                 "[MaterialManager] 默认纹理创建失败");
        return result;
    }

    // 2. 创建默认线性采样器
    FRHISamplerDesc samplerDesc = FRHISamplerDesc::LinearRepeat();
    result = device->CreateSampler(samplerDesc, m_DefaultSampler);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogMaterialManager, Error,
                 "[MaterialManager] 默认采样器创建失败");
        return result;
    }

    // 3. 创建 set 1 描述符集布局
    result = CreateDescSetLayout();
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogMaterialManager, Error,
                 "[MaterialManager] 描述符集布局创建失败");
        return result;
    }

    m_IsInitialized = true;

    LIMX_LOG(LogMaterialManager, Log,
             "[MaterialManager] 初始化完成 — 默认纹理 1×1 白色 RGBA8 + 线性采样器 + set 1 布局就绪");

    return ERHIResult::Success;
}

// ============================================================================
// Shutdown — 按依赖逆序销毁全部资源
// ============================================================================

void FMaterialManager::Shutdown()
{
    if (!m_IsInitialized)
    {
        return;
    }

    // 1. 先销毁所有材质实例 (依赖材质的纹理视图)
    m_MaterialInstances.Clear();

    // 2. 再销毁所有材质 (持有 UBO 和描述符集)
    m_Materials.Clear();

    // 3. 销毁默认纹理资源
    if (m_Device != nullptr)
    {
        m_Device->DestroyTextureView(m_DefaultTextureView);
        m_Device->DestroyTexture(m_DefaultTexture);
        m_Device->DestroySampler(m_DefaultSampler);
        m_Device->DestroyDescSetLayout(m_DescSetLayout);
    }

    m_Device         = nullptr;
    m_Context        = nullptr;
    m_IsInitialized  = false;

    LIMX_LOG(LogMaterialManager, Log,
             "[MaterialManager] 已关闭，全部材质资源已释放");
}

// ============================================================================
// CreateMaterial — 创建并注册一个新 FMaterial
// ============================================================================

FMaterial* FMaterialManager::CreateMaterial(const AnsiChar* debugName)
{
    LIMX_CHECK(m_IsInitialized);

    TUniquePtr<FMaterial> material = MakeUnique<FMaterial>();

    ERHIResult result = material->Initialize(
        m_Device,
        m_DescSetLayout,
        m_DefaultTextureView,
        m_DefaultSampler,
        debugName);

    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogMaterialManager, Error,
                 "[MaterialManager] 材质 '{}' 创建失败", debugName);
        return nullptr;
    }

    FMaterial* rawPtr = material.Get();
    m_Materials.Add(MoveTemp(material));

    LIMX_LOG(LogMaterialManager, Log,
             "[MaterialManager] 材质 '{}' 已创建，当前材质总数: {}",
             debugName, m_Materials.GetSize());

    return rawPtr;
}

// ============================================================================
// CreateMaterialInstance — 创建并注册一个新 FMaterialInstance
// ============================================================================

FMaterialInstance* FMaterialManager::CreateMaterialInstance(
    FMaterial*      parent,
    const AnsiChar* debugName)
{
    LIMX_CHECK(m_IsInitialized);
    LIMX_CHECK(parent != nullptr);

    TUniquePtr<FMaterialInstance> instance = MakeUnique<FMaterialInstance>();

    ERHIResult result = instance->Initialize(
        parent,
        m_Device,
        m_DescSetLayout,
        m_DefaultTextureView,
        m_DefaultSampler,
        debugName);

    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogMaterialManager, Error,
                 "[MaterialManager] 材质实例 '{}' 创建失败", debugName);
        return nullptr;
    }

    FMaterialInstance* rawPtr = instance.Get();
    m_MaterialInstances.Add(MoveTemp(instance));

    LIMX_LOG(LogMaterialManager, Log,
             "[MaterialManager] 材质实例 '{}' 已创建 (父材质: '{}')",
             debugName, parent->GetDebugName());

    return rawPtr;
}

// ============================================================================
// DestroyMaterial — 从注册表移除并销毁指定材质
// ============================================================================

void FMaterialManager::DestroyMaterial(FMaterial* material)
{
    if (material == nullptr)
    {
        return;
    }

    for (SizeType i = 0; i < m_Materials.GetSize(); ++i)
    {
        if (m_Materials[i].Get() == material)
        {
            LIMX_LOG(LogMaterialManager, Log,
                     "[MaterialManager] 材质 '{}' 已销毁",
                     material->GetDebugName());

            // TUniquePtr 析构时自动调用 FMaterial::~FMaterial → Shutdown()
            m_Materials.RemoveAt(i);
            return;
        }
    }

    LIMX_LOG(LogMaterialManager, Warning,
             "[MaterialManager] DestroyMaterial: 材质指针未在注册表中找到");
}

// ============================================================================
// DestroyMaterialInstance — 从注册表移除并销毁指定材质实例
// ============================================================================

void FMaterialManager::DestroyMaterialInstance(FMaterialInstance* instance)
{
    if (instance == nullptr)
    {
        return;
    }

    for (SizeType i = 0; i < m_MaterialInstances.GetSize(); ++i)
    {
        if (m_MaterialInstances[i].Get() == instance)
        {
            LIMX_LOG(LogMaterialManager, Log,
                     "[MaterialManager] 材质实例 '{}' 已销毁",
                     instance->GetDebugName());

            m_MaterialInstances.RemoveAt(i);
            return;
        }
    }

    LIMX_LOG(LogMaterialManager, Warning,
             "[MaterialManager] DestroyMaterialInstance: 实例指针未在注册表中找到");
}

// ============================================================================
// CreateDefaultMaterial — 创建预设灰色 PBR 材质
// ============================================================================

FMaterial* FMaterialManager::CreateDefaultMaterial(const AnsiChar* debugName)
{
    FMaterial* material = CreateMaterial(debugName);
    if (material == nullptr)
    {
        return nullptr;
    }

    // 预设参数: 中性灰色, 非金属, 中等粗糙度
    material->SetBaseColor(FVector4(0.5f, 0.5f, 0.5f, 1.0f));
    material->SetMetallic(0.0f);
    material->SetRoughness(0.5f);
    material->SetAO(1.0f);
    material->SetNormalScale(1.0f);
    material->SetEmissiveColor(FVector3(0.0f, 0.0f, 0.0f));
    material->SetAlphaCutoff(0.5f);
    material->SetBlendMode(EMaterialBlendMode::Opaque);

    // 立即将参数上传到 GPU
    material->Flush();

    return material;
}

// ============================================================================
// UploadDirtyMaterials — 批量刷新所有脏材质和脏实例
// ============================================================================

void FMaterialManager::UploadDirtyMaterials(FBindlessTable* bindless)
{
    // 刷新所有脏材质
    for (SizeType i = 0; i < m_Materials.GetSize(); ++i)
    {
        const bool wasDirty = m_Materials[i]->IsDirty();

        if (wasDirty)
        {
            m_Materials[i]->Flush();
        }

        // bindless 注册。
        //
        // 未注册的一律注册; 已注册的只在脏了之后重新写一遍。前者保证
        // 新建的材质一定进表, 后者保证参数改动能生效, 而下标始终不变。
        if (bindless != nullptr &&
            (wasDirty || !m_Materials[i]->IsBindlessRegistered()))
        {
            m_Materials[i]->RegisterBindless(*bindless);
        }
    }

    // 刷新所有脏材质实例
    for (SizeType i = 0; i < m_MaterialInstances.GetSize(); ++i)
    {
        const bool wasDirty = m_MaterialInstances[i]->IsDirty();

        if (wasDirty)
        {
            m_MaterialInstances[i]->Flush();
        }

        // 材质实例暂不进 bindless 表。
        //
        // FMaterialInstance 是与 FMaterial 平行的独立类 (不是派生), 而且
        // 目前没有任何使用者 —— 场景加载走的是 FMaterial。等它真的被用起来
        // 时再接, 现在接等于给一条没人走的路写代码。
        (void)wasDirty;
    }
}

// ============================================================================
// CreateDefaultTexture — 创建 1×1 白色 RGBA8 纹理并上传到 GPU
// ============================================================================

ERHIResult FMaterialManager::CreateDefaultTexture()
{
    // ---- 创建 GPU 纹理 ----
    FRHITextureDesc texDesc = FRHITextureDesc::Texture2D(
        1u, 1u,
        EPixelFormat::RGBA8_UNORM,
        1u,
        ETextureUsage::Sampled | ETextureUsage::TransferDst);
    texDesc.DebugName = "DefaultWhiteTexture_1x1";

    ERHIResult result = m_Device->CreateTexture(texDesc, m_DefaultTexture);
    if (!IsRHISuccess(result))
    {
        return result;
    }

    // ---- 创建 Staging Buffer 并填充白色像素 ----
    // 1×1 RGBA8 = 4 字节
    static constexpr UInt64 kPixelDataSize = 4u;
    static const UInt8      kWhitePixel[4] = { 255u, 255u, 255u, 255u };

    FRHIBufferDesc stagingDesc = FRHIBufferDesc::Staging(kPixelDataSize);
    stagingDesc.DebugName = "DefaultTextureStagingBuffer";

    FRHIBufferHandle stagingBuffer;
    result = m_Device->CreateBuffer(stagingDesc, stagingBuffer);
    if (!IsRHISuccess(result))
    {
        m_Device->DestroyTexture(m_DefaultTexture);
        return result;
    }

    void* mappedPtr = nullptr;
    result = m_Device->MapBuffer(stagingBuffer, &mappedPtr);
    if (!IsRHISuccess(result))
    {
        m_Device->DestroyBuffer(stagingBuffer);
        m_Device->DestroyTexture(m_DefaultTexture);
        return result;
    }

    MemCopy(mappedPtr, kWhitePixel, kPixelDataSize);
    m_Device->UnmapBuffer(stagingBuffer);

    // ---- 通过一次性命令缓冲区上传纹理 ----
    IRHICommandBuffer* cmdBuffer = m_Context->BeginSingleTimeCommands();

    // 布局转换: Undefined → TransferDst
    cmdBuffer->TransitionImageLayout(
        m_DefaultTexture,
        EImageLayout::Undefined,
        EImageLayout::TransferDst,
        EPipelineStageFlags::TopOfPipe,
        EPipelineStageFlags::Transfer,
        EAccessFlags::None,
        EAccessFlags::TransferWrite);

    // 拷贝 Staging Buffer → 纹理
    FRHIBufferTextureCopyRegion copyRegion = {};
    copyRegion.BufferOffset      = 0u;
    copyRegion.BufferRowLength   = 0u;
    copyRegion.BufferImageHeight = 0u;
    copyRegion.MipLevel          = 0u;
    copyRegion.BaseLayer         = 0u;
    copyRegion.LayerCount        = 1u;
    copyRegion.TextureOffset     = { 0, 0, 0 };
    copyRegion.TextureExtent     = { 1u, 1u, 1u };

    cmdBuffer->CopyBufferToTexture(
        stagingBuffer,
        m_DefaultTexture,
        EImageLayout::TransferDst,
        copyRegion);

    // 布局转换: TransferDst → ShaderReadOnly
    cmdBuffer->TransitionImageLayout(
        m_DefaultTexture,
        EImageLayout::TransferDst,
        EImageLayout::ShaderReadOnly,
        EPipelineStageFlags::Transfer,
        EPipelineStageFlags::FragmentShader,
        EAccessFlags::TransferWrite,
        EAccessFlags::ShaderRead);

    m_Context->EndSingleTimeCommands(cmdBuffer);

    // 释放 Staging Buffer
    m_Device->DestroyBuffer(stagingBuffer);

    // ---- 创建纹理视图 ----
    FRHITextureViewDesc viewDesc = {};
    viewDesc.Texture         = m_DefaultTexture;
    viewDesc.ViewType        = ETextureType::Texture2D;
    viewDesc.Format          = EPixelFormat::RGBA8_UNORM;
    viewDesc.BaseMipLevel    = 0u;
    viewDesc.MipLevelCount   = 1u;
    viewDesc.BaseArrayLayer  = 0u;
    viewDesc.ArrayLayerCount = 1u;

    result = m_Device->CreateTextureView(viewDesc, m_DefaultTextureView);
    if (!IsRHISuccess(result))
    {
        m_Device->DestroyTexture(m_DefaultTexture);
        return result;
    }

    LIMX_LOG(LogMaterialManager, Log,
             "[MaterialManager] 默认 1×1 白色纹理创建完成");

    return ERHIResult::Success;
}

// ============================================================================
// CreateDescSetLayout — 创建 set 1 描述符集布局
//
// Layout:
//   binding 0 : UniformBuffer        (Fragment) — FMaterialParams UBO
//   binding 1 : CombinedImageSampler (Fragment) — Albedo
//   binding 2 : CombinedImageSampler (Fragment) — Normal
//   binding 3 : CombinedImageSampler (Fragment) — MetallicRoughness
//   binding 4 : CombinedImageSampler (Fragment) — Occlusion
//   binding 5 : CombinedImageSampler (Fragment) — Emissive
// ============================================================================

ERHIResult FMaterialManager::CreateDescSetLayout()
{
    // 总计 6 个绑定
    FRHIDescriptorBinding bindings[6] = {};

    // binding 0: 材质参数 UBO
    bindings[0].Binding    = 0u;
    bindings[0].Type       = EDescriptorType::UniformBuffer;
    bindings[0].Count      = 1u;
    bindings[0].StageFlags = EShaderStage::Fragment;

    // binding 1~5: 5 个纹理采样器
    for (UInt32 slot = 0; slot < kMaterialTextureSlotCount; ++slot)
    {
        bindings[1 + slot].Binding    = 1u + slot;
        bindings[1 + slot].Type       = EDescriptorType::CombinedImageSampler;
        bindings[1 + slot].Count      = 1u;
        bindings[1 + slot].StageFlags = EShaderStage::Fragment;
    }

    FRHIDescSetLayoutDesc layoutDesc = {};
    layoutDesc.Bindings     = bindings;
    layoutDesc.BindingCount = 6u;
    layoutDesc.DebugName    = "MaterialSet1DescSetLayout";

    ERHIResult result = m_Device->CreateDescSetLayout(layoutDesc, m_DescSetLayout);
    if (IsRHISuccess(result))
    {
        LIMX_LOG(LogMaterialManager, Log,
                 "[MaterialManager] set 1 描述符集布局创建完成 "
                 "(1 UBO + 5 CombinedImageSampler)");
    }

    return result;
}

} // namespace Limx
