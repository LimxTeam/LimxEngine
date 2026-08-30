/*******************************************************************************
 * 文件: FSceneLoader.h
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   场景导入器 — 把资产管线解析出的中性场景变成 LScene 中的活节点
 *   打通 "文件 → CPU 数据 → GPU 资源 → 场景图" 的完整链路
 *
 * 设计哲学:
 *   导入是一次性动作, 不是一个对象 — 导入器不持有任何状态: 网格与纹理归
 *   FRenderResourceManager, 材质归 FMaterialManager, 节点归 LScene。
 *   如果导入器自己也持有一份, 卸载时就有三个地方需要保持一致, 而只要有
 *   一处漏掉, 表现就是显存缓慢泄漏 —— 这类问题几乎无法在事后定位。
 *
 *   纹理按用途决定色彩空间 — 基色与自发光是 sRGB, 法线、金属粗糙度、
 *   遮蔽是线性数据。文件里的声明只是提示; 真正的依据是这张图在材质中的
 *   位置, 因此判断放在这里而不是解码器里。
 *
 *   同一路径的纹理只上传一次 — Sponza 这类场景里几十个材质共享同一批贴图,
 *   逐材质上传会让显存占用翻好几倍。导入过程内维护一张路径→句柄的表。
 *
 * 技术特性:
 *   - 节点世界变换沿父链累乘后写入 LNode, 层级在导入时展平
 *   - 每个 FSubMesh 的材质槽位映射到 LMeshTrait 的逐槽位材质
 *   - 缺失贴图回退到材质的常量因子, 不中断导入
 *   - 导入失败时返回可定位的原因, 不留下半个场景
 *
 * 依赖关系:
 *   内部: Engine/LScene.h, Engine/Rendering/LMeshTrait.h,
 *          AssetPipeline/FAssetRegistry.h, RenderCore/Renderer/FRenderContext.h
 *
 * 注意事项:
 *   导入是同步的 — 大场景会阻塞调用线程数秒
 *   必须在 FMaterialManager 初始化之后调用
 *
 ******************************************************************************/

#pragma once

#include "Engine/EngineAPI.h"
#include "Engine/LScene.h"
#include "Engine/Rendering/LMeshTrait.h"
#include "RenderCore/Renderer/FRenderContext.h"
#include "AssetPipeline/FAssetTypes.h"

namespace Limx
{

// ============================================================================
// FSceneLoadOptions — 导入选项
// ============================================================================

/// 场景导入选项
struct FSceneLoadOptions
{
    /// 统一缩放 — 不同来源的资产单位不一 (Sponza 是厘米, glTF 通常是米)
    Float32 UniformScale = 1.0f;

    /// 是否加载并绑定纹理
    ///
    /// 关闭时材质只保留常量因子。用于把"几何吞吐"与"纹理带宽"分开测量。
    bool LoadTextures = true;

    /// 纹理各向异性过滤
    bool UseAnisotropy = true;

    /// 单个场景允许导入的最大节点数 — 0 表示不限制
    ///
    /// 防止误传一个超大文件后卡死在导入里, 且卡死时没有任何进度反馈。
    UInt32 MaxNodes = 0;
};

// ============================================================================
// FSceneLoadResult — 导入结果
// ============================================================================

/// 场景导入结果
struct FSceneLoadResult
{
    bool Succeeded = false;

    /// 失败原因 — 成功时为空
    FString Error;

    UInt32 NodeCount     = 0;
    UInt32 MeshCount     = 0;
    UInt32 MaterialCount = 0;

    /// 实际上传的纹理数 — 已去重, 小于材质引用的贴图总数
    UInt32 TextureCount = 0;

    /// 贴图缺失或解码失败的次数 — 非零不代表导入失败, 但值得看一眼
    UInt32 MissingTextureCount = 0;

    UInt64 VertexCount   = 0;
    UInt64 TriangleCount = 0;

    /// 导入后的场景世界包围盒 — 用于自动摆放相机
    FBoundingBox Bounds;

    /// 导入耗时 (毫秒)
    Float64 ElapsedMilliseconds = 0.0;

    // ====================================================================
    // 分项耗时 —— 优化之前必须先知道时间花在哪
    // ====================================================================
    //
    // 不分项的话很容易优化错地方: 导入慢了就并行化解码, 结果发现瓶颈
    // 其实在上传的逐资源等待上, 忙活半天总时间只掉了一成。这几个数字
    // 就是用来防止这件事的。
    //
    // 当前解码与上传是交替进行的 (逐材质逐槽位: 解一张、传一张), 因此
    // 两者的累加值可以直接相加, 不存在重叠。

    /// 资产文件解析 (glTF/OBJ 结构、顶点、材质定义)
    Float64 ParseMilliseconds = 0.0;

    /// 网格上传到 GPU
    Float64 MeshUploadMilliseconds = 0.0;

    /// 图像解码 (读文件 + 解 JPEG/PNG) —— CPU 密集, 可并行
    Float64 TextureDecodeMilliseconds = 0.0;

    /// 纹理上传到 GPU (含 mip 生成) —— 走命令缓冲区, 不可并行
    Float64 TextureUploadMilliseconds = 0.0;

    /// 解码出来的图像总字节数 —— 用来判断解码是不是真的按数据量在花时间
    UInt64 DecodedImageBytes = 0;

    /// 本次导入创建的材质 (非拥有指针, 由 FMaterialManager 管理生命周期)
    ///
    /// 卸载场景时必须连同这些材质一起销毁 —— 纹理的存活完全取决于
    /// "还有多少材质在引用它", 材质不销毁, 纹理就永远收不回来。
    TArray<FMaterial*> Materials;
};

// ============================================================================
// FSceneLoader — 场景导入器
// ============================================================================

/// 场景导入器 — 全部为静态方法, 不持有状态
class LIMX_ENGINE_API FSceneLoader
{
public:
    FSceneLoader()                               = delete;
    ~FSceneLoader()                              = delete;
    FSceneLoader(const FSceneLoader&)            = delete;
    FSceneLoader& operator=(const FSceneLoader&) = delete;

    /// 把资产文件导入到场景中
    ///
    /// 支持 .obj 与 .gltf/.glb —— 具体由 FAssetRegistry 按扩展名分派。
    ///
    /// @param scene    目标场景 (节点追加到其中, 不清空已有内容)
    /// @param context  渲染上下文 (提供资源管理器)
    /// @param path     资产文件路径
    /// @param options  导入选项
    /// @return 导入结果; Succeeded 为 false 时 Error 给出原因
    LIMX_NODISCARD static FSceneLoadResult LoadInto(
        LScene* scene,
        FRenderContext* context,
        const FString& path,
        const FSceneLoadOptions& options = FSceneLoadOptions());

    /// 销毁一次导入创建的全部材质
    ///
    /// 与场景销毁配对使用: 场景销毁释放网格引用, 本函数释放纹理引用。
    /// 两者都完成后调用 FRenderResourceManager::CollectUnreferenced,
    /// 显存才会真正回落。
    ///
    /// @param result LoadInto 的返回值
    /// @return 销毁的材质数
    static UInt32 UnloadMaterials(const FSceneLoadResult& result);
};

} // namespace Limx
