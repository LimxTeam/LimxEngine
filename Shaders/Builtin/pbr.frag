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
//
// row_major 与 FMatrix 的行主序存储一致 —— 不加的话 GLSL 按列主序解读,
// 等于把阴影矩阵整体转置, 阴影坐标会落到完全无关的位置。
layout(row_major, set = 2, binding = 0) uniform LightingUBO {
    LightData lights[16];
    vec4      lightCountVec;   // x=光源数量
    vec4      cameraPosition;  // xyz=相机世界位置
    vec4      ambientColor;    // xyz=环境光颜色, w=环境光强度
    mat4      cascadeViewProj[3];  // 各级联的视图投影矩阵
    vec4      cascadeSplits;         // xyz=各级外边界的径向距离
    vec4      shadowParams;          // x=深度偏移 y=法线偏移 z=贴图边长 w=启用
    vec4      iblParams;             // x=启用 IBL, y=IBL 强度倍数
} lighting;

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

        // 阴影 —— 当前只有主方向光(第 0 盏)投射阴影。
        // 其余光源按无遮挡处理: 每盏光一张阴影贴图的代价与收益在这个
        // 阶段完全不成比例, 而"只有主光有影子"在户外场景里几乎看不出来。
        float shadow = (i == 0 && int(light.positionAndType.w) == 0)
                           ? ComputeShadow(fragWorldPos, N, L)
                           : 1.0;

        // 累加该光源贡献
        Lo += (diffuse + specular) * radiance * NdotL * shadow;
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
        // 环境项的菲涅尔用几何法线与视线的夹角, 而非某个具体半程向量 ——
        // 环境光来自所有方向, 没有单一的半程向量可言。
        vec3  F_ambient  = FresnelSchlick(max(dot(N, V), 0.0), F0);
        vec3  kD_ambient = (vec3(1.0) - F_ambient) * (1.0 - metallic);

        vec3 irradiance = texture(irradianceMap, N).rgb
                        * lighting.iblParams.y;

        ambient = kD_ambient * irradiance * albedo * ao;
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
