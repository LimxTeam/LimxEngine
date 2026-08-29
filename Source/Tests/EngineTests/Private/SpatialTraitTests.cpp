/*******************************************************************************
 * 文件: SpatialTraitTests.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   空间 Trait 单元测试 — 节点变换传递、层级合成、销毁顺序安全性
 *
 * 设计哲学:
 *   测"节点摆在哪里, 它的 Trait 就在哪里" — 这条不成立时不会报任何错,
 *   只会让所有物体默默堆在原点。这个 bug 曾经一直存在, 因为演示场景里
 *   三个物体重叠在一起看不出异常, 直到加载真实场景才暴露。
 *
 *   销毁顺序不该成为正确性的前提 — LScene 逐个销毁 Trait, 顺序由容器决定。
 *   父级先于子级被销毁时, 子级的反向指针必须已经失效, 否则子级析构会往
 *   已释放的内存里写。这类崩溃只在关闭时出现, 且栈毫无信息量。
 *
 * 技术特性:
 *   - 用例走完整的 LScene/LRegistry 路径, 不直接 new Trait
 *   - 变换断言全部走容差, 不比较浮点相等
 *   - 销毁顺序用例覆盖正序与逆序两种
 *
 * 依赖关系:
 *   内部: EngineTests/EngineTestsMinimal.h
 *
 * 注意事项:
 *   用例结束时必须销毁场景, 否则 LRegistry 会跨用例残留对象
 *
 ******************************************************************************/

#include "EngineTests/EngineTestsMinimal.h"

using namespace Limx;

namespace
{

/// Float32 运算的通用容差
constexpr Float32 kTolerance = 1.0e-4f;

/// 断言两个向量在容差内相等
bool VectorsNearlyEqual(const FVector3& a, const FVector3& b,
                        Float32 tolerance = kTolerance)
{
    return FMath::Abs(a.X - b.X) <= tolerance &&
           FMath::Abs(a.Y - b.Y) <= tolerance &&
           FMath::Abs(a.Z - b.Z) <= tolerance;
}

/// 场景的 RAII 包装 — 用例提前返回时也能保证销毁
///
/// LRegistry 是进程级单例; 用例漏掉销毁, 残留对象会跟着进入下一个用例,
/// 于是失败出现在无辜的用例上。
struct FScopedScene
{
    LScene* Scene = nullptr;

    explicit FScopedScene(const AnsiChar* name)
        : Scene(LScene::Create(FName(name)))
    {
    }

    ~FScopedScene()
    {
        if (Scene != nullptr)
        {
            LRegistry::Get().Destroy(Scene);
            Scene = nullptr;
        }
    }

    FScopedScene(const FScopedScene&)            = delete;
    FScopedScene& operator=(const FScopedScene&) = delete;

    LScene* operator->() const { return Scene; }
};

} // namespace

// ============================================================================
// 节点变换传递
// ============================================================================

LIMX_TEST(LSpatialTrait, RootSpatialCarriesSpawnTransform)
{
    FScopedScene scene("TransformScene");
    LIMX_REQUIRE_TRUE(scene.Scene != nullptr);

    FTransform spawnTransform;
    spawnTransform.Translation = FVector3(3.0f, 4.0f, 5.0f);

    LNode* node = scene->SpawnNode<LNode>(FName("Node"), spawnTransform);
    LIMX_REQUIRE_TRUE(node != nullptr);

    LSpatialTrait* root = node->GetRootSpatial();
    LIMX_REQUIRE_TRUE(root != nullptr);

    LIMX_EXPECT_TRUE(VectorsNearlyEqual(
        root->GetWorldTransform().Translation, FVector3(3.0f, 4.0f, 5.0f)));
}

LIMX_TEST(LSpatialTrait, AddedTraitInheritsNodeTransform)
{
    FScopedScene scene("InheritScene");
    LIMX_REQUIRE_TRUE(scene.Scene != nullptr);

    FTransform spawnTransform;
    spawnTransform.Translation = FVector3(-2.0f, 7.5f, 1.25f);

    LNode* node = scene->SpawnNode<LNode>(FName("Node"), spawnTransform);
    LIMX_REQUIRE_TRUE(node != nullptr);

    // 后加的空间 Trait 必须自动挂到节点的根变换之下。
    // 不成立时它会停在原点, 而画面上只是"物体位置不对", 没有任何报错。
    LSpatialTrait* child = node->AddTrait<LSpatialTrait>(FName("Child"));
    LIMX_REQUIRE_TRUE(child != nullptr);

    LIMX_EXPECT_TRUE(child->GetParent() == node->GetRootSpatial());

    LIMX_EXPECT_TRUE(VectorsNearlyEqual(
        child->GetWorldTransform().Translation, FVector3(-2.0f, 7.5f, 1.25f)));
}

LIMX_TEST(LSpatialTrait, ChildLocalOffsetComposesWithNodeTransform)
{
    FScopedScene scene("ComposeScene");
    LIMX_REQUIRE_TRUE(scene.Scene != nullptr);

    FTransform spawnTransform;
    spawnTransform.Translation = FVector3(10.0f, 0.0f, 0.0f);

    LNode* node = scene->SpawnNode<LNode>(FName("Node"), spawnTransform);
    LIMX_REQUIRE_TRUE(node != nullptr);

    LSpatialTrait* child = node->AddTrait<LSpatialTrait>(FName("Child"));
    LIMX_REQUIRE_TRUE(child != nullptr);

    FTransform localOffset;
    localOffset.Translation = FVector3(0.0f, 2.0f, 0.0f);
    child->SetLocalTransform(localOffset);

    LIMX_EXPECT_TRUE(VectorsNearlyEqual(
        child->GetWorldTransform().Translation, FVector3(10.0f, 2.0f, 0.0f)));
}

LIMX_TEST(LSpatialTrait, ParentRotationRotatesChildOffset)
{
    FScopedScene scene("RotationScene");
    LIMX_REQUIRE_TRUE(scene.Scene != nullptr);

    // 节点绕 Y 轴转 90°
    FTransform spawnTransform;
    spawnTransform.Rotation = FQuat::FromAxisAngle(
        FVector3(0.0f, 1.0f, 0.0f), FMath::kPi * 0.5f);

    LNode* node = scene->SpawnNode<LNode>(FName("Node"), spawnTransform);
    LIMX_REQUIRE_TRUE(node != nullptr);

    LSpatialTrait* child = node->AddTrait<LSpatialTrait>(FName("Child"));
    LIMX_REQUIRE_TRUE(child != nullptr);

    // 子级本地偏移 +X, 父级转 90° 后应指向 -Z (右手系绕 Y)
    FTransform localOffset;
    localOffset.Translation = FVector3(1.0f, 0.0f, 0.0f);
    child->SetLocalTransform(localOffset);

    const FVector3 world = child->GetWorldTransform().Translation;

    LIMX_EXPECT_NEAR(world.X, 0.0f, 1.0e-3f);
    LIMX_EXPECT_NEAR(world.Y, 0.0f, 1.0e-3f);
    LIMX_EXPECT_NEAR(world.Z, -1.0f, 1.0e-3f);
}

LIMX_TEST(LSpatialTrait, MovingNodeMovesAttachedTraits)
{
    FScopedScene scene("MoveScene");
    LIMX_REQUIRE_TRUE(scene.Scene != nullptr);

    LNode* node = scene->SpawnNode<LNode>(FName("Node"), FTransform());
    LIMX_REQUIRE_TRUE(node != nullptr);

    LSpatialTrait* child = node->AddTrait<LSpatialTrait>(FName("Child"));
    LIMX_REQUIRE_TRUE(child != nullptr);

    FTransform moved;
    moved.Translation = FVector3(0.0f, 0.0f, 12.0f);
    node->GetRootSpatial()->SetLocalTransform(moved);

    // 世界变换是每次现算的 —— 缓存的话这里就会读到过期值
    LIMX_EXPECT_TRUE(VectorsNearlyEqual(
        child->GetWorldTransform().Translation, FVector3(0.0f, 0.0f, 12.0f)));
}

// ============================================================================
// 层级操作
// ============================================================================

LIMX_TEST(LSpatialTrait, DetachRestoresLocalTransformAsWorld)
{
    FScopedScene scene("DetachScene");
    LIMX_REQUIRE_TRUE(scene.Scene != nullptr);

    FTransform spawnTransform;
    spawnTransform.Translation = FVector3(5.0f, 0.0f, 0.0f);

    LNode* node = scene->SpawnNode<LNode>(FName("Node"), spawnTransform);
    LIMX_REQUIRE_TRUE(node != nullptr);

    LSpatialTrait* child = node->AddTrait<LSpatialTrait>(FName("Child"));
    LIMX_REQUIRE_TRUE(child != nullptr);

    FTransform localOffset;
    localOffset.Translation = FVector3(0.0f, 3.0f, 0.0f);
    child->SetLocalTransform(localOffset);

    child->DetachFromParent();

    LIMX_EXPECT_TRUE(child->GetParent() == nullptr);
    LIMX_EXPECT_TRUE(VectorsNearlyEqual(
        child->GetWorldTransform().Translation, FVector3(0.0f, 3.0f, 0.0f)));
}

LIMX_TEST(LSpatialTrait, AttachToSelfIsRejected)
{
    FScopedScene scene("SelfAttachScene");
    LIMX_REQUIRE_TRUE(scene.Scene != nullptr);

    LNode* node = scene->SpawnNode<LNode>(FName("Node"), FTransform());
    LIMX_REQUIRE_TRUE(node != nullptr);

    LSpatialTrait* root = node->GetRootSpatial();
    LIMX_REQUIRE_TRUE(root != nullptr);

    // 自挂自会让 GetWorldTransform 无限递归 —— 必须被挡住
    root->AttachTo(root);

    LIMX_EXPECT_TRUE(root->GetParent() == nullptr);
}

LIMX_TEST(LSpatialTrait, RootSpatialDoesNotAttachToItself)
{
    FScopedScene scene("RootScene");
    LIMX_REQUIRE_TRUE(scene.Scene != nullptr);

    LNode* node = scene->SpawnNode<LNode>(FName("Node"), FTransform());
    LIMX_REQUIRE_TRUE(node != nullptr);

    // 根空间 Trait 是在 SetRootSpatial 之前加进去的, 因此 OnAttached
    // 取到的 root 是 nullptr, 不会形成自环。
    LIMX_EXPECT_TRUE(node->GetRootSpatial()->GetParent() == nullptr);
}

// ============================================================================
// 销毁顺序安全性
// ============================================================================

LIMX_TEST(LSpatialTrait, DestroyingParentClearsChildBackPointer)
{
    FScopedScene scene("DestroyOrderScene");
    LIMX_REQUIRE_TRUE(scene.Scene != nullptr);

    LNode* node = scene->SpawnNode<LNode>(FName("Node"), FTransform());
    LIMX_REQUIRE_TRUE(node != nullptr);

    LSpatialTrait* parent = node->AddTrait<LSpatialTrait>(FName("Parent"));
    LSpatialTrait* child  = node->AddTrait<LSpatialTrait>(FName("Child"));

    LIMX_REQUIRE_TRUE(parent != nullptr);
    LIMX_REQUIRE_TRUE(child != nullptr);

    child->AttachTo(parent);
    LIMX_REQUIRE_TRUE(child->GetParent() == parent);

    // 父级先走 —— 子级的 m_Parent 必须当场失效, 否则子级析构时会往
    // 已释放的内存里写, 表现为关闭阶段的访问违规。
    LRegistry::Get().Destroy(parent);

    LIMX_EXPECT_TRUE(child->GetParent() == nullptr);

    // 悬垂指针已清, 此刻取世界变换不应触碰任何已释放对象
    LIMX_EXPECT_TRUE(VectorsNearlyEqual(
        child->GetWorldTransform().Translation, FVector3(0.0f, 0.0f, 0.0f)));

    LRegistry::Get().Destroy(child);
}

LIMX_TEST(LSpatialTrait, DestroyingChildRemovesItFromParentChildren)
{
    FScopedScene scene("ChildRemovalScene");
    LIMX_REQUIRE_TRUE(scene.Scene != nullptr);

    LNode* node = scene->SpawnNode<LNode>(FName("Node"), FTransform());
    LIMX_REQUIRE_TRUE(node != nullptr);

    LSpatialTrait* parent = node->AddTrait<LSpatialTrait>(FName("Parent"));
    LSpatialTrait* child  = node->AddTrait<LSpatialTrait>(FName("Child"));

    LIMX_REQUIRE_TRUE(parent != nullptr);
    LIMX_REQUIRE_TRUE(child != nullptr);

    child->AttachTo(parent);
    LIMX_REQUIRE_TRUE(parent->GetChildren().GetSize() == 1);

    LRegistry::Get().Destroy(child);

    // 父级的子数组里不能留下已释放的指针
    LIMX_EXPECT_TRUE(parent->GetChildren().GetSize() == 0);

    LRegistry::Get().Destroy(parent);
}

LIMX_TEST(LSpatialTrait, SceneDestructionSurvivesArbitraryTraitOrder)
{
    // 场景销毁时逐个 Destroy Trait, 顺序由容器决定。这里显式构造一条
    // 深链, 确保无论先销毁链头还是链尾都不会崩。
    {
        FScopedScene scene("DeepChainScene");
        LIMX_REQUIRE_TRUE(scene.Scene != nullptr);

        LNode* node = scene->SpawnNode<LNode>(FName("Node"), FTransform());
        LIMX_REQUIRE_TRUE(node != nullptr);

        LSpatialTrait* previous = node->GetRootSpatial();

        const AnsiChar* names[4] = { "L1", "L2", "L3", "L4" };

        for (UInt32 i = 0; i < 4; ++i)
        {
            LSpatialTrait* link = node->AddTrait<LSpatialTrait>(FName(names[i]));
            LIMX_REQUIRE_TRUE(link != nullptr);

            link->AttachTo(previous);
            previous = link;
        }

        // 作用域结束时 FScopedScene 触发场景销毁 —— 不崩即通过
    }

    LIMX_EXPECT_TRUE(true);
}

LIMX_TEST(LSpatialTrait, DeepChainComposesTranslations)
{
    FScopedScene scene("ChainTransformScene");
    LIMX_REQUIRE_TRUE(scene.Scene != nullptr);

    LNode* node = scene->SpawnNode<LNode>(FName("Node"), FTransform());
    LIMX_REQUIRE_TRUE(node != nullptr);

    LSpatialTrait* previous = node->GetRootSpatial();

    const AnsiChar* names[3] = { "A", "B", "C" };

    for (UInt32 i = 0; i < 3; ++i)
    {
        LSpatialTrait* link = node->AddTrait<LSpatialTrait>(FName(names[i]));
        LIMX_REQUIRE_TRUE(link != nullptr);

        link->AttachTo(previous);

        FTransform step;
        step.Translation = FVector3(1.0f, 0.0f, 0.0f);
        link->SetLocalTransform(step);

        previous = link;
    }

    // 三级各偏移 +1, 世界坐标应为 +3
    LIMX_EXPECT_TRUE(VectorsNearlyEqual(
        previous->GetWorldTransform().Translation, FVector3(3.0f, 0.0f, 0.0f)));
}
