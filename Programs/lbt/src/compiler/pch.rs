/*******************************************************************************
 * 文件: compiler/pch.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   预编译头 (PCH) 管理系统
 *   - PCH 文件生成与更新
 *   - PCH 依赖追踪
 *   - 多 PCH 支持 (共享 PCH / 模块 PCH)
 *   - PCH 兼容性检查
 *
 * 设计哲学:
 *   1. 自动化 - 自动检测和生成最优 PCH
 *   2. 增量性 - 只在必要时重新生成 PCH
 *   3. 兼容性 - 处理不同编译器的 PCH 格式差异
 *
 * 技术特性:
 *   - MSVC PCH (.pch)
 *   - Clang PCH (.pch / .gch)
 *   - GCC PCH (.gch)
 *   - 共享 PCH 优化
 *   - PCH 内容分析
 *
 ******************************************************************************/

use anyhow::{Context, Result};
use std::collections::{HashMap, HashSet};
use std::fs;
use std::path::{Path, PathBuf};
use std::time::SystemTime;

use super::{BuildConfiguration, Platform};

//=============================================================================
// PCH 类型
//=============================================================================

/// PCH 类型
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum PchType {
    /// 不使用 PCH
    None,
    /// 使用 PCH (源文件包含 PCH 头)
    Use,
    /// 创建 PCH (编译 PCH 头)
    Create,
}

/// PCH 格式 (编译器相关)
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PchFormat {
    /// MSVC 格式 (.pch)
    Msvc,
    /// Clang 格式 (.pch)
    Clang,
    /// GCC 格式 (.gch)
    Gcc,
}

impl PchFormat {
    /// 获取 PCH 文件扩展名
    pub fn extension(&self) -> &'static str {
        match self {
            Self::Msvc => "pch",
            Self::Clang => "pch",
            Self::Gcc => "gch",
        }
    }

    /// 从平台推断
    pub fn from_platform(platform: Platform) -> Self {
        match platform {
            Platform::Windows => Self::Msvc,
            Platform::Linux => Self::Gcc,
            Platform::MacOS => Self::Clang,
        }
    }
}

//=============================================================================
// PCH 配置
//=============================================================================

/// PCH 配置
#[derive(Debug, Clone)]
pub struct PchConfig {
    /// 是否启用 PCH
    pub enabled: bool,
    /// PCH 头文件路径
    pub header_file: PathBuf,
    /// PCH 源文件路径 (用于创建 PCH)
    pub source_file: Option<PathBuf>,
    /// PCH 输出路径
    pub output_file: PathBuf,
    /// 强制包含的头文件
    pub force_includes: Vec<PathBuf>,
    /// 排除使用 PCH 的文件
    pub exclude_files: HashSet<PathBuf>,
    /// PCH 内存限制 (字节)
    pub memory_limit: Option<usize>,
    /// 是否使用共享 PCH
    pub shared: bool,
    /// 共享 PCH 名称
    pub shared_name: Option<String>,
}

impl Default for PchConfig {
    fn default() -> Self {
        Self {
            enabled: true,
            header_file: PathBuf::from("PCH.h"),
            source_file: None,
            output_file: PathBuf::from("Intermediate/PCH.pch"),
            force_includes: Vec::new(),
            exclude_files: HashSet::new(),
            memory_limit: None,
            shared: false,
            shared_name: None,
        }
    }
}

impl PchConfig {
    pub fn new(header_file: PathBuf, output_file: PathBuf) -> Self {
        Self {
            header_file,
            output_file,
            ..Default::default()
        }
    }

    /// 设置 PCH 源文件
    pub fn with_source(&mut self, source: PathBuf) -> &mut Self {
        self.source_file = Some(source);
        self
    }

    /// 添加强制包含
    pub fn force_include(&mut self, header: PathBuf) -> &mut Self {
        self.force_includes.push(header);
        self
    }

    /// 排除文件
    pub fn exclude(&mut self, file: PathBuf) -> &mut Self {
        self.exclude_files.insert(file);
        self
    }

    /// 检查文件是否应使用 PCH
    pub fn should_use_pch(&self, source_file: &Path) -> bool {
        self.enabled && !self.exclude_files.contains(source_file)
    }

    /// 设置为共享 PCH
    pub fn make_shared(&mut self, name: &str) -> &mut Self {
        self.shared = true;
        self.shared_name = Some(name.to_string());
        self
    }
}

//=============================================================================
// PCH 信息
//=============================================================================

/// PCH 文件信息
#[derive(Debug, Clone)]
pub struct PchInfo {
    /// PCH 头文件
    pub header_file: PathBuf,
    /// PCH 二进制文件
    pub pch_file: PathBuf,
    /// PCH 对象文件 (MSVC)
    pub obj_file: Option<PathBuf>,
    /// 创建时间
    pub created_at: Option<SystemTime>,
    /// 文件大小
    pub file_size: u64,
    /// 依赖的头文件
    pub dependencies: Vec<PathBuf>,
    /// 包含的宏定义
    pub defines: Vec<String>,
    /// PCH 格式
    pub format: PchFormat,
    /// 是否有效
    pub is_valid: bool,
}

impl PchInfo {
    pub fn new(header_file: PathBuf, pch_file: PathBuf, format: PchFormat) -> Self {
        let (created_at, file_size) = if pch_file.exists() {
            let metadata = fs::metadata(&pch_file).ok();
            (
                metadata.as_ref().and_then(|m| m.modified().ok()),
                metadata.map(|m| m.len()).unwrap_or(0),
            )
        } else {
            (None, 0)
        };

        Self {
            header_file,
            pch_file,
            obj_file: None,
            created_at,
            file_size,
            dependencies: Vec::new(),
            defines: Vec::new(),
            format,
            is_valid: false,
        }
    }

    /// 检查 PCH 是否需要重建
    pub fn needs_rebuild(&self, header_modified: Option<SystemTime>) -> bool {
        if !self.pch_file.exists() {
            return true;
        }

        if let (Some(pch_time), Some(header_time)) = (self.created_at, header_modified) {
            return header_time > pch_time;
        }

        true
    }

    /// 检查依赖是否有变化
    pub fn check_dependencies(&self) -> bool {
        if let Some(pch_time) = self.created_at {
            for dep in &self.dependencies {
                if let Ok(metadata) = fs::metadata(dep) {
                    if let Ok(dep_time) = metadata.modified() {
                        if dep_time > pch_time {
                            return true;
                        }
                    }
                }
            }
        }
        false
    }
}

//=============================================================================
// PCH 生成器
//=============================================================================

/// PCH 生成器
pub struct PchGenerator {
    /// 配置
    config: PchConfig,
    /// PCH 格式
    format: PchFormat,
    /// 包含目录
    include_dirs: Vec<PathBuf>,
    /// 预定义宏
    defines: Vec<(String, Option<String>)>,
    /// 构建配置
    build_config: BuildConfiguration,
}

impl PchGenerator {
    pub fn new(config: PchConfig, format: PchFormat) -> Self {
        Self {
            config,
            format,
            include_dirs: Vec::new(),
            defines: Vec::new(),
            build_config: BuildConfiguration::Debug,
        }
    }

    /// 设置包含目录
    pub fn include_dirs(&mut self, dirs: Vec<PathBuf>) -> &mut Self {
        self.include_dirs = dirs;
        self
    }

    /// 添加宏定义
    pub fn define(&mut self, name: &str, value: Option<&str>) -> &mut Self {
        self.defines
            .push((name.to_string(), value.map(|s| s.to_string())));
        self
    }

    /// 设置构建配置
    pub fn build_config(&mut self, config: BuildConfiguration) -> &mut Self {
        self.build_config = config;
        self
    }

    /// 生成 PCH 头文件内容
    pub fn generate_header_content(&self, headers: &[PathBuf]) -> String {
        let mut content = String::with_capacity(4096);

        // 文件头注释
        content.push_str(
            "/*******************************************************************************\n",
        );
        content.push_str(" * 文件: PCH.h\n");
        content.push_str(" * 创建时间: Auto-generated by LBT\n");
        content.push_str(" * \n");
        content.push_str(" * 功能描述:\n");
        content.push_str(" *   预编译头文件 - 包含常用的稳定头文件\n");
        content.push_str(" *   此文件由 LBT 自动生成，请勿手动修改\n");
        content.push_str(" * \n");
        content.push_str(
            " ******************************************************************************/\n\n",
        );

        // 防止重复包含
        content.push_str("#pragma once\n\n");

        // 平台检测
        content.push_str("// 平台检测\n");
        content.push_str("#ifdef _WIN32\n");
        content.push_str("    #define LIMX_PLATFORM_WINDOWS 1\n");
        content.push_str("#elif defined(__linux__)\n");
        content.push_str("    #define LIMX_PLATFORM_LINUX 1\n");
        content.push_str("#elif defined(__APPLE__)\n");
        content.push_str("    #define LIMX_PLATFORM_MACOS 1\n");
        content.push_str("#endif\n\n");

        // Windows 头文件配置
        content.push_str("#if LIMX_PLATFORM_WINDOWS\n");
        content.push_str("    #ifndef WIN32_LEAN_AND_MEAN\n");
        content.push_str("        #define WIN32_LEAN_AND_MEAN\n");
        content.push_str("    #endif\n");
        content.push_str("    #ifndef NOMINMAX\n");
        content.push_str("        #define NOMINMAX\n");
        content.push_str("    #endif\n");
        content.push_str("#endif\n\n");

        // 包含用户指定的头文件
        content.push_str("// 项目头文件\n");
        for header in headers {
            let header_path = header.display().to_string().replace('\\', "/");
            content.push_str(&format!("#include \"{}\"\n", header_path));
        }

        content
    }

    /// 生成 PCH 源文件内容 (MSVC 需要)
    pub fn generate_source_content(&self) -> String {
        let header_name = self
            .config
            .header_file
            .file_name()
            .and_then(|n| n.to_str())
            .unwrap_or("PCH.h");

        format!(
            "/*******************************************************************************\n\
             * 文件: PCH.cpp\n\
             * 创建时间: Auto-generated by LBT\n\
             * \n\
             * 功能描述:\n\
             *   预编译头源文件 - 用于生成 PCH 二进制\n\
             *   此文件由 LBT 自动生成，请勿手动修改\n\
             * \n\
             ******************************************************************************/\n\n\
             #include \"{}\"\n",
            header_name
        )
    }

    /// 获取 MSVC PCH 编译参数
    pub fn msvc_create_args(&self) -> Vec<String> {
        let mut args = Vec::new();

        // 创建 PCH
        args.push("/Yc".to_string());
        args.push(format!("/Fp{}", self.config.output_file.display()));

        // PCH 头文件
        let header_name = self
            .config
            .header_file
            .file_name()
            .and_then(|n| n.to_str())
            .unwrap_or("PCH.h");
        args.push(format!("/Yu{}", header_name));

        args
    }

    /// 获取 MSVC PCH 使用参数
    pub fn msvc_use_args(&self) -> Vec<String> {
        let mut args = Vec::new();

        // 使用 PCH
        let header_name = self
            .config
            .header_file
            .file_name()
            .and_then(|n| n.to_str())
            .unwrap_or("PCH.h");
        args.push(format!("/Yu{}", header_name));
        args.push(format!("/Fp{}", self.config.output_file.display()));

        // 强制包含
        args.push(format!("/FI{}", header_name));

        args
    }

    /// 获取 Clang PCH 创建参数
    pub fn clang_create_args(&self) -> Vec<String> {
        let mut args = Vec::new();

        args.push("-x".to_string());
        args.push("c++-header".to_string());
        args.push("-o".to_string());
        args.push(self.config.output_file.display().to_string());

        args
    }

    /// 获取 Clang PCH 使用参数
    pub fn clang_use_args(&self) -> Vec<String> {
        let mut args = Vec::new();

        args.push("-include-pch".to_string());
        args.push(self.config.output_file.display().to_string());

        // 强制包含头文件
        args.push("-include".to_string());
        args.push(self.config.header_file.display().to_string());

        args
    }

    /// 获取 GCC PCH 创建参数
    pub fn gcc_create_args(&self) -> Vec<String> {
        let mut args = Vec::new();

        args.push("-x".to_string());
        args.push("c++-header".to_string());
        args.push("-o".to_string());

        // GCC PCH 输出到 .gch 文件
        let gch_path = self.config.header_file.with_extension("h.gch");
        args.push(gch_path.display().to_string());

        args
    }

    /// 获取 GCC PCH 使用参数
    pub fn gcc_use_args(&self) -> Vec<String> {
        let mut args = Vec::new();

        // GCC 自动查找 .gch 文件
        args.push("-include".to_string());
        args.push(self.config.header_file.display().to_string());

        args
    }

    /// 写入 PCH 头文件
    pub fn write_header(&self, headers: &[PathBuf]) -> Result<PathBuf> {
        let content = self.generate_header_content(headers);

        if let Some(parent) = self.config.header_file.parent() {
            fs::create_dir_all(parent)?;
        }

        fs::write(&self.config.header_file, content).with_context(|| {
            format!("写入 PCH 头文件失败: {}", self.config.header_file.display())
        })?;

        Ok(self.config.header_file.clone())
    }

    /// 写入 PCH 源文件 (MSVC)
    pub fn write_source(&self) -> Result<PathBuf> {
        let content = self.generate_source_content();

        let source_path = self
            .config
            .source_file
            .clone()
            .unwrap_or_else(|| self.config.header_file.with_extension("cpp"));

        if let Some(parent) = source_path.parent() {
            fs::create_dir_all(parent)?;
        }

        fs::write(&source_path, content)
            .with_context(|| format!("写入 PCH 源文件失败: {}", source_path.display()))?;

        Ok(source_path)
    }
}

//=============================================================================
// PCH 管理器
//=============================================================================

/// PCH 管理器 - 管理项目中的所有 PCH
pub struct PchManager {
    /// 模块 PCH 配置 (模块名 -> PCH 配置)
    module_pchs: HashMap<String, PchConfig>,
    /// 共享 PCH 配置
    shared_pchs: HashMap<String, PchConfig>,
    /// PCH 信息缓存
    pch_info_cache: HashMap<PathBuf, PchInfo>,
    /// 默认 PCH 格式
    default_format: PchFormat,
}

impl PchManager {
    pub fn new(format: PchFormat) -> Self {
        Self {
            module_pchs: HashMap::new(),
            shared_pchs: HashMap::new(),
            pch_info_cache: HashMap::new(),
            default_format: format,
        }
    }

    /// 注册模块 PCH
    pub fn register_module_pch(&mut self, module_name: &str, config: PchConfig) {
        self.module_pchs.insert(module_name.to_string(), config);
    }

    /// 注册共享 PCH
    pub fn register_shared_pch(&mut self, name: &str, config: PchConfig) {
        self.shared_pchs.insert(name.to_string(), config);
    }

    /// 获取模块的 PCH 配置
    pub fn get_module_pch(&self, module_name: &str) -> Option<&PchConfig> {
        self.module_pchs.get(module_name)
    }

    /// 获取共享 PCH 配置
    pub fn get_shared_pch(&self, name: &str) -> Option<&PchConfig> {
        self.shared_pchs.get(name)
    }

    /// 获取源文件应使用的 PCH
    pub fn get_pch_for_source(&self, source_file: &Path, module_name: &str) -> Option<&PchConfig> {
        // 先检查模块 PCH
        if let Some(config) = self.module_pchs.get(module_name) {
            if config.should_use_pch(source_file) {
                return Some(config);
            }
        }

        // 检查是否有适用的共享 PCH
        // 这里可以添加更复杂的匹配逻辑
        None
    }

    /// 检查 PCH 是否需要重建
    pub fn needs_rebuild(&mut self, pch_file: &Path) -> bool {
        if let Some(info) = self.pch_info_cache.get(pch_file) {
            return info.needs_rebuild(None) || info.check_dependencies();
        }
        true
    }

    /// 更新 PCH 信息缓存
    pub fn update_cache(&mut self, pch_file: PathBuf, info: PchInfo) {
        self.pch_info_cache.insert(pch_file, info);
    }

    /// 获取所有需要重建的 PCH
    pub fn get_outdated_pchs(&self) -> Vec<&PchConfig> {
        let mut outdated = Vec::new();

        for config in self.module_pchs.values() {
            if !config.output_file.exists() {
                outdated.push(config);
                continue;
            }

            if let Some(info) = self.pch_info_cache.get(&config.output_file) {
                if info.needs_rebuild(None) || info.check_dependencies() {
                    outdated.push(config);
                }
            } else {
                outdated.push(config);
            }
        }

        for config in self.shared_pchs.values() {
            if !config.output_file.exists() {
                outdated.push(config);
            }
        }

        outdated
    }

    /// 清理所有 PCH 文件
    pub fn clean_all(&self) -> Result<usize> {
        let mut count = 0;

        for config in self.module_pchs.values() {
            if config.output_file.exists() {
                fs::remove_file(&config.output_file)?;
                count += 1;
            }
        }

        for config in self.shared_pchs.values() {
            if config.output_file.exists() {
                fs::remove_file(&config.output_file)?;
                count += 1;
            }
        }

        Ok(count)
    }

    /// 获取统计信息
    pub fn stats(&self) -> PchStats {
        let total_pchs = self.module_pchs.len() + self.shared_pchs.len();

        let valid_pchs = self
            .module_pchs
            .values()
            .chain(self.shared_pchs.values())
            .filter(|c| c.output_file.exists())
            .count();

        let total_size: u64 = self
            .module_pchs
            .values()
            .chain(self.shared_pchs.values())
            .filter_map(|c| fs::metadata(&c.output_file).ok())
            .map(|m| m.len())
            .sum();

        PchStats {
            total_pchs,
            valid_pchs,
            module_pchs: self.module_pchs.len(),
            shared_pchs: self.shared_pchs.len(),
            total_size_bytes: total_size,
        }
    }
}

/// PCH 统计
#[derive(Debug, Clone)]
pub struct PchStats {
    pub total_pchs: usize,
    pub valid_pchs: usize,
    pub module_pchs: usize,
    pub shared_pchs: usize,
    pub total_size_bytes: u64,
}

impl PchStats {
    pub fn print(&self) {
        println!("\nPCH 统计:");
        println!(
            "  PCH 总数: {} (模块: {}, 共享: {})",
            self.total_pchs, self.module_pchs, self.shared_pchs
        );
        println!("  有效 PCH: {}", self.valid_pchs);
        println!("  总大小: {} MB", self.total_size_bytes / (1024 * 1024));
    }
}

//=============================================================================
// PCH 分析器
//=============================================================================

/// PCH 分析器 - 分析头文件并建议最优 PCH 配置
pub struct PchAnalyzer {
    /// 头文件使用频率
    header_frequency: HashMap<PathBuf, usize>,
    /// 头文件大小
    header_sizes: HashMap<PathBuf, usize>,
    /// 头文件依赖关系
    header_deps: HashMap<PathBuf, HashSet<PathBuf>>,
}

impl PchAnalyzer {
    pub fn new() -> Self {
        Self {
            header_frequency: HashMap::new(),
            header_sizes: HashMap::new(),
            header_deps: HashMap::new(),
        }
    }

    /// 分析源文件使用的头文件
    pub fn analyze_source(&mut self, _source_file: &Path, included_headers: &[PathBuf]) {
        for header in included_headers {
            *self.header_frequency.entry(header.clone()).or_default() += 1;

            if !self.header_sizes.contains_key(header) {
                let size = fs::metadata(header).map(|m| m.len() as usize).unwrap_or(0);
                self.header_sizes.insert(header.clone(), size);
            }
        }
    }

    /// 获取推荐的 PCH 头文件列表
    pub fn recommend_pch_headers(&self, min_frequency: usize, max_headers: usize) -> Vec<PathBuf> {
        let mut headers: Vec<_> = self
            .header_frequency
            .iter()
            .filter(|(_, &freq)| freq >= min_frequency)
            .collect();

        // 按频率和大小排序
        headers.sort_by(|(h1, f1), (h2, f2)| {
            let score1 = **f1 * self.header_sizes.get(*h1).unwrap_or(&0);
            let score2 = **f2 * self.header_sizes.get(*h2).unwrap_or(&0);
            score2.cmp(&score1)
        });

        headers
            .into_iter()
            .take(max_headers)
            .map(|(h, _)| h.clone())
            .collect()
    }

    /// 估算 PCH 带来的编译时间节省
    pub fn estimate_time_saving(&self, pch_headers: &[PathBuf], source_count: usize) -> f64 {
        let total_header_size: usize = pch_headers
            .iter()
            .filter_map(|h| self.header_sizes.get(h))
            .sum();

        // 粗略估算：每个源文件节省的时间与 PCH 大小成正比
        // 假设每 MB 头文件节省 0.1 秒编译时间
        let time_per_source = (total_header_size as f64 / (1024.0 * 1024.0)) * 0.1;
        time_per_source * source_count as f64
    }
}

impl Default for PchAnalyzer {
    fn default() -> Self {
        Self::new()
    }
}
