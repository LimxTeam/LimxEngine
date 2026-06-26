/*******************************************************************************
 * 文件: api_generator.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   模块 API 宏生成器
 *   - 为每个模块生成 API 导出宏头文件
 *   - 处理 DLL 导出/导入 (__declspec)
 *   - 生成模块注册代码
 *
 ******************************************************************************/

use anyhow::Result;
use std::fs;
use std::path::Path;

use crate::core::config::{Module, ModuleType};

/// 为模块生成 API 头文件
pub fn generate_module_api_header(module: &Module, output_dir: &Path) -> Result<()> {
    let name = &module.name;
    let name_upper = name.to_uppercase();

    // 提取短名称 (去掉 Limx 前缀)
    let short_name = if name.starts_with("Limx") {
        &name[4..]
    } else {
        name
    };
    let short_name_upper = short_name.to_uppercase();

    let content =
        generate_api_header_content(name, &name_upper, short_name, &short_name_upper, module);

    let output_path = output_dir.join(format!("{}API.generated.h", short_name));
    fs::create_dir_all(output_dir)?;
    fs::write(&output_path, content)?;

    Ok(())
}

fn generate_api_header_content(
    name: &str,
    name_upper: &str,
    short_name: &str,
    short_name_upper: &str,
    module: &Module,
) -> String {
    let default_namespace = format!("Limx::{}", short_name);
    let namespace = module.namespace.as_deref().unwrap_or(&default_namespace);

    format!(
        r#"/*******************************************************************************
 * 文件: {short_name}API.generated.h
 * 
 * 警告: 此文件由 LBT (Limx Build Tool) 自动生成
 *       请勿手动修改，任何修改都会被覆盖
 *
 * 功能描述:
 *   {name} 模块 API 导出宏定义
 *
 ******************************************************************************/

#pragma once

//=============================================================================
// 模块信息
//=============================================================================

#define {name_upper}_MODULE_NAME "{name}"
#define {name_upper}_MODULE_NAMESPACE "{namespace}"

//=============================================================================
// API 导出宏
//=============================================================================

#if defined({name_upper}_EXPORTS)
    // 构建此模块时导出符号
    #define {name_upper}_API __declspec(dllexport)
    #define {name_upper}_TEMPLATE_API
#elif defined({name_upper}_STATIC)
    // 静态链接
    #define {name_upper}_API
    #define {name_upper}_TEMPLATE_API
#else
    // 使用此模块时导入符号
    #define {name_upper}_API __declspec(dllimport)
    #define {name_upper}_TEMPLATE_API extern
#endif

// 简化别名
#define LIMX_{short_name_upper}_API {name_upper}_API

//=============================================================================
// 模块类型标记
//=============================================================================

#define {name_upper}_IS_STATIC {is_static}
#define {name_upper}_IS_SHARED {is_shared}
#define {name_upper}_LAYER {layer}

//=============================================================================
// 模块初始化
//=============================================================================

namespace {namespace}
{{
    /// 模块启动 (引擎启动时调用)
    {name_upper}_API void ModuleStartup();
    
    /// 模块关闭 (引擎关闭时调用)
    {name_upper}_API void ModuleShutdown();
    
    /// 获取模块版本
    {name_upper}_API const char* GetModuleVersion();
}}

//=============================================================================
// 自动模块注册
//=============================================================================

#define LIMX_IMPLEMENT_MODULE_{short_name_upper}() \
    namespace {namespace} \
    {{ \
        static bool s_bModuleInitialized = false; \
        void ModuleStartup() \
        {{ \
            if (s_bModuleInitialized) return; \
            s_bModuleInitialized = true; \
            /* 用户可在此添加模块初始化代码 */ \
        }} \
        void ModuleShutdown() \
        {{ \
            if (!s_bModuleInitialized) return; \
            s_bModuleInitialized = false; \
            /* 用户可在此添加模块清理代码 */ \
        }} \
        const char* GetModuleVersion() {{ return "0.1.0"; }} \
    }} \
    \
    namespace \
    {{ \
        struct AutoModuleRegister_{short_name} \
        {{ \
            AutoModuleRegister_{short_name}() \
            {{ \
                Limx::Core::ModuleManager::Register( \
                    {name_upper}_MODULE_NAME, \
                    &{namespace}::ModuleStartup, \
                    &{namespace}::ModuleShutdown \
                ); \
            }} \
        }}; \
        static AutoModuleRegister_{short_name} s_AutoModuleRegister; \
    }}

"#,
        is_static = if module.module_type == ModuleType::Static {
            1
        } else {
            0
        },
        is_shared = if module.module_type == ModuleType::Shared {
            1
        } else {
            0
        },
        layer = module.layer,
    )
}

/// 为所有模块生成 API 头文件
pub fn generate_all_api_headers(modules: &[Module], output_dir: &Path) -> Result<()> {
    for module in modules {
        // 只为库模块生成 API 头文件
        match module.module_type {
            ModuleType::Static | ModuleType::Shared => {
                generate_module_api_header(module, output_dir)?;
            }
            _ => {}
        }
    }

    // 生成主包含文件
    generate_master_api_header(modules, output_dir)?;

    Ok(())
}

/// 生成主 API 包含头文件
fn generate_master_api_header(modules: &[Module], output_dir: &Path) -> Result<()> {
    let mut content = String::from(
        r#"/*******************************************************************************
 * 文件: LimxAPI.generated.h
 * 
 * 警告: 此文件由 LBT (Limx Build Tool) 自动生成
 *
 * 功能描述:
 *   所有模块 API 头文件的统一包含
 *
 ******************************************************************************/

#pragma once

"#,
    );

    for module in modules {
        let short_name = if module.name.starts_with("Limx") {
            &module.name[4..]
        } else {
            &module.name
        };

        content.push_str(&format!("#include \"{short_name}API.generated.h\"\n"));
    }

    let output_path = output_dir.join("LimxAPI.generated.h");
    fs::write(&output_path, content)?;
    Ok(())
}
