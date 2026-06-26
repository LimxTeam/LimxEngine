/*******************************************************************************
 * 文件: FConsoleVariable.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   控制台变量 — 运行时可调参数系统
 *   支持 Int32/Float32/Bool/FString 类型的命名变量
 *   用于渲染参数调优、调试开关、性能配置等运行时可调场景
 *
 * 设计哲学:
 *   全局注册表 — 所有控制台变量通过名称注册到全局表
 *   类型安全 — 每个变量记录类型标签，防止类型混淆
 *   回调通知 — 变量修改时触发回调，便于系统响应
 *
 * 技术特性:
 *   - FConsoleVariable: 单个控制台变量
 *   - FConsoleVariableManager: 全局管理器 (注册/查找/设置)
 *   - 类型: Int32, Float32, Bool
 *   - OnChanged 回调: 变量修改时触发
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/Containers/FString.h,
 *          Core/Containers/TArray.h, Core/Templates/TFunction.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Containers/FString.h"
#include "Core/Containers/TArray.h"
#include "Core/Templates/TFunction.h"

namespace Limx
{

/// 控制台变量类型
enum class EConsoleVarType : UInt8
{
    Int32   = 0,
    Float32 = 1,
    Bool    = 2,
};

/// 控制台变量值 (联合存储)
union FConsoleVarValue
{
    Int32   AsInt32;
    Float32 AsFloat32;
    bool    AsBool;

    constexpr FConsoleVarValue() : AsInt32(0) {}
};

/// 单个控制台变量
class FConsoleVariable
{
public:
    FConsoleVariable()
        : m_Type(EConsoleVarType::Int32)
    {
    }

    /// 构造 Int32 变量
    FConsoleVariable(const AnsiChar* name, Int32 defaultValue,
                      const AnsiChar* description)
        : m_Name(name)
        , m_Description(description)
        , m_Type(EConsoleVarType::Int32)
    {
        m_Value.AsInt32 = defaultValue;
        m_DefaultValue.AsInt32 = defaultValue;
    }

    /// 构造 Float32 变量
    FConsoleVariable(const AnsiChar* name, Float32 defaultValue,
                      const AnsiChar* description)
        : m_Name(name)
        , m_Description(description)
        , m_Type(EConsoleVarType::Float32)
    {
        m_Value.AsFloat32 = defaultValue;
        m_DefaultValue.AsFloat32 = defaultValue;
    }

    /// 构造 Bool 变量
    FConsoleVariable(const AnsiChar* name, bool defaultValue,
                      const AnsiChar* description)
        : m_Name(name)
        , m_Description(description)
        , m_Type(EConsoleVarType::Bool)
    {
        m_Value.AsBool = defaultValue;
        m_DefaultValue.AsBool = defaultValue;
    }

    // ========================================================================
    // 获取
    // ========================================================================

    LIMX_NODISCARD Int32 GetInt32() const
    {
        LIMX_ASSERT(m_Type == EConsoleVarType::Int32);
        return m_Value.AsInt32;
    }

    LIMX_NODISCARD Float32 GetFloat32() const
    {
        LIMX_ASSERT(m_Type == EConsoleVarType::Float32);
        return m_Value.AsFloat32;
    }

    LIMX_NODISCARD bool GetBool() const
    {
        LIMX_ASSERT(m_Type == EConsoleVarType::Bool);
        return m_Value.AsBool;
    }

    // ========================================================================
    // 设置
    // ========================================================================

    void SetInt32(Int32 value)
    {
        LIMX_ASSERT(m_Type == EConsoleVarType::Int32);
        m_Value.AsInt32 = value;
        NotifyChanged();
    }

    void SetFloat32(Float32 value)
    {
        LIMX_ASSERT(m_Type == EConsoleVarType::Float32);
        m_Value.AsFloat32 = value;
        NotifyChanged();
    }

    void SetBool(bool value)
    {
        LIMX_ASSERT(m_Type == EConsoleVarType::Bool);
        m_Value.AsBool = value;
        NotifyChanged();
    }

    /// 重置为默认值
    void ResetToDefault()
    {
        m_Value = m_DefaultValue;
        NotifyChanged();
    }

    // ========================================================================
    // 回调
    // ========================================================================

    /// 注册修改回调
    void SetOnChanged(TFunction<void()> callback)
    {
        m_OnChanged = MoveTemp(callback);
    }

    // ========================================================================
    // 查询
    // ========================================================================

    LIMX_NODISCARD const FString& GetName() const { return m_Name; }
    LIMX_NODISCARD const FString& GetDescription() const
    {
        return m_Description;
    }
    LIMX_NODISCARD EConsoleVarType GetType() const { return m_Type; }

private:
    void NotifyChanged()
    {
        if (m_OnChanged)
        {
            m_OnChanged();
        }
    }

    FString           m_Name;         ///< 变量名
    FString           m_Description;  ///< 描述文本
    EConsoleVarType   m_Type;         ///< 类型标签
    FConsoleVarValue  m_Value;        ///< 当前值
    FConsoleVarValue  m_DefaultValue; ///< 默认值
    TFunction<void()> m_OnChanged;    ///< 修改回调
};

/// 控制台变量全局管理器
class FConsoleVariableManager
{
public:
    /// 获取全局单例
    static FConsoleVariableManager& Get()
    {
        static FConsoleVariableManager s_Instance;
        return s_Instance;
    }

    /// 注册变量 (转移所有权)
    FConsoleVariable* Register(FConsoleVariable&& variable)
    {
        m_Variables.Add(MoveTemp(variable));
        return &m_Variables[m_Variables.GetSize() - 1];
    }

    /// 按名称查找变量
    LIMX_NODISCARD FConsoleVariable* FindByName(
        const AnsiChar* name)
    {
        for (SizeType index = 0;
             index < m_Variables.GetSize(); ++index)
        {
            if (m_Variables[index].GetName() == name)
            {
                return &m_Variables[index];
            }
        }
        return nullptr;
    }

    /// 变量总数
    LIMX_NODISCARD SizeType GetVariableCount() const
    {
        return m_Variables.GetSize();
    }

    /// 按索引访问
    LIMX_NODISCARD FConsoleVariable& GetVariable(SizeType index)
    {
        return m_Variables[index];
    }

private:
    FConsoleVariableManager() = default;
    FConsoleVariableManager(const FConsoleVariableManager&) = delete;
    FConsoleVariableManager& operator=(
        const FConsoleVariableManager&) = delete;

    TArray<FConsoleVariable> m_Variables;  ///< 变量列表
};

} // namespace Limx
