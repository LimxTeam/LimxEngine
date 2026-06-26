/*******************************************************************************
 * 文件: compiler/diagnostics.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   编译诊断系统 - 统一的错误/警告/提示管理
 *   - 结构化诊断信息
 *   - 诊断格式化输出
 *   - 诊断聚合与统计
 *   - 诊断过滤与抑制
 *
 * 技术特性:
 *   - 跨编译器统一格式
 *   - 支持关联诊断 (相关位置)
 *   - 支持诊断修复建议
 *   - 支持 SARIF 格式输出
 *
 ******************************************************************************/

use std::collections::HashMap;
use std::fmt;
use std::path::PathBuf;

//=============================================================================
// 诊断严重级别
//=============================================================================

/// 诊断严重级别
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum DiagnosticSeverity {
    /// 提示信息
    Note,
    /// 警告
    Warning,
    /// 错误
    Error,
    /// 致命错误
    Fatal,
}

impl DiagnosticSeverity {
    /// 获取级别名称
    pub fn name(&self) -> &'static str {
        match self {
            Self::Note => "note",
            Self::Warning => "warning",
            Self::Error => "error",
            Self::Fatal => "fatal error",
        }
    }

    /// 获取 ANSI 颜色代码
    pub fn color_code(&self) -> &'static str {
        match self {
            Self::Note => "\x1b[36m",    // 青色
            Self::Warning => "\x1b[33m", // 黄色
            Self::Error => "\x1b[31m",   // 红色
            Self::Fatal => "\x1b[1;31m", // 粗体红色
        }
    }

    /// 是否为错误级别
    pub fn is_error(&self) -> bool {
        matches!(self, Self::Error | Self::Fatal)
    }
}

impl fmt::Display for DiagnosticSeverity {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.name())
    }
}

//=============================================================================
// 诊断信息
//=============================================================================

/// 诊断信息
#[derive(Debug, Clone)]
pub struct Diagnostic {
    /// 严重级别
    pub severity: DiagnosticSeverity,
    /// 源文件路径
    pub file: Option<PathBuf>,
    /// 行号 (1-indexed)
    pub line: u32,
    /// 列号 (1-indexed)
    pub column: u32,
    /// 错误/警告代码
    pub code: Option<String>,
    /// 诊断消息
    pub message: String,
    /// 来源 (编译器名称)
    pub source: Option<String>,
    /// 相关诊断
    pub related: Vec<RelatedDiagnostic>,
}

impl Diagnostic {
    /// 创建错误诊断
    pub fn error(message: impl Into<String>) -> Self {
        Self {
            severity: DiagnosticSeverity::Error,
            file: None,
            line: 0,
            column: 0,
            code: None,
            message: message.into(),
            source: None,
            related: Vec::new(),
        }
    }

    /// 创建警告诊断
    pub fn warning(message: impl Into<String>) -> Self {
        Self {
            severity: DiagnosticSeverity::Warning,
            file: None,
            line: 0,
            column: 0,
            code: None,
            message: message.into(),
            source: None,
            related: Vec::new(),
        }
    }

    /// 创建提示诊断
    pub fn note(message: impl Into<String>) -> Self {
        Self {
            severity: DiagnosticSeverity::Note,
            file: None,
            line: 0,
            column: 0,
            code: None,
            message: message.into(),
            source: None,
            related: Vec::new(),
        }
    }

    /// 设置文件位置
    pub fn at(mut self, file: PathBuf, line: u32, column: u32) -> Self {
        self.file = Some(file);
        self.line = line;
        self.column = column;
        self
    }

    /// 设置错误代码
    pub fn with_code(mut self, code: impl Into<String>) -> Self {
        self.code = Some(code.into());
        self
    }

    /// 设置来源
    pub fn from_source(mut self, source: impl Into<String>) -> Self {
        self.source = Some(source.into());
        self
    }

    /// 添加相关诊断
    pub fn with_related(mut self, related: RelatedDiagnostic) -> Self {
        self.related.push(related);
        self
    }

    /// 格式化为单行字符串
    pub fn format_oneline(&self) -> String {
        let mut result = String::new();

        // 文件位置
        if let Some(ref file) = self.file {
            result.push_str(&file.display().to_string());
            if self.line > 0 {
                result.push_str(&format!(":{}", self.line));
                if self.column > 0 {
                    result.push_str(&format!(":{}", self.column));
                }
            }
            result.push_str(": ");
        }

        // 严重级别
        result.push_str(self.severity.name());

        // 错误代码
        if let Some(ref code) = self.code {
            result.push_str(&format!(" {}", code));
        }

        result.push_str(": ");
        result.push_str(&self.message);

        result
    }

    /// 格式化为带颜色的字符串
    pub fn format_colored(&self) -> String {
        let reset = "\x1b[0m";
        let bold = "\x1b[1m";
        let color = self.severity.color_code();

        let mut result = String::new();

        // 文件位置 (粗体白色)
        if let Some(ref file) = self.file {
            result.push_str(bold);
            result.push_str(&file.display().to_string());
            if self.line > 0 {
                result.push_str(&format!(":{}", self.line));
                if self.column > 0 {
                    result.push_str(&format!(":{}", self.column));
                }
            }
            result.push_str(reset);
            result.push_str(": ");
        }

        // 严重级别 (带颜色)
        result.push_str(bold);
        result.push_str(color);
        result.push_str(self.severity.name());

        // 错误代码
        if let Some(ref code) = self.code {
            result.push_str(&format!(" {}", code));
        }

        result.push_str(reset);
        result.push_str(": ");

        // 消息 (粗体)
        result.push_str(bold);
        result.push_str(&self.message);
        result.push_str(reset);

        result
    }
}

impl fmt::Display for Diagnostic {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.format_oneline())
    }
}

//=============================================================================
// 相关诊断
//=============================================================================

/// 相关诊断信息 (如 "see declaration here")
#[derive(Debug, Clone)]
pub struct RelatedDiagnostic {
    /// 文件路径
    pub file: PathBuf,
    /// 行号
    pub line: u32,
    /// 列号
    pub column: u32,
    /// 消息
    pub message: String,
}

impl RelatedDiagnostic {
    pub fn new(file: PathBuf, line: u32, column: u32, message: impl Into<String>) -> Self {
        Self {
            file,
            line,
            column,
            message: message.into(),
        }
    }
}

//=============================================================================
// 诊断收集器
//=============================================================================

/// 诊断收集器
#[derive(Debug, Default)]
pub struct DiagnosticCollector {
    /// 所有诊断
    diagnostics: Vec<Diagnostic>,
    /// 按文件分组的诊断
    by_file: HashMap<PathBuf, Vec<usize>>,
    /// 错误计数
    error_count: usize,
    /// 警告计数
    warning_count: usize,
    /// 是否有致命错误
    has_fatal: bool,
}

impl DiagnosticCollector {
    pub fn new() -> Self {
        Self::default()
    }

    /// 添加诊断
    pub fn add(&mut self, diag: Diagnostic) {
        let index = self.diagnostics.len();

        // 更新计数
        match diag.severity {
            DiagnosticSeverity::Error => self.error_count += 1,
            DiagnosticSeverity::Fatal => {
                self.error_count += 1;
                self.has_fatal = true;
            }
            DiagnosticSeverity::Warning => self.warning_count += 1,
            _ => {}
        }

        // 按文件索引
        if let Some(ref file) = diag.file {
            self.by_file.entry(file.clone()).or_default().push(index);
        }

        self.diagnostics.push(diag);
    }

    /// 批量添加诊断
    pub fn add_all(&mut self, diags: impl IntoIterator<Item = Diagnostic>) {
        for diag in diags {
            self.add(diag);
        }
    }

    /// 获取所有诊断
    pub fn diagnostics(&self) -> &[Diagnostic] {
        &self.diagnostics
    }

    /// 获取错误数量
    pub fn error_count(&self) -> usize {
        self.error_count
    }

    /// 获取警告数量
    pub fn warning_count(&self) -> usize {
        self.warning_count
    }

    /// 是否有错误
    pub fn has_errors(&self) -> bool {
        self.error_count > 0
    }

    /// 是否有致命错误
    pub fn has_fatal(&self) -> bool {
        self.has_fatal
    }

    /// 获取指定文件的诊断
    pub fn diagnostics_for_file(&self, file: &PathBuf) -> Vec<&Diagnostic> {
        self.by_file
            .get(file)
            .map(|indices| indices.iter().map(|&i| &self.diagnostics[i]).collect())
            .unwrap_or_default()
    }

    /// 获取所有错误
    pub fn errors(&self) -> impl Iterator<Item = &Diagnostic> {
        self.diagnostics.iter().filter(|d| d.severity.is_error())
    }

    /// 获取所有警告
    pub fn warnings(&self) -> impl Iterator<Item = &Diagnostic> {
        self.diagnostics
            .iter()
            .filter(|d| d.severity == DiagnosticSeverity::Warning)
    }

    /// 清空所有诊断
    pub fn clear(&mut self) {
        self.diagnostics.clear();
        self.by_file.clear();
        self.error_count = 0;
        self.warning_count = 0;
        self.has_fatal = false;
    }

    /// 打印所有诊断到标准错误
    pub fn print_all(&self, colored: bool) {
        for diag in &self.diagnostics {
            if colored {
                eprintln!("{}", diag.format_colored());
            } else {
                eprintln!("{}", diag.format_oneline());
            }

            // 打印相关诊断
            for related in &diag.related {
                if colored {
                    eprintln!(
                        "  \x1b[36mnote\x1b[0m: {}:{}:{}: {}",
                        related.file.display(),
                        related.line,
                        related.column,
                        related.message
                    );
                } else {
                    eprintln!(
                        "  note: {}:{}:{}: {}",
                        related.file.display(),
                        related.line,
                        related.column,
                        related.message
                    );
                }
            }
        }
    }

    /// 打印摘要
    pub fn print_summary(&self, colored: bool) {
        if self.error_count > 0 || self.warning_count > 0 {
            let reset = if colored { "\x1b[0m" } else { "" };
            let red = if colored { "\x1b[31m" } else { "" };
            let yellow = if colored { "\x1b[33m" } else { "" };

            let mut parts = Vec::new();
            if self.error_count > 0 {
                parts.push(format!("{}{} error(s){}", red, self.error_count, reset));
            }
            if self.warning_count > 0 {
                parts.push(format!(
                    "{}{} warning(s){}",
                    yellow, self.warning_count, reset
                ));
            }

            eprintln!("\n{} generated.", parts.join(" and "));
        }
    }

    /// 合并另一个收集器
    pub fn merge(&mut self, other: DiagnosticCollector) {
        for diag in other.diagnostics {
            self.add(diag);
        }
    }
}

//=============================================================================
// 诊断过滤器
//=============================================================================

/// 诊断过滤器
#[derive(Debug, Clone, Default)]
pub struct DiagnosticFilter {
    /// 忽略的警告代码
    ignored_warnings: Vec<String>,
    /// 视为错误的警告代码
    warnings_as_errors: Vec<String>,
    /// 最大错误数 (超过后停止)
    max_errors: Option<usize>,
    /// 是否显示所有警告
    show_all_warnings: bool,
}

impl DiagnosticFilter {
    pub fn new() -> Self {
        Self::default()
    }

    /// 忽略指定警告
    pub fn ignore_warning(&mut self, code: impl Into<String>) -> &mut Self {
        self.ignored_warnings.push(code.into());
        self
    }

    /// 将警告视为错误
    pub fn treat_as_error(&mut self, code: impl Into<String>) -> &mut Self {
        self.warnings_as_errors.push(code.into());
        self
    }

    /// 设置最大错误数
    pub fn max_errors(&mut self, count: usize) -> &mut Self {
        self.max_errors = Some(count);
        self
    }

    /// 应用过滤器
    pub fn apply(&self, diag: &mut Diagnostic) -> bool {
        // 检查是否忽略
        if let Some(ref code) = diag.code {
            if self.ignored_warnings.iter().any(|c| c == code) {
                return false;
            }

            // 检查是否升级为错误
            if diag.severity == DiagnosticSeverity::Warning {
                if self.warnings_as_errors.iter().any(|c| c == code) {
                    diag.severity = DiagnosticSeverity::Error;
                }
            }
        }

        true
    }

    /// 检查是否应停止 (超过最大错误数)
    pub fn should_stop(&self, error_count: usize) -> bool {
        self.max_errors
            .map(|max| error_count >= max)
            .unwrap_or(false)
    }
}

//=============================================================================
// SARIF 输出格式
//=============================================================================

/// SARIF (Static Analysis Results Interchange Format) 输出
pub struct SarifOutput {
    /// 工具名称
    tool_name: String,
    /// 工具版本
    tool_version: String,
}

impl SarifOutput {
    pub fn new(tool_name: impl Into<String>, tool_version: impl Into<String>) -> Self {
        Self {
            tool_name: tool_name.into(),
            tool_version: tool_version.into(),
        }
    }

    /// 将诊断转换为 SARIF JSON
    pub fn to_json(&self, diagnostics: &[Diagnostic]) -> String {
        let mut results = Vec::new();

        for diag in diagnostics {
            let level = match diag.severity {
                DiagnosticSeverity::Error | DiagnosticSeverity::Fatal => "error",
                DiagnosticSeverity::Warning => "warning",
                DiagnosticSeverity::Note => "note",
            };

            let mut result = format!(
                r#"{{
          "ruleId": "{}",
          "level": "{}",
          "message": {{ "text": "{}" }}"#,
                diag.code.as_deref().unwrap_or("unknown"),
                level,
                Self::escape_json(&diag.message)
            );

            if let Some(ref file) = diag.file {
                result.push_str(&format!(
                    r#",
          "locations": [{{
            "physicalLocation": {{
              "artifactLocation": {{ "uri": "{}" }},
              "region": {{ "startLine": {}, "startColumn": {} }}
            }}
          }}]"#,
                    Self::escape_json(&file.display().to_string()),
                    diag.line,
                    diag.column
                ));
            }

            result.push_str("\n        }");
            results.push(result);
        }

        format!(
            r#"{{
  "$schema": "https://raw.githubusercontent.com/oasis-tcs/sarif-spec/master/Schemata/sarif-schema-2.1.0.json",
  "version": "2.1.0",
  "runs": [{{
    "tool": {{
      "driver": {{
        "name": "{}",
        "version": "{}"
      }}
    }},
    "results": [
        {}
    ]
  }}]
}}"#,
            Self::escape_json(&self.tool_name),
            Self::escape_json(&self.tool_version),
            results.join(",\n        ")
        )
    }

    fn escape_json(s: &str) -> String {
        s.replace('\\', "\\\\")
            .replace('"', "\\\"")
            .replace('\n', "\\n")
            .replace('\r', "\\r")
            .replace('\t', "\\t")
    }
}

//=============================================================================
// 诊断统计
//=============================================================================

/// 诊断统计信息
#[derive(Debug, Clone, Default)]
pub struct DiagnosticStats {
    /// 按严重级别统计
    pub by_severity: HashMap<DiagnosticSeverity, usize>,
    /// 按错误代码统计
    pub by_code: HashMap<String, usize>,
    /// 按文件统计
    pub by_file: HashMap<PathBuf, usize>,
    /// 最常见的错误
    pub most_common: Vec<(String, usize)>,
}

impl DiagnosticStats {
    /// 从诊断列表计算统计
    pub fn from_diagnostics(diagnostics: &[Diagnostic]) -> Self {
        let mut stats = Self::default();

        for diag in diagnostics {
            // 按严重级别
            *stats.by_severity.entry(diag.severity).or_default() += 1;

            // 按错误代码
            if let Some(ref code) = diag.code {
                *stats.by_code.entry(code.clone()).or_default() += 1;
            }

            // 按文件
            if let Some(ref file) = diag.file {
                *stats.by_file.entry(file.clone()).or_default() += 1;
            }
        }

        // 计算最常见错误
        let mut codes: Vec<_> = stats.by_code.iter().map(|(k, v)| (k.clone(), *v)).collect();
        codes.sort_by(|a, b| b.1.cmp(&a.1));
        stats.most_common = codes.into_iter().take(10).collect();

        stats
    }

    /// 打印统计信息
    pub fn print(&self) {
        println!("\n诊断统计:");
        println!("  按严重级别:");
        for (severity, count) in &self.by_severity {
            println!("    {}: {}", severity.name(), count);
        }

        if !self.most_common.is_empty() {
            println!("  最常见问题:");
            for (code, count) in &self.most_common {
                println!("    {}: {} 次", code, count);
            }
        }

        println!("  受影响文件: {} 个", self.by_file.len());
    }
}
