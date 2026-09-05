// ============================================================================
// taa.frag — 时域抗锯齿的解析
//
// 把本帧 (带亚像素抖动) 与重投影后的历史混合。抖动让每一帧在像素内取不同的
// 采样位置, 若干帧累积起来就等效于超采样。
//
// 三件事决定了 TAA 是"更清晰"还是"一团糊":
//
//   1. **重投影要准。** 用速度缓冲把本像素在上一帧的位置找出来。速度错了
//      就是把不相干的像素混进来 —— 表现是运动物体拖着一条尾巴。
//
//   2. **历史要会被拒绝。** 遮挡关系变化 (前景移开露出背景) 时, 重投影找到
//      的历史是前景的颜色, 与当前背景无关。邻域裁剪负责拒绝它。
//
//   3. **拒绝不能太狠。** 裁剪范围取小了, 历史几乎每帧都被拉回当前值, TAA
//      就退化成"什么都没做" —— 而那看起来完全正常, 只是锯齿没消掉。
//
// 用**方差裁剪**而不是简单的 3x3 min/max 包围盒: 后者会被邻域里单个亮点
// (高光、火花) 撑得很大, 于是失效的历史也落在范围内被接受, 表现为高光周围
// 的拖影。方差裁剪按均值 ± γ·标准差 定范围, 孤立亮点抬高标准差的幅度有限。
//
// 与 Shaders/Builtin/gbuffer.frag 写入的速度缓冲配套。那份速度是**不含抖动**
// 的 NDC 差值 —— 见 view_common.h 的说明: 把抖动算进速度等于告诉 TAA "画面
// 每帧都在抖", 而那正是 TAA 要消掉的东西。
// ============================================================================

#version 450

#include "reproject_common.h"

layout(location = 0) in vec2 fragUV;

// [0] 解析结果 —— 固定的一张纹理, 后处理采样它
// [1] 历史 —— 乒乓的两张之一, 下一帧读它
//
// 用 MRT 同时写两份而不是"写完再拷贝": 拷贝要多一次全屏读写, 而 MRT 的
// 第二次写入几乎免费 (同一个片元, 数据已在寄存器里)。
//
// 也不能只用乒乓的历史当解析结果让后处理去采样 —— 那样后处理的描述符要
// 逐帧改, 而改一个正在被上一帧使用的描述符集是验证层错误。
layout(location = 0) out vec4 outResolve;
layout(location = 1) out vec4 outHistory;

layout(set = 0, binding = 0) uniform sampler2D currentColor;
layout(set = 0, binding = 1) uniform sampler2D historyColor;
layout(set = 0, binding = 2) uniform sampler2D velocityBuffer;

layout(push_constant) uniform Params {
    // x = 混合系数 (当前帧的权重), y = 方差裁剪的 γ
    // z = 是否有可用历史 (0/1), w = 保留
    vec4 blend;

    // xy = 视口尺寸 (像素), zw = 1/视口尺寸
    vec4 screen;
} params;

// 亮度 —— 用于把火花状的异常值压下去
float Luminance(vec3 color)
{
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

void main()
{
    vec3 current = texture(currentColor, fragUV).rgb;

    // 没有可用历史 (第一帧、或刚重建交换链) 时直接输出当前帧。
    //
    // 不这么做的话第一帧会与一张未初始化的历史混合 —— 那张纹理的内容是
    // 未定义的, 可能是上一个应用留下的显存。表现是开场几帧的花屏, 而且
    // 每次运行都不一样, 极难复现。
    if (params.blend.z < 0.5)
    {
        outResolve = vec4(current, 1.0);
        outHistory = vec4(current, 1.0);
        return;
    }

    // ---- 重投影 ----
    //
    // 规则本身搬进了 reproject_common.h —— 那里的每个函数都是纯函数, 于是
    // CPU 能独立实现同一个规则, --reproject-check 拿两者逐像素比。留在这里
    // 的话, 符号 / 0.5 缩放 / Y 轴这三个约定就只能靠"看画面糊不糊"来验。
    //
    // 取速度用的是**本像素**的速度而不是邻域里速度最大的那个。后者 (常见的
    // "closest depth" 技巧) 能减少运动物体边缘的重影, 但需要深度缓冲, 而
    // 深度在前向通道之后是 DontCare 的。这是一处已知的取舍。
    vec2 velocity = texture(velocityBuffer, fragUV).rg;

    vec2 historyUV = ReprojectToHistoryUV(fragUV, velocity);

    // 重投影落到屏幕外 = 这块内容上一帧还不存在, 没有历史可用
    if (!ReprojectIsOnScreen(historyUV))
    {
        outResolve = vec4(current, 1.0);
        outHistory = vec4(current, 1.0);
        return;
    }

    vec3 history = texture(historyColor, historyUV).rgb;

    // ---- 方差裁剪 ----
    //
    // 统计 3x3 邻域的一阶矩与二阶矩, 得到均值与标准差, 用 均值 ± γ·标准差
    // 作为历史的可接受范围。
    vec3 moment1 = vec3(0.0);
    vec3 moment2 = vec3(0.0);

    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            vec2 offset = vec2(float(x), float(y)) * params.screen.zw;

            vec3 neighbor = texture(currentColor, fragUV + offset).rgb;

            moment1 += neighbor;
            moment2 += neighbor * neighbor;
        }
    }

    const float kSamples = 9.0;

    vec3 mean  = moment1 / kSamples;
    vec3 sigma = sqrt(max(moment2 / kSamples - mean * mean, vec3(0.0)));

    float gamma = params.blend.y;

    vec3 minColor = mean - gamma * sigma;
    vec3 maxColor = mean + gamma * sigma;

    history = clamp(history, minColor, maxColor);

    // ---- 混合 ----
    //
    // 按亮度加权: 亮的样本在指数滑动平均里权重过大, 表现为高光闪烁。
    // 这一步 (Karis 的做法) 让每个样本的权重与它的亮度成反比。
    float currentWeight = params.blend.x;
    float historyWeight = 1.0 - currentWeight;

    float currentLum = 1.0 / (1.0 + Luminance(current));
    float historyLum = 1.0 / (1.0 + Luminance(history));

    currentWeight *= currentLum;
    historyWeight *= historyLum;

    float total = max(currentWeight + historyWeight, 1.0e-5);

    vec3 result = (current * currentWeight + history * historyWeight) / total;

    outResolve = vec4(result, 1.0);
    outHistory = vec4(result, 1.0);
}
