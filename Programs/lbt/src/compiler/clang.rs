/*******************************************************************************
 * 文件: compiler/clang.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   Clang/LLVM 编译器实现 - 跨平台 C++ 编译支持
 *   - clang/clang++ 编译器封装
 *   - lld 链接器支持
 *   - 完整的编译选项
 *   - 诊断信息解析
 *
 * 技术特性:
 *   - 支持 Clang 15+
 *   - 支持 PCH 预编译头
 *   - 支持 LTO (Link Time Optimization)
 *   - 支持 ASAN/UBSAN/TSAN
 *   - 跨平台 (Windows/Linux/macOS)
 *
 ******************************************************************************/

use anyhow::{anyhow, Context, Result};
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
// Clang 编译器
//=============================================================================

/// Clang 编译器实现
pub struct ClangCompiler {
    /// 编译器路径
    clang_path: PathBuf,
    /// 版本字符串
    version: String,
    /// 主版本号
    major_version: u32,
    /// 安装目录
    install_dir: PathBuf,
    /// 目标三元组
    target_triple: String,
    /// 是否为 Apple Clang
    is_apple_clang: bool,
}

impl ClangCompiler {
    /// 检测并创建 Clang 编译器
    pub fn detect() -> Result<Self> {
        // 查找 clang++
        let clang_path = Self::find_clang()?;

        // 获取版本信息
        let output = Command::new(&clang_path)
            .arg("--version")
            .output()
            .context("执行 clang --version 失败")?;

        let version_output = String::from_utf8_lossy(&output.stdout);
        let (version, is_apple) = Self::parse_version(&version_output);
        let major_version = version
            .split('.')
            .next()
            .and_then(|s| s.parse().ok())
            .unwrap_or(0);

        let install_dir = clang_path
            .parent()
            .and_then(|p| p.parent())
            .unwrap_or(Path::new("/usr"))
            .to_path_buf();

        // 获取目标三元组
        let target_triple = Self::get_target_triple(&clang_path)?;

        Ok(Self {
            clang_path,
            version,
            major_version,
            install_dir,
            target_triple,
            is_apple_clang: is_apple,
        })
    }

    /// 从指定路径创建
    pub fn from_path(clang_path: PathBuf) -> Result<Self> {
        if !clang_path.exists() {
            return Err(anyhow!("clang 不存在: {:?}", clang_path));
        }

        let output = Command::new(&clang_path).arg("--version").output()?;

        let version_output = String::from_utf8_lossy(&output.stdout);
        let (version, is_apple) = Self::parse_version(&version_output);
        let major_version = version
            .split('.')
            .next()
            .and_then(|s| s.parse().ok())
            .unwrap_or(0);

        let install_dir = clang_path
            .parent()
            .and_then(|p| p.parent())
            .unwrap_or(Path::new("/usr"))
            .to_path_buf();

        let target_triple = Self::get_target_triple(&clang_path)?;

        Ok(Self {
            clang_path,
            version,
            major_version,
            install_dir,
            target_triple,
            is_apple_clang: is_apple,
        })
    }

    /// 查找 Clang 编译器
    fn find_clang() -> Result<PathBuf> {
        // 尝试 clang++ 优先
        if let Ok(path) = which::which("clang++") {
            return Ok(path);
        }

        // 尝试 clang
        if let Ok(path) = which::which("clang") {
            return Ok(path);
        }

        // Windows 特定路径
        #[cfg(target_os = "windows")]
        {
            let paths = [
                r"C:\Program Files\LLVM\bin\clang++.exe",
                r"C:\Program Files (x86)\LLVM\bin\clang++.exe",
            ];
            for path in &paths {
                let p = PathBuf::from(path);
                if p.exists() {
                    return Ok(p);
                }
            }
        }

        Err(anyhow!("无法找到 Clang 编译器"))
    }

    /// 解析版本字符串
    fn parse_version(output: &str) -> (String, bool) {
        let is_apple = output.contains("Apple");

        // 查找版本号
        for line in output.lines() {
            if let Some(pos) = line.find("version ") {
                let version_part = &line[pos + 8..];
                let version: String = version_part
                    .chars()
                    .take_while(|c| c.is_ascii_digit() || *c == '.')
                    .collect();
                if !version.is_empty() {
                    return (version, is_apple);
                }
            }
        }

        ("0.0.0".to_string(), is_apple)
    }

    /// 获取目标三元组
    fn get_target_triple(clang_path: &Path) -> Result<String> {
        let output = Command::new(clang_path).arg("-dumpmachine").output()?;

        Ok(String::from_utf8_lossy(&output.stdout).trim().to_string())
    }

    /// 构建编译命令参数
    fn build_compile_args(&self, unit: &CompileUnit, config: &CompilerConfig) -> Vec<String> {
        let mut args = Vec::with_capacity(64);

        // 基本选项
        args.push("-c".to_string());

        // 语言标准
        if unit.is_c_file {
            args.push(unit.language_standard.as_clang_flag().to_string());
        } else {
            args.push("-x".to_string());
            args.push("c++".to_string());
            args.push(unit.language_standard.as_clang_flag().to_string());
        }

        // 目标三元组
        args.push(format!("--target={}", self.target_triple));

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
            args.push("-glldb".to_string()); // LLDB 友好的调试信息
        }

        // 位置无关代码 (共享库需要)
        args.push("-fPIC".to_string());

        // 异常处理
        if config.enable_exceptions {
            args.push("-fexceptions".to_string());
            args.push("-fcxx-exceptions".to_string());
        } else {
            args.push("-fno-exceptions".to_string());
        }

        // RTTI
        if !config.enable_rtti {
            args.push("-fno-rtti".to_string());
        }

        // 颜色诊断
        args.push("-fcolor-diagnostics".to_string());

        // 优化相关
        if config.configuration.is_optimized() {
            args.push("-ffunction-sections".to_string());
            args.push("-fdata-sections".to_string());
            args.push("-fomit-frame-pointer".to_string());

            if config.enable_lto {
                args.push("-flto=thin".to_string());
            }
        }

        // Sanitizers
        if config.enable_asan {
            args.push("-fsanitize=address".to_string());
            args.push("-fno-omit-frame-pointer".to_string());
        }
        if config.enable_ubsan {
            args.push("-fsanitize=undefined".to_string());
        }

        // 代码覆盖率
        if config.enable_coverage {
            args.push("-fprofile-instr-generate".to_string());
            args.push("-fcoverage-mapping".to_string());
        }

        // 预编译头
        if let Some(ref pch) = unit.use_pch {
            args.push("-include-pch".to_string());
            args.push(format!("{}", pch.display()));
        }

        // 强制包含
        for include in &unit.force_includes {
            args.push("-include".to_string());
            args.push(format!("{}", include.display()));
        }

        // 包含目录
        for dir in &unit.include_dirs {
            args.push(format!("-I{}", dir.display()));
        }

        // 预处理器定义
        for def in &unit.defines {
            args.push(format!("-D{}", def));
        }

        // 配置相关定义
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
        match config.platform {
            Platform::Windows => {
                args.push("-DPLATFORM_WINDOWS=1".to_string());
                args.push("-D_WIN32".to_string());
                args.push("-D_WIN64".to_string());
                args.push("-DUNICODE".to_string());
                args.push("-D_UNICODE".to_string());
            }
            Platform::Linux => {
                args.push("-DPLATFORM_LINUX=1".to_string());
                args.push("-D__linux__".to_string());
            }
            Platform::MacOS => {
                args.push("-DPLATFORM_MAC=1".to_string());
                args.push("-D__APPLE__".to_string());
            }
        }

        // 额外选项
        for flag in &unit.extra_flags {
            args.push(flag.clone());
        }

        // 依赖文件生成
        let dep_file = unit.object_file.with_extension("d");
        args.push("-MD".to_string());
        args.push("-MF".to_string());
        args.push(format!("{}", dep_file.display()));

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

    /// 解析诊断行
    fn parse_diagnostic_line(&self, line: &str) -> Option<Diagnostic> {
        // Clang 格式: file:line:column: error/warning: message
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

        // 提取错误码 (如果有)
        let code = if message.starts_with('[') {
            message.find(']').map(|i| message[1..i].to_string())
        } else {
            None
        };

        Some(Diagnostic {
            severity,
            file: Some(file),
            line: line_num,
            column,
            code,
            message,
            source: Some("clang".to_string()),
            related: Vec::new(),
        })
    }
}

impl Compiler for ClangCompiler {
    fn name(&self) -> &str {
        if self.is_apple_clang {
            "Apple Clang"
        } else {
            "Clang"
        }
    }

    fn version(&self) -> &str {
        &self.version
    }

    fn supported_platforms(&self) -> &[Platform] {
        &[Platform::Windows, Platform::Linux, Platform::MacOS]
    }

    fn compile(&self, unit: &CompileUnit, config: &CompilerConfig) -> Result<CompileResult> {
        let start = Instant::now();

        // 确保输出目录存在
        if let Some(parent) = unit.object_file.parent() {
            fs::create_dir_all(parent)?;
        }

        let args = self.build_compile_args(unit, config);

        let output = Command::new(&self.clang_path)
            .args(&args)
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .output()
            .context("执行 clang 失败")?;

        let duration_ms = start.elapsed().as_millis() as u64;

        let stderr = String::from_utf8_lossy(&output.stderr);
        let diagnostics = self.parse_compile_output(&stderr);

        // 解析依赖文件
        let dep_file = unit.object_file.with_extension("d");
        let dependencies = Self::parse_dependency_file(&dep_file).unwrap_or_default();

        if output.status.success() {
            let mut result = CompileResult::success(unit.object_file.clone(), duration_ms);
            result.diagnostics = diagnostics;
            result.dependencies = dependencies;
            Ok(result)
        } else {
            let mut result = CompileResult::failure(diagnostics, duration_ms);
            result.dependencies = dependencies;
            Ok(result)
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
            LanguageStandard::Cpp23.as_clang_flag().to_string(),
            "-o".to_string(),
            format!("{}", output.display()),
            format!("{}", header.display()),
        ];

        if config.configuration.has_debug_info() {
            args.push("-g".to_string());
        }

        let output_result = Command::new(&self.clang_path)
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
        config: &CompilerConfig,
    ) -> Result<Vec<PathBuf>> {
        let mut args = vec![
            "-M".to_string(),
            "-MM".to_string(), // 不包含系统头文件
        ];

        for dir in &unit.include_dirs {
            args.push(format!("-I{}", dir.display()));
        }

        for def in &unit.defines {
            args.push(format!("-D{}", def));
        }

        args.push(format!("{}", unit.source_file.display()));

        let output = Command::new(&self.clang_path)
            .args(&args)
            .output()
            .context("生成依赖失败")?;

        let stdout = String::from_utf8_lossy(&output.stdout);
        Ok(Self::parse_makefile_deps(&stdout))
    }

    fn default_include_paths(&self) -> Vec<PathBuf> {
        vec![
            self.install_dir.join("include"),
            self.install_dir
                .join("lib/clang")
                .join(&self.version)
                .join("include"),
        ]
    }

    fn predefined_macros(&self) -> HashMap<String, String> {
        let mut macros = HashMap::new();
        macros.insert("__clang__".to_string(), "1".to_string());
        macros.insert(
            "__clang_major__".to_string(),
            self.major_version.to_string(),
        );
        macros.insert("__cplusplus".to_string(), "202302L".to_string());
        macros
    }
}

impl ClangCompiler {
    /// 解析依赖文件 (.d 文件)
    fn parse_dependency_file(dep_file: &Path) -> Result<Vec<PathBuf>> {
        if !dep_file.exists() {
            return Ok(Vec::new());
        }

        let content = fs::read_to_string(dep_file)?;
        Ok(Self::parse_makefile_deps(&content))
    }

    /// 解析 Makefile 格式的依赖
    fn parse_makefile_deps(content: &str) -> Vec<PathBuf> {
        let mut deps = Vec::new();

        // 移除反斜杠续行
        let content = content.replace("\\\n", " ");

        // 跳过目标部分 (target: deps)
        if let Some(colon_idx) = content.find(':') {
            let deps_part = &content[colon_idx + 1..];

            for dep in deps_part.split_whitespace() {
                let dep = dep.trim();
                if !dep.is_empty() {
                    deps.push(PathBuf::from(dep));
                }
            }
        }

        deps
    }
}

//=============================================================================
// LLD 链接器 (LLVM)
//=============================================================================

/// LLD 链接器实现
pub struct LldLinker {
    /// 链接器路径
    lld_path: PathBuf,
    /// 归档工具路径
    ar_path: PathBuf,
    /// 目标平台
    platform: Platform,
}

impl LldLinker {
    /// 检测 LLD 链接器
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

    /// 构建链接参数
    fn build_link_args(&self, unit: &LinkUnit, config: &LinkerConfig) -> Vec<String> {
        let mut args = Vec::with_capacity(32);

        match self.platform {
            Platform::Windows => {
                self.build_windows_link_args(&mut args, unit, config);
            }
            Platform::Linux | Platform::MacOS => {
                self.build_unix_link_args(&mut args, unit, config);
            }
        }

        args
    }

    fn build_windows_link_args(
        &self,
        args: &mut Vec<String>,
        unit: &LinkUnit,
        config: &LinkerConfig,
    ) {
        args.push("/NOLOGO".to_string());

        match unit.target_type {
            TargetType::DynamicLibrary => {
                args.push("/DLL".to_string());
            }
            _ => {}
        }

        args.push(format!("/OUT:{}", unit.output_file.display()));

        if config.debug_info {
            args.push("/DEBUG".to_string());
        }

        if config.enable_lto {
            args.push("/LTCG".to_string());
        }

        for dir in &unit.lib_dirs {
            args.push(format!("/LIBPATH:{}", dir.display()));
        }

        for lib in &unit.static_libs {
            args.push(format!("{}", lib.display()));
        }

        for lib in &unit.dynamic_libs {
            args.push(format!("{}.lib", lib));
        }

        for obj in &unit.object_files {
            args.push(format!("{}", obj.display()));
        }
    }

    fn build_unix_link_args(&self, args: &mut Vec<String>, unit: &LinkUnit, config: &LinkerConfig) {
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
            args.push("--lto=thin".to_string());
        }

        for dir in &unit.lib_dirs {
            args.push(format!("-L{}", dir.display()));
        }

        // 标准库
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

        let args = self.build_link_args(unit, config);

        let output = Command::new(&self.lld_path)
            .args(&args)
            .output()
            .context("执行 lld 失败")?;

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
        match self.platform {
            Platform::Windows => vec![],
            _ => vec![PathBuf::from("/usr/lib"), PathBuf::from("/usr/local/lib")],
        }
    }
}
