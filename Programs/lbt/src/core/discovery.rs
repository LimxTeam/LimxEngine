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
use std::fmt::Write as _;
use std::fs;
use std::path::{Path, PathBuf};
use tracing::debug;
use walkdir::WalkDir;

use super::config::{Module, ModuleConfig};
use super::error::LbtError;

/// 扫描目录发现所有模块
///
/// 只要有一个 *.limx.toml 读不出或解析不了, 整个发现过程就失败。
///
/// 这里原先是 `warn!` 之后继续: 解析不出来的模块被从结果里悄悄拿掉, 返回值
/// 依旧是 Ok。问题不在于少了一个模块, 而在于**少下去的方向恰好是"通过"那一
/// 侧** —— 模块集是所有下游判断的输入, 模块不在集合里, 它的依赖不会被解析、
/// 它的配置不会被校验、它的源文件不会被编译, 于是每一项检查都"没发现问题"。
/// `type = "static_library"` (合法值是 "static") 这样一个词的笔误, 或者任何
/// 一处 TOML 语法错误, 就足以让一个模块从 check / validate / build 三步里
/// 整个消失: 三步全绿, 模块不被重建, 上一次构建留下的陈旧 .lib 照常链进去。
///
/// 为什么把这个判断放在发现阶段, 而不是记录下来交给各命令自己决定:
/// discover_modules 有十余个调用点, "交给调用方"等于要求每个调用点都记得去
/// 查那份记录 —— 漏掉一个, 那个命令就退回到今天这个状态, 而漏掉的默认后果
/// 又是"通过"。也就是说那种设计本身就是这类缺陷的模具。放在这里, 调用方
/// 没有"忘记检查"这个选项: 返回类型是 Result, 现有的 `?` 会把它一路带到
/// main 的非零退出。
pub fn discover_modules(source_dir: &Path) -> Result<Vec<Module>> {
    let mut modules = Vec::new();

    // (配置文件路径, 完整错误链)。收集而不是撞上第一个就返回 —— 一次运行把
    // 所有坏掉的文件都报出来, 否则十个笔误得跑十遍才能改完。
    let mut parse_failures: Vec<(PathBuf, String)> = Vec::new();

    if !source_dir.exists() {
        return Err(anyhow::anyhow!("源目录不存在: {}", source_dir.display()));
    }

    // 目录遍历本身的错误也不能吞。原先是 filter_map(|e| e.ok()): 某个子目录
    // 读不动 (权限、路径过长、符号链接环) 时, 它下面的模块就整片不见, 而
    // 结果依然是 Ok —— 和上面解析失败是同一个形状的漏洞, 只是发生得更早。
    let mut walk_failures: Vec<String> = Vec::new();

    for entry in WalkDir::new(source_dir).max_depth(3) {
        let entry = match entry {
            Ok(e) => e,
            Err(e) => {
                walk_failures.push(format!("{}", e));
                continue;
            }
        };
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
                            // 顺着 anyhow 的错误链往下取。最外层那句只是"解析
                            // 失败 + 路径", 路径下面会单独打一行, 真正有信息量
                            // 的是 toml/serde 给出的行列位置与期望值 —— 它挂在
                            // LbtError::ConfigParseError 的 #[source] 上。原先
                            // 用 `{}` 格式化, 只取最外层, 于是那条 WARN 就算有
                            // 人看见也无从下手: 它不说哪一行、哪个字段、该写
                            // 什么。
                            let chain: Vec<String> =
                                e.chain().map(|c| c.to_string()).collect();
                            let reason = if chain.len() > 1 {
                                chain[1..].join(": ")
                            } else {
                                chain.join(": ")
                            };
                            parse_failures.push((path.to_path_buf(), reason));
                        }
                    }
                }
            }
        }
    }

    if !parse_failures.is_empty() || !walk_failures.is_empty() {
        return Err(discovery_failure(source_dir, &parse_failures, &walk_failures));
    }

    // 按层级排序
    modules.sort_by(|a, b| a.layer.cmp(&b.layer));

    Ok(modules)
}

/// 把发现阶段的全部失败汇总成一条错误
///
/// 单独拆出来是为了让消息里能说清后果: 读消息的人需要知道的不是"有个文件
/// 解析失败了", 而是"在你修好它之前, 任何绿色结论都不成立"。
fn discovery_failure(
    source_dir: &Path,
    parse_failures: &[(PathBuf, String)],
    walk_failures: &[String],
) -> anyhow::Error {
    let mut detail = String::new();

    for (path, reason) in parse_failures {
        let _ = write!(detail, "\n  ✗ {}", path.display());
        for line in reason.lines() {
            let _ = write!(detail, "\n      {}", line);
        }
    }

    for reason in walk_failures {
        let _ = write!(detail, "\n  ✗ 目录遍历失败: {}", reason);
    }

    anyhow::anyhow!(
        "模块发现失败: {} 个配置解析失败, {} 处目录遍历失败 (源目录: {}){}\n\n\
         这些模块不在模块集里, 因此不会参与依赖解析、配置校验和构建。\n\
         它们的状态不是\"没问题\", 而是\"没被看见\" —— 在修好之前, \
         check / validate / build 报出来的任何通过都不成立。",
        parse_failures.len(),
        walk_failures.len(),
        source_dir.display(),
        detail
    )
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
