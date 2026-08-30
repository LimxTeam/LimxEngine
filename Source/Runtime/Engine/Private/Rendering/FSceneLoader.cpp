/*******************************************************************************
 * 文件: FSceneLoader.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   场景导入器实现 — 资产解析 → 纹理上传 → 材质创建 → 网格上传 → 节点生成
 *
 * 设计哲学:
 *   纹理按解析后的完整路径去重 — 解析器写入 FTextureReference::Path 的
 *   已经是可直接打开的路径，导入器不再二次拼接。同一份贴图被几十个材质
 *   引用是常态，逐材质上传会让显存翻好几倍。
 *
 *   材质持有纹理引用而非裸句柄 — 导入器放掉创建引用后, 纹理的存活完全
 *   由"还有多少材质在用它"决定。材质随场景销毁, 引用归零, 显存回落。
 *
 *   缺失贴图不中断导入 — 真实资产里贴图缺失是常态 (大小写、路径分隔符、
 *   压缩格式不支持)。缺一张贴图就整场导入失败，等于把一个显示问题升级成
 *   一个加载问题。这里记录计数并回退到材质的常量因子。
 *
 *   节点层级在导入时展平 — 资产的父子关系已经通过 ComputeWorldTransform
 *   烘进世界变换。保留层级需要在 LScene 里重建一棵平行的树，而当前没有
 *   任何功能依赖它; 等到需要骨骼或可动部件时再建，那时才知道该怎么建。
 *
 * 技术特性:
 *   - 路径→纹理句柄的线性查重表 (纹理种类通常是几十, 无需哈希)
 *   - 基色/自发光按 sRGB 上传, 法线/金属粗糙度/遮蔽按线性上传
 *   - 材质槽位一一映射到 LMeshTrait::SetSectionMaterial
 *   - 导入过程中的资源引用在交给 Trait 后立即释放创建引用
 *
 * 依赖关系:
 *   内部: Engine/Rendering/FSceneLoader.h, AssetPipeline/FAssetRegistry.h,
 *          AssetPipeline/FImageDecoder.h, RenderCore/Material/FMaterialManager.h
 *
 * 注意事项:
 *   同步导入 — 大场景会阻塞调用线程数秒
 *
 ******************************************************************************/

#include "Engine/EngineMinimal.h"
#include "Engine/Rendering/FSceneLoader.h"
#include "Core/Threading/FJobExecutor.h"
#include "AssetPipeline/FAssetRegistry.h"
#include "AssetPipeline/FImageDecoder.h"
#include "RenderCore/Material/FMaterialManager.h"

namespace Limx
{

LIMX_DECLARE_LOG_CATEGORY(LogEngine)

namespace
{

/// 导入期间的纹理去重表
///
/// 键是解析后的完整路径。纹理种类在真实场景里是几十的量级, 线性查找
/// 比引入一张哈希表更划算, 也不必为 FString 定义哈希。
struct FTextureCache
{
    TArray<FString>                Keys;
    TArray<FTextureResourceHandle> Handles;

    /// 查已上传的纹理 — 未命中返回无效句柄
    LIMX_NODISCARD FTextureResourceHandle Find(const FString& key) const
    {
        for (SizeType i = 0; i < Keys.GetSize(); ++i)
        {
            if (Keys[i] == key)
            {
                return Handles[i];
            }
        }

        return FTextureResourceHandle();
    }

    void Add(const FString& key, FTextureResourceHandle handle)
    {
        Keys.Add(key);
        Handles.Add(handle);
    }
};

/// 纹理阶段的分项计时 —— 解码与上传分开累加
struct FTextureTiming
{
    Float64 DecodeMs = 0.0;
    Float64 UploadMs = 0.0;
    UInt64  DecodedBytes = 0;
};

/// 计算一张纹理的缓存键
///
/// 色彩空间进键: 同一张图既当基色 (sRGB) 又当法线 (线性) 时, 只按路径
/// 作键会让后取的那次拿到先上传的那张 —— 色彩空间是错的, 而画面上只表现为
/// "法线好像有点怪"。Sponza 里没有这种用法, 但那是资产碰巧如此。
FString MakeTextureCacheKey(const FAssetScene& assetScene,
                            const FTextureReference& reference,
                            bool isSrgb)
{
    FString key;

    if (reference.EmbeddedIndex >= 0)
    {
        const SizeType embeddedIndex =
            static_cast<SizeType>(reference.EmbeddedIndex);

        if (embeddedIndex >= assetScene.EmbeddedImages.GetSize())
        {
            return key;
        }

        // 内嵌图像用 "名称 + 下标" 作键 —— 它们没有路径, 但同一份 GLB 里
        // 同一个下标必然是同一张图。
        key = FString("embedded:");
        key.Append(assetScene.EmbeddedImages[embeddedIndex].Name.GetCStr());
    }
    else
    {
        // 解析器已经把 BaseDirectory 拼进 Path 了, 这里直接用。
        key = reference.Path;
    }

    key.Append(isSrgb ? "|srgb" : "|linear");

    return key;
}

// ============================================================================
// 并行预解码
// ============================================================================

/// 一张待解码的纹理
struct FPendingTexture
{
    FString                  Key;
    const FTextureReference* Reference = nullptr;
    bool                     IsSrgb    = false;

    // ---- 工作线程填写 ----
    FImageData Image;
    bool       Succeeded = false;
    FString    Error;
};

/// 把一份材质里的一个槽位登记进待解码列表 (已在列表里则跳过)
void CollectPendingTexture(const FAssetScene&       assetScene,
                           const FTextureReference& reference,
                           bool                     isSrgb,
                           TArray<FPendingTexture>& pending)
{
    if (!reference.IsValid())
    {
        return;
    }

    FString key = MakeTextureCacheKey(assetScene, reference, isSrgb);

    if (key.IsEmpty())
    {
        return;
    }

    for (SizeType i = 0; i < pending.GetSize(); ++i)
    {
        if (pending[i].Key == key)
        {
            return;
        }
    }

    FPendingTexture entry;
    entry.Key       = MoveTemp(key);
    entry.Reference = &reference;
    entry.IsSrgb    = isSrgb;

    pending.Add(MoveTemp(entry));
}

/// 并行解码全部待解码纹理, 再串行上传进缓存
///
/// 为什么要分成"扇出解码 + 收拢上传"两段:
///   解码是纯 CPU 且解码器无可变静态状态, 可以随便扇出;
///   上传要走命令缓冲区, 而 BeginSingleTimeCommands 用的是同一个命令池,
///   Vulkan 要求命令池由调用方外部同步 —— 多线程同时从一个池里分配是
///   未定义行为。资源管理器本身也一把锁都没有。
///
/// 因此上传留在调用线程上。实测它只占导入总时间的 2%, 并行化它的收益
/// 远小于给整条资源路径加锁的代价与风险。
void PrefetchTextures(FRenderResourceManager& resources,
                      FTextureCache&          cache,
                      const FAssetScene&      assetScene,
                      TArray<FPendingTexture>& pending,
                      bool                     useAnisotropy,
                      UInt32&                  outMissingCount,
                      FTextureTiming&          timing)
{
    if (pending.IsEmpty())
    {
        return;
    }

    // ---- 扇出解码 ----
    const Float64 decodeBegin = FPlatformTime::Seconds();

    {
        // 图随本次导入建、随本次导入拆。相比常驻一张全局图, 这里多付出的
        // 是几毫秒的线程创建 —— 与数秒的解码相比可以忽略, 换来的是不必
        // 处理全局图的初始化与关闭时序。等到有多处需要它时再提升为常驻。
        FTaskGraph graph;
        graph.Initialize(0);

        FPendingTexture*   entries     = pending.GetData();
        const FAssetScene* scenePtr    = &assetScene;

        // 批大小取 1: 每张图的解码耗时差异很大 (贴图尺寸从 512 到 2048
        // 不等), 细粒度才能让先做完的线程立刻接下一张。
        FJobExecutor::ParallelFor(
            graph, pending.GetSize(), 1,
            [entries, scenePtr](SizeType begin, SizeType end)
            {
                for (SizeType i = begin; i < end; ++i)
                {
                    FPendingTexture& entry = entries[i];

                    FImageDecodeResult decodeResult;

                    if (entry.Reference->EmbeddedIndex >= 0)
                    {
                        const FEmbeddedImage& embedded =
                            scenePtr->EmbeddedImages[
                                static_cast<SizeType>(
                                    entry.Reference->EmbeddedIndex)];

                        decodeResult = FImageDecoder::Decode(
                            embedded.Bytes.GetData(),
                            embedded.Bytes.GetSize(),
                            entry.Image);
                    }
                    else
                    {
                        decodeResult = FImageDecoder::DecodeFile(
                            entry.Reference->Path, entry.Image);
                    }

                    entry.Succeeded = decodeResult.Succeeded;

                    // 失败信息带回主线程再打 —— FLog 没有加锁, 从工作线程
                    // 写日志会让多条记录交错甚至撕裂。
                    if (!decodeResult.Succeeded)
                    {
                        entry.Error = decodeResult.ErrorMessage;
                    }
                }
            });

        graph.Shutdown();
    }

    timing.DecodeMs += (FPlatformTime::Seconds() - decodeBegin) * 1000.0;

    // ---- 收拢上传 ----
    const Float64 uploadBegin = FPlatformTime::Seconds();

    for (SizeType i = 0; i < pending.GetSize(); ++i)
    {
        FPendingTexture& entry = pending[i];

        if (!entry.Succeeded)
        {
            LIMX_LOG(LogEngine, Warning,
                     "[场景导入] 纹理 '{}' 解码失败: {}",
                     entry.Key.GetCStr(), entry.Error.GetCStr());
            ++outMissingCount;
            continue;
        }

        FTextureUploadOptions uploadOptions;
        uploadOptions.IsSrgb        = entry.IsSrgb;
        uploadOptions.UseAnisotropy = useAnisotropy;

        timing.DecodedBytes += entry.Image.Pixels.GetSize();

        const FTextureResourceHandle handle = resources.CreateTexture(
            entry.Image, uploadOptions, FName(entry.Key.GetCStr()));

        if (!handle.IsValid())
        {
            ++outMissingCount;
            continue;
        }

        cache.Add(entry.Key, handle);

        // 像素数据已经进了显存, 立刻放掉 —— 69 张 2K 贴图解出来是几百 MiB,
        // 全留到导入结束会让内存峰值翻好几倍。
        entry.Image.Reset();
    }

    timing.UploadMs += (FPlatformTime::Seconds() - uploadBegin) * 1000.0;
}

/// 取或上传一张纹理
///
/// @param isSrgb 由调用方按贴图在材质中的用途决定, 而非从文件推断
FTextureResourceHandle AcquireTexture(FRenderResourceManager& resources,
                                      FTextureCache& cache,
                                      const FAssetScene& assetScene,
                                      const FTextureReference& reference,
                                      bool isSrgb,
                                      bool useAnisotropy,
                                      UInt32& outMissingCount,
                                      FTextureTiming& timing)
{
    if (!reference.IsValid())
    {
        return FTextureResourceHandle();
    }

    // 正常路径下这里必然命中 —— PrefetchTextures 已经把本次导入用到的
    // 每一张图都解好并上传了。没命中只有两种可能: 那张图解码失败, 或者
    // 收集阶段漏掉了这个槽位。两者都该报出来, 而不是在这里默默重解一遍
    // (那会让并行化的收益被悄悄吃掉, 且没人知道)。
    const FString cacheKey = MakeTextureCacheKey(assetScene, reference, isSrgb);

    const FTextureResourceHandle cached = cache.Find(cacheKey);

    if (cached.IsValid())
    {
        return cached;
    }

    static_cast<void>(resources);
    static_cast<void>(useAnisotropy);
    static_cast<void>(timing);

    return FTextureResourceHandle();
}

/// 把一张纹理绑定到材质槽位; 句柄无效时保持默认贴图
///
/// 走引用计数版本 —— 材质持有一份自己的引用, 材质销毁时释放。
/// 这是导入的纹理能在运行时被回收的前提。
void BindMaterialTexture(FMaterial* material, UInt32 slot,
                         FRenderResourceManager& resources,
                         FTextureResourceHandle handle)
{
    if (material == nullptr || !handle.IsValid())
    {
        return;
    }

    material->BindTextureResource(slot, &resources, handle);
}

/// 把资产的 alpha 模式映射为渲染层的混合模式
EMaterialBlendMode MapBlendMode(EAlphaMode alphaMode)
{
    switch (alphaMode)
    {
    case EAlphaMode::Mask:  return EMaterialBlendMode::Masked;
    case EAlphaMode::Blend: return EMaterialBlendMode::Translucent;
    default:                return EMaterialBlendMode::Opaque;
    }
}

} // namespace

// ============================================================================
// LoadInto — 导入主流程
// ============================================================================

FSceneLoadResult FSceneLoader::LoadInto(LScene* scene,
                                         FRenderContext* context,
                                         const FString& path,
                                         const FSceneLoadOptions& options)
{
    FSceneLoadResult result;

    const Float64 startTime = FPlatformTime::Seconds();

    if (scene == nullptr || context == nullptr)
    {
        result.Error = FString("场景或渲染上下文为空");
        return result;
    }

    // ---- 1. 解析资产 ----
    static FAssetRegistry s_Registry;

    const Float64 parseBegin = FPlatformTime::Seconds();

    const FAssetHandle assetHandle = s_Registry.LoadScene(path);

    result.ParseMilliseconds =
        (FPlatformTime::Seconds() - parseBegin) * 1000.0;

    if (!assetHandle.IsValid())
    {
        result.Error = FString("资产解析失败: ");
        result.Error.Append(s_Registry.GetError(assetHandle));
        return result;
    }

    FTextureTiming textureTiming;

    const FAssetScene* assetScene = s_Registry.GetScene(assetHandle);

    if (assetScene == nullptr || assetScene->IsEmpty())
    {
        result.Error = FString("资产不含任何网格");
        s_Registry.ReleaseReference(assetHandle);
        return result;
    }

    FRenderResourceManager& resources = context->GetResourceManager();

    // ---- 2. 材质 (含纹理) ----
    FTextureCache      textureCache;
    TArray<FMaterial*> materials;

    materials.Reserve(assetScene->Materials.GetSize());

    // ---- 2a. 先把所有要用到的贴图收集起来, 并行解码后一次性上传 ----
    //
    // 原先是逐材质逐槽位"解一张、传一张", 解码因此完全串行 —— 而实测它
    // 占导入总时间的 91% (Sponza: 5530 ms / 6085 ms)。
    //
    // 收集阶段按缓存键去重: Sponza 的 25 个材质共引用 125 个槽位, 去重后
    // 只有 69 张不同的图。不去重的话并行解码会把同一张图解上好几遍。
    if (options.LoadTextures)
    {
        TArray<FPendingTexture> pending;

        for (SizeType i = 0; i < assetScene->Materials.GetSize(); ++i)
        {
            const FMaterialData& source = assetScene->Materials[i];

            CollectPendingTexture(*assetScene, source.BaseColorTexture,
                                  true, pending);
            CollectPendingTexture(*assetScene, source.NormalTexture,
                                  false, pending);
            CollectPendingTexture(*assetScene, source.MetallicRoughnessTexture,
                                  false, pending);
            CollectPendingTexture(*assetScene, source.OcclusionTexture,
                                  false, pending);
            CollectPendingTexture(*assetScene, source.EmissiveTexture,
                                  true, pending);
        }

        PrefetchTextures(resources, textureCache, *assetScene, pending,
                         options.UseAnisotropy, result.MissingTextureCount,
                         textureTiming);
    }

    for (SizeType i = 0; i < assetScene->Materials.GetSize(); ++i)
    {
        const FMaterialData& source = assetScene->Materials[i];

        FMaterial* material =
            FMaterialManager::Get().CreateMaterial(source.Name.GetCStr());

        if (material == nullptr)
        {
            LIMX_LOG(LogEngine, Warning,
                     "[场景导入] 材质 '{}' 创建失败", source.Name.GetCStr());
            materials.Add(nullptr);
            continue;
        }

        material->SetBaseColor(source.BaseColorFactor);
        material->SetMetallic(source.MetallicFactor);
        material->SetRoughness(source.RoughnessFactor);
        material->SetEmissiveColor(source.EmissiveFactor);
        material->SetNormalScale(source.NormalScale);
        material->SetAlphaCutoff(source.AlphaCutoff);
        material->SetBlendMode(MapBlendMode(source.AlphaMode));
        material->SetDoubleSided(source.DoubleSided);

        if (options.LoadTextures)
        {
            // 色彩空间按用途区分: 基色与自发光是 sRGB 编码的颜色,
            // 其余三张是线性数值。按错会让画面整体发暗或光照方向系统性偏移。
            BindMaterialTexture(material, kMaterialTextureSlotAlbedo, resources,
                AcquireTexture(resources, textureCache, *assetScene,
                               source.BaseColorTexture, true,
                               options.UseAnisotropy,
                               result.MissingTextureCount,
                               textureTiming));

            BindMaterialTexture(material, kMaterialTextureSlotNormal, resources,
                AcquireTexture(resources, textureCache, *assetScene,
                               source.NormalTexture, false,
                               options.UseAnisotropy,
                               result.MissingTextureCount,
                               textureTiming));

            BindMaterialTexture(material, kMaterialTextureSlotMetallicRoughness,
                resources,
                AcquireTexture(resources, textureCache, *assetScene,
                               source.MetallicRoughnessTexture, false,
                               options.UseAnisotropy,
                               result.MissingTextureCount,
                               textureTiming));

            BindMaterialTexture(material, kMaterialTextureSlotOcclusion,
                resources,
                AcquireTexture(resources, textureCache, *assetScene,
                               source.OcclusionTexture, false,
                               options.UseAnisotropy,
                               result.MissingTextureCount,
                               textureTiming));

            BindMaterialTexture(material, kMaterialTextureSlotEmissive,
                resources,
                AcquireTexture(resources, textureCache, *assetScene,
                               source.EmissiveTexture, true,
                               options.UseAnisotropy,
                               result.MissingTextureCount,
                               textureTiming));
        }

        materials.Add(material);
    }

    result.MaterialCount = static_cast<UInt32>(materials.GetSize());
    result.Materials     = materials;
    result.TextureCount  = static_cast<UInt32>(textureCache.Handles.GetSize());

    // ---- 3. 网格 ----
    TArray<FMeshResourceHandle> meshHandles;
    meshHandles.Reserve(assetScene->Meshes.GetSize());

    const Float64 meshUploadBegin = FPlatformTime::Seconds();

    for (SizeType i = 0; i < assetScene->Meshes.GetSize(); ++i)
    {
        const FMeshData& meshData = assetScene->Meshes[i];

        const FMeshResourceHandle handle =
            resources.CreateMesh(meshData, meshData.Name);

        meshHandles.Add(handle);

        if (!handle.IsValid())
        {
            LIMX_LOG(LogEngine, Warning,
                     "[场景导入] 网格 '{}' 上传失败",
                     meshData.Name.GetCStr());
            continue;
        }

        ++result.MeshCount;
        result.VertexCount   += meshData.Vertices.GetSize();
        result.TriangleCount += meshData.Indices.GetSize() / 3;
    }

    result.MeshUploadMilliseconds =
        (FPlatformTime::Seconds() - meshUploadBegin) * 1000.0;

    // ---- 4. 节点 ----
    for (SizeType i = 0; i < assetScene->Nodes.GetSize(); ++i)
    {
        if (options.MaxNodes > 0 && result.NodeCount >= options.MaxNodes)
        {
            LIMX_LOG(LogEngine, Warning,
                     "[场景导入] 达到节点上限 {}, 其余节点被跳过",
                     options.MaxNodes);
            break;
        }

        const FSceneNode& sourceNode = assetScene->Nodes[i];

        if (sourceNode.MeshIndex < 0)
        {
            continue;
        }

        const SizeType meshIndex = static_cast<SizeType>(sourceNode.MeshIndex);

        if (meshIndex >= meshHandles.GetSize() ||
            !meshHandles[meshIndex].IsValid())
        {
            continue;
        }

        // 层级在此展平 —— 父链的累乘由资产层完成
        FTransform worldTransform =
            assetScene->ComputeWorldTransform(static_cast<Int32>(i));

        worldTransform.Translation = worldTransform.Translation *
                                     options.UniformScale;
        worldTransform.Scale3D     = worldTransform.Scale3D *
                                     options.UniformScale;

        LNode* node = scene->SpawnNode<LNode>(sourceNode.Name, worldTransform);

        LMeshTrait* meshTrait = node->AddTrait<LMeshTrait>(FName("Mesh"));
        meshTrait->SetMesh(&resources, meshHandles[meshIndex]);
        meshTrait->SetVisible(true);

        // 逐槽位材质 —— 子网格的 MaterialSlot 就是资产的材质下标
        const FMeshData& meshData = assetScene->Meshes[meshIndex];

        for (SizeType s = 0; s < meshData.SubMeshes.GetSize(); ++s)
        {
            const Int32 materialIndex = meshData.SubMeshes[s].MaterialIndex;

            if (materialIndex < 0 ||
                static_cast<SizeType>(materialIndex) >= materials.GetSize())
            {
                continue;
            }

            meshTrait->SetSectionMaterial(materialIndex,
                                          materials[materialIndex]);
        }

        // 没有任何槽位材质时给一个兜底, 否则该节点会被静默跳过绘制
        if (materials.GetSize() > 0 && materials[0] != nullptr)
        {
            meshTrait->SetMaterial(materials[0]);
        }
        else
        {
            meshTrait->SetMaterial(
                FMaterialManager::Get().CreateDefaultMaterial("ImportFallback"));
        }

        ++result.NodeCount;
    }

    // ---- 5. 交出网格的创建引用 ----
    //
    // 每个引用该网格的 Trait 都已在 SetMesh 中加过自己那一份。
    // 不放掉这里的创建引用, 网格就永远不会随场景一起变成可回收状态。
    for (SizeType i = 0; i < meshHandles.GetSize(); ++i)
    {
        if (meshHandles[i].IsValid())
        {
            resources.ReleaseMeshReference(meshHandles[i]);
        }
    }

    // ---- 6. 交出纹理的创建引用 ----
    //
    // 每个引用该纹理的材质都已在 BindTextureResource 中加过自己那一份,
    // 因此这里可以安全地放掉创建时的引用。材质全部销毁后引用归零,
    // CollectUnreferenced 即可回收 —— 这正是关卡切换时显存能回落的原因。
    for (SizeType i = 0; i < textureCache.Handles.GetSize(); ++i)
    {
        resources.ReleaseTextureReference(textureCache.Handles[i]);
    }

    result.Bounds = assetScene->Bounds;

    // 包围盒同样要按导入缩放换算, 否则调用方据此摆放的相机会差一个量级
    if (result.Bounds.IsValid() && options.UniformScale != 1.0f)
    {
        result.Bounds = FBoundingBox(result.Bounds.Min * options.UniformScale,
                                     result.Bounds.Max * options.UniformScale);
    }

    // CPU 侧资产数据已全部转成 GPU 资源与场景节点, 可以放手了
    s_Registry.ReleaseReference(assetHandle);
    s_Registry.CollectUnreferenced();

    result.Succeeded           = result.NodeCount > 0;
    result.ElapsedMilliseconds =
        (FPlatformTime::Seconds() - startTime) * 1000.0;

    result.TextureDecodeMilliseconds = textureTiming.DecodeMs;
    result.TextureUploadMilliseconds = textureTiming.UploadMs;
    result.DecodedImageBytes         = textureTiming.DecodedBytes;

    if (!result.Succeeded)
    {
        result.Error = FString("资产解析成功但没有生成任何可绘制节点");
    }

    return result;
}

// ============================================================================
// UnloadMaterials — 销毁本次导入创建的材质
// ============================================================================

UInt32 FSceneLoader::UnloadMaterials(const FSceneLoadResult& result)
{
    UInt32 destroyed = 0;

    for (SizeType i = 0; i < result.Materials.GetSize(); ++i)
    {
        if (result.Materials[i] != nullptr)
        {
            FMaterialManager::Get().DestroyMaterial(result.Materials[i]);
            ++destroyed;
        }
    }

    return destroyed;
}

} // namespace Limx
