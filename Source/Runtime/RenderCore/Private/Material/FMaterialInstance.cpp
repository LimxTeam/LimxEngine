// ============================================================
// 文件名称：FMaterialInstance.cpp
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：合并优先 — Flush() 时先在 CPU 端合并父参数与实例覆盖，
//          再一次性写入 GPU UBO，避免部分更新导致的一致性问题。
//          纹理直接从父材质引用，实例不持有任何纹理资源的所有权。
// 功能描述：FMaterialInstance 完整实现 — 初始化/关闭 GPU 资源，
//          处理参数覆盖 setter，实现合并逻辑 (GetMergedParams)，
//          执行 Flush() 驱动的 GPU UBO 上传和描述符集更新。
// 技术特性：GetMergedParams() 在 CPU 端按位掩码逐字段合并；
//          UpdateDescriptorSet 纹理槽位全部来自父材质 GetTextureView()/GetSampler()；
//          UBO 使用 CpuToGpu 内存，MapBuffer+MemCopy+UnmapBuffer。
//
// ── 函数表 ──────────────────────────────────────────────────
// │ 函数名                      │ 描述                           │
// │────────────────────────────│───────────────────────────────│
// │ ~FMaterialInstance()       │ 析构时调用 Shutdown             │
// │ Initialize()               │ 创建 UBO + 分配描述符集 + 首次刷新│
// │ Shutdown()                 │ 释放 UBO + 描述符集             │
// │ SetOverrideBaseColor()     │ 覆盖基色, 启用位, 标脏          │
// │ SetOverrideMetallic()      │ 覆盖金属度, 启用位, 标脏         │
// │ SetOverrideRoughness()     │ 覆盖粗糙度, 启用位, 标脏         │
// │ SetOverrideAO()            │ 覆盖 AO, 启用位, 标脏           │
// │ SetOverrideNormalScale()   │ 覆盖法线强度, 启用位, 标脏        │
// │ SetOverrideEmissiveColor() │ 覆盖自发光, 启用位, 标脏         │
// │ SetOverrideAlphaCutoff()   │ 覆盖 Alpha 裁剪阈值, 启用位, 标脏 │
// │ SetOverrideBlendMode()     │ 覆盖混合模式, 启用位, 标脏        │
// │ ClearOverride()            │ 清除指定覆盖位, 标脏             │
// │ ClearAllOverrides()        │ 清除全部覆盖位, 标脏             │
// │ Flush()                    │ 合并参数 + 上传 UBO + 更新描述符  │
// │ GetMergedParams()          │ CPU 端合并父参数 + 覆盖参数       │
// │ UploadMergedParams()       │ 写入合并参数到 GPU UBO           │
// │ UpdateDescriptorSet()      │ 写入 UBO + 父材质纹理到描述符集   │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M0.5 材质系统 Agent A)  │
// ============================================================

#include "RenderCore/Material/FMaterialInstance.h"

namespace Limx
{

// 引入 Core 内存操作
using Memory::MemCopy;

// 日志类别 (复用 FMaterial.cpp 中定义的 LogMaterial)
LIMX_DECLARE_LOG_CATEGORY(LogMaterial)

// ============================================================================
// 析构
// ============================================================================

FMaterialInstance::~FMaterialInstance()
{
    Shutdown();
}

// ============================================================================
// Initialize — 创建 GPU 资源并执行首次刷新
// ============================================================================

ERHIResult FMaterialInstance::Initialize(
    FMaterial*              parent,
    IRHIDevice*             device,
    FRHIDescSetLayoutHandle descSetLayout,
    FRHITextureViewHandle   defaultView,
    FRHISamplerHandle       defaultSampler,
    const AnsiChar*         debugName)
{
    LIMX_CHECK(parent != nullptr);
    LIMX_CHECK(device != nullptr);

    m_Parent             = parent;
    m_Device             = device;
    m_DebugName          = debugName;
    m_DefaultTextureView = defaultView;
    m_DefaultSampler     = defaultSampler;
    m_OverrideMask       = EMaterialParamOverride::None;

    // 用父材质的当前参数初始化覆盖参数（作为参考基准，实际合并以父材质为准）
    m_OverrideParams = parent->GetParams();

    // ---- 创建实例独立的 GPU UBO ----
    FRHIBufferDesc uboDesc = FRHIBufferDesc::Uniform(
        static_cast<UInt64>(sizeof(FMaterialParams)));
    uboDesc.DebugName = debugName;

    ERHIResult result = device->CreateBuffer(uboDesc, m_ParamsUBO);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogMaterial, Error,
                 "[MaterialInstance] 实例 '{}' UBO 创建失败", debugName);
        return result;
    }

    // ---- 分配实例独立的 set 1 描述符集 ----
    result = device->AllocateDescriptorSet(descSetLayout, m_DescriptorSet);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogMaterial, Error,
                 "[MaterialInstance] 实例 '{}' 描述符集分配失败", debugName);
        device->DestroyBuffer(m_ParamsUBO);
        return result;
    }

    // 首次强制刷新 (上传父材质参数 + 写入描述符集绑定)
    m_IsDirty = true;
    Flush();

    LIMX_LOG(LogMaterial, Log,
             "[MaterialInstance] 实例 '{}' 初始化完成 (父材质: '{}')",
             debugName, parent->GetDebugName());

    return ERHIResult::Success;
}

// ============================================================================
// Shutdown — 释放 GPU 资源
// ============================================================================

void FMaterialInstance::Shutdown()
{
    if (m_Device == nullptr)
    {
        return;
    }

    m_Device->FreeDescriptorSet(m_DescriptorSet);
    m_Device->DestroyBuffer(m_ParamsUBO);

    m_Device  = nullptr;
    m_Parent  = nullptr;
    m_IsDirty = false;
}

// ============================================================================
// 参数覆盖 Setter
// ============================================================================

void FMaterialInstance::SetOverrideBaseColor(const FVector4& color)
{
    m_OverrideParams.BaseColor = color;
    m_OverrideMask |= EMaterialParamOverride::BaseColor;
    m_IsDirty = true;
}

void FMaterialInstance::SetOverrideMetallic(Float32 metallic)
{
    m_OverrideParams.Metallic = FMath::Clamp(metallic, 0.0f, 1.0f);
    m_OverrideMask |= EMaterialParamOverride::Metallic;
    m_IsDirty = true;
}

void FMaterialInstance::SetOverrideRoughness(Float32 roughness)
{
    m_OverrideParams.Roughness = FMath::Clamp(roughness, 0.0f, 1.0f);
    m_OverrideMask |= EMaterialParamOverride::Roughness;
    m_IsDirty = true;
}

void FMaterialInstance::SetOverrideAO(Float32 ao)
{
    m_OverrideParams.AO = FMath::Clamp(ao, 0.0f, 1.0f);
    m_OverrideMask |= EMaterialParamOverride::AO;
    m_IsDirty = true;
}

void FMaterialInstance::SetOverrideNormalScale(Float32 normalScale)
{
    m_OverrideParams.NormalScale = FMath::Max(0.0f, normalScale);
    m_OverrideMask |= EMaterialParamOverride::NormalScale;
    m_IsDirty = true;
}

void FMaterialInstance::SetOverrideEmissiveColor(const FVector3& color)
{
    m_OverrideParams.EmissiveColor.X = color.X;
    m_OverrideParams.EmissiveColor.Y = color.Y;
    m_OverrideParams.EmissiveColor.Z = color.Z;
    m_OverrideParams.EmissiveColor.W = 0.0f;
    m_OverrideMask |= EMaterialParamOverride::EmissiveColor;
    m_IsDirty = true;
}

void FMaterialInstance::SetOverrideAlphaCutoff(Float32 cutoff)
{
    m_OverrideParams.AlphaCutoff = FMath::Clamp(cutoff, 0.0f, 1.0f);
    m_OverrideMask |= EMaterialParamOverride::AlphaCutoff;
    m_IsDirty = true;
}

void FMaterialInstance::SetOverrideBlendMode(EMaterialBlendMode mode)
{
    m_OverrideParams.BlendMode = static_cast<UInt32>(mode);
    m_OverrideMask |= EMaterialParamOverride::BlendMode;
    m_IsDirty = true;
}

// ============================================================================
// ClearOverride — 清除指定参数的覆盖位
// ============================================================================

void FMaterialInstance::ClearOverride(EMaterialParamOverride overrideFlag)
{
    m_OverrideMask &= ~overrideFlag;
    m_IsDirty = true;
}

// ============================================================================
// ClearAllOverrides — 清除全部覆盖
// ============================================================================

void FMaterialInstance::ClearAllOverrides()
{
    m_OverrideMask = EMaterialParamOverride::None;
    m_IsDirty = true;
}

// ============================================================================
// Flush — 若实例已脏则合并参数并上传 GPU
// ============================================================================

void FMaterialInstance::Flush()
{
    if (!m_IsDirty)
    {
        return;
    }

    UploadMergedParams();
    UpdateDescriptorSet();

    m_IsDirty = false;
}

// ============================================================================
// GetMergedParams — CPU 端合并父参数 + 实例覆盖
// ============================================================================

FMaterialParams FMaterialInstance::GetMergedParams() const
{
    LIMX_CHECK(m_Parent != nullptr);

    // 以父材质参数为基础
    FMaterialParams merged = m_Parent->GetParams();

    // 逐字段按覆盖掩码选择实例值
    if ((m_OverrideMask & EMaterialParamOverride::BaseColor) !=
        EMaterialParamOverride::None)
    {
        merged.BaseColor = m_OverrideParams.BaseColor;
    }

    if ((m_OverrideMask & EMaterialParamOverride::Metallic) !=
        EMaterialParamOverride::None)
    {
        merged.Metallic = m_OverrideParams.Metallic;
    }

    if ((m_OverrideMask & EMaterialParamOverride::Roughness) !=
        EMaterialParamOverride::None)
    {
        merged.Roughness = m_OverrideParams.Roughness;
    }

    if ((m_OverrideMask & EMaterialParamOverride::AO) !=
        EMaterialParamOverride::None)
    {
        merged.AO = m_OverrideParams.AO;
    }

    if ((m_OverrideMask & EMaterialParamOverride::NormalScale) !=
        EMaterialParamOverride::None)
    {
        merged.NormalScale = m_OverrideParams.NormalScale;
    }

    if ((m_OverrideMask & EMaterialParamOverride::EmissiveColor) !=
        EMaterialParamOverride::None)
    {
        merged.EmissiveColor = m_OverrideParams.EmissiveColor;
    }

    if ((m_OverrideMask & EMaterialParamOverride::AlphaCutoff) !=
        EMaterialParamOverride::None)
    {
        merged.AlphaCutoff = m_OverrideParams.AlphaCutoff;
    }

    if ((m_OverrideMask & EMaterialParamOverride::BlendMode) !=
        EMaterialParamOverride::None)
    {
        merged.BlendMode = m_OverrideParams.BlendMode;
    }

    // TextureFlags 始终来自父材质 (实例不持有纹理)
    merged.TextureFlags = m_Parent->GetParams().TextureFlags;

    return merged;
}

// ============================================================================
// UploadMergedParams — 将合并参数写入实例 GPU UBO
// ============================================================================

void FMaterialInstance::UploadMergedParams()
{
    if (m_Device == nullptr || m_Parent == nullptr)
    {
        return;
    }

    FMaterialParams merged = GetMergedParams();

    void* mappedPtr = nullptr;
    ERHIResult result = m_Device->MapBuffer(m_ParamsUBO, &mappedPtr);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogMaterial, Error,
                 "[MaterialInstance] 实例 '{}' UBO 映射失败", m_DebugName);
        return;
    }

    MemCopy(mappedPtr, &merged, sizeof(FMaterialParams));

    m_Device->UnmapBuffer(m_ParamsUBO);
}

// ============================================================================
// UpdateDescriptorSet — 写入 set 1 描述符绑定
//
// binding 0: UniformBuffer       — 实例自身 UBO (合并参数)
// binding 1~5: CombinedImageSampler — 继承自父材质的 5 个纹理槽位
// ============================================================================

void FMaterialInstance::UpdateDescriptorSet()
{
    if (m_Device == nullptr || m_Parent == nullptr)
    {
        return;
    }

    // 6 个写入项: 1 个 UBO + 5 个从父材质继承的纹理槽位
    FRHIDescriptorWrite writes[6] = {};

    // binding 0: 实例合并参数 UBO
    writes[0] = FRHIDescriptorWrite::UniformBuffer(
        m_DescriptorSet,
        0,
        m_ParamsUBO,
        0,
        static_cast<UInt64>(sizeof(FMaterialParams)));

    // binding 1~5: 直接引用父材质当前绑定的纹理视图和采样器
    for (UInt32 slot = 0; slot < kMaterialTextureSlotCount; ++slot)
    {
        writes[1 + slot] = FRHIDescriptorWrite::CombinedImageSampler(
            m_DescriptorSet,
            1u + slot,
            m_Parent->GetTextureView(slot),
            m_Parent->GetSampler(slot),
            EImageLayout::ShaderReadOnly);
    }

    m_Device->UpdateDescriptorSets(writes, 6u);
}

} // namespace Limx
