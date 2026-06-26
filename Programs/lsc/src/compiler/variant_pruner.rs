// ============================================================
// 文件名称：variant_pruner.rs
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：着色器变体裁剪优化器 — 分析变体维度组合的实际
//           使用情况，识别死变体 (永远不会被运行时激活的组合)，
//           计算使用频率分布，生成裁剪建议。UE5 的变体系统
//           仅做编译时排除 (CULL_SHADER)，缺乏运行时使用数据
//           驱动的智能裁剪。我们提供静态分析 + 运行时统计
//           双重裁剪策略，大幅减少 PSO 缓存和磁盘占用
// 功能描述：解析变体维度定义 → 枚举所有维度组合 → 静态分析
//           互斥条件/不可达组合 → 集成运行时使用频率 → 生成
//           裁剪建议 + 预估节省空间
// 技术特性：维度笛卡尔积分析、互斥规则引擎、使用频率统计、
//           基于阈值的裁剪策略、Markdown 报告
//
// ── 结构体表 ──────────────────────────────────────────────
// │ 结构体名                    │ 描述                         │
// │────────────────────────────│─────────────────────────────│
// │ VariantPruner              │ 变体裁剪优化器                │
// │ VariantDimension           │ 变体维度定义                  │
// │ VariantCombination         │ 变体组合                     │
// │ MutualExclusionRule        │ 互斥规则                     │
// │ UsageRecord                │ 使用频率记录                  │
// │ PruneResult                │ 裁剪结果                     │
// │ PruneReport                │ 裁剪报告                     │
//
// ── 函数表 ──────────────────────────────────────────────
// │ 函数名                      │ 描述                         │
// │────────────────────────────│─────────────────────────────│
// │ new()                      │ 创建裁剪器                    │
// │ add_dimension()            │ 添加变体维度                  │
// │ add_exclusion_rule()       │ 添加互斥规则                  │
// │ enumerate_combinations()   │ 枚举所有组合                  │
// │ analyze()                  │ 执行裁剪分析                  │
// │ generate_report()          │ 生成裁剪报告                  │
//
// ── 更新历史 ──────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                         │
// │─────────────│──────────│─────────────────────────────│
// │ 2026-04-06  │ LimxTeam  │ 初始创建                      │
// ============================================================

use serde::{Deserialize, Serialize};
use std::collections::HashMap;

// =============================================================================
// 变体维度
// =============================================================================

/// 变体维度定义
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct VariantDimension {
    /// 维度名称 (如 "QUALITY_LEVEL", "HAS_NORMAL_MAP")
    pub name: String,
    /// 可选值列表
    pub values: Vec<String>,
    /// 是否布尔维度 (true/false)
    pub is_boolean: bool,
    /// 描述
    pub description: String,
}

impl VariantDimension {
    /// 创建布尔维度
    pub fn boolean(name: &str, description: &str) -> Self {
        Self {
            name: name.to_string(),
            values: vec!["0".to_string(), "1".to_string()],
            is_boolean: true,
            description: description.to_string(),
        }
    }

    /// 创建多值维度
    pub fn multi(name: &str, values: &[&str], description: &str) -> Self {
        Self {
            name: name.to_string(),
            values: values.iter().map(|v| v.to_string()).collect(),
            is_boolean: false,
            description: description.to_string(),
        }
    }

    /// 值数量
    pub fn value_count(&self) -> usize {
        self.values.len()
    }
}

// =============================================================================
// 变体组合
// =============================================================================

/// 变体组合 — 各维度取值的完整描述
#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct VariantCombination {
    /// 各维度的取值 (维度名 → 值)
    pub values: Vec<(String, String)>,
}

impl VariantCombination {
    /// 获取某维度的值
    pub fn get(&self, dimension: &str) -> Option<&str> {
        self.values
            .iter()
            .find(|(k, _)| k == dimension)
            .map(|(_, v)| v.as_str())
    }

    /// 生成宏定义字符串
    pub fn to_defines_string(&self) -> String {
        self.values
            .iter()
            .map(|(k, v)| format!("{}={}", k, v))
            .collect::<Vec<_>>()
            .join(" ")
    }

    /// 生成唯一键
    pub fn key(&self) -> String {
        self.values
            .iter()
            .map(|(k, v)| format!("{}:{}", k, v))
            .collect::<Vec<_>>()
            .join("|")
    }
}

// =============================================================================
// 互斥规则
// =============================================================================

/// 互斥规则 — 描述不可能同时出现的维度值组合
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct MutualExclusionRule {
    /// 规则名称
    pub name: String,
    /// 条件: 维度名 → 值
    pub conditions: Vec<(String, String)>,
    /// 描述
    pub reason: String,
}

impl MutualExclusionRule {
    /// 检查组合是否匹配此规则 (即应被排除)
    pub fn matches(&self, combination: &VariantCombination) -> bool {
        self.conditions
            .iter()
            .all(|(dim, val)| combination.get(dim) == Some(val.as_str()))
    }
}

// =============================================================================
// 使用频率记录
// =============================================================================

/// 使用频率记录
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct UsageRecord {
    /// 组合键 → 使用次数
    pub usage_counts: HashMap<String, u64>,
    /// 总使用次数
    pub total_uses: u64,
}

impl UsageRecord {
    /// 记录一次使用
    pub fn record_use(&mut self, combination: &VariantCombination) {
        let key = combination.key();
        *self.usage_counts.entry(key).or_insert(0) += 1;
        self.total_uses += 1;
    }

    /// 获取使用次数
    pub fn get_count(&self, combination: &VariantCombination) -> u64 {
        self.usage_counts
            .get(&combination.key())
            .copied()
            .unwrap_or(0)
    }

    /// 获取使用频率 (0.0-1.0)
    pub fn get_frequency(&self, combination: &VariantCombination) -> f64 {
        if self.total_uses == 0 {
            return 0.0;
        }
        self.get_count(combination) as f64 / self.total_uses as f64
    }
}

// =============================================================================
// 裁剪结果
// =============================================================================

/// 裁剪状态
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum PruneStatus {
    /// 保留
    Keep,
    /// 因互斥规则裁剪
    PrunedByRule,
    /// 因使用频率过低裁剪
    PrunedByFrequency,
    /// 从未使用
    NeverUsed,
}

/// 单个组合的裁剪结果
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PruneResult {
    /// 组合
    pub combination: VariantCombination,
    /// 裁剪状态
    pub status: PruneStatus,
    /// 裁剪原因
    pub reason: Option<String>,
    /// 使用次数
    pub usage_count: u64,
    /// 使用频率
    pub usage_frequency: f64,
}

// =============================================================================
// 裁剪报告
// =============================================================================

/// 裁剪报告
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PruneReport {
    /// 着色器名称
    pub shader_name: String,
    /// 总组合数 (裁剪前)
    pub total_combinations: usize,
    /// 保留的组合数
    pub kept_combinations: usize,
    /// 裁剪的组合数
    pub pruned_combinations: usize,
    /// 按规则裁剪数
    pub pruned_by_rule: usize,
    /// 按频率裁剪数
    pub pruned_by_frequency: usize,
    /// 从未使用数
    pub never_used: usize,
    /// 裁剪率 (百分比)
    pub prune_rate_percent: f64,
    /// 预估节省的编译时间 (秒, 假设每变体 0.5s)
    pub estimated_time_saved_seconds: f64,
    /// 预估节省的磁盘空间 (MB, 假设每变体 64KB)
    pub estimated_space_saved_mb: f64,
    /// 每个组合的详细结果
    pub results: Vec<PruneResult>,
    /// 维度信息
    pub dimensions: Vec<VariantDimension>,
}

impl PruneReport {
    /// 导出 Markdown 报告
    pub fn to_markdown(&self) -> String {
        let mut md = String::with_capacity(2048);
        md.push_str(&format!("# 变体裁剪报告: {}\n\n", self.shader_name));
        md.push_str("| 指标 | 值 |\n|------|----|\n");
        md.push_str(&format!("| 总组合数 | {} |\n", self.total_combinations));
        md.push_str(&format!("| 保留 | {} |\n", self.kept_combinations));
        md.push_str(&format!("| 裁剪 | **{}** |\n", self.pruned_combinations));
        md.push_str(&format!(
            "| 裁剪率 | **{:.1}%** |\n",
            self.prune_rate_percent
        ));
        md.push_str(&format!("| 规则裁剪 | {} |\n", self.pruned_by_rule));
        md.push_str(&format!("| 频率裁剪 | {} |\n", self.pruned_by_frequency));
        md.push_str(&format!("| 从未使用 | {} |\n", self.never_used));
        md.push_str(&format!(
            "| 预估节省编译时间 | {:.1}s |\n",
            self.estimated_time_saved_seconds
        ));
        md.push_str(&format!(
            "| 预估节省磁盘空间 | {:.1}MB |\n\n",
            self.estimated_space_saved_mb
        ));

        // 维度信息
        md.push_str("## 维度\n\n");
        md.push_str("| 维度 | 值数 | 类型 | 描述 |\n|------|------|------|------|\n");
        for dim in &self.dimensions {
            md.push_str(&format!(
                "| {} | {} | {} | {} |\n",
                dim.name,
                dim.value_count(),
                if dim.is_boolean { "布尔" } else { "多值" },
                dim.description,
            ));
        }
        md.push('\n');

        // 裁剪的组合 (仅显示前 20 个)
        let pruned: Vec<&PruneResult> = self
            .results
            .iter()
            .filter(|r| r.status != PruneStatus::Keep)
            .take(20)
            .collect();
        if !pruned.is_empty() {
            md.push_str("## 裁剪的组合 (前 20 个)\n\n");
            md.push_str("| 组合 | 原因 | 使用次数 |\n|------|------|----------|\n");
            for r in &pruned {
                md.push_str(&format!(
                    "| {} | {} | {} |\n",
                    r.combination.to_defines_string(),
                    r.reason.as_deref().unwrap_or("-"),
                    r.usage_count,
                ));
            }
        }

        md
    }
}

// =============================================================================
// 变体裁剪优化器
// =============================================================================

/// 变体裁剪优化器
pub struct VariantPruner {
    /// 变体维度
    dimensions: Vec<VariantDimension>,
    /// 互斥规则
    exclusion_rules: Vec<MutualExclusionRule>,
    /// 低频裁剪阈值 (频率低于此值将被标记)
    frequency_threshold: f64,
}

impl VariantPruner {
    /// 创建裁剪器
    pub fn new() -> Self {
        Self {
            dimensions: Vec::new(),
            exclusion_rules: Vec::new(),
            frequency_threshold: 0.001, // 默认: 使用频率 < 0.1% 的变体
        }
    }

    /// 设置频率裁剪阈值
    pub fn set_frequency_threshold(&mut self, threshold: f64) {
        self.frequency_threshold = threshold;
    }

    /// 添加变体维度
    pub fn add_dimension(&mut self, dimension: VariantDimension) {
        self.dimensions.push(dimension);
    }

    /// 添加互斥规则
    pub fn add_exclusion_rule(&mut self, rule: MutualExclusionRule) {
        self.exclusion_rules.push(rule);
    }

    /// 枚举所有可能的组合
    pub fn enumerate_combinations(&self) -> Vec<VariantCombination> {
        if self.dimensions.is_empty() {
            return vec![VariantCombination { values: vec![] }];
        }

        let mut combinations = vec![VariantCombination { values: vec![] }];

        for dim in &self.dimensions {
            let mut new_combinations = Vec::new();
            for combo in &combinations {
                for val in &dim.values {
                    let mut new_combo = combo.clone();
                    new_combo.values.push((dim.name.clone(), val.clone()));
                    new_combinations.push(new_combo);
                }
            }
            combinations = new_combinations;
        }

        combinations
    }

    /// 计算总组合数 (不枚举)
    pub fn total_combination_count(&self) -> usize {
        self.dimensions
            .iter()
            .map(|d| d.value_count())
            .product::<usize>()
            .max(1)
    }

    /// 执行裁剪分析
    pub fn analyze(&self, shader_name: &str, usage: Option<&UsageRecord>) -> PruneReport {
        let combinations = self.enumerate_combinations();
        let total = combinations.len();
        let mut results = Vec::with_capacity(total);

        let mut pruned_by_rule = 0usize;
        let mut pruned_by_freq = 0usize;
        let mut never_used = 0usize;
        let mut kept = 0usize;

        for combo in &combinations {
            // 1. 检查互斥规则
            let matching_rule = self.exclusion_rules.iter().find(|rule| rule.matches(combo));

            if let Some(rule) = matching_rule {
                pruned_by_rule += 1;
                results.push(PruneResult {
                    combination: combo.clone(),
                    status: PruneStatus::PrunedByRule,
                    reason: Some(format!("互斥规则: {} — {}", rule.name, rule.reason)),
                    usage_count: 0,
                    usage_frequency: 0.0,
                });
                continue;
            }

            // 2. 检查使用频率
            if let Some(usage_data) = usage {
                let count = usage_data.get_count(combo);
                let freq = usage_data.get_frequency(combo);

                if count == 0 {
                    never_used += 1;
                    results.push(PruneResult {
                        combination: combo.clone(),
                        status: PruneStatus::NeverUsed,
                        reason: Some("运行时从未使用".to_string()),
                        usage_count: 0,
                        usage_frequency: 0.0,
                    });
                } else if freq < self.frequency_threshold {
                    pruned_by_freq += 1;
                    results.push(PruneResult {
                        combination: combo.clone(),
                        status: PruneStatus::PrunedByFrequency,
                        reason: Some(format!(
                            "使用频率 {:.4}% 低于阈值 {:.2}%",
                            freq * 100.0,
                            self.frequency_threshold * 100.0,
                        )),
                        usage_count: count,
                        usage_frequency: freq,
                    });
                } else {
                    kept += 1;
                    results.push(PruneResult {
                        combination: combo.clone(),
                        status: PruneStatus::Keep,
                        reason: None,
                        usage_count: count,
                        usage_frequency: freq,
                    });
                }
            } else {
                // 无使用数据，仅基于规则裁剪
                kept += 1;
                results.push(PruneResult {
                    combination: combo.clone(),
                    status: PruneStatus::Keep,
                    reason: None,
                    usage_count: 0,
                    usage_frequency: 0.0,
                });
            }
        }

        let pruned_total = pruned_by_rule + pruned_by_freq + never_used;
        let prune_rate = if total > 0 {
            pruned_total as f64 / total as f64 * 100.0
        } else {
            0.0
        };

        PruneReport {
            shader_name: shader_name.to_string(),
            total_combinations: total,
            kept_combinations: kept,
            pruned_combinations: pruned_total,
            pruned_by_rule,
            pruned_by_frequency: pruned_by_freq,
            never_used,
            prune_rate_percent: prune_rate,
            estimated_time_saved_seconds: pruned_total as f64 * 0.5,
            estimated_space_saved_mb: pruned_total as f64 * 64.0 / 1024.0,
            results,
            dimensions: self.dimensions.clone(),
        }
    }
}

// =============================================================================
// 测试
// =============================================================================

#[cfg(test)]
mod tests {
    use super::*;

    fn setup_basic_pruner() -> VariantPruner {
        let mut pruner = VariantPruner::new();
        pruner.add_dimension(VariantDimension::boolean("HAS_NORMAL_MAP", "法线贴图"));
        pruner.add_dimension(VariantDimension::boolean("HAS_EMISSION", "自发光"));
        pruner.add_dimension(VariantDimension::multi(
            "QUALITY",
            &["LOW", "MEDIUM", "HIGH"],
            "质量等级",
        ));
        pruner
    }

    #[test]
    fn test_enumerate_combinations() {
        let pruner = setup_basic_pruner();
        let combos = pruner.enumerate_combinations();
        // 2 * 2 * 3 = 12
        assert_eq!(combos.len(), 12);
        assert_eq!(pruner.total_combination_count(), 12);
    }

    #[test]
    fn test_combination_key() {
        let combo = VariantCombination {
            values: vec![
                ("A".to_string(), "1".to_string()),
                ("B".to_string(), "2".to_string()),
            ],
        };
        assert_eq!(combo.key(), "A:1|B:2");
        assert_eq!(combo.get("A"), Some("1"));
        assert_eq!(combo.get("C"), None);
    }

    #[test]
    fn test_exclusion_rule() {
        let rule = MutualExclusionRule {
            name: "低质量无法线".to_string(),
            conditions: vec![
                ("QUALITY".to_string(), "LOW".to_string()),
                ("HAS_NORMAL_MAP".to_string(), "1".to_string()),
            ],
            reason: "低质量模式不支持法线贴图".to_string(),
        };

        let combo_match = VariantCombination {
            values: vec![
                ("HAS_NORMAL_MAP".to_string(), "1".to_string()),
                ("HAS_EMISSION".to_string(), "0".to_string()),
                ("QUALITY".to_string(), "LOW".to_string()),
            ],
        };
        assert!(rule.matches(&combo_match));

        let combo_no_match = VariantCombination {
            values: vec![
                ("HAS_NORMAL_MAP".to_string(), "1".to_string()),
                ("HAS_EMISSION".to_string(), "0".to_string()),
                ("QUALITY".to_string(), "HIGH".to_string()),
            ],
        };
        assert!(!rule.matches(&combo_no_match));
    }

    #[test]
    fn test_prune_by_rule() {
        let mut pruner = setup_basic_pruner();
        pruner.add_exclusion_rule(MutualExclusionRule {
            name: "低质量无法线".to_string(),
            conditions: vec![
                ("QUALITY".to_string(), "LOW".to_string()),
                ("HAS_NORMAL_MAP".to_string(), "1".to_string()),
            ],
            reason: "低质量不支持法线贴图".to_string(),
        });

        let report = pruner.analyze("PBR.frag", None);
        assert_eq!(report.total_combinations, 12);
        // 规则匹配: QUALITY=LOW & HAS_NORMAL_MAP=1, HAS_EMISSION=0或1 → 2个
        assert_eq!(report.pruned_by_rule, 2);
        assert_eq!(report.kept_combinations, 10);
    }

    #[test]
    fn test_prune_by_frequency() {
        let mut pruner = VariantPruner::new();
        pruner.add_dimension(VariantDimension::boolean("A", "特性A"));
        pruner.add_dimension(VariantDimension::boolean("B", "特性B"));
        pruner.set_frequency_threshold(0.05); // 5%

        let mut usage = UsageRecord::default();
        // 总共 100 次使用
        let combo_common = VariantCombination {
            values: vec![
                ("A".to_string(), "1".to_string()),
                ("B".to_string(), "0".to_string()),
            ],
        };
        for _ in 0..95 {
            usage.record_use(&combo_common);
        }
        let combo_rare = VariantCombination {
            values: vec![
                ("A".to_string(), "0".to_string()),
                ("B".to_string(), "1".to_string()),
            ],
        };
        for _ in 0..3 {
            usage.record_use(&combo_rare);
        }
        let combo_medium = VariantCombination {
            values: vec![
                ("A".to_string(), "1".to_string()),
                ("B".to_string(), "1".to_string()),
            ],
        };
        for _ in 0..2 {
            usage.record_use(&combo_medium);
        }
        // A=0, B=0 从未使用

        let report = pruner.analyze("test.frag", Some(&usage));
        assert_eq!(report.total_combinations, 4);
        assert_eq!(report.never_used, 1, "A=0,B=0 从未使用");
        assert!(report.pruned_by_frequency >= 1, "低频组合应被裁剪");
        assert!(report.kept_combinations >= 1, "高频组合应保留");
    }

    #[test]
    fn test_usage_record() {
        let mut usage = UsageRecord::default();
        let combo = VariantCombination {
            values: vec![("X".to_string(), "1".to_string())],
        };

        usage.record_use(&combo);
        usage.record_use(&combo);
        usage.record_use(&combo);

        assert_eq!(usage.get_count(&combo), 3);
        assert_eq!(usage.total_uses, 3);
        assert!((usage.get_frequency(&combo) - 1.0).abs() < 0.001);
    }

    #[test]
    fn test_empty_dimensions() {
        let pruner = VariantPruner::new();
        let combos = pruner.enumerate_combinations();
        assert_eq!(combos.len(), 1);
        assert_eq!(pruner.total_combination_count(), 1);
    }

    #[test]
    fn test_report_markdown() {
        let mut pruner = setup_basic_pruner();
        pruner.add_exclusion_rule(MutualExclusionRule {
            name: "test".to_string(),
            conditions: vec![
                ("QUALITY".to_string(), "LOW".to_string()),
                ("HAS_NORMAL_MAP".to_string(), "1".to_string()),
            ],
            reason: "不兼容".to_string(),
        });

        let report = pruner.analyze("PBR.frag", None);
        let md = report.to_markdown();

        assert!(md.contains("# 变体裁剪报告: PBR.frag"));
        assert!(md.contains("总组合数"));
        assert!(md.contains("裁剪率"));
        assert!(md.contains("维度"));
    }

    #[test]
    fn test_prune_rate_calculation() {
        let mut pruner = VariantPruner::new();
        pruner.add_dimension(VariantDimension::boolean("X", "x"));

        // 添加规则裁剪掉 X=1
        pruner.add_exclusion_rule(MutualExclusionRule {
            name: "no_x".to_string(),
            conditions: vec![("X".to_string(), "1".to_string())],
            reason: "测试".to_string(),
        });

        let report = pruner.analyze("test", None);
        assert_eq!(report.total_combinations, 2);
        assert_eq!(report.pruned_combinations, 1);
        assert!((report.prune_rate_percent - 50.0).abs() < 0.1);
    }

    #[test]
    fn test_estimated_savings() {
        let mut pruner = VariantPruner::new();
        pruner.add_dimension(VariantDimension::multi(
            "Q",
            &["A", "B", "C", "D"],
            "quality",
        ));
        pruner.add_exclusion_rule(MutualExclusionRule {
            name: "r".to_string(),
            conditions: vec![("Q".to_string(), "D".to_string())],
            reason: "unused".to_string(),
        });

        let report = pruner.analyze("test", None);
        assert_eq!(report.pruned_combinations, 1);
        assert!(report.estimated_time_saved_seconds > 0.0);
        assert!(report.estimated_space_saved_mb > 0.0);
    }

    #[test]
    fn test_combination_defines_string() {
        let combo = VariantCombination {
            values: vec![
                ("HAS_NORMAL".to_string(), "1".to_string()),
                ("QUALITY".to_string(), "HIGH".to_string()),
            ],
        };
        let defines = combo.to_defines_string();
        assert!(defines.contains("HAS_NORMAL=1"));
        assert!(defines.contains("QUALITY=HIGH"));
    }

    #[test]
    fn test_multiple_exclusion_rules() {
        let mut pruner = VariantPruner::new();
        pruner.add_dimension(VariantDimension::multi("A", &["1", "2", "3"], "维度A"));
        pruner.add_dimension(VariantDimension::multi("B", &["X", "Y"], "维度B"));

        // 规则1: A=1 且 B=X 互斥
        pruner.add_exclusion_rule(MutualExclusionRule {
            name: "r1".to_string(),
            conditions: vec![
                ("A".to_string(), "1".to_string()),
                ("B".to_string(), "X".to_string()),
            ],
            reason: "r1".to_string(),
        });
        // 规则2: A=3 且 B=Y 互斥
        pruner.add_exclusion_rule(MutualExclusionRule {
            name: "r2".to_string(),
            conditions: vec![
                ("A".to_string(), "3".to_string()),
                ("B".to_string(), "Y".to_string()),
            ],
            reason: "r2".to_string(),
        });

        let report = pruner.analyze("multi", None);
        assert_eq!(report.total_combinations, 6);
        assert_eq!(report.pruned_by_rule, 2);
        assert_eq!(report.kept_combinations, 4);
    }
}
