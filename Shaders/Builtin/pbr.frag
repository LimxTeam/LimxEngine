#version 450

// 非一致索引 —— 目前材质下标来自 push constant, 在一次绘制内是一致的,
// 严格说不需要这个限定符。但等 GPU 驱动渲染落地之后, 下标会来自实例
// 数据, 那时同一次绘制里不同的图元会取不同的材质 —— 届时它是必需的。
// 现在就写上, 免得那时改着色器又要重新验一遍画面。
#extension GL_EXT_nonuniform_qualifier : require

// ============================================================
// PBR 片段着色器 — Cook-Torrance 微表面 BRDF + 多光源
//
// BRDF 模型:
//   D: GGX/Trowbridge-Reitz 法线分布函数
//   G: Smith-GGX 几何遮蔽函数 (Schlick 近似)
//   F: Fresnel-Schlick 近似
//
// 光源类型:
//   0 = 方向光 (无衰减)
//   1 = 点光源 (距离衰减)
//   2 = 聚光灯 (距离衰减 + 锥角衰减)
//
// 描述符集:
//   set 0, binding 0: ViewProjUBO
//   set 1, binding 0: MaterialUBO
//   set 1, binding 1~5: Albedo/Normal/MetallicRoughness/Occlusion/Emissive
//   set 2, binding 0: FLightingUBO (光照数据)
// ============================================================

// ── 片段着色器输入 (来自顶点着色器) ──
// 与 pbr.vert 里的声明必须逐字段一致 —— push constant 的布局在整条管线上
// 是共享的, 两边不一致时偏移会错开。
layout(row_major, push_constant) uniform PushConstants {
    mat4 model;
    uint materialIndex;
} pc;

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragWorldNormal;
layout(location = 2) in vec3 fragColor;
layout(location = 3) in vec2 fragTexCoord;
layout(location = 4) in vec4 fragWorldTangent;   // xyz = 切线, w = 手性 (±1)
layout(location = 5) in float fragViewDepth;     // 视空间深度 (正值)

// ── 材质参数 (set 1, binding 0) — 必须匹配 FMaterialParams std140 布局 ──
// ── 材质表 (set 1) ──
//
// 字段顺序与 C++ 侧 FBindlessMaterial 逐字段对应, 共 80 字节。std430 下
// vec4 按 16 对齐、标量按 4 —— 两个 vec4 各自起于 16 的倍数, 中间的标量
// 凑满一组。改动顺序前先算对齐: 错位的表现是"材质参数整体串了一位",
// 画面上像是材质配错而不像 bug。
//
// C++ 侧有 static_assert 钉住 80 字节, 但那只保证 C++ 这一边 —— 两边的
// 字段顺序是否一致仍然只能靠人对。
struct MaterialData {
    vec4  BaseColor;                //  0
    float Metallic;                 // 16
    float Roughness;
    float AO;
    float NormalScale;
    vec4  EmissiveColor;            // 32
    float AlphaCutoff;              // 48
    uint  TextureFlags;
    uint  BlendMode;
    uint  AlbedoIndex;
    uint  NormalIndex;              // 64
    uint  MetallicRoughnessIndex;
    uint  OcclusionIndex;
    uint  EmissiveIndex;
};                                  // 80

layout(std430, set = 1, binding = 0) readonly buffer MaterialBuffer {
    MaterialData materials[];
};

// 全局纹理表。
//
// 数组大小必须与 C++ 侧 FBindlessTable::kMaxTextures 一致。用固定大小而
// 非无界数组 (sampler2D[]), 是为了不依赖 runtimeDescriptorArray 特性 ——
// 少一项特性要求就少一类"这台机器上跑不起来"。
//
// 数组是稀疏的: 声明 1024 槽而场景只用几十个。未写入的槽位不能被索引,
// 所以缺贴图的材质在 C++ 侧就被指向 0 号占位纹理, 着色器这里不做任何
// 有效性判断 —— 判断一旦漏写就是随机读显存。
layout(set = 1, binding = 1) uniform sampler2D bindlessTextures[1024];

// 本次绘制的材质。
//
// 全局变量而非 main 里的局部量: 下面若干个辅助函数直接引用 material,
// 改成局部量要给每个函数加参数。GLSL 的全局变量是逐调用存储, 语义上
// 与局部量一致。
MaterialData material;

// 采样全局表里的第 index 张贴图
vec4 SampleBindless(uint index, vec2 uv)
{
    return texture(bindlessTextures[nonuniformEXT(index)], uv);
}

// ── 光源数据结构 (std140, 80 bytes per light) ──
struct LightData {
    vec4 positionAndType;      // xyz=位置, w=类型
    vec4 directionAndRange;    // xyz=方向, w=衰减距离
    vec4 colorAndIntensity;    // xyz=颜色, w=强度
    vec4 attenuationParams;    // x=常量, y=线性, z=二次, w=保留
    vec4 spotParams;           // x=内锥角余弦, y=外锥角余弦, z=阴影块下标
};

// ── 光照 UBO (set 2, binding 0) ──
//
// row_major 与 FMatrix 的行主序存储一致 —— 不加的话 GLSL 按列主序解读,
// 等于把阴影矩阵整体转置, 阴影坐标会落到完全无关的位置。
layout(row_major, set = 2, binding = 0) uniform LightingUBO {
    vec4      lightCountVec;   // x=光源数量, y=其中方向光的数量
    vec4      cameraPosition;  // xyz=相机世界位置
    vec4      ambientColor;    // xyz=环境光颜色, w=环境光强度
    mat4      cascadeViewProj[3];  // 各级联的视图投影矩阵
    vec4      cascadeSplits;         // xyz=各级外边界的径向距离
    vec4      shadowParams;          // x=深度偏移 y=法线偏移 z=贴图边长 w=启用
    vec4      iblParams;             // x=启用 IBL, y=强度, z=预滤波最高 mip
    vec4      clusterParams;         // x=切片 Scale, y=切片 Bias, z/w=视口尺寸
    vec4      clusterConfig;         // x=分簇是否启用
} lighting;

// ── 光源数组 (set 2, binding 5) ──
//
// 从 UBO 挪进 storage buffer 的理由有两条: UBO 的保证上限是 65536 字节,
// 1024 盏 × 80 = 81920 已经超了; 而且分簇剔除的计算着色器要读同一份数据,
// 它产出的簇索引表必须是 storage buffer (要原子写入), 两者同一种缓冲区
// 就少一套绑定与屏障。
//
// std430 而非 std140: 这个结构全是 vec4, 两种布局给出的偏移完全相同
// (80 字节本就是 16 的倍数), 但 std430 对将来加标量成员更宽容 —— std140
// 会把数组元素补齐到 16 的倍数, 那种补齐 C++ 侧不会自动跟上。
//
// readonly 不是装饰: 它让驱动知道不必为这个绑定准备写入路径, 而且写错时
// 编译期就报错, 而不是运行时静默污染光源数据。
layout(std430, set = 2, binding = 5) readonly buffer LightBuffer {
    LightData lights[];
} lightBuffer;

// ── 分簇的产出 (set 2, binding 6/7) ──
//
// 由 FClusterLightPass 的两个计算着色器每帧写入。grid[i] 给出簇 i 在索引表
// 里的 (起点, 数量), indices 是全局的光源下标表。
layout(std430, set = 2, binding = 6) readonly buffer ClusterGrid {
    uvec2 grid[];
} clusterGrid;

layout(std430, set = 2, binding = 7) readonly buffer ClusterLightIndices {
    uint indices[];
} clusterIndices;

// ── 屏幕空间环境光遮蔽 (set 2, binding 8) ──
//
// 由 FGtaoPass 每帧写入。关闭 GTAO 时那张图被清成 1, 于是这里不需要任何
// 开关 —— 少一个 uniform、少一个分支。更要紧的是: 有分支的话"GTAO 通道
// 没跑"与"AO 恰好全是 1"在画面上无法区分, 而前者是缺陷。
layout(set = 2, binding = 8) uniform sampler2D ambientOcclusion;

#include "cluster_common.h"

// ── 阴影贴图数组 (set 2, binding 1) ──
//
// sampler2DArrayShadow 而非 sampler2DArray: 采样时硬件直接做深度比较并在
// 2x2 邻域上求平均, 返回 0~1 的通过比例。用普通采样器手写比较的话, 拿到的
// 是四个纹素线性插值后的**深度值**, 再与参考深度比较 —— 那等于在深度域
// 里插值, 边缘会出现明显的阶梯。
layout(set = 2, binding = 1) uniform sampler2DArrayShadow shadowMap;

// ── 漫反射辐照度立方体贴图 (set 2, binding 2) ──
//
// 存的是"法线朝向 n 的表面从整个上半球收到的总辐照度", 即环境图与余弦瓣的
// 卷积。着色时按法线查一次即可, 不必在这里做积分。
//
// 没有环境贴图时这里绑的是一张 1x1 黑图, 由 iblParams.x 决定是否采用 ——
// 描述符必须始终有效, 靠分支跳过采样并不能免除这一点。
layout(set = 2, binding = 2) uniform samplerCube irradianceMap;

// ── 镜面预滤波立方体贴图 (set 2, binding 3) ──
//
// mip 级对应粗糙度: 0 是完全光滑, 最后一级最粗糙。采样时用
// roughness * iblParams.z 换算 LOD, 硬件的三线性过滤负责级间插值。
layout(set = 2, binding = 3) uniform samplerCube prefilteredMap;

// ── 环境 BRDF 查找表 (set 2, binding 4) ──
//
// R = F0 的系数, G = 常数偏置。它与环境、与材质都无关, 只依赖
// (n·v, 粗糙度) —— 因此是一张算一次就能一直用的常量表。
layout(set = 2, binding = 4) uniform sampler2D brdfLut;

// ── 聚光灯阴影的每块数据 (set 2, binding 9) ──
//
// UV 变换随矩阵一起上传, 而不是让这里按块下标现算。现算意味着着色器要
// 复制一份图集布局的知识 (每行几块、块多大), 而那份复制品无法被单元测试
// 覆盖 —— 它与 C++ 侧漂移时的表现是"阴影取到了隔壁灯那一块", 画面上看是
// 形状完全不对的影子, 而不是没有影子。
//
// row_major 与 FMatrix 的行主序一致。漏掉的话矩阵整体转置, 阴影坐标会落到
// 完全无关的位置 —— 这个坑在分簇的两个计算着色器上已经踩过一次。
struct SpotShadowData {
    mat4 viewProj;
    vec4 uvTransform;   // xy=块偏移, zw=块缩放
    vec4 params;        // x=深度偏移, y=图集边长
};

layout(row_major, std430, set = 2, binding = 9) readonly buffer SpotShadowBuffer {
    SpotShadowData spotShadows[];
} spotShadowBuffer;

// ── 聚光灯阴影图集 (set 2, binding 10) ──
//
// sampler2DShadow 而非 sampler2DArrayShadow: 图集是一张 2D 纹理, 块靠 UV
// 偏移区分, 不是数组层。
layout(set = 2, binding = 10) uniform sampler2DShadow spotShadowAtlas;

const int SHADOW_CASCADE_COUNT = 3;

// ── 片段着色器输出 ──
layout(location = 0) out vec4 outColor;

// ── 常量 ──
const float PI = 3.14159265359;
const uint  TEX_ALBEDO             = 1u << 0;
const uint  TEX_NORMAL             = 1u << 1;
const uint  TEX_METALLIC_ROUGHNESS = 1u << 2;
const uint  TEX_OCCLUSION          = 1u << 3;
const uint  TEX_EMISSIVE           = 1u << 4;
// 法线贴图只存了 RG 两个通道 (BC5), Z 要重建 —— 与 C++ 侧的
// kMaterialTexFlagNormalTwoChannel 以及 material_common.h 里的
// TEX_NORMAL_TWO_CHANNEL 对应
const uint  TEX_NORMAL_TWO_CHANNEL = 1u << 5;
const uint  BLEND_MASKED           = 1u;

// ============================================================
// GGX/Trowbridge-Reitz 法线分布函数
// D(h) = α² / (π * ((n·h)² * (α²-1) + 1)²)
// ============================================================
float DistributionGGX(vec3 N, vec3 H, float roughness)
{
    float a  = roughness * roughness;
    float a2 = a * a;
    float NdotH  = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;

    float denominator = NdotH2 * (a2 - 1.0) + 1.0;
    denominator = PI * denominator * denominator;

    return a2 / max(denominator, 0.0000001);
}

// ============================================================
// Smith-GGX 几何遮蔽函数 (Schlick 近似)
// G_SchlickGGX(n,v,k) = (n·v) / ((n·v)(1-k) + k)
// k_direct = (α+1)² / 8
// G_Smith = G1(n,v,k) * G1(n,l,k)
// ============================================================
float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;

    float denominator = NdotV * (1.0 - k) + k;
    return NdotV / max(denominator, 0.0000001);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx1  = GeometrySchlickGGX(NdotV, roughness);
    float ggx2  = GeometrySchlickGGX(NdotL, roughness);
    return ggx1 * ggx2;
}

// ============================================================
// Fresnel-Schlick 近似
// F(h,v,F0) = F0 + (1-F0) * (1 - h·v)^5
// ============================================================
vec3 FresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// 微表面间多次散射的能量补偿
//
// 单次散射的 GGX 只统计"入射 → 一次反弹 → 出射"的光。粗糙表面的微平面
// 互相遮挡, 被挡住的那部分光其实会在微平面之间继续弹射, 最终仍有相当一部分
// 射出去 —— 单次散射模型把它们全当作被吸收了。
//
// 白炉测试给出的实测: 粗糙度 1 的金属只反射回 31% 的入射能量, 丢掉的
// 69% 全是这些多次弹射。而画面上它只表现为"粗糙金属偏暗", 看着像
// 材质参数没调好。
//
// 这里用 Fdez-Agüera (2019) 的近似: 丢失的能量 Ems = 1-(A+B) 按平均
// 菲涅尔 F_avg 做等比级数求和补回。它不需要任何额外贴图 —— 所需的
// A、B 就是已有的 BRDF 查找表。
//
// F_avg 的 1/21 来自 Schlick 菲涅尔在半球上的解析平均:
//   ∫F(θ)cosθ dω / π = F0 + (1-F0)/21
struct FEnergyTerms
{
    vec3 SingleScatter;   // FssEss —— 单次散射的镜面权重
    vec3 MultiScatter;    // FmsEms —— 多次弹射补回的能量
    vec3 Diffuse;         // kD    —— 剩给漫反射的能量
};

FEnergyTerms ComputeEnergyTerms(vec2 envBrdf, vec3 F0, vec3 diffuseColor)
{
    FEnergyTerms terms;

    terms.SingleScatter = F0 * envBrdf.x + envBrdf.y;

    // 单次散射漏掉的比例
    float Ems = 1.0 - (envBrdf.x + envBrdf.y);

    vec3 F_avg = F0 + (1.0 - F0) / 21.0;

    // 等比级数 Σ (F_avg·Ems)^n 的闭式解
    terms.MultiScatter = Ems * terms.SingleScatter * F_avg /
                         (1.0 - F_avg * Ems);

    // 漫反射拿走镜面 (含补偿) 之后剩下的那份 —— 这样三项加起来恰好是
    // 入射能量, 而不是各自独立地取一个"看着差不多"的系数
    terms.Diffuse = diffuseColor *
                    (1.0 - terms.SingleScatter - terms.MultiScatter);

    return terms;
}

// ============================================================
// 计算光源衰减
// ============================================================
float CalcAttenuation(LightData light, vec3 fragPos)
{
    int lightType = int(light.positionAndType.w);

    // 方向光 — 无衰减
    if (lightType == 0)
    {
        return 1.0;
    }

    // 点光/聚光灯 — 距离衰减
    vec3  lightPos  = light.positionAndType.xyz;
    float dist      = length(lightPos - fragPos);
    float maxRange  = light.directionAndRange.w;

    // 超出最大距离直接截断
    if (dist > maxRange)
    {
        return 0.0;
    }

    float attConst  = light.attenuationParams.x;
    float attLinear = light.attenuationParams.y;
    float attQuad   = light.attenuationParams.z;

    float attenuation = 1.0 / (attConst + attLinear * dist + attQuad * dist * dist);

    // 平滑距离截断 — 在最大距离处平滑衰减到零
    float distRatio    = dist / maxRange;
    float smoothFactor = clamp(1.0 - distRatio * distRatio, 0.0, 1.0);
    smoothFactor       = smoothFactor * smoothFactor;

    attenuation *= smoothFactor;

    // 聚光灯 — 额外锥角衰减
    if (lightType == 2)
    {
        vec3  spotDir    = normalize(light.directionAndRange.xyz);
        vec3  fragToLight = normalize(lightPos - fragPos);
        float theta      = dot(fragToLight, -spotDir);
        float innerCos   = light.spotParams.x;
        float outerCos   = light.spotParams.y;
        float epsilon    = innerCos - outerCos;

        // 平滑锥角过渡
        float spotIntensity = clamp(
            (theta - outerCos) / max(epsilon, 0.0001), 0.0, 1.0);

        attenuation *= spotIntensity;
    }

    return attenuation;
}

// ============================================================
// 获取光照方向 (从片段指向光源)
// ============================================================
vec3 GetLightDirection(LightData light, vec3 fragPos)
{
    int lightType = int(light.positionAndType.w);

    if (lightType == 0)
    {
        // 方向光 — 方向取反 (FLightData 存储的是光照射出的方向)
        return normalize(-light.directionAndRange.xyz);
    }
    else
    {
        // 点光/聚光灯 — 从片段指向光源位置
        return normalize(light.positionAndType.xyz - fragPos);
    }
}

// ============================================================
// 应用法线贴图
//
// 优先使用顶点切线 —— 屏幕导数构造的 TBN 在 UV 接缝与掠射角处会抖动,
// 且每片段要付 4 次导数运算。顶点切线由资产管线提供 (glTF 直接给出,
// OBJ 由解析器按三角形 UV 梯度生成), 手性存在 w 分量。
// w == 0 表示该网格没有有效切线, 此时才退回屏幕导数。
// ============================================================
vec3 ApplyNormalMap(vec3 baseNormal)
{
    if ((material.TextureFlags & TEX_NORMAL) == 0u)
    {
        return baseNormal;
    }

    // ── 切线空间法线的解码 ──
    //
    // 两条路径, 由 TEX_NORMAL_TWO_CHANNEL 区分, **不能合并成一条**:
    //
    //   BC5 —— 文件里只有 RG。采样得到的 z 恒为 0, 换算后是 -1, 直接
    //     拿来用会让整个表面的法线朝里。必须用 z = sqrt(1 - x² - y²)
    //     重建 —— 单位球面的约束使这个式子在切线空间下有唯一解 (z 取正)。
    //
    //   RGB —— 文件里存了 Z, 此时必须**用存的那个值**。看上去重建也能
    //     得到差不多的结果, 但两者在两种情况下会分道扬镳: 作者刻意压平
    //     过法线 (存的是非单位向量, 用来削弱凹凸感), 或贴图经压缩与 mip
    //     过滤之后长度不再为 1。重建会把这些统统抹平, 而抹平的表现只是
    //     "法线贴图的强度对不上", 不会有任何报错。glTF 规范也明确要求
    //     使用存储值。
    //
    // NormalScale 在两条路径上都只作用于 XY, 与 glTF 的
    // normalize((<sampled> * 2 - 1) * vec3(scale, scale, 1)) 一致。
    // 对 BC5 而言顺序尤其要紧: 缩放必须在重建 Z **之前**做, 否则
    // 用来求 Z 的那个 XY 已经不是原始值了。
    vec4 normalSample = SampleBindless(material.NormalIndex, fragTexCoord);

    vec3 tangentNormal;
    tangentNormal.xy =
        (normalSample.xy * 2.0 - 1.0) * max(material.NormalScale, 0.0);

    if ((material.TextureFlags & TEX_NORMAL_TWO_CHANNEL) != 0u)
    {
        tangentNormal.z =
            sqrt(max(0.0, 1.0 - dot(tangentNormal.xy, tangentNormal.xy)));
    }
    else
    {
        tangentNormal.z = normalSample.z * 2.0 - 1.0;
    }

    vec3  T;
    vec3  B;

    if (abs(fragWorldTangent.w) > 0.5 &&
        dot(fragWorldTangent.xyz, fragWorldTangent.xyz) > 0.0000001)
    {
        // Gram-Schmidt 再正交化 —— 插值后的切线不再严格垂直于法线
        T = normalize(fragWorldTangent.xyz);
        T = normalize(T - baseNormal * dot(baseNormal, T));
        B = cross(baseNormal, T) * fragWorldTangent.w;
    }
    else
    {
        vec3  dp1  = dFdx(fragWorldPos);
        vec3  dp2  = dFdy(fragWorldPos);
        vec2  duv1 = dFdx(fragTexCoord);
        vec2  duv2 = dFdy(fragTexCoord);
        float det  = duv1.x * duv2.y - duv2.x * duv1.y;

        if (abs(det) < 0.0000001)
        {
            return baseNormal;
        }

        T = normalize((dp1 * duv2.y - dp2 * duv1.y) / det);
        T = normalize(T - baseNormal * dot(baseNormal, T));
        B = normalize(cross(baseNormal, T)) * sign(det);
    }

    return normalize(mat3(T, B, baseNormal) * tangentNormal);
}

// ============================================================
// 阴影因子 — 1.0 表示完全受光, 0.0 表示完全在阴影中
//
// 两个偏移量分工不同:
//   法线偏移沿表面法线把采样点推出去, 在掠射角下效果最好 —— 那里深度
//   梯度极大, 单靠深度偏移要么不够要么大到让阴影脱离物体(peter-panning)。
//   深度偏移按 N·L 缩放, 正对光源时几乎为零, 掠射时增大。
//
// 3x3 PCF 叠在硬件的 2x2 之上, 等效 6x6 的滤波足迹。
// ============================================================
float ComputeShadow(vec3 worldPos, vec3 normal, vec3 lightDir)
{
    if (lighting.shadowParams.w < 0.5)
    {
        return 1.0;
    }

    float depthBias  = lighting.shadowParams.x;
    float normalBias = lighting.shadowParams.y;
    float mapSize    = max(lighting.shadowParams.z, 1.0);

    // ---- 选级 ----
    //
    // 按到相机的径向距离而非视空间 Z: 级联体积是按包围球拟合的, 球以径向
    // 距离定义。两者口径不一致时, 切片角落会选到未覆盖该处的级别,
    // 表现为视野边缘出现一圈错误阴影。
    float viewDistance = length(worldPos - lighting.cameraPosition.xyz);

    int cascade = SHADOW_CASCADE_COUNT - 1;

    if (viewDistance < lighting.cascadeSplits.x)
    {
        cascade = 0;
    }
    else if (viewDistance < lighting.cascadeSplits.y)
    {
        cascade = 1;
    }

    // 超出最后一级的覆盖范围 —— 判为无遮挡, 而不是硬套最后一级。
    // 硬套的话, 远处会采到贴图边界外的值, 呈现为一道贯穿地平线的假阴影。
    if (viewDistance > lighting.cascadeSplits.z)
    {
        return 1.0;
    }

    float NdotL = clamp(dot(normal, lightDir), 0.0, 1.0);

    // 法线偏移随级别放大 —— 每级的纹素世界尺寸约按切分比例增长,
    // 用同一个偏移量的话, 远处那级仍会有 acne。
    float cascadeScale = 1.0;
    if (cascade == 1) { cascadeScale = 2.5; }
    else if (cascade == 2) { cascadeScale = 6.0; }

    vec3 offsetPos = worldPos +
                     normal * (normalBias * cascadeScale * (1.0 - NdotL * 0.5));

    vec4 shadowClip = lighting.cascadeViewProj[cascade] * vec4(offsetPos, 1.0);

    // 正交投影下 w 恒为 1, 但保留除法以便将来换成透视光源
    vec3 projected = shadowClip.xyz / shadowClip.w;

    // XY 从 NDC [-1,1] 映射到纹理坐标 [0,1]; Z 在 Vulkan 下已是 [0,1]
    vec2 shadowUV = projected.xy * 0.5 + 0.5;
    float receiverDepth = projected.z;

    if (receiverDepth > 1.0 || receiverDepth < 0.0)
    {
        return 1.0;
    }

    // 深度偏移随掠射角增大 —— tan(acos(NdotL)) 的廉价近似
    float slopeScale = clamp(1.0 - NdotL, 0.0, 1.0);
    float bias = depthBias * cascadeScale * (1.0 + slopeScale * 4.0);

    float compareDepth = receiverDepth - bias;

    float texelSize = 1.0 / mapSize;
    float sum = 0.0;

    // 3x3 PCF 叠在硬件的 2x2 之上, 等效 6x6 的滤波足迹
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            vec2 offset = vec2(float(x), float(y)) * texelSize;
            sum += texture(shadowMap,
                           vec4(shadowUV + offset, float(cascade),
                                compareDepth));
        }
    }

    return sum / 9.0;
}

// ============================================================
// 聚光灯阴影 — 从图集里取
//
// 与级联那一套的差别不只是"少了选级"这一步:
//
//   1. 透视投影, w 不再恒为 1 —— 除法是必需的, 不是保险。
//   2. 采样坐标要先钳在块内再加偏移。**越过一块的边界就进了隔壁那盏灯
//      的块**, 而寻址模式救不了这个: ClampToBorder 只在整张图集的边界
//      上起作用, 图集内部的块与块之间没有"边界"可言。
//      漏掉这一句的表现是某盏灯的阴影边缘印着另一盏灯的影子。
//   3. 深度偏移是**世界单位**, 加在世界坐标上, 不是加在 NDC 深度上。
//      透视投影下 d(ndc)/dd ≈ near·far/((far-near)·d²) —— 近远平面取
//      0.05 与 30 时, 在 d=6 处只有 0.0014 每单位。照搬级联那个加在 NDC
//      上的 0.0015 等于把采样点沿光线推开一整个世界单位, 阴影整片脱离
//      物体; 而画面上那看起来像"这盏灯根本没有阴影"。
// ============================================================
float ComputeSpotShadow(vec3 worldPos, vec3 normal, vec3 lightDir,
                        int tileIndex)
{
    SpotShadowData data = spotShadowBuffer.spotShadows[tileIndex];

    float NdotL = clamp(dot(normal, lightDir), 0.0, 1.0);

    // 法线偏移沿法线把采样点推离表面 —— 掠射角下深度偏移救不了的那部分。
    // 用的是级联那一套的同一个参数: 两者的量纲相同 (世界单位), 而单独给
    // 聚光灯一个参数意味着多一个要调的旋钮, 且没有证据说明它该不一样。
    float normalBias = lighting.shadowParams.y;

    // 深度偏移沿**光照方向**把接收点朝光源挪一点, 而不是在 NDC 深度上减。
    // 挪的量随掠射角增大 —— 掠射时同一个纹素覆盖的深度跨度更大。
    //
    // 沿光线挪有一个不显然的好处: **它完全不会移动影子的边界。**
    //
    // 点光源的阴影测试是"从灯出发经过该点的射线有没有撞到遮挡物"。沿着
    // 那条射线本身挪, 挪完还在同一条射线上, 撞不撞得到不变。代入相似
    // 三角形算一遍, 偏移量在分子分母里正好约掉。
    //
    // 这不是推测: 把偏移量放大十倍, --shadow-check 量到的边界一动不动
    // (误差仍是 0.013)。而 NDC 深度上减一个常数就完全不同 —— 那等价于
    // 把接收面整体朝灯平移, 影子会跟着缩, 也就是 peter-panning。
    //
    // 反过来说, 边界判据抓不到"深度偏移调错"这一类。抓得到的是法线偏移
    // 过大 (那是沿法线挪, 会真的移动边界) 和偏移大到把采样点推过遮挡物
    // (那时影子整片消失)。
    float depthBias  = data.params.x;
    float slopeScale = clamp(1.0 - NdotL, 0.0, 1.0);

    vec3 offsetPos = worldPos +
                     normal   * (normalBias * (1.0 - NdotL * 0.5)) +
                     lightDir * (depthBias * (1.0 + slopeScale * 4.0));

    vec4 shadowClip = data.viewProj * vec4(offsetPos, 1.0);

    // 光源背后的点 —— 判为无遮挡。除以负的 w 会把它翻到锥的正面, 于是
    // 灯背后的墙上会出现一块与灯前方对称的假阴影。
    if (shadowClip.w <= 0.0)
    {
        return 1.0;
    }

    vec3 projected = shadowClip.xyz / shadowClip.w;

    float receiverDepth = projected.z;

    if (receiverDepth > 1.0 || receiverDepth < 0.0)
    {
        return 1.0;
    }

    // 块内归一化坐标。锥外的点落在 [0,1] 之外 —— 那里没有该灯的深度信息,
    // 判为无遮挡而不是钳进块内: 钳的话锥边缘那一圈纹素会被拉伸到整个锥外,
    // 表现为聚光灯的光斑外拖出一条长长的阴影。
    vec2 tileUV = projected.xy * 0.5 + 0.5;

    if (tileUV.x < 0.0 || tileUV.x > 1.0 ||
        tileUV.y < 0.0 || tileUV.y > 1.0)
    {
        return 1.0;
    }

    float atlasSize = max(data.params.y, 1.0);

    // 偏移已经在世界空间里做过了, 这里直接比。再在 NDC 上减一次的话,
    // 同一件事做了两遍而量纲还不同 —— 调其中一个另一个就失配。
    float compareDepth = receiverDepth;

    // PCF 的偏移在**块内**做, 加上块偏移之前先钳。顺序反了的话, 块边缘的
    // 像素会有一两个抽头落进隔壁的块。
    float texelSize = 1.0 / atlasSize;

    // 块内的半纹素余量 —— PCF 最多偏出一个纹素, 留一个半就够。
    vec2 tileInset = vec2(1.5) * texelSize / data.uvTransform.zw;

    float sum = 0.0;

    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            vec2 offset = vec2(float(x), float(y)) * texelSize /
                          data.uvTransform.zw;

            vec2 clamped = clamp(tileUV + offset, tileInset, 1.0 - tileInset);

            vec2 atlasUV = data.uvTransform.xy + clamped * data.uvTransform.zw;

            sum += texture(spotShadowAtlas, vec3(atlasUV, compareDepth));
        }
    }

    return sum / 9.0;
}

// ============================================================
// 主函数
// ============================================================
// ── 单盏光的贡献 ──
//
// 从原来的循环体里原样抽出来。抽出来的唯一理由是分簇与暴力两条路径必须
// 走**同一份代码** —— 各写一遍的话, --light-cull-check 比出来的差异里就
//混进了"两份代码本来就不完全一样"这个因素。
//
// lightIndex 只用于判断"是不是第 0 盏方向光"(当前只有它投射阴影)。
vec3 ShadeOneLight(LightData light, int lightIndex,
                   vec3 N, vec3 V, vec3 F0,
                   vec3 albedo, float roughness, float metallic)
{
    // 光照方向 (从片段指向光源)
    vec3 L = GetLightDirection(light, fragWorldPos);

    // 半程向量
    vec3 H = normalize(V + L);

    // 衰减
    float attenuation = CalcAttenuation(light, fragWorldPos);

    // 入射辐照度 = 光源颜色 × 强度 × 衰减
    vec3 radiance = light.colorAndIntensity.xyz *
                    light.colorAndIntensity.w *
                    attenuation;

    // ---- Cook-Torrance BRDF ----
    float D = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    vec3  F = FresnelSchlick(max(dot(H, V), 0.0), F0);

    vec3  numerator   = D * G * F;
    float denominator = 4.0 * max(dot(N, V), 0.0) *
                        max(dot(N, L), 0.0) + 0.0001;
    vec3 specular = numerator / denominator;

    // 能量守恒: kS = F (镜面反射比例)
    vec3 kS = F;

    // kD = 1 - kS (漫反射比例, 金属无漫反射)
    vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);

    // Lambert 漫反射
    vec3 diffuse = kD * albedo / PI;

    float NdotL = max(dot(N, L), 0.0);

    // 阴影。两条来源:
    //   主方向光 (第 0 盏) 走级联贴图;
    //   聚光灯若拿到了图集里的一块, 走图集。
    //
    // 点光源仍按无遮挡处理 —— 它需要六面立方体阴影, 是另一件事。
    //
    // 块下标从光源数据里读而非另建一张表: 两者只要有一处漂移, 这盏灯就会
    // 采到另一盏灯的块, 而那是个"有影子、但形状完全不对"的结果。
    float shadow = 1.0;

    if (lightIndex == 0 && int(light.positionAndType.w) == 0)
    {
        shadow = ComputeShadow(fragWorldPos, N, L);
    }
    else if (int(light.positionAndType.w) == 2 && light.spotParams.z >= 0.0)
    {
        shadow = ComputeSpotShadow(fragWorldPos, N, L,
                                   int(light.spotParams.z));
    }

    return (diffuse + specular) * radiance * NdotL * shadow;
}

void main()
{
    // 取本次绘制的材质。
    //
    // 必须在任何使用 material 的代码之前 —— 它是全局变量, 未赋值时内容
    // 未定义。放在 main 的第一行而不是散在各处, 是因为一旦漏赋值, 表现
    // 是整个物体的材质参数全是垃圾, 而不是某一项不对。
    material = materials[pc.materialIndex];

    // ---- 表面参数 (set 1 材质系统) ----
    vec4 baseColor = material.BaseColor;
    if ((material.TextureFlags & TEX_ALBEDO) != 0u)
    {
        baseColor *= SampleBindless(material.AlbedoIndex, fragTexCoord);
    }

    if (material.BlendMode == BLEND_MASKED && baseColor.a < material.AlphaCutoff)
    {
        discard;
    }

    vec3 albedo = fragColor * baseColor.rgb;

    float metallic  = clamp(material.Metallic, 0.0, 1.0);
    float roughness = clamp(material.Roughness, 0.04, 1.0);
    if ((material.TextureFlags & TEX_METALLIC_ROUGHNESS) != 0u)
    {
        vec4 mr =
            SampleBindless(material.MetallicRoughnessIndex, fragTexCoord);
        roughness = clamp(roughness * mr.g, 0.04, 1.0);
        metallic  = clamp(metallic * mr.b, 0.0, 1.0);
    }

    float ao = clamp(material.AO, 0.0, 1.0);
    if ((material.TextureFlags & TEX_OCCLUSION) != 0u)
    {
        ao *= SampleBindless(material.OcclusionIndex, fragTexCoord).r;
    }

    // 屏幕空间遮蔽与材质自带的遮蔽相乘。
    //
    // 两者管的不是同一件事: 材质的 AO 贴图记录的是**模型自身**的凹陷 (烘焙
    // 时就定下来了), 屏幕空间的 AO 记录的是**物体之间**的接触与遮挡 (运行时
    // 才知道)。相加会让两处同时凹陷的地方变亮, 取最小值会丢掉其中一个的
    // 层次 —— 相乘是唯一保留两者的做法。
    //
    // 用 gl_FragCoord 而非任何插值来的 UV: AO 是屏幕空间的量, 必须按像素
    // 位置取。
    ao *= texture(ambientOcclusion,
                  gl_FragCoord.xy / lighting.clusterParams.zw).r;

    vec3 emissive = material.EmissiveColor.rgb;
    if ((material.TextureFlags & TEX_EMISSIVE) != 0u)
    {
        emissive *= SampleBindless(material.EmissiveIndex, fragTexCoord).rgb;
    }

    // ---- 世界空间法线 (归一化) ----
    vec3 N = ApplyNormalMap(normalize(fragWorldNormal));

    // ---- 视线方向 (从片段指向相机) ----
    vec3 V = normalize(lighting.cameraPosition.xyz - fragWorldPos);

    // ---- 基础反射率 F0 ----
    // 非金属: 0.04 (塑料/玻璃基准)
    // 金属: 使用 albedo 作为 F0
    vec3 F0 = vec3(0.04);
    F0 = mix(F0, albedo, metallic);

    // ---- 累积所有光源的辐照度 ----
    //
    // 两条路径, 同一个着色函数 (ShadeOneLight):
    //   分簇开 — 方向光走 [0, dirCount), 其余走本像素所在簇的索引区间
    //   分簇关 — 全部光源一遍
    //
    // 用运行时分支而不是两个着色器变体, 是为了让 --light-cull-check 能在
    // **同一份代码**上比对两条路径。两个变体的话, 比出来的差异里就分不清
    // 哪些来自剔除、哪些来自编译器对两份代码的不同优化。
    vec3 Lo = vec3(0.0);

    int lightCount = int(lighting.lightCountVec.x);
    int dirCount   = int(lighting.lightCountVec.y);

    bool clustered = lighting.clusterConfig.x > 0.5;

    if (clustered)
    {
        // 方向光不参与分簇 —— 它照亮整个场景, 分给每个簇等于没剔除。
        // FLightManager 保证它们占据 [0, dirCount)。
        for (int i = 0; i < dirCount; ++i)
        {
            Lo += ShadeOneLight(lightBuffer.lights[i], i, N, V, F0,
                                albedo, roughness, metallic);
        }

        uint clusterIndex = ClusterIndexForFragment(
            gl_FragCoord.xy,
            lighting.clusterParams.zw,
            fragViewDepth,
            lighting.clusterParams.x,
            lighting.clusterParams.y);

        uvec2 range = clusterGrid.grid[clusterIndex];

        for (uint k = 0u; k < range.y; ++k)
        {
            int i = int(clusterIndices.indices[range.x + k]);

            Lo += ShadeOneLight(lightBuffer.lights[i], i, N, V, F0,
                                albedo, roughness, metallic);
        }
    }
    else
    {
        for (int i = 0; i < lightCount; ++i)
        {
            Lo += ShadeOneLight(lightBuffer.lights[i], i, N, V, F0,
                                albedo, roughness, metallic);
        }
    }

    // ---- 环境光 ----
    //
    // 常数环境光对每个朝向给出同样的值 —— 朝天的面与朝地的面收到一样多,
    // 物体因此没有任何"被环境照亮"的层次。更要命的是金属: 它的 kD 为 0,
    // 常数环境光乘上去等于零, 于是金属在没有直接光的方向上完全是黑的。
    //
    // 有辐照度贴图时改按法线查表。这里只处理漫反射项 —— 镜面的环境反射
    // 需要预滤波贴图与 BRDF 查找表, 尚未接入, 因此金属此刻仍只有直接光的
    // 高光。这一步先把"环境光有方向"这件事做对。
    vec3 ambient;

    if (lighting.iblParams.x > 0.5)
    {
        const float NdotV = max(dot(N, V), 0.0);

        vec3 irradiance = texture(irradianceMap, N).rgb;

        // ---- 镜面项 (split-sum) ----
        //
        //   ∫ L·f·(n·l) dl ≈ [预滤波贴图] · [F0·A + B]
        //
        // 反射方向按粗糙度选 mip。roughness 与 LOD 的映射必须与预滤波时
        // "粗糙度在各级之间线性铺开"严格互逆, 否则某一档粗糙度会取到
        // 邻近档的内容 —— 表现是粗糙度调节的手感不对, 而非明显的错误。
        vec3  R           = reflect(-V, N);
        float lod         = roughness * lighting.iblParams.z;
        vec3  prefiltered = textureLod(prefilteredMap, R, lod).rgb;

        vec2 envBrdf = texture(brdfLut, vec2(NdotV, roughness)).rg;

        // ---- 三项能量一次分配 ----
        //
        // 菲涅尔直接用 F0 而不再做粗糙度修正: 掠射角的衰减已经包含在
        // 查找表的 A、B 里了。再乘一次带粗糙度的 Schlick 等于把同一件事
        // 算两遍, 结果是掠射角偏暗。
        //
        // 多次散射的补偿项按辐照度着色而非按预滤波贴图: 弹射多次之后
        // 方向性早已散尽, 用近似均匀的辐照度比用有方向的反射更接近实际。
        FEnergyTerms energy =
            ComputeEnergyTerms(envBrdf, F0, albedo * (1.0 - metallic));

        vec3 iblColor = energy.SingleScatter * prefiltered +
                        (energy.MultiScatter + energy.Diffuse) * irradiance;

        // 环境遮蔽同时作用于三项。严格说镜面该用单独的镜面遮蔽项,
        // 但那需要额外的贴图; 共用 ao 是通行做法, 偏差主要出现在
        // 缝隙深处 —— 那里本来也几乎看不到反射。
        ambient = iblColor * ao * lighting.iblParams.y;
    }
    else
    {
        ambient = lighting.ambientColor.xyz *
                  lighting.ambientColor.w *
                  albedo * ao;
    }

    // ---- 最终颜色 — 线性 HDR, 不在此处做色调映射 ----
    //
    // 色调映射与 Gamma 校正已移到独立的后处理 Pass。写在这里的话:
    //   1. 每个片段各自映射一次, 而色调映射按定义是对最终图像的操作;
    //   2. Bloom、TAA 之类需要的是**线性 HDR** 输入, 颜色若在进入它们之前
    //      已被映射, 亮部信息已经丢失, 再怎么处理也找不回来;
    //   3. 曝光是全局参数, 逐材质着色器无处安放。
    vec3 color = ambient + Lo + emissive;

    outColor = vec4(color, baseColor.a);
}
