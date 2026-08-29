/*******************************************************************************
 * 文件: FSuballocationRegistry.h
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   子分配区间登记表 — 在一段连续地址空间内做带对齐与粒度约束的分配/回收
 *   这是显存块内部的分配算法核心，不含任何 Vulkan 类型，可独立单元测试
 *   提供 Validate() 自检全部内部不变式，使分配器缺陷在测试中可被直接捕获
 *
 * 设计哲学:
 *   算法与 API 解耦 — 显存分配器最容易出错的部分是区间管理本身（分裂、合并、
 *   对齐、粒度冲突），而不是 Vulkan 调用。把这部分抽成不依赖设备的纯逻辑，
 *   就能在无 GPU 的环境下用海量随机操作序列去压它，缺陷不必等到渲染时才暴露。
 *
 *   索引而非指针 — 节点存放在可增长数组中，节点间用 UInt32 索引互指。
 *   数组扩容时指针会失效而索引不会，这样节点池可以随需求增长而无需稳定地址。
 *
 *   不变式可验证 — Validate() 检查物理链完整性、无相邻空闲块、自由链表与节点
 *   状态一致、区间总和等于容量。任何分裂或合并的疏漏都会被它抓住。
 *
 * 技术特性:
 *   - 分级自由列表 + 位图: 按 2 的幂分桶，位图跳过空桶，分配近似 O(1)
 *   - 边界合并: 节点携带物理前驱/后继索引，释放时与相邻空闲区 O(1) 合并
 *   - 粒度约束: 支持 bufferImageGranularity —— 线性与非线性资源不共享同一粒度页
 *   - 节点槽复用: 合并释放出的节点槽进入空闲槽栈，避免数组无界增长
 *
 * 依赖关系:
 *   内部: Core/CoreMinimal.h, Core/Containers/TArray.h,
 *          Core/Memory/IAllocator.h, Core/Memory/DefaultAllocator.h
 *
 * 注意事项:
 *   非线程安全 — 并发访问需由调用方（内存分配器）加锁
 *   Allocate 返回的 NodeIndex 是释放时的唯一凭据，调用方必须保存
 *   粒度为 1 时等价于无粒度约束，全部相关检查被短路跳过
 *
 ******************************************************************************/

#pragma once

#include "Core/CoreMinimal.h"
#include "Core/Containers/TArray.h"
#include "Core/Memory/IAllocator.h"
#include "Core/Memory/DefaultAllocator.h"

namespace Limx
{

// ============================================================================
// ESuballocationType — 子分配的资源类型
//
// Vulkan 规范要求: 线性资源 (缓冲区、LINEAR 平铺图像) 与非线性资源
// (OPTIMAL 平铺图像) 若落入同一个 bufferImageGranularity 页内，行为未定义。
// 登记表据此在分配时避开冲突位置。
// ============================================================================

enum class ESuballocationType : UInt8
{
    /// 空闲区间
    Free = 0,

    /// 线性资源 — 缓冲区、VK_IMAGE_TILING_LINEAR 图像
    Linear = 1,

    /// 非线性资源 — VK_IMAGE_TILING_OPTIMAL 图像
    NonLinear = 2,
};

// ============================================================================
// FSuballocationResult — 一次成功分配的结果
// ============================================================================

/// 分配结果
struct FSuballocationResult
{
    /// 分配区间在整段空间内的起始偏移 (已满足对齐与粒度约束)
    UInt64 Offset = 0;

    /// 分配区间的字节数 (等于请求尺寸)
    UInt64 Size = 0;

    /// 节点索引 — 释放时必须原样传回 Free()
    UInt32 NodeIndex = 0xFFFFFFFFu;
};

// ============================================================================
// FSuballocationRegistry — 区间登记表
// ============================================================================

/// 连续地址空间内的子分配管理器
class FSuballocationRegistry
{
public:
    /// 无效节点索引
    static constexpr UInt32 kInvalidNode = 0xFFFFFFFFu;

    /// 最小分桶尺寸 (字节) — 小于此值的请求归入 0 号桶
    static constexpr UInt64 kMinBucketSize = 256;

    /// 分桶数量 — 覆盖 256B 到 256B << 23 (2 TB)
    static constexpr UInt32 kBucketCount = 24;

    FSuballocationRegistry() = default;
    ~FSuballocationRegistry();

    FSuballocationRegistry(const FSuballocationRegistry&)            = delete;
    FSuballocationRegistry& operator=(const FSuballocationRegistry&) = delete;

    FSuballocationRegistry(FSuballocationRegistry&& other) noexcept;
    FSuballocationRegistry& operator=(FSuballocationRegistry&& other) noexcept;

    // ========================================================================
    // 生命周期
    // ========================================================================

    /// 初始化为一整段空闲空间
    /// @param totalSize   管理的总字节数, 必须大于 0
    /// @param granularity 粒度约束 (Vulkan 的 bufferImageGranularity),
    ///                    传 1 表示无约束; 必须是 2 的幂
    /// @param allocator   节点数组使用的 CPU 分配器
    void Initialize(UInt64 totalSize, UInt64 granularity,
                    IAllocator& allocator = GetDefaultAllocator());

    /// 释放全部内部结构 — 不影响被管理的实际内存
    void Shutdown();

    // ========================================================================
    // 分配与回收
    // ========================================================================

    /// 尝试分配一段区间
    /// @param size       请求字节数, 必须大于 0
    /// @param alignment  对齐要求, 必须是 2 的幂
    /// @param type       资源类型 (影响粒度冲突判定), 不得为 Free
    /// @param outResult  成功时填充分配结果
    /// @return 是否分配成功 — 失败表示当前无满足约束的空闲区间
    LIMX_NODISCARD bool Allocate(UInt64 size, UInt64 alignment,
                                 ESuballocationType type,
                                 FSuballocationResult& outResult);

    /// 归还一段区间
    /// @param nodeIndex Allocate 返回的节点索引
    void Free(UInt32 nodeIndex);

    // ========================================================================
    // 状态查询
    // ========================================================================

    /// 管理的总字节数
    LIMX_NODISCARD UInt64 GetTotalSize() const { return m_TotalSize; }

    /// 已分配字节数 (不含因对齐产生的空隙)
    LIMX_NODISCARD UInt64 GetUsedSize() const { return m_UsedSize; }

    /// 剩余可用字节数 — 因碎片化, 未必能一次性分配出这么多
    LIMX_NODISCARD UInt64 GetFreeSize() const { return m_TotalSize - m_UsedSize; }

    /// 当前最大的单块空闲区间 — 反映碎片化程度
    LIMX_NODISCARD UInt64 GetLargestFreeRegion() const;

    /// 当前活跃的分配数
    LIMX_NODISCARD UInt32 GetAllocationCount() const { return m_AllocationCount; }

    /// 是否没有任何活跃分配
    LIMX_NODISCARD bool IsEmpty() const { return m_AllocationCount == 0; }

    /// 是否已初始化
    LIMX_NODISCARD bool IsInitialized() const { return m_TotalSize > 0; }

    /// 查询某个已分配节点的偏移
    LIMX_NODISCARD UInt64 GetNodeOffset(UInt32 nodeIndex) const;

    /// 查询某个已分配节点的尺寸
    LIMX_NODISCARD UInt64 GetNodeSize(UInt32 nodeIndex) const;

    // ========================================================================
    // 自检
    // ========================================================================

    /// 校验全部内部不变式
    ///
    /// 检查项:
    ///   1. 物理链首尾正确, 区间首尾相接无空隙无重叠
    ///   2. 全部区间尺寸之和等于总容量
    ///   3. 不存在物理相邻的两个空闲区间 (合并遗漏)
    ///   4. 自由链表中的节点全部标记为 Free, 且落在正确的分桶内
    ///   5. 位图与自由链表非空状态一致
    ///   6. 已分配字节数与节点状态吻合
    ///
    /// @return 全部不变式成立返回 true
    LIMX_NODISCARD bool Validate() const;

private:
    // ========================================================================
    // 节点
    // ========================================================================

    /// 一段连续区间的元数据
    ///
    /// 元数据存放在 CPU 侧独立数组中而非区间内部 —— 被管理的目标可能是
    /// 设备本地显存, CPU 根本无法写入其中的任何字节。
    struct FNode
    {
        /// 区间起始偏移
        UInt64 Offset = 0;

        /// 区间字节数
        UInt64 Size = 0;

        /// 物理地址上相邻的前一段
        UInt32 PrevPhysical = kInvalidNode;

        /// 物理地址上相邻的后一段
        UInt32 NextPhysical = kInvalidNode;

        /// 同一分桶自由链表中的前一个 (仅 Free 节点有效)
        UInt32 PrevFree = kInvalidNode;

        /// 同一分桶自由链表中的后一个 (仅 Free 节点有效)
        UInt32 NextFree = kInvalidNode;

        /// 区间类型
        ESuballocationType Type = ESuballocationType::Free;

        LIMX_NODISCARD bool IsFree() const
        {
            return Type == ESuballocationType::Free;
        }
    };

    // ========================================================================
    // 节点池
    // ========================================================================

    /// 取一个空闲节点槽 — 优先复用回收槽, 否则追加
    UInt32 AcquireNode();

    /// 归还节点槽
    void ReleaseNode(UInt32 nodeIndex);

    // ========================================================================
    // 自由链表
    // ========================================================================

    /// 计算尺寸对应的分桶下标
    LIMX_NODISCARD static UInt32 ComputeBucket(UInt64 size);

    /// 将节点插入其尺寸对应的自由链表头部
    void LinkFree(UInt32 nodeIndex);

    /// 将节点从其所在的自由链表摘除
    void UnlinkFree(UInt32 nodeIndex);

    // ========================================================================
    // 分配辅助
    // ========================================================================

    /// 在指定空闲节点内尝试放置一次分配
    /// @param nodeIndex   候选空闲节点
    /// @param size        请求尺寸
    /// @param alignment   对齐要求
    /// @param type        资源类型
    /// @param outOffset   成功时输出满足全部约束的起始偏移
    /// @return 该节点能否容纳本次分配
    LIMX_NODISCARD bool TryPlaceInNode(UInt32 nodeIndex, UInt64 size,
                                       UInt64 alignment,
                                       ESuballocationType type,
                                       UInt64& outOffset) const;

    /// 判断两个类型是否构成粒度冲突 (一个线性、一个非线性)
    LIMX_NODISCARD static bool IsGranularityConflict(ESuballocationType a,
                                                     ESuballocationType b);

    /// 判断两个偏移是否落在同一个粒度页内
    LIMX_NODISCARD bool IsOnSamePage(UInt64 offsetA, UInt64 offsetB) const;

    // ========================================================================
    // 成员数据
    // ========================================================================

    /// 节点池 — 索引稳定, 地址不稳定
    TArray<FNode> m_Nodes;

    /// 已回收可复用的节点槽
    TArray<UInt32> m_FreeNodeSlots;

    /// 各分桶自由链表头
    UInt32 m_FreeListHeads[kBucketCount] = {};

    /// 分桶非空位图 — 第 i 位置 1 表示第 i 桶有空闲节点
    UInt32 m_BucketMask = 0;

    /// 物理链首节点
    UInt32 m_PhysicalHead = kInvalidNode;

    /// 管理的总字节数
    UInt64 m_TotalSize = 0;

    /// 粒度约束 (2 的幂; 1 表示无约束)
    UInt64 m_Granularity = 1;

    /// 已分配字节数
    UInt64 m_UsedSize = 0;

    /// 活跃分配数
    UInt32 m_AllocationCount = 0;

    /// CPU 侧分配器
    IAllocator* m_Allocator = nullptr;
};

} // namespace Limx
