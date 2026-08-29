/*******************************************************************************
 * 文件: FGltfLoader.h
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   glTF 2.0 / GLB 解析器 — 产出与图形 API 无关的 FAssetScene
 *   支持外部 .bin、data: URI 内嵌缓冲区与 GLB 二进制容器
 *   材质原生即金属粗糙度工作流，无需换算
 *
 * 设计哲学:
 *   访问器是唯一的数据入口 — glTF 用 accessor → bufferView → buffer 三级
 *   间接描述每一段数据，还叠加 byteOffset、byteStride、componentType、
 *   normalized 等修饰。把这套解引用集中在一个取值函数里，各属性的读取
 *   就退化为"给我第 N 个访问器的第 i 个元素"，不必各自处理跨步与类型转换。
 *
 *   规范允许的都要接受 — 索引可以是 UNSIGNED_BYTE/SHORT/INT 三种宽度，
 *   位置可以带 byteStride 交错存放，法线可以缺席。任何一种情况不支持，
 *   都会让某些导出器产出的模型加载失败，而这类失败在真实资产上非常常见。
 *
 *   节点变换两种表达 — glTF 允许用 matrix 或 TRS 三元组描述节点变换，
 *   二者互斥。解析时统一分解为 FTransform，使下游只面对一种形态。
 *
 * 技术特性:
 *   - GLB 容器解析: 校验魔数、版本、块长度与 4 字节对齐
 *   - 访问器支持全部标量类型与 SCALAR/VEC2/VEC3/VEC4, 含 normalized 归一化
 *   - 支持稀疏访问器缺席时的默认零填充
 *   - 节点层级递归展开为扁平数组 + 父子索引
 *   - 仅接受 TRIANGLES 图元, 其余记入告警并跳过
 *
 * 依赖关系:
 *   内部: AssetPipeline/FAssetTypes.h, Core/Misc/FJson.h,
 *          Core/Misc/FBase64.h, Core/HAL/FPlatformFile.h
 *
 * 注意事项:
 *   不支持动画、蒙皮、变形目标 — 遇到时忽略并告警
 *   不支持 Draco/meshopt 压缩扩展 — 遇到时判定为失败而非静默产出错误几何
 *
 ******************************************************************************/

#pragma once

#include "AssetPipeline/FAssetTypes.h"

namespace Limx
{

// ============================================================================
// FGltfLoadOptions — 解析选项
// ============================================================================

/// glTF 解析选项
struct FGltfLoadOptions
{
    /// 源数据缺法线时是否生成
    bool GenerateMissingNormals = true;

    /// 缺切线且有纹理坐标时是否生成
    bool GenerateMissingTangents = true;

    /// 是否加载外部 .bin 缓冲区 — 关闭时仅接受内嵌数据
    bool LoadExternalBuffers = true;

    /// 是否把 GLB 内嵌图像的字节保留到 FAssetScene::EmbeddedImages
    bool KeepEmbeddedImages = true;

    /// 是否翻转纹理坐标的 V 轴
    ///
    /// glTF 的 UV 原点已在左上，与 Vulkan 图像约定一致，因此默认不翻转。
    /// 这与 OBJ 的默认值相反 —— 两种格式的约定本就不同。
    bool FlipTexCoordV = false;
};

// ============================================================================
// FGltfLoader — 解析入口
// ============================================================================

/// glTF 2.0 解析器 — 全静态接口
class LIMX_ASSETPIPELINE_API FGltfLoader
{
public:
    FGltfLoader()                              = delete;
    ~FGltfLoader()                             = delete;
    FGltfLoader(const FGltfLoader&)            = delete;
    FGltfLoader& operator=(const FGltfLoader&) = delete;

    /// 从文件解析 — 按扩展名自动区分 .gltf 与 .glb
    LIMX_NODISCARD static FAssetLoadResult LoadFromFile(
        const FString& path,
        FAssetScene& outScene,
        const FGltfLoadOptions& options = FGltfLoadOptions());

    /// 解析 .gltf (JSON 文本)
    /// @param json          glTF JSON 文本
    /// @param length        字节长度
    /// @param baseDirectory 解析外部 buffer 与 image 相对路径的基准目录
    LIMX_NODISCARD static FAssetLoadResult LoadFromJson(
        const AnsiChar* json, SizeType length,
        const FString& baseDirectory,
        FAssetScene& outScene,
        const FGltfLoadOptions& options = FGltfLoadOptions());

    /// 解析 .glb (二进制容器)
    /// @param data          GLB 字节流
    /// @param length        字节长度
    /// @param baseDirectory 解析外部引用的基准目录
    LIMX_NODISCARD static FAssetLoadResult LoadFromGlb(
        const UInt8* data, SizeType length,
        const FString& baseDirectory,
        FAssetScene& outScene,
        const FGltfLoadOptions& options = FGltfLoadOptions());
};

} // namespace Limx
