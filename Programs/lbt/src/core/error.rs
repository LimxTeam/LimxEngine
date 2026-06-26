/*******************************************************************************
 * 文件: error.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   LBT 错误类型定义 - 完整错误处理体系
 *
 ******************************************************************************/

use std::path::PathBuf;
use thiserror::Error;

/// LBT 错误类型
#[derive(Error, Debug)]
pub enum LbtError {
    // =========================================================================
    // 配置错误
    // =========================================================================
    #[error("模块配置文件解析失败: {path}")]
    ConfigParseError {
        path: PathBuf,
        #[source]
        source: toml::de::Error,
    },

    #[error("配置验证失败: {message}")]
    ConfigValidationError { message: String },

    #[error("缺少必需的配置字段: {field}")]
    MissingConfigField { field: String },

    // =========================================================================
    // 模块错误
    // =========================================================================
    #[error("模块 '{name}' 未找到")]
    ModuleNotFound { name: String },

    #[error("无效的模块名称: {0}")]
    InvalidModuleName(String),

    #[error("模块 '{name}' 类型无效: {module_type}")]
    InvalidModuleType { name: String, module_type: String },

    #[error("模块 '{name}' 层级无效: {layer} (有效范围: 0-5)")]
    InvalidModuleLayer { name: String, layer: u8 },

    // =========================================================================
    // 依赖错误
    // =========================================================================
    #[error("循环依赖检测: {cycle}")]
    CyclicDependency { cycle: String },

    #[error(
        "层级约束违反: 模块 '{from}' (Layer {from_layer}) 不能依赖模块 '{to}' (Layer {to_layer})"
    )]
    LayerViolation {
        from: String,
        from_layer: u8,
        to: String,
        to_layer: u8,
    },

    #[error("依赖模块 '{dependency}' 不存在 (被 '{module}' 引用)")]
    MissingDependency { module: String, dependency: String },

    #[error("依赖解析失败: {message}")]
    DependencyResolutionError { message: String },

    // =========================================================================
    // 生成器错误
    // =========================================================================
    #[error("CMake 生成失败: {message}")]
    CmakeGenerationError { message: String },

    #[error("VS 解决方案生成失败: {message}")]
    VsGenerationError { message: String },

    #[error("IDE 项目生成失败: {ide} - {message}")]
    IdeGenerationError { ide: String, message: String },

    // =========================================================================
    // 构建错误
    // =========================================================================
    #[error("编译失败: {module}")]
    CompilationError { module: String },

    #[error("链接失败: {module}")]
    LinkError { module: String },

    #[error("构建命令执行失败: {command}")]
    BuildCommandError { command: String },

    // =========================================================================
    // IO 错误
    // =========================================================================
    #[error("IO 错误: {0}")]
    IoError(#[from] std::io::Error),

    #[error("目录不存在: {path}")]
    DirectoryNotFound { path: PathBuf },

    #[error("文件不存在: {path}")]
    FileNotFound { path: PathBuf },

    #[error("权限不足: {path}")]
    PermissionDenied { path: PathBuf },

    // =========================================================================
    // 其他错误
    // =========================================================================
    #[error("内部错误: {message}")]
    InternalError { message: String },

    #[error("{0}")]
    Custom(String),
}

impl LbtError {
    /// 创建自定义错误
    pub fn custom<S: Into<String>>(message: S) -> Self {
        Self::Custom(message.into())
    }

    /// 是否为配置错误
    pub fn is_config_error(&self) -> bool {
        matches!(
            self,
            Self::ConfigParseError { .. }
                | Self::ConfigValidationError { .. }
                | Self::MissingConfigField { .. }
        )
    }

    /// 是否为依赖错误
    pub fn is_dependency_error(&self) -> bool {
        matches!(
            self,
            Self::CyclicDependency { .. }
                | Self::LayerViolation { .. }
                | Self::MissingDependency { .. }
                | Self::DependencyResolutionError { .. }
        )
    }

    /// 是否为构建错误
    pub fn is_build_error(&self) -> bool {
        matches!(
            self,
            Self::CompilationError { .. } | Self::LinkError { .. } | Self::BuildCommandError { .. }
        )
    }

    /// 获取错误代码
    pub fn error_code(&self) -> &'static str {
        match self {
            Self::ConfigParseError { .. } => "E001",
            Self::ConfigValidationError { .. } => "E002",
            Self::MissingConfigField { .. } => "E003",
            Self::ModuleNotFound { .. } => "E010",
            Self::InvalidModuleName(_) => "E011",
            Self::InvalidModuleType { .. } => "E012",
            Self::InvalidModuleLayer { .. } => "E013",
            Self::CyclicDependency { .. } => "E020",
            Self::LayerViolation { .. } => "E021",
            Self::MissingDependency { .. } => "E022",
            Self::DependencyResolutionError { .. } => "E023",
            Self::CmakeGenerationError { .. } => "E030",
            Self::VsGenerationError { .. } => "E031",
            Self::IdeGenerationError { .. } => "E032",
            Self::CompilationError { .. } => "E040",
            Self::LinkError { .. } => "E041",
            Self::BuildCommandError { .. } => "E042",
            Self::IoError(_) => "E050",
            Self::DirectoryNotFound { .. } => "E051",
            Self::FileNotFound { .. } => "E052",
            Self::PermissionDenied { .. } => "E053",
            Self::InternalError { .. } => "E099",
            Self::Custom(_) => "E100",
        }
    }
}

/// 错误结果类型别名
pub type LbtResult<T> = Result<T, LbtError>;
