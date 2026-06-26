/*******************************************************************************
 * 文件: TTypeMap.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   类型映射 — 以 C++ 类型为键的异构值容器
 *   每个类型最多关联一个值，通过编译时类型标识索引
 *   用于服务定位器、依赖注入、组件存储等场景
 *
 * 设计哲学:
 *   类型擦除 — 值以 void* 存储，通过模板方法保证类型安全
 *   运行时类型 ID — 使用函数指针地址作为唯一类型标识
 *   线性查找 — 条目数通常很少，线性扫描足够高效
 *
 * 技术特性:
 *   - TTypeMap: 异构类型映射
 *   - Set<T>: 设置类型关联值
 *   - Get<T>: 获取类型关联值
 *   - Has<T>: 是否包含
 *   - Remove<T>: 移除类型关联
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Containers/TArray.h,
 *          Core/Memory/DefaultAllocator.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Containers/TArray.h"
#include "Core/Memory/MemoryOps.h"
#include "Core/Memory/IAllocator.h"
#include "Core/Memory/DefaultAllocator.h"

namespace Limx
{

/// 类型映射 — 以类型为键的异构容器
class TTypeMap
{
    /// 类型 ID — 利用模板函数实例化的唯一地址
    using FTypeId = void(*)();

    template<typename T>
    static void TypeIdFunc() {}

    template<typename T>
    static FTypeId GetTypeId()
    {
        return &TypeIdFunc<T>;
    }

    /// 析构函数签名
    using FDestructor = void(*)(void*);

    /// 条目
    struct FEntry
    {
        FTypeId     TypeId;      ///< 类型标识
        void*       Data;        ///< 值指针
        SizeType    Size;        ///< 值大小
        FDestructor Destructor;  ///< 析构函数
    };

    /// 类型化析构
    template<typename T>
    static void DestructTyped(void* ptr)
    {
        static_cast<T*>(ptr)->~T();
    }

public:
    TTypeMap() = default;

    ~TTypeMap()
    {
        Clear();
    }

    // 不可拷贝
    TTypeMap(const TTypeMap&) = delete;
    TTypeMap& operator=(const TTypeMap&) = delete;

    // 可移动
    TTypeMap(TTypeMap&& other) noexcept
        : m_Entries(MoveTemp(other.m_Entries))
    {
    }

    TTypeMap& operator=(TTypeMap&& other) noexcept
    {
        if (this != &other)
        {
            Clear();
            m_Entries = MoveTemp(other.m_Entries);
        }
        return *this;
    }

    // ========================================================================
    // 设置
    // ========================================================================

    /// 设置类型关联值 (拷贝)
    template<typename T>
    void Set(const T& value)
    {
        FTypeId typeId = GetTypeId<T>();

        // 查找已有条目
        for (SizeType entryIndex = 0;
             entryIndex < m_Entries.GetSize(); ++entryIndex)
        {
            if (m_Entries[entryIndex].TypeId == typeId)
            {
                // 销毁旧值
                m_Entries[entryIndex].Destructor(
                    m_Entries[entryIndex].Data);
                // 构造新值
                new (m_Entries[entryIndex].Data) T(value);
                return;
            }
        }

        // 新条目
        FEntry entry;
        entry.TypeId = typeId;
        entry.Size = sizeof(T);
        entry.Data = GetDefaultAllocator().Allocate(
            sizeof(T), alignof(T));
        entry.Destructor = &DestructTyped<T>;
        new (entry.Data) T(value);
        m_Entries.Add(entry);
    }

    /// 设置类型关联值 (移动)
    template<typename T>
    void Set(T&& value)
    {
        FTypeId typeId = GetTypeId<T>();

        for (SizeType entryIndex = 0;
             entryIndex < m_Entries.GetSize(); ++entryIndex)
        {
            if (m_Entries[entryIndex].TypeId == typeId)
            {
                m_Entries[entryIndex].Destructor(
                    m_Entries[entryIndex].Data);
                new (m_Entries[entryIndex].Data) T(MoveTemp(value));
                return;
            }
        }

        FEntry entry;
        entry.TypeId = typeId;
        entry.Size = sizeof(T);
        entry.Data = GetDefaultAllocator().Allocate(
            sizeof(T), alignof(T));
        entry.Destructor = &DestructTyped<T>;
        new (entry.Data) T(MoveTemp(value));
        m_Entries.Add(entry);
    }

    // ========================================================================
    // 获取
    // ========================================================================

    /// 获取类型关联值 (可写)
    template<typename T>
    LIMX_NODISCARD T* Get()
    {
        FTypeId typeId = GetTypeId<T>();
        for (SizeType entryIndex = 0;
             entryIndex < m_Entries.GetSize(); ++entryIndex)
        {
            if (m_Entries[entryIndex].TypeId == typeId)
            {
                return static_cast<T*>(
                    m_Entries[entryIndex].Data);
            }
        }
        return nullptr;
    }

    /// 获取类型关联值 (只读)
    template<typename T>
    LIMX_NODISCARD const T* Get() const
    {
        FTypeId typeId = GetTypeId<T>();
        for (SizeType entryIndex = 0;
             entryIndex < m_Entries.GetSize(); ++entryIndex)
        {
            if (m_Entries[entryIndex].TypeId == typeId)
            {
                return static_cast<const T*>(
                    m_Entries[entryIndex].Data);
            }
        }
        return nullptr;
    }

    // ========================================================================
    // 查询与删除
    // ========================================================================

    /// 是否包含类型
    template<typename T>
    LIMX_NODISCARD bool Has() const
    {
        FTypeId typeId = GetTypeId<T>();
        for (SizeType entryIndex = 0;
             entryIndex < m_Entries.GetSize(); ++entryIndex)
        {
            if (m_Entries[entryIndex].TypeId == typeId)
                return true;
        }
        return false;
    }

    /// 移除类型关联
    template<typename T>
    bool Remove()
    {
        FTypeId typeId = GetTypeId<T>();
        for (SizeType entryIndex = 0;
             entryIndex < m_Entries.GetSize(); ++entryIndex)
        {
            if (m_Entries[entryIndex].TypeId == typeId)
            {
                DestroyEntry(m_Entries[entryIndex]);
                m_Entries.RemoveAt(entryIndex);
                return true;
            }
        }
        return false;
    }

    /// 清空所有
    void Clear()
    {
        for (SizeType entryIndex = 0;
             entryIndex < m_Entries.GetSize(); ++entryIndex)
        {
            DestroyEntry(m_Entries[entryIndex]);
        }
        m_Entries.Clear();
    }

    /// 条目数
    LIMX_NODISCARD SizeType GetCount() const
    {
        return m_Entries.GetSize();
    }

    /// 是否为空
    LIMX_NODISCARD bool IsEmpty() const
    {
        return m_Entries.GetSize() == 0;
    }

private:
    void DestroyEntry(FEntry& entry)
    {
        if (entry.Data != nullptr)
        {
            entry.Destructor(entry.Data);
            GetDefaultAllocator().Deallocate(entry.Data);
            entry.Data = nullptr;
        }
    }

    TArray<FEntry> m_Entries;  ///< 条目列表
};

} // namespace Limx
