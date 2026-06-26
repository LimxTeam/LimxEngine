// ============================================================
// 文件名称：FMaterial.cpp
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：GPU 资源最小化上传 — 通过脏标记机制确保 UBO 数据只在
//          参数真正变化时才写入 GPU，避免无效的 CPU→GPU 带宽消耗。
//          描述符集只在纹理绑定或首次初始化时重新写入。
// 功能描述：FMaterial 完整实现 — 创建/释放 GPU UBO 缓冲区和 set 1
//          描述符集，处理 PBR 参数 setter、纹理绑定/解绑，
//          执行脏标记驱动的 Flush() 上传逻辑。
// 技术特性：UBO 使用 CpuToGpu 内存，每次上传通过 MapBuffer+MemCopy+UnmapBuffer；
//          UpdateDescriptorSet 一次写入 6 个绑定 (1 个 UBO + 5 个纹理)；
//          每个纹理 setter 同步更新 TextureFlags 掩码位。
//
// ── 函数表 ──────────────────────────────────────────────────
// │ 函数名                   │ 描述                            │
// │─────────────────────────│────────────────────────────────│
// │ ~FMaterial()            │ 析构时自动调用 Shutdown           │
// │ Initialize()            │ 创建 UBO + 分配描述符集 + 写入绑定 │
// │ Shutdown()              │ 释放 UBO + 描述符集              │
// │ SetBaseColor()          │ 设置基色, 标脏                  │
// │ SetMetallic()           │ 设置金属度, 标脏                 │
// │ SetRoughness()          │ 设置粗糙度, 标脏                 │
// │ SetAO()                 │ 设置 AO, 标脏                  │
// │ SetNormalScale()        │ 设置法线强度, 标脏               │
// │ SetEmissiveColor()      │ 设置自发光, 标脏                 │
// │ SetAlphaCutoff()        │ 设置 Alpha 裁剪阈值, 标脏         │
// │ SetBlendMode()          │ 设置混合模式, 标脏               │
// │ BindTexture()           │ 绑定纹理 + 设置 TextureFlags, 标脏│
// │ UnbindTexture()         │ 解绑纹理 + 清除 TextureFlags, 标脏│
// │ Flush()                 │ 若脏则上传 UBO + 更新描述符集     │
// │ GetTextureView()        │ 获取指定槽位纹理视图              │
// │ GetSampler()            │ 获取指定槽位采样器               │
// │ UploadParams()          │ MapBuffer + MemCopy + Unmap     │
// │ UpdateDescriptorSet()   │ 写入 6 个描述符绑定              │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                            │
// │─────────────│──────────│────────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M0.5 材质系统 Agent A)  │
// ============================================================

#include "RenderCore/Material/FMaterial.h"

namespace Limx
{

// 引入 Core 内存操作
using Memory::MemCopy;

// 日志类别
LIMX_DECLARE_LOG_CATEGORY(LogMaterial)
LIMX_DEFINE_LOG_CATEGORY(LogMaterial)

// ============================================================================
// 析构
// ============================================================================

FMaterial::~FMaterial()
{
    Shutdown();
}

// ============================================================================
// Initialize — 创建 GPU 资源并写入初始描述符绑定
// ============================================================================

ERHIResult FMaterial::Initialize(
    IRHIDevice*             device,
    FRHIDescSetLayoutHandle descSetLayout,
    FRHITextureViewHandle   defaultView,
    FRHISamplerHandle       defaultSampler,
    const AnsiChar*         debugName)
{
    LIMX_CHECK(device != nullptr);

    m_Device          = device;
    m_DebugName       = debugName;
    m_DefaultTextureView = defaultView;
    m_DefaultSampler     = defaultSampler;

    // 所有槽位初始化为默认纹理
    for (UInt32 slot = 0; slot < kMaterialTextureSlotCount; ++slot)
    {
        m_TextureViews[slot] = defaultView;
        m_Samplers[slot]     = defaultSampler;
    }

    // ---- 创建材质参数 UBO (CpuToGpu, 64 字节) ----
    FRHIBufferDesc uboDesc = FRHIBufferDesc::Uniform(
        static_cast<UInt64>(sizeof(FMaterialParams)));
    uboDesc.DebugName = debugName;

    ERHIResult result = device->CreateBuffer(uboDesc, m_ParamsUBO);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogMaterial, Error,
                 "[Material] 材质 '{}' UBO 创建失败", debugName);
        return result;
    }

    // ---- 分配 set 1 描述符集 ----
    result = device->AllocateDescriptorSet(descSetLayout, m_DescriptorSet);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogMaterial, Error,
                 "[Material] 材质 '{}' 描述符集分配失败", debugName);
        device->DestroyBuffer(m_ParamsUBO);
        return result;
    }

    // 初始化时强制上传 UBO + 写入描述符集
    m_IsDirty = true;
    Flush();

    LIMX_LOG(LogMaterial, Log,
             "[Material] 材质 '{}' 初始化完成", debugName);

    return ERHIResult::Success;
}

// ============================================================================
// Shutdown — 释放 GPU 资源
// ============================================================================

void FMaterial::Shutdown()
{
    if (m_Device == nullptr)
    {
        return;
    }

    m_Device->FreeDescriptorSet(m_DescriptorSet);
    m_Device->DestroyBuffer(m_ParamsUBO);

    m_Device  = nullptr;
    m_IsDirty = false;
}

// ============================================================================
// PBR 参数 Setter — 每个 setter 更新 CPU 侧参数并标脏
// ============================================================================

void FMaterial::SetBaseColor(const FVector4& color)
{
    m_Params.BaseColor = color;
    m_IsDirty = true;
}

void FMaterial::SetMetallic(Float32 metallic)
{
    m_Params.Metallic = FMath::Clamp(metallic, 0.0f, 1.0f);
    m_IsDirty = true;
}

void FMaterial::SetRoughness(Float32 roughness)
{
    m_Params.Roughness = FMath::Clamp(roughness, 0.0f, 1.0f);
    m_IsDirty = true;
}

void FMaterial::SetAO(Float32 ao)
{
    m_Params.AO = FMath::Clamp(ao, 0.0f, 1.0f);
    m_IsDirty = true;
}

void FMaterial::SetNormalScale(Float32 normalScale)
{
    m_Params.NormalScale = FMath::Max(0.0f, normalScale);
    m_IsDirty = true;
}

void FMaterial::SetEmissiveColor(const FVector3& color)
{
    m_Params.EmissiveColor.X = color.X;
    m_Params.EmissiveColor.Y = color.Y;
    m_Params.EmissiveColor.Z = color.Z;
    m_Params.EmissiveColor.W = 0.0f;
    m_IsDirty = true;
}

void FMaterial::SetAlphaCutoff(Float32 cutoff)
{
    m_Params.AlphaCutoff = FMath::Clamp(cutoff, 0.0f, 1.0f);
    m_IsDirty = true;
}

void FMaterial::SetBlendMode(EMaterialBlendMode mode)
{
    m_Params.BlendMode = static_cast<UInt32>(mode);
    m_IsDirty = true;
}

// ============================================================================
// BindTexture — 绑定纹理到指定槽位
// ============================================================================

void FMaterial::BindTexture(
    UInt32               slot,
    FRHITextureViewHandle view,
    FRHISamplerHandle    sampler)
{
    LIMX_CHECK(slot < kMaterialTextureSlotCount);

    m_TextureViews[slot] = view;
    m_Samplers[slot]     = sampler;

    // 设置对应的纹理标志位
    m_Params.TextureFlags |= (1u << slot);

    m_IsDirty = true;
}

// ============================================================================
// UnbindTexture — 解绑指定槽位，回退到默认白色纹理
// ============================================================================

void FMaterial::UnbindTexture(UInt32 slot)
{
    LIMX_CHECK(slot < kMaterialTextureSlotCount);

    m_TextureViews[slot] = m_DefaultTextureView;
    m_Samplers[slot]     = m_DefaultSampler;

    // 清除对应的纹理标志位
    m_Params.TextureFlags &= ~(1u << slot);

    m_IsDirty = true;
}

// ============================================================================
// Flush — 若材质已脏则执行 GPU 上传
// ============================================================================

void FMaterial::Flush()
{
    if (!m_IsDirty)
    {
        return;
    }

    UploadParams();
    UpdateDescriptorSet();

    m_IsDirty = false;
}

// ============================================================================
// GetTextureView — 获取指定槽位的纹理视图 (含默认回退)
// ============================================================================

FRHITextureViewHandle FMaterial::GetTextureView(UInt32 slot) const
{
    LIMX_CHECK(slot < kMaterialTextureSlotCount);
    return m_TextureViews[slot];
}

// ============================================================================
// GetSampler — 获取指定槽位的采样器 (含默认回退)
// ============================================================================

FRHISamplerHandle FMaterial::GetSampler(UInt32 slot) const
{
    LIMX_CHECK(slot < kMaterialTextureSlotCount);
    return m_Samplers[slot];
}

// ============================================================================
// UploadParams — 将 CPU 侧 FMaterialParams 写入 GPU UBO
// ============================================================================

void FMaterial::UploadParams()
{
    if (m_Device == nullptr)
    {
        return;
    }

    void* mappedPtr = nullptr;
    ERHIResult result = m_Device->MapBuffer(m_ParamsUBO, &mappedPtr);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogMaterial, Error,
                 "[Material] 材质 '{}' UBO 映射失败", m_DebugName);
        return;
    }

    MemCopy(mappedPtr, &m_Params, sizeof(FMaterialParams));

    m_Device->UnmapBuffer(m_ParamsUBO);
}

// ============================================================================
// UpdateDescriptorSet — 写入 set 1 所有描述符绑定
//
// binding 0 : UniformBuffer       — FMaterialParams UBO
// binding 1 : CombinedImageSampler — Albedo 纹理
// binding 2 : CombinedImageSampler — Normal 纹理
// binding 3 : CombinedImageSampler — MetallicRoughness 纹理
// binding 4 : CombinedImageSampler — Occlusion 纹理
// binding 5 : CombinedImageSampler — Emissive 纹理
// ============================================================================

void FMaterial::UpdateDescriptorSet()
{
    if (m_Device == nullptr)
    {
        return;
    }

    // 总计 6 个写入项: 1 个 UBO + 5 个纹理
    FRHIDescriptorWrite writes[6] = {};

    // binding 0: 材质参数 UBO
    writes[0] = FRHIDescriptorWrite::UniformBuffer(
        m_DescriptorSet,
        0,
        m_ParamsUBO,
        0,
        static_cast<UInt64>(sizeof(FMaterialParams)));

    // binding 1~5: 5 个纹理槽位 (未绑定时使用默认白色纹理)
    for (UInt32 slot = 0; slot < kMaterialTextureSlotCount; ++slot)
    {
        writes[1 + slot] = FRHIDescriptorWrite::CombinedImageSampler(
            m_DescriptorSet,
            1u + slot,
            m_TextureViews[slot],
            m_Samplers[slot],
            EImageLayout::ShaderReadOnly);
    }

    m_Device->UpdateDescriptorSets(writes, 6u);
}

} // namespace Limx
