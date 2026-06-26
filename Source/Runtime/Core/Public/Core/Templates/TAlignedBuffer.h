/*******************************************************************************
 * 文件: TAlignedBuffer.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   对齐缓冲区 — 编译时对齐的栈存储模板
 *   提供指定大小和对齐的原始内存块，用于延迟构造对象
 *   用于类型擦除内部存储、SBO 缓冲区、延迟初始化等场景
 *
 * 设计哲学:
 *   编译时 — 大小和对齐通过模板参数编译时确定
 *   栈存储 — 内嵌在宿主对象中，无堆分配
 *   原始内存 — 不自动构造/析构，由使用者负责
 *
 * 技术特性:
 *   - TAlignedBuffer<Size, Alignment>: 对齐原始缓冲区
 *   - TTypedAlignedBuffer<T>: 按类型推导大小和对齐
 *   - GetPtr: 获取缓冲区指针
 *   - AsRef: 类型化引用访问
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"

namespace Limx
{

/// 对齐原始缓冲区
/// @tparam Size 缓冲区大小 (字节)
/// @tparam Alignment 对齐要求 (字节, 默认 16)
template<SizeType Size, SizeType Alignment = 16>
struct TAlignedBuffer
{
    static_assert(Size > 0,
        "Buffer size must be > 0");
    static_assert((Alignment & (Alignment - 1)) == 0,
        "Alignment must be a power of 2");

    alignas(Alignment) UInt8 Data[Size];

    /// 获取缓冲区指针
    LIMX_NODISCARD void* GetPtr()
    {
        return static_cast<void*>(Data);
    }

    LIMX_NODISCARD const void* GetPtr() const
    {
        return static_cast<const void*>(Data);
    }

    /// 类型化指针访问
    template<typename T>
    LIMX_NODISCARD T* As()
    {
        static_assert(sizeof(T) <= Size,
            "Type too large for buffer");
        static_assert(Alignment % alignof(T) == 0,
            "Buffer alignment insufficient for type");
        return reinterpret_cast<T*>(Data);
    }

    template<typename T>
    LIMX_NODISCARD const T* As() const
    {
        static_assert(sizeof(T) <= Size,
            "Type too large for buffer");
        static_assert(Alignment % alignof(T) == 0,
            "Buffer alignment insufficient for type");
        return reinterpret_cast<const T*>(Data);
    }

    /// 类型化引用访问
    template<typename T>
    LIMX_NODISCARD T& AsRef()
    {
        return *As<T>();
    }

    template<typename T>
    LIMX_NODISCARD const T& AsRef() const
    {
        return *As<T>();
    }

    /// 缓冲区大小
    LIMX_NODISCARD static constexpr SizeType GetSize()
    {
        return Size;
    }

    /// 缓冲区对齐
    LIMX_NODISCARD static constexpr SizeType GetAlignment()
    {
        return Alignment;
    }
};

/// 按类型推导的对齐缓冲区
/// @tparam T 目标类型 (自动推导大小和对齐)
template<typename T>
using TTypedAlignedBuffer = TAlignedBuffer<sizeof(T), alignof(T)>;

} // namespace Limx
