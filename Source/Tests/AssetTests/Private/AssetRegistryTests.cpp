/*******************************************************************************
 * 文件: AssetRegistryTests.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   FAssetRegistry 单元测试 — 路径去重、代际句柄失效、引用计数、
 *   延迟回收与统计
 *
 * 设计哲学:
 *   悬垂必须被验证为可检出 — 代际句柄的全部价值在于"槽位复用后旧句柄失效"。
 *   若不构造这一场景并断言 GetScene 返回 nullptr，代际号就只是个没人验证的
 *   摆设，而它失效的后果是读到别的资产的数据。
 *
 *   去重要用命中计数量化 — 断言"两次加载返回同一句柄"只能说明结果相同，
 *   不能说明第二次没有重新解码。命中/未命中计数才是去重是否真正生效的证据。
 *
 * 技术特性:
 *   - 用 RegisterScene/RegisterImage 注入内存数据, 不依赖磁盘文件
 *   - 覆盖槽位复用后的句柄失效与新句柄有效两种情形
 *
 * 依赖关系:
 *   内部: AssetTests/AssetTestsMinimal.h
 *
 * 注意事项:
 *   涉及磁盘加载的路径由集成验证覆盖, 单测只走内存注册接口
 *
 ******************************************************************************/

#include "AssetTests/AssetTestsMinimal.h"

using namespace Limx;

namespace
{

/// 构造一个含单个三角形的最小场景
FAssetScene MakeTestScene(Float32 scale)
{
    FAssetScene scene;

    FMeshData mesh;
    mesh.Name = FName("TestMesh");

    FMeshVertex vertex;

    vertex.Position = FVector3(0.0f, 0.0f, 0.0f);
    mesh.Vertices.Add(vertex);

    vertex.Position = FVector3(scale, 0.0f, 0.0f);
    mesh.Vertices.Add(vertex);

    vertex.Position = FVector3(0.0f, scale, 0.0f);
    mesh.Vertices.Add(vertex);

    mesh.Indices.Add(0);
    mesh.Indices.Add(1);
    mesh.Indices.Add(2);

    FSubMesh subMesh;
    subMesh.IndexOffset = 0;
    subMesh.IndexCount  = 3;
    mesh.SubMeshes.Add(subMesh);

    mesh.RecomputeBounds();

    scene.Meshes.Add(static_cast<FMeshData&&>(mesh));

    FSceneNode node;
    node.MeshIndex = 0;
    scene.Nodes.Add(node);
    scene.RootNodes.Add(0);

    return scene;
}

/// 构造一张纯色图像
FImageData MakeTestImage(UInt32 width, UInt32 height, UInt8 value)
{
    FImageData image;

    image.Width  = width;
    image.Height = height;
    image.Format = EImageFormat::RGBA8;

    const SizeType byteCount =
        static_cast<SizeType>(width) * height * 4;

    image.Pixels.Reserve(byteCount);

    for (SizeType i = 0; i < byteCount; ++i)
    {
        image.Pixels.Add(value);
    }

    return image;
}

} // namespace

// ============================================================================
// 基本注册与访问
// ============================================================================

LIMX_TEST(AssetRegistry, RegistersAndRetrievesScene)
{
    FAssetRegistry registry;

    const FAssetHandle handle =
        registry.RegisterScene(FString("test/mesh"), MakeTestScene(1.0f));

    LIMX_REQUIRE_TRUE(handle.IsValid());
    LIMX_REQUIRE_TRUE(registry.IsValid(handle));

    const FAssetScene* scene = registry.GetScene(handle);

    LIMX_REQUIRE_NOT_NULL(scene);
    LIMX_EXPECT_EQ(scene->Meshes.GetSize(), SizeType(1));
    LIMX_EXPECT_EQ(scene->GetTotalTriangleCount(), SizeType(1));

    LIMX_EXPECT_EQ(static_cast<Int32>(registry.GetState(handle)),
                   static_cast<Int32>(EAssetState::Loaded));
}

LIMX_TEST(AssetRegistry, RegistersAndRetrievesImage)
{
    FAssetRegistry registry;

    const FAssetHandle handle =
        registry.RegisterImage(FString("test/tex"), MakeTestImage(4, 4, 200));

    LIMX_REQUIRE_TRUE(handle.IsValid());

    const FImageData* image = registry.GetImage(handle);

    LIMX_REQUIRE_NOT_NULL(image);
    LIMX_EXPECT_EQ(image->Width, UInt32(4));
    LIMX_EXPECT_EQ(image->Height, UInt32(4));
    LIMX_EXPECT_EQ(image->Pixels[0], UInt8(200));
}

LIMX_TEST(AssetRegistry, TypeMismatchReturnsNull)
{
    FAssetRegistry registry;

    const FAssetHandle sceneHandle =
        registry.RegisterScene(FString("s"), MakeTestScene(1.0f));

    // 用场景句柄取图像应返回空而非把内存当作另一种类型解释
    LIMX_EXPECT_NULL(registry.GetImage(sceneHandle));
    LIMX_EXPECT_NOT_NULL(registry.GetScene(sceneHandle));
}

LIMX_TEST(AssetRegistry, DefaultHandleIsInvalid)
{
    FAssetRegistry registry;

    const FAssetHandle handle;

    LIMX_EXPECT_FALSE(handle.IsValid());
    LIMX_EXPECT_FALSE(registry.IsValid(handle));
    LIMX_EXPECT_NULL(registry.GetScene(handle));
    LIMX_EXPECT_NULL(registry.GetImage(handle));
    LIMX_EXPECT_EQ(registry.GetReferenceCount(handle), UInt32(0));
}

// ============================================================================
// 去重
// ============================================================================

LIMX_TEST(AssetRegistry, SameKeyReturnsSameEntry)
{
    FAssetRegistry registry;

    const FAssetHandle first =
        registry.RegisterScene(FString("shared"), MakeTestScene(1.0f));

    // 第二次以相同键注册应命中已有条目, 而非新增
    const FAssetHandle second =
        registry.RegisterScene(FString("shared"), MakeTestScene(99.0f));

    LIMX_EXPECT_TRUE(first == second);
    LIMX_EXPECT_EQ(registry.GetEntryCount(), SizeType(1));

    // 内容应是首次注册的那份 — 第二次的数据被丢弃
    const FAssetScene* scene = registry.GetScene(first);
    LIMX_REQUIRE_NOT_NULL(scene);
    LIMX_EXPECT_NEAR(scene->Meshes[0].Bounds.Max.X, 1.0f, 1.0e-5f);
}

LIMX_TEST(AssetRegistry, DeduplicationIsCountedAsCacheHit)
{
    FAssetRegistry registry;

    registry.RegisterScene(FString("a"), MakeTestScene(1.0f));
    registry.RegisterScene(FString("a"), MakeTestScene(1.0f));
    registry.RegisterScene(FString("a"), MakeTestScene(1.0f));

    const FAssetRegistryStats stats = registry.GetStats();

    // 命中计数才能证明后两次确实没有重新解码, 而不只是返回了相同的句柄
    LIMX_EXPECT_EQ(stats.CacheMisses, UInt64(1));
    LIMX_EXPECT_EQ(stats.CacheHits, UInt64(2));
    LIMX_EXPECT_EQ(stats.SceneCount, UInt32(1));
}

LIMX_TEST(AssetRegistry, KeyNormalizationMergesEquivalentPaths)
{
    FAssetRegistry registry;

    // Windows 路径大小写不敏感, 分隔符也可能混用 —
    // 不归一化会让同一文件被解码多次
    const FAssetHandle first =
        registry.RegisterScene(FString("Assets/Meshes/Cube.obj"),
                               MakeTestScene(1.0f));

    const FAssetHandle second =
        registry.RegisterScene(FString("assets\\meshes\\cube.obj"),
                               MakeTestScene(2.0f));

    LIMX_EXPECT_TRUE(first == second);
    LIMX_EXPECT_EQ(registry.GetEntryCount(), SizeType(1));
}

LIMX_TEST(AssetRegistry, DifferentKeysAreSeparateEntries)
{
    FAssetRegistry registry;

    const FAssetHandle first =
        registry.RegisterScene(FString("a"), MakeTestScene(1.0f));
    const FAssetHandle second =
        registry.RegisterScene(FString("b"), MakeTestScene(2.0f));

    LIMX_EXPECT_TRUE(first != second);
    LIMX_EXPECT_EQ(registry.GetEntryCount(), SizeType(2));

    LIMX_EXPECT_NEAR(registry.GetScene(first)->Meshes[0].Bounds.Max.X,
                     1.0f, 1.0e-5f);
    LIMX_EXPECT_NEAR(registry.GetScene(second)->Meshes[0].Bounds.Max.X,
                     2.0f, 1.0e-5f);
}

// ============================================================================
// 引用计数与回收
// ============================================================================

LIMX_TEST(AssetRegistry, ReferenceCountTracksLoadsAndReleases)
{
    FAssetRegistry registry;

    const FAssetHandle handle =
        registry.RegisterScene(FString("counted"), MakeTestScene(1.0f));

    // 首次注册即持有一份引用
    LIMX_EXPECT_EQ(registry.GetReferenceCount(handle), UInt32(1));

    registry.AddReference(handle);
    registry.AddReference(handle);
    LIMX_EXPECT_EQ(registry.GetReferenceCount(handle), UInt32(3));

    registry.ReleaseReference(handle);
    LIMX_EXPECT_EQ(registry.GetReferenceCount(handle), UInt32(2));

    // 重复注册也应递增引用
    registry.RegisterScene(FString("counted"), MakeTestScene(1.0f));
    LIMX_EXPECT_EQ(registry.GetReferenceCount(handle), UInt32(3));
}

LIMX_TEST(AssetRegistry, ReleaseDoesNotUnloadImmediately)
{
    FAssetRegistry registry;

    const FAssetHandle handle =
        registry.RegisterScene(FString("lazy"), MakeTestScene(1.0f));

    registry.ReleaseReference(handle);

    // 引用归零后数据仍然可用 —— 材质切换与 LOD 过渡会在一帧内
    // 放下又拾起资源, 立即释放会造成反复解码
    LIMX_EXPECT_EQ(registry.GetReferenceCount(handle), UInt32(0));
    LIMX_EXPECT_NOT_NULL(registry.GetScene(handle));
    LIMX_EXPECT_TRUE(registry.IsValid(handle));
}

LIMX_TEST(AssetRegistry, CollectUnreferencedFreesZeroRefEntries)
{
    FAssetRegistry registry;

    const FAssetHandle kept =
        registry.RegisterScene(FString("kept"), MakeTestScene(1.0f));
    const FAssetHandle dropped =
        registry.RegisterScene(FString("dropped"), MakeTestScene(2.0f));

    registry.ReleaseReference(dropped);

    const UInt32 collected = registry.CollectUnreferenced();

    LIMX_EXPECT_EQ(collected, UInt32(1));

    // 仍被引用的条目不受影响
    LIMX_EXPECT_NOT_NULL(registry.GetScene(kept));

    // 被回收的条目连同其句柄一并失效
    LIMX_EXPECT_NULL(registry.GetScene(dropped));
    LIMX_EXPECT_FALSE(registry.IsValid(dropped));
}

LIMX_TEST(AssetRegistry, CollectIsNoOpWhenAllReferenced)
{
    FAssetRegistry registry;

    registry.RegisterScene(FString("a"), MakeTestScene(1.0f));
    registry.RegisterScene(FString("b"), MakeTestScene(2.0f));

    LIMX_EXPECT_EQ(registry.CollectUnreferenced(), UInt32(0));
    LIMX_EXPECT_EQ(registry.GetEntryCount(), SizeType(2));
}

LIMX_TEST(AssetRegistry, UnloadIgnoresReferenceCount)
{
    FAssetRegistry registry;

    const FAssetHandle handle =
        registry.RegisterScene(FString("forced"), MakeTestScene(1.0f));

    registry.AddReference(handle);
    registry.AddReference(handle);
    LIMX_REQUIRE_EQ(registry.GetReferenceCount(handle), UInt32(3));

    registry.Unload(handle);

    LIMX_EXPECT_FALSE(registry.IsValid(handle));
    LIMX_EXPECT_NULL(registry.GetScene(handle));
}

// ============================================================================
// 代际句柄
// ============================================================================

LIMX_TEST(AssetRegistry, StaleHandleIsRejectedAfterSlotReuse)
{
    FAssetRegistry registry;

    const FAssetHandle original =
        registry.RegisterScene(FString("first"), MakeTestScene(1.0f));

    LIMX_REQUIRE_NOT_NULL(registry.GetScene(original));

    // 卸载后槽位进入空闲列表
    registry.ReleaseReference(original);
    LIMX_REQUIRE_EQ(registry.CollectUnreferenced(), UInt32(1));

    // 新资产会复用同一个槽位
    const FAssetHandle reused =
        registry.RegisterScene(FString("second"), MakeTestScene(2.0f));

    LIMX_REQUIRE_TRUE(registry.IsValid(reused));

    // 槽位索引相同但代际不同 —— 这正是代际号存在的意义:
    // 旧句柄必须失效, 而不是悄悄读到新资产的数据
    LIMX_EXPECT_EQ(original.Index, reused.Index);
    LIMX_EXPECT_NE(original.Generation, reused.Generation);

    LIMX_EXPECT_FALSE(registry.IsValid(original));
    LIMX_EXPECT_NULL(registry.GetScene(original));

    // 新句柄取到的是新数据
    const FAssetScene* scene = registry.GetScene(reused);
    LIMX_REQUIRE_NOT_NULL(scene);
    LIMX_EXPECT_NEAR(scene->Meshes[0].Bounds.Max.X, 2.0f, 1.0e-5f);
}

LIMX_TEST(AssetRegistry, SlotReuseKeepsArrayBounded)
{
    FAssetRegistry registry;

    // 反复加载卸载不应让条目数组无界增长
    for (Int32 round = 0; round < 100; ++round)
    {
        const FAssetHandle handle =
            registry.RegisterScene(StringFormat("asset_{}", round),
                                   MakeTestScene(1.0f));

        registry.ReleaseReference(handle);
        registry.CollectUnreferenced();
    }

    LIMX_EXPECT_EQ(registry.GetEntryCount(), SizeType(0));

    // 再加载一个应复用槽位
    const FAssetHandle handle =
        registry.RegisterScene(FString("final"), MakeTestScene(1.0f));

    LIMX_EXPECT_TRUE(registry.IsValid(handle));
    LIMX_EXPECT_EQ(registry.GetEntryCount(), SizeType(1));
}

// ============================================================================
// 失败缓存
// ============================================================================

LIMX_TEST(AssetRegistry, FailedLoadIsCachedAndReported)
{
    FAssetRegistry registry;

    // 不存在的文件
    const FAssetHandle first =
        registry.LoadScene(FString("does/not/exist.obj"));

    LIMX_REQUIRE_TRUE(first.IsValid());
    LIMX_EXPECT_EQ(static_cast<Int32>(registry.GetState(first)),
                   static_cast<Int32>(EAssetState::Failed));
    LIMX_EXPECT_NULL(registry.GetScene(first));
    LIMX_EXPECT_FALSE(registry.GetError(first).IsEmpty());

    // 二次请求应命中缓存 —— 一个损坏的文件可能被上百个材质引用,
    // 不缓存失败会造成上百次无谓的磁盘访问
    const FAssetHandle second =
        registry.LoadScene(FString("does/not/exist.obj"));

    LIMX_EXPECT_TRUE(first == second);

    const FAssetRegistryStats stats = registry.GetStats();
    LIMX_EXPECT_EQ(stats.CacheMisses, UInt64(1));
    LIMX_EXPECT_EQ(stats.CacheHits, UInt64(1));
    LIMX_EXPECT_EQ(stats.FailedCount, UInt32(1));
}

LIMX_TEST(AssetRegistry, UnknownExtensionFails)
{
    FAssetRegistry registry;

    const FAssetHandle handle =
        registry.LoadScene(FString("model.unknown"));

    LIMX_EXPECT_EQ(static_cast<Int32>(registry.GetState(handle)),
                   static_cast<Int32>(EAssetState::Failed));
    LIMX_EXPECT_FALSE(registry.GetError(handle).IsEmpty());
}

// ============================================================================
// 统计
// ============================================================================

LIMX_TEST(AssetRegistry, StatsReflectContents)
{
    FAssetRegistry registry;

    registry.RegisterScene(FString("s1"), MakeTestScene(1.0f));
    registry.RegisterScene(FString("s2"), MakeTestScene(2.0f));

    const FAssetHandle image =
        registry.RegisterImage(FString("i1"), MakeTestImage(8, 8, 128));

    FAssetRegistryStats stats = registry.GetStats();

    LIMX_EXPECT_EQ(stats.SceneCount, UInt32(2));
    LIMX_EXPECT_EQ(stats.ImageCount, UInt32(1));
    LIMX_EXPECT_EQ(stats.FailedCount, UInt32(0));
    LIMX_EXPECT_EQ(stats.UnreferencedCount, UInt32(0));

    // 8x8 RGBA8 = 256 字节
    LIMX_EXPECT_EQ(stats.ImageBytes, SizeType(256));
    LIMX_EXPECT_GT(stats.SceneBytes, SizeType(0));

    registry.ReleaseReference(image);
    stats = registry.GetStats();

    LIMX_EXPECT_EQ(stats.UnreferencedCount, UInt32(1));
}

LIMX_TEST(AssetRegistry, ClearRemovesEverything)
{
    FAssetRegistry registry;

    const FAssetHandle handle =
        registry.RegisterScene(FString("x"), MakeTestScene(1.0f));

    registry.Clear();

    LIMX_EXPECT_EQ(registry.GetEntryCount(), SizeType(0));
    LIMX_EXPECT_FALSE(registry.IsValid(handle));

    const FAssetRegistryStats stats = registry.GetStats();
    LIMX_EXPECT_EQ(stats.SceneCount, UInt32(0));
    LIMX_EXPECT_EQ(stats.CacheHits, UInt64(0));
    LIMX_EXPECT_EQ(stats.CacheMisses, UInt64(0));
}
