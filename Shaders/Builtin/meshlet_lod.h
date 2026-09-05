#ifndef LIMX_MESHLET_LOD_H
#define LIMX_MESHLET_LOD_H

// ============================================================================
// meshlet_lod.h — LOD 选择规则
//
// 与 C++ 侧的 FMeshLodDag.h / .cpp 里那两个函数**逐字对应**。两份实现必须
// 给出同一个选中集, 而 --lod-gpu-check 逐条比对它们的中间量。
//
// 为什么要有两份而不是一份: 参与渲染的是这一份, 而判据的参考实现是那一份。
// 判据拿被验的实现自己算一遍, 验的就只剩"这段代码等于它自己"。
//
// ── 选择规则为什么不重不漏 ──
//
//   一、它是**组级**谓词。自身量来自产出组, 父量来自消费组 —— 所以同一对
//       (产出组, 消费组) 的全部 meshlet 决策必然相同, 兄弟永不分歧。
//       这是"不裂"的一半, 而且是结构上成立的, 不靠任何运行期通信。
//
//   二、屏幕误差沿 DAG 的每条边**严格增**, 对每一个相机。误差严格增 (建 DAG
//       时的增长地板) 加上父球含子球 (于是 d' <= d), 两者合起来给出这一条。
//
//   三、于是表面上任意一点诱导出一条严格递增、从 0 到 +inf 的链, 而有限的
//       正阈值恰好落在其中一段里 —— 存在则不漏, 唯一则不重。
// ============================================================================

/// 根 meshlet 的父误差 —— 一个巨大的**有限**数
///
/// 不用 +inf: 运行期还要乘实例缩放, 而 inf 乘任何数之后与别的 inf 之间没有
/// 严格大小关系 —— 上面第二步要的正是那个严格。留八个数量级的余量, 乘完仍是
/// 有限数, 而"巨大的有限数 >= 任何阈值"与 +inf 一样成立。
///
/// 这与法线锥的 -2 哨兵、可见性编号的 +1 是同一个思路: 让退化情形落在合法值
/// 上、落在安全的一侧, 而不是靠一个分支。
const float kLodInfiniteError = 1.0e30;

/// 一个 meshlet 的 LOD 记录 —— 48 字节, 与 C++ 侧 FLodRecordGpu 逐字段对齐
struct MeshletLod
{
    vec4  selfSphere;     //  0..15  产出它的那个组的 LOD 球
    vec4  parentSphere;   // 16..31  将把它简化掉的那个组的 LOD 球
    float selfError;      // 32..35  相对**原始**表面的偏差上界
    float parentError;    // 36..39  同上; 根是 kLodInfiniteError
    uint  sourceGroup;    // 40..43  产出组 (诊断/着色用)
    uint  targetGroup;    // 44..47  消费组
};

/// 把世界空间误差投成屏幕上的像素偏差
///
/// d 取**球面最近点**到相机的距离, 并钳到近平面。
///
///   取最近点: 误差发生在球面上的某处, 取最近点让结果只会偏大 —— 而偏大是
///             "更早换到细的一层", 安全的一侧。用球心的话分母偏大、屏幕误差
///             偏小, 同样距离上会选到更粗的层。那条错**保住了单调性与割性质**,
///             所以覆盖判据看不见它 —— 要靠"逐 meshlet 兑现屏幕误差"才抓得住
///             (实测那条变异是 111 次破约、最坏 6.3 倍阈值)。
///
///   钳近平面: 不钳的话相机进球时 d -> 0, 屏幕误差 -> +inf, 而两个 +inf 之间
///             没有严格大小关系。近平面之内本来就不画东西, 所以钳在这里不改变
///             任何决策, 却让严格性在最显眼的时候 (相机贴近) 仍然成立。
///
/// 保守性: 半径 e 的误差球在球心距 D 处的精确投影半径是 e/sqrt(D²-e²), 而这里
/// 的分母是 D-r。只有 r >= e 时 D-r <= sqrt(D²-e²), 结果才只会偏大 —— 而
/// r >= e 是建 DAG 时钉死的不变量。
float MeshletLodProjectError(vec4 worldSphere, float worldError,
                             vec3 cameraPosition, float lodScale,
                             float nearPlane)
{
    const float distance =
        max(length(worldSphere.xyz - cameraPosition) - worldSphere.w, nearPlane);

    return worldError * lodScale / distance;
}

/// 选择规则: 自身误差 < 阈值 **且** 父误差 >= 阈值
///
/// 父侧用 >= 而不是 >: 阈值恰好等于某个组的屏幕误差时, 用 > 的话这个 meshlet
/// 落选, 而它的父需要"该值 < 阈值"也为假 —— **两个都不选, 洞**。只在恰好相等
/// 时出现, 随机阈值几乎测不到。
bool MeshletLodSelect(MeshletLod lod, vec3 cameraPosition, float lodScale,
                      float nearPlane, float threshold,
                      out float outSelfScreen, out float outParentScreen)
{
    outSelfScreen = MeshletLodProjectError(lod.selfSphere, lod.selfError,
                                           cameraPosition, lodScale, nearPlane);

    outParentScreen = MeshletLodProjectError(lod.parentSphere, lod.parentError,
                                             cameraPosition, lodScale,
                                             nearPlane);

    return outSelfScreen < threshold && outParentScreen >= threshold;
}

#endif // LIMX_MESHLET_LOD_H
