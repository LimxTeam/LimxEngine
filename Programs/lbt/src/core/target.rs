// ============================================================
// 文件名称：target.rs
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：显式目标声明，零歧义构建意图，编译期平台分离
// 功能描述：Target 系统 — 定义构建目标类型、平台规则与条件编译
//           支持 .limx.target.toml 格式，超越 UE 的 TargetRules.cs
// 技术特性：Target 类型分离 (Game/Editor/Server/Client/Program)
//           条件编译规则，平台特定覆盖，依赖白名单控制
//
// ── 类型/函数表 ──────────────────────────────────────────────
// │ TargetConfig              │ .limx.target.toml 完整结构         │
// │ TargetType                │ 目标类型枚举                        │
// │ TargetRules               │ 构建规则 (定义/包含/库/平台覆盖)     │
// │ TargetPlatformOverride    │ 平台特定覆盖规则                    │
// │ PluginRule                │ Plugin 启用/禁用规则                │
// │ TargetRegistry            │ 全局 Target 注册表                  │
// │ discover_targets()        │ 扫描目录发现所有 .limx.target.toml  │
// │ load_target()             │ 加载并验证单个 Target 文件           │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                               │
// │ 2026-04-06   │ LimxTeam  │ 初始创建 — 完整 Target 系统          │
// ============================================================

use anyhow::{anyhow, Context, Result};
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};
use tracing::{debug, warn};
use walkdir::WalkDir;

// ──────────────────────────────────────────────────────────────
// Target 类型
// ──────────────────────────────────────────────────────────────

/// 构建目标类型
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum TargetType {
    /// 游戏运行时可执行文件
    Game,
    /// 编辑器可执行文件 (含编辑器专属模块)
    Editor,
    /// 无头服务器 (无渲染)
    Server,
    /// 轻量客户端 (无物理/AI)
    Client,
    /// 独立程序 (工具/测试)
    Program,
    /// 动态库 (引擎插件)
    Plugin,
}

impl TargetType {
    /// 获取类型名称
    pub fn name(&self) -> &'static str {
        match self {
            Self::Game => "Game",
            Self::Editor => "Editor",
            Self::Server => "Server",
            Self::Client => "Client",
            Self::Program => "Program",
            Self::Plugin => "Plugin",
        }
    }

    /// 是否包含编辑器功能
    pub fn includes_editor(&self) -> bool {
        matches!(self, Self::Editor)
    }

    /// 是否为服务器端
    pub fn is_server(&self) -> bool {
        matches!(self, Self::Server)
    }

    /// 是否为可执行文件
    pub fn is_executable(&self) -> bool {
        matches!(
            self,
            Self::Game | Self::Editor | Self::Server | Self::Client | Self::Program
        )
    }

    /// 获取默认宏定义列表
    pub fn default_defines(&self) -> Vec<String> {
        let mut defines = vec![
            "LIMX_ENGINE=1".to_string(),
            format!("LIMX_TARGET_{}=1", self.name().to_uppercase()),
        ];
        if self.includes_editor() {
            defines.push("WITH_EDITOR=1".to_string());
        }
        if self.is_server() {
            defines.push("WITH_SERVER_CODE=1".to_string());
            defines.push("WITHOUT_CLIENT_CODE=1".to_string());
        }
        defines
    }
}

impl Default for TargetType {
    fn default() -> Self {
        Self::Game
    }
}

impl std::fmt::Display for TargetType {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}", self.name())
    }
}

// ──────────────────────────────────────────────────────────────
// 构建配置类型
// ──────────────────────────────────────────────────────────────

/// 构建配置
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum TargetBuildConfig {
    Debug,
    Development,
    Release,
    Shipping,
    Test,
}

impl Default for TargetBuildConfig {
    fn default() -> Self {
        Self::Development
    }
}

impl std::fmt::Display for TargetBuildConfig {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let s = match self {
            Self::Debug => "Debug",
            Self::Development => "Development",
            Self::Release => "Release",
            Self::Shipping => "Shipping",
            Self::Test => "Test",
        };
        write!(f, "{}", s)
    }
}

// ──────────────────────────────────────────────────────────────
// 平台特定覆盖
// ──────────────────────────────────────────────────────────────

/// 平台特定构建规则覆盖
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct TargetPlatformOverride {
    /// 额外宏定义
    #[serde(default)]
    pub defines: Vec<String>,

    /// 额外包含路径
    #[serde(default)]
    pub include_paths: Vec<PathBuf>,

    /// 额外链接库
    #[serde(default)]
    pub libraries: Vec<String>,

    /// 额外编译选项
    #[serde(default)]
    pub compiler_flags: Vec<String>,

    /// 是否禁用此平台
    #[serde(default)]
    pub disabled: bool,
}

// ──────────────────────────────────────────────────────────────
// Plugin 规则
// ──────────────────────────────────────────────────────────────

/// Target 对 Plugin 的引用规则
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PluginRule {
    /// Plugin 名称
    pub name: String,

    /// 是否启用
    #[serde(default = "default_true")]
    pub enabled: bool,

    /// 仅在特定配置下启用
    #[serde(default)]
    pub configs: Vec<TargetBuildConfig>,

    /// 是否为可选依赖
    #[serde(default)]
    pub optional: bool,
}

fn default_true() -> bool {
    true
}

// ──────────────────────────────────────────────────────────────
// 构建规则
// ──────────────────────────────────────────────────────────────

/// 构建规则 — 控制如何编译此 Target
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct TargetRules {
    /// 额外宏定义 (所有平台)
    #[serde(default)]
    pub defines: Vec<String>,

    /// 额外包含路径
    #[serde(default)]
    pub include_paths: Vec<PathBuf>,

    /// 额外链接库
    #[serde(default)]
    pub libraries: Vec<String>,

    /// 额外编译标志
    #[serde(default)]
    pub compiler_flags: Vec<String>,

    /// 启用/禁用 PCH
    #[serde(default = "default_true")]
    pub enable_pch: bool,

    /// 启用/禁用 Unity Build
    #[serde(default = "default_true")]
    pub enable_unity_build: bool,

    /// 启用/禁用 RTTI
    #[serde(default)]
    pub enable_rtti: bool,

    /// 启用/禁用 C++ 异常
    #[serde(default)]
    pub enable_exceptions: bool,

    /// 允许编译的模块白名单 (空 = 允许全部)
    #[serde(default)]
    pub allowed_modules: Vec<String>,

    /// 排除的模块黑名单
    #[serde(default)]
    pub excluded_modules: Vec<String>,

    /// 启用的 Plugin 列表
    #[serde(default)]
    pub plugins: Vec<PluginRule>,

    /// 平台特定覆盖 (key: "Windows"/"Linux"/"MacOS")
    #[serde(default)]
    pub platform_overrides: HashMap<String, TargetPlatformOverride>,

    /// 配置特定覆盖 (key: "Debug"/"Release" 等)
    #[serde(default)]
    pub config_overrides: HashMap<String, TargetRulesOverride>,
}

impl Default for TargetRules {
    fn default() -> Self {
        Self {
            defines: Vec::new(),
            include_paths: Vec::new(),
            libraries: Vec::new(),
            compiler_flags: Vec::new(),
            enable_pch: true,
            enable_unity_build: true,
            enable_rtti: false,
            enable_exceptions: false,
            allowed_modules: Vec::new(),
            excluded_modules: Vec::new(),
            plugins: Vec::new(),
            platform_overrides: HashMap::new(),
            config_overrides: HashMap::new(),
        }
    }
}

/// 配置特定覆盖
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct TargetRulesOverride {
    #[serde(default)]
    pub defines: Vec<String>,
    #[serde(default)]
    pub compiler_flags: Vec<String>,
    #[serde(default)]
    pub libraries: Vec<String>,
}

// ──────────────────────────────────────────────────────────────
// Target 元信息
// ──────────────────────────────────────────────────────────────

/// Target 元信息
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TargetInfo {
    /// Target 名称 (如 "LimxEngineGame")
    pub name: String,

    /// Target 类型
    #[serde(default)]
    pub target_type: TargetType,

    /// 默认构建配置
    #[serde(default)]
    pub default_config: TargetBuildConfig,

    /// 描述
    #[serde(default)]
    pub description: Option<String>,

    /// 默认入口模块 (Executable 类型)
    #[serde(default)]
    pub entry_module: Option<String>,

    /// 支持的平台 (空 = 全平台)
    #[serde(default)]
    pub supported_platforms: Vec<String>,

    /// 版本号
    #[serde(default = "default_version")]
    pub version: String,
}

fn default_version() -> String {
    "0.1.0".to_string()
}

// ──────────────────────────────────────────────────────────────
// Target 配置 (完整 .limx.target.toml 结构)
// ──────────────────────────────────────────────────────────────

/// 完整 Target 配置文件结构
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TargetConfig {
    /// Target 元信息
    pub target: TargetInfo,

    /// 构建规则
    #[serde(default)]
    pub rules: TargetRules,
}

// ──────────────────────────────────────────────────────────────
// 已解析的 Target (含运行时路径信息)
// ──────────────────────────────────────────────────────────────

/// 已解析的构建目标
#[derive(Debug, Clone)]
pub struct Target {
    /// Target 名称
    pub name: String,
    /// Target 类型
    pub target_type: TargetType,
    /// 默认构建配置
    pub default_config: TargetBuildConfig,
    /// 配置文件路径
    pub config_path: PathBuf,
    /// 完整配置
    pub config: TargetConfig,
    /// 合并后的宏定义 (类型默认 + 用户自定义)
    pub resolved_defines: Vec<String>,
}

impl Target {
    /// 检查模块是否被此 Target 允许
    pub fn allows_module(&self, module_name: &str) -> bool {
        let rules = &self.config.rules;

        if rules.excluded_modules.iter().any(|m| m == module_name) {
            return false;
        }

        if rules.allowed_modules.is_empty() {
            return true;
        }

        rules.allowed_modules.iter().any(|m| m == module_name)
    }

    /// 获取指定平台的合并编译规则
    pub fn resolve_for_platform(&self, platform: &str) -> ResolvedTargetRules {
        let base = &self.config.rules;
        let mut defines = self.resolved_defines.clone();
        let mut include_paths = base.include_paths.clone();
        let mut libraries = base.libraries.clone();
        let mut compiler_flags = base.compiler_flags.clone();

        if let Some(override_rules) = base.platform_overrides.get(platform) {
            if !override_rules.disabled {
                defines.extend(override_rules.defines.clone());
                include_paths.extend(override_rules.include_paths.clone());
                libraries.extend(override_rules.libraries.clone());
                compiler_flags.extend(override_rules.compiler_flags.clone());
            }
        }

        ResolvedTargetRules {
            defines,
            include_paths,
            libraries,
            compiler_flags,
            enable_pch: base.enable_pch,
            enable_unity_build: base.enable_unity_build,
            enable_rtti: base.enable_rtti,
            enable_exceptions: base.enable_exceptions,
        }
    }

    /// 获取指定配置的合并构建规则
    pub fn resolve_for_config(
        &self,
        platform: &str,
        config: TargetBuildConfig,
    ) -> ResolvedTargetRules {
        let mut resolved = self.resolve_for_platform(platform);
        let config_key = config.to_string();

        if let Some(config_override) = self.config.rules.config_overrides.get(&config_key) {
            resolved.defines.extend(config_override.defines.clone());
            resolved
                .compiler_flags
                .extend(config_override.compiler_flags.clone());
            resolved.libraries.extend(config_override.libraries.clone());
        }

        resolved
    }
}

/// 解析后的目标构建规则 (平台+配置已合并)
#[derive(Debug, Clone)]
pub struct ResolvedTargetRules {
    pub defines: Vec<String>,
    pub include_paths: Vec<PathBuf>,
    pub libraries: Vec<String>,
    pub compiler_flags: Vec<String>,
    pub enable_pch: bool,
    pub enable_unity_build: bool,
    pub enable_rtti: bool,
    pub enable_exceptions: bool,
}

// ──────────────────────────────────────────────────────────────
// Target 注册表
// ──────────────────────────────────────────────────────────────

/// 已发现的所有 Target 注册表
#[derive(Debug, Default)]
pub struct TargetRegistry {
    /// 所有 Target (name -> Target)
    pub targets: HashMap<String, Target>,
}

impl TargetRegistry {
    /// 创建空注册表
    pub fn new() -> Self {
        Self {
            targets: HashMap::new(),
        }
    }

    /// 注册 Target
    pub fn register(&mut self, target: Target) {
        self.targets.insert(target.name.clone(), target);
    }

    /// 按类型获取所有 Target
    pub fn by_type(&self, target_type: TargetType) -> Vec<&Target> {
        self.targets
            .values()
            .filter(|t| t.target_type == target_type)
            .collect()
    }

    /// 按名称查找 Target
    pub fn find(&self, name: &str) -> Option<&Target> {
        self.targets.get(name)
    }

    /// 获取默认 Target (仅一个 Game 类型时自动选择)
    pub fn default_target(&self) -> Option<&Target> {
        let games: Vec<_> = self.by_type(TargetType::Game);
        if games.len() == 1 {
            return Some(games[0]);
        }
        None
    }

    /// 打印所有 Target 信息
    pub fn print_summary(&self) {
        println!("\n已发现的构建目标 ({}):", self.targets.len());
        let mut sorted: Vec<_> = self.targets.values().collect();
        sorted.sort_by(|a, b| a.name.cmp(&b.name));
        for target in sorted {
            println!(
                "  [{:8}] {} — {}",
                target.target_type.name(),
                target.name,
                target
                    .config
                    .target
                    .description
                    .as_deref()
                    .unwrap_or("无描述")
            );
        }
    }
}

// ──────────────────────────────────────────────────────────────
// 发现与加载函数
// ──────────────────────────────────────────────────────────────

/// 扫描目录发现所有 .limx.target.toml 文件
pub fn discover_targets(root_dir: &Path) -> Result<TargetRegistry> {
    let mut registry = TargetRegistry::new();

    if !root_dir.exists() {
        return Ok(registry);
    }

    for entry in WalkDir::new(root_dir)
        .max_depth(4)
        .into_iter()
        .filter_map(|e| e.ok())
    {
        let path = entry.path();
        if path.is_file() {
            if let Some(name) = path.file_name().and_then(|n| n.to_str()) {
                if name.ends_with(".limx.target.toml") {
                    match load_target(path) {
                        Ok(target) => {
                            debug!(
                                "发现 Target: {} ({}) @ {}",
                                target.name,
                                target.target_type,
                                path.display()
                            );
                            registry.register(target);
                        }
                        Err(e) => {
                            warn!("加载 Target 配置失败: {} — {}", path.display(), e);
                        }
                    }
                }
            }
        }
    }

    Ok(registry)
}

/// 加载并解析单个 Target 配置文件
pub fn load_target(config_path: &Path) -> Result<Target> {
    let content = fs::read_to_string(config_path)
        .with_context(|| format!("无法读取 Target 配置文件: {}", config_path.display()))?;

    let config: TargetConfig = toml::from_str(&content)
        .with_context(|| format!("解析 Target 配置文件失败: {}", config_path.display()))?;

    validate_target_config(&config, config_path)?;

    let mut resolved_defines = config.target.target_type.default_defines();
    resolved_defines.extend(config.rules.defines.clone());

    Ok(Target {
        name: config.target.name.clone(),
        target_type: config.target.target_type,
        default_config: config.target.default_config,
        config_path: config_path.to_path_buf(),
        resolved_defines,
        config,
    })
}

/// 验证 Target 配置合法性
fn validate_target_config(config: &TargetConfig, path: &Path) -> Result<()> {
    if config.target.name.is_empty() {
        return Err(anyhow!("Target 名称不能为空: {}", path.display()));
    }

    if config
        .target
        .name
        .contains(|c: char| !c.is_alphanumeric() && c != '_' && c != '-')
    {
        return Err(anyhow!(
            "Target 名称 '{}' 包含非法字符 (仅允许字母数字、'_' 和 '-'): {}",
            config.target.name,
            path.display()
        ));
    }

    if config.target.target_type == TargetType::Game
        || config.target.target_type == TargetType::Editor
        || config.target.target_type == TargetType::Program
    {
        // 可执行目标无需额外验证
    }

    Ok(())
}

/// 生成示例 .limx.target.toml 内容
pub fn generate_example_target(name: &str, target_type: TargetType) -> String {
    let type_str = match target_type {
        TargetType::Game => "game",
        TargetType::Editor => "editor",
        TargetType::Server => "server",
        TargetType::Client => "client",
        TargetType::Program => "program",
        TargetType::Plugin => "plugin",
    };

    format!(
        r#"# {name} Target 配置
# 生成器: lbt new-target --name {name} --type {type_str}

[target]
name = "{name}"
target_type = "{type_str}"
default_config = "development"
description = "{name} 构建目标"
entry_module = "{name}"
supported_platforms = ["Windows"]
version = "0.1.0"

[rules]
enable_pch = true
enable_unity_build = true
enable_rtti = false
enable_exceptions = false

[rules.defines]
# 自定义宏定义 (类型默认宏会自动添加)

[rules.platform_overrides.Windows]
defines = ["PLATFORM_WINDOWS=1"]
libraries = ["d3d12.lib", "dxgi.lib"]

[rules.config_overrides.Debug]
defines = ["LIMX_DEBUG_CHECKS=1"]
compiler_flags = ["/Od", "/Zi"]

[rules.config_overrides.Release]
defines = ["LIMX_RELEASE=1"]
compiler_flags = ["/O2", "/GL"]
"#,
        name = name,
        type_str = type_str
    )
}

// ──────────────────────────────────────────────────────────────
// 单元测试
// ──────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_target_type_defines() {
        let defines = TargetType::Game.default_defines();
        assert!(defines.iter().any(|d| d.contains("LIMX_TARGET_GAME")));
        assert!(defines.iter().any(|d| d.contains("LIMX_ENGINE")));
    }

    #[test]
    fn test_target_type_editor_includes_editor() {
        assert!(TargetType::Editor.includes_editor());
        assert!(!TargetType::Game.includes_editor());
        assert!(!TargetType::Server.includes_editor());
    }

    #[test]
    fn test_target_allows_module() {
        let config = TargetConfig {
            target: TargetInfo {
                name: "TestGame".to_string(),
                target_type: TargetType::Game,
                default_config: TargetBuildConfig::Development,
                description: None,
                entry_module: None,
                supported_platforms: Vec::new(),
                version: "0.1.0".to_string(),
            },
            rules: TargetRules {
                excluded_modules: vec!["EditorOnly".to_string()],
                ..Default::default()
            },
        };

        let target = Target {
            name: "TestGame".to_string(),
            target_type: TargetType::Game,
            default_config: TargetBuildConfig::Development,
            config_path: PathBuf::from("test"),
            config,
            resolved_defines: Vec::new(),
        };

        assert!(target.allows_module("Core"));
        assert!(!target.allows_module("EditorOnly"));
    }

    #[test]
    fn test_validate_target_config_invalid_name() {
        let config = TargetConfig {
            target: TargetInfo {
                name: "my game!".to_string(),
                target_type: TargetType::Game,
                default_config: TargetBuildConfig::Development,
                description: None,
                entry_module: None,
                supported_platforms: Vec::new(),
                version: "0.1.0".to_string(),
            },
            rules: TargetRules::default(),
        };
        let result = validate_target_config(&config, Path::new("test.toml"));
        assert!(result.is_err());
    }

    #[test]
    fn test_generate_example_target() {
        let example = generate_example_target("LimxEngineGame", TargetType::Game);
        assert!(example.contains("LimxEngineGame"));
        assert!(example.contains("game"));
        assert!(example.contains("d3d12.lib"));
    }
}
