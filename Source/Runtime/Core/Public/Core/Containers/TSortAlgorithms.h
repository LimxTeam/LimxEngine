/*******************************************************************************
 * 文件: TSortAlgorithms.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   排序算法集合 — 提供 IntroSort、InsertionSort、HeapSort
 *   IntroSort 作为默认排序 — 平均 O(n log n)，最坏 O(n log n)
 *   用于 TArray 排序、索引排序、自定义比较器排序等场景
 *
 * 设计哲学:
 *   泛型接口 — 接受随机访问迭代器 (指针) + 比较器
 *   自适应 — IntroSort 在递归过深时自动退化为 HeapSort
 *   小数组优化 — 元素少于阈值时切换到 InsertionSort
 *
 * 技术特性:
 *   - Sort: IntroSort (默认排序)
 *   - InsertionSort: 插入排序 (小数组优化)
 *   - HeapSort: 堆排序 (最坏 O(n log n) 保底)
 *   - 默认比较器: TLess<T> (升序)
 *   - 支持自定义比较器
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/TypeTraits/TypeTraits.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/TypeTraits/TypeTraits.h"

namespace Limx
{

/// 默认升序比较器
template<typename T>
struct TLess
{
    LIMX_NODISCARD constexpr bool operator()(
        const T& a, const T& b) const
    {
        return a < b;
    }
};

/// 降序比较器
template<typename T>
struct TGreater
{
    LIMX_NODISCARD constexpr bool operator()(
        const T& a, const T& b) const
    {
        return a > b;
    }
};

namespace SortInternal
{

/// 插入排序阈值
static constexpr SizeType kInsertionSortThreshold = 16;

/// 交换两个元素
template<typename T>
FORCEINLINE void SwapElements(T& a, T& b)
{
    T temp = MoveTemp(a);
    a = MoveTemp(b);
    b = MoveTemp(temp);
}

/// 插入排序 — 用于小数组
template<typename T, typename Predicate>
void InsertionSortImpl(T* first, SizeType count, Predicate& pred)
{
    for (SizeType outerIndex = 1; outerIndex < count; ++outerIndex)
    {
        T key = MoveTemp(first[outerIndex]);
        SizeType innerIndex = outerIndex;

        while (innerIndex > 0 && pred(key, first[innerIndex - 1]))
        {
            first[innerIndex] = MoveTemp(first[innerIndex - 1]);
            --innerIndex;
        }

        first[innerIndex] = MoveTemp(key);
    }
}

/// 堆排序辅助 — 下沉
template<typename T, typename Predicate>
void SiftDown(T* data, SizeType nodeIndex, SizeType heapSize,
              Predicate& pred)
{
    while (true)
    {
        SizeType largest = nodeIndex;
        SizeType left = 2 * nodeIndex + 1;
        SizeType right = 2 * nodeIndex + 2;

        if (left < heapSize && pred(data[largest], data[left]))
        {
            largest = left;
        }
        if (right < heapSize && pred(data[largest], data[right]))
        {
            largest = right;
        }

        if (largest == nodeIndex) break;

        SwapElements(data[nodeIndex], data[largest]);
        nodeIndex = largest;
    }
}

/// 堆排序
template<typename T, typename Predicate>
void HeapSortImpl(T* data, SizeType count, Predicate& pred)
{
    if (count < 2) return;

    // 建堆
    for (SizeType parentIndex = count / 2;
         parentIndex > 0; --parentIndex)
    {
        SiftDown(data, parentIndex - 1, count, pred);
    }

    // 排序
    for (SizeType heapEnd = count - 1; heapEnd > 0; --heapEnd)
    {
        SwapElements(data[0], data[heapEnd]);
        SiftDown(data, static_cast<SizeType>(0), heapEnd, pred);
    }
}

/// 三路取中位数枢轴
template<typename T, typename Predicate>
SizeType MedianOfThree(T* data, SizeType a, SizeType b,
                         SizeType c, Predicate& pred)
{
    if (pred(data[a], data[b]))
    {
        if (pred(data[b], data[c])) return b;
        if (pred(data[a], data[c])) return c;
        return a;
    }
    if (pred(data[a], data[c])) return a;
    if (pred(data[b], data[c])) return c;
    return b;
}

/// IntroSort 递归核心
template<typename T, typename Predicate>
void IntroSortImpl(T* data, SizeType count,
                    SizeType depthLimit, Predicate& pred)
{
    while (count > kInsertionSortThreshold)
    {
        if (depthLimit == 0)
        {
            // 递归过深 — 退化为 HeapSort
            HeapSortImpl(data, count, pred);
            return;
        }
        --depthLimit;

        // 三路取中枢轴
        SizeType pivotIndex = MedianOfThree(
            data, 0, count / 2, count - 1, pred);
        SwapElements(data[pivotIndex], data[count - 1]);

        // Lomuto 分区
        SizeType storeIndex = 0;
        for (SizeType scanIndex = 0;
             scanIndex < count - 1; ++scanIndex)
        {
            if (pred(data[scanIndex], data[count - 1]))
            {
                SwapElements(data[storeIndex], data[scanIndex]);
                ++storeIndex;
            }
        }
        SwapElements(data[storeIndex], data[count - 1]);

        // 递归较小的分区，迭代较大的分区
        SizeType leftCount = storeIndex;
        SizeType rightCount = count - storeIndex - 1;

        if (leftCount < rightCount)
        {
            IntroSortImpl(data, leftCount, depthLimit, pred);
            data += storeIndex + 1;
            count = rightCount;
        }
        else
        {
            IntroSortImpl(data + storeIndex + 1, rightCount,
                           depthLimit, pred);
            count = leftCount;
        }
    }

    // 小数组用插入排序
    if (count > 1)
    {
        InsertionSortImpl(data, count, pred);
    }
}

/// 计算深度上限 — 2 * floor(log2(n))
inline SizeType CalculateDepthLimit(SizeType count)
{
    SizeType depth = 0;
    SizeType n = count;
    while (n > 1) { n >>= 1; ++depth; }
    return depth * 2;
}

} // namespace SortInternal

// ============================================================================
// 公开排序接口
// ============================================================================

/// IntroSort — 默认排序 (自定义比较器)
template<typename T, typename Predicate>
void Sort(T* data, SizeType count, Predicate pred)
{
    if (count < 2) return;

    SizeType depthLimit =
        SortInternal::CalculateDepthLimit(count);
    SortInternal::IntroSortImpl(data, count, depthLimit, pred);
}

/// IntroSort — 默认排序 (升序)
template<typename T>
void Sort(T* data, SizeType count)
{
    Sort(data, count, TLess<T>());
}

/// InsertionSort — 插入排序 (自定义比较器)
template<typename T, typename Predicate>
void InsertionSort(T* data, SizeType count, Predicate pred)
{
    if (count < 2) return;
    SortInternal::InsertionSortImpl(data, count, pred);
}

/// InsertionSort — 插入排序 (升序)
template<typename T>
void InsertionSort(T* data, SizeType count)
{
    InsertionSort(data, count, TLess<T>());
}

/// HeapSort — 堆排序 (自定义比较器)
template<typename T, typename Predicate>
void HeapSort(T* data, SizeType count, Predicate pred)
{
    if (count < 2) return;
    SortInternal::HeapSortImpl(data, count, pred);
}

/// HeapSort — 堆排序 (升序)
template<typename T>
void HeapSort(T* data, SizeType count)
{
    HeapSort(data, count, TLess<T>());
}

/// 检查数组是否已排序
template<typename T, typename Predicate>
LIMX_NODISCARD bool IsSorted(const T* data, SizeType count,
                               Predicate pred)
{
    for (SizeType index = 1; index < count; ++index)
    {
        if (pred(data[index], data[index - 1]))
        {
            return false;
        }
    }
    return true;
}

template<typename T>
LIMX_NODISCARD bool IsSorted(const T* data, SizeType count)
{
    return IsSorted(data, count, TLess<T>());
}

} // namespace Limx
