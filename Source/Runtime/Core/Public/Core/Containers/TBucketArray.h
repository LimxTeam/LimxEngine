/*******************************************************************************
 * 文件: TBucketArray.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   桶数组 — 分页稳定存储的动态数组
 *   以固定大小的桶 (页) 为单位分配，已有元素指针永不失效
 *   用于需要稳定指针的大型集合，如组件存储、实体列表等
 *
 * 设计哲学:
 *   分页存储 — 每桶固定 BucketSize 个元素，桶之间独立分配
 *   指针稳定 — 桶内地址固定，新桶不影响旧元素地址
 *   随机访问 — O(1) 索引计算 (桶号 = index / BucketSize)
 *
 * 技术特性:
 *   - TBucketArray<T, BucketSize>: 桶式动态数组
 *   - Add: 追加元素
 *   - operator[]: 随机访问
 *   - GetSize/GetCapacity: 查询
 *   - Clear: 清空
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h,
 *          Core/Containers/TArray.h, Core/Memory/DefaultAllocator.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Memory/MemoryOps.h"
#include "Core/Memory/DefaultAllocator.h"
#include "Core/Containers/TArray.h"

namespace Limx
{

/// 桶式稳定动态数组
/// @tparam T 元素类型
/// @tparam BucketSize 每桶元素数 (默认 64)
template<typename T, SizeType BucketSize = 64>
class TBucketArray
{
    static_assert(BucketSize > 0,
        "BucketSize must be > 0");

    struct FBucket
    {
        alignas(T) UInt8 Data[sizeof(T) * BucketSize];
        SizeType Count;  ///< 已使用元素数

        FBucket() : Count(0) {}

        T* GetPtr() { return reinterpret_cast<T*>(Data); }
        const T* GetPtr() const
        {
            return reinterpret_cast<const T*>(Data);
        }
    };

public:
    TBucketArray() : m_Size(0) {}

    ~TBucketArray()
    {
        Clear();
    }

    // 不可拷贝 (指针语义)
    TBucketArray(const TBucketArray&) = delete;
    TBucketArray& operator=(const TBucketArray&) = delete;

    // 可移动
    TBucketArray(TBucketArray&& other)
        : m_Buckets(MoveTemp(other.m_Buckets))
        , m_Size(other.m_Size)
    {
        other.m_Size = 0;
    }

    // ========================================================================
    // 追加
    // ========================================================================

    /// 追加元素 (拷贝)
    /// @return 新元素的稳定指针
    T* Add(const T& element)
    {
        FBucket* bucket = GetOrAllocLastBucket();
        SizeType slotIdx = bucket->Count;
        new (bucket->GetPtr() + slotIdx) T(element);
        ++bucket->Count;
        ++m_Size;
        return bucket->GetPtr() + slotIdx;
    }

    /// 追加元素 (移动)
    T* Add(T&& element)
    {
        FBucket* bucket = GetOrAllocLastBucket();
        SizeType slotIdx = bucket->Count;
        new (bucket->GetPtr() + slotIdx)
            T(MoveTemp(element));
        ++bucket->Count;
        ++m_Size;
        return bucket->GetPtr() + slotIdx;
    }

    // ========================================================================
    // 访问
    // ========================================================================

    LIMX_NODISCARD T& operator[](SizeType index)
    {
        LIMX_ASSERT(index < m_Size);
        SizeType bucketIdx = index / BucketSize;
        SizeType localIdx  = index % BucketSize;
        return m_Buckets[bucketIdx]->GetPtr()[localIdx];
    }

    LIMX_NODISCARD const T& operator[](
        SizeType index) const
    {
        LIMX_ASSERT(index < m_Size);
        SizeType bucketIdx = index / BucketSize;
        SizeType localIdx  = index % BucketSize;
        return m_Buckets[bucketIdx]->GetPtr()[localIdx];
    }

    // ========================================================================
    // 查询
    // ========================================================================

    LIMX_NODISCARD SizeType GetSize() const
    {
        return m_Size;
    }

    LIMX_NODISCARD SizeType GetCapacity() const
    {
        return m_Buckets.GetSize() * BucketSize;
    }

    LIMX_NODISCARD bool IsEmpty() const
    {
        return m_Size == 0;
    }

    LIMX_NODISCARD SizeType GetBucketCount() const
    {
        return m_Buckets.GetSize();
    }

    // ========================================================================
    // 清空
    // ========================================================================

    void Clear()
    {
        // 析构所有元素
        for (SizeType bucketIdx = 0;
             bucketIdx < m_Buckets.GetSize(); ++bucketIdx)
        {
            FBucket* bucket = m_Buckets[bucketIdx];
            for (SizeType elemIdx = 0;
                 elemIdx < bucket->Count; ++elemIdx)
            {
                bucket->GetPtr()[elemIdx].~T();
            }
            GetDefaultAllocator().Deallocate(bucket);
        }
        m_Buckets.Clear();
        m_Size = 0;
    }

    // ========================================================================
    // 迭代器
    // ========================================================================

    /// 简单线性迭代器
    struct FIterator
    {
        TBucketArray* m_Owner;
        SizeType      m_Index;

        FIterator(TBucketArray* owner, SizeType index)
            : m_Owner(owner), m_Index(index)
        {}

        T& operator*() { return (*m_Owner)[m_Index]; }
        T* operator->()
        {
            return &(*m_Owner)[m_Index];
        }

        FIterator& operator++()
        {
            ++m_Index;
            return *this;
        }

        bool operator!=(const FIterator& other) const
        {
            return m_Index != other.m_Index;
        }
    };

    struct FConstIterator
    {
        const TBucketArray* m_Owner;
        SizeType            m_Index;

        FConstIterator(const TBucketArray* owner,
                       SizeType index)
            : m_Owner(owner), m_Index(index)
        {}

        const T& operator*() const
        {
            return (*m_Owner)[m_Index];
        }

        FConstIterator& operator++()
        {
            ++m_Index;
            return *this;
        }

        bool operator!=(const FConstIterator& other) const
        {
            return m_Index != other.m_Index;
        }
    };

    FIterator begin()
    {
        return FIterator(this, 0);
    }

    FIterator end()
    {
        return FIterator(this, m_Size);
    }

    FConstIterator begin() const
    {
        return FConstIterator(this, 0);
    }

    FConstIterator end() const
    {
        return FConstIterator(this, m_Size);
    }

private:
    FBucket* GetOrAllocLastBucket()
    {
        if (m_Buckets.GetSize() == 0 ||
            m_Buckets.Last()->Count >= BucketSize)
        {
            FBucket* newBucket =
                static_cast<FBucket*>(
                    GetDefaultAllocator().Allocate(
                        sizeof(FBucket), alignof(FBucket)));
            new (newBucket) FBucket();
            m_Buckets.Add(newBucket);
        }
        return m_Buckets.Last();
    }

    TArray<FBucket*> m_Buckets;  ///< 桶指针列表
    SizeType         m_Size;     ///< 总元素数
};

} // namespace Limx
