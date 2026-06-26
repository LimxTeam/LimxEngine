#version 450

// ============================================================
// UI 片段着色器 — SDF 抗锯齿渲染 + 纹理采样
//
// set=0, binding=1: sampler2D (默认白色纹理或用户纹理)
//
// Push Constant (48 bytes):
//   vec4 RectBounds  — (left, top, right, bottom) 屏幕像素
//   vec4 Params      — (cornerRadius, borderWidth, feather, shaderType)
//   vec4 ExtraColor  — (R, G, B, A) 描边/阴影颜色
//
// 着色器路径 (shaderType = Params.w):
//   0 = Standard     — 纹理采样 × 顶点颜色（向后兼容）
//   1 = SDFRound     — SDF 抗锯齿圆角矩形填充
//   2 = SDFBorder    — SDF 抗锯齿圆角矩形 + 描边
//   3 = BoxShadow    — SDF 盒阴影（高斯衰减）
//   4 = SDFCircle    — SDF 抗锯齿圆形
//   5 = Font         — 灰度字体纹理 (R8 → alpha × 顶点颜色)
//   6 = Gradient     — 纯顶点颜色插值（无纹理采样）
// ============================================================

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec4 fragColor;

layout(set = 1, binding = 0) uniform sampler2D texSampler;

layout(push_constant) uniform DrawParams
{
    vec4 RectBounds;   // left, top, right, bottom
    vec4 Params;       // cornerRadius, borderWidth, featherSize, shaderType
    vec4 ExtraColor;   // 描边/阴影 RGBA
} draw;

layout(location = 0) out vec4 outColor;

// ============================================================
// SDF 核心函数 — 圆角矩形有符号距离
// p:        片段到矩形中心的偏移向量
// halfSize: 矩形半尺寸
// radius:   圆角半径
// 返回值: 负值=内部, 正值=外部, 0=边界
// ============================================================
float sdRoundedBox(vec2 p, vec2 halfSize, float radius)
{
    vec2 q = abs(p) - halfSize + vec2(radius);
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}

// ============================================================
// SDF 核心函数 — 圆形有符号距离
// p:      片段到圆心的偏移向量
// radius: 圆形半径
// ============================================================
float sdCircle(vec2 p, float radius)
{
    return length(p) - radius;
}

void main()
{
    int shaderType = int(draw.Params.w + 0.5);

    // --------------------------------------------------------
    // 路径 0: 标准 — 纹理采样 × 顶点颜色（向后兼容）
    // --------------------------------------------------------
    if (shaderType == 0)
    {
        vec4 texColor = texture(texSampler, fragUV);
        outColor = texColor * fragColor;
        return;
    }

    // 所有 SDF 路径的公共计算
    vec2 rectMin  = draw.RectBounds.xy;
    vec2 rectMax  = draw.RectBounds.zw;
    vec2 center   = (rectMin + rectMax) * 0.5;
    vec2 halfSize = (rectMax - rectMin) * 0.5;
    float radius  = draw.Params.x;
    float feather = max(draw.Params.z, 0.5);

    // gl_FragCoord.xy 是窗口坐标（像素中心偏移 0.5）
    vec2 fragPos = gl_FragCoord.xy;

    // --------------------------------------------------------
    // 路径 1: SDF 抗锯齿圆角矩形填充
    // --------------------------------------------------------
    if (shaderType == 1)
    {
        float dist = sdRoundedBox(fragPos - center, halfSize, radius);
        float alpha = 1.0 - smoothstep(-feather, feather, dist);
        outColor = fragColor * alpha;
        return;
    }

    // --------------------------------------------------------
    // 路径 2: SDF 抗锯齿圆角矩形 + 描边
    // --------------------------------------------------------
    if (shaderType == 2)
    {
        float borderWidth = draw.Params.y;
        float dist = sdRoundedBox(fragPos - center, halfSize, radius);

        // 外边界抗锯齿
        float outerAlpha = 1.0 - smoothstep(-feather, feather, dist);

        // 内填充区域（收缩 borderWidth）
        float innerRadius = max(radius - borderWidth, 0.0);
        vec2  innerHalf   = max(halfSize - vec2(borderWidth), vec2(0.0));
        float innerDist   = sdRoundedBox(
            fragPos - center, innerHalf, innerRadius);
        float innerAlpha  = 1.0 - smoothstep(-feather, feather, innerDist);

        // 填充色 + 描边色混合
        vec4 fillColor   = fragColor * innerAlpha;
        vec4 borderColor = draw.ExtraColor * (outerAlpha - innerAlpha);
        outColor = fillColor + borderColor;
        return;
    }

    // --------------------------------------------------------
    // 路径 3: SDF 盒阴影（高斯衰减）
    // --------------------------------------------------------
    if (shaderType == 3)
    {
        float blurRadius = max(draw.Params.y, 0.5);
        float dist = sdRoundedBox(fragPos - center, halfSize, radius);

        // 高斯近似衰减: exp(-0.5 * (dist/sigma)^2)
        // sigma ≈ blurRadius / 3 使得 3σ 处衰减到接近 0
        float sigma = blurRadius * 0.3333;
        float shadowAlpha;
        if (dist <= 0.0)
        {
            shadowAlpha = 1.0;
        }
        else
        {
            float t = dist / sigma;
            shadowAlpha = exp(-0.5 * t * t);
        }
        outColor = draw.ExtraColor * shadowAlpha;
        return;
    }

    // --------------------------------------------------------
    // 路径 4: SDF 抗锯齿圆形
    // --------------------------------------------------------
    if (shaderType == 4)
    {
        float circleRadius = min(halfSize.x, halfSize.y);
        float dist = sdCircle(fragPos - center, circleRadius);
        float alpha = 1.0 - smoothstep(-feather, feather, dist);
        outColor = fragColor * alpha;
        return;
    }

    // --------------------------------------------------------
    // 路径 5: 字体 — 灰度纹理 R 通道作为 alpha × 顶点颜色
    // 适用于 R8_UNORM 格式的字体图集纹理
    // --------------------------------------------------------
    if (shaderType == 5)
    {
        float glyphAlpha = texture(texSampler, fragUV).r;
        outColor = vec4(fragColor.rgb, fragColor.a * glyphAlpha);
        return;
    }

    // --------------------------------------------------------
    // 路径 6: 渐变 — 纯顶点颜色插值，不采样纹理
    // --------------------------------------------------------
    if (shaderType == 6)
    {
        outColor = fragColor;
        return;
    }

    // 未知路径: 回退到标准
    vec4 texColor = texture(texSampler, fragUV);
    outColor = texColor * fragColor;
}
