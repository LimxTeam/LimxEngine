// ============================================================
// 文件名称：FMaterial.h
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：PBR 材质参数化 — 以物理正确的 Cook-Torrance 微表面 BRDF
//          为参数设计基础，所有参数严格遵守 GLSL std140 对齐规则，
//          支持最多 5 个纹理槽位 (Albedo/Normal/MetallicRoughness/Occlusion/Emissive)。
//          脏标记机制确保 GPU UBO 只在参数真正变化时才上传，减少带宽消耗。
//          材质描述符集固定占用 set 1 (set 0 保留给 FRenderer 的场景级 ViewProj)。
// 功能描述：PBR 材质类 — 持有材质参数 UBO + 描述符集 (set 1)，
//          支持设置 PBR 标量参数和 5 个纹理槽位，提供脏标记延迟上传机制。
// 技术特性：FMaterialParams 严格遵守 GLSL std140 对齐 (64 字节)；
//          每个材质持有独立的 GPU UBO 缓冲区 (CpuToGpu) 和描述符集；
//          纹理槽位未绑定时自动使用初始化时传入的默认白色纹理；
//          Flush() 统一执行 UBO 上传 + 描述符集更新，仅在 m_IsDirty 时执行；
//          LIMX_NON_COPYABLE 防止意外拷贝导致 GPU 资源双重释放。
//
// ── 枚举表 ──────────────────────────────────────────────────
// │ 枚举名                    │ 描述                                 │
// │──────────────────────────│─────────────────────────────────────│
// │ EMaterialBlendMode       │ 混合模式 (Opaque/Masked/Translucent/Additive) │
//
// ── 结构体/类表 ──────────────────────────────────────────────
// │ 名称                      │ 描述                                 │
// │──────────────────────────│─────────────────────────────────────│
// │ FMaterialParams          │ PBR 参数 GPU UBO 数据 (64 bytes, std140) │
// │ FMaterial                │ PBR 材质类 (持有 UBO + set 1 描述符集)   │
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                    │ 描述                                 │
// │──────────────────────────│─────────────────────────────────────│
// │ Initialize()             │ 创建 GPU UBO + 分配并写入描述符集       │
// │ Shutdown()               │ 释放 GPU UBO + 描述符集               │
// │ SetBaseColor()           │ 设置基色 (RGBA), 自动标脏              │
// │ SetMetallic()            │ 设置金属度 [0,1], 自动标脏             │
// │ SetRoughness()           │ 设置粗糙度 [0,1], 自动标脏             │
// │ SetAO()                  │ 设置环境光遮蔽 [0,1], 自动标脏         │
// │ SetNormalScale()         │ 设置法线贴图强度, 自动标脏              │
// │ SetEmissiveColor()       │ 设置自发光颜色 (RGB), 自动标脏          │
// │ SetAlphaCutoff()         │ 设置 Alpha 裁剪阈值, 自动标脏           │
// │ SetBlendMode()           │ 设置混合模式, 自动标脏                  │
// │ BindTexture()            │ 绑定纹理到指定槽位, 自动标脏            │
// │ UnbindTexture()          │ 解绑槽位 (回退默认白色纹理), 自动标脏   │
// │ Flush()                  │ 若脏则上传 UBO + 刷新描述符集绑定       │
// │ GetDescriptorSet()       │ 获取 set 1 描述符集句柄 (只读)          │
// │ GetParams()              │ 获取 CPU 侧参数常量引用                 │
// │ GetTextureView()         │ 获取指定槽位的纹理视图                   │
// │ GetSampler()             │ 获取指定槽位的采样器                    │
// │ IsDirty()                │ 是否需要上传 GPU                       │
// │ MarkDirty()              │ 手动标脏 (强制下次 Flush 执行上传)       │
// │ GetDebugName()           │ 获取材质调试名称                        │
//
// ── 结构体字段表 ──────────────────────────────────────────────
// │ 字段名                    │ 类型                    │ 描述         │
// │──────────────────────────│───────────────────────│─────────────│
// │ m_Params                 │ FMaterialParams        │ CPU 侧 PBR 参数│
// │ m_ParamsUBO              │ FRHIBufferHandle       │ GPU UBO 缓冲区│
// │ m_DescriptorSet          │ FRHIDescriptorSetHandle│ set 1 描述符集│
// │ m_TextureViews[5]        │ FRHITextureViewHandle[]│ 各槽位纹理视图  │
// │ m_Samplers[5]            │ FRHISamplerHandle[]    │ 各槽位采样器   │
// │ m_DefaultTextureView     │ FRHITextureViewHandle  │ 默认白色纹理视图│
// │ m_DefaultSampler         │ FRHISamplerHandle      │ 默认采样器     │
// │ m_IsDirty                │ bool                   │ UBO/描述符脏标记│
// │ m_Device                 │ IRHIDevice*            │ RHI 设备(非拥有)│
// │ m_DebugName              │ const AnsiChar*        │ 调试名称       │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                                 │
// │─────────────│──────────│─────────────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M0.5 材质系统 Agent A)       │
// ============================================================

#pragma once

#include "RenderCore/RenderCoreMinimal.h"

namespace Limx
{

class FRenderResourceManager;

// ============================================================================
// 材质纹理槽位常量
// ============================================================================

/// 纹理槽位索引
static constexpr UInt32 kMaterialTextureSlotAlbedo           = 0;
static constexpr UInt32 kMaterialTextureSlotNormal           = 1;
static constexpr UInt32 kMaterialTextureSlotMetallicRoughness = 2;
static constexpr UInt32 kMaterialTextureSlotOcclusion        = 3;
static constexpr UInt32 kMaterialTextureSlotEmissive         = 4;
static constexpr UInt32 kMaterialTextureSlotCount            = 5;

/// FMaterialParams::TextureFlags 位掩码
static constexpr UInt32 kMaterialTexFlagAlbedo           = LIMX_BIT(0);
static constexpr UInt32 kMaterialTexFlagNormal           = LIMX_BIT(1);
static constexpr UInt32 kMaterialTexFlagMetallicRoughness = LIMX_BIT(2);
static constexpr UInt32 kMaterialTexFlagOcclusion        = LIMX_BIT(3);
static constexpr UInt32 kMaterialTexFlagEmissive         = LIMX_BIT(4);

/// 法线贴图只存了两个通道 (BC5), Z 必须由着色器重建
///
/// 为什么是逐材质的标志位, 而不是"约定法线槽位一律用 BC5":
///
///   1. 引擎并没有停止接收 RGB 法线图。glTF 内嵌的法线贴图是 PNG/JPEG,
///      OBJ/MTL 引用的也是, FSceneLoader 会照常把它们解成 RGBA8。一条
///      代码无法强制的"约定", 在第一次加载未烘焙资产时就会被打破 ——
///      而它的失效方式是: 作者存进去的 Z 被 sqrt 重建值覆盖, 光照
///      微妙地不对, 没有任何报错。
///
///   2. lat 自己就拒绝这个约定。它的 resolve_format 明确不从文件名猜
///      "这是法线图", 文档里写着三通道的切线空间法线会落到 BC1 ——
///      也就是说, 连烘焙工具都不保证"法线槽 ⇒ BC5"。
///
///   3. 这个标志位是免费的。TextureFlags 是一个 uint, 只用掉了 5 位;
///      不增加结构体大小、不增加描述符、材质内取值一致因此不产生分支
///      发散。而且它由 BindTextureResource 从纹理的实际像素格式推出来,
///      不是手工填的, 因此不会与真实数据脱节。
static constexpr UInt32 kMaterialTexFlagNormalTwoChannel = LIMX_BIT(5);

// ============================================================================
// EMaterialBlendMode — 材质混合模式
// ============================================================================

enum class EMaterialBlendMode : UInt32
{
    /// 不透明 — 禁用 Alpha 混合
    Opaque      = 0,

    /// 蒙版 — AlphaCutoff 阈值裁剪 (片段着色器 discard)
    Masked      = 1,

    /// 半透明 — 源 Alpha 混合
    Translucent = 2,

    /// 叠加 — 颜色叠加混合 (适用于粒子/辉光)
    Additive    = 3,

    Count
};

/// 该混合模式是否需要走"混合"路径
///
/// Opaque 与 Masked 都直接覆盖颜色附件: Masked 靠片段着色器 discard 实现
/// 镂空, 不需要混合, 也照常写深度。真正需要混合的是 Translucent 与 Additive,
/// 它们不写深度、必须由远及近绘制。
///
/// 分类规则单独成函数而非散落在各处的 if: 新增混合模式时, 忘了更新其中
/// 一处的后果是该模式在某个 Pass 里走错路径 —— 而画面上只表现为
/// "这个材质有时候不对", 极难定位。
LIMX_NODISCARD constexpr bool IsBlendedMode(EMaterialBlendMode mode)
{
    return mode == EMaterialBlendMode::Translucent ||
           mode == EMaterialBlendMode::Additive;
}


// ============================================================================
// FMaterialParams — PBR 材质参数 GPU UBO 数据布局
//
// 严格遵守 GLSL std140 规则, 总大小 64 字节, 16 字节对齐
// GLSL 侧布局:
//   layout(set=1, binding=0) uniform MaterialUBO {
//       vec4  BaseColor;        // offset  0
//       float Metallic;         // offset 16
//       float Roughness;        // offset 20
//       float AO;               // offset 24
//       float NormalScale;      // offset 28
//       vec4  EmissiveColor;    // offset 32 (w 分量未使用)
//       float AlphaCutoff;      // offset 48
//       uint  TextureFlags;     // offset 52
//       uint  BlendMode;        // offset 56
//       float _Padding;         // offset 60
//   };
// ============================================================================

struct alignas(16) FMaterialParams
{
    /// 基色 (RGBA)。对应 GLSL vec4。
    FVector4 BaseColor = FVector4(1.0f, 1.0f, 1.0f, 1.0f);

    /// 金属度因子 [0, 1]
    Float32  Metallic  = 0.0f;

    /// 粗糙度因子 [0, 1]
    Float32  Roughness = 0.5f;

    /// 环境光遮蔽因子 [0, 1]
    Float32  AO        = 1.0f;

    /// 法线贴图强度因子 [0, 1]
    Float32  NormalScale = 1.0f;

    /// 自发光颜色 (RGB)，w 分量未使用，填 0
    FVector4 EmissiveColor = FVector4(0.0f, 0.0f, 0.0f, 0.0f);

    /// Alpha 裁剪阈值，仅 Masked 模式下有效
    Float32  AlphaCutoff = 0.5f;

    /// 纹理槽位绑定掩码 (kMaterialTexFlag* 位组合)
    UInt32   TextureFlags = 0u;

    /// 混合模式索引 (EMaterialBlendMode 的底层整数值)
    ///
    /// 存为原始整数是 std140 布局的要求 —— 着色器里对应 uint。
    /// 消费方应通过 GetBlendMode() 取回强类型值。
    UInt32   BlendMode  = static_cast<UInt32>(EMaterialBlendMode::Opaque);

    /// 结构体尾部填充 — 保持 64 字节总大小
    Float32  _Padding   = 0.0f;

    /// 取强类型的混合模式
    LIMX_NODISCARD EMaterialBlendMode GetBlendMode() const
    {
        return static_cast<EMaterialBlendMode>(BlendMode);
    }
};

static_assert(sizeof(FMaterialParams) == 64,
    "FMaterialParams 大小必须为 64 字节以匹配 std140 UBO 布局");
static_assert(alignof(FMaterialParams) == 16,
    "FMaterialParams 必须 16 字节对齐以满足 std140 要求");

// ============================================================================
// FMaterial — PBR 材质
// ============================================================================

class FMaterial
{
public:
    LIMX_NON_COPYABLE(FMaterial);

    FMaterial() = default;
    ~FMaterial();

    // ========================================================================
    // 生命周期
    // ========================================================================

    /// 初始化材质 — 创建 GPU UBO + 分配 set 1 描述符集 + 写入默认绑定
    ///
    /// @param device            RHI 设备 (非拥有，必须在 Shutdown 前保持有效)
    /// @param descSetLayout     set 1 描述符集布局 (由 FMaterialManager 提供)
    /// @param defaultView       未绑定槽位使用的默认白色纹理视图
    /// @param defaultSampler    未绑定槽位使用的默认采样器
    /// @param debugName         调试名称 (可选)
    /// @return                  Success 或错误码
    ERHIResult Initialize(
        IRHIDevice*              device,
        FRHIDescSetLayoutHandle  descSetLayout,
        FRHITextureViewHandle    defaultView,
        FRHISamplerHandle        defaultSampler,
        const AnsiChar*          debugName = "Material");

    /// 关闭材质 — 释放 GPU UBO + 描述符集
    void Shutdown();

    // ========================================================================
    // PBR 参数设置 — 每个 setter 自动标脏
    // ========================================================================

    /// 设置基色 (RGBA)
    void SetBaseColor(const FVector4& color);

    /// 设置金属度 [0, 1]
    void SetMetallic(Float32 metallic);

    /// 设置粗糙度 [0, 1]
    void SetRoughness(Float32 roughness);

    /// 设置环境光遮蔽因子 [0, 1]
    void SetAO(Float32 ao);

    /// 设置法线贴图强度 [0, 1]
    void SetNormalScale(Float32 normalScale);

    /// 设置自发光颜色 (RGB)
    void SetEmissiveColor(const FVector3& color);

    /// 设置 Alpha 裁剪阈值 [0, 1]（仅 Masked 模式生效）
    void SetAlphaCutoff(Float32 cutoff);

    /// 设置混合模式
    void SetBlendMode(EMaterialBlendMode mode);

    /// 设置是否双面渲染
    ///
    /// 不进 FMaterialParams —— 它是光栅化状态而非着色器数据, 影响的是
    /// 选哪条管线, 而不是 UBO 里的某个数值。放进 UBO 只会白占 std140 空间,
    /// 还会让"改了这个值要重建管线"这件事被标脏机制掩盖掉。
    void SetDoubleSided(bool doubleSided) { m_IsDoubleSided = doubleSided; }

    LIMX_NODISCARD bool IsDoubleSided() const { return m_IsDoubleSided; }

    // ========================================================================
    // 纹理绑定
    // ========================================================================

    /// 绑定纹理到指定槽位
    ///
    /// @param slot      槽位索引 (kMaterialTextureSlot* 常量)
    /// @param view      纹理视图
    /// @param sampler   采样器
    void BindTexture(
        UInt32               slot,
        FRHITextureViewHandle view,
        FRHISamplerHandle    sampler);

    /// 绑定由资源管理器拥有的纹理到指定槽位
    ///
    /// 与裸句柄版本的区别在于**所有权**: 本重载会对纹理资源加一次引用,
    /// 并在解绑/销毁时释放。没有这一步的话, 导入的纹理引用计数永远停在
    /// 创建时的那一份, CollectUnreferenced 永远收不走它们 —— 关卡切换时
    /// 显存只增不减。
    ///
    /// @param slot     槽位索引 (kMaterialTextureSlot* 常量)
    /// @param manager  资源管理器 (非拥有, 必须活得比本材质长)
    /// @param handle   纹理资源句柄
    void BindTextureResource(UInt32 slot,
                             FRenderResourceManager* manager,
                             FTextureResourceHandle handle);

    /// 解绑指定槽位 — 回退到默认白色纹理，清除 TextureFlags 对应位
    ///
    /// 若该槽位持有的是资源管理器的纹理, 一并释放引用。
    void UnbindTexture(UInt32 slot);

    // ========================================================================
    // GPU 上传
    // ========================================================================

    /// 若材质已脏则执行 GPU 上传 (UBO 数据写入 + 描述符集更新)
    void Flush();

    // ========================================================================
    // 访问器
    // ========================================================================

    /// 获取 set 1 描述符集句柄 (供命令录制时 BindDescriptorSet 使用)
    LIMX_NODISCARD FRHIDescriptorSetHandle GetDescriptorSet() const
    {
        return m_DescriptorSet;
    }

private:
    /// 是否双面渲染 — 光栅化状态, 决定绘制时选用哪条管线
    bool m_IsDoubleSided = false;

public:

    /// 获取 CPU 侧参数常量引用
    LIMX_NODISCARD const FMaterialParams& GetParams() const
    {
        return m_Params;
    }

    /// 获取指定槽位的纹理视图 (含默认回退)
    LIMX_NODISCARD FRHITextureViewHandle GetTextureView(UInt32 slot) const;

    /// 获取指定槽位的采样器 (含默认回退)
    LIMX_NODISCARD FRHISamplerHandle GetSampler(UInt32 slot) const;

    /// 材质是否已脏 (参数或纹理变化尚未上传 GPU)
    LIMX_NODISCARD bool IsDirty() const { return m_IsDirty; }

    /// 手动标脏 — 强制下次 Flush() 执行全量上传
    void MarkDirty() { m_IsDirty = true; }

    /// 获取材质调试名称
    LIMX_NODISCARD const AnsiChar* GetDebugName() const
    {
        return m_DebugName;
    }

private:
    // ========================================================================
    // 内部方法
    // ========================================================================

    /// 将 m_Params 写入 GPU UBO (MapBuffer + MemCopy + UnmapBuffer)
    void UploadParams();

    /// 将所有描述符绑定 (UBO + 5 个纹理槽位) 写入 m_DescriptorSet
    void UpdateDescriptorSet();

public:
    /// 把本材质注册进 bindless 表, 记下分配到的下标
    ///
    /// 幂等: 已注册过就只更新表里的那一项, 下标不变。下标变动会让已经录进
    /// 命令缓冲区的 push constant 指向错误的材质。
    void RegisterBindless(class FBindlessTable& table);

    /// bindless 表里的下标 (未注册时为 0, 即默认材质)
    LIMX_NODISCARD UInt32 GetBindlessIndex() const { return m_BindlessIndex; }

    LIMX_NODISCARD bool IsBindlessRegistered() const
    {
        return m_BindlessRegistered;
    }

private:
    /// bindless 表下标
    UInt32 m_BindlessIndex     = 0;
    bool   m_BindlessRegistered = false;


    // ========================================================================
    // 成员
    // ========================================================================

    /// CPU 侧 PBR 参数 (与 GPU UBO 保持同步)
    FMaterialParams m_Params;

    /// GPU 侧参数 UBO (CpuToGpu 内存, 持续映射写入)
    FRHIBufferHandle m_ParamsUBO;

    /// set 1 描述符集 (UBO binding=0 + Albedo-Emissive binding=1~5)
    FRHIDescriptorSetHandle m_DescriptorSet;

    /// 每个槽位的纹理视图 (未绑定时存储默认视图)
    FRHITextureViewHandle m_TextureViews[kMaterialTextureSlotCount];

    /// 每个槽位的采样器 (未绑定时存储默认采样器)
    FRHISamplerHandle m_Samplers[kMaterialTextureSlotCount];

    /// 释放指定槽位持有的纹理引用 (若有)
    void ReleaseTextureResource(UInt32 slot);

    /// 各槽位持有的纹理资源句柄 — 无效表示该槽位用的是裸句柄或默认纹理
    ///
    /// 与 m_TextureViews 并存而非取代它: 程序化生成的纹理 (棋盘格、
    /// 默认白图) 不经过资源管理器, 只有裸句柄可用。
    FTextureResourceHandle m_TextureResources[kMaterialTextureSlotCount];

    /// 资源管理器 — 非拥有; 仅当至少有一个槽位持有资源句柄时非空
    FRenderResourceManager* m_TextureOwner = nullptr;

    /// 默认白色纹理视图 (非拥有，由 FMaterialManager 管理生命周期)
    FRHITextureViewHandle m_DefaultTextureView;

    /// 默认采样器 (非拥有，由 FMaterialManager 管理生命周期)
    FRHISamplerHandle m_DefaultSampler;

    /// UBO / 描述符集脏标记
    bool m_IsDirty = true;

    /// RHI 设备 (非拥有指针)
    IRHIDevice* m_Device = nullptr;

    /// 调试名称
    const AnsiChar* m_DebugName = "Material";
};

} // namespace Limx
