/*******************************************************************************
 * 文件名称：RHIPipelineState.h
 * 创建时间：2025-07-27
 * 创建者  ：LimxTeam
 * 设计哲学：描述符驱动的管线状态配置，所有图形管线固定功能阶段以 POD 结构体
 *          表达，支持编译时默认值，减少创建时的样板代码。计算管线保持极简。
 * 功能描述：图形管线和计算管线的完整创建描述符，涵盖顶点输入、输入装配、
 *          光栅化、多重采样、深度模板、颜色混合、动态状态等所有固定功能阶段，
 *          以及着色器阶段绑定。
 * 技术特性：所有描述符为纯 POD 聚合体，可值传递，可安全 memcpy。
 *          提供合理的默认值，大多数场景只需覆盖少量字段即可创建管线。
 *
 * ── 结构体表 ────────────────────────────────────────────────
 * │ 结构体名                        │ 描述                     │
 * │────────────────────────────────│────────────────────────│
 * │ FRHIShaderStageDesc            │ 着色器阶段绑定            │
 * │ FRHIVertexInputStateDesc       │ 顶点输入状态              │
 * │ FRHIInputAssemblyStateDesc     │ 输入装配状态              │
 * │ FRHIRasterizationStateDesc     │ 光栅化状态               │
 * │ FRHIMultisampleStateDesc       │ 多重采样状态              │
 * │ FRHIDepthStencilStateDesc      │ 深度模板状态              │
 * │ FRHIColorBlendAttachmentDesc   │ 单个颜色附件混合状态       │
 * │ FRHIColorBlendStateDesc        │ 颜色混合全局状态           │
 * │ FRHIDynamicStateDesc           │ 动态状态配置              │
 * │ FRHIGraphicsPipelineDesc       │ 图形管线完整创建描述符      │
 * │ FRHIComputePipelineDesc        │ 计算管线创建描述符         │
 *
 * ── 更新历史 ────────────────────────────────────────────────
 * │ 日期         │ 作者       │ 描述                     │
 * │─────────────│──────────│────────────────────────│
 * │ 2025-07-27  │ LimxTeam  │ 初始创建                  │
 * ============================================================
 ******************************************************************************/

#pragma once

#include "RHI/RHI/RHIResources.h"

namespace Limx
{

// ============================================================================
// FRHIShaderStageDesc — 着色器阶段绑定
// ============================================================================

struct FRHIShaderStageDesc
{
    // 着色器阶段
    EShaderStage Stage = EShaderStage::None;

    // 着色器模块句柄
    FRHIShaderHandle Shader;

    // 入口函数名
    const char* EntryPoint = "main";

    // 特化常量数据 (可选)
    const void* SpecializationData    = nullptr;
    UInt32      SpecializationDataSize = 0;
};

// ============================================================================
// FRHIVertexInputStateDesc — 顶点输入状态
// ============================================================================

struct FRHIVertexInputStateDesc
{
    // 顶点输入绑定描述数组
    const FRHIVertexInputBinding* Bindings    = nullptr;
    UInt32                        BindingCount = 0;

    // 顶点输入属性描述数组
    const FRHIVertexInputAttribute* Attributes    = nullptr;
    UInt32                          AttributeCount = 0;
};

// ============================================================================
// FRHIInputAssemblyStateDesc — 输入装配状态
// ============================================================================

struct FRHIInputAssemblyStateDesc
{
    // 图元拓扑
    EPrimitiveTopology Topology = EPrimitiveTopology::TriangleList;

    // 是否启用图元重启 (Strip 拓扑使用特殊索引值切断条带)
    bool IsPrimitiveRestartEnabled = false;
};

// ============================================================================
// FRHIRasterizationStateDesc — 光栅化状态
// ============================================================================

struct FRHIRasterizationStateDesc
{
    // 多边形填充模式
    EPolygonMode PolygonMode = EPolygonMode::Fill;

    // 面剔除模式
    ECullMode CullMode = ECullMode::Back;

    // 正面环绕方向
    EFrontFace FrontFace = EFrontFace::CounterClockwise;

    // 深度偏移 (用于消除 Z-fighting)
    bool    IsDepthBiasEnabled      = false;
    Float32 DepthBiasConstantFactor = 0.0f;
    Float32 DepthBiasClamp          = 0.0f;
    Float32 DepthBiasSlopeFactor    = 0.0f;

    // 线宽 (非 1.0 需要 wideLines 特性)
    Float32 LineWidth = 1.0f;

    // 是否启用深度钳位 (将超出近远平面的片段钳位而非裁剪)
    bool IsDepthClampEnabled = false;

    // 是否丢弃所有光栅化输出 (用于仅需顶点/几何处理的 Pass)
    bool IsRasterizerDiscardEnabled = false;
};

// ============================================================================
// FRHIMultisampleStateDesc — 多重采样状态
// ============================================================================

struct FRHIMultisampleStateDesc
{
    // 采样数
    ESampleCount RasterizationSamples = ESampleCount::Count1;

    // 是否启用逐采样着色 (每个采样点独立运行片段着色器)
    bool IsSampleShadingEnabled = false;

    // 最小采样着色比例 [0.0, 1.0]
    Float32 MinSampleShading = 1.0f;

    // 采样掩码 (nullptr 表示全部启用)
    const UInt32* SampleMask = nullptr;

    // Alpha 覆盖
    bool IsAlphaToCoverageEnabled = false;
    bool IsAlphaToOneEnabled      = false;
};

// ============================================================================
// FRHIStencilOpState — 模板操作状态 (单面)
// ============================================================================

struct FRHIStencilOpState
{
    EStencilOp FailOp      = EStencilOp::Keep;
    EStencilOp PassOp      = EStencilOp::Keep;
    EStencilOp DepthFailOp = EStencilOp::Keep;
    ECompareOp CompareOp   = ECompareOp::Always;
    UInt32     CompareMask  = 0xFF;
    UInt32     WriteMask    = 0xFF;
    UInt32     Reference    = 0;
};

// ============================================================================
// FRHIDepthStencilStateDesc — 深度模板状态
// ============================================================================

struct FRHIDepthStencilStateDesc
{
    // 深度测试
    bool       IsDepthTestEnabled  = true;
    bool       IsDepthWriteEnabled = true;
    ECompareOp DepthCompareOp      = ECompareOp::Less;

    // 深度范围测试
    bool    IsDepthBoundsTestEnabled = false;
    Float32 MinDepthBounds           = 0.0f;
    Float32 MaxDepthBounds           = 1.0f;

    // 模板测试
    bool             IsStencilTestEnabled = false;
    FRHIStencilOpState Front;
    FRHIStencilOpState Back;

    // 便捷工厂: 禁用深度测试
    static FRHIDepthStencilStateDesc Disabled()
    {
        FRHIDepthStencilStateDesc desc;
        desc.IsDepthTestEnabled = false;
        desc.IsDepthWriteEnabled = false;
        return desc;
    }

    // 便捷工厂: 只读深度测试 (不写入深度缓冲)
    static FRHIDepthStencilStateDesc ReadOnly()
    {
        FRHIDepthStencilStateDesc desc;
        desc.IsDepthTestEnabled = true;
        desc.IsDepthWriteEnabled = false;
        desc.DepthCompareOp = ECompareOp::LessOrEqual;
        return desc;
    }

    // 便捷工厂: 反向深度 (1.0=近, 0.0=远, 提高精度)
    static FRHIDepthStencilStateDesc ReverseZ()
    {
        FRHIDepthStencilStateDesc desc;
        desc.IsDepthTestEnabled = true;
        desc.IsDepthWriteEnabled = true;
        desc.DepthCompareOp = ECompareOp::Greater;
        return desc;
    }
};

// ============================================================================
// FRHIColorBlendAttachmentDesc — 单个颜色附件混合状态
// ============================================================================

struct FRHIColorBlendAttachmentDesc
{
    // 是否启用混合
    bool IsBlendEnabled = false;

    // RGB 混合
    EBlendFactor SrcColorBlendFactor = EBlendFactor::One;
    EBlendFactor DstColorBlendFactor = EBlendFactor::Zero;
    EBlendOp     ColorBlendOp        = EBlendOp::Add;

    // Alpha 混合
    EBlendFactor SrcAlphaBlendFactor = EBlendFactor::One;
    EBlendFactor DstAlphaBlendFactor = EBlendFactor::Zero;
    EBlendOp     AlphaBlendOp        = EBlendOp::Add;

    // 颜色写入掩码
    EColorWriteMask ColorWriteMask = EColorWriteMask::All;

    // 便捷工厂: 不混合 (直接覆盖)
    static FRHIColorBlendAttachmentDesc Opaque()
    {
        FRHIColorBlendAttachmentDesc desc;
        return desc;
    }

    // 便捷工厂: Alpha 混合
    static FRHIColorBlendAttachmentDesc AlphaBlend()
    {
        FRHIColorBlendAttachmentDesc desc;
        desc.IsBlendEnabled = true;
        desc.SrcColorBlendFactor = EBlendFactor::SrcAlpha;
        desc.DstColorBlendFactor = EBlendFactor::OneMinusSrcAlpha;
        desc.ColorBlendOp = EBlendOp::Add;
        desc.SrcAlphaBlendFactor = EBlendFactor::One;
        desc.DstAlphaBlendFactor = EBlendFactor::OneMinusSrcAlpha;
        desc.AlphaBlendOp = EBlendOp::Add;
        return desc;
    }

    // 便捷工厂: 加法混合
    static FRHIColorBlendAttachmentDesc Additive()
    {
        FRHIColorBlendAttachmentDesc desc;
        desc.IsBlendEnabled = true;
        desc.SrcColorBlendFactor = EBlendFactor::One;
        desc.DstColorBlendFactor = EBlendFactor::One;
        desc.ColorBlendOp = EBlendOp::Add;
        desc.SrcAlphaBlendFactor = EBlendFactor::One;
        desc.DstAlphaBlendFactor = EBlendFactor::One;
        desc.AlphaBlendOp = EBlendOp::Add;
        return desc;
    }

    // 便捷工厂: 预乘 Alpha 混合
    static FRHIColorBlendAttachmentDesc PremultipliedAlpha()
    {
        FRHIColorBlendAttachmentDesc desc;
        desc.IsBlendEnabled = true;
        desc.SrcColorBlendFactor = EBlendFactor::One;
        desc.DstColorBlendFactor = EBlendFactor::OneMinusSrcAlpha;
        desc.ColorBlendOp = EBlendOp::Add;
        desc.SrcAlphaBlendFactor = EBlendFactor::One;
        desc.DstAlphaBlendFactor = EBlendFactor::OneMinusSrcAlpha;
        desc.AlphaBlendOp = EBlendOp::Add;
        return desc;
    }
};

// ============================================================================
// FRHIColorBlendStateDesc — 颜色混合全局状态
// ============================================================================

struct FRHIColorBlendStateDesc
{
    // 是否启用逻辑运算 (与混合互斥)
    bool IsLogicOpEnabled = false;

    // 逻辑运算类型 (0=Clear, 1=And, 2=AndReverse, 3=Copy, ...)
    UInt32 LogicOp = 3;

    // 每个颜色附件的混合状态
    const FRHIColorBlendAttachmentDesc* Attachments    = nullptr;
    UInt32                              AttachmentCount = 0;

    // 混合常量 (EBlendFactor::ConstantColor 使用)
    Float32 BlendConstants[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
};

// ============================================================================
// FRHIDynamicStateDesc — 动态状态配置
// ============================================================================

struct FRHIDynamicStateDesc
{
    // 启用的动态状态位掩码
    EDynamicState EnabledStates = EDynamicState::Viewport | EDynamicState::Scissor;
};

// ============================================================================
// FRHIGraphicsPipelineDesc — 图形管线完整创建描述符
// ============================================================================

static constexpr UInt32 kMaxShaderStages = 6;

struct FRHIGraphicsPipelineDesc
{
    // 着色器阶段 (至少需要 Vertex + Fragment)
    FRHIShaderStageDesc ShaderStages[kMaxShaderStages];
    UInt32              ShaderStageCount = 0;

    // 顶点输入
    FRHIVertexInputStateDesc VertexInput;

    // 输入装配
    FRHIInputAssemblyStateDesc InputAssembly;

    // 光栅化
    FRHIRasterizationStateDesc Rasterization;

    // 多重采样
    FRHIMultisampleStateDesc Multisample;

    // 深度模板
    FRHIDepthStencilStateDesc DepthStencil;

    // 颜色混合
    FRHIColorBlendStateDesc ColorBlend;

    // 动态状态
    FRHIDynamicStateDesc DynamicState;

    // 管线布局 (描述符集 + Push Constants)
    FRHIPipelineLayoutHandle PipelineLayout;

    // 渲染通道
    FRHIRenderPassHandle RenderPass;

    // 子通道索引
    UInt32 SubpassIndex = 0;

    // 调试名称
    const char* DebugName = nullptr;
};

// ============================================================================
// FRHIComputePipelineDesc — 计算管线创建描述符
// ============================================================================

struct FRHIComputePipelineDesc
{
    // 计算着色器
    FRHIShaderStageDesc ComputeShader;

    // 管线布局
    FRHIPipelineLayoutHandle PipelineLayout;

    // 调试名称
    const char* DebugName = nullptr;
};

} // namespace Limx
