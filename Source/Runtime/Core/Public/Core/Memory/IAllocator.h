/*******************************************************************************
 * 文件: IAllocator.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   内存分配器接口 — 引擎所有动态内存分配的统一抽象
 *   定义分配/释放/重分配的虚函数接口
 *   所有引擎代码禁止裸 new/delete，必须通过分配器接口分配内存
 *
 * 设计哲学:
 *   策略模式 — 通过接口多态允许不同场景使用不同分配策略
 *   可审计 — 每次分配/释放均可被追踪 (Debug 模式)
 *   对齐感知 — 所有分配接口显式要求对齐参数
 *   零碎片 — 为后续池分配器/竞技场分配器预留接口
 *
 * 技术特性:
 *   - IAllocator: 分配器纯虚接口
 *   - AllocateUninitialized: 分配未初始化内存
 *   - Deallocate: 释放内存
 *   - Reallocate: 重分配 (可能移动)
 *   - GetAllocationSize: 查询分配大小 (可选)
 *
 * 依赖关系:
 *   内部: Core/CoreTypes.h
 *
 * 注意事项:
 *   分配器实例的生命周期必须长于其分配的所有内存
 *   跨分配器释放内存是未定义行为
 *   线程安全性由具体实现保证
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"

namespace Limx
{

// 默认内存对齐 — 16 字节 (满足 SSE 要求)
inline constexpr SizeType kDefaultAlignment = 16;

/// 内存分配器接口 — 所有引擎分配器的基类
/// 禁止拷贝和移动，分配器是引用语义的单例或长生命周期对象
class LIMX_CORE_API IAllocator
{
public:
    virtual ~IAllocator() = default;

    /// 分配指定大小的未初始化内存
    /// @param size       请求的字节数 (必须 > 0)
    /// @param alignment  对齐要求 (字节, 必须是 2 的幂, 默认 16)
    /// @return 分配的内存指针，失败返回 nullptr
    LIMX_NODISCARD virtual void* Allocate(
        SizeType size,
        SizeType alignment = kDefaultAlignment) = 0;

    /// 释放之前由本分配器分配的内存
    /// @param pointer 待释放的指针 (nullptr 是合法的空操作)
    virtual void Deallocate(void* pointer) = 0;

    /// 重分配内存块 — 可能移动到新地址
    /// 保留 min(oldSize, newSize) 字节的原始数据
    /// @param pointer    之前分配的指针 (nullptr 等价于 Allocate)
    /// @param newSize    新的字节数 (0 等价于 Deallocate)
    /// @param alignment  对齐要求
    /// @return 重分配后的指针，失败返回 nullptr (原内存不变)
    LIMX_NODISCARD virtual void* Reallocate(
        void* pointer,
        SizeType newSize,
        SizeType alignment = kDefaultAlignment) = 0;

    /// 查询某次分配的实际大小 (可选，不是所有分配器都支持)
    /// 默认返回 0 表示不支持
    LIMX_NODISCARD virtual SizeType GetAllocationSize(void* pointer) const
    {
        LIMX_UNUSED(pointer);
        return 0;
    }

    /// 分配器名称 — 用于调试和性能分析
    LIMX_NODISCARD virtual const AnsiChar* GetName() const = 0;

    LIMX_NON_COPYABLE(IAllocator);
    LIMX_NON_MOVABLE(IAllocator);

protected:
    IAllocator() = default;
};

// ============================================================================
// 类型化分配辅助函数
// ============================================================================

/// 通过分配器分配 count 个 T 的未初始化内存
template<typename T>
LIMX_NODISCARD FORCEINLINE T* AllocateArray(IAllocator& allocator, SizeType count)
{
    LIMX_ASSERT(count > 0);
    return static_cast<T*>(allocator.Allocate(
        count * sizeof(T),
        alignof(T) > kDefaultAlignment ? alignof(T) : kDefaultAlignment));
}

/// 通过分配器分配单个 T 的未初始化内存
template<typename T>
LIMX_NODISCARD FORCEINLINE T* AllocateOne(IAllocator& allocator)
{
    return static_cast<T*>(allocator.Allocate(
        sizeof(T),
        alignof(T) > kDefaultAlignment ? alignof(T) : kDefaultAlignment));
}

} // namespace Limx
