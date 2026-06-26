/*******************************************************************************
 * 文件: core/error.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   LSC 错误类型定义 (生产级增强版)
 *   - 详细的错误诊断信息
 *   - 源码位置追踪
 *   - 错误恢复建议
 *   - Vulkan 专属错误类型
 *
 * 技术特性:
 *   - 结构化错误信息
 *   - 错误链支持
 *   - 格式化输出
 *
 ******************************************************************************/

use std::path::PathBuf;
use thiserror::Error;

//=============================================================================
// 源码位置
//=============================================================================

/// 源码位置信息
#[derive(Debug, Clone, Default)]
pub struct SourceLocation {
    /// 文件路径
    pub file: Option<PathBuf>,
    /// 行号 (1-indexed)
    pub line: u32,
    /// 列号 (1-indexed)
    pub column: u32,
    /// 源码片段
    pub snippet: Option<String>,
}

impl SourceLocation {
    pub fn new(file: Option<PathBuf>, line: u32, column: u32) -> Self {
        Self {
            file,
            line,
            column,
            snippet: None,
        }
    }

    pub fn with_snippet(mut self, snippet: String) -> Self {
        self.snippet = Some(snippet);
        self
    }

    /// 格式化位置信息
    pub fn format(&self) -> String {
        let file_str = self
            .file
            .as_ref()
            .map(|p| p.display().to_string())
            .unwrap_or_else(|| "<unknown>".to_string());
        format!("{}:{}:{}", file_str, self.line, self.column)
    }
}

impl std::fmt::Display for SourceLocation {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}", self.format())
    }
}

//=============================================================================
// 编译诊断
//=============================================================================

/// 诊断严重级别
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DiagnosticSeverity {
    Error,
    Warning,
    Info,
    Hint,
}

impl std::fmt::Display for DiagnosticSeverity {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Error => write!(f, "error"),
            Self::Warning => write!(f, "warning"),
            Self::Info => write!(f, "info"),
            Self::Hint => write!(f, "hint"),
        }
    }
}

/// 编译诊断信息
#[derive(Debug, Clone)]
pub struct CompileDiagnostic {
    /// 严重级别
    pub severity: DiagnosticSeverity,
    /// 错误代码
    pub code: Option<String>,
    /// 消息
    pub message: String,
    /// 位置
    pub location: Option<SourceLocation>,
    /// 相关信息
    pub related: Vec<(SourceLocation, String)>,
    /// 修复建议
    pub suggestion: Option<String>,
}

impl CompileDiagnostic {
    pub fn error(message: impl Into<String>) -> Self {
        Self {
            severity: DiagnosticSeverity::Error,
            code: None,
            message: message.into(),
            location: None,
            related: Vec::new(),
            suggestion: None,
        }
    }

    pub fn warning(message: impl Into<String>) -> Self {
        Self {
            severity: DiagnosticSeverity::Warning,
            code: None,
            message: message.into(),
            location: None,
            related: Vec::new(),
            suggestion: None,
        }
    }

    pub fn with_code(mut self, code: impl Into<String>) -> Self {
        self.code = Some(code.into());
        self
    }

    pub fn with_location(mut self, location: SourceLocation) -> Self {
        self.location = Some(location);
        self
    }

    pub fn with_suggestion(mut self, suggestion: impl Into<String>) -> Self {
        self.suggestion = Some(suggestion.into());
        self
    }

    pub fn add_related(mut self, location: SourceLocation, message: impl Into<String>) -> Self {
        self.related.push((location, message.into()));
        self
    }

    /// 格式化诊断信息
    pub fn format(&self) -> String {
        let mut output = String::new();

        // 位置和严重级别
        if let Some(ref loc) = self.location {
            output.push_str(&format!("{}: ", loc));
        }
        output.push_str(&format!("{}", self.severity));
        if let Some(ref code) = self.code {
            output.push_str(&format!("[{}]", code));
        }
        output.push_str(&format!(": {}\n", self.message));

        // 源码片段
        if let Some(ref loc) = self.location {
            if let Some(ref snippet) = loc.snippet {
                output.push_str(&format!("  |\n"));
                output.push_str(&format!("{:4} | {}\n", loc.line, snippet));
                output.push_str(&format!(
                    "  | {:>width$}^\n",
                    "",
                    width = loc.column as usize
                ));
            }
        }

        // 相关信息
        for (loc, msg) in &self.related {
            output.push_str(&format!("  --> {}: {}\n", loc, msg));
        }

        // 修复建议
        if let Some(ref suggestion) = self.suggestion {
            output.push_str(&format!("  = help: {}\n", suggestion));
        }

        output
    }
}

impl std::fmt::Display for CompileDiagnostic {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}", self.format())
    }
}

//=============================================================================
// LSC 错误类型
//=============================================================================

/// LSC 错误类型
#[derive(Error, Debug)]
pub enum LscError {
    /// 编译错误 (带诊断信息)
    #[error("编译错误:\n{}", format_diagnostics(.0))]
    CompileWithDiagnostics(Vec<CompileDiagnostic>),

    /// 简单编译错误
    #[error("编译错误: {0}")]
    Compile(String),

    /// 着色器验证失败
    #[error("SPIR-V 验证失败: {message}\n  位置: {location}")]
    ValidationFailed {
        message: String,
        location: String,
        #[source]
        source: Option<Box<dyn std::error::Error + Send + Sync>>,
    },

    /// 简单验证错误
    #[error("着色器验证失败: {0}")]
    Validation(String),

    /// 反射提取错误
    #[error("反射错误 ({stage}): {message}")]
    ReflectionFailed { stage: String, message: String },

    /// 简单反射错误
    #[error("反射错误: {0}")]
    Reflection(String),

    /// IO 错误
    #[error("IO 错误 ({path}): {message}")]
    IoWithPath { path: PathBuf, message: String },

    /// 简单IO错误
    #[error("IO 错误: {0}")]
    Io(String),

    /// 解析错误
    #[error("解析错误 ({location}): {message}")]
    ParseWithLocation {
        location: SourceLocation,
        message: String,
    },

    /// 简单解析错误
    #[error("解析错误: {0}")]
    Parse(String),

    /// 无效的着色器阶段
    #[error("无效的着色器阶段 '{stage}'\n  支持的阶段: vertex, fragment, compute, geometry, tesscontrol, tesseval, raygen, raymiss, rayhit, callable, task, mesh")]
    InvalidStageDetailed {
        stage: String,
        file: Option<PathBuf>,
    },

    /// 简单阶段错误
    #[error("无效的着色器阶段: {0}")]
    InvalidStage(String),

    /// Include 文件未找到
    #[error("包含文件未找到: '{include_path}'\n  从: {from_file}\n  搜索路径: {}", format_search_paths(.search_paths))]
    IncludeNotFoundDetailed {
        include_path: String,
        from_file: PathBuf,
        search_paths: Vec<PathBuf>,
    },

    /// 简单Include错误
    #[error("包含文件未找到: {0}")]
    IncludeNotFound(String),

    /// SPIR-V 结构错误
    #[error("SPIR-V 结构错误: {message}\n  偏移: {offset}\n  期望: {expected}")]
    SpirvStructure {
        message: String,
        offset: usize,
        expected: String,
    },

    /// 简单SPIR-V错误
    #[error("SPIR-V 错误: {0}")]
    Spirv(String),

    /// 缓存错误
    #[error("缓存错误 ({operation}): {message}")]
    CacheOperation { operation: String, message: String },

    /// 简单缓存错误
    #[error("缓存错误: {0}")]
    Cache(String),

    /// 变体错误
    #[error("变体错误: {message}\n  变体: {variant_name}")]
    Variant {
        variant_name: String,
        message: String,
    },

    /// Vulkan 目标环境错误
    #[error("Vulkan 环境错误: {message}\n  要求: Vulkan {required_version}\n  当前: Vulkan {current_version}")]
    VulkanEnvironment {
        message: String,
        required_version: String,
        current_version: String,
    },

    /// 配置错误
    #[error("配置错误: {0}")]
    Config(String),

    /// 内部错误
    #[error("内部错误: {0}\n  请报告此问题到: https://github.com/LimxTeam/Limx")]
    Internal(String),

    /// 超时错误
    #[error("操作超时: {operation} (耗时 {elapsed_ms}ms, 限制 {timeout_ms}ms)")]
    Timeout {
        operation: String,
        elapsed_ms: u64,
        timeout_ms: u64,
    },
}

/// 格式化诊断列表
fn format_diagnostics(diagnostics: &[CompileDiagnostic]) -> String {
    diagnostics
        .iter()
        .map(|d| d.format())
        .collect::<Vec<_>>()
        .join("\n")
}

/// 格式化搜索路径
fn format_search_paths(paths: &[PathBuf]) -> String {
    if paths.is_empty() {
        "<none>".to_string()
    } else {
        paths
            .iter()
            .map(|p| format!("    - {}", p.display()))
            .collect::<Vec<_>>()
            .join("\n")
    }
}

//=============================================================================
// 错误转换
//=============================================================================

impl From<std::io::Error> for LscError {
    fn from(err: std::io::Error) -> Self {
        LscError::Io(err.to_string())
    }
}

impl From<shaderc::Error> for LscError {
    fn from(err: shaderc::Error) -> Self {
        // 尝试解析 shaderc 错误信息
        let msg = err.to_string();
        LscError::Compile(msg)
    }
}

impl From<serde_json::Error> for LscError {
    fn from(err: serde_json::Error) -> Self {
        LscError::Parse(format!("JSON 解析错误: {}", err))
    }
}

//=============================================================================
// 错误构建器
//=============================================================================

impl LscError {
    /// 创建带位置的IO错误
    pub fn io_with_path(path: impl Into<PathBuf>, message: impl Into<String>) -> Self {
        Self::IoWithPath {
            path: path.into(),
            message: message.into(),
        }
    }

    /// 创建包含文件未找到错误
    pub fn include_not_found(
        include_path: impl Into<String>,
        from_file: impl Into<PathBuf>,
        search_paths: Vec<PathBuf>,
    ) -> Self {
        Self::IncludeNotFoundDetailed {
            include_path: include_path.into(),
            from_file: from_file.into(),
            search_paths,
        }
    }

    /// 创建验证失败错误
    pub fn validation_failed(message: impl Into<String>, location: impl Into<String>) -> Self {
        Self::ValidationFailed {
            message: message.into(),
            location: location.into(),
            source: None,
        }
    }

    /// 创建变体错误
    pub fn variant(variant_name: impl Into<String>, message: impl Into<String>) -> Self {
        Self::Variant {
            variant_name: variant_name.into(),
            message: message.into(),
        }
    }

    /// 创建Vulkan环境错误
    pub fn vulkan_env(
        message: impl Into<String>,
        required: impl Into<String>,
        current: impl Into<String>,
    ) -> Self {
        Self::VulkanEnvironment {
            message: message.into(),
            required_version: required.into(),
            current_version: current.into(),
        }
    }

    /// 检查是否为可恢复错误
    pub fn is_recoverable(&self) -> bool {
        matches!(
            self,
            LscError::Cache(_) | LscError::CacheOperation { .. } | LscError::Timeout { .. }
        )
    }

    /// 获取错误代码
    pub fn error_code(&self) -> &'static str {
        match self {
            LscError::CompileWithDiagnostics(_) => "E001",
            LscError::Compile(_) => "E001",
            LscError::ValidationFailed { .. } => "E002",
            LscError::Validation(_) => "E002",
            LscError::ReflectionFailed { .. } => "E003",
            LscError::Reflection(_) => "E003",
            LscError::IoWithPath { .. } => "E004",
            LscError::Io(_) => "E004",
            LscError::ParseWithLocation { .. } => "E005",
            LscError::Parse(_) => "E005",
            LscError::InvalidStageDetailed { .. } => "E006",
            LscError::InvalidStage(_) => "E006",
            LscError::IncludeNotFoundDetailed { .. } => "E007",
            LscError::IncludeNotFound(_) => "E007",
            LscError::SpirvStructure { .. } => "E008",
            LscError::Spirv(_) => "E008",
            LscError::CacheOperation { .. } => "E009",
            LscError::Cache(_) => "E009",
            LscError::Variant { .. } => "E010",
            LscError::VulkanEnvironment { .. } => "E011",
            LscError::Config(_) => "E012",
            LscError::Internal(_) => "E999",
            LscError::Timeout { .. } => "E013",
        }
    }
}

/// LSC Result 类型别名
pub type LscResult<T> = Result<T, LscError>;
