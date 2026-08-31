// ============================================================================
// bloom_downsample.frag — 泛光的降采样链
//
// 13 抽头的降采样核 (Jimenez, SIGGRAPH 2014 "Next Generation Post Processing
// in Call of Duty")。不是简单的 2x2 box:
//
//   box 降采样在每一级都会让高频混叠, 而泛光链要降 6 级 —— 混叠逐级累积,
//   表现是画面上有细小高光时泛光会**闪烁**: 高光移动不到一个像素, 采样点
//   却跳到了另一个纹素上。那种闪烁在静止画面里看不出来, 一动起来就明显,
//   而那时人会去怀疑 TAA。
//
// 13 抽头分成两组: 4 个位于半纹素对角 (权重 0.5 合计), 9 个位于整纹素的
// 3x3 (权重 0.5 合计)。两组都是归一化的, 所以整个核的权重和恰好为 1 ——
// 能量守恒, 而 --bloom-check 直接断言这一点。
//
// ── 第 0 级兼做阈值提取 ──
//
// mip 0 那一次降采样同时做亮度阈值 (只有超过阈值的部分参与泛光)。分成两个
// 通道也行, 但那要多一张全分辨率的中间纹理和一次全屏读写 —— 而阈值本身
// 只是一个 max, 融进降采样几乎免费。
//
// 阈值用**软膝盖**而非硬截断: 硬截断会让亮度刚好跨过阈值的像素在泛光里
// 突然出现, 表现为物体边缘的泛光边界随相机移动而抖动。
// ============================================================================

#version 450

layout(location = 0) in vec2 fragUV;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D sourceTexture;

layout(push_constant) uniform Params {
    // xy = 源纹理的 1/尺寸, z = 是否做阈值提取 (0/1), w = 阈值
    vec4 texel;

    // x = 软膝盖宽度, y/z/w 保留
    vec4 knee;
} pc;

float Luminance(vec3 color)
{
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

/// 软膝盖阈值 —— 在阈值附近平滑过渡而非硬截断
///
/// 返回该像素参与泛光的比例 (0~1)。硬截断 (step) 会让亮度刚好跨过阈值的
/// 像素在泛光里突然出现/消失, 表现为泛光边界随相机移动而抖动。
vec3 ApplyThreshold(vec3 color)
{
    if (pc.texel.z < 0.5)
    {
        return color;
    }

    float lum       = Luminance(color);
    float threshold = pc.texel.w;
    float knee      = max(pc.knee.x, 1.0e-5);

    // 在 [threshold - knee, threshold + knee] 区间里二次过渡
    float soft = clamp(lum - threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);

    float contribution = max(soft, lum - threshold) / max(lum, 1.0e-5);

    return color * max(contribution, 0.0);
}

void main()
{
    vec2 t = pc.texel.xy;

    // 采样原点单独取出来。十三个抽头都相对它, 于是"整个核偏了多少"是一处
    // 可以检查的事实, 而不是散在十三行里的十三个坐标。
    vec2 uv = fragUV;

    // 13 抽头。坐标是相对**源**纹理的纹素尺寸。
    //
    //   a   b   c
    //     j   k
    //   d   e   f
    //     l   m
    //   g   h   i
    //
    // e 是中心, a~i 是整纹素的 3x3, j~m 是半纹素的对角。
    vec3 a = texture(sourceTexture, uv + vec2(-2.0 * t.x,  2.0 * t.y)).rgb;
    vec3 b = texture(sourceTexture, uv + vec2( 0.0,        2.0 * t.y)).rgb;
    vec3 c = texture(sourceTexture, uv + vec2( 2.0 * t.x,  2.0 * t.y)).rgb;

    vec3 d = texture(sourceTexture, uv + vec2(-2.0 * t.x,  0.0)).rgb;
    vec3 e = texture(sourceTexture, uv).rgb;
    vec3 f = texture(sourceTexture, uv + vec2( 2.0 * t.x,  0.0)).rgb;

    vec3 g = texture(sourceTexture, uv + vec2(-2.0 * t.x, -2.0 * t.y)).rgb;
    vec3 h = texture(sourceTexture, uv + vec2( 0.0,       -2.0 * t.y)).rgb;
    vec3 i = texture(sourceTexture, uv + vec2( 2.0 * t.x, -2.0 * t.y)).rgb;

    vec3 j = texture(sourceTexture, uv + vec2(-t.x,  t.y)).rgb;
    vec3 k = texture(sourceTexture, uv + vec2( t.x,  t.y)).rgb;
    vec3 l = texture(sourceTexture, uv + vec2(-t.x, -t.y)).rgb;
    vec3 m = texture(sourceTexture, uv + vec2( t.x, -t.y)).rgb;

    // 权重: 对角四个各 0.125 (合计 0.5), 3x3 的九个按 box 权重合计 0.5。
    //
    // 两组各占一半, 总和恰好 1.0 —— 这是能量守恒的来源, --bloom-check 会验。
    vec3 result = (j + k + l + m) * 0.125;

    result += (a + c + g + i) * 0.03125;
    result += (b + d + f + h) * 0.0625;
    result += e * 0.125;

    outColor = vec4(ApplyThreshold(result), 1.0);
}
