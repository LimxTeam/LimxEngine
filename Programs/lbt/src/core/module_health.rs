// ============================================================
// 文件名称：module_health.rs
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：模块健康度仪表盘 — 量化模块代码质量指标，提供
//           可操作的改进建议。UE5 没有内置的模块健康度分析，
//           开发者只能凭经验判断模块质量。我们提供数据驱动
//           的模块健康评估，帮助团队在问题恶化前发现隐患
// 功能描述：模块健康度分析 — 扫描模块源码目录，统计 API
//           表面积 (公开类/函数/枚举数)、代码复杂度指标、
//           依赖扇入/扇出比、代码膨胀检测 (大文件/大函数)、
//           头文件/实现文件比例、注释覆盖率，并汇总为
//           综合健康评分 (0-100)
// 技术特性：正则扫描 C++ 声明、圈复杂度估算、LOC 统计、
//           扇入扇出计算、可配置阈值、Markdown 报告导出
//
// ── 结构体表 ──────────────────────────────────────────────
// │ 结构体名                    │ 描述                         │
// │────────────────────────────│─────────────────────────────│
// │ ModuleHealthAnalyzer       │ 模块健康度分析器              │
// │ ModuleHealthReport         │ 健康度报告                    │
// │ ApiSurface                 │ API 表面积统计                │
// │ CodeMetrics                │ 代码度量指标                  │
// │ DependencyMetrics          │ 依赖度量                     │
// │ BloatWarning               │ 代码膨胀警告                  │
// │ HealthThresholds           │ 健康阈值配置                  │
//
// ── 函数表 ──────────────────────────────────────────────
// │ 函数名                      │ 描述                         │
// │────────────────────────────│─────────────────────────────│
// │ new()                      │ 创建分析器                    │
// │ analyze_module()           │ 分析单个模块                  │
// │ compute_health_score()     │ 计算健康评分                  │
// │ scan_api_surface()         │ 扫描 API 表面积              │
// │ compute_code_metrics()     │ 计算代码度量                  │
// │ detect_bloat()             │ 检测代码膨胀                  │
// │ to_markdown()              │ 导出 Markdown 报告           │
//
// ── 更新历史 ──────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                         │
// │─────────────│──────────│─────────────────────────────│
// │ 2026-04-06  │ LimxTeam  │ 初始创建                      │
// ============================================================

use serde::{Deserialize, Serialize};
use std::collections::HashMap;

// =============================================================================
// 健康阈值配置
// =============================================================================

/// 健康阈值配置
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct HealthThresholds {
    /// 大文件阈值 (行数)
    pub large_file_lines: usize,
    /// 大函数阈值 (行数)
    pub large_function_lines: usize,
    /// 高圈复杂度阈值
    pub high_cyclomatic_complexity: usize,
    /// API 表面积过大阈值 (公开符号数)
    pub excessive_api_surface: usize,
    /// 依赖扇出过大阈值
    pub excessive_fan_out: usize,
    /// 最大头文件/实现文件比例
    pub max_header_impl_ratio: f64,
    /// 最低注释覆盖率 (百分比)
    pub min_comment_coverage_percent: f64,
}

impl Default for HealthThresholds {
    fn default() -> Self {
        Self {
            large_file_lines: 1000,
            large_function_lines: 100,
            high_cyclomatic_complexity: 15,
            excessive_api_surface: 50,
            excessive_fan_out: 10,
            max_header_impl_ratio: 3.0,
            min_comment_coverage_percent: 10.0,
        }
    }
}

// =============================================================================
// API 表面积
// =============================================================================

/// API 表面积统计
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct ApiSurface {
    /// 公开类数量
    pub public_class_count: usize,
    /// 公开结构体数量
    pub public_struct_count: usize,
    /// 公开枚举数量
    pub public_enum_count: usize,
    /// 公开函数数量
    pub public_function_count: usize,
    /// 公开宏数量
    pub public_macro_count: usize,
    /// 导出符号总数
    pub total_exported_symbols: usize,
    /// 公开类名列表
    pub public_class_names: Vec<String>,
}

impl ApiSurface {
    /// 总 API 表面积
    pub fn total_surface(&self) -> usize {
        self.public_class_count
            + self.public_struct_count
            + self.public_enum_count
            + self.public_function_count
            + self.public_macro_count
    }
}

// =============================================================================
// 代码度量
// =============================================================================

/// 代码度量指标
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct CodeMetrics {
    /// 总行数 (含空行和注释)
    pub total_lines: usize,
    /// 代码行数 (不含空行和注释)
    pub code_lines: usize,
    /// 注释行数
    pub comment_lines: usize,
    /// 空行数
    pub blank_lines: usize,
    /// 头文件数
    pub header_file_count: usize,
    /// 实现文件数
    pub impl_file_count: usize,
    /// 平均文件行数
    pub avg_file_lines: f64,
    /// 最大文件行数
    pub max_file_lines: usize,
    /// 最大文件路径
    pub largest_file: String,
    /// 估算的圈复杂度 (模块级平均)
    pub avg_cyclomatic_complexity: f64,
    /// 最高圈复杂度
    pub max_cyclomatic_complexity: usize,
    /// #include 总数
    pub total_includes: usize,
    /// 注释覆盖率 (百分比)
    pub comment_coverage_percent: f64,
    /// 头文件/实现文件比例
    pub header_impl_ratio: f64,
}

// =============================================================================
// 依赖度量
// =============================================================================

/// 依赖度量
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct DependencyMetrics {
    /// 扇入 (被多少模块依赖)
    pub fan_in: usize,
    /// 扇出 (依赖多少模块)
    pub fan_out: usize,
    /// 不稳定度 I = fan_out / (fan_in + fan_out)
    pub instability: f64,
    /// 依赖的模块名列表
    pub depends_on: Vec<String>,
    /// 被依赖的模块名列表
    pub depended_by: Vec<String>,
}

impl DependencyMetrics {
    /// 计算不稳定度
    pub fn compute_instability(&mut self) {
        let total = self.fan_in + self.fan_out;
        self.instability = if total > 0 {
            self.fan_out as f64 / total as f64
        } else {
            0.0
        };
    }
}

// =============================================================================
// 代码膨胀警告
// =============================================================================

/// 膨胀警告严重程度
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Serialize, Deserialize)]
pub enum BloatSeverity {
    Info,
    Warning,
    Critical,
}

/// 代码膨胀警告
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct BloatWarning {
    pub severity: BloatSeverity,
    pub category: String,
    pub message: String,
    pub file: Option<String>,
    pub suggestion: String,
}

// =============================================================================
// 模块健康度报告
// =============================================================================

/// 健康评级
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum HealthGrade {
    /// 90-100: 优秀
    Excellent,
    /// 75-89: 良好
    Good,
    /// 60-74: 一般
    Fair,
    /// 40-59: 较差
    Poor,
    /// 0-39: 严重
    Critical,
}

impl HealthGrade {
    pub fn from_score(score: f64) -> Self {
        match score as u32 {
            90..=100 => Self::Excellent,
            75..=89 => Self::Good,
            60..=74 => Self::Fair,
            40..=59 => Self::Poor,
            _ => Self::Critical,
        }
    }

    pub fn display_name(&self) -> &'static str {
        match self {
            Self::Excellent => "优秀",
            Self::Good => "良好",
            Self::Fair => "一般",
            Self::Poor => "较差",
            Self::Critical => "严重",
        }
    }

    pub fn emoji(&self) -> &'static str {
        match self {
            Self::Excellent => "A+",
            Self::Good => "A",
            Self::Fair => "B",
            Self::Poor => "C",
            Self::Critical => "D",
        }
    }
}

/// 模块健康度报告
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ModuleHealthReport {
    /// 模块名
    pub module_name: String,
    /// 健康评分 (0-100)
    pub health_score: f64,
    /// 健康评级
    pub grade: HealthGrade,
    /// API 表面积
    pub api_surface: ApiSurface,
    /// 代码度量
    pub code_metrics: CodeMetrics,
    /// 依赖度量
    pub dependency_metrics: DependencyMetrics,
    /// 膨胀警告
    pub bloat_warnings: Vec<BloatWarning>,
    /// 改进建议
    pub recommendations: Vec<String>,
}

impl ModuleHealthReport {
    /// 导出 Markdown 报告
    pub fn to_markdown(&self) -> String {
        let mut md = String::with_capacity(4096);

        md.push_str(&format!("# 模块健康报告: {}\n\n", self.module_name));
        md.push_str(&format!(
            "**评分**: {:.0}/100 ({}) {}\n\n",
            self.health_score,
            self.grade.display_name(),
            self.grade.emoji(),
        ));

        // API 表面积
        md.push_str("## API 表面积\n\n");
        md.push_str(&format!("| 指标 | 数量 |\n|------|------|\n"));
        md.push_str(&format!(
            "| 公开类 | {} |\n",
            self.api_surface.public_class_count
        ));
        md.push_str(&format!(
            "| 公开结构体 | {} |\n",
            self.api_surface.public_struct_count
        ));
        md.push_str(&format!(
            "| 公开枚举 | {} |\n",
            self.api_surface.public_enum_count
        ));
        md.push_str(&format!(
            "| 公开函数 | {} |\n",
            self.api_surface.public_function_count
        ));
        md.push_str(&format!(
            "| **总计** | **{}** |\n\n",
            self.api_surface.total_surface()
        ));

        // 代码度量
        md.push_str("## 代码度量\n\n");
        md.push_str(&format!("| 指标 | 值 |\n|------|----|\n"));
        md.push_str(&format!("| 总行数 | {} |\n", self.code_metrics.total_lines));
        md.push_str(&format!(
            "| 代码行数 | {} |\n",
            self.code_metrics.code_lines
        ));
        md.push_str(&format!(
            "| 注释行数 | {} |\n",
            self.code_metrics.comment_lines
        ));
        md.push_str(&format!(
            "| 头文件数 | {} |\n",
            self.code_metrics.header_file_count
        ));
        md.push_str(&format!(
            "| 实现文件数 | {} |\n",
            self.code_metrics.impl_file_count
        ));
        md.push_str(&format!(
            "| 注释覆盖率 | {:.1}% |\n",
            self.code_metrics.comment_coverage_percent
        ));
        md.push_str(&format!(
            "| 最大文件 | {} ({} 行) |\n\n",
            self.code_metrics.largest_file, self.code_metrics.max_file_lines
        ));

        // 依赖
        md.push_str("## 依赖度量\n\n");
        md.push_str(&format!("| 指标 | 值 |\n|------|----|\n"));
        md.push_str(&format!("| 扇入 | {} |\n", self.dependency_metrics.fan_in));
        md.push_str(&format!("| 扇出 | {} |\n", self.dependency_metrics.fan_out));
        md.push_str(&format!(
            "| 不稳定度 | {:.2} |\n\n",
            self.dependency_metrics.instability
        ));

        // 警告
        if !self.bloat_warnings.is_empty() {
            md.push_str("## 膨胀警告\n\n");
            for w in &self.bloat_warnings {
                let icon = match w.severity {
                    BloatSeverity::Critical => "[!]",
                    BloatSeverity::Warning => "[~]",
                    BloatSeverity::Info => "[i]",
                };
                md.push_str(&format!("- {} **{}**: {}\n", icon, w.category, w.message));
                md.push_str(&format!("  - 建议: {}\n", w.suggestion));
            }
            md.push('\n');
        }

        // 建议
        if !self.recommendations.is_empty() {
            md.push_str("## 改进建议\n\n");
            for (i, rec) in self.recommendations.iter().enumerate() {
                md.push_str(&format!("{}. {}\n", i + 1, rec));
            }
        }

        md
    }
}

// =============================================================================
// 模块健康度分析器
// =============================================================================

/// 模块健康度分析器
pub struct ModuleHealthAnalyzer {
    thresholds: HealthThresholds,
}

impl ModuleHealthAnalyzer {
    /// 创建分析器
    pub fn new(thresholds: HealthThresholds) -> Self {
        Self { thresholds }
    }

    /// 使用默认阈值创建
    pub fn with_defaults() -> Self {
        Self::new(HealthThresholds::default())
    }

    /// 从源码内容分析模块健康度
    pub fn analyze_module(
        &self,
        module_name: &str,
        source_files: &HashMap<String, String>,
        dependency_metrics: DependencyMetrics,
    ) -> ModuleHealthReport {
        let api_surface = self.scan_api_surface(source_files);
        let code_metrics = self.compute_code_metrics(source_files);
        let bloat_warnings = self.detect_bloat(source_files, &code_metrics, &api_surface);
        let recommendations = self.generate_recommendations(
            &api_surface,
            &code_metrics,
            &dependency_metrics,
            &bloat_warnings,
        );

        let health_score = self.compute_health_score(
            &api_surface,
            &code_metrics,
            &dependency_metrics,
            &bloat_warnings,
        );

        ModuleHealthReport {
            module_name: module_name.to_string(),
            health_score,
            grade: HealthGrade::from_score(health_score),
            api_surface,
            code_metrics,
            dependency_metrics,
            bloat_warnings,
            recommendations,
        }
    }

    /// 扫描 API 表面积
    pub fn scan_api_surface(&self, source_files: &HashMap<String, String>) -> ApiSurface {
        let mut surface = ApiSurface::default();

        for (path, content) in source_files {
            // 只扫描头文件的公开 API
            if !is_header_file(path) {
                continue;
            }

            for line in content.lines() {
                let trimmed = line.trim();

                // 公开类声明
                if (trimmed.starts_with("class ") || trimmed.starts_with("class\t"))
                    && !trimmed.contains(';')  // 排除前向声明
                    && !trimmed.starts_with("class {")
                {
                    surface.public_class_count += 1;
                    if let Some(name) = extract_class_name(trimmed) {
                        surface.public_class_names.push(name);
                    }
                }

                // 公开结构体
                if (trimmed.starts_with("struct ") || trimmed.starts_with("struct\t"))
                    && !trimmed.contains(';')
                {
                    surface.public_struct_count += 1;
                }

                // 公开枚举
                if trimmed.starts_with("enum ") || trimmed.starts_with("enum class ") {
                    surface.public_enum_count += 1;
                }

                // 公开函数 (非成员，在命名空间级别)
                // 简化: 检测 API 导出宏
                if trimmed.contains("_API ")
                    && (trimmed.contains('(') || trimmed.contains("()"))
                    && !trimmed.starts_with("//")
                {
                    surface.public_function_count += 1;
                }

                // 宏定义
                if trimmed.starts_with("#define ") && !trimmed.starts_with("#define _") {
                    surface.public_macro_count += 1;
                }
            }
        }

        surface.total_exported_symbols = surface.total_surface();
        surface
    }

    /// 计算代码度量
    pub fn compute_code_metrics(&self, source_files: &HashMap<String, String>) -> CodeMetrics {
        let mut metrics = CodeMetrics::default();
        let mut file_line_counts: Vec<(String, usize)> = Vec::new();
        let mut total_complexity: usize = 0;
        let mut function_count: usize = 0;

        for (path, content) in source_files {
            let lines: Vec<&str> = content.lines().collect();
            let line_count = lines.len();
            file_line_counts.push((path.clone(), line_count));

            metrics.total_lines += line_count;

            if is_header_file(path) {
                metrics.header_file_count += 1;
            } else {
                metrics.impl_file_count += 1;
            }

            let mut in_block_comment = false;
            for line in &lines {
                let trimmed = line.trim();

                if trimmed.is_empty() {
                    metrics.blank_lines += 1;
                    continue;
                }

                // 块注释跟踪
                if trimmed.contains("/*") {
                    in_block_comment = true;
                }
                if in_block_comment {
                    metrics.comment_lines += 1;
                    if trimmed.contains("*/") {
                        in_block_comment = false;
                    }
                    continue;
                }

                if trimmed.starts_with("//") {
                    metrics.comment_lines += 1;
                    continue;
                }

                metrics.code_lines += 1;

                // #include 统计
                if trimmed.starts_with("#include") {
                    metrics.total_includes += 1;
                }

                // 圈复杂度估算: 计算分支关键字
                let complexity = count_complexity_keywords(trimmed);
                total_complexity += complexity;
                if complexity > 0 {
                    function_count += 1;
                }
            }

            // 跟踪最大文件
            if line_count > metrics.max_file_lines {
                metrics.max_file_lines = line_count;
                metrics.largest_file = path.clone();
            }
        }

        // 平均值
        let file_count = source_files.len();
        metrics.avg_file_lines = if file_count > 0 {
            metrics.total_lines as f64 / file_count as f64
        } else {
            0.0
        };

        metrics.avg_cyclomatic_complexity = if function_count > 0 {
            total_complexity as f64 / function_count as f64
        } else {
            0.0
        };
        metrics.max_cyclomatic_complexity = total_complexity;

        metrics.comment_coverage_percent = if metrics.code_lines + metrics.comment_lines > 0 {
            metrics.comment_lines as f64 / (metrics.code_lines + metrics.comment_lines) as f64
                * 100.0
        } else {
            0.0
        };

        metrics.header_impl_ratio = if metrics.impl_file_count > 0 {
            metrics.header_file_count as f64 / metrics.impl_file_count as f64
        } else {
            metrics.header_file_count as f64
        };

        metrics
    }

    /// 检测代码膨胀
    pub fn detect_bloat(
        &self,
        source_files: &HashMap<String, String>,
        code_metrics: &CodeMetrics,
        api_surface: &ApiSurface,
    ) -> Vec<BloatWarning> {
        let mut warnings = Vec::new();

        // 检测大文件
        for (path, content) in source_files {
            let line_count = content.lines().count();
            if line_count > self.thresholds.large_file_lines {
                let severity = if line_count > self.thresholds.large_file_lines * 2 {
                    BloatSeverity::Critical
                } else {
                    BloatSeverity::Warning
                };
                warnings.push(BloatWarning {
                    severity,
                    category: "大文件".to_string(),
                    message: format!("{} ({} 行)", path, line_count),
                    file: Some(path.clone()),
                    suggestion: format!(
                        "考虑将文件拆分为更小的模块，目标 < {} 行",
                        self.thresholds.large_file_lines,
                    ),
                });
            }
        }

        // API 表面积过大
        if api_surface.total_surface() > self.thresholds.excessive_api_surface {
            warnings.push(BloatWarning {
                severity: BloatSeverity::Warning,
                category: "API 表面积过大".to_string(),
                message: format!(
                    "公开符号数 {} 超过阈值 {}",
                    api_surface.total_surface(),
                    self.thresholds.excessive_api_surface,
                ),
                file: None,
                suggestion: "减少公开 API，将实现细节移入 Internal/ 命名空间或 .cpp 文件"
                    .to_string(),
            });
        }

        // 注释覆盖率过低
        if code_metrics.comment_coverage_percent < self.thresholds.min_comment_coverage_percent {
            warnings.push(BloatWarning {
                severity: BloatSeverity::Info,
                category: "注释覆盖率低".to_string(),
                message: format!(
                    "注释覆盖率 {:.1}% 低于最低要求 {:.1}%",
                    code_metrics.comment_coverage_percent,
                    self.thresholds.min_comment_coverage_percent,
                ),
                file: None,
                suggestion: "为公开 API 添加 Doxygen 风格注释".to_string(),
            });
        }

        // 头文件/实现文件比例异常
        if code_metrics.header_impl_ratio > self.thresholds.max_header_impl_ratio
            && code_metrics.impl_file_count > 0
        {
            warnings.push(BloatWarning {
                severity: BloatSeverity::Warning,
                category: "头重脚轻".to_string(),
                message: format!(
                    "头文件/实现文件比例 {:.1} 超过阈值 {:.1}",
                    code_metrics.header_impl_ratio, self.thresholds.max_header_impl_ratio,
                ),
                file: None,
                suggestion: "将更多实现从头文件移入 .cpp 文件以减少编译依赖".to_string(),
            });
        }

        warnings
    }

    /// 计算健康评分 (0-100)
    pub fn compute_health_score(
        &self,
        api_surface: &ApiSurface,
        code_metrics: &CodeMetrics,
        dep_metrics: &DependencyMetrics,
        bloat_warnings: &[BloatWarning],
    ) -> f64 {
        let mut score: f64 = 100.0;

        // API 表面积惩罚 (超过阈值每多10个扣2分)
        let api_excess = api_surface
            .total_surface()
            .saturating_sub(self.thresholds.excessive_api_surface);
        score -= (api_excess as f64 / 10.0) * 2.0;

        // 大文件惩罚 (每个大文件扣3分)
        let large_file_count = bloat_warnings
            .iter()
            .filter(|w| w.category == "大文件")
            .count();
        score -= large_file_count as f64 * 3.0;

        // 注释覆盖率奖罚
        if code_metrics.comment_coverage_percent >= self.thresholds.min_comment_coverage_percent {
            score += 5.0; // 良好注释奖励
        } else {
            score -= 5.0;
        }

        // 扇出过大惩罚
        if dep_metrics.fan_out > self.thresholds.excessive_fan_out {
            let excess = dep_metrics.fan_out - self.thresholds.excessive_fan_out;
            score -= excess as f64 * 2.0;
        }

        // 不稳定度惩罚 (高不稳定度 = 频繁变化的模块)
        if dep_metrics.instability > 0.8 {
            score -= 5.0;
        }

        // 严重膨胀警告惩罚
        let critical_count = bloat_warnings
            .iter()
            .filter(|w| w.severity == BloatSeverity::Critical)
            .count();
        score -= critical_count as f64 * 5.0;

        // 头文件比例异常惩罚
        if code_metrics.header_impl_ratio > self.thresholds.max_header_impl_ratio
            && code_metrics.impl_file_count > 0
        {
            score -= 5.0;
        }

        score.clamp(0.0, 100.0)
    }

    /// 生成改进建议
    fn generate_recommendations(
        &self,
        api_surface: &ApiSurface,
        code_metrics: &CodeMetrics,
        dep_metrics: &DependencyMetrics,
        bloat_warnings: &[BloatWarning],
    ) -> Vec<String> {
        let mut recs = Vec::new();

        if api_surface.total_surface() > self.thresholds.excessive_api_surface {
            recs.push(format!(
                "减少公开 API 符号数 (当前 {}, 建议 < {}), 使用 pimpl 或 Internal 命名空间隐藏实现",
                api_surface.total_surface(),
                self.thresholds.excessive_api_surface,
            ));
        }

        if dep_metrics.fan_out > self.thresholds.excessive_fan_out {
            recs.push(format!(
                "减少模块依赖数 (当前扇出 {}, 建议 < {}), 考虑引入中间抽象层",
                dep_metrics.fan_out, self.thresholds.excessive_fan_out,
            ));
        }

        if dep_metrics.instability > 0.8 {
            recs.push(format!(
                "模块不稳定度 {:.2} 过高, 考虑将稳定接口提取为独立模块",
                dep_metrics.instability,
            ));
        }

        if code_metrics.comment_coverage_percent < self.thresholds.min_comment_coverage_percent {
            recs.push("增加注释覆盖率, 优先为公开 API 和复杂算法添加文档".to_string());
        }

        let large_files: Vec<&BloatWarning> = bloat_warnings
            .iter()
            .filter(|w| w.category == "大文件" && w.severity == BloatSeverity::Critical)
            .collect();
        if !large_files.is_empty() {
            recs.push(format!(
                "拆分 {} 个超大文件, 每个文件建议不超过 {} 行",
                large_files.len(),
                self.thresholds.large_file_lines,
            ));
        }

        if code_metrics.header_impl_ratio > self.thresholds.max_header_impl_ratio
            && code_metrics.impl_file_count > 0
        {
            recs.push("将更多实现从头文件移入 .cpp 文件以降低编译耦合".to_string());
        }

        recs
    }
}

// =============================================================================
// 辅助函数
// =============================================================================

fn is_header_file(path: &str) -> bool {
    path.ends_with(".h") || path.ends_with(".hpp") || path.ends_with(".hxx")
}

fn extract_class_name(line: &str) -> Option<String> {
    // "class CORE_API FMyClass : public FBase {"
    let parts: Vec<&str> = line.split_whitespace().collect();
    for (i, part) in parts.iter().enumerate() {
        if *part == "class" {
            // 下一个词可能是 API 宏或类名
            if let Some(&next) = parts.get(i + 1) {
                if next.contains("_API") {
                    // API 宏后面才是类名
                    return parts.get(i + 2).map(|s| s.trim_matches(':').to_string());
                } else {
                    return Some(next.trim_matches(':').to_string());
                }
            }
        }
    }
    None
}

fn count_complexity_keywords(line: &str) -> usize {
    let keywords = [
        "if ", "if(", "else if", "for ", "for(", "while ", "while(", "switch ", "switch(", "case ",
        "catch ", "catch(", "&&", "||", "? ",
    ];
    keywords.iter().filter(|&&kw| line.contains(kw)).count()
}

// =============================================================================
// 测试
// =============================================================================

#[cfg(test)]
mod tests {
    use super::*;

    fn sample_header() -> String {
        r#"
// 文件: Core/Memory.h
#pragma once

#include "Platform.h"
#include "Types.h"

// 内存分配器接口
class IAllocator
{
public:
    virtual void* Allocate(size_t size) = 0;
    virtual void Deallocate(void* ptr) = 0;
};

// 块分配器
class CORE_API BlockAllocator : public IAllocator
{
public:
    void* Allocate(size_t size) override;
    void Deallocate(void* ptr) override;
private:
    void* m_Pool;
};

struct AllocationStats
{
    size_t TotalAllocated;
    size_t TotalFreed;
};

enum class AllocationPolicy
{
    Default,
    Aligned,
    Pool,
};

#define LIMX_ALLOC(size) BlockAllocator::GetDefault().Allocate(size)
"#
        .to_string()
    }

    fn sample_impl() -> String {
        r#"
// 文件: Core/Memory.cpp
#include "Memory.h"

void* BlockAllocator::Allocate(size_t size)
{
    if (size == 0)
    {
        return nullptr;
    }
    if (size > kMaxBlockSize)
    {
        return AllocateLarge(size);
    }
    for (auto& block : m_FreeList)
    {
        if (block.Size >= size)
        {
            return block.Ptr;
        }
    }
    return nullptr;
}

void BlockAllocator::Deallocate(void* ptr)
{
    if (ptr == nullptr) return;
    // 归还到空闲列表
}
"#
        .to_string()
    }

    fn build_test_files() -> HashMap<String, String> {
        let mut files = HashMap::new();
        files.insert("Core/Memory.h".to_string(), sample_header());
        files.insert("Core/Memory.cpp".to_string(), sample_impl());
        files
    }

    #[test]
    fn test_api_surface_scanning() {
        let analyzer = ModuleHealthAnalyzer::with_defaults();
        let files = build_test_files();
        let surface = analyzer.scan_api_surface(&files);

        assert_eq!(
            surface.public_class_count, 2,
            "应检测到 IAllocator 和 BlockAllocator"
        );
        assert_eq!(surface.public_struct_count, 1, "应检测到 AllocationStats");
        assert_eq!(surface.public_enum_count, 1, "应检测到 AllocationPolicy");
        assert!(surface.public_macro_count >= 1, "应检测到 LIMX_ALLOC 宏");
    }

    #[test]
    fn test_code_metrics() {
        let analyzer = ModuleHealthAnalyzer::with_defaults();
        let files = build_test_files();
        let metrics = analyzer.compute_code_metrics(&files);

        assert!(metrics.total_lines > 0);
        assert!(metrics.code_lines > 0);
        assert!(metrics.comment_lines > 0);
        assert_eq!(metrics.header_file_count, 1);
        assert_eq!(metrics.impl_file_count, 1);
        assert!(metrics.total_includes >= 2, "应检测到至少 2 个 #include");
        assert!(metrics.comment_coverage_percent > 0.0);
    }

    #[test]
    fn test_dependency_instability() {
        let mut dep = DependencyMetrics {
            fan_in: 5,
            fan_out: 15,
            instability: 0.0,
            depends_on: vec!["A".to_string(), "B".to_string()],
            depended_by: vec!["C".to_string()],
        };
        dep.compute_instability();
        assert!((dep.instability - 0.75).abs() < 0.01);
    }

    #[test]
    fn test_detect_large_file() {
        let analyzer = ModuleHealthAnalyzer::new(HealthThresholds {
            large_file_lines: 20, // 极低阈值便于测试
            ..Default::default()
        });
        let files = build_test_files();
        let metrics = analyzer.compute_code_metrics(&files);
        let surface = analyzer.scan_api_surface(&files);
        let warnings = analyzer.detect_bloat(&files, &metrics, &surface);

        assert!(
            warnings.iter().any(|w| w.category == "大文件"),
            "应检测到超过20行的大文件"
        );
    }

    #[test]
    fn test_health_score_range() {
        let analyzer = ModuleHealthAnalyzer::with_defaults();
        let files = build_test_files();
        let dep = DependencyMetrics::default();

        let report = analyzer.analyze_module("Core", &files, dep);
        assert!(
            report.health_score >= 0.0 && report.health_score <= 100.0,
            "健康评分应在 0-100 范围内, 实际: {}",
            report.health_score
        );
    }

    #[test]
    fn test_health_grade_mapping() {
        assert_eq!(HealthGrade::from_score(95.0), HealthGrade::Excellent);
        assert_eq!(HealthGrade::from_score(80.0), HealthGrade::Good);
        assert_eq!(HealthGrade::from_score(65.0), HealthGrade::Fair);
        assert_eq!(HealthGrade::from_score(45.0), HealthGrade::Poor);
        assert_eq!(HealthGrade::from_score(20.0), HealthGrade::Critical);
    }

    #[test]
    fn test_markdown_report() {
        let analyzer = ModuleHealthAnalyzer::with_defaults();
        let files = build_test_files();
        let dep = DependencyMetrics::default();

        let report = analyzer.analyze_module("Core", &files, dep);
        let md = report.to_markdown();

        assert!(md.contains("# 模块健康报告: Core"));
        assert!(md.contains("API 表面积"));
        assert!(md.contains("代码度量"));
        assert!(md.contains("依赖度量"));
    }

    #[test]
    fn test_healthy_module_high_score() {
        let analyzer = ModuleHealthAnalyzer::with_defaults();

        // 一个健康的小模块
        let mut files = HashMap::new();
        files.insert(
            "Math/Vector.h".to_string(),
            "// 向量数学库\nclass Vector3 {\npublic:\n    float X, Y, Z;\n};\n".to_string(),
        );
        files.insert(
            "Math/Vector.cpp".to_string(),
            "// 向量实现\n#include \"Vector.h\"\n".to_string(),
        );

        let dep = DependencyMetrics {
            fan_in: 10,
            fan_out: 1,
            instability: 0.09,
            ..Default::default()
        };

        let report = analyzer.analyze_module("Math", &files, dep);
        assert!(
            report.health_score >= 75.0,
            "健康小模块评分应 >= 75, 实际: {}",
            report.health_score
        );
    }

    #[test]
    fn test_unhealthy_module_low_score() {
        let analyzer = ModuleHealthAnalyzer::new(HealthThresholds {
            large_file_lines: 5,
            excessive_api_surface: 2,
            excessive_fan_out: 1,
            ..Default::default()
        });

        let files = build_test_files(); // 会触发多个警告
        let dep = DependencyMetrics {
            fan_in: 0,
            fan_out: 15,
            instability: 1.0,
            depends_on: (0..15).map(|i| format!("Dep{}", i)).collect(),
            ..Default::default()
        };

        let report = analyzer.analyze_module("BadModule", &files, dep);
        assert!(
            report.health_score < 70.0,
            "不健康模块评分应 < 70, 实际: {}",
            report.health_score
        );
        assert!(!report.bloat_warnings.is_empty());
        assert!(!report.recommendations.is_empty());
    }

    #[test]
    fn test_extract_class_name() {
        assert_eq!(
            extract_class_name("class IAllocator"),
            Some("IAllocator".to_string())
        );
        assert_eq!(
            extract_class_name("class CORE_API BlockAllocator : public IAllocator"),
            Some("BlockAllocator".to_string())
        );
    }

    #[test]
    fn test_empty_module() {
        let analyzer = ModuleHealthAnalyzer::with_defaults();
        let files = HashMap::new();
        let dep = DependencyMetrics::default();
        let report = analyzer.analyze_module("Empty", &files, dep);
        assert_eq!(report.code_metrics.total_lines, 0);
        assert_eq!(report.api_surface.total_surface(), 0);
    }
}
