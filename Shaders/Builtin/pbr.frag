#version 450

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
layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragWorldNormal;
layout(location = 2) in vec3 fragColor;
layout(location = 3) in vec2 fragTexCoord;
layout(location = 4) in vec4 fragWorldTangent;   // xyz = 切线, w = 手性 (±1)

// ── 材质参数 (set 1, binding 0) — 必须匹配 FMaterialParams std140 布局 ──
layout(set = 1, binding = 0) uniform MaterialUBO {
    vec4  BaseColor;
    float Metallic;
    float Roughness;
    float AO;
    float NormalScale;
    vec4  EmissiveColor;
    float AlphaCutoff;
    uint  TextureFlags;
    uint  BlendMode;
    float _Padding;
} material;

// ── 材质贴图 (set 1, binding 1~5) ──
layout(set = 1, binding = 1) uniform sampler2D materialAlbedoMap;
layout(set = 1, binding = 2) uniform sampler2D materialNormalMap;
layout(set = 1, binding = 3) uniform sampler2D materialMetallicRoughnessMap;
layout(set = 1, binding = 4) uniform sampler2D materialOcclusionMap;
layout(set = 1, binding = 5) uniform sampler2D materialEmissiveMap;

// ── 光源数据结构 (std140, 80 bytes per light) ──
struct LightData {
    vec4 positionAndType;      // xyz=位置, w=类型
    vec4 directionAndRange;    // xyz=方向, w=衰减距离
    vec4 colorAndIntensity;    // xyz=颜色, w=强度
    vec4 attenuationParams;    // x=常量, y=线性, z=二次, w=保留
    vec4 spotParams;           // x=内锥角余弦, y=外锥角余弦, z/w=保留
};

// ── 光照 UBO (set 2, binding 0) ──
layout(set = 2, binding = 0) uniform LightingUBO {
    LightData lights[16];
    vec4      lightCountVec;   // x=光源数量
    vec4      cameraPosition;  // xyz=相机世界位置
    vec4      ambientColor;    // xyz=环境光颜色, w=环境光强度
} lighting;

// ── 片段着色器输出 ──
layout(location = 0) out vec4 outColor;

// ── 常量 ──
const float PI = 3.14159265359;
const uint  TEX_ALBEDO             = 1u << 0;
const uint  TEX_NORMAL             = 1u << 1;
const uint  TEX_METALLIC_ROUGHNESS = 1u << 2;
const uint  TEX_OCCLUSION          = 1u << 3;
const uint  TEX_EMISSIVE           = 1u << 4;
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

    vec3 tangentNormal = texture(materialNormalMap, fragTexCoord).xyz * 2.0 - 1.0;
    tangentNormal.xy *= max(material.NormalScale, 0.0);
    tangentNormal.z = sqrt(max(0.0, 1.0 - dot(tangentNormal.xy, tangentNormal.xy)));

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
// 主函数
// ============================================================
void main()
{
    // ---- 表面参数 (set 1 材质系统) ----
    vec4 baseColor = material.BaseColor;
    if ((material.TextureFlags & TEX_ALBEDO) != 0u)
    {
        baseColor *= texture(materialAlbedoMap, fragTexCoord);
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
        vec4 mr = texture(materialMetallicRoughnessMap, fragTexCoord);
        roughness = clamp(roughness * mr.g, 0.04, 1.0);
        metallic  = clamp(metallic * mr.b, 0.0, 1.0);
    }

    float ao = clamp(material.AO, 0.0, 1.0);
    if ((material.TextureFlags & TEX_OCCLUSION) != 0u)
    {
        ao *= texture(materialOcclusionMap, fragTexCoord).r;
    }

    vec3 emissive = material.EmissiveColor.rgb;
    if ((material.TextureFlags & TEX_EMISSIVE) != 0u)
    {
        emissive *= texture(materialEmissiveMap, fragTexCoord).rgb;
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
    vec3 Lo = vec3(0.0);

    int lightCount = int(lighting.lightCountVec.x);

    for (int i = 0; i < lightCount; ++i)
    {
        LightData light = lighting.lights[i];

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

        // 镜面反射项
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

        // N·L 因子
        float NdotL = max(dot(N, L), 0.0);

        // 累加该光源贡献
        Lo += (diffuse + specular) * radiance * NdotL;
    }

    // ---- 环境光 ----
    vec3 ambient = lighting.ambientColor.xyz *
                   lighting.ambientColor.w *
                   albedo * ao;

    // ---- 最终颜色 ----
    vec3 color = ambient + Lo + emissive;

    // ---- HDR → LDR: Reinhard 色调映射 ----
    color = color / (color + vec3(1.0));

    // ---- Gamma 校正 (线性 → sRGB, γ=2.2) ----
    color = pow(color, vec3(1.0 / 2.2));

    outColor = vec4(color, baseColor.a);
}
