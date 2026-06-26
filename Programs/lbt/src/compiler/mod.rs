/*******************************************************************************
 * 文件: compiler/mod.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   LBT 编译器模块 - 生产级 C++ 编译执行系统
 *   - 多编译器支持 (MSVC/Clang/GCC)
 *   - 并行编译调度
 *   - 增量编译优化
 *   - 编译诊断解析
 *
 * 设计哲学:
 *   1. 零开销抽象 - 编译器特定优化不被抽象层阻挡
 *   2. 可组合性 - 编译阶段可独立控制
 *   3. 可观测性 - 完整的编译过程监控
 *
 * 技术特性:
 *   - 基于 trait 的编译器抽象
 *   - Rayon 并行编译
 *   - 精确的依赖追踪
 *   - 结构化错误报告
 *
 ******************************************************************************/

pub mod action;
pub mod clang;
pub mod compile_cache;
pub mod deps;
pub mod diagnostics;
pub mod distributed;
pub mod gcc;
pub mod include_analyzer;
pub mod linker;
pub mod msvc;
pub mod pch;
pub mod pch_analyzer;
pub mod platform;
pub mod response_file;
pub mod scheduler;
pub mod toolchain;
pub mod unity;

pub use diagnostics::*;
pub use platform::*;

use anyhow::Result;
use std::collections::HashMap;
use std::path::{Path, PathBuf};

//=============================================================================
// 核心类型定义
//=============================================================================

/// 构建配置类型
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum BuildConfiguration {
    /// 调试构建 - 无优化，完整调试信息
    Debug,
    /// 开发构建 - 部分优化，保留调试信息
    Development,
    /// 发布构建 - 完全优化，无调试信息
    Release,
    /// 发行构建 - 最大优化，符号剥离
    Shipping,
    /// 测试构建 - 用于自动化测试
    Test,
}

impl BuildConfiguration {
    /// 获取配置名称
    pub fn name(&self) -> &'static str {
        match self {
            Self::Debug => "Debug",
            Self::Development => "Development",
            Self::Release => "Release",
            Self::Shipping => "Shipping",
            Self::Test => "Test",
        }
    }

    /// 是否启用优化
    pub fn is_optimized(&self) -> bool {
        matches!(self, Self::Release | Self::Shipping)
    }

    /// 是否包含调试信息
    pub fn has_debug_info(&self) -> bool {
        matches!(self, Self::Debug | Self::Development | Self::Test)
    }

    /// 获取优化级别 (0-3)
    pub fn optimization_level(&self) -> u8 {
        match self {
            Self::Debug => 0,
            Self::Development => 1,
            Self::Release => 2,
            Self::Shipping => 3,
            Self::Test => 1,
        }
    }
}

impl Default for BuildConfiguration {
    fn default() -> Self {
        Self::Development
    }
}

/// 目标架构
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Architecture {
    X64,
    X86,
    ARM64,
    ARM32,
}

impl Architecture {
    pub fn name(&self) -> &'static str {
        match self {
            Self::X64 => "x64",
            Self::X86 => "x86",
            Self::ARM64 => "arm64",
            Self::ARM32 => "arm",
        }
    }

    pub fn pointer_size(&self) -> usize {
        match self {
            Self::X64 | Self::ARM64 => 8,
            Self::X86 | Self::ARM32 => 4,
        }
    }

    /// 从当前平台检测
    pub fn host() -> Self {
        #[cfg(target_arch = "x86_64")]
        {
            Self::X64
        }
        #[cfg(target_arch = "x86")]
        {
            Self::X86
        }
        #[cfg(target_arch = "aarch64")]
        {
            Self::ARM64
        }
        #[cfg(target_arch = "arm")]
        {
            Self::ARM32
        }
    }
}

impl Default for Architecture {
    fn default() -> Self {
        Self::host()
    }
}

/// 编译目标类型
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TargetType {
    /// 可执行文件
    Executable,
    /// 静态库
    StaticLibrary,
    /// 动态库/共享库
    DynamicLibrary,
    /// 仅头文件库
    HeaderOnly,
    /// 对象文件集合 (不链接)
    ObjectFiles,
}

impl TargetType {
    /// 获取输出文件扩展名
    pub fn extension(&self, platform: Platform) -> &'static str {
        match (self, platform) {
            (Self::Executable, Platform::Windows) => "exe",
            (Self::Executable, _) => "",
            (Self::StaticLibrary, Platform::Windows) => "lib",
            (Self::StaticLibrary, _) => "a",
            (Self::DynamicLibrary, Platform::Windows) => "dll",
            (Self::DynamicLibrary, Platform::Linux) => "so",
            (Self::DynamicLibrary, Platform::MacOS) => "dylib",
            (Self::HeaderOnly, _) => "",
            (Self::ObjectFiles, Platform::Windows) => "obj",
            (Self::ObjectFiles, _) => "o",
        }
    }

    /// 获取导入库扩展名 (仅 Windows DLL)
    pub fn import_lib_extension(&self, platform: Platform) -> Option<&'static str> {
        match (self, platform) {
            (Self::DynamicLibrary, Platform::Windows) => Some("lib"),
            _ => None,
        }
    }
}

/// 编译单元 - 单个源文件的编译任务
#[derive(Debug, Clone)]
pub struct CompileUnit {
    /// 源文件路径
    pub source_file: PathBuf,
    /// 输出对象文件路径
    pub object_file: PathBuf,
    /// 所属模块名
    pub module_name: String,
    /// 预处理器定义
    pub defines: Vec<String>,
    /// 包含目录
    pub include_dirs: Vec<PathBuf>,
    /// 强制包含的头文件
    pub force_includes: Vec<PathBuf>,
    /// 是否使用 PCH
    pub use_pch: Option<PathBuf>,
    /// 是否创建 PCH
    pub create_pch: bool,
    /// 额外编译选项
    pub extra_flags: Vec<String>,
    /// 依赖的头文件 (用于增量编译)
    pub dependencies: Vec<PathBuf>,
    /// 源文件内容哈希
    pub content_hash: Option<u64>,
    /// 语言标准
    pub language_standard: LanguageStandard,
    /// 是否为 C 文件 (否则为 C++)
    pub is_c_file: bool,
    /// 警告级别
    pub warning_level: WarningLevel,
    /// 是否将警告视为错误
    pub warnings_as_errors: bool,
}

impl CompileUnit {
    pub fn new(source: PathBuf, object: PathBuf, module: &str) -> Self {
        let is_c = source.extension().map(|e| e == "c").unwrap_or(false);

        Self {
            source_file: source,
            object_file: object,
            module_name: module.to_string(),
            defines: Vec::new(),
            include_dirs: Vec::new(),
            force_includes: Vec::new(),
            use_pch: None,
            create_pch: false,
            extra_flags: Vec::new(),
            dependencies: Vec::new(),
            content_hash: None,
            language_standard: if is_c {
                LanguageStandard::C17
            } else {
                LanguageStandard::default()
            },
            is_c_file: is_c,
            warning_level: WarningLevel::High,
            warnings_as_errors: true,
        }
    }

    /// 添加预处理器定义
    pub fn define(&mut self, name: &str, value: Option<&str>) -> &mut Self {
        match value {
            Some(v) => self.defines.push(format!("{}={}", name, v)),
            None => self.defines.push(name.to_string()),
        }
        self
    }

    /// 添加包含目录
    pub fn include(&mut self, dir: PathBuf) -> &mut Self {
        if !self.include_dirs.contains(&dir) {
            self.include_dirs.push(dir);
        }
        self
    }
}

/// 语言标准
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LanguageStandard {
    C11,
    C17,
    C23,
    Cpp14,
    Cpp17,
    Cpp20,
    Cpp23,
    CppLatest,
}

impl LanguageStandard {
    pub fn as_msvc_flag(&self) -> &'static str {
        match self {
            Self::C11 => "/std:c11",
            Self::C17 => "/std:c17",
            Self::C23 => "/std:clatest",
            Self::Cpp14 => "/std:c++14",
            Self::Cpp17 => "/std:c++17",
            Self::Cpp20 => "/std:c++20",
            Self::Cpp23 => "/std:c++23",
            Self::CppLatest => "/std:c++latest",
        }
    }

    pub fn as_clang_flag(&self) -> &'static str {
        match self {
            Self::C11 => "-std=c11",
            Self::C17 => "-std=c17",
            Self::C23 => "-std=c2x",
            Self::Cpp14 => "-std=c++14",
            Self::Cpp17 => "-std=c++17",
            Self::Cpp20 => "-std=c++20",
            Self::Cpp23 => "-std=c++23",
            Self::CppLatest => "-std=c++2c",
        }
    }

    pub fn as_gcc_flag(&self) -> &'static str {
        self.as_clang_flag() // GCC 使用相同的标志
    }
}

impl Default for LanguageStandard {
    fn default() -> Self {
        // 使用 CppLatest 而非 Cpp23 — 因为 MSVC /std:c++23 在 19.40 以下不可用
        // /std:c++latest 在所有支持的 MSVC 版本上均可工作
        Self::CppLatest
    }
}

/// 警告级别
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WarningLevel {
    /// 无警告
    None,
    /// 基本警告
    Low,
    /// 标准警告
    Medium,
    /// 高级警告
    High,
    /// 所有警告
    All,
}

impl WarningLevel {
    pub fn as_msvc_flag(&self) -> &'static str {
        match self {
            Self::None => "/W0",
            Self::Low => "/W1",
            Self::Medium => "/W2",
            Self::High => "/W3",
            Self::All => "/W4",
        }
    }

    pub fn as_clang_flags(&self) -> Vec<&'static str> {
        match self {
            Self::None => vec!["-w"],
            Self::Low => vec!["-Wall"],
            Self::Medium => vec!["-Wall"],
            Self::High => vec!["-Wall", "-Wextra"],
            Self::All => vec!["-Wall", "-Wextra", "-Wpedantic"],
        }
    }
}

impl Default for WarningLevel {
    fn default() -> Self {
        Self::High
    }
}

/// 链接单元 - 链接任务
#[derive(Debug, Clone)]
pub struct LinkUnit {
    /// 输出文件路径
    pub output_file: PathBuf,
    /// 输入对象文件
    pub object_files: Vec<PathBuf>,
    /// 输入静态库
    pub static_libs: Vec<PathBuf>,
    /// 链接的动态库
    pub dynamic_libs: Vec<String>,
    /// 库搜索路径
    pub lib_dirs: Vec<PathBuf>,
    /// 目标类型
    pub target_type: TargetType,
    /// 额外链接选项
    pub extra_flags: Vec<String>,
    /// 是否生成调试信息
    pub debug_info: bool,
    /// 是否启用增量链接
    pub incremental: bool,
    /// 模块定义文件 (.def)
    pub module_def: Option<PathBuf>,
    /// 导入库输出路径
    pub import_lib: Option<PathBuf>,
    /// PDB 输出路径
    pub pdb_file: Option<PathBuf>,
    /// 子系统 (Windows)
    pub subsystem: Option<Subsystem>,
    /// 入口点
    pub entry_point: Option<String>,
}

impl LinkUnit {
    pub fn new(output: PathBuf, target_type: TargetType) -> Self {
        Self {
            output_file: output,
            object_files: Vec::new(),
            static_libs: Vec::new(),
            dynamic_libs: Vec::new(),
            lib_dirs: Vec::new(),
            target_type,
            extra_flags: Vec::new(),
            debug_info: false,
            incremental: true,
            module_def: None,
            import_lib: None,
            pdb_file: None,
            subsystem: None,
            entry_point: None,
        }
    }

    pub fn add_object(&mut self, obj: PathBuf) -> &mut Self {
        self.object_files.push(obj);
        self
    }

    pub fn add_lib(&mut self, lib: PathBuf) -> &mut Self {
        self.static_libs.push(lib);
        self
    }

    pub fn link_lib(&mut self, lib: &str) -> &mut Self {
        self.dynamic_libs.push(lib.to_string());
        self
    }
}

/// Windows 子系统
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Subsystem {
    Console,
    Windows,
    Native,
    Posix,
    EfiApplication,
}

impl Subsystem {
    pub fn as_msvc_flag(&self) -> &'static str {
        match self {
            Self::Console => "/SUBSYSTEM:CONSOLE",
            Self::Windows => "/SUBSYSTEM:WINDOWS",
            Self::Native => "/SUBSYSTEM:NATIVE",
            Self::Posix => "/SUBSYSTEM:POSIX",
            Self::EfiApplication => "/SUBSYSTEM:EFI_APPLICATION",
        }
    }
}

/// 编译结果
#[derive(Debug, Clone)]
pub struct CompileResult {
    /// 是否成功
    pub success: bool,
    /// 输出文件
    pub output_file: Option<PathBuf>,
    /// 编译耗时 (毫秒)
    pub duration_ms: u64,
    /// 诊断信息
    pub diagnostics: Vec<Diagnostic>,
    /// 依赖文件列表 (从编译器输出解析)
    pub dependencies: Vec<PathBuf>,
}

impl CompileResult {
    pub fn success(output: PathBuf, duration_ms: u64) -> Self {
        Self {
            success: true,
            output_file: Some(output),
            duration_ms,
            diagnostics: Vec::new(),
            dependencies: Vec::new(),
        }
    }

    pub fn failure(diagnostics: Vec<Diagnostic>, duration_ms: u64) -> Self {
        Self {
            success: false,
            output_file: None,
            duration_ms,
            diagnostics,
            dependencies: Vec::new(),
        }
    }

    pub fn error_count(&self) -> usize {
        self.diagnostics
            .iter()
            .filter(|d| d.severity == DiagnosticSeverity::Error)
            .count()
    }

    pub fn warning_count(&self) -> usize {
        self.diagnostics
            .iter()
            .filter(|d| d.severity == DiagnosticSeverity::Warning)
            .count()
    }
}

/// 链接结果
#[derive(Debug, Clone)]
pub struct LinkResult {
    /// 是否成功
    pub success: bool,
    /// 输出文件
    pub output_file: Option<PathBuf>,
    /// 导入库文件
    pub import_lib: Option<PathBuf>,
    /// PDB 文件
    pub pdb_file: Option<PathBuf>,
    /// 链接耗时 (毫秒)
    pub duration_ms: u64,
    /// 诊断信息
    pub diagnostics: Vec<Diagnostic>,
}

impl LinkResult {
    pub fn success(output: PathBuf, duration_ms: u64) -> Self {
        Self {
            success: true,
            output_file: Some(output),
            import_lib: None,
            pdb_file: None,
            duration_ms,
            diagnostics: Vec::new(),
        }
    }

    pub fn failure(diagnostics: Vec<Diagnostic>, duration_ms: u64) -> Self {
        Self {
            success: false,
            output_file: None,
            import_lib: None,
            pdb_file: None,
            duration_ms,
            diagnostics,
        }
    }
}

//=============================================================================
// 编译器 Trait
//=============================================================================

/// 编译器抽象接口
pub trait Compiler: Send + Sync {
    /// 获取编译器名称
    fn name(&self) -> &str;

    /// 获取编译器版本
    fn version(&self) -> &str;

    /// 获取支持的平台
    fn supported_platforms(&self) -> &[Platform];

    /// 编译单个源文件
    fn compile(&self, unit: &CompileUnit, config: &CompilerConfig) -> Result<CompileResult>;

    /// 批量编译 (默认串行，子类可覆盖实现并行)
    fn compile_batch(
        &self,
        units: &[CompileUnit],
        config: &CompilerConfig,
    ) -> Vec<Result<CompileResult>> {
        units
            .iter()
            .map(|unit| self.compile(unit, config))
            .collect()
    }

    /// 创建预编译头
    fn create_pch(
        &self,
        header: &Path,
        output: &Path,
        config: &CompilerConfig,
    ) -> Result<CompileResult>;

    /// 生成依赖文件
    fn generate_dependencies(
        &self,
        unit: &CompileUnit,
        config: &CompilerConfig,
    ) -> Result<Vec<PathBuf>>;

    /// 获取默认包含路径
    fn default_include_paths(&self) -> Vec<PathBuf>;

    /// 获取预定义宏
    fn predefined_macros(&self) -> HashMap<String, String>;
}

/// 链接器抽象接口
pub trait Linker: Send + Sync {
    /// 获取链接器名称
    fn name(&self) -> &str;

    /// 链接生成目标文件
    fn link(&self, unit: &LinkUnit, config: &LinkerConfig) -> Result<LinkResult>;

    /// 创建静态库
    fn create_static_lib(
        &self,
        objects: &[PathBuf],
        output: &Path,
        config: &LinkerConfig,
    ) -> Result<LinkResult>;

    /// 获取默认库路径
    fn default_lib_paths(&self) -> Vec<PathBuf>;
}

/// 编译器配置
#[derive(Debug, Clone)]
pub struct CompilerConfig {
    /// 构建配置
    pub configuration: BuildConfiguration,
    /// 目标架构
    pub architecture: Architecture,
    /// 目标平台
    pub platform: Platform,
    /// 是否启用 RTTI
    pub enable_rtti: bool,
    /// 是否启用异常
    pub enable_exceptions: bool,
    /// 是否启用多线程运行时
    pub multithreaded_runtime: bool,
    /// 是否使用动态运行时 (MD vs MT)
    pub dynamic_runtime: bool,
    /// 是否启用地址消毒器
    pub enable_asan: bool,
    /// 是否启用未定义行为消毒器
    pub enable_ubsan: bool,
    /// 是否生成代码覆盖率数据
    pub enable_coverage: bool,
    /// 是否启用链接时优化
    pub enable_lto: bool,
    /// 自定义编译器路径
    pub compiler_path: Option<PathBuf>,
    /// 环境变量
    pub environment: HashMap<String, String>,
}

impl Default for CompilerConfig {
    fn default() -> Self {
        Self {
            configuration: BuildConfiguration::Development,
            architecture: Architecture::host(),
            platform: Platform::host(),
            enable_rtti: true,
            enable_exceptions: true,
            multithreaded_runtime: true,
            dynamic_runtime: true,
            enable_asan: false,
            enable_ubsan: false,
            enable_coverage: false,
            enable_lto: false,
            compiler_path: None,
            environment: HashMap::new(),
        }
    }
}

/// 链接器配置
#[derive(Debug, Clone)]
pub struct LinkerConfig {
    /// 构建配置
    pub configuration: BuildConfiguration,
    /// 目标架构
    pub architecture: Architecture,
    /// 目标平台
    pub platform: Platform,
    /// 是否生成调试信息
    pub debug_info: bool,
    /// 是否启用增量链接
    pub incremental: bool,
    /// 是否启用链接时优化
    pub enable_lto: bool,
    /// 是否生成 map 文件
    pub generate_map: bool,
    /// 自定义链接器路径
    pub linker_path: Option<PathBuf>,
    /// 堆栈大小
    pub stack_size: Option<usize>,
    /// 堆大小
    pub heap_size: Option<usize>,
}

impl Default for LinkerConfig {
    fn default() -> Self {
        Self {
            configuration: BuildConfiguration::Development,
            architecture: Architecture::host(),
            platform: Platform::host(),
            debug_info: true,
            incremental: true,
            enable_lto: false,
            generate_map: false,
            linker_path: None,
            stack_size: None,
            heap_size: None,
        }
    }
}

//=============================================================================
// 构建上下文
//=============================================================================

/// 构建上下文 - 包含构建所需的所有信息
#[derive(Debug, Clone)]
pub struct BuildContext {
    /// 项目根目录
    pub project_root: PathBuf,
    /// 源代码目录
    pub source_dir: PathBuf,
    /// 中间文件目录
    pub intermediate_dir: PathBuf,
    /// 输出目录
    pub output_dir: PathBuf,
    /// 构建配置
    pub configuration: BuildConfiguration,
    /// 目标架构
    pub architecture: Architecture,
    /// 目标平台
    pub platform: Platform,
    /// 并行编译任务数
    pub parallel_jobs: usize,
    /// 是否详细输出
    pub verbose: bool,
    /// 是否为重新构建 (清除缓存)
    pub rebuild: bool,
}

impl BuildContext {
    pub fn new(project_root: PathBuf) -> Self {
        let source_dir = project_root.join("Source");
        let intermediate_dir = project_root.join("Intermediate");
        let output_dir = project_root.join("Binaries");

        Self {
            project_root,
            source_dir,
            intermediate_dir,
            output_dir,
            configuration: BuildConfiguration::Development,
            architecture: Architecture::host(),
            platform: Platform::host(),
            parallel_jobs: num_cpus::get(),
            verbose: false,
            rebuild: false,
        }
    }

    /// 获取模块的中间目录
    pub fn module_intermediate_dir(&self, module_name: &str) -> PathBuf {
        self.intermediate_dir
            .join(self.configuration.name())
            .join(self.platform.name())
            .join(module_name)
    }

    /// 获取模块的输出目录
    pub fn module_output_dir(&self, module_name: &str) -> PathBuf {
        self.output_dir
            .join(self.configuration.name())
            .join(self.platform.name())
    }
}

//=============================================================================
// 辅助函数
//=============================================================================

/// 获取默认编译器
pub fn get_default_compiler(platform: Platform) -> Result<Box<dyn Compiler>> {
    match platform {
        Platform::Windows => {
            let msvc = msvc::MsvcCompiler::detect()?;
            Ok(Box::new(msvc))
        }
        Platform::Linux => {
            // 优先 Clang，回退 GCC
            if let Ok(clang) = clang::ClangCompiler::detect() {
                Ok(Box::new(clang))
            } else {
                let gcc = gcc::GccCompiler::detect()?;
                Ok(Box::new(gcc))
            }
        }
        Platform::MacOS => {
            let clang = clang::ClangCompiler::detect()?;
            Ok(Box::new(clang))
        }
    }
}

/// 获取默认链接器
pub fn get_default_linker(platform: Platform) -> Result<Box<dyn Linker>> {
    match platform {
        Platform::Windows => {
            let msvc_linker = msvc::MsvcLinker::detect()?;
            Ok(Box::new(msvc_linker))
        }
        Platform::Linux | Platform::MacOS => {
            let lld = linker::LldLinker::detect()?;
            Ok(Box::new(lld))
        }
    }
}
