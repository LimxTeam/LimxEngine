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
#include "Core/Memory/MemoryOps.h"

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

    /// 释放 — 不带尺寸信息的 IAllocator 标准入口
    ///
    /// 必须先定位指针归属的桶再释放。早期实现直接转交 m_Fallback，
    /// 那会把桶内 chunk 里的地址当作独立堆块归还给系统分配器，
    /// 造成堆损坏; 而通过 IAllocator 接口多态使用本分配器的调用方
    /// (如注入到容器) 走的正是这条无尺寸路径, 因此该缺陷必然被触发。
    ///
    /// 现按桶逐个询问归属: 命中则由该桶回收, 全部未命中才说明是
    /// 超出池管理范围、当初由 m_Fallback 分出的大块。
    void Deallocate(void* ptr) override
    {
        if (!ptr) return;

        for (SizeType index = 0; index < kBucketCount; ++index)
        {
            if (m_Buckets[index] && m_Buckets[index]->Owns(ptr))
            {
                m_Buckets[index]->Deallocate(ptr);
                return;
            }
        }

        // 无桶认领 — 只可能来自超大分配的回退路径
        m_Fallback->Deallocate(ptr);
    }

    /// 重分配 — 按新尺寸重新选桶并搬迁数据
    ///
    /// 旧块尺寸由其归属桶的块大小给出, 因此无需调用方提供原始尺寸;
    /// 拷贝量取新旧尺寸的较小者, 避免读越界。
    LIMX_NODISCARD void* Reallocate(
        void* ptr,
        SizeType newSize,
        SizeType alignment = kDefaultAlignment) override
    {
        if (ptr == nullptr)
        {
            return Allocate(newSize, alignment);
        }

        if (newSize == 0)
        {
            Deallocate(ptr);
            return nullptr;
        }

        // 定位旧块所在桶以获知其容量
        SizeType oldCapacity = 0;
        SizeType ownerBucket = kBucketCount;

        for (SizeType index = 0; index < kBucketCount; ++index)
        {
            if (m_Buckets[index] && m_Buckets[index]->Owns(ptr))
            {
                oldCapacity = m_Buckets[index]->GetBlockSize();
                ownerBucket = index;
                break;
            }
        }

        if (ownerBucket == kBucketCount)
        {
            // 不属于任何桶 — 原先走的是回退分配器, 继续交给它处理
            return m_Fallback->Reallocate(ptr, newSize, alignment);
        }

        // 新尺寸仍在原块容量内 — 无需搬迁
        if (newSize <= oldCapacity)
        {
            return ptr;
        }

        void* newBlock = Allocate(newSize, alignment);
        if (newBlock == nullptr)
        {
            // 分配失败时原内存必须保持有效
            return nullptr;
        }

        Memory::MemCopy(newBlock, ptr, oldCapacity);
        m_Buckets[ownerBucket]->Deallocate(ptr);

        return newBlock;
    }

    LIMX_NODISCARD const AnsiChar* GetName() const override
    {
        return "FPoolAllocator";
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
