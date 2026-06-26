// ============================================================
// 文件名称：LCameraTrait.h
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：相机即视角 — LCameraTrait 将节点变换转换为 View 矩阵，
//          通过 IsMain 标志让 FSceneManager 选择活跃相机，
//          每帧 BuildViewMatrix 提供 FRenderer 所需的 View 矩阵。
// 功能描述：LCameraTrait — 相机 Trait，持有 FOV/Near/Far 参数并与
//          FCamera 对接，通过 SetAsMain 声明为场景主相机。
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M1.0 Engine 渲染桥接) │
// ============================================================

#pragma once

#include "Engine/LSpatialTrait.h"
#include "RenderCore/Camera/FCamera.h"

namespace Limx
{

// ============================================================================
// LCameraTrait — 相机 Trait
// ============================================================================

class LIMX_ENGINE_API LCameraTrait : public LSpatialTrait
{
    LOBJECT_BODY(LCameraTrait)

public:
    LCameraTrait();
    ~LCameraTrait() override = default;

    // ====================================================================
    // 相机参数
    // ====================================================================

    void SetFOV(Float32 fovRadians);
    LIMX_NODISCARD Float32 GetFOV() const { return m_FOV; }

    void SetNearFar(Float32 nearPlane, Float32 farPlane);
    LIMX_NODISCARD Float32 GetNearPlane() const { return m_Near; }
    LIMX_NODISCARD Float32 GetFarPlane()  const { return m_Far; }

    void SetAspectRatio(Float32 aspect);
    LIMX_NODISCARD Float32 GetAspectRatio() const { return m_AspectRatio; }

    // ====================================================================
    // 主相机控制
    // ====================================================================

    void SetAsMain(bool isMain) { m_IsMain = isMain; }
    LIMX_NODISCARD bool IsMain() const { return m_IsMain; }

    // ====================================================================
    // 矩阵生成
    // ====================================================================

    /// 从当前世界变换计算 View 矩阵（列主序，RH 坐标系）
    LIMX_NODISCARD FMatrix BuildViewMatrix() const;

    /// 从 FOV/Near/Far/Aspect 计算 Projection 矩阵
    LIMX_NODISCARD FMatrix BuildProjectionMatrix() const;

    /// 获取相机世界位置
    LIMX_NODISCARD FVector3 GetCameraWorldPosition() const;

    // ====================================================================
    // 生命周期覆盖
    // ====================================================================

    void OnAttached(LNode* owner)  override;

private:
    Float32 m_FOV         = FMath::DegreesToRadians(45.0f);
    Float32 m_Near        = 0.1f;
    Float32 m_Far         = 1000.0f;
    Float32 m_AspectRatio = 16.0f / 9.0f;
    bool    m_IsMain      = false;
};

} // namespace Limx
