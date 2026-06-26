// ============================================================
// 文件名称：LLightTrait.h
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：光源即 Trait — LLightTrait 将光源参数与空间位置绑定，
//          Attach 时自动注册到 FLightManager，Detach 时自动注销，
//          位置变化时实时同步到光源数据。
// 功能描述：LLightTrait — 光源 Trait，包装 FLight 并驱动 FLightManager，
//          支持方向光/点光/聚光灯三种类型。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                   │ 描述                           │
// │────────────────────────│───────────────────────────────│
// │ SetLightType(type)      │ 设置光源类型                   │
// │ SetColor(color)         │ 设置光源颜色                   │
// │ SetIntensity(val)       │ 设置光照强度                   │
// │ SetRange(val)           │ 设置衰减距离 (点光/聚光灯)     │
// │ GetLightIndex()         │ 返回 FLightManager 中的索引   │
// │ IsRegistered()          │ 是否已注册到 FLightManager    │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M1.0 Engine 渲染桥接) │
// ============================================================

#pragma once

#include "Engine/LSpatialTrait.h"
#include "RenderCore/Lighting/FLight.h"
#include "RenderCore/Lighting/FLightManager.h"

namespace Limx
{

// ============================================================================
// LLightTrait — 光源 Trait
// ============================================================================

class LIMX_ENGINE_API LLightTrait : public LSpatialTrait
{
    LOBJECT_BODY(LLightTrait)

public:
    LLightTrait();
    ~LLightTrait() override = default;

    // ====================================================================
    // 光源参数
    // ====================================================================

    void SetLightType(ELightType type);
    LIMX_NODISCARD ELightType GetLightType() const { return m_LightType; }

    void SetColor(const FLinearColor& color);
    LIMX_NODISCARD const FLinearColor& GetColor() const { return m_Color; }

    void SetIntensity(Float32 intensity);
    LIMX_NODISCARD Float32 GetIntensity() const { return m_Intensity; }

    void SetRange(Float32 range);
    LIMX_NODISCARD Float32 GetRange() const { return m_Range; }

    // ====================================================================
    // FLightManager 状态
    // ====================================================================

    LIMX_NODISCARD UInt32 GetLightIndex() const { return m_LightIndex; }
    LIMX_NODISCARD bool   IsRegistered()  const { return m_LightIndex != kInvalidIndex; }

    // ====================================================================
    // 生命周期覆盖
    // ====================================================================

    void OnAttached(LNode* owner)  override;
    void OnDetached()              override;

    /// 每帧同步位置/方向到 FLightManager
    void Tick(Float32 deltaTime)   override;

private:
    /// 将当前参数同步到 FLightManager 中的 FLight
    void SyncToLightManager() const;

    static constexpr UInt32 kInvalidIndex = 0xFFFFFFFF;

    ELightType   m_LightType = ELightType::Directional;
    FLinearColor m_Color     = FLinearColor(1.0f, 1.0f, 1.0f, 1.0f);
    Float32      m_Intensity = 1.0f;
    Float32      m_Range     = 10.0f;
    UInt32       m_LightIndex = kInvalidIndex;
};

} // namespace Limx
