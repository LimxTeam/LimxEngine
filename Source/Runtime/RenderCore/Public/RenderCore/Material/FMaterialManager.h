// ============================================================
// 文件名称：FMaterialManager.h
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：集中生命周期管理 — 材质管理器是所有材质/材质实例 GPU 资源的
//          唯一分配者，持有默认 1×1 白色纹理和默认采样器作为未绑定槽位的
//          占位资源，对外统一提供 set 1 描述符集布局供管线布局构建使用。
//          批量上传策略: UploadDirtyMaterials() 每帧一次遍历所有已注册的
//          材质/实例，仅对已脏的对象执行 GPU 上传，摊销同步开销。
// 功能描述：材质管理器单例 — 创建 set 1 描述符集布局，管理默认白色纹理
//          资源，提供 CreateMaterial/CreateMaterialInstance 工厂接口，
//          每帧通过 UploadDirtyMaterials() 批量上传脏材质。
// 技术特性：单例模式 (Meyer's Singleton)；
//          TArray<TUniquePtr<FMaterial>> 管理材质生命周期；
//          TArray<TUniquePtr<FMaterialInstance>> 管理实例生命周期；
//          默认 1×1 白色 RGBA8 纹理通过 Staging Buffer + 一次性命令上传；
//          set 1 描述符集布局: binding 0 UBO + binding 1~5 CombinedImageSampler；
//          CreateDefaultMaterial() 创建一个标准灰色 PBR 材质作为预设。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                       │ 描述                              │
// │─────────────────────────────│──────────────────────────────────│
// │ Get()                       │ 获取单例引用                       │
// │ Initialize()                │ 创建默认纹理 + 采样器 + 描述符集布局 │
// │ Shutdown()                  │ 销毁全部材质 + 默认资源 + 布局       │
// │ IsInitialized()             │ 是否已初始化                       │
// │ CreateMaterial()            │ 创建并注册 FMaterial                │
// │ CreateMaterialInstance()    │ 创建并注册 FMaterialInstance        │
// │ DestroyMaterial()           │ 销毁指定材质 (从注册表移除)          │
// │ DestroyMaterialInstance()   │ 销毁指定材质实例                    │
// │ UploadDirtyMaterials()      │ 批量刷新所有已脏材质/实例的 GPU 数据  │
// │ CreateDefaultMaterial()     │ 创建一个标准灰色 PBR 预设材质        │
// │ GetDescSetLayout()          │ 获取 set 1 描述符集布局 (供管线布局) │
// │ GetDefaultTextureView()     │ 获取默认 1×1 白色纹理视图           │
// │ GetDefaultSampler()         │ 获取默认线性采样器                  │
// │ GetMaterialCount()          │ 获取已注册材质数量                   │
// │ GetMaterialInstanceCount()  │ 获取已注册材质实例数量               │
//
// ── 结构体字段表 ──────────────────────────────────────────────
// │ 字段名                       │ 类型                          │ 描述      │
// │─────────────────────────────│─────────────────────────────│──────────│
// │ m_Device                    │ IRHIDevice*                  │ 设备(非拥有)│
// │ m_Context                   │ FRenderContext*              │ 上下文(非拥有)│
// │ m_DescSetLayout             │ FRHIDescSetLayoutHandle      │ set 1 布局│
// │ m_DefaultTexture            │ FRHITextureHandle            │ 默认白色纹理│
// │ m_DefaultTextureView        │ FRHITextureViewHandle        │ 默认纹理视图│
// │ m_DefaultSampler            │ FRHISamplerHandle            │ 默认采样器  │
// │ m_Materials                 │ TArray<TUniquePtr<FMaterial>>│ 材质注册表  │
// │ m_MaterialInstances         │ TArray<TUniquePtr<FMaterialInstance>>│实例表│
// │ m_IsInitialized             │ bool                         │ 初始化标志  │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                              │
// │─────────────│──────────│──────────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M0.5 材质系统 Agent A)    │
// ============================================================

#pragma once

#include "RenderCore/Material/FMaterial.h"
#include "RenderCore/Material/FMaterialInstance.h"
#include "RenderCore/Renderer/FRenderContext.h"

namespace Limx
{

// 材质管理器日志类别
LIMX_DECLARE_LOG_CATEGORY(LogMaterialManager)

// ============================================================================
// FMaterialManager — 材质系统单例管理器
// ============================================================================

class FMaterialManager
{
public:
    LIMX_NON_COPYABLE(FMaterialManager);

    // ========================================================================
    // 单例
    // ========================================================================

    /// 获取单例引用 (Meyer's Singleton)
    static FMaterialManager& Get()
    {
        static FMaterialManager instance;
        return instance;
    }

    // ========================================================================
    // 生命周期
    // ========================================================================

    /// 初始化材质管理器
    ///
    /// 执行顺序：
    ///   1. 创建默认 1×1 白色 RGBA8 纹理 (通过 Staging Buffer 上传)
    ///   2. 创建默认线性采样器
    ///   3. 创建 set 1 描述符集布局 (1 UBO + 5 CombinedImageSampler)
    ///
    /// @param device   RHI 设备 (非拥有)
    /// @param context  渲染上下文 (非拥有，用于 BeginSingleTimeCommands)
    /// @return         Success 或错误码
    ERHIResult Initialize(IRHIDevice* device, FRenderContext* context);

    /// 关闭材质管理器
    ///
    /// 按顺序销毁：所有材质实例 → 所有材质 → 默认纹理视图 → 默认纹理 →
    ///             默认采样器 → 描述符集布局
    void Shutdown();

    /// 是否已初始化
    LIMX_NODISCARD bool IsInitialized() const { return m_IsInitialized; }

    // ========================================================================
    // 材质工厂
    // ========================================================================

    /// 创建并注册一个新 FMaterial
    ///
    /// @param debugName  调试名称 (可选)
    /// @return           新材质的非拥有指针，失败返回 nullptr
    LIMX_NODISCARD FMaterial* CreateMaterial(
        const AnsiChar* debugName = "Material");

    /// 创建并注册一个新 FMaterialInstance
    ///
    /// @param parent     父材质 (必须已由本管理器创建且仍然有效)
    /// @param debugName  调试名称 (可选)
    /// @return           新实例的非拥有指针，失败返回 nullptr
    LIMX_NODISCARD FMaterialInstance* CreateMaterialInstance(
        FMaterial*      parent,
        const AnsiChar* debugName = "MaterialInstance");

    /// 销毁指定材质 — 从注册表移除并释放 GPU 资源
    ///
    /// @param material  要销毁的材质指针 (调用后指针失效)
    void DestroyMaterial(FMaterial* material);

    /// 销毁指定材质实例
    ///
    /// @param instance  要销毁的实例指针 (调用后指针失效)
    void DestroyMaterialInstance(FMaterialInstance* instance);

    // ========================================================================
    // 预设材质
    // ========================================================================

    /// 创建标准灰色 PBR 预设材质
    ///
    /// 参数预设：BaseColor=(0.5,0.5,0.5,1.0), Metallic=0.0, Roughness=0.5
    ///
    /// @param debugName  调试名称
    /// @return           材质指针，失败返回 nullptr
    LIMX_NODISCARD FMaterial* CreateDefaultMaterial(
        const AnsiChar* debugName = "DefaultMaterial");

    // ========================================================================
    // 每帧批量上传
    // ========================================================================

    /// 批量刷新所有已脏材质和材质实例的 GPU 数据
    ///
    /// 应在每帧开始录制命令之前调用（BeginFrame 之后、RecordCommands 之前）
    void UploadDirtyMaterials();

    // ========================================================================
    // 访问器
    // ========================================================================

    /// 获取 set 1 描述符集布局句柄
    /// 用于在 FRenderer 或 FPassManager 构建管线布局时传入 set 1 的布局
    LIMX_NODISCARD FRHIDescSetLayoutHandle GetDescSetLayout() const
    {
        return m_DescSetLayout;
    }

    /// 获取默认 1×1 白色纹理视图 (未绑定槽位的占位符)
    LIMX_NODISCARD FRHITextureViewHandle GetDefaultTextureView() const
    {
        return m_DefaultTextureView;
    }

    /// 获取默认线性采样器
    LIMX_NODISCARD FRHISamplerHandle GetDefaultSampler() const
    {
        return m_DefaultSampler;
    }

    /// 已注册材质数量
    LIMX_NODISCARD SizeType GetMaterialCount() const
    {
        return m_Materials.GetSize();
    }

    /// 已注册材质实例数量
    LIMX_NODISCARD SizeType GetMaterialInstanceCount() const
    {
        return m_MaterialInstances.GetSize();
    }

private:
    FMaterialManager()  = default;
    ~FMaterialManager() = default;

    // ========================================================================
    // 内部方法
    // ========================================================================

    /// 创建默认 1×1 白色纹理 (通过 Staging Buffer 上传到 GPU)
    ERHIResult CreateDefaultTexture();

    /// 创建 set 1 的描述符集布局
    /// Layout:
    ///   binding 0 — UniformBuffer        (Fragment)  FMaterialParams
    ///   binding 1 — CombinedImageSampler (Fragment)  Albedo
    ///   binding 2 — CombinedImageSampler (Fragment)  Normal
    ///   binding 3 — CombinedImageSampler (Fragment)  MetallicRoughness
    ///   binding 4 — CombinedImageSampler (Fragment)  Occlusion
    ///   binding 5 — CombinedImageSampler (Fragment)  Emissive
    ERHIResult CreateDescSetLayout();

    // ========================================================================
    // 成员
    // ========================================================================

    /// RHI 设备 (非拥有)
    IRHIDevice* m_Device = nullptr;

    /// 渲染上下文 (非拥有，用于 BeginSingleTimeCommands)
    FRenderContext* m_Context = nullptr;

    /// set 1 描述符集布局 — UBO + 5 个 CombinedImageSampler
    FRHIDescSetLayoutHandle m_DescSetLayout;

    /// 默认 1×1 白色 RGBA8 纹理
    FRHITextureHandle m_DefaultTexture;

    /// 默认纹理视图
    FRHITextureViewHandle m_DefaultTextureView;

    /// 默认线性采样器
    FRHISamplerHandle m_DefaultSampler;

    /// 已注册的材质 (管理器拥有生命周期)
    TArray<TUniquePtr<FMaterial>> m_Materials;

    /// 已注册的材质实例 (管理器拥有生命周期)
    TArray<TUniquePtr<FMaterialInstance>> m_MaterialInstances;

    /// 初始化标志
    bool m_IsInitialized = false;
};

} // namespace Limx
