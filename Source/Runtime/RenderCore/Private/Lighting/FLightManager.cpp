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
    FRHIDescriptorBinding bindings[5] = {};

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

    FRHIDescSetLayoutDesc layoutDesc = {};
    layoutDesc.Bindings     = bindings;
    layoutDesc.BindingCount = 5;
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

    // 预分配光源数组容量
    m_Lights.Reserve(kMaxLightCount);

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

    // 打包所有启用的光源
    UInt32 activeLightCount = 0;
    for (SizeType i = 0; i < m_Lights.GetSize(); ++i)
    {
        if (!m_Lights[i].IsEnabled())
        {
            continue;
        }

        if (activeLightCount >= kMaxLightCount)
        {
            break;
        }

        uboData.Lights[activeLightCount] = m_Lights[i].ToGpuData();
        ++activeLightCount;
    }

    // 写入全局光照参数
    uboData.LightCount     = static_cast<Float32>(activeLightCount);
    uboData.LightCountPad0 = 0.0f;
    uboData.LightCountPad1 = 0.0f;
    uboData.LightCountPad2 = 0.0f;

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
