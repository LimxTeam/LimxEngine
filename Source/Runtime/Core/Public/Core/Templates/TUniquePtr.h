/*******************************************************************************
 * 文件: TUniquePtr.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   独占智能指针 — 替代 std::unique_ptr 的零 STL 依赖实现
 *   严格独占所有权语义：不可拷贝，仅可移动
 *   析构时自动释放所管理的对象
 *   支持自定义删除器
 *
 * 设计哲学:
 *   零开销抽象 — 默认删除器下与裸指针相同大小和性能
 *   所有权明确 — 编译时阻止拷贝，只允许显式移动转移所有权
 *   RAII — 构造即获取，析构即释放，无泄漏
 *
 * 技术特性:
 *   - 默认删除器: TDefaultDelete<T> (调用析构 + Deallocate)
 *   - 自定义删除器: 模板参数注入
 *   - 数组特化: TUniquePtr<T[]> 支持数组语义
 *   - 工厂函数: MakeUnique<T>(args...) 原地构造
 *   - 隐式 bool 转换: if (ptr) 检查有效性
 *   - Release: 释放所有权但不析构
 *   - Reset: 替换管理的对象
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h,
 *          Core/TypeTraits/TypeTraits.h, Core/Memory/MemoryOps.h,
 *          Core/Memory/DefaultAllocator.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/TypeTraits/TypeTraits.h"
#include "Core/Memory/MemoryOps.h"
#include "Core/Memory/DefaultAllocator.h"

namespace Limx
{

// ============================================================================
// 默认删除器
// ============================================================================

/// 默认删除器 — 通过分配器析构并释放单个对象
template<typename T>
struct TDefaultDelete
{
    IAllocator* Allocator = nullptr;

    TDefaultDelete() = default;

    explicit TDefaultDelete(IAllocator& allocator)
        : Allocator(&allocator)
    {
    }

    void operator()(T* pointer) const
    {
        if (pointer)
        {
            pointer->~T();
            if (Allocator)
            {
                Allocator->Deallocate(pointer);
            }
        }
    }
};

/// 数组删除器特化 — 析构并释放数组
template<typename T>
struct TDefaultDelete<T[]>
{
    IAllocator* Allocator = nullptr;
    SizeType    Count = 0;

    TDefaultDelete() = default;

    TDefaultDelete(IAllocator& allocator, SizeType count)
        : Allocator(&allocator)
        , Count(count)
    {
    }

    void operator()(T* pointer) const
    {
        if (pointer)
        {
            Memory::DestructItems(pointer, Count);
            if (Allocator)
            {
                Allocator->Deallocate(pointer);
            }
        }
    }
};

// ============================================================================
// TUniquePtr — 单对象版本
// ============================================================================

/// 独占智能指针 — 严格独占所有权，不可拷贝，仅可移动
/// @tparam T       管理的对象类型
/// @tparam Deleter 删除器类型 (默认 TDefaultDelete<T>)
template<typename T, typename Deleter = TDefaultDelete<T>>
class TUniquePtr
{
public:
    using ElementType = T;
    using DeleterType = Deleter;
    using PointerType = T*;

    // ========================================================================
    // 构造与析构
    // ========================================================================

    /// 默认构造 — 空指针
    TUniquePtr() noexcept
        : m_Pointer(nullptr)
        , m_Deleter()
    {
    }

    /// 从裸指针构造
    explicit TUniquePtr(PointerType pointer) noexcept
        : m_Pointer(pointer)
        , m_Deleter()
    {
    }

    /// 从裸指针 + 删除器构造
    TUniquePtr(PointerType pointer, const Deleter& deleter) noexcept
        : m_Pointer(pointer)
        , m_Deleter(deleter)
    {
    }

    /// 从裸指针 + 移动删除器构造
    TUniquePtr(PointerType pointer, Deleter&& deleter) noexcept
        : m_Pointer(pointer)
        , m_Deleter(MoveTemp(deleter))
    {
    }

    /// nullptr 构造
    TUniquePtr(decltype(nullptr)) noexcept
        : m_Pointer(nullptr)
        , m_Deleter()
    {
    }

    /// 移动构造 — 转移所有权
    TUniquePtr(TUniquePtr&& other) noexcept
        : m_Pointer(other.m_Pointer)
        , m_Deleter(MoveTemp(other.m_Deleter))
    {
        other.m_Pointer = nullptr;
    }

    /// 派生类移动构造 — 支持协变
    template<typename U, typename OtherDeleter>
    TUniquePtr(TUniquePtr<U, OtherDeleter>&& other) noexcept
        : m_Pointer(other.Release())
        , m_Deleter(MoveTemp(other.GetDeleter()))
    {
        static_assert(IsConvertibleV<U*, T*>,
            "U* 必须可隐式转换为 T*");
    }

    /// 析构 — 通过删除器释放对象
    ~TUniquePtr()
    {
        if (m_Pointer)
        {
            m_Deleter(m_Pointer);
        }
    }

    /// 禁止拷贝
    LIMX_NON_COPYABLE(TUniquePtr);

    // ========================================================================
    // 赋值运算符
    // ========================================================================

    /// 移动赋值
    TUniquePtr& operator=(TUniquePtr&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            m_Pointer = other.m_Pointer;
            m_Deleter = MoveTemp(other.m_Deleter);
            other.m_Pointer = nullptr;
        }
        return *this;
    }

    /// nullptr 赋值 — 释放当前对象
    TUniquePtr& operator=(decltype(nullptr)) noexcept
    {
        Reset();
        return *this;
    }

    // ========================================================================
    // 访问
    // ========================================================================

    /// 解引用
    LIMX_NODISCARD FORCEINLINE T& operator*() const
    {
        LIMX_ASSERT(m_Pointer != nullptr);
        return *m_Pointer;
    }

    /// 成员访问
    LIMX_NODISCARD FORCEINLINE PointerType operator->() const
    {
        LIMX_ASSERT(m_Pointer != nullptr);
        return m_Pointer;
    }

    /// 获取裸指针 (不释放所有权)
    LIMX_NODISCARD FORCEINLINE PointerType Get() const
    {
        return m_Pointer;
    }

    /// 有效性检查
    LIMX_NODISCARD FORCEINLINE explicit operator bool() const
    {
        return m_Pointer != nullptr;
    }

    LIMX_NODISCARD FORCEINLINE bool IsValid() const
    {
        return m_Pointer != nullptr;
    }

    /// 获取删除器引用
    LIMX_NODISCARD FORCEINLINE Deleter& GetDeleter() { return m_Deleter; }
    LIMX_NODISCARD FORCEINLINE const Deleter& GetDeleter() const { return m_Deleter; }

    // ========================================================================
    // 所有权管理
    // ========================================================================

    /// 释放所有权 — 返回裸指针，不析构，调用者接管
    LIMX_NODISCARD PointerType Release() noexcept
    {
        PointerType pointer = m_Pointer;
        m_Pointer = nullptr;
        return pointer;
    }

    /// 替换管理的对象 — 先释放旧对象
    void Reset(PointerType newPointer = nullptr) noexcept
    {
        if (m_Pointer)
        {
            m_Deleter(m_Pointer);
        }
        m_Pointer = newPointer;
    }

    /// 交换两个 TUniquePtr
    void Swap(TUniquePtr& other) noexcept
    {
        PointerType tempPtr = m_Pointer;
        m_Pointer = other.m_Pointer;
        other.m_Pointer = tempPtr;

        Deleter tempDel = MoveTemp(m_Deleter);
        m_Deleter = MoveTemp(other.m_Deleter);
        other.m_Deleter = MoveTemp(tempDel);
    }

    // ========================================================================
    // 比较运算符
    // ========================================================================

    LIMX_NODISCARD friend bool operator==(const TUniquePtr& lhs, const TUniquePtr& rhs)
    {
        return lhs.m_Pointer == rhs.m_Pointer;
    }

    LIMX_NODISCARD friend bool operator!=(const TUniquePtr& lhs, const TUniquePtr& rhs)
    {
        return lhs.m_Pointer != rhs.m_Pointer;
    }

    LIMX_NODISCARD friend bool operator==(const TUniquePtr& lhs, decltype(nullptr))
    {
        return lhs.m_Pointer == nullptr;
    }

    LIMX_NODISCARD friend bool operator!=(const TUniquePtr& lhs, decltype(nullptr))
    {
        return lhs.m_Pointer != nullptr;
    }

private:
    PointerType m_Pointer;
    Deleter     m_Deleter;
};

// ============================================================================
// MakeUnique — 工厂函数
// ============================================================================

/// 通过默认分配器创建 TUniquePtr — 原地构造对象
template<typename T, typename... Args>
LIMX_NODISCARD TUniquePtr<T> MakeUnique(Args&&... args)
{
    IAllocator& allocator = GetDefaultAllocator();
    void* memory = allocator.Allocate(sizeof(T),
        alignof(T) > kDefaultAlignment ? alignof(T) : kDefaultAlignment);
    T* object = new (memory) T(Forward<Args>(args)...);
    return TUniquePtr<T>(object, TDefaultDelete<T>(allocator));
}

/// 通过指定分配器创建 TUniquePtr
template<typename T, typename... Args>
LIMX_NODISCARD TUniquePtr<T> MakeUniqueWith(IAllocator& allocator, Args&&... args)
{
    void* memory = allocator.Allocate(sizeof(T),
        alignof(T) > kDefaultAlignment ? alignof(T) : kDefaultAlignment);
    T* object = new (memory) T(Forward<Args>(args)...);
    return TUniquePtr<T>(object, TDefaultDelete<T>(allocator));
}

} // namespace Limx
