/*******************************************************************************
 * 文件: compiler/gcc.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   GCC 编译器实现 - GNU Compiler Collection 支持
 *   - g++/gcc 编译器封装
 *   - GNU ld 链接器支持
 *   - 完整的编译选项
 *
 * 技术特性:
 *   - 支持 GCC 11+
 *   - 支持 PCH 预编译头
 *   - 支持 LTO
 *   - Linux/macOS 平台
 *
 ******************************************************************************/

use anyhow::{Context, Result};
use rayon::prelude::*;
use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::Instant;

use super::{
    CompileResult, CompileUnit, Compiler, CompilerConfig, Diagnostic, DiagnosticSeverity,
    LanguageStandard, LinkResult, LinkUnit, Linker, LinkerConfig, Platform, TargetType,
};

//=============================================================================
// GCC 编译器
//=============================================================================

/// GCC 编译器实现
pub struct GccCompiler {
    /// 编译器路径 (g++)
    gpp_path: PathBuf,
    /// C 编译器路径 (gcc)
    gcc_path: PathBuf,
    /// 版本字符串
    version: String,
    /// 主版本号
    major_version: u32,
    /// 目标三元组
    target_triple: String,
}

impl GccCompiler {
    /// 检测并创建 GCC 编译器
    pub fn detect() -> Result<Self> {
        let gpp_path = which::which("g++").context("无法找到 g++")?;

        let gcc_path = which::which("gcc").unwrap_or_else(|_| gpp_path.clone());

        let output = Command::new(&gpp_path)
            .arg("--version")
            .output()
            .context("执行 g++ --version 失败")?;

        let version_output = String::from_utf8_lossy(&output.stdout);
        let version = Self::parse_version(&version_output);
        let major_version = version
            .split('.')
            .next()
            .and_then(|s| s.parse().ok())
            .unwrap_or(0);

        let target_triple = Self::get_target_triple(&gpp_path)?;

        Ok(Self {
            gpp_path,
            gcc_path,
            version,
            major_version,
            target_triple,
        })
    }

    /// 解析版本字符串
    fn parse_version(output: &str) -> String {
        // "g++ (GCC) 13.2.0"
        for line in output.lines() {
            let parts: Vec<&str> = line.split_whitespace().collect();
            for part in parts {
                if part
                    .chars()
                    .next()
                    .map(|c| c.is_ascii_digit())
                    .unwrap_or(false)
                {
                    let version: String = part
                        .chars()
                        .take_while(|c| c.is_ascii_digit() || *c == '.')
                        .collect();
                    if version.contains('.') {
                        return version;
                    }
                }
            }
        }
        "0.0.0".to_string()
    }

    /// 获取目标三元组
    fn get_target_triple(gpp_path: &Path) -> Result<String> {
        let output = Command::new(gpp_path).arg("-dumpmachine").output()?;
        Ok(String::from_utf8_lossy(&output.stdout).trim().to_string())
    }

    /// 构建编译命令参数
    fn build_compile_args(&self, unit: &CompileUnit, config: &CompilerConfig) -> Vec<String> {
        let mut args = Vec::with_capacity(64);

        args.push("-c".to_string());

        // 语言标准
        args.push(unit.language_standard.as_gcc_flag().to_string());

        // 警告级别
        for flag in unit.warning_level.as_clang_flags() {
            args.push(flag.to_string());
        }
        if unit.warnings_as_errors {
            args.push("-Werror".to_string());
        }

        // 优化级别
        match config.configuration.optimization_level() {
            0 => args.push("-O0".to_string()),
            1 => args.push("-O1".to_string()),
            2 => args.push("-O2".to_string()),
            _ => args.push("-O3".to_string()),
        }

        // 调试信息
        if config.configuration.has_debug_info() {
            args.push("-g".to_string());
            args.push("-ggdb".to_string());
        }

        // 位置无关代码
        args.push("-fPIC".to_string());

        // 异常处理
        if !config.enable_exceptions {
            args.push("-fno-exceptions".to_string());
        }

        // RTTI
        if !config.enable_rtti {
            args.push("-fno-rtti".to_string());
        }

        // 颜色诊断
        args.push("-fdiagnostics-color=always".to_string());

        // 优化相关
        if config.configuration.is_optimized() {
            args.push("-ffunction-sections".to_string());
            args.push("-fdata-sections".to_string());

            if config.enable_lto {
                args.push("-flto".to_string());
            }
        }

        // Sanitizers
        if config.enable_asan {
            args.push("-fsanitize=address".to_string());
        }
        if config.enable_ubsan {
            args.push("-fsanitize=undefined".to_string());
        }

        // 代码覆盖率
        if config.enable_coverage {
            args.push("-fprofile-arcs".to_string());
            args.push("-ftest-coverage".to_string());
        }

        // 包含目录
        for dir in &unit.include_dirs {
            args.push(format!("-I{}", dir.display()));
        }

        // 预处理器定义
        for def in &unit.defines {
            args.push(format!("-D{}", def));
        }

        // 配置定义
        match config.configuration {
            super::BuildConfiguration::Debug => {
                args.push("-D_DEBUG".to_string());
                args.push("-DDEBUG=1".to_string());
            }
            super::BuildConfiguration::Development => {
                args.push("-DDEVELOPMENT=1".to_string());
            }
            super::BuildConfiguration::Release => {
                args.push("-DNDEBUG".to_string());
                args.push("-DRELEASE=1".to_string());
            }
            super::BuildConfiguration::Shipping => {
                args.push("-DNDEBUG".to_string());
                args.push("-DSHIPPING=1".to_string());
            }
            super::BuildConfiguration::Test => {
                args.push("-DTEST=1".to_string());
            }
        }

        // 平台定义
        args.push("-DPLATFORM_LINUX=1".to_string());

        // 依赖文件
        let dep_file = unit.object_file.with_extension("d");
        args.push("-MD".to_string());
        args.push("-MF".to_string());
        args.push(format!("{}", dep_file.display()));

        // 额外选项
        for flag in &unit.extra_flags {
            args.push(flag.clone());
        }

        // 输出文件
        args.push("-o".to_string());
        args.push(format!("{}", unit.object_file.display()));

        // 源文件
        args.push(format!("{}", unit.source_file.display()));

        args
    }

    /// 解析编译器输出
    fn parse_compile_output(&self, output: &str) -> Vec<Diagnostic> {
        let mut diagnostics = Vec::new();

        for line in output.lines() {
            if let Some(diag) = self.parse_diagnostic_line(line) {
                diagnostics.push(diag);
            }
        }

        diagnostics
    }

    fn parse_diagnostic_line(&self, line: &str) -> Option<Diagnostic> {
        // GCC 格式: file:line:column: error/warning: message
        let parts: Vec<&str> = line.splitn(5, ':').collect();

        if parts.len() < 5 {
            return None;
        }

        let file = PathBuf::from(parts[0]);
        let line_num: u32 = parts[1].parse().ok()?;
        let column: u32 = parts[2].parse().ok()?;

        let severity_str = parts[3].trim();
        let severity = match severity_str {
            "error" | "fatal error" => DiagnosticSeverity::Error,
            "warning" => DiagnosticSeverity::Warning,
            "note" => DiagnosticSeverity::Note,
            _ => return None,
        };

        let message = parts[4].trim().to_string();

        Some(Diagnostic {
            severity,
            file: Some(file),
            line: line_num,
            column,
            code: None,
            message,
            source: Some("g++".to_string()),
            related: Vec::new(),
        })
    }
}

impl Compiler for GccCompiler {
    fn name(&self) -> &str {
        "GCC"
    }

    fn version(&self) -> &str {
        &self.version
    }

    fn supported_platforms(&self) -> &[Platform] {
        &[Platform::Linux, Platform::MacOS]
    }

    fn compile(&self, unit: &CompileUnit, config: &CompilerConfig) -> Result<CompileResult> {
        let start = Instant::now();

        if let Some(parent) = unit.object_file.parent() {
            fs::create_dir_all(parent)?;
        }

        let compiler = if unit.is_c_file {
            &self.gcc_path
        } else {
            &self.gpp_path
        };
        let args = self.build_compile_args(unit, config);

        let output = Command::new(compiler)
            .args(&args)
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .output()
            .context("执行 g++ 失败")?;

        let duration_ms = start.elapsed().as_millis() as u64;

        let stderr = String::from_utf8_lossy(&output.stderr);
        let diagnostics = self.parse_compile_output(&stderr);

        if output.status.success() {
            let mut result = CompileResult::success(unit.object_file.clone(), duration_ms);
            result.diagnostics = diagnostics;
            Ok(result)
        } else {
            Ok(CompileResult::failure(diagnostics, duration_ms))
        }
    }

    fn compile_batch(
        &self,
        units: &[CompileUnit],
        config: &CompilerConfig,
    ) -> Vec<Result<CompileResult>> {
        units
            .par_iter()
            .map(|unit| self.compile(unit, config))
            .collect()
    }

    fn create_pch(
        &self,
        header: &Path,
        output: &Path,
        config: &CompilerConfig,
    ) -> Result<CompileResult> {
        let start = Instant::now();

        if let Some(parent) = output.parent() {
            fs::create_dir_all(parent)?;
        }

        let mut args = vec![
            "-x".to_string(),
            "c++-header".to_string(),
            LanguageStandard::Cpp23.as_gcc_flag().to_string(),
            "-o".to_string(),
            format!("{}", output.display()),
            format!("{}", header.display()),
        ];

        if config.configuration.has_debug_info() {
            args.push("-g".to_string());
        }

        let output_result = Command::new(&self.gpp_path)
            .args(&args)
            .output()
            .context("创建 PCH 失败")?;

        let duration_ms = start.elapsed().as_millis() as u64;

        if output_result.status.success() {
            Ok(CompileResult::success(output.to_path_buf(), duration_ms))
        } else {
            let stderr = String::from_utf8_lossy(&output_result.stderr);
            let diagnostics = self.parse_compile_output(&stderr);
            Ok(CompileResult::failure(diagnostics, duration_ms))
        }
    }

    fn generate_dependencies(
        &self,
        unit: &CompileUnit,
        _config: &CompilerConfig,
    ) -> Result<Vec<PathBuf>> {
        let mut args = vec!["-M".to_string(), "-MM".to_string()];

        for dir in &unit.include_dirs {
            args.push(format!("-I{}", dir.display()));
        }

        for def in &unit.defines {
            args.push(format!("-D{}", def));
        }

        args.push(format!("{}", unit.source_file.display()));

        let output = Command::new(&self.gpp_path).args(&args).output()?;

        let stdout = String::from_utf8_lossy(&output.stdout);
        Ok(Self::parse_makefile_deps(&stdout))
    }

    fn default_include_paths(&self) -> Vec<PathBuf> {
        vec![
            PathBuf::from("/usr/include"),
            PathBuf::from("/usr/local/include"),
            PathBuf::from(format!("/usr/include/c++/{}", self.major_version)),
        ]
    }

    fn predefined_macros(&self) -> HashMap<String, String> {
        let mut macros = HashMap::new();
        macros.insert("__GNUC__".to_string(), self.major_version.to_string());
        macros.insert("__cplusplus".to_string(), "202302L".to_string());
        macros
    }
}

impl GccCompiler {
    fn parse_makefile_deps(content: &str) -> Vec<PathBuf> {
        let mut deps = Vec::new();
        let content = content.replace("\\\n", " ");

        if let Some(colon_idx) = content.find(':') {
            let deps_part = &content[colon_idx + 1..];
            for dep in deps_part.split_whitespace() {
                if !dep.is_empty() {
                    deps.push(PathBuf::from(dep));
                }
            }
        }
        deps
    }
}

//=============================================================================
// GNU ld 链接器
//=============================================================================

/// GNU ld 链接器实现
pub struct GnuLinker {
    /// 链接器路径
    ld_path: PathBuf,
    /// 归档工具路径
    ar_path: PathBuf,
}

impl GnuLinker {
    /// 检测 GNU ld
    pub fn detect() -> Result<Self> {
        let ld_path = which::which("ld").context("无法找到 ld")?;
        let ar_path = which::which("ar").unwrap_or_else(|_| PathBuf::from("/usr/bin/ar"));

        Ok(Self { ld_path, ar_path })
    }

    fn build_link_args(&self, unit: &LinkUnit, config: &LinkerConfig) -> Vec<String> {
        let mut args = Vec::with_capacity(32);

        match unit.target_type {
            TargetType::DynamicLibrary => {
                args.push("-shared".to_string());
            }
            TargetType::Executable => {
                args.push("-pie".to_string());
            }
            _ => {}
        }

        args.push("-o".to_string());
        args.push(format!("{}", unit.output_file.display()));

        if config.configuration.is_optimized() {
            args.push("--gc-sections".to_string());
        }

        if config.enable_lto {
            args.push("-flto".to_string());
        }

        for dir in &unit.lib_dirs {
            args.push(format!("-L{}", dir.display()));
        }

        args.push("-lstdc++".to_string());
        args.push("-lm".to_string());
        args.push("-lpthread".to_string());
        args.push("-ldl".to_string());

        for lib in &unit.static_libs {
            args.push(format!("{}", lib.display()));
        }

        for lib in &unit.dynamic_libs {
            args.push(format!("-l{}", lib));
        }

        for obj in &unit.object_files {
            args.push(format!("{}", obj.display()));
        }

        args
    }
}

impl Linker for GnuLinker {
    fn name(&self) -> &str {
        "GNU ld"
    }

    fn link(&self, unit: &LinkUnit, config: &LinkerConfig) -> Result<LinkResult> {
        let start = Instant::now();

        if let Some(parent) = unit.output_file.parent() {
            fs::create_dir_all(parent)?;
        }

        // 使用 g++ 作为前端来链接 (处理 C++ 运行时)
        let gpp = which::which("g++").unwrap_or_else(|_| PathBuf::from("g++"));
        let args = self.build_link_args(unit, config);

        let output = Command::new(gpp)
            .args(&args)
            .output()
            .context("执行链接失败")?;

        let duration_ms = start.elapsed().as_millis() as u64;

        if output.status.success() {
            Ok(LinkResult::success(unit.output_file.clone(), duration_ms))
        } else {
            let stderr = String::from_utf8_lossy(&output.stderr);
            let diag = Diagnostic {
                severity: DiagnosticSeverity::Error,
                file: None,
                line: 0,
                column: 0,
                code: None,
                message: stderr.to_string(),
                source: Some("ld".to_string()),
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

        let mut args = vec!["rcs".to_string(), format!("{}", output.display())];

        for obj in objects {
            args.push(format!("{}", obj.display()));
        }

        let output_result = Command::new(&self.ar_path)
            .args(&args)
            .output()
            .context("执行 ar 失败")?;

        let duration_ms = start.elapsed().as_millis() as u64;

        if output_result.status.success() {
            Ok(LinkResult::success(output.to_path_buf(), duration_ms))
        } else {
            let stderr = String::from_utf8_lossy(&output_result.stderr);
            let diag = Diagnostic {
                severity: DiagnosticSeverity::Error,
                file: None,
                line: 0,
                column: 0,
                code: None,
                message: stderr.to_string(),
                source: Some("ar".to_string()),
                related: Vec::new(),
            };
            Ok(LinkResult::failure(vec![diag], duration_ms))
        }
    }

    fn default_lib_paths(&self) -> Vec<PathBuf> {
        vec![
            PathBuf::from("/usr/lib"),
            PathBuf::from("/usr/lib/x86_64-linux-gnu"),
            PathBuf::from("/usr/local/lib"),
        ]
    }
}
