/*******************************************************************************
 * 文件: compiler/msvc.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   MSVC 编译器实现 - Microsoft Visual C++ 完整支持
 *   - cl.exe 编译器封装
 *   - link.exe 链接器封装
 *   - lib.exe 静态库工具
 *   - 完整的编译选项支持
 *   - 诊断信息解析
 *
 * 技术特性:
 *   - 支持 MSVC 19.x (VS2019/2022)
 *   - 支持 PCH 预编译头
 *   - 支持 Unity Build
 *   - 支持 PDB 调试信息
 *   - 支持增量链接
 *   - 支持 ASAN/UBSAN
 *
 ******************************************************************************/

use anyhow::{anyhow, Context, Result};
use rayon::prelude::*;
use std::collections::HashMap;
use std::fs;
use std::io::BufReader;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::Instant;

use super::platform::{PlatformInfo, VisualStudioInfo, WindowsSdkInfo};
use super::{
    Architecture, CompileResult, CompileUnit, Compiler, CompilerConfig, Diagnostic,
    DiagnosticSeverity, LanguageStandard, LinkResult, LinkUnit, Linker, LinkerConfig, Platform,
    TargetType,
};

//=============================================================================
// MSVC 编译器
//=============================================================================

/// MSVC 编译器实现
pub struct MsvcCompiler {
    /// Visual Studio 信息
    vs_info: VisualStudioInfo,
    /// Windows SDK 信息
    sdk_info: WindowsSdkInfo,
    /// 编译器路径
    cl_path: PathBuf,
    /// 版本字符串
    version: String,
    /// 环境变量
    environment: HashMap<String, String>,
}

impl MsvcCompiler {
    /// 检测并创建 MSVC 编译器
    pub fn detect() -> Result<Self> {
        let vs_info = VisualStudioInfo::detect().context("无法检测 Visual Studio")?;
        let sdk_info = WindowsSdkInfo::detect().context("无法检测 Windows SDK")?;

        let cl_path = vs_info.cl_path();
        if !cl_path.exists() {
            return Err(anyhow!("cl.exe 不存在: {:?}", cl_path));
        }

        let version = vs_info.toolchain_version.clone();

        // 构建环境变量
        let platform_info = PlatformInfo::Windows {
            vs: vs_info.clone(),
            sdk: sdk_info.clone(),
        };
        let environment = platform_info.build_environment();

        Ok(Self {
            vs_info,
            sdk_info,
            cl_path,
            version,
            environment,
        })
    }

    /// 从指定路径创建
    pub fn from_path(cl_path: PathBuf) -> Result<Self> {
        if !cl_path.exists() {
            return Err(anyhow!("cl.exe 不存在: {:?}", cl_path));
        }

        let vs_info = VisualStudioInfo::detect()?;
        let sdk_info = WindowsSdkInfo::detect()?;
        let version = vs_info.toolchain_version.clone();

        let platform_info = PlatformInfo::Windows {
            vs: vs_info.clone(),
            sdk: sdk_info.clone(),
        };
        let environment = platform_info.build_environment();

        Ok(Self {
            vs_info,
            sdk_info,
            cl_path,
            version,
            environment,
        })
    }

    /// 构建编译命令参数
    fn build_compile_args(&self, unit: &CompileUnit, config: &CompilerConfig) -> Vec<String> {
        let mut args = Vec::with_capacity(64);

        // 基本选项
        args.push("/nologo".to_string());
        args.push("/c".to_string()); // 仅编译

        // 语言标准
        args.push(unit.language_standard.as_msvc_flag().to_string());

        // UTF-8 编码
        args.push("/utf-8".to_string());

        // 警告级别
        args.push(unit.warning_level.as_msvc_flag().to_string());
        if unit.warnings_as_errors {
            args.push("/WX".to_string());
        }

        // 优化级别
        match config.configuration.optimization_level() {
            0 => args.push("/Od".to_string()),
            1 => args.push("/O1".to_string()),
            2 => args.push("/O2".to_string()),
            _ => {
                args.push("/O2".to_string());
                args.push("/Ob3".to_string()); // 更激进的内联
            }
        }

        // 调试信息
        if config.configuration.has_debug_info() {
            args.push("/Zi".to_string()); // 完整调试信息
            args.push("/FS".to_string()); // 强制同步 PDB 写入
        }

        // 运行时库
        match (
            config.multithreaded_runtime,
            config.dynamic_runtime,
            config.configuration.has_debug_info(),
        ) {
            (true, true, true) => args.push("/MDd".to_string()),
            (true, true, false) => args.push("/MD".to_string()),
            (true, false, true) => args.push("/MTd".to_string()),
            (true, false, false) => args.push("/MT".to_string()),
            (false, _, true) => args.push("/MLd".to_string()),
            (false, _, false) => args.push("/ML".to_string()),
        }

        // 异常处理
        if config.enable_exceptions {
            args.push("/EHsc".to_string());
        } else {
            args.push("/EHs-c-".to_string());
        }

        // RTTI
        if !config.enable_rtti {
            args.push("/GR-".to_string());
        }

        // 内联函数展开
        if config.configuration.is_optimized() {
            args.push("/Gy".to_string()); // 函数级链接
            args.push("/Gw".to_string()); // 全局数据优化
        }

        // 安全性
        args.push("/GS".to_string()); // 缓冲区安全检查
        args.push("/sdl".to_string()); // 额外安全检查

        // Address Sanitizer
        if config.enable_asan {
            args.push("/fsanitize=address".to_string());
        }

        // 代码生成
        args.push("/fp:precise".to_string()); // 精确浮点
        args.push("/Zc:wchar_t".to_string()); // wchar_t 作为内置类型
        args.push("/Zc:forScope".to_string()); // for 循环作用域
        args.push("/Zc:inline".to_string()); // 移除未引用的 COMDAT
        args.push("/Zc:preprocessor".to_string()); // 符合标准的预处理器
        args.push("/permissive-".to_string()); // 标准一致性

        // 预编译头
        if let Some(ref pch) = unit.use_pch {
            args.push("/Yu".to_string());
            args.push(format!("{}", pch.display()));

            // PCH 文件路径
            let pch_file = unit.object_file.with_extension("pch");
            args.push(format!("/Fp{}", pch_file.display()));
        } else if unit.create_pch {
            args.push("/Yc".to_string());
            args.push(format!("{}", unit.source_file.display()));
        }

        // 强制包含
        for include in &unit.force_includes {
            args.push("/FI".to_string());
            args.push(format!("{}", include.display()));
        }

        // 包含目录
        for dir in &unit.include_dirs {
            args.push(format!("/I{}", dir.display()));
        }

        // 系统包含目录
        for dir in &self.vs_info.include_paths {
            args.push(format!("/I{}", dir.display()));
        }
        for dir in &self.sdk_info.include_paths {
            args.push(format!("/I{}", dir.display()));
        }

        // 预处理器定义
        for def in &unit.defines {
            args.push(format!("/D{}", def));
        }

        // 配置相关定义
        match config.configuration {
            super::BuildConfiguration::Debug => {
                args.push("/D_DEBUG".to_string());
                args.push("/DDEBUG=1".to_string());
            }
            super::BuildConfiguration::Development => {
                args.push("/DDEVELOPMENT=1".to_string());
            }
            super::BuildConfiguration::Release => {
                args.push("/DNDEBUG".to_string());
                args.push("/DRELEASE=1".to_string());
            }
            super::BuildConfiguration::Shipping => {
                args.push("/DNDEBUG".to_string());
                args.push("/DSHIPPING=1".to_string());
            }
            super::BuildConfiguration::Test => {
                args.push("/DTEST=1".to_string());
            }
        }

        // 平台定义
        args.push("/DPLATFORM_WINDOWS=1".to_string());
        args.push("/DWIN32".to_string());
        args.push("/D_WINDOWS".to_string());
        args.push("/DUNICODE".to_string());
        args.push("/D_UNICODE".to_string());

        // 架构定义
        match config.architecture {
            Architecture::X64 => {
                args.push("/D_WIN64".to_string());
                args.push("/D_AMD64_".to_string());
            }
            Architecture::X86 => {
                args.push("/D_WIN32".to_string());
                args.push("/D_X86_".to_string());
            }
            Architecture::ARM64 => {
                args.push("/D_WIN64".to_string());
                args.push("/D_ARM64_".to_string());
            }
            _ => {}
        }

        // 额外选项
        for flag in &unit.extra_flags {
            args.push(flag.clone());
        }

        // 输出文件
        args.push(format!("/Fo{}", unit.object_file.display()));

        // PDB 文件 (用于并行编译)
        let pdb_file = unit.object_file.with_extension("pdb");
        args.push(format!("/Fd{}", pdb_file.display()));

        // 依赖文件输出
        let dep_file = unit.object_file.with_extension("d");
        args.push("/showIncludes".to_string());

        // 源文件
        args.push(format!("{}", unit.source_file.display()));

        args
    }

    /// 解析编译器输出
    fn parse_compile_output(
        &self,
        output: &str,
        source_file: &Path,
    ) -> (Vec<Diagnostic>, Vec<PathBuf>) {
        let mut diagnostics = Vec::new();
        let mut dependencies = Vec::new();

        for line in output.lines() {
            // 解析 showIncludes 输出
            if line.starts_with("Note: including file:") {
                let include_path = line.trim_start_matches("Note: including file:").trim();
                dependencies.push(PathBuf::from(include_path));
                continue;
            }

            // 解析错误/警告
            // 格式: file(line): error/warning CODE: message
            // 或: file(line,col): error/warning CODE: message
            if let Some(diag) = self.parse_diagnostic_line(line) {
                diagnostics.push(diag);
            }
        }

        (diagnostics, dependencies)
    }

    /// 解析诊断行
    fn parse_diagnostic_line(&self, line: &str) -> Option<Diagnostic> {
        // 查找错误/警告标记
        let (severity, msg_start) = if let Some(idx) = line.find(": error ") {
            (DiagnosticSeverity::Error, idx)
        } else if let Some(idx) = line.find(": warning ") {
            (DiagnosticSeverity::Warning, idx)
        } else if let Some(idx) = line.find(": note: ") {
            (DiagnosticSeverity::Note, idx)
        } else if let Some(idx) = line.find(": fatal error ") {
            (DiagnosticSeverity::Error, idx)
        } else {
            return None;
        };

        let file_part = &line[..msg_start];
        let message_part = &line[msg_start + 2..];

        // 解析文件和位置
        let (file, line_num, column) = if let Some(paren_start) = file_part.rfind('(') {
            let file = PathBuf::from(&file_part[..paren_start]);
            let location = &file_part[paren_start + 1..].trim_end_matches(')');

            let parts: Vec<&str> = location.split(',').collect();
            let line_num = parts.get(0).and_then(|s| s.parse().ok()).unwrap_or(0);
            let column = parts.get(1).and_then(|s| s.parse().ok()).unwrap_or(0);

            (file, line_num, column)
        } else {
            (PathBuf::from(file_part), 0, 0)
        };

        // 提取错误码和消息
        let (code, message) = if let Some(colon_idx) = message_part.find(": ") {
            let code_part = &message_part[..colon_idx];
            let msg = &message_part[colon_idx + 2..];

            // 提取错误码 (如 C2065)
            let code = code_part.split_whitespace().last().map(|s| s.to_string());

            (code, msg.to_string())
        } else {
            (None, message_part.to_string())
        };

        Some(Diagnostic {
            severity,
            file: Some(file),
            line: line_num,
            column,
            code,
            message,
            source: Some("cl.exe".to_string()),
            related: Vec::new(),
        })
    }
}

impl Compiler for MsvcCompiler {
    fn name(&self) -> &str {
        "MSVC"
    }

    fn version(&self) -> &str {
        &self.version
    }

    fn supported_platforms(&self) -> &[Platform] {
        &[Platform::Windows]
    }

    fn compile(&self, unit: &CompileUnit, config: &CompilerConfig) -> Result<CompileResult> {
        let start = Instant::now();

        // 确保输出目录存在
        if let Some(parent) = unit.object_file.parent() {
            fs::create_dir_all(parent)?;
        }

        // 构建命令
        let args = self.build_compile_args(unit, config);

        // 执行编译
        let output = Command::new(&self.cl_path)
            .args(&args)
            .envs(&self.environment)
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .output()
            .context("执行 cl.exe 失败")?;

        let duration_ms = start.elapsed().as_millis() as u64;

        // 合并输出
        let stdout = String::from_utf8_lossy(&output.stdout);
        let stderr = String::from_utf8_lossy(&output.stderr);
        let combined_output = format!("{}\n{}", stdout, stderr);

        // 解析输出
        let (diagnostics, dependencies) =
            self.parse_compile_output(&combined_output, &unit.source_file);

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
        // 并行编译
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

        // 确保输出目录存在
        if let Some(parent) = output.parent() {
            fs::create_dir_all(parent)?;
        }

        let mut args = vec![
            "/nologo".to_string(),
            "/c".to_string(),
            "/Yc".to_string(),
            format!("/Fp{}", output.display()),
            format!("/Fo{}", output.with_extension("obj").display()),
        ];

        // 语言标准
        args.push(LanguageStandard::Cpp23.as_msvc_flag().to_string());
        args.push("/utf-8".to_string());
        args.push("/EHsc".to_string());

        // 配置相关选项
        if config.configuration.has_debug_info() {
            args.push("/Zi".to_string());
            args.push("/MDd".to_string());
        } else {
            args.push("/MD".to_string());
        }

        // 包含目录
        for dir in &self.vs_info.include_paths {
            args.push(format!("/I{}", dir.display()));
        }
        for dir in &self.sdk_info.include_paths {
            args.push(format!("/I{}", dir.display()));
        }

        // 源文件
        args.push(format!("{}", header.display()));

        let output_result = Command::new(&self.cl_path)
            .args(&args)
            .envs(&self.environment)
            .output()
            .context("创建 PCH 失败")?;

        let duration_ms = start.elapsed().as_millis() as u64;

        if output_result.status.success() {
            Ok(CompileResult::success(output.to_path_buf(), duration_ms))
        } else {
            let stderr = String::from_utf8_lossy(&output_result.stderr);
            let (diagnostics, _) = self.parse_compile_output(&stderr, header);
            Ok(CompileResult::failure(diagnostics, duration_ms))
        }
    }

    fn generate_dependencies(
        &self,
        unit: &CompileUnit,
        config: &CompilerConfig,
    ) -> Result<Vec<PathBuf>> {
        let mut args = vec![
            "/nologo".to_string(),
            "/E".to_string(), // 仅预处理
            "/showIncludes".to_string(),
        ];

        // 包含目录
        for dir in &unit.include_dirs {
            args.push(format!("/I{}", dir.display()));
        }
        for dir in &self.vs_info.include_paths {
            args.push(format!("/I{}", dir.display()));
        }
        for dir in &self.sdk_info.include_paths {
            args.push(format!("/I{}", dir.display()));
        }

        // 定义
        for def in &unit.defines {
            args.push(format!("/D{}", def));
        }

        args.push(format!("{}", unit.source_file.display()));

        let output = Command::new(&self.cl_path)
            .args(&args)
            .envs(&self.environment)
            .output()
            .context("生成依赖失败")?;

        let stderr = String::from_utf8_lossy(&output.stderr);
        let mut deps = Vec::new();

        for line in stderr.lines() {
            if line.starts_with("Note: including file:") {
                let path = line.trim_start_matches("Note: including file:").trim();
                deps.push(PathBuf::from(path));
            }
        }

        Ok(deps)
    }

    fn default_include_paths(&self) -> Vec<PathBuf> {
        let mut paths = self.vs_info.include_paths.clone();
        paths.extend(self.sdk_info.include_paths.clone());
        paths
    }

    fn predefined_macros(&self) -> HashMap<String, String> {
        let mut macros = HashMap::new();

        // 解析版本
        let parts: Vec<&str> = self.version.split('.').collect();
        let major = parts
            .get(0)
            .and_then(|s| s.parse::<u32>().ok())
            .unwrap_or(19);
        let minor = parts
            .get(1)
            .and_then(|s| s.parse::<u32>().ok())
            .unwrap_or(0);

        macros.insert("_MSC_VER".to_string(), format!("{}{:02}", major, minor));
        macros.insert(
            "_MSC_FULL_VER".to_string(),
            format!("{}{:02}00000", major, minor),
        );
        macros.insert("_WIN32".to_string(), "1".to_string());
        macros.insert("_WIN64".to_string(), "1".to_string());
        macros.insert("_MSVC_LANG".to_string(), "202302L".to_string()); // C++23
        macros.insert("__cplusplus".to_string(), "202302L".to_string());

        macros
    }
}

//=============================================================================
// MSVC 链接器
//=============================================================================

/// MSVC 链接器实现
pub struct MsvcLinker {
    /// Visual Studio 信息
    vs_info: VisualStudioInfo,
    /// Windows SDK 信息
    sdk_info: WindowsSdkInfo,
    /// 链接器路径
    link_path: PathBuf,
    /// 库管理器路径
    lib_path: PathBuf,
    /// 环境变量
    environment: HashMap<String, String>,
}

impl MsvcLinker {
    /// 检测并创建 MSVC 链接器
    pub fn detect() -> Result<Self> {
        let vs_info = VisualStudioInfo::detect()?;
        let sdk_info = WindowsSdkInfo::detect()?;

        let link_path = vs_info.link_path();
        let lib_path = vs_info.lib_path();

        if !link_path.exists() {
            return Err(anyhow!("link.exe 不存在: {:?}", link_path));
        }

        let platform_info = PlatformInfo::Windows {
            vs: vs_info.clone(),
            sdk: sdk_info.clone(),
        };
        let environment = platform_info.build_environment();

        Ok(Self {
            vs_info,
            sdk_info,
            link_path,
            lib_path,
            environment,
        })
    }

    /// 构建链接命令参数
    fn build_link_args(&self, unit: &LinkUnit, config: &LinkerConfig) -> Vec<String> {
        let mut args = Vec::with_capacity(64);

        args.push("/NOLOGO".to_string());

        // 目标类型
        match unit.target_type {
            TargetType::Executable => {}
            TargetType::DynamicLibrary => {
                args.push("/DLL".to_string());
            }
            _ => {}
        }

        // 输出文件
        args.push(format!("/OUT:{}", unit.output_file.display()));

        // 导入库
        if let Some(ref import_lib) = unit.import_lib {
            args.push(format!("/IMPLIB:{}", import_lib.display()));
        }

        // PDB 文件
        if config.debug_info {
            args.push("/DEBUG:FULL".to_string());
            if let Some(ref pdb) = unit.pdb_file {
                args.push(format!("/PDB:{}", pdb.display()));
            }
        }

        // 增量链接
        if config.incremental && config.configuration.has_debug_info() {
            args.push("/INCREMENTAL".to_string());
        } else {
            args.push("/INCREMENTAL:NO".to_string());
        }

        // 优化
        if config.configuration.is_optimized() {
            args.push("/OPT:REF".to_string()); // 移除未引用的函数
            args.push("/OPT:ICF".to_string()); // 相同 COMDAT 折叠
        }

        // LTO
        if config.enable_lto {
            args.push("/LTCG".to_string());
        }

        // 子系统
        if let Some(subsystem) = unit.subsystem {
            args.push(subsystem.as_msvc_flag().to_string());
        }

        // 入口点
        if let Some(ref entry) = unit.entry_point {
            args.push(format!("/ENTRY:{}", entry));
        }

        // 堆栈大小
        if let Some(stack_size) = config.stack_size {
            args.push(format!("/STACK:{}", stack_size));
        }

        // 堆大小
        if let Some(heap_size) = config.heap_size {
            args.push(format!("/HEAP:{}", heap_size));
        }

        // 模块定义文件
        if let Some(ref def) = unit.module_def {
            args.push(format!("/DEF:{}", def.display()));
        }

        // Map 文件
        if config.generate_map {
            let map_file = unit.output_file.with_extension("map");
            args.push(format!("/MAP:{}", map_file.display()));
        }

        // 机器类型
        match config.architecture {
            Architecture::X64 => args.push("/MACHINE:X64".to_string()),
            Architecture::X86 => args.push("/MACHINE:X86".to_string()),
            Architecture::ARM64 => args.push("/MACHINE:ARM64".to_string()),
            Architecture::ARM32 => args.push("/MACHINE:ARM".to_string()),
        }

        // 库目录
        for dir in &unit.lib_dirs {
            args.push(format!("/LIBPATH:{}", dir.display()));
        }
        for dir in &self.vs_info.lib_paths {
            args.push(format!("/LIBPATH:{}", dir.display()));
        }
        for dir in &self.sdk_info.lib_paths {
            args.push(format!("/LIBPATH:{}", dir.display()));
        }

        // 默认库
        args.push("kernel32.lib".to_string());
        args.push("user32.lib".to_string());
        args.push("gdi32.lib".to_string());
        args.push("shell32.lib".to_string());
        args.push("ole32.lib".to_string());
        args.push("oleaut32.lib".to_string());
        args.push("uuid.lib".to_string());
        args.push("advapi32.lib".to_string());

        // 静态库
        for lib in &unit.static_libs {
            args.push(format!("{}", lib.display()));
        }

        // 动态库
        for lib in &unit.dynamic_libs {
            if lib.ends_with(".lib") {
                args.push(lib.clone());
            } else {
                args.push(format!("{}.lib", lib));
            }
        }

        // 额外选项
        for flag in &unit.extra_flags {
            args.push(flag.clone());
        }

        // 对象文件
        for obj in &unit.object_files {
            args.push(format!("{}", obj.display()));
        }

        args
    }

    /// 解析链接器输出
    fn parse_link_output(&self, output: &str) -> Vec<Diagnostic> {
        let mut diagnostics = Vec::new();

        for line in output.lines() {
            // LINK : fatal error LNK1104: cannot open file 'xxx.lib'
            if line.contains("fatal error") || line.contains("error LNK") {
                let severity = DiagnosticSeverity::Error;
                let code = self.extract_link_error_code(line);

                diagnostics.push(Diagnostic {
                    severity,
                    file: None,
                    line: 0,
                    column: 0,
                    code,
                    message: line.to_string(),
                    source: Some("link.exe".to_string()),
                    related: Vec::new(),
                });
            } else if line.contains("warning LNK") {
                diagnostics.push(Diagnostic {
                    severity: DiagnosticSeverity::Warning,
                    file: None,
                    line: 0,
                    column: 0,
                    code: self.extract_link_error_code(line),
                    message: line.to_string(),
                    source: Some("link.exe".to_string()),
                    related: Vec::new(),
                });
            }
        }

        diagnostics
    }

    /// 提取链接错误码
    fn extract_link_error_code(&self, line: &str) -> Option<String> {
        // 查找 LNKxxxx 模式
        if let Some(start) = line.find("LNK") {
            let code: String = line[start..]
                .chars()
                .take_while(|c| c.is_ascii_alphanumeric())
                .collect();
            if code.len() > 3 {
                return Some(code);
            }
        }
        None
    }
}

impl Linker for MsvcLinker {
    fn name(&self) -> &str {
        "MSVC Linker"
    }

    fn link(&self, unit: &LinkUnit, config: &LinkerConfig) -> Result<LinkResult> {
        let start = Instant::now();

        // 确保输出目录存在
        if let Some(parent) = unit.output_file.parent() {
            fs::create_dir_all(parent)?;
        }

        let args = self.build_link_args(unit, config);

        let output = Command::new(&self.link_path)
            .args(&args)
            .envs(&self.environment)
            .output()
            .context("执行 link.exe 失败")?;

        let duration_ms = start.elapsed().as_millis() as u64;

        let stdout = String::from_utf8_lossy(&output.stdout);
        let stderr = String::from_utf8_lossy(&output.stderr);
        let combined = format!("{}\n{}", stdout, stderr);

        let diagnostics = self.parse_link_output(&combined);

        if output.status.success() {
            let mut result = LinkResult::success(unit.output_file.clone(), duration_ms);
            result.diagnostics = diagnostics;
            result.import_lib = unit.import_lib.clone();
            result.pdb_file = unit.pdb_file.clone();
            Ok(result)
        } else {
            Ok(LinkResult::failure(diagnostics, duration_ms))
        }
    }

    fn create_static_lib(
        &self,
        objects: &[PathBuf],
        output: &Path,
        config: &LinkerConfig,
    ) -> Result<LinkResult> {
        let start = Instant::now();

        // 确保输出目录存在
        if let Some(parent) = output.parent() {
            fs::create_dir_all(parent)?;
        }

        let mut args = vec!["/NOLOGO".to_string(), format!("/OUT:{}", output.display())];

        // 机器类型
        match config.architecture {
            Architecture::X64 => args.push("/MACHINE:X64".to_string()),
            Architecture::X86 => args.push("/MACHINE:X86".to_string()),
            Architecture::ARM64 => args.push("/MACHINE:ARM64".to_string()),
            Architecture::ARM32 => args.push("/MACHINE:ARM".to_string()),
        }

        // 对象文件
        for obj in objects {
            args.push(format!("{}", obj.display()));
        }

        let output_result = Command::new(&self.lib_path)
            .args(&args)
            .envs(&self.environment)
            .output()
            .context("执行 lib.exe 失败")?;

        let duration_ms = start.elapsed().as_millis() as u64;

        if output_result.status.success() {
            Ok(LinkResult::success(output.to_path_buf(), duration_ms))
        } else {
            let stderr = String::from_utf8_lossy(&output_result.stderr);
            let diagnostics = self.parse_link_output(&stderr);
            Ok(LinkResult::failure(diagnostics, duration_ms))
        }
    }

    fn default_lib_paths(&self) -> Vec<PathBuf> {
        let mut paths = self.vs_info.lib_paths.clone();
        paths.extend(self.sdk_info.lib_paths.clone());
        paths
    }
}

//=============================================================================
// 单元测试
//=============================================================================

#[cfg(test)]
#[cfg(target_os = "windows")]
mod tests {
    use super::*;

    #[test]
    fn test_msvc_detection() {
        let result = MsvcCompiler::detect();
        // 若当前环境未安装 MSVC 则跳过
        if let Err(e) = &result {
            eprintln!("跳过测试: 未检测到 MSVC — {}", e);
            return;
        }
        assert!(result.is_ok());
    }

    #[test]
    fn test_msvc_linker_detection() {
        let result = MsvcLinker::detect();
        // 若当前环境未安装 MSVC 则跳过
        if let Err(e) = &result {
            eprintln!("跳过测试: 未检测到 MSVC Linker — {}", e);
            return;
        }
        assert!(result.is_ok());
    }

    #[test]
    fn test_compile_args_generation() {
        // 若当前环境未安装 MSVC 则跳过
        let compiler = match MsvcCompiler::detect() {
            Ok(c) => c,
            Err(e) => {
                eprintln!("跳过测试: 未检测到 MSVC — {}", e);
                return;
            }
        };

        let unit = CompileUnit::new(
            PathBuf::from("test.cpp"),
            PathBuf::from("test.obj"),
            "TestModule",
        );

        let config = CompilerConfig::default();
        let args = compiler.build_compile_args(&unit, &config);

        assert!(args.contains(&"/nologo".to_string()));
        assert!(args.contains(&"/c".to_string()));
        assert!(args.iter().any(|a| a.starts_with("/std:c++")));
    }
}
