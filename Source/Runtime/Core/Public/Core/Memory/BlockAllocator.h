/*******************************************************************************
 * 文件: BlockAllocator.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   固定块池分配器 — O(1) 分配/释放的对象池
 *   预分配固定大小的内存块池，通过空闲链表管理
 *   适用于频繁创建/销毁相同大小对象的场景 (粒子、组件、节点等)
 *
 * 设计哲学:
 *   O(1) 分配/释放 — 空闲链表头部弹出/压入
 *   零碎片 — 所有块大小相同，天然无外部碎片
 *   页式扩展 — 当空闲块耗尽时分配新的页 (Page)
 *   IAllocator 兼容 — 实现 IAllocator 接口，可作为通用分配器使用
 *
 * 技术特性:
 *   - 块大小: 构造时指定，运行时不变
 *   - 空闲链表: 侵入式单链表 (利用空闲块自身存储 next 指针)
 *   - 页管理: 每页包含固定数量的块，页链表管理所有已分配页
 *   - 线程安全: 非线程安全，需外部同步
 *
 * 依赖关系:
 *   内部: Core/Memory/IAllocator.h, Core/Memory/DefaultAllocator.h,
 *          Core/Memory/MemoryOps.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Memory/IAllocator.h"
#include "Core/Memory/DefaultAllocator.h"
#include "Core/Memory/MemoryOps.h"

namespace Limx
{

/// 固定块池分配器
class BlockAllocator final : public IAllocator
{
    /// 空闲块链表节点 (侵入式 — 利用空闲块自身内存)
    struct FreeBlock
    {
        FreeBlock* Next;
    };

    /// 页头 — 追踪所有已分配的页
    struct PageHeader
    {
        PageHeader* NextPage;
    };

    /// 默认每页块数
    static constexpr SizeType kDefaultBlocksPerPage = 64;

public:
    // ========================================================================
    // 构造与析构
    // ========================================================================

    /// 构造块分配器
    /// @param blockSize      每个块的字节数 (至少 sizeof(void*))
    /// @param blocksPerPage  每页的块数
    /// @param parentAllocator 父分配器 (用于分配页)
    explicit BlockAllocator(SizeType blockSize,
                            SizeType blocksPerPage = kDefaultBlocksPerPage,
                            IAllocator* parentAllocator = nullptr)
        : m_BlockSize(blockSize < sizeof(FreeBlock) ?
                      sizeof(FreeBlock) : blockSize)
        , m_BlocksPerPage(blocksPerPage)
        , m_FreeList(nullptr)
        , m_PageList(nullptr)
        , m_TotalAllocated(0)
        , m_TotalFree(0)
        , m_Parent(parentAllocator ?
                   parentAllocator : &GetDefaultAllocator())
    {
        LIMX_ASSERT(blockSize > 0);
        LIMX_ASSERT(blocksPerPage > 0);
    }

    ~BlockAllocator() override
    {
        FreeAllPages();
    }

    // 不可拷贝/移动
    BlockAllocator(const BlockAllocator&) = delete;
    BlockAllocator& operator=(const BlockAllocator&) = delete;

    // ========================================================================
    // IAllocator 接口
    // ========================================================================

    void* Allocate(SizeType size, SizeType alignment) override
    {
        (void)alignment;  // 块分配器忽略对齐参数
        LIMX_ASSERT(size <= m_BlockSize);

        if (!m_FreeList)
        {
            AllocateNewPage();
        }

        // 从空闲链表头部弹出
        FreeBlock* block = m_FreeList;
        m_FreeList = block->Next;
        m_TotalFree--;
        m_TotalAllocated++;

        return static_cast<void*>(block);
    }

    void Deallocate(void* pointer) override
    {
        if (!pointer)
        {
            return;
        }

        // 压入空闲链表头部
        FreeBlock* block = static_cast<FreeBlock*>(pointer);
        block->Next = m_FreeList;
        m_FreeList = block;
        m_TotalFree++;
        m_TotalAllocated--;
    }

    // ========================================================================
    // 状态查询
    // ========================================================================

    /// 每块字节数
    LIMX_NODISCARD SizeType GetBlockSize() const { return m_BlockSize; }

    /// 当前已分配块数
    LIMX_NODISCARD SizeType GetAllocatedCount() const
    {
        return m_TotalAllocated;
    }

    /// 当前空闲块数
    LIMX_NODISCARD SizeType GetFreeCount() const { return m_TotalFree; }

    /// 释放所有页 — 所有已分配的块指针失效
    void Reset()
    {
        FreeAllPages();
        m_FreeList = nullptr;
        m_TotalAllocated = 0;
        m_TotalFree = 0;
    }

private:
    // ========================================================================
    // 页管理
    // ========================================================================

    /// 分配新页并将所有块加入空闲链表
    void AllocateNewPage()
    {
        // 页总大小 = 页头 + (块大小 * 每页块数)
        SizeType pageDataSize = m_BlockSize * m_BlocksPerPage;
        SizeType totalPageSize = sizeof(PageHeader) + pageDataSize;

        void* rawPage = m_Parent->Allocate(totalPageSize, alignof(void*));
        LIMX_ASSERT(rawPage != nullptr);

        // 设置页头
        PageHeader* page = static_cast<PageHeader*>(rawPage);
        page->NextPage = m_PageList;
        m_PageList = page;

        // 将页中的所有块链入空闲链表
        UInt8* blockStart = reinterpret_cast<UInt8*>(page) +
                            sizeof(PageHeader);
        for (SizeType index = 0; index < m_BlocksPerPage; ++index)
        {
            FreeBlock* block = reinterpret_cast<FreeBlock*>(
                blockStart + index * m_BlockSize);
            block->Next = m_FreeList;
            m_FreeList = block;
        }
        m_TotalFree += m_BlocksPerPage;
    }

    /// 释放所有页
    void FreeAllPages()
    {
        PageHeader* current = m_PageList;
        while (current)
        {
            PageHeader* next = current->NextPage;
            m_Parent->Deallocate(current);
            current = next;
        }
        m_PageList = nullptr;
    }

    // ========================================================================
    // 成员数据
    // ========================================================================

    SizeType    m_BlockSize;        ///< 每块字节数
    SizeType    m_BlocksPerPage;    ///< 每页块数
    FreeBlock*  m_FreeList;         ///< 空闲块链表头
    PageHeader* m_PageList;         ///< 页链表头
    SizeType    m_TotalAllocated;   ///< 已分配块数
    SizeType    m_TotalFree;        ///< 空闲块数
    IAllocator* m_Parent;           ///< 父分配器
};

} // namespace Limx
