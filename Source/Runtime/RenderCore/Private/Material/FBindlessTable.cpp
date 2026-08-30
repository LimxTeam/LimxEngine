/*******************************************************************************
 * 文件: FBindlessTable.cpp
 * 创建时间: 2026-08-31
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   FBindlessTable 的实现 — 描述符集布局、纹理注册、材质上传
 *
 ******************************************************************************/

#include "RenderCore/Material/FBindlessTable.h"

#include "Core/Logging/FLog.h"
#include "Core/Memory/MemoryOps.h"

namespace Limx
{

LIMX_DECLARE_LOG_CATEGORY(LogBindless)
LIMX_DEFINE_LOG_CATEGORY(LogBindless)

// ============================================================================
// 生命周期
// ============================================================================

ERHIResult FBindlessTable::Initialize(IRHIDevice*           device,
                                       UInt32                framesInFlight,
                                       FRHITextureViewHandle placeholderView,
                                       FRHISamplerHandle     placeholderSampler)
{
    if (device == nullptr || framesInFlight == 0 ||
        framesInFlight > kMaxFrames)
    {
        return ERHIResult::ErrorInvalidParameter;
    }

    m_Device         = device;
    m_FramesInFlight = framesInFlight;

    // ---- 描述符集布局 ----
    FRHIDescriptorBinding bindings[2] = {};

    bindings[0].Binding    = 0;
    bindings[0].Type       = EDescriptorType::StorageBuffer;
    bindings[0].Count      = 1;
    bindings[0].StageFlags = EShaderStage::Fragment | EShaderStage::Vertex;

    bindings[1].Binding    = 1;
    bindings[1].Type       = EDescriptorType::CombinedImageSampler;
    bindings[1].Count      = kMaxTextures;
    bindings[1].StageFlags = EShaderStage::Fragment;

    // 数组必然是稀疏的 —— 声明 1024 个槽位而场景只用几十个。
    bindings[1].PartiallyBound = true;

    // 场景加载时逐张注册, 而那时描述符集可能已经被绑过。
    bindings[1].UpdateAfterBind = true;

    FRHIDescSetLayoutDesc layoutDesc = {};
    layoutDesc.Bindings     = bindings;
    layoutDesc.BindingCount = 2;
    layoutDesc.DebugName    = "BindlessSetLayout";

    ERHIResult result = device->CreateDescSetLayout(layoutDesc, m_Layout);

    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogBindless, Error, "[Bindless] 描述符集布局创建失败");
        return result;
    }

    // ---- 每帧一份描述符集与材质缓冲区 ----
    for (UInt32 f = 0; f < framesInFlight; ++f)
    {
        result = device->AllocateDescriptorSet(m_Layout, m_Sets[f]);

        if (!IsRHISuccess(result))
        {
            LIMX_LOG(LogBindless, Error, "[Bindless] 第 {} 帧描述符集分配失败", f);
            return result;
        }

        FRHIBufferDesc bufferDesc = FRHIBufferDesc::Storage(
            sizeof(FBindlessMaterial) * kMaxMaterials,
            EMemoryUsage::CpuToGpu);
        bufferDesc.DebugName = "BindlessMaterialBuffer";

        result = device->CreateBuffer(bufferDesc, m_MaterialBuffers[f]);

        if (!IsRHISuccess(result))
        {
            LIMX_LOG(LogBindless, Error, "[Bindless] 第 {} 帧材质缓冲区创建失败", f);
            return result;
        }

        // 常驻映射 —— 材质数据每帧整体重写, 逐帧 Map/Unmap 是纯开销
        result = device->MapBuffer(m_MaterialBuffers[f], &m_MaterialMapped[f]);

        if (!IsRHISuccess(result))
        {
            return result;
        }

        FRHIDescriptorWrite write = {};
        write.DescriptorSet = m_Sets[f];
        write.Binding       = 0;
        write.Type          = EDescriptorType::StorageBuffer;
        write.Buffer        = m_MaterialBuffers[f];
        write.BufferOffset  = 0;
        write.BufferRange   = 0;

        device->UpdateDescriptorSets(&write, 1);
    }

    // ---- 占位纹理占据 0 号槽位 ----
    //
    // 必须是第一个注册的。缺贴图的材质无条件指向 0 号, 着色器不做有效性
    // 判断 —— 那个判断一旦漏写就是随机读显存, 而"0 号永远有效"这个不变量
    // 在这里一次性建立。
    const UInt32 placeholder =
        RegisterTexture(placeholderView, placeholderSampler);

    if (placeholder != kPlaceholderTexture)
    {
        LIMX_LOG(LogBindless, Error,
                 "[Bindless] 占位纹理没有落在 0 号槽位 (实际 {})", placeholder);
        return ERHIResult::ErrorUnknown;
    }

    LIMX_LOG(LogBindless, Display,
             "[Bindless] 已就绪 — 纹理表 {} 槽 / 材质表 {} 项 / {} 帧",
             kMaxTextures, kMaxMaterials, framesInFlight);

    return ERHIResult::Success;
}

void FBindlessTable::Shutdown(IRHIDevice* device)
{
    if (device == nullptr)
    {
        m_Device = nullptr;
        return;
    }

    for (UInt32 f = 0; f < m_FramesInFlight; ++f)
    {
        if (m_MaterialMapped[f] != nullptr)
        {
            device->UnmapBuffer(m_MaterialBuffers[f]);
            m_MaterialMapped[f] = nullptr;
        }

        if (m_MaterialBuffers[f].IsValid())
        {
            device->DestroyBuffer(m_MaterialBuffers[f]);
        }

        if (m_Sets[f].IsValid())
        {
            device->FreeDescriptorSet(m_Sets[f]);
        }
    }

    if (m_Layout.IsValid())
    {
        device->DestroyDescSetLayout(m_Layout);
    }

    m_Materials.Clear();
    m_TextureKeys.Clear();
    m_TextureCount = 0;
    m_Device       = nullptr;
}

// ============================================================================
// 注册
// ============================================================================

UInt32 FBindlessTable::RegisterTexture(FRHITextureViewHandle view,
                                        FRHISamplerHandle     sampler)
{
    if (m_Device == nullptr || !view.IsValid() || !sampler.IsValid())
    {
        return kPlaceholderTexture;
    }

    // 去重 —— 同一张贴图被多个材质引用是常态
    for (SizeType i = 0; i < m_TextureKeys.GetSize(); ++i)
    {
        if (m_TextureKeys[i].View.Packed == view.Packed &&
            m_TextureKeys[i].Sampler.Packed == sampler.Packed)
        {
            return static_cast<UInt32>(i);
        }
    }

    if (m_TextureCount >= kMaxTextures)
    {
        // 表满时退回占位纹理而不是返回无效值。
        //
        // 无效值要求每个使用处都判断, 漏判一处就是随机读显存; 而退回占位
        // 纹理的后果只是那个物体看起来不对 —— 可见, 可查, 不会崩。
        LIMX_LOG(LogBindless, Warning,
                 "[Bindless] 纹理表已满 ({} 槽), 退回占位纹理", kMaxTextures);
        return kPlaceholderTexture;
    }

    const UInt32 index = m_TextureCount;
    ++m_TextureCount;

    FTextureKey key;
    key.View    = view;
    key.Sampler = sampler;
    m_TextureKeys.Add(key);

    // 每一帧的描述符集都要写一遍 —— 它们指向同一张纹理, 但描述符集本身
    // 不能跨帧共享。
    for (UInt32 f = 0; f < m_FramesInFlight; ++f)
    {
        FRHIDescriptorWrite write = {};
        write.DescriptorSet = m_Sets[f];
        write.Binding       = 1;
        write.ArrayElement  = index;
        write.Type          = EDescriptorType::CombinedImageSampler;
        write.ImageView     = view;
        write.Sampler       = sampler;
        write.ImageLayout   = EImageLayout::ShaderReadOnly;

        m_Device->UpdateDescriptorSets(&write, 1);
    }

    return index;
}

UInt32 FBindlessTable::RegisterMaterial(const FBindlessMaterial& material)
{
    if (m_Materials.GetSize() >= kMaxMaterials)
    {
        LIMX_LOG(LogBindless, Warning,
                 "[Bindless] 材质表已满 ({} 项)", kMaxMaterials);
        return 0;
    }

    const UInt32 index = static_cast<UInt32>(m_Materials.GetSize());

    m_Materials.Add(material);

    return index;
}

void FBindlessTable::UpdateMaterial(UInt32 index,
                                     const FBindlessMaterial& material)
{
    if (index < m_Materials.GetSize())
    {
        m_Materials[index] = material;
    }
}

void FBindlessTable::Reset()
{
    m_Materials.Clear();

    // 纹理计数保留 0 号占位 —— 它在 Initialize 时写入, 关卡切换不该动它。
    m_TextureCount = 1;

    // 去重表同样只留占位那一项
    while (m_TextureKeys.GetSize() > 1)
    {
        m_TextureKeys.RemoveAt(m_TextureKeys.GetSize() - 1);
    }
}

// ============================================================================
// 逐帧
// ============================================================================

void FBindlessTable::Upload(UInt32 frameIndex)
{
    if (m_Device == nullptr || m_Materials.IsEmpty())
    {
        return;
    }

    const UInt32 slot = frameIndex % m_FramesInFlight;

    void* mapped = m_MaterialMapped[slot];

    if (mapped == nullptr)
    {
        return;
    }

    // 整体重写而非增量。
    //
    // Sponza 是 25 个材质 x 80 字节 = 2 KB; 为省这 2 KB 引入脏区间管理,
    // 换来的是一类"某帧材质没更新"的时序 bug —— 那种 bug 只在材质刚好在
    // 帧边界上改动时出现。
    Memory::MemCopy(mapped, m_Materials.GetData(),
                    m_Materials.GetSize() * sizeof(FBindlessMaterial));
}

} // namespace Limx
