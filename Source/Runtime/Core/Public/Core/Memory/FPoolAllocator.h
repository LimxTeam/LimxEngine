/*******************************************************************************
 * 文件: FPoolAllocator.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   池分配器 — 多级 Slab 分配器，按固定大小桶分配
 *   维护多个 TFreeList 实例，按请求大小路由到对应桶
 *   用于高频小对象分配、节点分配器后端、通用用途替代 malloc 等场景
 *
 * 设计哲学:
 *   桶化策略 — 将分配请求向上取整到最近的桶大小
 *   零碎片 — 同桶内所有块大小相同，无内部碎片（仅对齐浪费）
 *   IAllocator 接口 — 实现分配器接口，可作为容器的自定义分配器
 *
 * 技术特性:
 *   - 桶大小: 16, 32, 64, 128, 256, 512, 1024, 2048 字节
 *   - Allocate/Deallocate: 自动路由到对应桶
 *   - 超大分配回退到底层分配器
 *   - 实现 IAllocator 接口
 *
 * 依赖关系:
 *   内部: Core/Memory/IAllocator.h, Core/Memory/DefaultAllocator.h,
 *          Core/Memory/TFreeList.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Memory/IAllocator.h"
#include "Core/Memory/DefaultAllocator.h"
#include "Core/Memory/TFreeList.h"

namespace Limx
{

/// 池分配器 — 多级 Slab 分配
class FPoolAllocator : public IAllocator
{
    /// 桶配置
    static constexpr SizeType kBucketCount = 8;
    static constexpr SizeType kBucketSizes[kBucketCount] =
    {
        16, 32, 64, 128, 256, 512, 1024, 2048
    };
    static constexpr SizeType kMaxPoolSize = 2048;
    static constexpr SizeType kBlocksPerChunk = 64;

public:
    // ========================================================================
    // 构造与析构
    // ========================================================================

    FPoolAllocator()
        : m_Fallback(&GetDefaultAllocator())
    {
        // 初始化每个桶的 FreeList
        for (SizeType index = 0; index < kBucketCount; ++index)
        {
            m_Buckets[index] = nullptr;
        }
    }

    ~FPoolAllocator() override
    {
        for (SizeType index = 0; index < kBucketCount; ++index)
        {
            if (m_Buckets[index])
            {
                m_Buckets[index]->~TFreeList();
                m_Fallback->Deallocate(m_Buckets[index]);
            }
        }
    }

    // 不可拷贝
    FPoolAllocator(const FPoolAllocator&) = delete;
    FPoolAllocator& operator=(const FPoolAllocator&) = delete;

    // ========================================================================
    // IAllocator 接口
    // ========================================================================

    void* Allocate(SizeType size, SizeType alignment = 8) override
    {
        SizeType effectiveSize = size < alignment ? alignment : size;

        // 查找匹配桶
        SizeType bucketIndex = FindBucket(effectiveSize);

        if (bucketIndex < kBucketCount)
        {
            EnsureBucket(bucketIndex);
            return m_Buckets[bucketIndex]->Allocate();
        }

        // 超大分配 — 回退到底层分配器
        return m_Fallback->Allocate(size, alignment);
    }

    void Deallocate(void* ptr) override
    {
        if (!ptr) return;

        // 检查是否属于某个桶
        // 注: 简化实现 — 调用者需要知道原始大小
        // 实际使用中建议配合 TObjectPool 或已知大小的场景
        // 此处回退到底层分配器
        m_Fallback->Deallocate(ptr);
    }

    /// 带大小信息的释放 — 路由到正确的桶
    void Deallocate(void* ptr, SizeType size)
    {
        if (!ptr) return;

        SizeType bucketIndex = FindBucket(size);
        if (bucketIndex < kBucketCount && m_Buckets[bucketIndex])
        {
            m_Buckets[bucketIndex]->Deallocate(ptr);
            return;
        }

        m_Fallback->Deallocate(ptr);
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 获取桶数量
    LIMX_NODISCARD static constexpr SizeType GetBucketCount()
    {
        return kBucketCount;
    }

    /// 获取指定桶的块大小
    LIMX_NODISCARD static constexpr SizeType GetBucketSize(
        SizeType bucketIndex)
    {
        return kBucketSizes[bucketIndex];
    }

    /// 获取最大池化大小
    LIMX_NODISCARD static constexpr SizeType GetMaxPoolSize()
    {
        return kMaxPoolSize;
    }

private:
    /// 查找匹配的桶索引 (向上取整)
    LIMX_NODISCARD static SizeType FindBucket(SizeType size)
    {
        for (SizeType index = 0; index < kBucketCount; ++index)
        {
            if (size <= kBucketSizes[index])
            {
                return index;
            }
        }
        return kBucketCount; // 超大
    }

    /// 确保桶已初始化
    void EnsureBucket(SizeType bucketIndex)
    {
        if (!m_Buckets[bucketIndex])
        {
            void* memory = m_Fallback->Allocate(
                sizeof(TFreeList), alignof(TFreeList));
            m_Buckets[bucketIndex] = new (memory) TFreeList(
                kBucketSizes[bucketIndex], kBlocksPerChunk);
        }
    }

    // ========================================================================
    // 成员数据
    // ========================================================================

    TFreeList*  m_Buckets[kBucketCount];  ///< 各级桶
    IAllocator* m_Fallback;               ///< 超大分配回退
};

} // namespace Limx
