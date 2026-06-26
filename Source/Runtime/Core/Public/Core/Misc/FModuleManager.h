/*******************************************************************************
 * 文件: FModuleManager.h
 * 创建时间: 2026-04-06
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   模块管理器 — 引擎模块的注册、加载、卸载和依赖管理
 *   每个模块提供 IModule 接口，由管理器统一管理生命周期
 *   用于引擎各层级模块 (Core, Renderer, Physics 等) 的启动/关闭
 *
 * 设计哲学:
 *   注册表模式 — 模块通过名称注册，按依赖拓扑排序后加载
 *   显式生命周期 — StartupModule / ShutdownModule 明确调用
 *   全局单例 — 进程唯一的模块注册表
 *
 * 技术特性:
 *   - IModule: 模块纯虚接口 (StartupModule/ShutdownModule)
 *   - FModuleManager: 全局管理器 (Register/Load/Unload/Get)
 *   - 按名称查找模块
 *   - 按注册顺序启动/逆序关闭
 *
 * 依赖关系:
 *   内部: Core/Containers/FString.h, Core/Containers/TArray.h,
 *          Core/Templates/TPair.h
 *
 ******************************************************************************/

#pragma once

#include "Core/HAL/Platform.h"
#include "Core/HAL/PlatformTypes.h"
#include "Core/CoreMacros.h"
#include "Core/Containers/FString.h"
#include "Core/Containers/TArray.h"
#include "Core/Templates/TPair.h"

namespace Limx
{

// ============================================================================
// IModule — 模块接口
// ============================================================================

/// 模块纯虚接口
class IModule
{
public:
    virtual ~IModule() = default;

    /// 模块启动 — 初始化模块资源
    virtual void StartupModule() = 0;

    /// 模块关闭 — 释放模块资源
    virtual void ShutdownModule() = 0;

    /// 模块名称
    LIMX_NODISCARD virtual const AnsiChar* GetModuleName() const = 0;
};

// ============================================================================
// FModuleManager — 模块管理器
// ============================================================================

/// 模块管理器
class FModuleManager
{
    /// 模块记录
    struct ModuleEntry
    {
        FString  Name;        ///< 模块名称
        IModule* Module;      ///< 模块指针
        bool     IsLoaded;    ///< 是否已启动
    };

public:
    FModuleManager() = default;

    ~FModuleManager()
    {
        UnloadAll();
    }

    // 不可拷贝/移动
    FModuleManager(const FModuleManager&) = delete;
    FModuleManager& operator=(const FModuleManager&) = delete;

    // ========================================================================
    // 注册与查询
    // ========================================================================

    /// 注册模块 (不启动)
    void Register(IModule* module)
    {
        LIMX_ASSERT(module != nullptr);

        ModuleEntry entry;
        entry.Name = FString(module->GetModuleName());
        entry.Module = module;
        entry.IsLoaded = false;
        m_Modules.Add(MoveTemp(entry));
    }

    /// 按名称查找模块
    LIMX_NODISCARD IModule* FindModule(const AnsiChar* name) const
    {
        FString nameStr(name);
        for (SizeType index = 0;
             index < m_Modules.GetSize(); ++index)
        {
            if (m_Modules[index].Name == nameStr)
            {
                return m_Modules[index].Module;
            }
        }
        return nullptr;
    }

    /// 模块是否已加载
    LIMX_NODISCARD bool IsModuleLoaded(const AnsiChar* name) const
    {
        FString nameStr(name);
        for (SizeType index = 0;
             index < m_Modules.GetSize(); ++index)
        {
            if (m_Modules[index].Name == nameStr)
            {
                return m_Modules[index].IsLoaded;
            }
        }
        return false;
    }

    // ========================================================================
    // 生命周期
    // ========================================================================

    /// 加载指定模块
    bool LoadModule(const AnsiChar* name)
    {
        FString nameStr(name);
        for (SizeType index = 0;
             index < m_Modules.GetSize(); ++index)
        {
            if (m_Modules[index].Name == nameStr &&
                !m_Modules[index].IsLoaded)
            {
                m_Modules[index].Module->StartupModule();
                m_Modules[index].IsLoaded = true;
                return true;
            }
        }
        return false;
    }

    /// 卸载指定模块
    void UnloadModule(const AnsiChar* name)
    {
        FString nameStr(name);
        for (SizeType index = 0;
             index < m_Modules.GetSize(); ++index)
        {
            if (m_Modules[index].Name == nameStr &&
                m_Modules[index].IsLoaded)
            {
                m_Modules[index].Module->ShutdownModule();
                m_Modules[index].IsLoaded = false;
                return;
            }
        }
    }

    /// 按注册顺序加载所有模块
    void LoadAll()
    {
        for (SizeType index = 0;
             index < m_Modules.GetSize(); ++index)
        {
            if (!m_Modules[index].IsLoaded)
            {
                m_Modules[index].Module->StartupModule();
                m_Modules[index].IsLoaded = true;
            }
        }
    }

    /// 逆序卸载所有模块
    void UnloadAll()
    {
        for (SizeType index = m_Modules.GetSize();
             index > 0; --index)
        {
            if (m_Modules[index - 1].IsLoaded)
            {
                m_Modules[index - 1].Module->ShutdownModule();
                m_Modules[index - 1].IsLoaded = false;
            }
        }
    }

    /// 注册模块数量
    LIMX_NODISCARD SizeType GetModuleCount() const
    {
        return m_Modules.GetSize();
    }

    // ========================================================================
    // 全局实例
    // ========================================================================

    static FModuleManager& Get()
    {
        static FModuleManager s_Instance;
        return s_Instance;
    }

private:
    TArray<ModuleEntry> m_Modules;  ///< 模块列表
};

} // namespace Limx
