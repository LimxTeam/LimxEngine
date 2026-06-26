/*******************************************************************************
 * 文件: config.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   模块配置文件 (*.limx.toml) 的数据结构定义
 *
 ******************************************************************************/

use serde::{Deserialize, Serialize};
use std::path::PathBuf;

/// 模块配置文件结构
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ModuleConfig {
    pub module: ModuleInfo,

    #[serde(default)]
    pub dependencies: Dependencies,

    #[serde(default)]
    pub sources: Sources,

    #[serde(default)]
    pub compile: CompileOptions,

    #[serde(default)]
    pub precompiled_header: PrecompiledHeader,

    #[serde(default)]
    pub reflection: ReflectionOptions,
}

/// 模块基本信息
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ModuleInfo {
    /// 模块名称
    pub name: String,

    /// 模块类型: static, shared, executable
    #[serde(default = "default_module_type")]
    pub r#type: ModuleType,

    /// 命名空间 (如 "Limx::Core")
    #[serde(default)]
    pub namespace: Option<String>,

    /// API 导出宏名称 (如 "LIMX_CORE_API")
    #[serde(default)]
    pub api_macro: Option<String>,

    /// 架构层级 (0-5)
    #[serde(default)]
    pub layer: u8,

    /// 模块描述
    #[serde(default)]
    pub description: Option<String>,
}

/// 模块类型
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq, Default)]
#[serde(rename_all = "lowercase")]
pub enum ModuleType {
    #[default]
    Static,
    Shared,
    Executable,
    HeaderOnly,
    External,
}

fn default_module_type() -> ModuleType {
    ModuleType::Static
}

/// 依赖配置
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct Dependencies {
    /// 公开依赖 (传递给使用者)
    #[serde(default)]
    pub public: Vec<String>,

    /// 私有依赖 (不传递)
    #[serde(default)]
    pub private: Vec<String>,
}

/// 源文件配置
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Sources {
    /// 公开头文件目录
    #[serde(default = "default_include_dirs")]
    pub include_dirs: Vec<String>,

    /// 私有源文件目录
    #[serde(default = "default_private_dirs")]
    pub private_dirs: Vec<String>,

    /// 排除模式
    #[serde(default)]
    pub exclude: Vec<String>,
}

impl Default for Sources {
    fn default() -> Self {
        Self {
            include_dirs: default_include_dirs(),
            private_dirs: default_private_dirs(),
            exclude: Vec::new(),
        }
    }
}

fn default_include_dirs() -> Vec<String> {
    vec!["Public".to_string()]
}

fn default_private_dirs() -> Vec<String> {
    vec!["Private".to_string()]
}

/// 编译选项
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct CompileOptions {
    /// 预处理器定义
    #[serde(default)]
    pub defines: Vec<String>,

    /// 可选特性
    #[serde(default)]
    pub features: Vec<String>,

    /// 额外的编译标志
    #[serde(default)]
    pub flags: Vec<String>,

    /// 额外的头文件搜索路径
    #[serde(default)]
    pub include_paths: Vec<String>,

    /// 库搜索路径
    #[serde(default)]
    pub library_paths: Vec<String>,

    /// 链接库
    #[serde(default)]
    pub libraries: Vec<String>,
}

/// 预编译头配置
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PrecompiledHeader {
    /// 是否启用预编译头
    #[serde(default)]
    pub enabled: bool,

    /// 预编译头文件名
    #[serde(default)]
    pub header: Option<String>,
}

impl Default for PrecompiledHeader {
    fn default() -> Self {
        Self {
            enabled: false,
            header: None,
        }
    }
}

/// 反射选项
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ReflectionOptions {
    /// 是否启用 LHT 扫描
    #[serde(default = "default_reflection_enabled")]
    pub enabled: bool,

    /// 要扫描的反射宏
    #[serde(default = "default_reflection_macros")]
    pub macros: Vec<String>,
}

impl Default for ReflectionOptions {
    fn default() -> Self {
        Self {
            enabled: default_reflection_enabled(),
            macros: default_reflection_macros(),
        }
    }
}

fn default_reflection_enabled() -> bool {
    true
}

fn default_reflection_macros() -> Vec<String> {
    vec![
        "LCLASS".to_string(),
        "LSTRUCT".to_string(),
        "LENUM".to_string(),
        "LPROPERTY".to_string(),
        "LFUNCTION".to_string(),
    ]
}

/// 解析后的模块信息 (包含路径等运行时信息)
#[derive(Debug, Clone)]
pub struct Module {
    /// 模块名称
    pub name: String,

    /// 模块类型
    pub module_type: ModuleType,

    /// 命名空间
    pub namespace: Option<String>,

    /// 架构层级
    pub layer: u8,

    /// 模块根目录
    pub path: PathBuf,

    /// 配置文件路径
    pub config_path: PathBuf,

    /// 完整配置
    pub config: ModuleConfig,
}
