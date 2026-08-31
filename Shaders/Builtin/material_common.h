// ============================================================================
// material_common.h — bindless 材质表的声明与共用逻辑
//
// 被 pbr.frag、depth_only.frag、gbuffer.frag 共同包含。
//
// 材质结构放在这里而不是各自声明一遍: 它必须与 C++ 侧 FBindlessMaterial
// 逐字段对应, 而分散在三个文件里意味着改一处要记得改三处 —— 漏改的表现是
// "某条通道里材质参数整体串了一位", 画面上像配错而不像 bug。
// ============================================================================

#ifndef LIMX_MATERIAL_COMMON_H
#define LIMX_MATERIAL_COMMON_H

// ── 纹理存在标志 (与 C++ 侧 kMaterialTexFlag* 对应) ──
#define TEX_ALBEDO              (1u << 0)
#define TEX_NORMAL              (1u << 1)
#define TEX_METALLIC_ROUGHNESS  (1u << 2)
#define TEX_OCCLUSION           (1u << 3)
#define TEX_EMISSIVE            (1u << 4)

// ── 混合模式 (与 C++ 侧 EMaterialBlendMode 对应) ──
#define BLEND_OPAQUE      0u
#define BLEND_MASKED      1u
#define BLEND_TRANSLUCENT 2u
#define BLEND_ADDITIVE    3u

// ============================================================================
// 材质表
// ============================================================================

/// 与 C++ 侧 FBindlessMaterial 逐字段对应, 共 80 字节。
///
/// std430 下 vec4 按 16 对齐、标量按 4 —— 两个 vec4 各自起于 16 的倍数,
/// 中间的标量凑满一组。改动顺序前先算对齐。C++ 侧有 static_assert 钉住
/// 总大小, 但那只保证 C++ 那一边; 两边的字段顺序是否一致仍然只能靠人对。
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

/// 全局纹理表。
///
/// 数组大小必须与 C++ 侧 FBindlessTable::kMaxTextures 一致。用固定大小而非
/// 无界数组 (sampler2D[]) 是为了不依赖 runtimeDescriptorArray 特性 —— 少一
/// 项特性要求就少一类"这台机器上跑不起来"。
///
/// 数组是稀疏的: 声明 1024 槽而场景只用几十个。未写入的槽位不能被索引,
/// 所以缺贴图的材质在 C++ 侧就被指向 0 号占位纹理, 着色器这里不做任何
/// 有效性判断 —— 判断一旦漏写就是随机读显存。
layout(set = 1, binding = 1) uniform sampler2D bindlessTextures[1024];

/// 采样全局表里的第 index 张贴图
vec4 SampleBindless(uint index, vec2 uv)
{
    return texture(bindlessTextures[nonuniformEXT(index)], uv);
}

// ============================================================================
// Masked 材质的 alpha 测试
// ============================================================================

/// 是否应当丢弃这个片段
///
/// 深度预通道与阴影通道共用同一套判据 —— 两者必须**完全一致**, 否则同一片
/// 镂空叶片在深度图里被剔掉而在阴影图里保留 (或反之), 结果是叶子投出实心
/// 方块的影子, 或者阴影里出现本不该有的孔洞。
bool ShouldDiscardMasked(MaterialData mat, vec2 uv)
{
    if (mat.BlendMode != BLEND_MASKED)
    {
        return false;
    }

    float alpha = mat.BaseColor.a;

    if ((mat.TextureFlags & TEX_ALBEDO) != 0u)
    {
        alpha *= SampleBindless(mat.AlbedoIndex, uv).a;
    }

    return alpha < mat.AlphaCutoff;
}

#endif // LIMX_MATERIAL_COMMON_H
