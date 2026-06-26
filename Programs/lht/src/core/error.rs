/*******************************************************************************
 * 文件: error.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   LHT 错误类型定义 - 完整错误处理体系
 *
 ******************************************************************************/

use std::path::PathBuf;
use thiserror::Error;

/// LHT 错误类型
#[derive(Error, Debug)]
#[allow(dead_code)]
pub enum LhtError {
    // =========================================================================
    // 解析错误
    // =========================================================================
    #[error("头文件解析失败: {path}")]
    ParseError { path: PathBuf, message: String },

    #[error("反射宏语法错误: {message}")]
    MacroSyntaxError { message: String },

    #[error("无效的说明符: {specifier}")]
    InvalidSpecifier { specifier: String },

    #[error("类型解析失败: {type_name}")]
    TypeParseError { type_name: String },

    #[error("不支持的类型: {type_name}")]
    UnsupportedType { type_name: String },

    // =========================================================================
    // 代码生成错误
    // =========================================================================
    #[error("代码生成失败: {message}")]
    CodeGenError { message: String },

    #[error("模板渲染失败: {template}")]
    TemplateError { template: String },

    #[error("输出目录创建失败: {path}")]
    OutputDirectoryError { path: PathBuf },

    // =========================================================================
    // 热重载错误
    // =========================================================================
    #[error("编译失败: {module}")]
    CompilationError { module: String },

    #[error("DLL 加载失败: {path}")]
    DllLoadError { path: PathBuf },

    #[error("DLL 卸载失败: {path}")]
    DllUnloadError { path: PathBuf },

    #[error("符号查找失败: {symbol}")]
    SymbolNotFound { symbol: String },

    #[error("状态序列化失败: {message}")]
    SerializationError { message: String },

    #[error("状态反序列化失败: {message}")]
    DeserializationError { message: String },

    #[error("热重载回滚失败: {message}")]
    RollbackError { message: String },

    // =========================================================================
    // 文件监控错误
    // =========================================================================
    #[error("文件监控初始化失败: {message}")]
    WatcherInitError { message: String },

    #[error("文件监控错误: {path}")]
    WatchError { path: PathBuf },

    // =========================================================================
    // IO 错误
    // =========================================================================
    #[error("IO 错误: {0}")]
    IoError(#[from] std::io::Error),

    #[error("文件不存在: {path}")]
    FileNotFound { path: PathBuf },

    #[error("目录不存在: {path}")]
    DirectoryNotFound { path: PathBuf },

    // =========================================================================
    // 其他错误
    // =========================================================================
    #[error("内部错误: {message}")]
    InternalError { message: String },

    #[error("{0}")]
    Custom(String),
}

#[allow(dead_code)]
impl LhtError {
    /// 创建自定义错误
    pub fn custom<S: Into<String>>(message: S) -> Self {
        Self::Custom(message.into())
    }

    /// 是否为解析错误
    pub fn is_parse_error(&self) -> bool {
        matches!(
            self,
            Self::ParseError { .. }
                | Self::MacroSyntaxError { .. }
                | Self::InvalidSpecifier { .. }
                | Self::TypeParseError { .. }
        )
    }

    /// 是否为热重载错误
    pub fn is_hotreload_error(&self) -> bool {
        matches!(
            self,
            Self::CompilationError { .. }
                | Self::DllLoadError { .. }
                | Self::DllUnloadError { .. }
                | Self::SymbolNotFound { .. }
                | Self::SerializationError { .. }
                | Self::DeserializationError { .. }
                | Self::RollbackError { .. }
        )
    }

    /// 获取错误代码
    pub fn error_code(&self) -> &'static str {
        match self {
            Self::ParseError { .. } => "L001",
            Self::MacroSyntaxError { .. } => "L002",
            Self::InvalidSpecifier { .. } => "L003",
            Self::TypeParseError { .. } => "L004",
            Self::UnsupportedType { .. } => "L005",
            Self::CodeGenError { .. } => "L010",
            Self::TemplateError { .. } => "L011",
            Self::OutputDirectoryError { .. } => "L012",
            Self::CompilationError { .. } => "L020",
            Self::DllLoadError { .. } => "L021",
            Self::DllUnloadError { .. } => "L022",
            Self::SymbolNotFound { .. } => "L023",
            Self::SerializationError { .. } => "L024",
            Self::DeserializationError { .. } => "L025",
            Self::RollbackError { .. } => "L026",
            Self::WatcherInitError { .. } => "L030",
            Self::WatchError { .. } => "L031",
            Self::IoError(_) => "L050",
            Self::FileNotFound { .. } => "L051",
            Self::DirectoryNotFound { .. } => "L052",
            Self::InternalError { .. } => "L099",
            Self::Custom(_) => "L100",
        }
    }
}

/// 错误结果类型别名
#[allow(dead_code)]
pub type LhtResult<T> = Result<T, LhtError>;
