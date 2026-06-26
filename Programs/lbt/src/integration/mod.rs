/*******************************************************************************
 * 文件: integration/mod.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   LBT 工具集成模块 - 协调 LHT 和 LSC 的调用
 *   - 自动检测需要反射代码生成的头文件
 *   - 自动检测需要编译的着色器
 *   - 并行执行工具调用
 *   - 增量构建支持
 *
 * 设计哲学:
 *   1. 统一的构建流程
 *   2. 最小化重复工作
 *   3. 清晰的依赖关系
 *
 * 技术特性:
 *   - 通过子进程调用 LHT/LSC
 *   - 支持自定义工具路径
 *   - 构建状态追踪
 *   - 错误聚合和报告
 *
 ******************************************************************************/

use anyhow::{anyhow, Result};
use rayon::prelude::*;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::Instant;

//=============================================================================
// 工具配置
//=============================================================================

/// 工具链配置
#[derive(Debug, Clone)]
pub struct ToolchainConfig {
    /// LHT 可执行文件路径
    pub lht_path: PathBuf,
    /// LSC 可执行文件路径
    pub lsc_path: PathBuf,
    /// 并行工作线程数
    pub parallel_jobs: usize,
    /// 是否启用详细输出
    pub verbose: bool,
}

impl Default for ToolchainConfig {
    fn default() -> Self {
        Self {
            lht_path: PathBuf::from("lht"),
            lsc_path: PathBuf::from("lsc"),
            parallel_jobs: num_cpus::get(),
            verbose: false,
        }
    }
}

impl ToolchainConfig {
    /// 从 Cargo target 目录检测工具路径
    pub fn detect_from_cargo() -> Self {
        let mut config = Self::default();

        let exe_name = |tool: &str| {
            if cfg!(windows) {
                format!("{}.exe", tool)
            } else {
                tool.to_string()
            }
        };

        if let Ok(current_exe) = std::env::current_exe() {
            if let Some(dir) = current_exe.parent() {
                let lht = dir.join(exe_name("lht"));
                let lsc = dir.join(exe_name("lsc"));
                if lht.exists() {
                    config.lht_path = lht;
                }
                if lsc.exists() {
                    config.lsc_path = lsc;
                }
            }
        }

        let cwd = std::env::current_dir().unwrap_or_else(|_| PathBuf::from("."));
        for target_dir in [
            cwd.join("Programs").join("target").join("release"),
            cwd.join("Programs").join("target").join("debug"),
            cwd.join("target").join("release"),
            cwd.join("target").join("debug"),
        ] {
            let lht = target_dir.join(exe_name("lht"));
            let lsc = target_dir.join(exe_name("lsc"));
            if lht.exists() {
                config.lht_path = lht;
            }
            if lsc.exists() {
                config.lsc_path = lsc;
            }
        }

        // 尝试从 target/debug 或 target/release 找工具
        if let Ok(manifest_dir) = std::env::var("CARGO_MANIFEST_DIR") {
            let target_debug = PathBuf::from(&manifest_dir)
                .parent()
                .map(|p| p.join("target/debug"));

            if let Some(debug_dir) = target_debug {
                let lht = debug_dir.join("lht.exe");
                let lsc = debug_dir.join("lsc.exe");

                if lht.exists() {
                    config.lht_path = lht;
                }
                if lsc.exists() {
                    config.lsc_path = lsc;
                }
            }
        }

        config
    }

    /// 验证工具是否可用
    pub fn validate(&self) -> Result<()> {
        if !self.lht_path.exists() {
            return Err(anyhow!("LHT not found at: {:?}", self.lht_path));
        }
        if !self.lsc_path.exists() {
            return Err(anyhow!("LSC not found at: {:?}", self.lsc_path));
        }
        Ok(())
    }
}

//=============================================================================
// LHT 集成
//=============================================================================

/// LHT 任务配置
#[derive(Debug, Clone)]
pub struct LhtTask {
    /// 源目录 (包含 .h 文件)
    pub source_dir: PathBuf,
    /// 输出目录 (生成 .generated.h/.cpp)
    pub output_dir: PathBuf,
    /// 模块名称
    pub module_name: String,
    /// 额外的包含目录
    pub include_dirs: Vec<PathBuf>,
    /// 是否启用热重载支持
    pub hot_reload: bool,
}

/// LHT 执行结果
#[derive(Debug, Clone)]
pub struct LhtResult {
    /// 是否成功
    pub success: bool,
    /// 处理的文件数
    pub files_processed: usize,
    /// 生成的文件
    pub generated_files: Vec<PathBuf>,
    /// 错误消息
    pub errors: Vec<String>,
    /// 警告消息
    pub warnings: Vec<String>,
    /// 执行耗时 (毫秒)
    pub duration_ms: u64,
}

/// LHT 集成器
pub struct LhtIntegration {
    config: ToolchainConfig,
}

impl LhtIntegration {
    pub fn new(config: ToolchainConfig) -> Self {
        Self { config }
    }

    /// 执行 LHT 代码生成
    pub fn run(&self, task: &LhtTask) -> Result<LhtResult> {
        let start = Instant::now();

        let mut cmd = Command::new(&self.config.lht_path);
        cmd.arg("generate")
            .arg("--source-dir")
            .arg(&task.source_dir)
            .arg("--output-dir")
            .arg(&task.output_dir)
            .arg("--module")
            .arg(&task.module_name);

        if self.config.verbose {
            cmd.arg("--verbose");
        }

        cmd.stdout(Stdio::piped()).stderr(Stdio::piped());

        let output = cmd.output()?;
        let stdout = String::from_utf8_lossy(&output.stdout);
        let stderr = String::from_utf8_lossy(&output.stderr);

        let mut result = LhtResult {
            success: output.status.success(),
            files_processed: 0,
            generated_files: Vec::new(),
            errors: Vec::new(),
            warnings: Vec::new(),
            duration_ms: start.elapsed().as_millis() as u64,
        };

        // 解析输出
        for line in stdout.lines() {
            if line.starts_with("Generated: ") {
                if let Some(path) = line.strip_prefix("Generated: ") {
                    result.generated_files.push(PathBuf::from(path));
                }
            } else if line.starts_with("Processed: ") {
                if let Some(count) = line.strip_prefix("Processed: ") {
                    result.files_processed = count.parse().unwrap_or(0);
                }
            }
        }

        for line in stderr.lines() {
            if line.contains("error") || line.contains("Error") {
                result.errors.push(line.to_string());
            } else if line.contains("warning") || line.contains("Warning") {
                result.warnings.push(line.to_string());
            }
        }

        if !result.success && result.errors.is_empty() {
            result
                .errors
                .push(format!("LHT exited with code: {:?}", output.status.code()));
        }

        Ok(result)
    }

    /// 扫描目录查找需要处理的头文件
    pub fn scan_headers(&self, source_dir: &Path) -> Vec<PathBuf> {
        let mut headers = Vec::new();

        for entry in walkdir::WalkDir::new(source_dir)
            .into_iter()
            .filter_map(|e| e.ok())
        {
            let path = entry.path();
            if path.extension().map(|e| e == "h").unwrap_or(false) {
                // 检查是否包含反射宏
                if self.contains_reflection_macros(path) {
                    headers.push(path.to_path_buf());
                }
            }
        }

        headers
    }

    /// 检查文件是否包含反射宏
    fn contains_reflection_macros(&self, path: &Path) -> bool {
        if let Ok(content) = std::fs::read_to_string(path) {
            content.contains("LCLASS")
                || content.contains("LSTRUCT")
                || content.contains("LENUM")
                || content.contains("LPROPERTY")
                || content.contains("LFUNCTION")
        } else {
            false
        }
    }
}

//=============================================================================
// LSC 集成
//=============================================================================

/// LSC 任务配置
#[derive(Debug, Clone)]
pub struct LscTask {
    /// 着色器源文件或目录
    pub source: PathBuf,
    /// 输出目录
    pub output_dir: PathBuf,
    /// 包含目录
    pub include_dirs: Vec<PathBuf>,
    /// 宏定义
    pub defines: Vec<(String, Option<String>)>,
    /// 优化级别 (0-3)
    pub optimization_level: u32,
    /// 是否生成调试信息
    pub debug_info: bool,
    /// 是否生成反射数据
    pub generate_reflection: bool,
}

/// LSC 执行结果
#[derive(Debug, Clone)]
pub struct LscResult {
    /// 是否成功
    pub success: bool,
    /// 编译的着色器数
    pub shaders_compiled: usize,
    /// 输出文件
    pub output_files: Vec<PathBuf>,
    /// 错误
    pub errors: Vec<ShaderError>,
    /// 警告
    pub warnings: Vec<String>,
    /// 执行耗时 (毫秒)
    pub duration_ms: u64,
}

/// 着色器编译错误
#[derive(Debug, Clone)]
pub struct ShaderError {
    pub file: PathBuf,
    pub line: Option<u32>,
    pub column: Option<u32>,
    pub message: String,
}

/// LSC 集成器
pub struct LscIntegration {
    config: ToolchainConfig,
}

impl LscIntegration {
    pub fn new(config: ToolchainConfig) -> Self {
        Self { config }
    }

    /// 编译单个着色器
    pub fn compile(&self, task: &LscTask) -> Result<LscResult> {
        let start = Instant::now();
        let stem = task
            .source
            .file_stem()
            .and_then(|s| s.to_str())
            .unwrap_or("shader");
        let output_path = task.output_dir.join(format!("{}.spv", stem));

        let mut cmd = Command::new(&self.config.lsc_path);
        cmd.arg("compile")
            .arg("--source")
            .arg(&task.source)
            .arg("--output")
            .arg(&output_path);

        for include_dir in &task.include_dirs {
            cmd.arg("-I").arg(include_dir);
        }

        for (name, value) in &task.defines {
            if let Some(val) = value {
                cmd.arg("-D").arg(format!("{}={}", name, val));
            } else {
                cmd.arg("-D").arg(name);
            }
        }

        if task.optimization_level > 0 {
            cmd.arg("-O");
        }

        if task.debug_info {
            cmd.arg("--debug-info");
        }

        if !task.generate_reflection {
            cmd.arg("--reflection").arg("false");
        }

        if self.config.verbose {
            cmd.arg("--verbose");
        }

        cmd.stdout(Stdio::piped()).stderr(Stdio::piped());

        let output = cmd.output()?;
        let stdout = String::from_utf8_lossy(&output.stdout);
        let stderr = String::from_utf8_lossy(&output.stderr);

        let mut result = LscResult {
            success: output.status.success(),
            shaders_compiled: 0,
            output_files: Vec::new(),
            errors: Vec::new(),
            warnings: Vec::new(),
            duration_ms: start.elapsed().as_millis() as u64,
        };

        if result.success {
            result.output_files.push(output_path);
            result.shaders_compiled = 1;
        }

        // 解析输出
        for line in stdout.lines() {
            if line.starts_with("Output: ") {
                if let Some(path) = line.strip_prefix("Output: ") {
                    result.output_files.push(PathBuf::from(path));
                    result.shaders_compiled += 1;
                }
            }
        }

        // 解析错误
        for line in stderr.lines() {
            if line.contains("error") || line.contains("Error") {
                result.errors.push(ShaderError {
                    file: task.source.clone(),
                    line: None,
                    column: None,
                    message: line.to_string(),
                });
            } else if line.contains("warning") || line.contains("Warning") {
                result.warnings.push(line.to_string());
            }
        }

        if !result.success && result.errors.is_empty() {
            result.errors.push(ShaderError {
                file: task.source.clone(),
                line: None,
                column: None,
                message: format!("LSC exited with code: {:?}", output.status.code()),
            });
        }

        Ok(result)
    }

    /// 批量编译着色器
    pub fn compile_batch(&self, tasks: &[LscTask]) -> Vec<LscResult> {
        tasks
            .par_iter()
            .map(|task| {
                self.compile(task).unwrap_or_else(|e| LscResult {
                    success: false,
                    shaders_compiled: 0,
                    output_files: Vec::new(),
                    errors: vec![ShaderError {
                        file: task.source.clone(),
                        line: None,
                        column: None,
                        message: e.to_string(),
                    }],
                    warnings: Vec::new(),
                    duration_ms: 0,
                })
            })
            .collect()
    }

    /// 扫描目录查找着色器文件
    pub fn scan_shaders(&self, source_dir: &Path) -> Vec<PathBuf> {
        let mut shaders = Vec::new();
        let shader_extensions = [
            "glsl", "hlsl", "vert", "frag", "comp", "geom", "tesc", "tese",
        ];

        for entry in walkdir::WalkDir::new(source_dir)
            .into_iter()
            .filter_map(|e| e.ok())
        {
            let path = entry.path();
            if let Some(ext) = path.extension() {
                if shader_extensions.iter().any(|&e| ext == e) {
                    shaders.push(path.to_path_buf());
                }
            }
        }

        shaders
    }
}

//=============================================================================
// 统一构建协调器
//=============================================================================

/// 构建阶段
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BuildPhase {
    /// 反射代码生成
    ReflectionGeneration,
    /// 着色器编译
    ShaderCompilation,
    /// C++ 编译
    CppCompilation,
    /// 链接
    Linking,
}

/// 构建事件
#[derive(Debug, Clone)]
pub enum BuildEvent {
    PhaseStarted(BuildPhase),
    PhaseCompleted(BuildPhase, u64), // 阶段, 耗时ms
    FileProcessed(PathBuf),
    Error(String),
    Warning(String),
}

/// 构建回调
pub type BuildCallback = Box<dyn Fn(BuildEvent) + Send + Sync>;

/// 构建协调器
pub struct BuildCoordinator {
    config: ToolchainConfig,
    lht: LhtIntegration,
    lsc: LscIntegration,
    callbacks: Vec<BuildCallback>,
}

impl BuildCoordinator {
    pub fn new(config: ToolchainConfig) -> Self {
        let lht = LhtIntegration::new(config.clone());
        let lsc = LscIntegration::new(config.clone());

        Self {
            config,
            lht,
            lsc,
            callbacks: Vec::new(),
        }
    }

    /// 添加构建事件回调
    pub fn add_callback(&mut self, callback: BuildCallback) {
        self.callbacks.push(callback);
    }

    /// 发送事件
    fn emit(&self, event: BuildEvent) {
        for callback in &self.callbacks {
            callback(event.clone());
        }
    }

    /// 执行完整构建流程
    pub fn build(&self, project: &ProjectBuildConfig) -> Result<BuildSummary> {
        let total_start = Instant::now();
        let mut summary = BuildSummary::default();

        // 阶段 1: 反射代码生成
        if project.enable_reflection {
            self.emit(BuildEvent::PhaseStarted(BuildPhase::ReflectionGeneration));
            let phase_start = Instant::now();

            for module in &project.modules {
                let task = LhtTask {
                    source_dir: module.source_dir.clone(),
                    output_dir: module.generated_dir.clone(),
                    module_name: module.name.clone(),
                    include_dirs: module.include_dirs.clone(),
                    hot_reload: project.hot_reload,
                };

                match self.lht.run(&task) {
                    Ok(result) => {
                        summary.reflection_files += result.files_processed;
                        for file in &result.generated_files {
                            self.emit(BuildEvent::FileProcessed(file.clone()));
                        }
                        for error in result.errors {
                            summary.errors.push(error.clone());
                            self.emit(BuildEvent::Error(error));
                        }
                        for warning in result.warnings {
                            summary.warnings.push(warning.clone());
                            self.emit(BuildEvent::Warning(warning));
                        }
                    }
                    Err(e) => {
                        let error = format!("LHT failed for module {}: {}", module.name, e);
                        summary.errors.push(error.clone());
                        self.emit(BuildEvent::Error(error));
                    }
                }
            }

            let phase_duration = phase_start.elapsed().as_millis() as u64;
            summary.reflection_time_ms = phase_duration;
            self.emit(BuildEvent::PhaseCompleted(
                BuildPhase::ReflectionGeneration,
                phase_duration,
            ));
        }

        // 阶段 2: 着色器编译
        if project.enable_shaders {
            self.emit(BuildEvent::PhaseStarted(BuildPhase::ShaderCompilation));
            let phase_start = Instant::now();

            let shader_tasks: Vec<LscTask> = project
                .shader_dirs
                .iter()
                .flat_map(|dir| self.lsc.scan_shaders(dir))
                .map(|source| LscTask {
                    source,
                    output_dir: project.shader_output_dir.clone(),
                    include_dirs: project.shader_include_dirs.clone(),
                    defines: project.shader_defines.clone(),
                    optimization_level: if project.debug { 0 } else { 2 },
                    debug_info: project.debug,
                    generate_reflection: true,
                })
                .collect();

            let results = self.lsc.compile_batch(&shader_tasks);

            for result in results {
                summary.shaders_compiled += result.shaders_compiled;
                for file in &result.output_files {
                    self.emit(BuildEvent::FileProcessed(file.clone()));
                }
                for error in result.errors {
                    summary.errors.push(error.message.clone());
                    self.emit(BuildEvent::Error(error.message));
                }
                for warning in result.warnings {
                    summary.warnings.push(warning.clone());
                    self.emit(BuildEvent::Warning(warning));
                }
            }

            let phase_duration = phase_start.elapsed().as_millis() as u64;
            summary.shader_time_ms = phase_duration;
            self.emit(BuildEvent::PhaseCompleted(
                BuildPhase::ShaderCompilation,
                phase_duration,
            ));
        }

        summary.total_time_ms = total_start.elapsed().as_millis() as u64;
        summary.success = summary.errors.is_empty();

        Ok(summary)
    }
}

/// 项目构建配置
#[derive(Debug, Clone)]
pub struct ProjectBuildConfig {
    /// 模块列表
    pub modules: Vec<ModuleBuildConfig>,
    /// 是否启用反射代码生成
    pub enable_reflection: bool,
    /// 是否启用着色器编译
    pub enable_shaders: bool,
    /// 着色器源目录
    pub shader_dirs: Vec<PathBuf>,
    /// 着色器输出目录
    pub shader_output_dir: PathBuf,
    /// 着色器包含目录
    pub shader_include_dirs: Vec<PathBuf>,
    /// 着色器宏定义
    pub shader_defines: Vec<(String, Option<String>)>,
    /// 是否启用热重载
    pub hot_reload: bool,
    /// 是否为调试构建
    pub debug: bool,
}

/// 模块构建配置
#[derive(Debug, Clone)]
pub struct ModuleBuildConfig {
    /// 模块名称
    pub name: String,
    /// 源文件目录
    pub source_dir: PathBuf,
    /// 生成文件目录
    pub generated_dir: PathBuf,
    /// 包含目录
    pub include_dirs: Vec<PathBuf>,
}

/// 构建摘要
#[derive(Debug, Clone, Default)]
pub struct BuildSummary {
    /// 是否成功
    pub success: bool,
    /// 处理的反射文件数
    pub reflection_files: usize,
    /// 编译的着色器数
    pub shaders_compiled: usize,
    /// 反射阶段耗时
    pub reflection_time_ms: u64,
    /// 着色器阶段耗时
    pub shader_time_ms: u64,
    /// 总耗时
    pub total_time_ms: u64,
    /// 错误列表
    pub errors: Vec<String>,
    /// 警告列表
    pub warnings: Vec<String>,
}

impl BuildSummary {
    /// 打印构建摘要
    pub fn print(&self) {
        println!("\n=== 构建摘要 ===");
        println!("状态: {}", if self.success { "成功" } else { "失败" });
        println!(
            "反射文件: {} ({} ms)",
            self.reflection_files, self.reflection_time_ms
        );
        println!(
            "着色器: {} ({} ms)",
            self.shaders_compiled, self.shader_time_ms
        );
        println!("总耗时: {} ms", self.total_time_ms);

        if !self.warnings.is_empty() {
            println!("\n警告 ({}):", self.warnings.len());
            for (i, warning) in self.warnings.iter().take(10).enumerate() {
                println!("  {}. {}", i + 1, warning);
            }
            if self.warnings.len() > 10 {
                println!("  ... 还有 {} 个警告", self.warnings.len() - 10);
            }
        }

        if !self.errors.is_empty() {
            println!("\n错误 ({}):", self.errors.len());
            for (i, error) in self.errors.iter().enumerate() {
                println!("  {}. {}", i + 1, error);
            }
        }
    }
}
