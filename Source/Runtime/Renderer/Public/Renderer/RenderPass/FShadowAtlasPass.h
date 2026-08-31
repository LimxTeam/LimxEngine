/*******************************************************************************
 * 文件: FShadowAtlasPass.h
 * 创建时间: 2026-08-31
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   聚光灯阴影图集 Pass — 把每盏投影聚光灯的深度画进 4096² 图集的一块
 *
 * 设计哲学:
 *   一张图集而非每盏灯一张贴图 — 每盏一张的话, 描述符要么变成一个数组
 *     (于是有一个"最多几盏"的编译期上限, 且未用的槽位仍要绑有效纹理),
 *     要么每盏灯绘制前重绑一次 (于是片段着色器没法在一次绘制里处理多盏灯)。
 *     图集把这两件事都消掉了: 一个描述符, 块下标是一个整数。
 *
 *   整个图集只走**一次**渲染通道, 逐块换视口 — 每块各开一次通道也能跑,
 *     但那是 64 次 BeginRenderPass/EndRenderPass, 每次都带一次布局转换与
 *     一次 tile 内存的搬运。而清屏只需要在开头做一次: 没分出去的块保持
 *     深度 1.0, 比较采样对它恒判无遮挡, 语义上正好是"这块没有灯"。
 *
 *   光源矩阵不走 UBO, 而是**直接当 push constant 推** — 每块一次。
 *     否则每块每帧一份 UBO, 3 帧 × 64 块 = 192 个描述符集, 只为了搬同一个
 *     矩阵。视图矩阵逐块、模型矩阵逐物体, 两者的粒度不同, 所以一个在 push
 *     constant 里、一个在 set 3 的 storage buffer 里。
 *
 *     (这里曾经是另一种写法: 把阴影矩阵**预乘进 model** 再推 push constant,
 *      而 set 0 的 view/proj 填单位阵。那是逐物体数据还在 push constant 里
 *      时的权宜之计; GPU 驱动把 model 搬进 storage buffer 之后, push constant
 *      腾出来的 64 字节正好装视图矩阵, 权宜之计也就不需要了。)
 *
 *   块的分配不在这里做, 在 FLightManager 打包光源时做 — 分块下标必须同时
 *     写进光源数据 (给片段着色器) 与阴影数据 (给本 Pass), 两处各分一次
 *     必然有漂移的可能, 而漂移的表现是"这盏灯采到了别人的阴影"。
 *
 * 技术特性:
 *   - 4096² D32 深度图集, 切成 8×8 共 64 块, 每块 512²
 *   - 单次渲染通道, 逐块 SetViewport/SetScissor
 *   - 复用 depth_only 着色器与 FShadowPass 的两条剔除变体
 *   - 每块按该灯的视锥再剔一次投射体
 *
 * 依赖关系:
 *   内部: Renderer/RenderPass/IRenderPass.h,
 *         RenderCore/Lighting/FShadowAtlas.h
 *
 * 注意事项:
 *   Order 必须早于 ForwardPass —— 后者要采样本 Pass 的产物
 *
 ******************************************************************************/

#pragma once

#include "Renderer/RenderPass/IRenderPass.h"

#include "RenderCore/Lighting/FShadowAtlas.h"

namespace Limx
{

// ============================================================================
// FShadowAtlasPass — 聚光灯阴影图集
// ============================================================================

class FShadowAtlasPass final : public IRenderPass
{
public:
    FShadowAtlasPass()           = default;
    ~FShadowAtlasPass() override = default;

    // ====================================================================
    // IRenderPass 接口实现
    // ====================================================================

    LIMX_NODISCARD const AnsiChar* GetName() const override
    {
        return "ShadowAtlasPass";
    }

    /// 紧跟方向光阴影 (50), 早于 DepthPrePass (100)
    ///
    /// 与方向光阴影分成两个 Pass 而非合并: 两者的投影类型、贴图布局与剔除
    /// 依据都不同, 合并之后每个循环里都要判"这是级联还是图集"。
    LIMX_NODISCARD UInt32 GetOrder() const override
    {
        return 60;
    }

    ERHIResult Setup(const FPassSetupDesc& desc) override;

    void Execute(IRHICommandBuffer*        commandBuffer,
                 const FRenderPassContext& context) override;

    /// 图集是光源空间的固定分辨率资源, 与交换链无关
    ERHIResult OnResize(const FPassResizeDesc& desc) override;

    void ReleaseSwapchainResources(IRHIDevice* device) override;

    void Shutdown(IRHIDevice* device) override;

    // ====================================================================
    // 图集访问
    // ====================================================================

    /// 图集纹理视图 — 供前向 Pass 写入 set 2 binding 10
    LIMX_NODISCARD FRHITextureViewHandle GetAtlasView() const
    {
        return m_AtlasView;
    }

    /// 图集采样器 — 启用了深度比较
    LIMX_NODISCARD FRHISamplerHandle GetAtlasSampler() const
    {
        return m_AtlasSampler;
    }

    /// 上一帧实际绘制的块数 —— 自检与统计用
    LIMX_NODISCARD UInt32 GetRenderedTileCount() const
    {
        return m_RenderedTileCount;
    }

private:
    ERHIResult CreateAtlas(IRHIDevice* device);
    ERHIResult CreateAtlasRenderPass(IRHIDevice* device);
    ERHIResult CreateFramebuffer(IRHIDevice* device);
    ERHIResult CreateShaders(IRHIDevice* device);
    ERHIResult CreatePipeline(IRHIDevice* device, bool isDoubleSided,
                              FRHIGraphicsPipelineHandle& outPipeline);

    /// 按剔除模式取管线
    LIMX_NODISCARD FRHIGraphicsPipelineHandle SelectPipeline(
        bool isDoubleSided) const
    {
        return m_Pipelines[isDoubleSided ? 1u : 0u];
    }

    /// 绘制一块 — 设视口、按该灯的视锥剔除、逐投射体绘制
    void RecordTile(IRHICommandBuffer*        commandBuffer,
                    const FRenderPassContext& context,
                    const FSpotShadowData&    shadowData,
                    UInt32                    tileIndex);

    // ====================================================================
    // 成员
    // ====================================================================

    FRHITextureHandle          m_Atlas;
    FRHITextureViewHandle      m_AtlasView;
    FRHIFramebufferHandle      m_Framebuffer;
    FRHISamplerHandle          m_AtlasSampler;

    FRHIRenderPassHandle       m_RenderPass;

    FRHIShaderHandle           m_VertShader;
    FRHIShaderHandle           m_FragShader;

    static constexpr SizeType  kPipelineVariantCount = 2;
    FRHIGraphicsPipelineHandle m_Pipelines[kPipelineVariantCount];

    FRHIPipelineLayoutHandle   m_PipelineLayout;

    UInt32                     m_RenderedTileCount = 0;
};

} // namespace Limx
