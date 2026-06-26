// ============================================================
// 文件名称：LCameraTrait.cpp
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：每帧重算 — BuildViewMatrix 和 BuildProjectionMatrix 每帧
//          从世界变换实时计算，无持久缓存，简单可靠。
// 功能描述：LCameraTrait 完整实现 — View/Projection 矩阵生成
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

IMPLEMENT_LTYPE(LCameraTrait, LSpatialTrait)

// ============================================================================
// 构造
// ============================================================================

LCameraTrait::LCameraTrait()
    : m_FOV(FMath::DegreesToRadians(45.0f))
    , m_Near(0.1f)
    , m_Far(1000.0f)
    , m_AspectRatio(16.0f / 9.0f)
    , m_IsMain(false)
{
}

// ============================================================================
// 参数设置
// ============================================================================

void LCameraTrait::SetFOV(Float32 fovRadians)
{
    m_FOV = FMath::Clamp(fovRadians,
                          FMath::DegreesToRadians(1.0f),
                          FMath::DegreesToRadians(170.0f));
}

void LCameraTrait::SetNearFar(Float32 nearPlane, Float32 farPlane)
{
    LIMX_CHECK(nearPlane > 0.0f && farPlane > nearPlane);
    m_Near = nearPlane;
    m_Far  = farPlane;
}

void LCameraTrait::SetAspectRatio(Float32 aspect)
{
    m_AspectRatio = FMath::Max(aspect, 0.001f);
}

// ============================================================================
// 矩阵生成
// ============================================================================

FMatrix LCameraTrait::BuildViewMatrix() const
{
    FTransform worldTransform = GetWorldTransform();
    FVector3   position  = worldTransform.Translation;
    FQuat      rotation  = worldTransform.Rotation;

    // 相机前方向量（世界空间 +Z 方向经旋转）
    FVector3 forward = rotation.RotateVector(FVector3(0.0f, 0.0f, 1.0f));
    FVector3 up      = rotation.RotateVector(FVector3(0.0f, 1.0f, 0.0f));
    FVector3 target  = position + forward;

    return FMatrix::LookAt(position, target, up);
}

FMatrix LCameraTrait::BuildProjectionMatrix() const
{
    return FMatrix::Perspective(m_FOV, m_AspectRatio, m_Near, m_Far);
}

FVector3 LCameraTrait::GetCameraWorldPosition() const
{
    return GetWorldTransform().Translation;
}

// ============================================================================
// 生命周期
// ============================================================================

void LCameraTrait::OnAttached(LNode* owner)
{
    LSpatialTrait::OnAttached(owner);
    LIMX_LOG(LogEngine, Log,
             "[LCameraTrait] 相机附加到节点 '{}' (isMain={})",
             owner ? owner->GetName().GetCStr() : FString("null"),
             m_IsMain ? 1 : 0);
}

} // namespace Limx
