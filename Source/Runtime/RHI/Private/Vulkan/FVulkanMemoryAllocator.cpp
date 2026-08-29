/*******************************************************************************
 * 文件: FVulkanMemoryAllocator.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   Vulkan 显存分配器实现 — 块创建与销毁、子分配路由、专用分配、
 *   持久映射管理、非一致性内存刷新
 *
 * 设计哲学:
 *   块尺寸跟随堆容量 — 固定 256 MiB 的块在小显存设备上会一次吃掉可用堆的相当
 *   比例，导致第二个内存类型无块可用。按堆容量的八分之一取块尺寸并收敛到
 *   [32 MiB, 256 MiB]，让分配器在集显与高端独显上都表现合理。
 *
 *   失败路径必须收敛 — 显存不足是常态而非异常。子分配失败时依次尝试：
 *   现有块 → 新建块 → 按实际需求新建刚好够大的块 → 专用分配。
 *   每一步都可能失败，最终返回错误码而不是断言崩溃。
 *
 * 技术特性:
 *   - 主机可见块创建时整块映射一次, 生命周期内不再 Map/Unmap
 *   - 块内无活跃子分配时立即销毁并回收槽位, 显存及时归还驱动
 *   - Flush/Invalidate 按 nonCoherentAtomSize 向外扩展范围并夹紧到块边界
 *
 * 依赖关系:
 *   内部: Vulkan/FVulkanMemoryAllocator.h
 *
 * 注意事项:
 *   Free 后 allocation 被清零 — 调用方不得再使用其中的 Memory/Offset
 *
 ******************************************************************************/

#include "Vulkan/FVulkanMemoryAllocator.h"

namespace Limx
{

LIMX_DECLARE_LOG_CATEGORY(LogRHI)

namespace
{

/// 向上对齐 — alignment 必须是 2 的幂
FORCEINLINE VkDeviceSize AlignUpDeviceSize(VkDeviceSize value,
                                           VkDeviceSize alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

/// 向下对齐 — alignment 必须是 2 的幂
FORCEINLINE VkDeviceSize AlignDownDeviceSize(VkDeviceSize value,
                                             VkDeviceSize alignment)
{
    return value & ~(alignment - 1);
}

/// 逼近分配数上限的告警阈值 (百分比)
constexpr UInt32 kAllocationWarningPercent = 80;

} // namespace

// ============================================================================
// 生命周期
// ============================================================================

FVulkanMemoryAllocator::~FVulkanMemoryAllocator()
{
    Shutdown();
}

ERHIResult FVulkanMemoryAllocator::Initialize(
    VkDevice device,
    const VkPhysicalDeviceMemoryProperties& memoryProps,
    const VkPhysicalDeviceLimits& deviceLimits)
{
    LIMX_ASSERT(device != VK_NULL_HANDLE);

    m_Device           = device;
    m_MemoryProperties = memoryProps;

    // 粒度为 0 在规范上不合法, 但个别驱动会上报 0 — 归一到 1 (无约束)
    m_BufferImageGranularity =
        (deviceLimits.bufferImageGranularity > 0)
            ? deviceLimits.bufferImageGranularity
            : 1;

    m_NonCoherentAtomSize =
        (deviceLimits.nonCoherentAtomSize > 0)
            ? deviceLimits.nonCoherentAtomSize
            : 1;

    m_MaxDeviceAllocationCount = deviceLimits.maxMemoryAllocationCount;

    // 超过块尺寸下限一半的请求独占一块更划算 —— 塞进共享块会留下
    // 大到难以再利用的尾部空洞
    m_DedicatedThreshold = kMinBlockSize / 2;

    m_DeviceAllocationCount = 0;
    m_SuballocationCount    = 0;
    m_DedicatedCount        = 0;
    m_TotalReservedBytes    = 0;

    LIMX_LOG(LogRHI, Log,
        "[VkMemory] 分配器初始化 — 内存类型:{} 堆:{} 粒度:{} 原子:{} "
        "分配数上限:{}",
        m_MemoryProperties.memoryTypeCount,
        m_MemoryProperties.memoryHeapCount,
        static_cast<UInt64>(m_BufferImageGranularity),
        static_cast<UInt64>(m_NonCoherentAtomSize),
        m_MaxDeviceAllocationCount);

    return ERHIResult::Success;
}

void FVulkanMemoryAllocator::Shutdown()
{
    if (m_Device == VK_NULL_HANDLE)
    {
        return;
    }

    if (m_SuballocationCount > 0 || m_DedicatedCount > 0)
    {
        LIMX_LOG(LogRHI, Error,
            "[VkMemory] 关闭时仍有 {} 个子分配与 {} 个专用分配未归还 — 存在显存泄漏",
            m_SuballocationCount, m_DedicatedCount);
    }

    for (SizeType i = 0; i < m_Blocks.GetSize(); ++i)
    {
        if (!m_Blocks[i].IsActive())
        {
            continue;
        }

        if (m_Blocks[i].MappedBase != nullptr)
        {
            vkUnmapMemory(m_Device, m_Blocks[i].Memory);
            m_Blocks[i].MappedBase = nullptr;
        }

        vkFreeMemory(m_Device, m_Blocks[i].Memory, nullptr);
        m_Blocks[i].Memory = VK_NULL_HANDLE;
        m_Blocks[i].Registry.Shutdown();
    }

    m_Blocks.Clear();
    m_Blocks.Shrink();
    m_FreeBlockSlots.Clear();
    m_FreeBlockSlots.Shrink();

    for (UInt32 i = 0; i < kMaxMemoryTypes; ++i)
    {
        m_BlocksByType[i].Clear();
        m_BlocksByType[i].Shrink();
    }

    m_Device                = VK_NULL_HANDLE;
    m_DeviceAllocationCount = 0;
    m_SuballocationCount    = 0;
    m_DedicatedCount        = 0;
    m_TotalReservedBytes    = 0;
}

// ============================================================================
// 内存类型
// ============================================================================

UInt32 FVulkanMemoryAllocator::FindMemoryTypeIndex(
    UInt32 typeBits, VkMemoryPropertyFlags requiredProperties) const
{
    for (UInt32 i = 0; i < m_MemoryProperties.memoryTypeCount; ++i)
    {
        const bool typeAllowed = (typeBits & (1u << i)) != 0;
        const bool hasProperties =
            (m_MemoryProperties.memoryTypes[i].propertyFlags &
             requiredProperties) == requiredProperties;

        if (typeAllowed && hasProperties)
        {
            return i;
        }
    }

    return 0xFFFFFFFFu;
}

bool FVulkanMemoryAllocator::IsHostVisible(UInt32 memoryTypeIndex) const
{
    return (m_MemoryProperties.memoryTypes[memoryTypeIndex].propertyFlags &
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
}

bool FVulkanMemoryAllocator::IsHostCoherent(UInt32 memoryTypeIndex) const
{
    return (m_MemoryProperties.memoryTypes[memoryTypeIndex].propertyFlags &
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
}

VkDeviceSize FVulkanMemoryAllocator::ComputeBlockSize(
    UInt32 memoryTypeIndex) const
{
    const UInt32 heapIndex =
        m_MemoryProperties.memoryTypes[memoryTypeIndex].heapIndex;
    const VkDeviceSize heapSize =
        m_MemoryProperties.memoryHeaps[heapIndex].size;

    // 取堆容量的八分之一: 单块既不至于吃掉整个堆, 又足够容纳大量资源
    VkDeviceSize blockSize = heapSize / 8;

    if (blockSize < kMinBlockSize)
    {
        blockSize = kMinBlockSize;
    }

    if (blockSize > kMaxBlockSize)
    {
        blockSize = kMaxBlockSize;
    }

    // 堆本身小于下限时退让到堆容量的四分之一, 避免请求必然失败
    if (blockSize > heapSize)
    {
        blockSize = heapSize / 4;
    }

    return blockSize;
}

// ============================================================================
// 块管理
// ============================================================================

UInt32 FVulkanMemoryAllocator::CreateBlock(UInt32 memoryTypeIndex,
                                           VkDeviceSize blockSize)
{
    if (m_MaxDeviceAllocationCount > 0 &&
        m_DeviceAllocationCount >= m_MaxDeviceAllocationCount)
    {
        LIMX_LOG(LogRHI, Error,
            "[VkMemory] 已达设备分配数上限 {} — 无法再创建块",
            m_MaxDeviceAllocationCount);
        return 0xFFFFFFFFu;
    }

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize  = blockSize;
    allocInfo.memoryTypeIndex = memoryTypeIndex;

    VkDeviceMemory memory = VK_NULL_HANDLE;
    const VkResult vkResult =
        vkAllocateMemory(m_Device, &allocInfo, nullptr, &memory);

    if (vkResult != VK_SUCCESS)
    {
        LIMX_LOG(LogRHI, Warning,
            "[VkMemory] 块分配失败 — 类型:{} 尺寸:{} MiB 结果:{}",
            memoryTypeIndex,
            static_cast<UInt64>(blockSize / (1024 * 1024)),
            static_cast<Int32>(vkResult));
        return 0xFFFFFFFFu;
    }

    ++m_DeviceAllocationCount;
    m_TotalReservedBytes += blockSize;

    // ------------------------------------------------------------------
    // 主机可见的块整块映射一次
    //
    // 同一 VkDeviceMemory 同时只能有一个活跃映射，多个子分配各自 Map
    // 会直接违反规范。整块映射后子分配的地址由基址加偏移得出。
    // ------------------------------------------------------------------

    void* mappedBase = nullptr;

    if (IsHostVisible(memoryTypeIndex))
    {
        const VkResult mapResult =
            vkMapMemory(m_Device, memory, 0, VK_WHOLE_SIZE, 0, &mappedBase);

        if (mapResult != VK_SUCCESS)
        {
            LIMX_LOG(LogRHI, Error,
                "[VkMemory] 块映射失败 — 类型:{} 结果:{}",
                memoryTypeIndex, static_cast<Int32>(mapResult));

            vkFreeMemory(m_Device, memory, nullptr);
            --m_DeviceAllocationCount;
            m_TotalReservedBytes -= blockSize;
            return 0xFFFFFFFFu;
        }
    }

    // ------------------------------------------------------------------
    // 取一个块槽位 — 优先复用已销毁的槽位以保持索引稳定
    // ------------------------------------------------------------------

    UInt32 blockIndex = 0xFFFFFFFFu;

    if (m_FreeBlockSlots.GetSize() > 0)
    {
        blockIndex = m_FreeBlockSlots.Last();
        m_FreeBlockSlots.RemoveAt(m_FreeBlockSlots.GetSize() - 1);
    }
    else
    {
        blockIndex = static_cast<UInt32>(m_Blocks.Add(FMemoryBlock()));
    }

    m_Blocks[blockIndex].Memory          = memory;
    m_Blocks[blockIndex].Size            = blockSize;
    m_Blocks[blockIndex].MappedBase      = mappedBase;
    m_Blocks[blockIndex].MemoryTypeIndex = memoryTypeIndex;
    m_Blocks[blockIndex].Registry.Initialize(
        static_cast<UInt64>(blockSize),
        static_cast<UInt64>(m_BufferImageGranularity));

    m_BlocksByType[memoryTypeIndex].Add(blockIndex);

    LIMX_LOG(LogRHI, Log,
        "[VkMemory] 新建块 #{} — 类型:{} 尺寸:{} MiB 映射:{} (累计分配数 {}/{})",
        blockIndex, memoryTypeIndex,
        static_cast<UInt64>(blockSize / (1024 * 1024)),
        mappedBase != nullptr,
        m_DeviceAllocationCount, m_MaxDeviceAllocationCount);

    // 逼近上限时主动告警 — 静默撞墙的症状极难定位
    if (m_MaxDeviceAllocationCount > 0)
    {
        const UInt32 usedPercent =
            (m_DeviceAllocationCount * 100u) / m_MaxDeviceAllocationCount;

        if (usedPercent >= kAllocationWarningPercent)
        {
            LIMX_LOG(LogRHI, Warning,
                "[VkMemory] 设备分配数已用 {}% ({}/{}) — 接近上限",
                usedPercent, m_DeviceAllocationCount,
                m_MaxDeviceAllocationCount);
        }
    }

    return blockIndex;
}

void FVulkanMemoryAllocator::DestroyBlock(UInt32 blockIndex)
{
    FMemoryBlock& block = m_Blocks[blockIndex];

    if (!block.IsActive())
    {
        return;
    }

    LIMX_ASSERT_MSG(block.Registry.IsEmpty(),
                    "销毁仍有活跃子分配的显存块");

    if (block.MappedBase != nullptr)
    {
        vkUnmapMemory(m_Device, block.Memory);
        block.MappedBase = nullptr;
    }

    vkFreeMemory(m_Device, block.Memory, nullptr);

    const UInt32 memoryTypeIndex = block.MemoryTypeIndex;

    m_TotalReservedBytes -= block.Size;
    --m_DeviceAllocationCount;

    block.Memory          = VK_NULL_HANDLE;
    block.Size            = 0;
    block.MemoryTypeIndex = 0xFFFFFFFFu;
    block.Registry.Shutdown();

    // 从类型索引表中摘除
    TArray<UInt32>& typeBlocks = m_BlocksByType[memoryTypeIndex];
    for (SizeType i = 0; i < typeBlocks.GetSize(); ++i)
    {
        if (typeBlocks[i] == blockIndex)
        {
            typeBlocks.RemoveAtSwap(i);
            break;
        }
    }

    m_FreeBlockSlots.Add(blockIndex);

    LIMX_LOG(LogRHI, Log,
        "[VkMemory] 销毁块 #{} — 类型:{} (累计分配数 {}/{})",
        blockIndex, memoryTypeIndex,
        m_DeviceAllocationCount, m_MaxDeviceAllocationCount);
}

// ============================================================================
// 分配
// ============================================================================

ERHIResult FVulkanMemoryAllocator::AllocateDedicated(
    const VkMemoryRequirements& requirements,
    UInt32 memoryTypeIndex,
    FVulkanAllocation& outAllocation)
{
    if (m_MaxDeviceAllocationCount > 0 &&
        m_DeviceAllocationCount >= m_MaxDeviceAllocationCount)
    {
        LIMX_LOG(LogRHI, Error,
            "[VkMemory] 已达设备分配数上限 {} — 专用分配失败",
            m_MaxDeviceAllocationCount);
        return ERHIResult::ErrorOutOfDeviceMemory;
    }

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize  = requirements.size;
    allocInfo.memoryTypeIndex = memoryTypeIndex;

    VkDeviceMemory memory = VK_NULL_HANDLE;
    const VkResult vkResult =
        vkAllocateMemory(m_Device, &allocInfo, nullptr, &memory);

    if (vkResult != VK_SUCCESS)
    {
        LIMX_LOG(LogRHI, Error,
            "[VkMemory] 专用分配失败 — 尺寸:{} KiB 结果:{}",
            static_cast<UInt64>(requirements.size / 1024),
            static_cast<Int32>(vkResult));
        return ERHIResult::ErrorOutOfDeviceMemory;
    }

    ++m_DeviceAllocationCount;
    ++m_DedicatedCount;
    m_TotalReservedBytes += requirements.size;

    void* mappedBase = nullptr;

    if (IsHostVisible(memoryTypeIndex))
    {
        const VkResult mapResult =
            vkMapMemory(m_Device, memory, 0, VK_WHOLE_SIZE, 0, &mappedBase);

        if (mapResult != VK_SUCCESS)
        {
            mappedBase = nullptr;
        }
    }

    outAllocation.Memory          = memory;
    outAllocation.Offset          = 0;
    outAllocation.Size            = requirements.size;
    outAllocation.MappedPtr       = mappedBase;
    outAllocation.MemoryTypeIndex = memoryTypeIndex;
    outAllocation.BlockIndex      = FVulkanAllocation::kDedicatedBlockIndex;
    outAllocation.NodeIndex       = 0xFFFFFFFFu;

    return ERHIResult::Success;
}

ERHIResult FVulkanMemoryAllocator::Allocate(
    const VkMemoryRequirements& requirements,
    VkMemoryPropertyFlags requiredProperties,
    ESuballocationType type,
    FVulkanAllocation& outAllocation)
{
    LIMX_ASSERT(m_Device != VK_NULL_HANDLE);
    LIMX_ASSERT(type != ESuballocationType::Free);

    outAllocation = FVulkanAllocation();

    if (requirements.size == 0)
    {
        return ERHIResult::ErrorInvalidParameter;
    }

    const UInt32 memoryTypeIndex =
        FindMemoryTypeIndex(requirements.memoryTypeBits, requiredProperties);

    if (memoryTypeIndex == 0xFFFFFFFFu)
    {
        LIMX_LOG(LogRHI, Error,
            "[VkMemory] 未找到匹配的内存类型 — typeBits:{} props:{}",
            FHex(requirements.memoryTypeBits),
            FHex(static_cast<UInt32>(requiredProperties)));
        return ERHIResult::ErrorOutOfDeviceMemory;
    }

    // 对齐要求至少为 1 — 个别驱动对小资源上报 0
    const VkDeviceSize alignment =
        (requirements.alignment > 0) ? requirements.alignment : 1;

    // ------------------------------------------------------------------
    // 大资源走专用分配 — 塞进共享块会留下难以复用的尾部空洞
    // ------------------------------------------------------------------

    if (requirements.size >= m_DedicatedThreshold)
    {
        return AllocateDedicated(requirements, memoryTypeIndex, outAllocation);
    }

    // ------------------------------------------------------------------
    // 尝试在该类型已有的块中子分配
    // ------------------------------------------------------------------

    const TArray<UInt32>& candidateBlocks = m_BlocksByType[memoryTypeIndex];

    for (SizeType i = 0; i < candidateBlocks.GetSize(); ++i)
    {
        const UInt32 blockIndex = candidateBlocks[i];

        FSuballocationResult suballocation;
        if (!m_Blocks[blockIndex].Registry.Allocate(
                static_cast<UInt64>(requirements.size),
                static_cast<UInt64>(alignment),
                type, suballocation))
        {
            continue;
        }

        const FMemoryBlock& block = m_Blocks[blockIndex];

        outAllocation.Memory = block.Memory;
        outAllocation.Offset = static_cast<VkDeviceSize>(suballocation.Offset);
        outAllocation.Size   = static_cast<VkDeviceSize>(suballocation.Size);
        outAllocation.MappedPtr =
            (block.MappedBase != nullptr)
                ? static_cast<void*>(static_cast<UInt8*>(block.MappedBase) +
                                     suballocation.Offset)
                : nullptr;
        outAllocation.MemoryTypeIndex = memoryTypeIndex;
        outAllocation.BlockIndex      = blockIndex;
        outAllocation.NodeIndex       = suballocation.NodeIndex;

        ++m_SuballocationCount;
        return ERHIResult::Success;
    }

    // ------------------------------------------------------------------
    // 现有块都装不下 — 新建一块
    // ------------------------------------------------------------------

    VkDeviceSize blockSize = ComputeBlockSize(memoryTypeIndex);

    // 需求超过标准块尺寸时按实际需求放大, 否则新块同样装不下
    if (blockSize < requirements.size)
    {
        blockSize = AlignUpDeviceSize(requirements.size, alignment);
    }

    UInt32 newBlockIndex = CreateBlock(memoryTypeIndex, blockSize);

    // 首次失败可能是块过大 — 退让到刚好容纳本次需求再试一次
    if (newBlockIndex == 0xFFFFFFFFu && blockSize > requirements.size)
    {
        const VkDeviceSize fallbackSize =
            AlignUpDeviceSize(requirements.size, alignment);

        LIMX_LOG(LogRHI, Warning,
            "[VkMemory] 标准块创建失败, 退让到 {} KiB 重试",
            static_cast<UInt64>(fallbackSize / 1024));

        newBlockIndex = CreateBlock(memoryTypeIndex, fallbackSize);
    }

    // 仍然失败 — 最后尝试专用分配
    if (newBlockIndex == 0xFFFFFFFFu)
    {
        return AllocateDedicated(requirements, memoryTypeIndex, outAllocation);
    }

    FSuballocationResult suballocation;
    if (!m_Blocks[newBlockIndex].Registry.Allocate(
            static_cast<UInt64>(requirements.size),
            static_cast<UInt64>(alignment),
            type, suballocation))
    {
        // 新块装不下本次请求是逻辑错误 — 块尺寸已按需求放大过
        LIMX_LOG(LogRHI, Error,
            "[VkMemory] 新建块仍无法容纳 {} 字节请求 — 分配器逻辑异常",
            static_cast<UInt64>(requirements.size));

        DestroyBlock(newBlockIndex);
        return ERHIResult::ErrorOutOfDeviceMemory;
    }

    const FMemoryBlock& block = m_Blocks[newBlockIndex];

    outAllocation.Memory = block.Memory;
    outAllocation.Offset = static_cast<VkDeviceSize>(suballocation.Offset);
    outAllocation.Size   = static_cast<VkDeviceSize>(suballocation.Size);
    outAllocation.MappedPtr =
        (block.MappedBase != nullptr)
            ? static_cast<void*>(static_cast<UInt8*>(block.MappedBase) +
                                 suballocation.Offset)
            : nullptr;
    outAllocation.MemoryTypeIndex = memoryTypeIndex;
    outAllocation.BlockIndex      = newBlockIndex;
    outAllocation.NodeIndex       = suballocation.NodeIndex;

    ++m_SuballocationCount;
    return ERHIResult::Success;
}

// ============================================================================
// 回收
// ============================================================================

void FVulkanMemoryAllocator::Free(FVulkanAllocation& allocation)
{
    if (!allocation.IsValid())
    {
        return;
    }

    if (allocation.IsDedicated())
    {
        if (allocation.MappedPtr != nullptr)
        {
            vkUnmapMemory(m_Device, allocation.Memory);
        }

        vkFreeMemory(m_Device, allocation.Memory, nullptr);

        m_TotalReservedBytes -= allocation.Size;
        --m_DeviceAllocationCount;
        --m_DedicatedCount;

        allocation = FVulkanAllocation();
        return;
    }

    const UInt32 blockIndex = allocation.BlockIndex;

    if (blockIndex >= m_Blocks.GetSize() || !m_Blocks[blockIndex].IsActive())
    {
        LIMX_LOG(LogRHI, Error,
            "[VkMemory] Free 收到无效块索引 {}", blockIndex);
        allocation = FVulkanAllocation();
        return;
    }

    m_Blocks[blockIndex].Registry.Free(allocation.NodeIndex);
    --m_SuballocationCount;

    // 块内已无活跃分配 — 立即归还给驱动, 不做延迟回收:
    // 显存是稀缺资源, 空块长期滞留会挤压其他类型的可用堆
    if (m_Blocks[blockIndex].Registry.IsEmpty())
    {
        DestroyBlock(blockIndex);
    }

    allocation = FVulkanAllocation();
}

// ============================================================================
// 一致性
// ============================================================================

void FVulkanMemoryAllocator::ComputeAtomAlignedRange(
    const FVulkanAllocation& allocation,
    VkDeviceSize offset, VkDeviceSize size,
    VkDeviceSize& outOffset, VkDeviceSize& outSize) const
{
    const VkDeviceSize requestedSize =
        (size == VK_WHOLE_SIZE) ? (allocation.Size - offset) : size;

    const VkDeviceSize absoluteBegin = allocation.Offset + offset;
    const VkDeviceSize absoluteEnd   = absoluteBegin + requestedSize;

    // 范围必须向外扩展到原子边界 — 向内收缩会漏掉边缘字节
    VkDeviceSize alignedBegin =
        AlignDownDeviceSize(absoluteBegin, m_NonCoherentAtomSize);
    VkDeviceSize alignedEnd =
        AlignUpDeviceSize(absoluteEnd, m_NonCoherentAtomSize);

    // 夹紧到所属内存对象的范围内
    const VkDeviceSize blockSize =
        allocation.IsDedicated()
            ? allocation.Size
            : m_Blocks[allocation.BlockIndex].Size;

    if (alignedEnd > blockSize)
    {
        alignedEnd = blockSize;
    }

    if (alignedBegin > alignedEnd)
    {
        alignedBegin = alignedEnd;
    }

    outOffset = alignedBegin;
    outSize   = alignedEnd - alignedBegin;
}

ERHIResult FVulkanMemoryAllocator::Flush(const FVulkanAllocation& allocation,
                                         VkDeviceSize offset,
                                         VkDeviceSize size)
{
    if (!allocation.IsValid())
    {
        return ERHIResult::ErrorInvalidHandle;
    }

    // 一致性内存的写入对设备自动可见, 无需显式刷新
    if (IsHostCoherent(allocation.MemoryTypeIndex))
    {
        return ERHIResult::Success;
    }

    VkDeviceSize alignedOffset = 0;
    VkDeviceSize alignedSize   = 0;
    ComputeAtomAlignedRange(allocation, offset, size,
                            alignedOffset, alignedSize);

    if (alignedSize == 0)
    {
        return ERHIResult::Success;
    }

    VkMappedMemoryRange range = {};
    range.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = allocation.Memory;
    range.offset = alignedOffset;
    range.size   = alignedSize;

    const VkResult vkResult =
        vkFlushMappedMemoryRanges(m_Device, 1, &range);

    return (vkResult == VK_SUCCESS) ? ERHIResult::Success
                                    : ERHIResult::ErrorUnknown;
}

ERHIResult FVulkanMemoryAllocator::Invalidate(
    const FVulkanAllocation& allocation,
    VkDeviceSize offset, VkDeviceSize size)
{
    if (!allocation.IsValid())
    {
        return ERHIResult::ErrorInvalidHandle;
    }

    if (IsHostCoherent(allocation.MemoryTypeIndex))
    {
        return ERHIResult::Success;
    }

    VkDeviceSize alignedOffset = 0;
    VkDeviceSize alignedSize   = 0;
    ComputeAtomAlignedRange(allocation, offset, size,
                            alignedOffset, alignedSize);

    if (alignedSize == 0)
    {
        return ERHIResult::Success;
    }

    VkMappedMemoryRange range = {};
    range.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = allocation.Memory;
    range.offset = alignedOffset;
    range.size   = alignedSize;

    const VkResult vkResult =
        vkInvalidateMappedMemoryRanges(m_Device, 1, &range);

    return (vkResult == VK_SUCCESS) ? ERHIResult::Success
                                    : ERHIResult::ErrorUnknown;
}

// ============================================================================
// 统计
// ============================================================================

FVulkanMemoryStats FVulkanMemoryAllocator::GetStats() const
{
    FVulkanMemoryStats stats;

    stats.DeviceAllocationCount = m_DeviceAllocationCount;
    stats.DeviceAllocationLimit = m_MaxDeviceAllocationCount;
    stats.SuballocationCount    = m_SuballocationCount;
    stats.DedicatedCount        = m_DedicatedCount;
    stats.TotalReservedBytes    = m_TotalReservedBytes;

    for (SizeType i = 0; i < m_Blocks.GetSize(); ++i)
    {
        if (!m_Blocks[i].IsActive())
        {
            continue;
        }

        ++stats.BlockCount;
        stats.TotalUsedBytes +=
            static_cast<VkDeviceSize>(m_Blocks[i].Registry.GetUsedSize());
    }

    return stats;
}

void FVulkanMemoryAllocator::LogStats(const AnsiChar* context) const
{
    const FVulkanMemoryStats stats = GetStats();

    LIMX_LOG(LogRHI, Log,
        "[VkMemory] {} — 块:{} 子分配:{} 专用:{} | 设备分配数 {}/{} | "
        "已申请 {} MiB, 已占用 {} MiB",
        context, stats.BlockCount, stats.SuballocationCount,
        stats.DedicatedCount,
        stats.DeviceAllocationCount, stats.DeviceAllocationLimit,
        static_cast<UInt64>(stats.TotalReservedBytes / (1024 * 1024)),
        static_cast<UInt64>(stats.TotalUsedBytes / (1024 * 1024)));
}

} // namespace Limx
