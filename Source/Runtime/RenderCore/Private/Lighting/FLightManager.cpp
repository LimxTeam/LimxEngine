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

    // ---- 创建描述符集布局: set 2, binding 0 — FLightingUBO ----
    FRHIDescriptorBinding binding = {};
    binding.Binding    = 0;
    binding.Type       = EDescriptorType::UniformBuffer;
    binding.Count      = 1;
    binding.StageFlags = EShaderStage::Vertex | EShaderStage::Fragment;

    FRHIDescSetLayoutDesc layoutDesc = {};
    layoutDesc.Bindings     = &binding;
    layoutDesc.BindingCount = 1;
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

    // 环境光 — 微弱的白色环境光
    uboData.AmbientColorR    = 0.03f;
    uboData.AmbientColorG    = 0.03f;
    uboData.AmbientColorB    = 0.03f;
    uboData.AmbientIntensity = 1.0f;

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
// GetLightUBO — 获取指定帧的 UBO 句柄
// ============================================================================

FRHIBufferHandle FLightManager::GetLightUBO(UInt32 frameIndex) const
{
    LIMX_CHECK(frameIndex < m_MaxFramesInFlight);
    return m_LightUBOs[frameIndex];
}

} // namespace Limx
