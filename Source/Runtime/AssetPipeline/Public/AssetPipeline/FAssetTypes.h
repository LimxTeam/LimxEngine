/*******************************************************************************
 * 文件: FAssetTypes.h
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   资产中性数据结构 — 网格、材质、纹理引用、场景节点层级
 *   所有解析器 (OBJ/glTF) 都产出这一套结构，上传层只需认识这一套
 *
 * 设计哲学:
 *   格式无关的中间层 — 若让渲染层直接消费 glTF 的 accessor/bufferView，
 *   或 OBJ 的面索引三元组，每新增一种格式都要改渲染层。中性结构把"文件长什么样"
 *   与"GPU 需要什么"彻底隔开：解析器只负责填充它，上传层只负责读取它。
 *
 *   材质统一为金属粗糙度 — glTF 2.0 原生即是该工作流，OBJ/MTL 的
 *   Phong 参数在解析时换算过来。渲染层因此只需实现一套 BRDF，
 *   而不必为每种资产格式分支。
 *
 *   索引而非指针的层级 — 场景节点用数组下标互指而非指针。这样整个场景是
 *   一块可整体拷贝、可序列化的连续数据，且节点数组扩容不会让引用失效。
 *
 * 技术特性:
 *   - 顶点为交错布局的胖顶点, 属性缺失时填默认值并置位存在性标记
 *   - 子网格按材质切分索引区间, 一次网格可对应多个绘制批次
 *   - 包围盒在解析期累积, 供后续视锥剔除直接使用
 *   - 纹理以路径或内嵌索引引用, 不在此层做图像解码
 *
 * 依赖关系:
 *   内部: Core/CoreMinimal.h, Core/Containers/TArray.h, FString.h, FName.h,
 *          Core/Math/FVector.h, FTransform.h, FBoundingBox.h
 *
 * 注意事项:
 *   本层不做任何 GPU 操作, 也不解码图像 — 纹理只记录引用
 *   索引统一为 UInt32; 上传层可按实际顶点数降级为 UInt16
 *
 ******************************************************************************/

#pragma once

#include "Core/CoreMinimal.h"
#include "Core/Containers/TArray.h"
#include "Core/Containers/FString.h"
#include "Core/Containers/FName.h"
#include "Core/Math/FVector.h"
#include "Core/Math/FTransform.h"
#include "Core/Math/FBoundingBox.h"
#include "AssetPipeline/AssetPipelineAPI.h"

namespace Limx
{

// ============================================================================
// FMeshVertex — 交错顶点
// ============================================================================

/// 网格顶点
///
/// 采用交错胖顶点而非分离属性流：解析阶段的顶点去重需要按完整属性组合
/// 做哈希，交错布局让这一步只需比较一个结构体。上传层若需要分离流，
/// 可在 GPU 上传时重新打包。
struct FMeshVertex
{
    /// 局部空间位置
    FVector3 Position = FVector3(0.0f, 0.0f, 0.0f);

    /// 法线 — 缺失时由解析器生成或置为零向量
    FVector3 Normal = FVector3(0.0f, 0.0f, 0.0f);

    /// 切线 xyz + 手性 w (±1) — 缺失时 w 为 0 表示无效
    FVector4 Tangent = FVector4(0.0f, 0.0f, 0.0f, 0.0f);

    /// 主纹理坐标
    FVector2 TexCoord0 = FVector2(0.0f, 0.0f);

    /// 次纹理坐标 — 常用于光照贴图
    FVector2 TexCoord1 = FVector2(0.0f, 0.0f);

    /// 顶点色 (线性空间) — 缺失时为白色
    FVector4 Color = FVector4(1.0f, 1.0f, 1.0f, 1.0f);

    /// 按全部属性判等 — 顶点去重依赖它
    LIMX_NODISCARD bool operator==(const FMeshVertex& other) const;
};

// ============================================================================
// FSubMesh — 共享同一材质的索引区间
// ============================================================================

/// 子网格
struct FSubMesh
{
    /// 名称 — 来自 OBJ 的 g/o 或 glTF 的 primitive 序号
    FName Name;

    /// 在所属网格索引数组中的起始位置
    UInt32 IndexOffset = 0;

    /// 索引个数 — 必为 3 的倍数
    UInt32 IndexCount = 0;

    /// 材质下标 — 指向 FAssetScene::Materials, -1 表示使用默认材质
    Int32 MaterialIndex = -1;

    /// 该子网格的局部包围盒
    FBoundingBox Bounds;
};

// ============================================================================
// FMeshData — 一个网格
// ============================================================================

/// 网格数据
struct FMeshData
{
    FName Name;

    /// 顶点数组 — 已去重
    TArray<FMeshVertex> Vertices;

    /// 三角形索引 — 每三个构成一个三角形
    TArray<UInt32> Indices;

    /// 按材质切分的绘制批次
    TArray<FSubMesh> SubMeshes;

    /// 整个网格的局部包围盒
    FBoundingBox Bounds;

    /// 源数据是否提供了法线 — 为 false 时 Normal 由解析器生成
    bool HasNormals = false;

    /// 源数据是否提供了切线
    bool HasTangents = false;

    /// 源数据是否提供了纹理坐标
    bool HasTexCoords = false;

    /// 源数据是否提供了顶点色
    bool HasVertexColors = false;

    LIMX_NODISCARD SizeType GetVertexCount() const { return Vertices.GetSize(); }
    LIMX_NODISCARD SizeType GetIndexCount() const  { return Indices.GetSize(); }

    LIMX_NODISCARD SizeType GetTriangleCount() const
    {
        return Indices.GetSize() / 3;
    }

    LIMX_NODISCARD bool IsEmpty() const
    {
        return Vertices.GetSize() == 0 || Indices.GetSize() == 0;
    }

    /// 依据顶点位置重算整体与各子网格的包围盒
    void RecomputeBounds();

    /// 按面法线累加生成平滑法线 — 源数据缺法线时调用
    void GenerateNormals();

    /// 依据纹理坐标生成切线 — 需要已有法线与纹理坐标
    void GenerateTangents();
};

// ============================================================================
// 材质
// ============================================================================

/// 透明模式
enum class EAlphaMode : UInt8
{
    /// 完全不透明, 忽略 alpha
    Opaque = 0,

    /// 按 AlphaCutoff 做二值裁剪
    Mask = 1,

    /// 常规 alpha 混合
    Blend = 2,
};

/// 纹理引用
///
/// 只记录"去哪里找这张图"，不做解码。外部文件用相对路径，
/// GLB 内嵌图像用缓冲区下标。
struct FTextureReference
{
    /// 已解析的纹理路径 — 内嵌纹理时为空
    ///
    /// 解析器写入的是"资产所在目录 + 文件内相对路径"的拼接结果, 可直接打开。
    /// OBJ 与 glTF 两条路径在这一点上一致; 消费方不应再拼一次 BaseDirectory,
    /// 否则会得到形如 "dir/dir/tex.png" 的路径, 而症状只是贴图静默缺失。
    FString Path;

    /// 内嵌图像下标 — 指向 FAssetScene::EmbeddedImages, -1 表示外部文件
    Int32 EmbeddedIndex = -1;

    /// 使用的纹理坐标集 (0 或 1)
    UInt32 TexCoordSet = 0;

    /// 是否指向了某张纹理
    LIMX_NODISCARD bool IsValid() const
    {
        return EmbeddedIndex >= 0 || !Path.IsEmpty();
    }
};

/// 材质数据 — 金属粗糙度工作流
struct FMaterialData
{
    FName Name;

    /// 基色因子 (线性空间, 含 alpha)
    FVector4 BaseColorFactor = FVector4(1.0f, 1.0f, 1.0f, 1.0f);

    /// 金属度 [0,1]
    Float32 MetallicFactor = 1.0f;

    /// 粗糙度 [0,1]
    Float32 RoughnessFactor = 1.0f;

    /// 自发光因子 (线性空间)
    FVector3 EmissiveFactor = FVector3(0.0f, 0.0f, 0.0f);

    /// 法线贴图强度
    Float32 NormalScale = 1.0f;

    /// 环境光遮蔽强度
    Float32 OcclusionStrength = 1.0f;

    /// Mask 模式下的裁剪阈值
    Float32 AlphaCutoff = 0.5f;

    EAlphaMode AlphaMode = EAlphaMode::Opaque;

    /// 是否双面渲染
    bool DoubleSided = false;

    FTextureReference BaseColorTexture;
    FTextureReference MetallicRoughnessTexture;
    FTextureReference NormalTexture;
    FTextureReference OcclusionTexture;
    FTextureReference EmissiveTexture;
};

// ============================================================================
// 内嵌图像
// ============================================================================

/// 内嵌图像的原始字节
///
/// GLB 与 data: URI 会把图像直接放在资产文件内。此处只保存压缩字节与
/// MIME 类型，实际解码由图像解码器负责 (Day 4)。
struct FEmbeddedImage
{
    FName Name;

    /// MIME 类型, 如 "image/png"
    FString MimeType;

    /// 原始压缩字节 (PNG/JPEG 等)
    TArray<UInt8> Bytes;
};

// ============================================================================
// 场景层级
// ============================================================================

/// 场景节点
///
/// 用数组下标而非指针互指，使整个层级是一块连续、可整体拷贝的数据，
/// 且节点数组扩容不会让任何引用失效。
struct FSceneNode
{
    FName Name;

    /// 相对于父节点的变换
    FTransform LocalTransform;

    /// 网格下标 — 指向 FAssetScene::Meshes, -1 表示纯变换节点
    Int32 MeshIndex = -1;

    /// 父节点下标 — -1 表示根节点
    Int32 ParentIndex = -1;

    /// 子节点下标
    TArray<Int32> Children;
};

// ============================================================================
// FAssetScene — 一次解析的完整产物
// ============================================================================

/// 资产场景
struct FAssetScene
{
    FName Name;

    /// 资产文件所在目录 — 解析相对纹理路径时的基准
    FString BaseDirectory;

    TArray<FMeshData>      Meshes;
    TArray<FMaterialData>  Materials;
    TArray<FEmbeddedImage> EmbeddedImages;
    TArray<FSceneNode>     Nodes;

    /// 根节点下标
    TArray<Int32> RootNodes;

    /// 整个场景在世界空间的包围盒
    FBoundingBox Bounds;

    LIMX_NODISCARD bool IsEmpty() const { return Meshes.GetSize() == 0; }

    /// 全部网格的顶点总数
    LIMX_NODISCARD SizeType GetTotalVertexCount() const;

    /// 全部网格的三角形总数
    LIMX_NODISCARD SizeType GetTotalTriangleCount() const;

    /// 全部网格的绘制批次总数
    LIMX_NODISCARD SizeType GetTotalSubMeshCount() const;

    /// 计算某节点的世界变换 — 沿父链累乘
    LIMX_NODISCARD FTransform ComputeWorldTransform(Int32 nodeIndex) const;

    /// 依据节点层级与各网格包围盒重算场景包围盒
    void RecomputeBounds();

    /// 清空全部内容
    void Reset();
};

// ============================================================================
// 解析结果
// ============================================================================

/// 解析结果 — 失败时携带可定位的原因
struct FAssetLoadResult
{
    /// 是否成功
    bool Succeeded = false;

    /// 失败原因 — 成功时为空
    FString ErrorMessage;

    /// 出错所在行 (文本格式) 或字节偏移 (二进制格式), 0 表示不适用
    UInt32 ErrorLocation = 0;

    /// 解析期产生的告警 — 不影响成功, 但值得让调用方知道
    TArray<FString> Warnings;

    LIMX_NODISCARD static FAssetLoadResult Success()
    {
        FAssetLoadResult result;
        result.Succeeded = true;
        return result;
    }

    LIMX_NODISCARD static FAssetLoadResult Failure(const FString& message,
                                                   UInt32 location = 0)
    {
        FAssetLoadResult result;
        result.Succeeded     = false;
        result.ErrorMessage  = message;
        result.ErrorLocation = location;
        return result;
    }
};

} // namespace Limx
