/*******************************************************************************
 * 文件: FShadowPass.h
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   方向光阴影 Pass — 从光源视角渲染场景深度，产出供前向 Pass 采样的阴影贴图
 *
 * 设计哲学:
 *   阴影贴图自带尺寸，与交换链无关 — 它是光源空间的一张固定分辨率深度图，
 *   窗口缩放不该让阴影质量跟着抖动。因此本 Pass 不参与 OnResize 的尺寸重建。
 *
 *   光源矩阵由本 Pass 自己算并回写给 FLightManager — 绘制阴影贴图用的矩阵
 *   与片段着色器采样用的矩阵必须是同一个。让两处各自计算，快速转向时会
 *   相差一帧，表现为阴影"拖尾"。
 *
 *   级联按相机视锥的深度切片拟合，而非整个场景 — 用一张贴图覆盖整个场景时，
 *   近处的纹素密度被远处稀释：Sponza 30 单位宽的中庭挤进 2048²，脚下与
 *   三十米外分到的精度完全一样，而人眼对脚下的阴影边缘敏感得多。
 *   切成三级后，最近一级只需覆盖几米，纹素密度提高一个量级。
 *
 *   每级拟合到切片的**包围球**而非视锥角点的 AABB — 包围球在相机旋转时
 *   半径不变，正交体积因而尺寸恒定；用 AABB 的话体积随视角摆动，
 *   阴影边缘会随相机转动而闪烁 (shimmering)。这是级联阴影最经典的坑。
 *
 *   级联选择按到相机的**径向距离**而非视空间 Z — 与包围球拟合口径一致：
 *   球是按径向距离定义的，若改用平面距离选级，切片角落会选到未覆盖该处的
 *   级别，表现为视野边缘出现一圈错误阴影。
 *
 * 技术特性:
 *   - 深度专用 RenderPass，无颜色附件
 *   - 3 级级联，共用一张 2048² × 3 层的深度纹理数组
 *   - 复用 depth_only 着色器，Masked 材质在阴影中同样镂空
 *   - 单面/双面两条管线，与前向 Pass 的剔除选择保持一致
 *   - 正面剔除：绘制背面能把自遮挡推到物体内部
 *
 * 依赖关系:
 *   内部: Renderer/RenderPass/IRenderPass.h
 *
 * 注意事项:
 *   本 Pass 必须最先执行 (Order 最小) —— 前向 Pass 要采样它的产物
 *
 ******************************************************************************/

#pragma once

#include "Renderer/RenderPass/IRenderPass.h"
#include "Renderer/Recording/FParallelRecorder.h"

namespace Limx
{

// ============================================================================
// FShadowPass — 方向光阴影贴图 Pass
// ============================================================================

class FShadowPass final : public IRenderPass
{
public:
    /// 单级阴影贴图边长 — 正方形
    ///
    /// 2048 × 3 层 D32 共 48 MiB。加分辨率与加级联都能提精度, 但级联的
    /// 收益是"把精度花在该花的地方", 分辨率是均摊 —— 同样的显存,
    /// 3 级 2048 远好于 1 级 4096。
    static constexpr UInt32 kShadowMapSize = 2048;

    /// 级联层数
    ///
    /// 3 级是常见取舍: 2 级在中距离仍有明显的精度断层, 4 级起每级的
    /// 覆盖范围已经很接近, 增加的绘制开销换不回可见的质量。
    static constexpr UInt32 kCascadeCount = 3;

    FShadowPass()           = default;
    ~FShadowPass() override = default;

    // ====================================================================
    // IRenderPass 接口实现
    // ====================================================================

    LIMX_NODISCARD const AnsiChar* GetName() const override
    {
        return "ShadowPass";
    }

    /// 必须早于 DepthPrePass(100) 与 ForwardPass —— 后者要采样本 Pass 的产物
    LIMX_NODISCARD UInt32 GetOrder() const override
    {
        return 50;
    }

    ERHIResult Setup(const FPassSetupDesc& desc) override;

    /// 设置并行录制器 (可空 = 走内联路径)
    void SetRecorder(FParallelRecorder* recorder) { m_Recorder = recorder; }

    void Execute(IRHICommandBuffer*        commandBuffer,
                 const FRenderPassContext& context) override;

    ERHIResult OnResize(const FPassResizeDesc& desc) override;

    /// 释放与交换链尺寸相关的资源
    ///
    /// 阴影贴图是光源空间的固定分辨率资源, 与交换链无关, 因此这里无事可做。
    void ReleaseSwapchainResources(IRHIDevice* device) override;

    void Shutdown(IRHIDevice* device) override;

    // ====================================================================
    // 阴影贴图访问
    // ====================================================================

    /// 阴影贴图的纹理视图 — 供前向 Pass 写入 set 2 binding 1
    LIMX_NODISCARD FRHITextureViewHandle GetShadowMapView() const
    {
        return m_ShadowMapView;
    }

    /// 阴影贴图采样器 — 启用了深度比较，采样即得到 0/1 的遮挡结果
    LIMX_NODISCARD FRHISamplerHandle GetShadowSampler() const
    {
        return m_ShadowSampler;
    }

    /// 指定级联的光源视图投影矩阵
    LIMX_NODISCARD const FMatrix& GetCascadeViewProj(UInt32 cascade) const
    {
        return m_CascadeViewProj[cascade < kCascadeCount ? cascade : 0];
    }

    /// 各级联的外边界 — 到相机的径向距离
    LIMX_NODISCARD Float32 GetCascadeSplit(UInt32 cascade) const
    {
        return m_CascadeSplits[cascade < kCascadeCount ? cascade : 0];
    }

    /// 计算级联切分距离 — 纯函数, 与 GPU 无关
    ///
    /// 在对数分布与均匀分布之间按 lambda 加权:
    ///   纯对数 (lambda=1) 每级纹素密度相同, 数学上最优, 但最近一级会薄到
    ///   几十厘米, 相机稍一移动就跨级, 边界突变反而更扎眼;
    ///   纯均匀 (lambda=0) 则把太多精度浪费在远处。
    ///
    /// 输出 cascadeCount + 1 个距离: [0] 是近平面, [cascadeCount] 是最远,
    /// 中间是各级边界。必须严格递增 —— 相等或倒序会让某一级退化为空,
    /// 而着色器仍会去采样它, 采到的是未初始化的深度。
    ///
    /// @param nearPlane      相机近平面
    /// @param shadowDistance 阴影覆盖的最远距离
    /// @param cascadeCount   级数
    /// @param lambda         对数权重 [0, 1]
    /// @param outSplits      输出数组, 至少 cascadeCount + 1 个元素
    static void ComputeCascadeSplits(Float32 nearPlane,
                                     Float32 shadowDistance,
                                     UInt32 cascadeCount,
                                     Float32 lambda,
                                     Float32* outSplits);

    // ====================================================================
    // 光源与场景范围
    // ====================================================================

    /// 相机视锥参数 — 级联切分的依据
    struct FCameraFrustumInfo
    {
        FVector3 Position;
        FVector3 Forward;
        FVector3 Up;
        Float32  FovY        = 1.0f;
        Float32  AspectRatio = 1.0f;
        Float32  NearPlane   = 0.1f;

        /// 阴影覆盖的最远距离
        ///
        /// 刻意与相机远平面解耦: 远平面常设到几百米以求不裁掉天空盒,
        /// 而阴影在几十米外已无实际意义。用远平面切级会把两级浪费在
        /// 看不出阴影的距离上。
        Float32  ShadowDistance = 60.0f;
    };

    /// 设置方向光方向、相机视锥与场景包围盒 — 每帧在 Execute 之前调用
    ///
    /// 级联体积由相机视锥切片拟合; 场景包围盒只用来确定光源沿光线方向的
    /// 推移距离, 保证整个场景都落在近平面之后 —— 否则相机身后的高大物体
    /// 会被光源近平面裁掉, 它投下的阴影随之消失。
    void SetLightAndBounds(const FVector3& lightDirection,
                           const FBoundingBox& sceneBounds,
                           const FCameraFrustumInfo& cameraInfo);

    /// 是否已具备可用的光源信息
    LIMX_NODISCARD bool HasValidLight() const { return m_HasValidLight; }

private:
    /// 录制本级的公共状态 — 视口、裁剪、光源矩阵描述符集
    void RecordCascadeState(IRHICommandBuffer*        commandBuffer,
                            const FRenderPassContext& context,
                            UInt32                    cascade);

    /// 录制投射体的 [begin, end) 区间 (含本级视锥剔除)
    void RecordCasterRange(IRHICommandBuffer*           commandBuffer,
                           const FRenderPassContext&    context,
                           const FFrustum&              cascadeFrustum,
                           const TArray<FRenderObject>* casters,
                           SizeType                     begin,
                           SizeType                     end);

    /// 并行录制器 — 空则走内联路径
    FParallelRecorder* m_Recorder = nullptr;


    ERHIResult CreateShadowMap(IRHIDevice* device);
    ERHIResult CreateShadowRenderPass(IRHIDevice* device);
    ERHIResult CreateFramebuffers(IRHIDevice* device);
    ERHIResult CreateShaders(IRHIDevice* device);
    ERHIResult CreateShadowPipeline(IRHIDevice* device, bool isDoubleSided,
                                    FRHIGraphicsPipelineHandle& outPipeline);

    /// 按剔除模式取管线
    LIMX_NODISCARD FRHIGraphicsPipelineHandle SelectPipeline(
        bool isDoubleSided) const
    {
        return m_Pipelines[isDoubleSided ? 1u : 0u];
    }

    // ====================================================================
    // 成员
    // ====================================================================

    FRHITextureHandle          m_ShadowMap;

    /// 采样视图 — 覆盖全部层, 类型为 2D 数组
    FRHITextureViewHandle      m_ShadowMapView;

    /// 逐层视图 — 每级一个, 作为 Framebuffer 的深度附件
    ///
    /// 数组视图不能直接当附件用: 渲染目标必须是单层。因此采样与渲染
    /// 需要两套视图指向同一张纹理。
    FRHITextureViewHandle      m_CascadeViews[kCascadeCount];
    FRHIFramebufferHandle      m_CascadeFramebuffers[kCascadeCount];

    FRHISamplerHandle          m_ShadowSampler;

    FRHIRenderPassHandle       m_RenderPass;

    FRHIShaderHandle           m_VertShader;
    FRHIShaderHandle           m_FragShader;

    static constexpr SizeType  kPipelineVariantCount = 2;
    FRHIGraphicsPipelineHandle m_Pipelines[kPipelineVariantCount];

    FRHIPipelineLayoutHandle   m_PipelineLayout;

    FMatrix  m_CascadeViewProj[kCascadeCount];

    /// 各级外边界的径向距离
    Float32  m_CascadeSplits[kCascadeCount] = {};

    FVector3 m_LightDirection = FVector3(0.0f, -1.0f, 0.0f);
    bool     m_HasValidLight  = false;
};

} // namespace Limx
