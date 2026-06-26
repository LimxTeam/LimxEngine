/*******************************************************************************
 * 文件: FSubsystem.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   子系统基类 — 引擎各功能子系统的统一生命周期管理接口
 *   定义 Initialize/Shutdown/Tick 等生命周期回调
 *   支持优先级排序，确保子系统按依赖顺序初始化和关闭
 *   用于渲染子系统、物理子系统、音频子系统等统一管理
 *
 * 设计哲学:
 *   接口抽象 — 纯虚基类，各子系统继承并实现
 *   优先级排序 — 数值越小优先级越高 (先初始化，后关闭)
 *   可选 Tick — 默认空实现，需要帧更新的子系统覆盖
 *
 * 技术特性:
 *   - ISubsystem: 子系统纯虚接口
 *   - FSubsystemRegistry: 子系统注册表 (全局管理)
 *   - Initialize/Shutdown: 按优先级顺序初始化/逆序关闭
 *   - Tick: 每帧更新回调
 *   - GetName: 子系统名称 (调试用)
 *
 * 依赖关系:
 *   内部: Core/HAL/PlatformTypes.h, Core/CoreMacros.h,
 *          Core/Containers/TArray.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Containers/TArray.h"

namespace Limx
{

// ============================================================================
// ISubsystem — 子系统接口
// ============================================================================

/// 子系统纯虚接口
class ISubsystem
{
public:
    virtual ~ISubsystem() = default;

    /// 初始化子系统 — 返回是否成功
    virtual bool Initialize() = 0;

    /// 关闭子系统
    virtual void Shutdown() = 0;

    /// 每帧更新 (可选覆盖)
    /// @param deltaTime 帧间隔时间 (秒)
    virtual void Tick(Float32 /*deltaTime*/) {}

    /// 子系统名称 (调试标识)
    LIMX_NODISCARD virtual const AnsiChar* GetName() const = 0;

    /// 初始化优先级 (数值越小越先初始化)
    LIMX_NODISCARD virtual Int32 GetPriority() const { return 1000; }

    /// 是否需要每帧 Tick
    LIMX_NODISCARD virtual bool NeedsTick() const { return false; }
};

// ============================================================================
// FSubsystemRegistry — 子系统注册表
// ============================================================================

/// 子系统注册表 — 管理所有子系统的生命周期
class FSubsystemRegistry
{
public:
    FSubsystemRegistry() : m_IsInitialized(false) {}

    ~FSubsystemRegistry()
    {
        if (m_IsInitialized)
        {
            ShutdownAll();
        }
    }

    // 不可拷贝/移动
    FSubsystemRegistry(const FSubsystemRegistry&) = delete;
    FSubsystemRegistry& operator=(const FSubsystemRegistry&) = delete;

    // ========================================================================
    // 注册
    // ========================================================================

    /// 注册子系统 (注册表不拥有指针所有权)
    void Register(ISubsystem* subsystem)
    {
        LIMX_ASSERT(subsystem != nullptr);
        LIMX_ASSERT(!m_IsInitialized);
        m_Subsystems.Add(subsystem);
    }

    // ========================================================================
    // 生命周期
    // ========================================================================

    /// 按优先级排序后初始化所有子系统
    /// @return 全部初始化成功返回 true
    bool InitializeAll()
    {
        LIMX_ASSERT(!m_IsInitialized);

        // 按优先级排序 (简单插入排序 — 子系统数量通常很少)
        SortByPriority();

        for (SizeType index = 0;
             index < m_Subsystems.GetSize(); ++index)
        {
            if (!m_Subsystems[index]->Initialize())
            {
                // 回滚已初始化的子系统
                for (SizeType rollback = index; rollback > 0; --rollback)
                {
                    m_Subsystems[rollback - 1]->Shutdown();
                }
                return false;
            }
        }

        m_IsInitialized = true;
        return true;
    }

    /// 逆序关闭所有子系统
    void ShutdownAll()
    {
        if (!m_IsInitialized)
        {
            return;
        }

        // 逆序关闭 — 后初始化的先关闭
        for (SizeType index = m_Subsystems.GetSize();
             index > 0; --index)
        {
            m_Subsystems[index - 1]->Shutdown();
        }

        m_IsInitialized = false;
    }

    /// Tick 所有需要更新的子系统
    void TickAll(Float32 deltaTime)
    {
        for (SizeType index = 0;
             index < m_Subsystems.GetSize(); ++index)
        {
            if (m_Subsystems[index]->NeedsTick())
            {
                m_Subsystems[index]->Tick(deltaTime);
            }
        }
    }

    // ========================================================================
    // 查询
    // ========================================================================

    /// 获取子系统数量
    LIMX_NODISCARD SizeType GetCount() const
    {
        return m_Subsystems.GetSize();
    }

    /// 按索引获取子系统
    LIMX_NODISCARD ISubsystem* GetSubsystem(SizeType index) const
    {
        LIMX_ASSERT(index < m_Subsystems.GetSize());
        return m_Subsystems[index];
    }

    /// 按名称查找子系统
    LIMX_NODISCARD ISubsystem* FindByName(const AnsiChar* name) const
    {
        for (SizeType index = 0;
             index < m_Subsystems.GetSize(); ++index)
        {
            const AnsiChar* subsystemName =
                m_Subsystems[index]->GetName();
            if (StringEqual(subsystemName, name))
            {
                return m_Subsystems[index];
            }
        }
        return nullptr;
    }

    /// 是否已初始化
    LIMX_NODISCARD bool IsInitialized() const { return m_IsInitialized; }

    // ========================================================================
    // 全局实例
    // ========================================================================

    static FSubsystemRegistry& Get()
    {
        static FSubsystemRegistry s_Instance;
        return s_Instance;
    }

private:
    /// 按优先级排序 (插入排序)
    void SortByPriority()
    {
        for (SizeType outer = 1;
             outer < m_Subsystems.GetSize(); ++outer)
        {
            ISubsystem* current = m_Subsystems[outer];
            Int32 currentPriority = current->GetPriority();
            SizeType inner = outer;

            while (inner > 0 &&
                   m_Subsystems[inner - 1]->GetPriority() >
                       currentPriority)
            {
                m_Subsystems[inner] = m_Subsystems[inner - 1];
                --inner;
            }
            m_Subsystems[inner] = current;
        }
    }

    /// 字符串比较
    static bool StringEqual(const AnsiChar* a, const AnsiChar* b)
    {
        while (*a && *b)
        {
            if (*a != *b) return false;
            ++a;
            ++b;
        }
        return *a == *b;
    }

    TArray<ISubsystem*> m_Subsystems;     ///< 子系统列表
    bool                m_IsInitialized;  ///< 是否已初始化
};

} // namespace Limx
