/*******************************************************************************
 * 文件: TSlotMap.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   槽位映射 — 基于 Handle (索引+代) 的高性能关联容器
 *   紧凑存储有效元素，通过间接索引表实现 O(1) Handle→元素查找
 *   用于 ECS 实体存储、资源管理器、对象注册表等需要稳定句柄的场景
 *
 * 设计哲学:
 *   间接索引 — Handle.Index → 间接表 → 紧凑数组的实际位置
 *   代验证 — Handle.Generation 与间接表条目的代比较，检测悬挂引用
 *   紧凑遍历 — 有效元素连续存储，缓存友好
 *
 * 技术特性:
 *   - Insert: O(1) 插入，返回 Handle
 *   - Remove: O(1) 删除 (交换尾部 + 更新间接表)
 *   - Get: O(1) Handle→元素查找 (带代验证)
 *   - IsValid: O(1) Handle 有效性检查
 *   - 紧凑数组支持 range-for 遍历
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h,
 *          Core/Containers/TArray.h, Core/Templates/THandle.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Containers/TArray.h"
#include "Core/Templates/THandle.h"

namespace Limx
{

/// 槽位映射 — Handle→元素的 O(1) 关联容器
/// @tparam T   元素类型
/// @tparam Tag THandle 的类型标签
template<typename T, typename Tag>
class TSlotMap
{
    using HandleType = THandle<Tag>;

    /// 间接表条目
    struct IndirectEntry
    {
        UInt32 DataIndex;    ///< 紧凑数组中的位置
        UInt32 Generation;   ///< 当前代 (奇数=已占用, 偶数=空闲)
        Int32  NextFree;     ///< 空闲链表: 下一个空闲条目 (-1=链尾)
    };

    static constexpr Int32 kInvalidFree = -1;

public:
    // ========================================================================
    // 构造与析构
    // ========================================================================

    TSlotMap() : m_FreeHead(kInvalidFree) {}
    ~TSlotMap() = default;

    // 不可拷贝 (TArray 移动语义)
    TSlotMap(const TSlotMap&) = delete;
    TSlotMap& operator=(const TSlotMap&) = delete;

    TSlotMap(TSlotMap&& other) noexcept
        : m_Data(MoveTemp(other.m_Data))
        , m_Indirect(MoveTemp(other.m_Indirect))
        , m_BackMap(MoveTemp(other.m_BackMap))
        , m_FreeHead(other.m_FreeHead)
    {
        other.m_FreeHead = kInvalidFree;
    }

    // ========================================================================
    // 插入
    // ========================================================================

    /// 插入元素 (拷贝) — 返回 Handle
    LIMX_NODISCARD HandleType Insert(const T& value)
    {
        UInt32 slotIndex = AllocateSlot();
        IndirectEntry& slot = m_Indirect[slotIndex];

        // 紧凑数组追加
        UInt32 dataIndex = static_cast<UInt32>(m_Data.GetSize());
        m_Data.Add(value);
        m_BackMap.Add(slotIndex);

        slot.DataIndex = dataIndex;

        return HandleType(slotIndex, slot.Generation);
    }

    /// 插入元素 (移动)
    LIMX_NODISCARD HandleType Insert(T&& value)
    {
        UInt32 slotIndex = AllocateSlot();
        IndirectEntry& slot = m_Indirect[slotIndex];

        UInt32 dataIndex = static_cast<UInt32>(m_Data.GetSize());
        m_Data.Add(MoveTemp(value));
        m_BackMap.Add(slotIndex);

        slot.DataIndex = dataIndex;

        return HandleType(slotIndex, slot.Generation);
    }

    // ========================================================================
    // 删除
    // ========================================================================

    /// 删除 Handle 对应的元素
    /// @return 是否成功 (Handle 无效返回 false)
    bool Remove(HandleType handle)
    {
        if (!IsValid(handle)) return false;

        UInt32 slotIndex = handle.GetIndex();
        IndirectEntry& slot = m_Indirect[slotIndex];
        UInt32 dataIndex = slot.DataIndex;

        // 将紧凑数组最后一个元素交换到被删除位置
        UInt32 lastDataIndex =
            static_cast<UInt32>(m_Data.GetSize()) - 1;

        if (dataIndex != lastDataIndex)
        {
            m_Data[dataIndex] = MoveTemp(m_Data[lastDataIndex]);

            // 更新被交换元素的间接表条目
            UInt32 movedSlot = m_BackMap[lastDataIndex];
            m_Indirect[movedSlot].DataIndex = dataIndex;
            m_BackMap[dataIndex] = movedSlot;
        }

        m_Data.RemoveLast();
        m_BackMap.RemoveLast();

        // 回收间接表条目 (递增代→偶数=空闲)
        slot.Generation++;
        slot.NextFree = m_FreeHead;
        m_FreeHead = static_cast<Int32>(slotIndex);

        return true;
    }

    /// 清空所有元素
    void Clear()
    {
        m_Data.Clear();
        m_BackMap.Clear();

        // 重建空闲链表
        m_FreeHead = kInvalidFree;
        for (SizeType index = m_Indirect.GetSize();
             index > 0; --index)
        {
            SizeType slotIndex = index - 1;
            m_Indirect[slotIndex].Generation++;
            if ((m_Indirect[slotIndex].Generation & 1) != 0)
            {
                m_Indirect[slotIndex].Generation++;
            }
            m_Indirect[slotIndex].NextFree = m_FreeHead;
            m_FreeHead = static_cast<Int32>(slotIndex);
        }
    }

    // ========================================================================
    // 访问
    // ========================================================================

    /// 通过 Handle 获取元素指针 (无效返回 nullptr)
    LIMX_NODISCARD T* Get(HandleType handle)
    {
        if (!IsValid(handle)) return nullptr;
        return &m_Data[m_Indirect[handle.GetIndex()].DataIndex];
    }

    LIMX_NODISCARD const T* Get(HandleType handle) const
    {
        if (!IsValid(handle)) return nullptr;
        return &m_Data[m_Indirect[handle.GetIndex()].DataIndex];
    }

    /// 通过 Handle 获取元素引用 (断言保护)
    LIMX_NODISCARD T& operator[](HandleType handle)
    {
        LIMX_ASSERT(IsValid(handle));
        return m_Data[m_Indirect[handle.GetIndex()].DataIndex];
    }

    LIMX_NODISCARD const T& operator[](HandleType handle) const
    {
        LIMX_ASSERT(IsValid(handle));
        return m_Data[m_Indirect[handle.GetIndex()].DataIndex];
    }

    /// Handle 是否有效
    LIMX_NODISCARD bool IsValid(HandleType handle) const
    {
        UInt32 index = handle.GetIndex();
        if (index >= static_cast<UInt32>(m_Indirect.GetSize()))
        {
            return false;
        }
        return m_Indirect[index].Generation == handle.GetGeneration();
    }

    // ========================================================================
    // 紧凑数组访问 (用于遍历)
    // ========================================================================

    /// 紧凑数组数据指针
    LIMX_NODISCARD T* GetData() { return m_Data.GetData(); }
    LIMX_NODISCARD const T* GetData() const
    {
        return m_Data.GetData();
    }

    /// 元素数量
    LIMX_NODISCARD SizeType GetSize() const
    {
        return m_Data.GetSize();
    }

    /// 是否为空
    LIMX_NODISCARD bool IsEmpty() const
    {
        return m_Data.IsEmpty();
    }

    /// 迭代器 — 遍历紧凑数组中的所有有效元素
    LIMX_NODISCARD T* begin() { return m_Data.GetData(); }
    LIMX_NODISCARD const T* begin() const
    {
        return m_Data.GetData();
    }
    LIMX_NODISCARD T* end()
    {
        return m_Data.GetData() + m_Data.GetSize();
    }
    LIMX_NODISCARD const T* end() const
    {
        return m_Data.GetData() + m_Data.GetSize();
    }

private:
    /// 分配一个间接表条目 — 返回条目索引
    UInt32 AllocateSlot()
    {
        if (m_FreeHead != kInvalidFree)
        {
            UInt32 slotIndex = static_cast<UInt32>(m_FreeHead);
            IndirectEntry& slot = m_Indirect[slotIndex];
            m_FreeHead = slot.NextFree;
            slot.Generation++; // 偶数→奇数 = 已占用
            slot.NextFree = kInvalidFree;
            return slotIndex;
        }

        // 追加新条目
        IndirectEntry newSlot;
        newSlot.DataIndex = 0;
        newSlot.Generation = 1; // 奇数 = 已占用
        newSlot.NextFree = kInvalidFree;

        UInt32 slotIndex =
            static_cast<UInt32>(m_Indirect.GetSize());
        m_Indirect.Add(newSlot);
        return slotIndex;
    }

    // ========================================================================
    // 成员数据
    // ========================================================================

    TArray<T>              m_Data;      ///< 紧凑元素数组
    TArray<IndirectEntry>  m_Indirect;  ///< 间接索引表
    TArray<UInt32>         m_BackMap;   ///< 紧凑数组→间接表的反向映射
    Int32                  m_FreeHead;  ///< 空闲链表头
};

} // namespace Limx
