/*******************************************************************************
 * 文件: FAssetRegistry.h
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   资产注册表 — 按路径去重的解码结果缓存，带引用计数与代际句柄
 *   场景 (OBJ/glTF) 与图像 (PNG/JPEG) 共用同一套句柄与生命周期机制
 *
 * 设计哲学:
 *   路径即身份 — 同一张贴图常被十几个材质引用。若每次引用都重新解码，
 *   加载一个场景要付出十倍的解码时间与内存。以规范化后的路径为键去重，
 *   使解码次数等于不同文件的数量而非引用的数量。
 *
 *   代际句柄而非裸指针 — 槽位在资产卸载后会被复用。裸指针在那一刻变成悬垂，
 *   而访问悬垂指针的症状是随机的数据错乱。句柄带上代际号后，卸载即令旧句柄
 *   失效，误用会得到一个干脆的 nullptr 而不是错误的数据。
 *
 *   卸载是显式的 — 引用归零不立即释放。资源在一帧内被放下又拾起是常态
 *   (材质切换、LOD 过渡)，立即释放会造成反复的解码抖动。改为标记可回收，
 *   由调用方在合适的时机统一收割。
 *
 * 技术特性:
 *   - 句柄为 (槽位索引, 代际号) 二元组, 卸载后代际递增
 *   - 槽位复用, 数组不随加载卸载次数无界增长
 *   - 解码失败的条目也会被记录, 避免对损坏文件反复重试
 *   - 提供命中率与内存占用统计, 便于评估资产预算
 *
 * 依赖关系:
 *   内部: AssetPipeline/FAssetTypes.h, AssetPipeline/FImageTypes.h,
 *          Core/Containers/TMap.h
 *
 * 注意事项:
 *   非线程安全 — 并发加载需由调用方加锁, 或改为每线程独立注册表
 *   不监听文件变更 — 热重载需要外部触发 Reload
 *
 ******************************************************************************/

#pragma once

#include "AssetPipeline/FAssetTypes.h"
#include "AssetPipeline/FImageTypes.h"
#include "Core/Containers/TMap.h"

namespace Limx
{

// ============================================================================
// FAssetHandle — 代际句柄
// ============================================================================

/// 资产句柄
///
/// 槽位会在卸载后被复用，因此句柄必须携带代际号：持有旧句柄的代码
/// 在槽位被别的资产占用后会得到 nullptr，而不是悄无声息地读到别人的数据。
struct FAssetHandle
{
    /// 无效槽位
    static constexpr UInt32 kInvalidIndex = 0xFFFFFFFFu;

    /// 槽位索引
    UInt32 Index = kInvalidIndex;

    /// 代际号 — 与槽位当前代际不符即视为失效
    UInt32 Generation = 0;

    LIMX_NODISCARD bool IsValid() const { return Index != kInvalidIndex; }

    LIMX_NODISCARD bool operator==(const FAssetHandle& other) const
    {
        return Index == other.Index && Generation == other.Generation;
    }

    LIMX_NODISCARD bool operator!=(const FAssetHandle& other) const
    {
        return !(*this == other);
    }
};

// ============================================================================
// EAssetState — 条目状态
// ============================================================================

/// 资产条目的状态
enum class EAssetState : UInt8
{
    /// 槽位空闲
    Empty = 0,

    /// 已成功加载
    Loaded = 1,

    /// 加载失败 — 记录下来以免对同一个损坏文件反复重试
    Failed = 2,
};

// ============================================================================
// FAssetRegistryStats — 统计
// ============================================================================

/// 注册表统计
struct FAssetRegistryStats
{
    /// 当前持有的场景数
    UInt32 SceneCount = 0;

    /// 当前持有的图像数
    UInt32 ImageCount = 0;

    /// 加载失败的条目数
    UInt32 FailedCount = 0;

    /// 引用计数归零、可被回收的条目数
    UInt32 UnreferencedCount = 0;

    /// 请求命中已有条目的次数
    UInt64 CacheHits = 0;

    /// 请求触发实际加载的次数
    UInt64 CacheMisses = 0;

    /// 图像像素占用的总字节数
    SizeType ImageBytes = 0;

    /// 场景顶点与索引占用的估算字节数
    SizeType SceneBytes = 0;
};

// ============================================================================
// FAssetRegistry — 资产注册表
// ============================================================================

/// 资产注册表
class LIMX_ASSETPIPELINE_API FAssetRegistry
{
public:
    FAssetRegistry() = default;
    ~FAssetRegistry();

    FAssetRegistry(const FAssetRegistry&)            = delete;
    FAssetRegistry& operator=(const FAssetRegistry&) = delete;

    // ========================================================================
    // 加载
    // ========================================================================

    /// 加载场景 (按扩展名与内容判定 OBJ 或 glTF)
    ///
    /// 同一路径重复请求会命中缓存并递增引用计数，不会重新解析。
    /// @param path 资产路径
    /// @return 句柄; 加载失败时仍返回有效句柄, 但 GetScene 会返回 nullptr
    FAssetHandle LoadScene(const FString& path);

    /// 加载图像
    /// @param path    图像路径
    /// @param options 解码选项 — 选项不同的同一路径视为不同条目
    FAssetHandle LoadImage(const FString& path,
                           const FImageDecodeOptions& options =
                               FImageDecodeOptions());

    /// 注册一份已在内存中的场景 — 用于 GLB 内嵌或程序化生成的资产
    /// @param key   缓存键 (需保证唯一)
    /// @param scene 场景数据, 所有权转移到注册表
    FAssetHandle RegisterScene(const FString& key, FAssetScene&& scene);

    /// 注册一份已在内存中的图像
    FAssetHandle RegisterImage(const FString& key, FImageData&& image);

    // ========================================================================
    // 访问
    // ========================================================================

    /// 取场景 — 句柄失效、类型不符或加载失败时返回 nullptr
    LIMX_NODISCARD const FAssetScene* GetScene(FAssetHandle handle) const;

    /// 取图像 — 同上
    LIMX_NODISCARD const FImageData* GetImage(FAssetHandle handle) const;

    /// 条目状态
    LIMX_NODISCARD EAssetState GetState(FAssetHandle handle) const;

    /// 加载失败的原因 — 未失败时为空
    LIMX_NODISCARD FString GetError(FAssetHandle handle) const;

    /// 条目的来源路径
    LIMX_NODISCARD FString GetPath(FAssetHandle handle) const;

    /// 句柄是否仍指向有效条目
    LIMX_NODISCARD bool IsValid(FAssetHandle handle) const;

    // ========================================================================
    // 引用计数
    // ========================================================================

    /// 递增引用计数
    void AddReference(FAssetHandle handle);

    /// 递减引用计数 — 归零不立即释放, 需调用 CollectUnreferenced
    void ReleaseReference(FAssetHandle handle);

    /// 当前引用计数 — 句柄失效返回 0
    LIMX_NODISCARD UInt32 GetReferenceCount(FAssetHandle handle) const;

    // ========================================================================
    // 回收
    // ========================================================================

    /// 卸载全部引用计数为零的条目
    ///
    /// 引用归零不立即释放是刻意的：材质切换与 LOD 过渡会让资源在一帧内
    /// 被放下又拾起，立即释放会造成反复解码。由调用方在帧末或关卡切换时
    /// 统一收割。
    /// @return 被卸载的条目数
    UInt32 CollectUnreferenced();

    /// 强制卸载指定条目, 无视引用计数 — 卸载后该句柄立即失效
    void Unload(FAssetHandle handle);

    /// 清空全部条目
    void Clear();

    // ========================================================================
    // 统计
    // ========================================================================

    LIMX_NODISCARD FAssetRegistryStats GetStats() const;

    /// 条目总数 (含失败与未引用的)
    LIMX_NODISCARD SizeType GetEntryCount() const;

private:
    /// 条目类型
    enum class EEntryType : UInt8
    {
        None  = 0,
        Scene = 1,
        Image = 2,
    };

    /// 一个资产条目
    struct FEntry
    {
        EEntryType  Type  = EEntryType::None;
        EAssetState State = EAssetState::Empty;

        /// 代际号 — 每次卸载后递增
        UInt32 Generation = 1;

        /// 引用计数
        UInt32 ReferenceCount = 0;

        /// 缓存键 (规范化路径, 图像还带解码选项指纹)
        FString Key;

        /// 原始路径
        FString Path;

        /// 失败原因
        FString Error;

        FAssetScene Scene;
        FImageData  Image;
    };

    /// 取一个空闲槽位 — 优先复用
    UInt32 AcquireSlot();

    /// 在缓存中查找键 — 未找到返回 kInvalidIndex
    LIMX_NODISCARD UInt32 FindByKey(const FString& key) const;

    /// 校验句柄并返回条目 — 失效返回 nullptr
    LIMX_NODISCARD const FEntry* Resolve(FAssetHandle handle) const;
    LIMX_NODISCARD FEntry* Resolve(FAssetHandle handle);

    /// 释放条目持有的数据但保留槽位
    void ReleaseEntryData(FEntry& entry);

    /// 条目池
    TArray<FEntry> m_Entries;

    /// 空闲槽位
    TArray<UInt32> m_FreeSlots;

    /// 缓存键 → 槽位索引
    TMap<FString, UInt32> m_KeyToSlot;

    UInt64 m_CacheHits   = 0;
    UInt64 m_CacheMisses = 0;
};

} // namespace Limx
