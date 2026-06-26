/*******************************************************************************
 * 文件: TSharedPtr.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   共享智能指针 — 替代 std::shared_ptr 的零 STL 依赖实现
 *   引用计数所有权语义：多个 TSharedPtr 可共享同一对象
 *   最后一个 TSharedPtr 析构时自动释放所管理的对象
 *   配套 TWeakPtr 提供弱引用（不影响对象生命周期）
 *
 * 设计哲学:
 *   侵入式控制块 — 引用计数和对象内存合并分配，减少堆碎片
 *   原子引用计数 — 强/弱计数使用编译器内建原子操作，线程安全
 *   零 STL 依赖 — 不依赖 <atomic> 或 <memory>
 *
 * 技术特性:
 *   - SharedControlBlock: 强引用计数 + 弱引用计数 + 删除器
 *   - TSharedPtr<T>: 共享所有权智能指针
 *   - TWeakPtr<T>: 弱引用（可升级为 TSharedPtr）
 *   - MakeShared<T>(args...): 单次分配工厂函数
 *   - 原子操作: _InterlockedIncrement / _InterlockedDecrement (MSVC)
 *              __atomic_add_fetch / __atomic_sub_fetch (GCC/Clang)
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h,
 *          Core/TypeTraits/TypeTraits.h, Core/Memory/MemoryOps.h,
 *          Core/Memory/DefaultAllocator.h
 *
 * 注意事项:
 *   TSharedPtr 本身不是线程安全的（并发读写同一 TSharedPtr 实例需外部同步）
 *   但引用计数操作是原子的（多个线程可安全持有各自的 TSharedPtr 副本）
 *   避免循环引用 — 使用 TWeakPtr 打破循环
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/TypeTraits/TypeTraits.h"
#include "Core/Memory/MemoryOps.h"
#include "Core/Memory/DefaultAllocator.h"

// ============================================================================
// 原子操作内建函数声明
// ============================================================================

#if LIMX_COMPILER_MSVC
extern "C"
{
    long _InterlockedIncrement(volatile long* addend);
    long _InterlockedDecrement(volatile long* addend);
    long _InterlockedCompareExchange(volatile long* destination,
                                      long exchange, long comparand);
}
#pragma intrinsic(_InterlockedIncrement)
#pragma intrinsic(_InterlockedDecrement)
#pragma intrinsic(_InterlockedCompareExchange)
#endif

namespace Limx
{

// 前向声明
template<typename T> class TSharedPtr;
template<typename T> class TWeakPtr;

// ============================================================================
// 原子引用计数辅助
// ============================================================================

namespace Detail
{

/// 原子递增 — 返回递增后的值
FORCEINLINE Int32 AtomicIncrement(volatile Int32* target)
{
#if LIMX_COMPILER_MSVC
    return static_cast<Int32>(
        _InterlockedIncrement(reinterpret_cast<volatile long*>(target)));
#else
    return __atomic_add_fetch(target, 1, __ATOMIC_ACQ_REL);
#endif
}

/// 原子递减 — 返回递减后的值
FORCEINLINE Int32 AtomicDecrement(volatile Int32* target)
{
#if LIMX_COMPILER_MSVC
    return static_cast<Int32>(
        _InterlockedDecrement(reinterpret_cast<volatile long*>(target)));
#else
    return __atomic_sub_fetch(target, 1, __ATOMIC_ACQ_REL);
#endif
}

/// 原子比较交换 — 如果 *target == expected 则写入 desired，返回原值
FORCEINLINE Int32 AtomicCompareExchange(volatile Int32* target,
                                         Int32 desired, Int32 expected)
{
#if LIMX_COMPILER_MSVC
    return static_cast<Int32>(
        _InterlockedCompareExchange(
            reinterpret_cast<volatile long*>(target), desired, expected));
#else
    __atomic_compare_exchange_n(target, &expected, desired,
                                false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    return expected;
#endif
}

/// 原子加载
FORCEINLINE Int32 AtomicLoad(const volatile Int32* target)
{
#if LIMX_COMPILER_MSVC
    // MSVC x64 上 aligned Int32 读取本身是原子的
    // 使用 CompareExchange(0,0) 确保内存屏障
    return _InterlockedCompareExchange(
        const_cast<volatile long*>(reinterpret_cast<const volatile long*>(target)),
        0, 0);
#else
    return __atomic_load_n(target, __ATOMIC_ACQUIRE);
#endif
}

// ============================================================================
// 控制块
// ============================================================================

/// 共享控制块 — 管理引用计数和对象销毁
/// 由 MakeShared 创建，包含强/弱引用计数
class SharedControlBlock
{
public:
    SharedControlBlock(IAllocator* allocator)
        : m_StrongCount(1)
        , m_WeakCount(1)  // 弱计数初始为 1 — 代表所有强引用的集体
        , m_Allocator(allocator)
    {
    }

    virtual ~SharedControlBlock() = default;

    /// 增加强引用
    void AddStrongRef()
    {
        AtomicIncrement(&m_StrongCount);
    }

    /// 尝试增加强引用（从弱引用升级时使用）
    /// 如果强引用已为 0 则失败，返回 false
    bool TryAddStrongRef()
    {
        while (true)
        {
            Int32 current = AtomicLoad(&m_StrongCount);
            if (current <= 0)
            {
                return false;
            }
            if (AtomicCompareExchange(&m_StrongCount, current + 1, current) == current)
            {
                return true;
            }
        }
    }

    /// 释放强引用 — 强引用归零时析构对象
    void ReleaseStrongRef()
    {
        if (AtomicDecrement(&m_StrongCount) == 0)
        {
            DestroyObject();
            ReleaseWeakRef();  // 释放强引用集体持有的那一个弱引用
        }
    }

    /// 增加弱引用
    void AddWeakRef()
    {
        AtomicIncrement(&m_WeakCount);
    }

    /// 释放弱引用 — 弱引用归零时释放控制块本身
    void ReleaseWeakRef()
    {
        if (AtomicDecrement(&m_WeakCount) == 0)
        {
            DestroyControlBlock();
        }
    }

    /// 当前强引用数
    LIMX_NODISCARD Int32 GetStrongCount() const
    {
        return AtomicLoad(&m_StrongCount);
    }

    /// 当前弱引用数 (不含强引用的隐式弱引用)
    LIMX_NODISCARD Int32 GetWeakCount() const
    {
        return AtomicLoad(&m_WeakCount) - (GetStrongCount() > 0 ? 1 : 0);
    }

protected:
    /// 析构被管理的对象 (由子类实现)
    virtual void DestroyObject() = 0;

    /// 释放控制块本身 (由子类实现)
    virtual void DestroyControlBlock() = 0;

    volatile Int32 m_StrongCount;
    volatile Int32 m_WeakCount;
    IAllocator*    m_Allocator;
};

/// 内联控制块 — 对象和控制块在同一次分配中
/// 由 MakeShared 使用，避免二次分配
template<typename T>
class InlineControlBlock final : public SharedControlBlock
{
public:
    template<typename... Args>
    explicit InlineControlBlock(IAllocator* allocator, Args&&... args)
        : SharedControlBlock(allocator)
    {
        // 在预留的存储空间上原地构造对象
        new (&m_Storage) T(Forward<Args>(args)...);
    }

    T* GetObject()
    {
        return reinterpret_cast<T*>(&m_Storage);
    }

protected:
    void DestroyObject() override
    {
        GetObject()->~T();
    }

    void DestroyControlBlock() override
    {
        IAllocator* allocator = m_Allocator;
        this->~InlineControlBlock();
        allocator->Deallocate(this);
    }

private:
    // 对齐存储 — 足以容纳 T 且满足对齐要求
    alignas(T) UInt8 m_Storage[sizeof(T)];
};

/// 外部控制块 — 管理外部分配的对象指针
/// 当从裸指针构造 TSharedPtr 时使用
template<typename T>
class ExternalControlBlock final : public SharedControlBlock
{
public:
    ExternalControlBlock(IAllocator* allocator, T* objectPointer)
        : SharedControlBlock(allocator)
        , m_ObjectPointer(objectPointer)
    {
    }

protected:
    void DestroyObject() override
    {
        if (m_ObjectPointer)
        {
            m_ObjectPointer->~T();
            m_Allocator->Deallocate(m_ObjectPointer);
            m_ObjectPointer = nullptr;
        }
    }

    void DestroyControlBlock() override
    {
        IAllocator* allocator = m_Allocator;
        this->~ExternalControlBlock();
        allocator->Deallocate(this);
    }

private:
    T* m_ObjectPointer;
};

} // namespace Detail

// ============================================================================
// TSharedPtr
// ============================================================================

/// 共享智能指针 — 引用计数所有权，线程安全的引用计数操作
/// @tparam T 管理的对象类型
template<typename T>
class TSharedPtr
{
    template<typename U> friend class TSharedPtr;
    template<typename U> friend class TWeakPtr;

public:
    using ElementType = T;

    // ========================================================================
    // 构造与析构
    // ========================================================================

    /// 默认构造 — 空指针
    TSharedPtr() noexcept
        : m_Pointer(nullptr)
        , m_ControlBlock(nullptr)
    {
    }

    /// nullptr 构造
    TSharedPtr(decltype(nullptr)) noexcept
        : m_Pointer(nullptr)
        , m_ControlBlock(nullptr)
    {
    }

    /// 拷贝构造 — 增加引用计数
    TSharedPtr(const TSharedPtr& other) noexcept
        : m_Pointer(other.m_Pointer)
        , m_ControlBlock(other.m_ControlBlock)
    {
        if (m_ControlBlock)
        {
            m_ControlBlock->AddStrongRef();
        }
    }

    /// 派生类拷贝构造
    template<typename U>
    TSharedPtr(const TSharedPtr<U>& other) noexcept
        : m_Pointer(other.m_Pointer)
        , m_ControlBlock(other.m_ControlBlock)
    {
        static_assert(IsConvertibleV<U*, T*>,
            "U* 必须可隐式转换为 T*");
        if (m_ControlBlock)
        {
            m_ControlBlock->AddStrongRef();
        }
    }

    /// 移动构造 — 转移所有权，不改变引用计数
    TSharedPtr(TSharedPtr&& other) noexcept
        : m_Pointer(other.m_Pointer)
        , m_ControlBlock(other.m_ControlBlock)
    {
        other.m_Pointer = nullptr;
        other.m_ControlBlock = nullptr;
    }

    /// 派生类移动构造
    template<typename U>
    TSharedPtr(TSharedPtr<U>&& other) noexcept
        : m_Pointer(other.m_Pointer)
        , m_ControlBlock(other.m_ControlBlock)
    {
        static_assert(IsConvertibleV<U*, T*>,
            "U* 必须可隐式转换为 T*");
        other.m_Pointer = nullptr;
        other.m_ControlBlock = nullptr;
    }

    /// 析构 — 释放强引用
    ~TSharedPtr()
    {
        if (m_ControlBlock)
        {
            m_ControlBlock->ReleaseStrongRef();
        }
    }

    // ========================================================================
    // 赋值运算符
    // ========================================================================

    TSharedPtr& operator=(const TSharedPtr& other) noexcept
    {
        if (this != &other)
        {
            TSharedPtr temp(other);
            Swap(temp);
        }
        return *this;
    }

    TSharedPtr& operator=(TSharedPtr&& other) noexcept
    {
        if (this != &other)
        {
            TSharedPtr temp(MoveTemp(other));
            Swap(temp);
        }
        return *this;
    }

    TSharedPtr& operator=(decltype(nullptr)) noexcept
    {
        Reset();
        return *this;
    }

    // ========================================================================
    // 访问
    // ========================================================================

    LIMX_NODISCARD FORCEINLINE T& operator*() const
    {
        LIMX_ASSERT(m_Pointer != nullptr);
        return *m_Pointer;
    }

    LIMX_NODISCARD FORCEINLINE T* operator->() const
    {
        LIMX_ASSERT(m_Pointer != nullptr);
        return m_Pointer;
    }

    LIMX_NODISCARD FORCEINLINE T* Get() const
    {
        return m_Pointer;
    }

    LIMX_NODISCARD FORCEINLINE explicit operator bool() const
    {
        return m_Pointer != nullptr;
    }

    LIMX_NODISCARD FORCEINLINE bool IsValid() const
    {
        return m_Pointer != nullptr;
    }

    /// 当前强引用计数
    LIMX_NODISCARD Int32 GetSharedCount() const
    {
        return m_ControlBlock ? m_ControlBlock->GetStrongCount() : 0;
    }

    /// 是否是唯一持有者
    LIMX_NODISCARD bool IsUnique() const
    {
        return GetSharedCount() == 1;
    }

    // ========================================================================
    // 所有权管理
    // ========================================================================

    void Reset() noexcept
    {
        if (m_ControlBlock)
        {
            m_ControlBlock->ReleaseStrongRef();
            m_Pointer = nullptr;
            m_ControlBlock = nullptr;
        }
    }

    void Swap(TSharedPtr& other) noexcept
    {
        T* tempPtr = m_Pointer;
        m_Pointer = other.m_Pointer;
        other.m_Pointer = tempPtr;

        Detail::SharedControlBlock* tempCB = m_ControlBlock;
        m_ControlBlock = other.m_ControlBlock;
        other.m_ControlBlock = tempCB;
    }

    // ========================================================================
    // 比较运算符
    // ========================================================================

    LIMX_NODISCARD friend bool operator==(const TSharedPtr& lhs, const TSharedPtr& rhs)
    {
        return lhs.m_Pointer == rhs.m_Pointer;
    }

    LIMX_NODISCARD friend bool operator!=(const TSharedPtr& lhs, const TSharedPtr& rhs)
    {
        return lhs.m_Pointer != rhs.m_Pointer;
    }

    LIMX_NODISCARD friend bool operator==(const TSharedPtr& lhs, decltype(nullptr))
    {
        return lhs.m_Pointer == nullptr;
    }

    LIMX_NODISCARD friend bool operator!=(const TSharedPtr& lhs, decltype(nullptr))
    {
        return lhs.m_Pointer != nullptr;
    }

private:
    /// 内部构造 — 由 MakeShared 和 TWeakPtr::Lock 使用
    TSharedPtr(T* pointer, Detail::SharedControlBlock* controlBlock) noexcept
        : m_Pointer(pointer)
        , m_ControlBlock(controlBlock)
    {
    }

    T*                           m_Pointer;
    Detail::SharedControlBlock*  m_ControlBlock;

    // MakeShared 需要访问私有构造
    template<typename U, typename... Args>
    friend TSharedPtr<U> MakeShared(Args&&... args);

    template<typename U, typename... Args>
    friend TSharedPtr<U> MakeSharedWith(IAllocator& allocator, Args&&... args);
};

// ============================================================================
// TWeakPtr
// ============================================================================

/// 弱引用智能指针 — 不影响对象生命周期，可升级为 TSharedPtr
/// @tparam T 被引用的对象类型
template<typename T>
class TWeakPtr
{
    template<typename U> friend class TWeakPtr;

public:
    /// 默认构造 — 空弱引用
    TWeakPtr() noexcept
        : m_Pointer(nullptr)
        , m_ControlBlock(nullptr)
    {
    }

    /// 从 TSharedPtr 构造
    TWeakPtr(const TSharedPtr<T>& shared) noexcept
        : m_Pointer(shared.Get())
        , m_ControlBlock(shared.m_ControlBlock)
    {
        if (m_ControlBlock)
        {
            m_ControlBlock->AddWeakRef();
        }
    }

    /// 拷贝构造
    TWeakPtr(const TWeakPtr& other) noexcept
        : m_Pointer(other.m_Pointer)
        , m_ControlBlock(other.m_ControlBlock)
    {
        if (m_ControlBlock)
        {
            m_ControlBlock->AddWeakRef();
        }
    }

    /// 移动构造
    TWeakPtr(TWeakPtr&& other) noexcept
        : m_Pointer(other.m_Pointer)
        , m_ControlBlock(other.m_ControlBlock)
    {
        other.m_Pointer = nullptr;
        other.m_ControlBlock = nullptr;
    }

    /// 析构
    ~TWeakPtr()
    {
        if (m_ControlBlock)
        {
            m_ControlBlock->ReleaseWeakRef();
        }
    }

    // ========================================================================
    // 赋值运算符
    // ========================================================================

    TWeakPtr& operator=(const TWeakPtr& other) noexcept
    {
        if (this != &other)
        {
            TWeakPtr temp(other);
            Swap(temp);
        }
        return *this;
    }

    TWeakPtr& operator=(TWeakPtr&& other) noexcept
    {
        if (this != &other)
        {
            TWeakPtr temp(MoveTemp(other));
            Swap(temp);
        }
        return *this;
    }

    TWeakPtr& operator=(const TSharedPtr<T>& shared) noexcept
    {
        TWeakPtr temp(shared);
        Swap(temp);
        return *this;
    }

    // ========================================================================
    // 访问
    // ========================================================================

    /// 尝试升级为 TSharedPtr — 对象已销毁时返回空
    LIMX_NODISCARD TSharedPtr<T> Lock() const
    {
        if (m_ControlBlock && m_ControlBlock->TryAddStrongRef())
        {
            TSharedPtr<T> result;
            result.m_Pointer = m_Pointer;
            result.m_ControlBlock = m_ControlBlock;
            return result;
        }
        return TSharedPtr<T>();
    }

    /// 对象是否仍然存活
    LIMX_NODISCARD bool IsExpired() const
    {
        return !m_ControlBlock || m_ControlBlock->GetStrongCount() <= 0;
    }

    /// 重置弱引用
    void Reset() noexcept
    {
        if (m_ControlBlock)
        {
            m_ControlBlock->ReleaseWeakRef();
            m_Pointer = nullptr;
            m_ControlBlock = nullptr;
        }
    }

    void Swap(TWeakPtr& other) noexcept
    {
        T* tempPtr = m_Pointer;
        m_Pointer = other.m_Pointer;
        other.m_Pointer = tempPtr;

        Detail::SharedControlBlock* tempCB = m_ControlBlock;
        m_ControlBlock = other.m_ControlBlock;
        other.m_ControlBlock = tempCB;
    }

private:
    T*                           m_Pointer;
    Detail::SharedControlBlock*  m_ControlBlock;
};

// ============================================================================
// MakeShared — 工厂函数 (单次分配: 控制块 + 对象)
// ============================================================================

/// 通过默认分配器创建 TSharedPtr — 控制块与对象单次分配
template<typename T, typename... Args>
LIMX_NODISCARD TSharedPtr<T> MakeShared(Args&&... args)
{
    IAllocator& allocator = GetDefaultAllocator();
    constexpr SizeType blockAlignment =
        alignof(Detail::InlineControlBlock<T>) > kDefaultAlignment
            ? alignof(Detail::InlineControlBlock<T>) : kDefaultAlignment;

    void* memory = allocator.Allocate(
        sizeof(Detail::InlineControlBlock<T>), blockAlignment);

    auto* controlBlock = new (memory)
        Detail::InlineControlBlock<T>(&allocator, Forward<Args>(args)...);

    return TSharedPtr<T>(controlBlock->GetObject(), controlBlock);
}

/// 通过指定分配器创建 TSharedPtr
template<typename T, typename... Args>
LIMX_NODISCARD TSharedPtr<T> MakeSharedWith(IAllocator& allocator, Args&&... args)
{
    constexpr SizeType blockAlignment =
        alignof(Detail::InlineControlBlock<T>) > kDefaultAlignment
            ? alignof(Detail::InlineControlBlock<T>) : kDefaultAlignment;

    void* memory = allocator.Allocate(
        sizeof(Detail::InlineControlBlock<T>), blockAlignment);

    auto* controlBlock = new (memory)
        Detail::InlineControlBlock<T>(&allocator, Forward<Args>(args)...);

    return TSharedPtr<T>(controlBlock->GetObject(), controlBlock);
}

// ============================================================================
// StaticCastShared — 共享指针安全向下转型
// ============================================================================

/// 将 TSharedPtr<Base> 静态转型为 TSharedPtr<Derived>
/// 调用者必须确保底层对象确实是 Derived 类型
/// 转型后两个 TSharedPtr 共享同一引用计数
template<typename To, typename From>
LIMX_NODISCARD TSharedPtr<To> StaticCastShared(const TSharedPtr<From>& source)
{
    // static_cast 验证类型可转换性
    To* rawPointer = static_cast<To*>(source.Get());
    if (rawPointer == nullptr)
    {
        return TSharedPtr<To>();
    }
    // 复用源的控制块 — 通过 MakeShared 的内部构造函数路径
    // 这里直接增加强引用后创建新的 TSharedPtr
    TSharedPtr<From> copy(source);  // 增加引用计数
    // 使用 placement 技巧：TSharedPtr<To> 和 TSharedPtr<From> 内存布局相同
    // 安全方式：使用友元构造
    TSharedPtr<To> result;
    // 通过 TSharedPtr<U> 拷贝构造模板（基类→派生类方向需要手动）
    // 直接 memcpy 内部指针和控制块（二进制兼容）
    static_assert(sizeof(TSharedPtr<To>) == sizeof(TSharedPtr<From>),
                  "TSharedPtr 布局必须一致");
    Memory::MemCopy(&result, &copy, sizeof(TSharedPtr<To>));
    // 清零 copy 避免析构时减引用
    Memory::MemZero(&copy, sizeof(TSharedPtr<From>));
    return result;
}

} // namespace Limx
