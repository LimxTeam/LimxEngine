/*******************************************************************************
 * 文件: LinearAllocator.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   线性分配器 (帧分配器) — 极速顺序分配，批量重置
 *   从预分配的连续内存块中线性推进指针分配
 *   不支持单独释放，仅支持整体 Reset (将指针归零)
 *   适用于每帧临时数据、命令缓冲区、渲染暂存区等场景
 *
 * 设计哲学:
 *   O(1) 分配 — 仅指针递增 + 对齐调整，无链表/哈希开销
 *   批量重置 — Reset() 将指针归零，不逐一析构
 *   零碎片 — 连续分配，无外部碎片
 *   固定容量 — 构造时确定容量，不自动扩展
 *
 * 技术特性:
 *   - Allocate: 对齐指针递增
 *   - Deallocate: 空操作 (不支持单独释放)
 *   - Reset: 将偏移归零，重用全部内存
 *   - GetUsed/GetRemaining: 使用量查询
 *
 * 依赖关系:
 *   内部: Core/Memory/IAllocator.h, Core/Memory/DefaultAllocator.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Memory/IAllocator.h"
#include "Core/Memory/DefaultAllocator.h"

namespace Limx
{

/// 线性分配器 — 极速顺序分配，批量重置
class LinearAllocator final : public IAllocator
{
public:
    // ========================================================================
    // 构造与析构
    // ========================================================================

    /// 构造指定容量的线性分配器
    /// @param capacity       总容量 (字节)
    /// @param parentAllocator 父分配器 (用于分配底层缓冲区)
    explicit LinearAllocator(SizeType capacity,
                             IAllocator* parentAllocator = nullptr)
        : m_Buffer(nullptr)
        , m_Capacity(capacity)
        , m_Offset(0)
        , m_LastAllocationOffset(kSizeTypeMax)
        , m_LastAllocationSize(0)
        , m_Parent(parentAllocator ?
                   parentAllocator : &GetDefaultAllocator())
    {
        LIMX_ASSERT(capacity > 0);
        m_Buffer = static_cast<UInt8*>(
            m_Parent->Allocate(capacity, alignof(void*)));
        LIMX_ASSERT(m_Buffer != nullptr);
    }

    ~LinearAllocator() override
    {
        if (m_Buffer)
        {
            m_Parent->Deallocate(m_Buffer);
        }
    }

    // 不可拷贝
    LinearAllocator(const LinearAllocator&) = delete;
    LinearAllocator& operator=(const LinearAllocator&) = delete;

    // 可移动
    LinearAllocator(LinearAllocator&& other) noexcept
        : m_Buffer(other.m_Buffer)
        , m_Capacity(other.m_Capacity)
        , m_Offset(other.m_Offset)
        , m_LastAllocationOffset(other.m_LastAllocationOffset)
        , m_LastAllocationSize(other.m_LastAllocationSize)
        , m_Parent(other.m_Parent)
    {
        other.m_Buffer = nullptr;
        other.m_Capacity = 0;
        other.m_Offset = 0;
        other.m_LastAllocationOffset = kSizeTypeMax;
        other.m_LastAllocationSize = 0;
    }

    // ========================================================================
    // IAllocator 接口
    // ========================================================================

    void* Allocate(SizeType size, SizeType alignment) override
    {
        if (size == 0)
        {
            return nullptr;
        }

        // 必须按绝对地址对齐, 而非按缓冲区内偏移对齐 —
        // 缓冲区自身的对齐不一定覆盖调用方请求的对齐 (见 AlignUpAddress 说明)
        SizeType alignedOffset = AlignUpAddress(m_Offset, alignment);

        // 容量不足 — 这是 IAllocator 契约中的正常失败路径而非程序错误,
        // 因此返回 nullptr 让调用方决策, 不做断言中断
        if (alignedOffset + size > m_Capacity)
        {
            return nullptr;
        }

        void* pointer = m_Buffer + alignedOffset;

        // 记录本次分配的位置与尺寸 — Reallocate 借此判断能否原地扩展
        m_LastAllocationOffset = alignedOffset;
        m_LastAllocationSize   = size;

        m_Offset = alignedOffset + size;
        return pointer;
    }

    void Deallocate(void* /*pointer*/) override
    {
        // 线性分配器不支持单独释放 — 空操作
    }

    /// 重分配 — 仅支持扩展最近一次分配
    ///
    /// 线性分配器不记录每块的尺寸, 因此对任意历史指针无从得知原有字节数,
    /// 也就无法安全搬迁数据。但"扩展刚分配的那一块"是构建缓冲区时的常见
    /// 模式且可精确处理: 该块位于已用区末尾, 直接向后延伸即可, 零拷贝。
    ///
    /// 其余情况按 IAllocator 契约返回 nullptr 并保持原内存不变 — 这是
    /// 线性分配语义的固有限制, 调用方应改用支持重分配的分配器。
    LIMX_NODISCARD void* Reallocate(
        void* pointer,
        SizeType newSize,
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

        // 仅当目标恰为最近一次分配时, 才知道它的原始尺寸与边界
        const bool isLastAllocation =
            (m_LastAllocationOffset != kSizeTypeMax) &&
            (pointer == m_Buffer + m_LastAllocationOffset);

        if (!isLastAllocation)
        {
            return nullptr;
        }

        if (m_LastAllocationOffset + newSize > m_Capacity)
        {
            // 延伸后越界 — 原内存保持有效
            return nullptr;
        }

        // 原地伸缩: 数据不动, 只调整分配游标
        m_LastAllocationSize = newSize;
        m_Offset             = m_LastAllocationOffset + newSize;

        return pointer;
    }

    LIMX_NODISCARD const AnsiChar* GetName() const override
    {
        return "LinearAllocator";
    }

    // ========================================================================
    // 重置与状态
    // ========================================================================

    /// 重置 — 将偏移归零，重用全部内存
    /// 调用者负责确保所有之前分配的对象已不再使用
    void Reset()
    {
        m_Offset = 0;

        // 游标回退后原先的"最近一次分配"不再位于已用区末尾, 必须失效,
        // 否则后续 Reallocate 会基于陈旧偏移改写已被重新分配出去的区域
        m_LastAllocationOffset = kSizeTypeMax;
        m_LastAllocationSize   = 0;
    }

    /// 已使用字节数
    LIMX_NODISCARD SizeType GetUsed() const { return m_Offset; }

    /// 剩余可用字节数
    LIMX_NODISCARD SizeType GetRemaining() const
    {
        return m_Capacity - m_Offset;
    }

    /// 总容量
    LIMX_NODISCARD SizeType GetCapacity() const { return m_Capacity; }

    /// 保存当前偏移位置 (用于局部回滚)
    LIMX_NODISCARD SizeType SaveMark() const { return m_Offset; }

    /// 回滚到之前保存的位置
    void RestoreMark(SizeType mark)
    {
        LIMX_ASSERT(mark <= m_Offset);
        m_Offset = mark;

        // 同 Reset: 回退越过了最近一次分配则该记录失效
        if (m_LastAllocationOffset == kSizeTypeMax ||
            m_LastAllocationOffset >= mark)
        {
            m_LastAllocationOffset = kSizeTypeMax;
            m_LastAllocationSize   = 0;
        }
    }

private:
    /// 将缓冲区内偏移向上调整, 使 m_Buffer + 返回值 满足绝对地址对齐
    ///
    /// 底层缓冲区按 alignof(void*) 申请, 只保证 8 字节对齐。若直接把偏移
    /// 向上取整到 alignment, 当 alignment 超过缓冲区自身对齐 (AVX 的 32、
    /// AVX-512 的 64) 时, m_Buffer + alignedOffset 并不满足请求对齐,
    /// 调用方用对齐加载指令访问会崩溃。因此对齐运算必须落在绝对地址上。
    LIMX_NODISCARD SizeType AlignUpAddress(SizeType offset,
                                           SizeType alignment) const
    {
        const UInt64 baseAddress = reinterpret_cast<UInt64>(m_Buffer);
        const UInt64 currentAddress = baseAddress + static_cast<UInt64>(offset);
        const UInt64 alignmentMask = static_cast<UInt64>(alignment) - 1;
        const UInt64 alignedAddress =
            (currentAddress + alignmentMask) & ~alignmentMask;

        return static_cast<SizeType>(alignedAddress - baseAddress);
    }

    // ========================================================================
    // 成员数据
    // ========================================================================

    UInt8*      m_Buffer;    ///< 底层连续缓冲区
    SizeType    m_Capacity;  ///< 总容量 (字节)
    SizeType    m_Offset;    ///< 当前分配偏移

    /// 最近一次分配的起始偏移 — kSizeTypeMax 表示无有效记录
    SizeType    m_LastAllocationOffset;

    /// 最近一次分配的字节数
    SizeType    m_LastAllocationSize;

    IAllocator* m_Parent;    ///< 父分配器
};

} // namespace Limx
