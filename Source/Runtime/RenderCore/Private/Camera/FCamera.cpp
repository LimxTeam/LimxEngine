// ============================================================
// 文件名称：FCamera.cpp
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：最小化相机抽象 — 纯数学状态对象，延迟计算矩阵。
// 功能描述：FCamera 完整实现 — 位置/朝向管理、方向向量计算、
//          透视/正交投影矩阵计算、脏标志延迟更新、
//          WASD+鼠标右键 FPS 风格相机控制。
// 技术特性：Euler 角 (Yaw/Pitch) 驱动方向向量；
//          FMatrix::LookAt 计算视图矩阵；
//          FMatrix::Perspective / FMatrix::Ortho 计算投影矩阵；
//          mutable 脏标志实现 const 方法中的延迟计算。
//
// ── 函数表 ──────────────────────────────────────────────────
// │ 函数名                        │ 描述                           │
// │──────────────────────────────│───────────────────────────────│
// │ FCamera()                    │ 默认构造 — 位于原点，面向 -Z     │
// │ GetViewMatrix()              │ 返回缓存的视图矩阵              │
// │ SetPosition()                │ 设置位置，标记视图脏             │
// │ SetRotation()                │ 设置 Yaw/Pitch，标记视图脏      │
// │ GetForwardVector()           │ 从 Yaw/Pitch 计算前方向量       │
// │ GetRightVector()             │ 前方向量叉乘世界上得右方向量      │
// │ GetUpVector()                │ 右方向量叉乘前方向量得上方向量    │
// │ GetProjectionMatrix()        │ 返回缓存的投影矩阵              │
// │ SetPerspective()             │ 设置透视参数，标记投影脏          │
// │ SetOrthographic()            │ 设置正交参数，标记投影脏          │
// │ SetAspectRatio()             │ 更新宽高比，标记投影脏            │
// │ RecalculateViewMatrix()      │ 用 LookAt 重算视图矩阵          │
// │ RecalculateProjectionMatrix()│ 用 Perspective/Ortho 重算投影   │
// │ ProcessInput()               │ WASD+鼠标右键 FPS 相机控制   │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建                        │
// │ 2026-04-07  │ LimxTeam  │ M0.3 WASD+鼠标右键相机控制     │
// ============================================================

#include "RenderCore/Camera/FCamera.h"
#include "ApplicationCore/Input/FInputManager.h"

namespace Limx
{

// ============================================================================
// 构造函数
// ============================================================================

FCamera::FCamera()
    : m_Position(FVector3(0.0f, 0.0f, 0.0f))
    , m_ViewMatrix(FMatrix::Identity())
    , m_ProjectionMatrix(FMatrix::Identity())
{
}

// ============================================================================
// 视图矩阵
// ============================================================================

const FMatrix& FCamera::GetViewMatrix() const
{
    if (m_UseExternalMatrices)
    {
        return m_ViewMatrix;
    }

    if (m_IsViewDirty)
    {
        RecalculateViewMatrix();
        m_IsViewDirty = false;
    }
    return m_ViewMatrix;
}

void FCamera::SetExternalMatrices(const FMatrix& view,
                                  const FMatrix& projection,
                                  const FVector3& position)
{
    m_ViewMatrix          = view;
    m_ProjectionMatrix    = projection;
    m_Position            = position;
    m_UseExternalMatrices = true;
    m_IsViewDirty         = false;
    m_IsProjectionDirty   = false;
}

void FCamera::ClearExternalMatrices()
{
    if (!m_UseExternalMatrices)
    {
        return;
    }

    m_UseExternalMatrices = false;
    m_IsViewDirty         = true;
    m_IsProjectionDirty   = true;
}

void FCamera::SetPosition(const FVector3& position)
{
    m_Position    = position;
    m_IsViewDirty = true;
}

void FCamera::SetRotation(Float32 yaw, Float32 pitch)
{
    m_Yaw   = yaw;

    // 限制俯仰角范围 [-89°, +89°]，防止万向节锁
    constexpr Float32 kMaxPitch = FMath::DegreesToRadians(89.0f);
    m_Pitch = FMath::Clamp(pitch, -kMaxPitch, kMaxPitch);

    m_IsViewDirty = true;
}

// ============================================================================
// 方向向量
// ============================================================================

FVector3 FCamera::GetForwardVector() const
{
    // 从 Yaw/Pitch 计算前方方向向量 (右手系, -Z 为默认前方)
    Float32 sinYaw, cosYaw;
    FMath::SinCos(m_Yaw, sinYaw, cosYaw);

    Float32 sinPitch, cosPitch;
    FMath::SinCos(m_Pitch, sinPitch, cosPitch);

    return FVector3(
        sinYaw * cosPitch,
        sinPitch,
        -cosYaw * cosPitch
    );
}

FVector3 FCamera::GetRightVector() const
{
    FVector3 forward = GetForwardVector();
    FVector3 worldUp(0.0f, 1.0f, 0.0f);
    return FVector3::Cross(forward, worldUp).GetSafeNormal();
}

FVector3 FCamera::GetUpVector() const
{
    FVector3 forward = GetForwardVector();
    FVector3 right   = GetRightVector();
    return FVector3::Cross(right, forward);
}

// ============================================================================
// 投影矩阵
// ============================================================================

const FMatrix& FCamera::GetProjectionMatrix() const
{
    if (m_UseExternalMatrices)
    {
        return m_ProjectionMatrix;
    }

    if (m_IsProjectionDirty)
    {
        RecalculateProjectionMatrix();
        m_IsProjectionDirty = false;
    }
    return m_ProjectionMatrix;
}

void FCamera::SetPerspective(Float32 fovY, Float32 aspectRatio,
                              Float32 nearPlane, Float32 farPlane)
{
    m_IsOrthographic    = false;
    m_FovY              = fovY;
    m_AspectRatio       = aspectRatio;
    m_NearPlane         = nearPlane;
    m_FarPlane          = farPlane;
    m_IsProjectionDirty = true;
}

void FCamera::SetOrthographic(Float32 width, Float32 height,
                               Float32 nearPlane, Float32 farPlane)
{
    m_IsOrthographic    = true;
    m_OrthoWidth        = width;
    m_OrthoHeight       = height;
    m_NearPlane         = nearPlane;
    m_FarPlane          = farPlane;
    m_IsProjectionDirty = true;
}

void FCamera::SetAspectRatio(Float32 aspectRatio)
{
    m_AspectRatio       = aspectRatio;
    m_IsProjectionDirty = true;
}

// ============================================================================
// ProcessInput — WASD + 鼠标右键拖拽 FPS 风格相机控制
// ============================================================================

void FCamera::ProcessInput(Float32 deltaTime)
{
    if (m_UseExternalMatrices)
    {
        return;
    }

    FInputManager& input = FInputManager::Get();

    // ---- 鼠标右键拖拽: 旋转相机朝向 ----
    if (input.IsMouseButtonDown(EMouseButton::Right))
    {
        Float32 deltaX = input.GetMouseDeltaX();
        Float32 deltaY = input.GetMouseDeltaY();

        Float32 newYaw   = m_Yaw   + deltaX * m_MouseSensitivity;
        Float32 newPitch = m_Pitch - deltaY * m_MouseSensitivity;

        SetRotation(newYaw, newPitch);
    }

    // ---- WASD 移动: 基于相机方向 ----
    FVector3 forward = GetForwardVector();
    FVector3 right   = GetRightVector();
    FVector3 movement(0.0f, 0.0f, 0.0f);

    // W = 0x57, A = 0x41, S = 0x53, D = 0x44 (Win32 Virtual-Key Codes)
    if (input.IsKeyDown(0x57)) { movement = movement + forward; }  // W 前进
    if (input.IsKeyDown(0x53)) { movement = movement - forward; }  // S 后退
    if (input.IsKeyDown(0x44)) { movement = movement + right; }    // D 右移
    if (input.IsKeyDown(0x41)) { movement = movement - right; }    // A 左移

    // Q/E 垂直移动 (Q = 0x51 下降, E = 0x45 上升)
    FVector3 worldUp(0.0f, 1.0f, 0.0f);
    if (input.IsKeyDown(0x45)) { movement = movement + worldUp; }  // E 上升
    if (input.IsKeyDown(0x51)) { movement = movement - worldUp; }  // Q 下降

    // 归一化移动方向并应用速度
    Float32 lengthSq = FVector3::Dot(movement, movement);
    if (lengthSq > 0.0001f)
    {
        Float32 invLength = 1.0f / FMath::Sqrt(lengthSq);
        movement = movement * (invLength * m_MoveSpeed * deltaTime);

        // Shift 加速 (0x10 = VK_SHIFT)
        if (input.IsKeyDown(0x10))
        {
            movement = movement * 3.0f;
        }

        SetPosition(m_Position + movement);
    }
}

// ============================================================================
// 内部矩阵重算
// ============================================================================

void FCamera::RecalculateViewMatrix() const
{
    FVector3 forward = GetForwardVector();
    FVector3 target  = m_Position + forward;
    FVector3 worldUp(0.0f, 1.0f, 0.0f);

    m_ViewMatrix = FMatrix::LookAt(m_Position, target, worldUp);
}

void FCamera::RecalculateProjectionMatrix() const
{
    if (m_IsOrthographic)
    {
        Float32 halfW = m_OrthoWidth  * 0.5f;
        Float32 halfH = m_OrthoHeight * 0.5f;
        m_ProjectionMatrix = FMatrix::Ortho(
            -halfW, halfW, -halfH, halfH,
            m_NearPlane, m_FarPlane);
    }
    else
    {
        m_ProjectionMatrix = FMatrix::Perspective(
            m_FovY, m_AspectRatio,
            m_NearPlane, m_FarPlane);
    }
}

} // namespace Limx
