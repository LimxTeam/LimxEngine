// ============================================================
// 文件名称：meshlet_common.h
// 创建时间：2026-09-02
// 创建者  ：LimxTeam
// 设计哲学：剔除的判据、CPU 的参考实现、着色器里跑的那一份，必须是
//          **同一个公式的同一次书写**。三处各写一遍的话，判据比的是
//          "两个错误一不一样"，而不是"结果对不对"。
//          C++ 侧的参考实现 (--meshlet-cull-check) 是逐字照抄这里的，
//          抄的地方留了注释指回来。
// 功能描述：meshlet 的 GPU 表示，与视锥/背面两条剔除判据。
// ============================================================

#ifndef LIMX_MESHLET_COMMON_H
#define LIMX_MESHLET_COMMON_H

// ============================================================================
// 与 C++ 的 FMeshlet 逐字段一致 (48 字节)
// ============================================================================

struct Meshlet
{
    uint vertexOffset;
    uint vertexCount;
    uint triangleOffset;
    uint triangleCount;

    // xyz = 局部空间球心, w = 半径
    vec4 boundingSphere;

    // xyz = 单位轴, w = 半角余弦; w <= -2 表示无效锥
    vec4 normalCone;
};

// ============================================================================
// 一个实例
//
// 变换存成 3x4 行主序 —— 平移在第四列。用 mat4 的话每个实例多 16 字节,
// 而最后一行恒为 (0,0,0,1)。
// ============================================================================

struct MeshletInstance
{
    vec4 transformRow0;
    vec4 transformRow1;
    vec4 transformRow2;

    // x = meshlet 起点, y = meshlet 个数, z = 源对象下标, w = 保留
    uvec4 meshletRange;

    // 这个实例所属网格在三份汇总缓冲区里的起点:
    //   x = 顶点, y = meshlet 局部顶点表, z = meshlet 三角形 (以字节计)
    //
    // 用**逐实例基址**而不是在汇总时改写数据里的偏移量。改写要在 GPU 上
    // 再跑一遍重定位 (或者把数据搬回 CPU), 而基址只是每个实例多三个 uint,
    // 汇总本身仍然是纯粹的缓冲区拷贝。
    uvec4 bufferBases;
};

/// 法线锥无效的标记 —— 与 C++ 的 kInvalidConeCosine 同值
const float kInvalidConeCosine = -2.0;

// ============================================================================
// MeshletCullView — 逐视图的剔除参数
//
// 放 UBO 而不是 push constant: 六个平面 (96 字节) + 视图投影矩阵 (64) +
// 相机位置 (16) + 金字塔参数 (16) 是 192 字节, 而 Vulkan 只保证 push
// constant 有 **128 字节**。
//
// 在 RTX 3060 上塞得下 (它给 256), 但那是碰巧 —— 靠碰巧成立的东西会在
// 别人的机器上炸, 而那种炸法是 vkCreatePipelineLayout 直接失败, 看起来
// 像"这个引擎跑不起来"。
//
// 三个剔除着色器共用这一个结构体。分开写三份的话, 改了一处忘了另一处的
// 表现是"实例级与 meshlet 级用了不同的视锥" —— 而那只在物体压着视锥边界
// 时出现。
// ============================================================================

struct MeshletCullView
{
    // 六个视锥平面, xyz = 单位法线, w = 到原点的有符号距离
    vec4 planes[6];

    mat4 viewProj;

    vec4 cameraPosition;

    // x = 屏幕宽, y = 屏幕高, z = 金字塔最高级, w = 近裁剪面
    vec4 hizParams;
};

// ============================================================================
// 把局部空间的包围球变到世界空间
//
// 半径的缩放取三列长度的最大值。
//
// 这对 TRS 变换 (平移·旋转·缩放) 是**精确**的: 那时矩阵是 R·diag(s),
// 三列分别是 s_i·r_i 而 r_i 正交, 于是奇异值恰好是 |s_i|, 最大列长就是
// 谱范数。含切变的一般矩阵上它是**低估**, 会把球缩小 —— 那时必须换成
// Frobenius 范数 (一定不小于谱范数)。这个引擎的变换全是 TRS。
// ============================================================================

vec4 MeshletWorldSphere(vec4 localSphere, vec4 row0, vec4 row1, vec4 row2)
{
    const vec3 center = vec3(
        dot(row0, vec4(localSphere.xyz, 1.0)),
        dot(row1, vec4(localSphere.xyz, 1.0)),
        dot(row2, vec4(localSphere.xyz, 1.0)));

    const float scaleX = length(vec3(row0.x, row1.x, row2.x));
    const float scaleY = length(vec3(row0.y, row1.y, row2.y));
    const float scaleZ = length(vec3(row0.z, row1.z, row2.z));

    const float scale = max(scaleX, max(scaleY, scaleZ));

    return vec4(center, localSphere.w * scale);
}

/// 三个轴的缩放是不是一致 —— 不一致时法线锥不能直接旋转过去
bool MeshletUniformScale(vec4 row0, vec4 row1, vec4 row2)
{
    const float scaleX = length(vec3(row0.x, row1.x, row2.x));
    const float scaleY = length(vec3(row0.y, row1.y, row2.y));
    const float scaleZ = length(vec3(row0.z, row1.z, row2.z));

    const float maximum = max(scaleX, max(scaleY, scaleZ));
    const float minimum = min(scaleX, min(scaleY, scaleZ));

    // 千分之一的相对差之内算一致。取绝对差的话, 一个尺度 0.001 的物体
    // 会永远被判成一致, 一个尺度 1000 的会永远被判成不一致。
    return (maximum - minimum) <= maximum * 1.0e-3 + 1.0e-9;
}

// ============================================================================
// 视锥剔除 —— 球心到某个平面的有符号距离小于 -半径 时整球在背面
//
// 平面约定与 C++ 的 FPlane 一致: dot(n, p) + d < 0 是背面。
// 与 draw_cull.comp 里那一段是同一条判据 —— 两级剔除必须用同一个,
// 不然实例级留下的东西 meshlet 级可能全剔掉, 反过来也一样。
// ============================================================================

bool MeshletSphereVisible(vec4 worldSphere, vec4 planes[6])
{
    for (int p = 0; p < 6; ++p)
    {
        if (dot(planes[p].xyz, worldSphere.xyz) + planes[p].w
            < -worldSphere.w)
        {
            return false;
        }
    }

    return true;
}

// ============================================================================
// 背面剔除 —— 法线锥
//
// 判据: 从相机看过去, 这个 meshlet 的每一个三角形都背对着相机。
//
//   dot(d, axis) >= cutoff * length(d) + radius
//
// 其中 d 是球心减相机位置。左边是"球心方向与锥轴有多合", 右边把锥的
// 张角 (cutoff) 与球的大小 (radius) 都算进去 —— 球越大, 越可能有某个
// 位置能看到正面, 所以门槛越高。
//
// 两种情况必须**不剔**:
//   锥无效 (w <= -2): 法线散得超过半球, 任何方向都可能看到正面。
//   缩放不一致: 法线不能靠旋转变过去 (要用逆转置), 而那时锥的张角也变了。
//
// 这两条都是"存疑就不剔"。剔错的后果是画面上少一块, 而那与"这一块本来
// 就该被剔"长得完全一样 —— 没有症状的错误是最难查的那种。
//
// ── 曾经有第三条, 删掉了 ──
//
// 第一版还有一条 "相机在包围球内就不剔"。它是**可证明的死代码**:
//
//   dot(d, axis) <= |d| = distance                        (柯西-施瓦茨)
//   相机在球内时 distance <= radius
//   有效锥的 cutoff 恒为正 (见 FMeshlet::NormalCone 的哨兵约定),
//     所以 cutoff * distance + radius >= radius >= distance >= dot(d, axis)
//
// 于是那个 >= 永远不成立, 那一条 early-out 改不了任何结果。
//
// 是变异验证把它逼出来的: 把它删掉, 判据一动不动地绿。**一条不会红的
// 分支就是没有判据的分支** —— 留着它只会让下一个人以为它在起作用。
// 真要防的是"哪天让 cutoff 变成负数", 而那件事由 Day 8 的哨兵判据看着
// (余弦要么是无效标记, 要么严格为正)。
// ============================================================================

bool MeshletBackfaceCull(vec4 localCone, vec4 worldSphere,
                         vec3 cameraPosition, vec4 row0, vec4 row1, vec4 row2)
{
    if (localCone.w <= kInvalidConeCosine)
    {
        return false;
    }

    if (!MeshletUniformScale(row0, row1, row2))
    {
        return false;
    }

    // 均匀缩放下法线只需旋转 —— 把轴按三列方向变过去再归一化
    const vec3 axis = normalize(vec3(
        dot(row0.xyz, localCone.xyz),
        dot(row1.xyz, localCone.xyz),
        dot(row2.xyz, localCone.xyz)));

    const vec3 d = worldSphere.xyz - cameraPosition;

    const float distance = length(d);

    return dot(d, axis) >= localCone.w * distance + worldSphere.w;
}

// ============================================================================
// 遮挡剔除 —— 层次深度金字塔
//
// 把世界包围球投到屏幕上得到一个矩形与一个**最近**深度, 然后在金字塔上
// 挑一级 (那一级的一个纹素盖得住整个矩形), 取那几个纹素的最大值。
// 包围球的最近深度比那个最大值还远, 就说明矩形覆盖的每个像素上都有更近的
// 遮挡物 —— 全挡住了。
//
// ── 四种情况必须**不剔** ──
//
//   球与近平面相交:  投影在近平面附近发散, 算出来的矩形没有意义。
//   矩形退化:        宽或高为零 (球退化、或者刚好在边界上)。
//   金字塔无效:      第一帧还没有上一帧的深度可用。
//   相机在球内:      整个屏幕都是它, 而它自己就是最近的东西。
//
// 与背面剔除同一个原则: **存疑就不剔**。剔错的后果是画面上少一块, 而那与
// "这一块本来就该被剔"长得完全一样 —— 没有症状的错误是最难查的那种。
//
// 而与背面剔除不同的是, 这里的"存疑"不是保守到浪费: 两阶段剔除会把第一
// 阶段剔掉的东西在新的金字塔上**再测一遍**, 所以第一阶段宁可多留。
// ============================================================================

/// 把世界包围球投成屏幕矩形与最近深度
///
/// 返回是否投影成功 (失败时调用方必须**不剔**)。
/// outRect = (minX, minY, maxX, maxY), 单位是纹素; outNearest = NDC 深度。
// 单个轴上的切线斜率
//
// 在"这个轴 + 相机前向"张成的平面里, 球退化成一个半径不变的圆 (可达的
// (u·a, u·f) 就是单位圆盘), 于是 viewX/viewZ 在球上的极值**恰好**是从
// 原点向这个圆作的两条切线的斜率。这是精确解, 不是近似。
//
// 切点 P± = (t / |C|²) · (t·C ± r·C⊥), 其中 C = (lateral, depth),
// C⊥ = (-depth, lateral), t = sqrt(|C|² - r²)。取比值时 |C|² 约掉。
bool MeshletSphereAxisSlopes(float lateral, float depth, float radius,
                             out vec2 outSlopes)
{
    outSlopes = vec2(0.0);

    const float squared = lateral * lateral + depth * depth - radius * radius;

    // 原点落在圆内 —— 这个轴上的投影无界
    if (squared <= 0.0)
    {
        return false;
    }

    const float t = sqrt(squared);

    const float lowerNumerator   = t * lateral - radius * depth;
    const float lowerDenominator = t * depth + radius * lateral;

    const float upperNumerator   = t * lateral + radius * depth;
    const float upperDenominator = t * depth - radius * lateral;

    // 切点跑到相机后面 —— 同样无界
    if (lowerDenominator <= 0.0 || upperDenominator <= 0.0)
    {
        return false;
    }

    outSlopes = vec2(lowerNumerator / lowerDenominator,
                     upperNumerator / upperDenominator);

    return true;
}

bool MeshletProjectSphere(vec4 worldSphere, mat4 viewProj, vec3 cameraPosition,
                          vec2 screenSize, float nearPlane,
                          out vec4 outRect, out float outNearest)
{
    outRect    = vec4(0.0);
    outNearest = 0.0;

    const vec3 delta = worldSphere.xyz - cameraPosition;

    // ── 这里原来还有一条"相机在球内就返回失败" ──
    //
    // 它是死的, 被下面那条近平面判断完全盖住: 相机在球内意味着 |delta| < r,
    // 而球心视深度 = dot(delta, forward) <= |delta| < r, 于是
    // centerClip.w - r < 0 <= nearPlane 恒成立, 下面那条必然先返回。
    //
    // 变异验证是这么发现的: 把它改成恒不成立, 判据一点反应都没有 ——
    // **一条不会红的分支就是没有判据的分支**, 而这一条连红的可能都没有。
    // 留着它只会让人以为那种情形被专门处理过。

    // 屏幕矩形: 逐轴解切线, **精确**包围。
    //
    // ── 原来那个近似是错的 ──
    //
    // 第一版取球心沿相机右/上各推一个半径的四个点, 投影之后取包围盒, 并且
    // 注释里写着"它一定包得住"。它包不住: 距离 d 半径 r 的球张开的角是
    // asin(r/d), 而那四个点张开的角只有 atan(r/d) —— 球越近越大, 差得越多。
    //
    // 探针判据量出来的数: 304 个探针里 203 个的矩形包不住真实投影, 最多缺
    // 977 个像素。而画面判据对此**全绿** —— 因为包不住的后果是第一阶段错剔,
    // 而第二阶段又把它补了回来。真实场景里 meshlet 的包围球相对距离都很小,
    // 误差落在亚像素上, 于是这个错在画面上从来没露过头。
    //
    // 现在的做法在每个轴上解从相机到球的切线, 那是精确的极值。
    //
    // ── 相机基从矩阵里取 ──
    //
    // 第 0 行 = P00·右, 第 1 行 = P11·上, 第 3 行 = 前 (clip.w 就是视深度,
    // 所以第 3 行是单位长的前向)。
    //
    // **GLSL 的 m[i] 取的是第 i 列, 不是第 i 行** —— 与 row_major 这个
    // 存储限定符无关: 那个限定符只说数据在内存里怎么摆, 不改变下标的
    // 语义。写成 viewProj[0].xyz 拿到的是第一列, 而第一列与相机的右方向
    // 毫无关系。
    //
    // 这个错不崩、不报错: 投出来的矩形只是"不对", 而遮挡测试拿一个不对的
    // 矩形去查金字塔, 结果是**几乎什么都剔不掉** —— 看起来像"这个场景就是
    // 没什么遮挡"。实测五种相机摆位下被剔的数量恒为 3, 而测试数从 2 到
    // 201 都有 —— 那个"恒为 3"才是线索。
    const vec3 right = normalize(vec3(viewProj[0][0], viewProj[1][0],
                                      viewProj[2][0]));

    const vec3 up = normalize(vec3(viewProj[0][1], viewProj[1][1],
                                   viewProj[2][1]));

    const vec3 forward = vec3(viewProj[0][3], viewProj[1][3], viewProj[2][3]);

    const float centerDepth = dot(delta, forward);

    // 球的最前端穿到近平面之内 —— 投影发散, 存疑不剔
    //
    // 球在视深度方向的范围恰好是 [centerDepth - r, centerDepth + r], 所以
    // 这一句就是"最前端在近平面之内"。
    //
    // ── 这条判断原来写了两遍 ──
    //
    // 一遍在最前面拿 centerClip.w 判, 一遍在最后拿最近点的裁剪 w 判。而
    // centerClip.w 恒等于 centerDepth (相机自己的 w 是 0, 所以矩阵最后一行
    // 点乘 delta 就是视深度), 最近点的裁剪 w 恒等于 centerDepth - r ——
    // **两句是同一个条件**。
    //
    // 于是把前一句改成恒不成立, 判据毫无反应: 后一句照样拦下来。变异验证
    // 就是这么发现的。删掉一句, 留下的这句才有判据可言。
    if (centerDepth - worldSphere.w <= nearPlane)
    {
        return false;
    }

    vec2 slopesX;
    vec2 slopesY;

    if (!MeshletSphereAxisSlopes(dot(delta, right), centerDepth, worldSphere.w,
                                 slopesX) ||
        !MeshletSphereAxisSlopes(dot(delta, up), centerDepth, worldSphere.w,
                                 slopesY))
    {
        return false;
    }

    // 斜率 -> 归一化设备坐标。
    //
    // 不直接乘 P00/P11 是因为它们的**符号**从矩阵里取不出来 (取模长会把
    // 负号吃掉, 而 Vulkan 的 Y 常常是翻的)。造一个"视空间横向比值正好等于
    // 这个斜率"的世界点再投影, 符号就由矩阵自己带出来了。
    vec2 minimum = vec2(1.0e30);
    vec2 maximum = vec2(-1.0e30);

    for (int i = 0; i < 2; ++i)
    {
        const vec3 pointX = cameraPosition + forward * centerDepth +
                            right * (slopesX[i] * centerDepth);

        const vec3 pointY = cameraPosition + forward * centerDepth +
                            up * (slopesY[i] * centerDepth);

        const vec4 clipX = viewProj * vec4(pointX, 1.0);
        const vec4 clipY = viewProj * vec4(pointY, 1.0);

        if (clipX.w <= nearPlane || clipY.w <= nearPlane)
        {
            return false;
        }

        const float ndcX = clipX.x / clipX.w;
        const float ndcY = clipY.y / clipY.w;

        minimum = min(minimum, vec2(ndcX, ndcY));
        maximum = max(maximum, vec2(ndcX, ndcY));
    }

    const vec2 lower = (minimum * 0.5 + 0.5) * screenSize;
    const vec2 upper = (maximum * 0.5 + 0.5) * screenSize;

    outRect = vec4(lower, upper);

    // 最近深度: 视深度最小的那一点。
    //
    // 标准透视矩阵的 clip.z/clip.w 只依赖视深度, 而且随它单调, 所以球上
    // 深度最小的点就是视深度最小的点 —— 视深度 = 球心视深度 - 半径。
    //
    // ── 原来那个也是错的 ──
    //
    // 第一版取"球面上离相机最近的那一点" (球心沿视线往回挪一个半径)。那一点
    // 的**视深度**是 centerDepth - r·cos(夹角), 只有球正对着相机时才等于
    // centerDepth - r; 偏到一侧就偏大, 而偏大就是把没挡住的判成挡住了。
    // 探针量出来 255 个投影成功的探针里 156 个中招, 最多偏 0.0076。
    const vec3 nearestPoint =
        cameraPosition + forward * (centerDepth - worldSphere.w);

    const vec4 nearestClip = viewProj * vec4(nearestPoint, 1.0);

    outNearest = nearestClip.z / nearestClip.w;

    return true;
}

// ============================================================================
// 遮挡测试 —— 在金字塔上挑一级, 比最大深度
//
// 挑哪一级: 那一级的**一个纹素**要盖得住整个屏幕矩形。于是取矩形较长边的
// 以 2 为底的对数, 向上取整。
//
// 挑小了 (级数低、纹素细) 要采很多纹素才盖得住矩形; 挑大了 (纹素粗) 那个
// 纹素覆盖的范围远大于矩形, 里面混进了矩形之外的遮挡信息 —— 而混进来的
// 只会让最大值变大, 于是更不容易剔。**保守的方向**, 不会错剔。
//
// 采 2x2 而不是 1 个: 矩形可能横跨两个纹素的边界。采一个的话那时盖不住,
// 而盖不住就可能错剔。
// ============================================================================

// 金字塔查询: 矩形覆盖范围内的最大深度
//
// 与判定分开是给**探针**用的 —— 判据要单独验这个最大值的保守性
// (查到的值不许小于矩形范围内第 0 级的真实最大值)。合在一起的话这个中间
// 量在外面看不见, 而它正是几种错法的落点: 取最小、漏收奇数尺寸的最后
// 一行、挑级数向下取整、只采一个纹素 —— 全都只让这个数偏小, 而偏小的
// 后果是**错剔**。
//
// 返回 -1 表示矩形退化, 调用方按"不剔"处理。
float MeshletHizMaxDepth(vec4 rect, sampler2D hiz, vec2 hizSize,
                         float maxLevel, out float outLevel)
{
    outLevel = 0.0;

    const vec2 extent = rect.zw - rect.xy;

    // 退化矩形 —— 存疑就不剔
    if (extent.x <= 0.0 || extent.y <= 0.0)
    {
        return -1.0;
    }

    const float level =
        clamp(ceil(log2(max(extent.x, extent.y))), 0.0, maxLevel);

    outLevel = level;

    const vec2 levelSize = max(floor(hizSize / exp2(level)), vec2(1.0));

    // 矩形在这一级上的纹素范围
    const vec2 lower = floor(rect.xy / exp2(level));
    const vec2 upper = floor(rect.zw / exp2(level));

    float maximum = 0.0;

    for (int dy = 0; dy < 2; ++dy)
    {
        for (int dx = 0; dx < 2; ++dx)
        {
            const vec2 coord = clamp(
                vec2(dx == 0 ? lower.x : upper.x,
                     dy == 0 ? lower.y : upper.y),
                vec2(0.0), levelSize - vec2(1.0));

            maximum = max(maximum,
                          texelFetch(hiz, ivec2(coord), int(level)).x);
        }
    }

    return maximum;
}

bool MeshletHizOccluded(vec4 rect, float nearest, sampler2D hiz,
                        vec2 hizSize, float maxLevel)
{
    float level = 0.0;

    const float maximum =
        MeshletHizMaxDepth(rect, hiz, hizSize, maxLevel, level);

    if (maximum < 0.0)
    {
        return false;
    }

    // 包围球最近的那一点都比这片区域里最远的遮挡物还远 —— 全挡住了。
    //
    // 用严格大于而不是大于等于: 相等时那个包围体**就是**那个遮挡物
    // (自己挡自己), 剔掉它画面上就少一块。
    return nearest > maximum;
}

#endif // LIMX_MESHLET_COMMON_H
