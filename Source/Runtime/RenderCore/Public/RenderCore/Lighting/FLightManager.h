// ============================================================
// 文件名称：FLightManager.h
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：单例集中管理 — 所有光源通过 FLightManager 统一创建、
//          存储、销毁；每帧将活跃光源打包为 FLightingUBO 上传
//          GPU，消费者通过描述符集绑定即可获取光照数据。
//          光源管理与渲染管线解耦，上层只需调用 UploadLightData。
// 功能描述：光源管理器 — 单例模式，管理场景中所有光源的生命周期，
//          创建 GPU 端光照 UBO 缓冲区 (每帧一个)，每帧将启用的
//          光源打包为 FLightingUBO 上传，对外暴露缓冲区句柄和
//          描述符集布局供管线集成。
// 技术特性：每并行帧一个 UBO 避免写入冲突；光源数组栈分配无堆开销；
//          描述符集布局 (set 2, binding 0) 对外暴露用于管线布局构建；
//          UploadLightData 只在光源数据变化时执行完整打包 (脏标记)。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                        │ 描述                          │
// │──────────────────────────────│──────────────────────────────│
// │ Get()                        │ 获取单例引用                   │
// │ Initialize()                 │ 创建 GPU 资源 (UBO+描述符集布局) │
// │ Shutdown()                   │ 释放 GPU 资源                  │
// │ AddLight()                   │ 添加光源 (移动语义)              │
// │ RemoveLight()                │ 按索引移除光源                  │
// │ GetLight()                   │ 按索引获取光源引用               │
// │ GetLightCount()              │ 获取光源数量                    │
// │ ClearAllLights()             │ 移除所有光源                    │
// │ UploadLightData()            │ 打包并上传光照数据到 GPU          │
// │ GetLightUBO()                │ 获取指定帧的 UBO 缓冲区句柄      │
// │ GetDescSetLayout()           │ 获取描述符集布局 (set 2)         │
// │ IsInitialized()              │ 是否已初始化                    │
//
// ── 结构体字段表 ──────────────────────────────────────────────
// │ 字段名                        │ 类型                        │ 描述          │
// │──────────────────────────────│───────────────────────────│──────────────│
// │ m_Lights                     │ TArray<FLight>             │ 光源集合       │
// │ m_LightUBOs                  │ TArray<FRHIBufferHandle>   │ UBO 句柄数组   │
// │ m_DescSetLayout              │ FRHIDescSetLayoutHandle    │ 描述符集布局    │
// │ m_Device                     │ IRHIDevice*                │ RHI 设备指针   │
// │ m_MaxFramesInFlight          │ UInt32                     │ 最大并行帧数   │
// │ m_IsInitialized              │ bool                       │ 初始化标志     │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                          │
// │─────────────│──────────│──────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M0.5 光照系统)       │
// ============================================================

#pragma once

#include "RenderCore/Lighting/FLight.h"

namespace Limx
{

// 前向声明
class IRHIDevice;

// ============================================================================
// FLightManager — 光源管理器 (单例)
// ============================================================================

class FLightManager
{
public:
    LIMX_NON_COPYABLE(FLightManager);
    LIMX_NON_MOVABLE(FLightManager);

    // ====================================================================
    // 单例访问
    // ====================================================================

    /// 获取单例引用
    static FLightManager& Get();

    // ====================================================================
    // 生命周期
    // ====================================================================

    /// 初始化光源管理器 — 创建描述符集布局 + UBO 缓冲区
    /// @param device             RHI 设备 (非拥有)
    /// @param maxFramesInFlight  并行帧数 (通常 2 或 3)
    /// @return Success 或错误码
    ERHIResult Initialize(IRHIDevice* device, UInt32 maxFramesInFlight);

    /// 释放所有 GPU 资源
    void Shutdown();

    /// 是否已初始化
    LIMX_NODISCARD bool IsInitialized() const { return m_IsInitialized; }

    // ====================================================================
    // 光源管理
    // ====================================================================

    /// 添加光源 (移动语义，返回光源在数组中的索引)
    /// 超过 kMaxLightCount 时忽略并返回 UINT32_MAX
    UInt32 AddLight(FLight&& light);

    /// 按索引移除光源 (交换删除，不保持顺序)
    void RemoveLight(UInt32 index);

    /// 按索引获取光源引用 (可修改)
    LIMX_NODISCARD FLight& GetLight(UInt32 index);

    /// 按索引获取光源引用 (只读)
    LIMX_NODISCARD const FLight& GetLight(UInt32 index) const;

    /// 获取当前光源数量
    LIMX_NODISCARD UInt32 GetLightCount() const;

    /// 移除所有光源
    void ClearAllLights();

    // ====================================================================
    // GPU 数据上传
    // ====================================================================

    /// 将所有启用的光源打包为 FLightingUBO 并上传到指定帧的 UBO
    /// @param frameIndex       当前帧索引
    /// @param cameraPosition   相机世界空间位置 (用于镜面反射计算)
    void UploadLightData(UInt32 frameIndex, const FVector3& cameraPosition);

    // ====================================================================
    // 阴影
    // ====================================================================

    /// 设置主方向光的阴影矩阵与参数
    ///
    /// 由渲染器在阴影 Pass 计算出光源视锥后调用。矩阵随光照 UBO 一同上传,
    /// 因此片段着色器拿到的矩阵与本帧实际绘制阴影贴图用的矩阵必然一致 ——
    /// 两者若来自不同时刻, 阴影会整体偏移一帧, 表现为快速移动时阴影"拖尾"。
    ///
    /// @param info 级联矩阵、切分距离与偏移参数
    void SetShadowInfo(const FCascadedShadowInfo& info);

    /// 关闭阴影 — 片段着色器将全部按无遮挡处理
    void DisableShadow() { m_IsShadowEnabled = false; }

    LIMX_NODISCARD bool IsShadowEnabled() const { return m_IsShadowEnabled; }

    // ====================================================================
    // 基于图像的光照 (IBL)
    // ====================================================================

    /// 启用 IBL 并设置强度倍数
    ///
    /// 启用后片段着色器的环境项改用辐照度贴图, 常数环境光不再参与 ——
    /// 两者相加会让环境光被计两次, 表现为开启 IBL 后场景整体偏亮偏灰。
    void EnableIbl(Float32 intensity)
    {
        m_IsIblEnabled = true;
        m_IblIntensity = intensity;
    }

    /// 关闭 IBL — 环境项退回常数环境光
    void DisableIbl() { m_IsIblEnabled = false; }

    LIMX_NODISCARD bool IsIblEnabled() const { return m_IsIblEnabled; }

    // ====================================================================
    // GPU 资源访问器 (供集成者使用)
    // ====================================================================

    /// 获取指定帧的光照 UBO 缓冲区句柄
    LIMX_NODISCARD FRHIBufferHandle GetLightUBO(UInt32 frameIndex) const;

    /// 获取描述符集布局 (set 2) — 用于构建管线布局
    LIMX_NODISCARD FRHIDescSetLayoutHandle GetDescSetLayout() const
    {
        return m_DescSetLayout;
    }

private:
    FLightManager();
    ~FLightManager();

    // ====================================================================
    // 成员
    // ====================================================================

    /// 光源集合
    TArray<FLight>              m_Lights;

    /// 每帧一个 UBO 缓冲区 (避免写入冲突)
    TArray<FRHIBufferHandle>    m_LightUBOs;

    /// 描述符集布局 (set 2, binding 0: UniformBuffer, Vertex+Fragment)

    FRHIDescSetLayoutHandle     m_DescSetLayout;
    
    /// 主方向光的级联阴影数据 —— 随光照 UBO 一同上传
    FCascadedShadowInfo m_ShadowInfo;
    bool                m_IsShadowEnabled = false;

    /// IBL 开关与强度 —— 随光照 UBO 一同上传
    bool                m_IsIblEnabled = false;
    Float32             m_IblIntensity = 1.0f;

    /// RHI 设备 (非拥有指针)
    IRHIDevice*                 m_Device = nullptr;

    /// 最大并行帧数
    UInt32                      m_MaxFramesInFlight = 0;

    /// 初始化标志
    bool                        m_IsInitialized = false;
};

} // namespace Limx
