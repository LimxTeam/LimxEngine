/*******************************************************************************
 * 文件: core/build_config.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   构建配置管理
 *   - 构建配置文件 (lbt.config.toml)
 *   - 全局构建选项
 *   - 平台特定配置
 *
 ******************************************************************************/

use anyhow::Result;
use serde::{Deserialize, Serialize};
use std::fs;
use std::path::{Path, PathBuf};

/// 构建配置文件名
pub const BUILD_CONFIG_FILE: &str = "lbt.config.toml";

/// 构建配置
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct BuildConfig {
    /// 项目名称
    pub project_name: String,
    /// 源代码目录
    pub source_dir: PathBuf,
    /// 输出目录
    pub output_dir: PathBuf,
    /// 中间文件目录
    pub intermediate_dir: PathBuf,
    /// 编译器选项
    pub compiler: CompilerConfig,
    /// 链接器选项
    pub linker: LinkerConfig,
    /// PCH 配置
    pub pch: PchConfig,
    /// Unity Build 配置
    pub unity_build: UnityBuildConfig,
    /// 平台配置
    pub platform: PlatformConfig,
}

impl Default for BuildConfig {
    fn default() -> Self {
        Self {
            project_name: "LimxEngine".to_string(),
            source_dir: PathBuf::from("Source"),
            output_dir: PathBuf::from("Binaries"),
            intermediate_dir: PathBuf::from("Intermediate"),
            compiler: CompilerConfig::default(),
            linker: LinkerConfig::default(),
            pch: PchConfig::default(),
            unity_build: UnityBuildConfig::default(),
            platform: PlatformConfig::default(),
        }
    }
}

/// 编译器配置
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct CompilerConfig {
    /// C++ 标准
    pub cpp_standard: String,
    /// 优化级别
    pub optimization_level: OptimizationLevel,
    /// 警告级别
    pub warning_level: u8,
    /// 将警告视为错误
    pub warnings_as_errors: bool,
    /// 启用 RTTI
    pub enable_rtti: bool,
    /// 启用异常
    pub enable_exceptions: bool,
    /// 并行编译
    pub parallel_compile: bool,
    /// 并行编译线程数 (0 = 自动)
    pub parallel_jobs: u32,
    /// 额外编译选项
    pub extra_flags: Vec<String>,
    /// 全局宏定义
    pub defines: Vec<String>,
    /// 全局包含路径
    pub include_paths: Vec<PathBuf>,
}

impl Default for CompilerConfig {
    fn default() -> Self {
        Self {
            cpp_standard: "c++23".to_string(),
            optimization_level: OptimizationLevel::Development,
            warning_level: 4,
            warnings_as_errors: true,
            enable_rtti: true,
            enable_exceptions: true,
            parallel_compile: true,
            parallel_jobs: 0,
            extra_flags: Vec::new(),
            defines: vec![
                "LIMX_ENGINE".to_string(),
                "UNICODE".to_string(),
                "_UNICODE".to_string(),
            ],
            include_paths: Vec::new(),
        }
    }
}

/// 优化级别
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum OptimizationLevel {
    Debug,
    Development,
    Release,
    Shipping,
}

impl OptimizationLevel {
    pub fn as_cmake_type(&self) -> &'static str {
        match self {
            Self::Debug => "Debug",
            Self::Development => "RelWithDebInfo",
            Self::Release => "Release",
            Self::Shipping => "MinSizeRel",
        }
    }

    pub fn msvc_flags(&self) -> &'static str {
        match self {
            Self::Debug => "/Od /Zi",
            Self::Development => "/O2 /Zi",
            Self::Release => "/O2 /GL",
            Self::Shipping => "/O1 /GL",
        }
    }
}

/// 链接器配置
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct LinkerConfig {
    /// 启用增量链接
    pub incremental_link: bool,
    /// 生成调试信息
    pub generate_debug_info: bool,
    /// 启用链接时优化 (LTO)
    pub lto: bool,
    /// 额外链接选项
    pub extra_flags: Vec<String>,
    /// 额外库路径
    pub library_paths: Vec<PathBuf>,
    /// 额外链接库
    pub libraries: Vec<String>,
}

impl Default for LinkerConfig {
    fn default() -> Self {
        Self {
            incremental_link: true,
            generate_debug_info: true,
            lto: false,
            extra_flags: Vec::new(),
            library_paths: Vec::new(),
            libraries: Vec::new(),
        }
    }
}

/// PCH 配置
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct PchConfig {
    /// 启用 PCH
    pub enabled: bool,
    /// PCH 头文件模式
    pub header_pattern: String,
    /// 共享 PCH
    pub shared: bool,
}

impl Default for PchConfig {
    fn default() -> Self {
        Self {
            enabled: true,
            header_pattern: "{Module}PCH.h".to_string(),
            shared: false,
        }
    }
}

/// Unity Build 配置
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct UnityBuildConfig {
    /// 启用 Unity Build
    pub enabled: bool,
    /// 每个 Unity 文件的源文件数
    pub batch_size: usize,
    /// 排除的文件模式
    pub exclude_patterns: Vec<String>,
}

impl Default for UnityBuildConfig {
    fn default() -> Self {
        Self {
            enabled: true,
            batch_size: 8,
            exclude_patterns: vec!["*PCH.cpp".to_string(), "*.generated.cpp".to_string()],
        }
    }
}

/// 平台配置
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct PlatformConfig {
    /// 目标平台
    pub target: TargetPlatform,
    /// 架构
    pub architecture: Architecture,
    /// Windows SDK 版本
    pub windows_sdk_version: Option<String>,
    /// VS 工具链版本
    pub vs_toolset: Option<String>,
}

impl Default for PlatformConfig {
    fn default() -> Self {
        Self {
            target: TargetPlatform::Windows,
            architecture: Architecture::X64,
            windows_sdk_version: None,
            vs_toolset: None,
        }
    }
}

/// 目标平台
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum TargetPlatform {
    Windows,
    Linux,
    MacOS,
}

/// 架构
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Architecture {
    X64,
    X86,
    ARM64,
}

impl BuildConfig {
    /// 从文件加载配置
    pub fn load(path: &Path) -> Result<Self> {
        if path.exists() {
            let content = fs::read_to_string(path)?;
            let config: Self = toml::from_str(&content)?;
            Ok(config)
        } else {
            Ok(Self::default())
        }
    }

    /// 保存配置到文件
    pub fn save(&self, path: &Path) -> Result<()> {
        let content = toml::to_string_pretty(self)?;
        fs::write(path, content)?;
        Ok(())
    }

    /// 从目录加载配置
    pub fn load_from_dir(dir: &Path) -> Result<Self> {
        Self::load(&dir.join(BUILD_CONFIG_FILE))
    }

    /// 生成示例配置文件
    pub fn generate_example() -> String {
        let config = Self::default();
        toml::to_string_pretty(&config).unwrap_or_default()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_build_config_default() {
        let config = BuildConfig::default();
        assert_eq!(config.project_name, "LimxEngine");
        assert!(config.compiler.parallel_compile);
        assert!(config.pch.enabled);
    }

    #[test]
    fn test_optimization_level() {
        assert_eq!(OptimizationLevel::Debug.as_cmake_type(), "Debug");
        assert_eq!(OptimizationLevel::Release.as_cmake_type(), "Release");
    }

    #[test]
    fn test_generate_example() {
        let example = BuildConfig::generate_example();
        assert!(!example.is_empty());
        assert!(example.contains("project_name"));
    }
}
