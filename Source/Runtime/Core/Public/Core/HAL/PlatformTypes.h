/*******************************************************************************
 * 文件: PlatformTypes.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   引擎基础类型定义 — 完全脱离 STL 的类型系统
 *   定义所有固定宽度整数、浮点数、指针尺寸类型、字符类型
 *   以及类型范围常量，为整个引擎提供统一的类型基础
 *
 * 设计哲学:
 *   零 STL 依赖 — 所有类型均基于 C++ 内建类型别名
 *   编译时验证 — 使用 static_assert 确保类型宽度符合预期
 *   语义清晰 — 类型名称直接表达宽度和符号性 (Int32, UInt64, etc.)
 *
 * 技术特性:
 *   - 固定宽度整数: Int8/16/32/64, UInt8/16/32/64
 *   - 浮点类型: Float32 (IEEE 754 单精度), Float64 (IEEE 754 双精度)
 *   - 指针相关: SizeType, PtrDiffType, IntPtr, UIntPtr
 *   - 字符类型: AnsiChar, WideChar, Char8, Char16, Char32, TChar
 *   - 字节类型: Byte
 *   - 类型范围常量: kInt8Min ~ kUInt64Max
 *   - 编译时宽度验证
 *
 * 依赖关系:
 *   内部: Core/HAL/Platform.h (编译器/平台检测)
 *
 * 注意事项:
 *   禁止包含任何 STL 头文件 (<cstdint>, <cstddef>, <climits> 等)
 *   所有类型宽度均通过 static_assert 在编译时验证
 *   当前仅保证在 MSVC x64 / Clang x64 / GCC x64 上正确
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"

namespace Limx
{

// ============================================================================
// 固定宽度有符号整数
// ============================================================================

/// 8 位有符号整数 [-128, 127]
using Int8 = signed char;

/// 16 位有符号整数 [-32768, 32767]
using Int16 = signed short;

/// 32 位有符号整数 [-2147483648, 2147483647]
using Int32 = signed int;

/// 64 位有符号整数
using Int64 = signed long long;

// ============================================================================
// 固定宽度无符号整数
// ============================================================================

/// 8 位无符号整数 [0, 255]
using UInt8 = unsigned char;

/// 16 位无符号整数 [0, 65535]
using UInt16 = unsigned short;

/// 32 位无符号整数 [0, 4294967295]
using UInt32 = unsigned int;

/// 64 位无符号整数
using UInt64 = unsigned long long;

// ============================================================================
// 浮点类型
// ============================================================================

/// 32 位浮点数 (IEEE 754 单精度, 约 7 位有效数字)
using Float32 = float;

/// 64 位浮点数 (IEEE 754 双精度, 约 15 位有效数字)
using Float64 = double;

// ============================================================================
// 字节类型
// ============================================================================

/// 单字节无符号类型，用于原始内存操作
using Byte = unsigned char;

// ============================================================================
// 指针/尺寸相关类型
// ============================================================================

/// 无符号尺寸类型 — 等价于 std::size_t，但无 STL 依赖
/// 通过 decltype(sizeof(0)) 从编译器内建推导
using SizeType = decltype(sizeof(0));

/// 有符号指针差值类型 — 等价于 std::ptrdiff_t
/// 用于两个指针之间的距离
using PtrDiffType = decltype(static_cast<char*>(nullptr) - static_cast<char*>(nullptr));

/// 有符号指针尺寸整数 — 可安全存储指针值的有符号整数
#if LIMX_POINTER_SIZE == 8
    using IntPtr = signed long long;
#else
    using IntPtr = signed int;
#endif

/// 无符号指针尺寸整数 — 可安全存储指针值的无符号整数
#if LIMX_POINTER_SIZE == 8
    using UIntPtr = unsigned long long;
#else
    using UIntPtr = unsigned int;
#endif

// ============================================================================
// 字符类型
// ============================================================================

/// ANSI 字符 (1 字节, 用于 ASCII / UTF-8 代码单元)
using AnsiChar = char;

/// 宽字符 (Windows: 2 字节 UTF-16, Linux/macOS: 4 字节 UTF-32)
using WideChar = wchar_t;

/// UTF-8 代码单元 (C++20)
using Char8 = char8_t;

/// UTF-16 代码单元
using Char16 = char16_t;

/// UTF-32 代码点
using Char32 = char32_t;

/// 引擎默认字符类型 — UTF-8
/// 所有引擎内部字符串均使用 UTF-8 编码
/// 仅在与 Win32 API 交互时转换为 UTF-16
using TChar = AnsiChar;

// ============================================================================
// 布尔类型别名 (文档用途，C++ bool 已满足需求)
// ============================================================================

/// 显式布尔类型，与 C++ 内建 bool 完全一致
/// 仅用于类型表语义明确性 (如反射系统中区分 Bool 与 Int8)
using Bool = bool;

// ============================================================================
// 空指针常量
// ============================================================================

/// 类型安全的空指针常量 — 直接使用 C++11 nullptr 关键字
/// 此处仅做文档声明，实际代码中直接使用 nullptr

// ============================================================================
// 类型范围常量 (不依赖 <climits> 或 <limits>)
// ============================================================================

// --- 有符号整数范围 ---

inline constexpr Int8  kInt8Min  = -128;
inline constexpr Int8  kInt8Max  = 127;
inline constexpr Int16 kInt16Min = -32768;
inline constexpr Int16 kInt16Max = 32767;
inline constexpr Int32 kInt32Min = (-2147483647 - 1);
inline constexpr Int32 kInt32Max = 2147483647;
inline constexpr Int64 kInt64Min = (-9223372036854775807LL - 1);
inline constexpr Int64 kInt64Max = 9223372036854775807LL;

// --- 无符号整数范围 ---

inline constexpr UInt8  kUInt8Max  = 0xFFU;
inline constexpr UInt16 kUInt16Max = 0xFFFFU;
inline constexpr UInt32 kUInt32Max = 0xFFFFFFFFU;
inline constexpr UInt64 kUInt64Max = 0xFFFFFFFFFFFFFFFFULL;

// --- 浮点数范围 ---

/// Float32 最大有限值 (约 3.4028235e+38)
inline constexpr Float32 kFloat32Max = 3.402823466e+38F;

/// Float32 最小正规化值 (约 1.175494e-38)
inline constexpr Float32 kFloat32Min = 1.175494351e-38F;

/// Float32 机器精度 (约 1.192093e-07)
inline constexpr Float32 kFloat32Epsilon = 1.192092896e-07F;

/// Float64 最大有限值
inline constexpr Float64 kFloat64Max = 1.7976931348623158e+308;

/// Float64 最小正规化值
inline constexpr Float64 kFloat64Min = 2.2250738585072014e-308;

/// Float64 机器精度
inline constexpr Float64 kFloat64Epsilon = 2.2204460492503131e-16;

// --- 尺寸类型范围 ---

inline constexpr SizeType kSizeTypeMax = static_cast<SizeType>(-1);

// --- 常用数学常量 ---

inline constexpr Float64 kPi       = 3.14159265358979323846;
inline constexpr Float64 kTwoPi    = 6.28318530717958647692;
inline constexpr Float64 kHalfPi   = 1.57079632679489661923;
inline constexpr Float64 kInvPi    = 0.31830988618379067154;
inline constexpr Float64 kE        = 2.71828182845904523536;
inline constexpr Float64 kSqrt2    = 1.41421356237309504880;
inline constexpr Float64 kInvSqrt2 = 0.70710678118654752440;

inline constexpr Float32 kPiF       = 3.14159265358979323846F;
inline constexpr Float32 kTwoPiF    = 6.28318530717958647692F;
inline constexpr Float32 kHalfPiF   = 1.57079632679489661923F;
inline constexpr Float32 kInvPiF    = 0.31830988618379067154F;
inline constexpr Float32 kSqrt2F    = 1.41421356237309504880F;
inline constexpr Float32 kInvSqrt2F = 0.70710678118654752440F;

// ============================================================================
// 编译时类型宽度验证
// ============================================================================

static_assert(sizeof(Int8)   == 1, "Int8 必须为 1 字节");
static_assert(sizeof(Int16)  == 2, "Int16 必须为 2 字节");
static_assert(sizeof(Int32)  == 4, "Int32 必须为 4 字节");
static_assert(sizeof(Int64)  == 8, "Int64 必须为 8 字节");

static_assert(sizeof(UInt8)  == 1, "UInt8 必须为 1 字节");
static_assert(sizeof(UInt16) == 2, "UInt16 必须为 2 字节");
static_assert(sizeof(UInt32) == 4, "UInt32 必须为 4 字节");
static_assert(sizeof(UInt64) == 8, "UInt64 必须为 8 字节");

static_assert(sizeof(Float32) == 4, "Float32 必须为 4 字节");
static_assert(sizeof(Float64) == 8, "Float64 必须为 8 字节");

static_assert(sizeof(Byte) == 1, "Byte 必须为 1 字节");
static_assert(sizeof(Bool) == 1, "Bool 必须为 1 字节");

static_assert(sizeof(SizeType) == LIMX_POINTER_SIZE,
    "SizeType 宽度必须等于指针宽度");
static_assert(sizeof(IntPtr) == LIMX_POINTER_SIZE,
    "IntPtr 宽度必须等于指针宽度");
static_assert(sizeof(UIntPtr) == LIMX_POINTER_SIZE,
    "UIntPtr 宽度必须等于指针宽度");

static_assert(sizeof(AnsiChar) == 1, "AnsiChar 必须为 1 字节");
static_assert(sizeof(Char8)    == 1, "Char8 必须为 1 字节");
static_assert(sizeof(Char16)   == 2, "Char16 必须为 2 字节");
static_assert(sizeof(Char32)   == 4, "Char32 必须为 4 字节");

// 验证指针可安全存入 IntPtr/UIntPtr
static_assert(sizeof(void*) == sizeof(IntPtr),
    "指针大小与 IntPtr 不匹配");
static_assert(sizeof(void*) == sizeof(UIntPtr),
    "指针大小与 UIntPtr 不匹配");

} // namespace Limx
