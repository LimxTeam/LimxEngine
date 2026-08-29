/*******************************************************************************
 * 文件: FTrackingAllocator.h
 * 创建时间: 2026-08-29
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   追踪型分配器 — 包装任意 IAllocator 并统计分配/释放次数与字节数
 *   用于在单元测试中验证容器、智能指针等设施是否成对释放了全部内存
 *   提供峰值占用与当前占用，可用于回归检测意外的内存膨胀
 *
 * 设计哲学:
 *   头部内联记账 — 在每次分配的用户指针之前藏一个 FAllocationHeader，
 *   记录原始请求大小与到基址的偏移。释放时无需查表即可还原大小并回退基址，
 *   因此追踪本身不产生额外分配，不会与被测分配器互相干扰。
 *
 *   包装而非替换 — 追踪器不实现分配策略，只转发给底层分配器。这样被测代码
 *   走的仍是真实的分配路径，测出的行为与生产环境一致。
 *
 * 技术特性:
 *   - Allocate/Deallocate 均为 O(1)，无查表、无锁
 *   - 头部占用 2 * SizeType，通过提升对齐保证不破坏用户请求的对齐语义
 *   - 统计项: 分配次数/释放次数/失败次数/当前字节/峰值字节/累计字节
 *   - FLeakScope 提供作用域级泄漏断言基线
 *
 * 依赖关系:
 *   内部: Core/Memory/IAllocator.h, Core/Memory/DefaultAllocator.h,
 *          Core/Memory/MemoryOps.h, Testing/TestingAPI.h
 *
 * 注意事项:
 *   非线程安全 — 统计字段为普通整数，多线程测试需各线程独立实例
 *   实际向底层申请的字节数大于用户请求 (多出一个对齐单位)，
 *   因此 GetCurrentBytes 反映的是用户视角的请求量而非物理占用量
 *
 ******************************************************************************/

#pragma once

#include "Core/CoreMinimal.h"
#include "Core/Memory/IAllocator.h"
#include "Core/Memory/DefaultAllocator.h"
#include "Core/Memory/MemoryOps.h"
#include "Testing/TestingAPI.h"

namespace Limx
{

// ============================================================================
// FTrackingAllocator — 统计包装分配器
// ============================================================================

/// 追踪分配器 — 转发到底层分配器并记录全部分配活动
class LIMX_TESTING_API FTrackingAllocator final : public IAllocator
{
public:
    /// @param underlying 实际执行分配的底层分配器
    explicit FTrackingAllocator(IAllocator& underlying = GetDefaultAllocator())
        : m_Underlying(underlying)
    {
    }

    ~FTrackingAllocator() override = default;

    // ========================================================================
    // IAllocator 实现
    // ========================================================================

    LIMX_NODISCARD void* Allocate(SizeType size,
                                  SizeType alignment = kDefaultAlignment) override
    {
        if (size == 0)
        {
            return nullptr;
        }

        // 头部必须完整放入用户指针之前的空隙, 故对齐不得小于头部尺寸
        const SizeType effectiveAlignment =
            (alignment < sizeof(FAllocationHeader)) ? sizeof(FAllocationHeader)
                                                    : alignment;

        void* base = m_Underlying.Allocate(size + effectiveAlignment,
                                           effectiveAlignment);
        if (base == nullptr)
        {
            ++m_FailedAllocations;
            return nullptr;
        }

        // 用户指针后移一个对齐单位, 空出的区间末尾安放头部
        UInt8* userPointer = static_cast<UInt8*>(base) + effectiveAlignment;

        FAllocationHeader* header =
            reinterpret_cast<FAllocationHeader*>(userPointer) - 1;
        header->Size       = size;
        header->BaseOffset = effectiveAlignment;

        ++m_AllocationCount;
        m_CurrentBytes += size;
        m_TotalBytes   += size;

        if (m_CurrentBytes > m_PeakBytes)
        {
            m_PeakBytes = m_CurrentBytes;
        }

        return userPointer;
    }

    void Deallocate(void* pointer) override
    {
        if (pointer == nullptr)
        {
            return;
        }

        const FAllocationHeader* header =
            reinterpret_cast<const FAllocationHeader*>(pointer) - 1;

        const SizeType size       = header->Size;
        const SizeType baseOffset = header->BaseOffset;

        ++m_DeallocationCount;

        // 释放量不应超过当前占用 — 超出说明出现了重复释放或头部损坏
        LIMX_ASSERT(m_CurrentBytes >= size);
        m_CurrentBytes -= size;

        void* base = static_cast<UInt8*>(pointer) - baseOffset;
        m_Underlying.Deallocate(base);
    }

    LIMX_NODISCARD void* Reallocate(void* pointer, SizeType newSize,
                                    SizeType alignment = kDefaultAlignment) override
    {
        if (pointer == nullptr)
        {
            return Allocate(newSize, alignment);
        }

        if (newSize == 0)
        {
            Deallocate(pointer);
            return nullptr;
        }

        const FAllocationHeader* header =
            reinterpret_cast<const FAllocationHeader*>(pointer) - 1;
        const SizeType oldSize = header->Size;

        void* newPointer = Allocate(newSize, alignment);
        if (newPointer == nullptr)
        {
            // 分配失败时保持原内存不变, 符合 IAllocator 契约
            return nullptr;
        }

        const SizeType copySize = (oldSize < newSize) ? oldSize : newSize;
        Memory::MemCopy(newPointer, pointer, copySize);

        Deallocate(pointer);
        return newPointer;
    }

    LIMX_NODISCARD SizeType GetAllocationSize(void* pointer) const override
    {
        if (pointer == nullptr)
        {
            return 0;
        }

        const FAllocationHeader* header =
            reinterpret_cast<const FAllocationHeader*>(pointer) - 1;
        return header->Size;
    }

    LIMX_NODISCARD const AnsiChar* GetName() const override
    {
        return "FTrackingAllocator";
    }

    // ========================================================================
    // 统计查询
    // ========================================================================

    /// 累计分配次数
    LIMX_NODISCARD UInt64 GetAllocationCount() const { return m_AllocationCount; }

    /// 累计释放次数
    LIMX_NODISCARD UInt64 GetDeallocationCount() const { return m_DeallocationCount; }

    /// 分配失败次数
    LIMX_NODISCARD UInt64 GetFailedAllocationCount() const { return m_FailedAllocations; }

    /// 尚未释放的分配数 — 非零即存在泄漏
    LIMX_NODISCARD UInt64 GetLiveAllocationCount() const
    {
        return m_AllocationCount - m_DeallocationCount;
    }

    /// 当前占用字节 (用户请求口径)
    LIMX_NODISCARD SizeType GetCurrentBytes() const { return m_CurrentBytes; }

    /// 历史峰值占用字节
    LIMX_NODISCARD SizeType GetPeakBytes() const { return m_PeakBytes; }

    /// 累计分配字节 (不含释放抵扣)
    LIMX_NODISCARD SizeType GetTotalBytes() const { return m_TotalBytes; }

    /// 是否存在未释放的分配
    LIMX_NODISCARD bool HasLeaks() const
    {
        return m_AllocationCount != m_DeallocationCount;
    }

    /// 清空全部统计 — 不影响已分配内存, 仅重置计数基线
    void ResetStatistics()
    {
        m_AllocationCount   = 0;
        m_DeallocationCount = 0;
        m_FailedAllocations = 0;
        m_CurrentBytes      = 0;
        m_PeakBytes         = 0;
        m_TotalBytes        = 0;
    }

private:
    /// 藏在用户指针之前的记账头
    struct FAllocationHeader
    {
        /// 用户请求的字节数
        SizeType Size;

        /// 用户指针回退到底层基址的字节偏移
        SizeType BaseOffset;
    };

    /// 实际执行分配的底层分配器
    IAllocator& m_Underlying;

    UInt64 m_AllocationCount   = 0;
    UInt64 m_DeallocationCount = 0;
    UInt64 m_FailedAllocations = 0;

    SizeType m_CurrentBytes = 0;
    SizeType m_PeakBytes    = 0;
    SizeType m_TotalBytes   = 0;
};

// ============================================================================
// FLeakScope — 作用域泄漏基线
// ============================================================================

/// 记录进入作用域时的分配基线，用于断言一段代码前后分配数守恒
///
/// 用法:
///     FTrackingAllocator allocator;
///     {
///         FLeakScope scope(allocator);
///         TArray<Int32> values(allocator);
///         values.Add(1);
///         // 离开作用域前 values 析构, 分配数应回到基线
///     }
class FLeakScope
{
public:
    explicit FLeakScope(const FTrackingAllocator& allocator)
        : m_Allocator(allocator)
        , m_BaselineLiveCount(allocator.GetLiveAllocationCount())
        , m_BaselineBytes(allocator.GetCurrentBytes())
    {
    }

    FLeakScope(const FLeakScope&)            = delete;
    FLeakScope& operator=(const FLeakScope&) = delete;

    /// 相对基线新增的未释放分配数 — 0 表示无泄漏
    LIMX_NODISCARD Int64 GetLeakedAllocationCount() const
    {
        return static_cast<Int64>(m_Allocator.GetLiveAllocationCount()) -
               static_cast<Int64>(m_BaselineLiveCount);
    }

    /// 相对基线新增的未释放字节数
    LIMX_NODISCARD Int64 GetLeakedBytes() const
    {
        return static_cast<Int64>(m_Allocator.GetCurrentBytes()) -
               static_cast<Int64>(m_BaselineBytes);
    }

    /// 相对基线是否存在泄漏
    LIMX_NODISCARD bool HasLeaked() const
    {
        return GetLeakedAllocationCount() != 0;
    }

private:
    const FTrackingAllocator& m_Allocator;

    UInt64   m_BaselineLiveCount = 0;
    SizeType m_BaselineBytes     = 0;
};

} // namespace Limx
