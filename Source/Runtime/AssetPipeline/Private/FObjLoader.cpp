/*******************************************************************************
 * 文件: FObjLoader.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   OBJ / MTL 解析器实现 — 词法扫描、面三角化、顶点去重、材质换算
 *
 * 设计哲学:
 *   去重键就是索引三元组 — OBJ 的面按 (位置/UV/法线) 三个独立索引描述顶点，
 *   同一位置在不同面上可配不同法线。直接用这三个索引组成哈希键做折叠，
 *   既精确又不必比较浮点值，也就不存在容差取舍问题。
 *
 *   Phong 到 PBR 的换算写明依据 — Ns 到粗糙度采用 sqrt(2/(Ns+2))，
 *   这是 Blinn-Phong 指数与 GGX 粗糙度的通行对应关系。金属度默认取 0：
 *   OBJ 没有金属度概念，从 Ks 反推极不可靠，误判为金属会让材质整体发黑。
 *   若 MTL 带有 PBR 扩展字段 (Pr/Pm)，一律优先采用而不做近似。
 *
 *   跳过而非中止 — 真实资产库里总有个别损坏的行。遇到畸形行记入告警并继续，
 *   让调用方拿到尽可能完整的模型，同时保留问题的可见性。
 *
 * 技术特性:
 *   - 单遍扫描, 面数据边解析边折叠, 不保留中间的面列表
 *   - 扇形三角化任意多边形, n 边形产出 n-2 个三角形
 *   - 贴图行跳过 -o/-s/-bm 等选项参数, 取末尾的文件名
 *   - 路径分隔符统一为正斜杠, 兼容 Windows 与 Unix 风格的 MTL
 *
 * 依赖关系:
 *   内部: AssetPipeline/FObjLoader.h, Core/Containers/TMap.h,
 *          Core/HAL/FPlatformFile.h, Core/Misc/FPath.h
 *
 * 注意事项:
 *   面索引一律按 1 起始解释; 负索引相对于当前已读取的元素数
 *
 ******************************************************************************/

#include "AssetPipeline/FObjLoader.h"
#include "Core/Containers/TMap.h"
#include "Core/HAL/FPlatformFile.h"

namespace Limx
{

namespace
{

// ============================================================================
// 词法辅助
// ============================================================================

FORCEINLINE bool IsSpace(AnsiChar c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

FORCEINLINE bool IsLineEnd(AnsiChar c)
{
    return c == '\n' || c == '\0';
}

FORCEINLINE bool IsDigitChar(AnsiChar c)
{
    return c >= '0' && c <= '9';
}

/// 跳过空白 (不跨行)
void SkipSpaces(const AnsiChar* text, SizeType length, SizeType& cursor)
{
    while (cursor < length && IsSpace(text[cursor]))
    {
        ++cursor;
    }
}

/// 跳到行尾
void SkipToLineEnd(const AnsiChar* text, SizeType length, SizeType& cursor)
{
    while (cursor < length && text[cursor] != '\n')
    {
        ++cursor;
    }
}

/// 解析一个浮点数
///
/// OBJ 的数字比 JSON 宽松: 允许 ".5"、"1." 这类写法, 因此不复用 JSON 的
/// 严格实现。同样采用尾数加十进制指数的方式以保持精度。
bool ParseFloatToken(const AnsiChar* text, SizeType length, SizeType& cursor,
                     Float32& outValue)
{
    SkipSpaces(text, length, cursor);

    if (cursor >= length)
    {
        return false;
    }

    bool isNegative = false;

    if (text[cursor] == '-' || text[cursor] == '+')
    {
        isNegative = (text[cursor] == '-');
        ++cursor;
    }

    UInt64 mantissa      = 0;
    Int32  decimalExp    = 0;
    Int32  digitCount    = 0;
    bool   sawAnyDigit   = false;

    while (cursor < length && IsDigitChar(text[cursor]))
    {
        sawAnyDigit = true;

        if (digitCount < 18)
        {
            mantissa = mantissa * 10u +
                       static_cast<UInt64>(text[cursor] - '0');
            ++digitCount;
        }
        else
        {
            ++decimalExp;
        }

        ++cursor;
    }

    if (cursor < length && text[cursor] == '.')
    {
        ++cursor;

        while (cursor < length && IsDigitChar(text[cursor]))
        {
            sawAnyDigit = true;

            if (digitCount < 18)
            {
                mantissa = mantissa * 10u +
                           static_cast<UInt64>(text[cursor] - '0');
                ++digitCount;
                --decimalExp;
            }

            ++cursor;
        }
    }

    if (!sawAnyDigit)
    {
        return false;
    }

    if (cursor < length && (text[cursor] == 'e' || text[cursor] == 'E'))
    {
        const SizeType exponentStart = cursor;
        ++cursor;

        bool exponentNegative = false;
        if (cursor < length && (text[cursor] == '-' || text[cursor] == '+'))
        {
            exponentNegative = (text[cursor] == '-');
            ++cursor;
        }

        if (cursor < length && IsDigitChar(text[cursor]))
        {
            Int32 explicitExponent = 0;

            while (cursor < length && IsDigitChar(text[cursor]))
            {
                if (explicitExponent < 10000)
                {
                    explicitExponent =
                        explicitExponent * 10 + (text[cursor] - '0');
                }

                ++cursor;
            }

            decimalExp += exponentNegative ? -explicitExponent
                                           : explicitExponent;
        }
        else
        {
            // 'e' 之后没有数字 — 它不属于本数字, 回退
            cursor = exponentStart;
        }
    }

    Float64 value = static_cast<Float64>(mantissa);

    if (decimalExp != 0)
    {
        if (decimalExp > 38 || decimalExp < -45)
        {
            // 超出 Float32 表示范围 — 夹紧而非产生 inf/0 的意外
            value = (decimalExp > 0) ? 3.0e38 : 0.0;
        }
        else
        {
            value *= FMath::Pow(10.0, static_cast<Float64>(decimalExp));
        }
    }

    outValue = static_cast<Float32>(isNegative ? -value : value);
    return true;
}

/// 解析一个整数 (可带符号)
bool ParseIntToken(const AnsiChar* text, SizeType length, SizeType& cursor,
                   Int32& outValue)
{
    if (cursor >= length)
    {
        return false;
    }

    bool isNegative = false;

    if (text[cursor] == '-' || text[cursor] == '+')
    {
        isNegative = (text[cursor] == '-');
        ++cursor;
    }

    if (cursor >= length || !IsDigitChar(text[cursor]))
    {
        return false;
    }

    Int64 value = 0;

    while (cursor < length && IsDigitChar(text[cursor]))
    {
        value = value * 10 + (text[cursor] - '0');

        // 索引不可能达到这个量级 — 提前夹紧避免溢出
        if (value > 0x7FFFFFFF)
        {
            value = 0x7FFFFFFF;
        }

        ++cursor;
    }

    outValue = static_cast<Int32>(isNegative ? -value : value);
    return true;
}

/// 读取一个以空白分隔的记号
FString ReadToken(const AnsiChar* text, SizeType length, SizeType& cursor)
{
    SkipSpaces(text, length, cursor);

    const SizeType start = cursor;

    while (cursor < length && !IsSpace(text[cursor]) && !IsLineEnd(text[cursor]))
    {
        ++cursor;
    }

    if (cursor == start)
    {
        return FString();
    }

    return FString(text + start, cursor - start);
}

/// 读取行内剩余全部内容并去除首尾空白 — 用于可能含空格的名称
FString ReadRestOfLine(const AnsiChar* text, SizeType length, SizeType& cursor)
{
    SkipSpaces(text, length, cursor);

    const SizeType start = cursor;
    SkipToLineEnd(text, length, cursor);

    SizeType end = cursor;
    while (end > start && IsSpace(text[end - 1]))
    {
        --end;
    }

    if (end <= start)
    {
        return FString();
    }

    return FString(text + start, end - start);
}

/// 比较记号与字面量
bool TokenEquals(const FString& token, const AnsiChar* literal)
{
    SizeType i = 0;
    const SizeType tokenLength = token.GetLength();

    while (literal[i] != '\0')
    {
        if (i >= tokenLength || token[i] != literal[i])
        {
            return false;
        }

        ++i;
    }

    return i == tokenLength;
}

/// 把路径分隔符统一为正斜杠
FString NormalizePath(const FString& path)
{
    FString result;

    for (SizeType i = 0; i < path.GetLength(); ++i)
    {
        result.AppendChar(path[i] == '\\' ? '/' : path[i]);
    }

    return result;
}

/// 拼接目录与相对路径
FString CombinePath(const FString& directory, const FString& relative)
{
    if (directory.IsEmpty())
    {
        return NormalizePath(relative);
    }

    FString normalized = NormalizePath(relative);

    // 已是绝对路径时不做拼接
    if (normalized.GetLength() >= 2 && normalized[1] == ':')
    {
        return normalized;
    }

    if (normalized.GetLength() >= 1 && normalized[0] == '/')
    {
        return normalized;
    }

    FString result = NormalizePath(directory);

    if (!result.EndsWith("/"))
    {
        result.AppendChar('/');
    }

    result.Append(normalized);
    return result;
}

/// 取路径的目录部分
FString GetDirectoryOf(const FString& path)
{
    const FString normalized = NormalizePath(path);

    SizeType lastSlash = normalized.GetLength();
    for (SizeType i = normalized.GetLength(); i > 0; --i)
    {
        if (normalized[i - 1] == '/')
        {
            lastSlash = i - 1;
            break;
        }
    }

    if (lastSlash >= normalized.GetLength())
    {
        return FString();
    }

    return normalized.Left(lastSlash);
}

// ============================================================================
// 顶点去重
// ============================================================================

/// OBJ 面顶点的索引三元组
///
/// 直接以三个源索引作为去重键，而不是比较解析出的浮点属性：
/// 索引相同必然属性相同，无需容差判断，也不受浮点表示差异影响。
struct FObjVertexKey
{
    Int32 Position = 0;
    Int32 TexCoord = 0;
    Int32 Normal   = 0;

    LIMX_NODISCARD bool operator==(const FObjVertexKey& other) const
    {
        return Position == other.Position &&
               TexCoord == other.TexCoord &&
               Normal == other.Normal;
    }
};

/// FNV-1a 混合三个索引
struct FObjVertexKeyHash
{
    LIMX_NODISCARD SizeType operator()(const FObjVertexKey& key) const
    {
        UInt64 hash = 1469598103934665603ull;

        hash = (hash ^ static_cast<UInt64>(static_cast<UInt32>(key.Position))) *
               1099511628211ull;
        hash = (hash ^ static_cast<UInt64>(static_cast<UInt32>(key.TexCoord))) *
               1099511628211ull;
        hash = (hash ^ static_cast<UInt64>(static_cast<UInt32>(key.Normal))) *
               1099511628211ull;

        return static_cast<SizeType>(hash);
    }
};

/// 把 OBJ 的 1 起始 / 负数索引解析为 0 起始的绝对下标
///
/// @param rawIndex   源文件中的索引 (可能为负)
/// @param elementCount 该类元素当前已读取的数量
/// @return 0 起始下标, 越界返回 -1
Int32 ResolveObjIndex(Int32 rawIndex, SizeType elementCount)
{
    if (rawIndex > 0)
    {
        const Int32 resolved = rawIndex - 1;
        return (static_cast<SizeType>(resolved) < elementCount) ? resolved : -1;
    }

    if (rawIndex < 0)
    {
        // 负索引相对于当前末尾: -1 表示最后一个元素
        const Int64 resolved =
            static_cast<Int64>(elementCount) + static_cast<Int64>(rawIndex);

        return (resolved >= 0) ? static_cast<Int32>(resolved) : -1;
    }

    // OBJ 索引从 1 起始, 0 非法
    return -1;
}

// ============================================================================
// 材质换算
// ============================================================================

/// Blinn-Phong 镜面指数换算为 GGX 粗糙度
///
/// 采用通行的对应关系 roughness = sqrt(2 / (Ns + 2))：
/// Ns = 0 时得到 1.0 (完全粗糙)，Ns = 1000 时约 0.045 (接近镜面)。
/// 该式来源于把 Blinn-Phong 的高光瓣宽度匹配到 GGX 的等效宽度。
Float32 SpecularExponentToRoughness(Float32 specularExponent)
{
    const Float32 clamped = FMath::Max(specularExponent, 0.0f);
    return FMath::Clamp(FMath::Sqrt(2.0f / (clamped + 2.0f)), 0.0f, 1.0f);
}

/// 解析贴图行, 跳过 -o/-s/-bm 等选项后取文件名
///
/// MTL 的贴图行形如: map_Kd -o 1 1 -s 2 2 brick.png
/// 选项以 '-' 开头且各带若干数值参数，数量因选项而异，因此采用
/// "取最后一个不以 '-' 开头且前一个记号不是选项的记号"这一稳健策略：
/// 直接扫描到行尾，保留最后一个非数值、非选项的记号作为文件名。
FString ParseTextureFileName(const AnsiChar* text, SizeType length,
                             SizeType& cursor)
{
    FString fileName;

    while (cursor < length && !IsLineEnd(text[cursor]))
    {
        const FString token = ReadToken(text, length, cursor);

        if (token.IsEmpty())
        {
            break;
        }

        // 选项标志本身跳过
        if (token[0] == '-')
        {
            continue;
        }

        // 纯数值记号是选项的参数, 不会是文件名
        bool isNumeric = true;
        for (SizeType i = 0; i < token.GetLength(); ++i)
        {
            const AnsiChar c = token[i];
            if (!IsDigitChar(c) && c != '.' && c != '-' && c != '+')
            {
                isNumeric = false;
                break;
            }
        }

        if (isNumeric)
        {
            continue;
        }

        fileName = token;
    }

    return fileName;
}

} // namespace

// ============================================================================
// MTL 解析
// ============================================================================

FAssetLoadResult FObjLoader::ParseMaterialLibrary(
    const AnsiChar* text, SizeType length,
    const FString& baseDirectory,
    FAssetScene& outScene,
    TArray<FString>& outWarnings)
{
    if (text == nullptr || length == 0)
    {
        return FAssetLoadResult::Failure(FString("MTL 内容为空"));
    }

    SizeType cursor = 0;
    UInt32   line   = 1;

    Int32 currentMaterial = -1;

    // 记录 Ns 是否出现过 — 决定是否用近似式推导粗糙度
    bool sawSpecularExponent = false;
    bool sawPbrRoughness     = false;
    bool sawPbrMetallic      = false;

    while (cursor < length)
    {
        SkipSpaces(text, length, cursor);

        if (cursor >= length)
        {
            break;
        }

        // 空行与注释
        if (text[cursor] == '\n')
        {
            ++cursor;
            ++line;
            continue;
        }

        if (text[cursor] == '#')
        {
            SkipToLineEnd(text, length, cursor);
            continue;
        }

        const FString keyword = ReadToken(text, length, cursor);

        if (TokenEquals(keyword, "newmtl"))
        {
            const FString name = ReadRestOfLine(text, length, cursor);

            FMaterialData material;
            material.Name = FName(name.GetCStr());

            // OBJ 材质默认是非金属 — 见下方 Pm 的说明
            material.MetallicFactor  = 0.0f;
            material.RoughnessFactor = 1.0f;

            currentMaterial = static_cast<Int32>(outScene.Materials.Add(material));

            sawSpecularExponent = false;
            sawPbrRoughness     = false;
            sawPbrMetallic      = false;
        }
        else if (currentMaterial < 0)
        {
            // newmtl 之前出现的属性无处安放
            outWarnings.Add(StringFormat(
                "MTL 第 {} 行: newmtl 之前出现属性 '{}', 已忽略",
                line, keyword.GetCStr()));

            SkipToLineEnd(text, length, cursor);
        }
        else
        {
            FMaterialData& material = outScene.Materials[currentMaterial];

            if (TokenEquals(keyword, "Kd"))
            {
                Float32 r = 1.0f;
                Float32 g = 1.0f;
                Float32 b = 1.0f;

                if (ParseFloatToken(text, length, cursor, r) &&
                    ParseFloatToken(text, length, cursor, g) &&
                    ParseFloatToken(text, length, cursor, b))
                {
                    material.BaseColorFactor.X = r;
                    material.BaseColorFactor.Y = g;
                    material.BaseColorFactor.Z = b;
                }
            }
            else if (TokenEquals(keyword, "Ke"))
            {
                Float32 r = 0.0f;
                Float32 g = 0.0f;
                Float32 b = 0.0f;

                if (ParseFloatToken(text, length, cursor, r) &&
                    ParseFloatToken(text, length, cursor, g) &&
                    ParseFloatToken(text, length, cursor, b))
                {
                    material.EmissiveFactor = FVector3(r, g, b);
                }
            }
            else if (TokenEquals(keyword, "Ns"))
            {
                Float32 exponent = 0.0f;
                if (ParseFloatToken(text, length, cursor, exponent))
                {
                    sawSpecularExponent = true;

                    // PBR 扩展字段优先, 未出现时才用近似
                    if (!sawPbrRoughness)
                    {
                        material.RoughnessFactor =
                            SpecularExponentToRoughness(exponent);
                    }
                }
            }
            else if (TokenEquals(keyword, "Pr"))
            {
                Float32 roughness = 1.0f;
                if (ParseFloatToken(text, length, cursor, roughness))
                {
                    // MTL 的 PBR 扩展 — 直接取值, 不做任何近似
                    material.RoughnessFactor = FMath::Clamp(roughness, 0.0f, 1.0f);
                    sawPbrRoughness = true;
                }
            }
            else if (TokenEquals(keyword, "Pm"))
            {
                Float32 metallic = 0.0f;
                if (ParseFloatToken(text, length, cursor, metallic))
                {
                    material.MetallicFactor = FMath::Clamp(metallic, 0.0f, 1.0f);
                    sawPbrMetallic = true;
                }
            }
            else if (TokenEquals(keyword, "d"))
            {
                Float32 opacity = 1.0f;
                if (ParseFloatToken(text, length, cursor, opacity))
                {
                    material.BaseColorFactor.W = FMath::Clamp(opacity, 0.0f, 1.0f);

                    if (material.BaseColorFactor.W < 1.0f)
                    {
                        material.AlphaMode = EAlphaMode::Blend;
                    }
                }
            }
            else if (TokenEquals(keyword, "Tr"))
            {
                // Tr 是 d 的补数
                Float32 transparency = 0.0f;
                if (ParseFloatToken(text, length, cursor, transparency))
                {
                    material.BaseColorFactor.W =
                        FMath::Clamp(1.0f - transparency, 0.0f, 1.0f);

                    if (material.BaseColorFactor.W < 1.0f)
                    {
                        material.AlphaMode = EAlphaMode::Blend;
                    }
                }
            }
            else if (TokenEquals(keyword, "map_Kd"))
            {
                const FString file = ParseTextureFileName(text, length, cursor);
                if (!file.IsEmpty())
                {
                    material.BaseColorTexture.Path =
                        CombinePath(baseDirectory, file);
                }
            }
            else if (TokenEquals(keyword, "map_Ke"))
            {
                const FString file = ParseTextureFileName(text, length, cursor);
                if (!file.IsEmpty())
                {
                    material.EmissiveTexture.Path =
                        CombinePath(baseDirectory, file);
                }
            }
            else if (TokenEquals(keyword, "map_Bump") ||
                     TokenEquals(keyword, "map_bump") ||
                     TokenEquals(keyword, "bump") ||
                     TokenEquals(keyword, "norm"))
            {
                const FString file = ParseTextureFileName(text, length, cursor);
                if (!file.IsEmpty())
                {
                    material.NormalTexture.Path =
                        CombinePath(baseDirectory, file);
                }
            }
            else if (TokenEquals(keyword, "map_Pr"))
            {
                const FString file = ParseTextureFileName(text, length, cursor);
                if (!file.IsEmpty())
                {
                    // PBR 扩展把粗糙度单独成图; 引擎按 glTF 约定使用
                    // 金属粗糙度合并图, 故记在同一槽位
                    material.MetallicRoughnessTexture.Path =
                        CombinePath(baseDirectory, file);
                }
            }
            else if (TokenEquals(keyword, "map_Pm"))
            {
                const FString file = ParseTextureFileName(text, length, cursor);
                if (!file.IsEmpty() &&
                    material.MetallicRoughnessTexture.Path.IsEmpty())
                {
                    material.MetallicRoughnessTexture.Path =
                        CombinePath(baseDirectory, file);
                }
            }
            else if (TokenEquals(keyword, "map_d"))
            {
                const FString file = ParseTextureFileName(text, length, cursor);
                if (!file.IsEmpty())
                {
                    // 独立的不透明度贴图 — 引擎从基色 alpha 取值,
                    // 此处仅标记材质需要混合
                    material.AlphaMode = EAlphaMode::Mask;
                }
            }

            SkipToLineEnd(text, length, cursor);
        }

        // 越过行尾换行
        if (cursor < length && text[cursor] == '\n')
        {
            ++cursor;
            ++line;
        }
    }

    LIMX_UNUSED(sawSpecularExponent);
    LIMX_UNUSED(sawPbrMetallic);

    return FAssetLoadResult::Success();
}

// ============================================================================
// OBJ 解析
// ============================================================================

FAssetLoadResult FObjLoader::LoadFromMemory(const AnsiChar* text,
                                            SizeType length,
                                            const FString& baseDirectory,
                                            FAssetScene& outScene,
                                            const FObjLoadOptions& options)
{
    outScene.Reset();
    outScene.BaseDirectory = NormalizePath(baseDirectory);

    if (text == nullptr || length == 0)
    {
        return FAssetLoadResult::Failure(FString("OBJ 内容为空"));
    }

    TArray<FString> warnings;

    // 源数据的三类属性数组
    TArray<FVector3> positions;
    TArray<FVector3> normals;
    TArray<FVector2> texCoords;

    // 输出网格 — OBJ 的全部几何合并为一个网格, 按材质切分子网格
    FMeshData mesh;
    mesh.Name = FName("ObjMesh");

    TMap<FObjVertexKey, UInt32, FObjVertexKeyHash> vertexLookup;

    // 当前材质与当前子网格的起始索引
    Int32  currentMaterial     = -1;
    UInt32 currentSubMeshStart = 0;
    FName  currentGroupName    = FName("default");

    // 面顶点缓冲 — 每行复用, 避免逐面分配
    TArray<UInt32> faceVertices;

    SizeType cursor = 0;
    UInt32   line   = 1;

    // 提交当前累积的索引为一个子网格
    auto FlushSubMesh = [&]()
    {
        const UInt32 indexCount =
            static_cast<UInt32>(mesh.Indices.GetSize()) - currentSubMeshStart;

        if (indexCount == 0)
        {
            return;
        }

        FSubMesh subMesh;
        subMesh.Name          = currentGroupName;
        subMesh.IndexOffset   = currentSubMeshStart;
        subMesh.IndexCount    = indexCount;
        subMesh.MaterialIndex = currentMaterial;

        mesh.SubMeshes.Add(subMesh);

        currentSubMeshStart = static_cast<UInt32>(mesh.Indices.GetSize());
    };

    while (cursor < length)
    {
        SkipSpaces(text, length, cursor);

        if (cursor >= length)
        {
            break;
        }

        if (text[cursor] == '\n')
        {
            ++cursor;
            ++line;
            continue;
        }

        if (text[cursor] == '#')
        {
            SkipToLineEnd(text, length, cursor);
            continue;
        }

        const FString keyword = ReadToken(text, length, cursor);

        // ---------------------------------------------------------------
        // 顶点位置
        // ---------------------------------------------------------------
        if (TokenEquals(keyword, "v"))
        {
            Float32 x = 0.0f;
            Float32 y = 0.0f;
            Float32 z = 0.0f;

            if (ParseFloatToken(text, length, cursor, x) &&
                ParseFloatToken(text, length, cursor, y) &&
                ParseFloatToken(text, length, cursor, z))
            {
                positions.Add(FVector3(x, y, z));
            }
            else
            {
                warnings.Add(StringFormat("OBJ 第 {} 行: 顶点坐标不完整", line));
            }
        }
        // ---------------------------------------------------------------
        // 法线
        // ---------------------------------------------------------------
        else if (TokenEquals(keyword, "vn"))
        {
            Float32 x = 0.0f;
            Float32 y = 0.0f;
            Float32 z = 0.0f;

            if (ParseFloatToken(text, length, cursor, x) &&
                ParseFloatToken(text, length, cursor, y) &&
                ParseFloatToken(text, length, cursor, z))
            {
                normals.Add(FVector3(x, y, z));
            }
            else
            {
                warnings.Add(StringFormat("OBJ 第 {} 行: 法线分量不完整", line));
            }
        }
        // ---------------------------------------------------------------
        // 纹理坐标
        // ---------------------------------------------------------------
        else if (TokenEquals(keyword, "vt"))
        {
            Float32 u = 0.0f;
            Float32 v = 0.0f;

            if (ParseFloatToken(text, length, cursor, u))
            {
                // v 分量可省略 (一维纹理)
                if (!ParseFloatToken(text, length, cursor, v))
                {
                    v = 0.0f;
                }

                // OBJ 的 UV 原点在左下, Vulkan 图像原点在左上
                texCoords.Add(FVector2(u, options.FlipTexCoordV ? (1.0f - v) : v));
            }
            else
            {
                warnings.Add(StringFormat("OBJ 第 {} 行: 纹理坐标不完整", line));
            }
        }
        // ---------------------------------------------------------------
        // 面
        // ---------------------------------------------------------------
        else if (TokenEquals(keyword, "f"))
        {
            faceVertices.Clear();

            while (true)
            {
                SkipSpaces(text, length, cursor);

                if (cursor >= length || IsLineEnd(text[cursor]))
                {
                    break;
                }

                // 解析 v[/vt][/vn]
                Int32 positionIndex = 0;
                Int32 texCoordIndex = 0;
                Int32 normalIndex   = 0;

                if (!ParseIntToken(text, length, cursor, positionIndex))
                {
                    break;
                }

                if (cursor < length && text[cursor] == '/')
                {
                    ++cursor;

                    // v//vn 形式: 中间的 vt 为空
                    if (cursor < length && text[cursor] != '/')
                    {
                        ParseIntToken(text, length, cursor, texCoordIndex);
                    }

                    if (cursor < length && text[cursor] == '/')
                    {
                        ++cursor;
                        ParseIntToken(text, length, cursor, normalIndex);
                    }
                }

                const Int32 resolvedPosition =
                    ResolveObjIndex(positionIndex, positions.GetSize());

                if (resolvedPosition < 0)
                {
                    warnings.Add(StringFormat(
                        "OBJ 第 {} 行: 顶点索引 {} 越界", line, positionIndex));
                    continue;
                }

                const Int32 resolvedTexCoord =
                    (texCoordIndex != 0)
                        ? ResolveObjIndex(texCoordIndex, texCoords.GetSize())
                        : -1;

                const Int32 resolvedNormal =
                    (normalIndex != 0)
                        ? ResolveObjIndex(normalIndex, normals.GetSize())
                        : -1;

                // 折叠为唯一顶点
                FObjVertexKey key;
                key.Position = resolvedPosition;
                key.TexCoord = resolvedTexCoord;
                key.Normal   = resolvedNormal;

                UInt32 vertexIndex = 0;

                if (const UInt32* existing = vertexLookup.Find(key))
                {
                    vertexIndex = *existing;
                }
                else
                {
                    FMeshVertex vertex;
                    vertex.Position = positions[resolvedPosition];

                    if (resolvedTexCoord >= 0)
                    {
                        vertex.TexCoord0 = texCoords[resolvedTexCoord];
                        mesh.HasTexCoords = true;
                    }

                    if (resolvedNormal >= 0)
                    {
                        vertex.Normal = normals[resolvedNormal];
                        mesh.HasNormals = true;
                    }

                    vertexIndex = static_cast<UInt32>(mesh.Vertices.Add(vertex));
                    vertexLookup.Add(key, vertexIndex);
                }

                faceVertices.Add(vertexIndex);
            }

            // 扇形三角化 — n 边形产出 n-2 个三角形
            if (faceVertices.GetSize() >= 3)
            {
                for (SizeType i = 1; i + 1 < faceVertices.GetSize(); ++i)
                {
                    mesh.Indices.Add(faceVertices[0]);
                    mesh.Indices.Add(faceVertices[i]);
                    mesh.Indices.Add(faceVertices[i + 1]);
                }
            }
            else if (faceVertices.GetSize() > 0)
            {
                warnings.Add(StringFormat(
                    "OBJ 第 {} 行: 面只有 {} 个顶点, 已跳过",
                    line, faceVertices.GetSize()));
            }
        }
        // ---------------------------------------------------------------
        // 材质切换
        // ---------------------------------------------------------------
        else if (TokenEquals(keyword, "usemtl"))
        {
            const FString name = ReadRestOfLine(text, length, cursor);

            // 切换材质前先把已累积的索引封成一个子网格
            FlushSubMesh();

            currentMaterial = -1;

            for (SizeType i = 0; i < outScene.Materials.GetSize(); ++i)
            {
                if (outScene.Materials[i].Name == name.GetCStr())
                {
                    currentMaterial = static_cast<Int32>(i);
                    break;
                }
            }

            if (currentMaterial < 0 && !name.IsEmpty())
            {
                warnings.Add(StringFormat(
                    "OBJ 第 {} 行: 未找到材质 '{}', 将使用默认材质",
                    line, name.GetCStr()));
            }
        }
        // ---------------------------------------------------------------
        // 材质库
        // ---------------------------------------------------------------
        else if (TokenEquals(keyword, "mtllib"))
        {
            const FString libraryName = ReadRestOfLine(text, length, cursor);

            if (options.LoadMaterialLibrary && !libraryName.IsEmpty())
            {
                const FString libraryPath =
                    CombinePath(outScene.BaseDirectory, libraryName);

                const FString mtlText = FPlatformFile::ReadAllText(libraryPath);

                if (mtlText.IsEmpty())
                {
                    warnings.Add(StringFormat(
                        "OBJ 第 {} 行: 无法读取材质库 '{}'",
                        line, libraryPath.GetCStr()));
                }
                else
                {
                    const FAssetLoadResult mtlResult = ParseMaterialLibrary(
                        mtlText.GetCStr(), mtlText.GetLength(),
                        outScene.BaseDirectory, outScene, warnings);

                    if (!mtlResult.Succeeded)
                    {
                        warnings.Add(StringFormat(
                            "材质库 '{}' 解析失败: {}",
                            libraryPath.GetCStr(),
                            mtlResult.ErrorMessage.GetCStr()));
                    }
                }
            }
        }
        // ---------------------------------------------------------------
        // 组 / 对象
        // ---------------------------------------------------------------
        else if (TokenEquals(keyword, "o") || TokenEquals(keyword, "g"))
        {
            const FString name = ReadRestOfLine(text, length, cursor);

            if (options.SplitByGroup)
            {
                FlushSubMesh();
            }

            currentGroupName = FName(name.IsEmpty() ? "default" : name.GetCStr());
        }
        // ---------------------------------------------------------------
        // 已知但不支持的记录 — 明确跳过并告警, 避免被当作未知记录反复报警
        // ---------------------------------------------------------------
        else if (TokenEquals(keyword, "p") || TokenEquals(keyword, "l") ||
                 TokenEquals(keyword, "curv") || TokenEquals(keyword, "surf"))
        {
            warnings.Add(StringFormat(
                "OBJ 第 {} 行: 不支持的图元类型 '{}', 已跳过",
                line, keyword.GetCStr()));
        }

        SkipToLineEnd(text, length, cursor);

        if (cursor < length && text[cursor] == '\n')
        {
            ++cursor;
            ++line;
        }
    }

    // 收尾: 提交最后一段索引
    FlushSubMesh();

    if (mesh.Vertices.GetSize() == 0 || mesh.Indices.GetSize() == 0)
    {
        FAssetLoadResult result =
            FAssetLoadResult::Failure(FString("OBJ 未包含任何有效几何"));
        result.Warnings = static_cast<TArray<FString>&&>(warnings);
        return result;
    }

    // ------------------------------------------------------------------
    // 补齐缺失属性
    // ------------------------------------------------------------------

    if (!mesh.HasNormals && options.GenerateMissingNormals)
    {
        mesh.GenerateNormals();
    }

    if (mesh.HasTexCoords && options.GenerateTangents)
    {
        mesh.GenerateTangents();
    }

    mesh.RecomputeBounds();

    outScene.Name = FName("ObjScene");
    outScene.Meshes.Add(static_cast<FMeshData&&>(mesh));

    // 单网格场景 — 建一个根节点承载它
    FSceneNode node;
    node.Name        = FName("ObjRoot");
    node.MeshIndex   = 0;
    node.ParentIndex = -1;

    outScene.Nodes.Add(node);
    outScene.RootNodes.Add(0);

    outScene.RecomputeBounds();

    FAssetLoadResult result = FAssetLoadResult::Success();
    result.Warnings = static_cast<TArray<FString>&&>(warnings);

    return result;
}

FAssetLoadResult FObjLoader::LoadFromFile(const FString& path,
                                          FAssetScene& outScene,
                                          const FObjLoadOptions& options)
{
    const FString text = FPlatformFile::ReadAllText(path);

    if (text.IsEmpty())
    {
        return FAssetLoadResult::Failure(
            StringFormat("无法读取 OBJ 文件: {}", path.GetCStr()));
    }

    return LoadFromMemory(text.GetCStr(), text.GetLength(),
                          GetDirectoryOf(path), outScene, options);
}

} // namespace Limx
