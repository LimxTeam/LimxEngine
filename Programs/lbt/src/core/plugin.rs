// ============================================================
// 文件名称：plugin.rs
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：模块化插件声明，零运行时开销，编译期插件感知
// 功能描述：Plugin 系统 — 定义引擎插件的结构、依赖与启用规则
//           支持 .limx.plugin.toml 格式，超越 UE 的 .uplugin
// 技术特性：插件模块管理，跨插件依赖解析，版本约束，
//           条件加载规则，热加载标记
//
// ── 类型/函数表 ──────────────────────────────────────────────
// │ PluginConfig              │ .limx.plugin.toml 完整结构         │
// │ PluginModuleEntry         │ 插件内的模块条目                    │
// │ PluginDependency          │ 插件间依赖声明                      │
// │ PluginLoadingPhase        │ 插件加载阶段                        │
// │ PluginRegistry            │ 全局插件注册表                      │
// │ ResolvedPlugin            │ 已解析的插件实例                    │
// │ discover_plugins()        │ 扫描目录发现所有 .limx.plugin.toml  │
// │ load_plugin()             │ 加载并验证单个 Plugin 文件           │
// │ resolve_plugin_deps()     │ 解析插件依赖顺序                    │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                               │
// │ 2026-04-06   │ LimxTeam  │ 初始创建 — 完整 Plugin 系统          │
// ============================================================

use anyhow::{anyhow, Context, Result};
use serde::{Deserialize, Serialize};
use std::collections::{HashMap, HashSet, VecDeque};
use std::fs;
use std::path::{Path, PathBuf};
use tracing::{debug, warn};
use walkdir::WalkDir;

// ──────────────────────────────────────────────────────────────
// 插件加载阶段
// ──────────────────────────────────────────────────────────────

/// 插件加载阶段 (控制模块的加载时机)
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub enum PluginLoadingPhase {
    /// 最早阶段 — 引擎核心初始化前
    EarliestPossible,
    /// 默认阶段 — 引擎启动时
    Default,
    /// 后期阶段 — 游戏系统初始化后
    PostDefault,
    /// 编辑器专属 — 仅编辑器目标加载
    EditorAndProgram,
    /// 开发工具 — 调试/开发配置下
    Developer,
    /// 测试专属
    Testing,
}

impl Default for PluginLoadingPhase {
    fn default() -> Self {
        Self::Default
    }
}

impl PluginLoadingPhase {
    pub fn name(&self) -> &'static str {
        match self {
            Self::EarliestPossible => "EarliestPossible",
            Self::Default => "Default",
            Self::PostDefault => "PostDefault",
            Self::EditorAndProgram => "EditorAndProgram",
            Self::Developer => "Developer",
            Self::Testing => "Testing",
        }
    }

    /// 是否为编辑器专属阶段
    pub fn is_editor_only(&self) -> bool {
        matches!(self, Self::EditorAndProgram | Self::Developer)
    }

    /// 是否在发布版本中加载
    pub fn loads_in_shipping(&self) -> bool {
        matches!(
            self,
            Self::EarliestPossible | Self::Default | Self::PostDefault
        )
    }
}

// ──────────────────────────────────────────────────────────────
// 插件内模块条目
// ──────────────────────────────────────────────────────────────

/// 插件内的模块条目
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PluginModuleEntry {
    /// 模块名称 (对应 *.limx.toml 中的 name)
    pub name: String,

    /// 加载阶段
    #[serde(default)]
    pub loading_phase: PluginLoadingPhase,

    /// 仅在编辑器目标中包含
    #[serde(default)]
    pub editor_only: bool,

    /// 仅在指定平台包含 (空 = 全平台)
    #[serde(default)]
    pub platforms: Vec<String>,

    /// 模块类型: Runtime/Editor/Developer/ThirdParty
    #[serde(default = "default_module_type_str")]
    pub module_type: String,
}

fn default_module_type_str() -> String {
    "Runtime".to_string()
}

// ──────────────────────────────────────────────────────────────
// 插件依赖声明
// ──────────────────────────────────────────────────────────────

/// 插件间的依赖声明
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PluginDependency {
    /// 依赖的插件名称
    pub name: String,

    /// 是否为可选依赖
    #[serde(default)]
    pub optional: bool,

    /// 是否为编辑器专属依赖
    #[serde(default)]
    pub editor_only: bool,

    /// 要求的最低版本 (">=1.0.0" 格式)
    #[serde(default)]
    pub version_constraint: Option<String>,
}

// ──────────────────────────────────────────────────────────────
// 插件元信息
// ──────────────────────────────────────────────────────────────

/// 插件元信息
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PluginInfo {
    /// 插件唯一名称
    pub name: String,

    /// 版本 (SemVer 格式)
    #[serde(default = "default_version")]
    pub version: String,

    /// 描述
    #[serde(default)]
    pub description: Option<String>,

    /// 作者
    #[serde(default)]
    pub author: Option<String>,

    /// 分类
    #[serde(default)]
    pub category: Option<String>,

    /// 是否为引擎内置插件
    #[serde(default)]
    pub engine_plugin: bool,

    /// 是否可热加载 (仅 Editor 目标生效)
    #[serde(default)]
    pub can_hot_reload: bool,

    /// 是否默认启用
    #[serde(default)]
    pub enabled_by_default: bool,

    /// 插件图标路径
    #[serde(default)]
    pub icon: Option<String>,

    /// 文档 URL
    #[serde(default)]
    pub docs_url: Option<String>,
}

fn default_version() -> String {
    "0.1.0".to_string()
}

// ──────────────────────────────────────────────────────────────
// 完整 .limx.plugin.toml 结构
// ──────────────────────────────────────────────────────────────

/// .limx.plugin.toml 完整配置结构
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PluginConfig {
    /// 插件元信息
    pub plugin: PluginInfo,

    /// 插件包含的模块列表
    #[serde(default)]
    pub modules: Vec<PluginModuleEntry>,

    /// 插件依赖的其他插件
    #[serde(default)]
    pub dependencies: Vec<PluginDependency>,

    /// 第三方库声明
    #[serde(default)]
    pub third_party: Vec<ThirdPartyLibrary>,
}

/// 第三方库声明
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ThirdPartyLibrary {
    /// 库名称
    pub name: String,
    /// 版本
    #[serde(default)]
    pub version: Option<String>,
    /// 包含路径 (相对于插件根目录)
    #[serde(default)]
    pub include_dirs: Vec<PathBuf>,
    /// 库文件路径 (相对于插件根目录)
    #[serde(default)]
    pub lib_dirs: Vec<PathBuf>,
    /// 链接库名称
    #[serde(default)]
    pub libraries: Vec<String>,
    /// 宏定义
    #[serde(default)]
    pub defines: Vec<String>,
    /// 平台限制
    #[serde(default)]
    pub platforms: Vec<String>,
}

// ──────────────────────────────────────────────────────────────
// 已解析的插件实例
// ──────────────────────────────────────────────────────────────

/// 已解析的插件实例 (含运行时路径等信息)
#[derive(Debug, Clone)]
pub struct ResolvedPlugin {
    /// 插件名称
    pub name: String,
    /// 插件根目录
    pub root_path: PathBuf,
    /// 配置文件路径
    pub config_path: PathBuf,
    /// 完整配置
    pub config: PluginConfig,
    /// 包含的模块名称列表
    pub module_names: Vec<String>,
    /// 依赖的插件名称 (拓扑排序后)
    pub dependency_names: Vec<String>,
}

impl ResolvedPlugin {
    /// 获取运行时模块列表 (非编辑器专属)
    pub fn runtime_modules(&self) -> Vec<&PluginModuleEntry> {
        self.config
            .modules
            .iter()
            .filter(|m| !m.editor_only && m.loading_phase.loads_in_shipping())
            .collect()
    }

    /// 获取编辑器模块列表
    pub fn editor_modules(&self) -> Vec<&PluginModuleEntry> {
        self.config
            .modules
            .iter()
            .filter(|m| m.editor_only || m.loading_phase.is_editor_only())
            .collect()
    }

    /// 检查插件是否支持指定平台
    pub fn supports_platform(&self, platform: &str) -> bool {
        self.config
            .modules
            .iter()
            .filter(|m| !m.platforms.is_empty())
            .any(|m| m.platforms.iter().any(|p| p.eq_ignore_ascii_case(platform)))
            || self.config.modules.iter().any(|m| m.platforms.is_empty())
    }
}

// ──────────────────────────────────────────────────────────────
// 插件注册表
// ──────────────────────────────────────────────────────────────

/// 全局插件注册表
#[derive(Debug, Default)]
pub struct PluginRegistry {
    /// 所有已发现的插件 (name -> ResolvedPlugin)
    pub plugins: HashMap<String, ResolvedPlugin>,
}

impl PluginRegistry {
    pub fn new() -> Self {
        Self {
            plugins: HashMap::new(),
        }
    }

    /// 注册插件
    pub fn register(&mut self, plugin: ResolvedPlugin) {
        self.plugins.insert(plugin.name.clone(), plugin);
    }

    /// 按名称查找插件
    pub fn find(&self, name: &str) -> Option<&ResolvedPlugin> {
        self.plugins.get(name)
    }

    /// 获取引擎内置插件
    pub fn engine_plugins(&self) -> Vec<&ResolvedPlugin> {
        self.plugins
            .values()
            .filter(|p| p.config.plugin.engine_plugin)
            .collect()
    }

    /// 获取用户插件
    pub fn user_plugins(&self) -> Vec<&ResolvedPlugin> {
        self.plugins
            .values()
            .filter(|p| !p.config.plugin.engine_plugin)
            .collect()
    }

    /// 获取默认启用的插件
    pub fn default_enabled_plugins(&self) -> Vec<&ResolvedPlugin> {
        self.plugins
            .values()
            .filter(|p| p.config.plugin.enabled_by_default)
            .collect()
    }

    /// 打印所有插件摘要
    pub fn print_summary(&self) {
        println!("\n已发现的插件 ({}):", self.plugins.len());
        let mut sorted: Vec<_> = self.plugins.values().collect();
        sorted.sort_by(|a, b| a.name.cmp(&b.name));
        for plugin in sorted {
            let tags = [
                if plugin.config.plugin.engine_plugin {
                    "[引擎]"
                } else {
                    "[用户]"
                },
                if plugin.config.plugin.can_hot_reload {
                    "[热加载]"
                } else {
                    ""
                },
                if plugin.config.plugin.enabled_by_default {
                    "[默认启用]"
                } else {
                    ""
                },
            ]
            .concat();
            println!(
                "  {} v{} {} — 模块数: {}",
                plugin.name,
                plugin.config.plugin.version,
                tags,
                plugin.module_names.len(),
            );
        }
    }
}

// ──────────────────────────────────────────────────────────────
// 发现与加载函数
// ──────────────────────────────────────────────────────────────

/// 扫描目录发现所有 .limx.plugin.toml 文件
pub fn discover_plugins(root_dir: &Path) -> Result<PluginRegistry> {
    let mut registry = PluginRegistry::new();

    if !root_dir.exists() {
        return Ok(registry);
    }

    let scan_dirs: Vec<PathBuf> = {
        let mut dirs = vec![root_dir.join("Plugins")];
        // 也扫描 Source 目录内的内置插件
        dirs.push(root_dir.join("Source").join("Plugins"));
        dirs
    };

    for scan_dir in &scan_dirs {
        if !scan_dir.exists() {
            continue;
        }
        for entry in WalkDir::new(scan_dir)
            .max_depth(4)
            .into_iter()
            .filter_map(|e| e.ok())
        {
            let path = entry.path();
            if path.is_file() {
                if let Some(name) = path.file_name().and_then(|n| n.to_str()) {
                    if name.ends_with(".limx.plugin.toml") {
                        match load_plugin(path) {
                            Ok(plugin) => {
                                debug!(
                                    "发现 Plugin: {} v{} @ {}",
                                    plugin.name,
                                    plugin.config.plugin.version,
                                    path.display()
                                );
                                registry.register(plugin);
                            }
                            Err(e) => {
                                warn!("加载 Plugin 配置失败: {} — {}", path.display(), e);
                            }
                        }
                    }
                }
            }
        }
    }

    Ok(registry)
}

/// 加载并解析单个 Plugin 配置文件
pub fn load_plugin(config_path: &Path) -> Result<ResolvedPlugin> {
    let content = fs::read_to_string(config_path)
        .with_context(|| format!("无法读取 Plugin 配置: {}", config_path.display()))?;

    let config: PluginConfig = toml::from_str(&content)
        .with_context(|| format!("解析 Plugin 配置失败: {}", config_path.display()))?;

    validate_plugin_config(&config, config_path)?;

    let root_path = config_path
        .parent()
        .ok_or_else(|| anyhow!("无法获取 Plugin 根目录: {}", config_path.display()))?
        .to_path_buf();

    let module_names: Vec<String> = config.modules.iter().map(|m| m.name.clone()).collect();

    let dependency_names: Vec<String> =
        config.dependencies.iter().map(|d| d.name.clone()).collect();

    Ok(ResolvedPlugin {
        name: config.plugin.name.clone(),
        root_path,
        config_path: config_path.to_path_buf(),
        config,
        module_names,
        dependency_names,
    })
}

/// 验证 Plugin 配置
fn validate_plugin_config(config: &PluginConfig, path: &Path) -> Result<()> {
    if config.plugin.name.is_empty() {
        return Err(anyhow!("Plugin 名称不能为空: {}", path.display()));
    }

    if config.plugin.name.contains(|c: char| c.is_whitespace()) {
        return Err(anyhow!(
            "Plugin 名称 '{}' 不能包含空格: {}",
            config.plugin.name,
            path.display()
        ));
    }

    // 检查是否有重名模块
    let mut seen_modules: HashSet<&str> = HashSet::new();
    for module in &config.modules {
        if !seen_modules.insert(module.name.as_str()) {
            return Err(anyhow!(
                "Plugin '{}' 中存在重名模块 '{}': {}",
                config.plugin.name,
                module.name,
                path.display()
            ));
        }
    }

    Ok(())
}

/// 解析插件依赖顺序 (拓扑排序)
/// 返回按加载顺序排列的插件名称列表
pub fn resolve_plugin_deps<'a>(
    plugins: &'a [&'a ResolvedPlugin],
) -> Result<Vec<&'a ResolvedPlugin>> {
    let plugin_map: HashMap<&str, &ResolvedPlugin> =
        plugins.iter().map(|p| (p.name.as_str(), *p)).collect();

    // Kahn 算法拓扑排序
    // 边方向: dep → plugin (依赖必须先于被依赖者加载)
    let mut in_degree: HashMap<&str, usize> =
        plugins.iter().map(|p| (p.name.as_str(), 0)).collect();

    // 构建反向邻接表: dep_name → 所有依赖它的插件名
    let mut dependents: HashMap<&str, Vec<&str>> = HashMap::new();

    for plugin in plugins {
        for dep_name in &plugin.dependency_names {
            if plugin_map.contains_key(dep_name.as_str()) {
                // plugin 依赖 dep_name，递增 plugin 的入度
                if let Some(deg) = in_degree.get_mut(plugin.name.as_str()) {
                    *deg += 1;
                }
                dependents
                    .entry(dep_name.as_str())
                    .or_default()
                    .push(plugin.name.as_str());
            }
        }
    }

    let mut queue: VecDeque<&str> = in_degree
        .iter()
        .filter(|(_, &count)| count == 0)
        .map(|(&name, _)| name)
        .collect();

    let mut sorted: Vec<&ResolvedPlugin> = Vec::new();

    while let Some(name) = queue.pop_front() {
        if let Some(&plugin) = plugin_map.get(name) {
            sorted.push(plugin);
            // 递减所有依赖当前节点的插件的入度
            if let Some(deps) = dependents.get(name) {
                for &dependent_name in deps {
                    if let Some(count) = in_degree.get_mut(dependent_name) {
                        *count -= 1;
                        if *count == 0 {
                            queue.push_back(dependent_name);
                        }
                    }
                }
            }
        }
    }

    if sorted.len() != plugins.len() {
        return Err(anyhow!("检测到插件循环依赖，无法解析加载顺序"));
    }

    Ok(sorted)
}

/// 生成示例 .limx.plugin.toml 内容
pub fn generate_example_plugin(name: &str) -> String {
    format!(
        r#"# {name} Plugin 配置
# 生成器: lbt new-plugin --name {name}

[plugin]
name = "{name}"
version = "0.1.0"
description = "{name} 引擎插件"
author = "LimxTeam"
category = "Rendering"
engine_plugin = false
can_hot_reload = true
enabled_by_default = false
docs_url = "https://docs.limxengine.com/plugins/{name_lower}"

[[modules]]
name = "{name}Runtime"
loading_phase = "Default"
editor_only = false
module_type = "Runtime"
platforms = ["Windows"]

[[modules]]
name = "{name}Editor"
loading_phase = "EditorAndProgram"
editor_only = true
module_type = "Editor"

[[dependencies]]
name = "VulkanRHI"
optional = false
editor_only = false

# [[third_party]]
# name = "MyLib"
# version = "1.2.3"
# include_dirs = ["ThirdParty/MyLib/include"]
# lib_dirs = ["ThirdParty/MyLib/lib/Win64"]
# libraries = ["MyLib.lib"]
"#,
        name = name,
        name_lower = name.to_lowercase()
    )
}

// ──────────────────────────────────────────────────────────────
// 单元测试
// ──────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    fn make_test_plugin(name: &str, deps: Vec<&str>) -> ResolvedPlugin {
        ResolvedPlugin {
            name: name.to_string(),
            root_path: PathBuf::from(name),
            config_path: PathBuf::from(format!("{}.limx.plugin.toml", name)),
            module_names: Vec::new(),
            dependency_names: deps.into_iter().map(|s| s.to_string()).collect(),
            config: PluginConfig {
                plugin: PluginInfo {
                    name: name.to_string(),
                    version: "0.1.0".to_string(),
                    description: None,
                    author: None,
                    category: None,
                    engine_plugin: false,
                    can_hot_reload: false,
                    enabled_by_default: false,
                    icon: None,
                    docs_url: None,
                },
                modules: Vec::new(),
                dependencies: Vec::new(),
                third_party: Vec::new(),
            },
        }
    }

    #[test]
    fn test_resolve_plugin_deps_simple() {
        let a = make_test_plugin("A", vec!["B"]);
        let b = make_test_plugin("B", vec![]);
        let plugins = vec![&a, &b];
        let sorted = resolve_plugin_deps(&plugins).unwrap();
        // B 必须在 A 之前
        let b_pos = sorted.iter().position(|p| p.name == "B").unwrap();
        let a_pos = sorted.iter().position(|p| p.name == "A").unwrap();
        assert!(b_pos < a_pos);
    }

    #[test]
    fn test_plugin_loading_phase_editor_only() {
        assert!(PluginLoadingPhase::EditorAndProgram.is_editor_only());
        assert!(PluginLoadingPhase::Developer.is_editor_only());
        assert!(!PluginLoadingPhase::Default.is_editor_only());
    }

    #[test]
    fn test_generate_example_plugin() {
        let example = generate_example_plugin("VulkanExtensions");
        assert!(example.contains("VulkanExtensions"));
        assert!(example.contains("engine_plugin = false"));
    }
}
