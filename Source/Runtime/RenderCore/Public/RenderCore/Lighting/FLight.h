// ============================================================
// 文件名称：FLight.h
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：数据驱动光源 — 光源对象仅持有 CPU 侧属性参数，
//          GPU 端数据通过 FLightManager 批量打包上传，
//          实现 CPU/GPU 数据解耦。光源类型通过枚举区分，
//          避免虚函数开销，利于 SOA 打包。
// 功能描述：光源系统核心类型定义 — ELightType 光源类型枚举、
//          FLightData GPU 端光源数据 (std140 对齐)、
//          FLightingUBO 场景级光照 UBO 数据布局、
//          FLight CPU 侧光源对象 (类型+变换+光照属性)。
// 技术特性：FLightData 按 std140 规则对齐 (16 字节边界)，
//          单光源 80 字节；FLightingUBO 包含最多 16 盏光源
//          + 全局光照参数，总计 1344 字节；
//          FLight 提供便捷工厂创建方向光/点光/聚光灯。
//
// ── 枚举表 ──────────────────────────────────────────────────
// │ 枚举名                     │ 描述                          │
// │───────────────────────────│──────────────────────────────│
// │ ELightType                │ 光源类型 (方向光/点光/聚光灯)    │
//
// ── 结构体/类表 ──────────────────────────────────────────────
// │ 名称                       │ 描述                          │
// │───────────────────────────│──────────────────────────────│
// │ FLightData                │ 单光源 GPU 数据 (std140, 80B)  │
// │ FLightingUBO              │ 场景光照 UBO (std140, 1344B)   │
// │ FLight                    │ CPU 侧光源对象                 │
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                      │ 描述                          │
// │────────────────────────────│──────────────────────────────│
// │ FLight::CreateDirectional()│ 创建方向光                     │
// │ FLight::CreatePoint()      │ 创建点光源                     │
// │ FLight::CreateSpot()       │ 创建聚光灯                     │
// │ FLight::ToGpuData()        │ 转换为 GPU 数据                │
// │ FLight::SetDirection()     │ 设置光源方向                   │
// │ FLight::SetPosition()      │ 设置光源位置                   │
// │ FLight::SetColor()         │ 设置光源颜色                   │
// │ FLight::SetIntensity()     │ 设置光源强度                   │
// │ FLight::SetRange()         │ 设置衰减距离                   │
// │ FLight::SetSpotAngles()    │ 设置聚光灯内外锥角              │
// │ FLight::SetEnabled()       │ 设置启用/禁用                  │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                          │
// │─────────────│──────────│──────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M0.5 光照系统)       │
// ============================================================

#pragma once

#include "RenderCore/RenderCoreMinimal.h"

namespace Limx
{

// ============================================================================
// ELightType — 光源类型
// ============================================================================

enum class ELightType : UInt32
{
    Directional = 0,   // 方向光 (平行光，无位置，无衰减)
    Point       = 1,   // 点光源 (全向，有位置，距离衰减)
    Spot        = 2,   // 聚光灯 (锥形，有位置+方向，距离+角度衰减)

    Count       = 3
};

// ============================================================================
// kMaxLightCount — 场景最大光源数量
// ============================================================================

static constexpr UInt32 kMaxLightCount = 16;

// ============================================================================
// FLightData — 单光源 GPU 数据 (std140 对齐, 80 字节)
//
// std140 布局规则:
//   vec4 → 16 字节对齐
//   float → 4 字节对齐，但在 vec4 之后自然对齐
//   整体结构体按 16 字节倍数填充
//
// 内存布局:
//   偏移 0:  PositionAndType  (vec4) — xyz=位置, w=类型(float cast)
//   偏移 16: DirectionAndRange (vec4) — xyz=方向, w=衰减距离
//   偏移 32: ColorAndIntensity (vec4) — xyz=颜色, w=强度
//   偏移 48: AttenuationParams (vec4) — x=常量衰减, y=线性衰减, z=二次衰减, w=保留
//   偏移 64: SpotParams        (vec4) — x=内锥角余弦, y=外锥角余弦, z=保留, w=保留
//   总计: 80 字节 (5 × vec4)
// ============================================================================

struct FLightData
{
    // vec4: xyz=世界空间位置 (方向光忽略), w=光源类型 (float cast of ELightType)
    Float32 PositionX    = 0.0f;
    Float32 PositionY    = 0.0f;
    Float32 PositionZ    = 0.0f;
    Float32 Type         = 0.0f;

    // vec4: xyz=世界空间方向 (归一化, 点光源忽略), w=最大衰减距离
    Float32 DirectionX   = 0.0f;
    Float32 DirectionY   = -1.0f;
    Float32 DirectionZ   = 0.0f;
    Float32 Range        = 10.0f;

    // vec4: xyz=光源颜色 (线性空间 RGB), w=光源强度乘数
    Float32 ColorR       = 1.0f;
    Float32 ColorG       = 1.0f;
    Float32 ColorB       = 1.0f;
    Float32 Intensity    = 1.0f;

    // vec4: 衰减参数 — x=常量, y=线性, z=二次, w=保留 (pad)
    Float32 AttConstant  = 1.0f;
    Float32 AttLinear    = 0.09f;
    Float32 AttQuadratic = 0.032f;
    Float32 AttPad       = 0.0f;

    // vec4: 聚光灯参数 — x=内锥角余弦, y=外锥角余弦, z=保留, w=保留
    Float32 SpotInnerCos = 0.9763f;   // cos(12.5°) // NOLINT
    Float32 SpotOuterCos = 0.9659f;   // cos(15°)   // NOLINT
    Float32 SpotPad0     = 0.0f;
    Float32 SpotPad1     = 0.0f;
};

// 编译时验证 FLightData 大小 (std140 要求 16 字节倍数)
static_assert(sizeof(FLightData) == 80,
    "FLightData 必须为 80 字节 (5 × vec4, std140 对齐)");

// ============================================================================
// FCascadedShadowInfo — 渲染器交给光照系统的级联阴影数据
// ============================================================================

/// 级联阴影信息
///
/// 单独成结构而非一串参数: 参数数量已到七个, 顺序写错编译器不会报错
/// (都是 Float32/FMatrix), 而症状是阴影整体错位, 极难定位到调用点。
struct FCascadedShadowInfo
{
    static constexpr UInt32 kCascadeCount = 3;

    FMatrix CascadeViewProj[kCascadeCount];

    /// 各级外边界的径向距离
    Float32 CascadeSplits[kCascadeCount] = {};

    Float32 DepthBias     = 0.0015f;
    Float32 NormalBias    = 0.05f;
    Float32 ShadowMapSize = 2048.0f;
};

// ============================================================================
// FLightingUBO — 场景级光照 Uniform Buffer (std140, set 2, binding 0)
//
// 内存布局:
//   偏移 0:      Lights[16]     — 16 × FLightData = 1280 字节
//   偏移 1280:   LightCount     (vec4 的 .x) — 活跃光源数量
//   偏移 1296:   CameraPosition (vec4 的 .xyz) — 相机世界空间位置
//   偏移 1312:   AmbientColor   (vec4 的 .xyz) — 环境光颜色
//   偏移 1328:   AmbientIntensity (同上 vec4 的 .w) — 环境光强度
//   偏移 1344:   CascadeViewProj[3] (mat4×3) — 各级联的光源视图投影矩阵
//   偏移 1536:   CascadeSplits  (vec4) — xyz=各级外边界的径向距离
//   偏移 1552:   ShadowParams   (vec4) — x=深度偏移, y=法线偏移,
//                                        z=阴影贴图边长, w=是否启用
//   总计: 1568 字节
//
// 阴影数据放在光照 UBO 而非另建一个: 它逻辑上就是"主方向光的属性",
// 且与光源数据在同一个更新时机。单独一个 UBO 意味着多一次绑定、
// 多一份帧同步, 却没有任何一处需要单独更新它。
//
// 注意 std140: mat4 数组的每个元素本就 16 字节对齐, 无需额外填充;
// 但结构体在数组之前必须已对齐到 16, 因此环境光那一组 vec4 之后正好接上。
// ============================================================================

struct FLightingUBO
{
    // 光源数据数组 (固定 16 盏，实际使用 LightCount 盏)
    FLightData Lights[kMaxLightCount];

    // vec4: x=活跃光源数量 (uint→float), y/z/w=保留
    Float32 LightCount     = 0.0f;
    Float32 LightCountPad0 = 0.0f;
    Float32 LightCountPad1 = 0.0f;
    Float32 LightCountPad2 = 0.0f;

    // vec4: xyz=相机世界空间位置, w=保留
    Float32 CameraPositionX = 0.0f;
    Float32 CameraPositionY = 0.0f;
    Float32 CameraPositionZ = 0.0f;
    Float32 CameraPositionW = 0.0f;

    // vec4: xyz=环境光颜色 (线性 RGB), w=环境光强度
    Float32 AmbientColorR    = 0.03f;
    Float32 AmbientColorG    = 0.03f;
    Float32 AmbientColorB    = 0.03f;
    Float32 AmbientIntensity = 1.0f;

    /// 级联层数 — 必须与 FShadowPass::kCascadeCount 及着色器常量一致
    static constexpr UInt32 kShadowCascadeCount = 3;

    // mat4[3]: 各级联的视图投影矩阵 —— 把世界坐标变换到该级的阴影贴图空间
    FMatrix CascadeViewProj[kShadowCascadeCount];

    // vec4: xyz=各级外边界的径向距离, w=保留
    //
    // 用径向距离而非视空间 Z: 级联体积是按包围球拟合的, 球以径向距离定义。
    // 两者口径不一致时, 切片角落会选到未覆盖该处的级别, 表现为视野边缘
    // 出现一圈错误阴影。
    Float32 CascadeSplit0 = 0.0f;
    Float32 CascadeSplit1 = 0.0f;
    Float32 CascadeSplit2 = 0.0f;
    Float32 CascadeSplitPad = 0.0f;

    // vec4: x=深度偏移, y=法线偏移, z=阴影贴图边长, w=是否启用(0/1)
    //
    // 两个偏移量分工不同, 都必须有:
    //   深度偏移抵消"深度贴图分辨率有限导致的自遮挡"(shadow acne);
    //   法线偏移沿法线把采样点推离表面, 处理掠射角下深度偏移救不了的情形。
    //   只用深度偏移的话, 掠射角处要么仍有 acne, 要么偏移大到阴影脱离物体
    //   (peter-panning)。
    Float32 ShadowDepthBias   = 0.0015f;
    Float32 ShadowNormalBias  = 0.05f;
    Float32 ShadowMapSize     = 2048.0f;
    Float32 ShadowEnabled     = 0.0f;

    // vec4: x=是否启用 IBL(0/1), y=IBL 强度倍数, z/w=保留
    //
    // 需要一个显式开关而非"辐照度贴图是否为黑"来判断: 没有环境贴图时
    // 描述符上绑的是一张 1x1 的黑色占位图, 采样它得到的确实是零 ——
    // 但那样常数环境光也一并被乘没了, 场景会整个暗下去。开关让着色器
    // 能在"用 IBL"与"用常数环境光"之间选择, 而不是把两者相加或相乘。
    Float32 IblEnabled   = 0.0f;
    Float32 IblIntensity = 1.0f;
    Float32 IblPad0      = 0.0f;
    Float32 IblPad1      = 0.0f;
};

// 编译时验证 FLightingUBO 大小
// 16 × 80 + 3 × 16 = 1280 + 48 = 1328 字节 (光源 + 计数 + 相机 + 环境光)
// + mat4×3 级联矩阵 192 + vec4 切分 16 + vec4 阴影参数 16 = 1552 字节
// + vec4 IBL 参数 16 = 1568 字节
// std140 要求数组元素与结构体尾部都对齐到 16 字节, 1568 已是 16 的倍数
static_assert(sizeof(FLightingUBO) == 1568,
    "FLightingUBO 必须为 1568 字节 (std140 对齐)");

// ============================================================================
// FLight — CPU 侧光源对象
// ============================================================================

class FLight
{
public:
    LIMX_NON_COPYABLE(FLight);

    FLight();
    ~FLight() = default;

    // 允许移动
    FLight(FLight&& other) noexcept;
    FLight& operator=(FLight&& other) noexcept;

    // ====================================================================
    // 便捷工厂
    // ====================================================================

    /// 创建方向光
    /// @param direction  光照方向 (从光源射出的方向，将被归一化)
    /// @param color      光源颜色 (线性 RGB)
    /// @param intensity  光源强度
    static FLight CreateDirectional(
        const FVector3& direction,
        const FLinearColor& color,
        Float32 intensity);

    /// 创建点光源
    /// @param position   世界空间位置
    /// @param color      光源颜色 (线性 RGB)
    /// @param intensity  光源强度
    /// @param range      最大衰减距离
    static FLight CreatePoint(
        const FVector3& position,
        const FLinearColor& color,
        Float32 intensity,
        Float32 range = 10.0f);

    /// 创建聚光灯
    /// @param position       世界空间位置
    /// @param direction      光照方向 (将被归一化)
    /// @param color          光源颜色 (线性 RGB)
    /// @param intensity      光源强度
    /// @param innerAngleDeg  内锥角 (度, 全亮区域)
    /// @param outerAngleDeg  外锥角 (度, 衰减边界)
    /// @param range          最大衰减距离
    static FLight CreateSpot(
        const FVector3& position,
        const FVector3& direction,
        const FLinearColor& color,
        Float32 intensity,
        Float32 innerAngleDeg = 12.5f,
        Float32 outerAngleDeg = 17.5f,
        Float32 range = 10.0f);

    // ====================================================================
    // GPU 数据转换
    // ====================================================================

    /// 将当前光源状态转换为 GPU 数据 (FLightData)
    LIMX_NODISCARD FLightData ToGpuData() const;

    // ====================================================================
    // 属性访问器
    // ====================================================================

    LIMX_NODISCARD ELightType GetType() const { return m_Type; }

    LIMX_NODISCARD const FVector3& GetPosition() const { return m_Position; }
    void SetPosition(const FVector3& position) { m_Position = position; }

    LIMX_NODISCARD const FVector3& GetDirection() const { return m_Direction; }
    /// 设置光照方向 (将被归一化)
    void SetDirection(const FVector3& direction);

    LIMX_NODISCARD const FLinearColor& GetColor() const { return m_Color; }
    void SetColor(const FLinearColor& color) { m_Color = color; }

    LIMX_NODISCARD Float32 GetIntensity() const { return m_Intensity; }
    void SetIntensity(Float32 intensity) { m_Intensity = intensity; }

    LIMX_NODISCARD Float32 GetRange() const { return m_Range; }
    void SetRange(Float32 range) { m_Range = FMath::Max(range, 0.001f); }

    LIMX_NODISCARD Float32 GetAttConstant() const { return m_AttConstant; }
    LIMX_NODISCARD Float32 GetAttLinear() const { return m_AttLinear; }
    LIMX_NODISCARD Float32 GetAttQuadratic() const { return m_AttQuadratic; }

    /// 设置衰减参数 (常量/线性/二次)
    void SetAttenuation(Float32 constant, Float32 linear, Float32 quadratic);

    LIMX_NODISCARD Float32 GetSpotInnerAngle() const { return m_SpotInnerAngleDeg; }
    LIMX_NODISCARD Float32 GetSpotOuterAngle() const { return m_SpotOuterAngleDeg; }

    /// 设置聚光灯内外锥角 (度)
    /// @param innerDeg  内锥角 (度, 全亮区域)
    /// @param outerDeg  外锥角 (度, 衰减边界, 必须 >= 内锥角)
    void SetSpotAngles(Float32 innerDeg, Float32 outerDeg);

    LIMX_NODISCARD bool IsEnabled() const { return m_IsEnabled; }
    void SetEnabled(bool isEnabled) { m_IsEnabled = isEnabled; }

    LIMX_NODISCARD const AnsiChar* GetDebugName() const { return m_DebugName; }
    void SetDebugName(const AnsiChar* name) { m_DebugName = name; }

private:
    // ====================================================================
    // 成员
    // ====================================================================

    ELightType   m_Type               = ELightType::Directional;

    /// 世界空间位置 (方向光忽略)
    FVector3     m_Position           = FVector3(0.0f, 0.0f, 0.0f);

    /// 光照方向 (归一化, 点光源忽略)
    FVector3     m_Direction          = FVector3(0.0f, -1.0f, 0.0f);

    /// 光源颜色 (线性 RGB)
    FLinearColor m_Color              = FLinearColor(1.0f, 1.0f, 1.0f, 1.0f);

    /// 光源强度乘数
    Float32      m_Intensity          = 1.0f;

    /// 最大衰减距离 (方向光忽略)
    Float32      m_Range              = 10.0f;

    /// 衰减参数: 1 / (c + l*d + q*d²)
    Float32      m_AttConstant        = 1.0f;
    Float32      m_AttLinear          = 0.09f;
    Float32      m_AttQuadratic       = 0.032f;

    /// 聚光灯内锥角 (度)
    Float32      m_SpotInnerAngleDeg  = 12.5f;

    /// 聚光灯外锥角 (度)
    Float32      m_SpotOuterAngleDeg  = 17.5f;

    /// 是否启用
    bool         m_IsEnabled          = true;

    /// 调试名称 (非拥有指针，不管理生命周期)
    const AnsiChar* m_DebugName       = "UnnamedLight";
};

} // namespace Limx
