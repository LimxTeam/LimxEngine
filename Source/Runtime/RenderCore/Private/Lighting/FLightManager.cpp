// ============================================================
// 文件名称：FLightManager.cpp
// 创建时间：2026-04-07
// 创建者  ：LimxTeam
// 设计哲学：单例集中管理 — 光源的 GPU 资源生命周期完全由
//          FLightManager 控制，外部只需添加/移除光源并在每帧
//          调用 UploadLightData 即可完成光照数据上传。
// 功能描述：FLightManager 完整实现 — 单例构造/析构、GPU 资源
//          创建 (UBO+描述符集布局)、光源增删查、每帧光照数据
//          打包上传、资源释放。
// 技术特性：每并行帧独立 UBO (CpuToGpu, 1328 字节)；
//          描述符集布局 set 2, binding 0 (UniformBuffer, Vertex+Fragment)；
//          UploadLightData 每帧打包所有启用光源到 FLightingUBO，
//          通过 MapBuffer 直接写入 GPU 可见内存。
//
// ── 函数表 ──────────────────────────────────────────────────
// │ 函数名                        │ 描述                          │
// │──────────────────────────────│──────────────────────────────│
// │ Get()                        │ 返回静态局部单例引用            │
// │ FLightManager()              │ 私有默认构造                   │
// │ ~FLightManager()             │ 析构 (调用 Shutdown)           │
// │ Initialize()                 │ 创建描述符集布局 + UBO          │
// │ Shutdown()                   │ 释放 UBO + 描述符集布局         │
// │ AddLight()                   │ 添加光源 (移动入数组)            │
// │ RemoveLight()                │ 交换删除光源                    │
// │ GetLight()                   │ 索引访问 (可变/只读)            │
// │ GetLightCount()              │ 返回数组大小                    │
// │ ClearAllLights()             │ 清空光源数组                    │
// │ UploadLightData()            │ 打包 + MapBuffer + MemCopy     │
// │ GetLightUBO()                │ 按帧索引返回 UBO 句柄           │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                          │
// │─────────────│──────────│──────────────────────────────│
// │ 2026-04-07  │ LimxTeam  │ 初始创建 (M0.5 光照系统)       │
// ============================================================

#include "RenderCore/Lighting/FLightManager.h"
#include "RenderCore/Lighting/FClusterGrid.h"

namespace Limx
{

// 引入内存操作函数
using Memory::MemCopy;
using Memory::MemZero;

// 日志分类
LIMX_DECLARE_LOG_CATEGORY(LogLighting)
LIMX_DEFINE_LOG_CATEGORY(LogLighting)

// ============================================================================
// 单例
// ============================================================================

FLightManager& FLightManager::Get()
{
    static FLightManager instance;
    return instance;
}

// ============================================================================
// 构造/析构
// ============================================================================

FLightManager::FLightManager()
    : m_Device(nullptr)
    , m_MaxFramesInFlight(0)
    , m_IsInitialized(false)
{
}

FLightManager::~FLightManager()
{
    Shutdown();
}

// ============================================================================
// Initialize — 创建描述符集布局 + UBO 缓冲区
// ============================================================================

ERHIResult FLightManager::Initialize(IRHIDevice* device, UInt32 maxFramesInFlight)
{
    LIMX_CHECK(device != nullptr);
    LIMX_CHECK(maxFramesInFlight > 0);

    if (m_IsInitialized)
    {
        LIMX_LOG(LogLighting, Warning,
                 "[LightManager] 重复初始化，跳过");
        return ERHIResult::Success;
    }

    m_Device             = device;
    m_MaxFramesInFlight  = maxFramesInFlight;

    // ---- 创建描述符集布局 ----
    //   set 2, binding 0 — FLightingUBO
    //   set 2, binding 1 — 阴影贴图 (深度纹理 + 比较采样器)
    //   set 2, binding 2 — 漫反射辐照度立方体贴图
    //   set 2, binding 3 — 镜面预滤波立方体贴图
    //   set 2, binding 4 — 环境 BRDF 查找表
    //
    // 阴影贴图放在 set 2 而非 set 0: set 0 的描述符集也会被阴影 Pass 自己
    // 绑定 (它需要光源矩阵), 把正在写入的阴影贴图放进去会形成"同一帧内
    // 既作为附件写入又作为纹理读取"的冲突。set 2 只有前向 Pass 绑定,
    // 天然避开这个问题。
    //   set 2, binding 5 — 光源数组 storage buffer
    //
    // 光源从 UBO 挪到 SSBO 之后, binding 0 里只剩全局参数。StageFlags 必须
    // 含 Compute: 分簇剔除的计算着色器读的是同一份数据。
    //   set 2, binding 6 — 每簇的 (起点, 数量)
    //   set 2, binding 7 — 全局光源索引表
    //   set 2, binding 8 — 屏幕空间环境光遮蔽
    //   set 2, binding 9 — 聚光灯阴影的每块数据 (矩阵 + UV 变换)
    //   set 2, binding 10 — 聚光灯阴影图集 (深度纹理 + 比较采样器)
    //   set 2, binding 11 — 光追阴影可见度掩码 (全分辨率 R8)
    //
    // 与阴影图集并存而不是二选一: 光追阴影一次只处理一盏灯, 其余的仍然
    // 走图集。着色器按 UBO 里的开关决定这一盏用哪一个。
    FRHIDescriptorBinding bindings[14] = {};

    bindings[0].Binding    = 0;
    bindings[0].Type       = EDescriptorType::UniformBuffer;
    bindings[0].Count      = 1;
    bindings[0].StageFlags = EShaderStage::Vertex | EShaderStage::Fragment;

    bindings[1].Binding    = 1;
    bindings[1].Type       = EDescriptorType::CombinedImageSampler;
    bindings[1].Count      = 1;
    bindings[1].StageFlags = EShaderStage::Fragment;

    // binding 2 — 漫反射辐照度立方体贴图
    //
    // 即使场景没有环境贴图, 这个绑定也必须写入一个有效的视图:
    // 着色器里出现的描述符必须在管线绑定时有效, 靠 uniform 分支跳过采样
    // 并不能免除这一点。因此渲染器会准备一张 1x1 的黑色立方体贴图兜底。
    bindings[2].Binding    = 2;
    bindings[2].Type       = EDescriptorType::CombinedImageSampler;
    bindings[2].Count      = 1;
    bindings[2].StageFlags = EShaderStage::Fragment;

    bindings[3].Binding    = 3;
    bindings[3].Type       = EDescriptorType::CombinedImageSampler;
    bindings[3].Count      = 1;
    bindings[3].StageFlags = EShaderStage::Fragment;

    bindings[4].Binding    = 4;
    bindings[4].Type       = EDescriptorType::CombinedImageSampler;
    bindings[4].Count      = 1;
    bindings[4].StageFlags = EShaderStage::Fragment;

    bindings[5].Binding    = 5;
    bindings[5].Type       = EDescriptorType::StorageBuffer;
    bindings[5].Count      = 1;
    bindings[5].StageFlags = EShaderStage::Fragment;

    // 分簇的产出。计算通道用的是它自己的 set 0, 不经过这里 —— 这两个
    // binding 只给片段着色器读。
    bindings[6].Binding    = 6;
    bindings[6].Type       = EDescriptorType::StorageBuffer;
    bindings[6].Count      = 1;
    bindings[6].StageFlags = EShaderStage::Fragment;

    bindings[7].Binding    = 7;
    bindings[7].Type       = EDescriptorType::StorageBuffer;
    bindings[7].Count      = 1;
    bindings[7].StageFlags = EShaderStage::Fragment;

    bindings[8].Binding    = 8;
    bindings[8].Type       = EDescriptorType::CombinedImageSampler;
    bindings[8].Count      = 1;
    bindings[8].StageFlags = EShaderStage::Fragment;

    // binding 9/10 — 聚光灯阴影
    //
    // 阴影图集不放在 binding 1 那张级联贴图旁边另起一个数组层, 而是单独
    // 一张纹理: 两者的分辨率、投影类型与采样方式都不同 (级联是正交 +
    // 数组层, 图集是透视 + 块偏移), 硬塞进一个 sampler2DArrayShadow 之后
    // 每次采样都要先判断"这一层是级联还是图集"。
    bindings[9].Binding    = 9;
    bindings[9].Type       = EDescriptorType::StorageBuffer;
    bindings[9].Count      = 1;
    bindings[9].StageFlags = EShaderStage::Fragment;

    bindings[10].Binding    = 10;
    bindings[10].Type       = EDescriptorType::CombinedImageSampler;
    bindings[10].Count      = 1;
    bindings[10].StageFlags = EShaderStage::Fragment;

    FRHIDescSetLayoutDesc layoutDesc = {};
    layoutDesc.Bindings     = bindings;
    bindings[11].Binding    = 11;
    bindings[11].Type       = EDescriptorType::CombinedImageSampler;
    bindings[11].Count      = 1;
    bindings[11].StageFlags = EShaderStage::Fragment;

    // binding 12/13 — 光追 AO 与光追反射
    //
    // 与阴影掩码同理: 布局里无条件声明, 生效与否由 UBO 的标志位决定。
    // 按"支不支持光追"去改布局的话, 同一个引擎在两台机器上会有两套不同的
    // 描述符集布局 —— 而管线布局是按对象判兼容的, 那意味着两套管线。
    bindings[12] = bindings[11];
    bindings[12].Binding = 12;

    bindings[13] = bindings[11];
    bindings[13].Binding = 13;

    layoutDesc.BindingCount = 14;
    layoutDesc.DebugName    = "LightingDescSetLayout_Set2";

    ERHIResult result = m_Device->CreateDescSetLayout(
        layoutDesc, m_DescSetLayout);
    if (!IsRHISuccess(result))
    {
        LIMX_LOG(LogLighting, Error,
                 "[LightManager] 描述符集布局创建失败");
        return result;
    }

    // ---- 为每并行帧创建一个 UBO 缓冲区 ----
    m_LightUBOs.Reserve(maxFramesInFlight);

    for (UInt32 i = 0; i < maxFramesInFlight; ++i)
    {
        FRHIBufferDesc bufferDesc =
            FRHIBufferDesc::Uniform(sizeof(FLightingUBO));
        bufferDesc.DebugName = "LightingUBO";

        FRHIBufferHandle buffer;
        result = m_Device->CreateBuffer(bufferDesc, buffer);
        if (!IsRHISuccess(result))
        {
            LIMX_LOG(LogLighting, Error,
                     "[LightManager] UBO 缓冲区 [{}] 创建失败", i);
            Shutdown();
            return result;
        }

        m_LightUBOs.Add(buffer);
    }

    // ---- 为每并行帧创建一个光源 storage buffer ----
    m_LightStorageBuffers.Reserve(maxFramesInFlight);

    for (UInt32 i = 0; i < maxFramesInFlight; ++i)
    {
        FRHIBufferDesc bufferDesc = {};
        bufferDesc.Size        = sizeof(FLightData) * kMaxLightCount;
        bufferDesc.Usage       = EBufferUsage::StorageBuffer;
        bufferDesc.MemoryUsage = EMemoryUsage::CpuToGpu;
        bufferDesc.DebugName   = "LightStorageBuffer";

        FRHIBufferHandle buffer;
        result = m_Device->CreateBuffer(bufferDesc, buffer);
        if (!IsRHISuccess(result))
        {
            LIMX_LOG(LogLighting, Error,
                     "[LightManager] 光源 storage buffer [{}] 创建失败", i);
            Shutdown();
            return result;
        }

        m_LightStorageBuffers.Add(buffer);
    }

    // ---- 为每并行帧创建一个聚光灯阴影 storage buffer ----
    //
    // 尺寸按块数而非光源数: 图集只有 64 块, 再多的灯也拿不到第 65 块。
    // 按 kMaxLightCount 开的话是 1024 × 96 = 96 KiB 里 94 KiB 永远是零,
    // 而且着色器索引的是块下标, 越界与否得另外判 —— 现在越界不可能发生。
    m_SpotShadowBuffers.Reserve(maxFramesInFlight);

    for (UInt32 i = 0; i < maxFramesInFlight; ++i)
    {
        FRHIBufferDesc bufferDesc = {};
        bufferDesc.Size        = sizeof(FSpotShadowData) * kShadowTileCount;
        bufferDesc.Usage       = EBufferUsage::StorageBuffer;
        bufferDesc.MemoryUsage = EMemoryUsage::CpuToGpu;
        bufferDesc.DebugName   = "SpotShadowBuffer";

        FRHIBufferHandle buffer;
        result = m_Device->CreateBuffer(bufferDesc, buffer);
        if (!IsRHISuccess(result))
        {
            LIMX_LOG(LogLighting, Error,
                     "[LightManager] 聚光灯阴影 storage buffer [{}] 创建失败", i);
            Shutdown();
            return result;
        }

        m_SpotShadowBuffers.Add(buffer);
    }

    // 预分配光源数组容量
    m_Lights.Reserve(kMaxLightCount);
    m_SpotShadowCasters.Reserve(kShadowTileCount);

    m_IsInitialized = true;

    LIMX_LOG(LogLighting, Log,
             "[LightManager] 初始化完成 — {} 帧 UBO, 每帧 {} 字节, 最大 {} 光源",
             maxFramesInFlight,
             static_cast<UInt64>(sizeof(FLightingUBO)),
             kMaxLightCount);

    return ERHIResult::Success;
}

// ============================================================================
// Shutdown — 释放 GPU 资源
// ============================================================================

void FLightManager::Shutdown()
{
    if (!m_IsInitialized || m_Device == nullptr)
    {
        return;
    }

    // 释放 UBO 缓冲区
    for (SizeType i = 0; i < m_LightUBOs.GetSize(); ++i)
    {
        m_Device->DestroyBuffer(m_LightUBOs[i]);
    }
    m_LightUBOs.Clear();

    // 释放光源 storage buffer
    for (SizeType i = 0; i < m_LightStorageBuffers.GetSize(); ++i)
    {
        m_Device->DestroyBuffer(m_LightStorageBuffers[i]);
    }
    m_LightStorageBuffers.Clear();

    // 释放聚光灯阴影 storage buffer
    for (SizeType i = 0; i < m_SpotShadowBuffers.GetSize(); ++i)
    {
        m_Device->DestroyBuffer(m_SpotShadowBuffers[i]);
    }
    m_SpotShadowBuffers.Clear();

    m_SpotShadowCasters.Clear();

    // 释放描述符集布局
    m_Device->DestroyDescSetLayout(m_DescSetLayout);

    // 清空光源
    m_Lights.Clear();

    m_Device            = nullptr;
    m_MaxFramesInFlight = 0;
    m_IsInitialized     = false;

    LIMX_LOG(LogLighting, Log,
             "[LightManager] 已关闭");
}

// ============================================================================
// AddLight — 添加光源
// ============================================================================

UInt32 FLightManager::AddLight(FLight&& light)
{
    if (m_Lights.GetSize() >= kMaxLightCount)
    {
        LIMX_LOG(LogLighting, Warning,
                 "[LightManager] 光源数量已达上限 ({}), 忽略添加: {}",
                 kMaxLightCount, light.GetDebugName());
        return 0xFFFFFFFF;
    }

    UInt32 index = static_cast<UInt32>(m_Lights.GetSize());
    m_Lights.Add(static_cast<FLight&&>(light));

    LIMX_LOG(LogLighting, Log,
             "[LightManager] 添加光源 [{}]: {}",
             index, m_Lights[index].GetDebugName());

    return index;
}

// ============================================================================
// RemoveLight — 交换删除
// ============================================================================

void FLightManager::RemoveLight(UInt32 index)
{
    if (index >= static_cast<UInt32>(m_Lights.GetSize()))
    {
        LIMX_LOG(LogLighting, Warning,
                 "[LightManager] 移除无效索引: {}", index);
        return;
    }

    SizeType lastIndex = m_Lights.GetSize() - 1;

    LIMX_LOG(LogLighting, Log,
             "[LightManager] 移除光源 [{}]: {}",
             index, m_Lights[index].GetDebugName());

    // 交换到末尾再移除 (如果不是最后一个)
    if (static_cast<SizeType>(index) < lastIndex)
    {
        m_Lights[index] = static_cast<FLight&&>(m_Lights[lastIndex]);
    }

    m_Lights.RemoveAt(lastIndex);
}

// ============================================================================
// GetLight — 索引访问
// ============================================================================

FLight& FLightManager::GetLight(UInt32 index)
{
    LIMX_CHECK(index < static_cast<UInt32>(m_Lights.GetSize()));
    return m_Lights[index];
}

const FLight& FLightManager::GetLight(UInt32 index) const
{
    LIMX_CHECK(index < static_cast<UInt32>(m_Lights.GetSize()));
    return m_Lights[index];
}

// ============================================================================
// GetLightCount
// ============================================================================

UInt32 FLightManager::GetLightCount() const
{
    return static_cast<UInt32>(m_Lights.GetSize());
}

// ============================================================================
// ClearAllLights
// ============================================================================

void FLightManager::ClearAllLights()
{
    LIMX_LOG(LogLighting, Log,
             "[LightManager] 清空所有光源 (共 {} 盏)",
             m_Lights.GetSize());
    m_Lights.Clear();
}

// ============================================================================
// UploadLightData — 打包并上传光照 UBO
// ============================================================================

void FLightManager::UploadLightData(
    UInt32 frameIndex,
    const FVector3& cameraPosition)
{
    if (!m_IsInitialized || m_Device == nullptr)
    {
        return;
    }

    LIMX_CHECK(frameIndex < m_MaxFramesInFlight);

    // 在栈上构建 FLightingUBO
    FLightingUBO uboData;
    MemZero(&uboData, sizeof(FLightingUBO));

    // 打包所有启用的光源, 写进 storage buffer
    UInt32 activeLightCount = 0;
    UInt32 directionalCount = 0;

    // 阴影块的分配从零开始重来。沿用上一帧的分配看似能少写几次缓冲区,
    // 但灯的增删会让"块下标"与"第几盏投影灯"错位, 而那的表现是某盏灯
    // 突然采到别人的阴影 —— 只在增删灯的那一帧之后才出现, 极难复现。
    m_SpotShadowCasters.Clear();

    UInt32 shadowRequestCount = 0;

    void* lightPtr = nullptr;

    if (IsRHISuccess(m_Device->MapBuffer(m_LightStorageBuffers[frameIndex],
                                        &lightPtr)) &&
        lightPtr != nullptr)
    {
        FLightData* const lights = static_cast<FLightData*>(lightPtr);

        // 两趟: 方向光先写, 其余随后。
        //
        // 方向光必须占据 [0, DirectionalCount) —— 它们不参与分簇剔除, 所以
        // 分簇模式下片段着色器要单独遍历它们。散落在缓冲区各处的话, 那一遍
        // 就得扫过全部光源并逐个判类型, 每像素 O(N) 的分支, 而分簇的全部
        // 意义就是消掉那个 O(N)。
        bool truncated = false;

        for (UInt32 phase = 0; phase < 2u && !truncated; ++phase)
        {
            const bool wantDirectional = (phase == 0u);

            for (SizeType i = 0; i < m_Lights.GetSize(); ++i)
            {
                if (!m_Lights[i].IsEnabled())
                {
                    continue;
                }

                const bool isDirectional =
                    (m_Lights[i].GetType() == ELightType::Directional);

                if (isDirectional != wantDirectional)
                {
                    continue;
                }

                if (activeLightCount >= kMaxLightCount)
                {
                    // 超过上限时明确报出来。静默截断的表现是"多放的光源
                    // 不亮", 而那看起来像是强度或衰减参数没调好。
                    LIMX_LOG(LogLighting, Warning,
                             "[LightManager] 活跃光源超过上限 {} — 其余被忽略",
                             kMaxLightCount);
                    truncated = true;
                    break;
                }

                lights[activeLightCount] = m_Lights[i].ToGpuData();

                // ---- 阴影块分配 ----
                //
                // 块下标就是"本帧第几块", 与光源在缓冲区里的位置无关。两者
                // 绑定的话, 方向光排在前面这件事就会把块 0~N 白白占掉。
                //
                // 聚光灯占一块, 点光源占**连续的六块** (立方体的六个面)。
                // 连续是着色器那边的前提: 它拿到的是第一面的块下标, 再加上
                // 由片段方向算出的面下标。不连续的话每盏点光要传六个下标,
                // 而 FLightData 里只有一个 float 的位置。
                if (m_Lights[i].CastsShadow())
                {
                    const bool isSpot =
                        (m_Lights[i].GetType() == ELightType::Spot);
                    const bool isPoint =
                        (m_Lights[i].GetType() == ELightType::Point);

                    const UInt32 needed =
                        isSpot ? 1u : (isPoint ? kCubeFaceCount : 0u);

                    if (needed > 0u)
                    {
                        shadowRequestCount += needed;

                        const UInt32 tileIndex =
                            static_cast<UInt32>(m_SpotShadowCasters.GetSize());

                        if (tileIndex + needed <= kShadowTileCount)
                        {
                            if (isSpot)
                            {
                                const FMatrix viewProj = ComputeSpotShadowMatrix(
                                    m_Lights[i].GetPosition(),
                                    m_Lights[i].GetDirection(),
                                    lights[activeLightCount].SpotOuterCos,
                                    m_Lights[i].GetRange());

                                m_SpotShadowCasters.Add(
                                    MakeSpotShadowData(tileIndex, viewProj));
                            }
                            else
                            {
                                for (UInt32 face = 0; face < kCubeFaceCount;
                                     ++face)
                                {
                                    const FMatrix viewProj =
                                        ComputeCubeFaceShadowMatrix(
                                            m_Lights[i].GetPosition(), face,
                                            m_Lights[i].GetRange());

                                    m_SpotShadowCasters.Add(
                                        MakeSpotShadowData(tileIndex + face,
                                                           viewProj));
                                }
                            }

                            lights[activeLightCount].ShadowTileIndex =
                                static_cast<Float32>(tileIndex);
                        }
                    }
                }

                ++activeLightCount;

                if (wantDirectional)
                {
                    ++directionalCount;
                }
            }
        }

        m_Device->UnmapBuffer(m_LightStorageBuffers[frameIndex]);
    }
    else
    {
        LIMX_LOG(LogLighting, Error,
                 "[LightManager] 光源 storage buffer 映射失败 — 本帧无光照");
    }

    m_ActiveLightCount = activeLightCount;

    // 要块的灯多过块数时明确报出来。
    //
    // 静默丢弃的表现是"有些灯没有影子", 而那与"这盏灯本来就没开阴影"在
    // 画面上完全一样 —— 于是没人会去查图集满没满。
    //
    // 只在数目**变化**时报一次。每帧都报的话, 一个场景跑十秒就是六百行同样
    // 的警告 —— 而淹在六百行里的东西与没报没有区别, 后面真正要紧的日志也一
    // 起被冲掉了。
    if (shadowRequestCount != m_LastShadowRequestCount)
    {
        if (shadowRequestCount > kShadowTileCount)
        {
            LIMX_LOG(LogLighting, Warning,
                     "[LightManager] 请求阴影的聚光灯 {} 盏, 超过图集的 {} 块 — "
                     "其余按无遮挡处理",
                     shadowRequestCount, kShadowTileCount);
        }

        m_LastShadowRequestCount = shadowRequestCount;
    }

    UploadSpotShadowData(frameIndex);

    // 写入全局光照参数
    uboData.LightCount       = static_cast<Float32>(activeLightCount);
    uboData.DirectionalCount = static_cast<Float32>(directionalCount);
    // 哪一盏灯走光追阴影 —— 着色器按它决定用掩码还是用阴影贴图
    uboData.RayTracedShadowLight =
        static_cast<Float32>(m_RayTracedShadowLight);

    uboData.RayTracedFlags   = m_RayTracedFlags;

    // 分簇参数
    const FClusterSliceMapping mapping =
        ComputeSliceMapping(m_ClusterNearPlane, m_ClusterFarPlane);

    uboData.ClusterSliceScale = mapping.Scale;
    uboData.ClusterSliceBias  = mapping.Bias;
    uboData.ClusterScreenW    = m_ClusterScreenWidth;
    uboData.ClusterScreenH    = m_ClusterScreenHeight;
    uboData.ClusteredEnabled  = m_IsClusteredEnabled ? 1.0f : 0.0f;

    uboData.CameraPositionX = cameraPosition.X;
    uboData.CameraPositionY = cameraPosition.Y;
    uboData.CameraPositionZ = cameraPosition.Z;
    uboData.CameraPositionW = 0.0f;

    // 常数环境光 —— 没有环境贴图时的兜底
    //
    // 它的职责只有一个: 别让没被直接光照到的表面变成纯黑。真正的环境
    // 光照请用 IBL, 这里只是让场景在缺少 HDRI 时仍然可读。
    //
    // 0.15 而非原先的 0.03: 原值是对着一个双重 sRGB 编码的输出用肉眼调
    // 出来的 (见 tonemap.frag), 那时它渲染出来是 45/255; 编码修正之后
    // 同样的 0.03 只剩 6.8/255 —— Sponza 的室内会整片全黑。0.15 恢复的
    // 正是当初调这个值时想要的观感。
    //
    // 这类"参数是照着 bug 调出来的"在 bug 修好那一刻会同时暴露, 一并
    // 处理才不会留下一个看着没道理的常数。
    uboData.AmbientColorR    = 0.15f;
    uboData.AmbientColorG    = 0.15f;
    uboData.AmbientColorB    = 0.15f;
    uboData.AmbientIntensity = 1.0f;

    // 阴影 —— 矩阵与参数由渲染器在阴影 Pass 之后写入本管理器
    for (UInt32 i = 0; i < FLightingUBO::kShadowCascadeCount; ++i)
    {
        uboData.CascadeViewProj[i] = m_ShadowInfo.CascadeViewProj[i];
    }

    uboData.CascadeSplit0 = m_ShadowInfo.CascadeSplits[0];
    uboData.CascadeSplit1 = m_ShadowInfo.CascadeSplits[1];
    uboData.CascadeSplit2 = m_ShadowInfo.CascadeSplits[2];

    uboData.ShadowDepthBias   = m_ShadowInfo.DepthBias;
    uboData.ShadowNormalBias  = m_ShadowInfo.NormalBias;
    uboData.ShadowMapSize     = m_ShadowInfo.ShadowMapSize;
    uboData.ShadowEnabled     = m_IsShadowEnabled ? 1.0f : 0.0f;

    uboData.IblEnabled           = m_IsIblEnabled ? 1.0f : 0.0f;
    uboData.IblIntensity         = m_IblIntensity;
    uboData.IblPrefilteredMaxLod = m_IblPrefilteredMaxLod;

    // 映射 UBO 并写入
    void* mappedPtr = nullptr;
    ERHIResult result = m_Device->MapBuffer(
        m_LightUBOs[frameIndex], &mappedPtr);
    if (IsRHISuccess(result))
    {
        MemCopy(mappedPtr, &uboData, sizeof(FLightingUBO));
        m_Device->UnmapBuffer(m_LightUBOs[frameIndex]);
    }
}

// ============================================================================
// GetLightStorageBuffer — 指定帧的光源 storage buffer
// ============================================================================

FRHIBufferHandle FLightManager::GetLightStorageBuffer(UInt32 frameIndex) const
{
    if (frameIndex >= m_LightStorageBuffers.GetSize())
    {
        return FRHIBufferHandle();
    }

    return m_LightStorageBuffers[frameIndex];
}

// ============================================================================
// UploadSpotShadowData — 把本帧的阴影块写进 storage buffer
//
// 先整块清零再写前缀。只写用到的那几块也能跑, 但上一帧留下的数据会一直
// 躺在后面 —— 而只要有一个环节把块下标算错一位, 读到的就是上一帧某盏灯
// 的矩阵: 一个**看着挺像回事**的阴影, 位置却是错的。清零之后同样的错误
// 会读到全零矩阵, 那是一眼可见的坏。
//
// 代价是每帧 6 KiB 的写入, 在 CpuToGpu 内存上不到一微秒。
// ============================================================================

void FLightManager::UploadSpotShadowData(UInt32 frameIndex)
{
    if (frameIndex >= m_SpotShadowBuffers.GetSize())
    {
        return;
    }

    void* mapped = nullptr;

    if (!IsRHISuccess(m_Device->MapBuffer(m_SpotShadowBuffers[frameIndex],
                                          &mapped)) ||
        mapped == nullptr)
    {
        LIMX_LOG(LogLighting, Error,
                 "[LightManager] 聚光灯阴影缓冲区映射失败 — 本帧无聚光灯阴影");
        return;
    }

    MemZero(mapped, sizeof(FSpotShadowData) * kShadowTileCount);

    if (!m_SpotShadowCasters.IsEmpty())
    {
        MemCopy(mapped, m_SpotShadowCasters.GetData(),
                sizeof(FSpotShadowData) * m_SpotShadowCasters.GetSize());
    }

    m_Device->UnmapBuffer(m_SpotShadowBuffers[frameIndex]);
}

// ============================================================================
// GetSpotShadowBuffer — 聚光灯阴影数据 (set 2, binding 9)
// ============================================================================

FRHIBufferHandle FLightManager::GetSpotShadowBuffer(UInt32 frameIndex) const
{
    if (frameIndex >= m_SpotShadowBuffers.GetSize())
    {
        return FRHIBufferHandle();
    }

    return m_SpotShadowBuffers[frameIndex];
}

// ============================================================================
// SetShadowInfo — 记录主方向光的级联阴影数据
// ============================================================================

void FLightManager::SetShadowInfo(const FCascadedShadowInfo& info)
{
    m_ShadowInfo      = info;
    m_IsShadowEnabled = true;
}

// ============================================================================
// GetLightUBO — 获取指定帧的 UBO 句柄
// ============================================================================

FRHIBufferHandle FLightManager::GetLightUBO(UInt32 frameIndex) const
{
    LIMX_CHECK(frameIndex < m_MaxFramesInFlight);
    return m_LightUBOs[frameIndex];
}

} // namespace Limx
