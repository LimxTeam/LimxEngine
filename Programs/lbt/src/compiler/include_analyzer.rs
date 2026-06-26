// ============================================================
// 文件名称：include_analyzer.rs
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：未使用头文件检测 — 精确识别冗余 #include，减少
//           编译时间和依赖耦合。UE5 内置的 IncludeTool 仅做
//           基础检查，我们提供符号级引用追踪 + 传递包含分析 +
//           可操作的清理建议，帮助开发者持续优化编译速度
// 功能描述：扫描每个 .cpp/.h 文件的 #include 指令，分析被
//           包含头文件中声明的符号是否在当前文件中被使用，
//           检测传递包含 (A 包含 B，B 包含 C，A 也包含 C)，
//           并生成详细的清理建议报告
// 技术特性：正则扫描 #include、符号使用检测、传递包含检测、
//           保守策略 (只报告高确信度的冗余)、Markdown 报告
//
// ── 结构体表 ──────────────────────────────────────────────
// │ 结构体名                    │ 描述                         │
// │────────────────────────────│─────────────────────────────│
// │ IncludeAnalyzer            │ 未使用头文件分析器             │
// │ IncludeDirective           │ 包含指令描述                  │
// │ IncludeAnalysisReport      │ 分析报告                     │
// │ FileAnalysisResult         │ 单文件分析结果                │
// │ RedundantInclude           │ 冗余包含描述                  │
// │ TransitiveInclude          │ 传递包含描述                  │
//
// ── 函数表 ──────────────────────────────────────────────
// │ 函数名                      │ 描述                         │
// │────────────────────────────│─────────────────────────────│
// │ new()                      │ 创建分析器                    │
// │ analyze_file()             │ 分析单个文件                  │
// │ analyze_all()              │ 分析所有文件                  │
// │ scan_includes()            │ 扫描 #include 指令            │
// │ extract_symbols()          │ 提取头文件声明的符号           │
// │ check_symbol_usage()       │ 检查符号使用情况              │
// │ detect_transitive()        │ 检测传递包含                  │
// │ generate_report()          │ 生成报告                     │
//
// ── 更新历史 ──────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                         │
// │─────────────│──────────│─────────────────────────────│
// │ 2026-04-06  │ LimxTeam  │ 初始创建                      │
// ============================================================

use serde::{Deserialize, Serialize};
use std::collections::{HashMap, HashSet};

// =============================================================================
// 包含指令
// =============================================================================

/// 包含指令类型
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum IncludeStyle {
    /// "header.h" — 项目本地包含
    Quoted,
    /// <header.h> — 系统/外部包含
    Angled,
}

/// 包含指令
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct IncludeDirective {
    /// 包含路径
    pub path: String,
    /// 包含风格
    pub style: IncludeStyle,
    /// 所在行号 (1-indexed)
    pub line_number: usize,
    /// 是否在 #ifdef 条件块内
    pub is_conditional: bool,
}

// =============================================================================
// 冗余包含
// =============================================================================

/// 冗余原因
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub enum RedundancyReason {
    /// 头文件中无符号被当前文件使用
    NoSymbolUsed,
    /// 已被其他包含间接提供 (传递包含)
    TransitivelyIncluded,
    /// 重复包含
    DuplicateInclude,
}

/// 冗余包含描述
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RedundantInclude {
    /// 冗余的包含路径
    pub include_path: String,
    /// 所在文件
    pub in_file: String,
    /// 行号
    pub line_number: usize,
    /// 冗余原因
    pub reason: RedundancyReason,
    /// 详细说明
    pub explanation: String,
    /// 确信度 (0.0-1.0)
    pub confidence: f64,
}

/// 传递包含描述
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TransitiveInclude {
    /// 直接包含的头文件 (提供者)
    pub provider: String,
    /// 被间接包含的头文件
    pub transitively_included: String,
    /// 也直接包含了该文件的文件 (冗余方)
    pub redundant_in: String,
}

// =============================================================================
// 头文件符号
// =============================================================================

/// 头文件导出的符号
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct HeaderSymbols {
    /// 类名
    pub classes: Vec<String>,
    /// 结构体名
    pub structs: Vec<String>,
    /// 枚举名
    pub enums: Vec<String>,
    /// 函数名
    pub functions: Vec<String>,
    /// 类型别名
    pub typedefs: Vec<String>,
    /// 宏名
    pub macros: Vec<String>,
}

impl HeaderSymbols {
    /// 所有符号
    pub fn all_symbols(&self) -> Vec<&str> {
        let mut all = Vec::new();
        for s in &self.classes {
            all.push(s.as_str());
        }
        for s in &self.structs {
            all.push(s.as_str());
        }
        for s in &self.enums {
            all.push(s.as_str());
        }
        for s in &self.functions {
            all.push(s.as_str());
        }
        for s in &self.typedefs {
            all.push(s.as_str());
        }
        for s in &self.macros {
            all.push(s.as_str());
        }
        all
    }

    /// 符号总数
    pub fn count(&self) -> usize {
        self.classes.len()
            + self.structs.len()
            + self.enums.len()
            + self.functions.len()
            + self.typedefs.len()
            + self.macros.len()
    }
}

// =============================================================================
// 分析结果
// =============================================================================

/// 单文件分析结果
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct FileAnalysisResult {
    /// 文件路径
    pub file_path: String,
    /// 所有 #include 指令
    pub includes: Vec<IncludeDirective>,
    /// 冗余包含列表
    pub redundant_includes: Vec<RedundantInclude>,
    /// 使用中的包含数
    pub used_include_count: usize,
    /// 冗余包含数
    pub redundant_include_count: usize,
    /// 利用率 (used / total)
    pub utilization_percent: f64,
}

/// 整体分析报告
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct IncludeAnalysisReport {
    /// 分析的文件数
    pub files_analyzed: usize,
    /// 总 #include 数
    pub total_includes: usize,
    /// 冗余 #include 数
    pub total_redundant: usize,
    /// 重复 #include 数
    pub duplicate_includes: usize,
    /// 传递包含数
    pub transitive_includes: usize,
    /// 平均利用率
    pub avg_utilization_percent: f64,
    /// 预估清理后编译加速 (百分比)
    pub estimated_speedup_percent: f64,
    /// 每个文件的详细结果
    pub file_results: Vec<FileAnalysisResult>,
    /// 传递包含列表
    pub transitive_include_list: Vec<TransitiveInclude>,
    /// 最多冗余包含的文件 (Top N)
    pub worst_files: Vec<(String, usize)>,
}

impl IncludeAnalysisReport {
    /// 导出 Markdown 报告
    pub fn to_markdown(&self) -> String {
        let mut md = String::with_capacity(4096);
        md.push_str("# 未使用头文件分析报告\n\n");
        md.push_str(&format!("| 指标 | 值 |\n|------|----|\n"));
        md.push_str(&format!("| 分析文件数 | {} |\n", self.files_analyzed));
        md.push_str(&format!("| 总 #include 数 | {} |\n", self.total_includes));
        md.push_str(&format!(
            "| 冗余 #include 数 | **{}** |\n",
            self.total_redundant
        ));
        md.push_str(&format!("| 重复包含 | {} |\n", self.duplicate_includes));
        md.push_str(&format!("| 传递包含 | {} |\n", self.transitive_includes));
        md.push_str(&format!(
            "| 平均利用率 | {:.1}% |\n",
            self.avg_utilization_percent
        ));
        md.push_str(&format!(
            "| 预估编译加速 | **{:.1}%** |\n\n",
            self.estimated_speedup_percent
        ));

        if !self.worst_files.is_empty() {
            md.push_str("## 冗余包含最多的文件\n\n");
            md.push_str("| 文件 | 冗余数 |\n|------|--------|\n");
            for (file, count) in &self.worst_files {
                md.push_str(&format!("| {} | {} |\n", file, count));
            }
            md.push('\n');
        }

        md
    }
}

// =============================================================================
// 未使用头文件分析器
// =============================================================================

/// 未使用头文件分析器
pub struct IncludeAnalyzer {
    /// 头文件 → 导出符号映射
    header_symbols: HashMap<String, HeaderSymbols>,
    /// 头文件 → 其包含的头文件
    header_includes: HashMap<String, Vec<String>>,
}

impl IncludeAnalyzer {
    /// 创建分析器
    pub fn new() -> Self {
        Self {
            header_symbols: HashMap::new(),
            header_includes: HashMap::new(),
        }
    }

    /// 注册头文件的导出符号
    pub fn register_header(&mut self, path: &str, content: &str) {
        let symbols = extract_symbols_from_source(content);
        let includes = scan_include_paths(content);
        self.header_symbols.insert(path.to_string(), symbols);
        self.header_includes.insert(path.to_string(), includes);
    }

    /// 分析单个源文件
    pub fn analyze_file(&self, file_path: &str, content: &str) -> FileAnalysisResult {
        let includes = scan_includes(content);
        let mut redundant = Vec::new();
        let mut seen_includes: HashSet<String> = HashSet::new();

        for inc in &includes {
            // 检查重复包含
            if !seen_includes.insert(inc.path.clone()) {
                redundant.push(RedundantInclude {
                    include_path: inc.path.clone(),
                    in_file: file_path.to_string(),
                    line_number: inc.line_number,
                    reason: RedundancyReason::DuplicateInclude,
                    explanation: format!("'{}' 在此文件中被重复包含", inc.path),
                    confidence: 1.0,
                });
                continue;
            }

            // 跳过条件包含 (保守策略)
            if inc.is_conditional {
                continue;
            }

            // 检查符号使用
            if let Some(symbols) = self.header_symbols.get(&inc.path) {
                let all_syms = symbols.all_symbols();
                let any_used = if all_syms.is_empty() {
                    false // 头文件无可识别符号视为未使用
                } else {
                    all_syms.iter().any(|sym| {
                        // 在内容中搜索符号使用 (排除 #include 行本身)
                        content.lines().any(|line| {
                            let trimmed = line.trim();
                            !trimmed.starts_with("#include") && trimmed.contains(sym)
                        })
                    })
                };

                if !any_used {
                    redundant.push(RedundantInclude {
                        include_path: inc.path.clone(),
                        in_file: file_path.to_string(),
                        line_number: inc.line_number,
                        reason: RedundancyReason::NoSymbolUsed,
                        explanation: format!(
                            "'{}' 导出 {} 个符号, 但当前文件未使用任何一个",
                            inc.path,
                            all_syms.len(),
                        ),
                        confidence: if all_syms.is_empty() { 0.6 } else { 0.85 },
                    });
                }
            }

            // 检查传递包含
            self.check_transitive_redundancy(file_path, &inc.path, &includes, &mut redundant);
        }

        let total = includes.len();
        let redundant_count = redundant.len();
        let used = total.saturating_sub(redundant_count);
        let utilization = if total > 0 {
            used as f64 / total as f64 * 100.0
        } else {
            100.0
        };

        FileAnalysisResult {
            file_path: file_path.to_string(),
            includes,
            redundant_includes: redundant,
            used_include_count: used,
            redundant_include_count: redundant_count,
            utilization_percent: utilization,
        }
    }

    /// 检查传递包含冗余
    fn check_transitive_redundancy(
        &self,
        file_path: &str,
        include_path: &str,
        all_includes: &[IncludeDirective],
        redundant: &mut Vec<RedundantInclude>,
    ) {
        // 检查其他被包含的头文件是否已经包含了 include_path
        for other_inc in all_includes {
            if other_inc.path == include_path {
                continue;
            }
            if let Some(other_includes) = self.header_includes.get(&other_inc.path) {
                if other_includes.contains(&include_path.to_string()) {
                    // other_inc 已经包含了 include_path，当前的直接包含是冗余的
                    // 避免重复添加
                    let already_reported = redundant.iter().any(|r| {
                        r.include_path == include_path
                            && r.reason == RedundancyReason::TransitivelyIncluded
                    });
                    if !already_reported {
                        redundant.push(RedundantInclude {
                            include_path: include_path.to_string(),
                            in_file: file_path.to_string(),
                            line_number: 0,
                            reason: RedundancyReason::TransitivelyIncluded,
                            explanation: format!(
                                "'{}' 已被 '{}' 间接包含, 直接包含是冗余的",
                                include_path, other_inc.path,
                            ),
                            confidence: 0.9,
                        });
                    }
                    return;
                }
            }
        }
    }

    /// 批量分析所有文件并生成报告
    pub fn analyze_all(&self, files: &HashMap<String, String>) -> IncludeAnalysisReport {
        let mut file_results = Vec::new();
        let mut total_includes = 0usize;
        let mut total_redundant = 0usize;
        let mut total_duplicates = 0usize;
        let mut total_transitive = 0usize;
        let mut util_sum = 0.0f64;
        let mut transitive_list = Vec::new();

        for (path, content) in files {
            let result = self.analyze_file(path, content);
            total_includes += result.includes.len();
            total_redundant += result.redundant_include_count;
            util_sum += result.utilization_percent;

            for r in &result.redundant_includes {
                match r.reason {
                    RedundancyReason::DuplicateInclude => total_duplicates += 1,
                    RedundancyReason::TransitivelyIncluded => {
                        total_transitive += 1;
                        transitive_list.push(TransitiveInclude {
                            provider: r.explanation.clone(),
                            transitively_included: r.include_path.clone(),
                            redundant_in: path.clone(),
                        });
                    }
                    _ => {}
                }
            }

            file_results.push(result);
        }

        let files_analyzed = file_results.len();
        let avg_util = if files_analyzed > 0 {
            util_sum / files_analyzed as f64
        } else {
            100.0
        };

        // 预估加速: 冗余包含占比 * 估算系数 (每个冗余 #include 约增加 0.5% 编译时间)
        let estimated_speedup = if total_includes > 0 {
            (total_redundant as f64 / total_includes as f64) * 30.0
        } else {
            0.0
        };

        // 最差文件排行
        let mut worst: Vec<(String, usize)> = file_results
            .iter()
            .filter(|r| r.redundant_include_count > 0)
            .map(|r| (r.file_path.clone(), r.redundant_include_count))
            .collect();
        worst.sort_by(|a, b| b.1.cmp(&a.1));
        worst.truncate(10);

        IncludeAnalysisReport {
            files_analyzed,
            total_includes,
            total_redundant,
            duplicate_includes: total_duplicates,
            transitive_includes: total_transitive,
            avg_utilization_percent: avg_util,
            estimated_speedup_percent: estimated_speedup,
            file_results,
            transitive_include_list: transitive_list,
            worst_files: worst,
        }
    }
}

// =============================================================================
// 辅助函数
// =============================================================================

/// 扫描 #include 指令
pub fn scan_includes(content: &str) -> Vec<IncludeDirective> {
    let mut includes = Vec::new();
    let mut conditional_depth: usize = 0;

    for (idx, line) in content.lines().enumerate() {
        let trimmed = line.trim();

        // 追踪条件编译深度
        if trimmed.starts_with("#if")
            || trimmed.starts_with("#ifdef")
            || trimmed.starts_with("#ifndef")
        {
            conditional_depth += 1;
        }
        if trimmed.starts_with("#endif") {
            conditional_depth = conditional_depth.saturating_sub(1);
        }

        if !trimmed.starts_with("#include") {
            continue;
        }

        let rest = trimmed["#include".len()..].trim();
        if let Some(path) = rest.strip_prefix('"').and_then(|s| s.strip_suffix('"')) {
            includes.push(IncludeDirective {
                path: path.to_string(),
                style: IncludeStyle::Quoted,
                line_number: idx + 1,
                is_conditional: conditional_depth > 0,
            });
        } else if let Some(path) = rest.strip_prefix('<').and_then(|s| s.strip_suffix('>')) {
            includes.push(IncludeDirective {
                path: path.to_string(),
                style: IncludeStyle::Angled,
                line_number: idx + 1,
                is_conditional: conditional_depth > 0,
            });
        }
    }

    includes
}

/// 仅扫描包含路径 (简化版)
fn scan_include_paths(content: &str) -> Vec<String> {
    scan_includes(content)
        .into_iter()
        .map(|inc| inc.path)
        .collect()
}

/// 从源码提取导出符号
pub fn extract_symbols_from_source(content: &str) -> HeaderSymbols {
    let mut symbols = HeaderSymbols::default();

    for line in content.lines() {
        let trimmed = line.trim();

        // 类 (排除前向声明: 以';'结尾但不含'{}'的行)
        if (trimmed.starts_with("class ") || trimmed.starts_with("class\t"))
            && !(trimmed.ends_with(';') && !trimmed.contains('{'))
        {
            if let Some(name) = extract_identifier_after(trimmed, "class") {
                symbols.classes.push(name);
            }
        }

        // 结构体 (排除前向声明)
        if (trimmed.starts_with("struct ") || trimmed.starts_with("struct\t"))
            && !(trimmed.ends_with(';') && !trimmed.contains('{'))
        {
            if let Some(name) = extract_identifier_after(trimmed, "struct") {
                symbols.structs.push(name);
            }
        }

        // 枚举
        if trimmed.starts_with("enum ") {
            let rest = if trimmed.starts_with("enum class ") {
                &trimmed["enum class ".len()..]
            } else {
                &trimmed["enum ".len()..]
            };
            if let Some(name) = rest
                .split(|c: char| !c.is_alphanumeric() && c != '_')
                .next()
            {
                if !name.is_empty() {
                    symbols.enums.push(name.to_string());
                }
            }
        }

        // 函数 (简化: 检测返回类型 + 函数名 + 左括号)
        if !trimmed.starts_with("//")
            && !trimmed.starts_with('#')
            && !trimmed.starts_with("class")
            && !trimmed.starts_with("struct")
            && trimmed.contains('(')
            && !trimmed.contains("=")
            && !trimmed.starts_with("if")
            && !trimmed.starts_with("for")
            && !trimmed.starts_with("while")
            && !trimmed.starts_with("switch")
        {
            if let Some(func_name) = extract_function_name(trimmed) {
                if func_name.len() > 1
                    && func_name
                        .chars()
                        .next()
                        .map_or(false, |c| c.is_alphabetic())
                {
                    symbols.functions.push(func_name);
                }
            }
        }

        // typedef / using
        if trimmed.starts_with("typedef ") || trimmed.starts_with("using ") {
            let parts: Vec<&str> = trimmed.split_whitespace().collect();
            if parts.len() >= 2 {
                let name = parts.last().unwrap_or(&"").trim_end_matches(';');
                if !name.is_empty() && name != "=" {
                    symbols.typedefs.push(name.to_string());
                }
            }
        }

        // 宏
        if trimmed.starts_with("#define ") {
            let rest = &trimmed["#define ".len()..];
            if let Some(name) = rest
                .split(|c: char| !c.is_alphanumeric() && c != '_')
                .next()
            {
                if !name.is_empty() && !name.starts_with('_') {
                    symbols.macros.push(name.to_string());
                }
            }
        }
    }

    symbols
}

/// 提取关键字后的标识符
fn extract_identifier_after(line: &str, keyword: &str) -> Option<String> {
    let parts: Vec<&str> = line.split_whitespace().collect();
    for (i, part) in parts.iter().enumerate() {
        if *part == keyword {
            // 跳过可能的 API 宏
            if let Some(&next) = parts.get(i + 1) {
                if next.contains("_API") {
                    return parts
                        .get(i + 2)
                        .map(|s| s.trim_matches(':').trim_matches('{').to_string());
                } else {
                    return Some(next.trim_matches(':').trim_matches('{').to_string());
                }
            }
        }
    }
    None
}

/// 提取函数名
fn extract_function_name(line: &str) -> Option<String> {
    let paren_pos = line.find('(')?;
    let before_paren = line[..paren_pos].trim();
    let parts: Vec<&str> = before_paren.split_whitespace().collect();
    parts.last().map(|s| {
        s.trim_start_matches('*')
            .trim_start_matches('&')
            .to_string()
    })
}

// =============================================================================
// 测试
// =============================================================================

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_scan_includes_basic() {
        let source = r#"
#include "Core.h"
#include <vector>
#include "Math/Vector.h"

void main() {}
"#;
        let includes = scan_includes(source);
        assert_eq!(includes.len(), 3);
        assert_eq!(includes[0].path, "Core.h");
        assert_eq!(includes[0].style, IncludeStyle::Quoted);
        assert_eq!(includes[1].path, "vector");
        assert_eq!(includes[1].style, IncludeStyle::Angled);
        assert_eq!(includes[2].path, "Math/Vector.h");
    }

    #[test]
    fn test_conditional_include_detection() {
        let source = r#"
#include "Always.h"
#ifdef PLATFORM_WINDOWS
#include "Windows.h"
#endif
#include "Common.h"
"#;
        let includes = scan_includes(source);
        assert_eq!(includes.len(), 3);
        assert!(!includes[0].is_conditional, "Always.h 不在条件块内");
        assert!(includes[1].is_conditional, "Windows.h 在 #ifdef 内");
        assert!(!includes[2].is_conditional, "Common.h 在 #endif 后");
    }

    #[test]
    fn test_extract_symbols() {
        let header = r#"
#pragma once
#include "Base.h"

class IAllocator
{
public:
    virtual void* Allocate(size_t size) = 0;
};

class CORE_API BlockAllocator : public IAllocator
{
};

struct AllocationStats
{
    size_t Total;
};

enum class AllocPolicy { Default, Pool };

typedef unsigned int uint32;

#define MAX_ALLOC_SIZE 4096
"#;
        let symbols = extract_symbols_from_source(header);
        assert!(symbols.classes.contains(&"IAllocator".to_string()));
        assert!(symbols.classes.contains(&"BlockAllocator".to_string()));
        assert!(symbols.structs.contains(&"AllocationStats".to_string()));
        assert!(symbols.enums.contains(&"AllocPolicy".to_string()));
        assert!(symbols.macros.contains(&"MAX_ALLOC_SIZE".to_string()));
    }

    #[test]
    fn test_detect_unused_include() {
        let mut analyzer = IncludeAnalyzer::new();

        // 注册头文件: Math.h 导出 Vector3
        analyzer.register_header("Math.h", "class Vector3 { float X, Y, Z; };");
        // 注册头文件: Audio.h 导出 AudioPlayer
        analyzer.register_header("Audio.h", "class AudioPlayer { void Play(); };");

        // 源文件包含 Math.h 和 Audio.h，但只使用了 Vector3
        let source = r#"
#include "Math.h"
#include "Audio.h"

void Test() {
    Vector3 pos;
}
"#;
        let result = analyzer.analyze_file("test.cpp", source);
        assert!(
            result
                .redundant_includes
                .iter()
                .any(|r| r.include_path == "Audio.h" && r.reason == RedundancyReason::NoSymbolUsed),
            "Audio.h 未使用任何符号应被标记为冗余"
        );
    }

    #[test]
    fn test_detect_duplicate_include() {
        let analyzer = IncludeAnalyzer::new();

        let source = r#"
#include "Core.h"
#include "Math.h"
#include "Core.h"
"#;
        let result = analyzer.analyze_file("test.cpp", source);
        assert!(result
            .redundant_includes
            .iter()
            .any(|r| r.include_path == "Core.h" && r.reason == RedundancyReason::DuplicateInclude));
    }

    #[test]
    fn test_detect_transitive_include() {
        let mut analyzer = IncludeAnalyzer::new();

        // Container.h 包含 Memory.h
        analyzer.register_header(
            "Container.h",
            "#include \"Memory.h\"\nclass Array { void* data; };",
        );
        analyzer.register_header("Memory.h", "class Allocator { void* Alloc(); };");

        // 源文件同时直接包含 Container.h 和 Memory.h
        // 但 Memory.h 已被 Container.h 间接包含
        let source = r#"
#include "Container.h"
#include "Memory.h"

void Test() {
    Array arr;
    Allocator alloc;
}
"#;
        let result = analyzer.analyze_file("test.cpp", source);
        assert!(
            result
                .redundant_includes
                .iter()
                .any(|r| r.include_path == "Memory.h"
                    && r.reason == RedundancyReason::TransitivelyIncluded),
            "Memory.h 被 Container.h 传递包含, 应被标记"
        );
    }

    #[test]
    fn test_analyze_all_report() {
        let mut analyzer = IncludeAnalyzer::new();
        analyzer.register_header("A.h", "class Alpha {};");
        analyzer.register_header("B.h", "class Beta {};");

        let mut files = HashMap::new();
        // file1 使用 A 但不使用 B
        files.insert(
            "file1.cpp".to_string(),
            "#include \"A.h\"\n#include \"B.h\"\nAlpha a;\n".to_string(),
        );
        // file2 使用 B 但不使用 A
        files.insert(
            "file2.cpp".to_string(),
            "#include \"A.h\"\n#include \"B.h\"\nBeta b;\n".to_string(),
        );

        let report = analyzer.analyze_all(&files);
        assert_eq!(report.files_analyzed, 2);
        assert!(report.total_redundant >= 2, "至少有 2 个冗余包含");
        assert!(report.avg_utilization_percent < 100.0);
    }

    #[test]
    fn test_report_markdown() {
        let mut analyzer = IncludeAnalyzer::new();
        analyzer.register_header("X.h", "class XClass {};");

        let mut files = HashMap::new();
        files.insert(
            "main.cpp".to_string(),
            "#include \"X.h\"\nvoid f() {}\n".to_string(),
        );

        let report = analyzer.analyze_all(&files);
        let md = report.to_markdown();
        assert!(md.contains("# 未使用头文件分析报告"));
        assert!(md.contains("分析文件数"));
    }

    #[test]
    fn test_no_false_positive_on_used_include() {
        let mut analyzer = IncludeAnalyzer::new();
        analyzer.register_header("Math.h", "class Vector3 { float X; };");

        let source = "#include \"Math.h\"\nVector3 position;\n";
        let result = analyzer.analyze_file("test.cpp", source);

        // Math.h 被使用，不应报冗余 (除非被传递包含)
        let no_symbol_unused = result
            .redundant_includes
            .iter()
            .filter(|r| r.include_path == "Math.h" && r.reason == RedundancyReason::NoSymbolUsed)
            .count();
        assert_eq!(no_symbol_unused, 0, "使用中的包含不应被标记为 NoSymbolUsed");
    }

    #[test]
    fn test_empty_source() {
        let analyzer = IncludeAnalyzer::new();
        let result = analyzer.analyze_file("empty.cpp", "");
        assert_eq!(result.includes.len(), 0);
        assert_eq!(result.redundant_include_count, 0);
        assert_eq!(result.utilization_percent, 100.0);
    }

    #[test]
    fn test_conditional_includes_skipped() {
        let mut analyzer = IncludeAnalyzer::new();
        analyzer.register_header("Unused.h", "class Unused {};");

        let source = r#"
#ifdef DEBUG_MODE
#include "Unused.h"
#endif
"#;
        let result = analyzer.analyze_file("test.cpp", source);
        // 条件包含应被跳过 (保守策略)
        let no_sym = result
            .redundant_includes
            .iter()
            .filter(|r| r.reason == RedundancyReason::NoSymbolUsed)
            .count();
        assert_eq!(no_sym, 0, "条件包含不应被标记为未使用");
    }

    #[test]
    fn test_header_symbols_count() {
        let symbols =
            extract_symbols_from_source("class A {};\nstruct B {};\nenum C { X };\n#define D 1\n");
        assert_eq!(symbols.count(), 4);
        assert_eq!(symbols.all_symbols().len(), 4);
    }
}
