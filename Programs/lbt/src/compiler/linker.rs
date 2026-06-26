/*******************************************************************************
 * 文件: compiler/linker.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   链接器抽象与通用实现
 *   - LLD 链接器包装
 *   - 链接配置管理
 *   - 符号导出控制
 *   - 版本脚本支持
 *
 ******************************************************************************/

use anyhow::{anyhow, Context, Result};
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::Instant;

use super::{
    Architecture, Diagnostic, DiagnosticSeverity, LinkResult, LinkUnit, Linker, LinkerConfig,
    Platform, TargetType,
};

//=============================================================================
// LLD 链接器 (跨平台)
//=============================================================================

/// LLD 链接器实现 (已在 clang.rs 中定义，这里提供额外功能)
pub struct LldLinker {
    /// 链接器路径
    pub lld_path: PathBuf,
    /// 归档工具路径  
    pub ar_path: PathBuf,
    /// 目标平台
    pub platform: Platform,
}

impl LldLinker {
    /// 检测 LLD
    pub fn detect() -> Result<Self> {
        let platform = Platform::host();

        let lld_name = match platform {
            Platform::Windows => "lld-link",
            _ => "ld.lld",
        };

        let lld_path = which::which(lld_name)
            .or_else(|_| which::which("lld"))
            .context("无法找到 LLD 链接器")?;

        let ar_path = which::which("llvm-ar")
            .or_else(|_| which::which("ar"))
            .unwrap_or_else(|_| PathBuf::from("ar"));

        Ok(Self {
            lld_path,
            ar_path,
            platform,
        })
    }

    /// 从指定路径创建
    pub fn from_path(lld_path: PathBuf, platform: Platform) -> Result<Self> {
        if !lld_path.exists() {
            return Err(anyhow!("LLD 不存在: {:?}", lld_path));
        }

        let ar_path = lld_path
            .parent()
            .map(|p| p.join("llvm-ar"))
            .filter(|p| p.exists())
            .or_else(|| which::which("llvm-ar").ok())
            .unwrap_or_else(|| PathBuf::from("ar"));

        Ok(Self {
            lld_path,
            ar_path,
            platform,
        })
    }
}

impl Linker for LldLinker {
    fn name(&self) -> &str {
        "LLD"
    }

    fn link(&self, unit: &LinkUnit, config: &LinkerConfig) -> Result<LinkResult> {
        let start = Instant::now();

        if let Some(parent) = unit.output_file.parent() {
            fs::create_dir_all(parent)?;
        }

        let args = match self.platform {
            Platform::Windows => self.build_windows_args(unit, config),
            _ => self.build_unix_args(unit, config),
        };

        let output = Command::new(&self.lld_path)
            .args(&args)
            .output()
            .context("执行 LLD 失败")?;

        let duration_ms = start.elapsed().as_millis() as u64;

        if output.status.success() {
            let mut result = LinkResult::success(unit.output_file.clone(), duration_ms);
            result.import_lib = unit.import_lib.clone();
            result.pdb_file = unit.pdb_file.clone();
            Ok(result)
        } else {
            let stderr = String::from_utf8_lossy(&output.stderr);
            let diag = Diagnostic {
                severity: DiagnosticSeverity::Error,
                file: None,
                line: 0,
                column: 0,
                code: None,
                message: stderr.to_string(),
                source: Some("lld".to_string()),
                related: Vec::new(),
            };
            Ok(LinkResult::failure(vec![diag], duration_ms))
        }
    }

    fn create_static_lib(
        &self,
        objects: &[PathBuf],
        output: &Path,
        _config: &LinkerConfig,
    ) -> Result<LinkResult> {
        let start = Instant::now();

        if let Some(parent) = output.parent() {
            fs::create_dir_all(parent)?;
        }

        let mut args = vec!["rcs".to_string(), output.display().to_string()];
        for obj in objects {
            args.push(obj.display().to_string());
        }

        let result = Command::new(&self.ar_path)
            .args(&args)
            .output()
            .context("执行归档工具失败")?;

        let duration_ms = start.elapsed().as_millis() as u64;

        if result.status.success() {
            Ok(LinkResult::success(output.to_path_buf(), duration_ms))
        } else {
            let stderr = String::from_utf8_lossy(&result.stderr);
            Ok(LinkResult::failure(
                vec![Diagnostic::error(stderr.to_string())],
                duration_ms,
            ))
        }
    }

    fn default_lib_paths(&self) -> Vec<PathBuf> {
        match self.platform {
            Platform::Windows => vec![],
            _ => vec![PathBuf::from("/usr/lib"), PathBuf::from("/usr/local/lib")],
        }
    }
}

impl LldLinker {
    fn build_windows_args(&self, unit: &LinkUnit, config: &LinkerConfig) -> Vec<String> {
        let mut args = Vec::with_capacity(32);

        args.push("/NOLOGO".to_string());

        match unit.target_type {
            TargetType::DynamicLibrary => args.push("/DLL".to_string()),
            _ => {}
        }

        args.push(format!("/OUT:{}", unit.output_file.display()));

        if let Some(ref import_lib) = unit.import_lib {
            args.push(format!("/IMPLIB:{}", import_lib.display()));
        }

        if config.debug_info {
            args.push("/DEBUG:FULL".to_string());
            if let Some(ref pdb) = unit.pdb_file {
                args.push(format!("/PDB:{}", pdb.display()));
            }
        }

        if config.incremental {
            args.push("/INCREMENTAL".to_string());
        } else {
            args.push("/INCREMENTAL:NO".to_string());
        }

        if config.configuration.is_optimized() {
            args.push("/OPT:REF".to_string());
            args.push("/OPT:ICF".to_string());
        }

        if config.enable_lto {
            args.push("/LTCG".to_string());
        }

        if let Some(subsystem) = unit.subsystem {
            args.push(subsystem.as_msvc_flag().to_string());
        }

        match config.architecture {
            Architecture::X64 => args.push("/MACHINE:X64".to_string()),
            Architecture::X86 => args.push("/MACHINE:X86".to_string()),
            Architecture::ARM64 => args.push("/MACHINE:ARM64".to_string()),
            Architecture::ARM32 => args.push("/MACHINE:ARM".to_string()),
        }

        for dir in &unit.lib_dirs {
            args.push(format!("/LIBPATH:{}", dir.display()));
        }

        for lib in &unit.static_libs {
            args.push(lib.display().to_string());
        }

        for lib in &unit.dynamic_libs {
            if lib.ends_with(".lib") {
                args.push(lib.clone());
            } else {
                args.push(format!("{}.lib", lib));
            }
        }

        for flag in &unit.extra_flags {
            args.push(flag.clone());
        }

        for obj in &unit.object_files {
            args.push(obj.display().to_string());
        }

        args
    }

    fn build_unix_args(&self, unit: &LinkUnit, config: &LinkerConfig) -> Vec<String> {
        let mut args = Vec::with_capacity(32);

        match unit.target_type {
            TargetType::DynamicLibrary => {
                args.push("-shared".to_string());
                if self.platform == Platform::MacOS {
                    args.push("-dynamiclib".to_string());
                }
            }
            TargetType::Executable => {
                if self.platform != Platform::MacOS {
                    args.push("-pie".to_string());
                }
            }
            _ => {}
        }

        args.push("-o".to_string());
        args.push(unit.output_file.display().to_string());

        if config.configuration.is_optimized() {
            args.push("--gc-sections".to_string());
        }

        if config.enable_lto {
            args.push("--lto=thin".to_string());
        }

        for dir in &unit.lib_dirs {
            args.push(format!("-L{}", dir.display()));
        }

        // 标准库
        args.push("-lstdc++".to_string());
        args.push("-lm".to_string());

        if self.platform == Platform::Linux {
            args.push("-lpthread".to_string());
            args.push("-ldl".to_string());
        }

        for lib in &unit.static_libs {
            args.push(lib.display().to_string());
        }

        for lib in &unit.dynamic_libs {
            args.push(format!("-l{}", lib));
        }

        for flag in &unit.extra_flags {
            args.push(flag.clone());
        }

        for obj in &unit.object_files {
            args.push(obj.display().to_string());
        }

        args
    }
}

//=============================================================================
// 链接脚本生成器
//=============================================================================

/// 版本脚本生成器 (Linux)
pub struct VersionScriptGenerator {
    /// 导出的符号
    exported_symbols: Vec<String>,
    /// 本地符号 (不导出)
    local_patterns: Vec<String>,
}

impl VersionScriptGenerator {
    pub fn new() -> Self {
        Self {
            exported_symbols: Vec::new(),
            local_patterns: vec!["*".to_string()],
        }
    }

    /// 添加导出符号
    pub fn export(&mut self, symbol: impl Into<String>) -> &mut Self {
        self.exported_symbols.push(symbol.into());
        self
    }

    /// 添加导出模式
    pub fn export_pattern(&mut self, pattern: impl Into<String>) -> &mut Self {
        self.exported_symbols.push(pattern.into());
        self
    }

    /// 生成版本脚本内容
    pub fn generate(&self) -> String {
        let mut script = String::new();
        script.push_str("{\n");
        script.push_str("  global:\n");

        for symbol in &self.exported_symbols {
            script.push_str(&format!("    {};\n", symbol));
        }

        script.push_str("  local:\n");
        for pattern in &self.local_patterns {
            script.push_str(&format!("    {};\n", pattern));
        }

        script.push_str("};\n");
        script
    }

    /// 保存到文件
    pub fn save(&self, path: &Path) -> Result<()> {
        let content = self.generate();
        fs::write(path, content)?;
        Ok(())
    }
}

impl Default for VersionScriptGenerator {
    fn default() -> Self {
        Self::new()
    }
}

//=============================================================================
// 模块定义文件生成器 (Windows)
//=============================================================================

/// DEF 文件生成器 (Windows DLL)
pub struct DefFileGenerator {
    /// 库名称
    library_name: String,
    /// 导出函数
    exports: Vec<DefExport>,
}

/// DEF 导出条目
pub struct DefExport {
    /// 符号名
    pub name: String,
    /// 序号
    pub ordinal: Option<u32>,
    /// 是否为数据 (NONAME)
    pub noname: bool,
    /// 是否为私有
    pub private: bool,
}

impl DefFileGenerator {
    pub fn new(library_name: impl Into<String>) -> Self {
        Self {
            library_name: library_name.into(),
            exports: Vec::new(),
        }
    }

    /// 添加导出
    pub fn export(&mut self, name: impl Into<String>) -> &mut Self {
        self.exports.push(DefExport {
            name: name.into(),
            ordinal: None,
            noname: false,
            private: false,
        });
        self
    }

    /// 添加带序号的导出
    pub fn export_with_ordinal(&mut self, name: impl Into<String>, ordinal: u32) -> &mut Self {
        self.exports.push(DefExport {
            name: name.into(),
            ordinal: Some(ordinal),
            noname: false,
            private: false,
        });
        self
    }

    /// 生成 DEF 文件内容
    pub fn generate(&self) -> String {
        let mut content = String::new();

        content.push_str(&format!("LIBRARY {}\n", self.library_name));
        content.push_str("EXPORTS\n");

        for export in &self.exports {
            content.push_str(&format!("    {}", export.name));

            if let Some(ordinal) = export.ordinal {
                content.push_str(&format!(" @{}", ordinal));
            }

            if export.noname {
                content.push_str(" NONAME");
            }

            if export.private {
                content.push_str(" PRIVATE");
            }

            content.push('\n');
        }

        content
    }

    /// 保存到文件
    pub fn save(&self, path: &Path) -> Result<()> {
        let content = self.generate();
        fs::write(path, content)?;
        Ok(())
    }
}

//=============================================================================
// 响应文件生成器
//=============================================================================

/// 响应文件生成器 (用于过长的命令行)
pub struct ResponseFileGenerator;

impl ResponseFileGenerator {
    /// 生成响应文件内容
    pub fn generate(args: &[String]) -> String {
        args.iter()
            .map(|arg| {
                // 如果参数包含空格，需要引号
                if arg.contains(' ') || arg.contains('\t') {
                    format!("\"{}\"", arg.replace('\\', "\\\\").replace('"', "\\\""))
                } else {
                    arg.clone()
                }
            })
            .collect::<Vec<_>>()
            .join("\n")
    }

    /// 保存响应文件
    pub fn save(args: &[String], path: &Path) -> Result<()> {
        let content = Self::generate(args);
        fs::write(path, content)?;
        Ok(())
    }

    /// 检查是否需要响应文件 (命令行过长)
    pub fn needs_response_file(args: &[String]) -> bool {
        let total_len: usize = args.iter().map(|a| a.len() + 1).sum();

        // Windows 命令行限制约 32KB，留些余量
        if cfg!(windows) {
            total_len > 30000
        } else {
            // Unix 通常限制更大，但也有限制
            total_len > 100000
        }
    }
}

//=============================================================================
// 链接诊断
//=============================================================================

/// 链接错误类型
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LinkErrorType {
    /// 未定义符号
    UndefinedSymbol,
    /// 重复定义
    DuplicateSymbol,
    /// 无法找到库
    LibraryNotFound,
    /// 无法打开文件
    CannotOpenFile,
    /// 架构不匹配
    ArchitectureMismatch,
    /// 其他错误
    Other,
}

impl LinkErrorType {
    /// 从错误消息推断类型
    pub fn from_message(message: &str) -> Self {
        let lower = message.to_lowercase();

        if lower.contains("undefined") || lower.contains("unresolved") {
            Self::UndefinedSymbol
        } else if lower.contains("duplicate") || lower.contains("multiple definition") {
            Self::DuplicateSymbol
        } else if lower.contains("cannot find") && lower.contains("lib") {
            Self::LibraryNotFound
        } else if lower.contains("cannot open") || lower.contains("no such file") {
            Self::CannotOpenFile
        } else if lower.contains("architecture") || lower.contains("machine type") {
            Self::ArchitectureMismatch
        } else {
            Self::Other
        }
    }

    /// 获取建议修复方法
    pub fn suggestion(&self) -> &'static str {
        match self {
            Self::UndefinedSymbol => "确保所有依赖库已链接，或检查是否遗漏了实现",
            Self::DuplicateSymbol => "检查是否有重复的定义，或使用 weak 符号",
            Self::LibraryNotFound => "检查库路径是否正确，或安装缺失的库",
            Self::CannotOpenFile => "检查文件路径和权限",
            Self::ArchitectureMismatch => "确保所有对象文件使用相同的目标架构编译",
            Self::Other => "检查链接器输出以获取更多信息",
        }
    }
}

/// 解析链接器输出中的未定义符号
pub fn parse_undefined_symbols(output: &str) -> Vec<String> {
    let mut symbols = Vec::new();

    for line in output.lines() {
        let lower = line.to_lowercase();

        // MSVC 格式: "error LNK2019: unresolved external symbol _xxx"
        if lower.contains("unresolved external symbol") {
            if let Some(start) = line.find("symbol") {
                let sym_part = &line[start + 7..];
                let symbol = sym_part
                    .split_whitespace()
                    .next()
                    .map(|s| s.trim_matches(|c| c == '"' || c == '\''))
                    .unwrap_or("");
                if !symbol.is_empty() {
                    symbols.push(symbol.to_string());
                }
            }
        }
        // LLD/GNU 格式: "undefined reference to `xxx'"
        else if lower.contains("undefined reference to") {
            if let Some(start) = line.find('`') {
                if let Some(end) = line[start + 1..].find('\'') {
                    let symbol = &line[start + 1..start + 1 + end];
                    symbols.push(symbol.to_string());
                }
            }
        }
    }

    symbols
}
