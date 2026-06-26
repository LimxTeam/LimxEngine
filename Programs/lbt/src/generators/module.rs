/*******************************************************************************
 * 文件: module_generator.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   模块代码自动生成器
 *   - 根据 .limx.toml 配置生成模块头文件
 *   - 自动生成命名空间宏、API 导出宏
 *   - 生成模块入口函数
 *
 * 设计哲学:
 *   像 UE 一样，让开发者只需关注业务逻辑
 *   模块框架代码由构建工具自动生成
 *
 ******************************************************************************/

use anyhow::Result;
use std::fs;
use std::path::Path;

use crate::core::config::Module;

/// 为所有模块生成代码
pub fn generate_all_modules(modules: &[Module], output_dir: &Path) -> Result<()> {
    fs::create_dir_all(output_dir)?;

    for module in modules {
        generate_module_header(module, output_dir)?;
    }

    Ok(())
}

/// 生成单个模块的头文件
pub fn generate_module_header(module: &Module, output_dir: &Path) -> Result<()> {
    let module_name = &module.name;
    let short_name = module_name.strip_prefix("Limx").unwrap_or(module_name);

    // 生成命名空间宏
    let namespace = module.namespace.as_deref().unwrap_or("Limx");
    let namespace_parts: Vec<&str> = namespace.split("::").collect();

    // 生成 API 宏名称
    let api_macro = module
        .config
        .module
        .api_macro
        .clone()
        .unwrap_or_else(|| format!("LIMX_{}_API", short_name.to_uppercase()));

    // 导出宏定义
    let exports_macro = format!("LIMX_{}_EXPORTS", short_name.to_uppercase());

    let mut content = String::new();

    // 文件头
    content.push_str(&format!(
        r#"/*******************************************************************************
 * 文件: {short_name}Module.generated.h
 * 
 * 自动生成文件 - 请勿手动编辑
 * 由 lbt generate-module 命令生成
 *
 * 模块: {module_name}
 * 命名空间: {namespace}
 *
 ******************************************************************************/

#pragma once

"#
    ));

    // API 导出宏 (DLL 模式 - dllexport/dllimport)
    content.push_str(&format!(
        r#"// =============================================================================
// API 导出宏
// =============================================================================

#if defined({exports_macro})
    #define {api_macro} __declspec(dllexport)
#else
    #define {api_macro} __declspec(dllimport)
#endif

"#
    ));

    // UE 风格模块类声明 (无 namespace)
    content.push_str(&format!(
        r#"// =============================================================================
// 模块类声明
// =============================================================================

class {api_macro} F{short_name}Module
{{
public:
    /// 模块启动 (引擎初始化时调用)
    void StartupModule();

    /// 模块关闭 (引擎关闭时调用)
    void ShutdownModule();
}};

"#
    ));

    // 便捷宏
    let macro_prefix = short_name.to_uppercase();
    content.push_str(&format!(
        r#"// =============================================================================
// 便捷宏
// =============================================================================

/// 声明一个导出类
#define {macro_prefix}_CLASS class {api_macro}

/// 声明一个导出结构体
#define {macro_prefix}_STRUCT struct {api_macro}

"#
    ));

    // 写入文件
    let header_path = output_dir.join(format!("{}Module.generated.h", short_name));
    fs::write(&header_path, content)?;

    Ok(())
}

/// 生成模块的 PCH 头文件
pub fn generate_pch_header(module: &Module, output_dir: &Path) -> Result<()> {
    let module_name = &module.name;
    let short_name = module_name.strip_prefix("Limx").unwrap_or(module_name);

    if !module.config.precompiled_header.enabled {
        return Ok(());
    }

    let mut content = String::new();

    content.push_str(&format!(
        r#"/*******************************************************************************
 * 文件: {short_name}PCH.generated.h
 * 
 * 自动生成文件 - 请勿手动编辑
 * 预编译头文件
 *
 ******************************************************************************/

#pragma once

// 模块头文件
#include "{short_name}Module.generated.h"

// 平台头文件
#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <Windows.h>
#endif

"#
    ));

    let pch_path = output_dir.join(format!("{}PCH.generated.h", short_name));
    fs::write(&pch_path, content)?;

    Ok(())
}
