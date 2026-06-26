/*******************************************************************************
 * 文件: FStackAllocator.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   栈分配器 — LIFO 顺序分配/释放，支持作用域标记 (Marker)
 *   从预分配缓冲区的栈顶分配，通过回退标记批量释放
 *   用于帧临时数据、递归算法工作内存、渲染提交缓冲区等场景
 *
 * 设计哲学:
 *   LIFO 顺序 — 后分配的先释放 (通过 Marker 回退)
 *   O(1) 分配 — 移动栈顶指针即可，无碎片
 *   批量释放 — FreeToMarker 一次回退到标记位置
 *   无逐个释放 — 不支持单独释放某次分配
 *
 * 技术特性:
 *   - Allocate(size, alignment): O(1) 对齐分配
 *   - GetMarker(): 获取当前栈顶标记
 *   - FreeToMarker(marker): 回退到标记位置
 *   - Reset(): 回退到起点
 *   - FScopeMarker: RAII 作用域标记 (离开作用域自动回退)
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h,
 *          Core/Memory/IAllocator.h, Core/Memory/DefaultAllocator.h
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

/// 栈标记 — 记录栈顶位置
using StackMarker = SizeType;

/// 栈分配器 — LIFO 顺序分配/释放
class FStackAllocator
{
public:
    // ========================================================================
    // 构造与析构
    // ========================================================================

    /// 指定容量构造
    explicit FStackAllocator(SizeType capacity)
        : m_Buffer(nullptr)
        , m_Capacity(capacity)
        , m_Top(0)
        , m_Allocator(&GetDefaultAllocator())
    {
        LIMX_ASSERT(capacity > 0);
        m_Buffer = static_cast<UInt8*>(
            m_Allocator->Allocate(capacity, 16));
    }

    ~FStackAllocator()
    {
        if (m_Buffer)
        {
            m_Allocator->Deallocate(m_Buffer);
        }
    }

    // 不可拷贝/移动
    FStackAllocator(const FStackAllocator&) = delete;
    FStackAllocator& operator=(const FStackAllocator&) = delete;

    // ========================================================================
    // 分配
    // ========================================================================

    /// 分配指定大小和对齐的内存 — O(1)
    /// @return 对齐后的内存指针，空间不足返回 nullptr
    LIMX_NODISCARD void* Allocate(SizeType size, SizeType alignment = 8)
    {
        LIMX_ASSERT(size > 0);
        LIMX_ASSERT(alignment > 0 && (alignment & (alignment - 1)) == 0);

        // 对齐栈顶
        SizeType aligned = (m_Top + alignment - 1) & ~(alignment - 1);

        if (aligned + size > m_Capacity)
        {
            return nullptr;
        }

        void* ptr = m_Buffer + aligned;
        m_Top = aligned + size;
        return ptr;
    }

    /// 类型化分配 — 分配 N 个 T 的空间
    template<typename T>
    LIMX_NODISCARD T* AllocateTyped(SizeType count = 1)
    {
        return static_cast<T*>(
            Allocate(sizeof(T) * count, alignof(T)));
    }

    // ========================================================================
    // 标记与释放
    // ========================================================================

    /// 获取当前栈顶标记
    LIMX_NODISCARD StackMarker GetMarker() const { return m_Top; }

    /// 回退到指定标记位置
    void FreeToMarker(StackMarker marker)
    {
        LIMX_ASSERT(marker <= m_Top);
        m_Top = marker;
    }

    /// 重置 — 回退到起点
    void Reset() { m_Top = 0; }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 已使用字节数
    LIMX_NODISCARD SizeType GetUsed() const { return m_Top; }

    /// 剩余字节数
    LIMX_NODISCARD SizeType GetRemaining() const
    {
        return m_Capacity - m_Top;
    }

    /// 总容量
    LIMX_NODISCARD SizeType GetCapacity() const { return m_Capacity; }

    /// 是否为空
    LIMX_NODISCARD bool IsEmpty() const { return m_Top == 0; }

private:
    UInt8*      m_Buffer;     ///< 内存缓冲区
    SizeType    m_Capacity;   ///< 总容量
    SizeType    m_Top;        ///< 栈顶偏移
    IAllocator* m_Allocator;  ///< 底层分配器
};

/// RAII 作用域标记 — 离开作用域自动回退栈
class FScopeMarker
{
public:
    explicit FScopeMarker(FStackAllocator& allocator)
        : m_Allocator(allocator)
        , m_Marker(allocator.GetMarker())
    {
    }

    ~FScopeMarker()
    {
        m_Allocator.FreeToMarker(m_Marker);
    }

    // 不可拷贝/移动
    FScopeMarker(const FScopeMarker&) = delete;
    FScopeMarker& operator=(const FScopeMarker&) = delete;

    /// 获取进入时的标记
    LIMX_NODISCARD StackMarker GetMarker() const { return m_Marker; }

private:
    FStackAllocator& m_Allocator;  ///< 关联的栈分配器
    StackMarker      m_Marker;     ///< 进入时的标记位置
};

} // namespace Limx
