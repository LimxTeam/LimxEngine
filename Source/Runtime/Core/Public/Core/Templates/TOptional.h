/*******************************************************************************
 * 文件: TOptional.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   可选值类型 — 替代 std::optional 的零 STL 依赖实现
 *   表示一个值可能存在也可能不存在的语义
 *   内联存储，不触发堆分配
 *
 * 设计哲学:
 *   显式语义 — 强制调用者处理"无值"情况，消除空指针误用
 *   零开销 — 内联对齐存储，sizeof = sizeof(T) + 对齐填充 + 1 字节标志
 *   值语义 — 支持拷贝/移动/比较，行为类似基本类型
 *
 * 技术特性:
 *   - 内联对齐存储 (alignas(T))
 *   - Emplace: 原地构造
 *   - GetValue / operator*: 访问值 (带断言)
 *   - GetValueOr: 提供默认值的安全访问
 *   - HasValue / operator bool: 检查是否有值
 *   - Reset: 销毁值并回到无值状态
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h,
 *          Core/TypeTraits/TypeTraits.h, Core/Memory/MemoryOps.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/TypeTraits/TypeTraits.h"
#include "Core/Memory/MemoryOps.h"

namespace Limx
{

/// 空可选值标记类型
struct NullOptT
{
    explicit constexpr NullOptT(int) {}
};

/// 空可选值常量
inline constexpr NullOptT NullOpt{0};

/// 原地构造标记类型
struct InPlaceT
{
    explicit constexpr InPlaceT() = default;
};

/// 原地构造常量
inline constexpr InPlaceT InPlace{};

/// 可选值类型 — 值可能存在也可能不存在
/// @tparam T 被包装的值类型
template<typename T>
class TOptional
{
public:
    using ValueType = T;

    // ========================================================================
    // 构造与析构
    // ========================================================================

    /// 默认构造 — 无值
    TOptional() noexcept
        : m_HasValue(false)
    {
    }

    /// NullOpt 构造 — 显式无值
    TOptional(NullOptT) noexcept
        : m_HasValue(false)
    {
    }

    /// 从值拷贝构造
    TOptional(const T& value)
        : m_HasValue(false)
    {
        Construct(value);
    }

    /// 从值移动构造
    TOptional(T&& value)
        : m_HasValue(false)
    {
        Construct(MoveTemp(value));
    }

    /// 原地构造
    template<typename... Args>
    explicit TOptional(InPlaceT, Args&&... args)
        : m_HasValue(false)
    {
        EmplaceInternal(Forward<Args>(args)...);
    }

    /// 拷贝构造
    TOptional(const TOptional& other)
        : m_HasValue(false)
    {
        if (other.m_HasValue)
        {
            Construct(other.GetValue());
        }
    }

    /// 移动构造
    TOptional(TOptional&& other) noexcept
        : m_HasValue(false)
    {
        if (other.m_HasValue)
        {
            Construct(MoveTemp(other.GetValueMutable()));
            other.Reset();
        }
    }

    /// 析构
    ~TOptional()
    {
        Reset();
    }

    // ========================================================================
    // 赋值运算符
    // ========================================================================

    TOptional& operator=(NullOptT) noexcept
    {
        Reset();
        return *this;
    }

    TOptional& operator=(const T& value)
    {
        if (m_HasValue)
        {
            GetValueMutable() = value;
        }
        else
        {
            Construct(value);
        }
        return *this;
    }

    TOptional& operator=(T&& value)
    {
        if (m_HasValue)
        {
            GetValueMutable() = MoveTemp(value);
        }
        else
        {
            Construct(MoveTemp(value));
        }
        return *this;
    }

    TOptional& operator=(const TOptional& other)
    {
        if (this != &other)
        {
            if (other.m_HasValue)
            {
                if (m_HasValue)
                {
                    GetValueMutable() = other.GetValue();
                }
                else
                {
                    Construct(other.GetValue());
                }
            }
            else
            {
                Reset();
            }
        }
        return *this;
    }

    TOptional& operator=(TOptional&& other) noexcept
    {
        if (this != &other)
        {
            if (other.m_HasValue)
            {
                if (m_HasValue)
                {
                    GetValueMutable() = MoveTemp(other.GetValueMutable());
                }
                else
                {
                    Construct(MoveTemp(other.GetValueMutable()));
                }
                other.Reset();
            }
            else
            {
                Reset();
            }
        }
        return *this;
    }

    // ========================================================================
    // 值访问
    // ========================================================================

    /// 获取值引用 (有断言保护)
    LIMX_NODISCARD FORCEINLINE const T& GetValue() const
    {
        LIMX_ASSERT(m_HasValue);
        return *reinterpret_cast<const T*>(&m_Storage);
    }

    LIMX_NODISCARD FORCEINLINE T& GetValue()
    {
        LIMX_ASSERT(m_HasValue);
        return *reinterpret_cast<T*>(&m_Storage);
    }

    /// 获取值，无值时返回默认值
    LIMX_NODISCARD FORCEINLINE const T& GetValueOr(const T& defaultValue) const
    {
        return m_HasValue ? GetValue() : defaultValue;
    }

    /// 解引用运算符
    LIMX_NODISCARD FORCEINLINE const T& operator*() const
    {
        return GetValue();
    }

    LIMX_NODISCARD FORCEINLINE T& operator*()
    {
        return GetValue();
    }

    /// 箭头运算符
    LIMX_NODISCARD FORCEINLINE const T* operator->() const
    {
        LIMX_ASSERT(m_HasValue);
        return reinterpret_cast<const T*>(&m_Storage);
    }

    LIMX_NODISCARD FORCEINLINE T* operator->()
    {
        LIMX_ASSERT(m_HasValue);
        return reinterpret_cast<T*>(&m_Storage);
    }

    // ========================================================================
    // 状态查询
    // ========================================================================

    /// 是否有值
    LIMX_NODISCARD FORCEINLINE bool HasValue() const { return m_HasValue; }

    /// 隐式 bool 转换
    LIMX_NODISCARD FORCEINLINE explicit operator bool() const { return m_HasValue; }

    // ========================================================================
    // 修改操作
    // ========================================================================

    /// 原地构造值 — 先销毁旧值
    template<typename... Args>
    T& Emplace(Args&&... args)
    {
        Reset();
        return EmplaceInternal(Forward<Args>(args)...);
    }

    /// 销毁值并回到无值状态
    void Reset()
    {
        if (m_HasValue)
        {
            reinterpret_cast<T*>(&m_Storage)->~T();
            m_HasValue = false;
        }
    }

    // ========================================================================
    // 比较运算符
    // ========================================================================

    LIMX_NODISCARD friend bool operator==(const TOptional& lhs, const TOptional& rhs)
    {
        if (lhs.m_HasValue != rhs.m_HasValue)
        {
            return false;
        }
        if (!lhs.m_HasValue)
        {
            return true;  // 两个都无值
        }
        return lhs.GetValue() == rhs.GetValue();
    }

    LIMX_NODISCARD friend bool operator!=(const TOptional& lhs, const TOptional& rhs)
    {
        return !(lhs == rhs);
    }

    LIMX_NODISCARD friend bool operator==(const TOptional& opt, NullOptT)
    {
        return !opt.m_HasValue;
    }

    LIMX_NODISCARD friend bool operator!=(const TOptional& opt, NullOptT)
    {
        return opt.m_HasValue;
    }

    LIMX_NODISCARD friend bool operator==(const TOptional& opt, const T& value)
    {
        return opt.m_HasValue && opt.GetValue() == value;
    }

    LIMX_NODISCARD friend bool operator!=(const TOptional& opt, const T& value)
    {
        return !(opt == value);
    }

private:
    /// 在存储区域拷贝构造
    void Construct(const T& value)
    {
        new (&m_Storage) T(value);
        m_HasValue = true;
    }

    /// 在存储区域移动构造
    void Construct(T&& value)
    {
        new (&m_Storage) T(MoveTemp(value));
        m_HasValue = true;
    }

    /// 在存储区域原地构造
    template<typename... Args>
    T& EmplaceInternal(Args&&... args)
    {
        T* pointer = new (&m_Storage) T(Forward<Args>(args)...);
        m_HasValue = true;
        return *pointer;
    }

    /// 获取可变值引用 (内部用)
    FORCEINLINE T& GetValueMutable()
    {
        return *reinterpret_cast<T*>(&m_Storage);
    }

    // ========================================================================
    // 成员数据
    // ========================================================================

    alignas(T) UInt8 m_Storage[sizeof(T)];  ///< 对齐内联存储
    bool m_HasValue;                          ///< 是否持有值
};

} // namespace Limx
