/*******************************************************************************
 * 文件: TFunction.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   类型擦除可调用对象 — 替代 std::function 的零 STL 依赖实现
 *   可封装函数指针、Lambda、成员函数绑定等任意可调用对象
 *   支持小缓冲区优化 (SBO)，小型可调用对象不触发堆分配
 *
 * 设计哲学:
 *   类型擦除 — 通过虚函数表实现运行时多态调用
 *   SBO 优化 — 56 字节内联缓冲区，绝大多数 Lambda 零堆分配
 *   值语义 — 支持拷贝/移动，行为类似基本类型
 *   零 STL — 不依赖 <functional> 或 <type_traits>
 *
 * 技术特性:
 *   - SBO: 56 字节内联缓冲区 (sizeof(TFunction) = 64)
 *   - 支持: operator(), 隐式 bool, Reset, Swap
 *   - 可封装: 函数指针, Lambda, 仿函数, 绑定成员函数
 *   - 拷贝语义: 可调用对象必须可拷贝
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h,
 *          Core/TypeTraits/TypeTraits.h, Core/Memory/MemoryOps.h,
 *          Core/Memory/DefaultAllocator.h
 *
 * 注意事项:
 *   封装的可调用对象必须是可拷贝的
 *   调用空 TFunction 触发断言
 *   TFunction 不是线程安全的
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

// 主模板 — 未特化时不定义
template<typename Signature>
class TFunction;

// ============================================================================
// TFunction<ReturnType(Args...)> — 偏特化
// ============================================================================

template<typename ReturnType, typename... Args>
class TFunction<ReturnType(Args...)>
{
    // SBO 内联缓冲区大小 — 56 字节 + 8 字节虚表指针 = 64 字节
    static constexpr SizeType kSBOSize = 56;

    // ========================================================================
    // 内部类型擦除基类
    // ========================================================================

    struct ICallable
    {
        virtual ~ICallable() = default;
        virtual ReturnType Invoke(Args... args) = 0;
        virtual void CopyTo(void* destination) const = 0;
        virtual void MoveTo(void* destination) = 0;
        virtual SizeType GetSize() const = 0;
    };

    /// 具体可调用对象的类型擦除包装
    template<typename Functor>
    struct CallableImpl final : ICallable
    {
        Functor m_Functor;

        explicit CallableImpl(const Functor& functor)
            : m_Functor(functor)
        {
        }

        explicit CallableImpl(Functor&& functor)
            : m_Functor(MoveTemp(functor))
        {
        }

        ReturnType Invoke(Args... args) override
        {
            return m_Functor(Forward<Args>(args)...);
        }

        void CopyTo(void* destination) const override
        {
            new (destination) CallableImpl(m_Functor);
        }

        void MoveTo(void* destination) override
        {
            new (destination) CallableImpl(MoveTemp(m_Functor));
        }

        SizeType GetSize() const override
        {
            return sizeof(CallableImpl);
        }
    };

public:
    // ========================================================================
    // 构造与析构
    // ========================================================================

    /// 默认构造 — 空函数
    TFunction()
        : m_Callable(nullptr)
        , m_IsHeap(false)
        , m_Allocator(&GetDefaultAllocator())
    {
    }

    /// nullptr 构造
    TFunction(decltype(nullptr))
        : m_Callable(nullptr)
        , m_IsHeap(false)
        , m_Allocator(&GetDefaultAllocator())
    {
    }

    /// 从可调用对象构造
    template<typename Functor>
    TFunction(Functor&& functor)
        : m_Callable(nullptr)
        , m_IsHeap(false)
        , m_Allocator(&GetDefaultAllocator())
    {
        using DecayedFunctor = DecayT<Functor>;
        using ImplType = CallableImpl<DecayedFunctor>;

        if constexpr (sizeof(ImplType) <= kSBOSize)
        {
            // SBO 路径 — 内联存储
            m_Callable = new (&m_Storage) ImplType(Forward<Functor>(functor));
            m_IsHeap = false;
        }
        else
        {
            // 堆分配路径
            void* memory = m_Allocator->Allocate(sizeof(ImplType), alignof(ImplType));
            m_Callable = new (memory) ImplType(Forward<Functor>(functor));
            m_IsHeap = true;
        }
    }

    /// 拷贝构造
    TFunction(const TFunction& other)
        : m_Callable(nullptr)
        , m_IsHeap(false)
        , m_Allocator(other.m_Allocator)
    {
        if (other.m_Callable)
        {
            if (other.m_IsHeap)
            {
                SizeType size = other.m_Callable->GetSize();
                void* memory = m_Allocator->Allocate(size, kDefaultAlignment);
                other.m_Callable->CopyTo(memory);
                m_Callable = static_cast<ICallable*>(memory);
                m_IsHeap = true;
            }
            else
            {
                other.m_Callable->CopyTo(&m_Storage);
                m_Callable = reinterpret_cast<ICallable*>(&m_Storage);
                m_IsHeap = false;
            }
        }
    }

    /// 移动构造
    TFunction(TFunction&& other) noexcept
        : m_Callable(nullptr)
        , m_IsHeap(false)
        , m_Allocator(other.m_Allocator)
    {
        if (other.m_Callable)
        {
            if (other.m_IsHeap)
            {
                // 堆对象直接转移指针
                m_Callable = other.m_Callable;
                m_IsHeap = true;
                other.m_Callable = nullptr;
                other.m_IsHeap = false;
            }
            else
            {
                // SBO 对象需要移动到本地存储
                other.m_Callable->MoveTo(&m_Storage);
                m_Callable = reinterpret_cast<ICallable*>(&m_Storage);
                m_IsHeap = false;
                other.m_Callable->~ICallable();
                other.m_Callable = nullptr;
            }
        }
    }

    /// 析构
    ~TFunction()
    {
        Destroy();
    }

    // ========================================================================
    // 赋值运算符
    // ========================================================================

    TFunction& operator=(const TFunction& other)
    {
        if (this != &other)
        {
            TFunction temp(other);
            Swap(temp);
        }
        return *this;
    }

    TFunction& operator=(TFunction&& other) noexcept
    {
        if (this != &other)
        {
            Destroy();
            m_Callable = nullptr;
            m_IsHeap = false;
            m_Allocator = other.m_Allocator;

            if (other.m_Callable)
            {
                if (other.m_IsHeap)
                {
                    m_Callable = other.m_Callable;
                    m_IsHeap = true;
                    other.m_Callable = nullptr;
                    other.m_IsHeap = false;
                }
                else
                {
                    other.m_Callable->MoveTo(&m_Storage);
                    m_Callable = reinterpret_cast<ICallable*>(&m_Storage);
                    m_IsHeap = false;
                    other.m_Callable->~ICallable();
                    other.m_Callable = nullptr;
                }
            }
        }
        return *this;
    }

    TFunction& operator=(decltype(nullptr))
    {
        Destroy();
        m_Callable = nullptr;
        m_IsHeap = false;
        return *this;
    }

    /// 从新的可调用对象赋值
    template<typename Functor>
    TFunction& operator=(Functor&& functor)
    {
        TFunction temp(Forward<Functor>(functor));
        Swap(temp);
        return *this;
    }

    // ========================================================================
    // 调用
    // ========================================================================

    /// 调用封装的可调用对象
    ReturnType operator()(Args... args) const
    {
        LIMX_ASSERT(m_Callable != nullptr);
        return m_Callable->Invoke(Forward<Args>(args)...);
    }

    // ========================================================================
    // 状态查询
    // ========================================================================

    /// 是否持有可调用对象
    LIMX_NODISCARD FORCEINLINE explicit operator bool() const
    {
        return m_Callable != nullptr;
    }

    LIMX_NODISCARD FORCEINLINE bool IsValid() const
    {
        return m_Callable != nullptr;
    }

    /// 是否使用堆分配
    LIMX_NODISCARD FORCEINLINE bool IsHeapAllocated() const
    {
        return m_IsHeap;
    }

    // ========================================================================
    // 所有权管理
    // ========================================================================

    /// 重置为空
    void Reset()
    {
        Destroy();
        m_Callable = nullptr;
        m_IsHeap = false;
    }

    /// 交换
    void Swap(TFunction& other) noexcept
    {
        // 简单实现: 通过移动构造实现交换
        TFunction temp(MoveTemp(*this));
        *this = MoveTemp(other);
        other = MoveTemp(temp);
    }

    // ========================================================================
    // 比较
    // ========================================================================

    LIMX_NODISCARD friend bool operator==(const TFunction& func, decltype(nullptr))
    {
        return func.m_Callable == nullptr;
    }

    LIMX_NODISCARD friend bool operator!=(const TFunction& func, decltype(nullptr))
    {
        return func.m_Callable != nullptr;
    }

private:
    void Destroy()
    {
        if (m_Callable)
        {
            if (m_IsHeap)
            {
                m_Callable->~ICallable();
                m_Allocator->Deallocate(m_Callable);
            }
            else
            {
                m_Callable->~ICallable();
            }
        }
    }

    // ========================================================================
    // 成员数据
    // ========================================================================

    alignas(16) UInt8 m_Storage[kSBOSize];  ///< SBO 内联缓冲区
    ICallable*   m_Callable;                 ///< 可调用对象指针 (SBO 或堆)
    bool         m_IsHeap;                   ///< 是否使用堆分配
    IAllocator*  m_Allocator;                ///< 内存分配器
};

} // namespace Limx
