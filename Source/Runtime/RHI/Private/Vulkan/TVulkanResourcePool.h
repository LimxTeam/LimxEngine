// ============================================================
// 文件名称：TVulkanResourcePool.h
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：基于 THandle<Tag> 的代检测资源池，分配 O(1)，释放 O(1)，
//          查找 O(1)。空闲槽位通过索引栈复用，代递增防止悬挂引用。
// 功能描述：Vulkan 后端内部使用的泛型资源池模板，将 Vulkan 原生对象
//          (VkBuffer/VkImage/VkPipeline 等) 与 THandle 句柄系统桥接，
//          提供类型安全的分配、释放、查找操作。
// 技术特性：TArray + 并行代数组 + 空闲索引栈，无锁单线程设计；
//          释放时递增代计数器使旧句柄自动失效。
//
// ── 函数/方法表 ──────────────────────────────────────────────
// │ 函数名                    │ 描述                           │
// │──────────────────────────│───────────────────────────────│
// │ Allocate()               │ 分配槽位并返回句柄               │
// │ Free()                   │ 释放槽位并使句柄失效              │
// │ Get()                    │ 通过句柄查找资源数据指针           │
// │ IsValid()                │ 检查句柄是否指向有效资源           │
// │ Clear()                  │ 清空所有槽位 (不析构 Vulkan 对象)  │
// │ GetSize()                │ 获取已分配槽位总数                │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                           │
// │─────────────│──────────│───────────────────────────────│
// │ 2026-04-06  │ LimxTeam  │ 初始创建                        │
// ============================================================

#pragma once

#include "RHI/RHIMinimal.h"

namespace Limx
{

/// Vulkan 资源池 — 将 THandle<Tag> 句柄与内部数据结构绑定
/// @tparam TData Vulkan 资源内部数据类型 (如 FVulkanBufferData)
/// @tparam TTag  句柄标签类型 (如 RHITags::BufferTag)
template<typename TData, typename TTag>
class TVulkanResourcePool
{
public:
    // ========================================================================
    // 构造/析构
    // ========================================================================

    TVulkanResourcePool() = default;
    ~TVulkanResourcePool() = default;

    // ========================================================================
    // 分配
    // ========================================================================

    /// 分配一个槽位并存入数据，返回对应句柄
    THandle<TTag> Allocate(const TData& data)
    {
        UInt32 index;

        if (m_FreeIndices.GetSize() > 0)
        {
            // 复用已释放的槽位
            index = m_FreeIndices[m_FreeIndices.GetSize() - 1];
            m_FreeIndices.RemoveAt(m_FreeIndices.GetSize() - 1);
            m_Data[index] = data;
        }
        else
        {
            // 追加新槽位
            index = static_cast<UInt32>(m_Data.GetSize());
            m_Data.Add(data);
            m_Generations.Add(0);
        }

        return THandle<TTag>(index, m_Generations[index]);
    }

    // ========================================================================
    // 释放
    // ========================================================================

    /// 释放句柄指向的槽位，递增代使旧句柄失效
    void Free(THandle<TTag>& handle)
    {
        if (!handle.IsValid())
        {
            return;
        }

        UInt32 index = handle.GetIndex();
        if (index >= static_cast<UInt32>(m_Data.GetSize()))
        {
            return;
        }
        if (m_Generations[index] != handle.GetGeneration())
        {
            return;
        }

        // 递增代计数器使所有旧句柄失效
        m_Generations[index]++;

        // 清空数据
        m_Data[index] = TData{};

        // 回收索引
        m_FreeIndices.Add(index);

        // 使传入句柄失效
        handle.Invalidate();
    }

    // ========================================================================
    // 查找
    // ========================================================================

    /// 通过句柄获取资源数据指针，句柄无效或过期返回 nullptr
    LIMX_NODISCARD TData* Get(THandle<TTag> handle)
    {
        if (!handle.IsValid())
        {
            return nullptr;
        }

        UInt32 index = handle.GetIndex();
        if (index >= static_cast<UInt32>(m_Data.GetSize()))
        {
            return nullptr;
        }
        if (m_Generations[index] != handle.GetGeneration())
        {
            return nullptr;
        }

        return &m_Data[index];
    }

    /// 通过句柄获取资源数据常量指针
    LIMX_NODISCARD const TData* Get(THandle<TTag> handle) const
    {
        if (!handle.IsValid())
        {
            return nullptr;
        }

        UInt32 index = handle.GetIndex();
        if (index >= static_cast<UInt32>(m_Data.GetSize()))
        {
            return nullptr;
        }
        if (m_Generations[index] != handle.GetGeneration())
        {
            return nullptr;
        }

        return &m_Data[index];
    }

    // ========================================================================
    // 状态查询
    // ========================================================================

    /// 检查句柄是否指向有效资源
    LIMX_NODISCARD bool IsValid(THandle<TTag> handle) const
    {
        if (!handle.IsValid())
        {
            return false;
        }

        UInt32 index = handle.GetIndex();
        if (index >= static_cast<UInt32>(m_Data.GetSize()))
        {
            return false;
        }

        return m_Generations[index] == handle.GetGeneration();
    }

    /// 获取已分配槽位总数 (含已释放但未复用的)
    LIMX_NODISCARD SizeType GetSize() const
    {
        return m_Data.GetSize();
    }

    // ========================================================================
    // 清空
    // ========================================================================

    /// 清空所有槽位 — 注意: 不会调用 Vulkan 销毁函数，
    /// 调用方必须先遍历销毁所有 Vulkan 对象再调用此方法
    void Clear()
    {
        m_Data.Clear();
        m_Generations.Clear();
        m_FreeIndices.Clear();
    }

private:
    TArray<TData>  m_Data;         // 资源数据数组
    TArray<UInt32> m_Generations;  // 每个槽位的代计数器
    TArray<UInt32> m_FreeIndices;  // 可复用的空闲索引栈
};

} // namespace Limx
