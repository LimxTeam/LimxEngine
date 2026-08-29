/*******************************************************************************
 * 文件: FRenderResources.h
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   GPU 资源类型与句柄 — 网格、纹理及其代际句柄
 *   这是资产管线的中性数据在 GPU 侧的对应物
 *
 * 设计哲学:
 *   资源由管理器拥有, 场景只持句柄 — 此前 GPU 缓冲区由 FRenderer 直接持有，
 *   场景节点只是引用它们的裸句柄。这个方向是反的: 渲染器是消费者而非所有者，
 *   一旦要加载任意资产, 渲染器就得为每种资产的生命周期负责。
 *   把所有权移到资源管理器后, 渲染器只读取, 场景只引用, 三方各司其职。
 *
 *   索引宽度按顶点数自适应 — 顶点数不超过 65535 时用 16 位索引，
 *   索引缓冲区带宽直接减半。真实场景里绝大多数网格都在这个范围内，
 *   而带宽是索引装配阶段的实际瓶颈。
 *
 *   子网格即绘制批次 — 一个网格按材质切分为若干段, 每段是一次绘制调用。
 *   把这个切分保留到 GPU 侧, 渲染时无需再回查 CPU 数据。
 *
 * 技术特性:
 *   - 句柄为 (槽位索引, 代际号) 二元组, 资源卸载后旧句柄自动失效
 *   - 网格携带包围盒, 供视锥剔除直接使用
 *   - 纹理记录格式与 mip 层数, 供描述符更新与内存统计
 *
 * 依赖关系:
 *   内部: Core/CoreMinimal.h, RHI/RHI/RHIResources.h,
 *          Core/Math/FBoundingBox.h
 *
 * 注意事项:
 *   资源结构只描述 GPU 对象, 不持有 CPU 侧的像素或顶点数据 ——
 *   上传完成后 CPU 数据即可释放
 *
 ******************************************************************************/

#pragma once

#include "Core/CoreMinimal.h"
#include "Core/Containers/TArray.h"
#include "Core/Containers/FName.h"
#include "Core/Math/FBoundingBox.h"
#include "RHI/RHI/RHIResources.h"
#include "RHI/RHI/RHIDefinitions.h"
#include "RenderCore/RenderCoreAPI.h"

namespace Limx
{

// ============================================================================
// FRenderResourceHandle — 代际句柄
// ============================================================================

/// GPU 资源句柄
///
/// 槽位在资源卸载后会被复用。裸索引在那一刻会指向别的资源，
/// 而"渲染出了别的模型"这种症状极难溯源。代际号让失效变成显式的 nullptr。
struct FRenderResourceHandle
{
    static constexpr UInt32 kInvalidIndex = 0xFFFFFFFFu;

    UInt32 Index      = kInvalidIndex;
    UInt32 Generation = 0;

    LIMX_NODISCARD bool IsValid() const { return Index != kInvalidIndex; }

    LIMX_NODISCARD bool operator==(const FRenderResourceHandle& other) const
    {
        return Index == other.Index && Generation == other.Generation;
    }

    LIMX_NODISCARD bool operator!=(const FRenderResourceHandle& other) const
    {
        return !(*this == other);
    }
};

/// 网格资源句柄 — 与纹理句柄类型隔离, 避免互相误传
struct FMeshResourceHandle : public FRenderResourceHandle
{
};

/// 纹理资源句柄
struct FTextureResourceHandle : public FRenderResourceHandle
{
};

// ============================================================================
// FMeshSection — 一次绘制批次
// ============================================================================

/// 网格中共享同一材质的索引区间
struct FMeshSection
{
    FName Name;

    /// 在索引缓冲区中的起始位置 (以索引个数计, 非字节)
    UInt32 IndexOffset = 0;

    /// 索引个数 — 必为 3 的倍数
    UInt32 IndexCount = 0;

    /// 材质槽位 — 指向所属渲染对象的材质数组, -1 表示使用默认材质
    Int32 MaterialSlot = -1;

    /// 该批次的局部包围盒
    FBoundingBox Bounds;
};

// ============================================================================
// FMeshResource — GPU 网格
// ============================================================================

/// GPU 网格资源
struct FMeshResource
{
    FName Name;

    /// 顶点缓冲区 (设备本地)
    FRHIBufferHandle VertexBuffer;

    /// 索引缓冲区 (设备本地)
    FRHIBufferHandle IndexBuffer;

    UInt32 VertexCount = 0;
    UInt32 IndexCount  = 0;

    /// 索引宽度 — 顶点数不超过 65535 时为 UInt16, 索引带宽减半
    EIndexType IndexType = EIndexType::UInt32;

    /// 按材质切分的绘制批次
    TArray<FMeshSection> Sections;

    /// 局部空间包围盒 — 供视锥剔除使用
    FBoundingBox Bounds;

    /// 顶点缓冲区字节数
    UInt64 VertexBufferBytes = 0;

    /// 索引缓冲区字节数
    UInt64 IndexBufferBytes = 0;

    LIMX_NODISCARD bool IsValid() const
    {
        return VertexBuffer.IsValid() && IndexBuffer.IsValid() &&
               VertexCount > 0 && IndexCount > 0;
    }

    LIMX_NODISCARD UInt64 GetTotalBytes() const
    {
        return VertexBufferBytes + IndexBufferBytes;
    }
};

// ============================================================================
// FTextureResource — GPU 纹理
// ============================================================================

/// GPU 纹理资源
struct FTextureResource
{
    FName Name;

    FRHITextureHandle     Texture;
    FRHITextureViewHandle View;

    /// 采样器 — 通常由管理器按配置共享, 而非每张纹理一个
    FRHISamplerHandle Sampler;

    UInt32 Width     = 0;
    UInt32 Height    = 0;
    UInt32 MipLevels = 1;

    EPixelFormat Format = EPixelFormat::Unknown;

    /// 显存占用字节数 (含全部 mip)
    UInt64 MemoryBytes = 0;

    LIMX_NODISCARD bool IsValid() const
    {
        return Texture.IsValid() && View.IsValid() &&
               Width > 0 && Height > 0;
    }
};

// ============================================================================
// FRenderResourceStats — 资源统计
// ============================================================================

/// GPU 资源统计
struct FRenderResourceStats
{
    UInt32 MeshCount    = 0;
    UInt32 TextureCount = 0;

    /// 引用计数为零、可被回收的资源数
    UInt32 UnreferencedCount = 0;

    /// 已退役、等待 GPU 用完后销毁的资源数
    UInt32 PendingReleaseCount = 0;

    /// 顶点与索引缓冲区占用的显存
    UInt64 MeshBytes = 0;

    /// 纹理占用的显存
    UInt64 TextureBytes = 0;

    /// 全部网格的顶点总数
    UInt64 TotalVertices = 0;

    /// 全部网格的三角形总数
    UInt64 TotalTriangles = 0;

    /// 全部网格的绘制批次总数
    UInt32 TotalSections = 0;

    LIMX_NODISCARD UInt64 GetTotalBytes() const
    {
        return MeshBytes + TextureBytes;
    }
};

} // namespace Limx
