// ============================================================
// 文件名称：FMaterialInstance.h
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：参数化变体 — 材质实例持有对父材质的非拥有引用，
//          通过覆盖掩码 (EMaterialParamOverride) 选择性地覆盖父材质的
//          PBR 标量参数，生成合并后的最终参数上传到独立的 GPU UBO。
//          纹理绑定直接复用父材质的槽位视图/采样器，不额外持有纹理资源。
//          此设计允许同一父材质派生出大量低开销的参数变体（颜色/金属度变化等），
//          而无需为每个变体完整分配所有 PBR 纹理资源。
// 功能描述：材质实例类 — 引用父材质，用覆盖掩码选择性覆盖 PBR 参数，
//          生成合并参数并上传到自身独立的 GPU UBO + set 1 描述符集。
// 技术特性：EMaterialParamOverride 枚举位掩码控制覆盖字段；
//          GetMergedParams() 在 CPU 端合并父参数 + 实例覆盖；
//          实例拥有独立的 UBO 缓冲区和描述符集 (set 1)；
//          纹理绑定直接引用父材质的 GetTextureView()/GetSampler() 结果；
//          Flush() 仅在 m_IsDirty 时重新合并并上传。
//
// ── 枚举表 ──────────────────────────────────────────────────
// │ 枚举名                     │ 描述                                │
// │───────────────────────────│────────────────────────────────────│
// │ EMaterialParamOverride    │ 可覆盖参数位掩码                     │
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                     │ 描述                                │
// │───────────────────────────│────────────────────────────────────│
// │ Initialize()              │ 创建 UBO + 分配描述符集 + 写入初始绑定│
// │ Shutdown()                │ 释放 UBO + 描述符集                 │
// │ SetOverrideBaseColor()    │ 覆盖基色 (启用 BaseColor 覆盖位)      │
// │ SetOverrideMetallic()     │ 覆盖金属度                           │
// │ SetOverrideRoughness()    │ 覆盖粗糙度                           │
// │ SetOverrideAO()           │ 覆盖 AO                             │
// │ SetOverrideNormalScale()  │ 覆盖法线强度                         │
// │ SetOverrideEmissiveColor()│ 覆盖自发光颜色                       │
// │ SetOverrideAlphaCutoff()  │ 覆盖 Alpha 裁剪阈值                  │
// │ SetOverrideBlendMode()    │ 覆盖混合模式                         │
// │ ClearOverride()           │ 清除指定参数的覆盖 (回退父材质值)      │
// │ ClearAllOverrides()       │ 清除全部覆盖                         │
// │ Flush()                   │ 若脏则合并参数 + 上传 UBO + 刷新描述符│
// │ GetDescriptorSet()        │ 获取 set 1 描述符集句柄               │
// │ GetMergedParams()         │ 计算父参数 + 覆盖的最终合并参数        │
// │ GetParent()               │ 获取父材质指针                        │
// │ IsDirty()                 │ 是否需要上传 GPU                     │
//
// ── 结构体字段表 ──────────────────────────────────────────────
// │ 字段名                     │ 类型                    │ 描述        │
// │───────────────────────────│───────────────────────│────────────│
// │ m_Parent                  │ FMaterial*             │ 父材质(非拥有)│
// │ m_OverrideParams          │ FMaterialParams        │ 覆盖参数数据  │
// │ m_OverrideMask            │ EMaterialParamOverride │ 覆盖位掩码   │
// │ m_ParamsUBO               │ FRHIBufferHandle       │ 实例 GPU UBO│
// │ m_DescriptorSet           │ FRHIDescriptorSetHandle│ 实例描述符集  │
// │ m_DefaultTextureView      │ FRHITextureViewHandle  │ 默认纹理视图  │
// │ m_DefaultSampler          │ FRHISamplerHandle      │ 默认采样器    │
// │ m_IsDirty                 │ bool                   │ 脏标记       │
// │ m_Device                  │ IRHIDevice*            │ 设备(非拥有)  │
// │ m_DebugName               │ const AnsiChar*        │ 调试名称      │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                                │
// │─────────────│──────────│────────────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M0.5 材质系统 Agent A)      │
// ============================================================

#pragma once

#include "RenderCore/Material/FMaterial.h"

namespace Limx
{

// ============================================================================
// EMaterialParamOverride — 可覆盖参数位掩码
// ============================================================================

enum class EMaterialParamOverride : UInt32
{
    None          = 0u,

    BaseColor     = LIMX_BIT(0),
    Metallic      = LIMX_BIT(1),
    Roughness     = LIMX_BIT(2),
    AO            = LIMX_BIT(3),
    NormalScale   = LIMX_BIT(4),
    EmissiveColor = LIMX_BIT(5),
    AlphaCutoff   = LIMX_BIT(6),
    BlendMode     = LIMX_BIT(7),

    /// 覆盖全部标量参数 (不含纹理绑定)
    All           = 0xFFu,
};
LIMX_DEFINE_ENUM_BITWISE_OPS(EMaterialParamOverride)

// ============================================================================
// FMaterialInstance — 材质实例
// ============================================================================

class FMaterialInstance
{
public:
    LIMX_NON_COPYABLE(FMaterialInstance);

    FMaterialInstance() = default;
    ~FMaterialInstance();

    // ========================================================================
    // 生命周期
    // ========================================================================

    /// 初始化材质实例 — 创建 GPU UBO + 分配 set 1 描述符集
    ///
    /// @param parent         父材质 (非拥有，必须在 Shutdown 前保持有效)
    /// @param device         RHI 设备 (非拥有)
    /// @param descSetLayout  set 1 描述符集布局 (由 FMaterialManager 提供)
    /// @param defaultView    默认白色纹理视图
    /// @param defaultSampler 默认采样器
    /// @param debugName      调试名称
    /// @return               Success 或错误码
    ERHIResult Initialize(
        FMaterial*              parent,
        IRHIDevice*             device,
        FRHIDescSetLayoutHandle descSetLayout,
        FRHITextureViewHandle   defaultView,
        FRHISamplerHandle       defaultSampler,
        const AnsiChar*         debugName = "MaterialInstance");

    /// 关闭材质实例 — 释放 GPU UBO + 描述符集
    void Shutdown();

    // ========================================================================
    // 参数覆盖 — 每个 setter 启用对应覆盖位并标脏
    // ========================================================================

    /// 覆盖基色 (RGBA)
    void SetOverrideBaseColor(const FVector4& color);

    /// 覆盖金属度 [0, 1]
    void SetOverrideMetallic(Float32 metallic);

    /// 覆盖粗糙度 [0, 1]
    void SetOverrideRoughness(Float32 roughness);

    /// 覆盖环境光遮蔽因子 [0, 1]
    void SetOverrideAO(Float32 ao);

    /// 覆盖法线贴图强度
    void SetOverrideNormalScale(Float32 normalScale);

    /// 覆盖自发光颜色 (RGB)
    void SetOverrideEmissiveColor(const FVector3& color);

    /// 覆盖 Alpha 裁剪阈值 [0, 1]
    void SetOverrideAlphaCutoff(Float32 cutoff);

    /// 覆盖混合模式
    void SetOverrideBlendMode(EMaterialBlendMode mode);

    // ========================================================================
    // 覆盖管理
    // ========================================================================

    /// 清除指定参数的覆盖位 (回退到父材质的对应值)
    void ClearOverride(EMaterialParamOverride overrideFlag);

    /// 清除全部覆盖 — 实例参数完全回退到父材质
    void ClearAllOverrides();

    // ========================================================================
    // GPU 上传
    // ========================================================================

    /// 若实例已脏则合并父参数 + 实例覆盖并上传 GPU
    void Flush();

    // ========================================================================
    // 访问器
    // ========================================================================

    /// 获取 set 1 描述符集句柄 (供命令录制时 BindDescriptorSet 使用)
    LIMX_NODISCARD FRHIDescriptorSetHandle GetDescriptorSet() const
    {
        return m_DescriptorSet;
    }

    /// 计算父参数 + 实例覆盖的合并最终参数 (仅 CPU 计算，不写入 GPU)
    LIMX_NODISCARD FMaterialParams GetMergedParams() const;

    /// 获取父材质指针 (非拥有)
    LIMX_NODISCARD FMaterial* GetParent() const { return m_Parent; }

    /// 实例是否已脏
    LIMX_NODISCARD bool IsDirty() const { return m_IsDirty; }

    /// 手动标脏
    void MarkDirty() { m_IsDirty = true; }

    /// 获取实例调试名称
    LIMX_NODISCARD const AnsiChar* GetDebugName() const
    {
        return m_DebugName;
    }

private:
    // ========================================================================
    // 内部方法
    // ========================================================================

    /// 将合并参数写入 GPU UBO
    void UploadMergedParams();

    /// 写入 set 1 描述符集 (UBO + 从父材质继承的 5 个纹理槽位)
    void UpdateDescriptorSet();

    // ========================================================================
    // 成员
    // ========================================================================

    /// 父材质 (非拥有指针)
    FMaterial* m_Parent = nullptr;

    /// 实例覆盖参数数据 (与父材质合并后上传 GPU)
    FMaterialParams m_OverrideParams;

    /// 覆盖位掩码 — 哪些参数字段使用实例值而非父值
    EMaterialParamOverride m_OverrideMask = EMaterialParamOverride::None;

    /// 实例独立的 GPU UBO (存储合并后的最终 FMaterialParams)
    FRHIBufferHandle m_ParamsUBO;

    /// 实例独立的 set 1 描述符集
    FRHIDescriptorSetHandle m_DescriptorSet;

    /// 默认白色纹理视图 (非拥有)
    FRHITextureViewHandle m_DefaultTextureView;

    /// 默认采样器 (非拥有)
    FRHISamplerHandle m_DefaultSampler;

    /// 脏标记
    bool m_IsDirty = true;

    /// RHI 设备 (非拥有指针)
    IRHIDevice* m_Device = nullptr;

    /// 调试名称
    const AnsiChar* m_DebugName = "MaterialInstance";
};

} // namespace Limx
