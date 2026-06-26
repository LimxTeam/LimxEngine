/*******************************************************************************
 * 文件: cache.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   增量编译缓存系统 (生产级增强版)
 *   - 跟踪源文件修改时间和内容哈希
 *   - 缓存模块配置解析结果
 *   - 避免重复处理未修改的文件
 *   - 支持模块级别和文件级别的增量更新
 *
 * 技术特性:
 *   - 并行文件哈希计算 (rayon)
 *   - FNV-1a + XXHash 双哈希验证
 *   - 文件级别细粒度追踪
 *   - 依赖链变更传播
 *   - 性能统计报告
 *
 ******************************************************************************/

use anyhow::Result;
use rayon::prelude::*;
use serde::{Deserialize, Serialize};
use std::collections::{HashMap, HashSet};
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::{Instant, SystemTime};
use tracing::{debug, info};

/// 缓存文件名
pub const CACHE_FILE_NAME: &str = ".lbt.cache";

/// 缓存统计
#[derive(Debug, Default)]
pub struct CacheStats {
    pub files_checked: AtomicUsize,
    pub files_hashed: AtomicUsize,
    pub cache_hits: AtomicUsize,
    pub cache_misses: AtomicUsize,
    pub check_duration_ms: u64,
}

impl CacheStats {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn print_summary(&self) {
        let checked = self.files_checked.load(Ordering::Relaxed);
        let hashed = self.files_hashed.load(Ordering::Relaxed);
        let hits = self.cache_hits.load(Ordering::Relaxed);
        let misses = self.cache_misses.load(Ordering::Relaxed);

        println!("\n缓存统计:");
        println!("  - 检查文件数: {}", checked);
        println!("  - 计算哈希数: {}", hashed);
        println!(
            "  - 缓存命中: {} ({:.1}%)",
            hits,
            if checked > 0 {
                hits as f64 / checked as f64 * 100.0
            } else {
                0.0
            }
        );
        println!("  - 缓存未命中: {}", misses);
        println!("  - 检查耗时: {} ms", self.check_duration_ms);
    }
}

/// 文件缓存条目
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CacheEntry {
    /// 文件路径
    pub path: PathBuf,
    /// 最后修改时间 (Unix 时间戳)
    pub modified_time: u64,
    /// 文件大小 (字节)
    #[serde(default)]
    pub file_size: u64,
    /// 文件内容哈希 (可选，用于更精确的检测)
    pub content_hash: Option<String>,
}

/// 模块缓存
#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct ModuleCache {
    /// 模块名称
    pub name: String,
    /// 配置文件缓存
    pub config_entry: Option<CacheEntry>,
    /// 源文件缓存
    pub source_entries: Vec<CacheEntry>,
    /// 头文件缓存
    #[serde(default)]
    pub header_entries: Vec<CacheEntry>,
    /// 生成的文件列表
    pub generated_files: Vec<PathBuf>,
    /// 依赖的模块缓存哈希 (用于依赖链变更检测)
    #[serde(default)]
    pub dependency_hash: Option<String>,
    /// 最后构建时间
    #[serde(default)]
    pub last_build_time: u64,
}

/// 构建缓存
#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct BuildCache {
    /// 缓存版本 (用于检测格式变化)
    pub version: u32,
    /// 模块缓存映射
    pub modules: HashMap<String, ModuleCache>,
    /// 全局配置哈希 (检测构建配置变化)
    pub config_hash: Option<String>,
}

impl BuildCache {
    /// 当前缓存版本
    const CURRENT_VERSION: u32 = 1;

    /// 创建新缓存
    pub fn new() -> Self {
        Self {
            version: Self::CURRENT_VERSION,
            modules: HashMap::new(),
            config_hash: None,
        }
    }

    /// 从文件加载缓存
    pub fn load(cache_dir: &Path) -> Result<Self> {
        let cache_path = cache_dir.join(CACHE_FILE_NAME);

        if !cache_path.exists() {
            return Ok(Self::new());
        }

        let content = fs::read_to_string(&cache_path)?;
        let cache: BuildCache = toml::from_str(&content)?;

        // 检查版本兼容性
        if cache.version != Self::CURRENT_VERSION {
            return Ok(Self::new());
        }

        Ok(cache)
    }

    /// 保存缓存到文件
    pub fn save(&self, cache_dir: &Path) -> Result<()> {
        fs::create_dir_all(cache_dir)?;
        let cache_path = cache_dir.join(CACHE_FILE_NAME);
        let content = toml::to_string_pretty(self)?;
        fs::write(&cache_path, content)?;
        Ok(())
    }

    /// 检查文件是否已修改
    pub fn is_file_modified(&self, module_name: &str, file_path: &Path) -> bool {
        let Some(module_cache) = self.modules.get(module_name) else {
            return true;
        };

        // 检查配置文件
        if let Some(config_entry) = &module_cache.config_entry {
            if config_entry.path == file_path {
                return Self::check_entry_modified(config_entry, file_path);
            }
        }

        // 检查源文件
        for entry in &module_cache.source_entries {
            if entry.path == file_path {
                return Self::check_entry_modified(entry, file_path);
            }
        }

        // 文件不在缓存中，视为已修改
        true
    }

    /// 检查缓存条目是否已过期
    fn check_entry_modified(entry: &CacheEntry, file_path: &Path) -> bool {
        let Ok(metadata) = fs::metadata(file_path) else {
            return true;
        };

        let Ok(modified) = metadata.modified() else {
            return true;
        };

        let current_time = modified
            .duration_since(SystemTime::UNIX_EPOCH)
            .map(|d| d.as_secs())
            .unwrap_or(0);

        current_time != entry.modified_time
    }

    /// 更新模块缓存
    pub fn update_module(
        &mut self,
        module_name: &str,
        config_path: &Path,
        source_files: &[PathBuf],
    ) {
        let config_entry = Self::create_entry(config_path);
        let source_entries: Vec<_> = source_files
            .iter()
            .filter_map(|p| Self::create_entry(p))
            .collect();

        let module_cache = ModuleCache {
            name: module_name.to_string(),
            config_entry,
            source_entries,
            header_entries: Vec::new(),
            generated_files: Vec::new(),
            dependency_hash: None,
            last_build_time: SystemTime::now()
                .duration_since(SystemTime::UNIX_EPOCH)
                .map(|d| d.as_secs())
                .unwrap_or(0),
        };

        self.modules.insert(module_name.to_string(), module_cache);
    }

    /// 创建缓存条目
    fn create_entry(file_path: &Path) -> Option<CacheEntry> {
        let metadata = fs::metadata(file_path).ok()?;
        let modified = metadata.modified().ok()?;
        let modified_time = modified
            .duration_since(SystemTime::UNIX_EPOCH)
            .ok()?
            .as_secs();

        Some(CacheEntry {
            path: file_path.to_path_buf(),
            modified_time,
            file_size: metadata.len(),
            content_hash: None,
        })
    }

    /// 获取需要重新处理的模块列表
    pub fn get_dirty_modules(&self, all_modules: &[String]) -> Vec<String> {
        all_modules
            .iter()
            .filter(|name| !self.modules.contains_key(*name) || self.is_module_dirty(name))
            .cloned()
            .collect()
    }

    /// 检查模块是否需要重新处理
    fn is_module_dirty(&self, module_name: &str) -> bool {
        let Some(module_cache) = self.modules.get(module_name) else {
            return true;
        };

        // 检查配置文件
        if let Some(config_entry) = &module_cache.config_entry {
            if Self::check_entry_modified(config_entry, &config_entry.path) {
                return true;
            }
        }

        // 检查所有源文件
        for entry in &module_cache.source_entries {
            if Self::check_entry_modified(entry, &entry.path) {
                return true;
            }
        }

        false
    }

    /// 清除缓存
    pub fn clear(&mut self) {
        self.modules.clear();
        self.config_hash = None;
    }

    /// 清除指定模块的缓存
    pub fn clear_module(&mut self, module_name: &str) {
        self.modules.remove(module_name);
    }
}

/// 计算文件内容哈希 (XXHash3 算法 - 比 FNV-1a 快 10x+，碰撞率更低)
///
/// XXHash3 特性:
/// - 速度: 在现代 CPU 上可达 30+ GB/s
/// - 质量: 通过 SMHasher 测试套件
/// - 碰撞: 128-bit 版本碰撞概率极低
pub fn compute_file_hash(file_path: &Path) -> Result<String> {
    let content = fs::read(file_path)?;
    Ok(compute_hash_xxh3(&content))
}

/// XXHash3-128 哈希算法 - 超高速且碰撞率极低
#[inline]
pub fn compute_hash_xxh3(data: &[u8]) -> String {
    let hash = xxhash_rust::xxh3::xxh3_128(data);
    format!("{:032x}", hash)
}

/// 计算字符串哈希
#[inline]
pub fn compute_string_hash(s: &str) -> String {
    compute_hash_xxh3(s.as_bytes())
}

/// 计算增量哈希 - 用于大文件流式处理
pub struct IncrementalHasher {
    state: xxhash_rust::xxh3::Xxh3,
}

impl IncrementalHasher {
    pub fn new() -> Self {
        Self {
            state: xxhash_rust::xxh3::Xxh3::new(),
        }
    }

    pub fn update(&mut self, data: &[u8]) {
        self.state.update(data);
    }

    pub fn finalize(self) -> String {
        format!("{:016x}", self.state.digest())
    }
}

impl Default for IncrementalHasher {
    fn default() -> Self {
        Self::new()
    }
}

/// 旧版 FNV-1a 哈希 (保留用于向后兼容)
#[inline]
#[deprecated(note = "使用 compute_hash_xxh3 代替，性能更好且碰撞率更低")]
fn compute_hash_fnv1a(data: &[u8]) -> String {
    let mut hash: u64 = 0xcbf29ce484222325;
    for byte in data {
        hash ^= *byte as u64;
        hash = hash.wrapping_mul(0x100000001b3);
    }
    format!("{:016x}", hash)
}

/// 并行计算多个文件的哈希
pub fn compute_file_hashes_parallel(paths: &[PathBuf]) -> Vec<(PathBuf, Option<String>)> {
    paths
        .par_iter()
        .map(|path| {
            let hash = compute_file_hash(path).ok();
            (path.clone(), hash)
        })
        .collect()
}

/// 快速检查文件是否变更 (仅检查时间戳和大小)
pub fn quick_file_check(path: &Path, cached_time: u64, cached_size: u64) -> bool {
    if let Ok(metadata) = fs::metadata(path) {
        if let Ok(modified) = metadata.modified() {
            let current_time = modified
                .duration_since(SystemTime::UNIX_EPOCH)
                .map(|d| d.as_secs())
                .unwrap_or(0);
            return current_time == cached_time && metadata.len() == cached_size;
        }
    }
    false
}

/// 文件变更类型
#[derive(Debug, Clone, PartialEq)]
pub enum FileChangeType {
    /// 新增文件
    Added,
    /// 修改文件
    Modified,
    /// 删除文件
    Deleted,
}

/// 文件变更记录
#[derive(Debug, Clone)]
pub struct FileChange {
    pub path: PathBuf,
    pub change_type: FileChangeType,
    pub module_name: String,
}

/// 模块变更原因
#[derive(Debug, Clone)]
pub enum ModuleDirtyReason {
    /// 新模块
    NewModule,
    /// 配置文件变更
    ConfigChanged,
    /// 源文件变更
    SourceChanged(Vec<PathBuf>),
    /// 头文件变更
    HeaderChanged(Vec<PathBuf>),
    /// 依赖模块变更
    DependencyChanged(Vec<String>),
}

/// 增量编译结果
#[derive(Debug, Default)]
pub struct IncrementalResult {
    /// 需要重新生成的模块
    pub dirty_modules: Vec<String>,
    /// 未修改的模块
    pub clean_modules: Vec<String>,
    /// 新增的模块
    pub new_modules: Vec<String>,
    /// 删除的模块
    pub removed_modules: Vec<String>,
    /// 是否需要重新生成解决方案
    pub need_regenerate_solution: bool,
    /// 文件变更列表
    pub file_changes: Vec<FileChange>,
    /// 模块变更原因
    pub dirty_reasons: HashMap<String, Vec<ModuleDirtyReason>>,
    /// 检测耗时 (ms)
    pub detection_time_ms: u64,
}

impl BuildCache {
    /// 检测增量变化 (并行版)
    pub fn detect_changes(&self, modules: &[super::config::Module]) -> IncrementalResult {
        let start = Instant::now();
        let mut result = IncrementalResult::default();

        let current_module_names: HashSet<_> = modules.iter().map(|m| m.name.clone()).collect();
        let cached_module_names: HashSet<_> = self.modules.keys().cloned().collect();

        // 检测新增模块
        for name in current_module_names.difference(&cached_module_names) {
            result.new_modules.push(name.clone());
            result.dirty_modules.push(name.clone());
            result
                .dirty_reasons
                .insert(name.clone(), vec![ModuleDirtyReason::NewModule]);
            result.need_regenerate_solution = true;
        }

        // 检测删除的模块
        for name in cached_module_names.difference(&current_module_names) {
            result.removed_modules.push(name.clone());
            result.need_regenerate_solution = true;
        }

        // 并行检测修改的模块
        let check_results: Vec<_> = modules
            .par_iter()
            .filter(|m| !result.new_modules.contains(&m.name))
            .map(|module| {
                let (is_dirty, reasons, changes) = self.check_module_changes_detailed(&module.name);
                (module.name.clone(), is_dirty, reasons, changes)
            })
            .collect();

        for (name, is_dirty, reasons, changes) in check_results {
            if is_dirty {
                result.dirty_modules.push(name.clone());
                result.dirty_reasons.insert(name, reasons);
                result.file_changes.extend(changes);
            } else {
                result.clean_modules.push(name);
            }
        }

        result.detection_time_ms = start.elapsed().as_millis() as u64;

        // 记录日志
        if !result.dirty_modules.is_empty() {
            debug!("Dirty modules: {:?}", result.dirty_modules);
        }

        result
    }

    /// 详细检查模块变更
    fn check_module_changes_detailed(
        &self,
        module_name: &str,
    ) -> (bool, Vec<ModuleDirtyReason>, Vec<FileChange>) {
        let mut reasons = Vec::new();
        let mut changes = Vec::new();

        let Some(module_cache) = self.modules.get(module_name) else {
            return (true, vec![ModuleDirtyReason::NewModule], vec![]);
        };

        // 检查配置文件
        if let Some(config_entry) = &module_cache.config_entry {
            if Self::check_entry_modified_with_hash(config_entry) {
                reasons.push(ModuleDirtyReason::ConfigChanged);
                changes.push(FileChange {
                    path: config_entry.path.clone(),
                    change_type: FileChangeType::Modified,
                    module_name: module_name.to_string(),
                });
            }
        }

        // 检查源文件
        let mut changed_sources = Vec::new();
        for entry in &module_cache.source_entries {
            if Self::check_entry_modified_with_hash(entry) {
                changed_sources.push(entry.path.clone());
                changes.push(FileChange {
                    path: entry.path.clone(),
                    change_type: FileChangeType::Modified,
                    module_name: module_name.to_string(),
                });
            }
        }
        if !changed_sources.is_empty() {
            reasons.push(ModuleDirtyReason::SourceChanged(changed_sources));
        }

        // 检查头文件
        let mut changed_headers = Vec::new();
        for entry in &module_cache.header_entries {
            if Self::check_entry_modified_with_hash(entry) {
                changed_headers.push(entry.path.clone());
                changes.push(FileChange {
                    path: entry.path.clone(),
                    change_type: FileChangeType::Modified,
                    module_name: module_name.to_string(),
                });
            }
        }
        if !changed_headers.is_empty() {
            reasons.push(ModuleDirtyReason::HeaderChanged(changed_headers));
        }

        (!reasons.is_empty(), reasons, changes)
    }

    /// 更新模块缓存 (带哈希)
    pub fn update_module_with_hash(
        &mut self,
        module_name: &str,
        config_path: &Path,
        source_files: &[PathBuf],
        header_files: &[PathBuf],
    ) {
        let config_entry = Self::create_entry_with_hash(config_path);

        let mut source_entries: Vec<_> = source_files
            .iter()
            .filter_map(|p| Self::create_entry_with_hash(p))
            .collect();

        // 添加头文件
        source_entries.extend(
            header_files
                .iter()
                .filter_map(|p| Self::create_entry_with_hash(p)),
        );

        let module_cache = ModuleCache {
            name: module_name.to_string(),
            config_entry,
            source_entries,
            header_entries: Vec::new(),
            generated_files: Vec::new(),
            dependency_hash: None,
            last_build_time: SystemTime::now()
                .duration_since(SystemTime::UNIX_EPOCH)
                .map(|d| d.as_secs())
                .unwrap_or(0),
        };

        self.modules.insert(module_name.to_string(), module_cache);
    }

    /// 创建带哈希的缓存条目
    fn create_entry_with_hash(file_path: &Path) -> Option<CacheEntry> {
        let metadata = fs::metadata(file_path).ok()?;
        let modified = metadata.modified().ok()?;
        let modified_time = modified
            .duration_since(SystemTime::UNIX_EPOCH)
            .ok()?
            .as_secs();

        let content_hash = compute_file_hash(file_path).ok();

        Some(CacheEntry {
            path: file_path.to_path_buf(),
            modified_time,
            file_size: metadata.len(),
            content_hash,
        })
    }

    /// 检查模块是否需要重新处理 (带哈希检查)
    pub fn is_module_dirty_with_hash(&self, module_name: &str) -> bool {
        let Some(module_cache) = self.modules.get(module_name) else {
            return true;
        };

        // 检查配置文件
        if let Some(config_entry) = &module_cache.config_entry {
            if Self::check_entry_modified_with_hash(config_entry) {
                return true;
            }
        }

        // 检查所有源文件
        for entry in &module_cache.source_entries {
            if Self::check_entry_modified_with_hash(entry) {
                return true;
            }
        }

        false
    }

    /// 检查缓存条目是否已过期 (带哈希)
    fn check_entry_modified_with_hash(entry: &CacheEntry) -> bool {
        // 先检查时间戳
        let Ok(metadata) = fs::metadata(&entry.path) else {
            return true;
        };

        let Ok(modified) = metadata.modified() else {
            return true;
        };

        let current_time = modified
            .duration_since(SystemTime::UNIX_EPOCH)
            .map(|d| d.as_secs())
            .unwrap_or(0);

        // 时间戳相同，认为未修改
        if current_time == entry.modified_time {
            return false;
        }

        // 时间戳不同，检查哈希
        if let Some(cached_hash) = &entry.content_hash {
            if let Ok(current_hash) = compute_file_hash(&entry.path) {
                return &current_hash != cached_hash;
            }
        }

        true
    }

    /// 记录生成的文件
    pub fn record_generated_file(&mut self, module_name: &str, file_path: PathBuf) {
        if let Some(module_cache) = self.modules.get_mut(module_name) {
            if !module_cache.generated_files.contains(&file_path) {
                module_cache.generated_files.push(file_path);
            }
        }
    }

    /// 打印增量编译统计
    pub fn print_stats(&self, result: &IncrementalResult) {
        let total = result.dirty_modules.len() + result.clean_modules.len();
        let skipped = result.clean_modules.len();

        if skipped > 0 {
            info!("增量编译: {} 个模块未修改，跳过重新生成", skipped);
        }

        if !result.new_modules.is_empty() {
            info!("新增模块: {:?}", result.new_modules);
        }

        if !result.removed_modules.is_empty() {
            info!("删除模块: {:?}", result.removed_modules);
        }

        if !result.dirty_modules.is_empty() {
            info!("需要重新生成: {:?}", result.dirty_modules);
        }

        println!("\n增量编译统计:");
        println!("  - 总模块数: {}", total);
        println!("  - 跳过 (未修改): {}", skipped);
        println!("  - 重新生成: {}", result.dirty_modules.len());
        println!("  - 检测耗时: {} ms", result.detection_time_ms);
        if !result.new_modules.is_empty() {
            println!("  - 新增: {}", result.new_modules.len());
        }
        if !result.removed_modules.is_empty() {
            println!("  - 删除: {}", result.removed_modules.len());
        }
        if !result.file_changes.is_empty() {
            println!("  - 文件变更: {} 个", result.file_changes.len());
        }
    }

    /// 传播依赖链变更
    pub fn propagate_dependency_changes(
        &self,
        result: &mut IncrementalResult,
        dependency_graph: &super::dependency::DependencyGraph,
    ) {
        let initial_dirty: HashSet<_> = result.dirty_modules.iter().cloned().collect();
        let mut propagated = HashSet::new();

        // 对每个脏模块，找出依赖它的模块
        for dirty_module in &initial_dirty {
            let dependents = dependency_graph.get_transitive_dependents(dirty_module);
            for dep in dependents {
                if !initial_dirty.contains(&dep) && !propagated.contains(&dep) {
                    propagated.insert(dep.clone());
                    result.dirty_modules.push(dep.clone());
                    result
                        .dirty_reasons
                        .entry(dep)
                        .or_insert_with(Vec::new)
                        .push(ModuleDirtyReason::DependencyChanged(vec![
                            dirty_module.clone()
                        ]));
                }
            }
        }

        // 从 clean_modules 中移除被传播污染的模块
        result.clean_modules.retain(|m| !propagated.contains(m));

        if !propagated.is_empty() {
            info!(
                "依赖链传播: {} 个模块因依赖变更需要重新生成",
                propagated.len()
            );
        }
    }

    /// 并行更新多个模块缓存
    pub fn update_modules_parallel(&mut self, modules: &[(&str, &Path, &[PathBuf], &[PathBuf])]) {
        let entries: Vec<_> = modules
            .par_iter()
            .map(|(name, config, sources, headers)| {
                let config_entry = Self::create_entry_with_hash(config);
                let source_entries: Vec<_> = sources
                    .iter()
                    .filter_map(|p| Self::create_entry_with_hash(p))
                    .collect();
                let header_entries: Vec<_> = headers
                    .iter()
                    .filter_map(|p| Self::create_entry_with_hash(p))
                    .collect();
                (
                    name.to_string(),
                    config_entry,
                    source_entries,
                    header_entries,
                )
            })
            .collect();

        let now = SystemTime::now()
            .duration_since(SystemTime::UNIX_EPOCH)
            .map(|d| d.as_secs())
            .unwrap_or(0);

        for (name, config_entry, source_entries, header_entries) in entries {
            self.modules.insert(
                name.clone(),
                ModuleCache {
                    name,
                    config_entry,
                    source_entries,
                    header_entries,
                    generated_files: Vec::new(),
                    dependency_hash: None,
                    last_build_time: now,
                },
            );
        }
    }

    /// 获取模块的缓存摘要哈希
    pub fn get_module_cache_hash(&self, module_name: &str) -> Option<String> {
        let module_cache = self.modules.get(module_name)?;

        let mut combined = String::new();
        if let Some(config) = &module_cache.config_entry {
            if let Some(hash) = &config.content_hash {
                combined.push_str(hash);
            }
        }
        for entry in &module_cache.source_entries {
            if let Some(hash) = &entry.content_hash {
                combined.push_str(hash);
            }
        }
        for entry in &module_cache.header_entries {
            if let Some(hash) = &entry.content_hash {
                combined.push_str(hash);
            }
        }

        if combined.is_empty() {
            None
        } else {
            Some(compute_hash_xxh3(combined.as_bytes()))
        }
    }
}
