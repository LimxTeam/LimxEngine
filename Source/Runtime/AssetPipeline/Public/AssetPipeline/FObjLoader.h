/*******************************************************************************
 * 文件: FObjLoader.h
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   Wavefront OBJ / MTL 解析器 — 产出与图形 API 无关的 FAssetScene
 *   支持多边形三角化、负索引、顶点去重、按材质切分子网格
 *   MTL 的 Phong 参数在解析时换算为金属粗糙度工作流
 *
 * 设计哲学:
 *   顶点去重是必须的 — OBJ 的面用 (位置/UV/法线) 三元组独立索引，同一位置
 *   在不同面上可能配不同的法线或 UV。GPU 只接受单一索引流，因此必须把每种
 *   出现过的属性组合折叠成唯一顶点。不做去重会让 Sponza 这类模型的顶点数
 *   膨胀数倍。
 *
 *   Phong 到 PBR 的换算要讲清楚 — OBJ 诞生于 PBR 之前，Ns/Ks 与金属度、
 *   粗糙度没有严格对应。本实现采用业界通行的近似并在注释中写明依据，
 *   同时优先采用 MTL 的 PBR 扩展字段 (Pr/Pm)，有则不做近似。
 *
 *   宽容但不静默 — 面对畸形行选择跳过而非中止解析，因为真实资产库里
 *   总有个别损坏的行；但每次跳过都记入 Warnings，使问题可见而非被吞掉。
 *
 * 技术特性:
 *   - 扇形三角化任意凸多边形面
 *   - 支持 1 起始的正索引与相对末尾的负索引
 *   - 缺失法线时按面积加权生成; 有 UV 时一并生成切线
 *   - MTL 与 OBJ 分离解析, 可单独调用 MTL 解析器
 *
 * 依赖关系:
 *   内部: AssetPipeline/FAssetTypes.h, Core/HAL/FPlatformFile.h
 *
 * 注意事项:
 *   不支持自由曲面 (curv/surf/parm)、点/线图元 (p/l) — 遇到时跳过并告警
 *   平滑组 (s) 被忽略 — 法线生成一律按整网格平滑
 *
 ******************************************************************************/

#pragma once

#include "AssetPipeline/FAssetTypes.h"

namespace Limx
{

// ============================================================================
// FObjLoadOptions — 解析选项
// ============================================================================

/// OBJ 解析选项
struct FObjLoadOptions
{
    /// 源数据缺法线时是否生成
    bool GenerateMissingNormals = true;

    /// 有纹理坐标时是否生成切线
    bool GenerateTangents = true;

    /// 是否解析 mtllib 引用的材质库
    bool LoadMaterialLibrary = true;

    /// 是否翻转纹理坐标的 V 轴
    ///
    /// OBJ 的 UV 原点在左下，而 Vulkan 的图像原点在左上。多数工具导出的
    /// OBJ 需要翻转 V 才能正确采样，故默认开启。
    bool FlipTexCoordV = true;

    /// 是否把每个 o/g 组切分为独立子网格 — 关闭时只按材质切分
    bool SplitByGroup = false;
};

// ============================================================================
// FObjLoader — 解析入口
// ============================================================================

/// Wavefront OBJ 解析器 — 全静态接口
class LIMX_ASSETPIPELINE_API FObjLoader
{
public:
    FObjLoader()                             = delete;
    ~FObjLoader()                            = delete;
    FObjLoader(const FObjLoader&)            = delete;
    FObjLoader& operator=(const FObjLoader&) = delete;

    /// 从文件解析 OBJ
    /// @param path      OBJ 文件路径
    /// @param outScene  输出场景 (调用前会被清空)
    /// @param options   解析选项
    LIMX_NODISCARD static FAssetLoadResult LoadFromFile(
        const FString& path,
        FAssetScene& outScene,
        const FObjLoadOptions& options = FObjLoadOptions());

    /// 从内存中的 OBJ 文本解析
    /// @param text          OBJ 文本
    /// @param length        字节长度
    /// @param baseDirectory 解析 mtllib 与贴图相对路径的基准目录
    /// @param outScene      输出场景
    /// @param options       解析选项
    LIMX_NODISCARD static FAssetLoadResult LoadFromMemory(
        const AnsiChar* text, SizeType length,
        const FString& baseDirectory,
        FAssetScene& outScene,
        const FObjLoadOptions& options = FObjLoadOptions());

    /// 单独解析一个 MTL 材质库并追加到场景的材质数组
    /// @param text           MTL 文本
    /// @param length         字节长度
    /// @param baseDirectory  贴图相对路径的基准目录
    /// @param outScene       材质追加到该场景
    /// @param outWarnings    解析告警
    LIMX_NODISCARD static FAssetLoadResult ParseMaterialLibrary(
        const AnsiChar* text, SizeType length,
        const FString& baseDirectory,
        FAssetScene& outScene,
        TArray<FString>& outWarnings);
};

} // namespace Limx
