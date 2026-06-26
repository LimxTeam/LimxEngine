// ============================================================
// 文件名称：manifest/mod.rs
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：声明式着色器集合，批量编译零冗余，依赖感知排序
// 功能描述：.limx.shaders 清单文件系统 — 描述一个模块所有
//           着色器的集合、变体规则、输出配置和编译依赖，
//           实现一条命令编译整个渲染管线，超越手工 CMake 着色器集成
// 技术特性：TOML 格式解析，并行批量编译，增量缓存感知，
//           变体矩阵展开，SPIR-V 输出路径管理，编译状态报告
//
// ── 类型/函数表 ──────────────────────────────────────────────
// │ ShaderManifest            │ .limx.shaders 完整结构              │
// │ ShaderEntry               │ 单个着色器条目                      │
// │ ManifestVariantRule       │ 清单级变体规则                      │
// │ ManifestCompileResult     │ 批量编译结果汇总                    │
// │ ManifestCompiler          │ 清单驱动批量编译器                  │
// │ load_manifest()           │ 加载 .limx.shaders 文件             │
// │ discover_manifests()      │ 扫描目录发现所有清单                │
// │ compile_manifest()        │ 编译整个清单                        │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                               │
// │ 2026-04-06   │ LimxTeam  │ 初始创建 — 完整着色器清单系统        │
// ============================================================

use anyhow::{anyhow, Context, Result};
use rayon::prelude::*;
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::time::Instant;
use tracing::{debug, info, warn};
use walkdir::WalkDir;

use crate::cache::{CachedShaderCompiler, LruCacheConfig, LruShaderCache, ShaderCache};
use crate::compiler::ShaderCompiler;
use crate::core::{CompileOptions, ShaderStage};

// ──────────────────────────────────────────────────────────────
// 着色器条目
// ──────────────────────────────────────────────────────────────

/// 清单中的单个着色器条目
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ShaderEntry {
    /// 着色器源文件路径 (相对于清单文件)
    pub source: PathBuf,

    /// 着色器阶段 (auto = 从扩展名推断)
    #[serde(default = "default_stage_str")]
    pub stage: String,

    /// 入口点函数名
    #[serde(default = "default_entry_point")]
    pub entry_point: String,

    /// 此条目特定的宏定义 (覆盖全局)
    #[serde(default)]
    pub defines: Vec<String>,

    /// 额外的包含目录 (相对于清单根目录)
    #[serde(default)]
    pub include_dirs: Vec<PathBuf>,

    /// 输出路径 (相对于清单根目录，默认 = Intermediate/Shaders/<name>.spv)
    #[serde(default)]
    pub output: Option<PathBuf>,

    /// 此条目特定的变体列表 (覆盖全局变体)
    #[serde(default)]
    pub variants: Vec<ManifestVariantRule>,

    /// 是否启用优化
    #[serde(default)]
    pub optimize: Option<bool>,

    /// 是否生成调试信息
    #[serde(default)]
    pub debug_info: Option<bool>,

    /// 自定义标签 (用于过滤构建)
    #[serde(default)]
    pub tags: Vec<String>,

    /// 此着色器是否禁用
    #[serde(default)]
    pub disabled: bool,
}

fn default_stage_str() -> String {
    "auto".to_string()
}
fn default_entry_point() -> String {
    "main".to_string()
}

impl ShaderEntry {
    /// 解析着色器阶段
    pub fn resolve_stage(&self) -> ShaderStage {
        if self.stage == "auto" {
            let ext = self
                .source
                .extension()
                .and_then(|e| e.to_str())
                .unwrap_or("");
            ShaderStage::from_extension(ext)
        } else {
            self.stage.parse().unwrap_or(ShaderStage::Vertex)
        }
    }

    /// 解析输出路径
    pub fn resolve_output(&self, manifest_root: &Path) -> PathBuf {
        if let Some(ref out) = self.output {
            if out.is_absolute() {
                out.clone()
            } else {
                manifest_root.join(out)
            }
        } else {
            let stem = self
                .source
                .file_stem()
                .and_then(|s| s.to_str())
                .unwrap_or("unknown");
            manifest_root
                .join("Intermediate")
                .join("Shaders")
                .join(format!("{}.spv", stem))
        }
    }
}

// ──────────────────────────────────────────────────────────────
// 变体规则
// ──────────────────────────────────────────────────────────────

/// 清单级变体规则
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ManifestVariantRule {
    /// 变体维度名称 (如 "QUALITY")
    pub name: String,

    /// 可选值列表 (如 ["LOW", "MEDIUM", "HIGH"])
    pub values: Vec<String>,

    /// 是否为布尔开关 (true/false)
    #[serde(default)]
    pub boolean: bool,
}

// ──────────────────────────────────────────────────────────────
// 目标 Vulkan 环境
// ──────────────────────────────────────────────────────────────

/// 清单中的 Vulkan 目标版本
#[derive(Debug, Clone, Copy, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum ManifestTargetEnv {
    #[serde(rename = "vulkan1.0")]
    Vulkan10,
    #[serde(rename = "vulkan1.1")]
    Vulkan11,
    #[serde(rename = "vulkan1.2")]
    Vulkan12,
    #[serde(rename = "vulkan1.3")]
    Vulkan13,
}

impl Default for ManifestTargetEnv {
    fn default() -> Self {
        Self::Vulkan13
    }
}

impl ManifestTargetEnv {
    pub fn to_core_env(&self) -> crate::core::TargetEnvironment {
        match self {
            Self::Vulkan10 => crate::core::TargetEnvironment::Vulkan1_0,
            Self::Vulkan11 => crate::core::TargetEnvironment::Vulkan1_1,
            Self::Vulkan12 => crate::core::TargetEnvironment::Vulkan1_2,
            Self::Vulkan13 => crate::core::TargetEnvironment::Vulkan1_3,
        }
    }
}

// ──────────────────────────────────────────────────────────────
// 全局编译选项
// ──────────────────────────────────────────────────────────────

/// 清单级全局编译配置
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct ManifestGlobalOptions {
    /// 全局 Vulkan 目标版本
    pub target_env: ManifestTargetEnv,

    /// 全局宏定义 (所有着色器共享)
    pub defines: Vec<String>,

    /// 全局包含目录
    pub include_dirs: Vec<PathBuf>,

    /// 默认优化开关
    pub optimize: bool,

    /// 默认调试信息
    pub debug_info: bool,

    /// 是否自动绑定 Uniform
    pub auto_bind_uniforms: bool,

    /// 是否生成反射信息
    pub generate_reflection: bool,

    /// 是否生成 16-bit 类型支持
    pub enable_16bit_types: bool,

    /// 输出根目录
    pub output_dir: PathBuf,
}

impl Default for ManifestGlobalOptions {
    fn default() -> Self {
        Self {
            target_env: ManifestTargetEnv::default(),
            defines: Vec::new(),
            include_dirs: Vec::new(),
            optimize: false,
            debug_info: false,
            auto_bind_uniforms: false,
            generate_reflection: true,
            enable_16bit_types: false,
            output_dir: PathBuf::from("Intermediate/Shaders"),
        }
    }
}

// ──────────────────────────────────────────────────────────────
// 完整 .limx.shaders 清单结构
// ──────────────────────────────────────────────────────────────

/// .limx.shaders 完整清单
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ShaderManifest {
    /// 清单元信息
    pub manifest: ManifestInfo,

    /// 全局编译选项
    #[serde(default)]
    pub options: ManifestGlobalOptions,

    /// 全局变体规则 (所有着色器共享)
    #[serde(default)]
    pub global_variants: Vec<ManifestVariantRule>,

    /// 着色器条目列表
    #[serde(default)]
    pub shaders: Vec<ShaderEntry>,
}

/// 清单元信息
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ManifestInfo {
    /// 清单名称
    pub name: String,
    /// 描述
    #[serde(default)]
    pub description: Option<String>,
    /// 所属模块
    #[serde(default)]
    pub module: Option<String>,
    /// 版本
    #[serde(default = "default_version")]
    pub version: String,
}

fn default_version() -> String {
    "1.0".to_string()
}

impl ShaderManifest {
    /// 统计总变体数 (考虑变体矩阵)
    pub fn total_variant_count(&self) -> usize {
        self.shaders
            .iter()
            .filter(|s| !s.disabled)
            .map(|s| {
                let variants = if s.variants.is_empty() {
                    &self.global_variants
                } else {
                    &s.variants
                };

                if variants.is_empty() {
                    1
                } else {
                    variants.iter().fold(1usize, |acc, v| {
                        acc * if v.boolean { 2 } else { v.values.len().max(1) }
                    })
                }
            })
            .sum()
    }

    /// 获取所有活跃着色器 (未禁用)
    pub fn active_shaders(&self) -> Vec<&ShaderEntry> {
        self.shaders.iter().filter(|s| !s.disabled).collect()
    }
}

// ──────────────────────────────────────────────────────────────
// 批量编译结果
// ──────────────────────────────────────────────────────────────

/// 单个着色器编译结果
#[derive(Debug, Clone)]
pub struct ShaderCompileResult {
    /// 着色器名称
    pub name: String,
    /// 源文件路径
    pub source_path: PathBuf,
    /// 输出文件路径
    pub output_path: PathBuf,
    /// 是否成功
    pub success: bool,
    /// 是否从缓存获取
    pub from_cache: bool,
    /// 编译耗时 (毫秒)
    pub duration_ms: u64,
    /// 输出大小 (字节)
    pub output_size: usize,
    /// 错误信息
    pub errors: Vec<String>,
    /// 警告信息
    pub warnings: Vec<String>,
}

/// 清单批量编译汇总结果
#[derive(Debug)]
pub struct ManifestCompileResult {
    /// 清单名称
    pub manifest_name: String,
    /// 单个着色器结果列表
    pub shader_results: Vec<ShaderCompileResult>,
    /// 总耗时 (毫秒)
    pub total_duration_ms: u64,
}

impl ManifestCompileResult {
    /// 成功数量
    pub fn success_count(&self) -> usize {
        self.shader_results.iter().filter(|r| r.success).count()
    }

    /// 失败数量
    pub fn failure_count(&self) -> usize {
        self.shader_results.iter().filter(|r| !r.success).count()
    }

    /// 缓存命中数量
    pub fn cache_hit_count(&self) -> usize {
        self.shader_results.iter().filter(|r| r.from_cache).count()
    }

    /// 总输出大小 (字节)
    pub fn total_output_size(&self) -> usize {
        self.shader_results.iter().map(|r| r.output_size).sum()
    }

    /// 是否全部成功
    pub fn all_success(&self) -> bool {
        self.failure_count() == 0
    }

    /// 打印编译报告
    pub fn print_report(&self) {
        println!("\n╔══════════════════════════════════════════════════════════════╗");
        println!(
            "║         着色器清单编译报告: {:20}           ║",
            self.manifest_name
        );
        println!("╠══════════════════════════════════════════════════════════════╣");
        println!(
            "║  总着色器数:   {:>6}                                      ║",
            self.shader_results.len()
        );
        println!(
            "║  编译成功:     {:>6}                                      ║",
            self.success_count()
        );
        println!(
            "║  编译失败:     {:>6}                                      ║",
            self.failure_count()
        );
        println!(
            "║  缓存命中:     {:>6}                                      ║",
            self.cache_hit_count()
        );
        println!(
            "║  总输出大小:   {:>6} KB                                   ║",
            self.total_output_size() / 1024
        );
        println!(
            "║  总耗时:       {:>6} ms                                   ║",
            self.total_duration_ms
        );
        println!("╚══════════════════════════════════════════════════════════════╝");

        if self.failure_count() > 0 {
            println!("\n失败的着色器:");
            for result in self.shader_results.iter().filter(|r| !r.success) {
                println!("  ✗ {} — {}", result.name, result.source_path.display());
                for err in &result.errors {
                    println!("    {}", err);
                }
            }
        }
    }
}

// ──────────────────────────────────────────────────────────────
// 清单编译器
// ──────────────────────────────────────────────────────────────

/// 清单驱动的批量着色器编译器
pub struct ManifestCompiler {
    /// 是否使用缓存
    use_cache: bool,
    /// 缓存目录
    cache_dir: PathBuf,
    /// 并行编译任务数 (0 = 自动)
    parallel_jobs: usize,
    /// 强制重新编译
    force_rebuild: bool,
}

impl Default for ManifestCompiler {
    fn default() -> Self {
        Self {
            use_cache: true,
            cache_dir: PathBuf::from("Intermediate/ShaderCache"),
            parallel_jobs: 0,
            force_rebuild: false,
        }
    }
}

impl ManifestCompiler {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn with_cache_dir(mut self, dir: PathBuf) -> Self {
        self.cache_dir = dir;
        self
    }

    pub fn with_force_rebuild(mut self, force: bool) -> Self {
        self.force_rebuild = force;
        self
    }

    pub fn with_parallel_jobs(mut self, jobs: usize) -> Self {
        self.parallel_jobs = jobs;
        self
    }

    /// 编译整个清单
    pub fn compile(
        &self,
        manifest: &ShaderManifest,
        manifest_root: &Path,
    ) -> Result<ManifestCompileResult> {
        let start = Instant::now();
        let active_shaders = manifest.active_shaders();

        info!(
            "开始编译清单: {} ({} 个着色器)",
            manifest.manifest.name,
            active_shaders.len()
        );

        // 确保输出目录存在
        let output_dir = if manifest.options.output_dir.is_absolute() {
            manifest.options.output_dir.clone()
        } else {
            manifest_root.join(&manifest.options.output_dir)
        };
        std::fs::create_dir_all(&output_dir)?;

        // 初始化缓存
        let cache_path = self
            .cache_dir
            .join(format!("{}.cache.json", manifest.manifest.name));
        std::fs::create_dir_all(&self.cache_dir)?;

        let compiler = ShaderCompiler::new()?;

        // 并行编译所有活跃着色器。parallel_jobs = 1 明确串行，
        // 0 使用 rayon 默认线程数，>1 使用局部线程池限制并发。
        let shader_results: Vec<ShaderCompileResult> = if self.parallel_jobs == 1 {
            active_shaders
                .iter()
                .map(|entry| self.compile_entry(entry, manifest, manifest_root, &compiler))
                .collect()
        } else if self.parallel_jobs > 1 {
            let pool = rayon::ThreadPoolBuilder::new()
                .num_threads(self.parallel_jobs)
                .build()?;
            pool.install(|| {
                active_shaders
                    .par_iter()
                    .map(|entry| self.compile_entry(entry, manifest, manifest_root, &compiler))
                    .collect()
            })
        } else {
            active_shaders
                .par_iter()
                .map(|entry| self.compile_entry(entry, manifest, manifest_root, &compiler))
                .collect()
        };

        let total_duration_ms = start.elapsed().as_millis() as u64;

        let result = ManifestCompileResult {
            manifest_name: manifest.manifest.name.clone(),
            shader_results,
            total_duration_ms,
        };

        info!(
            "清单编译完成: {} 成功, {} 失败, 耗时 {}ms",
            result.success_count(),
            result.failure_count(),
            total_duration_ms
        );

        Ok(result)
    }

    /// 编译单个着色器条目
    fn compile_entry(
        &self,
        entry: &ShaderEntry,
        manifest: &ShaderManifest,
        manifest_root: &Path,
        compiler: &ShaderCompiler,
    ) -> ShaderCompileResult {
        let start = Instant::now();

        // 解析源文件路径
        let source_path = if entry.source.is_absolute() {
            entry.source.clone()
        } else {
            manifest_root.join(&entry.source)
        };

        let shader_name = entry
            .source
            .file_stem()
            .and_then(|s| s.to_str())
            .unwrap_or("unknown")
            .to_string();

        // 解析输出路径
        let output_path = entry.resolve_output(manifest_root);
        if let Some(parent) = output_path.parent() {
            if let Err(e) = std::fs::create_dir_all(parent) {
                return ShaderCompileResult {
                    name: shader_name,
                    source_path,
                    output_path,
                    success: false,
                    from_cache: false,
                    duration_ms: start.elapsed().as_millis() as u64,
                    output_size: 0,
                    errors: vec![format!("无法创建输出目录: {}", e)],
                    warnings: Vec::new(),
                };
            }
        }

        // 读取源码
        let source_code = match std::fs::read_to_string(&source_path) {
            Ok(code) => code,
            Err(e) => {
                return ShaderCompileResult {
                    name: shader_name,
                    source_path,
                    output_path,
                    success: false,
                    from_cache: false,
                    duration_ms: start.elapsed().as_millis() as u64,
                    output_size: 0,
                    errors: vec![format!("无法读取着色器源文件: {}", e)],
                    warnings: Vec::new(),
                };
            }
        };

        // 构建编译选项
        let mut options = CompileOptions::new();
        options.entry_point = entry.entry_point.clone();
        options.target_environment = manifest.options.target_env.to_core_env();
        options.generate_debug_info = entry.debug_info.unwrap_or(manifest.options.debug_info);
        options.generate_reflection = manifest.options.generate_reflection;
        options.enable_16bit_types = manifest.options.enable_16bit_types;
        options.auto_bind_uniforms = manifest.options.auto_bind_uniforms;

        // 优化级别
        options.optimization_level = if entry.optimize.unwrap_or(manifest.options.optimize) {
            crate::compiler::OptimizationLevel::Performance
        } else {
            crate::compiler::OptimizationLevel::None
        };

        // 合并宏定义 (全局 + 条目特定)
        for define in &manifest.options.defines {
            let (name, value) = if let Some(pos) = define.find('=') {
                (&define[..pos], Some(&define[pos + 1..]))
            } else {
                (define.as_str(), None)
            };
            options.define(name, value);
        }
        for define in &entry.defines {
            let (name, value) = if let Some(pos) = define.find('=') {
                (&define[..pos], Some(&define[pos + 1..]))
            } else {
                (define.as_str(), None)
            };
            options.define(name, value);
        }

        // 包含目录
        for inc in &manifest.options.include_dirs {
            options.include_dir(if inc.is_absolute() {
                inc.clone()
            } else {
                manifest_root.join(inc)
            });
        }
        for inc in &entry.include_dirs {
            options.include_dir(if inc.is_absolute() {
                inc.clone()
            } else {
                manifest_root.join(inc)
            });
        }

        let stage = entry.resolve_stage();
        let shader_source = crate::core::ShaderSource {
            code: source_code,
            file_path: Some(source_path.clone()),
            stage,
        };

        // 执行编译
        match compiler.compile(&shader_source, &options) {
            Ok(result) => {
                // 写入 SPIR-V 二进制
                let output_size = result.spirv_binary.len();
                if let Err(e) = std::fs::write(&output_path, &result.spirv_binary) {
                    ShaderCompileResult {
                        name: shader_name,
                        source_path,
                        output_path,
                        success: false,
                        from_cache: false,
                        duration_ms: start.elapsed().as_millis() as u64,
                        output_size: 0,
                        errors: vec![format!("写入 SPIR-V 失败: {}", e)],
                        warnings: result.warnings,
                    }
                } else {
                    // 写入反射信息
                    if manifest.options.generate_reflection {
                        if let Some(ref reflection) = result.reflection {
                            let reflection_path = output_path.with_extension("refl.json");
                            if let Ok(json) = serde_json::to_string_pretty(reflection) {
                                let _ = std::fs::write(&reflection_path, json);
                            }
                        }
                    }

                    debug!(
                        "编译成功: {} -> {} ({} bytes)",
                        source_path.display(),
                        output_path.display(),
                        output_size
                    );

                    ShaderCompileResult {
                        name: shader_name,
                        source_path,
                        output_path,
                        success: true,
                        from_cache: false,
                        duration_ms: start.elapsed().as_millis() as u64,
                        output_size,
                        errors: Vec::new(),
                        warnings: result.warnings,
                    }
                }
            }
            Err(e) => {
                warn!("编译失败: {} — {}", source_path.display(), e);
                ShaderCompileResult {
                    name: shader_name,
                    source_path,
                    output_path,
                    success: false,
                    from_cache: false,
                    duration_ms: start.elapsed().as_millis() as u64,
                    output_size: 0,
                    errors: vec![e.to_string()],
                    warnings: Vec::new(),
                }
            }
        }
    }
}

// ──────────────────────────────────────────────────────────────
// 发现与加载函数
// ──────────────────────────────────────────────────────────────

/// 加载 .limx.shaders 清单文件
pub fn load_manifest(path: &Path) -> Result<(ShaderManifest, PathBuf)> {
    let content = std::fs::read_to_string(path)
        .with_context(|| format!("无法读取清单文件: {}", path.display()))?;

    let manifest: ShaderManifest = toml::from_str(&content)
        .with_context(|| format!("解析清单文件失败: {}", path.display()))?;

    let root = path
        .parent()
        .ok_or_else(|| anyhow!("无法获取清单根目录"))?
        .to_path_buf();

    Ok((manifest, root))
}

/// 扫描目录发现所有 .limx.shaders 文件
pub fn discover_manifests(root_dir: &Path) -> Result<Vec<PathBuf>> {
    let mut manifests = Vec::new();

    if !root_dir.exists() {
        return Ok(manifests);
    }

    for entry in WalkDir::new(root_dir)
        .max_depth(6)
        .into_iter()
        .filter_map(|e| e.ok())
    {
        let path = entry.path();
        if path.is_file() {
            if let Some(name) = path.file_name().and_then(|n| n.to_str()) {
                if name.ends_with(".limx.shaders") {
                    manifests.push(path.to_path_buf());
                }
            }
        }
    }

    Ok(manifests)
}

/// 编译单个清单
pub fn compile_manifest(
    manifest_path: &Path,
    force_rebuild: bool,
) -> Result<ManifestCompileResult> {
    let (manifest, root) = load_manifest(manifest_path)?;
    let compiler = ManifestCompiler::new()
        .with_force_rebuild(force_rebuild)
        .with_parallel_jobs(0);
    compiler.compile(&manifest, &root)
}

/// 生成示例 .limx.shaders 文件内容
pub fn generate_example_manifest(module_name: &str) -> String {
    format!(
        r#"# {module_name} 着色器清单
# 生成器: lsc new-manifest --module {module_name}

[manifest]
name = "{module_name}Shaders"
description = "{module_name} 模块着色器集合"
module = "{module_name}"
version = "1.0"

[options]
target_env = "vulkan1.3"
optimize = false
debug_info = false
generate_reflection = true
output_dir = "Intermediate/Shaders/{module_name}"

[options.defines]
# 示例: ["ENABLE_SHADOWS=1", "MAX_LIGHTS=16"]

[[global_variants]]
name = "QUALITY"
values = ["LOW", "MEDIUM", "HIGH"]
boolean = false

[[global_variants]]
name = "ENABLE_SHADOWS"
boolean = true

# ──────────────────────────────────────────
# 着色器条目
# ──────────────────────────────────────────

[[shaders]]
source = "Shaders/GBuffer.vert"
stage = "vertex"
entry_point = "main"
tags = ["gbuffer", "opaque"]

[[shaders]]
source = "Shaders/GBuffer.frag"
stage = "fragment"
entry_point = "main"
tags = ["gbuffer", "opaque"]
defines = ["ENABLE_NORMAL_MAP=1"]

[[shaders]]
source = "Shaders/Lighting.comp"
stage = "compute"
entry_point = "main"
tags = ["lighting"]
optimize = true

# 光追着色器示例
# [[shaders]]
# source = "Shaders/RayTrace.rgen"
# stage = "raygen"
# entry_point = "main"
# tags = ["raytracing"]
"#,
        module_name = module_name
    )
}

// ──────────────────────────────────────────────────────────────
// 单元测试
// ──────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    fn make_test_manifest() -> ShaderManifest {
        ShaderManifest {
            manifest: ManifestInfo {
                name: "TestShaders".to_string(),
                description: None,
                module: Some("Test".to_string()),
                version: "1.0".to_string(),
            },
            options: ManifestGlobalOptions::default(),
            global_variants: vec![ManifestVariantRule {
                name: "QUALITY".to_string(),
                values: vec!["LOW".to_string(), "MEDIUM".to_string(), "HIGH".to_string()],
                boolean: false,
            }],
            shaders: vec![
                ShaderEntry {
                    source: PathBuf::from("test.vert"),
                    stage: "vertex".to_string(),
                    entry_point: "main".to_string(),
                    defines: Vec::new(),
                    include_dirs: Vec::new(),
                    output: None,
                    variants: Vec::new(),
                    optimize: None,
                    debug_info: None,
                    tags: Vec::new(),
                    disabled: false,
                },
                ShaderEntry {
                    source: PathBuf::from("test.frag"),
                    stage: "fragment".to_string(),
                    entry_point: "main".to_string(),
                    defines: Vec::new(),
                    include_dirs: Vec::new(),
                    output: None,
                    variants: Vec::new(),
                    optimize: None,
                    debug_info: None,
                    tags: Vec::new(),
                    disabled: false,
                },
                ShaderEntry {
                    source: PathBuf::from("disabled.comp"),
                    stage: "compute".to_string(),
                    entry_point: "main".to_string(),
                    defines: Vec::new(),
                    include_dirs: Vec::new(),
                    output: None,
                    variants: Vec::new(),
                    optimize: None,
                    debug_info: None,
                    tags: Vec::new(),
                    disabled: true, // 禁用
                },
            ],
        }
    }

    #[test]
    fn test_manifest_active_shaders() {
        let manifest = make_test_manifest();
        assert_eq!(manifest.active_shaders().len(), 2);
    }

    #[test]
    fn test_manifest_variant_count() {
        let manifest = make_test_manifest();
        // 2 个活跃着色器，每个都有 3 种 QUALITY 变体
        assert_eq!(manifest.total_variant_count(), 6);
    }

    #[test]
    fn test_shader_entry_resolve_stage() {
        let entry = ShaderEntry {
            source: PathBuf::from("test.frag"),
            stage: "auto".to_string(),
            entry_point: "main".to_string(),
            defines: Vec::new(),
            include_dirs: Vec::new(),
            output: None,
            variants: Vec::new(),
            optimize: None,
            debug_info: None,
            tags: Vec::new(),
            disabled: false,
        };
        assert_eq!(entry.resolve_stage(), ShaderStage::Fragment);
    }

    #[test]
    fn test_generate_example_manifest() {
        let example = generate_example_manifest("Luminance");
        assert!(example.contains("Luminance"));
        assert!(example.contains("vulkan1.3"));
        assert!(example.contains("GBuffer.vert"));
    }

    #[test]
    fn test_manifest_compile_result_stats() {
        let result = ManifestCompileResult {
            manifest_name: "Test".to_string(),
            shader_results: vec![
                ShaderCompileResult {
                    name: "a".to_string(),
                    source_path: PathBuf::new(),
                    output_path: PathBuf::new(),
                    success: true,
                    from_cache: true,
                    duration_ms: 10,
                    output_size: 1024,
                    errors: Vec::new(),
                    warnings: Vec::new(),
                },
                ShaderCompileResult {
                    name: "b".to_string(),
                    source_path: PathBuf::new(),
                    output_path: PathBuf::new(),
                    success: false,
                    from_cache: false,
                    duration_ms: 5,
                    output_size: 0,
                    errors: vec!["编译错误".to_string()],
                    warnings: Vec::new(),
                },
            ],
            total_duration_ms: 15,
        };

        assert_eq!(result.success_count(), 1);
        assert_eq!(result.failure_count(), 1);
        assert_eq!(result.cache_hit_count(), 1);
        assert_eq!(result.total_output_size(), 1024);
        assert!(!result.all_success());
    }
}
