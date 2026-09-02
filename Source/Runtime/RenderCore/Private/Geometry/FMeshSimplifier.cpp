// ============================================================
// 文件名称：FMeshSimplifier.cpp
// 创建时间：2026-09-02
// 创建者  ：LimxTeam
// 功能描述：FMeshSimplifier 的实现 — QEM 边坍缩。
// ============================================================

#include "RenderCore/RenderCoreMinimal.h"
#include "RenderCore/Geometry/FMeshSimplifier.h"

#include "Core/Containers/TArray.h"
#include "Core/Math/FMath.h"

namespace Limx
{

LIMX_DEFINE_LOG_CATEGORY(LogMeshSimplifier)

namespace
{

// ============================================================================
// 二次型 —— 对称 4x4 只存上三角的 10 个数
//
// 用 Float64 累加。二次误差是"平面到点的距离平方"的和, 一个顶点周围几十个
// 三角形加起来, 量级跨得很开; Float32 在坍缩到后期 (误差已经很小) 时会把
// 小量吃掉, 表现为"误差报 0 而实际偏了一截" —— 而误差是上界这件事正是
// 下游 LOD 选择的全部依据。
// ============================================================================

struct FQuadric
{
    Float64 M[10] = {};

    void AddPlane(Float64 a, Float64 b, Float64 c, Float64 d, Float64 weight)
    {
        M[0] += weight * a * a;
        M[1] += weight * a * b;
        M[2] += weight * a * c;
        M[3] += weight * a * d;
        M[4] += weight * b * b;
        M[5] += weight * b * c;
        M[6] += weight * b * d;
        M[7] += weight * c * c;
        M[8] += weight * c * d;
        M[9] += weight * d * d;
    }

    void Add(const FQuadric& other)
    {
        for (UInt32 i = 0; i < 10; ++i)
        {
            M[i] += other.M[i];
        }
    }

    /// v^T Q v —— 点到这一族平面的加权距离平方和
    LIMX_NODISCARD Float64 Evaluate(Float64 x, Float64 y, Float64 z) const
    {
        return M[0] * x * x + 2.0 * M[1] * x * y + 2.0 * M[2] * x * z +
               2.0 * M[3] * x + M[4] * y * y + 2.0 * M[5] * y * z +
               2.0 * M[6] * y + M[7] * z * z + 2.0 * M[8] * z + M[9];
    }
};

/// 解 Q 的最优点 (梯度为零处)。奇异时返回 false, 调用方退回候选点。
bool SolveOptimalPosition(const FQuadric& q, Float64& outX, Float64& outY,
                          Float64& outZ)
{
    // 3x3 线性方程组: [a  b  c][x]   [-d]
    //                 [b  e  f][y] = [-g]
    //                 [c  f  h][z]   [-i]
    const Float64 a = q.M[0];
    const Float64 b = q.M[1];
    const Float64 c = q.M[2];
    const Float64 d = q.M[3];
    const Float64 e = q.M[4];
    const Float64 f = q.M[5];
    const Float64 g = q.M[6];
    const Float64 h = q.M[7];
    const Float64 i = q.M[8];

    const Float64 det = a * (e * h - f * f) - b * (b * h - f * c) +
                        c * (b * f - e * c);

    // 阈值不是"很小就当奇异", 而是与矩阵量级比。平面几乎共面时 det 天然
    // 就小, 那时解出来的点会飞到很远的地方 —— 而那个点的误差算出来仍然
    // 很小 (它确实离那族平面都近), 于是判据看不出问题, 画面上却是一根刺。
    Float64 scale = 0.0;

    for (UInt32 k = 0; k < 10; ++k)
    {
        scale = FMath::Max(scale, (q.M[k] < 0.0) ? -q.M[k] : q.M[k]);
    }

    if (scale <= 0.0 || ((det < 0.0) ? -det : det) < 1.0e-10 * scale * scale * scale)
    {
        return false;
    }

    const Float64 inverseDet = 1.0 / det;

    // 伴随矩阵求逆后乘 -(d, g, i)
    const Float64 i00 = (e * h - f * f) * inverseDet;
    const Float64 i01 = (c * f - b * h) * inverseDet;
    const Float64 i02 = (b * f - c * e) * inverseDet;
    const Float64 i11 = (a * h - c * c) * inverseDet;
    const Float64 i12 = (b * c - a * f) * inverseDet;
    const Float64 i22 = (a * e - b * b) * inverseDet;

    outX = -(i00 * d + i01 * g + i02 * i);
    outY = -(i01 * d + i11 * g + i12 * i);
    outZ = -(i02 * d + i12 * g + i22 * i);

    return true;
}

// ============================================================================
// 懒惰失效的二叉堆
//
// 一次坍缩会改掉邻域里几十条边的代价。真去堆里找到它们再调整, 要么维护
// 反向下标 (一堆状态), 要么线性扫 (退化成 O(n^2))。懒惰失效的做法是:
// 旧条目留在堆里不管, 弹出时拿版本号一比, 过期的直接丢。
// ============================================================================

struct FCollapseCandidate
{
    Float64 Cost    = 0.0;
    UInt32  V0      = 0;
    UInt32  V1      = 0;
    UInt32  Version = 0;
};

class FCandidateHeap
{
public:
    void Push(const FCollapseCandidate& candidate)
    {
        m_Items.Add(candidate);

        SizeType child = m_Items.GetSize() - 1;

        while (child > 0)
        {
            const SizeType parent = (child - 1) / 2;

            if (m_Items[parent].Cost <= m_Items[child].Cost)
            {
                break;
            }

            Swap(parent, child);
            child = parent;
        }
    }

    LIMX_NODISCARD bool IsEmpty() const { return m_Items.IsEmpty(); }

    LIMX_NODISCARD FCollapseCandidate Pop()
    {
        const FCollapseCandidate top = m_Items[0];

        m_Items[0] = m_Items[m_Items.GetSize() - 1];
        m_Items.RemoveAt(m_Items.GetSize() - 1);

        SizeType parent = 0;

        while (true)
        {
            const SizeType left  = parent * 2 + 1;
            const SizeType right = left + 1;

            SizeType smallest = parent;

            if (left < m_Items.GetSize() &&
                m_Items[left].Cost < m_Items[smallest].Cost)
            {
                smallest = left;
            }

            if (right < m_Items.GetSize() &&
                m_Items[right].Cost < m_Items[smallest].Cost)
            {
                smallest = right;
            }

            if (smallest == parent)
            {
                break;
            }

            Swap(parent, smallest);
            parent = smallest;
        }

        return top;
    }

private:
    void Swap(SizeType a, SizeType b)
    {
        const FCollapseCandidate temporary = m_Items[a];

        m_Items[a] = m_Items[b];
        m_Items[b] = temporary;
    }

    TArray<FCollapseCandidate> m_Items;
};

// ============================================================================
// 焊接
// ============================================================================

/// 三个浮点的哈希 —— 只在容差为 0 时用得上
UInt32 HashPosition(const FVector3& position)
{
    const auto* bits = reinterpret_cast<const UInt32*>(&position);

    UInt32 hash = 2166136261u;

    for (UInt32 i = 0; i < 3; ++i)
    {
        hash ^= bits[i];
        hash *= 16777619u;
    }

    return hash;
}

/// 按位置把顶点焊到一起, 返回 原顶点 -> 焊接顶点 的映射
void WeldByPosition(const TArray<FMeshVertex>& vertices, Float32 tolerance,
                    TArray<UInt32>& outRemap, TArray<FVector3>& outPositions)
{
    outRemap.Clear();
    outRemap.Reserve(vertices.GetSize());
    outPositions.Clear();

    if (tolerance <= 0.0f)
    {
        // 逐位相同才焊 —— 走哈希表, 线性时间
        constexpr UInt32 kSlotCount = 1024;

        TArray<TArray<UInt32>> buckets;
        buckets.SetSize(static_cast<SizeType>(kSlotCount));

        for (SizeType i = 0; i < vertices.GetSize(); ++i)
        {
            const FVector3& position = vertices[i].Position;

            const UInt32 slot = HashPosition(position) & (kSlotCount - 1u);

            UInt32 found = 0xFFFFFFFFu;

            for (SizeType k = 0; k < buckets[slot].GetSize(); ++k)
            {
                const UInt32 candidate = buckets[slot][k];

                if (outPositions[candidate].X == position.X &&
                    outPositions[candidate].Y == position.Y &&
                    outPositions[candidate].Z == position.Z)
                {
                    found = candidate;
                    break;
                }
            }

            if (found == 0xFFFFFFFFu)
            {
                found = static_cast<UInt32>(outPositions.GetSize());
                outPositions.Add(position);
                buckets[slot].Add(found);
            }

            outRemap.Add(found);
        }

        return;
    }

    // 有容差时退回 O(n^2) —— 容差焊接只在导入阶段用一次, 而且哈希分桶在
    // 容差跨桶时会漏配, 那种漏配的表现是"某几个点没焊上", 比慢更糟。
    const Float32 toleranceSquared = tolerance * tolerance;

    for (SizeType i = 0; i < vertices.GetSize(); ++i)
    {
        const FVector3& position = vertices[i].Position;

        UInt32 found = 0xFFFFFFFFu;

        for (SizeType k = 0; k < outPositions.GetSize(); ++k)
        {
            if ((outPositions[k] - position).LengthSquared() <=
                toleranceSquared)
            {
                found = static_cast<UInt32>(k);
                break;
            }
        }

        if (found == 0xFFFFFFFFu)
        {
            found = static_cast<UInt32>(outPositions.GetSize());
            outPositions.Add(position);
        }

        outRemap.Add(found);
    }
}

// ============================================================================
// 简化的内部状态
// ============================================================================

struct FSimplifyState
{
    TArray<FVector3> Positions;
    TArray<FQuadric> Quadrics;

    /// 三角形的三个焊接顶点下标; 死掉的三角形三个下标全置 0xFFFFFFFF
    TArray<UInt32> Triangles;

    /// 每个三角形**最初**的单位法线
    ///
    /// 逐步的翻转检查 (这一次坍缩前后法线不许掉头) 拦不住慢慢转过去的:
    /// 每一步都只转几度、点积始终为正, 几十步下来就转过了九十度。表现是
    /// 一片"鳍" —— 三角形立在表面上而不是贴着表面。
    ///
    /// 实测: UV 球简化到一成, 二十一个三角形的三个顶点全落在接缝那条经线
    /// 上 (z 恰好为 0), 整片立在子午面里, 与脚下表面的法线点积在 -0.03 到
    /// 0 之间 —— 正好是"立着"。
    TArray<FVector3> OriginalNormals;

    /// 每个顶点关联的三角形下标
    TArray<TArray<UInt32>> Incident;

    /// 顶点版本 —— 懒惰堆用它判过期
    TArray<UInt32> Version;

    /// 顶点是否被锁住 (开边界)
    TArray<UInt8> Locked;

    /// 每个顶点的**几何偏差**
    ///
    /// 含义: 所有被并进这个顶点的原始顶点, 到这个顶点当前位置的最大距离。
    ///
    /// ── 为什么不能拿二次误差当它 ──
    ///
    /// 二次误差是"到一族平面的加权距离平方和", 面积加权之后量纲是
    /// 距离² × 面积, 开方出来根本不是一个距离。第一版就是拿它开方当误差
    /// 报出去的, 判据当场量出来: 球体简化一半, 实际最大偏差 0.0129 而报
    /// 出去的是 0.000656 —— 差了二十倍, 而且是**报小**的方向。
    ///
    /// 二次误差是个好的**排序**启发式 (它确实知道哪条边坍缩起来"更不亏"),
    /// 但它不是界。界要单独记。
    ///
    /// ── 量的是"到面的距离", 不是"到代表顶点的距离" ──
    ///
    /// 中间还错过一版: 记"被并进来的原始点到代表顶点的最大距离"。那个数
    /// 是界, 但量错了东西 —— 一块**平面**简化一半, 顶点沿着面滑得很远而
    /// 面本身分毫未变, 真实偏差是 0, 那一版却报 1.92 (整块平面的尺度)。
    /// 判据当场把它顶了出来: 松了三千万倍。
    ///
    /// LOD 要的是"表面偏了多少", 不是"顶点滑了多远"。所以量的是每个原始
    /// 点到**当前局部三角形**的最短距离: 平面上恒为 0, 曲面上才长起来。
    ///
    /// 它仍然是界: 只在局部三角形里取最小, 而真实的最近三角形可能在别处 ——
    /// 那只会让真实距离更小, 于是这个数只会更大, 方向是对的。
    ///
    /// 与上一次的值取大: 表面越简化越粗, 距离只增不减, 而别的簇不会因为
    /// 这次坍缩重新量一遍。取大是那一步的补偿。
    TArray<Float32> Deviation;

    /// 原始位置 (焊接之后、任何坍缩之前)
    TArray<FVector3> OriginalPositions;

    /// 簇的侵入式链表 —— 头、尾、下一个
    TArray<UInt32> ClusterHead;
    TArray<UInt32> ClusterTail;
    TArray<UInt32> ClusterNext;

    UInt32 AliveTriangles = 0;
};

LIMX_NODISCARD bool IsTriangleAlive(const FSimplifyState& state, UInt32 tri)
{
    return state.Triangles[tri * 3] != 0xFFFFFFFFu;
}

/// 三角形的面积加权法线 (未归一化, 长度是两倍面积)
FVector3 TriangleAreaNormal(const FSimplifyState& state, UInt32 tri)
{
    const FVector3& a = state.Positions[state.Triangles[tri * 3 + 0]];
    const FVector3& b = state.Positions[state.Triangles[tri * 3 + 1]];
    const FVector3& c = state.Positions[state.Triangles[tri * 3 + 2]];

    return FVector3::Cross(b - a, c - a);
}

/// 点到三角形的最短距离平方
///
/// 判据那边也有一份 —— 两份是**故意**的。判据拿被验的实现自己算一遍,
/// 验的就只剩"这段代码等于它自己"。
Float32 PointTriangleDistanceSquared(const FVector3& p, const FVector3& a,
                                     const FVector3& b, const FVector3& c)
{
    const FVector3 ab = b - a;
    const FVector3 ac = c - a;
    const FVector3 ap = p - a;

    const Float32 d1 = FVector3::Dot(ab, ap);
    const Float32 d2 = FVector3::Dot(ac, ap);

    if (d1 <= 0.0f && d2 <= 0.0f)
    {
        return (p - a).LengthSquared();
    }

    const FVector3 bp = p - b;

    const Float32 d3 = FVector3::Dot(ab, bp);
    const Float32 d4 = FVector3::Dot(ac, bp);

    if (d3 >= 0.0f && d4 <= d3)
    {
        return (p - b).LengthSquared();
    }

    const Float32 vc = d1 * d4 - d3 * d2;

    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
    {
        const Float32 denominator = d1 - d3;
        const Float32 v = (denominator != 0.0f) ? (d1 / denominator) : 0.0f;

        return (p - (a + ab * v)).LengthSquared();
    }

    const FVector3 cp = p - c;

    const Float32 d5 = FVector3::Dot(ab, cp);
    const Float32 d6 = FVector3::Dot(ac, cp);

    if (d6 >= 0.0f && d5 <= d6)
    {
        return (p - c).LengthSquared();
    }

    const Float32 vb = d5 * d2 - d1 * d6;

    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
    {
        const Float32 denominator = d2 - d6;
        const Float32 w = (denominator != 0.0f) ? (d2 / denominator) : 0.0f;

        return (p - (a + ac * w)).LengthSquared();
    }

    const Float32 va = d3 * d6 - d5 * d4;

    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
    {
        const Float32 denominator = (d4 - d3) + (d5 - d6);
        const Float32 w =
            (denominator != 0.0f) ? ((d4 - d3) / denominator) : 0.0f;

        return (p - (b + (c - b) * w)).LengthSquared();
    }

    const Float32 denominator = va + vb + vc;

    if (denominator == 0.0f)
    {
        return (p - a).LengthSquared();
    }

    const Float32 v = vb / denominator;
    const Float32 w = vc / denominator;

    return (p - (a + ab * v + ac * w)).LengthSquared();
}

/// 标出只属于一个三角形的边上的顶点
void MarkOpenBoundary(FSimplifyState& state)
{
    // 统计每条有向边出现的次数。一条无向边被两个三角形共享时, 两个方向
    // 各出现一次; 只出现一次的那条就是开边界。
    //
    // 用"每个顶点一张小表"而不是全局哈希表: 顶点的邻接度是个位数, 线性
    // 查找比哈希快, 而且不必处理冲突。
    TArray<TArray<UInt32>> outgoing;
    outgoing.SetSize(state.Positions.GetSize());

    const SizeType triangleCount = state.Triangles.GetSize() / 3;

    for (SizeType t = 0; t < triangleCount; ++t)
    {
        if (!IsTriangleAlive(state, static_cast<UInt32>(t)))
        {
            continue;
        }

        for (UInt32 e = 0; e < 3; ++e)
        {
            const UInt32 from = state.Triangles[t * 3 + e];
            const UInt32 to   = state.Triangles[t * 3 + (e + 1) % 3];

            outgoing[from].Add(to);
        }
    }

    for (SizeType v = 0; v < outgoing.GetSize(); ++v)
    {
        for (SizeType k = 0; k < outgoing[v].GetSize(); ++k)
        {
            const UInt32 other = outgoing[v][k];

            bool hasOpposite = false;

            for (SizeType m = 0; m < outgoing[other].GetSize(); ++m)
            {
                if (outgoing[other][m] == static_cast<UInt32>(v))
                {
                    hasOpposite = true;
                    break;
                }
            }

            if (!hasOpposite)
            {
                state.Locked[v]     = 1;
                state.Locked[other] = 1;
            }
        }
    }
}

/// 坍缩之后会不会有三角形翻转或退化
///
/// 只看**保留下来**的三角形: 同时含 v0 与 v1 的那些会在坍缩里消失, 不必看。
bool WouldFlip(const FSimplifyState& state, UInt32 v0, UInt32 v1,
               const FVector3& target)
{
    for (UInt32 side = 0; side < 2; ++side)
    {
        const UInt32 vertex = (side == 0) ? v0 : v1;
        const UInt32 other  = (side == 0) ? v1 : v0;

        for (SizeType k = 0; k < state.Incident[vertex].GetSize(); ++k)
        {
            const UInt32 tri = state.Incident[vertex][k];

            if (!IsTriangleAlive(state, tri))
            {
                continue;
            }

            const UInt32 i0 = state.Triangles[tri * 3 + 0];
            const UInt32 i1 = state.Triangles[tri * 3 + 1];
            const UInt32 i2 = state.Triangles[tri * 3 + 2];

            // 含另一端的三角形会被这次坍缩吃掉
            if (i0 == other || i1 == other || i2 == other)
            {
                continue;
            }

            FVector3 p[3] = {
                state.Positions[i0],
                state.Positions[i1],
                state.Positions[i2],
            };

            const FVector3 before = FVector3::Cross(p[1] - p[0], p[2] - p[0]);

            for (UInt32 c = 0; c < 3; ++c)
            {
                if (state.Triangles[tri * 3 + c] == vertex)
                {
                    p[c] = target;
                }
            }

            const FVector3 after = FVector3::Cross(p[1] - p[0], p[2] - p[0]);

            // ── 这里原来还有一条"面积塌成零就拒绝" ──
            //
            // 它被下面那条纵横比检查完全盖住了: 面积为零时纵横比也是零,
            // 而零一定小于 0.05, 也一定小于坍缩前的值。变异验证证实了这
            // 一点 —— 把那一条删掉, 判据毫无反应。
            //
            // 留着一条不会红的分支等于留一处没有判据的地方, 所以删掉。

            // 翻转: 这一次坍缩就让法线掉头。
            //
            // 它与下面"锚住最初法线"那条有很大重叠 —— 变异验证里把这一条
            // 单独删掉, 判据不红。原因是几何上的: 两者都要求离最初的朝向
            // 不超过 60 度, 于是坍缩前后最多差 120 度, 单步掉头 (差 180 度)
            // 的余地很窄。
            //
            // 留着不是因为它有用武之地 —— 是因为它**便宜**而且方向正确:
            // 60 度那个余量将来若为了简化率放宽 (比如放到 85 度), 单步掉头
            // 的空间立刻打开, 而那时这一条就是唯一的拦截。删掉它等于把一个
            // 将来要用的护栏提前拆了。
            if (FVector3::Dot(before, after) <= 0.0f)
            {
                return true;
            }

            // 不许离**最初**的朝向超过九十度。
            //
            // 逐步检查只看这一次坍缩前后, 拦不住慢慢转过去的 —— 每步几度,
            // 几十步就立起来了。拿最初的法线当锚, 那条路就被堵死了。
            const FVector3& originalNormal = state.OriginalNormals[tri];

            // 阈值取 cos 60 度而不是 cos 90 度。
            //
            // 卡在 90 度是不够的: 三角形转到 89 度仍然放行, 而那时它已经
            // 基本立在表面上了 —— 判据量到的那批"翻面"三角形与脚下表面的
            // 点积在 -0.03 到 0 之间, 正是 90 度出头一点点。留 30 度的余量
            // 才挡得住。
            //
            // 代价是简化会早一点停 (更多坍缩被拒)。实测球体到一成仍然到得了
            // 目标, 所以这个余量是付得起的。
            if (originalNormal.LengthSquared() > 0.0f &&
                FVector3::Dot(after.GetSafeNormal(), originalNormal) < 0.5f)
            {
                return true;
            }

            // 不许**新造**细条三角形。
            //
            // 二次误差天生爱造细条: 沿着一条长边坍缩, 误差可以很小, 而留下
            // 的三角形又长又薄。细条在画面上是走样的边, 在数值上是不可靠的
            // 法线 —— 判据当场量到过: 球体简化到一成, 二十一个三角形与脚下
            // 那片原始表面法线"反向", 而它们的点积全在 -0.03 到 0 之间,
            // 面积只有平均值的百分之一。那不是翻过来了, 是normal 已经没有
            // 意义了。
            //
            // 品质用"归一化纵横比"衡量: 4√3·面积 / 三边平方和, 正三角形是
            // 1, 越细越接近 0。只挡**变坏**的那些 —— 输入里本来就有的细条
            // (UV 球两极那一圈) 不因此卡住简化。
            const auto Aspect = [](const FVector3& p0, const FVector3& p1,
                                   const FVector3& p2) -> Float32
            {
                const Float32 sum = (p1 - p0).LengthSquared() +
                                    (p2 - p1).LengthSquared() +
                                    (p0 - p2).LengthSquared();

                if (sum <= 0.0f)
                {
                    return 0.0f;
                }

                const Float32 area =
                    FVector3::Cross(p1 - p0, p2 - p0).Length() * 0.5f;

                return 6.9282032f * area / sum;
            };

            const FVector3 originalPoints[3] = {
                state.Positions[i0],
                state.Positions[i1],
                state.Positions[i2],
            };

            const Float32 aspectBefore = Aspect(
                originalPoints[0], originalPoints[1], originalPoints[2]);

            const Float32 aspectAfter = Aspect(p[0], p[1], p[2]);

            if (aspectAfter < 0.05f && aspectAfter < aspectBefore)
            {
                return true;
            }
        }
    }

    return false;
}

} // namespace

// ============================================================================
// FMeshSimplifier::Simplify
// ============================================================================

FMeshSimplifyResult FMeshSimplifier::Simplify(
    const TArray<FMeshVertex>& vertices, const TArray<UInt32>& indices,
    const FMeshSimplifyOptions& options)
{
    FMeshSimplifyResult result;

    if (vertices.IsEmpty() || indices.GetSize() < 3 ||
        indices.GetSize() % 3 != 0)
    {
        LIMX_LOG(LogMeshSimplifier, Error,
                 "[简化器] 输入不合法 — 顶点 {} 索引 {}", vertices.GetSize(),
                 indices.GetSize());
        return result;
    }

    // ---- 焊接 ----
    TArray<UInt32>   remap;
    TArray<FVector3> positions;

    WeldByPosition(vertices, options.WeldTolerance, remap, positions);

    FSimplifyState state;
    state.Positions = positions;

    state.Quadrics.SetSize(positions.GetSize());
    state.Version.SetSize(positions.GetSize(), 0u);
    state.Locked.SetSize(positions.GetSize(), UInt8(0));
    state.Deviation.SetSize(positions.GetSize(), 0.0f);
    state.OriginalPositions = positions;

    state.ClusterHead.SetSize(positions.GetSize());
    state.ClusterTail.SetSize(positions.GetSize());
    state.ClusterNext.SetSize(positions.GetSize(), 0xFFFFFFFFu);

    for (SizeType v = 0; v < positions.GetSize(); ++v)
    {
        state.ClusterHead[v] = static_cast<UInt32>(v);
        state.ClusterTail[v] = static_cast<UInt32>(v);
    }
    state.Incident.SetSize(positions.GetSize());

    // ---- 三角形 (焊接之后可能出现退化的, 直接丢掉) ----
    const SizeType inputTriangles = indices.GetSize() / 3;

    for (SizeType t = 0; t < inputTriangles; ++t)
    {
        const UInt32 a = remap[indices[t * 3 + 0]];
        const UInt32 b = remap[indices[t * 3 + 1]];
        const UInt32 c = remap[indices[t * 3 + 2]];

        if (a == b || b == c || a == c)
        {
            continue;
        }

        const UInt32 tri = static_cast<UInt32>(state.Triangles.GetSize() / 3);

        state.Triangles.Add(a);
        state.Triangles.Add(b);
        state.Triangles.Add(c);

        state.Incident[a].Add(tri);
        state.Incident[b].Add(tri);
        state.Incident[c].Add(tri);

        state.OriginalNormals.Add(
            FVector3::Cross(positions[b] - positions[a],
                            positions[c] - positions[a])
                .GetSafeNormal());

        ++state.AliveTriangles;
    }

    result.WeldedVertexCount = static_cast<UInt32>(positions.GetSize());

    if (state.AliveTriangles == 0)
    {
        LIMX_LOG(LogMeshSimplifier, Error,
                 "[简化器] 焊接之后一个三角形都不剩");
        return result;
    }

    // ---- 二次型: 每个三角形的平面按面积加权累加到三个顶点 ----
    const SizeType triangleCount = state.Triangles.GetSize() / 3;

    for (SizeType t = 0; t < triangleCount; ++t)
    {
        const FVector3 areaNormal =
            TriangleAreaNormal(state, static_cast<UInt32>(t));

        const Float32 doubleArea = areaNormal.Length();

        if (doubleArea <= 0.0f)
        {
            continue;
        }

        const FVector3 normal = areaNormal / doubleArea;
        const FVector3& point = state.Positions[state.Triangles[t * 3]];

        const Float64 a = normal.X;
        const Float64 b = normal.Y;
        const Float64 c = normal.Z;
        const Float64 d = -(a * point.X + b * point.Y + c * point.Z);

        // 面积加权: 大三角形的平面更能代表这块表面。不加权的话密集的小
        // 三角形会把二次型带偏, 简化优先啃大面 —— 那正好是反的。
        const Float64 weight = 0.5 * static_cast<Float64>(doubleArea);

        for (UInt32 k = 0; k < 3; ++k)
        {
            state.Quadrics[state.Triangles[t * 3 + k]].AddPlane(a, b, c, d,
                                                                weight);
        }
    }

    if (options.LockOpenBoundary)
    {
        MarkOpenBoundary(state);
    }

    // 调用方指定的锁 —— 按输入顶点下标给, 这里映射到焊接顶点。
    //
    // 只要并成同一个位置的输入顶点里有**任何一个**被锁, 这个位置就锁死。
    // 宁可多锁: 少锁一个的后果是裂缝 (相邻两组的边界对不上), 而多锁一个
    // 的后果只是那里少简化一点。
    for (SizeType i = 0; i < options.LockedVertices.GetSize(); ++i)
    {
        const UInt32 inputIndex = options.LockedVertices[i];

        if (inputIndex < remap.GetSize())
        {
            state.Locked[remap[inputIndex]] = 1;
        }
    }

    // ---- 候选边入堆 ----
    //
    // 坍缩位置在三个候选里挑: 解出来的最优点、两个端点。最优点在几乎共面
    // 时会飞出去, 那时 SolveOptimalPosition 自己会拒绝。
    const auto EvaluateEdge = [&state](UInt32 v0, UInt32 v1, FVector3& outTarget)
        -> Float64
    {
        FQuadric merged = state.Quadrics[v0];
        merged.Add(state.Quadrics[v1]);

        Float64 bestCost = 0.0;
        bool    hasBest  = false;

        FVector3 bestTarget;

        Float64 solvedX = 0.0;
        Float64 solvedY = 0.0;
        Float64 solvedZ = 0.0;

        if (SolveOptimalPosition(merged, solvedX, solvedY, solvedZ))
        {
            bestCost = merged.Evaluate(solvedX, solvedY, solvedZ);
            bestTarget = FVector3(static_cast<Float32>(solvedX),
                                  static_cast<Float32>(solvedY),
                                  static_cast<Float32>(solvedZ));
            hasBest = true;
        }

        const FVector3 candidates[3] = {
            state.Positions[v0],
            state.Positions[v1],
            (state.Positions[v0] + state.Positions[v1]) * 0.5f,
        };

        for (UInt32 k = 0; k < 3; ++k)
        {
            const Float64 cost = merged.Evaluate(candidates[k].X,
                                                 candidates[k].Y,
                                                 candidates[k].Z);

            if (!hasBest || cost < bestCost)
            {
                bestCost   = cost;
                bestTarget = candidates[k];
                hasBest    = true;
            }
        }

        // 数值上可能出现极小的负值 —— 二次型理论上半正定, 但累加有舍入。
        // 夹到 0 而不是放过去: 负的代价会排到堆顶, 让简化器优先做一次
        // "误差为负"的坍缩, 而那个负号会一路传到 LOD 误差里。
        outTarget = bestTarget;

        return (bestCost > 0.0) ? bestCost : 0.0;
    };

    FCandidateHeap heap;

    const auto PushEdgesAround = [&](UInt32 vertex)
    {
        for (SizeType k = 0; k < state.Incident[vertex].GetSize(); ++k)
        {
            const UInt32 tri = state.Incident[vertex][k];

            if (!IsTriangleAlive(state, tri))
            {
                continue;
            }

            for (UInt32 e = 0; e < 3; ++e)
            {
                const UInt32 a = state.Triangles[tri * 3 + e];
                const UInt32 b = state.Triangles[tri * 3 + (e + 1) % 3];

                if (a != vertex && b != vertex)
                {
                    continue;
                }

                // 每条边只从小下标那一侧入堆, 免得同一条边进两次
                const UInt32 v0 = FMath::Min(a, b);
                const UInt32 v1 = FMath::Max(a, b);

                if (state.Locked[v0] != 0 && state.Locked[v1] != 0)
                {
                    continue;
                }

                FVector3 target;

                FCollapseCandidate candidate;
                candidate.Cost    = EvaluateEdge(v0, v1, target);
                candidate.V0      = v0;
                candidate.V1      = v1;
                candidate.Version = state.Version[v0] + state.Version[v1];

                heap.Push(candidate);
            }
        }
    };

    for (SizeType v = 0; v < state.Positions.GetSize(); ++v)
    {
        PushEdgesAround(static_cast<UInt32>(v));
    }

    // ---- 坍缩 ----
    // 目标三角形数。
    //
    // ── 这里原来有一个让"只给误差上限"完全失效的错 ──
    //
    // 第一版在没给目标数时把 target 取成**当前**的存活三角形数, 于是循环条件
    // `AliveTriangles > target` 当场为假 —— 一次坍缩都不做。而头文件明写
    // "0 表示不按数量停, 只看误差上限"。
    //
    // 表现极其安静: 返回的网格与输入一模一样, 误差 0, 而"误差是上界""不退化"
    // "流形保持""确定性"四条判据全部满分 —— 又一次"什么都不做也满分"。
    // 第三天要按误差预算建 DAG, 那时每一层都会等于上一层。
    //
    // 两个都不给才是"什么都不做": 那时没有任何停止条件, 一直简化下去会把
    // 网格化成一个点, 而那种结果没有任何调用方想要。
    const UInt32 target =
        (options.TargetTriangleCount > 0)
            ? options.TargetTriangleCount
            : ((options.MaxError > 0.0f) ? 0u : state.AliveTriangles);

    Float64 runningMax = 0.0;

    while (state.AliveTriangles > target && !heap.IsEmpty())
    {
        const FCollapseCandidate candidate = heap.Pop();

        // 过期: 两端里有谁在这条边入堆之后动过
        if (candidate.Version !=
            state.Version[candidate.V0] + state.Version[candidate.V1])
        {
            continue;
        }

        const UInt32 v0 = candidate.V0;
        const UInt32 v1 = candidate.V1;

        if (state.Locked[v0] != 0 && state.Locked[v1] != 0)
        {
            continue;
        }

        // 重新算一次位置 —— 堆里只存代价, 位置不存 (存了要多 12 字节,
        // 而重算一次的代价可以忽略)
        FVector3 target3;

        const Float64 cost = EvaluateEdge(v0, v1, target3);

        // 锁住的那一端必须留在原地
        if (state.Locked[v0] != 0)
        {
            target3 = state.Positions[v0];
        }
        else if (state.Locked[v1] != 0)
        {
            target3 = state.Positions[v1];
        }

        if (WouldFlip(state, v0, v1, target3))
        {
            continue;
        }

        // 误差上限按**偏差**判, 不按二次误差 —— 后者不是距离, 拿它跟一个
        // 以世界单位给的上限比是在比两个不同的量。
        //
        // 算的是**这次坍缩之后**的偏差, 不是坍缩之前的。
        //
        // 第一版拿的是两端已有的偏差, 而那要求先有一次被放行的坍缩把偏差顶过
        // 上限 —— 也就是说它永远拦不住**第一次**越界, MaxError 根本不是上界。
        // 现在把坍缩后的局部表面先算出来 (与 WouldFlip 同一套代换), 拿它量。
        if (options.MaxError > 0.0f)
        {
            Float32 wouldBe = 0.0f;

            for (UInt32 side = 0; side < 2 && wouldBe <= options.MaxError;
                 ++side)
            {
                const UInt32 root = (side == 0) ? v0 : v1;

                for (UInt32 member = state.ClusterHead[root];
                     member != 0xFFFFFFFFu;
                     member = state.ClusterNext[member])
                {
                    const FVector3& point = state.OriginalPositions[member];

                    Float32 nearest = 3.4e38f;

                    // 坍缩之后 v0 周围会是哪些三角形: 两端各自的入射三角形里,
                    // 不含另一端的那些 (含另一端的会被这次坍缩吃掉), 并把
                    // 移动的那个顶点代换成 target3。
                    for (UInt32 which = 0; which < 2; ++which)
                    {
                        const UInt32 vertex = (which == 0) ? v0 : v1;
                        const UInt32 other  = (which == 0) ? v1 : v0;

                        for (SizeType k = 0;
                             k < state.Incident[vertex].GetSize(); ++k)
                        {
                            const UInt32 tri = state.Incident[vertex][k];

                            if (!IsTriangleAlive(state, tri))
                            {
                                continue;
                            }

                            const UInt32 i0 = state.Triangles[tri * 3 + 0];
                            const UInt32 i1 = state.Triangles[tri * 3 + 1];
                            const UInt32 i2 = state.Triangles[tri * 3 + 2];

                            if (i0 == other || i1 == other || i2 == other)
                            {
                                continue;
                            }

                            FVector3 p[3] = {
                                state.Positions[i0],
                                state.Positions[i1],
                                state.Positions[i2],
                            };

                            for (UInt32 c = 0; c < 3; ++c)
                            {
                                if (state.Triangles[tri * 3 + c] == vertex)
                                {
                                    p[c] = target3;
                                }
                            }

                            nearest = FMath::Min(
                                nearest,
                                PointTriangleDistanceSquared(point, p[0], p[1],
                                                             p[2]));
                        }
                    }

                    if (nearest < 3.4e38f)
                    {
                        wouldBe = FMath::Max(wouldBe, FMath::Sqrt(nearest));

                        if (wouldBe > options.MaxError)
                        {
                            break;
                        }
                    }
                }
            }

            if (wouldBe > options.MaxError)
            {
                // 这条边太贵了就跳过它, 而不是整个停下来 —— 堆是按二次误差
                // 排的, 而二次误差与偏差不是同一个序, 堆顶那条贵不代表后面
                // 每一条都贵。第一版在这里 break, 表现是"误差上限一给就几乎
                // 不简化"。
                continue;
            }
        }

        // ---- 两个簇合并 (O(1): 把 v1 的链表接到 v0 的尾巴上) ----
        state.ClusterNext[state.ClusterTail[v0]] = state.ClusterHead[v1];
        state.ClusterTail[v0] = state.ClusterTail[v1];

        // ---- 真正坍缩: v1 并进 v0 ----
        state.Positions[v0] = target3;
        state.Quadrics[v0].Add(state.Quadrics[v1]);

        if (state.Locked[v1] != 0)
        {
            state.Locked[v0] = 1;
        }

        // 含两端的三角形死掉
        for (SizeType k = 0; k < state.Incident[v1].GetSize(); ++k)
        {
            const UInt32 tri = state.Incident[v1][k];

            if (!IsTriangleAlive(state, tri))
            {
                continue;
            }

            const UInt32 i0 = state.Triangles[tri * 3 + 0];
            const UInt32 i1 = state.Triangles[tri * 3 + 1];
            const UInt32 i2 = state.Triangles[tri * 3 + 2];

            if (i0 == v0 || i1 == v0 || i2 == v0)
            {
                state.Triangles[tri * 3 + 0] = 0xFFFFFFFFu;
                state.Triangles[tri * 3 + 1] = 0xFFFFFFFFu;
                state.Triangles[tri * 3 + 2] = 0xFFFFFFFFu;

                --state.AliveTriangles;
            }
        }

        // 其余的把 v1 改写成 v0
        for (SizeType k = 0; k < state.Incident[v1].GetSize(); ++k)
        {
            const UInt32 tri = state.Incident[v1][k];

            if (!IsTriangleAlive(state, tri))
            {
                continue;
            }

            for (UInt32 c = 0; c < 3; ++c)
            {
                if (state.Triangles[tri * 3 + c] == v1)
                {
                    state.Triangles[tri * 3 + c] = v0;
                }
            }

            state.Incident[v0].Add(tri);
        }

        state.Incident[v1].Clear();

        // ---- 偏差: 每个原始点到**新的**局部表面的最短距离 ----
        //
        // 必须在三角形改写完之后算 —— 那时 v0 周围的三角形才是坍缩后的样子。
        {
            Float32 deviation =
                FMath::Max(state.Deviation[v0], state.Deviation[v1]);

            for (UInt32 member = state.ClusterHead[v0]; member != 0xFFFFFFFFu;
                 member = state.ClusterNext[member])
            {
                const FVector3& point = state.OriginalPositions[member];

                Float32 nearest = 3.4e38f;

                for (SizeType k = 0; k < state.Incident[v0].GetSize(); ++k)
                {
                    const UInt32 tri = state.Incident[v0][k];

                    if (!IsTriangleAlive(state, tri))
                    {
                        continue;
                    }

                    nearest = FMath::Min(
                        nearest,
                        PointTriangleDistanceSquared(
                            point, state.Positions[state.Triangles[tri * 3 + 0]],
                            state.Positions[state.Triangles[tri * 3 + 1]],
                            state.Positions[state.Triangles[tri * 3 + 2]]));
                }

                if (nearest < 3.4e38f)
                {
                    deviation = FMath::Max(deviation, FMath::Sqrt(nearest));
                }
            }

            state.Deviation[v0] = deviation;
        }

        ++state.Version[v0];
        ++state.Version[v1];

        // **单调**: 记下来的是到目前为止的最大值, 不是这一次的偏差。
        //
        // 逐顶点的偏差上界本身是不减的, 但不同顶点之间没有可比性 —— 这一次
        // 坍缩的两个顶点偏差可能比上一次的小。直接记它会让后面某一层的误差
        // 小于前面, 而 LOD 选择规则 ("自身误差 < 阈值且父误差 ≥ 阈值") 在
        // 那种数据上会同时选中父与子, 表面被画两遍。
        runningMax =
            FMath::Max(runningMax, static_cast<Float64>(state.Deviation[v0]));

        ++result.CollapseCount;

        PushEdgesAround(v0);
    }

    // ---- 收尾: 对**最终**表面把误差重量一遍 ----
    //
    // 坍缩过程里记的那个数是不够的: 一个簇只在**它自己**坍缩的时候被量,
    // 而它旁边的簇后来又坍缩时, 它脚下那片表面变粗了却没人回头重量。实测
    // 差得不多 (球体上报 0.0126 而实际 0.0129), 但方向是**报小**的 ——
    // 而报小正是那条"误差必须是上界"最不能容忍的方向。
    //
    // 所以在这里把每个原始点对着最终的局部三角形重量一遍。仍然只在局部
    // 里取最小 (真正的最近三角形可能在别处, 那只会让真实距离更小), 所以
    // 得到的仍是上界; 而判据那边是对**全部**三角形取最小, 两边算的不是
    // 同一件事 —— 判据因此仍然验得住这一步。
    Float32 finalError = 0.0f;

    {
        // 先把还活着的三角形收一遍
        TArray<UInt32> alive;

        for (SizeType t = 0; t < state.Triangles.GetSize() / 3; ++t)
        {
            if (IsTriangleAlive(state, static_cast<UInt32>(t)))
            {
                alive.Add(static_cast<UInt32>(t));
            }
        }

        // 每个原始点对**全部**存活三角形取最近 —— 精确值, 不是界的界。
        //
        // 中间试过两版局部的做法, 都栽在同一件事上: 点会沿着表面滑走。
        // 一环量出来平面报 0.19 (真实 0), 两环报 0.12 —— 因为被并进来的
        // 原始点早滑出了代表顶点周围那几个三角形, 于是"最近的局部三角形"
        // 量的是到一条边的距离, 而不是到它脚下那块面的距离。平面上这个
        // 差别是 32000 倍。
        //
        // 全局扫是 O(点数 × 三角形数)。简化器是**离线**工具, 而且第三天
        // 起它是在 meshlet 组 (几千个三角形) 上跑, 不是在整张网格上。
        // 拿一个空间结构换这点时间, 换来的是一处可能出错而判据看不见的
        // 地方 —— 不划算。
        for (SizeType v = 0; v < state.Positions.GetSize(); ++v)
        {
            for (UInt32 member = state.ClusterHead[v]; member != 0xFFFFFFFFu;
                 member = state.ClusterNext[member])
            {
                const FVector3& point = state.OriginalPositions[member];

                Float32 nearest = 3.4e38f;

                for (SizeType k = 0; k < alive.GetSize(); ++k)
                {
                    const UInt32 tri = alive[k];

                    nearest = FMath::Min(
                        nearest,
                        PointTriangleDistanceSquared(
                            point,
                            state.Positions[state.Triangles[tri * 3 + 0]],
                            state.Positions[state.Triangles[tri * 3 + 1]],
                            state.Positions[state.Triangles[tri * 3 + 2]]));
                }

                if (nearest < 3.4e38f)
                {
                    finalError = FMath::Max(finalError, FMath::Sqrt(nearest));
                }
            }
        }
    }

    // 与过程里记下的那个取大 —— 单调性由它保证: 过程里的 runningMax 是
    // 不减的, 而收尾这一遍只会把它抬高。
    result.Error = FMath::Max(static_cast<Float32>(runningMax), finalError);
    result.ReachedTarget = (state.AliveTriangles <= target);

    // ---- 导出 ----
    //
    // 焊接顶点 -> 输出顶点。属性取原始重复顶点的平均。
    TArray<UInt32> outputIndex;
    outputIndex.SetSize(state.Positions.GetSize(), 0xFFFFFFFFu);

    TArray<UInt32> attributeCount;
    attributeCount.SetSize(state.Positions.GetSize(), 0u);

    TArray<FMeshVertex> accumulated;
    accumulated.SetSize(state.Positions.GetSize());

    for (SizeType i = 0; i < vertices.GetSize(); ++i)
    {
        const UInt32 welded = remap[i];

        FMeshVertex& sum = accumulated[welded];
        const FMeshVertex& source = vertices[i];

        if (attributeCount[welded] == 0)
        {
            sum = source;
        }
        else
        {
            sum.Normal    = sum.Normal + source.Normal;
            sum.Tangent   = sum.Tangent + source.Tangent;
            sum.TexCoord0 = sum.TexCoord0 + source.TexCoord0;
            sum.TexCoord1 = sum.TexCoord1 + source.TexCoord1;
            sum.Color     = sum.Color + source.Color;
        }

        ++attributeCount[welded];
    }

    for (SizeType t = 0; t < triangleCount; ++t)
    {
        if (!IsTriangleAlive(state, static_cast<UInt32>(t)))
        {
            continue;
        }

        for (UInt32 c = 0; c < 3; ++c)
        {
            const UInt32 welded = state.Triangles[t * 3 + c];

            if (outputIndex[welded] == 0xFFFFFFFFu)
            {
                outputIndex[welded] =
                    static_cast<UInt32>(result.Vertices.GetSize());

                FMeshVertex vertex = accumulated[welded];

                const Float32 inverse =
                    (attributeCount[welded] > 1)
                        ? 1.0f / static_cast<Float32>(attributeCount[welded])
                        : 1.0f;

                vertex.Position  = state.Positions[welded];
                vertex.Normal    = (vertex.Normal * inverse).GetSafeNormal();
                vertex.Tangent   = vertex.Tangent * inverse;
                vertex.TexCoord0 = vertex.TexCoord0 * inverse;
                vertex.TexCoord1 = vertex.TexCoord1 * inverse;
                vertex.Color     = vertex.Color * inverse;

                result.Vertices.Add(vertex);
            }

            result.Indices.Add(outputIndex[welded]);
        }
    }

    LIMX_LOG(LogMeshSimplifier, Log,
             "[简化器] {} -> {} 三角形 (焊接后顶点 {}), 坍缩 {} 次, "
             "误差 {}{}",
             inputTriangles, result.Indices.GetSize() / 3,
             result.WeldedVertexCount, result.CollapseCount, result.Error,
             result.ReachedTarget ? "" : " (**没到目标**)");

    return result;
}

} // namespace Limx
