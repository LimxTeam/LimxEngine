/*******************************************************************************
 * 文件: CoreMacros.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   引擎核心工具宏定义集合
 *   提供编译器提示、断言、内存布局、位操作、字符串化等基础设施
 *   所有宏均以 LIMX_ 前缀命名，避免与外部代码冲突
 *
 * 设计哲学:
 *   编译时可消除 — Shipping 构建中所有断言和检查宏展开为空操作
 *   零开销抽象 — 编译器提示类宏不产生额外运行时指令
 *   单一定义 — 每个宏有且仅有一个定义点
 *
 * 技术特性:
 *   - 函数属性: FORCEINLINE, NOINLINE, LIMX_NODISCARD, LIMX_NORETURN
 *   - 分支提示: LIMX_LIKELY, LIMX_UNLIKELY
 *   - 断言系统: LIMX_ASSERT, LIMX_CHECK, LIMX_VERIFY, LIMX_ENSURE
 *   - 内存布局: LIMX_ALIGNOF, LIMX_ALIGNAS, LIMX_OFFSET_OF
 *   - 实用工具: LIMX_ARRAY_COUNT, LIMX_STRINGIFY, LIMX_CONCAT
 *   - 禁止复制/移动: LIMX_NON_COPYABLE, LIMX_NON_MOVABLE
 *   - 位操作: LIMX_BIT, LIMX_HAS_FLAG, LIMX_SET_FLAG, LIMX_CLEAR_FLAG
 *
 * 依赖关系:
 *   内部: Core/HAL/Platform.h
 *
 * 注意事项:
 *   断言失败在 Debug 构建中触发调试器断点 (__debugbreak)
 *   Shipping 构建中 LIMX_ASSERT 和 LIMX_CHECK 被完全消除
 *   LIMX_VERIFY 和 LIMX_ENSURE 在 Shipping 中仍执行表达式但不检查结果
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"

// ============================================================================
// 函数属性宏
// ============================================================================

/// 强制内联 — 提示编译器必须内联此函数
#if LIMX_COMPILER_MSVC
    #define FORCEINLINE    __forceinline
#elif LIMX_COMPILER_CLANG || LIMX_COMPILER_GCC
    #define FORCEINLINE    inline __attribute__((always_inline))
#else
    #define FORCEINLINE    inline
#endif

/// 禁止内联 — 确保函数不会被内联（用于调试/性能分析关键路径）
#if LIMX_COMPILER_MSVC
    #define NOINLINE __declspec(noinline)
#elif LIMX_COMPILER_CLANG || LIMX_COMPILER_GCC
    #define NOINLINE __attribute__((noinline))
#else
    #define NOINLINE
#endif

/// 返回值不可忽略 — C++17 [[nodiscard]]
#define LIMX_NODISCARD [[nodiscard]]

/// 函数不会返回 — C++11 [[noreturn]]
#define LIMX_NORETURN [[noreturn]]

/// 标记为已废弃
#define LIMX_DEPRECATED(Message) [[deprecated(Message)]]

/// 标记可能未使用的变量（消除编译器警告）
#define LIMX_UNUSED(x) ((void)(x))

/// 限制指针别名 — 告知编译器指针不重叠，允许更激进的优化
#if LIMX_COMPILER_MSVC
    #define LIMX_RESTRICT __restrict
#elif LIMX_COMPILER_CLANG || LIMX_COMPILER_GCC
    #define LIMX_RESTRICT __restrict__
#else
    #define LIMX_RESTRICT
#endif

// ============================================================================
// 分支预测提示
// ============================================================================

/// 分支大概率成立 — 帮助 CPU 分支预测器优化
#if LIMX_COMPILER_CLANG || LIMX_COMPILER_GCC
    #define LIMX_LIKELY(x)   __builtin_expect(!!(x), 1)
    #define LIMX_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
    #define LIMX_LIKELY(x)   (x)
    #define LIMX_UNLIKELY(x) (x)
#endif

// ============================================================================
// 编译器内部函数
// ============================================================================

/// 触发调试器断点
#if LIMX_COMPILER_MSVC
    #define LIMX_DEBUG_BREAK() __debugbreak()
#elif LIMX_COMPILER_CLANG || LIMX_COMPILER_GCC
    #define LIMX_DEBUG_BREAK() __builtin_trap()
#else
    #define LIMX_DEBUG_BREAK() ((void)0)
#endif

/// 标记不可达代码路径 — 编译器可据此进行优化
#if LIMX_COMPILER_MSVC
    #define LIMX_UNREACHABLE() __assume(false)
#elif LIMX_COMPILER_CLANG || LIMX_COMPILER_GCC
    #define LIMX_UNREACHABLE() __builtin_unreachable()
#else
    #define LIMX_UNREACHABLE() ((void)0)
#endif

// ============================================================================
// 断言系统
//
// LIMX_ASSERT(expr)    — Debug 构建中检查，Shipping 完全消除
// LIMX_CHECK(expr)     — Debug+Development 构建中检查，Shipping 消除
// LIMX_VERIFY(expr)    — 始终执行表达式，非 Shipping 检查结果
// LIMX_ENSURE(expr)    — 同 VERIFY，但首次失败时触发断点
// LIMX_STATIC_ASSERT   — 编译时断言 (直接使用 static_assert)
// ============================================================================

#if LIMX_BUILD_DEBUG

    #define LIMX_ASSERT(expr) \
        do \
        { \
            if (LIMX_UNLIKELY(!(expr))) \
            { \
                LIMX_DEBUG_BREAK(); \
            } \
        } while (false)

    #define LIMX_ASSERT_MSG(expr, msg) \
        do \
        { \
            if (LIMX_UNLIKELY(!(expr))) \
            { \
                LIMX_DEBUG_BREAK(); \
            } \
        } while (false)

#else
    #define LIMX_ASSERT(expr)          ((void)0)
    #define LIMX_ASSERT_MSG(expr, msg) ((void)0)
#endif

#if LIMX_BUILD_WITH_CHECKS

    #define LIMX_CHECK(expr) \
        do \
        { \
            if (LIMX_UNLIKELY(!(expr))) \
            { \
                LIMX_DEBUG_BREAK(); \
            } \
        } while (false)

    #define LIMX_CHECK_MSG(expr, msg) \
        do \
        { \
            if (LIMX_UNLIKELY(!(expr))) \
            { \
                LIMX_DEBUG_BREAK(); \
            } \
        } while (false)

    #define LIMX_VERIFY(expr) \
        do \
        { \
            if (LIMX_UNLIKELY(!(expr))) \
            { \
                LIMX_DEBUG_BREAK(); \
            } \
        } while (false)

    #define LIMX_ENSURE(expr) \
        ([&]() -> bool \
        { \
            static bool s_HasFired = false; \
            bool bResult = !!(expr); \
            if (LIMX_UNLIKELY(!bResult && !s_HasFired)) \
            { \
                s_HasFired = true; \
                LIMX_DEBUG_BREAK(); \
            } \
            return bResult; \
        }())

#else
    #define LIMX_CHECK(expr)          ((void)0)
    #define LIMX_CHECK_MSG(expr, msg) ((void)0)
    #define LIMX_VERIFY(expr)         ((void)(expr))
    #define LIMX_ENSURE(expr)         (!!(expr))
#endif

// ============================================================================
// 字符串化与拼接
// ============================================================================

/// 将宏参数转换为字符串字面量
#define LIMX_STRINGIFY_IMPL(x) #x
#define LIMX_STRINGIFY(x)      LIMX_STRINGIFY_IMPL(x)

/// 拼接两个宏标记
#define LIMX_CONCAT_IMPL(a, b) a##b
#define LIMX_CONCAT(a, b)      LIMX_CONCAT_IMPL(a, b)

/// 生成唯一名称 (基于行号)
#define LIMX_UNIQUE_NAME(prefix) LIMX_CONCAT(prefix, __LINE__)

// ============================================================================
// 内存布局
// ============================================================================

/// 获取类型或表达式的对齐要求 (字节)
#define LIMX_ALIGNOF(type) alignof(type)

/// 指定变量/类型的对齐方式
#define LIMX_ALIGNAS(alignment) alignas(alignment)

/// 获取成员在结构体中的偏移量 (字节)
/// 使用编译器内建而非 <cstddef> 中的 offsetof
#if LIMX_COMPILER_MSVC
    #define LIMX_OFFSET_OF(type, member) \
        (static_cast<Limx::SizeType>( \
            reinterpret_cast<Limx::UIntPtr>( \
                &reinterpret_cast<const volatile char&>( \
                    static_cast<type*>(nullptr)->member))))
#else
    #define LIMX_OFFSET_OF(type, member) __builtin_offsetof(type, member)
#endif

// ============================================================================
// 数组与容量
// ============================================================================

/// 编译时获取 C 数组元素数量 — 类型安全版本
/// 对非数组类型调用时会触发编译错误
namespace Limx::Detail
{
    template<typename T, decltype(sizeof(0)) N>
    constexpr decltype(sizeof(0)) ArrayCountHelper(const T (&)[N]) noexcept
    {
        return N;
    }
} // namespace Limx::Detail

#define LIMX_ARRAY_COUNT(array) Limx::Detail::ArrayCountHelper(array)

// ============================================================================
// 位操作
// ============================================================================

/// 生成第 n 位为 1 的位掩码 (0-indexed)
#define LIMX_BIT(n) (1ULL << (n))

/// 检查 flags 中是否设置了 flag
#define LIMX_HAS_FLAG(flags, flag) (((flags) & (flag)) == (flag))

/// 在 flags 中设置 flag
#define LIMX_SET_FLAG(flags, flag) ((flags) |= (flag))

/// 在 flags 中清除 flag
#define LIMX_CLEAR_FLAG(flags, flag) ((flags) &= ~(flag))

/// 在 flags 中切换 flag
#define LIMX_TOGGLE_FLAG(flags, flag) ((flags) ^= (flag))

/// 为 enum class 定义位运算符 (operator|, operator&, operator^, operator~, 及复合赋值)
/// 用于将 enum class 作为类型安全的位掩码使用
#define LIMX_DEFINE_ENUM_BITWISE_OPS(EnumType)                                   \
    inline constexpr EnumType operator|(EnumType a, EnumType b)                  \
    {                                                                            \
        using T = __underlying_type(EnumType);                                   \
        return static_cast<EnumType>(static_cast<T>(a) | static_cast<T>(b));     \
    }                                                                            \
    inline constexpr EnumType operator&(EnumType a, EnumType b)                  \
    {                                                                            \
        using T = __underlying_type(EnumType);                                   \
        return static_cast<EnumType>(static_cast<T>(a) & static_cast<T>(b));     \
    }                                                                            \
    inline constexpr EnumType operator^(EnumType a, EnumType b)                  \
    {                                                                            \
        using T = __underlying_type(EnumType);                                   \
        return static_cast<EnumType>(static_cast<T>(a) ^ static_cast<T>(b));     \
    }                                                                            \
    inline constexpr EnumType operator~(EnumType a)                              \
    {                                                                            \
        using T = __underlying_type(EnumType);                                   \
        return static_cast<EnumType>(~static_cast<T>(a));                        \
    }                                                                            \
    inline EnumType& operator|=(EnumType& a, EnumType b)                         \
    {                                                                            \
        return a = a | b;                                                        \
    }                                                                            \
    inline EnumType& operator&=(EnumType& a, EnumType b)                         \
    {                                                                            \
        return a = a & b;                                                        \
    }                                                                            \
    inline EnumType& operator^=(EnumType& a, EnumType b)                         \
    {                                                                            \
        return a = a ^ b;                                                        \
    }                                                                            \
    inline constexpr bool EnumHasAnyFlags(EnumType flags, EnumType test)          \
    {                                                                            \
        using T = __underlying_type(EnumType);                                   \
        return (static_cast<T>(flags) & static_cast<T>(test)) != 0;              \
    }                                                                            \
    inline constexpr bool EnumHasAllFlags(EnumType flags, EnumType test)          \
    {                                                                            \
        using T = __underlying_type(EnumType);                                   \
        return (static_cast<T>(flags) & static_cast<T>(test))                    \
            == static_cast<T>(test);                                             \
    }

// ============================================================================
// 类特性控制
// ============================================================================

/// 禁止拷贝构造和拷贝赋值
#define LIMX_NON_COPYABLE(ClassName) \
    ClassName(const ClassName&) = delete; \
    ClassName& operator=(const ClassName&) = delete

/// 禁止移动构造和移动赋值
#define LIMX_NON_MOVABLE(ClassName) \
    ClassName(ClassName&&) = delete; \
    ClassName& operator=(ClassName&&) = delete

/// 同时禁止拷贝和移动
#define LIMX_NON_COPYABLE_NON_MOVABLE(ClassName) \
    LIMX_NON_COPYABLE(ClassName); \
    LIMX_NON_MOVABLE(ClassName)

// ============================================================================
// 平台特定扩展
// ============================================================================

/// CPU 缓存行大小 (字节) — 用于避免伪共享
inline constexpr Limx::SizeType kCacheLineSize = 64;

/// 缓存行对齐
#define LIMX_CACHE_LINE_ALIGNED LIMX_ALIGNAS(64)

// ============================================================================
// LIMX_CRT_IMPORT — 与 CRT 声明保持一致的链接属性
// ============================================================================
//
// 本工程不包含 CRT 头文件, 少数几个 CRT 函数由我们自己前向声明。问题是
// 第三方库 (Vulkan SDK、stb) 会间接把真正的 CRT 头文件拉进同一个翻译
// 单元, 于是同一个函数被声明两次 —— 一次带 dllimport, 一次不带, MSVC
// 报 C4273 "inconsistent dll linkage"。
//
// UCRT 里绝大多数字符串函数 (strlen、strcmp、strstr、strchr) 声明时不带
// dllimport, 因为编译器把它们当内建处理; 只有少数带 _ACRTIMP, 例如
// strncmp。所以这里不是给所有声明一律加, 而是哪个带就给哪个加。
//
// _ACRTIMP 的条件是 `!defined _CORECRT_BUILD && defined _DLL`, 我们照抄:
// 用 /MD 链接动态 CRT 时为 dllimport, 静态 CRT (/MT) 时为空。
//
// 为什么不继续用 #pragma warning(disable: 4273): 那是把不一致藏起来,
// 而这里是把它消除。压制还有个副作用 —— 同一段区域里真正的链接不一致
// 也会被一并吞掉。
#if LIMX_COMPILER_MSVC && defined(_DLL)
    #define LIMX_CRT_IMPORT __declspec(dllimport)
#else
    #define LIMX_CRT_IMPORT
#endif
