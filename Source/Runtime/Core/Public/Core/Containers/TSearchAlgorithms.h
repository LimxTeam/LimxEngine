/*******************************************************************************
 * 文件: TSearchAlgorithms.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   搜索算法集合 — 提供线性查找、二分查找、下界/上界
 *   用于已排序和未排序数组的元素查找
 *   适用于 TArray、原始指针数组等随机访问序列
 *
 * 设计哲学:
 *   泛型接口 — 接受指针 + 大小 + 可选比较器/谓词
 *   索引返回 — 返回找到的索引，未找到返回 kNotFound
 *   STL 无关 — 零 STL 依赖
 *
 * 技术特性:
 *   - LinearSearch: 线性查找 O(n)
 *   - BinarySearch: 二分查找 O(log n)，要求已排序
 *   - LowerBound: 第一个 >= value 的位置
 *   - UpperBound: 第一个 > value 的位置
 *   - FindIf: 按谓词线性查找
 *   - Contains: 检查是否包含
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

/// 未找到标记
static constexpr SizeType kNotFound = static_cast<SizeType>(-1);

// ============================================================================
// 线性查找
// ============================================================================

/// 线性查找 — 返回第一个等于 value 的索引
template<typename T>
LIMX_NODISCARD SizeType LinearSearch(const T* data, SizeType count,
                                       const T& value)
{
    for (SizeType index = 0; index < count; ++index)
    {
        if (data[index] == value) return index;
    }
    return kNotFound;
}

/// 按谓词线性查找 — 返回第一个满足谓词的索引
template<typename T, typename Predicate>
LIMX_NODISCARD SizeType FindIf(const T* data, SizeType count,
                                 Predicate pred)
{
    for (SizeType index = 0; index < count; ++index)
    {
        if (pred(data[index])) return index;
    }
    return kNotFound;
}

/// 检查是否包含
template<typename T>
LIMX_NODISCARD bool Contains(const T* data, SizeType count,
                               const T& value)
{
    return LinearSearch(data, count, value) != kNotFound;
}

/// 统计元素出现次数
template<typename T>
LIMX_NODISCARD SizeType CountOf(const T* data, SizeType count,
                                  const T& value)
{
    SizeType result = 0;
    for (SizeType index = 0; index < count; ++index)
    {
        if (data[index] == value) ++result;
    }
    return result;
}

// ============================================================================
// 二分查找 (要求已排序)
// ============================================================================

/// 二分查找 — 返回等于 value 的索引 (未找到返回 kNotFound)
/// @param data  已排序数组
/// @param count 元素数
/// @param value 查找值
template<typename T>
LIMX_NODISCARD SizeType BinarySearch(const T* data, SizeType count,
                                       const T& value)
{
    SizeType low = 0;
    SizeType high = count;

    while (low < high)
    {
        SizeType mid = low + (high - low) / 2;

        if (data[mid] == value)
        {
            return mid;
        }
        else if (data[mid] < value)
        {
            low = mid + 1;
        }
        else
        {
            high = mid;
        }
    }

    return kNotFound;
}

/// 二分查找 — 自定义比较器
/// @param less 比较器 less(a, b) 当 a < b 时返回 true
template<typename T, typename Predicate>
LIMX_NODISCARD SizeType BinarySearch(const T* data, SizeType count,
                                       const T& value,
                                       Predicate less)
{
    SizeType low = 0;
    SizeType high = count;

    while (low < high)
    {
        SizeType mid = low + (high - low) / 2;

        if (less(data[mid], value))
        {
            low = mid + 1;
        }
        else if (less(value, data[mid]))
        {
            high = mid;
        }
        else
        {
            return mid; // 相等
        }
    }

    return kNotFound;
}

// ============================================================================
// 下界/上界 (要求已排序)
// ============================================================================

/// 下界 — 第一个 >= value 的索引 (不存在则返回 count)
template<typename T>
LIMX_NODISCARD SizeType LowerBound(const T* data, SizeType count,
                                     const T& value)
{
    SizeType low = 0;
    SizeType high = count;

    while (low < high)
    {
        SizeType mid = low + (high - low) / 2;
        if (data[mid] < value)
        {
            low = mid + 1;
        }
        else
        {
            high = mid;
        }
    }

    return low;
}

/// 下界 — 自定义比较器
template<typename T, typename Predicate>
LIMX_NODISCARD SizeType LowerBound(const T* data, SizeType count,
                                     const T& value, Predicate less)
{
    SizeType low = 0;
    SizeType high = count;

    while (low < high)
    {
        SizeType mid = low + (high - low) / 2;
        if (less(data[mid], value))
        {
            low = mid + 1;
        }
        else
        {
            high = mid;
        }
    }

    return low;
}

/// 上界 — 第一个 > value 的索引 (不存在则返回 count)
template<typename T>
LIMX_NODISCARD SizeType UpperBound(const T* data, SizeType count,
                                     const T& value)
{
    SizeType low = 0;
    SizeType high = count;

    while (low < high)
    {
        SizeType mid = low + (high - low) / 2;
        if (value < data[mid])
        {
            high = mid;
        }
        else
        {
            low = mid + 1;
        }
    }

    return low;
}

/// 上界 — 自定义比较器
template<typename T, typename Predicate>
LIMX_NODISCARD SizeType UpperBound(const T* data, SizeType count,
                                     const T& value, Predicate less)
{
    SizeType low = 0;
    SizeType high = count;

    while (low < high)
    {
        SizeType mid = low + (high - low) / 2;
        if (less(value, data[mid]))
        {
            high = mid;
        }
        else
        {
            low = mid + 1;
        }
    }

    return low;
}

/// 等值范围 — 返回 [lower, upper) 的索引对
template<typename T>
void EqualRange(const T* data, SizeType count, const T& value,
                 SizeType& outLower, SizeType& outUpper)
{
    outLower = LowerBound(data, count, value);
    outUpper = UpperBound(data, count, value);
}

} // namespace Limx
