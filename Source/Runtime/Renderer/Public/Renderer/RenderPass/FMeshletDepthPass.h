/*******************************************************************************
 * 文件: FMeshletDepthPass.h
 * 创建时间: 2026-09-02
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   把剔除通道选出来的 meshlet 光栅化进一张独立的深度图 —— 两条路径:
 *   网格着色器, 与不支持时的计算展开 + 经典顶点着色器。
 *
 * 设计哲学:
 *   **回退路径不是"以后再说"。** 网格着色器在相当一部分在役硬件上没有,
 *     而一条只在新卡上跑的渲染路径等于没有路径。更要紧的是: 两条路径
 *     给出同一张图这件事本身就是最强的判据 —— 它把"簇化数据结构解得对
 *     不对"与"某一条光栅化路径写得对不对"分开了。
 *
 *   **两条路径的顶点数学只有一份实现。** 在 meshlet_raster_common.h 里,
 *     连运算顺序都钉死成 (viewProj * model) * p —— 与 depth_only.vert
 *     逐字相同。写成 viewProj * (model * p) 数学上等价而浮点上不等价,
 *     于是深度差一两个最低位: 不崩、不报错, 只让逐位比对永远差那么一点。
 *
 *   **画进独立的深度图, 不覆盖共享深度。** 判据要把两者放在一起比,
 *     覆盖掉就没有对照了。等 VisBuffer 那一天这条路径成为主路径时再谈
 *     取代, 而那时判据已经建立起来了。
 *
 * 技术特性:
 *   - Order 12: 紧跟 FMeshletCullPass (11), 早于深度预通道 (100)
 *   - 深度格式与共享深度一致, 尺寸随交换链
 *   - 网格路径: 一个工作组一个可见 meshlet, DrawMeshTasksIndirect
 *   - 回退路径: 计算着色器展开成顶点流 + DrawIndirect
 *
 * 注意事项:
 *   本通道只画**不透明**批次 —— 蒙版材质要 alpha 测试, 而那要到材质解析
 *   那一天才有地方放。判据据此成立: 本路径的三角形集合是经典深度预通道
 *   的子集, 于是每个像素上本路径的深度只能等于或者远于经典路径的。
 *
 ******************************************************************************/

#pragma once

#include "Renderer/RenderPass/IRenderPass.h"

#include "Core/Containers/TArray.h"

namespace Limx
{

/// 展开顶点流的容量 (以顶点计)
///
/// 每个顶点 8 字节, 2M 个 = 16 MiB。够 5400 个装满的 meshlet。
/// 超出时**丢弃并报数** —— 不是越界写。
inline constexpr UInt32 kMaxExpandedVertices = 2097152;

// ============================================================================
// FMeshletDepthPass
// ============================================================================

class FMeshletDepthPass final : public IRenderPass
{
public:
    /// 光栅化路径
    enum class EMode : UInt32
    {
        /// 网格着色器 —— 设备支持时的默认
        MeshShader,

        /// 计算展开 + 经典顶点着色器
        Fallback,
    };

    FMeshletDepthPass()           = default;
    ~FMeshletDepthPass() override = default;

    LIMX_NODISCARD const AnsiChar* GetName() const override
    {
        return "MeshletDepthPass";
    }

    /// 12 — 紧跟 FMeshletCullPass (11)
    LIMX_NODISCARD UInt32 GetOrder() const override { return 12; }

    ERHIResult Setup(const FPassSetupDesc& desc) override;

    void Execute(IRHICommandBuffer*        commandBuffer,
                 const FRenderPassContext& context) override;

    ERHIResult OnResize(const FPassResizeDesc& desc) override;

    void ReleaseSwapchainResources(IRHIDevice* device) override;

    void Shutdown(IRHIDevice* device) override;

    // ====================================================================
    // 开关
    // ====================================================================

    void SetEnabled(bool enabled) { m_Enabled = enabled; }

    LIMX_NODISCARD bool IsEnabled() const { return m_Enabled; }

    /// 选路径。请求网格着色器而设备不支持时**返回 false 并保持原样**
    ///
    /// 不静默退回: "我要网格着色器路径"与"给了我回退路径"必须分得开,
    /// 不然判据比的是两次同样的回退。
    LIMX_NODISCARD bool SetMode(EMode mode);

    LIMX_NODISCARD EMode GetMode() const { return m_Mode; }

    LIMX_NODISCARD bool IsMeshShaderAvailable() const
    {
        return m_MeshShaderAvailable;
    }

    // ====================================================================
    // 结果
    // ====================================================================

    LIMX_NODISCARD FRHITextureHandle GetDepthTexture() const
    {
        return m_DepthTexture;
    }

    /// 上一次执行画了多少个 meshlet (由剔除通道的计数器给出)
    LIMX_NODISCARD UInt32 GetDrawnMeshlets() const { return m_DrawnMeshlets; }

private:
    ERHIResult CreateDepthTarget(IRHIDevice* device, FRHIExtent2D extent);
    ERHIResult CreateRenderPass(IRHIDevice* device);
    ERHIResult CreatePipelines(IRHIDevice* device);
    ERHIResult CreateDescriptors(IRHIDevice* device, UInt32 frameCount);

    void DestroyDepthTarget(IRHIDevice* device);

    bool  m_Enabled = false;
    EMode m_Mode    = EMode::MeshShader;

    bool m_MeshShaderAvailable = false;

    IRHIDevice* m_Device = nullptr;

    UInt32 m_FrameCount = 0;

    FRHIExtent2D m_Extent = {};

    FRHITextureHandle     m_DepthTexture;
    FRHITextureViewHandle m_DepthView;
    FRHIRenderPassHandle  m_RenderPass;
    FRHIFramebufferHandle m_Framebuffer;

    /// 展开出来的顶点流与间接绘制参数 (逐并行帧)
    TArray<FRHIBufferHandle> m_ExpandedBuffers;
    TArray<FRHIBufferHandle> m_DrawArgsBuffers;

    /// 归零源 —— 间接绘制参数每帧要复位 (vertexCount=0, instanceCount=1)
    FRHIBufferHandle m_ResetSource;

    /// 全场景顶点缓冲区 —— 由本帧的对象列表定, 逐帧可能变
    FRHIBufferHandle m_SceneVertexBuffer;
    FRHIBufferHandle m_SceneMeshletVertexBuffer;
    FRHIBufferHandle m_SceneMeshletTriangleBuffer;

    FRHIDescSetLayoutHandle m_MeshSetLayout;
    FRHIDescSetLayoutHandle m_ExpandSetLayout;
    FRHIDescSetLayoutHandle m_FallbackSetLayout;

    TArray<FRHIDescriptorSetHandle> m_MeshSets;
    TArray<FRHIDescriptorSetHandle> m_ExpandSets;
    TArray<FRHIDescriptorSetHandle> m_FallbackSets;

    FRHIPipelineLayoutHandle m_MeshPipelineLayout;
    FRHIPipelineLayoutHandle m_ExpandPipelineLayout;
    FRHIPipelineLayoutHandle m_FallbackPipelineLayout;

    FRHIGraphicsPipelineHandle m_MeshPipeline;
    FRHIComputePipelineHandle  m_ExpandPipeline;
    FRHIGraphicsPipelineHandle m_FallbackPipeline;

    FRHIShaderHandle m_MeshShader;
    FRHIShaderHandle m_FragmentShader;
    FRHIShaderHandle m_ExpandShader;
    FRHIShaderHandle m_FallbackVertexShader;

    /// 上一次描述符指向的那组场景缓冲区 —— 变了才重写描述符
    FRHIBufferHandle m_BoundVertexBuffer;

    UInt32 m_DrawnMeshlets = 0;
};

} // namespace Limx
