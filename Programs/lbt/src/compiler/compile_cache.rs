// ============================================================
// 文件名称：compile_cache.rs
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：内容寻址编译缓存 — 基于输入哈希精确匹配已编译
//           产物，跳过重复编译。UE5 完全依赖时间戳的增量编译
//           无法跨分支/跨机器复用产物，我们做到真正的内容寻址
//           缓存，类似 ccache/sccache 但深度集成于构建系统
// 功能描述：编译缓存系统 — 基于源码+编译选项+头文件依赖的
//           SHA-256 内容哈希进行精确缓存命中判定，支持 LRU
//           淘汰策略、缓存统计报告、命中率追踪、磁盘持久化
// 技术特性：SHA-256 (sha2 crate) 内容寻址、LRU 淘汰、
//           JSON 磁盘持久化、parking_lot::RwLock 并发安全、
//           分片目录存储产物文件、跨分支缓存复用
//
// ── 结构体表 ──────────────────────────────────────────────
// │ 结构体名                    │ 描述                         │
// │────────────────────────────│─────────────────────────────│
// │ CompileCache               │ 编译缓存管理器 (线程安全)      │
// │ CompileCacheInner           │ 缓存内部状态                  │
// │ CacheEntry                 │ 缓存条目                     │
// │ CacheKey                   │ 缓存键 (SHA-256 哈希)         │
// │ CacheStats                 │ 缓存统计                     │
// │ CacheConfig                │ 缓存配置                     │
// │ CacheReport                │ 缓存报告                     │
//
// ── 函数表 ──────────────────────────────────────────────
// │ 函数名                      │ 描述                         │
// │────────────────────────────│─────────────────────────────│
// │ new()                      │ 创建缓存管理器                │
// │ load_or_create()           │ 从磁盘加载或创建新缓存         │
// │ compute_cache_key()        │ 计算缓存键 (真 SHA-256)       │
// │ hash_file()                │ 计算文件内容 SHA-256           │
// │ lookup()                   │ 查询缓存                     │
// │ store()                    │ 存储编译产物到缓存+磁盘        │
// │ restore_artifact()         │ 从缓存恢复产物到目标路径        │
// │ evict_lru()                │ LRU 淘汰                     │
// │ get_stats()                │ 获取缓存统计                  │
// │ generate_report()          │ 生成缓存报告                  │
// │ clear()                    │ 清空缓存                     │
// │ save_index()               │ 持久化缓存索引到磁盘           │
// │ load_index()               │ 从磁盘加载缓存索引             │
//
// ── 更新历史 ──────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                         │
// │─────────────│──────────│─────────────────────────────│
// │ 2026-04-06  │ LimxTeam  │ 初始创建                      │
// │ 2026-04-06  │ LimxTeam  │ 真 SHA-256 + 磁盘持久化 +     │
// │             │           │ 并发安全 + 管线集成             │
// ============================================================

use parking_lot::RwLock;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use std::io;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::{SystemTime, UNIX_EPOCH};

// =============================================================================
// 缓存配置
// =============================================================================

/// 缓存配置
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CacheConfig {
    /// 最大缓存条目数
    pub max_entries: usize,
    /// 最大缓存大小 (字节)
    pub max_size_bytes: u64,
    /// 缓存目录路径 (磁盘存储根目录)
    pub cache_dir: PathBuf,
    /// 是否启用
    pub enabled: bool,
}

impl Default for CacheConfig {
    fn default() -> Self {
        Self {
            max_entries: 50_000,
            max_size_bytes: 10 * 1024 * 1024 * 1024, // 10 GB
            cache_dir: PathBuf::from("Intermediate/CompileCache"),
            enabled: true,
        }
    }
}

// =============================================================================
// 缓存键
// =============================================================================

/// 缓存键 — 基于编译输入的 SHA-256 内容哈希
#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct CacheKey {
    /// SHA-256 哈希 (64 字符十六进制)
    pub hash: String,
}

impl CacheKey {
    /// 从原始哈希字符串创建
    pub fn from_hash(hash: String) -> Self {
        Self { hash }
    }

    /// 获取缓存子目录分片 (前两位字符)
    pub fn shard_prefix(&self) -> &str {
        if self.hash.len() >= 2 {
            &self.hash[..2]
        } else {
            "00"
        }
    }

    /// 获取此键在缓存目录下的产物存储路径
    pub fn artifact_dir(&self, cache_root: &Path) -> PathBuf {
        cache_root
            .join("objects")
            .join(self.shard_prefix())
            .join(&self.hash)
    }
}

/// 计算缓存键的输入
#[derive(Debug, Clone)]
pub struct CacheKeyInput {
    /// 源文件内容哈希
    pub source_hash: String,
    /// 编译选项指纹 (编译器版本 + flags + defines)
    pub compiler_fingerprint: String,
    /// 头文件依赖哈希 (所有 #include 文件的聚合哈希)
    pub dependencies_hash: String,
    /// 目标平台标识
    pub target_platform: String,
    /// 构建配置 (Debug/Development/Release)
    pub build_configuration: String,
}

/// 计算缓存键 — 使用真正的 SHA-256
pub fn compute_cache_key(input: &CacheKeyInput) -> CacheKey {
    let combined = format!(
        "lbt-cc-v1|{}|{}|{}|{}|{}",
        input.source_hash,
        input.compiler_fingerprint,
        input.dependencies_hash,
        input.target_platform,
        input.build_configuration,
    );
    CacheKey {
        hash: sha256_string(&combined),
    }
}

/// 对字符串计算 SHA-256，返回 64 字符十六进制
fn sha256_string(input: &str) -> String {
    let mut hasher = Sha256::new();
    hasher.update(input.as_bytes());
    hex::encode(hasher.finalize())
}

/// 对文件内容计算 SHA-256，返回 64 字符十六进制
pub fn hash_file(path: &Path) -> io::Result<String> {
    let content = std::fs::read(path)?;
    let mut hasher = Sha256::new();
    hasher.update(&content);
    Ok(hex::encode(hasher.finalize()))
}

/// 对多个文件计算聚合 SHA-256 (按路径排序后逐文件哈希)
pub fn hash_files(paths: &[PathBuf]) -> io::Result<String> {
    let mut sorted_paths = paths.to_vec();
    sorted_paths.sort();

    let mut hasher = Sha256::new();
    for path in &sorted_paths {
        match std::fs::read(path) {
            Ok(content) => {
                // 包含路径信息防止不同路径相同内容的碰撞
                hasher.update(path.to_string_lossy().as_bytes());
                hasher.update(b"|");
                hasher.update(&content);
                hasher.update(b"\n");
            }
            Err(_) => {
                // 文件不存在时用路径的哈希作为占位
                hasher.update(b"MISSING:");
                hasher.update(path.to_string_lossy().as_bytes());
                hasher.update(b"\n");
            }
        }
    }
    Ok(hex::encode(hasher.finalize()))
}

// =============================================================================
// 缓存条目
// =============================================================================

/// 缓存条目
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CacheEntry {
    /// 缓存键
    pub key: CacheKey,
    /// 源文件路径
    pub source_file: String,
    /// 产物文件名 (如 main.obj — 仅文件名，不含路径)
    pub artifact_name: String,
    /// 产物大小 (字节)
    pub artifact_size: u64,
    /// 创建时间戳 (Unix 秒)
    pub created_at: u64,
    /// 最后访问时间戳
    pub last_accessed: u64,
    /// 访问次数
    pub access_count: u32,
    /// 编译耗时 (毫秒)
    pub compile_time_ms: u64,
    /// 编译器版本
    pub compiler_version: String,
    /// 构建配置
    pub build_config: String,
}

impl CacheEntry {
    /// 更新访问时间
    pub fn touch(&mut self) {
        self.last_accessed = current_timestamp();
        self.access_count += 1;
    }
}

// =============================================================================
// 缓存统计
// =============================================================================

/// 缓存统计
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct CacheStats {
    /// 总查询次数
    pub total_lookups: u64,
    /// 命中次数
    pub hits: u64,
    /// 未命中次数
    pub misses: u64,
    /// 存储次数
    pub stores: u64,
    /// 淘汰次数
    pub evictions: u64,
    /// 当前条目数
    pub current_entries: usize,
    /// 当前总大小 (字节)
    pub current_size_bytes: u64,
    /// 累计节省的编译时间 (毫秒)
    pub saved_compile_time_ms: u64,
}

impl CacheStats {
    /// 命中率
    pub fn hit_rate(&self) -> f64 {
        if self.total_lookups == 0 {
            0.0
        } else {
            self.hits as f64 / self.total_lookups as f64
        }
    }

    /// 命中率百分比
    pub fn hit_rate_percent(&self) -> f64 {
        self.hit_rate() * 100.0
    }

    /// 缓存使用效率 (节省毫秒数 / 缓存体积 MB)
    pub fn efficiency_score(&self) -> f64 {
        if self.current_size_bytes == 0 {
            0.0
        } else {
            self.saved_compile_time_ms as f64 / (self.current_size_bytes as f64 / 1024.0 / 1024.0)
        }
    }
}

// =============================================================================
// 缓存报告
// =============================================================================

/// 缓存报告
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CacheReport {
    /// 统计数据
    pub stats: CacheStats,
    /// 配置信息
    pub config: CacheConfig,
    /// 最热门的缓存条目 (按访问次数)
    pub hottest_entries: Vec<CacheEntryDigest>,
    /// 最大的缓存条目 (按产物大小)
    pub largest_entries: Vec<CacheEntryDigest>,
    /// 最旧的缓存条目
    pub oldest_entries: Vec<CacheEntryDigest>,
    /// 空间利用率百分比
    pub space_utilization_percent: f64,
    /// 按构建配置分组的统计
    pub per_config_stats: HashMap<String, ConfigCacheStats>,
}

/// 缓存条目摘要 (用于报告)
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CacheEntryDigest {
    pub source_file: String,
    pub artifact_size: u64,
    pub access_count: u32,
    pub compile_time_ms: u64,
    pub created_at: u64,
}

/// 按构建配置分组的统计
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct ConfigCacheStats {
    pub entry_count: usize,
    pub total_size: u64,
    pub total_hits: u64,
}

// =============================================================================
// 磁盘持久化索引
// =============================================================================

/// 持久化到磁盘的索引文件结构
#[derive(Debug, Serialize, Deserialize)]
struct DiskIndex {
    /// 版本号
    version: u32,
    /// 缓存条目
    entries: Vec<CacheEntry>,
    /// 统计数据
    stats: CacheStats,
}

const INDEX_VERSION: u32 = 1;
const INDEX_FILENAME: &str = "cache_index.json";

// =============================================================================
// 编译缓存管理器 (线程安全)
// =============================================================================

/// 编译缓存内部状态
struct CompileCacheInner {
    /// 配置
    config: CacheConfig,
    /// 缓存索引 (键 -> 条目)
    index: HashMap<CacheKey, CacheEntry>,
    /// 统计数据
    stats: CacheStats,
}

/// 编译缓存管理器 — 线程安全，支持磁盘持久化
///
/// 通过 `Arc<RwLock>` 包装内部状态，多线程可同时读缓存，
/// 写入时自动获取排他锁。调用 `save_index()` 将索引持久化到磁盘。
pub struct CompileCache {
    inner: Arc<RwLock<CompileCacheInner>>,
}

impl Clone for CompileCache {
    fn clone(&self) -> Self {
        Self {
            inner: Arc::clone(&self.inner),
        }
    }
}

impl CompileCache {
    /// 创建缓存管理器
    pub fn new(config: CacheConfig) -> Self {
        Self {
            inner: Arc::new(RwLock::new(CompileCacheInner {
                config,
                index: HashMap::new(),
                stats: CacheStats::default(),
            })),
        }
    }

    /// 使用默认配置创建
    pub fn with_defaults() -> Self {
        Self::new(CacheConfig::default())
    }

    /// 从磁盘加载已有缓存索引，若不存在则创建新缓存
    pub fn load_or_create(config: CacheConfig) -> Self {
        let index_path = config.cache_dir.join(INDEX_FILENAME);
        if index_path.exists() {
            match std::fs::read_to_string(&index_path) {
                Ok(json) => {
                    if let Ok(disk_index) = serde_json::from_str::<DiskIndex>(&json) {
                        if disk_index.version == INDEX_VERSION {
                            let mut index = HashMap::with_capacity(disk_index.entries.len());
                            for entry in disk_index.entries {
                                index.insert(entry.key.clone(), entry);
                            }
                            return Self {
                                inner: Arc::new(RwLock::new(CompileCacheInner {
                                    config,
                                    index,
                                    stats: disk_index.stats,
                                })),
                            };
                        }
                    }
                }
                Err(_) => {}
            }
        }
        Self::new(config)
    }

    /// 查询缓存 — 返回条目的克隆 (避免持有读锁)
    pub fn lookup(&self, key: &CacheKey) -> Option<CacheEntry> {
        let mut inner = self.inner.write();
        inner.stats.total_lookups += 1;

        if let Some(entry) = inner.index.get_mut(key) {
            entry.touch();
            let cloned = entry.clone();
            inner.stats.hits += 1;
            inner.stats.saved_compile_time_ms += cloned.compile_time_ms;
            Some(cloned)
        } else {
            inner.stats.misses += 1;
            None
        }
    }

    /// 存储编译产物到缓存索引 + 复制产物文件到缓存目录
    ///
    /// `artifact_source_path` 是刚编译出的 .obj 文件的实际路径，
    /// 该文件会被复制到缓存目录的分片子目录中。
    pub fn store(&self, entry: CacheEntry, artifact_source_path: &Path) -> io::Result<()> {
        let mut inner = self.inner.write();

        // 复制产物到缓存目录
        let artifact_dir = entry.key.artifact_dir(&inner.config.cache_dir);
        std::fs::create_dir_all(&artifact_dir)?;
        let cached_artifact = artifact_dir.join(&entry.artifact_name);
        std::fs::copy(artifact_source_path, &cached_artifact)?;

        let artifact_size = entry.artifact_size;

        // LRU 淘汰
        while needs_eviction_inner(&inner, artifact_size) {
            if !evict_lru_inner(&mut inner) {
                break;
            }
        }

        inner.stats.current_size_bytes += artifact_size;
        inner.stats.current_entries += 1;
        inner.stats.stores += 1;
        inner.index.insert(entry.key.clone(), entry);

        Ok(())
    }

    /// 仅在索引中存储条目 (不复制磁盘产物 — 用于测试或索引导入)
    pub fn store_index_only(&self, entry: CacheEntry) {
        let mut inner = self.inner.write();
        let artifact_size = entry.artifact_size;

        while needs_eviction_inner(&inner, artifact_size) {
            if !evict_lru_inner(&mut inner) {
                break;
            }
        }

        inner.stats.current_size_bytes += artifact_size;
        inner.stats.current_entries += 1;
        inner.stats.stores += 1;
        inner.index.insert(entry.key.clone(), entry);
    }

    /// 从缓存恢复产物到目标路径
    ///
    /// 在缓存命中后调用此方法将缓存中的 .obj 复制到编译输出目录。
    pub fn restore_artifact(&self, key: &CacheKey, target_path: &Path) -> io::Result<bool> {
        let inner = self.inner.read();

        if let Some(entry) = inner.index.get(key) {
            let cached_artifact = key
                .artifact_dir(&inner.config.cache_dir)
                .join(&entry.artifact_name);

            if cached_artifact.exists() {
                if let Some(parent) = target_path.parent() {
                    std::fs::create_dir_all(parent)?;
                }
                std::fs::copy(&cached_artifact, target_path)?;
                return Ok(true);
            }
        }
        Ok(false)
    }

    /// 持久化缓存索引到磁盘
    pub fn save_index(&self) -> io::Result<()> {
        let inner = self.inner.read();
        let cache_dir = &inner.config.cache_dir;
        std::fs::create_dir_all(cache_dir)?;

        let disk_index = DiskIndex {
            version: INDEX_VERSION,
            entries: inner.index.values().cloned().collect(),
            stats: inner.stats.clone(),
        };

        let json = serde_json::to_string_pretty(&disk_index)
            .map_err(|e| io::Error::new(io::ErrorKind::Other, e))?;

        let index_path = cache_dir.join(INDEX_FILENAME);
        // 先写临时文件再原子重命名，避免索引损坏
        let tmp_path = cache_dir.join("cache_index.tmp");
        std::fs::write(&tmp_path, &json)?;
        std::fs::rename(&tmp_path, &index_path)?;

        Ok(())
    }

    /// 获取统计数据 (克隆)
    pub fn get_stats(&self) -> CacheStats {
        self.inner.read().stats.clone()
    }

    /// 获取缓存配置 (克隆)
    pub fn get_config(&self) -> CacheConfig {
        self.inner.read().config.clone()
    }

    /// 获取缓存条目数
    pub fn entry_count(&self) -> usize {
        self.inner.read().index.len()
    }

    /// 检查缓存中是否存在某键
    pub fn contains(&self, key: &CacheKey) -> bool {
        self.inner.read().index.contains_key(key)
    }

    /// 清空缓存 (索引 + 磁盘产物)
    pub fn clear(&self) -> io::Result<()> {
        let mut inner = self.inner.write();
        let objects_dir = inner.config.cache_dir.join("objects");
        if objects_dir.exists() {
            std::fs::remove_dir_all(&objects_dir)?;
        }
        inner.index.clear();
        inner.stats.current_entries = 0;
        inner.stats.current_size_bytes = 0;
        Ok(())
    }

    /// 仅清空索引 (不删除磁盘文件 — 用于测试)
    pub fn clear_index_only(&self) {
        let mut inner = self.inner.write();
        inner.index.clear();
        inner.stats.current_entries = 0;
        inner.stats.current_size_bytes = 0;
    }

    /// LRU 淘汰 — 移除最近最少使用的条目及其磁盘产物
    pub fn evict_lru(&self) -> bool {
        let mut inner = self.inner.write();
        evict_lru_inner(&mut inner)
    }

    /// 批量淘汰到目标大小以下
    pub fn evict_to_size(&self, target_size_bytes: u64) -> usize {
        let mut inner = self.inner.write();
        let mut evicted = 0;
        while inner.stats.current_size_bytes > target_size_bytes && !inner.index.is_empty() {
            if evict_lru_inner(&mut inner) {
                evicted += 1;
            } else {
                break;
            }
        }
        evicted
    }

    /// 生成缓存报告
    pub fn generate_report(&self) -> CacheReport {
        let inner = self.inner.read();
        let mut entries: Vec<&CacheEntry> = inner.index.values().collect();

        // 最热门 (按访问次数)
        let mut by_access = entries.clone();
        by_access.sort_by(|a, b| b.access_count.cmp(&a.access_count));
        let hottest: Vec<CacheEntryDigest> = by_access
            .iter()
            .take(10)
            .map(|e| entry_to_digest(e))
            .collect();

        // 最大 (按产物大小)
        let mut by_size = entries.clone();
        by_size.sort_by(|a, b| b.artifact_size.cmp(&a.artifact_size));
        let largest: Vec<CacheEntryDigest> = by_size
            .iter()
            .take(10)
            .map(|e| entry_to_digest(e))
            .collect();

        // 最旧 (按创建时间)
        entries.sort_by_key(|e| e.created_at);
        let oldest: Vec<CacheEntryDigest> = entries
            .iter()
            .take(10)
            .map(|e| entry_to_digest(e))
            .collect();

        // 空间利用率
        let space_util = if inner.config.max_size_bytes > 0 {
            inner.stats.current_size_bytes as f64 / inner.config.max_size_bytes as f64 * 100.0
        } else {
            0.0
        };

        // 按构建配置分组
        let mut per_config: HashMap<String, ConfigCacheStats> = HashMap::new();
        for entry in inner.index.values() {
            let stat = per_config.entry(entry.build_config.clone()).or_default();
            stat.entry_count += 1;
            stat.total_size += entry.artifact_size;
            stat.total_hits += entry.access_count as u64;
        }

        CacheReport {
            stats: inner.stats.clone(),
            config: inner.config.clone(),
            hottest_entries: hottest,
            largest_entries: largest,
            oldest_entries: oldest,
            space_utilization_percent: space_util,
            per_config_stats: per_config,
        }
    }

    /// 格式化显示节省的编译时间
    pub fn saved_time_display(&self) -> String {
        let ms = self.inner.read().stats.saved_compile_time_ms;
        if ms < 1000 {
            format!("{}ms", ms)
        } else if ms < 60_000 {
            format!("{:.1}s", ms as f64 / 1000.0)
        } else {
            format!("{:.1}min", ms as f64 / 60_000.0)
        }
    }
}

// =============================================================================
// 内部辅助函数 (在持有锁的上下文中调用)
// =============================================================================

fn needs_eviction_inner(inner: &CompileCacheInner, incoming_size: u64) -> bool {
    if !inner.config.enabled {
        return false;
    }
    let would_exceed_entries = inner.stats.current_entries >= inner.config.max_entries;
    let would_exceed_size =
        inner.stats.current_size_bytes + incoming_size > inner.config.max_size_bytes;
    would_exceed_entries || would_exceed_size
}

fn evict_lru_inner(inner: &mut CompileCacheInner) -> bool {
    if inner.index.is_empty() {
        return false;
    }

    let oldest_key = inner
        .index
        .iter()
        .min_by_key(|(_, entry)| entry.last_accessed)
        .map(|(key, _)| key.clone());

    if let Some(key) = oldest_key {
        // 删除磁盘产物
        let artifact_dir = key.artifact_dir(&inner.config.cache_dir);
        let _ = std::fs::remove_dir_all(&artifact_dir);

        if let Some(removed) = inner.index.remove(&key) {
            inner.stats.current_size_bytes = inner
                .stats
                .current_size_bytes
                .saturating_sub(removed.artifact_size);
            inner.stats.current_entries = inner.stats.current_entries.saturating_sub(1);
            inner.stats.evictions += 1;
            return true;
        }
    }
    false
}

fn entry_to_digest(entry: &CacheEntry) -> CacheEntryDigest {
    CacheEntryDigest {
        source_file: entry.source_file.clone(),
        artifact_size: entry.artifact_size,
        access_count: entry.access_count,
        compile_time_ms: entry.compile_time_ms,
        created_at: entry.created_at,
    }
}

fn current_timestamp() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

// =============================================================================
// 测试
// =============================================================================

#[cfg(test)]
mod tests {
    use super::*;

    fn make_entry(key_hash: &str, source: &str, size: u64, config: &str) -> CacheEntry {
        CacheEntry {
            key: CacheKey::from_hash(key_hash.to_string()),
            source_file: source.to_string(),
            artifact_name: format!("{}.obj", source.replace(".cpp", "")),
            artifact_size: size,
            created_at: current_timestamp(),
            last_accessed: current_timestamp(),
            access_count: 0,
            compile_time_ms: 500,
            compiler_version: "MSVC 19.38".to_string(),
            build_config: config.to_string(),
        }
    }

    #[test]
    fn test_real_sha256() {
        // 验证使用真正的 SHA-256
        let hash = sha256_string("hello world");
        assert_eq!(hash.len(), 64, "SHA-256 应产生 64 字符十六进制");
        // 已知值验证
        assert_eq!(
            hash,
            "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9"
        );
    }

    #[test]
    fn test_cache_store_and_lookup() {
        let cache = CompileCache::with_defaults();
        let entry = make_entry("aabbccdd", "main.cpp", 4096, "Debug");
        let key = entry.key.clone();

        cache.store_index_only(entry);
        assert_eq!(cache.entry_count(), 1);

        let result = cache.lookup(&key);
        assert!(result.is_some());
        assert_eq!(result.unwrap().source_file, "main.cpp");
    }

    #[test]
    fn test_cache_miss() {
        let cache = CompileCache::with_defaults();
        let key = CacheKey::from_hash("nonexistent".to_string());

        let result = cache.lookup(&key);
        assert!(result.is_none());
        assert_eq!(cache.get_stats().misses, 1);
    }

    #[test]
    fn test_hit_rate() {
        let cache = CompileCache::with_defaults();
        let entry = make_entry("abc123", "test.cpp", 1024, "Release");
        let key = entry.key.clone();

        cache.store_index_only(entry);
        cache.lookup(&key);
        cache.lookup(&CacheKey::from_hash("missing".to_string()));

        let stats = cache.get_stats();
        assert_eq!(stats.hits, 1);
        assert_eq!(stats.misses, 1);
        assert!((stats.hit_rate() - 0.5).abs() < 0.001);
    }

    #[test]
    fn test_lru_eviction() {
        let config = CacheConfig {
            max_entries: 3,
            max_size_bytes: 1_000_000,
            cache_dir: PathBuf::from("test_cache_lru"),
            enabled: true,
        };
        let cache = CompileCache::new(config);

        for i in 0..3 {
            let mut entry = make_entry(
                &format!("key{}", i),
                &format!("file{}.cpp", i),
                100,
                "Debug",
            );
            entry.last_accessed = 1000 + i as u64;
            cache.store_index_only(entry);
        }
        assert_eq!(cache.entry_count(), 3);

        let entry4 = make_entry("key3", "file3.cpp", 100, "Debug");
        cache.store_index_only(entry4);

        assert_eq!(cache.entry_count(), 3);
        assert!(!cache.contains(&CacheKey::from_hash("key0".to_string())));
        assert!(cache.contains(&CacheKey::from_hash("key3".to_string())));
    }

    #[test]
    fn test_size_based_eviction() {
        let config = CacheConfig {
            max_entries: 100,
            max_size_bytes: 300,
            cache_dir: PathBuf::from("test_cache_size"),
            enabled: true,
        };
        let cache = CompileCache::new(config);

        for i in 0..3 {
            let mut entry = make_entry(&format!("s{}", i), &format!("f{}.cpp", i), 100, "Debug");
            entry.last_accessed = 100 + i as u64;
            cache.store_index_only(entry);
        }

        let big = make_entry("big", "big.cpp", 200, "Debug");
        cache.store_index_only(big);

        assert!(cache.get_stats().current_size_bytes <= 300);
        assert!(cache.contains(&CacheKey::from_hash("big".to_string())));
    }

    #[test]
    fn test_compute_cache_key_deterministic() {
        let input = CacheKeyInput {
            source_hash: "abc123".to_string(),
            compiler_fingerprint: "msvc_19.38".to_string(),
            dependencies_hash: "dep456".to_string(),
            target_platform: "win64".to_string(),
            build_configuration: "Debug".to_string(),
        };

        let key1 = compute_cache_key(&input);
        let key2 = compute_cache_key(&input);
        assert_eq!(key1, key2, "相同输入应产生相同键");
        assert_eq!(key1.hash.len(), 64, "SHA-256 哈希应为 64 字符");

        let mut input2 = input.clone();
        input2.build_configuration = "Release".to_string();
        let key3 = compute_cache_key(&input2);
        assert_ne!(key1, key3, "不同输入应产生不同键");
    }

    #[test]
    fn test_cache_key_shard() {
        let key = CacheKey::from_hash("abcdef1234".to_string());
        assert_eq!(key.shard_prefix(), "ab");
    }

    #[test]
    fn test_clear_cache() {
        let cache = CompileCache::with_defaults();
        cache.store_index_only(make_entry("k1", "a.cpp", 100, "Debug"));
        cache.store_index_only(make_entry("k2", "b.cpp", 200, "Debug"));

        assert_eq!(cache.entry_count(), 2);
        cache.clear_index_only();
        assert_eq!(cache.entry_count(), 0);
        assert_eq!(cache.get_stats().current_size_bytes, 0);
    }

    #[test]
    fn test_cache_report() {
        let cache = CompileCache::with_defaults();
        cache.store_index_only(make_entry("r1", "render.cpp", 8192, "Release"));
        cache.store_index_only(make_entry("r2", "physics.cpp", 4096, "Debug"));
        cache.store_index_only(make_entry("r3", "audio.cpp", 2048, "Release"));

        let report = cache.generate_report();
        assert_eq!(report.stats.current_entries, 3);
        assert_eq!(report.per_config_stats.len(), 2);
        assert!(report.per_config_stats.contains_key("Release"));
        assert!(report.per_config_stats.contains_key("Debug"));
    }

    #[test]
    fn test_disk_persistence_round_trip() {
        let tmp = std::env::temp_dir().join("lbt_cache_test_persist");
        let _ = std::fs::remove_dir_all(&tmp);

        let config = CacheConfig {
            max_entries: 100,
            max_size_bytes: 1_000_000,
            cache_dir: tmp.clone(),
            enabled: true,
        };

        // 创建缓存并写入
        {
            let cache = CompileCache::new(config.clone());
            cache.store_index_only(make_entry("p1", "persist.cpp", 512, "Debug"));
            cache.store_index_only(make_entry("p2", "persist2.cpp", 1024, "Release"));
            cache.save_index().unwrap();
        }

        // 从磁盘加载
        {
            let cache = CompileCache::load_or_create(config.clone());
            assert_eq!(cache.entry_count(), 2);
            assert!(cache.contains(&CacheKey::from_hash("p1".to_string())));
            assert!(cache.contains(&CacheKey::from_hash("p2".to_string())));
        }

        let _ = std::fs::remove_dir_all(&tmp);
    }

    #[test]
    fn test_thread_safety() {
        // 验证多线程并发访问不 panic
        let cache = CompileCache::with_defaults();
        let cache_clone = cache.clone();

        let handle = std::thread::spawn(move || {
            for i in 0..100 {
                let entry = make_entry(
                    &format!("thread_key_{}", i),
                    &format!("thread_{}.cpp", i),
                    64,
                    "Debug",
                );
                cache_clone.store_index_only(entry);
            }
        });

        for i in 0..100 {
            let key = CacheKey::from_hash(format!("thread_key_{}", i));
            let _ = cache.lookup(&key);
        }

        handle.join().unwrap();
        // 由于并发竞争，条目数可能在 0..200 范围内
        assert!(cache.entry_count() <= 200);
    }

    #[test]
    fn test_saved_time_display() {
        let cache = CompileCache::with_defaults();
        let entry = make_entry("t1", "slow.cpp", 100, "Debug");
        let key = entry.key.clone();
        cache.store_index_only(entry);

        cache.lookup(&key);
        assert_eq!(cache.saved_time_display(), "500ms");
    }

    #[test]
    fn test_evict_to_size() {
        let cache = CompileCache::with_defaults();
        for i in 0..10 {
            let mut entry = make_entry(&format!("e{}", i), &format!("f{}.cpp", i), 1000, "Debug");
            entry.last_accessed = 100 + i as u64;
            cache.store_index_only(entry);
        }
        assert_eq!(cache.get_stats().current_size_bytes, 10_000);

        let evicted = cache.evict_to_size(5000);
        assert!(evicted >= 5);
        assert!(cache.get_stats().current_size_bytes <= 5000);
    }

    #[test]
    fn test_access_count_increments() {
        let cache = CompileCache::with_defaults();
        let entry = make_entry("ac1", "counted.cpp", 100, "Debug");
        let key = entry.key.clone();
        cache.store_index_only(entry);

        cache.lookup(&key);
        cache.lookup(&key);
        cache.lookup(&key);

        let found = cache.lookup(&key).unwrap();
        assert_eq!(found.access_count, 4);
    }

    #[test]
    fn test_efficiency_score() {
        let mut stats = CacheStats::default();
        stats.saved_compile_time_ms = 60_000;
        stats.current_size_bytes = 100 * 1024 * 1024;
        let eff = stats.efficiency_score();
        assert!(eff > 0.0, "效率评分应大于0");
    }

    #[test]
    fn test_hash_file() {
        let tmp = std::env::temp_dir().join("lbt_hash_test.txt");
        std::fs::write(&tmp, b"hello world").unwrap();
        let hash = hash_file(&tmp).unwrap();
        assert_eq!(hash.len(), 64);
        // sha256("hello world") 的已知值
        assert_eq!(
            hash,
            "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9"
        );
        let _ = std::fs::remove_file(&tmp);
    }
}
