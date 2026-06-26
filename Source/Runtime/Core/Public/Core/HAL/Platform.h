/*******************************************************************************
 * 文件: Platform.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   平台、编译器、架构检测宏定义
 *   在编译时确定目标平台特征，为上层代码提供统一的条件编译接口
 *   所有平台相关的分支判断均通过本文件定义的宏完成
 *
 * 设计哲学:
 *   零运行时开销 — 所有检测均在预处理阶段完成
 *   单一职责 — 仅负责平台特征检测，不包含类型定义
 *   防御性设计 — 未知平台/编译器触发编译错误而非静默降级
 *
 * 技术特性:
 *   - 编译器检测: MSVC, Clang, GCC (含版本号提取)
 *   - 平台检测: Windows, Linux, macOS
 *   - 架构检测: x64, ARM64
 *   - C++ 标准版本检测 (要求 C++23)
 *   - 字节序检测
 *   - 调试/发布模式检测
 *
 * 依赖关系:
 *   无外部依赖 — 本文件是整个引擎的最底层头文件
 *
 * 注意事项:
 *   本文件必须在所有其他引擎头文件之前被包含
 *   禁止在本文件中包含任何其他头文件
 *
 ******************************************************************************/

#pragma once

// ============================================================================
// 编译器检测
// ============================================================================

#if defined(_MSC_VER)
    #define LIMX_COMPILER_MSVC    1
    #define LIMX_COMPILER_CLANG   0
    #define LIMX_COMPILER_GCC     0
    #define LIMX_COMPILER_VERSION _MSC_VER
#elif defined(__clang__)
    #define LIMX_COMPILER_MSVC    0
    #define LIMX_COMPILER_CLANG   1
    #define LIMX_COMPILER_GCC     0
    #define LIMX_COMPILER_VERSION (__clang_major__ * 10000 + __clang_minor__ * 100 + __clang_patchlevel__)
#elif defined(__GNUC__)
    #define LIMX_COMPILER_MSVC    0
    #define LIMX_COMPILER_CLANG   0
    #define LIMX_COMPILER_GCC     1
    #define LIMX_COMPILER_VERSION (__GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__)
#else
    #error "不支持的编译器 — Limx Engine 仅支持 MSVC 19.38+、Clang 16+、GCC 13+"
#endif

// MSVC 最低版本检查 (19.38 = VS 2022 17.8)
#if LIMX_COMPILER_MSVC && LIMX_COMPILER_VERSION < 1938
    #error "MSVC 版本过低 — Limx Engine 要求 MSVC 19.38+ (Visual Studio 2022 17.8+)"
#endif

// ============================================================================
// 平台检测
// ============================================================================

#if defined(_WIN32) || defined(_WIN64)
    #define LIMX_PLATFORM_WINDOWS 1
    #define LIMX_PLATFORM_LINUX   0
    #define LIMX_PLATFORM_MACOS   0
#elif defined(__linux__)
    #define LIMX_PLATFORM_WINDOWS 0
    #define LIMX_PLATFORM_LINUX   1
    #define LIMX_PLATFORM_MACOS   0
#elif defined(__APPLE__) && defined(__MACH__)
    #define LIMX_PLATFORM_WINDOWS 0
    #define LIMX_PLATFORM_LINUX   0
    #define LIMX_PLATFORM_MACOS   1
#else
    #error "不支持的目标平台 — Limx Engine 当前仅支持 Windows、Linux、macOS"
#endif

// ============================================================================
// 架构检测
// ============================================================================

#if defined(_M_X64) || defined(__x86_64__)
    #define LIMX_ARCH_X64   1
    #define LIMX_ARCH_ARM64 0
#elif defined(_M_ARM64) || defined(__aarch64__)
    #define LIMX_ARCH_X64   0
    #define LIMX_ARCH_ARM64 1
#else
    #error "不支持的 CPU 架构 — Limx Engine 当前仅支持 x64 和 ARM64"
#endif

// 指针大小 (字节)
#if defined(_WIN64) || defined(__LP64__) || defined(__x86_64__) || defined(__aarch64__)
    #define LIMX_POINTER_SIZE 8
#else
    #define LIMX_POINTER_SIZE 4
#endif

// ============================================================================
// C++ 标准版本检测
// ============================================================================

#if LIMX_COMPILER_MSVC
    #define LIMX_CPP_VERSION _MSVC_LANG
#else
    #define LIMX_CPP_VERSION __cplusplus
#endif

// MSVC /std:c++latest 在部分版本中 _MSVC_LANG 仍报告 202004 (C++20)
// 但实际已启用 C++23 特性 (constexpr, if consteval, auto(), deducing this 等)
// 因此对 MSVC 放宽检查: 要求 _MSVC_LANG >= 202004 且 _MSC_VER >= 1938
#if LIMX_COMPILER_MSVC
    #if LIMX_CPP_VERSION < 202004L
        #error "C++ 标准版本过低 — Limx Engine 要求 /std:c++latest (MSVC 19.38+)"
    #endif
#else
    #if LIMX_CPP_VERSION < 202302L
        #error "C++ 标准版本过低 — Limx Engine 要求 -std=c++23 (GCC 13+ / Clang 16+)"
    #endif
#endif

// ============================================================================
// 字节序检测
// ============================================================================

// 现代主流平台 (x64/ARM64) 均为小端序
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    #define LIMX_LITTLE_ENDIAN 0
    #define LIMX_BIG_ENDIAN    1
#else
    #define LIMX_LITTLE_ENDIAN 1
    #define LIMX_BIG_ENDIAN    0
#endif

// ============================================================================
// 构建配置检测
// ============================================================================

#if defined(NDEBUG) || defined(LIMX_SHIPPING)
    #define LIMX_BUILD_DEBUG       0
    #define LIMX_BUILD_DEVELOPMENT 0
    #define LIMX_BUILD_SHIPPING    1
#elif defined(LIMX_DEVELOPMENT)
    #define LIMX_BUILD_DEBUG       0
    #define LIMX_BUILD_DEVELOPMENT 1
    #define LIMX_BUILD_SHIPPING    0
#else
    #define LIMX_BUILD_DEBUG       1
    #define LIMX_BUILD_DEVELOPMENT 0
    #define LIMX_BUILD_SHIPPING    0
#endif

// 调试模式总开关 (Debug 或 Development 均视为非 Shipping)
#define LIMX_BUILD_WITH_CHECKS (!LIMX_BUILD_SHIPPING)

// ============================================================================
// SIMD 指令集检测
// ============================================================================

#if defined(__AVX2__) || (LIMX_COMPILER_MSVC && defined(__AVX2__))
    #define LIMX_HAS_AVX2 1
#else
    #define LIMX_HAS_AVX2 0
#endif

#if defined(__AVX__) || (LIMX_COMPILER_MSVC && defined(__AVX__))
    #define LIMX_HAS_AVX 1
#else
    #define LIMX_HAS_AVX 0
#endif

#if defined(__SSE4_2__) || LIMX_COMPILER_MSVC
    // MSVC x64 默认启用 SSE4.2
    #define LIMX_HAS_SSE42 1
#else
    #define LIMX_HAS_SSE42 0
#endif

// ============================================================================
// 平台特定功能标记
// ============================================================================

// 是否支持 Vulkan (当前所有目标平台均支持)
#define LIMX_HAS_VULKAN 1

// 是否支持硬件光线追踪
#define LIMX_HAS_HARDWARE_RAYTRACING 1

// 是否支持 Mesh Shader
#define LIMX_HAS_MESH_SHADER 1
