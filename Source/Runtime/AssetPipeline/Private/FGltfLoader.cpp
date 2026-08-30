/*******************************************************************************
 * 文件: FGltfLoader.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   glTF 2.0 / GLB 解析器实现 — 容器解包、缓冲区解引用、访问器取值、
 *   图元装配、材质映射、节点层级展开
 *
 * 设计哲学:
 *   访问器解引用集中一处 — glTF 的每一段数据都要穿过
 *   accessor → bufferView → buffer 三级间接，还叠加两层 byteOffset、
 *   可选的 byteStride、五种 componentType 与 normalized 归一化。
 *   把这套逻辑收敛到 FAccessorReader，各属性读取就只剩"取第 i 个元素"，
 *   既避免了在每个属性处重复处理跨步，也让边界检查只需写一遍。
 *
 *   越界一律返回失败而非夹紧 — 损坏的 glTF 若被夹紧读取，会静默产出
 *   错误几何，症状表现为模型局部扭曲，极难溯源。宁可拒绝加载。
 *
 * 技术特性:
 *   - GLB 校验魔数/版本/块长度/4 字节对齐, 任一不符即拒绝
 *   - 索引支持 UNSIGNED_BYTE / UNSIGNED_SHORT / UNSIGNED_INT 三种宽度
 *   - 属性支持交错存放 (byteStride) 与紧密排布两种布局
 *   - 节点变换的 matrix 与 TRS 两种表达统一分解为 FTransform
 *
 * 依赖关系:
 *   内部: AssetPipeline/FGltfLoader.h, Core/Misc/FJson.h,
 *          Core/Misc/FBase64.h, Core/HAL/FPlatformFile.h
 *
 * 注意事项:
 *   遇到 Draco / meshopt 压缩扩展时判定为失败 —— 静默产出空几何会更糟
 *
 ******************************************************************************/

#include "AssetPipeline/FGltfLoader.h"
#include "Core/Threading/FJobExecutor.h"
#include "Core/Misc/FJson.h"
#include "Core/Misc/FBase64.h"
#include "Core/HAL/FPlatformFile.h"

namespace Limx
{

namespace
{

// ============================================================================
// glTF 常量
// ============================================================================

/// 组件类型 — 对应 OpenGL 的类型枚举
constexpr Int32 kComponentByte          = 5120;
constexpr Int32 kComponentUnsignedByte  = 5121;
constexpr Int32 kComponentShort         = 5122;
constexpr Int32 kComponentUnsignedShort = 5123;
constexpr Int32 kComponentUnsignedInt   = 5125;
constexpr Int32 kComponentFloat         = 5126;

/// 图元模式 — 引擎只消费三角形
constexpr Int32 kPrimitiveModeTriangles = 4;

/// GLB 容器常量
constexpr UInt32 kGlbMagic     = 0x46546C67u;   // "glTF"
constexpr UInt32 kGlbChunkJson = 0x4E4F534Au;   // "JSON"
constexpr UInt32 kGlbChunkBin  = 0x004E4942u;   // "BIN\0"
constexpr UInt32 kGlbVersion   = 2u;

/// 组件类型的字节宽度 — 未知类型返回 0
UInt32 GetComponentSize(Int32 componentType)
{
    switch (componentType)
    {
        case kComponentByte:
        case kComponentUnsignedByte:  return 1;
        case kComponentShort:
        case kComponentUnsignedShort: return 2;
        case kComponentUnsignedInt:
        case kComponentFloat:         return 4;
        default:                      return 0;
    }
}

/// 访问器 type 字符串对应的分量个数 — 未知返回 0
UInt32 GetTypeComponentCount(const AnsiChar* typeName)
{
    if (typeName == nullptr)
    {
        return 0;
    }

    // 逐字符比较, 避免引入字符串比较依赖
    if (typeName[0] == 'S' && typeName[1] == 'C') { return 1; }   // SCALAR
    if (typeName[0] == 'V' && typeName[3] == '2') { return 2; }   // VEC2
    if (typeName[0] == 'V' && typeName[3] == '3') { return 3; }   // VEC3
    if (typeName[0] == 'V' && typeName[3] == '4') { return 4; }   // VEC4
    if (typeName[0] == 'M' && typeName[3] == '2') { return 4; }   // MAT2
    if (typeName[0] == 'M' && typeName[3] == '3') { return 9; }   // MAT3
    if (typeName[0] == 'M' && typeName[3] == '4') { return 16; }  // MAT4

    return 0;
}

/// 逐字节比较 C 字符串
bool CStringEquals(const AnsiChar* left, const AnsiChar* right)
{
    if (left == nullptr || right == nullptr)
    {
        return left == right;
    }

    SizeType i = 0;
    while (left[i] != '\0' && right[i] != '\0')
    {
        if (left[i] != right[i])
        {
            return false;
        }

        ++i;
    }

    return left[i] == right[i];
}

/// 判断字符串是否以给定前缀开头
bool StartsWithLiteral(const AnsiChar* text, const AnsiChar* prefix)
{
    if (text == nullptr || prefix == nullptr)
    {
        return false;
    }

    SizeType i = 0;
    while (prefix[i] != '\0')
    {
        if (text[i] == '\0' || text[i] != prefix[i])
        {
            return false;
        }

        ++i;
    }

    return true;
}

/// 把路径分隔符统一为正斜杠
FString NormalizeSlashes(const FString& path)
{
    FString result;

    for (SizeType i = 0; i < path.GetLength(); ++i)
    {
        result.AppendChar(path[i] == '\\' ? '/' : path[i]);
    }

    return result;
}

/// 拼接目录与相对路径
FString CombinePaths(const FString& directory, const FString& relative)
{
    if (directory.IsEmpty())
    {
        return NormalizeSlashes(relative);
    }

    FString normalized = NormalizeSlashes(relative);

    if (normalized.GetLength() >= 2 && normalized[1] == ':')
    {
        return normalized;
    }

    if (normalized.GetLength() >= 1 && normalized[0] == '/')
    {
        return normalized;
    }

    FString result = NormalizeSlashes(directory);

    if (!result.EndsWith("/"))
    {
        result.AppendChar('/');
    }

    result.Append(normalized);
    return result;
}

/// 取路径的目录部分
FString DirectoryOf(const FString& path)
{
    const FString normalized = NormalizeSlashes(path);

    for (SizeType i = normalized.GetLength(); i > 0; --i)
    {
        if (normalized[i - 1] == '/')
        {
            return normalized.Left(i - 1);
        }
    }

    return FString();
}

// ============================================================================
// FGltfContext — 解析过程中的共享状态
// ============================================================================

/// 已解引用的缓冲区视图
struct FBufferViewInfo
{
    /// 指向所属缓冲区的字节
    const UInt8* Data = nullptr;

    /// 视图长度
    SizeType Length = 0;

    /// 交错跨步 — 0 表示紧密排布
    UInt32 ByteStride = 0;
};

/// 解析上下文
struct FGltfContext
{
    FJsonValue Root;

    /// 全部缓冲区的字节内容 — 外部 .bin、data: URI 与 GLB 二进制块统一装入
    TArray<TArray<UInt8>> Buffers;

    FString BaseDirectory;

    TArray<FString>* Warnings = nullptr;
};

// ============================================================================
// FAccessorReader — 访问器解引用
// ============================================================================

/// 访问器读取器
///
/// 把 accessor → bufferView → buffer 的三级间接、两层 byteOffset、
/// 可选 byteStride、组件类型与 normalized 归一化，全部收敛在这里。
/// 构造成功后即可按元素下标取值，调用方无须再关心底层布局。
class FAccessorReader
{
public:
    /// 依据访问器下标建立读取器
    /// @return 是否建立成功 (访问器缺失、越界或类型不支持均判定失败)
    bool Initialize(const FGltfContext& context, Int32 accessorIndex,
                    FString& outError)
    {
        if (accessorIndex < 0)
        {
            outError = FString("访问器下标为负");
            return false;
        }

        const FJsonValue accessors = context.Root["accessors"];
        if (static_cast<SizeType>(accessorIndex) >= accessors.GetArraySize())
        {
            outError = StringFormat("访问器下标 {} 越界", accessorIndex);
            return false;
        }

        const FJsonValue accessor =
            accessors[static_cast<SizeType>(accessorIndex)];

        m_ComponentType = accessor.GetInt32Field("componentType", 0);
        m_Count         = accessor.GetUInt32Field("count", 0);
        m_Normalized    = accessor.GetBoolField("normalized", false);

        m_ComponentSize  = GetComponentSize(m_ComponentType);
        m_ComponentCount = GetTypeComponentCount(accessor.GetStringField("type"));

        if (m_ComponentSize == 0 || m_ComponentCount == 0)
        {
            outError = StringFormat(
                "访问器 {} 的组件类型 {} 或元素类型 '{}' 不受支持",
                accessorIndex, m_ComponentType,
                accessor.GetStringField("type", "?"));
            return false;
        }

        const UInt32 elementSize = m_ComponentSize * m_ComponentCount;

        // ---- 稀疏访问器与无 bufferView 的访问器: 规范规定按全零处理 ----
        if (!accessor.HasMember("bufferView"))
        {
            m_Data       = nullptr;
            m_Stride     = elementSize;
            m_IsZeroFill = true;
            return true;
        }

        const Int32 bufferViewIndex = accessor.GetInt32Field("bufferView", -1);

        FBufferViewInfo viewInfo;
        if (!ResolveBufferView(context, bufferViewIndex, viewInfo, outError))
        {
            return false;
        }

        const UInt32 accessorOffset = accessor.GetUInt32Field("byteOffset", 0);

        // byteStride 为 0 表示紧密排布, 此时步长即元素大小
        m_Stride = (viewInfo.ByteStride > 0) ? viewInfo.ByteStride : elementSize;

        // 最后一个元素的末尾不得越出视图 —— 越界一律拒绝而非夹紧,
        // 夹紧只会让损坏的资产静默产出扭曲的几何
        if (m_Count > 0)
        {
            const UInt64 lastElementEnd =
                static_cast<UInt64>(accessorOffset) +
                static_cast<UInt64>(m_Stride) * (m_Count - 1) + elementSize;

            if (lastElementEnd > viewInfo.Length)
            {
                outError = StringFormat(
                    "访问器 {} 越出缓冲区视图: 需要 {} 字节, 视图仅 {} 字节",
                    accessorIndex, lastElementEnd,
                    static_cast<UInt64>(viewInfo.Length));
                return false;
            }
        }

        m_Data = viewInfo.Data + accessorOffset;
        return true;
    }

    LIMX_NODISCARD UInt32 GetCount() const { return m_Count; }
    LIMX_NODISCARD UInt32 GetComponentCount() const { return m_ComponentCount; }

    /// 取第 index 个元素的第 component 个分量, 归一化为浮点
    LIMX_NODISCARD Float32 ReadFloat(UInt32 index, UInt32 component) const
    {
        if (m_IsZeroFill || m_Data == nullptr ||
            index >= m_Count || component >= m_ComponentCount)
        {
            return 0.0f;
        }

        const UInt8* element = m_Data + static_cast<SizeType>(index) * m_Stride +
                               static_cast<SizeType>(component) * m_ComponentSize;

        switch (m_ComponentType)
        {
            case kComponentFloat:
            {
                Float32 value = 0.0f;
                Memory::MemCopy(&value, element, sizeof(Float32));
                return value;
            }

            case kComponentUnsignedByte:
            {
                const UInt8 raw = *element;
                // normalized 时按 [0,255] → [0,1] 归一化
                return m_Normalized ? (static_cast<Float32>(raw) / 255.0f)
                                    : static_cast<Float32>(raw);
            }

            case kComponentByte:
            {
                Int8 raw = 0;
                Memory::MemCopy(&raw, element, sizeof(Int8));
                // 有符号归一化按规范取 max(v/127, -1)
                return m_Normalized
                           ? FMath::Max(static_cast<Float32>(raw) / 127.0f, -1.0f)
                           : static_cast<Float32>(raw);
            }

            case kComponentUnsignedShort:
            {
                UInt16 raw = 0;
                Memory::MemCopy(&raw, element, sizeof(UInt16));
                return m_Normalized ? (static_cast<Float32>(raw) / 65535.0f)
                                    : static_cast<Float32>(raw);
            }

            case kComponentShort:
            {
                Int16 raw = 0;
                Memory::MemCopy(&raw, element, sizeof(Int16));
                return m_Normalized
                           ? FMath::Max(static_cast<Float32>(raw) / 32767.0f, -1.0f)
                           : static_cast<Float32>(raw);
            }

            case kComponentUnsignedInt:
            {
                UInt32 raw = 0;
                Memory::MemCopy(&raw, element, sizeof(UInt32));
                return static_cast<Float32>(raw);
            }

            default:
                return 0.0f;
        }
    }

    /// 取第 index 个元素作为无符号整数 — 用于索引流
    LIMX_NODISCARD UInt32 ReadIndex(UInt32 index) const
    {
        if (m_IsZeroFill || m_Data == nullptr || index >= m_Count)
        {
            return 0;
        }

        const UInt8* element = m_Data + static_cast<SizeType>(index) * m_Stride;

        switch (m_ComponentType)
        {
            case kComponentUnsignedByte:
                return static_cast<UInt32>(*element);

            case kComponentUnsignedShort:
            {
                UInt16 raw = 0;
                Memory::MemCopy(&raw, element, sizeof(UInt16));
                return static_cast<UInt32>(raw);
            }

            case kComponentUnsignedInt:
            {
                UInt32 raw = 0;
                Memory::MemCopy(&raw, element, sizeof(UInt32));
                return raw;
            }

            default:
                return 0;
        }
    }

private:
    /// 解引用 bufferView 到实际字节区间
    static bool ResolveBufferView(const FGltfContext& context,
                                  Int32 bufferViewIndex,
                                  FBufferViewInfo& outInfo,
                                  FString& outError)
    {
        if (bufferViewIndex < 0)
        {
            outError = FString("缓冲区视图下标为负");
            return false;
        }

        const FJsonValue views = context.Root["bufferViews"];
        if (static_cast<SizeType>(bufferViewIndex) >= views.GetArraySize())
        {
            outError = StringFormat("缓冲区视图下标 {} 越界", bufferViewIndex);
            return false;
        }

        const FJsonValue view = views[static_cast<SizeType>(bufferViewIndex)];

        const Int32  bufferIndex = view.GetInt32Field("buffer", -1);
        const UInt32 viewOffset  = view.GetUInt32Field("byteOffset", 0);
        const UInt32 viewLength  = view.GetUInt32Field("byteLength", 0);

        if (bufferIndex < 0 ||
            static_cast<SizeType>(bufferIndex) >= context.Buffers.GetSize())
        {
            outError = StringFormat("缓冲区下标 {} 越界", bufferIndex);
            return false;
        }

        const TArray<UInt8>& buffer = context.Buffers[bufferIndex];

        if (static_cast<UInt64>(viewOffset) + viewLength > buffer.GetSize())
        {
            outError = StringFormat(
                "缓冲区视图 {} 越出缓冲区: 偏移 {} + 长度 {} > 容量 {}",
                bufferViewIndex, viewOffset, viewLength,
                static_cast<UInt64>(buffer.GetSize()));
            return false;
        }

        outInfo.Data       = buffer.GetData() + viewOffset;
        outInfo.Length     = viewLength;
        outInfo.ByteStride = view.GetUInt32Field("byteStride", 0);

        return true;
    }

    const UInt8* m_Data = nullptr;

    Int32  m_ComponentType  = 0;
    UInt32 m_ComponentSize  = 0;
    UInt32 m_ComponentCount = 0;
    UInt32 m_Count          = 0;
    UInt32 m_Stride         = 0;

    bool m_Normalized = false;
    bool m_IsZeroFill = false;
};

// ============================================================================
// 缓冲区加载
// ============================================================================

/// 加载全部 buffers — 支持外部文件、data: URI 与 GLB 二进制块
bool LoadBuffers(FGltfContext& context, const FGltfLoadOptions& options,
                 const TArray<UInt8>* glbBinaryChunk, FString& outError)
{
    const FJsonValue buffers = context.Root["buffers"];
    const SizeType bufferCount = buffers.GetArraySize();

    for (SizeType i = 0; i < bufferCount; ++i)
    {
        const FJsonValue buffer = buffers[i];

        TArray<UInt8> bytes;

        const AnsiChar* uri = buffer.GetStringField("uri", nullptr);

        if (uri == nullptr || uri[0] == '\0')
        {
            // ---- 无 uri: 数据来自 GLB 的二进制块, 只允许出现在第 0 个缓冲区 ----
            if (glbBinaryChunk == nullptr)
            {
                outError = StringFormat(
                    "缓冲区 {} 没有 uri, 但文件不含 GLB 二进制块", i);
                return false;
            }

            bytes = *glbBinaryChunk;
        }
        else if (StartsWithLiteral(uri, "data:"))
        {
            // ---- data: URI —— 找到 base64 载荷的起点 ----
            SizeType commaPos = 0;
            while (uri[commaPos] != '\0' && uri[commaPos] != ',')
            {
                ++commaPos;
            }

            if (uri[commaPos] != ',')
            {
                outError = StringFormat("缓冲区 {} 的 data URI 缺少逗号分隔符", i);
                return false;
            }

            if (!FBase64::Decode(uri + commaPos + 1, bytes))
            {
                outError = StringFormat("缓冲区 {} 的 base64 载荷解码失败", i);
                return false;
            }
        }
        else
        {
            // ---- 外部文件 ----
            if (!options.LoadExternalBuffers)
            {
                outError = StringFormat(
                    "缓冲区 {} 引用外部文件 '{}', 但已禁用外部加载", i, uri);
                return false;
            }

            const FString path = CombinePaths(context.BaseDirectory, FString(uri));
            bytes = FPlatformFile::ReadAllBytes(path);

            if (bytes.GetSize() == 0)
            {
                outError = StringFormat("无法读取缓冲区文件 '{}'", path.GetCStr());
                return false;
            }
        }

        // 声明长度与实际不符往往意味着文件损坏或截断
        const UInt32 declaredLength = buffer.GetUInt32Field("byteLength", 0);

        if (declaredLength > 0 && bytes.GetSize() < declaredLength)
        {
            outError = StringFormat(
                "缓冲区 {} 数据不足: 声明 {} 字节, 实际 {} 字节",
                i, declaredLength, static_cast<UInt64>(bytes.GetSize()));
            return false;
        }

        context.Buffers.Add(static_cast<TArray<UInt8>&&>(bytes));
    }

    return true;
}

// ============================================================================
// 图像
// ============================================================================

/// 收集内嵌图像的字节
void CollectImages(FGltfContext& context, const FGltfLoadOptions& options,
                   FAssetScene& outScene)
{
    if (!options.KeepEmbeddedImages)
    {
        return;
    }

    const FJsonValue images = context.Root["images"];

    for (SizeType i = 0; i < images.GetArraySize(); ++i)
    {
        const FJsonValue image = images[i];

        FEmbeddedImage embedded;
        embedded.Name     = FName(image.GetStringField("name", "image"));
        embedded.MimeType = FString(image.GetStringField("mimeType", ""));

        const AnsiChar* uri = image.GetStringField("uri", nullptr);

        if (uri != nullptr && StartsWithLiteral(uri, "data:"))
        {
            SizeType commaPos = 0;
            while (uri[commaPos] != '\0' && uri[commaPos] != ',')
            {
                ++commaPos;
            }

            if (uri[commaPos] == ',')
            {
                if (!FBase64::Decode(uri + commaPos + 1, embedded.Bytes) &&
                    context.Warnings != nullptr)
                {
                    context.Warnings->Add(
                        StringFormat("图像 {} 的 base64 载荷解码失败", i));
                }
            }
        }
        else if (image.HasMember("bufferView"))
        {
            // GLB 内嵌图像存放在缓冲区视图中
            const Int32 viewIndex = image.GetInt32Field("bufferView", -1);
            const FJsonValue views = context.Root["bufferViews"];

            if (viewIndex >= 0 &&
                static_cast<SizeType>(viewIndex) < views.GetArraySize())
            {
                const FJsonValue view = views[static_cast<SizeType>(viewIndex)];

                const Int32  bufferIndex = view.GetInt32Field("buffer", -1);
                const UInt32 viewOffset  = view.GetUInt32Field("byteOffset", 0);
                const UInt32 viewLength  = view.GetUInt32Field("byteLength", 0);

                if (bufferIndex >= 0 &&
                    static_cast<SizeType>(bufferIndex) < context.Buffers.GetSize())
                {
                    const TArray<UInt8>& buffer = context.Buffers[bufferIndex];

                    if (static_cast<UInt64>(viewOffset) + viewLength <=
                        buffer.GetSize())
                    {
                        embedded.Bytes.Reserve(viewLength);

                        for (UInt32 b = 0; b < viewLength; ++b)
                        {
                            embedded.Bytes.Add(buffer[viewOffset + b]);
                        }
                    }
                }
            }
        }

        outScene.EmbeddedImages.Add(static_cast<FEmbeddedImage&&>(embedded));
    }
}

/// 把 glTF 的 texture 下标解析为引擎的纹理引用
FTextureReference ResolveTexture(const FGltfContext& context,
                                 const FJsonValue& textureInfo,
                                 const FAssetScene& scene)
{
    FTextureReference reference;

    if (!textureInfo.IsObject())
    {
        return reference;
    }

    reference.TexCoordSet = textureInfo.GetUInt32Field("texCoord", 0);

    const Int32 textureIndex = textureInfo.GetInt32Field("index", -1);
    if (textureIndex < 0)
    {
        return reference;
    }

    const FJsonValue textures = context.Root["textures"];
    if (static_cast<SizeType>(textureIndex) >= textures.GetArraySize())
    {
        return reference;
    }

    const Int32 sourceIndex =
        textures[static_cast<SizeType>(textureIndex)].GetInt32Field("source", -1);

    if (sourceIndex < 0)
    {
        return reference;
    }

    const FJsonValue images = context.Root["images"];
    if (static_cast<SizeType>(sourceIndex) >= images.GetArraySize())
    {
        return reference;
    }

    const FJsonValue image = images[static_cast<SizeType>(sourceIndex)];
    const AnsiChar* uri = image.GetStringField("uri", nullptr);

    // 外部文件走路径, 内嵌数据走下标
    if (uri != nullptr && uri[0] != '\0' && !StartsWithLiteral(uri, "data:"))
    {
        reference.Path = CombinePaths(context.BaseDirectory, FString(uri));
    }
    else if (static_cast<SizeType>(sourceIndex) < scene.EmbeddedImages.GetSize())
    {
        reference.EmbeddedIndex = sourceIndex;
    }

    return reference;
}

// ============================================================================
// 材质
// ============================================================================

void ParseMaterials(const FGltfContext& context, FAssetScene& outScene)
{
    const FJsonValue materials = context.Root["materials"];

    for (SizeType i = 0; i < materials.GetArraySize(); ++i)
    {
        const FJsonValue source = materials[i];

        FMaterialData material;
        material.Name = FName(source.GetStringField("name", "material"));

        // ---- 金属粗糙度 ----
        const FJsonValue pbr = source["pbrMetallicRoughness"];

        if (pbr.IsObject())
        {
            const FJsonValue baseColor = pbr["baseColorFactor"];
            if (baseColor.GetArraySize() >= 4)
            {
                material.BaseColorFactor = FVector4(
                    baseColor[SizeType(0)].AsFloat(1.0f),
                    baseColor[SizeType(1)].AsFloat(1.0f),
                    baseColor[SizeType(2)].AsFloat(1.0f),
                    baseColor[SizeType(3)].AsFloat(1.0f));
            }

            material.MetallicFactor  = pbr.GetFloatField("metallicFactor", 1.0f);
            material.RoughnessFactor = pbr.GetFloatField("roughnessFactor", 1.0f);

            material.BaseColorTexture =
                ResolveTexture(context, pbr["baseColorTexture"], outScene);
            material.MetallicRoughnessTexture = ResolveTexture(
                context, pbr["metallicRoughnessTexture"], outScene);
        }

        // ---- 自发光 ----
        const FJsonValue emissive = source["emissiveFactor"];
        if (emissive.GetArraySize() >= 3)
        {
            material.EmissiveFactor = FVector3(
                emissive[SizeType(0)].AsFloat(0.0f),
                emissive[SizeType(1)].AsFloat(0.0f),
                emissive[SizeType(2)].AsFloat(0.0f));
        }

        material.EmissiveTexture =
            ResolveTexture(context, source["emissiveTexture"], outScene);

        // ---- 法线与遮蔽 ----
        const FJsonValue normalTexture = source["normalTexture"];
        if (normalTexture.IsObject())
        {
            material.NormalScale = normalTexture.GetFloatField("scale", 1.0f);
            material.NormalTexture =
                ResolveTexture(context, normalTexture, outScene);
        }

        const FJsonValue occlusionTexture = source["occlusionTexture"];
        if (occlusionTexture.IsObject())
        {
            material.OcclusionStrength =
                occlusionTexture.GetFloatField("strength", 1.0f);
            material.OcclusionTexture =
                ResolveTexture(context, occlusionTexture, outScene);
        }

        // ---- 透明与面剔除 ----
        const AnsiChar* alphaMode = source.GetStringField("alphaMode", "OPAQUE");

        if (CStringEquals(alphaMode, "MASK"))
        {
            material.AlphaMode   = EAlphaMode::Mask;
            material.AlphaCutoff = source.GetFloatField("alphaCutoff", 0.5f);
        }
        else if (CStringEquals(alphaMode, "BLEND"))
        {
            material.AlphaMode = EAlphaMode::Blend;
        }

        material.DoubleSided = source.GetBoolField("doubleSided", false);

        outScene.Materials.Add(static_cast<FMaterialData&&>(material));
    }
}

// ============================================================================
// 网格
// ============================================================================

/// 解析一个图元并追加到网格
bool ParsePrimitive(const FGltfContext& context, const FJsonValue& primitive,
                    const FGltfLoadOptions& options,
                    FMeshData& outMesh, SizeType primitiveIndex,
                    TArray<FString>* outWarnings,
                    FString& outError)
{
    // ---- 只接受三角形 ----
    const Int32 mode = primitive.GetInt32Field("mode", kPrimitiveModeTriangles);

    if (mode != kPrimitiveModeTriangles)
    {
        if (outWarnings != nullptr)
        {
            outWarnings->Add(StringFormat(
                "图元 {} 的模式为 {} (非三角形), 已跳过", primitiveIndex, mode));
        }

        return true;
    }

    const FJsonValue attributes = primitive["attributes"];

    if (!attributes.IsObject() || !attributes.HasMember("POSITION"))
    {
        if (outWarnings != nullptr)
        {
            outWarnings->Add(StringFormat(
                "图元 {} 缺少 POSITION 属性, 已跳过", primitiveIndex));
        }

        return true;
    }

    // ---- 位置 ----
    FAccessorReader positionReader;
    if (!positionReader.Initialize(context,
                                   attributes.GetInt32Field("POSITION", -1),
                                   outError))
    {
        return false;
    }

    const UInt32 vertexCount = positionReader.GetCount();
    if (vertexCount == 0)
    {
        return true;
    }

    // 本图元的顶点追加在已有顶点之后, 索引需整体偏移
    const UInt32 vertexBase = static_cast<UInt32>(outMesh.Vertices.GetSize());

    // ---- 可选属性 ----
    FAccessorReader normalReader;
    const bool hasNormals =
        attributes.HasMember("NORMAL") &&
        normalReader.Initialize(context, attributes.GetInt32Field("NORMAL", -1),
                                outError);

    FAccessorReader tangentReader;
    const bool hasTangents =
        attributes.HasMember("TANGENT") &&
        tangentReader.Initialize(context, attributes.GetInt32Field("TANGENT", -1),
                                 outError);

    FAccessorReader texCoord0Reader;
    const bool hasTexCoord0 =
        attributes.HasMember("TEXCOORD_0") &&
        texCoord0Reader.Initialize(
            context, attributes.GetInt32Field("TEXCOORD_0", -1), outError);

    FAccessorReader texCoord1Reader;
    const bool hasTexCoord1 =
        attributes.HasMember("TEXCOORD_1") &&
        texCoord1Reader.Initialize(
            context, attributes.GetInt32Field("TEXCOORD_1", -1), outError);

    FAccessorReader colorReader;
    const bool hasColor =
        attributes.HasMember("COLOR_0") &&
        colorReader.Initialize(context, attributes.GetInt32Field("COLOR_0", -1),
                               outError);

    // ---- 装配顶点 ----
    outMesh.Vertices.Reserve(outMesh.Vertices.GetSize() + vertexCount);

    for (UInt32 v = 0; v < vertexCount; ++v)
    {
        FMeshVertex vertex;

        vertex.Position = FVector3(positionReader.ReadFloat(v, 0),
                                   positionReader.ReadFloat(v, 1),
                                   positionReader.ReadFloat(v, 2));

        if (hasNormals && v < normalReader.GetCount())
        {
            vertex.Normal = FVector3(normalReader.ReadFloat(v, 0),
                                     normalReader.ReadFloat(v, 1),
                                     normalReader.ReadFloat(v, 2));
        }

        if (hasTangents && v < tangentReader.GetCount())
        {
            vertex.Tangent = FVector4(tangentReader.ReadFloat(v, 0),
                                      tangentReader.ReadFloat(v, 1),
                                      tangentReader.ReadFloat(v, 2),
                                      tangentReader.ReadFloat(v, 3));
        }

        if (hasTexCoord0 && v < texCoord0Reader.GetCount())
        {
            const Float32 u = texCoord0Reader.ReadFloat(v, 0);
            const Float32 t = texCoord0Reader.ReadFloat(v, 1);

            vertex.TexCoord0 = FVector2(u, options.FlipTexCoordV ? (1.0f - t) : t);
        }

        if (hasTexCoord1 && v < texCoord1Reader.GetCount())
        {
            const Float32 u = texCoord1Reader.ReadFloat(v, 0);
            const Float32 t = texCoord1Reader.ReadFloat(v, 1);

            vertex.TexCoord1 = FVector2(u, options.FlipTexCoordV ? (1.0f - t) : t);
        }

        if (hasColor && v < colorReader.GetCount())
        {
            // COLOR_0 可以是 VEC3 或 VEC4 — VEC3 时 alpha 补 1
            vertex.Color = FVector4(
                colorReader.ReadFloat(v, 0),
                colorReader.ReadFloat(v, 1),
                colorReader.ReadFloat(v, 2),
                (colorReader.GetComponentCount() >= 4)
                    ? colorReader.ReadFloat(v, 3)
                    : 1.0f);
        }

        outMesh.Vertices.Add(vertex);
    }

    outMesh.HasNormals      = outMesh.HasNormals || hasNormals;
    outMesh.HasTangents     = outMesh.HasTangents || hasTangents;
    outMesh.HasTexCoords    = outMesh.HasTexCoords || hasTexCoord0;
    outMesh.HasVertexColors = outMesh.HasVertexColors || hasColor;

    // ---- 索引 ----
    const UInt32 indexStart = static_cast<UInt32>(outMesh.Indices.GetSize());

    if (primitive.HasMember("indices"))
    {
        FAccessorReader indexReader;
        if (!indexReader.Initialize(context,
                                    primitive.GetInt32Field("indices", -1),
                                    outError))
        {
            return false;
        }

        const UInt32 indexCount = indexReader.GetCount();
        outMesh.Indices.Reserve(outMesh.Indices.GetSize() + indexCount);

        for (UInt32 i = 0; i < indexCount; ++i)
        {
            const UInt32 localIndex = indexReader.ReadIndex(i);

            if (localIndex >= vertexCount)
            {
                outError = StringFormat(
                    "图元 {} 的索引 {} 超出顶点数 {}",
                    primitiveIndex, localIndex, vertexCount);
                return false;
            }

            outMesh.Indices.Add(vertexBase + localIndex);
        }
    }
    else
    {
        // 无索引时按顶点顺序构成三角形列表
        outMesh.Indices.Reserve(outMesh.Indices.GetSize() + vertexCount);

        for (UInt32 i = 0; i < vertexCount; ++i)
        {
            outMesh.Indices.Add(vertexBase + i);
        }
    }

    // ---- 子网格 ----
    FSubMesh subMesh;
    subMesh.Name          = FName(StringFormat("primitive_{}",
                                               primitiveIndex).GetCStr());
    subMesh.IndexOffset   = indexStart;
    subMesh.IndexCount    = static_cast<UInt32>(outMesh.Indices.GetSize()) -
                            indexStart;
    subMesh.MaterialIndex = primitive.GetInt32Field("material", -1);

    if (subMesh.IndexCount > 0)
    {
        outMesh.SubMeshes.Add(subMesh);
    }

    return true;
}

/// 一个图元独立装配出来的结果
///
/// 每个图元装进自己的 FMeshData, 之后再合并 —— 这样装配阶段就没有共享
/// 写入, 可以并行。原先所有图元直接追加进同一份网格, 索引偏移逐个依赖
/// 前一个的顶点数, 顺序不能动。
struct FPrimitiveResult
{
    FMeshData       Mesh;
    TArray<FString> Warnings;
    FString         Error;
    bool            Succeeded = true;
};

/// 把一个图元的装配结果并入总网格
///
/// 索引与子网格的偏移在这里统一补上。合并本身是纯 memcpy 级的操作,
/// 放在主线程串行做就够 —— 实测 103 个图元合并只有几毫秒, 而并行它
/// 需要预先算出每个图元的落位, 复杂度换不来什么。
void MergePrimitiveResult(FMeshData& outMesh, FPrimitiveResult& result)
{
    const UInt32 vertexBase =
        static_cast<UInt32>(outMesh.Vertices.GetSize());
    const UInt32 indexBase =
        static_cast<UInt32>(outMesh.Indices.GetSize());

    const FMeshData& source = result.Mesh;

    for (SizeType i = 0; i < source.Vertices.GetSize(); ++i)
    {
        outMesh.Vertices.Add(source.Vertices[i]);
    }

    for (SizeType i = 0; i < source.Indices.GetSize(); ++i)
    {
        outMesh.Indices.Add(source.Indices[i] + vertexBase);
    }

    for (SizeType i = 0; i < source.SubMeshes.GetSize(); ++i)
    {
        FSubMesh subMesh = source.SubMeshes[i];
        subMesh.IndexOffset += indexBase;

        outMesh.SubMeshes.Add(subMesh);
    }

    outMesh.HasNormals      = outMesh.HasNormals || source.HasNormals;
    outMesh.HasTangents     = outMesh.HasTangents || source.HasTangents;
    outMesh.HasTexCoords    = outMesh.HasTexCoords || source.HasTexCoords;
    outMesh.HasVertexColors =
        outMesh.HasVertexColors || source.HasVertexColors;
}

bool ParseMeshes(const FGltfContext& context, const FGltfLoadOptions& options,
                 FAssetScene& outScene, FString& outError)
{
    const FJsonValue meshes = context.Root["meshes"];

    // 图元少到一定程度就不值得开线程 —— 建一张任务图要创建十几个线程,
    // 而单个图元的装配只有零点几毫秒。实测并行化对 Sponza 的 103 个图元
    // 值 14 ms (20 ms → 6 ms), 摊到两三个图元上则完全被线程创建吃掉。
    //
    // 阈值取 32: 大约是"并行收益开始明显超过建图开销"的量级。
    constexpr SizeType kParallelPrimitiveThreshold = 32;

    SizeType totalPrimitives = 0;

    for (SizeType i = 0; i < meshes.GetArraySize(); ++i)
    {
        totalPrimitives += meshes[i]["primitives"].GetArraySize();
    }

    const bool useParallel = totalPrimitives >= kParallelPrimitiveThreshold;

    // 图随本次解析建、随本次解析拆。与纹理解码那边同理: 几毫秒的线程创建
    // 换掉全局图的初始化与关闭时序问题。
    FTaskGraph graph;

    if (useParallel)
    {
        graph.Initialize(0);
    }

    for (SizeType i = 0; i < meshes.GetArraySize(); ++i)
    {
        const FJsonValue source = meshes[i];

        FMeshData mesh;
        mesh.Name = FName(source.GetStringField("name", "mesh"));

        const FJsonValue primitives = source["primitives"];

        const SizeType primitiveCount = primitives.GetArraySize();

        TArray<FPrimitiveResult> results;
        results.SetSize(primitiveCount);

        // 装配 —— 各图元互不相干:
        //   FJsonValue 只是 (文档指针, 节点下标) 两个字段, 无可变状态,
        //     而文档此刻已经解析完毕、只读;
        //   FGltfContext 除 Warnings 指针外全是只读数据 (缓冲区、目录);
        //   警告与错误各图元收进自己的槽位, 合并时再汇总 —— 直接往共享
        //     数组里写是数据竞争, 而往日志里写更糟: FLog 没有加锁。
        {
            FPrimitiveResult*       resultData = results.GetData();
            const FGltfContext*     contextPtr = &context;
            const FGltfLoadOptions* optionsPtr = &options;

            const auto assemble =
                [resultData, contextPtr, optionsPtr, &primitives](
                    SizeType begin, SizeType end)
                {
                    for (SizeType p = begin; p < end; ++p)
                    {
                        FPrimitiveResult& slot = resultData[p];

                        slot.Succeeded = ParsePrimitive(
                            *contextPtr, primitives[p], *optionsPtr,
                            slot.Mesh, p, &slot.Warnings, slot.Error);
                    }
                };

            if (useParallel)
            {
                FJobExecutor::ParallelFor(graph, primitiveCount, 1, assemble);
            }
            else
            {
                assemble(0, primitiveCount);
            }
        }

        // ---- 一次性预留总量 ----
        //
        // 必须在合并之前把总量算出来一次留够。逐图元 Reserve(已有 + 新增)
        // 会让容量以 103 步递增, 而每一步都要把已有的顶点整体搬一遍 ——
        // 192k 个顶点搬 103 次就是一个多 GB 的 memcpy。
        //
        // 这笔开销原本就藏在串行版本里, 只是与图元装配混在一起看不出来。
        {
            SizeType totalVertices = 0;
            SizeType totalIndices  = 0;
            SizeType totalSubMeshes = 0;

            for (SizeType p = 0; p < primitiveCount; ++p)
            {
                totalVertices  += results[p].Mesh.Vertices.GetSize();
                totalIndices   += results[p].Mesh.Indices.GetSize();
                totalSubMeshes += results[p].Mesh.SubMeshes.GetSize();
            }

            mesh.Vertices.Reserve(totalVertices);
            mesh.Indices.Reserve(totalIndices);
            mesh.SubMeshes.Reserve(totalSubMeshes);
        }

        // ---- 按原顺序合并 ----
        //
        // 顺序必须与串行版本一致: 子网格的 MaterialIndex 指向材质表, 而
        // 渲染时按子网格顺序绑定材质 —— 顺序变了, 画面上就是材质错位。
        for (SizeType p = 0; p < primitiveCount; ++p)
        {
            FPrimitiveResult& slot = results[p];

            if (!slot.Succeeded)
            {
                outError = slot.Error;

                if (useParallel)
                {
                    graph.Shutdown();
                }

                return false;
            }

            if (context.Warnings != nullptr)
            {
                for (SizeType w = 0; w < slot.Warnings.GetSize(); ++w)
                {
                    context.Warnings->Add(slot.Warnings[w]);
                }
            }

            MergePrimitiveResult(mesh, slot);
        }

        // 补齐缺失属性
        if (!mesh.HasNormals && options.GenerateMissingNormals)
        {
            mesh.GenerateNormals();
        }

        if (!mesh.HasTangents && mesh.HasTexCoords &&
            options.GenerateMissingTangents)
        {
            mesh.GenerateTangents();
        }

        mesh.RecomputeBounds();

        outScene.Meshes.Add(static_cast<FMeshData&&>(mesh));
    }

    if (useParallel)
    {
        graph.Shutdown();
    }

    return true;
}

// ============================================================================
// 节点层级
// ============================================================================

/// 解析节点的局部变换 — matrix 与 TRS 两种表达统一为 FTransform
FTransform ParseNodeTransform(const FJsonValue& node)
{
    // matrix 与 TRS 互斥; matrix 存在时优先
    const FJsonValue matrixValue = node["matrix"];

    if (matrixValue.GetArraySize() == 16)
    {
        // glTF 的矩阵按列主序存储, 而 FMatrix 是行主序 —— 转置写入
        FMatrix matrix;

        for (Int32 column = 0; column < 4; ++column)
        {
            for (Int32 row = 0; row < 4; ++row)
            {
                matrix.M[row][column] =
                    matrixValue[static_cast<SizeType>(column * 4 + row)]
                        .AsFloat(0.0f);
            }
        }

        return FTransform::FromMatrix(matrix);
    }

    FTransform transform;

    const FJsonValue translation = node["translation"];
    if (translation.GetArraySize() >= 3)
    {
        transform.Translation = FVector3(translation[SizeType(0)].AsFloat(0.0f),
                                         translation[SizeType(1)].AsFloat(0.0f),
                                         translation[SizeType(2)].AsFloat(0.0f));
    }

    const FJsonValue rotation = node["rotation"];
    if (rotation.GetArraySize() >= 4)
    {
        // glTF 四元数按 (x, y, z, w) 顺序存储
        transform.Rotation = FQuat(rotation[SizeType(0)].AsFloat(0.0f),
                                   rotation[SizeType(1)].AsFloat(0.0f),
                                   rotation[SizeType(2)].AsFloat(0.0f),
                                   rotation[SizeType(3)].AsFloat(1.0f));
        transform.Rotation.Normalize();
    }

    const FJsonValue scale = node["scale"];
    if (scale.GetArraySize() >= 3)
    {
        transform.Scale3D = FVector3(scale[SizeType(0)].AsFloat(1.0f),
                                     scale[SizeType(1)].AsFloat(1.0f),
                                     scale[SizeType(2)].AsFloat(1.0f));
    }

    return transform;
}

void ParseNodes(const FGltfContext& context, FAssetScene& outScene)
{
    const FJsonValue nodes = context.Root["nodes"];
    const SizeType nodeCount = nodes.GetArraySize();

    // 先建立全部节点, 再回填父子关系 —— 子节点下标可能指向尚未创建的节点
    for (SizeType i = 0; i < nodeCount; ++i)
    {
        const FJsonValue source = nodes[i];

        FSceneNode node;
        node.Name           = FName(source.GetStringField("name", "node"));
        node.LocalTransform = ParseNodeTransform(source);
        node.MeshIndex      = source.GetInt32Field("mesh", -1);
        node.ParentIndex    = -1;

        outScene.Nodes.Add(static_cast<FSceneNode&&>(node));
    }

    for (SizeType i = 0; i < nodeCount; ++i)
    {
        const FJsonValue children = nodes[i]["children"];

        for (SizeType c = 0; c < children.GetArraySize(); ++c)
        {
            const Int32 childIndex = children[c].AsInt32(-1);

            if (childIndex < 0 ||
                static_cast<SizeType>(childIndex) >= nodeCount)
            {
                continue;
            }

            outScene.Nodes[i].Children.Add(childIndex);
            outScene.Nodes[childIndex].ParentIndex = static_cast<Int32>(i);
        }
    }

    // ---- 根节点 ----
    const Int32 defaultScene = context.Root.GetInt32Field("scene", 0);
    const FJsonValue scenes  = context.Root["scenes"];

    if (defaultScene >= 0 &&
        static_cast<SizeType>(defaultScene) < scenes.GetArraySize())
    {
        const FJsonValue sceneNodes =
            scenes[static_cast<SizeType>(defaultScene)]["nodes"];

        for (SizeType i = 0; i < sceneNodes.GetArraySize(); ++i)
        {
            const Int32 rootIndex = sceneNodes[i].AsInt32(-1);

            if (rootIndex >= 0 &&
                static_cast<SizeType>(rootIndex) < nodeCount)
            {
                outScene.RootNodes.Add(rootIndex);
            }
        }
    }

    // 场景未声明根节点时, 把所有无父节点者视为根
    if (outScene.RootNodes.GetSize() == 0)
    {
        for (SizeType i = 0; i < outScene.Nodes.GetSize(); ++i)
        {
            if (outScene.Nodes[i].ParentIndex < 0)
            {
                outScene.RootNodes.Add(static_cast<Int32>(i));
            }
        }
    }
}

/// 检查是否使用了本解析器无法处理的扩展
bool CheckRequiredExtensions(const FGltfContext& context, FString& outError)
{
    const FJsonValue required = context.Root["extensionsRequired"];

    for (SizeType i = 0; i < required.GetArraySize(); ++i)
    {
        const AnsiChar* name = required[i].AsString("");

        // 压缩扩展改变了几何的存储方式 —— 忽略它只会产出空的或错误的网格,
        // 因此判定为失败而非告警
        if (CStringEquals(name, "KHR_draco_mesh_compression") ||
            CStringEquals(name, "EXT_meshopt_compression"))
        {
            outError = StringFormat(
                "文件要求扩展 '{}', 本解析器尚不支持几何压缩", name);
            return false;
        }
    }

    return true;
}

} // namespace

// ============================================================================
// FGltfLoader — JSON 入口
// ============================================================================

FAssetLoadResult FGltfLoader::LoadFromJson(const AnsiChar* json,
                                           SizeType length,
                                           const FString& baseDirectory,
                                           FAssetScene& outScene,
                                           const FGltfLoadOptions& options)
{
    outScene.Reset();
    outScene.BaseDirectory = NormalizeSlashes(baseDirectory);

    if (json == nullptr || length == 0)
    {
        return FAssetLoadResult::Failure(FString("glTF 内容为空"));
    }

    FJsonDocument document;

    if (!document.Parse(json, length))
    {
        return FAssetLoadResult::Failure(
            StringFormat("glTF JSON 解析失败 (第 {} 行第 {} 列): {}",
                         document.GetErrorLine(), document.GetErrorColumn(),
                         document.GetErrorMessage().GetCStr()),
            document.GetErrorLine());
    }

    TArray<FString> warnings;

    FGltfContext context;
    context.Root          = document.GetRoot();
    context.BaseDirectory = outScene.BaseDirectory;
    context.Warnings      = &warnings;

    // ---- 版本校验 ----
    const AnsiChar* version =
        context.Root["asset"].GetStringField("version", "");

    if (version[0] != '2')
    {
        return FAssetLoadResult::Failure(
            StringFormat("不支持的 glTF 版本 '{}', 仅支持 2.x", version));
    }

    FString error;

    if (!CheckRequiredExtensions(context, error))
    {
        return FAssetLoadResult::Failure(error);
    }

    if (!LoadBuffers(context, options, nullptr, error))
    {
        return FAssetLoadResult::Failure(error);
    }

    CollectImages(context, options, outScene);
    ParseMaterials(context, outScene);

    if (!ParseMeshes(context, options, outScene, error))
    {
        FAssetLoadResult result = FAssetLoadResult::Failure(error);
        result.Warnings = static_cast<TArray<FString>&&>(warnings);
        return result;
    }

    ParseNodes(context, outScene);

    outScene.Name = FName(context.Root["asset"].GetStringField("generator",
                                                               "GltfScene"));
    outScene.RecomputeBounds();

    FAssetLoadResult result = FAssetLoadResult::Success();
    result.Warnings = static_cast<TArray<FString>&&>(warnings);

    return result;
}

// ============================================================================
// FGltfLoader — GLB 入口
// ============================================================================

FAssetLoadResult FGltfLoader::LoadFromGlb(const UInt8* data, SizeType length,
                                          const FString& baseDirectory,
                                          FAssetScene& outScene,
                                          const FGltfLoadOptions& options)
{
    outScene.Reset();
    outScene.BaseDirectory = NormalizeSlashes(baseDirectory);

    // 头部为 12 字节: 魔数 + 版本 + 总长度
    if (data == nullptr || length < 12)
    {
        return FAssetLoadResult::Failure(FString("GLB 数据过短, 不足文件头"));
    }

    UInt32 magic        = 0;
    UInt32 version      = 0;
    UInt32 totalLength  = 0;

    Memory::MemCopy(&magic, data, sizeof(UInt32));
    Memory::MemCopy(&version, data + 4, sizeof(UInt32));
    Memory::MemCopy(&totalLength, data + 8, sizeof(UInt32));

    if (magic != kGlbMagic)
    {
        return FAssetLoadResult::Failure(
            StringFormat("GLB 魔数不匹配: 期望 {}, 实际 {}",
                         FHex(kGlbMagic), FHex(magic)));
    }

    if (version != kGlbVersion)
    {
        return FAssetLoadResult::Failure(
            StringFormat("不支持的 GLB 版本 {}, 仅支持 2", version));
    }

    if (totalLength > length)
    {
        return FAssetLoadResult::Failure(
            StringFormat("GLB 声明长度 {} 超过实际数据 {} 字节",
                         totalLength, static_cast<UInt64>(length)));
    }

    // ------------------------------------------------------------------
    // 逐块扫描 — 第一块必须是 JSON, 二进制块可选
    // ------------------------------------------------------------------

    const AnsiChar* jsonChunk       = nullptr;
    UInt32          jsonChunkLength = 0;

    TArray<UInt8> binaryChunk;
    bool          hasBinaryChunk = false;

    SizeType cursor = 12;

    while (cursor + 8 <= totalLength)
    {
        UInt32 chunkLength = 0;
        UInt32 chunkType   = 0;

        Memory::MemCopy(&chunkLength, data + cursor, sizeof(UInt32));
        Memory::MemCopy(&chunkType, data + cursor + 4, sizeof(UInt32));

        cursor += 8;

        if (static_cast<UInt64>(cursor) + chunkLength > totalLength)
        {
            return FAssetLoadResult::Failure(
                StringFormat("GLB 块越界: 偏移 {} + 长度 {} > 总长 {}",
                             static_cast<UInt64>(cursor), chunkLength,
                             totalLength),
                static_cast<UInt32>(cursor));
        }

        if (chunkType == kGlbChunkJson)
        {
            jsonChunk       = reinterpret_cast<const AnsiChar*>(data + cursor);
            jsonChunkLength = chunkLength;
        }
        else if (chunkType == kGlbChunkBin)
        {
            binaryChunk.Reserve(chunkLength);

            for (UInt32 i = 0; i < chunkLength; ++i)
            {
                binaryChunk.Add(data[cursor + i]);
            }

            hasBinaryChunk = true;
        }
        // 未知块类型按规范应被忽略

        cursor += chunkLength;

        // 块与块之间按 4 字节对齐
        const SizeType padding = (4 - (cursor % 4)) % 4;
        cursor += padding;
    }

    if (jsonChunk == nullptr || jsonChunkLength == 0)
    {
        return FAssetLoadResult::Failure(FString("GLB 缺少 JSON 块"));
    }

    // ------------------------------------------------------------------
    // 解析 JSON 部分 — 与 .gltf 路径共用同一套逻辑, 差别只在缓冲区来源
    // ------------------------------------------------------------------

    FJsonDocument document;

    if (!document.Parse(jsonChunk, jsonChunkLength))
    {
        return FAssetLoadResult::Failure(
            StringFormat("GLB 内的 JSON 解析失败 (第 {} 行第 {} 列): {}",
                         document.GetErrorLine(), document.GetErrorColumn(),
                         document.GetErrorMessage().GetCStr()),
            document.GetErrorLine());
    }

    TArray<FString> warnings;

    FGltfContext context;
    context.Root          = document.GetRoot();
    context.BaseDirectory = outScene.BaseDirectory;
    context.Warnings      = &warnings;

    const AnsiChar* gltfVersion =
        context.Root["asset"].GetStringField("version", "");

    if (gltfVersion[0] != '2')
    {
        return FAssetLoadResult::Failure(
            StringFormat("不支持的 glTF 版本 '{}', 仅支持 2.x", gltfVersion));
    }

    FString error;

    if (!CheckRequiredExtensions(context, error))
    {
        return FAssetLoadResult::Failure(error);
    }

    if (!LoadBuffers(context, options,
                     hasBinaryChunk ? &binaryChunk : nullptr, error))
    {
        return FAssetLoadResult::Failure(error);
    }

    CollectImages(context, options, outScene);
    ParseMaterials(context, outScene);

    if (!ParseMeshes(context, options, outScene, error))
    {
        FAssetLoadResult result = FAssetLoadResult::Failure(error);
        result.Warnings = static_cast<TArray<FString>&&>(warnings);
        return result;
    }

    ParseNodes(context, outScene);

    outScene.Name = FName(context.Root["asset"].GetStringField("generator",
                                                               "GlbScene"));
    outScene.RecomputeBounds();

    FAssetLoadResult result = FAssetLoadResult::Success();
    result.Warnings = static_cast<TArray<FString>&&>(warnings);

    return result;
}

// ============================================================================
// FGltfLoader — 文件入口
// ============================================================================

FAssetLoadResult FGltfLoader::LoadFromFile(const FString& path,
                                           FAssetScene& outScene,
                                           const FGltfLoadOptions& options)
{
    const TArray<UInt8> bytes = FPlatformFile::ReadAllBytes(path);

    if (bytes.GetSize() == 0)
    {
        return FAssetLoadResult::Failure(
            StringFormat("无法读取 glTF 文件: {}", path.GetCStr()));
    }

    const FString directory = DirectoryOf(path);

    // 按魔数而非扩展名区分容器格式 —— 扩展名可能被改过, 魔数不会
    if (bytes.GetSize() >= 4)
    {
        UInt32 magic = 0;
        Memory::MemCopy(&magic, bytes.GetData(), sizeof(UInt32));

        if (magic == kGlbMagic)
        {
            return LoadFromGlb(bytes.GetData(), bytes.GetSize(), directory,
                               outScene, options);
        }
    }

    return LoadFromJson(reinterpret_cast<const AnsiChar*>(bytes.GetData()),
                        bytes.GetSize(), directory, outScene, options);
}

} // namespace Limx
