/*******************************************************************************
 * 文件: discovery.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   模块发现功能
 *   - 扫描目录查找 *.limx.toml 文件
 *   - 解析模块配置
 *   - 创建新模块模板
 *
 ******************************************************************************/

use anyhow::{Context, Result};
use std::fs;
use std::path::Path;
use tracing::{debug, warn};
use walkdir::WalkDir;

use super::config::{Module, ModuleConfig};
use super::error::LbtError;

/// 扫描目录发现所有模块
pub fn discover_modules(source_dir: &Path) -> Result<Vec<Module>> {
    let mut modules = Vec::new();

    if !source_dir.exists() {
        return Err(anyhow::anyhow!("源目录不存在: {}", source_dir.display()));
    }

    for entry in WalkDir::new(source_dir)
        .max_depth(3)
        .into_iter()
        .filter_map(|e| e.ok())
    {
        let path = entry.path();

        if path.is_file() {
            if let Some(name) = path.file_name().and_then(|n| n.to_str()) {
                if name.ends_with(".limx.toml") {
                    match parse_module_config(path) {
                        Ok(module) => {
                            debug!("发现模块: {} @ {}", module.name, path.display());
                            modules.push(module);
                        }
                        Err(e) => {
                            warn!("解析模块配置失败: {} - {}", path.display(), e);
                        }
                    }
                }
            }
        }
    }

    // 按层级排序
    modules.sort_by(|a, b| a.layer.cmp(&b.layer));

    Ok(modules)
}

/// 解析单个模块配置文件
fn parse_module_config(config_path: &Path) -> Result<Module> {
    let content = fs::read_to_string(config_path)
        .with_context(|| format!("无法读取配置文件: {}", config_path.display()))?;

    let config: ModuleConfig =
        toml::from_str(&content).map_err(|e| LbtError::ConfigParseError {
            path: config_path.to_path_buf(),
            source: e,
        })?;

    let module_dir = config_path
        .parent()
        .ok_or_else(|| anyhow::anyhow!("无法获取模块目录"))?;

    Ok(Module {
        name: config.module.name.clone(),
        module_type: config.module.r#type.clone(),
        namespace: config.module.namespace.clone(),
        layer: config.module.layer,
        path: module_dir.to_path_buf(),
        config_path: config_path.to_path_buf(),
        config,
    })
}

/// 创建新模块
pub fn create_module(source_dir: &Path, name: &str, layer: u8) -> Result<()> {
    // 验证模块名
    if name.is_empty() || !name.chars().all(|c| c.is_alphanumeric() || c == '_') {
        return Err(LbtError::InvalidModuleName(name.to_string()).into());
    }

    let module_dir = source_dir.join(name);

    if module_dir.exists() {
        return Err(anyhow::anyhow!("模块目录已存在: {}", module_dir.display()));
    }

    // 创建目录结构
    fs::create_dir_all(module_dir.join("Public").join("Limx").join(name))?;
    fs::create_dir_all(module_dir.join("Private"))?;

    // 生成配置文件
    let config_content = generate_module_config(name, layer);
    fs::write(
        module_dir.join(format!("{}.limx.toml", name)),
        config_content,
    )?;

    // 生成占位头文件
    let header_content = generate_module_header(name);
    fs::write(
        module_dir
            .join("Public")
            .join("Limx")
            .join(name)
            .join(format!("{}API.h", name)),
        header_content,
    )?;

    Ok(())
}

/// 生成模块配置模板
fn generate_module_config(name: &str, layer: u8) -> String {
    format!(
        r#"# {name} 模块配置
# 由 LBT 自动生成

[module]
name = "Limx{name}"
type = "static"
namespace = "Limx::{name}"
layer = {layer}
description = "{name} 模块"

[dependencies]
public = []
private = []

[sources]
include_dirs = ["Public"]
private_dirs = ["Private"]
exclude = ["Tests/*"]

[compile]
defines = ["LIMX_{name_upper}_API"]
features = []

[reflection]
enabled = true
macros = ["LCLASS", "LSTRUCT", "LPROPERTY", "LFUNCTION"]
"#,
        name = name,
        layer = layer,
        name_upper = name.to_uppercase()
    )
}

/// 收集模块的源文件
pub fn collect_module_sources(module_path: &Path) -> Vec<std::path::PathBuf> {
    let mut sources = Vec::new();

    for subdir in &["Private", "Public"] {
        let dir = module_path.join(subdir);
        if dir.exists() {
            for entry in WalkDir::new(&dir).into_iter().filter_map(|e| e.ok()) {
                let path = entry.path();
                if path.is_file() {
                    if let Some(ext) = path.extension().and_then(|e| e.to_str()) {
                        if ext == "cpp" || ext == "c" || ext == "cc" || ext == "cxx" {
                            sources.push(path.to_path_buf());
                        }
                    }
                }
            }
        }
    }

    sources
}

/// 收集模块的头文件
pub fn collect_module_headers(module_path: &Path) -> Vec<std::path::PathBuf> {
    let mut headers = Vec::new();

    for subdir in &["Private", "Public"] {
        let dir = module_path.join(subdir);
        if dir.exists() {
            for entry in WalkDir::new(&dir).into_iter().filter_map(|e| e.ok()) {
                let path = entry.path();
                if path.is_file() {
                    if let Some(ext) = path.extension().and_then(|e| e.to_str()) {
                        if ext == "h" || ext == "hpp" || ext == "hxx" {
                            headers.push(path.to_path_buf());
                        }
                    }
                }
            }
        }
    }

    headers
}

/// 生成模块头文件模板
fn generate_module_header(name: &str) -> String {
    format!(
        r#"/*******************************************************************************
 * 文件: {name}API.h
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   {name} 模块公共 API 定义
 *
 ******************************************************************************/

#pragma once

// 模块导出宏
#ifdef LIMX_{name_upper}_EXPORTS
    #define LIMX_{name_upper}_API __declspec(dllexport)
#else
    #define LIMX_{name_upper}_API __declspec(dllimport)
#endif

namespace Limx::{name}
{{
    // TODO: 添加模块公共接口
}}
"#,
        name = name,
        name_upper = name.to_uppercase()
    )
}
