// ============================================================
// 文件名称：bilateral_common.h
// 创建时间：2026-09-01
// 创建者  ：LimxTeam
// 设计哲学：这两个函数曾经有过两份实现的机会，而其中一份的分母符号写错了
//          整整一个周期 —— 它把 0.1..100 的距离映到 -0.1..-0.05，负数且随
//          距离减小，于是双边加权全数下溢，上采样静默退化成最近邻。不崩、
//          不报错、画面上只有边缘一点点台阶。
//          所以这里只留一份，谁要用谁包含。多一份实现就多一次写反的机会，
//          而这一类错误的特征恰恰是**没有症状**。
// 功能描述：透视深度的线性化，与半分辨率上采样的深度加权。
// ============================================================

#ifndef LIMX_BILATERAL_COMMON_H
#define LIMX_BILATERAL_COMMON_H

/// 把 [0,1] 的 NDC 深度还原成沿相机前向的世界距离 (正数)
///
/// 推导 (右手系视空间, -Z 为前方, Vulkan NDC z ∈ [0,1]):
///
///   投影矩阵的第三、四行给出
///     z_clip = A*z_view + B      A = far/(near-far)
///     w_clip = -z_view           B = far*near/(near-far)
///   于是
///     z_ndc = z_clip / w_clip = -A - B/z_view
///   反解并取距离 = -z_view:
///     距离 = B / (z_ndc + A)
///
/// 注意分母是 **+A** 不是 -A。A 与 B 在这套约定下都是负数, 二者相除得正。
float LinearizeViewDepth(float ndcDepth, float nearPlane, float farPlane)
{
    const float a = farPlane / (nearPlane - farPlane);
    const float b = farPlane * nearPlane / (nearPlane - farPlane);

    const float denom = ndcDepth + a;

    // z_ndc 等于 -a 时分母为零 —— 那对应无穷远。钳一个极小的**负**量, 让它
    // 给出一个极大的正距离: 钳成正的会翻转符号, 得到极大的负距离, 而那正是
    // 上面说的那种"没有症状"的失败。
    return b / ((abs(denom) < 1.0e-7) ? -1.0e-7 : denom);
}

/// 两个线性深度之间的双边权重
///
/// 问的是"这两个样本在不在同一个表面上", 而那只跟相对深度差有关, 与它们
/// 离相机多远无关 —— 所以是相对差而不是绝对差。
///
/// 0.05 的相对差 (5%) 之内算同一个表面: 取得太紧 (比如 0.01) 会让斜面上的
/// 相邻像素也被判成不同表面, 权重退化成只剩最近的一个, 那就是最近邻上采样,
/// 边缘出现台阶; 取得太松就退化回双线性, 不连续处渗色。
float BilateralDepthWeight(float centerDepth, float neighborDepth)
{
    const float relative =
        abs(centerDepth - neighborDepth) /
        max(max(centerDepth, neighborDepth), 1.0e-4);

    return exp(-relative / 0.05);
}

#endif // LIMX_BILATERAL_COMMON_H
