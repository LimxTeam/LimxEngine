/*******************************************************************************
 * 文件: FAssetRegistry.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   资产注册表实现 — 路径去重、代际句柄、引用计数与延迟回收
 *
 * 设计哲学:
 *   解码选项参与缓存键 — 同一张 PNG 以"保留 16 位"与"降为 8 位"两种选项
 *   加载会得到内容不同的两份数据。若只以路径为键，后一次请求会拿到前一次的
 *   结果，表现为高度图精度莫名丢失。把影响输出的选项并入键即可杜绝。
 *
 *   失败也要缓存 — 损坏或缺失的贴图在一个场景里可能被上百个材质引用。
 *   不缓存失败会导致上百次无谓的磁盘访问与解码尝试。记下失败并复用，
 *   使每个坏文件只被尝试一次。
 *
 * 技术特性:
 *   - 槽位复用配合代际号, 数组不随加载卸载次数无界增长
 *   - 卸载时释放数据但保留键映射的清除, 避免残留指向空槽的键
 *   - 统计区分命中与未命中, 便于评估去重收益
 *
 * 依赖关系:
 *   内部: AssetPipeline/FAssetRegistry.h, FObjLoader.h, FGltfLoader.h,
 *          FImageDecoder.h
 *
 * 注意事项:
 *   非线程安全 — 并发加载需外部加锁
 *
 ******************************************************************************/

#include "AssetPipeline/FAssetRegistry.h"
#include "AssetPipeline/FObjLoader.h"
#include "AssetPipeline/FGltfLoader.h"
#include "AssetPipeline/FImageDecoder.h"

namespace Limx
{

namespace
{

/// 把路径分隔符统一为正斜杠并转为小写
///
/// Windows 的路径大小写不敏感，同一文件可能以不同大小写被引用。
/// 若不归一化，同一张贴图会被解码多次。
FString NormalizeKey(const FString& path)
{
    FString result;

    for (SizeType i = 0; i < path.GetLength(); ++i)
    {
        AnsiChar c = path[i];

        if (c == '\\')
        {
            c = '/';
        }
        else if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<AnsiChar>(c - 'A' + 'a');
        }

        result.AppendChar(c);
    }

    return result;
}

/// 为解码选项生成指纹并附加到键
///
/// 同一路径以不同选项加载会得到内容不同的图像，必须视为不同条目。
FString MakeImageKey(const FString& path, const FImageDecodeOptions& options)
{
    FString key = NormalizeKey(path);

    key.Append("|");
    key.AppendChar(options.ForceFourChannels ? '4' : 'n');
    key.AppendChar(options.ReduceSixteenBitToEight ? '8' : 'h');
    key.AppendChar(options.FlipVertically ? 'f' : '-');

    return key;
}

/// 判断路径是否以给定后缀结尾 (大小写不敏感)
bool HasExtension(const FString& path, const AnsiChar* extension)
{
    SizeType extensionLength = 0;
    while (extension[extensionLength] != '\0')
    {
        ++extensionLength;
    }

    if (path.GetLength() < extensionLength)
    {
        return false;
    }

    const SizeType start = path.GetLength() - extensionLength;

    for (SizeType i = 0; i < extensionLength; ++i)
    {
        AnsiChar a = path[start + i];
        AnsiChar b = extension[i];

        if (a >= 'A' && a <= 'Z') { a = static_cast<AnsiChar>(a - 'A' + 'a'); }
        if (b >= 'A' && b <= 'Z') { b = static_cast<AnsiChar>(b - 'A' + 'a'); }

        if (a != b)
        {
            return false;
        }
    }

    return true;
}

/// 估算场景占用的字节数
SizeType EstimateSceneBytes(const FAssetScene& scene)
{
    SizeType total = 0;

    for (SizeType i = 0; i < scene.Meshes.GetSize(); ++i)
    {
        const FMeshData& mesh = scene.Meshes[i];

        total += mesh.Vertices.GetSize() * sizeof(FMeshVertex);
        total += mesh.Indices.GetSize() * sizeof(UInt32);
    }

    for (SizeType i = 0; i < scene.EmbeddedImages.GetSize(); ++i)
    {
        total += scene.EmbeddedImages[i].Bytes.GetSize();
    }

    return total;
}

} // namespace

// ============================================================================
// 生命周期
// ============================================================================

FAssetRegistry::~FAssetRegistry()
{
    Clear();
}

// ============================================================================
// 槽位管理
// ============================================================================

UInt32 FAssetRegistry::AcquireSlot()
{
    if (m_FreeSlots.GetSize() > 0)
    {
        const UInt32 slot = m_FreeSlots.Last();
        m_FreeSlots.RemoveAt(m_FreeSlots.GetSize() - 1);
        return slot;
    }

    return static_cast<UInt32>(m_Entries.Add(FEntry()));
}

UInt32 FAssetRegistry::FindByKey(const FString& key) const
{
    if (const UInt32* slot = m_KeyToSlot.Find(key))
    {
        return *slot;
    }

    return FAssetHandle::kInvalidIndex;
}

const FAssetRegistry::FEntry* FAssetRegistry::Resolve(FAssetHandle handle) const
{
    if (!handle.IsValid() || handle.Index >= m_Entries.GetSize())
    {
        return nullptr;
    }

    const FEntry& entry = m_Entries[handle.Index];

    // 代际不符说明该槽位已被卸载并复用, 旧句柄必须被拒绝
    if (entry.Generation != handle.Generation ||
        entry.Type == EEntryType::None)
    {
        return nullptr;
    }

    return &entry;
}

FAssetRegistry::FEntry* FAssetRegistry::Resolve(FAssetHandle handle)
{
    const FEntry* entry =
        static_cast<const FAssetRegistry*>(this)->Resolve(handle);

    return const_cast<FEntry*>(entry);
}

void FAssetRegistry::ReleaseEntryData(FEntry& entry)
{
    entry.Scene.Reset();
    entry.Image.Reset();
    entry.Error.Clear();
}

// ============================================================================
// 加载
// ============================================================================

FAssetHandle FAssetRegistry::LoadScene(const FString& path)
{
    const FString key = NormalizeKey(path);

    // ---- 缓存命中 ----
    const UInt32 existing = FindByKey(key);

    if (existing != FAssetHandle::kInvalidIndex)
    {
        ++m_CacheHits;

        FEntry& entry = m_Entries[existing];
        ++entry.ReferenceCount;

        FAssetHandle handle;
        handle.Index      = existing;
        handle.Generation = entry.Generation;

        return handle;
    }

    ++m_CacheMisses;

    const UInt32 slot = AcquireSlot();

    // AcquireSlot 可能扩容数组, 之后一律通过索引访问
    m_Entries[slot].Type           = EEntryType::Scene;
    m_Entries[slot].Key            = key;
    m_Entries[slot].Path           = path;
    m_Entries[slot].ReferenceCount = 1;

    // ---- 按扩展名选择解析器 ----
    FAssetLoadResult result;

    if (HasExtension(path, ".obj"))
    {
        result = FObjLoader::LoadFromFile(path, m_Entries[slot].Scene);
    }
    else if (HasExtension(path, ".gltf") || HasExtension(path, ".glb"))
    {
        result = FGltfLoader::LoadFromFile(path, m_Entries[slot].Scene);
    }
    else
    {
        result = FAssetLoadResult::Failure(StringFormat(
            "无法由扩展名判定场景格式: {}", path.GetCStr()));
    }

    if (result.Succeeded)
    {
        m_Entries[slot].State = EAssetState::Loaded;
    }
    else
    {
        // 失败也要缓存 —— 一个损坏的文件可能被上百个材质引用,
        // 不缓存会导致上百次无谓的磁盘访问
        m_Entries[slot].State = EAssetState::Failed;
        m_Entries[slot].Error = result.ErrorMessage;
        m_Entries[slot].Scene.Reset();
    }

    m_KeyToSlot.Add(key, slot);

    FAssetHandle handle;
    handle.Index      = slot;
    handle.Generation = m_Entries[slot].Generation;

    return handle;
}

FAssetHandle FAssetRegistry::LoadImage(const FString& path,
                                       const FImageDecodeOptions& options)
{
    const FString key = MakeImageKey(path, options);

    const UInt32 existing = FindByKey(key);

    if (existing != FAssetHandle::kInvalidIndex)
    {
        ++m_CacheHits;

        FEntry& entry = m_Entries[existing];
        ++entry.ReferenceCount;

        FAssetHandle handle;
        handle.Index      = existing;
        handle.Generation = entry.Generation;

        return handle;
    }

    ++m_CacheMisses;

    const UInt32 slot = AcquireSlot();

    m_Entries[slot].Type           = EEntryType::Image;
    m_Entries[slot].Key            = key;
    m_Entries[slot].Path           = path;
    m_Entries[slot].ReferenceCount = 1;

    const FImageDecodeResult result =
        FImageDecoder::DecodeFile(path, m_Entries[slot].Image, options);

    if (result.Succeeded)
    {
        m_Entries[slot].State = EAssetState::Loaded;
    }
    else
    {
        m_Entries[slot].State = EAssetState::Failed;
        m_Entries[slot].Error = result.ErrorMessage;
        m_Entries[slot].Image.Reset();
    }

    m_KeyToSlot.Add(key, slot);

    FAssetHandle handle;
    handle.Index      = slot;
    handle.Generation = m_Entries[slot].Generation;

    return handle;
}

FAssetHandle FAssetRegistry::RegisterScene(const FString& key,
                                           FAssetScene&& scene)
{
    const FString normalizedKey = NormalizeKey(key);

    const UInt32 existing = FindByKey(normalizedKey);

    if (existing != FAssetHandle::kInvalidIndex)
    {
        ++m_CacheHits;

        FEntry& entry = m_Entries[existing];
        ++entry.ReferenceCount;

        FAssetHandle handle;
        handle.Index      = existing;
        handle.Generation = entry.Generation;

        return handle;
    }

    ++m_CacheMisses;

    const UInt32 slot = AcquireSlot();

    m_Entries[slot].Type           = EEntryType::Scene;
    m_Entries[slot].State          = EAssetState::Loaded;
    m_Entries[slot].Key            = normalizedKey;
    m_Entries[slot].Path           = key;
    m_Entries[slot].ReferenceCount = 1;
    m_Entries[slot].Scene          = static_cast<FAssetScene&&>(scene);

    m_KeyToSlot.Add(normalizedKey, slot);

    FAssetHandle handle;
    handle.Index      = slot;
    handle.Generation = m_Entries[slot].Generation;

    return handle;
}

FAssetHandle FAssetRegistry::RegisterImage(const FString& key,
                                           FImageData&& image)
{
    const FString normalizedKey = NormalizeKey(key);

    const UInt32 existing = FindByKey(normalizedKey);

    if (existing != FAssetHandle::kInvalidIndex)
    {
        ++m_CacheHits;

        FEntry& entry = m_Entries[existing];
        ++entry.ReferenceCount;

        FAssetHandle handle;
        handle.Index      = existing;
        handle.Generation = entry.Generation;

        return handle;
    }

    ++m_CacheMisses;

    const UInt32 slot = AcquireSlot();

    m_Entries[slot].Type           = EEntryType::Image;
    m_Entries[slot].State          = EAssetState::Loaded;
    m_Entries[slot].Key            = normalizedKey;
    m_Entries[slot].Path           = key;
    m_Entries[slot].ReferenceCount = 1;
    m_Entries[slot].Image          = static_cast<FImageData&&>(image);

    m_KeyToSlot.Add(normalizedKey, slot);

    FAssetHandle handle;
    handle.Index      = slot;
    handle.Generation = m_Entries[slot].Generation;

    return handle;
}

// ============================================================================
// 访问
// ============================================================================

const FAssetScene* FAssetRegistry::GetScene(FAssetHandle handle) const
{
    const FEntry* entry = Resolve(handle);

    if (entry == nullptr || entry->Type != EEntryType::Scene ||
        entry->State != EAssetState::Loaded)
    {
        return nullptr;
    }

    return &entry->Scene;
}

const FImageData* FAssetRegistry::GetImage(FAssetHandle handle) const
{
    const FEntry* entry = Resolve(handle);

    if (entry == nullptr || entry->Type != EEntryType::Image ||
        entry->State != EAssetState::Loaded)
    {
        return nullptr;
    }

    return &entry->Image;
}

EAssetState FAssetRegistry::GetState(FAssetHandle handle) const
{
    const FEntry* entry = Resolve(handle);
    return (entry != nullptr) ? entry->State : EAssetState::Empty;
}

FString FAssetRegistry::GetError(FAssetHandle handle) const
{
    const FEntry* entry = Resolve(handle);
    return (entry != nullptr) ? entry->Error : FString();
}

FString FAssetRegistry::GetPath(FAssetHandle handle) const
{
    const FEntry* entry = Resolve(handle);
    return (entry != nullptr) ? entry->Path : FString();
}

bool FAssetRegistry::IsValid(FAssetHandle handle) const
{
    return Resolve(handle) != nullptr;
}

// ============================================================================
// 引用计数
// ============================================================================

void FAssetRegistry::AddReference(FAssetHandle handle)
{
    if (FEntry* entry = Resolve(handle))
    {
        ++entry->ReferenceCount;
    }
}

void FAssetRegistry::ReleaseReference(FAssetHandle handle)
{
    FEntry* entry = Resolve(handle);

    if (entry != nullptr && entry->ReferenceCount > 0)
    {
        --entry->ReferenceCount;
    }
}

UInt32 FAssetRegistry::GetReferenceCount(FAssetHandle handle) const
{
    const FEntry* entry = Resolve(handle);
    return (entry != nullptr) ? entry->ReferenceCount : 0;
}

// ============================================================================
// 回收
// ============================================================================

UInt32 FAssetRegistry::CollectUnreferenced()
{
    UInt32 collected = 0;

    for (SizeType i = 0; i < m_Entries.GetSize(); ++i)
    {
        FEntry& entry = m_Entries[i];

        if (entry.Type == EEntryType::None || entry.ReferenceCount > 0)
        {
            continue;
        }

        m_KeyToSlot.Remove(entry.Key);

        ReleaseEntryData(entry);

        entry.Type  = EEntryType::None;
        entry.State = EAssetState::Empty;
        entry.Key.Clear();
        entry.Path.Clear();

        // 代际递增使一切指向该槽位的旧句柄立即失效
        ++entry.Generation;

        m_FreeSlots.Add(static_cast<UInt32>(i));
        ++collected;
    }

    return collected;
}

void FAssetRegistry::Unload(FAssetHandle handle)
{
    FEntry* entry = Resolve(handle);

    if (entry == nullptr)
    {
        return;
    }

    m_KeyToSlot.Remove(entry->Key);

    ReleaseEntryData(*entry);

    entry->Type           = EEntryType::None;
    entry->State          = EAssetState::Empty;
    entry->ReferenceCount = 0;
    entry->Key.Clear();
    entry->Path.Clear();

    ++entry->Generation;

    m_FreeSlots.Add(handle.Index);
}

void FAssetRegistry::Clear()
{
    for (SizeType i = 0; i < m_Entries.GetSize(); ++i)
    {
        ReleaseEntryData(m_Entries[i]);
    }

    m_Entries.Clear();
    m_FreeSlots.Clear();
    m_KeyToSlot.Clear();

    m_CacheHits   = 0;
    m_CacheMisses = 0;
}

// ============================================================================
// 统计
// ============================================================================

FAssetRegistryStats FAssetRegistry::GetStats() const
{
    FAssetRegistryStats stats;

    stats.CacheHits   = m_CacheHits;
    stats.CacheMisses = m_CacheMisses;

    for (SizeType i = 0; i < m_Entries.GetSize(); ++i)
    {
        const FEntry& entry = m_Entries[i];

        if (entry.Type == EEntryType::None)
        {
            continue;
        }

        if (entry.State == EAssetState::Failed)
        {
            ++stats.FailedCount;
        }

        if (entry.ReferenceCount == 0)
        {
            ++stats.UnreferencedCount;
        }

        if (entry.Type == EEntryType::Scene)
        {
            ++stats.SceneCount;
            stats.SceneBytes += EstimateSceneBytes(entry.Scene);
        }
        else if (entry.Type == EEntryType::Image)
        {
            ++stats.ImageCount;
            stats.ImageBytes += entry.Image.Pixels.GetSize();
        }
    }

    return stats;
}

SizeType FAssetRegistry::GetEntryCount() const
{
    SizeType count = 0;

    for (SizeType i = 0; i < m_Entries.GetSize(); ++i)
    {
        if (m_Entries[i].Type != EEntryType::None)
        {
            ++count;
        }
    }

    return count;
}

} // namespace Limx
