/*******************************************************************************
 * 文件: FVulkanMemoryAllocator.h
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   Vulkan 显存分配器 — 以大块 vkAllocateMemory 加块内子分配的方式供应显存
 *   按内存类型分池，块内复用 FSuballocationRegistry 做区间管理
 *   对主机可见的块整块持久映射，子分配共享该映射
 *
 * 设计哲学:
 *   分配次数是硬资源 — Vulkan 的 maxMemoryAllocationCount 通常仅 4096，
 *   而一个真实场景的网格与贴图轻易过万。逐资源调用 vkAllocateMemory 会在
 *   加载中途直接撞墙，且驱动侧每次分配都是重量级操作。本分配器把 vkAllocateMemory
 *   的调用次数从"每资源一次"降到"每块一次"，使资源数量与分配次数解耦。
 *
 *   映射必须整块 — Vulkan 规范规定同一 VkDeviceMemory 同时只能存在一个活跃映射。
 *   多个子分配共享一块内存，若各自调用 vkMapMemory 会直接违规。因此主机可见的块
 *   在创建时映射一次并长期持有，子分配的指针由块基址加偏移得出。
 *
 *   大块走专用路径 — 超过阈值的请求单独 vkAllocateMemory。把巨型资源塞进共享块
 *   会造成难以回收的碎片，而它们本身数量少，专用分配不会威胁分配次数预算。
 *
 * 技术特性:
 *   - 块尺寸按堆容量自适应, 上下限收敛到 [32 MiB, 256 MiB]
 *   - 块槽位复用: 空块销毁后槽位入栈, 保证 FVulkanAllocation 中的块索引稳定
 *   - 非一致性内存的 Flush/Invalidate 按 nonCoherentAtomSize 对齐范围
 *   - 逼近 maxMemoryAllocationCount 时主动告警, 避免静默失败
 *
 * 依赖关系:
 *   内部: Vulkan/VulkanCommon.h, RHI/RHI/IRHIDevice.h,
 *          RHI/Memory/FSuballocationRegistry.h
 *   外部: vulkan-1
 *
 * 注意事项:
 *   非线程安全 — 当前渲染路径为单线程提交; 引入多线程录制时需在此加锁
 *   Shutdown 前必须归还全部分配, 否则块内仍有活跃子分配会触发断言
 *
 ******************************************************************************/

#pragma once

#include "Vulkan/VulkanCommon.h"
#include "RHI/RHI/IRHIDevice.h"
#include "RHI/Memory/FSuballocationRegistry.h"

namespace Limx
{

// ============================================================================
// FVulkanAllocation — 一次显存子分配的句柄
// ============================================================================

/// 显存子分配
///
/// 同时描述共享块内的子分配与专用分配两种来源，调用方无需区分：
/// 绑定资源时一律使用 Memory + Offset，写入时一律使用 MappedPtr。
struct FVulkanAllocation
{
    /// 所属的设备内存对象
    VkDeviceMemory Memory = VK_NULL_HANDLE;

    /// 在该内存对象内的起始偏移
    VkDeviceSize Offset = 0;

    /// 本次分配的字节数
    VkDeviceSize Size = 0;

    /// 主机可访问地址 — 已包含 Offset; 不可映射时为 nullptr
    void* MappedPtr = nullptr;

    /// 所属内存类型索引
    UInt32 MemoryTypeIndex = 0xFFFFFFFFu;

    /// 所属块索引 — kDedicatedBlockIndex 表示这是一次专用分配
    UInt32 BlockIndex = 0xFFFFFFFFu;

    /// 块内登记表的节点索引 — 专用分配时无意义
    UInt32 NodeIndex = 0xFFFFFFFFu;

    /// 专用分配的块索引哨兵
    static constexpr UInt32 kDedicatedBlockIndex = 0xFFFFFFFEu;

    /// 是否持有有效显存
    LIMX_NODISCARD bool IsValid() const
    {
        return Memory != VK_NULL_HANDLE;
    }

    /// 是否为专用分配
    LIMX_NODISCARD bool IsDedicated() const
    {
        return BlockIndex == kDedicatedBlockIndex;
    }
};

// ============================================================================
// FVulkanMemoryStats — 分配器统计
// ============================================================================

/// 显存使用统计
struct FVulkanMemoryStats
{
    /// 当前存活的 vkAllocateMemory 调用数 (块 + 专用分配)
    UInt32 DeviceAllocationCount = 0;

    /// 设备允许的最大分配数
    UInt32 DeviceAllocationLimit = 0;

    /// 当前块数量
    UInt32 BlockCount = 0;

    /// 当前活跃的子分配数
    UInt32 SuballocationCount = 0;

    /// 专用分配数
    UInt32 DedicatedCount = 0;

    /// 已向驱动申请的显存总量
    VkDeviceSize TotalReservedBytes = 0;

    /// 已被子分配占用的显存总量
    VkDeviceSize TotalUsedBytes = 0;
};

// ============================================================================
// FVulkanMemoryAllocator — 显存分配器
// ============================================================================

/// Vulkan 显存分配器
class FVulkanMemoryAllocator
{
public:
    /// Vulkan 规范定义的内存类型上限
    static constexpr UInt32 kMaxMemoryTypes = VK_MAX_MEMORY_TYPES;

    /// 块尺寸下限 — 小于此值时分块收益不足以抵消管理开销
    static constexpr VkDeviceSize kMinBlockSize = 32ull * 1024ull * 1024ull;

    /// 块尺寸上限 — 再大会让空块回收的粒度过粗
    static constexpr VkDeviceSize kMaxBlockSize = 256ull * 1024ull * 1024ull;

    FVulkanMemoryAllocator() = default;
    ~FVulkanMemoryAllocator();

    FVulkanMemoryAllocator(const FVulkanMemoryAllocator&)            = delete;
    FVulkanMemoryAllocator& operator=(const FVulkanMemoryAllocator&) = delete;

    // ========================================================================
    // 生命周期
    // ========================================================================

    /// 初始化
    /// @param device         逻辑设备
    /// @param memoryProps    物理设备内存属性
    /// @param deviceLimits   物理设备限制 (取粒度、原子尺寸、分配数上限)
    ERHIResult Initialize(VkDevice device,
                          const VkPhysicalDeviceMemoryProperties& memoryProps,
                          const VkPhysicalDeviceLimits& deviceLimits);

    /// 释放全部块与专用分配
    void Shutdown();

    /// 是否已初始化
    LIMX_NODISCARD bool IsInitialized() const
    {
        return m_Device != VK_NULL_HANDLE;
    }

    // ========================================================================
    // 分配
    // ========================================================================

    /// 按内存需求分配显存
    /// @param requirements       vkGet*MemoryRequirements 的结果
    /// @param requiredProperties 必需的内存属性标志
    /// @param type               资源类型 (决定 bufferImageGranularity 约束)
    /// @param outAllocation      成功时填充
    ERHIResult Allocate(const VkMemoryRequirements& requirements,
                        VkMemoryPropertyFlags requiredProperties,
                        ESuballocationType type,
                        FVulkanAllocation& outAllocation);

    /// 归还一次分配 — 调用后 allocation 被重置为无效
    void Free(FVulkanAllocation& allocation);

    // ========================================================================
    // 一致性
    // ========================================================================

    /// 将主机写入刷新到设备可见 — 内存已是 HOST_COHERENT 时为空操作
    /// @param allocation 目标分配
    /// @param offset     相对于分配起点的偏移
    /// @param size       字节数; VK_WHOLE_SIZE 表示直到分配末尾
    ERHIResult Flush(const FVulkanAllocation& allocation,
                     VkDeviceSize offset = 0,
                     VkDeviceSize size = VK_WHOLE_SIZE);

    /// 使主机缓存失效以读取设备写入 — HOST_COHERENT 时为空操作
    ERHIResult Invalidate(const FVulkanAllocation& allocation,
                          VkDeviceSize offset = 0,
                          VkDeviceSize size = VK_WHOLE_SIZE);

    // ========================================================================
    // 查询
    // ========================================================================

    /// 当前统计快照
    LIMX_NODISCARD FVulkanMemoryStats GetStats() const;

    /// 输出一份人类可读的用量报告到日志
    void LogStats(const AnsiChar* context) const;

private:
    // ========================================================================
    // 显存块
    // ========================================================================

    /// 一次 vkAllocateMemory 得到的大块显存
    struct FMemoryBlock
    {
        /// 设备内存对象 — VK_NULL_HANDLE 表示该槽位空闲可复用
        VkDeviceMemory Memory = VK_NULL_HANDLE;

        /// 块容量
        VkDeviceSize Size = 0;

        /// 整块映射的基址 — 不可映射时为 nullptr
        void* MappedBase = nullptr;

        /// 所属内存类型
        UInt32 MemoryTypeIndex = 0xFFFFFFFFu;

        /// 块内区间管理
        FSuballocationRegistry Registry;

        /// 槽位是否在用
        LIMX_NODISCARD bool IsActive() const
        {
            return Memory != VK_NULL_HANDLE;
        }
    };

    // ========================================================================
    // 内部实现
    // ========================================================================

    /// 查找满足需求的内存类型索引 — 未找到返回 0xFFFFFFFF
    LIMX_NODISCARD UInt32 FindMemoryTypeIndex(
        UInt32 typeBits, VkMemoryPropertyFlags requiredProperties) const;

    /// 该内存类型对应的块尺寸 — 按所属堆容量自适应
    LIMX_NODISCARD VkDeviceSize ComputeBlockSize(UInt32 memoryTypeIndex) const;

    /// 内存类型是否主机可见
    LIMX_NODISCARD bool IsHostVisible(UInt32 memoryTypeIndex) const;

    /// 内存类型是否主机一致
    LIMX_NODISCARD bool IsHostCoherent(UInt32 memoryTypeIndex) const;

    /// 新建一个块并返回其索引 — 失败返回 0xFFFFFFFF
    UInt32 CreateBlock(UInt32 memoryTypeIndex, VkDeviceSize blockSize);

    /// 销毁指定块 — 块内必须已无活跃子分配
    void DestroyBlock(UInt32 blockIndex);

    /// 走专用分配路径
    ERHIResult AllocateDedicated(const VkMemoryRequirements& requirements,
                                 UInt32 memoryTypeIndex,
                                 FVulkanAllocation& outAllocation);

    /// 计算 Flush/Invalidate 所需的对齐范围
    void ComputeAtomAlignedRange(const FVulkanAllocation& allocation,
                                 VkDeviceSize offset, VkDeviceSize size,
                                 VkDeviceSize& outOffset,
                                 VkDeviceSize& outSize) const;

    // ========================================================================
    // 成员数据
    // ========================================================================

    VkDevice m_Device = VK_NULL_HANDLE;

    /// 物理设备内存属性
    VkPhysicalDeviceMemoryProperties m_MemoryProperties = {};

    /// 线性与非线性资源的共存粒度
    VkDeviceSize m_BufferImageGranularity = 1;

    /// 非一致性内存的刷新原子尺寸
    VkDeviceSize m_NonCoherentAtomSize = 1;

    /// 设备允许的最大 vkAllocateMemory 存活数
    UInt32 m_MaxDeviceAllocationCount = 0;

    /// 超过此尺寸的请求走专用分配
    VkDeviceSize m_DedicatedThreshold = 0;

    /// 全部块 — 索引稳定, 空槽可复用
    TArray<FMemoryBlock> m_Blocks;

    /// 已销毁可复用的块槽位
    TArray<UInt32> m_FreeBlockSlots;

    /// 每个内存类型持有的块索引
    TArray<UInt32> m_BlocksByType[kMaxMemoryTypes];

    /// 当前存活的 vkAllocateMemory 调用数
    UInt32 m_DeviceAllocationCount = 0;

    /// 当前活跃的子分配数
    UInt32 m_SuballocationCount = 0;

    /// 当前专用分配数
    UInt32 m_DedicatedCount = 0;

    /// 已向驱动申请的显存总量
    VkDeviceSize m_TotalReservedBytes = 0;
};

} // namespace Limx
