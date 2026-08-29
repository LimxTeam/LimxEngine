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

/// 取或上传一张纹理
///
/// @param isSrgb 由调用方按贴图在材质中的用途决定, 而非从文件推断
FTextureResourceHandle AcquireTexture(FRenderResourceManager& resources,
                                      FTextureCache& cache,
                                      const FAssetScene& assetScene,
                                      const FTextureReference& reference,
                                      bool isSrgb,
                                      bool useAnisotropy,
                                      UInt32& outMissingCount)
{
    if (!reference.IsValid())
    {
        return FTextureResourceHandle();
    }

    FImageData image;
    FString    cacheKey;

    if (reference.EmbeddedIndex >= 0)
    {
        const SizeType embeddedIndex =
            static_cast<SizeType>(reference.EmbeddedIndex);

        if (embeddedIndex >= assetScene.EmbeddedImages.GetSize())
        {
            ++outMissingCount;
            return FTextureResourceHandle();
        }

        const FEmbeddedImage& embedded =
            assetScene.EmbeddedImages[embeddedIndex];

        // 内嵌图像用 "名称 + 下标" 作键 —— 它们没有路径, 但同一份 GLB 里
        // 同一个下标必然是同一张图。
        cacheKey = FString("embedded:");
        cacheKey.Append(embedded.Name.GetCStr());

        FTextureResourceHandle cached = cache.Find(cacheKey);
        if (cached.IsValid())
        {
            return cached;
        }

        const FImageDecodeResult decodeResult = FImageDecoder::Decode(
            embedded.Bytes.GetData(), embedded.Bytes.GetSize(), image);

        if (!decodeResult.Succeeded)
        {
            LIMX_LOG(LogEngine, Warning,
                     "[场景导入] 内嵌纹理 '{}' 解码失败: {}",
                     embedded.Name.GetCStr(), decodeResult.ErrorMessage);
            ++outMissingCount;
            return FTextureResourceHandle();
        }
    }
    else
    {
        // 解析器已经把 BaseDirectory 拼进 Path 了, 这里直接用。
        // 参见 FTextureReference::Path 的契约说明。
        cacheKey = reference.Path;

        FTextureResourceHandle cached = cache.Find(cacheKey);
        if (cached.IsValid())
        {
            return cached;
        }

        const FImageDecodeResult decodeResult =
            FImageDecoder::DecodeFile(cacheKey, image);

        if (!decodeResult.Succeeded)
        {
            LIMX_LOG(LogEngine, Warning,
                     "[场景导入] 纹理 '{}' 加载失败: {}",
                     cacheKey.GetCStr(), decodeResult.ErrorMessage);
            ++outMissingCount;
            return FTextureResourceHandle();
        }
    }

    FTextureUploadOptions uploadOptions;
    uploadOptions.IsSrgb        = isSrgb;
    uploadOptions.UseAnisotropy = useAnisotropy;

    const FTextureResourceHandle handle =
        resources.CreateTexture(image, uploadOptions, FName(cacheKey.GetCStr()));

    if (!handle.IsValid())
    {
        ++outMissingCount;
        return handle;
    }

    cache.Add(cacheKey, handle);

    return handle;
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

    const FAssetHandle assetHandle = s_Registry.LoadScene(path);

    if (!assetHandle.IsValid())
    {
        result.Error = FString("资产解析失败: ");
        result.Error.Append(s_Registry.GetError(assetHandle));
        return result;
    }

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
                               result.MissingTextureCount));

            BindMaterialTexture(material, kMaterialTextureSlotNormal, resources,
                AcquireTexture(resources, textureCache, *assetScene,
                               source.NormalTexture, false,
                               options.UseAnisotropy,
                               result.MissingTextureCount));

            BindMaterialTexture(material, kMaterialTextureSlotMetallicRoughness,
                resources,
                AcquireTexture(resources, textureCache, *assetScene,
                               source.MetallicRoughnessTexture, false,
                               options.UseAnisotropy,
                               result.MissingTextureCount));

            BindMaterialTexture(material, kMaterialTextureSlotOcclusion,
                resources,
                AcquireTexture(resources, textureCache, *assetScene,
                               source.OcclusionTexture, false,
                               options.UseAnisotropy,
                               result.MissingTextureCount));

            BindMaterialTexture(material, kMaterialTextureSlotEmissive,
                resources,
                AcquireTexture(resources, textureCache, *assetScene,
                               source.EmissiveTexture, true,
                               options.UseAnisotropy,
                               result.MissingTextureCount));
        }

        materials.Add(material);
    }

    result.MaterialCount = static_cast<UInt32>(materials.GetSize());
    result.Materials     = materials;
    result.TextureCount  = static_cast<UInt32>(textureCache.Handles.GetSize());

    // ---- 3. 网格 ----
    TArray<FMeshResourceHandle> meshHandles;
    meshHandles.Reserve(assetScene->Meshes.GetSize());

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
