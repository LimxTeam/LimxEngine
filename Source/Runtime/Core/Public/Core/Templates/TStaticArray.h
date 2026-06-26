/*******************************************************************************
 * 文件: TStaticArray.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   编译时固定大小数组 — 替代 std::array 的零 STL 依赖实现
 *   栈上分配的连续内存，大小为编译时常量
 *   用于固定维度向量、查找表、小型缓冲区等场景
 *
 * 设计哲学:
 *   零开销 — 与 C 数组完全相同的内存布局，无额外开销
 *   constexpr — 支持编译时初始化和操作
 *   值语义 — 可拷贝、可比较、可聚合初始化
 *
 * 技术特性:
 *   - 存储: T[N] 内联数组
 *   - 访问: operator[], GetData(), Front(), Back()
 *   - 查询: GetSize(), IsEmpty() (constexpr)
 *   - 填充: Fill(value)
 *   - 迭代: begin()/end() range-based for 支持
 *   - 聚合初始化: TStaticArray<int, 3> a = {1, 2, 3}
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"

namespace Limx
{

/// 编译时固定大小数组
/// @tparam T 元素类型
/// @tparam N 元素数量 (编译时常量)
template<typename T, SizeType N>
struct TStaticArray
{
    // 公开数据成员 — 支持聚合初始化
    T Data[N];

    // ========================================================================
    // 元素访问
    // ========================================================================

    LIMX_NODISCARD constexpr T& operator[](SizeType index)
    {
        LIMX_ASSERT(index < N);
        return Data[index];
    }

    LIMX_NODISCARD constexpr const T& operator[](SizeType index) const
    {
        LIMX_ASSERT(index < N);
        return Data[index];
    }

    LIMX_NODISCARD constexpr T* GetData() { return Data; }
    LIMX_NODISCARD constexpr const T* GetData() const { return Data; }

    LIMX_NODISCARD constexpr T& Front()
    {
        static_assert(N > 0, "Front() on empty TStaticArray");
        return Data[0];
    }

    LIMX_NODISCARD constexpr const T& Front() const
    {
        static_assert(N > 0, "Front() on empty TStaticArray");
        return Data[0];
    }

    LIMX_NODISCARD constexpr T& Back()
    {
        static_assert(N > 0, "Back() on empty TStaticArray");
        return Data[N - 1];
    }

    LIMX_NODISCARD constexpr const T& Back() const
    {
        static_assert(N > 0, "Back() on empty TStaticArray");
        return Data[N - 1];
    }

    // ========================================================================
    // 大小查询
    // ========================================================================

    LIMX_NODISCARD static constexpr SizeType GetSize() { return N; }
    LIMX_NODISCARD static constexpr bool IsEmpty() { return N == 0; }
    LIMX_NODISCARD static constexpr SizeType GetSizeBytes()
    {
        return N * sizeof(T);
    }

    // ========================================================================
    // 操作
    // ========================================================================

    /// 填充所有元素为指定值
    constexpr void Fill(const T& value)
    {
        for (SizeType index = 0; index < N; ++index)
        {
            Data[index] = value;
        }
    }

    // ========================================================================
    // 迭代器
    // ========================================================================

    LIMX_NODISCARD constexpr T* begin() { return Data; }
    LIMX_NODISCARD constexpr const T* begin() const { return Data; }
    LIMX_NODISCARD constexpr T* end() { return Data + N; }
    LIMX_NODISCARD constexpr const T* end() const { return Data + N; }

    // ========================================================================
    // 比较
    // ========================================================================

    LIMX_NODISCARD constexpr bool operator==(
        const TStaticArray& other) const
    {
        for (SizeType index = 0; index < N; ++index)
        {
            if (!(Data[index] == other.Data[index]))
            {
                return false;
            }
        }
        return true;
    }

    LIMX_NODISCARD constexpr bool operator!=(
        const TStaticArray& other) const
    {
        return !(*this == other);
    }
};

} // namespace Limx
