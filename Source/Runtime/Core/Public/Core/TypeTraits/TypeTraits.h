/*******************************************************************************
 * 文件: TypeTraits.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   零 STL 依赖的类型特征库 — 替代 <type_traits>
 *   提供编译时类型查询、类型变换、条件选择、完美转发等基础元编程设施
 *   所有特征均通过模板特化在编译时求值，零运行时开销
 *
 * 设计哲学:
 *   完全替代 STL — 禁止包含 <type_traits>, <utility> 等任何标准库头文件
 *   编译时求值 — 所有操作在编译时完成，不产生运行时代码
 *   命名一致 — 使用 PascalCase 风格 (RemoveConst 而非 remove_const)
 *   值语义 — 类型查询提供 ::Value 静态成员和 _v 变量模板
 *
 * 技术特性:
 *   - 类型修饰符操作: RemoveConst, RemoveVolatile, RemoveReference, RemovePointer
 *   - 类型添加操作: AddConst, AddVolatile, AddLValueReference, AddPointer
 *   - 类型查询: IsConst, IsVolatile, IsPointer, IsReference, IsArray
 *   - 类型分类: IsIntegral, IsFloatingPoint, IsArithmetic, IsVoid, IsEnum
 *   - 类型关系: IsSame, IsBaseOf, IsConvertible
 *   - 条件选择: Conditional, EnableIf
 *   - 完美转发: MoveTemp (std::move), Forward (std::forward)
 *   - 其他: DeclVal, IntegralConstant, Decay
 *
 * 依赖关系:
 *   内部: Core/HAL/Platform.h, Core/HAL/PlatformTypes.h
 *
 * 注意事项:
 *   MoveTemp 对应 std::move — 命名遵循 UE 惯例以避免与 STL 混淆
 *   Forward 对应 std::forward
 *   禁止在本文件中包含任何 STL 头文件
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"

namespace Limx
{

// ============================================================================
// 编译时常量
// ============================================================================

/// 编译时整数常量 — 等价于 std::integral_constant
template<typename T, T Val>
struct IntegralConstant
{
    static constexpr T Value = Val;
    using ValueType = T;
    using Type = IntegralConstant;

    constexpr operator ValueType() const noexcept { return Value; }
    constexpr ValueType operator()() const noexcept { return Value; }
};

/// 编译时布尔真
using TrueType = IntegralConstant<bool, true>;

/// 编译时布尔假
using FalseType = IntegralConstant<bool, false>;

// ============================================================================
// 类型修饰符移除
// ============================================================================

/// 移除顶层 const
template<typename T> struct RemoveConst           { using Type = T; };
template<typename T> struct RemoveConst<const T>  { using Type = T; };
template<typename T> using RemoveConstT = typename RemoveConst<T>::Type;

/// 移除顶层 volatile
template<typename T> struct RemoveVolatile              { using Type = T; };
template<typename T> struct RemoveVolatile<volatile T>  { using Type = T; };
template<typename T> using RemoveVolatileT = typename RemoveVolatile<T>::Type;

/// 移除顶层 const 和 volatile
template<typename T> struct RemoveCV { using Type = RemoveVolatileT<RemoveConstT<T>>; };
template<typename T> using RemoveCVT = typename RemoveCV<T>::Type;

/// 移除引用
template<typename T> struct RemoveReference       { using Type = T; };
template<typename T> struct RemoveReference<T&>   { using Type = T; };
template<typename T> struct RemoveReference<T&&>  { using Type = T; };
template<typename T> using RemoveReferenceT = typename RemoveReference<T>::Type;

/// 移除指针
template<typename T> struct RemovePointer                   { using Type = T; };
template<typename T> struct RemovePointer<T*>               { using Type = T; };
template<typename T> struct RemovePointer<T* const>         { using Type = T; };
template<typename T> struct RemovePointer<T* volatile>      { using Type = T; };
template<typename T> struct RemovePointer<T* const volatile>{ using Type = T; };
template<typename T> using RemovePointerT = typename RemovePointer<T>::Type;

/// 移除数组维度
template<typename T> struct RemoveExtent                  { using Type = T; };
template<typename T> struct RemoveExtent<T[]>             { using Type = T; };
template<typename T, decltype(sizeof(0)) N>
struct RemoveExtent<T[N]>                                 { using Type = T; };
template<typename T> using RemoveExtentT = typename RemoveExtent<T>::Type;

// ============================================================================
// 类型修饰符添加
// ============================================================================

/// 添加 const
template<typename T> struct AddConst { using Type = const T; };
template<typename T> using AddConstT = typename AddConst<T>::Type;

/// 添加 volatile
template<typename T> struct AddVolatile { using Type = volatile T; };
template<typename T> using AddVolatileT = typename AddVolatile<T>::Type;

/// 添加左值引用 (对 void 特化为 void)
namespace Detail
{
    template<typename T, typename = void>
    struct AddLValueReferenceHelper { using Type = T; };
    template<typename T>
    struct AddLValueReferenceHelper<T, decltype(void(static_cast<T(*)()>(nullptr)))>
    {
        using Type = T&;
    };
} // namespace Detail

template<typename T> struct AddLValueReference
{
    using Type = typename Detail::AddLValueReferenceHelper<T>::Type;
};
template<typename T> using AddLValueReferenceT =
    typename AddLValueReference<T>::Type;

/// 添加右值引用
namespace Detail
{
    template<typename T, typename = void>
    struct AddRValueReferenceHelper { using Type = T; };
    template<typename T>
    struct AddRValueReferenceHelper<T, decltype(void(static_cast<T(*)()>(nullptr)))>
    {
        using Type = T&&;
    };
} // namespace Detail

template<typename T> struct AddRValueReference
{
    using Type = typename Detail::AddRValueReferenceHelper<T>::Type;
};
template<typename T> using AddRValueReferenceT =
    typename AddRValueReference<T>::Type;

/// 添加指针
template<typename T> struct AddPointer
{
    using Type = RemoveReferenceT<T>*;
};
template<typename T> using AddPointerT = typename AddPointer<T>::Type;

// ============================================================================
// 类型查询 — 修饰符
// ============================================================================

/// 是否为 const
template<typename T> struct IsConst          : FalseType {};
template<typename T> struct IsConst<const T> : TrueType {};
template<typename T> inline constexpr bool IsConstV = IsConst<T>::Value;

/// 是否为 volatile
template<typename T> struct IsVolatile             : FalseType {};
template<typename T> struct IsVolatile<volatile T> : TrueType {};
template<typename T> inline constexpr bool IsVolatileV = IsVolatile<T>::Value;

/// 是否为指针
template<typename T> struct IsPointer                    : FalseType {};
template<typename T> struct IsPointer<T*>                : TrueType {};
template<typename T> struct IsPointer<T* const>          : TrueType {};
template<typename T> struct IsPointer<T* volatile>       : TrueType {};
template<typename T> struct IsPointer<T* const volatile> : TrueType {};
template<typename T> inline constexpr bool IsPointerV = IsPointer<T>::Value;

/// 是否为左值引用
template<typename T> struct IsLValueReference      : FalseType {};
template<typename T> struct IsLValueReference<T&>  : TrueType {};
template<typename T> inline constexpr bool IsLValueReferenceV =
    IsLValueReference<T>::Value;

/// 是否为右值引用
template<typename T> struct IsRValueReference       : FalseType {};
template<typename T> struct IsRValueReference<T&&>  : TrueType {};
template<typename T> inline constexpr bool IsRValueReferenceV =
    IsRValueReference<T>::Value;

/// 是否为引用 (左值或右值)
template<typename T> struct IsReference : IntegralConstant<bool,
    IsLValueReferenceV<T> || IsRValueReferenceV<T>> {};
template<typename T> inline constexpr bool IsReferenceV = IsReference<T>::Value;

/// 是否为数组
template<typename T> struct IsArray                             : FalseType {};
template<typename T> struct IsArray<T[]>                        : TrueType {};
template<typename T, decltype(sizeof(0)) N> struct IsArray<T[N]>: TrueType {};
template<typename T> inline constexpr bool IsArrayV = IsArray<T>::Value;

// ============================================================================
// 类型查询 — 基础类型分类
// ============================================================================

/// 是否为 void
template<typename T> struct IsVoid
    : IntegralConstant<bool, false> {};
template<> struct IsVoid<void>                : TrueType {};
template<> struct IsVoid<const void>          : TrueType {};
template<> struct IsVoid<volatile void>       : TrueType {};
template<> struct IsVoid<const volatile void> : TrueType {};
template<typename T> inline constexpr bool IsVoidV = IsVoid<T>::Value;

/// 是否为空类型 (nullptr_t)
template<typename T> struct IsNullPointer
    : IntegralConstant<bool, false> {};
template<> struct IsNullPointer<decltype(nullptr)> : TrueType {};
template<typename T> inline constexpr bool IsNullPointerV =
    IsNullPointer<RemoveCVT<T>>::Value;

/// 是否为整数类型
namespace Detail
{
    template<typename T> struct IsIntegralHelper : FalseType {};
    template<> struct IsIntegralHelper<bool>               : TrueType {};
    template<> struct IsIntegralHelper<char>                : TrueType {};
    template<> struct IsIntegralHelper<signed char>         : TrueType {};
    template<> struct IsIntegralHelper<unsigned char>       : TrueType {};
    template<> struct IsIntegralHelper<wchar_t>             : TrueType {};
    template<> struct IsIntegralHelper<char8_t>             : TrueType {};
    template<> struct IsIntegralHelper<char16_t>            : TrueType {};
    template<> struct IsIntegralHelper<char32_t>            : TrueType {};
    template<> struct IsIntegralHelper<short>               : TrueType {};
    template<> struct IsIntegralHelper<unsigned short>      : TrueType {};
    template<> struct IsIntegralHelper<int>                 : TrueType {};
    template<> struct IsIntegralHelper<unsigned int>        : TrueType {};
    template<> struct IsIntegralHelper<long>                : TrueType {};
    template<> struct IsIntegralHelper<unsigned long>       : TrueType {};
    template<> struct IsIntegralHelper<long long>           : TrueType {};
    template<> struct IsIntegralHelper<unsigned long long>  : TrueType {};
} // namespace Detail

template<typename T> struct IsIntegral
    : Detail::IsIntegralHelper<RemoveCVT<T>> {};
template<typename T> inline constexpr bool IsIntegralV = IsIntegral<T>::Value;

/// 是否为浮点类型
namespace Detail
{
    template<typename T> struct IsFloatingPointHelper     : FalseType {};
    template<> struct IsFloatingPointHelper<float>        : TrueType {};
    template<> struct IsFloatingPointHelper<double>       : TrueType {};
    template<> struct IsFloatingPointHelper<long double>  : TrueType {};
} // namespace Detail

template<typename T> struct IsFloatingPoint
    : Detail::IsFloatingPointHelper<RemoveCVT<T>> {};
template<typename T> inline constexpr bool IsFloatingPointV =
    IsFloatingPoint<T>::Value;

/// 是否为算术类型 (整数或浮点)
template<typename T> struct IsArithmetic
    : IntegralConstant<bool, IsIntegralV<T> || IsFloatingPointV<T>> {};
template<typename T> inline constexpr bool IsArithmeticV =
    IsArithmetic<T>::Value;

/// 是否为有符号整数
template<typename T> struct IsSigned
    : IntegralConstant<bool,
        IsArithmeticV<T> && (static_cast<RemoveCVT<T>>(-1) < static_cast<RemoveCVT<T>>(0))> {};
template<typename T> inline constexpr bool IsSignedV = IsSigned<T>::Value;

/// 是否为无符号整数
template<typename T> struct IsUnsigned
    : IntegralConstant<bool,
        IsArithmeticV<T> && !IsSignedV<T>> {};
template<typename T> inline constexpr bool IsUnsignedV = IsUnsigned<T>::Value;

/// 是否为枚举类型 — 使用编译器内建
template<typename T> struct IsEnum
    : IntegralConstant<bool, __is_enum(T)> {};
template<typename T> inline constexpr bool IsEnumV = IsEnum<T>::Value;

/// 获取枚举底层类型 — 使用编译器内建 __underlying_type
template<typename T> struct TUnderlyingType
{
    using Type = __underlying_type(T);
};

/// 是否为类/结构体类型 — 使用编译器内建
template<typename T> struct IsClass
    : IntegralConstant<bool, __is_class(T)> {};
template<typename T> inline constexpr bool IsClassV = IsClass<T>::Value;

// ============================================================================
// 类型关系
// ============================================================================

/// 两个类型是否相同
template<typename A, typename B> struct IsSame : FalseType {};
template<typename T> struct IsSame<T, T>       : TrueType {};
template<typename A, typename B> inline constexpr bool IsSameV =
    IsSame<A, B>::Value;

/// B 是否为 A 的基类 — 使用编译器内建
template<typename Base, typename Derived> struct IsBaseOf
    : IntegralConstant<bool, __is_base_of(Base, Derived)> {};
template<typename Base, typename Derived> inline constexpr bool IsBaseOfV =
    IsBaseOf<Base, Derived>::Value;

/// A 是否可隐式转换为 B
namespace Detail
{
    template<typename From, typename To>
    struct IsConvertibleHelper
    {
    private:
        static void TestFunc(To);

        template<typename F,
            typename = decltype(TestFunc(static_cast<F(*)()>(nullptr)()))>
        static TrueType Test(int);

        template<typename>
        static FalseType Test(...);

    public:
        using Type = decltype(Test<From>(0));
    };

    // void -> void 特化
    template<>
    struct IsConvertibleHelper<void, void>
    {
        using Type = TrueType;
    };
} // namespace Detail

template<typename From, typename To> struct IsConvertible
    : Detail::IsConvertibleHelper<From, To>::Type {};
template<typename From, typename To> inline constexpr bool IsConvertibleV =
    IsConvertible<From, To>::Value;

// ============================================================================
// 条件选择
// ============================================================================

/// 编译时条件选择 — 等价于 std::conditional
template<bool Condition, typename TrueT, typename FalseT>
struct Conditional { using Type = TrueT; };

template<typename TrueT, typename FalseT>
struct Conditional<false, TrueT, FalseT> { using Type = FalseT; };

template<bool Condition, typename TrueT, typename FalseT>
using ConditionalT = typename Conditional<Condition, TrueT, FalseT>::Type;

/// SFINAE 启用条件 — 等价于 std::enable_if
template<bool Condition, typename T = void>
struct EnableIf {};

template<typename T>
struct EnableIf<true, T> { using Type = T; };

template<bool Condition, typename T = void>
using EnableIfT = typename EnableIf<Condition, T>::Type;

// ============================================================================
// 类型衰变 (Decay)
// ============================================================================

/// 类型衰变 — 移除引用、cv 限定，数组退化为指针，函数退化为函数指针
template<typename T>
struct Decay
{
private:
    using U = RemoveReferenceT<T>;

public:
    using Type = ConditionalT<
        IsArrayV<U>,
        AddPointerT<RemoveExtentT<U>>,
        RemoveCVT<U>
    >;
};

template<typename T> using DecayT = typename Decay<T>::Type;

// ============================================================================
// 完美转发与移动语义
// ============================================================================

/// DeclVal — 编译时获取类型的右值引用，仅用于 decltype 上下文
/// 等价于 std::declval，禁止在运行时调用
template<typename T>
AddRValueReferenceT<T> DeclVal() noexcept;

/// MoveTemp — 无条件将左值转换为右值引用，启用移动语义
/// 等价于 std::move，命名遵循 UE 惯例以避免与 STL 名称冲突
/// 使用 LIMX_NODISCARD 防止忽略返回值
template<typename T>
LIMX_NODISCARD constexpr RemoveReferenceT<T>&& MoveTemp(T&& arg) noexcept
{
    return static_cast<RemoveReferenceT<T>&&>(arg);
}

/// Forward — 完美转发，保持参数的值类别 (左值/右值)
/// 等价于 std::forward
/// 左值引用版本
template<typename T>
constexpr T&& Forward(RemoveReferenceT<T>& arg) noexcept
{
    return static_cast<T&&>(arg);
}

/// Forward — 右值引用版本
template<typename T>
constexpr T&& Forward(RemoveReferenceT<T>&& arg) noexcept
{
    static_assert(!IsLValueReferenceV<T>,
        "Forward<T> 不能将右值转发为左值引用");
    return static_cast<T&&>(arg);
}

/// Swap — 交换两个值
template<typename T>
constexpr void Swap(T& a, T& b) noexcept
{
    T temp = MoveTemp(a);
    a = MoveTemp(b);
    b = MoveTemp(temp);
}

// ============================================================================
// 辅助类型特征
// ============================================================================

/// 是否为相同类型 (忽略 cv 限定)
template<typename A, typename B>
struct IsSameDecayed : IsSame<RemoveCVT<A>, RemoveCVT<B>> {};
template<typename A, typename B>
inline constexpr bool IsSameDecayedV = IsSameDecayed<A, B>::Value;

/// 是否为 Limx 基础整数类型
template<typename T>
struct IsLimxIntegral : IntegralConstant<bool,
    IsSameDecayedV<T, signed char>         ||
    IsSameDecayedV<T, unsigned char>       ||
    IsSameDecayedV<T, signed short>        ||
    IsSameDecayedV<T, unsigned short>      ||
    IsSameDecayedV<T, signed int>          ||
    IsSameDecayedV<T, unsigned int>        ||
    IsSameDecayedV<T, signed long long>    ||
    IsSameDecayedV<T, unsigned long long>> {};
template<typename T> inline constexpr bool IsLimxIntegralV =
    IsLimxIntegral<T>::Value;

/// 是否为 Limx 浮点类型
template<typename T>
struct IsLimxFloat : IntegralConstant<bool,
    IsSameDecayedV<T, float> || IsSameDecayedV<T, double>> {};
template<typename T> inline constexpr bool IsLimxFloatV =
    IsLimxFloat<T>::Value;

/// VoidT — 用于 SFINAE 检测的 void 别名模板
template<typename...>
using VoidT = void;

} // namespace Limx
