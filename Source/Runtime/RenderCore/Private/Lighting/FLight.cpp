// ============================================================
// 文件名称：FLight.cpp
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：数据驱动光源 — 所有光源属性为纯数据操作，
//          ToGpuData() 将 CPU 对象打包为 GPU std140 布局。
// 功能描述：FLight 完整实现 — 构造/移动语义、便捷工厂方法
//          (方向光/点光/聚光灯)、属性修改器、GPU 数据转换。
// 技术特性：工厂方法返回值语义 (RVO/NRVO)；方向向量自动归一化；
//          聚光灯角度自动钳位并预计算余弦值；衰减参数可自定义。
//
// ── 函数表 ──────────────────────────────────────────────────
// │ 函数名                        │ 描述                          │
// │──────────────────────────────│──────────────────────────────│
// │ FLight()                     │ 默认构造 (方向光)               │
// │ FLight(FLight&&)             │ 移动构造                       │
// │ operator=(FLight&&)          │ 移动赋值                       │
// │ CreateDirectional()          │ 创建方向光                     │
// │ CreatePoint()                │ 创建点光源                     │
// │ CreateSpot()                 │ 创建聚光灯                     │
// │ ToGpuData()                  │ 转换为 GPU 数据 (FLightData)    │
// │ SetDirection()               │ 设置并归一化方向                │
// │ SetAttenuation()             │ 设置衰减参数                   │
// │ SetSpotAngles()              │ 设置聚光灯锥角                  │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                          │
// │─────────────│──────────│──────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M0.5 光照系统)       │
// ============================================================

#include "RenderCore/Lighting/FLight.h"

namespace Limx
{

// ============================================================================
// 构造
// ============================================================================

FLight::FLight()
    : m_Type(ELightType::Directional)
    , m_Position(0.0f, 0.0f, 0.0f)
    , m_Direction(0.0f, -1.0f, 0.0f)
    , m_Color(1.0f, 1.0f, 1.0f, 1.0f)
    , m_Intensity(1.0f)
    , m_Range(10.0f)
    , m_AttConstant(1.0f)
    , m_AttLinear(0.09f)
    , m_AttQuadratic(0.032f)
    , m_SpotInnerAngleDeg(12.5f)
    , m_SpotOuterAngleDeg(17.5f)
    , m_IsEnabled(true)
    , m_DebugName("UnnamedLight")
{
}

// ============================================================================
// 移动语义
// ============================================================================

FLight::FLight(FLight&& other) noexcept
    : m_Type(other.m_Type)
    , m_Position(other.m_Position)
    , m_Direction(other.m_Direction)
    , m_Color(other.m_Color)
    , m_Intensity(other.m_Intensity)
    , m_Range(other.m_Range)
    , m_AttConstant(other.m_AttConstant)
    , m_AttLinear(other.m_AttLinear)
    , m_AttQuadratic(other.m_AttQuadratic)
    , m_SpotInnerAngleDeg(other.m_SpotInnerAngleDeg)
    , m_SpotOuterAngleDeg(other.m_SpotOuterAngleDeg)
    , m_IsEnabled(other.m_IsEnabled)
    , m_CastsShadow(other.m_CastsShadow)
    , m_DebugName(other.m_DebugName)
{
    other.m_IsEnabled = false;
    other.m_DebugName = "MovedLight";
}

FLight& FLight::operator=(FLight&& other) noexcept
{
    if (this != &other)
    {
        m_Type               = other.m_Type;
        m_Position           = other.m_Position;
        m_Direction          = other.m_Direction;
        m_Color              = other.m_Color;
        m_Intensity          = other.m_Intensity;
        m_Range              = other.m_Range;
        m_AttConstant        = other.m_AttConstant;
        m_AttLinear          = other.m_AttLinear;
        m_AttQuadratic       = other.m_AttQuadratic;
        m_SpotInnerAngleDeg  = other.m_SpotInnerAngleDeg;
        m_SpotOuterAngleDeg  = other.m_SpotOuterAngleDeg;
        m_IsEnabled          = other.m_IsEnabled;
        m_CastsShadow        = other.m_CastsShadow;
        m_DebugName          = other.m_DebugName;

        other.m_IsEnabled = false;
        other.m_DebugName = "MovedLight";
    }
    return *this;
}

// ============================================================================
// CreateDirectional — 创建方向光
// ============================================================================

FLight FLight::CreateDirectional(
    const FVector3& direction,
    const FLinearColor& color,
    Float32 intensity)
{
    FLight light;
    light.m_Type      = ELightType::Directional;
    light.m_Color     = color;
    light.m_Intensity = intensity;
    light.m_DebugName = "DirectionalLight";

    // 归一化方向
    Float32 lengthSq = direction.X * direction.X +
                        direction.Y * direction.Y +
                        direction.Z * direction.Z;
    if (lengthSq > FMath::kSmallNumber)
    {
        Float32 invLength = 1.0f / FMath::Sqrt(lengthSq);
        light.m_Direction = FVector3(
            direction.X * invLength,
            direction.Y * invLength,
            direction.Z * invLength);
    }
    else
    {
        // 退化方向 — 默认向下
        light.m_Direction = FVector3(0.0f, -1.0f, 0.0f);
    }

    return light;
}

// ============================================================================
// CreatePoint — 创建点光源
// ============================================================================

FLight FLight::CreatePoint(
    const FVector3& position,
    const FLinearColor& color,
    Float32 intensity,
    Float32 range)
{
    FLight light;
    light.m_Type      = ELightType::Point;
    light.m_Position  = position;
    light.m_Color     = color;
    light.m_Intensity = intensity;
    light.m_Range     = FMath::Max(range, 0.001f);
    light.m_DebugName = "PointLight";

    return light;
}

// ============================================================================
// CreateSpot — 创建聚光灯
// ============================================================================

FLight FLight::CreateSpot(
    const FVector3& position,
    const FVector3& direction,
    const FLinearColor& color,
    Float32 intensity,
    Float32 innerAngleDeg,
    Float32 outerAngleDeg,
    Float32 range)
{
    FLight light;
    light.m_Type      = ELightType::Spot;
    light.m_Position  = position;
    light.m_Color     = color;
    light.m_Intensity = intensity;
    light.m_Range     = FMath::Max(range, 0.001f);
    light.m_DebugName = "SpotLight";

    // 归一化方向
    Float32 lengthSq = direction.X * direction.X +
                        direction.Y * direction.Y +
                        direction.Z * direction.Z;
    if (lengthSq > FMath::kSmallNumber)
    {
        Float32 invLength = 1.0f / FMath::Sqrt(lengthSq);
        light.m_Direction = FVector3(
            direction.X * invLength,
            direction.Y * invLength,
            direction.Z * invLength);
    }
    else
    {
        light.m_Direction = FVector3(0.0f, -1.0f, 0.0f);
    }

    // 钳位聚光灯角度
    light.SetSpotAngles(innerAngleDeg, outerAngleDeg);

    return light;
}

// ============================================================================
// ToGpuData — 将 CPU 光源转换为 GPU std140 数据
// ============================================================================

FLightData FLight::ToGpuData() const
{
    FLightData data;

    // 位置 + 类型
    data.PositionX = m_Position.X;
    data.PositionY = m_Position.Y;
    data.PositionZ = m_Position.Z;
    data.Type      = static_cast<Float32>(static_cast<UInt32>(m_Type));

    // 方向 + 衰减距离
    data.DirectionX = m_Direction.X;
    data.DirectionY = m_Direction.Y;
    data.DirectionZ = m_Direction.Z;
    data.Range      = m_Range;

    // 颜色 + 强度
    data.ColorR    = m_Color.R;
    data.ColorG    = m_Color.G;
    data.ColorB    = m_Color.B;
    data.Intensity = m_Intensity;

    // 衰减参数
    data.AttConstant  = m_AttConstant;
    data.AttLinear    = m_AttLinear;
    data.AttQuadratic = m_AttQuadratic;
    data.AttPad       = 0.0f;

    // 聚光灯参数 — 预计算余弦值
    data.SpotInnerCos = FMath::Cos(
        FMath::DegreesToRadians(m_SpotInnerAngleDeg));
    data.SpotOuterCos = FMath::Cos(
        FMath::DegreesToRadians(m_SpotOuterAngleDeg));
    // 阴影块下标由 FLightManager 在打包时分配 —— 它才知道有多少盏灯在争
    // 那 64 块。这里恒填 -1 (不投影), 而不是留着不写: 结构体是逐字段赋值
    // 的, 漏一个字段的表现是那一格残留上一盏灯的值, 于是本该没有阴影的灯
    // 采到了别人的块。
    data.ShadowTileIndex = -1.0f;
    data.SpotPad1        = 0.0f;

    return data;
}

// ============================================================================
// SetDirection — 归一化方向
// ============================================================================

void FLight::SetDirection(const FVector3& direction)
{
    Float32 lengthSq = direction.X * direction.X +
                        direction.Y * direction.Y +
                        direction.Z * direction.Z;
    if (lengthSq > FMath::kSmallNumber)
    {
        Float32 invLength = 1.0f / FMath::Sqrt(lengthSq);
        m_Direction = FVector3(
            direction.X * invLength,
            direction.Y * invLength,
            direction.Z * invLength);
    }
}

// ============================================================================
// SetAttenuation — 设置衰减参数
// ============================================================================

void FLight::SetAttenuation(Float32 constant, Float32 linear, Float32 quadratic)
{
    m_AttConstant  = FMath::Max(constant, 0.0f);
    m_AttLinear    = FMath::Max(linear, 0.0f);
    m_AttQuadratic = FMath::Max(quadratic, 0.0f);

    // 确保常量衰减至少为极小值，避免除零
    if (m_AttConstant < FMath::kSmallNumber &&
        m_AttLinear < FMath::kSmallNumber &&
        m_AttQuadratic < FMath::kSmallNumber)
    {
        m_AttConstant = 1.0f;
    }
}

// ============================================================================
// SetSpotAngles — 设置聚光灯内外锥角
// ============================================================================

void FLight::SetSpotAngles(Float32 innerDeg, Float32 outerDeg)
{
    // 钳位到合理范围 (0, 90) 度
    innerDeg = FMath::Clamp(innerDeg, 0.1f, 89.0f);
    outerDeg = FMath::Clamp(outerDeg, 0.1f, 89.0f);

    // 外锥角必须 >= 内锥角
    if (outerDeg < innerDeg)
    {
        outerDeg = innerDeg;
    }

    m_SpotInnerAngleDeg = innerDeg;
    m_SpotOuterAngleDeg = outerDeg;
}

} // namespace Limx
