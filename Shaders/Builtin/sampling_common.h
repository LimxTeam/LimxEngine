#ifndef LIMX_SAMPLING_COMMON_H
#define LIMX_SAMPLING_COMMON_H

// ============================================================================
// sampling_common.h — 采样与多重重要性采样
//
// 从 path_trace.comp 里抽出来的, 目的是让**探针能验它**。
//
// 在此之前, 余弦加权采样只被"梯度炉"间接验着: 环境带一项线性梯度时, 余弦
// 加权的平均 (x + 2/3·y) 与均匀加权的平均 (x + 1/2·y) 差 1/6·y, 于是"按余弦
// 采样却按均匀 pdf 算权重"这类错会让炉子的值偏 10%。
//
// 那条判据管用, 但它是**间接**的: 它只看得见"合起来的那个数不对", 看不见
// 分布本身哪里不对。而分布的矩是有解析值的 —— 直接验它。
//
// 余弦加权半球采样 (pdf = cosθ/π) 的解析矩:
//
//     ∫ (cosθ/π) dω               = 1        pdf 归一
//     E[cosθ]   = ∫ cosθ·(cosθ/π) dω = 2/3
//     E[cos²θ]  = ∫ cos²θ·(cosθ/π) dω = 1/2
//     E[方向]                      = (0, 0, 2/3)   局部坐标系
//     E[sin²θ]  = 1 - E[cos²θ]      = 1/2
//
// 这几个数不依赖场景、不依赖材质, 也不依赖别的任何东西 —— 采样器对不对,
// 一比就知道。
// ============================================================================

// ============================================================================
// 随机数 —— PCG
//
// 与 path_trace.comp 里那份是同一套。低差异序列在这里是**有害**的: 它会让
// "方差 ∝ 1/N"那条判据失效 (低差异序列收敛更快, 斜率不是 -1), 而那条判据
// 验的是"样本之间真的独立"。
// ============================================================================

uint SamplingPcgNext(inout uint state)
{
    state = state * 747796405u + 2891336453u;

    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;

    return (word >> 22u) ^ word;
}

float SamplingNextFloat(inout uint state)
{
    // 除以 2^32 而不是 (2^32 - 1): 要的是 [0, 1) 的半开区间。
    // 取到 1.0 的话 sqrt(1-u1) 给出 cosθ = 0, 那是切平面方向 —— 贡献为零
    // 却仍然占一个样本, 而且下游的 1/cosθ 会炸。
    return float(SamplingPcgNext(state)) * (1.0 / 4294967296.0);
}

// ============================================================================
// 余弦加权半球采样
// ============================================================================

/// 在以 +Z 为法线的局部半球上按 cosθ/π 采一个方向
///
/// r = sqrt(u1) 是关键的一步: 它把均匀分布的 u1 变成"面积按 r dr 加权"的
/// 圆盘采样, 投到半球上正好是余弦加权。写成 r = u1 的话得到的是别的分布,
/// 而那种错在纯白炉里**完全隐形** (各向同性环境下两种分布的平均相同)。
vec3 SampleCosineHemisphere(float u1, float u2, out float outCosTheta)
{
    const float r        = sqrt(u1);
    const float phi      = 6.28318530718 * u2;
    const float cosTheta = sqrt(max(0.0, 1.0 - u1));

    outCosTheta = cosTheta;

    return vec3(r * cos(phi), r * sin(phi), cosTheta);
}

/// 余弦加权采样的 pdf
float CosineHemispherePdf(float cosTheta)
{
    return max(cosTheta, 0.0) * 0.31830988618;   // cosθ/π
}

// ============================================================================
// 多重重要性采样
//
// 两个采样策略 (例如"按 BRDF 采方向"与"直接采光源上一点") 估计同一个积分时,
// 各自在不同的情形下方差小。MIS 按 pdf 给两边分配权重, 而**权重之和必须
// 恒为 1** —— 那是无偏性的全部依据: 少了就丢能量, 多了就凭空多出能量。
//
// 这一条是能逐位验的: 平衡启发式的两个权重是 a/(a+b) 与 b/(a+b), 它们的和
// 在浮点上也应当极接近 1。判据拿一大片 (a, b) 组合去验, 包括极端情形
// (一边为零、相差十几个数量级)。
// ============================================================================

/// 平衡启发式 —— w_a = p_a / (p_a + p_b)
///
/// 两个 pdf 都为零时返回 0: 那时这个方向上两种策略都采不到, 贡献本来就是零。
/// 返回 0.5 之类的"中间值"会在分母为零的地方凭空造出权重。
float MisBalanceHeuristic(float pdfA, float pdfB)
{
    const float sum = pdfA + pdfB;

    return (sum > 0.0) ? (pdfA / sum) : 0.0;
}

/// 幂启发式 (β = 2) —— w_a = p_a² / (p_a² + p_b²)
///
/// 比平衡启发式更"果断": 一边明显占优时权重更接近 1, 于是方差更小。
/// Veach 的论文里 β = 2 是实测出来的折中。
///
/// 平方在这里有溢出风险: pdf 可以很大 (小光源上的立体角 pdf 轻易上千),
/// 而 1e20 的平方就溢出了 float32。所以先归一化再平方 —— 结果一样, 而
/// 中间量的量级被压住。
float MisPowerHeuristic(float pdfA, float pdfB)
{
    const float sum = pdfA + pdfB;

    if (sum <= 0.0)
    {
        return 0.0;
    }

    const float a = pdfA / sum;
    const float b = pdfB / sum;

    const float aa = a * a;
    const float bb = b * b;

    const float denominator = aa + bb;

    return (denominator > 0.0) ? (aa / denominator) : 0.0;
}

#endif // LIMX_SAMPLING_COMMON_H
