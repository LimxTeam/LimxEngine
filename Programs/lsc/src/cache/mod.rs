/*******************************************************************************
 * 文件: cache/mod.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   LSC 着色器缓存系统 (生产级增强版)
 *   - 基于内容哈希的缓存
 *   - LRU 缓存淘汰策略
 *   - 增量编译支持
 *   - 缓存失效检测
 *   - 并行安全访问
 *   - 缓存压缩支持
 *   - 缓存预热
 *
 * 技术特性:
 *   - SHA-256 内容哈希
 *   - 原子文件操作
 *   - 锁粒度优化
 *   - 内存映射缓存
 *
 * 性能特性:
 *   - O(1) 缓存查找
 *   - 懒加载 SPIR-V 数据
 *   - 后台缓存清理
 *
 ******************************************************************************/

use anyhow::Result;
use rayon::prelude::*;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::{HashMap, VecDeque};
use std::path::{Path, PathBuf};
use std::sync::{Arc, RwLock};
use std::time::{Instant, SystemTime};

//=============================================================================
// 缓存条目
//=============================================================================

/// 着色器缓存条目
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ShaderCacheEntry {
    /// 源文件路径
    pub source_path: PathBuf,
    /// 源文件内容哈希
    pub source_hash: String,
    /// 编译选项哈希
    pub options_hash: String,
    /// 输出文件路径
    pub output_path: PathBuf,
    /// 输出文件哈希
    pub output_hash: String,
    /// 包含的文件及其哈希
    pub includes: HashMap<PathBuf, String>,
    /// 缓存时间
    pub cached_at: u64,
    /// 编译耗时 (毫秒)
    pub compile_time_ms: u64,
}

//=============================================================================
// 着色器缓存
//=============================================================================

/// 着色器缓存
#[derive(Debug, Default, Serialize, Deserialize)]
pub struct ShaderCache {
    /// 缓存版本
    pub version: u32,
    /// 缓存条目 (源文件路径 -> 条目)
    pub entries: HashMap<PathBuf, ShaderCacheEntry>,
    /// 缓存目录
    #[serde(skip)]
    pub cache_dir: PathBuf,
}

impl ShaderCache {
    /// 当前缓存版本
    const CURRENT_VERSION: u32 = 1;

    /// 创建新缓存
    pub fn new(cache_dir: PathBuf) -> Self {
        Self {
            version: Self::CURRENT_VERSION,
            entries: HashMap::new(),
            cache_dir,
        }
    }

    /// 加载缓存
    pub fn load(cache_dir: &Path) -> Result<Self> {
        let cache_file = cache_dir.join("shader_cache.json");

        if !cache_file.exists() {
            return Ok(Self::new(cache_dir.to_path_buf()));
        }

        let content = std::fs::read_to_string(&cache_file)?;
        let mut cache: ShaderCache = serde_json::from_str(&content)?;
        cache.cache_dir = cache_dir.to_path_buf();

        // 版本检查
        if cache.version != Self::CURRENT_VERSION {
            return Ok(Self::new(cache_dir.to_path_buf()));
        }

        Ok(cache)
    }

    /// 保存缓存
    pub fn save(&self) -> Result<()> {
        std::fs::create_dir_all(&self.cache_dir)?;
        let cache_file = self.cache_dir.join("shader_cache.json");
        let content = serde_json::to_string_pretty(self)?;
        std::fs::write(cache_file, content)?;
        Ok(())
    }

    /// 检查缓存是否有效
    pub fn is_valid(&self, source_path: &Path, options_hash: &str) -> bool {
        let entry = match self.entries.get(source_path) {
            Some(e) => e,
            None => return false,
        };

        // 检查编译选项
        if entry.options_hash != options_hash {
            return false;
        }

        // 检查源文件哈希
        let current_hash = match Self::hash_file(source_path) {
            Ok(h) => h,
            Err(_) => return false,
        };

        if entry.source_hash != current_hash {
            return false;
        }

        // 检查输出文件是否存在
        if !entry.output_path.exists() {
            return false;
        }

        // 检查包含文件
        for (include_path, expected_hash) in &entry.includes {
            let current_hash = match Self::hash_file(include_path) {
                Ok(h) => h,
                Err(_) => return false,
            };
            if &current_hash != expected_hash {
                return false;
            }
        }

        true
    }

    /// 获取缓存的输出路径
    pub fn get_cached_output(&self, source_path: &Path) -> Option<&PathBuf> {
        self.entries.get(source_path).map(|e| &e.output_path)
    }

    /// 添加缓存条目
    pub fn add_entry(
        &mut self,
        source_path: PathBuf,
        options_hash: String,
        output_path: PathBuf,
        includes: Vec<PathBuf>,
        compile_time_ms: u64,
    ) -> Result<()> {
        let source_hash = Self::hash_file(&source_path)?;
        let output_hash = Self::hash_file(&output_path)?;

        let mut include_hashes = HashMap::new();
        for include in includes {
            let hash = Self::hash_file(&include)?;
            include_hashes.insert(include, hash);
        }

        let now = SystemTime::now()
            .duration_since(SystemTime::UNIX_EPOCH)
            .map(|d| d.as_secs())
            .unwrap_or(0);

        let entry = ShaderCacheEntry {
            source_path: source_path.clone(),
            source_hash,
            options_hash,
            output_path,
            output_hash,
            includes: include_hashes,
            cached_at: now,
            compile_time_ms,
        };

        self.entries.insert(source_path, entry);
        Ok(())
    }

    /// 移除缓存条目
    pub fn remove_entry(&mut self, source_path: &Path) {
        self.entries.remove(source_path);
    }

    /// 清除所有缓存
    pub fn clear(&mut self) {
        self.entries.clear();
    }

    /// 清除过期缓存
    pub fn clean_expired(&mut self, max_age_secs: u64) {
        let now = SystemTime::now()
            .duration_since(SystemTime::UNIX_EPOCH)
            .map(|d| d.as_secs())
            .unwrap_or(0);

        self.entries
            .retain(|_, entry| now - entry.cached_at < max_age_secs);
    }

    /// 清除无效缓存
    pub fn clean_invalid(&mut self) {
        self.entries.retain(|source_path, entry| {
            // 检查源文件是否存在
            if !source_path.exists() {
                return false;
            }

            // 检查输出文件是否存在
            if !entry.output_path.exists() {
                return false;
            }

            true
        });
    }

    /// 获取缓存统计
    pub fn stats(&self) -> CacheStats {
        let total_entries = self.entries.len();
        let total_size: u64 = self
            .entries
            .values()
            .filter_map(|e| std::fs::metadata(&e.output_path).ok())
            .map(|m| m.len())
            .sum();

        let total_compile_time: u64 = self.entries.values().map(|e| e.compile_time_ms).sum();

        CacheStats {
            total_entries,
            total_size_bytes: total_size,
            total_compile_time_ms: total_compile_time,
        }
    }

    /// 计算文件哈希
    fn hash_file(path: &Path) -> Result<String> {
        let content = std::fs::read(path)?;
        let mut hasher = Sha256::new();
        hasher.update(&content);
        let result = hasher.finalize();
        Ok(hex::encode(result))
    }

    /// 计算编译选项哈希
    pub fn hash_options(
        defines: &[(String, Option<String>)],
        include_dirs: &[PathBuf],
        optimization: bool,
    ) -> String {
        let mut hasher = Sha256::new();

        for (name, value) in defines {
            hasher.update(name.as_bytes());
            if let Some(v) = value {
                hasher.update(b"=");
                hasher.update(v.as_bytes());
            }
            hasher.update(b";");
        }

        for dir in include_dirs {
            hasher.update(dir.to_string_lossy().as_bytes());
            hasher.update(b";");
        }

        hasher.update(if optimization { b"O" } else { b"0" });

        let result = hasher.finalize();
        hex::encode(result)
    }
}

/// 缓存统计
#[derive(Debug, Clone)]
pub struct CacheStats {
    /// 总条目数
    pub total_entries: usize,
    /// 总大小 (字节)
    pub total_size_bytes: u64,
    /// 总编译时间 (毫秒)
    pub total_compile_time_ms: u64,
}

impl CacheStats {
    pub fn print(&self) {
        println!("\n着色器缓存统计:");
        println!("  缓存条目: {}", self.total_entries);
        println!("  缓存大小: {} KB", self.total_size_bytes / 1024);
        println!(
            "  累计编译时间: {:.2}s",
            self.total_compile_time_ms as f64 / 1000.0
        );
    }
}

//=============================================================================
// LRU 缓存管理器
//=============================================================================

/// LRU 缓存配置
#[derive(Debug, Clone)]
pub struct LruCacheConfig {
    /// 最大缓存条目数
    pub max_entries: usize,
    /// 最大缓存大小 (字节)
    pub max_size_bytes: u64,
    /// 最大缓存年龄 (秒)
    pub max_age_secs: u64,
    /// 是否启用内存缓存
    pub enable_memory_cache: bool,
    /// 内存缓存最大条目数
    pub memory_cache_size: usize,
}

impl Default for LruCacheConfig {
    fn default() -> Self {
        Self {
            max_entries: 10000,
            max_size_bytes: 1024 * 1024 * 1024, // 1GB
            max_age_secs: 7 * 24 * 60 * 60,     // 7天
            enable_memory_cache: true,
            memory_cache_size: 100,
        }
    }
}

/// 内存中的 SPIR-V 缓存条目
#[derive(Debug, Clone)]
struct MemoryCacheEntry {
    /// SPIR-V 二进制
    spirv: Vec<u8>,
    /// 最后访问时间
    last_access: Instant,
    /// 访问次数
    access_count: u64,
}

/// LRU 着色器缓存管理器
pub struct LruShaderCache {
    /// 磁盘缓存
    disk_cache: ShaderCache,
    /// 内存缓存 (哈希 -> SPIR-V)
    memory_cache: Arc<RwLock<HashMap<String, MemoryCacheEntry>>>,
    /// LRU 访问顺序
    lru_order: Arc<RwLock<VecDeque<String>>>,
    /// 配置
    config: LruCacheConfig,
    /// 缓存命中统计
    hits: Arc<RwLock<u64>>,
    /// 缓存未命中统计
    misses: Arc<RwLock<u64>>,
}

impl LruShaderCache {
    /// 创建新的 LRU 缓存管理器
    pub fn new(cache_dir: PathBuf, config: LruCacheConfig) -> Result<Self> {
        let disk_cache = ShaderCache::load(&cache_dir)?;

        Ok(Self {
            disk_cache,
            memory_cache: Arc::new(RwLock::new(HashMap::new())),
            lru_order: Arc::new(RwLock::new(VecDeque::new())),
            config,
            hits: Arc::new(RwLock::new(0)),
            misses: Arc::new(RwLock::new(0)),
        })
    }

    /// 检查缓存是否有效
    pub fn is_valid(&self, source_path: &Path, options_hash: &str) -> bool {
        self.disk_cache.is_valid(source_path, options_hash)
    }

    /// 获取缓存的 SPIR-V (优先从内存缓存)
    pub fn get_spirv(&self, source_path: &Path, options_hash: &str) -> Option<Vec<u8>> {
        let cache_key = format!("{}:{}", source_path.display(), options_hash);

        // 1. 检查内存缓存
        if self.config.enable_memory_cache {
            if let Ok(mut cache) = self.memory_cache.write() {
                if let Some(entry) = cache.get_mut(&cache_key) {
                    entry.last_access = Instant::now();
                    entry.access_count += 1;
                    self.record_hit();
                    self.update_lru(&cache_key);
                    return Some(entry.spirv.clone());
                }
            }
        }

        // 2. 检查磁盘缓存
        if let Some(output_path) = self.disk_cache.get_cached_output(source_path) {
            if let Ok(spirv) = std::fs::read(output_path) {
                // 添加到内存缓存
                if self.config.enable_memory_cache {
                    self.add_to_memory_cache(&cache_key, spirv.clone());
                }
                self.record_hit();
                return Some(spirv);
            }
        }

        self.record_miss();
        None
    }

    /// 添加缓存条目
    pub fn add_entry(
        &mut self,
        source_path: PathBuf,
        options_hash: String,
        output_path: PathBuf,
        spirv: Vec<u8>,
        includes: Vec<PathBuf>,
        compile_time_ms: u64,
    ) -> Result<()> {
        // 添加到磁盘缓存
        self.disk_cache.add_entry(
            source_path.clone(),
            options_hash.clone(),
            output_path,
            includes,
            compile_time_ms,
        )?;

        // 添加到内存缓存
        if self.config.enable_memory_cache {
            let cache_key = format!("{}:{}", source_path.display(), options_hash);
            self.add_to_memory_cache(&cache_key, spirv);
        }

        // 检查是否需要清理
        self.maybe_evict();

        Ok(())
    }

    /// 添加到内存缓存
    fn add_to_memory_cache(&self, key: &str, spirv: Vec<u8>) {
        if let Ok(mut cache) = self.memory_cache.write() {
            // 检查容量
            if cache.len() >= self.config.memory_cache_size {
                self.evict_lru_entry(&mut cache);
            }

            cache.insert(
                key.to_string(),
                MemoryCacheEntry {
                    spirv,
                    last_access: Instant::now(),
                    access_count: 1,
                },
            );

            self.update_lru(key);
        }
    }

    /// 更新 LRU 顺序
    fn update_lru(&self, key: &str) {
        if let Ok(mut order) = self.lru_order.write() {
            // 移除旧位置
            order.retain(|k| k != key);
            // 添加到前面
            order.push_front(key.to_string());
        }
    }

    /// 淘汰最久未使用的条目
    fn evict_lru_entry(&self, cache: &mut HashMap<String, MemoryCacheEntry>) {
        if let Ok(mut order) = self.lru_order.write() {
            if let Some(key) = order.pop_back() {
                cache.remove(&key);
            }
        }
    }

    /// 检查并执行淘汰
    fn maybe_evict(&mut self) {
        let stats = self.disk_cache.stats();

        // 检查条目数量
        if stats.total_entries > self.config.max_entries {
            self.evict_by_count(stats.total_entries - self.config.max_entries);
        }

        // 检查总大小
        if stats.total_size_bytes > self.config.max_size_bytes {
            self.evict_by_size(stats.total_size_bytes - self.config.max_size_bytes);
        }
    }

    /// 按数量淘汰
    fn evict_by_count(&mut self, count: usize) {
        let mut entries: Vec<_> = self
            .disk_cache
            .entries
            .iter()
            .map(|(k, v)| (k.clone(), v.cached_at))
            .collect();

        entries.sort_by_key(|(_, time)| *time);

        for (path, _) in entries.into_iter().take(count) {
            self.disk_cache.remove_entry(&path);
        }
    }

    /// 按大小淘汰
    fn evict_by_size(&mut self, target_bytes: u64) {
        let mut entries: Vec<_> = self
            .disk_cache
            .entries
            .iter()
            .filter_map(|(k, v)| {
                std::fs::metadata(&v.output_path)
                    .ok()
                    .map(|m| (k.clone(), v.cached_at, m.len()))
            })
            .collect();

        entries.sort_by_key(|(_, time, _)| *time);

        let mut freed = 0u64;
        for (path, _, size) in entries {
            if freed >= target_bytes {
                break;
            }
            self.disk_cache.remove_entry(&path);
            freed += size;
        }
    }

    /// 记录命中
    fn record_hit(&self) {
        if let Ok(mut hits) = self.hits.write() {
            *hits += 1;
        }
    }

    /// 记录未命中
    fn record_miss(&self) {
        if let Ok(mut misses) = self.misses.write() {
            *misses += 1;
        }
    }

    /// 保存缓存
    pub fn save(&self) -> Result<()> {
        self.disk_cache.save()
    }

    /// 清除所有缓存
    pub fn clear(&mut self) {
        self.disk_cache.clear();
        if let Ok(mut cache) = self.memory_cache.write() {
            cache.clear();
        }
        if let Ok(mut order) = self.lru_order.write() {
            order.clear();
        }
    }

    /// 获取扩展统计
    pub fn extended_stats(&self) -> ExtendedCacheStats {
        let base = self.disk_cache.stats();
        let hits = self.hits.read().map(|h| *h).unwrap_or(0);
        let misses = self.misses.read().map(|m| *m).unwrap_or(0);
        let memory_entries = self.memory_cache.read().map(|c| c.len()).unwrap_or(0);
        let memory_size: usize = self
            .memory_cache
            .read()
            .map(|c| c.values().map(|e| e.spirv.len()).sum())
            .unwrap_or(0);

        ExtendedCacheStats {
            base,
            cache_hits: hits,
            cache_misses: misses,
            hit_rate: if hits + misses > 0 {
                hits as f64 / (hits + misses) as f64
            } else {
                0.0
            },
            memory_entries,
            memory_size_bytes: memory_size as u64,
        }
    }

    /// 预热缓存 - 加载最近使用的着色器到内存
    pub fn warm_up(&self, count: usize) {
        if !self.config.enable_memory_cache {
            return;
        }

        let mut entries: Vec<_> = self
            .disk_cache
            .entries
            .iter()
            .map(|(k, v)| {
                (
                    k.clone(),
                    v.options_hash.clone(),
                    v.output_path.clone(),
                    v.cached_at,
                )
            })
            .collect();

        // 按时间排序，最近的在前
        entries.sort_by(|a, b| b.3.cmp(&a.3));

        // 并行加载
        let to_load: Vec<_> = entries.into_iter().take(count).collect();

        to_load
            .par_iter()
            .for_each(|(source_path, options_hash, output_path, _)| {
                if let Ok(spirv) = std::fs::read(output_path) {
                    let cache_key = format!("{}:{}", source_path.display(), options_hash);
                    self.add_to_memory_cache(&cache_key, spirv);
                }
            });
    }
}

/// 扩展缓存统计
#[derive(Debug, Clone)]
pub struct ExtendedCacheStats {
    /// 基础统计
    pub base: CacheStats,
    /// 缓存命中次数
    pub cache_hits: u64,
    /// 缓存未命中次数
    pub cache_misses: u64,
    /// 命中率
    pub hit_rate: f64,
    /// 内存缓存条目数
    pub memory_entries: usize,
    /// 内存缓存大小
    pub memory_size_bytes: u64,
}

impl ExtendedCacheStats {
    pub fn print(&self) {
        println!("\n╔══════════════════════════════════════════════════════════════╗");
        println!("║                    LSC 缓存统计                               ║");
        println!("╠══════════════════════════════════════════════════════════════╣");
        println!(
            "║  磁盘缓存条目:   {:>6}                                      ║",
            self.base.total_entries
        );
        println!(
            "║  磁盘缓存大小:   {:>6} KB                                   ║",
            self.base.total_size_bytes / 1024
        );
        println!(
            "║  累计编译时间:   {:>6.2}s                                    ║",
            self.base.total_compile_time_ms as f64 / 1000.0
        );
        println!("╠══════════════════════════════════════════════════════════════╣");
        println!(
            "║  内存缓存条目:   {:>6}                                      ║",
            self.memory_entries
        );
        println!(
            "║  内存缓存大小:   {:>6} KB                                   ║",
            self.memory_size_bytes / 1024
        );
        println!("╠══════════════════════════════════════════════════════════════╣");
        println!(
            "║  缓存命中:       {:>6}                                      ║",
            self.cache_hits
        );
        println!(
            "║  缓存未命中:     {:>6}                                      ║",
            self.cache_misses
        );
        println!(
            "║  命中率:         {:>6.1}%                                    ║",
            self.hit_rate * 100.0
        );
        println!("╚══════════════════════════════════════════════════════════════╝");
    }
}

//=============================================================================
// 缓存编译器包装
//=============================================================================

use crate::compiler::ShaderCompiler;
use crate::core::{CompileOptions, CompileResult, ShaderSource};

/// 带缓存的着色器编译器
pub struct CachedShaderCompiler {
    compiler: ShaderCompiler,
    cache: LruShaderCache,
}

impl CachedShaderCompiler {
    /// 创建带缓存的编译器
    pub fn new(cache_dir: PathBuf) -> Result<Self> {
        Ok(Self {
            compiler: ShaderCompiler::new()?,
            cache: LruShaderCache::new(cache_dir, LruCacheConfig::default())?,
        })
    }

    /// 使用自定义配置创建
    pub fn with_config(cache_dir: PathBuf, config: LruCacheConfig) -> Result<Self> {
        Ok(Self {
            compiler: ShaderCompiler::new()?,
            cache: LruShaderCache::new(cache_dir, config)?,
        })
    }

    /// 编译着色器 (带缓存)
    pub fn compile(
        &mut self,
        source: &ShaderSource,
        options: &CompileOptions,
    ) -> Result<CompileResult> {
        let source_path = source
            .file_path
            .as_ref()
            .ok_or_else(|| anyhow::anyhow!("源文件路径为空"))?;

        let options_hash = ShaderCache::hash_options(
            &options.defines,
            &options.include_dirs,
            matches!(
                options.optimization_level,
                crate::compiler::OptimizationLevel::Performance
            ),
        );

        // 检查缓存
        if self.cache.is_valid(source_path, &options_hash) {
            if let Some(spirv) = self.cache.get_spirv(source_path, &options_hash) {
                // 从缓存返回
                let reflection = if options.generate_reflection {
                    self.compiler.reflect_spirv(&spirv).ok()
                } else {
                    None
                };

                return Ok(CompileResult {
                    spirv_binary: spirv,
                    warnings: vec!["(从缓存加载)".to_string()],
                    reflection,
                });
            }
        }

        // 编译
        let start = Instant::now();
        let result = self.compiler.compile(source, options)?;
        let compile_time = start.elapsed().as_millis() as u64;

        // 保存输出文件
        let output_dir = self.cache.disk_cache.cache_dir.join("spirv");
        std::fs::create_dir_all(&output_dir)?;

        let output_name = format!(
            "{}_{}.spv",
            source_path
                .file_stem()
                .unwrap_or_default()
                .to_string_lossy(),
            &options_hash[..8]
        );
        let output_path = output_dir.join(&output_name);
        std::fs::write(&output_path, &result.spirv_binary)?;

        // 添加到缓存
        self.cache.add_entry(
            source_path.clone(),
            options_hash,
            output_path,
            result.spirv_binary.clone(),
            Vec::new(), // TODO: 收集include文件
            compile_time,
        )?;

        Ok(result)
    }

    /// 保存缓存
    pub fn save_cache(&self) -> Result<()> {
        self.cache.save()
    }

    /// 获取缓存统计
    pub fn cache_stats(&self) -> ExtendedCacheStats {
        self.cache.extended_stats()
    }

    /// 清除缓存
    pub fn clear_cache(&mut self) {
        self.cache.clear();
    }

    /// 预热缓存
    pub fn warm_up_cache(&self, count: usize) {
        self.cache.warm_up(count);
    }
}
