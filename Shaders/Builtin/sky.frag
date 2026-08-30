#version 450

// ============================================================
// 天空盒片段着色器 — 采样环境立方体贴图
//
// 输出的是**线性 HDR**, 不做任何色调映射:
//   本 Pass 写进共享的 HDR 目标, 与几何体的输出并列, 之后统一由
//   FPostProcessPass 做曝光与 ACES。若在这里先压一次, 天空的曝光
//   将与场景其余部分脱钩 —— 调整曝光时地面变暗而天空不变。
//
// 强度倍数单独给出而非烘进贴图:
//   HDRI 的绝对量级取决于拍摄时的标定, 不同来源相差可达上百倍。
//   把它做成运行时参数, 才能在不重新转换立方体贴图的前提下配平
//   天空与直接光的相对亮度。
// ============================================================

layout(location = 0) in vec3 fragViewDirection;

layout(set = 1, binding = 0) uniform samplerCube environmentMap;

layout(push_constant) uniform SkyParams {
    float intensity;   // 线性强度倍数
    float _pad0;
    float _pad1;
    float _pad2;
} params;

layout(location = 0) out vec4 outColor;

void main()
{
    // 逐片段归一化: 顶点着色器给出的方向在三角形内是线性插值的,
    // 线性插值不保长度, 直接拿去采样会在画面边缘产生方向偏差
    const vec3 direction = normalize(fragViewDirection);

    const vec3 radiance = texture(environmentMap, direction).rgb;

    outColor = vec4(radiance * max(params.intensity, 0.0), 1.0);
}
