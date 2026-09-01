// ============================================================
// 文件名称：FRayTracedReflectionPass.h
// 创建时间：2026-09-01
// 创建者  ：LimxTeam
// 设计哲学：屏幕空间反射的缺口是**看不出来的** —— 相机背后的、被挡住的、
//          视野外的东西反射不出来，而画面上只是"那里没反射"，与"那里本来
//          就不该有反射"长得一样。光追反射没有这个缺口，代价是命中之后
//          要自己把顶点、法线、材质取回来。手写的每一步都可能错位，
//          所以这个通道带一个调试输出模式：把命中距离、材质下标、插值法线
//          原样写出来，让判据逐像素对，而不是对着一张颜色图猜。
// 功能描述：逐像素沿反射方向发一条射线，命中后按重心坐标插值顶点属性、
//          查材质、算一次朗伯着色（带一条阴影射线），输出反射辐射度。
// 技术特性：RGBA32F 目标（rgb=辐射度 或 调试原始量，a=命中距离，-1=未命中）；
//          几何用缓冲区设备地址取，不占描述符槽。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                          │ 描述                      │
// │────────────────────────────────│─────────────────────────│
// │ SetInputs()                    │ 绑定深度与法线输入        │
// │ SetSceneBuffers()              │ 绑定几何表与材质表        │
// │ SetDebugOutput()               │ 切换调试原始量输出        │
// │ GetReflectionView()            │ 取反射图                  │
// ============================================================

#pragma once

#include "Renderer/RendererMinimal.h"
#include "Renderer/RenderPass/IRenderPass.h"

#include "RenderCore/Lighting/FLight.h"

namespace Limx
{

class LIMX_RENDERER_API FRayTracedReflectionPass final : public IRenderPass
{
public:
    FRayTracedReflectionPass() = default;
    ~FRayTracedReflectionPass() override = default;

    LIMX_NODISCARD const AnsiChar* GetName() const override
    {
        return "RayTracedReflectionPass";
    }

    /// 排在光追 AO (130) 之后, 天空 (150) 之前
    LIMX_NODISCARD UInt32 GetOrder() const override { return 140; }

    ERHIResult Setup(const FPassSetupDesc& desc) override;

    void Execute(IRHICommandBuffer*        commandBuffer,
                 const FRenderPassContext& context) override;

    ERHIResult OnResize(const FPassResizeDesc& desc) override;

    void ReleaseSwapchainResources(IRHIDevice* device) override;

    void Shutdown(IRHIDevice* device) override;

    void SetInputs(FRHITextureHandle depthTexture,
                   FRHITextureViewHandle depthView,
                   FRHITextureViewHandle normalView)
    {
        m_DepthTexture = depthTexture;
        m_DepthView    = depthView;
        m_NormalView   = normalView;
    }

    void SetTlas(FRHIAccelStructHandle tlas) { m_Tlas = tlas; }

    /// 几何表与材质表 —— 命中之后要靠它们取回顶点与材质
    void SetSceneBuffers(FRHIBufferHandle geometryTable,
                         UInt64 geometryTableBytes,
                         FRHIBufferHandle materialBuffer,
                         UInt64 materialBufferBytes)
    {
        m_GeometryTable      = geometryTable;
        m_GeometryTableBytes = geometryTableBytes;
        m_MaterialBuffer     = materialBuffer;
        m_MaterialBufferBytes = materialBufferBytes;
    }

    void SetCameraParams(const FMatrix& viewProj, const FVector3& position)
    {
        m_ViewProj   = viewProj;
        m_CameraPos  = position;
    }

    void SetLight(const FLightData& light) { m_Light = light; }

    void SetEnabled(bool enabled) { m_Enabled = enabled; }

    LIMX_NODISCARD bool IsEnabled() const { return m_Enabled; }

    /// 调试输出: rgb = (命中距离, 材质下标, 世界法线 y)
    ///
    /// 判据要验的是"顶点取回来了没有、材质对不对、法线插值对不对", 而这
    /// 三样在着色之后就混成了一个颜色 —— 颜色对不上说不清是哪一步错了。
    void SetDebugOutput(bool enabled) { m_DebugOutput = enabled; }

    LIMX_NODISCARD bool IsDebugOutput() const { return m_DebugOutput; }

    void SetMaxDistance(Float32 distance) { m_MaxDistance = distance; }

    LIMX_NODISCARD Float32 GetMaxDistance() const { return m_MaxDistance; }

    LIMX_NODISCARD FRHITextureHandle GetReflectionTexture() const
    {
        return m_ReflectionTexture;
    }

    LIMX_NODISCARD FRHITextureViewHandle GetReflectionView() const
    {
        return m_ReflectionView;
    }

private:
    ERHIResult CreateTarget(IRHIDevice* device, FRHIExtent2D extent);
    ERHIResult CreatePipeline(IRHIDevice* device);
    ERHIResult CreateDescriptors(IRHIDevice* device);

    IRHIDevice* m_Device = nullptr;

    bool m_Enabled     = false;
    bool m_DebugOutput = false;

    FRHIExtent2D m_Extent = {};

    FRHITextureHandle     m_ReflectionTexture;
    FRHITextureViewHandle m_ReflectionView;

    FRHITextureHandle     m_DepthTexture;
    FRHITextureViewHandle m_DepthView;
    FRHITextureViewHandle m_NormalView;

    FRHISamplerHandle m_PointSampler;

    FRHIAccelStructHandle m_Tlas;

    FRHIBufferHandle m_GeometryTable;
    UInt64           m_GeometryTableBytes = 0;
    FRHIBufferHandle m_MaterialBuffer;
    UInt64           m_MaterialBufferBytes = 0;

    FRHIShaderHandle          m_Shader;
    FRHIDescSetLayoutHandle   m_SetLayout;
    FRHIDescriptorSetHandle   m_DescriptorSet;
    FRHIPipelineLayoutHandle  m_PipelineLayout;
    FRHIComputePipelineHandle m_Pipeline;

    FLightData m_Light;

    FMatrix  m_ViewProj  = FMatrix::kIdentity;
    FVector3 m_CameraPos = FVector3(0.0f, 0.0f, 0.0f);

    /// 反射射线的最大距离
    Float32 m_MaxDistance = 100.0f;

    Float32 m_NormalOffset = 1.0e-3f;
    Float32 m_RayTMin      = 1.0e-3f;

    /// 环境光常量 —— 未命中时的返回值, 也是命中处的环境项
    Float32 m_Ambient = 0.03f;

    bool m_LayoutInitialized = false;
};

} // namespace Limx
