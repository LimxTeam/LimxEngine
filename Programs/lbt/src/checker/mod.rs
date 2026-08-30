/*******************************************************************************
 * 文件: checker/mod.rs
 * 创建时间: 2026-04-07
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   Limx 源码静态检查器 — 类似 `cargo check` 的快速 lint 工具
 *   扫描 C++ 源文件，检测违反 Limx Engine 编码规范的模式：
 *   - 禁止 STL 头文件 / std:: 命名空间
 *   - 禁止裸 new/delete
 *   - 禁止 C 标准库类型 (int/unsigned/size_t/uint32_t)
 *   - 禁止 C 内存函数 (memcpy/memset/memcmp)
 *   - 空指针解引用风险检测
 *   - 缺少文件头注释检测
 *
 * 技术特性:
 *   - 基于正则表达式的快速行级扫描 (无需完整 AST)
 *   - 并行文件处理 (rayon)
 *   - 结构化诊断输出 (文件:行:列 + 规则 ID + 严重级别)
 *   - 可选 MSVC /analyze + /Zs 深度分析模式
 *
 ******************************************************************************/

use anyhow::{Context, Result};
use rayon::prelude::*;
use regex::Regex;
use std::fs;
use std::path::{Path, PathBuf};

// ============================================================================
// 诊断结构体
// ============================================================================

/// 检查严重级别
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CheckSeverity {
    /// 错误 — 必须修复
    Error,
    /// 警告 — 强烈建议修复
    Warning,
    /// 提示 — 可选改进
    Note,
}

impl std::fmt::Display for CheckSeverity {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            CheckSeverity::Error => write!(f, "error"),
            CheckSeverity::Warning => write!(f, "warning"),
            CheckSeverity::Note => write!(f, "note"),
        }
    }
}

/// 单条检查诊断
#[derive(Debug, Clone)]
pub struct CheckDiagnostic {
    /// 规则 ID (如 "limx-no-stl")
    pub rule_id: &'static str,
    /// 严重级别
    pub severity: CheckSeverity,
    /// 消息
    pub message: String,
    /// 文件路径
    pub file: PathBuf,
    /// 行号 (1-indexed)
    pub line: usize,
    /// 匹配的源码片段
    pub snippet: String,
}

impl std::fmt::Display for CheckDiagnostic {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "{}({}): {} [{}]: {}",
            self.file.display(),
            self.line,
            self.severity,
            self.rule_id,
            self.message,
        )
    }
}

/// 检查结果汇总
#[derive(Debug, Default)]
pub struct CheckReport {
    pub diagnostics: Vec<CheckDiagnostic>,
    pub files_checked: usize,
    pub lines_checked: usize,
}

impl CheckReport {
    pub fn error_count(&self) -> usize {
        self.diagnostics
            .iter()
            .filter(|d| d.severity == CheckSeverity::Error)
            .count()
    }

    pub fn warning_count(&self) -> usize {
        self.diagnostics
            .iter()
            .filter(|d| d.severity == CheckSeverity::Warning)
            .count()
    }

    pub fn note_count(&self) -> usize {
        self.diagnostics
            .iter()
            .filter(|d| d.severity == CheckSeverity::Note)
            .count()
    }

    pub fn is_clean(&self) -> bool {
        self.error_count() == 0 && self.warning_count() == 0
    }
}

// ============================================================================
// Lint 规则
// ============================================================================

/// 单条 lint 规则
struct LintRule {
    /// 规则 ID
    id: &'static str,
    /// 严重级别
    severity: CheckSeverity,
    /// 描述
    description: &'static str,
    /// 正则匹配模式
    pattern: Regex,
    /// 是否仅在 #include 行检查
    include_only: bool,
    /// 在注释行中是否跳过
    skip_in_comments: bool,
    /// 排除模式 — 匹配此正则的行不报告 (替代 look-behind)
    exclude_pattern: Option<Regex>,
    /// 文件级豁免 — 这些文件是封装层本身，允许使用底层 API
    exempt_files: Vec<&'static str>,
}

/// 检查文件路径是否匹配豁免列表中的任一后缀
fn is_file_exempt(file: &Path, exempt_files: &[&str]) -> bool {
    let file_str = file.to_string_lossy();
    // 统一路径分隔符为 /
    let normalized = file_str.replace('\\', "/");
    exempt_files
        .iter()
        .any(|suffix| normalized.ends_with(suffix))
}

/// 构建所有 Limx lint 规则
fn build_lint_rules() -> Vec<LintRule> {
    vec![
        // ================================================================
        // 禁止 STL 头文件
        // ================================================================
        LintRule {
            id: "limx-no-stl",
            severity: CheckSeverity::Error,
            description: "禁止包含 STL 头文件 — 使用 Core 模块替代",
            pattern: Regex::new(
                r#"#\s*include\s*<\s*(vector|string|map|unordered_map|set|unordered_set|list|deque|queue|stack|array|bitset|memory|functional|optional|variant|any|tuple|algorithm|numeric|iterator|ranges|span|format|source_location|expected|print|stacktrace|mdspan)\s*>"#
            ).unwrap(),
            include_only: true,
            skip_in_comments: true,
            exclude_pattern: None,
            exempt_files: vec![],
        },

        // ================================================================
        // 禁止 C 标准库头文件
        // ================================================================
        LintRule {
            id: "limx-no-cstdlib",
            severity: CheckSeverity::Error,
            description: "禁止包含 C 标准库头文件 — 使用 Core 模块替代",
            pattern: Regex::new(
                r#"#\s*include\s*<\s*(cstdint|cstring|cmath|cstdio|cstdlib|cassert|climits|cfloat|cinttypes|cstddef|ctime|cstdarg)\s*>"#
            ).unwrap(),
            include_only: true,
            skip_in_comments: true,
            exclude_pattern: None,
            exempt_files: vec![],
        },

        // ================================================================
        // 禁止 std:: 命名空间
        // ================================================================
        LintRule {
            id: "limx-no-std-ns",
            severity: CheckSeverity::Error,
            description: "禁止使用 std:: 命名空间 — 使用 Limx 类型替代",
            pattern: Regex::new(
                r"\bstd::(vector|string|map|unordered_map|set|array|unique_ptr|shared_ptr|weak_ptr|optional|variant|function|pair|tuple|atomic|mutex|thread|future|sort|find|min|max|clamp|move|forward|swap|begin|end|size_t|string_view|span|format|cout|cerr|endl|printf|memcpy|memset|memmove)\b"
            ).unwrap(),
            include_only: false,
            skip_in_comments: true,
            exclude_pattern: None,
            exempt_files: vec![],
        },

        // ================================================================
        // 禁止裸 new/delete
        // ================================================================
        LintRule {
            id: "limx-no-raw-new",
            severity: CheckSeverity::Error,
            description: "禁止裸 new — 使用 MakeUnique/MakeShared/分配器",
            // 匹配 `new TypeName` — 排除 placement new / operator new
            pattern: Regex::new(
                r"\bnew\s+[A-Z]\w*"
            ).unwrap(),
            include_only: false,
            skip_in_comments: true,
            // 排除 placement new、operator new、MakeUnique/MakeShared 内部
            exclude_pattern: Some(Regex::new(
                r"(placement|operator\s+new|MakeUnique|MakeShared|::new)"
            ).unwrap()),
            // 无豁免。
            //
            // 这份清单原有八个文件, 但其中只有 FThread.h 与 TRefCounted.h
            // 真的含裸 new/delete, 其余六个早已改用分配器, 豁免只是没人
            // 撤。第四周把那两处也改掉了 (FThread 用 TUniquePtr 表达跨线程
            // 所有权转移, TRefCounted::Delete 走默认分配器), 于是整份清单
            // 一起清空 —— 规则从此对全工程生效。
            //
            // 分配器与智能指针自身的实现走 placement new + 显式析构, 由
            // exclude_pattern 排除, 不需要按文件豁免。
            exempt_files: vec![],
        },
        LintRule {
            id: "limx-no-raw-delete",
            severity: CheckSeverity::Error,
            description: "禁止裸 delete — 使用 RAII 智能指针",
            pattern: Regex::new(
                r"\bdelete\s+\w+"
            ).unwrap(),
            include_only: false,
            skip_in_comments: true,
            exclude_pattern: None,
            // 无豁免 —— 理由同 limx-no-raw-new
            exempt_files: vec![],
        },

        // ================================================================
        // 禁止 C 基本类型
        // ================================================================
        LintRule {
            id: "limx-no-c-types",
            severity: CheckSeverity::Warning,
            description: "禁止 C 基本类型 — 使用 Int32/UInt32/Float32/SizeType 等",
            pattern: Regex::new(
                r"\b(size_t|ptrdiff_t|intptr_t|uintptr_t|int8_t|int16_t|int32_t|int64_t|uint8_t|uint16_t|uint32_t|uint64_t)\s+[a-zA-Z_]\w*"
            ).unwrap(),
            include_only: false,
            skip_in_comments: true,
            // 排除 extern "C" 块和 Win32 API 前向声明
            exclude_pattern: Some(Regex::new(
                r#"(extern\s+"C"|__stdcall|__cdecl|WINAPI|typedef)"#
            ).unwrap()),
            exempt_files: vec![],
        },

        // ================================================================
        // 禁止 C 内存函数
        // ================================================================
        LintRule {
            id: "limx-no-c-memfn",
            severity: CheckSeverity::Error,
            description: "禁止 C 内存函数 — 使用 MemCopy/MemSet/MemZero/MemMove",
            pattern: Regex::new(
                r"\b(memcpy|memset|memmove|memcmp|malloc|calloc|realloc|free)\s*\("
            ).unwrap(),
            include_only: false,
            skip_in_comments: true,
            // 排除 Mem 命名空间封装内部 (MemoryOps.h 等)
            exclude_pattern: Some(Regex::new(
                r"(MemCopy|MemSet|MemZero|MemMove|__builtin_|intrinsic)"
            ).unwrap()),
            // MemoryOps.h 和 DefaultAllocator.h 是封装层本身
            exempt_files: vec![
                "Memory/MemoryOps.h",
                "Memory/DefaultAllocator.h",
                "Memory/IAllocator.h",
                "HAL/FPlatformFile.h",
                "HAL/FPlatformMemory.h",
            ],
        },

        // ================================================================
        // 禁止 C 数学函数
        // ================================================================
        LintRule {
            id: "limx-no-c-math",
            severity: CheckSeverity::Warning,
            description: "禁止裸 C 数学函数 — 使用 FMath:: 命名空间",
            pattern: Regex::new(
                r"\b(sqrtf?|sinf?|cosf?|tanf?|atan2f?|powf?|logf?|log2f?|expf?|fabsf?|floorf?|ceilf?|roundf?|fmodf?)\s*\("
            ).unwrap(),
            include_only: false,
            skip_in_comments: true,
            // 排除 FMath:: 封装内部和 intrinsic
            exclude_pattern: Some(Regex::new(
                r"(FMath::|__builtin_|_mm_|intrinsic|static\s+constexpr)"
            ).unwrap()),
            // FMath.h 是封装层本身
            exempt_files: vec![
                "Math/FMath.h",
                "Math/FVector.h",
                "Math/FMatrix.h",
                "Math/FQuat.h",
                "Math/FAngle.h",
                "Math/FColor.h",
                "Math/FColorHSL.h",
                "Math/FTransform.h",
            ],
        },

        // ================================================================
        // 空指针解引用风险
        // ================================================================
        LintRule {
            id: "limx-null-deref-risk",
            severity: CheckSeverity::Note,
            description: "直接解引用裸指针 — 建议在解引用前用 LIMX_CHECK/IsValid() 验证",
            // 匹配 `变量->成员` 模式 (小写开头变量名, 非 this/m_ 成员)
            pattern: Regex::new(
                r"\b[a-z_][a-zA-Z0-9_]*\s*->\s*[a-zA-Z_]"
            ).unwrap(),
            include_only: false,
            skip_in_comments: true,
            // 排除: 已有 LIMX_CHECK/IsValid/LIMX_ASSERT, this->, 行内 if 条件
            exclude_pattern: Some(Regex::new(
                r"(LIMX_CHECK|LIMX_ASSERT|LIMX_VERIFY|LIMX_ENSURE|IsValid\s*\(|this->|if\s*\(|while\s*\(|&&|\|\|)"
            ).unwrap()),
            // 智能指针/内存/RHI 内部允许裸指针操作
            exempt_files: vec![
                "Templates/TUniquePtr.h",
                "Templates/TSharedPtr.h",
                "Templates/TObjectPtr.h",
                "Memory/IAllocator.h",
                "Memory/DefaultAllocator.h",
                "Platform/Vulkan/",
            ],
        },

        // ================================================================
        // 缺少文件头注释
        // ================================================================
        LintRule {
            id: "limx-file-header",
            severity: CheckSeverity::Note,
            description: "文件应包含标准头部注释块",
            pattern: Regex::new(r"^$").unwrap(), // 占位，由特殊逻辑处理
            include_only: false,
            skip_in_comments: false,
            exclude_pattern: None,
            exempt_files: vec![],
        },
    ]
}

// ============================================================================
// 检查器核心
// ============================================================================

/// Limx 源码检查器
pub struct LimxChecker {
    rules: Vec<LintRule>,
}

impl LimxChecker {
    pub fn new() -> Self {
        Self {
            rules: build_lint_rules(),
        }
    }

    /// 扫描目录下所有 C++ 源文件
    pub fn check_directory(&self, source_dir: &Path) -> Result<CheckReport> {
        // 收集所有 .h / .cpp 文件
        let files = collect_source_files(source_dir)?;

        // 并行检查
        let results: Vec<(Vec<CheckDiagnostic>, usize)> = files
            .par_iter()
            .filter_map(|file| match self.check_file(file) {
                Ok(result) => Some(result),
                Err(e) => {
                    eprintln!("  跳过 {}: {}", file.display(), e);
                    None
                }
            })
            .collect();

        let mut report = CheckReport {
            diagnostics: Vec::new(),
            files_checked: results.len(),
            lines_checked: 0,
        };

        for (diags, line_count) in results {
            report.diagnostics.extend(diags);
            report.lines_checked += line_count;
        }

        // 按文件路径 + 行号排序
        report
            .diagnostics
            .sort_by(|a, b| a.file.cmp(&b.file).then(a.line.cmp(&b.line)));

        Ok(report)
    }

    /// 检查单个文件
    fn check_file(&self, file: &Path) -> Result<(Vec<CheckDiagnostic>, usize)> {
        let content = fs::read_to_string(file)
            .with_context(|| format!("读取文件失败: {}", file.display()))?;

        let lines: Vec<&str> = content.lines().collect();
        let line_count = lines.len();
        let mut diagnostics = Vec::new();

        // 文件头检查 — 第一行应是 // 或 /*
        if !lines.is_empty() {
            let first_line = lines[0].trim();
            if !first_line.starts_with("//") && !first_line.starts_with("/*") {
                diagnostics.push(CheckDiagnostic {
                    rule_id: "limx-file-header",
                    severity: CheckSeverity::Note,
                    message: "文件缺少标准头部注释块".to_string(),
                    file: file.to_path_buf(),
                    line: 1,
                    snippet: first_line.to_string(),
                });
            }
        }

        // 逐行规则扫描
        let mut in_block_comment = false;

        for (line_idx, line) in lines.iter().enumerate() {
            let line_num = line_idx + 1;

            // 先剥 BOM 再 trim。
            //
            // str::trim 按 Unicode White_Space 判定, 而 U+FEFF 不在其中,
            // 于是带 BOM 的文件第一行 trim 完仍以 BOM 开头, 下面的
            // starts_with("/*") 失配 —— 块注释状态永远进不去, 整段文件头
            // 注释就被当成代码扫。症状是文件头里凡提到 std:: 之类的散文
            // 全部报错, 而同一份内容去掉 BOM 就干净。
            let trimmed = line.trim_start_matches('\u{FEFF}').trim();

            // NOLINT 行内抑制 — 支持 // NOLINT 和 // NOLINTNEXTLINE
            if trimmed.contains("// NOLINT") {
                continue;
            }

            // 块注释追踪
            if in_block_comment {
                if trimmed.contains("*/") {
                    in_block_comment = false;
                }
                continue;
            }
            if trimmed.starts_with("/*") && !trimmed.contains("*/") {
                in_block_comment = true;
                continue;
            }

            // 跳过行注释
            let is_comment = trimmed.starts_with("//");

            for rule in &self.rules {
                // 文件头规则已单独处理
                if rule.id == "limx-file-header" {
                    continue;
                }

                // 跳过注释
                if rule.skip_in_comments && is_comment {
                    continue;
                }

                // include-only 规则只检查 #include 行
                if rule.include_only && !trimmed.starts_with('#') {
                    continue;
                }

                // 文件级豁免检查
                if !rule.exempt_files.is_empty() && is_file_exempt(file, &rule.exempt_files) {
                    continue;
                }

                // 空指针风险规则噪声太大，仅标记首次出现
                if rule.id == "limx-null-deref-risk" {
                    // 只在函数体内检查 (简化：跳过声明/定义行)
                    if trimmed.starts_with("//")
                        || trimmed.starts_with("*")
                        || trimmed.starts_with("class ")
                        || trimmed.starts_with("struct ")
                        || trimmed.starts_with("namespace ")
                        || trimmed.starts_with("template")
                        || trimmed.starts_with("virtual ")
                        || trimmed.starts_with("static ")
                        || trimmed.starts_with("using ")
                        || trimmed.starts_with("#")
                        || trimmed.starts_with("extern ")
                    {
                        continue;
                    }
                }

                if rule.pattern.is_match(trimmed) {
                    // 排除模式检查 — 替代 look-behind
                    if let Some(ref exclude) = rule.exclude_pattern {
                        if exclude.is_match(trimmed) {
                            continue;
                        }
                    }

                    // 提取匹配片段
                    let matched = rule
                        .pattern
                        .find(trimmed)
                        .map(|m| m.as_str().to_string())
                        .unwrap_or_default();

                    diagnostics.push(CheckDiagnostic {
                        rule_id: rule.id,
                        severity: rule.severity,
                        message: rule.description.to_string(),
                        file: file.to_path_buf(),
                        line: line_num,
                        snippet: matched,
                    });
                }
            }
        }

        Ok((diagnostics, line_count))
    }
}

// ============================================================================
// 工具函数
// ============================================================================

/// 递归收集目录下所有 .h / .cpp 文件
fn collect_source_files(dir: &Path) -> Result<Vec<PathBuf>> {
    let mut files = Vec::new();
    collect_files_recursive(dir, &mut files)?;
    Ok(files)
}

fn collect_files_recursive(dir: &Path, files: &mut Vec<PathBuf>) -> Result<()> {
    if !dir.is_dir() {
        return Ok(());
    }

    for entry in fs::read_dir(dir)? {
        let entry = entry?;
        let path = entry.path();

        if path.is_dir() {
            if should_skip_lint_dir(&path) {
                continue;
            }
            collect_files_recursive(&path, files)?;
        } else if let Some(ext) = path.extension() {
            let ext_str = ext.to_string_lossy().to_lowercase();
            if ext_str == "h" || ext_str == "hpp" || ext_str == "cpp" || ext_str == "cc" {
                files.push(path);
            }
        }
    }

    Ok(())
}

fn should_skip_lint_dir(path: &Path) -> bool {
    path.file_name()
        .and_then(|name| name.to_str())
        .map(|name| {
            matches!(
                name.to_ascii_lowercase().as_str(),
                "thirdparty" | "intermediate" | "binaries"
            )
        })
        .unwrap_or(false)
}
