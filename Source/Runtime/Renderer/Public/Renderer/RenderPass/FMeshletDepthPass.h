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

// ============================================================================
// FMeshletResolveResult — 与 meshlet_resolve.comp 的 ResolveResult 一致
// ============================================================================

struct FMeshletResolveResult
{
    /// 由重心坐标重算的 NDC 深度
    Float32 Depth = 1.0f;

    /// 八面体编码的世界法线
    Float32 NormalX = 0.0f;
    Float32 NormalY = 0.0f;

    /// bindless 材质下标; 0xFFFFFFFF = 这个像素上没有几何体
    UInt32 Material = 0xFFFFFFFFu;

    /// 透视校正插值出来的世界坐标
    ///
    /// 不是给着色用的, 是给判据用的: 把它投回屏幕必须落在这个像素的中心
    /// 上。那一条与场景无关, 而且只有透视校正的权重才满足它。
    Float32 WorldX = 0.0f;
    Float32 WorldY = 0.0f;
    Float32 WorldZ = 0.0f;
};

static_assert(sizeof(FMeshletResolveResult) == 28,
              "FMeshletResolveResult 必须是 28 字节 — 与 "
              "meshlet_resolve.comp 的 ResolveResult 逐字段一致 "
              "(那边用 scalar 布局, 所以是紧凑的)");

/// 展开顶点流的容量 (以顶点计)
///
/// 每个顶点 8 字节, 2M 个 = 16 MiB。够 5400 个装满的 meshlet。
/// 超出时**丢弃并报数** —— 不是越界写。
inline constexpr UInt32 kMaxExpandedVertices = 2097152;

// ============================================================================
// FMeshletExpandStats — 一次展开的结果
//
// 两个数必须分开, 而不是一个。见 meshlet_expand.comp 里那段说明:
// 槽位分配器加的是"想写多少", 而 vkCmdDrawIndirect 读的必须是"真写了多少"。
// 合成一个的话, 被丢掉的三角形照样把绘制的顶点数加了上去 —— 顶点着色器
// 拿 gl_VertexIndex 无条件索引那条流, 于是 GPU 读到缓冲区之外。
// ============================================================================

struct FMeshletExpandStats
{
    /// 本帧生效的流容量 (以顶点计) —— 判据可以把它压小
    UInt32 Capacity = 0;

    /// 槽位分配器发出去的顶点数 (含被丢掉的那些)
    ///
    /// 大于容量就是溢出的证据。没有这个数的话, 溢出与"场景恰好正好装满"
    /// 在计数上分不开。
    UInt32 Requested = 0;

    /// 真正写进流里的顶点数 —— 也就是交给 DrawIndirect 的那个数
    UInt32 Written = 0;

    /// 这一帧有没有溢出过
    LIMX_NODISCARD bool HasOverflow() const { return Requested > Capacity; }
};

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

    /// 可见性缓冲区 (R32_UINT)
    ///
    /// 每个像素记的是 (可见记录槽位, 三角形序号) + 1, 0 表示这里没有
    /// 几何体。可见记录唯一确定实例与 meshlet, 所以不必再存它们 ——
    /// 存了反而多一层"这几个数一致吗"的问题。
    LIMX_NODISCARD FRHITextureHandle GetVisibilityTexture() const
    {
        return m_VisibilityTexture;
    }

    /// 上一次执行画了多少个 meshlet (由剔除通道的计数器给出)
    LIMX_NODISCARD UInt32 GetDrawnMeshlets() const { return m_DrawnMeshlets; }

    /// 材质解析的结果缓冲区 (每像素 FMeshletResolveResult)
    ///
    /// 一个像素一条: 由重心坐标重算的 NDC 深度、八面体编码的世界法线、
    /// bindless 材质下标 (0xFFFFFFFF = 这里没有几何体)。
    LIMX_NODISCARD FRHIBufferHandle GetResolveBuffer(UInt32 frameIndex) const;

    /// 两阶段遮挡剔除开关
    ///
    /// 打开之后本通道多做三件事: 画完第一阶段就地建一次金字塔、把第一阶段
    /// 被遮挡剔掉的重测一遍、把活下来的补画一遍。
    ///
    /// **单阶段是不够的。** 第一阶段用的是上一帧的金字塔, 而这一帧才露出来
    /// 的东西会被它判成"挡住了" —— 表现为物体闪一帧才出现。第二阶段用这一
    /// 帧的深度重测, 那条近似就没了。
    void SetOcclusionCullEnabled(bool enabled);

    LIMX_NODISCARD bool IsOcclusionCullEnabled() const
    {
        return m_OcclusionCull;
    }

    /// 层次深度金字塔 —— 只作诊断与判据
    LIMX_NODISCARD FRHITextureHandle GetHizTexture() const
    {
        return m_HizTexture;
    }

    LIMX_NODISCARD UInt32 GetHizLevelCount() const { return m_HizLevels; }

    LIMX_NODISCARD FRHIExtent2D GetHizExtent() const { return m_Extent; }

    /// 第二阶段补画了多少个 meshlet (上一帧的数)
    LIMX_NODISCARD UInt32 GetPhase2Meshlets() const { return m_Phase2Meshlets; }

    /// 材质解析开关
    ///
    /// 与光栅化分开: 合在一起的话, "解析整个没跑"与"解析跑了但结果不对"
    /// 在判据上分不开 —— 前者的输出缓冲区里是上一帧的内容。
    void SetResolveEnabled(bool enabled) { m_ResolveEnabled = enabled; }

    LIMX_NODISCARD bool IsResolveEnabled() const { return m_ResolveEnabled; }

    // ====================================================================
    // 展开顶点流的容量与统计
    // ====================================================================

    /// 上一次展开的统计 —— 回读隔着并行帧数, 与本通道别的计数同理
    LIMX_NODISCARD const FMeshletExpandStats& GetExpandStats() const
    {
        return m_ExpandStats;
    }

    /// 判据用: 把展开流的容量压到一个很小的数, 逼出溢出
    ///
    /// 与 FMeshletCullPass::SetVisibleCapacityOverride 同一个理由 —— 靠场景
    /// 规模走到这条路径要堆出两百万个顶点的可见几何, 那种场景跑一次要几
    /// 分钟。走不到的分支就是没有判据的分支。
    ///
    /// 压小的只是着色器用来判越界的那个数, 缓冲区本身还是满的 —— 于是
    /// 判据验的是"交给 DrawIndirect 的顶点数不许超过容量"这条不变式本身,
    /// 而不是"会不会踩到没映射的页"那件靠堆布局的事。
    ///
    /// 0 表示不覆盖。
    void SetExpandedCapacityOverride(UInt32 capacity)
    {
        m_ExpandedCapacityOverride = capacity;
    }

    LIMX_NODISCARD UInt32 GetExpandedCapacity() const
    {
        return (m_ExpandedCapacityOverride != 0) ? m_ExpandedCapacityOverride
                                                 : kMaxExpandedVertices;
    }

private:
    ERHIResult CreateDepthTarget(IRHIDevice* device, FRHIExtent2D extent);
    ERHIResult CreateRenderPass(IRHIDevice* device);
    ERHIResult CreatePipelines(IRHIDevice* device);
    ERHIResult CreateDescriptors(IRHIDevice* device, UInt32 frameCount);
    ERHIResult CreateHizResources(IRHIDevice* device, FRHIExtent2D extent);

    void BuildHiz(IRHICommandBuffer* commandBuffer, UInt32 frameIndex);

    void DestroyDepthTarget(IRHIDevice* device);

    bool  m_Enabled = false;
    EMode m_Mode    = EMode::MeshShader;

    bool m_MeshShaderAvailable = false;

    IRHIDevice* m_Device = nullptr;

    UInt32 m_FrameCount = 0;

    FRHIExtent2D m_Extent = {};

    FRHITextureHandle     m_DepthTexture;
    FRHITextureViewHandle m_DepthView;

    FRHITextureHandle     m_VisibilityTexture;
    FRHITextureViewHandle m_VisibilityView;
    FRHIRenderPassHandle  m_RenderPass;
    FRHIFramebufferHandle m_Framebuffer;

    /// 第二遍用的渲染通道 —— 附件**加载**而不是清除
    ///
    /// 与第一遍只差 LoadOp 与初始布局。Vulkan 要求管线与渲染通道兼容,
    /// 而"兼容"不看 LoadOp —— 但清除值的语义不同, 所以还是分成两个,
    /// 免得靠"兼容性恰好允许"来成立。
    FRHIRenderPassHandle  m_LoadRenderPass;
    FRHIFramebufferHandle m_LoadFramebuffer;

    FRHIGraphicsPipelineHandle m_MeshPipelineLoad;
    FRHIGraphicsPipelineHandle m_FallbackPipelineLoad;

    /// 建管线时选哪个渲染通道 —— 只在 CreatePipelines 里短暂为真
    bool m_UseLoadRenderPass = false;

    /// 展开出来的顶点流与间接绘制参数 (逐并行帧)
    TArray<FRHIBufferHandle> m_ExpandedBuffers;
    TArray<FRHIBufferHandle> m_DrawArgsBuffers;

    /// 间接绘制参数的回读 —— 判据要看"交出去的顶点数"与"想写的顶点数"
    TArray<FRHIBufferHandle> m_DrawArgsReadbacks;

    /// 每个帧下标上一轮展开时**生效的**容量
    ///
    /// 回读隔着并行帧数, 读回来的两个数属于上一轮。拿**当前**的容量去比
    /// 它们的话, 容量刚被改小的那两帧会报出一次并不存在的越界 —— 判据里
    /// 的假警比漏报还坏, 下一个人会去把那条判据关掉。
    TArray<UInt32> m_ExpandedCapacityInFlight;

    /// 判据压小的展开流容量; 0 表示不覆盖
    UInt32 m_ExpandedCapacityOverride = 0;

    FMeshletExpandStats m_ExpandStats;

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

    // ---- 两阶段遮挡剔除 ----
    bool m_OcclusionCull = false;

    /// 层次深度金字塔 —— mip 0 与深度图同尺寸, 逐级减半 (向上取整)
    ///
    /// 尺寸不对齐到二的幂: 对齐要么放大 (浪费显存与带宽) 要么缩小
    /// (丢掉边上的遮挡信息)。向上取整的逐级减半在边界上用钳边取样,
    /// 取到同一个纹素两次 —— 最大值不变, 仍然保守。
    FRHITextureHandle              m_HizTexture;
    TArray<FRHITextureViewHandle>  m_HizLevelViews;
    FRHITextureViewHandle          m_HizFullView;
    FRHISamplerHandle              m_HizSampler;

    UInt32 m_HizLevels = 0;

    FRHIDescSetLayoutHandle   m_HizCopySetLayout;
    FRHIDescSetLayoutHandle   m_HizBuildSetLayout;
    FRHIPipelineLayoutHandle  m_HizCopyLayout;
    FRHIPipelineLayoutHandle  m_HizBuildLayout;
    FRHIComputePipelineHandle m_HizCopyPipeline;
    FRHIComputePipelineHandle m_HizBuildPipeline;
    FRHIShaderHandle          m_HizCopyShader;
    FRHIShaderHandle          m_HizBuildShader;

    FRHIDescriptorSetHandle         m_HizCopySet;
    TArray<FRHIDescriptorSetHandle> m_HizBuildSets;

    FRHISamplerHandle m_DepthSampler;

    /// 第二阶段
    FRHIDescSetLayoutHandle         m_Phase2SetLayout;
    FRHIPipelineLayoutHandle        m_Phase2Layout;
    FRHIComputePipelineHandle       m_Phase2Pipeline;
    FRHIShaderHandle                m_Phase2Shader;
    TArray<FRHIDescriptorSetHandle> m_Phase2Sets;

    /// 第二阶段的间接分派参数与第二次绘制的间接参数
    TArray<FRHIBufferHandle> m_Phase2DispatchBuffers;
    TArray<FRHIBufferHandle> m_Phase2RasterArgsBuffers;

    /// 第一阶段画完时的可见数 —— 第二次绘制从这里往后画
    TArray<FRHIBufferHandle> m_Phase1CountBuffers;
    TArray<FRHIBufferHandle> m_Phase1Readbacks;
    TArray<FRHIBufferHandle> m_Phase2FinalReadbacks;

    UInt32 m_Phase2Meshlets = 0;

    // ---- 材质解析 ----
    bool m_ResolveEnabled = false;

    TArray<FRHIBufferHandle> m_ResolveBuffers;

    /// 逐实例的材质下标 —— 与实例表平行
    TArray<FRHIBufferHandle> m_MaterialBuffers;

    FRHIDescSetLayoutHandle   m_ResolveSetLayout;
    FRHIPipelineLayoutHandle  m_ResolvePipelineLayout;
    FRHIComputePipelineHandle m_ResolvePipeline;
    FRHIShaderHandle          m_ResolveShader;

    TArray<FRHIDescriptorSetHandle> m_ResolveSets;

    FRHITextureViewHandle m_VisibilityStorageView;

    FRHIShaderHandle m_MeshShader;
    FRHIShaderHandle m_FragmentShader;
    FRHIShaderHandle m_FallbackFragmentShader;
    FRHIShaderHandle m_ExpandShader;
    FRHIShaderHandle m_FallbackVertexShader;

    /// 上一次描述符指向的那组场景缓冲区 —— 变了才重写描述符
    /// 每个帧下标各记一份"描述符已经指到哪个场景顶点缓冲区了"
    ///
    /// 一份是不够的: 那样一次要改所有帧下标的集, 而别的集可能正在被用。
    TArray<FRHIBufferHandle> m_BoundVertexBuffers;

    UInt32 m_DrawnMeshlets = 0;
};

} // namespace Limx
