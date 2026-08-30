/*******************************************************************************
 * 文件: FBindlessTable.h
 * 创建时间: 2026-08-31
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   全局纹理表与材质表 — 一个描述符集容纳整个场景的贴图与材质参数
 *   绘制时不再逐材质绑定描述符集, 材质下标随 push constant 走
 *
 * 设计哲学:
 *   下标必须永远有效 — PARTIALLY_BOUND 允许数组里存在未写入的槽位, 但
 *     着色器仍然不能索引到它们, 那是未定义行为而且验证层未必抓得到 (索引
 *     是运行时算出来的)。因此缺贴图的材质指向 0 号占位纹理, 而不是 -1 或
 *     任何"无效"标记 —— 无效标记要求着色器分支判断, 而分支判断一旦漏写
 *     就是随机读显存。
 *
 *   注册即固定 — 纹理与材质的下标一经分配就不再变动。下标变动意味着已经
 *     录进命令缓冲区的 push constant 会指向错误的材质, 而那种错误表现为
 *     "某些物体偶尔用错贴图", 极难复现。
 *
 *   材质数据每帧整体上传 — 不做脏标记增量更新。Sponza 是 25 个材质、
 *     80 字节一个, 总共 2 KB; 为省这 2 KB 引入脏区间管理, 换来的是一类
 *     "某帧材质没更新"的时序 bug。
 *
 * 技术特性:
 *   - set 1 binding 0: StorageBuffer, 材质数组 (std430)
 *   - set 1 binding 1: sampler2D[kMaxTextures], PARTIALLY_BOUND + UPDATE_AFTER_BIND
 *   - 每个在飞帧一份材质缓冲区 —— GPU 可能还在读上一帧的
 *
 * 依赖关系:
 *   内部: RHI/RHI/IRHIDevice.h, Core/Containers/TArray.h,
 *          Core/Containers/TMap.h
 *
 ******************************************************************************/

#pragma once

#include "RenderCore/RenderCoreMinimal.h"

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Containers/TArray.h"
#include "Core/Math/FVector.h"
#include "RHI/RHI/IRHIDevice.h"

namespace Limx
{

// ============================================================================
// FBindlessMaterial — 材质在 GPU 上的布局 (std430)
// ============================================================================

/// 与 pbr.frag 里的 MaterialData 逐字段对应
///
/// std430 下 vec4 按 16 字节对齐, float/uint 按 4 字节。字段顺序是排过的:
/// 两个 vec4 各自起于 16 的倍数, 中间的标量凑满一组。改动顺序前先算对齐,
/// 错位的表现是"材质参数整体串了一位", 画面上像是材质配错而不是像 bug。
struct FBindlessMaterial
{
    FVector4 BaseColor      = FVector4(1.0f, 1.0f, 1.0f, 1.0f);   // 0

    Float32  Metallic       = 0.0f;                                // 16
    Float32  Roughness      = 1.0f;
    Float32  AO             = 1.0f;
    Float32  NormalScale    = 1.0f;

    FVector4 EmissiveColor  = FVector4(0.0f, 0.0f, 0.0f, 0.0f);   // 32

    Float32  AlphaCutoff    = 0.5f;                                // 48
    UInt32   TextureFlags   = 0;
    UInt32   BlendMode      = 0;
    UInt32   AlbedoIndex    = 0;

    UInt32   NormalIndex             = 0;                          // 64
    UInt32   MetallicRoughnessIndex  = 0;
    UInt32   OcclusionIndex          = 0;
    UInt32   EmissiveIndex           = 0;
};                                                                 // 共 80 字节

static_assert(sizeof(FBindlessMaterial) == 80,
              "FBindlessMaterial 必须是 80 字节 — 与着色器里的 MaterialData 对齐");

// ============================================================================
// FBindlessTable
// ============================================================================

/// 全局纹理表 + 材质表
class LIMX_RENDERCORE_API FBindlessTable
{
public:
    /// 纹理表容量
    ///
    /// Sponza 用 69 张。取 1024 是留给多场景与运行时加载的余量 —— 描述符
    /// 本身很小 (一个 COMBINED_IMAGE_SAMPLER 几十字节), 而扩容意味着重建
    /// 描述符集, 那要等 GPU 空闲。
    static constexpr UInt32 kMaxTextures = 1024;

    /// 材质表容量
    static constexpr UInt32 kMaxMaterials = 4096;

    /// 缺贴图时指向的占位槽位
    ///
    /// 固定为 0 号 —— 着色器无条件采样, 不做有效性分支。分支一旦漏写就是
    /// 随机读显存, 而"总是有效的下标"这个不变量在注册时就能保证。
    static constexpr UInt32 kPlaceholderTexture = 0;

    FBindlessTable() = default;
    ~FBindlessTable() = default;

    LIMX_NON_COPYABLE(FBindlessTable);

    // ========================================================================
    // 生命周期
    // ========================================================================

    /// 创建描述符集布局、描述符集与材质缓冲区
    ///
    /// @param placeholderView    占位纹理视图 (缺贴图的材质指向它)
    /// @param placeholderSampler 占位采样器
    ERHIResult Initialize(IRHIDevice*           device,
                          UInt32                framesInFlight,
                          FRHITextureViewHandle placeholderView,
                          FRHISamplerHandle     placeholderSampler);

    void Shutdown(IRHIDevice* device);

    LIMX_NODISCARD bool IsInitialized() const { return m_Device != nullptr; }

    LIMX_NODISCARD FRHIDescSetLayoutHandle GetLayout() const
    {
        return m_Layout;
    }

    LIMX_NODISCARD FRHIDescriptorSetHandle GetDescriptorSet(UInt32 frame) const
    {
        return m_Sets[frame % m_FramesInFlight];
    }

    // ========================================================================
    // 注册
    // ========================================================================

    /// 注册一张纹理, 返回它在全局表里的下标
    ///
    /// 下标一经分配不再变动 —— 变动会让已经录进命令缓冲区的材质下标指向
    /// 错误的贴图。
    ///
    /// 表满时返回 kPlaceholderTexture 并告警, 而不是返回一个无效值:
    /// 无效值要求每个使用处都判断, 漏判一处就是随机读显存。
    UInt32 RegisterTexture(FRHITextureViewHandle view,
                           FRHISamplerHandle     sampler);

    /// 注册一份材质, 返回下标
    UInt32 RegisterMaterial(const FBindlessMaterial& material);

    /// 更新已注册的材质 (下标不变)
    void UpdateMaterial(UInt32 index, const FBindlessMaterial& material);

    /// 清空材质与纹理表 (关卡切换)
    ///
    /// 必须在 GPU 空闲之后调用 —— 表里的描述符可能还被在飞的命令缓冲区
    /// 引用着。
    void Reset();

    // ========================================================================
    // 逐帧
    // ========================================================================

    /// 把材质数组上传到本帧的缓冲区
    void Upload(UInt32 frameIndex);

    LIMX_NODISCARD UInt32 GetTextureCount() const { return m_TextureCount; }
    LIMX_NODISCARD UInt32 GetMaterialCount() const
    {
        return static_cast<UInt32>(m_Materials.GetSize());
    }

private:
    IRHIDevice* m_Device         = nullptr;
    UInt32      m_FramesInFlight = 0;

    FRHIDescSetLayoutHandle m_Layout;

    /// 每个在飞帧一份描述符集与材质缓冲区
    ///
    /// 纹理描述符在每一份里都写一遍 —— 它们指向同一批纹理, 但描述符集
    /// 本身不能跨帧共享写入 (写入时 GPU 可能正在读另一帧的那一份)。
    static constexpr UInt32 kMaxFrames = 4;

    FRHIDescriptorSetHandle m_Sets[kMaxFrames];
    FRHIBufferHandle        m_MaterialBuffers[kMaxFrames];
    void*                   m_MaterialMapped[kMaxFrames] = {};

    UInt32 m_TextureCount = 0;

    /// 已注册的 (视图, 采样器) 对 — 用于去重
    ///
    /// 25 个材质 x 5 个槽位 = 125 次注册, 而实际只有 69 张不同的贴图。
    /// 不去重的话同一张图会占掉多个槽位, 而且同一份数据在描述符里出现
    /// 多次 —— 表满得更快, 且看统计数字时会误以为场景用了更多贴图。
    ///
    /// 线性查找而非哈希: 一百多项、只在加载时调用, O(n^2) 是一万多次
    /// 句柄比较, 可以忽略; 换来的是不必为句柄对设计哈希。
    struct FTextureKey
    {
        FRHITextureViewHandle View;
        FRHISamplerHandle     Sampler;
    };

    TArray<FTextureKey> m_TextureKeys;

    TArray<FBindlessMaterial> m_Materials;
};

} // namespace Limx
