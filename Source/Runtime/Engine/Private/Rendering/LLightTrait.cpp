// ============================================================
// 文件名称：LLightTrait.cpp
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：附加即注册 — OnAttached 自动向 FLightManager 添加光源，
//          OnDetached 自动移除，零额外配置，开发者只需关心参数。
// 功能描述：LLightTrait 完整实现 — 光源注册/注销/参数同步
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M1.0 Engine 渲染桥接) │
// ============================================================

#include "Engine/EngineMinimal.h"

namespace Limx
{

LIMX_DECLARE_LOG_CATEGORY(LogEngine)

IMPLEMENT_LTYPE(LLightTrait, LSpatialTrait)

// ============================================================================
// 构造
// ============================================================================

LLightTrait::LLightTrait()
    : m_LightType(ELightType::Directional)
    , m_Color(FLinearColor(1.0f, 1.0f, 1.0f, 1.0f))
    , m_Intensity(1.0f)
    , m_Range(10.0f)
    , m_LightIndex(kInvalidIndex)
{
}

// ============================================================================
// 参数设置（同步到 FLightManager）
// ============================================================================

void LLightTrait::SetLightType(ELightType type)
{
    m_LightType = type;
    if (IsRegistered())
    {
        SyncToLightManager();
    }
}

void LLightTrait::SetColor(const FLinearColor& color)
{
    m_Color = color;
    if (IsRegistered())
    {
        SyncToLightManager();
    }
}

void LLightTrait::SetIntensity(Float32 intensity)
{
    m_Intensity = intensity;
    if (IsRegistered())
    {
        SyncToLightManager();
    }
}

void LLightTrait::SetRange(Float32 range)
{
    m_Range = range;
    if (IsRegistered())
    {
        SyncToLightManager();
    }
}

// ============================================================================
// FLightManager 同步
// ============================================================================

void LLightTrait::SyncToLightManager() const
{
    if (!IsRegistered())
    {
        return;
    }

    FLight& light = FLightManager::Get().GetLight(m_LightIndex);
    light.SetColor(m_Color);
    light.SetIntensity(m_Intensity);

    // 同步位置/方向
    FVector3 worldPos = GetWorldLocation();
    FVector3 forward  = GetWorldTransform().Rotation.RotateVector(
        FVector3(0.0f, 0.0f, 1.0f));

    light.SetPosition(worldPos);
    light.SetDirection(forward);
}

// ============================================================================
// 生命周期
// ============================================================================

void LLightTrait::OnAttached(LNode* owner)
{
    LSpatialTrait::OnAttached(owner);

    // 根据类型创建对应 FLight 并注册到 FLightManager
    FLight newLight;
    switch (m_LightType)
    {
    case ELightType::Directional:
        newLight = FLight::CreateDirectional(
            FVector3(0.0f, -1.0f, 0.0f),
            m_Color,
            m_Intensity);
        break;
    case ELightType::Point:
        newLight = FLight::CreatePoint(
            GetWorldLocation(),
            m_Color,
            m_Intensity,
            m_Range);
        break;
    case ELightType::Spot:
        newLight = FLight::CreateSpot(
            GetWorldLocation(),
            FVector3(0.0f, -1.0f, 0.0f),
            m_Color,
            m_Intensity,
            m_Range,
            FMath::DegreesToRadians(30.0f),
            FMath::DegreesToRadians(45.0f));
        break;
    default:
        break;
    }

    m_LightIndex = FLightManager::Get().AddLight(MoveTemp(newLight));

    LIMX_LOG(LogEngine, Log,
             "[LLightTrait] 光源注册到 FLightManager (index={})", m_LightIndex);
}

void LLightTrait::OnDetached()
{
    if (IsRegistered())
    {
        FLightManager::Get().RemoveLight(m_LightIndex);
        m_LightIndex = kInvalidIndex;
        LIMX_LOG(LogEngine, Log, "[LLightTrait] 光源从 FLightManager 注销");
    }
    LSpatialTrait::OnDetached();
}

void LLightTrait::Tick(Float32 deltaTime)
{
    (void)deltaTime;
    // 每帧同步位置/方向（节点可能已移动）
    if (IsRegistered())
    {
        SyncToLightManager();
    }
}

} // namespace Limx
