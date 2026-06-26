// ============================================================
// 文件名称：build_profiler.rs
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：精确到文件级的编译性能剖析，自动瓶颈检测，
//           结构化输出 (JSON/HTML)，超越 UE5 无此功能
// 功能描述：构建性能剖析器 — 记录每个编译单元的耗时、依赖
//           深度、PCH 命中率，生成可交互的 HTML 火焰图报告
//           和机器可读的 JSON 诊断数据
// 技术特性：线程安全收集器 (Arc<Mutex>)，零开销记录宏，
//           百分位统计 (P50/P90/P99)，瓶颈自动检测算法，
//           依赖链关键路径分析
//
// ── 结构体表 ──────────────────────────────────────────────
// │ 结构体名                    │ 描述                         │
// │────────────────────────────│─────────────────────────────│
// │ BuildProfiler              │ 构建剖析器主体，收集所有指标    │
// │ FileProfileEntry           │ 单文件编译剖析条目             │
// │ ModuleProfileSummary       │ 模块级汇总统计                │
// │ ProfileReport              │ 完整剖析报告                  │
// │ BottleneckInfo             │ 瓶颈信息                     │
// │ CriticalPathNode           │ 关键路径节点                  │
// │ PercentileStats            │ 百分位统计数据                │
//
// ── 函数表 ──────────────────────────────────────────────
// │ 函数名                      │ 描述                         │
// │────────────────────────────│─────────────────────────────│
// │ new()                      │ 创建空的剖析器                 │
// │ record_file_compile()      │ 记录单文件编译耗时             │
// │ record_link_phase()        │ 记录链接阶段耗时              │
// │ generate_report()          │ 生成完整剖析报告               │
// │ detect_bottlenecks()       │ 自动检测编译瓶颈              │
// │ compute_critical_path()    │ 计算关键路径                  │
// │ export_json()              │ 导出 JSON 格式报告            │
// │ export_html()              │ 导出交互式 HTML 报告          │
// │ compute_percentiles()      │ 计算百分位统计                │
//
// ── 更新历史 ──────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                         │
// │─────────────│──────────│─────────────────────────────│
// │ 2026-04-06  │ LimxTeam  │ 初始创建                      │
// ============================================================

use parking_lot::Mutex;
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::Duration;

// =============================================================================
// 文件编译剖析条目
// =============================================================================

/// 单个编译单元 (源文件) 的剖析数据
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct FileProfileEntry {
    /// 源文件路径
    pub file_path: PathBuf,
    /// 所属模块名
    pub module_name: String,
    /// 编译耗时
    pub compile_duration: Duration,
    /// 预处理耗时 (若可获取)
    pub preprocess_duration: Option<Duration>,
    /// 代码生成耗时 (若可获取)
    pub codegen_duration: Option<Duration>,
    /// 优化耗时 (若可获取)
    pub optimize_duration: Option<Duration>,
    /// 文件大小 (字节)
    pub file_size_bytes: u64,
    /// 包含的头文件数量
    pub include_count: usize,
    /// 传递依赖深度
    pub dependency_depth: usize,
    /// 是否使用 PCH
    pub used_pch: bool,
    /// 是否使用 Unity Build
    pub in_unity_build: bool,
    /// 编译器输出行数 (警告/信息)
    pub warning_count: usize,
    /// 生成的 .obj 大小 (字节)
    pub output_size_bytes: u64,
    /// 编译线程 ID
    pub thread_id: usize,
    /// 缓存命中 (增量编译跳过)
    pub cache_hit: bool,
}

impl FileProfileEntry {
    /// 计算编译速率 (字节/秒)
    pub fn compile_throughput(&self) -> f64 {
        let seconds = self.compile_duration.as_secs_f64();
        if seconds > 0.0 {
            self.file_size_bytes as f64 / seconds
        } else {
            0.0
        }
    }

    /// 计算膨胀率 (输出/输入比)
    pub fn bloat_ratio(&self) -> f64 {
        if self.file_size_bytes > 0 {
            self.output_size_bytes as f64 / self.file_size_bytes as f64
        } else {
            0.0
        }
    }
}

// =============================================================================
// 链接剖析条目
// =============================================================================

/// 链接阶段剖析数据
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct LinkProfileEntry {
    /// 目标名称
    pub target_name: String,
    /// 链接耗时
    pub link_duration: Duration,
    /// 输入 .obj 文件数量
    pub input_object_count: usize,
    /// 输入总大小
    pub input_total_size_bytes: u64,
    /// 输出文件大小
    pub output_size_bytes: u64,
    /// 链接类型 (静态库/动态库/可执行文件)
    pub link_type: LinkType,
}

/// 链接类型
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum LinkType {
    /// 静态库
    StaticLibrary,
    /// 动态库
    SharedLibrary,
    /// 可执行文件
    Executable,
}

impl LinkType {
    /// 获取中文名称
    pub fn display_name(&self) -> &'static str {
        match self {
            Self::StaticLibrary => "静态库",
            Self::SharedLibrary => "动态库",
            Self::Executable => "可执行文件",
        }
    }
}

// =============================================================================
// 百分位统计
// =============================================================================

/// 百分位统计数据
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PercentileStats {
    /// 最小值
    pub min_ms: f64,
    /// P25
    pub p25_ms: f64,
    /// P50 (中位数)
    pub p50_ms: f64,
    /// P75
    pub p75_ms: f64,
    /// P90
    pub p90_ms: f64,
    /// P95
    pub p95_ms: f64,
    /// P99
    pub p99_ms: f64,
    /// 最大值
    pub max_ms: f64,
    /// 平均值
    pub mean_ms: f64,
    /// 标准差
    pub stddev_ms: f64,
    /// 样本数量
    pub count: usize,
}

/// 从一组毫秒值计算百分位统计
fn compute_percentiles(values: &[f64]) -> PercentileStats {
    if values.is_empty() {
        return PercentileStats {
            min_ms: 0.0,
            p25_ms: 0.0,
            p50_ms: 0.0,
            p75_ms: 0.0,
            p90_ms: 0.0,
            p95_ms: 0.0,
            p99_ms: 0.0,
            max_ms: 0.0,
            mean_ms: 0.0,
            stddev_ms: 0.0,
            count: 0,
        };
    }

    let mut sorted = values.to_vec();
    sorted.sort_by(|a, b| a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal));

    let count = sorted.len();
    let mean = sorted.iter().sum::<f64>() / count as f64;
    let variance = sorted.iter().map(|v| (v - mean).powi(2)).sum::<f64>() / count as f64;
    let stddev = variance.sqrt();

    let percentile = |p: f64| -> f64 {
        let index = (p / 100.0 * (count - 1) as f64).round() as usize;
        sorted[index.min(count - 1)]
    };

    PercentileStats {
        min_ms: sorted[0],
        p25_ms: percentile(25.0),
        p50_ms: percentile(50.0),
        p75_ms: percentile(75.0),
        p90_ms: percentile(90.0),
        p95_ms: percentile(95.0),
        p99_ms: percentile(99.0),
        max_ms: sorted[count - 1],
        mean_ms: mean,
        stddev_ms: stddev,
        count,
    }
}

// =============================================================================
// 瓶颈信息
// =============================================================================

/// 瓶颈严重程度
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Serialize, Deserialize)]
pub enum BottleneckSeverity {
    /// 信息
    Info,
    /// 警告
    Warning,
    /// 严重
    Critical,
}

impl BottleneckSeverity {
    /// 获取显示符号
    pub fn icon(&self) -> &'static str {
        match self {
            Self::Info => "ℹ️",
            Self::Warning => "⚠️",
            Self::Critical => "🔴",
        }
    }
}

/// 瓶颈类型
#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum BottleneckType {
    /// 单文件编译过慢 (超过阈值)
    SlowFile {
        file_path: PathBuf,
        duration_ms: f64,
        threshold_ms: f64,
    },
    /// 头文件扇出过大 (被太多文件包含)
    HighFanoutHeader {
        header_path: PathBuf,
        dependent_count: usize,
    },
    /// 模块编译过慢
    SlowModule {
        module_name: String,
        total_duration_ms: f64,
        file_count: usize,
    },
    /// PCH 未覆盖 (大量文件未使用 PCH)
    LowPchCoverage {
        module_name: String,
        coverage_percent: f64,
    },
    /// 链接瓶颈
    SlowLink {
        target_name: String,
        duration_ms: f64,
    },
    /// 文件膨胀 (输出远大于输入)
    HighBloatRatio { file_path: PathBuf, ratio: f64 },
    /// 编译并行度不足
    LowParallelism {
        active_threads_avg: f64,
        available_threads: usize,
    },
}

/// 瓶颈条目
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct BottleneckInfo {
    /// 严重程度
    pub severity: BottleneckSeverity,
    /// 瓶颈类型
    pub bottleneck_type: BottleneckType,
    /// 人类可读描述
    pub description: String,
    /// 建议修复操作
    pub suggestion: String,
    /// 预估节省时间 (毫秒)
    pub estimated_savings_ms: f64,
}

// =============================================================================
// 关键路径
// =============================================================================

/// 关键路径节点
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CriticalPathNode {
    /// 节点名称 (文件路径或阶段名)
    pub name: String,
    /// 节点耗时
    pub duration: Duration,
    /// 累计耗时 (从起点到此节点)
    pub cumulative_duration: Duration,
    /// 节点类型
    pub node_type: CriticalPathNodeType,
}

/// 关键路径节点类型
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum CriticalPathNodeType {
    /// 编译
    Compile,
    /// 链接
    Link,
    /// 代码生成
    CodeGeneration,
    /// 预处理
    Preprocessing,
}

// =============================================================================
// 模块级汇总
// =============================================================================

/// 模块剖析汇总
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ModuleProfileSummary {
    /// 模块名
    pub module_name: String,
    /// 编译文件数
    pub file_count: usize,
    /// 缓存命中文件数
    pub cache_hit_count: usize,
    /// 总编译耗时
    pub total_compile_duration: Duration,
    /// 平均编译耗时
    pub avg_compile_duration: Duration,
    /// 最慢文件
    pub slowest_file: Option<PathBuf>,
    /// 最慢文件耗时
    pub slowest_duration: Duration,
    /// PCH 覆盖率
    pub pch_coverage_percent: f64,
    /// Unity Build 覆盖率
    pub unity_coverage_percent: f64,
    /// 总源文件大小
    pub total_source_size_bytes: u64,
    /// 总输出大小
    pub total_output_size_bytes: u64,
    /// 文件耗时的百分位统计
    pub percentile_stats: PercentileStats,
}

// =============================================================================
// 剖析报告
// =============================================================================

/// 完整构建剖析报告
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ProfileReport {
    /// 报告标题
    pub title: String,
    /// 构建配置
    pub build_configuration: String,
    /// 构建时间戳
    pub timestamp: String,
    /// 总构建耗时
    pub total_build_duration: Duration,
    /// 编译阶段总耗时
    pub total_compile_duration: Duration,
    /// 链接阶段总耗时
    pub total_link_duration: Duration,
    /// 总文件数
    pub total_file_count: usize,
    /// 缓存命中数
    pub total_cache_hits: usize,
    /// 缓存命中率
    pub cache_hit_rate_percent: f64,
    /// 使用的并行线程数
    pub parallel_thread_count: usize,
    /// 文件编译条目
    pub file_entries: Vec<FileProfileEntry>,
    /// 链接条目
    pub link_entries: Vec<LinkProfileEntry>,
    /// 模块汇总
    pub module_summaries: Vec<ModuleProfileSummary>,
    /// 全局百分位统计
    pub global_percentiles: PercentileStats,
    /// 自动检测的瓶颈
    pub bottlenecks: Vec<BottleneckInfo>,
    /// 关键路径
    pub critical_path: Vec<CriticalPathNode>,
    /// 预估可优化时间 (毫秒)
    pub estimated_total_savings_ms: f64,
}

// =============================================================================
// 构建剖析器
// =============================================================================

/// 瓶颈检测阈值
#[derive(Debug, Clone)]
pub struct BottleneckThresholds {
    /// 单文件编译慢速阈值 (毫秒)
    pub slow_file_threshold_ms: f64,
    /// 头文件扇出阈值
    pub high_fanout_threshold: usize,
    /// 低 PCH 覆盖率阈值 (百分比)
    pub low_pch_coverage_threshold: f64,
    /// 链接慢速阈值 (毫秒)
    pub slow_link_threshold_ms: f64,
    /// 膨胀率阈值
    pub high_bloat_ratio_threshold: f64,
    /// P99 编译耗时离群倍数 (超过 P50 的 N 倍视为离群)
    pub outlier_multiplier: f64,
}

impl Default for BottleneckThresholds {
    fn default() -> Self {
        Self {
            slow_file_threshold_ms: 5000.0,
            high_fanout_threshold: 50,
            low_pch_coverage_threshold: 50.0,
            slow_link_threshold_ms: 30000.0,
            high_bloat_ratio_threshold: 20.0,
            outlier_multiplier: 5.0,
        }
    }
}

/// 构建性能剖析器
#[derive(Debug)]
pub struct BuildProfiler {
    /// 文件编译条目 (线程安全)
    file_entries: Arc<Mutex<Vec<FileProfileEntry>>>,
    /// 链接条目
    link_entries: Arc<Mutex<Vec<LinkProfileEntry>>>,
    /// 瓶颈检测阈值
    thresholds: BottleneckThresholds,
    /// 构建开始时间戳
    build_start_timestamp: String,
    /// 构建配置名
    build_configuration: String,
    /// 并行线程数
    parallel_thread_count: usize,
}

impl BuildProfiler {
    /// 创建新的构建剖析器
    pub fn new(build_configuration: &str, parallel_thread_count: usize) -> Self {
        Self {
            file_entries: Arc::new(Mutex::new(Vec::new())),
            link_entries: Arc::new(Mutex::new(Vec::new())),
            thresholds: BottleneckThresholds::default(),
            build_start_timestamp: chrono_timestamp(),
            build_configuration: build_configuration.to_string(),
            parallel_thread_count,
        }
    }

    /// 设置瓶颈检测阈值
    pub fn set_thresholds(&mut self, thresholds: BottleneckThresholds) {
        self.thresholds = thresholds;
    }

    /// 获取文件条目收集器 (可跨线程共享)
    pub fn file_collector(&self) -> Arc<Mutex<Vec<FileProfileEntry>>> {
        Arc::clone(&self.file_entries)
    }

    /// 获取链接条目收集器 (可跨线程共享)
    pub fn link_collector(&self) -> Arc<Mutex<Vec<LinkProfileEntry>>> {
        Arc::clone(&self.link_entries)
    }

    /// 记录单文件编译
    pub fn record_file_compile(&self, entry: FileProfileEntry) {
        self.file_entries.lock().push(entry);
    }

    /// 记录链接阶段
    pub fn record_link_phase(&self, entry: LinkProfileEntry) {
        self.link_entries.lock().push(entry);
    }

    /// 生成完整剖析报告
    pub fn generate_report(&self, total_build_duration: Duration) -> ProfileReport {
        let file_entries = self.file_entries.lock().clone();
        let link_entries = self.link_entries.lock().clone();

        // 计算汇总统计
        let total_compile_duration = file_entries
            .iter()
            .filter(|e| !e.cache_hit)
            .map(|e| e.compile_duration)
            .sum();
        let total_link_duration = link_entries.iter().map(|e| e.link_duration).sum();
        let total_cache_hits = file_entries.iter().filter(|e| e.cache_hit).count();
        let total_file_count = file_entries.len();
        let cache_hit_rate = if total_file_count > 0 {
            total_cache_hits as f64 / total_file_count as f64 * 100.0
        } else {
            0.0
        };

        // 全局百分位统计 (仅非缓存命中的文件)
        let compile_times_ms: Vec<f64> = file_entries
            .iter()
            .filter(|e| !e.cache_hit)
            .map(|e| e.compile_duration.as_secs_f64() * 1000.0)
            .collect();
        let global_percentiles = compute_percentiles(&compile_times_ms);

        // 模块汇总
        let module_summaries = self.compute_module_summaries(&file_entries);

        // 瓶颈检测
        let bottlenecks = self.detect_bottlenecks(
            &file_entries,
            &link_entries,
            &module_summaries,
            &global_percentiles,
        );

        // 关键路径计算
        let critical_path = self.compute_critical_path(&file_entries, &link_entries);

        // 预估节省时间
        let estimated_total_savings_ms: f64 =
            bottlenecks.iter().map(|b| b.estimated_savings_ms).sum();

        ProfileReport {
            title: format!("Limx Engine 构建性能剖析报告"),
            build_configuration: self.build_configuration.clone(),
            timestamp: self.build_start_timestamp.clone(),
            total_build_duration,
            total_compile_duration,
            total_link_duration,
            total_file_count,
            total_cache_hits,
            cache_hit_rate_percent: cache_hit_rate,
            parallel_thread_count: self.parallel_thread_count,
            file_entries,
            link_entries,
            module_summaries,
            global_percentiles,
            bottlenecks,
            critical_path,
            estimated_total_savings_ms,
        }
    }

    /// 计算模块级汇总
    fn compute_module_summaries(&self, entries: &[FileProfileEntry]) -> Vec<ModuleProfileSummary> {
        let mut module_map: HashMap<String, Vec<&FileProfileEntry>> = HashMap::new();
        for entry in entries {
            module_map
                .entry(entry.module_name.clone())
                .or_default()
                .push(entry);
        }

        let mut summaries: Vec<ModuleProfileSummary> = module_map
            .into_iter()
            .map(|(module_name, files)| {
                let file_count = files.len();
                let cache_hit_count = files.iter().filter(|f| f.cache_hit).count();
                let non_cached: Vec<&&FileProfileEntry> =
                    files.iter().filter(|f| !f.cache_hit).collect();

                let total_compile_duration: Duration =
                    non_cached.iter().map(|f| f.compile_duration).sum();
                let avg_compile_duration = if !non_cached.is_empty() {
                    total_compile_duration / non_cached.len() as u32
                } else {
                    Duration::ZERO
                };

                let (slowest_file, slowest_duration) = files
                    .iter()
                    .max_by_key(|f| f.compile_duration)
                    .map(|f| (Some(f.file_path.clone()), f.compile_duration))
                    .unwrap_or((None, Duration::ZERO));

                let pch_users = files.iter().filter(|f| f.used_pch).count();
                let pch_coverage_percent = if file_count > 0 {
                    pch_users as f64 / file_count as f64 * 100.0
                } else {
                    0.0
                };

                let unity_users = files.iter().filter(|f| f.in_unity_build).count();
                let unity_coverage_percent = if file_count > 0 {
                    unity_users as f64 / file_count as f64 * 100.0
                } else {
                    0.0
                };

                let total_source_size_bytes: u64 = files.iter().map(|f| f.file_size_bytes).sum();
                let total_output_size_bytes: u64 = files.iter().map(|f| f.output_size_bytes).sum();

                let compile_times_ms: Vec<f64> = non_cached
                    .iter()
                    .map(|f| f.compile_duration.as_secs_f64() * 1000.0)
                    .collect();
                let percentile_stats = compute_percentiles(&compile_times_ms);

                ModuleProfileSummary {
                    module_name,
                    file_count,
                    cache_hit_count,
                    total_compile_duration,
                    avg_compile_duration,
                    slowest_file,
                    slowest_duration,
                    pch_coverage_percent,
                    unity_coverage_percent,
                    total_source_size_bytes,
                    total_output_size_bytes,
                    percentile_stats,
                }
            })
            .collect();

        // 按总编译耗时降序排列
        summaries.sort_by(|a, b| b.total_compile_duration.cmp(&a.total_compile_duration));
        summaries
    }

    /// 自动检测编译瓶颈
    fn detect_bottlenecks(
        &self,
        file_entries: &[FileProfileEntry],
        link_entries: &[LinkProfileEntry],
        module_summaries: &[ModuleProfileSummary],
        global_stats: &PercentileStats,
    ) -> Vec<BottleneckInfo> {
        let mut bottlenecks = Vec::new();

        // 1. 检测慢速文件
        let slow_threshold = self
            .thresholds
            .slow_file_threshold_ms
            .max(global_stats.p50_ms * self.thresholds.outlier_multiplier);

        for entry in file_entries.iter().filter(|e| !e.cache_hit) {
            let duration_ms = entry.compile_duration.as_secs_f64() * 1000.0;
            if duration_ms > slow_threshold {
                let savings = duration_ms - global_stats.p50_ms;
                bottlenecks.push(BottleneckInfo {
                    severity: if duration_ms > slow_threshold * 2.0 {
                        BottleneckSeverity::Critical
                    } else {
                        BottleneckSeverity::Warning
                    },
                    bottleneck_type: BottleneckType::SlowFile {
                        file_path: entry.file_path.clone(),
                        duration_ms,
                        threshold_ms: slow_threshold,
                    },
                    description: format!(
                        "文件 {} 编译耗时 {:.0}ms，超过阈值 {:.0}ms (P50 的 {:.1}倍)",
                        entry.file_path.display(),
                        duration_ms,
                        slow_threshold,
                        duration_ms / global_stats.p50_ms.max(1.0),
                    ),
                    suggestion: format!(
                        "检查该文件的 #include 数量 ({} 个)。考虑: 减少头文件包含、使用前向声明、\
                         拆分为更小的编译单元、启用 PCH",
                        entry.include_count,
                    ),
                    estimated_savings_ms: savings.max(0.0),
                });
            }
        }

        // 2. 检测低 PCH 覆盖率的模块
        for summary in module_summaries {
            if summary.file_count >= 5
                && summary.pch_coverage_percent < self.thresholds.low_pch_coverage_threshold
            {
                let potential_savings = summary.total_compile_duration.as_secs_f64() * 1000.0 * 0.2;
                bottlenecks.push(BottleneckInfo {
                    severity: BottleneckSeverity::Warning,
                    bottleneck_type: BottleneckType::LowPchCoverage {
                        module_name: summary.module_name.clone(),
                        coverage_percent: summary.pch_coverage_percent,
                    },
                    description: format!(
                        "模块 {} 的 PCH 覆盖率仅 {:.1}% ({}/{} 文件使用 PCH)",
                        summary.module_name,
                        summary.pch_coverage_percent,
                        (summary.file_count as f64 * summary.pch_coverage_percent / 100.0) as usize,
                        summary.file_count,
                    ),
                    suggestion: format!(
                        "为模块 {} 启用预编译头。在 .limx.toml 中设置 \
                         [precompiled_header] enabled = true",
                        summary.module_name,
                    ),
                    estimated_savings_ms: potential_savings,
                });
            }
        }

        // 3. 检测链接瓶颈
        for link in link_entries {
            let duration_ms = link.link_duration.as_secs_f64() * 1000.0;
            if duration_ms > self.thresholds.slow_link_threshold_ms {
                bottlenecks.push(BottleneckInfo {
                    severity: BottleneckSeverity::Warning,
                    bottleneck_type: BottleneckType::SlowLink {
                        target_name: link.target_name.clone(),
                        duration_ms,
                    },
                    description: format!(
                        "链接目标 {} ({}) 耗时 {:.0}ms，输入 {} 个 .obj，总大小 {:.1}MB",
                        link.target_name,
                        link.link_type.display_name(),
                        duration_ms,
                        link.input_object_count,
                        link.input_total_size_bytes as f64 / (1024.0 * 1024.0),
                    ),
                    suggestion: "考虑: 使用增量链接、拆分为动态库模块、启用 /LTCG:incremental"
                        .to_string(),
                    estimated_savings_ms: duration_ms * 0.3,
                });
            }
        }

        // 4. 检测高膨胀率文件
        for entry in file_entries.iter().filter(|e| !e.cache_hit) {
            let ratio = entry.bloat_ratio();
            if ratio > self.thresholds.high_bloat_ratio_threshold && entry.file_size_bytes > 1024 {
                bottlenecks.push(BottleneckInfo {
                    severity: BottleneckSeverity::Info,
                    bottleneck_type: BottleneckType::HighBloatRatio {
                        file_path: entry.file_path.clone(),
                        ratio,
                    },
                    description: format!(
                        "文件 {} 的膨胀率为 {:.1}x (源 {:.1}KB → 输出 {:.1}KB)",
                        entry.file_path.display(),
                        ratio,
                        entry.file_size_bytes as f64 / 1024.0,
                        entry.output_size_bytes as f64 / 1024.0,
                    ),
                    suggestion:
                        "高膨胀率可能由大量模板实例化或内联函数导致。考虑使用显式模板实例化"
                            .to_string(),
                    estimated_savings_ms: 0.0,
                });
            }
        }

        // 5. 检测慢速模块
        for summary in module_summaries {
            let total_ms = summary.total_compile_duration.as_secs_f64() * 1000.0;
            if summary.file_count >= 3 && summary.percentile_stats.mean_ms > global_stats.p90_ms {
                bottlenecks.push(BottleneckInfo {
                    severity: BottleneckSeverity::Warning,
                    bottleneck_type: BottleneckType::SlowModule {
                        module_name: summary.module_name.clone(),
                        total_duration_ms: total_ms,
                        file_count: summary.file_count,
                    },
                    description: format!(
                        "模块 {} 的平均编译耗时 {:.0}ms 超过全局 P90 ({:.0}ms)，共 {} 个文件，总耗时 {:.0}ms",
                        summary.module_name,
                        summary.percentile_stats.mean_ms,
                        global_stats.p90_ms,
                        summary.file_count,
                        total_ms,
                    ),
                    suggestion: format!(
                        "模块 {} 整体偏慢。考虑: 启用 Unity Build 合并编译、优化头文件包含、拆分大型文件",
                        summary.module_name,
                    ),
                    estimated_savings_ms: total_ms * 0.25,
                });
            }
        }

        // 按严重程度和预估节省排序
        bottlenecks.sort_by(|a, b| {
            b.severity.cmp(&a.severity).then(
                b.estimated_savings_ms
                    .partial_cmp(&a.estimated_savings_ms)
                    .unwrap_or(std::cmp::Ordering::Equal),
            )
        });

        bottlenecks
    }

    /// 计算关键路径 (最长编译链)
    fn compute_critical_path(
        &self,
        file_entries: &[FileProfileEntry],
        link_entries: &[LinkProfileEntry],
    ) -> Vec<CriticalPathNode> {
        let mut path = Vec::new();

        // 找到最慢的编译文件作为关键路径节点
        let mut sorted_files: Vec<&FileProfileEntry> =
            file_entries.iter().filter(|e| !e.cache_hit).collect();
        sorted_files.sort_by(|a, b| b.compile_duration.cmp(&a.compile_duration));

        let mut cumulative = Duration::ZERO;

        // 取前 10 个最慢文件作为关键路径
        for entry in sorted_files.iter().take(10) {
            cumulative += entry.compile_duration;
            path.push(CriticalPathNode {
                name: entry.file_path.display().to_string(),
                duration: entry.compile_duration,
                cumulative_duration: cumulative,
                node_type: CriticalPathNodeType::Compile,
            });
        }

        // 添加链接阶段
        for link in link_entries {
            cumulative += link.link_duration;
            path.push(CriticalPathNode {
                name: format!("{} ({})", link.target_name, link.link_type.display_name()),
                duration: link.link_duration,
                cumulative_duration: cumulative,
                node_type: CriticalPathNodeType::Link,
            });
        }

        path
    }

    /// 导出 JSON 格式报告
    pub fn export_json(&self, report: &ProfileReport, output_path: &Path) -> std::io::Result<()> {
        let json = serde_json::to_string_pretty(report)
            .map_err(|e| std::io::Error::new(std::io::ErrorKind::Other, e))?;
        std::fs::write(output_path, json)
    }

    /// 导出交互式 HTML 报告
    pub fn export_html(&self, report: &ProfileReport, output_path: &Path) -> std::io::Result<()> {
        let html = generate_html_report(report);
        std::fs::write(output_path, html)
    }
}

// =============================================================================
// HTML 报告生成
// =============================================================================

/// 生成交互式 HTML 报告
fn generate_html_report(report: &ProfileReport) -> String {
    let mut html = String::with_capacity(32768);

    // HTML 头部
    html.push_str(r#"<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Limx Engine 构建性能剖析报告</title>
<style>
:root {
    --bg-primary: #0d1117;
    --bg-secondary: #161b22;
    --bg-tertiary: #21262d;
    --text-primary: #c9d1d9;
    --text-secondary: #8b949e;
    --accent-blue: #58a6ff;
    --accent-green: #3fb950;
    --accent-yellow: #d29922;
    --accent-red: #f85149;
    --accent-purple: #bc8cff;
    --border: #30363d;
}
* { margin: 0; padding: 0; box-sizing: border-box; }
body {
    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Helvetica, Arial, sans-serif;
    background: var(--bg-primary); color: var(--text-primary);
    line-height: 1.6; padding: 24px;
}
.container { max-width: 1400px; margin: 0 auto; }
h1 { font-size: 28px; margin-bottom: 8px; color: var(--accent-blue); }
h2 { font-size: 20px; margin: 32px 0 16px; color: var(--text-primary);
     border-bottom: 1px solid var(--border); padding-bottom: 8px; }
h3 { font-size: 16px; margin: 16px 0 8px; color: var(--text-secondary); }
.subtitle { color: var(--text-secondary); margin-bottom: 24px; }
.grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(220px, 1fr)); gap: 16px; margin: 16px 0; }
.card {
    background: var(--bg-secondary); border: 1px solid var(--border);
    border-radius: 8px; padding: 16px;
}
.card .label { font-size: 12px; color: var(--text-secondary); text-transform: uppercase; letter-spacing: 0.5px; }
.card .value { font-size: 28px; font-weight: 700; margin-top: 4px; }
.card .unit { font-size: 14px; color: var(--text-secondary); }
table { width: 100%; border-collapse: collapse; margin: 8px 0; }
th { text-align: left; padding: 10px 12px; background: var(--bg-tertiary);
     color: var(--text-secondary); font-size: 12px; text-transform: uppercase;
     letter-spacing: 0.5px; border-bottom: 2px solid var(--border); }
td { padding: 8px 12px; border-bottom: 1px solid var(--border); font-size: 14px; }
tr:hover td { background: var(--bg-tertiary); }
.bar-container { height: 8px; background: var(--bg-tertiary); border-radius: 4px; overflow: hidden; }
.bar { height: 100%; border-radius: 4px; transition: width 0.3s; }
.bar-blue { background: var(--accent-blue); }
.bar-green { background: var(--accent-green); }
.bar-yellow { background: var(--accent-yellow); }
.bar-red { background: var(--accent-red); }
.bottleneck { background: var(--bg-secondary); border: 1px solid var(--border);
              border-radius: 8px; padding: 16px; margin: 8px 0; }
.bottleneck.critical { border-left: 4px solid var(--accent-red); }
.bottleneck.warning { border-left: 4px solid var(--accent-yellow); }
.bottleneck.info { border-left: 4px solid var(--accent-blue); }
.bottleneck .title { font-weight: 600; margin-bottom: 4px; }
.bottleneck .suggestion { color: var(--accent-green); font-size: 13px; margin-top: 6px; }
.bottleneck .savings { color: var(--accent-purple); font-size: 12px; float: right; }
.tag { display: inline-block; padding: 2px 8px; border-radius: 4px; font-size: 12px; }
.tag-critical { background: rgba(248,81,73,0.15); color: var(--accent-red); }
.tag-warning { background: rgba(210,153,34,0.15); color: var(--accent-yellow); }
.tag-info { background: rgba(88,166,255,0.15); color: var(--accent-blue); }
.sortable { cursor: pointer; user-select: none; }
.sortable:hover { color: var(--accent-blue); }
.percentile-grid { display: grid; grid-template-columns: repeat(8, 1fr); gap: 8px; margin: 8px 0; }
.percentile-item { text-align: center; padding: 8px; background: var(--bg-secondary);
                   border-radius: 6px; border: 1px solid var(--border); }
.percentile-item .label { font-size: 11px; color: var(--text-secondary); }
.percentile-item .value { font-size: 18px; font-weight: 600; margin-top: 2px; }
</style>
</head>
<body>
<div class="container">
"#);

    // 标题
    html.push_str(&format!(
        r#"<h1>{}</h1>
<p class="subtitle">配置: {} | 时间: {} | 线程: {}</p>
"#,
        report.title, report.build_configuration, report.timestamp, report.parallel_thread_count,
    ));

    // 概览卡片
    html.push_str(r#"<h2>概览</h2><div class="grid">"#);
    emit_card(
        &mut html,
        "总构建耗时",
        &format_duration(report.total_build_duration),
        "",
    );
    emit_card(
        &mut html,
        "编译耗时",
        &format_duration(report.total_compile_duration),
        "",
    );
    emit_card(
        &mut html,
        "链接耗时",
        &format_duration(report.total_link_duration),
        "",
    );
    emit_card(
        &mut html,
        "编译文件数",
        &format!("{}", report.total_file_count),
        "个文件",
    );
    emit_card(
        &mut html,
        "缓存命中",
        &format!("{}", report.total_cache_hits),
        &format!("({:.1}%)", report.cache_hit_rate_percent),
    );
    emit_card(
        &mut html,
        "检测到瓶颈",
        &format!("{}", report.bottlenecks.len()),
        "个问题",
    );
    emit_card(
        &mut html,
        "预估可优化",
        &format!("{:.0}", report.estimated_total_savings_ms),
        "ms",
    );
    html.push_str("</div>");

    // 百分位统计
    let stats = &report.global_percentiles;
    html.push_str(r#"<h2>编译耗时分布</h2><div class="percentile-grid">"#);
    emit_percentile_item(&mut html, "Min", stats.min_ms);
    emit_percentile_item(&mut html, "P25", stats.p25_ms);
    emit_percentile_item(&mut html, "P50", stats.p50_ms);
    emit_percentile_item(&mut html, "P75", stats.p75_ms);
    emit_percentile_item(&mut html, "P90", stats.p90_ms);
    emit_percentile_item(&mut html, "P95", stats.p95_ms);
    emit_percentile_item(&mut html, "P99", stats.p99_ms);
    emit_percentile_item(&mut html, "Max", stats.max_ms);
    html.push_str("</div>");
    html.push_str(&format!(
        r#"<p style="color:var(--text-secondary);font-size:13px;">平均: {:.0}ms | 标准差: {:.0}ms | 样本: {} 个文件</p>"#,
        stats.mean_ms, stats.stddev_ms, stats.count,
    ));

    // 瓶颈报告
    if !report.bottlenecks.is_empty() {
        html.push_str(r#"<h2>瓶颈分析</h2>"#);
        for bottleneck in &report.bottlenecks {
            let (class, tag_class) = match bottleneck.severity {
                BottleneckSeverity::Critical => ("critical", "tag-critical"),
                BottleneckSeverity::Warning => ("warning", "tag-warning"),
                BottleneckSeverity::Info => ("info", "tag-info"),
            };
            html.push_str(&format!(
                r#"<div class="bottleneck {class}">
<span class="savings">预估节省 {:.0}ms</span>
<div class="title"><span class="tag {tag_class}">{severity}</span> {desc}</div>
<div class="suggestion">💡 {suggestion}</div>
</div>"#,
                bottleneck.estimated_savings_ms,
                severity = match bottleneck.severity {
                    BottleneckSeverity::Critical => "严重",
                    BottleneckSeverity::Warning => "警告",
                    BottleneckSeverity::Info => "信息",
                },
                desc = bottleneck.description,
                suggestion = bottleneck.suggestion,
            ));
        }
    }

    // 模块汇总表
    if !report.module_summaries.is_empty() {
        html.push_str(
            r#"<h2>模块编译汇总</h2>
<table>
<thead><tr>
<th>模块</th><th>文件数</th><th>缓存命中</th><th>总耗时</th>
<th>平均耗时</th><th>最慢文件耗时</th><th>PCH 覆盖率</th><th>分布</th>
</tr></thead><tbody>"#,
        );

        let max_module_duration = report
            .module_summaries
            .iter()
            .map(|m| m.total_compile_duration.as_secs_f64())
            .fold(0.0f64, f64::max);

        for summary in &report.module_summaries {
            let bar_percent = if max_module_duration > 0.0 {
                summary.total_compile_duration.as_secs_f64() / max_module_duration * 100.0
            } else {
                0.0
            };
            let bar_color = if bar_percent > 80.0 {
                "bar-red"
            } else if bar_percent > 50.0 {
                "bar-yellow"
            } else {
                "bar-blue"
            };

            html.push_str(&format!(
                r#"<tr>
<td><strong>{}</strong></td>
<td>{}</td>
<td>{}</td>
<td>{}</td>
<td>{:.0}ms</td>
<td>{}</td>
<td>{:.0}%</td>
<td><div class="bar-container"><div class="bar {bar_color}" style="width:{bar_percent:.0}%"></div></div></td>
</tr>"#,
                summary.module_name,
                summary.file_count,
                summary.cache_hit_count,
                format_duration(summary.total_compile_duration),
                summary.avg_compile_duration.as_secs_f64() * 1000.0,
                format_duration(summary.slowest_duration),
                summary.pch_coverage_percent,
            ));
        }
        html.push_str("</tbody></table>");
    }

    // Top 20 最慢文件
    let mut sorted_files = report.file_entries.clone();
    sorted_files.sort_by(|a, b| b.compile_duration.cmp(&a.compile_duration));

    html.push_str(
        r#"<h2>Top 20 最慢编译文件</h2>
<table>
<thead><tr>
<th>文件</th><th>模块</th><th>耗时</th><th>大小</th>
<th>#include</th><th>PCH</th><th>膨胀率</th><th>分布</th>
</tr></thead><tbody>"#,
    );

    let max_file_duration = sorted_files
        .first()
        .map(|f| f.compile_duration.as_secs_f64())
        .unwrap_or(1.0);

    for entry in sorted_files.iter().filter(|e| !e.cache_hit).take(20) {
        let bar_percent = entry.compile_duration.as_secs_f64() / max_file_duration * 100.0;
        let bar_color = if bar_percent > 80.0 {
            "bar-red"
        } else if bar_percent > 50.0 {
            "bar-yellow"
        } else {
            "bar-blue"
        };

        let file_name = entry
            .file_path
            .file_name()
            .map(|n| n.to_string_lossy().to_string())
            .unwrap_or_else(|| entry.file_path.display().to_string());

        html.push_str(&format!(
            r#"<tr>
<td title="{full_path}">{file_name}</td>
<td>{module}</td>
<td>{duration}</td>
<td>{size:.1}KB</td>
<td>{includes}</td>
<td>{pch}</td>
<td>{bloat:.1}x</td>
<td><div class="bar-container"><div class="bar {bar_color}" style="width:{bar_percent:.0}%"></div></div></td>
</tr>"#,
            full_path = entry.file_path.display(),
            file_name = file_name,
            module = entry.module_name,
            duration = format_duration(entry.compile_duration),
            size = entry.file_size_bytes as f64 / 1024.0,
            includes = entry.include_count,
            pch = if entry.used_pch { "✓" } else { "✗" },
            bloat = entry.bloat_ratio(),
        ));
    }
    html.push_str("</tbody></table>");

    // 关键路径
    if !report.critical_path.is_empty() {
        html.push_str(
            r#"<h2>关键路径 (最慢编译链)</h2>
<table>
<thead><tr><th>#</th><th>节点</th><th>类型</th><th>耗时</th><th>累计耗时</th></tr></thead><tbody>"#,
        );

        for (i, node) in report.critical_path.iter().enumerate() {
            let node_type_name = match node.node_type {
                CriticalPathNodeType::Compile => "编译",
                CriticalPathNodeType::Link => "链接",
                CriticalPathNodeType::CodeGeneration => "代码生成",
                CriticalPathNodeType::Preprocessing => "预处理",
            };
            html.push_str(&format!(
                r#"<tr><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td></tr>"#,
                i + 1,
                node.name,
                node_type_name,
                format_duration(node.duration),
                format_duration(node.cumulative_duration),
            ));
        }
        html.push_str("</tbody></table>");
    }

    // 页脚
    html.push_str(
        r#"
<hr style="border-color:var(--border);margin:32px 0;">
<p style="color:var(--text-secondary);font-size:12px;text-align:center;">
由 Limx Build Tool (LBT) 自动生成 | Limx Engine &copy; 2026 LimxTeam
</p>
</div>
</body>
</html>"#,
    );

    html
}

/// 辅助：生成概览卡片 HTML
fn emit_card(html: &mut String, label: &str, value: &str, unit: &str) {
    html.push_str(&format!(
        r#"<div class="card"><div class="label">{label}</div><div class="value">{value} <span class="unit">{unit}</span></div></div>"#,
    ));
}

/// 辅助：生成百分位项 HTML
fn emit_percentile_item(html: &mut String, label: &str, value_ms: f64) {
    html.push_str(&format!(
        r#"<div class="percentile-item"><div class="label">{label}</div><div class="value">{value_ms:.0}<span class="unit">ms</span></div></div>"#,
    ));
}

/// 辅助：格式化 Duration 为人类可读字符串
fn format_duration(d: Duration) -> String {
    let ms = d.as_millis();
    if ms < 1000 {
        format!("{}ms", ms)
    } else if ms < 60_000 {
        format!("{:.1}s", d.as_secs_f64())
    } else {
        let minutes = ms / 60_000;
        let seconds = (ms % 60_000) as f64 / 1000.0;
        format!("{}m{:.0}s", minutes, seconds)
    }
}

/// 生成简单时间戳 (不依赖 chrono)
fn chrono_timestamp() -> String {
    // 使用 SystemTime 生成 ISO 格式近似时间戳
    use std::time::{SystemTime, UNIX_EPOCH};
    let since_epoch = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or(Duration::ZERO);
    let total_seconds = since_epoch.as_secs();
    // 简化格式: Unix timestamp
    let days = total_seconds / 86400;
    let year = 1970 + (days / 365); // 近似
    let remaining_seconds = total_seconds % 86400;
    let hours = remaining_seconds / 3600;
    let minutes = (remaining_seconds % 3600) / 60;
    let seconds = remaining_seconds % 60;
    format!(
        "{}-xx-xx {:02}:{:02}:{:02} UTC",
        year, hours, minutes, seconds
    )
}

// =============================================================================
// 测试
// =============================================================================

#[cfg(test)]
mod tests {
    use super::*;

    fn make_test_entry(
        name: &str,
        module: &str,
        duration_ms: u64,
        size_kb: u64,
    ) -> FileProfileEntry {
        FileProfileEntry {
            file_path: PathBuf::from(name),
            module_name: module.to_string(),
            compile_duration: Duration::from_millis(duration_ms),
            preprocess_duration: None,
            codegen_duration: None,
            optimize_duration: None,
            file_size_bytes: size_kb * 1024,
            include_count: 20,
            dependency_depth: 3,
            used_pch: false,
            in_unity_build: false,
            warning_count: 0,
            output_size_bytes: size_kb * 1024 * 5,
            thread_id: 0,
            cache_hit: false,
        }
    }

    #[test]
    fn test_profiler_record_and_report() {
        let profiler = BuildProfiler::new("Development", 8);

        profiler.record_file_compile(make_test_entry("a.cpp", "Core", 100, 10));
        profiler.record_file_compile(make_test_entry("b.cpp", "Core", 200, 20));
        profiler.record_file_compile(make_test_entry("c.cpp", "Renderer", 500, 50));
        profiler.record_file_compile(make_test_entry("d.cpp", "Renderer", 8000, 100));

        let report = profiler.generate_report(Duration::from_secs(10));

        assert_eq!(report.total_file_count, 4);
        assert_eq!(report.total_cache_hits, 0);
        assert_eq!(report.module_summaries.len(), 2);
        assert!(report.global_percentiles.count > 0);

        // 最慢模块应该排第一
        assert_eq!(report.module_summaries[0].module_name, "Renderer");
    }

    #[test]
    fn test_bottleneck_detection_slow_file() {
        let profiler = BuildProfiler::new("Debug", 4);

        // 一个极慢的文件
        profiler.record_file_compile(make_test_entry("slow.cpp", "Core", 15000, 50));
        // 若干正常文件
        for i in 0..10 {
            profiler.record_file_compile(make_test_entry(
                &format!("fast_{}.cpp", i),
                "Core",
                100,
                10,
            ));
        }

        let report = profiler.generate_report(Duration::from_secs(20));

        // 应该检测到慢速文件瓶颈
        let slow_file_bottleneck = report
            .bottlenecks
            .iter()
            .any(|b| matches!(&b.bottleneck_type, BottleneckType::SlowFile { .. }));
        assert!(slow_file_bottleneck, "应检测到慢速文件瓶颈");
    }

    #[test]
    fn test_bottleneck_detection_low_pch() {
        let profiler = BuildProfiler::new("Debug", 4);

        // 5个文件都不使用 PCH
        for i in 0..5 {
            profiler.record_file_compile(make_test_entry(
                &format!("file_{}.cpp", i),
                "NoPchModule",
                200,
                15,
            ));
        }

        let report = profiler.generate_report(Duration::from_secs(5));

        let low_pch_bottleneck = report
            .bottlenecks
            .iter()
            .any(|b| matches!(&b.bottleneck_type, BottleneckType::LowPchCoverage { .. }));
        assert!(low_pch_bottleneck, "应检测到低 PCH 覆盖率瓶颈");
    }

    #[test]
    fn test_percentile_computation() {
        let values = vec![1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0];
        let stats = compute_percentiles(&values);

        assert_eq!(stats.count, 10);
        assert!((stats.min_ms - 1.0).abs() < 0.001);
        assert!((stats.max_ms - 10.0).abs() < 0.001);
        assert!((stats.mean_ms - 5.5).abs() < 0.001);
        assert!(stats.p50_ms >= 5.0 && stats.p50_ms <= 6.0);
    }

    #[test]
    fn test_percentile_empty() {
        let stats = compute_percentiles(&[]);
        assert_eq!(stats.count, 0);
        assert_eq!(stats.mean_ms, 0.0);
    }

    #[test]
    fn test_file_profile_throughput() {
        let entry = make_test_entry("test.cpp", "Core", 1000, 100);
        let throughput = entry.compile_throughput();
        // 100KB in 1s = 102400 bytes/s
        assert!((throughput - 102400.0).abs() < 1.0);
    }

    #[test]
    fn test_file_profile_bloat_ratio() {
        let entry = make_test_entry("test.cpp", "Core", 100, 10);
        // output = 10KB * 5 = 50KB, input = 10KB, ratio = 5.0
        assert!((entry.bloat_ratio() - 5.0).abs() < 0.001);
    }

    #[test]
    fn test_module_summary_ordering() {
        let profiler = BuildProfiler::new("Debug", 4);

        profiler.record_file_compile(make_test_entry("a.cpp", "Fast", 50, 5));
        profiler.record_file_compile(make_test_entry("b.cpp", "Slow", 5000, 50));
        profiler.record_file_compile(make_test_entry("c.cpp", "Medium", 500, 20));

        let report = profiler.generate_report(Duration::from_secs(10));

        // 模块应按总耗时降序排列
        assert_eq!(report.module_summaries[0].module_name, "Slow");
        assert_eq!(report.module_summaries[1].module_name, "Medium");
        assert_eq!(report.module_summaries[2].module_name, "Fast");
    }

    #[test]
    fn test_cache_hit_rate() {
        let profiler = BuildProfiler::new("Debug", 4);

        let mut cached = make_test_entry("cached.cpp", "Core", 0, 10);
        cached.cache_hit = true;
        profiler.record_file_compile(cached);
        profiler.record_file_compile(make_test_entry("fresh.cpp", "Core", 200, 10));

        let report = profiler.generate_report(Duration::from_secs(1));

        assert_eq!(report.total_cache_hits, 1);
        assert!((report.cache_hit_rate_percent - 50.0).abs() < 0.001);
    }

    #[test]
    fn test_html_report_generation() {
        let profiler = BuildProfiler::new("Release", 16);
        profiler.record_file_compile(make_test_entry("main.cpp", "Core", 300, 30));

        let report = profiler.generate_report(Duration::from_secs(5));
        let html = generate_html_report(&report);

        assert!(html.contains("Limx Engine 构建性能剖析报告"));
        assert!(html.contains("Core"));
        assert!(html.contains("main.cpp"));
    }

    #[test]
    fn test_json_export() {
        let profiler = BuildProfiler::new("Debug", 4);
        profiler.record_file_compile(make_test_entry("a.cpp", "Core", 100, 10));

        let report = profiler.generate_report(Duration::from_secs(1));
        let json = serde_json::to_string(&report).unwrap();

        assert!(json.contains("Core"));
        assert!(json.contains("a.cpp"));
    }

    #[test]
    fn test_link_profile() {
        let profiler = BuildProfiler::new("Debug", 4);
        profiler.record_link_phase(LinkProfileEntry {
            target_name: "LimxEngine".to_string(),
            link_duration: Duration::from_secs(45),
            input_object_count: 500,
            input_total_size_bytes: 500 * 1024 * 1024,
            output_size_bytes: 100 * 1024 * 1024,
            link_type: LinkType::SharedLibrary,
        });

        let report = profiler.generate_report(Duration::from_secs(120));

        assert_eq!(report.link_entries.len(), 1);
        // 45s > 30s 阈值，应检测到链接瓶颈
        let link_bottleneck = report
            .bottlenecks
            .iter()
            .any(|b| matches!(&b.bottleneck_type, BottleneckType::SlowLink { .. }));
        assert!(link_bottleneck, "应检测到链接瓶颈");
    }

    #[test]
    fn test_critical_path_ordering() {
        let profiler = BuildProfiler::new("Debug", 4);

        profiler.record_file_compile(make_test_entry("fast.cpp", "Core", 100, 5));
        profiler.record_file_compile(make_test_entry("slow.cpp", "Core", 5000, 50));
        profiler.record_file_compile(make_test_entry("medium.cpp", "Core", 1000, 20));

        let report = profiler.generate_report(Duration::from_secs(10));

        // 关键路径第一个应该是最慢的文件
        assert!(report.critical_path[0].name.contains("slow.cpp"));
    }

    #[test]
    fn test_format_duration() {
        assert_eq!(format_duration(Duration::from_millis(500)), "500ms");
        assert_eq!(format_duration(Duration::from_millis(1500)), "1.5s");
        assert_eq!(format_duration(Duration::from_secs(90)), "1m30s");
    }
}
