/*******************************************************************************
 * 文件: TObjectFactory.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   对象工厂 — 基于类型名称的对象注册与实例化
 *   运行时通过字符串名称创建已注册类型的实例
 *   用于反射系统对象创建、序列化反序列化、插件类型注册等场景
 *
 * 设计哲学:
 *   注册表模式 — 全局注册表存储类型名称到创建函数的映射
 *   工厂函数 — 每个注册类型关联一个无参创建函数
 *   类型安全 — 模板基类约束，创建后返回基类指针
 *
 * 技术特性:
 *   - TObjectFactory<BaseType>: 参数化对象工厂
 *   - Register<DerivedType>: 注册派生类型
 *   - Create: 按名称创建实例
 *   - IsRegistered: 是否已注册
 *   - GetRegisteredCount: 已注册类型数
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Containers/FString.h,
 *          Core/Containers/TArray.h, Core/Templates/TUniquePtr.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Containers/FString.h"
#include "Core/Containers/TArray.h"
#include "Core/Templates/TUniquePtr.h"
#include "Core/Memory/IAllocator.h"
#include "Core/Memory/DefaultAllocator.h"

namespace Limx
{

/// 对象工厂
/// @tparam BaseType 基类类型 (所有注册类型必须继承自此)
template<typename BaseType>
class TObjectFactory
{
    /// 创建函数签名
    using FCreatorFunc = BaseType*(*)();

    /// 注册条目
    struct FEntry
    {
        FString      TypeName;  ///< 类型名称
        FCreatorFunc Creator;   ///< 创建函数
    };

public:
    TObjectFactory() = default;

    // ========================================================================
    // 注册
    // ========================================================================

    /// 注册派生类型
    /// @tparam DerivedType 派生类型 (必须有默认构造函数)
    /// @param typeName 类型名称
    template<typename DerivedType>
    void Register(const AnsiChar* typeName)
    {
        // 避免重复注册
        for (SizeType entryIndex = 0;
             entryIndex < m_Entries.GetSize(); ++entryIndex)
        {
            if (m_Entries[entryIndex].TypeName == typeName)
            {
                // 更新已有注册
                m_Entries[entryIndex].Creator = &CreateInstance<DerivedType>;
                return;
            }
        }

        FEntry entry;
        entry.TypeName = typeName;
        entry.Creator = &CreateInstance<DerivedType>;
        m_Entries.Add(MoveTemp(entry));
    }

    /// 注销类型
    bool Unregister(const AnsiChar* typeName)
    {
        for (SizeType entryIndex = 0;
             entryIndex < m_Entries.GetSize(); ++entryIndex)
        {
            if (m_Entries[entryIndex].TypeName == typeName)
            {
                m_Entries.RemoveAt(entryIndex);
                return true;
            }
        }
        return false;
    }

    // ========================================================================
    // 创建
    // ========================================================================

    /// 按名称创建实例 (裸指针，调用者负责释放)
    LIMX_NODISCARD BaseType* Create(const AnsiChar* typeName) const
    {
        FCreatorFunc creator = FindCreator(typeName);
        if (creator == nullptr) return nullptr;
        return creator();
    }

    /// 按名称创建实例 (智能指针)
    LIMX_NODISCARD TUniquePtr<BaseType> CreateUnique(
        const AnsiChar* typeName) const
    {
        return TUniquePtr<BaseType>(Create(typeName));
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 是否已注册
    LIMX_NODISCARD bool IsRegistered(
        const AnsiChar* typeName) const
    {
        return FindCreator(typeName) != nullptr;
    }

    /// 已注册类型数
    LIMX_NODISCARD SizeType GetRegisteredCount() const
    {
        return m_Entries.GetSize();
    }

    /// 获取已注册类型名称列表
    void GetRegisteredTypeNames(TArray<FString>& outNames) const
    {
        outNames.Clear();
        for (SizeType entryIndex = 0;
             entryIndex < m_Entries.GetSize(); ++entryIndex)
        {
            outNames.Add(m_Entries[entryIndex].TypeName);
        }
    }

private:
    /// 模板创建函数
    template<typename DerivedType>
    static BaseType* CreateInstance()
    {
        void* memory = GetDefaultAllocator().Allocate(
            sizeof(DerivedType), alignof(DerivedType));
        return new (memory) DerivedType();
    }

    /// 查找创建函数
    LIMX_NODISCARD FCreatorFunc FindCreator(
        const AnsiChar* typeName) const
    {
        for (SizeType entryIndex = 0;
             entryIndex < m_Entries.GetSize(); ++entryIndex)
        {
            if (m_Entries[entryIndex].TypeName == typeName)
            {
                return m_Entries[entryIndex].Creator;
            }
        }
        return nullptr;
    }

    TArray<FEntry> m_Entries;  ///< 注册条目列表
};

} // namespace Limx
