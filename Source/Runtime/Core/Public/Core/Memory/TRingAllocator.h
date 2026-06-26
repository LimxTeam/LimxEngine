/*******************************************************************************
 * 文件: TRingAllocator.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   环形分配器 — 循环覆写的帧级暂时内存
 *   固定大小内存环，每帧重置偏移量，适合单帧有效的临时分配
 *   用于帧常量缓冲区、临时渲染数据、每帧字符串格式化等场景
 *
 * 设计哲学:
 *   零释放 — 无需 Deallocate，整体 Reset 即可
 *   线性推进 — 指针单向推进，超出时回绕到头部
 *   对齐保证 — 每次分配满足指定对齐
 *
 * 技术特性:
 *   - TRingAllocator: 环形内存分配器
 *   - Allocate: 分配对齐内存
 *   - Reset: 重置到帧起始
 *   - GetUsed/GetCapacity: 查询
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h,
 *          Core/Memory/DefaultAllocator.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Memory/DefaultAllocator.h"

namespace Limx
{

/// 环形帧内存分配器
class TRingAllocator
{
public:
    /// 构造
    /// @param capacityBytes 环形缓冲区容量 (字节)
    explicit TRingAllocator(SizeType capacityBytes)
        : m_Capacity(capacityBytes)
        , m_Offset(0)
    {
        LIMX_ASSERT(capacityBytes > 0);
        m_Buffer = static_cast<UInt8*>(
            GetDefaultAllocator().Allocate(
                capacityBytes, 16));
    }

    ~TRingAllocator()
    {
        if (m_Buffer != nullptr)
        {
            GetDefaultAllocator().Deallocate(m_Buffer);
            m_Buffer = nullptr;
        }
    }

    // 不可拷贝
    TRingAllocator(const TRingAllocator&) = delete;
    TRingAllocator& operator=(
        const TRingAllocator&) = delete;

    // ========================================================================
    // 分配
    // ========================================================================

    /// 分配对齐内存块
    /// @param sizeBytes 大小 (字节)
    /// @param alignment 对齐 (必须为 2 的幂)
    /// @return 内存指针，容量不足时回绕；若单次分配超出容量返回 nullptr
    LIMX_NODISCARD void* Allocate(SizeType sizeBytes,
                                   SizeType alignment = 16)
    {
        LIMX_ASSERT((alignment & (alignment - 1)) == 0);
        if (sizeBytes > m_Capacity) return nullptr;

        // 对齐当前偏移
        SizeType alignedOffset =
            (m_Offset + alignment - 1) & ~(alignment - 1);

        // 是否需要回绕
        if (alignedOffset + sizeBytes > m_Capacity)
        {
            alignedOffset = 0;
        }

        m_Offset = alignedOffset + sizeBytes;
        return static_cast<void*>(
            m_Buffer + alignedOffset);
    }

    /// 类型化分配 (便捷封装)
    template<typename T>
    LIMX_NODISCARD T* AllocateTyped(SizeType count = 1)
    {
        return static_cast<T*>(
            Allocate(sizeof(T) * count, alignof(T)));
    }

    // ========================================================================
    // 重置
    // ========================================================================

    /// 重置偏移量 (帧开始时调用)
    void Reset()
    {
        m_Offset = 0;
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 当前已使用字节数
    LIMX_NODISCARD SizeType GetUsed() const
    {
        return m_Offset;
    }

    /// 总容量
    LIMX_NODISCARD SizeType GetCapacity() const
    {
        return m_Capacity;
    }

    /// 剩余可用字节数
    LIMX_NODISCARD SizeType GetRemaining() const
    {
        return m_Capacity - m_Offset;
    }

    /// 使用率 [0.0, 1.0]
    LIMX_NODISCARD Float32 GetUsageRatio() const
    {
        return static_cast<Float32>(m_Offset) /
               static_cast<Float32>(m_Capacity);
    }

private:
    UInt8*   m_Buffer;    ///< 内存缓冲区
    SizeType m_Capacity;  ///< 容量 (字节)
    SizeType m_Offset;    ///< 当前写入偏移
};

} // namespace Limx
