/*******************************************************************************
 * 文件名称：RHIDefinitions.h
 * 创建时间：2025-07-27
 * 创建者  ：LimxTeam
 * 设计哲学：零开销枚举抽象，所有 RHI 枚举值与 Vulkan 后端一一对应，
 *          公开接口不暴露任何 Vulkan 头文件，保持后端可替换性。
 *          枚举底层类型显式指定，保证 ABI 稳定与内存布局可预测。
 * 功能描述：渲染硬件接口核心枚举与基础结构体定义，涵盖像素格式、
 *          图元拓扑、混合因子、比较运算、着色器阶段、资源用途、
 *          采样器参数、描述符类型等 GPU 管线全部配置维度。
 * 技术特性：所有枚举使用 enum class + 显式底层类型 (UInt8/UInt16/UInt32)，
 *          基础结构体使用 POD 布局保证跨编译单元二进制兼容。
 *
 * ── 枚举表 ──────────────────────────────────────────────────
 * │ 枚举名                     │ 描述                          │
 * │───────────────────────────│──────────────────────────────│
 * │ EPixelFormat               │ GPU 像素/纹理格式              │
 * │ EPrimitiveTopology         │ 图元装配拓扑                   │
 * │ EBlendFactor               │ 混合因子                      │
 * │ EBlendOp                   │ 混合运算                      │
 * │ ECompareOp                 │ 深度/模板比较运算               │
 * │ EStencilOp                 │ 模板运算                      │
 * │ ECullMode                  │ 面剔除模式                     │
 * │ EFrontFace                 │ 正面环绕方向                   │
 * │ EPolygonMode               │ 多边形填充模式                  │
 * │ EShaderStage               │ 着色器阶段 (位掩码)             │
 * │ EBufferUsage               │ 缓冲区用途 (位掩码)             │
 * │ ETextureUsage              │ 纹理用途 (位掩码)               │
 * │ ETextureType               │ 纹理维度类型                   │
 * │ EFilter                    │ 纹理过滤模式                   │
 * │ ESamplerAddressMode        │ 采样器寻址模式                  │
 * │ ESamplerMipmapMode         │ Mipmap 过滤模式                │
 * │ ELoadOp                    │ 附件加载操作                   │
 * │ EStoreOp                   │ 附件存储操作                   │
 * │ EImageLayout               │ 图像布局                      │
 * │ EQueueType                 │ GPU 队列族类型                 │
 * │ EMemoryUsage               │ 内存分配策略                   │
 * │ EIndexType                 │ 索引缓冲区元素类型              │
 * │ EVertexInputRate           │ 顶点输入步进率                  │
 * │ ESampleCount               │ 多重采样数                     │
 * │ EColorWriteMask            │ 颜色写入掩码 (位掩码)            │
 * │ EDescriptorType            │ 描述符绑定类型                  │
 * │ EPipelineBindPoint         │ 管线绑定点                     │
 * │ EDynamicState              │ 动态管线状态 (位掩码)            │
 *
 * ── 结构体表 ────────────────────────────────────────────────
 * │ 结构体名                   │ 描述                          │
 * │───────────────────────────│──────────────────────────────│
 * │ FRHIViewport               │ 视口矩形 (x/y/w/h/minZ/maxZ) │
 * │ FRHIScissorRect            │ 裁剪矩形 (x/y/w/h)            │
 * │ FRHIClearValue             │ 清除值 (颜色/深度/模板)          │
 * │ FRHIExtent2D               │ 2D 尺寸                       │
 * │ FRHIExtent3D               │ 3D 尺寸                       │
 * │ FRHIOffset2D               │ 2D 偏移                       │
 * │ FRHIOffset3D               │ 3D 偏移                       │
 *
 * ── 更新历史 ────────────────────────────────────────────────
 * │ 日期         │ 作者       │ 描述                          │
 * │─────────────│──────────│──────────────────────────────│
 * │ 2025-07-27  │ LimxTeam  │ 初始创建                       │
 * ============================================================
 ******************************************************************************/

#pragma once

#include "Core/CoreTypes.h"

namespace Limx
{

// ============================================================================
// EPixelFormat — GPU 像素/纹理格式
// ============================================================================

enum class EPixelFormat : UInt16
{
    Unknown = 0,

    // ---- 8 位单通道 ----
    R8_UNORM,
    R8_SNORM,
    R8_UINT,
    R8_SINT,

    // ---- 8 位双通道 ----
    RG8_UNORM,
    RG8_SNORM,
    RG8_UINT,
    RG8_SINT,

    // ---- 8 位四通道 ----
    RGBA8_UNORM,
    RGBA8_SNORM,
    RGBA8_UINT,
    RGBA8_SINT,
    RGBA8_SRGB,

    // ---- 8 位四通道 (BGRA 序) ----
    BGRA8_UNORM,
    BGRA8_SRGB,

    // ---- 16 位单通道 ----
    R16_UNORM,
    R16_SNORM,
    R16_UINT,
    R16_SINT,
    R16_SFLOAT,

    // ---- 16 位双通道 ----
    RG16_UNORM,
    RG16_SNORM,
    RG16_UINT,
    RG16_SINT,
    RG16_SFLOAT,

    // ---- 16 位四通道 ----
    RGBA16_UNORM,
    RGBA16_SNORM,
    RGBA16_UINT,
    RGBA16_SINT,
    RGBA16_SFLOAT,

    // ---- 32 位单通道 ----
    R32_UINT,
    R32_SINT,
    R32_SFLOAT,

    // ---- 32 位双通道 ----
    RG32_UINT,
    RG32_SINT,
    RG32_SFLOAT,

    // ---- 32 位三通道 ----
    RGB32_UINT,
    RGB32_SINT,
    RGB32_SFLOAT,

    // ---- 32 位四通道 ----
    RGBA32_UINT,
    RGBA32_SINT,
    RGBA32_SFLOAT,

    // ---- 64 位单通道 (光谱渲染精度) ----
    R64_SFLOAT,

    // ---- 打包格式 ----
    B10G11R11_UFLOAT_PACK32,
    E5B9G9R9_UFLOAT_PACK32,
    A2R10G10B10_UNORM_PACK32,
    A2B10G10R10_UNORM_PACK32,

    // ---- 深度/模板格式 ----
    D16_UNORM,
    D32_SFLOAT,
    D24_UNORM_S8_UINT,
    D32_SFLOAT_S8_UINT,
    S8_UINT,

    // ---- BC 压缩格式 ----
    BC1_UNORM,
    BC1_SRGB,
    BC2_UNORM,
    BC2_SRGB,
    BC3_UNORM,
    BC3_SRGB,
    BC4_UNORM,
    BC4_SNORM,
    BC5_UNORM,
    BC5_SNORM,
    BC6H_UFLOAT,
    BC6H_SFLOAT,
    BC7_UNORM,
    BC7_SRGB,

    Count
};

// ============================================================================
// EPrimitiveTopology — 图元装配拓扑
// ============================================================================

enum class EPrimitiveTopology : UInt8
{
    PointList = 0,
    LineList,
    LineStrip,
    TriangleList,
    TriangleStrip,
    TriangleFan,
    LineListWithAdjacency,
    LineStripWithAdjacency,
    TriangleListWithAdjacency,
    TriangleStripWithAdjacency,
    PatchList,

    Count
};

// ============================================================================
// EBlendFactor — 混合因子
// ============================================================================

enum class EBlendFactor : UInt8
{
    Zero = 0,
    One,
    SrcColor,
    OneMinusSrcColor,
    DstColor,
    OneMinusDstColor,
    SrcAlpha,
    OneMinusSrcAlpha,
    DstAlpha,
    OneMinusDstAlpha,
    ConstantColor,
    OneMinusConstantColor,
    ConstantAlpha,
    OneMinusConstantAlpha,
    SrcAlphaSaturate,
    Src1Color,
    OneMinusSrc1Color,
    Src1Alpha,
    OneMinusSrc1Alpha,

    Count
};

// ============================================================================
// EBlendOp — 混合运算
// ============================================================================

enum class EBlendOp : UInt8
{
    Add = 0,
    Subtract,
    ReverseSubtract,
    Min,
    Max,

    Count
};

// ============================================================================
// ECompareOp — 深度/模板比较运算
// ============================================================================

enum class ECompareOp : UInt8
{
    Never = 0,
    Less,
    Equal,
    LessOrEqual,
    Greater,
    NotEqual,
    GreaterOrEqual,
    Always,

    Count
};

// ============================================================================
// EStencilOp — 模板运算
// ============================================================================

enum class EStencilOp : UInt8
{
    Keep = 0,
    Zero,
    Replace,
    IncrementAndClamp,
    DecrementAndClamp,
    Invert,
    IncrementAndWrap,
    DecrementAndWrap,

    Count
};

// ============================================================================
// ECullMode — 面剔除模式
// ============================================================================

enum class ECullMode : UInt8
{
    None = 0,
    Front,
    Back,
    FrontAndBack,

    Count
};

// ============================================================================
// EFrontFace — 正面环绕方向
// ============================================================================

enum class EFrontFace : UInt8
{
    CounterClockwise = 0,
    Clockwise,

    Count
};

// ============================================================================
// EPolygonMode — 多边形填充模式
// ============================================================================

enum class EPolygonMode : UInt8
{
    Fill = 0,
    Line,
    Point,

    Count
};

// ============================================================================
// EShaderStage — 着色器阶段 (位掩码，可组合)
// ============================================================================

enum class EShaderStage : UInt32
{
    None                = 0,
    Vertex              = 1 << 0,
    TessellationControl = 1 << 1,
    TessellationEval    = 1 << 2,
    Geometry            = 1 << 3,
    Fragment            = 1 << 4,
    Compute             = 1 << 5,
    Task                = 1 << 6,
    Mesh                = 1 << 7,
    RayGeneration       = 1 << 8,
    RayAnyHit           = 1 << 9,
    RayClosestHit       = 1 << 10,
    RayMiss             = 1 << 11,
    RayIntersection     = 1 << 12,
    Callable            = 1 << 13,

    AllGraphics         = Vertex | TessellationControl | TessellationEval
                        | Geometry | Fragment,
    AllMeshShading      = Task | Mesh | Fragment,
    AllRayTracing       = RayGeneration | RayAnyHit | RayClosestHit
                        | RayMiss | RayIntersection | Callable,
    All                 = 0x3FFF
};

LIMX_DEFINE_ENUM_BITWISE_OPS(EShaderStage)

// ============================================================================
// EBufferUsage — 缓冲区用途 (位掩码，可组合)
// ============================================================================

enum class EBufferUsage : UInt32
{
    None            = 0,
    VertexBuffer    = 1 << 0,
    IndexBuffer     = 1 << 1,
    UniformBuffer   = 1 << 2,
    StorageBuffer   = 1 << 3,
    IndirectBuffer  = 1 << 4,
    TransferSrc     = 1 << 5,
    TransferDst     = 1 << 6,
    UniformTexel    = 1 << 7,
    StorageTexel    = 1 << 8,
    AccelStructBuild       = 1 << 9,
    AccelStructStorage     = 1 << 10,
    ShaderBindingTable     = 1 << 11,
    ShaderDeviceAddress    = 1 << 12,
};

LIMX_DEFINE_ENUM_BITWISE_OPS(EBufferUsage)

// ============================================================================
// ETextureUsage — 纹理用途 (位掩码，可组合)
// ============================================================================

enum class ETextureUsage : UInt32
{
    None                    = 0,
    Sampled                 = 1 << 0,
    Storage                 = 1 << 1,
    ColorAttachment         = 1 << 2,
    DepthStencilAttachment  = 1 << 3,
    TransientAttachment     = 1 << 4,
    InputAttachment         = 1 << 5,
    TransferSrc             = 1 << 6,
    TransferDst             = 1 << 7,
    ShadingRate             = 1 << 8,
};

LIMX_DEFINE_ENUM_BITWISE_OPS(ETextureUsage)

// ============================================================================
// ETextureType — 纹理维度类型
// ============================================================================

enum class ETextureType : UInt8
{
    Texture1D = 0,
    Texture2D,
    Texture3D,
    TextureCube,
    Texture1DArray,
    Texture2DArray,
    TextureCubeArray,

    Count
};

// ============================================================================
// EFilter — 纹理过滤模式
// ============================================================================

enum class EFilter : UInt8
{
    Nearest = 0,
    Linear,

    Count
};

// ============================================================================
// ESamplerAddressMode — 采样器寻址模式
// ============================================================================

enum class ESamplerAddressMode : UInt8
{
    Repeat = 0,
    MirroredRepeat,
    ClampToEdge,
    ClampToBorder,
    MirrorClampToEdge,

    Count
};

// ============================================================================
// ESamplerMipmapMode — Mipmap 过滤模式
// ============================================================================

enum class ESamplerMipmapMode : UInt8
{
    Nearest = 0,
    Linear,

    Count
};

// ============================================================================
// ELoadOp — 渲染通道附件加载操作
// ============================================================================

enum class ELoadOp : UInt8
{
    Load = 0,
    Clear,
    DontCare,

    Count
};

// ============================================================================
// EStoreOp — 渲染通道附件存储操作
// ============================================================================

enum class EStoreOp : UInt8
{
    Store = 0,
    DontCare,

    Count
};

// ============================================================================
// EImageLayout — 图像内存布局
// ============================================================================

enum class EImageLayout : UInt8
{
    Undefined = 0,
    General,
    ColorAttachment,
    DepthStencilAttachment,
    DepthStencilReadOnly,
    ShaderReadOnly,
    TransferSrc,
    TransferDst,
    Preinitialized,
    PresentSrc,
    DepthReadOnlyStencilAttachment,
    DepthAttachmentStencilReadOnly,
    DepthAttachment,
    DepthReadOnly,
    StencilAttachment,
    StencilReadOnly,
    ReadOnly,
    AttachmentOptimal,

    Count
};

// ============================================================================
// EQueueType — GPU 队列族类型
// ============================================================================

enum class EQueueType : UInt8
{
    Graphics = 0,
    Compute,
    Transfer,
    VideoDecode,
    VideoEncode,

    Count
};

// ============================================================================
// EMemoryUsage — 内存分配策略
// ============================================================================

enum class EMemoryUsage : UInt8
{
    GpuOnly = 0,
    CpuToGpu,
    GpuToCpu,
    CpuOnly,

    Count
};

// ============================================================================
// EIndexType — 索引缓冲区元素类型
// ============================================================================

enum class EIndexType : UInt8
{
    UInt16 = 0,
    UInt32,

    Count
};

// ============================================================================
// EVertexInputRate — 顶点输入步进率
// ============================================================================

enum class EVertexInputRate : UInt8
{
    PerVertex = 0,
    PerInstance,

    Count
};

// ============================================================================
// ESampleCount — 多重采样计数
// ============================================================================

enum class ESampleCount : UInt8
{
    Count1  = 1,
    Count2  = 2,
    Count4  = 4,
    Count8  = 8,
    Count16 = 16,
    Count32 = 32,
    Count64 = 64,
};

// ============================================================================
// EColorWriteMask — 颜色写入掩码 (位掩码)
// ============================================================================

enum class EColorWriteMask : UInt8
{
    None = 0,
    R    = 1 << 0,
    G    = 1 << 1,
    B    = 1 << 2,
    A    = 1 << 3,
    All  = R | G | B | A,
};

LIMX_DEFINE_ENUM_BITWISE_OPS(EColorWriteMask)

// ============================================================================
// EDescriptorType — 描述符绑定类型
// ============================================================================

enum class EDescriptorType : UInt8
{
    Sampler = 0,
    CombinedImageSampler,
    SampledImage,
    StorageImage,
    UniformTexelBuffer,
    StorageTexelBuffer,
    UniformBuffer,
    StorageBuffer,
    UniformBufferDynamic,
    StorageBufferDynamic,
    InputAttachment,
    AccelerationStructure,

    Count
};

// ============================================================================
// EPipelineBindPoint — 管线绑定点
// ============================================================================

enum class EPipelineBindPoint : UInt8
{
    Graphics = 0,
    Compute,
    RayTracing,

    Count
};

// ============================================================================
// EDynamicState — 动态管线状态 (位掩码)
// ============================================================================

enum class EDynamicState : UInt32
{
    None                = 0,
    Viewport            = 1 << 0,
    Scissor             = 1 << 1,
    LineWidth           = 1 << 2,
    DepthBias           = 1 << 3,
    BlendConstants      = 1 << 4,
    DepthBounds         = 1 << 5,
    StencilCompareMask  = 1 << 6,
    StencilWriteMask    = 1 << 7,
    StencilReference    = 1 << 8,
    CullMode            = 1 << 9,
    FrontFace           = 1 << 10,
    PrimitiveTopology   = 1 << 11,
    VertexInputBinding  = 1 << 12,
    DepthTestEnable     = 1 << 13,
    DepthWriteEnable    = 1 << 14,
    DepthCompareOp      = 1 << 15,
    StencilTestEnable   = 1 << 16,
    StencilOp           = 1 << 17,
    RasterizerDiscard   = 1 << 18,
    DepthBiasEnable     = 1 << 19,
    PrimitiveRestart    = 1 << 20,
};

LIMX_DEFINE_ENUM_BITWISE_OPS(EDynamicState)

// ============================================================================
// EAccessFlags — 内存访问标志 (位掩码，用于管线屏障)
// ============================================================================

enum class EAccessFlags : UInt32
{
    None                        = 0,
    IndirectCommandRead         = 1 << 0,
    IndexRead                   = 1 << 1,
    VertexAttributeRead         = 1 << 2,
    UniformRead                 = 1 << 3,
    InputAttachmentRead         = 1 << 4,
    ShaderRead                  = 1 << 5,
    ShaderWrite                 = 1 << 6,
    ColorAttachmentRead         = 1 << 7,
    ColorAttachmentWrite        = 1 << 8,
    DepthStencilAttachmentRead  = 1 << 9,
    DepthStencilAttachmentWrite = 1 << 10,
    TransferRead                = 1 << 11,
    TransferWrite               = 1 << 12,
    HostRead                    = 1 << 13,
    HostWrite                   = 1 << 14,
    MemoryRead                  = 1 << 15,
    MemoryWrite                 = 1 << 16,
    AccelStructRead             = 1 << 17,
    AccelStructWrite            = 1 << 18,
};

LIMX_DEFINE_ENUM_BITWISE_OPS(EAccessFlags)

// ============================================================================
// EPipelineStageFlags — 管线阶段标志 (位掩码，用于同步)
// ============================================================================

enum class EPipelineStageFlags : UInt32
{
    None                    = 0,
    TopOfPipe               = 1 << 0,
    DrawIndirect            = 1 << 1,
    VertexInput             = 1 << 2,
    VertexShader            = 1 << 3,
    TessControlShader       = 1 << 4,
    TessEvalShader          = 1 << 5,
    GeometryShader          = 1 << 6,
    FragmentShader          = 1 << 7,
    EarlyFragmentTests      = 1 << 8,
    LateFragmentTests       = 1 << 9,
    ColorAttachmentOutput   = 1 << 10,
    ComputeShader           = 1 << 11,
    Transfer                = 1 << 12,
    BottomOfPipe            = 1 << 13,
    Host                    = 1 << 14,
    AllGraphics             = 1 << 15,
    AllCommands             = 1 << 16,
    AccelStructBuild        = 1 << 17,
    RayTracingShader        = 1 << 18,
    TaskShader              = 1 << 19,
    MeshShader              = 1 << 20,
};

LIMX_DEFINE_ENUM_BITWISE_OPS(EPipelineStageFlags)

// ============================================================================
// 基础结构体 — POD 布局，跨编译单元二进制兼容
// ============================================================================

// 2D 尺寸
struct FRHIExtent2D
{
    UInt32 Width  = 0;
    UInt32 Height = 0;
};

// 3D 尺寸
struct FRHIExtent3D
{
    UInt32 Width  = 0;
    UInt32 Height = 0;
    UInt32 Depth  = 1;
};

// 2D 偏移
struct FRHIOffset2D
{
    Int32 X = 0;
    Int32 Y = 0;
};

// 3D 偏移
struct FRHIOffset3D
{
    Int32 X = 0;
    Int32 Y = 0;
    Int32 Z = 0;
};

// 视口矩形
struct FRHIViewport
{
    Float32 X        = 0.0f;
    Float32 Y        = 0.0f;
    Float32 Width    = 0.0f;
    Float32 Height   = 0.0f;
    Float32 MinDepth = 0.0f;
    Float32 MaxDepth = 1.0f;
};

// 裁剪矩形
struct FRHIScissorRect
{
    Int32  X      = 0;
    Int32  Y      = 0;
    UInt32 Width  = 0;
    UInt32 Height = 0;
};

// 清除颜色值
struct FRHIClearColorValue
{
    Float32 R = 0.0f;
    Float32 G = 0.0f;
    Float32 B = 0.0f;
    Float32 A = 1.0f;
};

// 清除深度模板值
struct FRHIClearDepthStencilValue
{
    Float32 Depth   = 1.0f;
    UInt32  Stencil = 0;
};

// 顶点输入绑定描述
struct FRHIVertexInputBinding
{
    UInt32           Binding   = 0;
    UInt32           Stride    = 0;
    EVertexInputRate InputRate = EVertexInputRate::PerVertex;
};

// 顶点输入属性描述
// ============================================================================
// EFormatFeature — 像素格式在最优平铺下支持的能力
// ============================================================================

/// 像素格式能力位
///
/// 存在的理由: mip 链由 vkCmdBlitImage 逐级降采样生成, 而 blit 要求源格式
/// 支持 BlitSrc + 线性过滤。这一支持是**按格式、按设备**变化的, 不是普遍
/// 保证。不查就 blit, 在缺少该能力的设备上会撞上校验层错误, 而症状是
/// "某些机器上纹理全黑" —— 这类问题在开发机上永远复现不出来。
enum class EFormatFeature : UInt32
{
    None                   = 0,

    /// 可作为采样图像
    SampledImage           = 1u << 0,

    /// 采样时支持线性过滤 (mip 生成的前提之一)
    SampledImageLinear     = 1u << 1,

    /// 可作为存储图像
    StorageImage           = 1u << 2,

    /// 可作为颜色附件
    ColorAttachment        = 1u << 3,

    /// 颜色附件支持混合
    ColorAttachmentBlend   = 1u << 4,

    /// 可作为深度模板附件
    DepthStencilAttachment = 1u << 5,

    /// 可作为 blit 源 (mip 生成的前提之一)
    BlitSrc                = 1u << 6,

    /// 可作为 blit 目标
    BlitDst                = 1u << 7,
};

LIMX_NODISCARD constexpr EFormatFeature operator|(EFormatFeature a,
                                                   EFormatFeature b)
{
    return static_cast<EFormatFeature>(static_cast<UInt32>(a) |
                                       static_cast<UInt32>(b));
}

LIMX_NODISCARD constexpr EFormatFeature operator&(EFormatFeature a,
                                                   EFormatFeature b)
{
    return static_cast<EFormatFeature>(static_cast<UInt32>(a) &
                                       static_cast<UInt32>(b));
}

/// 测试是否包含全部指定能力位
LIMX_NODISCARD constexpr bool HasFormatFeature(EFormatFeature value,
                                                EFormatFeature required)
{
    return (static_cast<UInt32>(value) & static_cast<UInt32>(required)) ==
           static_cast<UInt32>(required);
}

// ============================================================================
// Mip 链层数
// ============================================================================

/// 由尺寸推算完整 mip 链的层数
///
/// 逐级折半直到 1x1, 因此层数是 floor(log2(max(w, h))) + 1。
/// 非二次幂尺寸同样成立 —— Vulkan 规定第 i 级的尺寸是 max(floor(base >> i), 1)。
///
/// 这里最容易错的是边界: 1x1 是 1 层不是 0 层, 1024x1024 是 11 层不是 10 层。
/// 少算一层会让最小的那级 mip 永远采不到, 多算一层会让 vkCreateImage 直接失败。
LIMX_NODISCARD constexpr UInt32 ComputeMipLevelCount(UInt32 width,
                                                     UInt32 height)
{
    if (width == 0 || height == 0)
    {
        return 0;
    }

    UInt32 extent = (width > height) ? width : height;
    UInt32 levels = 1;

    while (extent > 1)
    {
        extent >>= 1;
        ++levels;
    }

    return levels;
}

// ============================================================================
// FRHIVertexInputAttribute — 顶点输入属性
// ============================================================================

struct FRHIVertexInputAttribute
{
    UInt32       Location = 0;
    UInt32       Binding  = 0;
    EPixelFormat Format   = EPixelFormat::Unknown;
    UInt32       Offset   = 0;
};

// ============================================================================
// 工具函数 — 像素格式查询
// ============================================================================

// 获取像素格式每像素字节数 (压缩格式返回每块字节数)
inline constexpr UInt32 GetPixelFormatByteSize(EPixelFormat format)
{
    switch (format)
    {
        // 8 位单通道
        case EPixelFormat::R8_UNORM:
        case EPixelFormat::R8_SNORM:
        case EPixelFormat::R8_UINT:
        case EPixelFormat::R8_SINT:
            return 1;

        // 8 位双通道 / 16 位单通道
        case EPixelFormat::RG8_UNORM:
        case EPixelFormat::RG8_SNORM:
        case EPixelFormat::RG8_UINT:
        case EPixelFormat::RG8_SINT:
        case EPixelFormat::R16_UNORM:
        case EPixelFormat::R16_SNORM:
        case EPixelFormat::R16_UINT:
        case EPixelFormat::R16_SINT:
        case EPixelFormat::R16_SFLOAT:
        case EPixelFormat::D16_UNORM:
        case EPixelFormat::S8_UINT:
            return 2;

        // 8 位四通道 / 16 位双通道 / 32 位单通道 / 打包格式 / 深度24+模板8
        case EPixelFormat::RGBA8_UNORM:
        case EPixelFormat::RGBA8_SNORM:
        case EPixelFormat::RGBA8_UINT:
        case EPixelFormat::RGBA8_SINT:
        case EPixelFormat::RGBA8_SRGB:
        case EPixelFormat::BGRA8_UNORM:
        case EPixelFormat::BGRA8_SRGB:
        case EPixelFormat::RG16_UNORM:
        case EPixelFormat::RG16_SNORM:
        case EPixelFormat::RG16_UINT:
        case EPixelFormat::RG16_SINT:
        case EPixelFormat::RG16_SFLOAT:
        case EPixelFormat::R32_UINT:
        case EPixelFormat::R32_SINT:
        case EPixelFormat::R32_SFLOAT:
        case EPixelFormat::B10G11R11_UFLOAT_PACK32:
        case EPixelFormat::E5B9G9R9_UFLOAT_PACK32:
        case EPixelFormat::A2R10G10B10_UNORM_PACK32:
        case EPixelFormat::A2B10G10R10_UNORM_PACK32:
        case EPixelFormat::D24_UNORM_S8_UINT:
        case EPixelFormat::D32_SFLOAT:
            return 4;

        // 16 位四通道 / 32 位双通道 / 深度32+模板8 (实际8字节对齐)
        case EPixelFormat::RGBA16_UNORM:
        case EPixelFormat::RGBA16_SNORM:
        case EPixelFormat::RGBA16_UINT:
        case EPixelFormat::RGBA16_SINT:
        case EPixelFormat::RGBA16_SFLOAT:
        case EPixelFormat::RG32_UINT:
        case EPixelFormat::RG32_SINT:
        case EPixelFormat::RG32_SFLOAT:
        case EPixelFormat::D32_SFLOAT_S8_UINT:
        case EPixelFormat::R64_SFLOAT:
            return 8;

        // 32 位三通道
        case EPixelFormat::RGB32_UINT:
        case EPixelFormat::RGB32_SINT:
        case EPixelFormat::RGB32_SFLOAT:
            return 12;

        // 32 位四通道
        case EPixelFormat::RGBA32_UINT:
        case EPixelFormat::RGBA32_SINT:
        case EPixelFormat::RGBA32_SFLOAT:
            return 16;

        // BC 压缩 — 每 4x4 块的字节数
        case EPixelFormat::BC1_UNORM:
        case EPixelFormat::BC1_SRGB:
        case EPixelFormat::BC4_UNORM:
        case EPixelFormat::BC4_SNORM:
            return 8;

        case EPixelFormat::BC2_UNORM:
        case EPixelFormat::BC2_SRGB:
        case EPixelFormat::BC3_UNORM:
        case EPixelFormat::BC3_SRGB:
        case EPixelFormat::BC5_UNORM:
        case EPixelFormat::BC5_SNORM:
        case EPixelFormat::BC6H_UFLOAT:
        case EPixelFormat::BC6H_SFLOAT:
        case EPixelFormat::BC7_UNORM:
        case EPixelFormat::BC7_SRGB:
            return 16;

        default:
            return 0;
    }
}

// 判断是否为深度格式
inline constexpr bool IsDepthFormat(EPixelFormat format)
{
    return format == EPixelFormat::D16_UNORM
        || format == EPixelFormat::D32_SFLOAT
        || format == EPixelFormat::D24_UNORM_S8_UINT
        || format == EPixelFormat::D32_SFLOAT_S8_UINT;
}

// 判断是否为模板格式
inline constexpr bool IsStencilFormat(EPixelFormat format)
{
    return format == EPixelFormat::S8_UINT
        || format == EPixelFormat::D24_UNORM_S8_UINT
        || format == EPixelFormat::D32_SFLOAT_S8_UINT;
}

// 判断是否为深度+模板组合格式
inline constexpr bool IsDepthStencilFormat(EPixelFormat format)
{
    return format == EPixelFormat::D24_UNORM_S8_UINT
        || format == EPixelFormat::D32_SFLOAT_S8_UINT;
}

// 判断是否为 BC 压缩格式
inline constexpr bool IsCompressedFormat(EPixelFormat format)
{
    return format >= EPixelFormat::BC1_UNORM
        && format <= EPixelFormat::BC7_SRGB;
}

// 判断是否为 sRGB 格式
inline constexpr bool IsSRGBFormat(EPixelFormat format)
{
    return format == EPixelFormat::RGBA8_SRGB
        || format == EPixelFormat::BGRA8_SRGB
        || format == EPixelFormat::BC1_SRGB
        || format == EPixelFormat::BC2_SRGB
        || format == EPixelFormat::BC3_SRGB
        || format == EPixelFormat::BC7_SRGB;
}

// 判断是否为浮点格式
inline constexpr bool IsFloatFormat(EPixelFormat format)
{
    return format == EPixelFormat::R16_SFLOAT
        || format == EPixelFormat::RG16_SFLOAT
        || format == EPixelFormat::RGBA16_SFLOAT
        || format == EPixelFormat::R32_SFLOAT
        || format == EPixelFormat::RG32_SFLOAT
        || format == EPixelFormat::RGB32_SFLOAT
        || format == EPixelFormat::RGBA32_SFLOAT
        || format == EPixelFormat::R64_SFLOAT
        || format == EPixelFormat::B10G11R11_UFLOAT_PACK32
        || format == EPixelFormat::E5B9G9R9_UFLOAT_PACK32
        || format == EPixelFormat::BC6H_UFLOAT
        || format == EPixelFormat::BC6H_SFLOAT;
}

// 获取索引类型字节大小
inline constexpr UInt32 GetIndexTypeByteSize(EIndexType indexType)
{
    switch (indexType)
    {
        case EIndexType::UInt16: return 2;
        case EIndexType::UInt32: return 4;
        default:                 return 0;
    }
}

} // namespace Limx
