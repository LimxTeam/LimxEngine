// ============================================================
// 文件名称：PathTraceCheck.cpp
// 创建时间：2026-09-02
// 创建者  ：LimxTeam
// 设计哲学：一条判据的容差从哪来, 决定了它到底在验什么。
//
//          蒙特卡洛估计量的误差不是"实现的误差", 是**采样的误差**, 而它
//          有确切的量: 单样本标准差 σ 除以 √N。所以这里每一条容差都写成
//          5·σ̂/√N, σ̂ 由同一次渲染回读的 Σ L² 现算 —— 不是查表, 不是
//          经验值, 更不是"跑一遍看看差多少再往上取整"。
//
//          为什么是 5 倍而不是 3 倍: 判据是**逐像素**判的, 一张 128×128
//          的图有 16384 个像素。3σ 的单侧误判率 1.3e-3, 乘 16384 等于
//          有二十个像素本来就会超标 —— 那样的判据每次跑都红, 于是很快
//          就会被"放宽一点"改成永远不红的样子。5σ 的双侧误判率 5.7e-7,
//          乘 16384 是 0.0093, 即一百次运行里大约有一次会有一个像素
//          越界。这是 Bonferroni 的账, 不是拍脑袋的余量。
//
//          还有一条容差不来自统计: 着色器在 float32 里累加 N 个样本,
//          舍入的相对误差约 √N·2^-24。N = 4096 时是 3.8e-6。所以每条
//          预算再加一个 1e-5 的相对下限 —— 它比最坏情况大约 3 倍,
//          又比任何一种实现错误小几个数量级。
//
//          三条判据各自能抓什么、抓不到什么, 写在各自的函数头上。尤其是
//          白炉: 它单独**验不了采样分布** (各向同性环境下余弦加权平均与
//          均匀加权平均相等), 所以这里给它配了一个带梯度的环境, 让两者
//          分开。不写清楚这一点的话, "白炉通过"会被当成"采样也对"。
// 功能描述：离线参考路径追踪器的三条判据 —— 白炉、能量守恒、方差标度,
//          以及一张 Cornell 盒参考图的离线渲染。
// 技术特性：场景全部程序生成; 每条判据带元判据 (射线真打到几何体、盒子
//          真的封闭、方差真的不为零); 判定只由返回值给出, 不解析日志。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                          │ 描述                      │
// │────────────────────────────────│─────────────────────────│
// │ FSceneBuilder                  │ 三角形汤的累积器           │
// │ BuildIcosphere()               │ 细分正二十面体 (凸)        │
// │ BuildCornellBox()              │ Cornell 盒 (可开口/封闭)   │
// │ ComputeAggregate()             │ 全图统计量与标准误         │
// │ RunFurnaceChecks()             │ 判据一: 白炉               │
// │ RunEnergyConservationChecks()  │ 判据二: 能量守恒           │
// │ RunVarianceScalingChecks()     │ 判据三: 方差 ∝ 1/N         │
// │ RunPathTraceChecks()           │ 三条判据的入口             │
// │ RenderPathTraceReferenceImage()│ Cornell 盒参考图           │
// ============================================================

// 预编译头必须排在最前 —— MSVC 的 /Yu 会丢弃它之前的一切内容。
#include "Launch/LaunchMinimal.h"

#include "PathTraceCheck.h"

#include "Core/Containers/TArray.h"
#include "Core/HAL/FPlatformFile.h"
#include "Core/Math/FMath.h"
#include "Core/Math/FVector.h"
#include "RHI/RHI/IRHIDevice.h"
#include "RenderCore/Renderer/FRenderContext.h"
#include "Renderer/RayTracing/FPathTracer.h"

namespace Limx
{

LIMX_DEFINE_LOG_CATEGORY(LogPathTraceCheck)

namespace
{

// ============================================================================
// 容差的两个来源
// ============================================================================

/// 统计容差的倍数
///
/// Bonferroni 的账: 逐像素判据一次要判 128×128 = 16384 个像素。
///   3σ 双侧误判率 2.7e-3 × 16384 = 44 个像素本来就会超标 —— 那样的判据
///     每次都红, 于是很快会被"放宽一点"改成永远不红的样子;
///   5σ   是 5.7e-7 × 16384 = 9.3e-3, 约百次运行出一次;
///   5.5σ 是 3.8e-8 × 16384 = 6.2e-4, 约一千六百次运行出一次。
/// 取 5.5。放宽这 10% 不会削弱判据: 下面记录的每一条变异造成的偏差都在
/// 40σ 以上, 与 5σ 还是 5.5σ 毫无关系。
constexpr Float64 kSigmaMultiplier = 5.5;

/// 浮点累加的相对下限 —— float32 里累加 4096 个样本的舍入约 3.8e-6,
/// 这里留三倍
constexpr Float64 kFloatFloor = 1.0e-5;

/// "恰好相等"的容差 —— 只用在解析上样本值逐个精确的场合 (白炉里每个
/// 样本恰好是 1.0, 和是整数, 除以 2 的幂仍是精确的)
constexpr Float64 kExactTolerance = 1.0e-6;

// ============================================================================
// FSceneBuilder — 三角形汤的累积器
//
// 不做顶点合并: 合并要一张哈希表, 而合并本身对参考实现没有任何好处 ——
// 几何法线是逐三角形算的, 共享顶点省下的那点显存换不来任何正确性。
// 相邻三角形共享的顶点由**同样的浮点运算**算出, 因此逐位相同, 网格仍然
// 是水密的。
// ============================================================================

struct FSceneBuilder
{
    TArray<Float32> Positions;
    TArray<UInt32>  Indices;
    TArray<UInt32>  TriangleMaterials;
    TArray<FPathTraceMaterial> Materials;

    UInt32 AddMaterial(Float32 albedoR, Float32 albedoG, Float32 albedoB,
                       Float32 emissionR = 0.0f, Float32 emissionG = 0.0f,
                       Float32 emissionB = 0.0f)
    {
        FPathTraceMaterial material;
        material.AlbedoR   = albedoR;
        material.AlbedoG   = albedoG;
        material.AlbedoB   = albedoB;
        material.EmissionR = emissionR;
        material.EmissionG = emissionG;
        material.EmissionB = emissionB;

        Materials.Add(material);

        return static_cast<UInt32>(Materials.GetSize() - 1);
    }

    UInt32 AddMaterialGray(Float32 albedo, Float32 emission = 0.0f)
    {
        return AddMaterial(albedo, albedo, albedo, emission, emission,
                           emission);
    }

    void AddTriangle(const FVector3& a, const FVector3& b, const FVector3& c,
                     UInt32 material)
    {
        const UInt32 base = static_cast<UInt32>(Positions.GetSize() / 3);

        AddPosition(a);
        AddPosition(b);
        AddPosition(c);

        Indices.Add(base + 0u);
        Indices.Add(base + 1u);
        Indices.Add(base + 2u);

        TriangleMaterials.Add(material);
    }

    void AddQuad(const FVector3& v0, const FVector3& v1, const FVector3& v2,
                 const FVector3& v3, UInt32 material)
    {
        AddTriangle(v0, v1, v2, material);
        AddTriangle(v0, v2, v3, material);
    }

    /// 轴对齐盒子的六个面 —— 绕序无所谓, 着色器把几何法线翻向来向
    void AddBox(const FVector3& lo, const FVector3& hi, UInt32 material)
    {
        AddQuad(FVector3(lo.X, lo.Y, lo.Z), FVector3(lo.X, lo.Y, hi.Z),
                FVector3(lo.X, hi.Y, hi.Z), FVector3(lo.X, hi.Y, lo.Z),
                material);

        AddQuad(FVector3(hi.X, lo.Y, lo.Z), FVector3(hi.X, hi.Y, lo.Z),
                FVector3(hi.X, hi.Y, hi.Z), FVector3(hi.X, lo.Y, hi.Z),
                material);

        AddQuad(FVector3(lo.X, lo.Y, lo.Z), FVector3(hi.X, lo.Y, lo.Z),
                FVector3(hi.X, lo.Y, hi.Z), FVector3(lo.X, lo.Y, hi.Z),
                material);

        AddQuad(FVector3(lo.X, hi.Y, lo.Z), FVector3(lo.X, hi.Y, hi.Z),
                FVector3(hi.X, hi.Y, hi.Z), FVector3(hi.X, hi.Y, lo.Z),
                material);

        AddQuad(FVector3(lo.X, lo.Y, lo.Z), FVector3(lo.X, hi.Y, lo.Z),
                FVector3(hi.X, hi.Y, lo.Z), FVector3(hi.X, lo.Y, lo.Z),
                material);

        AddQuad(FVector3(lo.X, lo.Y, hi.Z), FVector3(hi.X, lo.Y, hi.Z),
                FVector3(hi.X, hi.Y, hi.Z), FVector3(lo.X, hi.Y, hi.Z),
                material);
    }

    void Fill(FPathTraceScene& outScene) const
    {
        outScene.Positions   = Positions.GetData();
        outScene.VertexCount = static_cast<UInt32>(Positions.GetSize() / 3);
        outScene.Indices     = Indices.GetData();
        outScene.IndexCount  = static_cast<UInt32>(Indices.GetSize());
        outScene.TriangleMaterials = TriangleMaterials.GetData();
        outScene.Materials     = Materials.GetData();
        outScene.MaterialCount = static_cast<UInt32>(Materials.GetSize());
    }

private:
    void AddPosition(const FVector3& v)
    {
        Positions.Add(v.X);
        Positions.Add(v.Y);
        Positions.Add(v.Z);
    }
};

// ============================================================================
// 凸几何: 细分正二十面体
//
// 白炉测试要一个**凸**物体: 凸多面体上任何一条从几何法线半球出发的射线
// 都不会再撞回自己, 于是"逃逸"必定发生在第一次散射, 结果与最大弹射次数
// 无关。这一点让判据在 maxBounce = 1 时就该成立, 而不是"弹够多次才收敛"。
//
// 顶点全在单位球面上 —— 球面点集的凸包就是它自己, 所以细分多少次都还是凸的。
// 相邻三角形的公共中点由同一个 normalize(a + b) 算出 (浮点加法可交换),
// 逐位相同, 网格水密。
// ============================================================================

void SubdivideSphereTriangle(FSceneBuilder& builder, const FVector3& a,
                             const FVector3& b, const FVector3& c,
                             UInt32 depth, Float32 radius,
                             const FVector3& center, UInt32 material)
{
    if (depth == 0)
    {
        builder.AddTriangle(center + a * radius, center + b * radius,
                            center + c * radius, material);
        return;
    }

    const FVector3 ab = (a + b).GetSafeNormal();
    const FVector3 bc = (b + c).GetSafeNormal();
    const FVector3 ca = (c + a).GetSafeNormal();

    SubdivideSphereTriangle(builder, a, ab, ca, depth - 1, radius, center,
                            material);
    SubdivideSphereTriangle(builder, ab, b, bc, depth - 1, radius, center,
                            material);
    SubdivideSphereTriangle(builder, ca, bc, c, depth - 1, radius, center,
                            material);
    SubdivideSphereTriangle(builder, ab, bc, ca, depth - 1, radius, center,
                            material);
}

void AddIcosphere(FSceneBuilder& builder, const FVector3& center,
                  Float32 radius, UInt32 subdivisions, UInt32 material)
{
    // 黄金比 —— 正二十面体的十二个顶点是三个互相垂直的黄金矩形的角
    constexpr Float32 t = 1.61803399f;

    const FVector3 v[12] =
    {
        FVector3(-1.0f,    t,  0.0f).GetSafeNormal(),
        FVector3( 1.0f,    t,  0.0f).GetSafeNormal(),
        FVector3(-1.0f,   -t,  0.0f).GetSafeNormal(),
        FVector3( 1.0f,   -t,  0.0f).GetSafeNormal(),
        FVector3( 0.0f, -1.0f,    t).GetSafeNormal(),
        FVector3( 0.0f,  1.0f,    t).GetSafeNormal(),
        FVector3( 0.0f, -1.0f,   -t).GetSafeNormal(),
        FVector3( 0.0f,  1.0f,   -t).GetSafeNormal(),
        FVector3(    t,  0.0f, -1.0f).GetSafeNormal(),
        FVector3(    t,  0.0f,  1.0f).GetSafeNormal(),
        FVector3(   -t,  0.0f, -1.0f).GetSafeNormal(),
        FVector3(   -t,  0.0f,  1.0f).GetSafeNormal(),
    };

    const UInt32 faces[20][3] =
    {
        {  0, 11,  5 }, {  0,  5,  1 }, {  0,  1,  7 }, {  0,  7, 10 },
        {  0, 10, 11 }, {  1,  5,  9 }, {  5, 11,  4 }, { 11, 10,  2 },
        { 10,  7,  6 }, {  7,  1,  8 }, {  3,  9,  4 }, {  3,  4,  2 },
        {  3,  2,  6 }, {  3,  6,  8 }, {  3,  8,  9 }, {  4,  9,  5 },
        {  2,  4, 11 }, {  6,  2, 10 }, {  8,  6,  7 }, {  9,  8,  1 },
    };

    for (UInt32 f = 0; f < 20; ++f)
    {
        SubdivideSphereTriangle(builder, v[faces[f][0]], v[faces[f][1]],
                                v[faces[f][2]], subdivisions, radius, center,
                                material);
    }
}

// ============================================================================
// Cornell 盒
//
// 房间是 [-1,1]³。开口那一面是 +Z —— 封闭时补上, 白炉的多次弹射版本
// 敞开 (光线必须有地方逃, 否则反照率为 1 时路径永远不终止, 白炉根本
// 没有"收敛后的值")。
//
// 墙是**实心盒**而不是单张面片 —— 这一条是被判据逼出来的。
//
// 用面片时, "封闭盒里一条射线都逃不掉"这条元判据实测有 1.7e-6 的样本漏了
// 出去。原因不在追踪器: 命中点由重心坐标重建, u+v 在浮点里可能略微超过 1,
// 于是重建出的点落在三角形外几个 1e-7。落在地板的 x = -1 那条边之外时,
// 它就跑到了左墙**背面**, 而左墙只有一张面片, 背面之外什么都没有 ——
// 下一次弹射直接飞进虚空。
//
// 把墙做成实心盒之后, 那个"漏出去"的点落在墙的**体内**: 墙盒是封闭的,
// 从里面出发的射线必定命中它自己的某一张内面, 于是路径被关在墙里,
// 计入截断而不是逃逸。解析值一点不受影响 —— 封闭均匀自发光环境里
// L = 1/(1-a) 对**任何**封闭区域都成立, 墙体内部也是一个封闭区域。
//
// 相邻的墙盒故意互相重叠 (每个都往外多伸一个厚度), 角上因此没有任何
// 一条暴露的棱。
//
// 两个内盒**贴地放**: 底面与地板共面。共面在这里无害 —— 两张面的材质与
// 翻向来向之后的法线都一样, 命中谁都得到同一个结果; 而抬起来留缝会造出
// 一条极窄的通道, 那种地方的收敛慢得多, 平白给判据加噪声。
// ============================================================================

struct FCornellMaterials
{
    UInt32 Floor   = 0;
    UInt32 Ceiling = 0;
    UInt32 Back    = 0;
    UInt32 Left    = 0;
    UInt32 Right   = 0;
    UInt32 Front   = 0;
    UInt32 Blocks  = 0;
};

void BuildCornellBox(FSceneBuilder& builder, const FCornellMaterials& mat,
                     bool closeFront, bool includeBlocks)
{
    constexpr Float32 lo = -1.0f;
    constexpr Float32 hi =  1.0f;

    /// 墙厚 —— 只要比"重心坐标重建的浮点误差"大得多就够, 0.1 是它的
    /// 一百万倍
    constexpr Float32 t = 0.1f;

    constexpr Float32 outerLo = lo - t;
    constexpr Float32 outerHi = hi + t;

    // 开口那一侧的墙不往前伸 —— 伸出去的部分会挡住本该从开口逃出去的
    // 路径, 把"开口盒"变成一个开口更小的盒子。
    const Float32 frontLimit = closeFront ? outerHi : hi;

    // 地板
    builder.AddBox(FVector3(outerLo, outerLo, outerLo),
                   FVector3(outerHi, lo, frontLimit), mat.Floor);

    // 天花板
    builder.AddBox(FVector3(outerLo, hi, outerLo),
                   FVector3(outerHi, outerHi, frontLimit), mat.Ceiling);

    // 左墙
    builder.AddBox(FVector3(outerLo, outerLo, outerLo),
                   FVector3(lo, outerHi, frontLimit), mat.Left);

    // 右墙
    builder.AddBox(FVector3(hi, outerLo, outerLo),
                   FVector3(outerHi, outerHi, frontLimit), mat.Right);

    // 后墙
    builder.AddBox(FVector3(outerLo, outerLo, outerLo),
                   FVector3(outerHi, outerHi, lo), mat.Back);

    if (closeFront)
    {
        builder.AddBox(FVector3(outerLo, outerLo, hi),
                       FVector3(outerHi, outerHi, outerHi), mat.Front);
    }

    if (includeBlocks)
    {
        // 矮盒 (右前) 与高盒 (左后)
        builder.AddBox(FVector3(0.10f, -1.00f, -0.10f),
                       FVector3(0.70f, -0.40f,  0.50f), mat.Blocks);

        builder.AddBox(FVector3(-0.70f, -1.00f, -0.60f),
                       FVector3(-0.10f,  0.20f,  0.00f), mat.Blocks);
    }
}

/// 相机: 盒外正对开口
FPathTraceCamera MakeOutsideCamera()
{
    FPathTraceCamera camera;
    camera.Position = FVector3(0.0f, 0.0f, 2.6f);
    camera.Forward  = FVector3(0.0f, 0.0f, -1.0f);
    camera.Right    = FVector3(1.0f, 0.0f, 0.0f);
    camera.Up       = FVector3(0.0f, 1.0f, 0.0f);

    // 45°: 在 z = 1 的开口平面上, 视锥半高 1.6·tan(22.5°) = 0.663 < 1,
    // 于是每一条主射线都是从开口穿进去的 —— 没有一条打在盒子外壁上。
    // 这一点是元判据"首次命中比例"要求接近 1 的依据。
    camera.FovY = 0.7853982f;

    return camera;
}

/// 相机: 盒内
FPathTraceCamera MakeInsideCamera()
{
    FPathTraceCamera camera;
    camera.Position = FVector3(0.0f, 0.0f, 0.85f);
    camera.Forward  = FVector3(0.0f, 0.0f, -1.0f);
    camera.Right    = FVector3(1.0f, 0.0f, 0.0f);
    camera.Up       = FVector3(0.0f, 1.0f, 0.0f);
    camera.FovY     = 1.0471976f;

    return camera;
}

// ============================================================================
// 统计量
// ============================================================================

struct FAggregate
{
    /// 全图 (像素 × 样本) 的均值
    Float64 Mean = 0.0;

    /// 单样本方差 σ² 与标准差 σ
    Float64 Variance = 0.0;
    Float64 StdDev   = 0.0;

    /// 均值的标准误 σ/√(像素数 × spp)
    Float64 StandardError = 0.0;

    /// 因达到最大弹射次数而终止的样本比例
    Float64 TruncatedFraction = 0.0;

    /// 主射线命中几何体的样本比例
    Float64 PrimaryHitFraction = 0.0;

    /// 通道之间的最大差 —— 灰度场景里必须恰好是 0
    Float64 MaxChannelSpread = 0.0;

    /// 逐像素 |均值 - 参考值| / (kSigmaMultiplier·σ̂_p/√N + 下限) 的最大值
    /// (由调用方填)
    Float64 WorstPixelRatio = 0.0;

    /// 越界像素数 (由调用方填)
    UInt32 OutlierPixels = 0;

    UInt32 PixelCount  = 0;
    UInt32 SampleCount = 0;
};

FAggregate ComputeAggregate(const TArray<FPathTracePixel>& pixels, UInt32 spp)
{
    FAggregate out;
    out.PixelCount  = static_cast<UInt32>(pixels.GetSize());
    out.SampleCount = spp;

    if (out.PixelCount == 0 || spp == 0)
    {
        return out;
    }

    Float64 sum        = 0.0;
    Float64 sumSquares = 0.0;
    Float64 truncated  = 0.0;
    Float64 primary    = 0.0;
    Float64 spread     = 0.0;

    for (SizeType i = 0; i < pixels.GetSize(); ++i)
    {
        const FPathTracePixel& p = pixels[i];

        sum        += static_cast<Float64>(p.SumR);
        sumSquares += static_cast<Float64>(p.SumSqR);
        truncated  += static_cast<Float64>(p.SumTruncated);
        primary    += static_cast<Float64>(p.SumPrimaryHit);

        const Float64 dg = FMath::Abs(static_cast<Float64>(p.SumR) -
                                      static_cast<Float64>(p.SumG));
        const Float64 db = FMath::Abs(static_cast<Float64>(p.SumR) -
                                      static_cast<Float64>(p.SumB));

        spread = FMath::Max(spread, FMath::Max(dg, db));
    }

    const Float64 n = static_cast<Float64>(out.PixelCount) *
                      static_cast<Float64>(spp);

    out.Mean = sum / n;

    // 方差用"平方和减均值平方"算, 而这在 σ 恰好为 0 时会因抵消得到一个
    // 极小的负数。钳到 0 —— 负方差开根号是 NaN, 而 NaN 会让所有比较都
    // 返回 false, 于是判据静默通过。
    const Float64 variance = (n > 1.0)
        ? FMath::Max(0.0, (sumSquares - sum * sum / n) / (n - 1.0))
        : 0.0;

    out.Variance      = variance;
    out.StdDev        = FMath::Sqrt(variance);
    out.StandardError = out.StdDev / FMath::Sqrt(n);

    out.TruncatedFraction  = truncated / n;
    out.PrimaryHitFraction = primary / n;
    out.MaxChannelSpread   = spread / static_cast<Float64>(spp);

    return out;
}

/// 逐像素判据: |均值_p - reference| 是否落在预算里
///
/// 预算 = kSigmaMultiplier · σ̂_p/√N + kFloatFloor · max(1, |reference|)
///
/// σ̂_p 是**这个像素自己**的样本标准差, 不是全图的 —— 不同像素的方差可以
/// 差很多 (背景像素方差为零, 角落里的像素方差最大), 用全图平均的话,
/// 高方差像素被放行, 低方差像素被冤枉。
void EvaluatePerPixel(const TArray<FPathTracePixel>& pixels, UInt32 spp,
                      Float64 reference, FAggregate& stats)
{
    stats.WorstPixelRatio = 0.0;
    stats.OutlierPixels   = 0;

    if (spp < 2)
    {
        return;
    }

    const Float64 n = static_cast<Float64>(spp);

    for (SizeType i = 0; i < pixels.GetSize(); ++i)
    {
        const FPathTracePixel& p = pixels[i];

        const Float64 sum   = static_cast<Float64>(p.SumR);
        const Float64 sumSq = static_cast<Float64>(p.SumSqR);

        const Float64 mean = sum / n;

        const Float64 variance =
            FMath::Max(0.0, (sumSq - sum * sum / n) / (n - 1.0));

        const Float64 budget =
            kSigmaMultiplier * FMath::Sqrt(variance / n) +
            kFloatFloor * FMath::Max(1.0, FMath::Abs(reference));

        const Float64 deviation = FMath::Abs(mean - reference);
        const Float64 ratio     = deviation / budget;

        if (ratio > stats.WorstPixelRatio)
        {
            stats.WorstPixelRatio = ratio;
        }

        if (ratio > 1.0)
        {
            ++stats.OutlierPixels;
        }
    }
}

/// 把一个很小的数放大到日志能看清的量级 (格式化只给六位小数)
Float64 Scaled(Float64 value, Float64 scale)
{
    return value * scale;
}

// ============================================================================
// 判据一: 白炉
//
// 三个部分, 各自补上另外两个的盲区:
//
//   1a 纯白炉 (凸球)。反照率 1、各向同性环境 1、无自发光 —— 每个样本的
//      值**恰好**是 1.0, 单样本方差为 0, 于是这一条的统计预算是零,
//      容差只剩浮点舍入。任何"权重不等于反照率"的实现在这里都是数量级
//      的偏差 (少除一次 pdf 得到 0.21, 漏掉余弦得到发散)。
//
//   1b 梯度炉 (水平面, 解析值 a·(base + 2/3·grad))。1a 抓不到"采样分布
//      与 pdf 不匹配": 各向同性环境下, 余弦加权平均与均匀加权平均是同一
//      个数, 于是"按均匀采样却按余弦 pdf 算权重"照样得到 1.0。加一项
//      线性梯度之后两者相差 grad/6, 反照率 1、梯度 1 时是 10%。
//
//   1c 多次弹射白炉 (开口 Cornell 盒)。1a/1b 都只散射一次 —— 通量的
//      连乘、俄罗斯轮盘、二次射线的自遮挡都没被走到。开口盒里路径要弹
//      很多次才逃得出去, 而反照率为 1 时截断偏差**恰好等于**存活比例,
//      于是有一条精确的恒等式可以判: L + 存活比例 ≡ 1。
// ============================================================================

bool RunFurnaceChecks(FPathTracer& tracer)
{
    bool passed = true;

    // ------------------------------------------------------------------
    // 1a 纯白炉 —— 凸球
    // ------------------------------------------------------------------
    {
        FSceneBuilder builder;
        const UInt32 white = builder.AddMaterialGray(1.0f, 0.0f);

        AddIcosphere(builder, FVector3(0.0f, 0.0f, 0.0f), 1.0f, 2, white);

        FPathTraceScene scene;
        builder.Fill(scene);

        if (tracer.SetScene(scene) != ERHIResult::Success)
        {
            LIMX_LOG(LogPathTraceCheck, Error,
                     "[白炉] 凸球场景上传失败");
            return false;
        }

        FPathTraceCamera camera;
        camera.Position = FVector3(0.0f, 0.0f, 3.5f);
        camera.Forward  = FVector3(0.0f, 0.0f, -1.0f);
        camera.Right    = FVector3(1.0f, 0.0f, 0.0f);
        camera.Up       = FVector3(0.0f, 1.0f, 0.0f);
        camera.FovY     = 0.6981317f;

        FPathTraceSettings settings;
        settings.Width               = 128;
        settings.Height              = 128;
        settings.SamplesPerPixel     = 512;
        settings.RussianRouletteStartDepth = 1;
        settings.EnvironmentRadiance = 1.0f;
        settings.EnvironmentGradientY = 0.0f;
        settings.NormalOffset        = 1.0e-4f;

        const UInt32 bounceList[4] = { 1u, 2u, 4u, 8u };

        for (UInt32 i = 0; i < 4; ++i)
        {
            settings.MaxBounce = bounceList[i];

            TArray<FPathTracePixel> pixels;

            if (tracer.Render(camera, settings, pixels) != ERHIResult::Success)
            {
                LIMX_LOG(LogPathTraceCheck, Error, "[白炉] 凸球渲染失败");
                return false;
            }

            FAggregate stats = ComputeAggregate(pixels,
                                                settings.SamplesPerPixel);
            EvaluatePerPixel(pixels, settings.SamplesPerPixel, 1.0, stats);

            LIMX_LOG(LogPathTraceCheck, Display,
                     "[白炉/凸球] 弹射 {} — 均值 {} 偏差 {}e-9 σ {} "
                     "截断 {} 首次命中 {} 越界像素 {}/{}",
                     settings.MaxBounce, stats.Mean,
                     Scaled(FMath::Abs(stats.Mean - 1.0), 1.0e9),
                     stats.StdDev, stats.TruncatedFraction,
                     stats.PrimaryHitFraction, stats.OutlierPixels,
                     stats.PixelCount);

            // 元判据: 射线真的打到球了。没有这一条, 一棵空的加速结构会
            // 得到满屏环境光 —— 而满屏 1.0 恰恰是白炉判通过的样子。
            if (stats.PrimaryHitFraction < 0.25)
            {
                LIMX_LOG(LogPathTraceCheck, Error,
                         "[白炉/凸球] 首次命中比例 {} < 0.25 — "
                         "射线没打到几何体, 这一条判据是空转的",
                         stats.PrimaryHitFraction);
                passed = false;
            }

            // 凸几何: 任何散射方向都逃得掉, 一条都不该被截断
            if (stats.TruncatedFraction != 0.0)
            {
                LIMX_LOG(LogPathTraceCheck, Error,
                         "[白炉/凸球] 截断比例 {} != 0 — 凸物体上不该有"
                         "第二次命中 (自遮挡?)",
                         stats.TruncatedFraction);
                passed = false;
            }

            // 每个样本恰好是 1.0 → 单样本方差恰好是 0
            if (stats.StdDev > kExactTolerance)
            {
                LIMX_LOG(LogPathTraceCheck, Error,
                         "[白炉/凸球] 单样本标准差 {} != 0 — 白炉里每个"
                         "样本都该恰好是 1.0",
                         stats.StdDev);
                passed = false;
            }

            if (stats.MaxChannelSpread > kExactTolerance)
            {
                LIMX_LOG(LogPathTraceCheck, Error,
                         "[白炉/凸球] 通道最大差 {} != 0 — 灰度材质不该"
                         "出现通道差", stats.MaxChannelSpread);
                passed = false;
            }

            if (FMath::Abs(stats.Mean - 1.0) > kExactTolerance ||
                stats.OutlierPixels > 0)
            {
                LIMX_LOG(LogPathTraceCheck, Error,
                         "[白炉/凸球] 弹射 {} — 均值 {} 偏离 1.0, "
                         "越界像素 {} 个 (最坏 {} 倍预算)",
                         settings.MaxBounce, stats.Mean, stats.OutlierPixels,
                         stats.WorstPixelRatio);
                passed = false;
            }
        }
    }

    // ------------------------------------------------------------------
    // 1b 梯度炉 —— 水平面, 单次散射有解析解
    //
    // 平面法线是 +Y。半球上
    //     余弦加权:  ∫(base + grad·ω.y)·cosθ dω / π = base + (2/3)·grad
    //     均匀加权:  ∫(base + grad·ω.y) dω / (2π)   = base + (1/2)·grad
    // 反照率 a 乘在外面。取 a = 1, base = 1, grad = 1 → 5/3。
    // ------------------------------------------------------------------
    {
        FSceneBuilder builder;
        const UInt32 white = builder.AddMaterialGray(1.0f, 0.0f);

        // 足够大, 让每一条主射线都落在面上 —— 相机 60° 视场、距地 2 个
        // 单位时最远的角落射线落在 1.7 附近, 100 是它的六十倍。
        constexpr Float32 half = 100.0f;

        builder.AddQuad(FVector3(-half, 0.0f, -half),
                        FVector3( half, 0.0f, -half),
                        FVector3( half, 0.0f,  half),
                        FVector3(-half, 0.0f,  half), white);

        FPathTraceScene scene;
        builder.Fill(scene);

        if (tracer.SetScene(scene) != ERHIResult::Success)
        {
            LIMX_LOG(LogPathTraceCheck, Error, "[梯度炉] 平面场景上传失败");
            return false;
        }

        FPathTraceCamera camera;
        camera.Position = FVector3(0.0f, 2.0f, 0.0f);
        camera.Forward  = FVector3(0.0f, -1.0f, 0.0f);
        camera.Right    = FVector3(1.0f, 0.0f, 0.0f);
        camera.Up       = FVector3(0.0f, 0.0f, -1.0f);
        camera.FovY     = 1.0471976f;

        FPathTraceSettings settings;
        settings.Width                = 128;
        settings.Height               = 128;
        settings.SamplesPerPixel      = 4096;
        settings.RussianRouletteStartDepth = 1;
        settings.EnvironmentRadiance  = 1.0f;
        settings.EnvironmentGradientY = 1.0f;
        settings.NormalOffset         = 1.0e-4f;

        // base + (2/3)·grad
        const Float64 reference = 1.0 + 2.0 / 3.0;

        const UInt32 bounceList[2] = { 1u, 4u };

        for (UInt32 i = 0; i < 2; ++i)
        {
            settings.MaxBounce = bounceList[i];

            TArray<FPathTracePixel> pixels;

            if (tracer.Render(camera, settings, pixels) != ERHIResult::Success)
            {
                LIMX_LOG(LogPathTraceCheck, Error, "[梯度炉] 渲染失败");
                return false;
            }

            FAggregate stats = ComputeAggregate(pixels,
                                                settings.SamplesPerPixel);
            EvaluatePerPixel(pixels, settings.SamplesPerPixel, reference,
                             stats);

            const Float64 deviation = FMath::Abs(stats.Mean - reference);
            const Float64 budget = kSigmaMultiplier * stats.StandardError +
                                   kFloatFloor * reference;

            LIMX_LOG(LogPathTraceCheck, Display,
                     "[白炉/梯度] 弹射 {} — 均值 {} 解析 {} 偏差 {}e-6 "
                     "σ {} 预算 {}e-6 越界像素 {}/{} (最坏 {} 倍)",
                     settings.MaxBounce, stats.Mean, reference,
                     Scaled(deviation, 1.0e6), stats.StdDev,
                     Scaled(budget, 1.0e6), stats.OutlierPixels,
                     stats.PixelCount, stats.WorstPixelRatio);

            // 元判据: 平面必须铺满整个画面, 否则背景像素 (= 环境本身)
            // 会把这条判据稀释成"环境算得对不对"
            if (stats.PrimaryHitFraction < 0.999999)
            {
                LIMX_LOG(LogPathTraceCheck, Error,
                         "[白炉/梯度] 首次命中比例 {} < 1 — 有主射线漏出"
                         "平面之外", stats.PrimaryHitFraction);
                passed = false;
            }

            // 元判据: 方差必须不为零, 否则"预算由实测 σ 推出"是空话
            if (stats.StdDev < 0.1)
            {
                LIMX_LOG(LogPathTraceCheck, Error,
                         "[白炉/梯度] 单样本标准差 {} 过小 — 解析上应为 "
                         "0.2357 (grad=1 时 cosθ 的标准差)", stats.StdDev);
                passed = false;
            }

            // 全图均值与逐像素分开判。
            //
            // 两者抓的不是同一类错: 均值抓"系统性偏差", 逐像素抓"有效样本
            // 数不对"。后者的典型来源是分块累加时样本号没有全局化 —— 那种
            // 实现的全图均值仍然是对的 (样本没错, 只是重复了), 只有逐像素
            // 的离散度会大出一截。实测那条变异的全图偏差 1.29e-4 < 预算
            // 1.75e-4 (逃过), 逐像素越界 2755 个 (被抓)。
            if (deviation > budget)
            {
                LIMX_LOG(LogPathTraceCheck, Error,
                         "[白炉/梯度] 弹射 {} — 全图均值偏差 {} 超预算 {}",
                         settings.MaxBounce, deviation, budget);
                passed = false;
            }

            if (stats.OutlierPixels > 0)
            {
                LIMX_LOG(LogPathTraceCheck, Error,
                         "[白炉/梯度] 弹射 {} — {} 个像素超出各自的 "
                         "5.5σ/√N 预算 (最坏 {} 倍), 共 {} 个像素",
                         settings.MaxBounce, stats.OutlierPixels,
                         stats.WorstPixelRatio, stats.PixelCount);
                passed = false;
            }
        }
    }

    // ------------------------------------------------------------------
    // 1c 多次弹射白炉 —— 开口 Cornell 盒
    // ------------------------------------------------------------------
    {
        FSceneBuilder builder;
        const UInt32 white = builder.AddMaterialGray(1.0f, 0.0f);

        FCornellMaterials mat;
        mat.Floor = mat.Ceiling = mat.Back = white;
        mat.Left  = mat.Right   = mat.Front = white;
        mat.Blocks = white;

        BuildCornellBox(builder, mat, false, true);

        FPathTraceScene scene;
        builder.Fill(scene);

        if (tracer.SetScene(scene) != ERHIResult::Success)
        {
            LIMX_LOG(LogPathTraceCheck, Error,
                     "[白炉/开口盒] 场景上传失败");
            return false;
        }

        const FPathTraceCamera camera = MakeOutsideCamera();

        FPathTraceSettings settings;
        settings.Width                = 96;
        settings.Height               = 96;
        settings.SamplesPerPixel      = 256;
        settings.RussianRouletteStartDepth = 1;
        settings.EnvironmentRadiance  = 1.0f;
        settings.EnvironmentGradientY = 0.0f;
        settings.NormalOffset         = 1.0e-4f;

        const UInt32 bounceList[4] = { 2u, 8u, 32u, 128u };

        Float64 previousTruncated = 2.0;

        for (UInt32 i = 0; i < 4; ++i)
        {
            settings.MaxBounce = bounceList[i];

            TArray<FPathTracePixel> pixels;

            if (tracer.Render(camera, settings, pixels) != ERHIResult::Success)
            {
                LIMX_LOG(LogPathTraceCheck, Error, "[白炉/开口盒] 渲染失败");
                return false;
            }

            const FAggregate stats =
                ComputeAggregate(pixels, settings.SamplesPerPixel);

            // 恒等式 L_p + 截断比例_p ≡ 1 —— 逐像素, 精确
            //
            // 反照率为 1 时每个样本要么逃出去 (贡献恰好 1), 要么被截断
            // (贡献 0 并计入截断)。两者互斥且穷尽, 所以两个比例之和必须
            // 精确为 1。这条恒等式与统计无关: 它是对**通量连乘**的直接
            // 约束, 而截断计数完全不经过通量。
            Float64 worstIdentity = 0.0;
            const Float64 n = static_cast<Float64>(settings.SamplesPerPixel);

            for (SizeType p = 0; p < pixels.GetSize(); ++p)
            {
                const Float64 value =
                    (static_cast<Float64>(pixels[p].SumR) +
                     static_cast<Float64>(pixels[p].SumTruncated)) / n;

                worstIdentity =
                    FMath::Max(worstIdentity, FMath::Abs(value - 1.0));
            }

            const Float64 deviation = FMath::Abs(stats.Mean - 1.0);

            LIMX_LOG(LogPathTraceCheck, Display,
                     "[白炉/开口盒] 弹射 {} — 均值 {} 截断比例 {} "
                     "偏差 {}e-6 恒等式最大残差 {}e-9 首次命中 {}",
                     settings.MaxBounce, stats.Mean, stats.TruncatedFraction,
                     Scaled(deviation, 1.0e6), Scaled(worstIdentity, 1.0e9),
                     stats.PrimaryHitFraction);

            if (stats.PrimaryHitFraction < 0.999999)
            {
                LIMX_LOG(LogPathTraceCheck, Error,
                         "[白炉/开口盒] 首次命中比例 {} < 1 — 主射线没有"
                         "全部穿进盒子", stats.PrimaryHitFraction);
                passed = false;
            }

            if (worstIdentity > kExactTolerance)
            {
                LIMX_LOG(LogPathTraceCheck, Error,
                         "[白炉/开口盒] 弹射 {} — 恒等式 L + 截断 ≡ 1 的"
                         "最大残差 {} 超过 {}",
                         settings.MaxBounce, worstIdentity, kExactTolerance);
                passed = false;
            }

            // 截断比例必须随弹射次数单调下降
            if (stats.TruncatedFraction >= previousTruncated)
            {
                LIMX_LOG(LogPathTraceCheck, Error,
                         "[白炉/开口盒] 弹射 {} 的截断比例 {} 没有低于"
                         "上一档的 {} — 多给弹射次数却没有更多路径逃出去",
                         settings.MaxBounce, stats.TruncatedFraction,
                         previousTruncated);
                passed = false;
            }

            previousTruncated = stats.TruncatedFraction;

            // 最后一档: 截断比例必须小到让白炉值本身也落在 1e-3 以内
            if (i == 3)
            {
                if (stats.TruncatedFraction > 1.0e-3)
                {
                    LIMX_LOG(LogPathTraceCheck, Error,
                             "[白炉/开口盒] 弹射 128 时截断比例仍有 {} — "
                             "路径逃不出去, 白炉值收敛不到 1.0",
                             stats.TruncatedFraction);
                    passed = false;
                }

                const Float64 budget = stats.TruncatedFraction +
                                       kSigmaMultiplier * stats.StandardError +
                                       kFloatFloor;

                if (deviation > budget)
                {
                    LIMX_LOG(LogPathTraceCheck, Error,
                             "[白炉/开口盒] 弹射 128 — 偏差 {} 超预算 {} "
                             "(截断 {} + 5σ/√n {})",
                             deviation, budget, stats.TruncatedFraction,
                             kSigmaMultiplier * stats.StandardError);
                    passed = false;
                }
            }
        }
    }

    return passed;
}

// ============================================================================
// 判据二: 能量守恒
//
// 封闭盒, 每一面都以 L_e = 1 各向同性自发光, 反照率一律 a。这样的封闭
// 环境里辐射度处处相同:
//     L = L_e + a·(1/π)∫L·cosθ dω = 1 + a·L  →  L = 1/(1-a)
// 截到 N 次弹射时是前 N+1 项:
//     L_N = Σ_{k=0}^{N} a^k = (1 - a^(N+1)) / (1 - a)
// 与几何、与相机位置**都无关** —— 只要相机在盒子里、盒子是封闭的。
//
// 这一条解析值是逐个 N 给出的, 所以判的不只是"收敛到哪", 而是"每一次
// 弹射加进来的量对不对"。俄罗斯轮盘从第一次散射就开, 所以"轮盘不补偿"
// 这类错误在这里是数量级的偏差。
//
// 元判据用同一个盒子跑一次白炉: 封闭盒里没有任何一条路径逃得掉, 所以
// 反照率 1、环境 1 时结果必须恒为 0 而截断比例必须恒为 1。任何一条
// 漏出去的射线都说明盒子有缝, 而有缝的盒子上面那个解析值就不成立了。
// ============================================================================

bool RunEnergyConservationChecks(FPathTracer& tracer)
{
    bool passed = true;

    const FPathTraceCamera camera = MakeInsideCamera();

    // ------------------------------------------------------------------
    // 元判据: 盒子真的是封闭的
    // ------------------------------------------------------------------
    {
        FSceneBuilder builder;
        const UInt32 white = builder.AddMaterialGray(1.0f, 0.0f);

        FCornellMaterials mat;
        mat.Floor = mat.Ceiling = mat.Back = white;
        mat.Left  = mat.Right   = mat.Front = white;
        mat.Blocks = white;

        BuildCornellBox(builder, mat, true, true);

        FPathTraceScene scene;
        builder.Fill(scene);

        if (tracer.SetScene(scene) != ERHIResult::Success)
        {
            LIMX_LOG(LogPathTraceCheck, Error, "[能量守恒] 封闭盒上传失败");
            return false;
        }

        FPathTraceSettings settings;
        settings.Width                = 96;
        settings.Height               = 96;
        settings.SamplesPerPixel      = 128;
        settings.MaxBounce            = 32;
        settings.RussianRouletteStartDepth = 64;
        settings.EnvironmentRadiance  = 1.0f;
        settings.EnvironmentGradientY = 0.0f;
        settings.NormalOffset         = 1.0e-4f;

        TArray<FPathTracePixel> pixels;

        if (tracer.Render(camera, settings, pixels) != ERHIResult::Success)
        {
            LIMX_LOG(LogPathTraceCheck, Error, "[能量守恒] 封闭性探测失败");
            return false;
        }

        const FAggregate stats =
            ComputeAggregate(pixels, settings.SamplesPerPixel);

        const Float64 leakFraction = 1.0 - stats.TruncatedFraction;

        LIMX_LOG(LogPathTraceCheck, Display,
                 "[能量守恒/封闭性] 32 次弹射 — 漏出比例 {}e-9 "
                 "(逃逸辐射度 {}) 首次命中 {}",
                 Scaled(leakFraction, 1.0e9), stats.Mean,
                 stats.PrimaryHitFraction);

        if (leakFraction != 0.0 || stats.Mean != 0.0)
        {
            LIMX_LOG(LogPathTraceCheck, Error,
                     "[能量守恒/封闭性] 封闭盒里有 {} 的样本漏了出去 — "
                     "L = 1/(1-a) 的前提不成立", leakFraction);
            passed = false;
        }

        if (stats.PrimaryHitFraction != 1.0)
        {
            LIMX_LOG(LogPathTraceCheck, Error,
                     "[能量守恒/封闭性] 首次命中比例 {} != 1 — 相机不在"
                     "盒子里?", stats.PrimaryHitFraction);
            passed = false;
        }
    }

    // ------------------------------------------------------------------
    // 主判据: 逐个反照率、逐个弹射次数对解析值
    // ------------------------------------------------------------------
    const Float32 albedoList[4] = { 0.25f, 0.5f, 0.75f, 0.9f };
    const UInt32  bounceList[7] = { 0u, 1u, 2u, 4u, 8u, 16u, 24u };

    for (UInt32 a = 0; a < 4; ++a)
    {
        const Float32 albedo = albedoList[a];

        FSceneBuilder builder;
        const UInt32 surface = builder.AddMaterialGray(albedo, 1.0f);

        FCornellMaterials mat;
        mat.Floor = mat.Ceiling = mat.Back = surface;
        mat.Left  = mat.Right   = mat.Front = surface;
        mat.Blocks = surface;

        BuildCornellBox(builder, mat, true, true);

        FPathTraceScene scene;
        builder.Fill(scene);

        if (tracer.SetScene(scene) != ERHIResult::Success)
        {
            LIMX_LOG(LogPathTraceCheck, Error,
                     "[能量守恒] 反照率 {} 的场景上传失败", albedo);
            return false;
        }

        FPathTraceSettings settings;
        settings.Width                = 96;
        settings.Height               = 96;
        settings.SamplesPerPixel      = 256;
        settings.RussianRouletteStartDepth = 1;
        settings.EnvironmentRadiance  = 0.0f;
        settings.EnvironmentGradientY = 0.0f;
        settings.NormalOffset         = 1.0e-4f;

        const Float64 albedo64 = static_cast<Float64>(albedo);
        const Float64 limit    = 1.0 / (1.0 - albedo64);

        Float64 previousMean  = -1.0;
        Float64 previousError = 0.0;

        for (UInt32 b = 0; b < 7; ++b)
        {
            settings.MaxBounce = bounceList[b];

            TArray<FPathTracePixel> pixels;

            if (tracer.Render(camera, settings, pixels) != ERHIResult::Success)
            {
                LIMX_LOG(LogPathTraceCheck, Error, "[能量守恒] 渲染失败");
                return false;
            }

            FAggregate stats = ComputeAggregate(pixels,
                                                settings.SamplesPerPixel);

            // L_N = (1 - a^(N+1)) / (1 - a)
            const Float64 analytic =
                (1.0 - FMath::Pow(albedo64,
                                  static_cast<Float64>(bounceList[b]) + 1.0)) /
                (1.0 - albedo64);

            // 逐像素的越界数只记录, 不判。
            //
            // 这里的单样本分布是**重尾且离散**的: 俄罗斯轮盘补偿之后,
            // 通量在每一次存活时被放大回 1, 于是样本值是 1.25、2.25、
            // 3.25 …… 这样一串, 大值出现的概率按几何级数掉。256 个样本
            // 的均值离正态还很远, 5.5σ 的高斯阈值在这里不成立 (实测
            // a=0.25 时 9216 个像素里有 16 个越界, 而高斯下应当是 0.006 个)。
            // 全图 236 万个样本的均值才落在中心极限定理管得住的范围里,
            // 所以判据判的是全图均值。
            EvaluatePerPixel(pixels, settings.SamplesPerPixel, analytic,
                             stats);

            const Float64 deviation = FMath::Abs(stats.Mean - analytic);
            const Float64 budget = kSigmaMultiplier * stats.StandardError +
                                   kFloatFloor * FMath::Max(1.0, analytic);

            LIMX_LOG(LogPathTraceCheck, Display,
                     "[能量守恒] a={} N={} — 均值 {} 解析 {} 偏差 {}e-6 "
                     "σ {} 预算 {}e-6 上界 {} 越界像素 {} (仅记录)",
                     albedo, bounceList[b], stats.Mean, analytic,
                     Scaled(deviation, 1.0e6), stats.StdDev,
                     Scaled(budget, 1.0e6), limit, stats.OutlierPixels);

            if (deviation > budget)
            {
                LIMX_LOG(LogPathTraceCheck, Error,
                         "[能量守恒] a={} N={} — 偏差 {} 超预算 {} "
                         "(5.5σ/√n = {}, σ = {})",
                         albedo, bounceList[b], deviation, budget,
                         kSigmaMultiplier * stats.StandardError, stats.StdDev);
                passed = false;
            }

            // 解析上界 1/(1-a): 任何一次弹射都只会把结果往上抬, 但永远
            // 抬不过这个几何级数的和
            if (stats.Mean > limit + kSigmaMultiplier * stats.StandardError +
                                 kFloatFloor * limit)
            {
                LIMX_LOG(LogPathTraceCheck, Error,
                         "[能量守恒] a={} N={} — 均值 {} 超过解析上界 {}",
                         albedo, bounceList[b], stats.Mean, limit);
                passed = false;
            }

            // 单调不减 —— 两次都是独立的蒙特卡洛估计, 所以允许各自的
            // 统计涨落, 但不允许系统性下降
            if (previousMean >= 0.0)
            {
                const Float64 slack =
                    kSigmaMultiplier * (stats.StandardError + previousError) +
                    kFloatFloor * FMath::Max(1.0, analytic);

                if (stats.Mean < previousMean - slack)
                {
                    LIMX_LOG(LogPathTraceCheck, Error,
                             "[能量守恒] a={} N={} — 均值 {} 低于上一档的 "
                             "{} 超过涨落 {}",
                             albedo, bounceList[b], stats.Mean, previousMean,
                             slack);
                    passed = false;
                }
            }

            previousMean  = stats.Mean;
            previousError = stats.StandardError;

            // 元判据: 封闭盒里一条都不该逃逸, 所以截断以外的终止只能是
            // 轮盘。N = 0 时轮盘还没轮到开, 截断比例必须恰好是 1。
            if (bounceList[b] == 0u && stats.TruncatedFraction != 1.0)
            {
                LIMX_LOG(LogPathTraceCheck, Error,
                         "[能量守恒] a={} N=0 — 截断比例 {} != 1, "
                         "封闭盒里主射线必定命中且必定被截断",
                         albedo, stats.TruncatedFraction);
                passed = false;
            }
        }

        // 收敛: 最后一档与极限之差必须落在解析尾项 a^(N+1)/(1-a) 之内
        const Float64 tail = FMath::Pow(albedo64, 25.0) / (1.0 - albedo64);

        LIMX_LOG(LogPathTraceCheck, Display,
                 "[能量守恒] a={} — 极限 {} 尾项 {} 末档均值 {}",
                 albedo, limit, tail, previousMean);

        if (FMath::Abs(previousMean - limit) >
            tail + kSigmaMultiplier * previousError + kFloatFloor * limit)
        {
            LIMX_LOG(LogPathTraceCheck, Error,
                     "[能量守恒] a={} — 末档均值 {} 距极限 {} 超过尾项 {} "
                     "加统计涨落", albedo, previousMean, limit, tail);
            passed = false;
        }
    }

    return passed;
}

// ============================================================================
// 判据三: 方差 ∝ 1/N
//
// 估计量是 N 个独立同分布样本的平均, 所以它的方差**必须**是 σ²/N。
// 在对数坐标上, ln Var 对 ln N 的斜率是 -1。
//
// 这一条验的是前两条都验不到的东西: **样本之间真的独立**。随机数种子
// 里把 (像素, 样本号) 加在一起再混合、或者按块重置状态, 都会让相邻样本
// 成对重复 —— 画面上完全看不出来 (均值还是对的, 白炉照样是 1.0), 只有
// 有效样本数比名义上少, 表现为一条比 -1 平缓的斜率。
//
// 方差不能用"逐样本的平方和"去算 —— 那样得到的是 σ²/N 的定义式本身,
// 恒等成立, 什么都没验。这里用 M 次**独立试验**的图像均值去算样本方差,
// 试验之间只差一个种子。
// ============================================================================

bool RunVarianceScalingChecks(FPathTracer& tracer)
{
    bool passed = true;

    FSceneBuilder builder;
    const UInt32 surface = builder.AddMaterialGray(0.6f, 1.0f);

    FCornellMaterials mat;
    mat.Floor = mat.Ceiling = mat.Back = surface;
    mat.Left  = mat.Right   = mat.Front = surface;
    mat.Blocks = surface;

    BuildCornellBox(builder, mat, true, true);

    FPathTraceScene scene;
    builder.Fill(scene);

    if (tracer.SetScene(scene) != ERHIResult::Success)
    {
        LIMX_LOG(LogPathTraceCheck, Error, "[方差标度] 场景上传失败");
        return false;
    }

    const FPathTraceCamera camera = MakeInsideCamera();

    constexpr UInt32 kTrialCount = 12;
    constexpr UInt32 kPointCount = 4;

    const UInt32 sppList[kPointCount] = { 16u, 64u, 256u, 1024u };

    FPathTraceSettings settings;
    settings.Width                = 64;
    settings.Height               = 64;
    settings.MaxBounce            = 8;
    settings.RussianRouletteStartDepth = 1;
    settings.EnvironmentRadiance  = 0.0f;
    settings.EnvironmentGradientY = 0.0f;
    settings.NormalOffset         = 1.0e-4f;

    const UInt32 pixelCount = settings.Width * settings.Height;

    Float64 variance[kPointCount] = {};

    TArray<Float64> trialMeans;
    trialMeans.SetSize(static_cast<SizeType>(pixelCount) * kTrialCount);

    for (UInt32 point = 0; point < kPointCount; ++point)
    {
        settings.SamplesPerPixel = sppList[point];

        for (UInt32 trial = 0; trial < kTrialCount; ++trial)
        {
            // 试验号进哈希 —— 每次试验拿到的是一条完全不同的随机数流,
            // 而不是同一条流的另一段 (后者在样本数翻倍时会与前一档重叠)。
            settings.TrialIndex = point * 1000u + trial;

            TArray<FPathTracePixel> pixels;

            if (tracer.Render(camera, settings, pixels) != ERHIResult::Success)
            {
                LIMX_LOG(LogPathTraceCheck, Error, "[方差标度] 渲染失败");
                return false;
            }

            const Float64 n = static_cast<Float64>(settings.SamplesPerPixel);

            for (UInt32 p = 0; p < pixelCount; ++p)
            {
                trialMeans[static_cast<SizeType>(p) * kTrialCount + trial] =
                    static_cast<Float64>(pixels[p].SumR) / n;
            }
        }

        // 逐像素求 M 次试验之间的样本方差, 再对像素取平均
        Float64 accumulated = 0.0;

        for (UInt32 p = 0; p < pixelCount; ++p)
        {
            Float64 mean = 0.0;

            for (UInt32 trial = 0; trial < kTrialCount; ++trial)
            {
                mean += trialMeans[static_cast<SizeType>(p) * kTrialCount +
                                   trial];
            }

            mean /= static_cast<Float64>(kTrialCount);

            Float64 sumSquares = 0.0;

            for (UInt32 trial = 0; trial < kTrialCount; ++trial)
            {
                const Float64 d =
                    trialMeans[static_cast<SizeType>(p) * kTrialCount + trial] -
                    mean;

                sumSquares += d * d;
            }

            accumulated += sumSquares / static_cast<Float64>(kTrialCount - 1);
        }

        variance[point] = accumulated / static_cast<Float64>(pixelCount);

        LIMX_LOG(LogPathTraceCheck, Display,
                 "[方差标度] spp={} — 均值的方差 {}e-6 (×spp = {}e-3), "
                 "{} 次独立试验",
                 sppList[point], Scaled(variance[point], 1.0e6),
                 Scaled(variance[point] * static_cast<Float64>(sppList[point]),
                        1.0e3),
                 kTrialCount);
    }

    // 元判据: 方差必须真的存在, 也必须真的在降
    if (variance[0] <= 1.0e-6)
    {
        LIMX_LOG(LogPathTraceCheck, Error,
                 "[方差标度] spp=16 时方差只有 {} — 估计量没有随机性, "
                 "回归出来的斜率没有意义", variance[0]);
        passed = false;
    }

    if (variance[kPointCount - 1] >= variance[0] / 8.0)
    {
        LIMX_LOG(LogPathTraceCheck, Error,
                 "[方差标度] spp 从 16 涨到 1024 (64 倍), 方差只从 {} 降到 "
                 "{} — 降幅不足八倍", variance[0],
                 variance[kPointCount - 1]);
        passed = false;
    }

    // 对数坐标上的最小二乘斜率
    Float64 meanX = 0.0;
    Float64 meanY = 0.0;

    Float64 logX[kPointCount] = {};
    Float64 logY[kPointCount] = {};

    for (UInt32 i = 0; i < kPointCount; ++i)
    {
        logX[i] = static_cast<Float64>(
            FMath::Log(static_cast<Float32>(sppList[i])));
        logY[i] = static_cast<Float64>(
            FMath::Log(static_cast<Float32>(variance[i])));

        meanX += logX[i];
        meanY += logY[i];
    }

    meanX /= static_cast<Float64>(kPointCount);
    meanY /= static_cast<Float64>(kPointCount);

    Float64 numerator   = 0.0;
    Float64 denominator = 0.0;

    for (UInt32 i = 0; i < kPointCount; ++i)
    {
        numerator   += (logX[i] - meanX) * (logY[i] - meanY);
        denominator += (logX[i] - meanX) * (logX[i] - meanX);
    }

    const Float64 slope = (denominator > 0.0) ? (numerator / denominator) : 0.0;

    // 残差 —— 斜率对了但点不在一条直线上, 说明标度律本身不成立
    Float64 maxResidual = 0.0;

    for (UInt32 i = 0; i < kPointCount; ++i)
    {
        const Float64 predicted = meanY + slope * (logX[i] - meanX);

        maxResidual = FMath::Max(maxResidual,
                                 FMath::Abs(logY[i] - predicted));
    }

    // 容差 0.08: M = 12 次试验的方差估计相对标准差 √(2/11) = 0.426,
    // 对 64×64 = 4096 个像素取平均后降到 0.426/64 = 0.0067, 折成对数
    // 也是 0.0067。四个点跨 ln(64) = 4.16, 斜率的统计不确定度约 0.005。
    // 0.08 是它的十六倍 —— 既容得下试验涨落, 又远小于任何一种"样本不
    // 独立"能造成的偏离 (成对重复的样本会把斜率抬到 -0.5 附近)。
    constexpr Float64 kSlopeTolerance = 0.08;

    LIMX_LOG(LogPathTraceCheck, Display,
             "[方差标度] 对数回归斜率 {} (目标 -1, 容差 {}), "
             "最大残差 {}e-3",
             slope, kSlopeTolerance, Scaled(maxResidual, 1.0e3));

    if (FMath::Abs(slope + 1.0) > kSlopeTolerance)
    {
        LIMX_LOG(LogPathTraceCheck, Error,
                 "[方差标度] 斜率 {} 偏离 -1 超过 {} — 样本之间不独立, "
                 "或者估计量不是简单平均", slope, kSlopeTolerance);
        passed = false;
    }

    if (maxResidual > 0.15)
    {
        LIMX_LOG(LogPathTraceCheck, Error,
                 "[方差标度] 四个点在对数坐标上不共线, 最大残差 {} — "
                 "方差与 spp 之间不是幂律关系", maxResidual);
        passed = false;
    }

    return passed;
}

} // namespace

// ============================================================================
// RunPathTraceChecks
// ============================================================================

bool RunPathTraceChecks(FRenderContext* context)
{
    if (context == nullptr || context->GetDevice() == nullptr)
    {
        LIMX_LOG(LogPathTraceCheck, Error, "[路径追踪自检] 渲染上下文为空");
        return false;
    }

    IRHIDevice* device = context->GetDevice();

    // "不支持"判失败而不是判通过 —— 判通过的话这条判据在任何不支持光追的
    // 机器上都是空的, 而那正是它最需要说话的场合。
    if (!device->IsRayTracingSupported())
    {
        LIMX_LOG(LogPathTraceCheck, Error,
                 "[路径追踪自检] 设备不支持光线追踪 — 判据无法执行, "
                 "判定为失败");
        return false;
    }

    FPathTracer tracer;

    if (tracer.Initialize(device, context) != ERHIResult::Success)
    {
        LIMX_LOG(LogPathTraceCheck, Error, "[路径追踪自检] 路径追踪器初始化失败");
        return false;
    }

    bool passed = true;

    passed = RunFurnaceChecks(tracer) && passed;
    passed = RunEnergyConservationChecks(tracer) && passed;
    passed = RunVarianceScalingChecks(tracer) && passed;

    tracer.Shutdown();

    if (passed)
    {
        LIMX_LOG(LogPathTraceCheck, Display, "[路径追踪自检] 通过");
    }
    else
    {
        LIMX_LOG(LogPathTraceCheck, Error, "[路径追踪自检] 失败");
    }

    return passed;
}

// ============================================================================
// RenderPathTraceReferenceImage
// ============================================================================

bool RenderPathTraceReferenceImage(FRenderContext* context,
                                   const FString& outputPath,
                                   UInt32 width, UInt32 height,
                                   UInt32 samplesPerPixel)
{
    if (context == nullptr || context->GetDevice() == nullptr)
    {
        LIMX_LOG(LogPathTraceCheck, Error, "[路径追踪参考图] 渲染上下文为空");
        return false;
    }

    IRHIDevice* device = context->GetDevice();

    if (!device->IsRayTracingSupported())
    {
        LIMX_LOG(LogPathTraceCheck, Error,
                 "[路径追踪参考图] 设备不支持光线追踪");
        return false;
    }

    FPathTracer tracer;

    if (tracer.Initialize(device, context) != ERHIResult::Success)
    {
        return false;
    }

    // 经典 Cornell 盒的反照率与光源辐射度
    FSceneBuilder builder;

    const UInt32 whiteMat = builder.AddMaterial(0.725f, 0.710f, 0.680f);
    const UInt32 redMat   = builder.AddMaterial(0.630f, 0.065f, 0.050f);
    const UInt32 greenMat = builder.AddMaterial(0.140f, 0.450f, 0.091f);
    const UInt32 lightMat = builder.AddMaterial(0.0f, 0.0f, 0.0f,
                                                17.0f, 12.0f, 4.0f);

    FCornellMaterials mat;
    mat.Floor   = whiteMat;
    mat.Ceiling = whiteMat;
    mat.Back    = whiteMat;
    mat.Left    = redMat;
    mat.Right   = greenMat;
    mat.Front   = whiteMat;
    mat.Blocks  = whiteMat;

    BuildCornellBox(builder, mat, false, true);

    // 天花板灯 —— 略低于天花板, 免得与天花板共面
    constexpr Float32 lightY = 0.999f;

    builder.AddQuad(FVector3(-0.25f, lightY, -0.25f),
                    FVector3( 0.25f, lightY, -0.25f),
                    FVector3( 0.25f, lightY,  0.25f),
                    FVector3(-0.25f, lightY,  0.25f), lightMat);

    FPathTraceScene scene;
    builder.Fill(scene);

    if (tracer.SetScene(scene) != ERHIResult::Success)
    {
        tracer.Shutdown();
        return false;
    }

    const FPathTraceCamera camera = MakeOutsideCamera();

    FPathTraceSettings settings;
    settings.Width                = width;
    settings.Height               = height;
    settings.SamplesPerPixel      = samplesPerPixel;
    settings.MaxBounce            = 16;
    settings.RussianRouletteStartDepth = 3;
    settings.EnvironmentRadiance  = 0.0f;
    settings.EnvironmentGradientY = 0.0f;
    settings.NormalOffset         = 1.0e-4f;

    TArray<FPathTracePixel> pixels;

    if (tracer.Render(camera, settings, pixels) != ERHIResult::Success)
    {
        tracer.Shutdown();
        return false;
    }

    tracer.Shutdown();

    const FString header = StringFormat("P6\n{} {}\n255\n", width, height);

    TArray<UInt8> file;
    file.Reserve(header.GetLength() +
                 static_cast<SizeType>(width) * height * 3);

    for (SizeType i = 0; i < header.GetLength(); ++i)
    {
        file.Add(static_cast<UInt8>(header[i]));
    }

    const Float32 inverseSamples =
        1.0f / static_cast<Float32>(samplesPerPixel);

    for (SizeType i = 0; i < pixels.GetSize(); ++i)
    {
        const Float32 channels[3] =
        {
            pixels[i].SumR * inverseSamples,
            pixels[i].SumG * inverseSamples,
            pixels[i].SumB * inverseSamples,
        };

        for (UInt32 c = 0; c < 3; ++c)
        {
            // Reinhard + sRGB 近似 —— 参考图是给人看的, 数值判据一律
            // 用回读的浮点值, 不经过这一步
            const Float32 mapped = channels[c] / (1.0f + channels[c]);
            const Float32 gamma  = FMath::Pow(FMath::Clamp(mapped, 0.0f, 1.0f),
                                              1.0f / 2.2f);

            file.Add(static_cast<UInt8>(
                FMath::Clamp(gamma * 255.0f + 0.5f, 0.0f, 255.0f)));
        }
    }

    const bool written = FPlatformFile::WriteAllBytes(outputPath,
                                                      file.GetData(),
                                                      file.GetSize());

    if (written)
    {
        LIMX_LOG(LogPathTraceCheck, Display,
                 "[路径追踪参考图] 已写入 {} ({}x{}, {} spp)",
                 outputPath.GetCStr(), width, height, samplesPerPixel);
    }
    else
    {
        LIMX_LOG(LogPathTraceCheck, Error,
                 "[路径追踪参考图] 写入失败: {}", outputPath.GetCStr());
    }

    return written;
}

} // namespace Limx
