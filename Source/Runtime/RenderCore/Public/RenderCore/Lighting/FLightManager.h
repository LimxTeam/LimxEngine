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
#include "RenderCore/Lighting/FShadowAtlas.h"

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
    void EnableIbl(Float32 intensity, Float32 prefilteredMaxLod)
    {
        m_IsIblEnabled        = true;
        m_IblIntensity        = intensity;
        m_IblPrefilteredMaxLod = prefilteredMaxLod;
    }

    /// 关闭 IBL — 环境项退回常数环境光
    void DisableIbl() { m_IsIblEnabled = false; }

    LIMX_NODISCARD bool IsIblEnabled() const { return m_IsIblEnabled; }

    LIMX_NODISCARD Float32 GetIblPrefilteredMaxLod() const
    {
        return m_IblPrefilteredMaxLod;
    }

    // ====================================================================
    // GPU 资源访问器 (供集成者使用)
    // ====================================================================

    /// 获取指定帧的光照 UBO 缓冲区句柄
    LIMX_NODISCARD FRHIBufferHandle GetLightUBO(UInt32 frameIndex) const;

    /// 获取指定帧的光源 storage buffer 句柄 (set 2, binding 5)
    ///
    /// 每并行帧一份。共用一份的话, 帧 N+1 的上传会覆写帧 N 的片段着色器
    /// 正在读的数据 —— 那是只在高帧率下偶发的画面撕裂, 验证层不报。
    LIMX_NODISCARD FRHIBufferHandle GetLightStorageBuffer(
        UInt32 frameIndex) const;

    /// 上一次上传的活跃光源数 —— 分簇剔除要用它决定分派规模
    LIMX_NODISCARD UInt32 GetActiveLightCount() const
    {
        return m_ActiveLightCount;
    }

    /// 获取指定帧的聚光灯阴影 storage buffer 句柄 (set 2, binding 9)
    LIMX_NODISCARD FRHIBufferHandle GetSpotShadowBuffer(
        UInt32 frameIndex) const;

    /// 本帧分配出去的阴影块 —— 阴影图集 Pass 照着它逐块绘制
    ///
    /// 由 UploadLightData 填写。分块与矩阵在那里算一次, 图集绘制与片段
    /// 着色器采样用的因此是**同一份**数据 —— 各算一遍的话, 快速转动的
    /// 聚光灯会出现"影子跟不上灯"的一帧延迟, 而那看着像是阴影偏移没调好。
    LIMX_NODISCARD const TArray<FSpotShadowData>& GetSpotShadowCasters() const
    {
        return m_SpotShadowCasters;
    }

    /// 设置分簇光照的每帧参数
    ///
    /// 由 FRenderer 在 UploadLightData 之前调用。近远平面必须与
    /// FClusterLightPass 用的**同一对** —— 两处不一致的表现是片段着色器
    /// 算出的切片下标与计算着色器写表时用的不是同一套, 于是每个像素都去
    /// 查错误的簇, 而画面依然"有光"。
    void SetClusterParams(bool enabled, Float32 nearPlane, Float32 farPlane,
                          Float32 screenWidth, Float32 screenHeight)
    {
        m_IsClusteredEnabled  = enabled;
        m_ClusterNearPlane    = nearPlane;
        m_ClusterFarPlane     = farPlane;
        m_ClusterScreenWidth  = screenWidth;
        m_ClusterScreenHeight = screenHeight;
    }

    LIMX_NODISCARD bool IsClusteredEnabled() const
    {
        return m_IsClusteredEnabled;
    }

    /// 获取描述符集布局 (set 2) — 用于构建管线布局
    /// 指定哪一盏灯的阴影走光线追踪 (-1 = 都不走)
    /// 光追产出的开关 —— 第 0 位 = AO 生效, 第 1 位 = 反射生效
    void SetRayTracedFlags(Float32 flags) { m_RayTracedFlags = flags; }

    LIMX_NODISCARD Float32 GetRayTracedFlags() const
    {
        return m_RayTracedFlags;
    }

    void SetRayTracedShadowLight(Int32 lightIndex)
    {
        m_RayTracedShadowLight = lightIndex;
    }

    LIMX_NODISCARD Int32 GetRayTracedShadowLight() const
    {
        return m_RayTracedShadowLight;
    }

    LIMX_NODISCARD FRHIDescSetLayoutHandle GetDescSetLayout() const
    {
        return m_DescSetLayout;
    }

private:
    FLightManager();
    ~FLightManager();

    /// 把本帧分配出去的阴影块写进指定帧的 storage buffer
    void UploadSpotShadowData(UInt32 frameIndex);

    // ====================================================================
    // 成员
    // ====================================================================

    /// 光源集合
    TArray<FLight>              m_Lights;

    /// 每帧一个 UBO 缓冲区 (避免写入冲突)
    TArray<FRHIBufferHandle>    m_LightUBOs;

    /// 每并行帧一个光源 storage buffer (kMaxLightCount × 80 字节)
    TArray<FRHIBufferHandle>    m_LightStorageBuffers;

    /// 每并行帧一个聚光灯阴影 storage buffer (kShadowTileCount × 96 字节)
    TArray<FRHIBufferHandle>    m_SpotShadowBuffers;

    /// 本帧分配出去的阴影块 (下标即块下标)
    TArray<FSpotShadowData>     m_SpotShadowCasters;

    /// 上一次报过"图集不够用"时的请求数 —— 只在数目变化时才再报一次
    ///
    /// 初值取一个不可能的数, 保证第一帧一定会走一次判断。用 0 的话, 场景
    /// 一开始就没有投影灯时会被当成"和上次一样"而跳过 —— 那一次跳过没有
    /// 后果, 但这类"初值恰好等于合法值"的写法迟早会有。
    UInt32                      m_LastShadowRequestCount = 0xFFFFFFFFu;

    /// 上一次 UploadLightData 写进去的活跃光源数
    UInt32                      m_ActiveLightCount = 0;

    // ---- 分簇光照的每帧参数 ----
    bool                        m_IsClusteredEnabled  = false;
    Float32                     m_ClusterNearPlane    = 0.1f;
    Float32                     m_ClusterFarPlane     = 100.0f;
    Float32                     m_ClusterScreenWidth  = 1.0f;
    Float32                     m_ClusterScreenHeight = 1.0f;

    /// 描述符集布局 (set 2, binding 0: UniformBuffer, Vertex+Fragment)

    /// 哪一盏灯走光追阴影 (-1 = 都不走)
    Int32 m_RayTracedShadowLight = -1;

    /// 光追产出的开关 (位域, 见 SetRayTracedFlags)
    Float32 m_RayTracedFlags = 0.0f;

    FRHIDescSetLayoutHandle     m_DescSetLayout;
    
    /// 主方向光的级联阴影数据 —— 随光照 UBO 一同上传
    FCascadedShadowInfo m_ShadowInfo;
    bool                m_IsShadowEnabled = false;

    /// IBL 开关、强度与预滤波 LOD 上限 —— 随光照 UBO 一同上传
    bool                m_IsIblEnabled         = false;
    Float32             m_IblIntensity         = 1.0f;
    Float32             m_IblPrefilteredMaxLod = 0.0f;

    /// RHI 设备 (非拥有指针)
    IRHIDevice*                 m_Device = nullptr;

    /// 最大并行帧数
    UInt32                      m_MaxFramesInFlight = 0;

    /// 初始化标志
    bool                        m_IsInitialized = false;
};

} // namespace Limx
