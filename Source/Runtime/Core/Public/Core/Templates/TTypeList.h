/*******************************************************************************
 * 文件: TTypeList.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   编译时类型列表 — 元编程基础设施
 *   提供类型列表的编译时操作: 长度、索引访问、包含检查、追加、过滤等
 *   用于反射系统类型注册、变体类型列表、事件分发表等场景
 *
 * 设计哲学:
 *   纯编译时 — 所有操作在编译时完成，零运行时开销
 *   递归模板 — 经典参数包递归特化模式
 *   组合友好 — 可与 TVariant、TFunction 等模板组合使用
 *
 * 技术特性:
 *   - TTypeList<Types...>: 类型列表容器
 *   - TypeListSize: 列表长度
 *   - TypeListAt: 按索引访问类型
 *   - TypeListContains: 包含检查
 *   - TypeListAppend: 追加类型
 *   - TypeListPrepend: 前置类型
 *   - TypeListIndexOf: 查找类型索引
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/TypeTraits/TypeTraits.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/TypeTraits/TypeTraits.h"

namespace Limx
{

// ============================================================================
// TTypeList — 编译时类型列表
// ============================================================================

/// 类型列表容器
template<typename... Types>
struct TTypeList {};

// ============================================================================
// TypeListSize — 列表长度
// ============================================================================

template<typename List>
struct TypeListSize;

template<typename... Types>
struct TypeListSize<TTypeList<Types...>>
{
    static constexpr SizeType Value = sizeof...(Types);
};

template<typename List>
inline constexpr SizeType TypeListSizeV =
    TypeListSize<List>::Value;

// ============================================================================
// TypeListAt — 按索引访问类型
// ============================================================================

template<SizeType Index, typename List>
struct TypeListAt;

template<typename First, typename... Rest>
struct TypeListAt<0, TTypeList<First, Rest...>>
{
    using Type = First;
};

template<SizeType Index, typename First, typename... Rest>
struct TypeListAt<Index, TTypeList<First, Rest...>>
{
    using Type = typename TypeListAt<
        Index - 1, TTypeList<Rest...>>::Type;
};

template<SizeType Index, typename List>
using TypeListAtT = typename TypeListAt<Index, List>::Type;

// ============================================================================
// TypeListContains — 包含检查
// ============================================================================

template<typename T, typename List>
struct TypeListContains;

template<typename T>
struct TypeListContains<T, TTypeList<>>
    : FalseType {};

template<typename T, typename First, typename... Rest>
struct TypeListContains<T, TTypeList<First, Rest...>>
    : IntegralConstant<bool,
        IsSame<T, First>::Value ||
        TypeListContains<T, TTypeList<Rest...>>::Value> {};

template<typename T, typename List>
inline constexpr bool TypeListContainsV =
    TypeListContains<T, List>::Value;

// ============================================================================
// TypeListIndexOf — 查找类型索引
// ============================================================================

template<typename T, typename List>
struct TypeListIndexOf;

template<typename T>
struct TypeListIndexOf<T, TTypeList<>>
{
    static constexpr SizeType Value = static_cast<SizeType>(-1);
};

template<typename T, typename First, typename... Rest>
struct TypeListIndexOf<T, TTypeList<First, Rest...>>
{
    static constexpr SizeType Value =
        IsSame<T, First>::Value
            ? 0
            : (TypeListIndexOf<T, TTypeList<Rest...>>::Value ==
               static_cast<SizeType>(-1)
                   ? static_cast<SizeType>(-1)
                   : 1 + TypeListIndexOf<
                         T, TTypeList<Rest...>>::Value);
};

template<typename T, typename List>
inline constexpr SizeType TypeListIndexOfV =
    TypeListIndexOf<T, List>::Value;

// ============================================================================
// TypeListAppend — 追加类型
// ============================================================================

template<typename List, typename T>
struct TypeListAppend;

template<typename... Types, typename T>
struct TypeListAppend<TTypeList<Types...>, T>
{
    using Type = TTypeList<Types..., T>;
};

template<typename List, typename T>
using TypeListAppendT = typename TypeListAppend<List, T>::Type;

// ============================================================================
// TypeListPrepend — 前置类型
// ============================================================================

template<typename T, typename List>
struct TypeListPrepend;

template<typename T, typename... Types>
struct TypeListPrepend<T, TTypeList<Types...>>
{
    using Type = TTypeList<T, Types...>;
};

template<typename T, typename List>
using TypeListPrependT = typename TypeListPrepend<T, List>::Type;

// ============================================================================
// TypeListConcat — 连接两个类型列表
// ============================================================================

template<typename ListA, typename ListB>
struct TypeListConcat;

template<typename... TypesA, typename... TypesB>
struct TypeListConcat<TTypeList<TypesA...>, TTypeList<TypesB...>>
{
    using Type = TTypeList<TypesA..., TypesB...>;
};

template<typename ListA, typename ListB>
using TypeListConcatT = typename TypeListConcat<ListA, ListB>::Type;

// ============================================================================
// TypeListHead / TypeListTail — 首元素与尾列表
// ============================================================================

template<typename List>
struct TypeListHead;

template<typename First, typename... Rest>
struct TypeListHead<TTypeList<First, Rest...>>
{
    using Type = First;
};

template<typename List>
using TypeListHeadT = typename TypeListHead<List>::Type;

template<typename List>
struct TypeListTail;

template<typename First, typename... Rest>
struct TypeListTail<TTypeList<First, Rest...>>
{
    using Type = TTypeList<Rest...>;
};

template<typename List>
using TypeListTailT = typename TypeListTail<List>::Type;

// ============================================================================
// TypeListIsEmpty — 是否为空列表
// ============================================================================

template<typename List>
struct TypeListIsEmpty : FalseType {};

template<>
struct TypeListIsEmpty<TTypeList<>> : TrueType {};

template<typename List>
inline constexpr bool TypeListIsEmptyV =
    TypeListIsEmpty<List>::Value;

} // namespace Limx
