/*******************************************************************************
 * 文件: FSuballocationRegistry.cpp
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   子分配区间登记表实现 — 分桶自由列表、区间分裂与合并、粒度冲突规避、
 *   内部不变式自检
 *
 * 设计哲学:
 *   引用不跨越节点分配 — 节点池是可增长数组，AcquireNode 可能触发重分配从而
 *   使已取得的引用悬垂。因此实现中一律通过 m_Nodes[index] 就近访问，绝不把
 *   节点引用缓存到跨越 AcquireNode 的作用域里。这是本文件最重要的编码约束。
 *
 *   合并即不变式 — "任意两个物理相邻的区间不同时为空闲"是全部逻辑赖以成立的
 *   前提: 它保证空闲区间的物理前驱与后继必定是已分配区间，从而粒度冲突只需
 *   检查直接邻居两侧，无需回溯整条物理链。
 *
 * 技术特性:
 *   - FloorLog2 / CountTrailingZeros 均为无分支循环的常数步位运算
 *   - 分配先在精确桶内线性探测, 失败后经位图跳至下一个非空高位桶
 *   - 释放按 后继 → 前驱 顺序合并, 保证一次调用最多回收两个节点槽
 *
 * 依赖关系:
 *   内部: RHI/Memory/FSuballocationRegistry.h
 *
 * 注意事项:
 *   Validate 为 O(节点数), 仅供测试与调试调用, 不应出现在渲染热路径
 *
 ******************************************************************************/

#include "RHI/Memory/FSuballocationRegistry.h"

namespace Limx
{

namespace
{

/// 向上对齐到 alignment 的整数倍 — alignment 必须是 2 的幂
FORCEINLINE UInt64 AlignUpTo(UInt64 value, UInt64 alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

/// floor(log2(value)) — value 必须大于 0, 常数步二分
FORCEINLINE UInt32 FloorLog2(UInt64 value)
{
    UInt32 result = 0;

    if (value >= (1ull << 32)) { value >>= 32; result += 32; }
    if (value >= (1ull << 16)) { value >>= 16; result += 16; }
    if (value >= (1ull << 8))  { value >>= 8;  result += 8;  }
    if (value >= (1ull << 4))  { value >>= 4;  result += 4;  }
    if (value >= (1ull << 2))  { value >>= 2;  result += 2;  }
    if (value >= (1ull << 1))  {               result += 1;  }

    return result;
}

/// 最低置位位的下标 — value 必须非零, 常数步二分
FORCEINLINE UInt32 CountTrailingZeros(UInt32 value)
{
    UInt32 result = 0;

    if ((value & 0x0000FFFFu) == 0) { value >>= 16; result += 16; }
    if ((value & 0x000000FFu) == 0) { value >>= 8;  result += 8;  }
    if ((value & 0x0000000Fu) == 0) { value >>= 4;  result += 4;  }
    if ((value & 0x00000003u) == 0) { value >>= 2;  result += 2;  }
    if ((value & 0x00000001u) == 0) {               result += 1;  }

    return result;
}

} // namespace

// ============================================================================
// 生命周期
// ============================================================================

FSuballocationRegistry::~FSuballocationRegistry()
{
    Shutdown();
}

FSuballocationRegistry::FSuballocationRegistry(
    FSuballocationRegistry&& other) noexcept
    : m_Nodes(static_cast<TArray<FNode>&&>(other.m_Nodes))
    , m_FreeNodeSlots(static_cast<TArray<UInt32>&&>(other.m_FreeNodeSlots))
    , m_BucketMask(other.m_BucketMask)
    , m_PhysicalHead(other.m_PhysicalHead)
    , m_TotalSize(other.m_TotalSize)
    , m_Granularity(other.m_Granularity)
    , m_UsedSize(other.m_UsedSize)
    , m_AllocationCount(other.m_AllocationCount)
    , m_Allocator(other.m_Allocator)
{
    for (UInt32 i = 0; i < kBucketCount; ++i)
    {
        m_FreeListHeads[i] = other.m_FreeListHeads[i];
        other.m_FreeListHeads[i] = kInvalidNode;
    }

    other.m_BucketMask      = 0;
    other.m_PhysicalHead    = kInvalidNode;
    other.m_TotalSize       = 0;
    other.m_Granularity     = 1;
    other.m_UsedSize        = 0;
    other.m_AllocationCount = 0;
    other.m_Allocator       = nullptr;
}

FSuballocationRegistry& FSuballocationRegistry::operator=(
    FSuballocationRegistry&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }

    Shutdown();

    m_Nodes           = static_cast<TArray<FNode>&&>(other.m_Nodes);
    m_FreeNodeSlots   = static_cast<TArray<UInt32>&&>(other.m_FreeNodeSlots);
    m_BucketMask      = other.m_BucketMask;
    m_PhysicalHead    = other.m_PhysicalHead;
    m_TotalSize       = other.m_TotalSize;
    m_Granularity     = other.m_Granularity;
    m_UsedSize        = other.m_UsedSize;
    m_AllocationCount = other.m_AllocationCount;
    m_Allocator       = other.m_Allocator;

    for (UInt32 i = 0; i < kBucketCount; ++i)
    {
        m_FreeListHeads[i] = other.m_FreeListHeads[i];
        other.m_FreeListHeads[i] = kInvalidNode;
    }

    other.m_BucketMask      = 0;
    other.m_PhysicalHead    = kInvalidNode;
    other.m_TotalSize       = 0;
    other.m_Granularity     = 1;
    other.m_UsedSize        = 0;
    other.m_AllocationCount = 0;
    other.m_Allocator       = nullptr;

    return *this;
}

void FSuballocationRegistry::Initialize(UInt64 totalSize, UInt64 granularity,
                                        IAllocator& allocator)
{
    LIMX_ASSERT(totalSize > 0);
    LIMX_ASSERT(granularity > 0);
    LIMX_ASSERT((granularity & (granularity - 1)) == 0);

    Shutdown();

    m_Allocator   = &allocator;
    m_TotalSize   = totalSize;
    m_Granularity = granularity;

    m_Nodes         = TArray<FNode>(allocator);
    m_FreeNodeSlots = TArray<UInt32>(allocator);

    for (UInt32 i = 0; i < kBucketCount; ++i)
    {
        m_FreeListHeads[i] = kInvalidNode;
    }

    m_BucketMask      = 0;
    m_UsedSize        = 0;
    m_AllocationCount = 0;

    // 初始状态: 一整段空闲区间覆盖全部空间
    const UInt32 rootIndex = AcquireNode();

    m_Nodes[rootIndex].Offset       = 0;
    m_Nodes[rootIndex].Size         = totalSize;
    m_Nodes[rootIndex].PrevPhysical = kInvalidNode;
    m_Nodes[rootIndex].NextPhysical = kInvalidNode;
    m_Nodes[rootIndex].Type         = ESuballocationType::Free;

    m_PhysicalHead = rootIndex;

    LinkFree(rootIndex);
}

void FSuballocationRegistry::Shutdown()
{
    m_Nodes.Clear();
    m_Nodes.Shrink();
    m_FreeNodeSlots.Clear();
    m_FreeNodeSlots.Shrink();

    for (UInt32 i = 0; i < kBucketCount; ++i)
    {
        m_FreeListHeads[i] = kInvalidNode;
    }

    m_BucketMask      = 0;
    m_PhysicalHead    = kInvalidNode;
    m_TotalSize       = 0;
    m_Granularity     = 1;
    m_UsedSize        = 0;
    m_AllocationCount = 0;
}

// ============================================================================
// 节点池
// ============================================================================

UInt32 FSuballocationRegistry::AcquireNode()
{
    if (m_FreeNodeSlots.GetSize() > 0)
    {
        const UInt32 index = m_FreeNodeSlots.Last();
        m_FreeNodeSlots.RemoveAt(m_FreeNodeSlots.GetSize() - 1);

        m_Nodes[index] = FNode();
        return index;
    }

    const SizeType index = m_Nodes.Add(FNode());
    return static_cast<UInt32>(index);
}

void FSuballocationRegistry::ReleaseNode(UInt32 nodeIndex)
{
    // 置为默认态, 避免陈旧链接在后续复用时被误读
    m_Nodes[nodeIndex] = FNode();
    m_FreeNodeSlots.Add(nodeIndex);
}

// ============================================================================
// 自由链表
// ============================================================================

UInt32 FSuballocationRegistry::ComputeBucket(UInt64 size)
{
    if (size < kMinBucketSize)
    {
        return 0;
    }

    const UInt32 bucket = FloorLog2(size / kMinBucketSize);
    return (bucket < kBucketCount) ? bucket : (kBucketCount - 1);
}

void FSuballocationRegistry::LinkFree(UInt32 nodeIndex)
{
    const UInt32 bucket = ComputeBucket(m_Nodes[nodeIndex].Size);
    const UInt32 head   = m_FreeListHeads[bucket];

    m_Nodes[nodeIndex].PrevFree = kInvalidNode;
    m_Nodes[nodeIndex].NextFree = head;

    if (head != kInvalidNode)
    {
        m_Nodes[head].PrevFree = nodeIndex;
    }

    m_FreeListHeads[bucket] = nodeIndex;
    m_BucketMask |= (1u << bucket);
}

void FSuballocationRegistry::UnlinkFree(UInt32 nodeIndex)
{
    const UInt32 bucket = ComputeBucket(m_Nodes[nodeIndex].Size);
    const UInt32 prev   = m_Nodes[nodeIndex].PrevFree;
    const UInt32 next   = m_Nodes[nodeIndex].NextFree;

    if (prev != kInvalidNode)
    {
        m_Nodes[prev].NextFree = next;
    }
    else
    {
        m_FreeListHeads[bucket] = next;

        if (next == kInvalidNode)
        {
            m_BucketMask &= ~(1u << bucket);
        }
    }

    if (next != kInvalidNode)
    {
        m_Nodes[next].PrevFree = prev;
    }

    m_Nodes[nodeIndex].PrevFree = kInvalidNode;
    m_Nodes[nodeIndex].NextFree = kInvalidNode;
}

// ============================================================================
// 粒度判定
// ============================================================================

bool FSuballocationRegistry::IsGranularityConflict(ESuballocationType a,
                                                   ESuballocationType b)
{
    // 空闲区间不占用粒度页, 与任何类型都不冲突
    if (a == ESuballocationType::Free || b == ESuballocationType::Free)
    {
        return false;
    }

    // 线性与非线性资源不得共享同一粒度页
    return a != b;
}

bool FSuballocationRegistry::IsOnSamePage(UInt64 offsetA, UInt64 offsetB) const
{
    const UInt64 pageMask = ~(m_Granularity - 1);
    return (offsetA & pageMask) == (offsetB & pageMask);
}

// ============================================================================
// 放置判定
// ============================================================================

bool FSuballocationRegistry::TryPlaceInNode(UInt32 nodeIndex, UInt64 size,
                                            UInt64 alignment,
                                            ESuballocationType type,
                                            UInt64& outOffset) const
{
    const FNode& node = m_Nodes[nodeIndex];

    if (!node.IsFree() || node.Size < size)
    {
        return false;
    }

    const UInt64 nodeEnd = node.Offset + node.Size;

    UInt64 offset = AlignUpTo(node.Offset, alignment);

    // ------------------------------------------------------------------
    // 粒度约束 — 前侧
    //
    // 若物理前驱与本次分配类型冲突, 且前驱的最后一个字节与候选起点落在
    // 同一粒度页内, 则把起点推到下一个粒度边界。前驱位置固定, 只能移动自己。
    // ------------------------------------------------------------------

    if (m_Granularity > 1 && node.PrevPhysical != kInvalidNode)
    {
        const FNode& prev = m_Nodes[node.PrevPhysical];

        if (IsGranularityConflict(prev.Type, type) && prev.Size > 0)
        {
            const UInt64 prevLastByte = prev.Offset + prev.Size - 1;

            if (IsOnSamePage(prevLastByte, offset))
            {
                offset = AlignUpTo(offset, m_Granularity);
            }
        }
    }

    // 对齐后越界或剩余不足
    if (offset < node.Offset || offset > nodeEnd)
    {
        return false;
    }

    if (nodeEnd - offset < size)
    {
        return false;
    }

    // ------------------------------------------------------------------
    // 粒度约束 — 后侧
    //
    // 若物理后继与本次分配类型冲突, 且本次分配的最后一个字节与后继起点
    // 落在同一粒度页内, 则该空闲区间不可用 —— 后继位置固定, 无法通过
    // 移动本次分配来化解 (向后移动只会让重叠更严重)。
    // ------------------------------------------------------------------

    if (m_Granularity > 1 && node.NextPhysical != kInvalidNode)
    {
        const FNode& next = m_Nodes[node.NextPhysical];

        if (IsGranularityConflict(next.Type, type))
        {
            const UInt64 lastByte = offset + size - 1;

            if (IsOnSamePage(lastByte, next.Offset))
            {
                return false;
            }
        }
    }

    outOffset = offset;
    return true;
}

// ============================================================================
// 分配
// ============================================================================

bool FSuballocationRegistry::Allocate(UInt64 size, UInt64 alignment,
                                      ESuballocationType type,
                                      FSuballocationResult& outResult)
{
    if (size == 0 || type == ESuballocationType::Free || m_TotalSize == 0)
    {
        return false;
    }

    LIMX_ASSERT(alignment > 0);
    LIMX_ASSERT((alignment & (alignment - 1)) == 0);

    UInt32 chosenNode   = kInvalidNode;
    UInt64 chosenOffset = 0;

    // ------------------------------------------------------------------
    // 第一轮: 精确桶内线性探测
    //
    // 该桶内区间尺寸落在 [2^k, 2^(k+1)) 区间, 未必都能容纳本次请求,
    // 因此必须逐个验证而不能取头即用。
    // ------------------------------------------------------------------

    const UInt32 startBucket = ComputeBucket(size);

    for (UInt32 cursor = m_FreeListHeads[startBucket];
         cursor != kInvalidNode;
         cursor = m_Nodes[cursor].NextFree)
    {
        UInt64 candidateOffset = 0;
        if (TryPlaceInNode(cursor, size, alignment, type, candidateOffset))
        {
            chosenNode   = cursor;
            chosenOffset = candidateOffset;
            break;
        }
    }

    // ------------------------------------------------------------------
    // 第二轮: 经位图跳到更高的非空桶
    //
    // 更高桶内的区间尺寸必定大于请求, 但对齐与粒度调整仍可能吃掉空间,
    // 故同样需要逐个验证。
    // ------------------------------------------------------------------

    if (chosenNode == kInvalidNode && startBucket + 1 < kBucketCount)
    {
        const UInt32 higherBucketsMask =
            m_BucketMask & ~((1u << (startBucket + 1)) - 1u);

        UInt32 remaining = higherBucketsMask;

        while (remaining != 0 && chosenNode == kInvalidNode)
        {
            const UInt32 bucket = CountTrailingZeros(remaining);

            for (UInt32 cursor = m_FreeListHeads[bucket];
                 cursor != kInvalidNode;
                 cursor = m_Nodes[cursor].NextFree)
            {
                UInt64 candidateOffset = 0;
                if (TryPlaceInNode(cursor, size, alignment, type,
                                   candidateOffset))
                {
                    chosenNode   = cursor;
                    chosenOffset = candidateOffset;
                    break;
                }
            }

            remaining &= ~(1u << bucket);
        }
    }

    if (chosenNode == kInvalidNode)
    {
        return false;
    }

    // ------------------------------------------------------------------
    // 分裂: [nodeOffset, chosenOffset) | [chosenOffset, +size) | [end, nodeEnd)
    //
    // 复用 chosenNode 承载中段的已分配区间, 前后剩余各起一个新节点。
    // ------------------------------------------------------------------

    UnlinkFree(chosenNode);

    const UInt64 nodeOffset = m_Nodes[chosenNode].Offset;
    const UInt64 nodeEnd    = nodeOffset + m_Nodes[chosenNode].Size;

    const UInt64 frontPadding  = chosenOffset - nodeOffset;
    const UInt64 backRemainder = nodeEnd - (chosenOffset + size);

    // 先取足所需节点槽 —— AcquireNode 可能使 m_Nodes 重分配,
    // 之后一律通过索引访问, 不缓存任何节点引用
    UInt32 frontIndex = kInvalidNode;
    UInt32 backIndex  = kInvalidNode;

    if (frontPadding > 0)
    {
        frontIndex = AcquireNode();
    }

    if (backRemainder > 0)
    {
        backIndex = AcquireNode();
    }

    const UInt32 prevIndex = m_Nodes[chosenNode].PrevPhysical;
    const UInt32 nextIndex = m_Nodes[chosenNode].NextPhysical;

    m_Nodes[chosenNode].Offset   = chosenOffset;
    m_Nodes[chosenNode].Size     = size;
    m_Nodes[chosenNode].Type     = type;
    m_Nodes[chosenNode].PrevFree = kInvalidNode;
    m_Nodes[chosenNode].NextFree = kInvalidNode;

    if (frontIndex != kInvalidNode)
    {
        m_Nodes[frontIndex].Offset       = nodeOffset;
        m_Nodes[frontIndex].Size         = frontPadding;
        m_Nodes[frontIndex].Type         = ESuballocationType::Free;
        m_Nodes[frontIndex].PrevPhysical = prevIndex;
        m_Nodes[frontIndex].NextPhysical = chosenNode;

        m_Nodes[chosenNode].PrevPhysical = frontIndex;

        if (prevIndex != kInvalidNode)
        {
            m_Nodes[prevIndex].NextPhysical = frontIndex;
        }
        else
        {
            m_PhysicalHead = frontIndex;
        }

        LinkFree(frontIndex);
    }

    if (backIndex != kInvalidNode)
    {
        m_Nodes[backIndex].Offset       = chosenOffset + size;
        m_Nodes[backIndex].Size         = backRemainder;
        m_Nodes[backIndex].Type         = ESuballocationType::Free;
        m_Nodes[backIndex].PrevPhysical = chosenNode;
        m_Nodes[backIndex].NextPhysical = nextIndex;

        m_Nodes[chosenNode].NextPhysical = backIndex;

        if (nextIndex != kInvalidNode)
        {
            m_Nodes[nextIndex].PrevPhysical = backIndex;
        }

        LinkFree(backIndex);
    }

    m_UsedSize += size;
    ++m_AllocationCount;

    outResult.Offset    = chosenOffset;
    outResult.Size      = size;
    outResult.NodeIndex = chosenNode;

    return true;
}

// ============================================================================
// 回收
// ============================================================================

void FSuballocationRegistry::Free(UInt32 nodeIndex)
{
    if (nodeIndex >= m_Nodes.GetSize())
    {
        LIMX_ASSERT_MSG(false, "FSuballocationRegistry::Free 收到越界节点索引");
        return;
    }

    if (m_Nodes[nodeIndex].IsFree())
    {
        LIMX_ASSERT_MSG(false, "FSuballocationRegistry::Free 重复释放同一节点");
        return;
    }

    m_UsedSize -= m_Nodes[nodeIndex].Size;
    --m_AllocationCount;

    m_Nodes[nodeIndex].Type = ESuballocationType::Free;

    // ------------------------------------------------------------------
    // 与后继合并 — 先做后继, 使前驱合并时只需处理一次链接改写
    // ------------------------------------------------------------------

    const UInt32 nextIndex = m_Nodes[nodeIndex].NextPhysical;

    if (nextIndex != kInvalidNode && m_Nodes[nextIndex].IsFree())
    {
        UnlinkFree(nextIndex);

        m_Nodes[nodeIndex].Size += m_Nodes[nextIndex].Size;

        const UInt32 nextNext = m_Nodes[nextIndex].NextPhysical;
        m_Nodes[nodeIndex].NextPhysical = nextNext;

        if (nextNext != kInvalidNode)
        {
            m_Nodes[nextNext].PrevPhysical = nodeIndex;
        }

        ReleaseNode(nextIndex);
    }

    // ------------------------------------------------------------------
    // 与前驱合并 — 合并后由前驱承载整段, 当前节点槽回收
    // ------------------------------------------------------------------

    UInt32 mergedIndex = nodeIndex;
    const UInt32 prevIndex = m_Nodes[nodeIndex].PrevPhysical;

    if (prevIndex != kInvalidNode && m_Nodes[prevIndex].IsFree())
    {
        UnlinkFree(prevIndex);

        m_Nodes[prevIndex].Size += m_Nodes[nodeIndex].Size;

        const UInt32 tailIndex = m_Nodes[nodeIndex].NextPhysical;
        m_Nodes[prevIndex].NextPhysical = tailIndex;

        if (tailIndex != kInvalidNode)
        {
            m_Nodes[tailIndex].PrevPhysical = prevIndex;
        }

        ReleaseNode(nodeIndex);
        mergedIndex = prevIndex;
    }

    LinkFree(mergedIndex);
}

// ============================================================================
// 查询
// ============================================================================

UInt64 FSuballocationRegistry::GetLargestFreeRegion() const
{
    UInt64 largest = 0;

    for (UInt32 bucket = 0; bucket < kBucketCount; ++bucket)
    {
        for (UInt32 cursor = m_FreeListHeads[bucket];
             cursor != kInvalidNode;
             cursor = m_Nodes[cursor].NextFree)
        {
            if (m_Nodes[cursor].Size > largest)
            {
                largest = m_Nodes[cursor].Size;
            }
        }
    }

    return largest;
}

UInt64 FSuballocationRegistry::GetNodeOffset(UInt32 nodeIndex) const
{
    if (nodeIndex >= m_Nodes.GetSize())
    {
        return 0;
    }

    return m_Nodes[nodeIndex].Offset;
}

UInt64 FSuballocationRegistry::GetNodeSize(UInt32 nodeIndex) const
{
    if (nodeIndex >= m_Nodes.GetSize())
    {
        return 0;
    }

    return m_Nodes[nodeIndex].Size;
}

// ============================================================================
// 自检
// ============================================================================

bool FSuballocationRegistry::Validate() const
{
    if (m_TotalSize == 0)
    {
        // 未初始化状态下物理链必须为空
        return m_PhysicalHead == kInvalidNode;
    }

    // ------------------------------------------------------------------
    // 1-3. 沿物理链遍历: 首尾相接、总和守恒、无相邻空闲
    // ------------------------------------------------------------------

    UInt64 expectedOffset = 0;
    UInt64 totalSpan      = 0;
    UInt64 usedSpan       = 0;
    UInt32 allocCount     = 0;
    UInt32 visited        = 0;

    UInt32 cursor   = m_PhysicalHead;
    UInt32 previous = kInvalidNode;

    if (cursor != kInvalidNode && m_Nodes[cursor].PrevPhysical != kInvalidNode)
    {
        return false;   // 链首不应有前驱
    }

    while (cursor != kInvalidNode)
    {
        const FNode& node = m_Nodes[cursor];

        if (node.Size == 0)
        {
            return false;   // 不允许零长区间
        }

        if (node.Offset != expectedOffset)
        {
            return false;   // 区间之间出现空隙或重叠
        }

        if (node.PrevPhysical != previous)
        {
            return false;   // 物理链前后不一致
        }

        if (previous != kInvalidNode &&
            m_Nodes[previous].IsFree() && node.IsFree())
        {
            return false;   // 相邻空闲区间未合并
        }

        if (node.IsFree())
        {
            // 空闲节点必须在自由链表中可达 —— 由第 4 步反向校验
        }
        else
        {
            usedSpan += node.Size;
            ++allocCount;
        }

        expectedOffset += node.Size;
        totalSpan      += node.Size;

        previous = cursor;
        cursor   = node.NextPhysical;

        if (++visited > m_Nodes.GetSize() + 1)
        {
            return false;   // 物理链成环
        }
    }

    if (totalSpan != m_TotalSize)
    {
        return false;
    }

    if (usedSpan != m_UsedSize || allocCount != m_AllocationCount)
    {
        return false;
    }

    // ------------------------------------------------------------------
    // 4-5. 自由链表与位图一致性
    // ------------------------------------------------------------------

    UInt32 freeNodesInLists = 0;

    for (UInt32 bucket = 0; bucket < kBucketCount; ++bucket)
    {
        const bool maskSaysNonEmpty = (m_BucketMask & (1u << bucket)) != 0;
        const bool listIsNonEmpty   = m_FreeListHeads[bucket] != kInvalidNode;

        if (maskSaysNonEmpty != listIsNonEmpty)
        {
            return false;   // 位图与链表状态不符
        }

        UInt32 chainCursor = m_FreeListHeads[bucket];
        UInt32 chainPrev   = kInvalidNode;
        UInt32 chainLength = 0;

        while (chainCursor != kInvalidNode)
        {
            const FNode& node = m_Nodes[chainCursor];

            if (!node.IsFree())
            {
                return false;   // 自由链表中混入已分配节点
            }

            if (ComputeBucket(node.Size) != bucket)
            {
                return false;   // 节点落在错误的分桶
            }

            if (node.PrevFree != chainPrev)
            {
                return false;   // 自由链表双向不一致
            }

            chainPrev   = chainCursor;
            chainCursor = node.NextFree;

            ++freeNodesInLists;

            if (++chainLength > m_Nodes.GetSize() + 1)
            {
                return false;   // 自由链表成环
            }
        }
    }

    // ------------------------------------------------------------------
    // 6. 物理链上的空闲节点数应与自由链表中的总数一致
    // ------------------------------------------------------------------

    UInt32 freeNodesOnChain = 0;

    for (UInt32 walk = m_PhysicalHead;
         walk != kInvalidNode;
         walk = m_Nodes[walk].NextPhysical)
    {
        if (m_Nodes[walk].IsFree())
        {
            ++freeNodesOnChain;
        }
    }

    if (freeNodesOnChain != freeNodesInLists)
    {
        return false;
    }

    return true;
}

} // namespace Limx
