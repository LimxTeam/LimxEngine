/*******************************************************************************
 * 文件: MemoryOps.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   底层内存操作原语 — 完全脱离 STL 的内存操作接口
 *   封装 CRT 内建内存函数 (memcpy/memset/memmove/memcmp)
 *   提供类型安全的构造/析构/拷贝/移动操作
 *   所有引擎代码必须通过本文件的接口操作原始内存
 *
 * 设计哲学:
 *   零 STL 依赖 — 通过前向声明 CRT 函数避免 #include <cstring>
 *   类型安全 — 模板封装确保构造/析构语义正确
 *   编译器友好 — CRT 函数声明后编译器自动识别为内建并优化
 *
 * 技术特性:
 *   - 原始字节操作: MemCopy, MemMove, MemSet, MemZero, MemCompare
 *   - 类型构造: ConstructItems, DefaultConstructItems
 *   - 类型析构: DestructItems
 *   - 类型拷贝: CopyConstructItems, CopyAssignItems
 *   - 类型移动: MoveConstructItems, MoveAssignItems
 *   - 对齐分配: AlignedAlloc, AlignedFree
 *
 * 依赖关系:
 *   内部: Core/HAL/Platform.h, Core/HAL/PlatformTypes.h, Core/CoreMacros.h,
 *          Core/TypeTraits/TypeTraits.h
 *
 * 注意事项:
 *   禁止包含 <cstring>, <cstdlib>, <new> 等任何 STL/CRT 头文件
 *   CRT 函数通过 extern "C" 前向声明，由链接器解析
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/TypeTraits/TypeTraits.h"

// ============================================================================
// CRT 内存函数前向声明 — 无需 #include <cstring>
// 编译器 (MSVC/Clang/GCC) 识别这些声明后自动内联优化
//
// 这四个函数在 UCRT 里都声明为编译器内建, 不带 dllimport, 因此不需要
// LIMX_CRT_IMPORT。第三方库把真 CRT 头文件拉进同一个翻译单元时, 两边
// 的链接属性本就一致。
// ============================================================================

extern "C"
{
    void* memcpy(void* destination, const void* source, Limx::SizeType size);
    void* memmove(void* destination, const void* source, Limx::SizeType size);
    void* memset(void* destination, int value, Limx::SizeType size);
    int   memcmp(const void* buffer1, const void* buffer2, Limx::SizeType size);
}

// ============================================================================
// CRT 内存分配函数前向声明 — 无需 #include <cstdlib>
// ============================================================================

extern "C"
{
    void* malloc(Limx::SizeType size);
    void  free(void* pointer);
    void* realloc(void* pointer, Limx::SizeType newSize);
}

// ============================================================================
// 对齐分配函数前向声明
// ============================================================================

#if LIMX_PLATFORM_WINDOWS
extern "C"
{
    void* _aligned_malloc(Limx::SizeType size, Limx::SizeType alignment);
    void  _aligned_free(void* pointer);
    void* _aligned_realloc(void* pointer, Limx::SizeType size, Limx::SizeType alignment);
}
#endif


// ============================================================================
// Placement new 操作符前向声明 — 无需 #include <new>
// ============================================================================

// 全局 placement new — 在指定地址构造对象
inline void* operator new(Limx::SizeType, void* location) noexcept
{
    return location;
}

// 对应的 placement delete (仅编译器内部使用，构造异常时调用)
inline void operator delete(void*, void*) noexcept {}

namespace Limx::Memory
{

// ============================================================================
// 原始字节操作
// ============================================================================

/// 内存拷贝 — 源和目标不可重叠
/// 等价于 memcpy，编译器自动优化为 SIMD 指令
FORCEINLINE void* MemCopy(void* LIMX_RESTRICT destination,
                          const void* LIMX_RESTRICT source,
                          SizeType byteCount)
{
    return memcpy(destination, source, byteCount);
}

/// 内存移动 — 源和目标可重叠
/// 等价于 memmove，当区域重叠时安全
FORCEINLINE void* MemMove(void* destination, const void* source, SizeType byteCount)
{
    return memmove(destination, source, byteCount);
}

/// 内存填充 — 按字节填充
/// 等价于 memset
FORCEINLINE void* MemSet(void* destination, int value, SizeType byteCount)
{
    return memset(destination, value, byteCount);
}

/// 内存清零 — 将指定区域填充为 0
FORCEINLINE void* MemZero(void* destination, SizeType byteCount)
{
    return memset(destination, 0, byteCount);
}

/// 内存比较 — 逐字节比较两块内存
/// 返回值: 0 相等, <0 buffer1 < buffer2, >0 buffer1 > buffer2
LIMX_NODISCARD FORCEINLINE int MemCompare(const void* buffer1,
                                           const void* buffer2,
                                           SizeType byteCount)
{
    return memcmp(buffer1, buffer2, byteCount);
}

// ============================================================================
// 对齐内存分配
// ============================================================================

/// 对齐内存分配 — 返回满足指定对齐要求的内存块
/// alignment 必须是 2 的幂且 >= sizeof(void*)
LIMX_NODISCARD inline void* AlignedAlloc(SizeType size, SizeType alignment)
{
#if LIMX_PLATFORM_WINDOWS
    return _aligned_malloc(size, alignment);
#else
    // POSIX: 使用 aligned_alloc (C11)
    // 要求 size 为 alignment 的倍数
    SizeType alignedSize = (size + alignment - 1) & ~(alignment - 1);
    void* pointer = nullptr;
    // 前向声明 posix_memalign
    extern "C" int posix_memalign(void**, SizeType, SizeType);
    posix_memalign(&pointer, alignment, alignedSize);
    return pointer;
#endif
}

/// 释放对齐分配的内存
inline void AlignedFree(void* pointer)
{
#if LIMX_PLATFORM_WINDOWS
    _aligned_free(pointer);
#else
    free(pointer);
#endif
}

/// 对齐内存重分配
LIMX_NODISCARD inline void* AlignedRealloc(void* pointer,
                                            SizeType newSize,
                                            SizeType alignment)
{
#if LIMX_PLATFORM_WINDOWS
    return _aligned_realloc(pointer, newSize, alignment);
#else
    // POSIX 没有 aligned_realloc — 需要手动实现
    void* newPointer = AlignedAlloc(newSize, alignment);
    if (newPointer && pointer)
    {
        // 注意: 这里无法知道原始大小，调用者需要自行管理
        // 实际使用中由 Allocator 层跟踪分配大小
        MemCopy(newPointer, pointer, newSize);
        AlignedFree(pointer);
    }
    return newPointer;
#endif
}

// ============================================================================
// 类型化构造/析构操作
// ============================================================================

/// 在指定地址默认构造 count 个 T 对象
/// 对 POD 类型优化为 MemZero
template<typename T>
FORCEINLINE void DefaultConstructItems(T* destination, SizeType count)
{
    if constexpr (IsArithmeticV<T> || IsPointerV<T> || IsEnumV<T>)
    {
        // POD 类型 — 直接清零
        MemZero(destination, count * sizeof(T));
    }
    else
    {
        // 非 POD — 逐个调用默认构造函数
        for (SizeType index = 0; index < count; ++index)
        {
            new (destination + index) T();
        }
    }
}

/// 在指定地址拷贝构造 count 个 T 对象
template<typename T>
FORCEINLINE void CopyConstructItems(T* destination, const T* source, SizeType count)
{
    if constexpr (IsArithmeticV<T> || IsPointerV<T> || IsEnumV<T>)
    {
        // POD 类型 — 直接内存拷贝
        MemCopy(destination, source, count * sizeof(T));
    }
    else
    {
        for (SizeType index = 0; index < count; ++index)
        {
            new (destination + index) T(source[index]);
        }
    }
}

/// 在指定地址移动构造 count 个 T 对象
template<typename T>
FORCEINLINE void MoveConstructItems(T* destination, T* source, SizeType count)
{
    if constexpr (IsArithmeticV<T> || IsPointerV<T> || IsEnumV<T>)
    {
        MemCopy(destination, source, count * sizeof(T));
    }
    else
    {
        for (SizeType index = 0; index < count; ++index)
        {
            new (destination + index) T(MoveTemp(source[index]));
        }
    }
}

/// 拷贝赋值 count 个 T 对象
template<typename T>
FORCEINLINE void CopyAssignItems(T* destination, const T* source, SizeType count)
{
    if constexpr (IsArithmeticV<T> || IsPointerV<T> || IsEnumV<T>)
    {
        MemCopy(destination, source, count * sizeof(T));
    }
    else
    {
        for (SizeType index = 0; index < count; ++index)
        {
            destination[index] = source[index];
        }
    }
}

/// 移动赋值 count 个 T 对象
template<typename T>
FORCEINLINE void MoveAssignItems(T* destination, T* source, SizeType count)
{
    if constexpr (IsArithmeticV<T> || IsPointerV<T> || IsEnumV<T>)
    {
        MemCopy(destination, source, count * sizeof(T));
    }
    else
    {
        for (SizeType index = 0; index < count; ++index)
        {
            destination[index] = MoveTemp(source[index]);
        }
    }
}

/// 析构 count 个 T 对象
/// 对 POD 类型优化为空操作
template<typename T>
FORCEINLINE void DestructItems(T* items, SizeType count)
{
    if constexpr (!(IsArithmeticV<T> || IsPointerV<T> || IsEnumV<T>))
    {
        for (SizeType index = 0; index < count; ++index)
        {
            items[index].~T();
        }
    }
}

/// 在指定内存区域内向后搬移元素（用于插入操作）
/// 从后向前移动，避免覆盖
template<typename T>
FORCEINLINE void RelocateItemsBackward(T* destination, T* source, SizeType count)
{
    if constexpr (IsArithmeticV<T> || IsPointerV<T> || IsEnumV<T>)
    {
        MemMove(destination, source, count * sizeof(T));
    }
    else
    {
        if (destination > source)
        {
            // 从后向前移动构造 + 析构
            for (SizeType index = count; index > 0; --index)
            {
                new (destination + index - 1) T(MoveTemp(source[index - 1]));
                source[index - 1].~T();
            }
        }
        else
        {
            // destination <= source: 前向搬移。两段重叠时,
            // [source, destination + count) 已被搬过来的数据覆盖,
            // 只有尾部 [destination + count, source + count) 仍持有
            // 被移空的源对象需要析构。
            //
            // 对整段调 DestructItems 会销毁刚搬过来的活对象 —— 对平凡析构
            // 的类型看不出问题, 对 TUniquePtr 这类就是提前释放。
            MoveConstructItems(destination, source, count);

            T* tailBegin = destination + count;

            if (tailBegin < source)
            {
                tailBegin = source;
            }

            const SizeType tailCount =
                static_cast<SizeType>((source + count) - tailBegin);

            DestructItems(tailBegin, tailCount);
        }
    }
}

} // namespace Limx::Memory
