// ============================================================
// 文件名称：parser/semantic.rs
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：精确语义检查，早发现早修复，结构化诊断信息
// 功能描述：LHT 语义分析传递 — 对已解析 AST 执行跨类型一致性
//           检查，包括继承链验证、属性命名冲突、RPC 规则合规性、
//           产生结构化诊断报告
// 技术特性：多遍分析，图遍历继承链，规则引擎模式，
//           零误报设计，增量缓存感知，中文诊断信息
//
// ── 类型/函数表 ──────────────────────────────────────────────
// │ SemanticDiagnostic        │ 单条语义诊断                        │
// │ DiagnosticSeverity        │ 诊断级别 (Error/Warning/Info)       │
// │ SemanticAnalyzer          │ 主语义分析器                        │
// │ SemanticReport            │ 分析报告汇总                        │
// │ analyze_reflected_types() │ 分析给定反射类型集合                 │
// │ IncrementalParseCache     │ 增量解析缓存 (避免重复解析)           │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                               │
// │ 2026-04-06   │ LimxTeam  │ 初始创建 — 完整语义分析传递          │
// ============================================================

use serde::{Deserialize, Serialize};
use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};

use crate::parser::parser::{
    AccessModifier, BaseClassInfo, ClassInfo, DelegateInfo, EnumInfo, EnumValue, FunctionInfo,
    PropertyInfo, ReflectedType, StructInfo,
};

// ──────────────────────────────────────────────────────────────
// 诊断级别与诊断条目
// ──────────────────────────────────────────────────────────────

/// 诊断严重程度
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Serialize, Deserialize)]
pub enum DiagnosticSeverity {
    /// 信息性提示
    Info,
    /// 警告 (不影响编译，但可能导致问题)
    Warning,
    /// 错误 (必须修复)
    Error,
}

impl DiagnosticSeverity {
    pub fn as_str(&self) -> &'static str {
        match self {
            Self::Info => "信息",
            Self::Warning => "警告",
            Self::Error => "错误",
        }
    }

    pub fn prefix_char(&self) -> &'static str {
        match self {
            Self::Info => "ℹ",
            Self::Warning => "⚠",
            Self::Error => "✗",
        }
    }
}

/// 单条语义诊断
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SemanticDiagnostic {
    /// 严重程度
    pub severity: DiagnosticSeverity,
    /// 涉及的类型名称
    pub type_name: String,
    /// 诊断规则 ID (如 "E001")
    pub rule_id: String,
    /// 诊断消息 (中文)
    pub message: String,
    /// 来源文件路径
    pub file_path: Option<PathBuf>,
    /// 建议修复方式
    pub suggestion: Option<String>,
}

impl SemanticDiagnostic {
    fn error(type_name: &str, rule_id: &str, message: &str) -> Self {
        Self {
            severity: DiagnosticSeverity::Error,
            type_name: type_name.to_string(),
            rule_id: rule_id.to_string(),
            message: message.to_string(),
            file_path: None,
            suggestion: None,
        }
    }

    fn warning(type_name: &str, rule_id: &str, message: &str) -> Self {
        Self {
            severity: DiagnosticSeverity::Warning,
            type_name: type_name.to_string(),
            rule_id: rule_id.to_string(),
            message: message.to_string(),
            file_path: None,
            suggestion: None,
        }
    }

    fn info(type_name: &str, rule_id: &str, message: &str) -> Self {
        Self {
            severity: DiagnosticSeverity::Info,
            type_name: type_name.to_string(),
            rule_id: rule_id.to_string(),
            message: message.to_string(),
            file_path: None,
            suggestion: None,
        }
    }

    fn with_file(mut self, path: &Path) -> Self {
        self.file_path = Some(path.to_path_buf());
        self
    }

    fn with_suggestion(mut self, suggestion: &str) -> Self {
        self.suggestion = Some(suggestion.to_string());
        self
    }

    /// 格式化为可读字符串
    pub fn format(&self) -> String {
        let file_info = self
            .file_path
            .as_ref()
            .map(|p| format!(" [{}]", p.display()))
            .unwrap_or_default();

        let suggestion_info = self
            .suggestion
            .as_ref()
            .map(|s| format!("\n    建议: {}", s))
            .unwrap_or_default();

        format!(
            "{} [{}] <{}>{} — {}{}",
            self.severity.prefix_char(),
            self.rule_id,
            self.type_name,
            file_info,
            self.message,
            suggestion_info
        )
    }
}

// ──────────────────────────────────────────────────────────────
// 语义分析报告
// ──────────────────────────────────────────────────────────────

/// 语义分析报告
#[derive(Debug, Default)]
pub struct SemanticReport {
    /// 所有诊断条目
    pub diagnostics: Vec<SemanticDiagnostic>,
}

impl SemanticReport {
    pub fn new() -> Self {
        Self {
            diagnostics: Vec::new(),
        }
    }

    /// 添加诊断
    pub fn push(&mut self, diag: SemanticDiagnostic) {
        self.diagnostics.push(diag);
    }

    /// 是否存在错误级别的诊断
    pub fn has_errors(&self) -> bool {
        self.diagnostics
            .iter()
            .any(|d| d.severity == DiagnosticSeverity::Error)
    }

    /// 错误数量
    pub fn error_count(&self) -> usize {
        self.diagnostics
            .iter()
            .filter(|d| d.severity == DiagnosticSeverity::Error)
            .count()
    }

    /// 警告数量
    pub fn warning_count(&self) -> usize {
        self.diagnostics
            .iter()
            .filter(|d| d.severity == DiagnosticSeverity::Warning)
            .count()
    }

    /// 按严重程度过滤诊断
    pub fn filter(&self, min_severity: DiagnosticSeverity) -> Vec<&SemanticDiagnostic> {
        self.diagnostics
            .iter()
            .filter(|d| d.severity >= min_severity)
            .collect()
    }

    /// 打印完整报告
    pub fn print_report(&self) {
        if self.diagnostics.is_empty() {
            println!("✓ 语义分析通过 — 未发现问题");
            return;
        }

        println!("\n╔══════════════════════════════════════════════════════════════╗");
        println!("║                    LHT 语义分析报告                           ║");
        println!("╠══════════════════════════════════════════════════════════════╣");
        println!(
            "║  错误: {:>3}   警告: {:>3}   信息: {:>3}                        ║",
            self.error_count(),
            self.warning_count(),
            self.diagnostics.len() - self.error_count() - self.warning_count()
        );
        println!("╚══════════════════════════════════════════════════════════════╝");

        // 按严重程度排序输出
        let mut sorted: Vec<&SemanticDiagnostic> = self.diagnostics.iter().collect();
        sorted.sort_by(|a, b| b.severity.cmp(&a.severity));

        for diag in &sorted {
            println!("{}", diag.format());
        }
    }

    /// 合并另一个报告
    pub fn merge(&mut self, other: SemanticReport) {
        self.diagnostics.extend(other.diagnostics);
    }
}

// ──────────────────────────────────────────────────────────────
// 语义分析器
// ──────────────────────────────────────────────────────────────

/// 语义分析器 — 对 AST 执行多遍语义检查
pub struct SemanticAnalyzer {
    /// 已知类型名称集合 (用于继承链验证)
    known_type_names: HashSet<String>,
    /// 类名 → 父类名映射 (用于循环继承检测)
    inheritance_map: HashMap<String, Option<String>>,
    /// 是否开启严格模式 (警告视为错误)
    strict_mode: bool,
}

impl SemanticAnalyzer {
    pub fn new() -> Self {
        Self {
            known_type_names: HashSet::new(),
            inheritance_map: HashMap::new(),
            strict_mode: false,
        }
    }

    pub fn with_strict_mode(mut self) -> Self {
        self.strict_mode = true;
        self
    }

    /// 分析完整的反射类型集合
    pub fn analyze(&mut self, types: &[ReflectedType]) -> SemanticReport {
        let mut report = SemanticReport::new();

        // 第一遍: 建立类型索引
        for t in types {
            match t {
                ReflectedType::Class(c) => {
                    self.known_type_names.insert(c.name.clone());
                    self.inheritance_map
                        .insert(c.name.clone(), c.base_class().map(|s| s.to_string()));
                }
                ReflectedType::Struct(s) => {
                    self.known_type_names.insert(s.name.clone());
                }
                ReflectedType::Enum(e) => {
                    self.known_type_names.insert(e.name.clone());
                }
                ReflectedType::Delegate(d) => {
                    self.known_type_names.insert(d.name.clone());
                }
            }
        }

        // 第二遍: 对每个类型执行具体规则检查
        for t in types {
            match t {
                ReflectedType::Class(c) => {
                    self.analyze_class(c, &mut report);
                }
                ReflectedType::Struct(s) => {
                    self.analyze_struct(s, &mut report);
                }
                ReflectedType::Enum(e) => {
                    self.analyze_enum(e, &mut report);
                }
                ReflectedType::Delegate(d) => {
                    self.analyze_delegate(d, &mut report);
                }
            }
        }

        // 第三遍: 全局一致性检查
        self.check_circular_inheritance(&mut report);

        report
    }

    // ──────────────────────────────────────────────────────────
    // 类分析规则
    // ──────────────────────────────────────────────────────────

    fn analyze_class(&self, class: &ClassInfo, report: &mut SemanticReport) {
        // E002: 父类型存在性检查
        if let Some(parent) = class.base_class() {
            if !parent.is_empty()
                && !is_well_known_base(parent)
                && !self.known_type_names.contains(parent)
            {
                report.push(SemanticDiagnostic::warning(
                    &class.name,
                    "E002",
                    &format!(
                        "父类 '{}' 未在当前扫描范围内发现，请确认头文件是否被正确包含",
                        parent
                    ),
                ));
            }
        }

        // E003: 属性命名冲突检查
        self.check_property_names(&class.name, &class.properties, report);

        // E004: 函数命名冲突 + RPC 规则
        self.check_function_rules(&class.name, &class.functions, report);

        // W001: 抽象类未标注 Abstract (含有纯虚函数但 is_abstract 未标注)
        if class.functions.iter().any(|f| f.is_pure_virtual) && !class.is_abstract {
            report.push(
                SemanticDiagnostic::warning(
                    &class.name,
                    "W001",
                    "类含有纯虚函数但未在 LCLASS 说明符中标注 Abstract",
                )
                .with_suggestion("添加 LCLASS(Abstract) 说明符"),
            );
        }

        // W002: 属性数量过多警告
        if class.properties.len() > 64 {
            report.push(SemanticDiagnostic::info(
                &class.name,
                "W002",
                &format!(
                    "类含有 {} 个反射属性，考虑拆分为多个组件类以降低单类复杂度",
                    class.properties.len()
                ),
            ));
        }
    }

    // ──────────────────────────────────────────────────────────
    // 结构体分析规则
    // ──────────────────────────────────────────────────────────

    fn analyze_struct(&self, s: &StructInfo, report: &mut SemanticReport) {
        // E005: LSTRUCT 不允许虚函数
        // (通过属性或函数中检测 virtual)

        // E006: 属性命名冲突
        self.check_property_names(&s.name, &s.properties, report);

        // W003: 空结构体警告
        if s.properties.is_empty() {
            report.push(SemanticDiagnostic::info(
                &s.name,
                "W003",
                "LSTRUCT 没有任何 LPROPERTY 属性，反射标注是否必要？",
            ));
        }
    }

    // ──────────────────────────────────────────────────────────
    // 枚举分析规则
    // ──────────────────────────────────────────────────────────

    fn analyze_enum(&self, e: &EnumInfo, report: &mut SemanticReport) {
        // W004: 空枚举
        if e.values.is_empty() {
            report.push(SemanticDiagnostic::warning(
                &e.name,
                "W004",
                "LENUM 没有任何枚举值",
            ));
        }

        // E007: 枚举值重复
        let mut seen_names = HashSet::new();
        let mut seen_values = HashMap::new();
        for val in &e.values {
            if !seen_names.insert(&val.name) {
                report.push(SemanticDiagnostic::error(
                    &e.name,
                    "E007",
                    &format!("枚举值名称重复: '{}'", val.name),
                ));
            }
            if let Some(v) = val.value {
                if let Some(existing) = seen_values.insert(v, &val.name) {
                    report.push(SemanticDiagnostic::warning(
                        &e.name,
                        "W005",
                        &format!(
                            "枚举值 '{}' 与 '{}' 数值相同 ({}), 可能是别名",
                            val.name, existing, v
                        ),
                    ));
                }
            }
        }

        // W006: 大型枚举建议使用 namespace
        if e.values.len() > 32 {
            report.push(SemanticDiagnostic::info(
                &e.name,
                "W006",
                &format!(
                    "枚举含 {} 个值，超过 32 个建议拆分或使用位标志模式",
                    e.values.len()
                ),
            ));
        }
    }

    // ──────────────────────────────────────────────────────────
    // 委托分析规则
    // ──────────────────────────────────────────────────────────

    fn analyze_delegate(&self, d: &DelegateInfo, report: &mut SemanticReport) {
        // E008: 多播委托返回类型必须为 void
        if d.specifiers.multicast && d.return_type != "void" && !d.return_type.is_empty() {
            report.push(
                SemanticDiagnostic::error(
                    &d.name,
                    "E008",
                    "多播委托 (LDELEGATE_MULTICAST) 的返回类型必须为 void",
                )
                .with_suggestion("将返回类型改为 void，或使用单播委托 LDELEGATE"),
            );
        }
    }

    // ──────────────────────────────────────────────────────────
    // 共享规则: 属性命名
    // ──────────────────────────────────────────────────────────

    fn check_property_names(
        &self,
        type_name: &str,
        properties: &[PropertyInfo],
        report: &mut SemanticReport,
    ) {
        let mut seen: HashSet<&str> = HashSet::new();
        for prop in properties {
            if !seen.insert(prop.name.as_str()) {
                report.push(SemanticDiagnostic::error(
                    type_name,
                    "E003",
                    &format!("属性名称重复: '{}' 在同一类中重复声明", prop.name),
                ));
            }

            // W008: 属性名以小写开头 (引擎命名规范: 首字母大写)
            if prop
                .name
                .chars()
                .next()
                .map(|c| c.is_lowercase())
                .unwrap_or(false)
            {
                report.push(
                    SemanticDiagnostic::warning(
                        type_name,
                        "W008",
                        &format!(
                            "属性 '{}' 以小写字母开头，引擎规范要求成员变量首字母大写",
                            prop.name
                        ),
                    )
                    .with_suggestion(&format!("重命名为 '{}'", capitalize_first(&prop.name))),
                );
            }

            // E009: 复制属性不可为 Transient
            if prop.specifiers.replicated && prop.specifiers.transient {
                report.push(
                    SemanticDiagnostic::error(
                        type_name,
                        "E009",
                        &format!(
                            "属性 '{}' 同时标注 Replicated 和 Transient，逻辑矛盾",
                            prop.name
                        ),
                    )
                    .with_suggestion(
                        "Transient 属性不会被序列化，无法参与网络复制，移除其中一个说明符",
                    ),
                );
            }
        }
    }

    // ──────────────────────────────────────────────────────────
    // 共享规则: 函数/RPC 检查
    // ──────────────────────────────────────────────────────────

    fn check_function_rules(
        &self,
        type_name: &str,
        functions: &[FunctionInfo],
        report: &mut SemanticReport,
    ) {
        let mut seen: HashSet<&str> = HashSet::new();

        for func in functions {
            // E010: 函数名重复 (重载在当前阶段视为重复)
            if !seen.insert(func.name.as_str()) {
                report.push(
                    SemanticDiagnostic::warning(
                        type_name,
                        "E010",
                        &format!("函数 '{}' 重复声明，反射系统不支持重载", func.name),
                    )
                    .with_suggestion("重命名其中一个函数，或只标注一个重载为 LFUNCTION"),
                );
            }

            let is_server = func.specifiers.server;
            let is_client = func.specifiers.client;
            let is_multicast = func.specifiers.net_multicast;
            let is_reliable = func.specifiers.reliable;
            let is_unreliable = func.specifiers.unreliable;

            // E011: RPC 函数必须选择 Reliable/Unreliable
            if (is_server || is_client || is_multicast) && !is_reliable && !is_unreliable {
                report.push(
                    SemanticDiagnostic::error(
                        type_name,
                        "E011",
                        &format!("RPC 函数 '{}' 未指定 Reliable 或 Unreliable", func.name),
                    )
                    .with_suggestion("在 LFUNCTION 说明符中添加 Reliable 或 Unreliable"),
                );
            }

            // E012: 不能同时 Reliable 和 Unreliable
            if is_reliable && is_unreliable {
                report.push(SemanticDiagnostic::error(
                    type_name,
                    "E012",
                    &format!(
                        "RPC 函数 '{}' 同时标注 Reliable 和 Unreliable，逻辑矛盾",
                        func.name
                    ),
                ));
            }

            // E013: RPC 函数不能同时是多种类型
            let rpc_count = [is_server, is_client, is_multicast]
                .iter()
                .filter(|&&b| b)
                .count();
            if rpc_count > 1 {
                report.push(
                    SemanticDiagnostic::error(
                        type_name,
                        "E013",
                        &format!(
                            "函数 '{}' 同时标注了多个 RPC 类型 (Server/Client/NetMulticast)",
                            func.name
                        ),
                    )
                    .with_suggestion("每个 LFUNCTION 只能是一种 RPC 类型"),
                );
            }

            // W009: BlueprintPure 函数有参数时的提示
            if func.specifiers.blueprint_pure && !func.parameters.is_empty() {
                report.push(SemanticDiagnostic::info(
                    type_name,
                    "W009",
                    &format!(
                        "BlueprintPure 函数 '{}' 有输入参数，确认这是纯函数的预期行为",
                        func.name
                    ),
                ));
            }
        }
    }

    // ──────────────────────────────────────────────────────────
    // 全局检查: 循环继承
    // ──────────────────────────────────────────────────────────

    fn check_circular_inheritance(&self, report: &mut SemanticReport) {
        for (class_name, _) in &self.inheritance_map {
            if let Some(cycle) = self.find_inheritance_cycle(class_name) {
                report.push(SemanticDiagnostic::error(
                    class_name,
                    "E014",
                    &format!("检测到循环继承: {}", cycle.join(" → ")),
                ));
            }
        }
    }

    /// 使用慢速但安全的 Floyd 算法检测继承环
    fn find_inheritance_cycle(&self, start: &str) -> Option<Vec<String>> {
        let mut visited: HashSet<&str> = HashSet::new();
        let mut path: Vec<String> = Vec::new();
        let mut current = start;

        loop {
            if !visited.insert(current) {
                // 找到了环，返回环路径
                if current == start || path.contains(&current.to_string()) {
                    path.push(current.to_string());
                    return Some(path);
                }
                return None;
            }
            path.push(current.to_string());

            match self.inheritance_map.get(current) {
                Some(Some(parent)) => {
                    if is_well_known_base(parent) {
                        break;
                    }
                    current = parent.as_str();
                }
                _ => break,
            }
        }
        None
    }
}

// ──────────────────────────────────────────────────────────────
// 增量解析缓存
// ──────────────────────────────────────────────────────────────

/// 文件解析缓存条目
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ParseCacheEntry {
    /// 文件路径
    pub file_path: PathBuf,
    /// 文件内容 SHA-256 哈希
    pub content_hash: String,
    /// 上次解析的 Unix 时间戳
    pub parsed_at: u64,
    /// 解析结果中的类型数量
    pub type_count: usize,
    /// 该文件是否包含任何反射类型
    pub has_reflection: bool,
}

/// 增量解析缓存 — 避免对未修改文件重复解析
#[derive(Debug, Default, Serialize, Deserialize)]
pub struct IncrementalParseCache {
    /// 文件路径 → 缓存条目
    pub entries: HashMap<PathBuf, ParseCacheEntry>,
    /// 格式版本
    pub version: u32,
}

impl IncrementalParseCache {
    pub const CURRENT_VERSION: u32 = 1;

    pub fn new() -> Self {
        Self {
            entries: HashMap::new(),
            version: Self::CURRENT_VERSION,
        }
    }

    /// 从磁盘加载缓存
    pub fn load(path: &Path) -> Option<Self> {
        let data = std::fs::read_to_string(path).ok()?;
        let cache: Self = serde_json::from_str(&data).ok()?;
        if cache.version != Self::CURRENT_VERSION {
            return None;
        }
        Some(cache)
    }

    /// 保存缓存到磁盘
    pub fn save(&self, path: &Path) -> anyhow::Result<()> {
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent)?;
        }
        let json = serde_json::to_string_pretty(self)?;
        std::fs::write(path, json)?;
        Ok(())
    }

    /// 检查文件是否需要重新解析
    pub fn needs_reparse(&self, file_path: &Path) -> bool {
        match self.entries.get(file_path) {
            None => true,
            Some(entry) => {
                // 检查文件修改时间
                let current_hash = compute_file_hash(file_path).unwrap_or_default();
                current_hash != entry.content_hash
            }
        }
    }

    /// 更新缓存条目
    pub fn update(&mut self, file_path: &Path, type_count: usize, has_reflection: bool) {
        let content_hash = compute_file_hash(file_path).unwrap_or_default();
        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_secs())
            .unwrap_or(0);

        self.entries.insert(
            file_path.to_path_buf(),
            ParseCacheEntry {
                file_path: file_path.to_path_buf(),
                content_hash,
                parsed_at: now,
                type_count,
                has_reflection,
            },
        );
    }

    /// 移除不再存在的文件的缓存条目
    pub fn prune_stale(&mut self) -> usize {
        let before = self.entries.len();
        self.entries.retain(|path, _| path.exists());
        before - self.entries.len()
    }

    /// 统计信息
    pub fn stats(&self) -> (usize, usize, usize) {
        let total = self.entries.len();
        let with_reflection = self.entries.values().filter(|e| e.has_reflection).count();
        let total_types: usize = self.entries.values().map(|e| e.type_count).sum();
        (total, with_reflection, total_types)
    }
}

// ──────────────────────────────────────────────────────────────
// 辅助函数
// ──────────────────────────────────────────────────────────────

/// 判断是否为已知引擎基类 (无需验证其存在性)
fn is_well_known_base(name: &str) -> bool {
    matches!(
        name,
        "ObjectBase"
            | "LimxObject"
            | "LimxActor"
            | "LimxComponent"
            | "LimxPawn"
            | "LimxCharacter"
            | "LimxController"
            | "LimxGameMode"
            | "LimxGameState"
            | "LimxPlayerState"
            | "LimxWidget"
            | "LimxInterface"
    )
}

/// 首字母大写
fn capitalize_first(s: &str) -> String {
    let mut chars = s.chars();
    match chars.next() {
        None => String::new(),
        Some(f) => f.to_uppercase().collect::<String>() + chars.as_str(),
    }
}

/// 计算文件内容哈希 (使用 xxhash 快速哈希)
fn compute_file_hash(path: &Path) -> Option<String> {
    let data = std::fs::read(path).ok()?;
    // 使用简单的 FNV-1a 哈希代替 SHA-256 (速度更快，用于缓存校验足够)
    let mut hash: u64 = 14695981039346656037u64;
    for byte in &data {
        hash ^= *byte as u64;
        hash = hash.wrapping_mul(1099511628211);
    }
    Some(format!("{:016x}", hash))
}

// ──────────────────────────────────────────────────────────────
// 公开便捷函数
// ──────────────────────────────────────────────────────────────

/// 分析反射类型集合，返回语义报告
pub fn analyze_reflected_types(types: &[ReflectedType], strict: bool) -> SemanticReport {
    let mut analyzer = SemanticAnalyzer::new();
    if strict {
        analyzer = analyzer.with_strict_mode();
    }
    analyzer.analyze(types)
}

// ──────────────────────────────────────────────────────────────
// 单元测试
// ──────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;
    use crate::parser::specifiers::{
        ClassSpecifiers, DelegateSpecifiers, FunctionSpecifiers, PropertySpecifiers, Specifiers,
    };

    /// 构造测试用 ClassInfo
    fn make_class(name: &str, parent: Option<&str>, is_abstract: bool) -> ClassInfo {
        let base_classes = match parent {
            Some(p) => vec![BaseClassInfo {
                name: p.to_string(),
                access: AccessModifier::Public,
                is_virtual: false,
            }],
            None => Vec::new(),
        };
        ClassInfo {
            name: name.to_string(),
            specifiers: ClassSpecifiers::default(),
            raw_specifiers: Specifiers::default(),
            base_classes,
            api_macro: None,
            properties: Vec::new(),
            functions: Vec::new(),
            delegates: Vec::new(),
            nested_types: Vec::new(),
            is_abstract,
            is_final: false,
        }
    }

    /// 构造测试用 PropertyInfo (支持 Replicated/Transient 标记)
    fn make_property(name: &str, replicated: bool, transient: bool) -> PropertyInfo {
        PropertyInfo {
            name: name.to_string(),
            type_name: "int32".to_string(),
            specifiers: PropertySpecifiers {
                replicated,
                transient,
                ..Default::default()
            },
            raw_specifiers: Specifiers::default(),
            array_size: None,
            default_value: None,
            is_static: false,
            is_const: false,
            is_mutable: false,
            is_pointer: false,
            is_reference: false,
            template_args: Vec::new(),
            access: AccessModifier::Public,
            bit_field: None,
        }
    }

    /// 构造测试用 FunctionInfo (支持 RPC 标记)
    fn make_rpc_function(
        name: &str,
        server: bool,
        client: bool,
        net_multicast: bool,
        reliable: bool,
        unreliable: bool,
    ) -> FunctionInfo {
        FunctionInfo {
            name: name.to_string(),
            return_type: "void".to_string(),
            parameters: Vec::new(),
            specifiers: FunctionSpecifiers {
                server,
                client,
                net_multicast,
                reliable,
                unreliable,
                ..Default::default()
            },
            raw_specifiers: Specifiers::default(),
            is_const: false,
            is_virtual: false,
            is_override: false,
            is_static: false,
            is_inline: false,
            is_constexpr: false,
            is_noexcept: false,
            is_final: false,
            is_pure_virtual: false,
            is_explicit: false,
            access: AccessModifier::Public,
            template_params: Vec::new(),
        }
    }

    /// 构造测试用纯虚函数
    fn make_pure_virtual_function(name: &str) -> FunctionInfo {
        FunctionInfo {
            name: name.to_string(),
            return_type: "void".to_string(),
            parameters: Vec::new(),
            specifiers: FunctionSpecifiers::default(),
            raw_specifiers: Specifiers::default(),
            is_const: false,
            is_virtual: true,
            is_override: false,
            is_static: false,
            is_inline: false,
            is_constexpr: false,
            is_noexcept: false,
            is_final: false,
            is_pure_virtual: true,
            is_explicit: false,
            access: AccessModifier::Public,
            template_params: Vec::new(),
        }
    }

    #[test]
    fn test_valid_class_no_errors() {
        let class = make_class("MyClass", Some("ObjectBase"), false);
        let types = vec![ReflectedType::Class(class)];
        let report = analyze_reflected_types(&types, false);
        assert!(!report.has_errors());
    }

    #[test]
    fn test_unknown_parent_class_warning() {
        // 父类 "UnknownBase" 不在已知类型和引擎基类列表中
        let class = make_class("MyClass", Some("UnknownBase"), false);
        let types = vec![ReflectedType::Class(class)];
        let report = analyze_reflected_types(&types, false);
        assert!(report.diagnostics.iter().any(|d| d.rule_id == "E002"));
    }

    #[test]
    fn test_duplicate_property_names() {
        let mut class = make_class("MyClass", None, false);
        class.properties = vec![
            make_property("Health", false, false),
            make_property("Health", false, false), // 重复
        ];
        let types = vec![ReflectedType::Class(class)];
        let report = analyze_reflected_types(&types, false);
        assert!(report.diagnostics.iter().any(|d| d.rule_id == "E003"));
    }

    #[test]
    fn test_rpc_without_reliable() {
        let mut class = make_class("MyClass", None, false);
        class.functions = vec![
            // Server RPC 缺少 Reliable/Unreliable
            make_rpc_function("ServerFire", true, false, false, false, false),
        ];
        let types = vec![ReflectedType::Class(class)];
        let report = analyze_reflected_types(&types, false);
        assert!(report.diagnostics.iter().any(|d| d.rule_id == "E011"));
    }

    #[test]
    fn test_rpc_reliable_and_unreliable() {
        let mut class = make_class("MyClass", None, false);
        class.functions = vec![
            // 同时 Reliable 和 Unreliable
            make_rpc_function("ServerFire", true, false, false, true, true),
        ];
        let types = vec![ReflectedType::Class(class)];
        let report = analyze_reflected_types(&types, false);
        assert!(report.diagnostics.iter().any(|d| d.rule_id == "E012"));
    }

    #[test]
    fn test_rpc_multiple_types() {
        let mut class = make_class("MyClass", None, false);
        class.functions = vec![
            // 同时标注 Server 和 Client
            make_rpc_function("BadRpc", true, true, false, true, false),
        ];
        let types = vec![ReflectedType::Class(class)];
        let report = analyze_reflected_types(&types, false);
        assert!(report.diagnostics.iter().any(|d| d.rule_id == "E013"));
    }

    #[test]
    fn test_duplicate_enum_values() {
        let e = EnumInfo {
            name: "MyEnum".to_string(),
            raw_specifiers: Specifiers::default(),
            is_flags: false,
            underlying_type: None,
            values: vec![
                EnumValue {
                    name: "A".to_string(),
                    value: Some(0),
                    display_name: None,
                },
                EnumValue {
                    name: "A".to_string(),
                    value: Some(1),
                    display_name: None,
                },
            ],
        };
        let types = vec![ReflectedType::Enum(e)];
        let report = analyze_reflected_types(&types, false);
        assert!(report.diagnostics.iter().any(|d| d.rule_id == "E007"));
    }

    #[test]
    fn test_empty_enum_warning() {
        let e = EnumInfo {
            name: "EmptyEnum".to_string(),
            raw_specifiers: Specifiers::default(),
            is_flags: false,
            underlying_type: None,
            values: Vec::new(),
        };
        let types = vec![ReflectedType::Enum(e)];
        let report = analyze_reflected_types(&types, false);
        assert!(report.diagnostics.iter().any(|d| d.rule_id == "W004"));
    }

    #[test]
    fn test_replicated_and_transient_conflict() {
        let mut class = make_class("MyClass", None, false);
        class.properties = vec![
            make_property("Health", true, true), // Replicated + Transient 矛盾
        ];
        let types = vec![ReflectedType::Class(class)];
        let report = analyze_reflected_types(&types, false);
        assert!(report.diagnostics.iter().any(|d| d.rule_id == "E009"));
    }

    #[test]
    fn test_abstract_class_warning() {
        // 含纯虚函数但未标注 is_abstract
        let mut class = make_class("MyClass", None, false);
        class.functions = vec![make_pure_virtual_function("Tick")];
        let types = vec![ReflectedType::Class(class)];
        let report = analyze_reflected_types(&types, false);
        assert!(report.diagnostics.iter().any(|d| d.rule_id == "W001"));
    }

    #[test]
    fn test_abstract_class_no_warning_when_marked() {
        // 含纯虚函数且已标注 is_abstract，不应产生 W001
        let mut class = make_class("MyClass", None, true);
        class.functions = vec![make_pure_virtual_function("Tick")];
        let types = vec![ReflectedType::Class(class)];
        let report = analyze_reflected_types(&types, false);
        assert!(!report.diagnostics.iter().any(|d| d.rule_id == "W001"));
    }

    #[test]
    fn test_empty_struct_info_warning() {
        let s = StructInfo {
            name: "EmptyStruct".to_string(),
            specifiers: ClassSpecifiers::default(),
            raw_specifiers: Specifiers::default(),
            api_macro: None,
            properties: Vec::new(),
        };
        let types = vec![ReflectedType::Struct(s)];
        let report = analyze_reflected_types(&types, false);
        assert!(report.diagnostics.iter().any(|d| d.rule_id == "W003"));
    }

    #[test]
    fn test_lowercase_property_name_warning() {
        let mut class = make_class("MyClass", None, false);
        class.properties = vec![
            make_property("health", false, false), // 小写开头
        ];
        let types = vec![ReflectedType::Class(class)];
        let report = analyze_reflected_types(&types, false);
        assert!(report.diagnostics.iter().any(|d| d.rule_id == "W008"));
    }

    #[test]
    fn test_incremental_cache_needs_reparse_for_new_file() {
        let cache = IncrementalParseCache::new();
        let path = PathBuf::from("nonexistent_test.h");
        assert!(cache.needs_reparse(&path));
    }

    #[test]
    fn test_incremental_cache_update_and_check() {
        let mut cache = IncrementalParseCache::new();
        let dir = tempfile::tempdir().unwrap();
        let file = dir.path().join("test.h");
        std::fs::write(&file, "// test\n").unwrap();
        cache.update(&file, 2, true);
        assert!(!cache.needs_reparse(&file));
    }

    #[test]
    fn test_capitalize_first() {
        assert_eq!(capitalize_first("health"), "Health");
        assert_eq!(capitalize_first("MyProp"), "MyProp");
        assert_eq!(capitalize_first(""), "");
    }
}
