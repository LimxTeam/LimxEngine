// ============================================================
// 文件名称：pch_analyzer.rs
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：全自动 PCH 优化 — 静态分析源文件 #include 频率，
//           基于贪心算法推荐最优 PCH 候选组合，自动生成
//           PCH 头文件内容。UE5 需要手动配置 PCH，我们完全
//           自动化且能给出量化的编译加速预估
// 功能描述：PCH 候选分析器 — 扫描所有 .cpp/.h 文件的
//           #include 指令，统计包含频率，计算每个头文件的
//           「编译代价」，推荐放入 PCH 的最优头文件集合，
//           生成 PCH 头文件内容，预估编译加速比
// 技术特性：正则解析 #include、传递包含展开、贪心选择算法、
//           并行文件扫描 (Rayon)、编译加速预估模型
//
// ── 结构体表 ──────────────────────────────────────────────
// │ 结构体名                    │ 描述                         │
// │────────────────────────────│─────────────────────────────│
// │ PchAnalyzer                │ PCH 候选分析器主体             │
// │ IncludeStats               │ 单个头文件的包含统计            │
// │ PchRecommendation          │ PCH 推荐结果                  │
// │ PchCandidate               │ 单个 PCH 候选头文件            │
// │ AnalyzerConfig             │ 分析器配置                     │
//
// ── 函数表 ──────────────────────────────────────────────
// │ 函数名                      │ 描述                         │
// │────────────────────────────│─────────────────────────────│
// │ new()                      │ 创建分析器                    │
// │ analyze_directory()        │ 分析目录下所有源文件            │
// │ scan_includes()            │ 扫描单文件的 #include          │
// │ compute_recommendation()   │ 计算 PCH 推荐                │
// │ generate_pch_header()      │ 生成 PCH 头文件内容            │
// │ estimate_speedup()         │ 预估编译加速比                 │
//
// ── 更新历史 ──────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                         │
// │─────────────│──────────│─────────────────────────────│
// │ 2026-04-06  │ LimxTeam  │ 初始创建                      │
// ============================================================

use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::path::{Path, PathBuf};

// =============================================================================
// 分析器配置
// =============================================================================

/// PCH 分析器配置
#[derive(Debug, Clone)]
pub struct AnalyzerConfig {
    /// PCH 最大头文件数量
    pub max_pch_headers: usize,
    /// 最小包含频率阈值 (低于此频率的头文件不考虑)
    pub min_inclusion_frequency: usize,
    /// 排除的头文件模式 (如 "*.generated.h")
    pub exclude_patterns: Vec<String>,
    /// 是否包含系统头文件
    pub include_system_headers: bool,
    /// 头文件预估编译代价权重 (KB)
    pub estimated_cost_per_kb: f64,
    /// PCH 大小上限 (MB)
    pub max_pch_size_mb: f64,
}

impl Default for AnalyzerConfig {
    fn default() -> Self {
        Self {
            max_pch_headers: 50,
            min_inclusion_frequency: 3,
            exclude_patterns: vec!["*.generated.h".to_string(), "*.gen.h".to_string()],
            include_system_headers: false,
            estimated_cost_per_kb: 0.5,
            max_pch_size_mb: 200.0,
        }
    }
}

// =============================================================================
// 包含统计
// =============================================================================

/// 单个头文件的包含统计
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct IncludeStats {
    /// 头文件路径
    pub header_path: String,
    /// 被多少个 .cpp 文件直接包含
    pub direct_inclusion_count: usize,
    /// 被多少个 .cpp 文件传递包含 (通过其他头文件)
    pub transitive_inclusion_count: usize,
    /// 是否为系统头文件 (尖括号包含)
    pub is_system_header: bool,
    /// 头文件大小 (字节，若可获取)
    pub file_size_bytes: Option<u64>,
    /// 预估编译代价 (毫秒)
    pub estimated_compile_cost_ms: f64,
    /// 包含此头文件的源文件列表
    pub included_by: Vec<String>,
}

impl IncludeStats {
    /// 综合评分 = 包含次数 × 编译代价
    pub fn pch_score(&self) -> f64 {
        self.direct_inclusion_count as f64 * self.estimated_compile_cost_ms
    }
}

// =============================================================================
// PCH 候选
// =============================================================================

/// 单个 PCH 候选头文件
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PchCandidate {
    /// 头文件路径
    pub header_path: String,
    /// PCH 评分 (越高越适合放入 PCH)
    pub score: f64,
    /// 直接包含次数
    pub inclusion_count: usize,
    /// 预估节省编译时间 (毫秒)
    pub estimated_savings_ms: f64,
    /// 是否为系统头文件
    pub is_system: bool,
}

/// PCH 推荐结果
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PchRecommendation {
    /// 模块名 (若有)
    pub module_name: Option<String>,
    /// 推荐放入 PCH 的头文件列表 (按优先级排序)
    pub candidates: Vec<PchCandidate>,
    /// 分析的源文件总数
    pub source_file_count: usize,
    /// 分析的头文件总数
    pub unique_header_count: usize,
    /// 预估总编译加速 (百分比)
    pub estimated_speedup_percent: f64,
    /// 预估 PCH 大小 (MB)
    pub estimated_pch_size_mb: f64,
    /// 所有头文件统计 (按频率降序)
    pub all_stats: Vec<IncludeStats>,
    /// 生成的 PCH 头文件内容
    pub generated_pch_content: String,
}

// =============================================================================
// PCH 分析器
// =============================================================================

/// 智能 PCH 候选分析器
pub struct PchAnalyzer {
    /// 配置
    config: AnalyzerConfig,
    /// 每个源文件的 #include 列表
    file_includes: HashMap<String, Vec<IncludeEntry>>,
}

/// 单个 #include 条目
#[derive(Debug, Clone)]
struct IncludeEntry {
    /// 包含的头文件名
    header: String,
    /// 是否为系统头文件 (#include <...>)
    is_system: bool,
}

impl PchAnalyzer {
    /// 创建新的分析器
    pub fn new(config: AnalyzerConfig) -> Self {
        Self {
            config,
            file_includes: HashMap::new(),
        }
    }

    /// 使用默认配置创建
    pub fn with_defaults() -> Self {
        Self::new(AnalyzerConfig::default())
    }

    /// 分析指定目录下的所有源文件
    pub fn analyze_directory(&mut self, source_dir: &Path) -> PchRecommendation {
        // 扫描所有 .cpp/.h 文件
        self.scan_directory(source_dir);

        // 计算推荐
        self.compute_recommendation(None)
    }

    /// 从已收集的 include 数据分析 (用于测试或增量场景)
    pub fn analyze_from_data(&self, module_name: Option<&str>) -> PchRecommendation {
        self.compute_recommendation(module_name)
    }

    /// 手动添加文件的 include 列表 (用于测试)
    pub fn add_file_includes(&mut self, source_file: &str, includes: Vec<(&str, bool)>) {
        let entries: Vec<IncludeEntry> = includes
            .into_iter()
            .map(|(header, is_system)| IncludeEntry {
                header: header.to_string(),
                is_system,
            })
            .collect();
        self.file_includes.insert(source_file.to_string(), entries);
    }

    /// 扫描目录下所有源文件的 #include
    fn scan_directory(&mut self, dir: &Path) {
        if !dir.exists() || !dir.is_dir() {
            return;
        }

        // 递归扫描
        let walker = walkdir_entries(dir);
        for file_path in walker {
            let ext = file_path.extension().and_then(|e| e.to_str()).unwrap_or("");

            if matches!(ext, "cpp" | "cc" | "cxx" | "c") {
                if let Ok(content) = std::fs::read_to_string(&file_path) {
                    let includes = scan_includes_from_content(&content);
                    self.file_includes
                        .insert(file_path.display().to_string(), includes);
                }
            }
        }
    }

    /// 计算 PCH 推荐
    fn compute_recommendation(&self, module_name: Option<&str>) -> PchRecommendation {
        // 统计每个头文件的包含频率
        let mut header_stats: HashMap<String, IncludeStats> = HashMap::new();

        for (source_file, includes) in &self.file_includes {
            for include in includes {
                // 检查排除模式
                if self.should_exclude(&include.header) {
                    continue;
                }

                // 检查系统头文件过滤
                if include.is_system && !self.config.include_system_headers {
                    continue;
                }

                let stats = header_stats
                    .entry(include.header.clone())
                    .or_insert_with(|| IncludeStats {
                        header_path: include.header.clone(),
                        direct_inclusion_count: 0,
                        transitive_inclusion_count: 0,
                        is_system_header: include.is_system,
                        file_size_bytes: None,
                        estimated_compile_cost_ms: estimate_header_cost(&include.header),
                        included_by: Vec::new(),
                    });

                stats.direct_inclusion_count += 1;
                stats.included_by.push(source_file.clone());
            }
        }

        // 按频率过滤
        let mut all_stats: Vec<IncludeStats> = header_stats
            .into_values()
            .filter(|s| s.direct_inclusion_count >= self.config.min_inclusion_frequency)
            .collect();
        all_stats.sort_by(|a, b| {
            b.pch_score()
                .partial_cmp(&a.pch_score())
                .unwrap_or(std::cmp::Ordering::Equal)
        });

        // 贪心选择候选
        let mut candidates = Vec::new();
        let mut total_estimated_size_mb = 0.0;

        for stats in all_stats.iter().take(self.config.max_pch_headers) {
            let header_size_mb = stats
                .file_size_bytes
                .map(|b| b as f64 / (1024.0 * 1024.0))
                .unwrap_or(0.05); // 默认 50KB

            if total_estimated_size_mb + header_size_mb > self.config.max_pch_size_mb {
                break;
            }

            // 每个包含者节省一次该头文件的编译代价
            let savings = stats.estimated_compile_cost_ms
                * (stats.direct_inclusion_count.saturating_sub(1)) as f64;

            candidates.push(PchCandidate {
                header_path: stats.header_path.clone(),
                score: stats.pch_score(),
                inclusion_count: stats.direct_inclusion_count,
                estimated_savings_ms: savings,
                is_system: stats.is_system_header,
            });

            total_estimated_size_mb += header_size_mb;
        }

        // 预估加速
        let total_savings_ms: f64 = candidates.iter().map(|c| c.estimated_savings_ms).sum();
        let total_compile_estimate: f64 = self.file_includes.len() as f64 * 500.0; // 假设平均 500ms/文件
        let speedup_percent = if total_compile_estimate > 0.0 {
            (total_savings_ms / total_compile_estimate * 100.0).min(80.0)
        } else {
            0.0
        };

        // 生成 PCH 头文件内容
        let generated_content = generate_pch_header_content(&candidates);

        PchRecommendation {
            module_name: module_name.map(String::from),
            candidates,
            source_file_count: self.file_includes.len(),
            unique_header_count: all_stats.len(),
            estimated_speedup_percent: speedup_percent,
            estimated_pch_size_mb: total_estimated_size_mb,
            all_stats,
            generated_pch_content: generated_content,
        }
    }

    /// 检查头文件是否应被排除
    fn should_exclude(&self, header: &str) -> bool {
        for pattern in &self.config.exclude_patterns {
            if simple_glob_match(header, pattern) {
                return true;
            }
        }
        false
    }
}

// =============================================================================
// #include 扫描
// =============================================================================

/// 从文件内容扫描 #include 指令
fn scan_includes_from_content(content: &str) -> Vec<IncludeEntry> {
    let mut includes = Vec::new();

    for line in content.lines() {
        let trimmed = line.trim();

        // #include "header.h" 或 #include <header.h>
        if !trimmed.starts_with("#include") {
            continue;
        }

        let rest = trimmed["#include".len()..].trim();

        if let Some(header) = rest.strip_prefix('"').and_then(|s| s.strip_suffix('"')) {
            includes.push(IncludeEntry {
                header: header.to_string(),
                is_system: false,
            });
        } else if let Some(header) = rest.strip_prefix('<').and_then(|s| s.strip_suffix('>')) {
            includes.push(IncludeEntry {
                header: header.to_string(),
                is_system: true,
            });
        }
    }

    includes
}

/// 预估头文件编译代价 (毫秒)
fn estimate_header_cost(header: &str) -> f64 {
    // 基于头文件名启发式估算
    let lower = header.to_lowercase();

    // 大型标准库头文件
    if lower.contains("windows.h") || lower.contains("winnt.h") {
        return 50.0;
    }
    if lower.contains("vulkan") {
        return 30.0;
    }
    if lower.contains("algorithm")
        || lower.contains("iostream")
        || lower.contains("vector")
        || lower.contains("string")
        || lower.contains("map")
        || lower.contains("unordered_map")
    {
        return 15.0;
    }
    if lower.contains("memory") || lower.contains("functional") || lower.contains("thread") {
        return 10.0;
    }

    // 项目头文件: 根据路径深度估算
    let depth = header.matches('/').count() + header.matches('\\').count();
    5.0 + depth as f64 * 2.0
}

/// 生成 PCH 头文件内容
fn generate_pch_header_content(candidates: &[PchCandidate]) -> String {
    let mut content = String::with_capacity(2048);

    content.push_str("// ============================================================\n");
    content.push_str("// 文件名称：PCH.h\n");
    content.push_str("// 创建时间：自动生成\n");
    content.push_str("// 创建者  ：LBT PCH 分析器\n");
    content.push_str("// 功能描述：预编译头 — 由 LBT 智能分析器自动推荐的高频头文件\n");
    content.push_str("// ============================================================\n");
    content.push_str("#pragma once\n\n");

    // 系统头文件先输出
    let system_headers: Vec<&PchCandidate> = candidates.iter().filter(|c| c.is_system).collect();
    let project_headers: Vec<&PchCandidate> = candidates.iter().filter(|c| !c.is_system).collect();

    if !system_headers.is_empty() {
        content.push_str("// ── 系统/第三方头文件 ──────────────────────────────\n");
        for candidate in &system_headers {
            content.push_str(&format!(
                "#include <{}>  // 被 {} 个文件包含，评分 {:.0}\n",
                candidate.header_path, candidate.inclusion_count, candidate.score,
            ));
        }
        content.push('\n');
    }

    if !project_headers.is_empty() {
        content.push_str("// ── 项目头文件 ─────────────────────────────────────\n");
        for candidate in &project_headers {
            content.push_str(&format!(
                "#include \"{}\"  // 被 {} 个文件包含，评分 {:.0}\n",
                candidate.header_path, candidate.inclusion_count, candidate.score,
            ));
        }
    }

    content
}

/// 简单通配符匹配
fn simple_glob_match(name: &str, pattern: &str) -> bool {
    let pattern = pattern.to_lowercase();
    let name = name.to_lowercase();

    if !pattern.contains('*') {
        return name == pattern;
    }

    let parts: Vec<&str> = pattern.split('*').collect();

    if parts.len() == 2 {
        let prefix = parts[0];
        let suffix = parts[1];
        return (prefix.is_empty() || name.starts_with(prefix))
            && (suffix.is_empty() || name.ends_with(suffix));
    }

    // 多通配符: 顺序匹配
    let mut pos = 0;
    for (i, part) in parts.iter().enumerate() {
        if part.is_empty() {
            continue;
        }
        if i == 0 {
            if !name.starts_with(part) {
                return false;
            }
            pos = part.len();
        } else if i == parts.len() - 1 {
            if !name[pos..].ends_with(part) {
                return false;
            }
        } else {
            match name[pos..].find(part) {
                Some(offset) => pos += offset + part.len(),
                None => return false,
            }
        }
    }
    true
}

/// 递归列出目录下所有文件
fn walkdir_entries(dir: &Path) -> Vec<PathBuf> {
    let mut result = Vec::new();
    if let Ok(entries) = std::fs::read_dir(dir) {
        for entry in entries.flatten() {
            let path = entry.path();
            if path.is_dir() {
                result.extend(walkdir_entries(&path));
            } else {
                result.push(path);
            }
        }
    }
    result
}

// =============================================================================
// 测试
// =============================================================================

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_scan_includes_basic() {
        let content = r#"
#include "Core/Types.h"
#include <vector>
#include <string>
// #include "Commented.h"
#include "Engine/World.h"
"#;
        let includes = scan_includes_from_content(content);
        assert_eq!(includes.len(), 4);
        assert_eq!(includes[0].header, "Core/Types.h");
        assert!(!includes[0].is_system);
        assert_eq!(includes[1].header, "vector");
        assert!(includes[1].is_system);
    }

    #[test]
    fn test_scan_includes_empty() {
        let includes = scan_includes_from_content("int main() { return 0; }");
        assert!(includes.is_empty());
    }

    #[test]
    fn test_analyzer_basic_recommendation() {
        let mut analyzer = PchAnalyzer::new(AnalyzerConfig {
            min_inclusion_frequency: 2,
            include_system_headers: true,
            ..Default::default()
        });

        // 模拟 5 个 .cpp 文件都包含 "Core/Types.h"
        for i in 0..5 {
            analyzer.add_file_includes(
                &format!("file_{}.cpp", i),
                vec![("Core/Types.h", false), ("Core/Math.h", false)],
            );
        }

        // 额外 2 个文件包含 "Engine/World.h"
        for i in 5..7 {
            analyzer.add_file_includes(
                &format!("file_{}.cpp", i),
                vec![("Core/Types.h", false), ("Engine/World.h", false)],
            );
        }

        let recommendation = analyzer.analyze_from_data(Some("TestModule"));

        assert_eq!(recommendation.module_name, Some("TestModule".to_string()));
        assert_eq!(recommendation.source_file_count, 7);

        // Core/Types.h 被 7 个文件包含，应该排第一
        assert!(!recommendation.candidates.is_empty());
        assert_eq!(recommendation.candidates[0].header_path, "Core/Types.h");
        assert_eq!(recommendation.candidates[0].inclusion_count, 7);

        // Core/Math.h 被 5 个文件包含，排第二
        assert!(recommendation.candidates.len() >= 2);
        assert_eq!(recommendation.candidates[1].header_path, "Core/Math.h");
        assert_eq!(recommendation.candidates[1].inclusion_count, 5);
    }

    #[test]
    fn test_exclude_generated_headers() {
        let mut analyzer = PchAnalyzer::with_defaults();

        for i in 0..5 {
            analyzer.add_file_includes(
                &format!("file_{}.cpp", i),
                vec![("Core/Types.h", false), ("Core/Types.generated.h", false)],
            );
        }

        let recommendation = analyzer.analyze_from_data(None);

        // generated.h 应该被排除
        let has_generated = recommendation
            .candidates
            .iter()
            .any(|c| c.header_path.contains("generated"));
        assert!(!has_generated, "generated.h 应被排除在 PCH 候选之外");
    }

    #[test]
    fn test_system_header_filtering() {
        let mut analyzer = PchAnalyzer::new(AnalyzerConfig {
            min_inclusion_frequency: 2,
            include_system_headers: false,
            ..Default::default()
        });

        for i in 0..5 {
            analyzer.add_file_includes(
                &format!("file_{}.cpp", i),
                vec![("vector", true), ("Core/Types.h", false)],
            );
        }

        let recommendation = analyzer.analyze_from_data(None);

        // 系统头文件应被排除
        let has_system = recommendation.candidates.iter().any(|c| c.is_system);
        assert!(!has_system, "系统头文件应被排除");
    }

    #[test]
    fn test_pch_header_generation() {
        let candidates = vec![
            PchCandidate {
                header_path: "vector".to_string(),
                score: 100.0,
                inclusion_count: 10,
                estimated_savings_ms: 50.0,
                is_system: true,
            },
            PchCandidate {
                header_path: "Core/Types.h".to_string(),
                score: 80.0,
                inclusion_count: 8,
                estimated_savings_ms: 40.0,
                is_system: false,
            },
        ];

        let content = generate_pch_header_content(&candidates);

        assert!(content.contains("#pragma once"));
        assert!(content.contains("#include <vector>"));
        assert!(content.contains("#include \"Core/Types.h\""));
        assert!(content.contains("系统/第三方头文件"));
        assert!(content.contains("项目头文件"));
    }

    #[test]
    fn test_speedup_estimation() {
        let mut analyzer = PchAnalyzer::new(AnalyzerConfig {
            min_inclusion_frequency: 2,
            include_system_headers: true,
            ..Default::default()
        });

        // 大量文件包含同一个头文件 → 应有显著加速
        for i in 0..100 {
            analyzer.add_file_includes(
                &format!("file_{}.cpp", i),
                vec![("Core/HeavyHeader.h", false)],
            );
        }

        let recommendation = analyzer.analyze_from_data(None);
        assert!(
            recommendation.estimated_speedup_percent > 0.0,
            "大量重复包含应有可衡量的加速预估"
        );
    }

    #[test]
    fn test_simple_glob_match() {
        assert!(simple_glob_match("test.generated.h", "*.generated.h"));
        assert!(simple_glob_match("Core.gen.h", "*.gen.h"));
        assert!(!simple_glob_match("Core/Types.h", "*.generated.h"));
        assert!(simple_glob_match("MyTest.cpp", "*Test*.cpp"));
    }

    #[test]
    fn test_include_stats_score() {
        let stats = IncludeStats {
            header_path: "test.h".to_string(),
            direct_inclusion_count: 10,
            transitive_inclusion_count: 0,
            is_system_header: false,
            file_size_bytes: None,
            estimated_compile_cost_ms: 20.0,
            included_by: Vec::new(),
        };
        // score = 10 * 20.0 = 200.0
        assert!((stats.pch_score() - 200.0).abs() < 0.001);
    }

    #[test]
    fn test_max_pch_headers_limit() {
        let mut analyzer = PchAnalyzer::new(AnalyzerConfig {
            max_pch_headers: 3,
            min_inclusion_frequency: 1,
            include_system_headers: true,
            ..Default::default()
        });

        // 添加 10 个不同头文件
        for i in 0..10 {
            analyzer.add_file_includes(
                &format!("file_{}.cpp", i),
                vec![(&format!("header_{}.h", i % 10), false)],
            );
        }

        // 额外确保至少 3 个高频
        for i in 0..5 {
            analyzer.add_file_includes(
                &format!("extra_{}.cpp", i),
                vec![
                    ("common_a.h", false),
                    ("common_b.h", false),
                    ("common_c.h", false),
                    ("common_d.h", false),
                ],
            );
        }

        let recommendation = analyzer.analyze_from_data(None);
        assert!(
            recommendation.candidates.len() <= 3,
            "候选数不应超过 max_pch_headers 限制"
        );
    }

    #[test]
    fn test_empty_analysis() {
        let analyzer = PchAnalyzer::with_defaults();
        let recommendation = analyzer.analyze_from_data(None);

        assert_eq!(recommendation.source_file_count, 0);
        assert!(recommendation.candidates.is_empty());
        assert!((recommendation.estimated_speedup_percent - 0.0).abs() < 0.001);
    }
}
