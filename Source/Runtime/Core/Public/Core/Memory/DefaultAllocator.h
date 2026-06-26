/*******************************************************************************
 * 文件: DefaultAllocator.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   默认堆内存分配器 — IAllocator 的标准实现
 *   基于平台对齐分配函数 (_aligned_malloc / posix_memalign)
 *   作为引擎默认分配器，在没有指定特定分配器时使用
 *
 * 设计哲学:
 *   简单可靠 — 直接封装平台 API，不做额外簿记
 *   全局单例 — 通过 GetDefault() 获取唯一实例
 *   线程安全 — 底层平台分配器本身是线程安全的
 *
 * 技术特性:
 *   - Windows: _aligned_malloc / _aligned_free / _aligned_realloc
 *   - Linux/macOS: posix_memalign / free / 手动 realloc
 *   - 全局单例访问: DefaultAllocator::GetDefault()
 *
 * 依赖关系:
 *   内部: Core/Memory/IAllocator.h, Core/Memory/MemoryOps.h
 *
 ******************************************************************************/

#pragma once

#include "Core/Memory/IAllocator.h"
#include "Core/Memory/MemoryOps.h"

namespace Limx
{

/// 默认堆分配器 — 封装平台对齐分配 API
/// 线程安全 (依赖底层 CRT 的线程安全保证)
class LIMX_CORE_API DefaultAllocator final : public IAllocator
{
public:
    /// 获取全局默认分配器实例
    static DefaultAllocator& GetDefault()
    {
        static DefaultAllocator s_Instance;
        return s_Instance;
    }

    LIMX_NODISCARD void* Allocate(
        SizeType size,
        SizeType alignment = kDefaultAlignment) override
    {
        LIMX_ASSERT(size > 0);
        LIMX_ASSERT((alignment & (alignment - 1)) == 0);  // 2 的幂

        return Memory::AlignedAlloc(size, alignment);
    }

    void Deallocate(void* pointer) override
    {
        if (pointer)
        {
            Memory::AlignedFree(pointer);
        }
    }

    LIMX_NODISCARD void* Reallocate(
        void* pointer,
        SizeType newSize,
        SizeType alignment = kDefaultAlignment) override
    {
        if (newSize == 0)
        {
            Deallocate(pointer);
            return nullptr;
        }

        if (!pointer)
        {
            return Allocate(newSize, alignment);
        }

        return Memory::AlignedRealloc(pointer, newSize, alignment);
    }

    LIMX_NODISCARD const AnsiChar* GetName() const override
    {
        return "DefaultAllocator";
    }

private:
    DefaultAllocator() = default;
    ~DefaultAllocator() override = default;
};

// ============================================================================
// 全局分配器访问
// ============================================================================

/// 获取引擎默认分配器的便捷函数
FORCEINLINE IAllocator& GetDefaultAllocator()
{
    return DefaultAllocator::GetDefault();
}

} // namespace Limx
