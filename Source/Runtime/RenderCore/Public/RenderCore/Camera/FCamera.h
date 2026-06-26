// ============================================================
// 文件名称：FCamera.h
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：最小化相机抽象 — 仅封装视图/投影矩阵计算，
//          不持有 GPU 资源，纯数学状态对象。
// 功能描述：FCamera 基础相机类 — 管理位置、朝向(Yaw/Pitch)、
//          投影参数(FOV/近远裁剪面)，按需计算并缓存视图矩阵和
//          投影矩阵，供渲染器每帧查询。
//          M0.3 新增 WASD + 鼠标右键拖拽 FPS 风格相机控制。
// 技术特性：Euler 角驱动朝向 (Yaw + Pitch，无 Roll)；
//          脏标志延迟计算 (仅参数变化时重算矩阵)；
//          右手坐标系 + Vulkan NDC (Y翻转, Z [0,1])；
//          支持透视和正交两种投影模式。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                      │ 描述                           │
// │────────────────────────────│───────────────────────────────│
// │ GetViewMatrix()            │ 获取视图矩阵 (延迟计算)          │
// │ GetProjectionMatrix()      │ 获取投影矩阵 (延迟计算)          │
// │ SetPosition()              │ 设置相机世界坐标位置              │
// │ SetRotation()              │ 设置 Yaw/Pitch 旋转 (弧度)     │
// │ SetPerspective()           │ 设置透视投影参数                 │
// │ SetOrthographic()          │ 设置正交投影参数                 │
// │ SetAspectRatio()           │ 设置宽高比                      │
// │ GetForwardVector()         │ 获取相机前方方向向量              │
// │ GetRightVector()           │ 获取相机右方方向向量              │
// │ GetUpVector()              │ 获取相机上方方向向量              │
// │ ProcessInput()             │ WASD+鼠标右键 FPS 风格控制    │
//
// ── 结构体字段表 ──────────────────────────────────────────────
// │ 字段名                      │ 类型       │ 描述               │
// │────────────────────────────│──────────│───────────────────│
// │ m_Position                 │ FVector3  │ 世界坐标位置         │
// │ m_Yaw                     │ Float32   │ 偏航角 (弧度)        │
// │ m_Pitch                   │ Float32   │ 俯仰角 (弧度)        │
// │ m_FovY                    │ Float32   │ 垂直视场角 (弧度)     │
// │ m_AspectRatio              │ Float32   │ 宽高比              │
// │ m_NearPlane               │ Float32   │ 近裁剪面             │
// │ m_FarPlane                │ Float32   │ 远裁剪面             │
// │ m_IsOrthographic          │ bool      │ 正交投影模式标志      │
// │ m_OrthoWidth              │ Float32   │ 正交宽度             │
// │ m_OrthoHeight             │ Float32   │ 正交高度             │
// │ m_ViewMatrix              │ FMatrix   │ 缓存的视图矩阵       │
// │ m_ProjectionMatrix        │ FMatrix   │ 缓存的投影矩阵       │
// │ m_IsViewDirty             │ bool      │ 视图矩阵脏标志       │
// │ m_IsProjectionDirty       │ bool      │ 投影矩阵脏标志       │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建                        │
// │ 2026-04-07  │ LimxTeam  │ M0.3 WASD+鼠标右键相机控制     │
// ============================================================

#pragma once

#include "RenderCore/RenderCoreMinimal.h"

namespace Limx
{

// ============================================================================
// FCamera — 基础相机类
// ============================================================================

class FCamera
{
public:
    LIMX_NON_COPYABLE(FCamera);

    FCamera();
    ~FCamera() = default;

    // ====================================================================
    // 视图矩阵
    // ====================================================================

    /// 获取视图矩阵 (延迟计算，仅参数变化时重算)
    LIMX_NODISCARD const FMatrix& GetViewMatrix() const;

    /// 使用外部提供的 View/Projection 矩阵 (场景相机桥接路径)
    void SetExternalMatrices(const FMatrix& view,
                             const FMatrix& projection,
                             const FVector3& position);

    /// 清除外部矩阵覆盖，恢复内部位置/朝向/投影驱动
    void ClearExternalMatrices();

    /// 当前是否由外部 View/Projection 矩阵驱动
    LIMX_NODISCARD bool HasExternalMatrices() const { return m_UseExternalMatrices; }

    /// 设置相机世界坐标位置
    void SetPosition(const FVector3& position);

    /// 获取相机世界坐标位置
    LIMX_NODISCARD const FVector3& GetPosition() const { return m_Position; }

    /// 设置相机旋转 (Yaw = 偏航, Pitch = 俯仰, 弧度)
    void SetRotation(Float32 yaw, Float32 pitch);

    /// 获取偏航角 (弧度)
    LIMX_NODISCARD Float32 GetYaw() const { return m_Yaw; }

    /// 获取俯仰角 (弧度)
    LIMX_NODISCARD Float32 GetPitch() const { return m_Pitch; }

    /// 获取相机前方方向向量 (单位向量)
    LIMX_NODISCARD FVector3 GetForwardVector() const;

    /// 获取相机右方方向向量 (单位向量)
    LIMX_NODISCARD FVector3 GetRightVector() const;

    /// 获取相机上方方向向量 (单位向量)
    LIMX_NODISCARD FVector3 GetUpVector() const;

    // ====================================================================
    // 输入处理
    // ====================================================================

    /// WASD + 鼠标右键拖拽 FPS 风格相机控制
    /// @param deltaTime 帧间隔时间 (秒)
    void ProcessInput(Float32 deltaTime);

    /// 设置移动速度 (单位/秒)
    void SetMoveSpeed(Float32 speed) { m_MoveSpeed = speed; }

    /// 设置鼠标灵敏度
    void SetMouseSensitivity(Float32 sensitivity) { m_MouseSensitivity = sensitivity; }

    // ====================================================================
    // 投影矩阵
    // ====================================================================

    /// 获取投影矩阵 (延迟计算，仅参数变化时重算)
    LIMX_NODISCARD const FMatrix& GetProjectionMatrix() const;

    /// 设置透视投影参数
    /// @param fovY       垂直视场角 (弧度)
    /// @param aspectRatio 宽高比 (width / height)
    /// @param nearPlane   近裁剪面
    /// @param farPlane    远裁剪面
    void SetPerspective(Float32 fovY, Float32 aspectRatio,
                        Float32 nearPlane, Float32 farPlane);

    /// 设置正交投影参数
    /// @param width     正交宽度
    /// @param height    正交高度
    /// @param nearPlane 近裁剪面
    /// @param farPlane  远裁剪面
    void SetOrthographic(Float32 width, Float32 height,
                         Float32 nearPlane, Float32 farPlane);

    /// 仅更新宽高比 (窗口尺寸变化时调用)
    void SetAspectRatio(Float32 aspectRatio);

    /// 获取宽高比
    LIMX_NODISCARD Float32 GetAspectRatio() const { return m_AspectRatio; }

    /// 获取垂直视场角 (弧度)
    LIMX_NODISCARD Float32 GetFovY() const { return m_FovY; }

    /// 获取近裁剪面距离
    LIMX_NODISCARD Float32 GetNearPlane() const { return m_NearPlane; }

    /// 获取远裁剪面距离
    LIMX_NODISCARD Float32 GetFarPlane() const { return m_FarPlane; }

    /// 是否为正交投影模式
    LIMX_NODISCARD bool IsOrthographic() const { return m_IsOrthographic; }

private:
    /// 重新计算视图矩阵
    void RecalculateViewMatrix() const;

    /// 重新计算投影矩阵
    void RecalculateProjectionMatrix() const;

    // ---- 位置与朝向 ----
    FVector3 m_Position;
    Float32  m_Yaw   = 0.0f;
    Float32  m_Pitch = 0.0f;

    // ---- 投影参数 ----
    Float32  m_FovY        = FMath::DegreesToRadians(60.0f);
    Float32  m_AspectRatio  = 16.0f / 9.0f;
    Float32  m_NearPlane    = 0.1f;
    Float32  m_FarPlane     = 1000.0f;

    // ---- 正交投影参数 ----
    bool     m_IsOrthographic = false;
    Float32  m_OrthoWidth     = 10.0f;
    Float32  m_OrthoHeight    = 10.0f;

    // ---- 输入控制参数 ----
    Float32  m_MoveSpeed        = 2.0f;    // 移动速度 (单位/秒)
    Float32  m_MouseSensitivity = 0.003f;  // 鼠标灵敏度 (弧度/像素)

    // ---- 缓存矩阵 (mutable 支持 const 方法中延迟计算) ----
    mutable FMatrix m_ViewMatrix;
    mutable FMatrix m_ProjectionMatrix;
    mutable bool    m_IsViewDirty       = true;
    mutable bool    m_IsProjectionDirty = true;
    bool            m_UseExternalMatrices = false;
};

} // namespace Limx
