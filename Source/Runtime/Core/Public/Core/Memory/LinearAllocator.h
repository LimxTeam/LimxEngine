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
        , m_Parent(other.m_Parent)
    {
        other.m_Buffer = nullptr;
        other.m_Capacity = 0;
        other.m_Offset = 0;
    }

    // ========================================================================
    // IAllocator 接口
    // ========================================================================

    void* Allocate(SizeType size, SizeType alignment) override
    {
        // 对齐偏移
        SizeType alignedOffset = AlignUp(m_Offset, alignment);

        if (alignedOffset + size > m_Capacity)
        {
            // 容量不足
            LIMX_ASSERT(false);
            return nullptr;
        }

        void* pointer = m_Buffer + alignedOffset;
        m_Offset = alignedOffset + size;
        return pointer;
    }

    void Deallocate(void* /*pointer*/) override
    {
        // 线性分配器不支持单独释放 — 空操作
    }

    // ========================================================================
    // 重置与状态
    // ========================================================================

    /// 重置 — 将偏移归零，重用全部内存
    /// 调用者负责确保所有之前分配的对象已不再使用
    void Reset()
    {
        m_Offset = 0;
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
    }

private:
    /// 向上对齐
    static SizeType AlignUp(SizeType offset, SizeType alignment)
    {
        SizeType mask = alignment - 1;
        return (offset + mask) & ~mask;
    }

    // ========================================================================
    // 成员数据
    // ========================================================================

    UInt8*      m_Buffer;    ///< 底层连续缓冲区
    SizeType    m_Capacity;  ///< 总容量 (字节)
    SizeType    m_Offset;    ///< 当前分配偏移
    IAllocator* m_Parent;    ///< 父分配器
};

} // namespace Limx
