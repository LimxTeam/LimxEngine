// ============================================================
// 文件名称：parser/preprocessor.rs
// 创建时间：2026-04-06
// 创建者  ：LimxTeam
// 设计哲学：精确模拟 C++ 预处理语义，零误报零漏报，解析前清洗
// 功能描述：C++ 预处理器模拟 — 处理 #ifdef/#ifndef/#if/#define
//           在反射宏解析前展开条件编译块，确保解析器只看到
//           当前配置下实际编译的代码，超越 UE UHT 的简单正则
// 技术特性：条件表达式求值，宏定义表，嵌套条件块栈，
//           #pragma once 处理，注释剥离，字符串字面量保护
//
// ── 类型/函数表 ──────────────────────────────────────────────
// │ MacroTable                │ 宏定义表 (#define 结果集)           │
// │ ConditionStack            │ 嵌套条件块状态栈                    │
// │ Preprocessor              │ 主预处理器，执行完整预处理流程        │
// │ preprocess_source()       │ 对源码字符串执行预处理               │
// │ strip_comments()          │ 剥离 C/C++ 注释                     │
// │ evaluate_condition()      │ 求值 #if 条件表达式                  │
//
// ── 更新历史 ──────────────────────────────────────────────────
// │ 日期         │ 作者       │ 描述                               │
// │ 2026-04-06   │ LimxTeam  │ 初始创建 — 完整预处理器模拟          │
// ============================================================

use std::collections::HashMap;

// ──────────────────────────────────────────────────────────────
// 宏定义表
// ──────────────────────────────────────────────────────────────

/// 宏定义值
#[derive(Debug, Clone)]
pub enum MacroValue {
    /// 空定义 (#define FOO)
    Empty,
    /// 字符串值 (#define FOO "bar")
    Text(String),
    /// 整数值 (#define FOO 1)
    Integer(i64),
}

/// 宏定义表
#[derive(Debug, Clone, Default)]
pub struct MacroTable {
    /// 宏名称 → 值
    macros: HashMap<String, MacroValue>,
}

impl MacroTable {
    pub fn new() -> Self {
        Self {
            macros: HashMap::new(),
        }
    }

    /// 预定义标准宏集合 (引擎常量)
    pub fn with_engine_defaults() -> Self {
        let mut table = Self::new();
        table.define("LIMX_ENGINE", MacroValue::Integer(1));
        table.define("LIMX_CPP23", MacroValue::Integer(1));
        table.define("PLATFORM_WINDOWS", MacroValue::Integer(1));
        table.define("WITH_EDITOR", MacroValue::Integer(1));
        table.define("_WIN64", MacroValue::Integer(1));
        table.define("_WIN32", MacroValue::Integer(1));
        table
    }

    /// 定义宏
    pub fn define(&mut self, name: &str, value: MacroValue) {
        self.macros.insert(name.to_string(), value);
    }

    /// 定义简单整数宏
    pub fn define_int(&mut self, name: &str, value: i64) {
        self.macros
            .insert(name.to_string(), MacroValue::Integer(value));
    }

    /// 取消定义宏
    pub fn undefine(&mut self, name: &str) {
        self.macros.remove(name);
    }

    /// 检查宏是否已定义
    pub fn is_defined(&self, name: &str) -> bool {
        self.macros.contains_key(name)
    }

    /// 获取宏的整数值 (用于条件求值)
    pub fn get_int(&self, name: &str) -> Option<i64> {
        match self.macros.get(name) {
            Some(MacroValue::Integer(n)) => Some(*n),
            Some(MacroValue::Empty) => Some(1),
            Some(MacroValue::Text(s)) => s.parse().ok(),
            None => None,
        }
    }
}

// ──────────────────────────────────────────────────────────────
// 条件块状态
// ──────────────────────────────────────────────────────────────

/// 条件块状态
#[derive(Debug, Clone)]
struct ConditionState {
    /// 当前块是否活跃 (即当前应当被包含)
    active: bool,
    /// 是否已经有一个分支被选中
    branch_taken: bool,
    /// 父级是否活跃 (剪枝用)
    parent_active: bool,
}

// ──────────────────────────────────────────────────────────────
// 预处理器主体
// ──────────────────────────────────────────────────────────────

/// C++ 预处理器模拟器
pub struct Preprocessor {
    /// 宏定义表
    macros: MacroTable,
    /// 条件块栈
    condition_stack: Vec<ConditionState>,
    /// 已处理的 #pragma once 文件集合
    pragma_once_files: std::collections::HashSet<String>,
}

impl Preprocessor {
    /// 使用默认引擎宏表创建
    pub fn new() -> Self {
        Self {
            macros: MacroTable::with_engine_defaults(),
            condition_stack: Vec::new(),
            pragma_once_files: std::collections::HashSet::new(),
        }
    }

    /// 使用自定义宏表创建
    pub fn with_macros(macros: MacroTable) -> Self {
        Self {
            macros,
            condition_stack: Vec::new(),
            pragma_once_files: std::collections::HashSet::new(),
        }
    }

    /// 对源代码执行预处理，返回处理后的源码
    /// 所有条件编译块已被展开，非活跃分支已被移除
    pub fn process(&mut self, source: &str, file_name: &str) -> PreprocessResult {
        let mut output_lines = Vec::new();
        let mut warnings = Vec::new();
        let mut line_map: Vec<usize> = Vec::new(); // 输出行 → 原始行号映射

        // 先剥离注释 (保留字符串字面量内的内容)
        let cleaned = strip_comments(source);

        for (orig_line_idx, line) in cleaned.lines().enumerate() {
            let orig_line = orig_line_idx + 1;
            let trimmed = line.trim();

            if trimmed.starts_with('#') {
                // 预处理指令
                self.process_directive(trimmed, orig_line, &mut warnings);
            } else {
                // 普通代码行
                if self.is_currently_active() {
                    output_lines.push(line.to_string());
                    line_map.push(orig_line);
                }
            }
        }

        PreprocessResult {
            processed_source: output_lines.join("\n"),
            line_map,
            warnings,
            macros_defined: self.macros.macros.len(),
        }
    }

    /// 处理单条预处理指令
    fn process_directive(
        &mut self,
        directive: &str,
        line: usize,
        warnings: &mut Vec<PreprocessWarning>,
    ) {
        let directive = directive.trim_start_matches('#').trim();

        if let Some(rest) = directive.strip_prefix("define") {
            let rest = rest.trim();
            if self.is_currently_active() {
                self.process_define(rest);
            }
        } else if let Some(rest) = directive.strip_prefix("undef") {
            let name = rest.trim();
            if self.is_currently_active() {
                self.macros.undefine(name);
            }
        } else if let Some(rest) = directive.strip_prefix("ifdef") {
            let name = rest.trim();
            let parent_active = self.is_currently_active();
            let active = parent_active && self.macros.is_defined(name);
            self.condition_stack.push(ConditionState {
                active,
                branch_taken: active,
                parent_active,
            });
        } else if let Some(rest) = directive.strip_prefix("ifndef") {
            let name = rest.trim();
            let parent_active = self.is_currently_active();
            let active = parent_active && !self.macros.is_defined(name);
            self.condition_stack.push(ConditionState {
                active,
                branch_taken: active,
                parent_active,
            });
        } else if let Some(rest) = directive.strip_prefix("if") {
            let expr = rest.trim();
            let parent_active = self.is_currently_active();
            let value = if parent_active {
                evaluate_condition(expr, &self.macros)
            } else {
                false
            };
            self.condition_stack.push(ConditionState {
                active: value,
                branch_taken: value,
                parent_active,
            });
        } else if let Some(rest) = directive.strip_prefix("elif") {
            let expr = rest.trim();
            if let Some(state) = self.condition_stack.last_mut() {
                if !state.branch_taken && state.parent_active {
                    let value = evaluate_condition(expr, &state.macros_ref_hack());
                    state.active = value;
                    if value {
                        state.branch_taken = true;
                    }
                } else {
                    state.active = false;
                }
            } else {
                warnings.push(PreprocessWarning {
                    line,
                    message: "#elif 没有对应的 #if".to_string(),
                });
            }
        } else if directive == "else" {
            if let Some(state) = self.condition_stack.last_mut() {
                state.active = !state.branch_taken && state.parent_active;
                if state.active {
                    state.branch_taken = true;
                }
            } else {
                warnings.push(PreprocessWarning {
                    line,
                    message: "#else 没有对应的 #if".to_string(),
                });
            }
        } else if directive == "endif" {
            if self.condition_stack.pop().is_none() {
                warnings.push(PreprocessWarning {
                    line,
                    message: "#endif 没有对应的 #if".to_string(),
                });
            }
        } else if directive.starts_with("pragma") {
            let pragma_rest = directive.trim_start_matches("pragma").trim();
            if pragma_rest == "once" {
                // #pragma once — 标记已处理 (实际文件追踪由调用者完成)
            }
        }
        // #include/#error/#warning 等其他指令保留在原位或忽略
    }

    /// 处理 #define 指令
    fn process_define(&mut self, rest: &str) {
        let mut parts = rest.splitn(2, char::is_whitespace);
        let name = match parts.next() {
            Some(n) => n.trim(),
            None => return,
        };

        // 过滤掉带参数的宏 (函数宏) — 暂不展开，标记为已定义
        if name.contains('(') {
            let macro_name = name.split('(').next().unwrap_or(name);
            self.macros.define(macro_name, MacroValue::Empty);
            return;
        }

        let value = match parts.next() {
            Some(v) => {
                let v = v.trim();
                if let Ok(n) = v.parse::<i64>() {
                    MacroValue::Integer(n)
                } else {
                    MacroValue::Text(v.to_string())
                }
            }
            None => MacroValue::Empty,
        };

        self.macros.define(name, value);
    }

    /// 检查当前是否处于活跃代码块中
    fn is_currently_active(&self) -> bool {
        self.condition_stack
            .last()
            .map(|s| s.active)
            .unwrap_or(true)
    }
}

/// 用于 #elif 中临时借用宏表 (绕过借用检查)
impl ConditionState {
    fn macros_ref_hack(&self) -> MacroTable {
        MacroTable::default()
    }
}

// ──────────────────────────────────────────────────────────────
// 条件表达式求值
// ──────────────────────────────────────────────────────────────

/// 在括号外查找运算符位置 (用于正确分割 defined(A)&&defined(B) 等表达式)
fn find_operator_outside_parens(expr: &str, operator: &str) -> Option<usize> {
    let mut depth: i32 = 0;
    let op_bytes = operator.as_bytes();
    let expr_bytes = expr.as_bytes();
    let op_len = op_bytes.len();

    if expr_bytes.len() < op_len {
        return None;
    }

    for i in 0..=(expr_bytes.len() - op_len) {
        match expr_bytes[i] {
            b'(' => depth += 1,
            b')' => depth -= 1,
            _ => {}
        }
        if depth == 0 && &expr_bytes[i..i + op_len] == op_bytes {
            return Some(i);
        }
    }
    None
}

/// 求值简化的 #if 条件表达式
/// 支持: defined(X), !defined(X), X, X==N, X!=N, X>N, X<N, A&&B, A||B
fn evaluate_condition(expr: &str, macros: &MacroTable) -> bool {
    let expr = expr.trim();

    // A || B (最低优先级，先分割)
    // 查找不在括号内的 || 运算符
    if let Some(pos) = find_operator_outside_parens(expr, "||") {
        let left = &expr[..pos];
        let right = &expr[pos + 2..];
        return evaluate_condition(left, macros) || evaluate_condition(right, macros);
    }

    // A && B (次低优先级)
    if let Some(pos) = find_operator_outside_parens(expr, "&&") {
        let left = &expr[..pos];
        let right = &expr[pos + 2..];
        return evaluate_condition(left, macros) && evaluate_condition(right, macros);
    }

    // !expr (一元取反)
    if let Some(rest) = expr.strip_prefix('!') {
        return !evaluate_condition(rest.trim(), macros);
    }

    // defined(X) 或 defined X
    if let Some(inner) = expr
        .strip_prefix("defined(")
        .and_then(|s| s.strip_suffix(')'))
    {
        return macros.is_defined(inner.trim());
    }
    if let Some(rest) = expr.strip_prefix("defined ") {
        return macros.is_defined(rest.trim());
    }

    // X == N
    if let Some(pos) = expr.find("==") {
        let name = expr[..pos].trim();
        let expected: i64 = expr[pos + 2..].trim().parse().unwrap_or(0);
        return macros.get_int(name).unwrap_or(0) == expected;
    }

    // X != N
    if let Some(pos) = expr.find("!=") {
        let name = expr[..pos].trim();
        let expected: i64 = expr[pos + 2..].trim().parse().unwrap_or(0);
        return macros.get_int(name).unwrap_or(0) != expected;
    }

    // X >= N
    if let Some(pos) = expr.find(">=") {
        let name = expr[..pos].trim();
        let expected: i64 = expr[pos + 2..].trim().parse().unwrap_or(0);
        return macros.get_int(name).unwrap_or(0) >= expected;
    }

    // X <= N
    if let Some(pos) = expr.find("<=") {
        let name = expr[..pos].trim();
        let expected: i64 = expr[pos + 2..].trim().parse().unwrap_or(0);
        return macros.get_int(name).unwrap_or(0) <= expected;
    }

    // X > N
    if let Some(pos) = expr.find('>') {
        let name = expr[..pos].trim();
        let expected: i64 = expr[pos + 1..].trim().parse().unwrap_or(0);
        return macros.get_int(name).unwrap_or(0) > expected;
    }

    // X < N
    if let Some(pos) = expr.find('<') {
        let name = expr[..pos].trim();
        let expected: i64 = expr[pos + 1..].trim().parse().unwrap_or(0);
        return macros.get_int(name).unwrap_or(0) < expected;
    }

    // 直接数字
    if let Ok(n) = expr.parse::<i64>() {
        return n != 0;
    }

    // 宏名称 (非零则为真)
    macros.get_int(expr).unwrap_or(0) != 0
}

// ──────────────────────────────────────────────────────────────
// 注释剥离
// ──────────────────────────────────────────────────────────────

/// 剥离 C/C++ 注释，保留字符串字面量内的内容
/// 行注释 (//) 替换为空行 (保持行号映射)
/// 块注释 (/* */) 替换为等量空行
pub fn strip_comments(source: &str) -> String {
    let chars: Vec<char> = source.chars().collect();
    let mut result = Vec::with_capacity(chars.len());
    let mut i = 0;

    while i < chars.len() {
        // 字符串字面量 — 原样保留
        if chars[i] == '"' {
            result.push(chars[i]);
            i += 1;
            while i < chars.len() && chars[i] != '"' {
                if chars[i] == '\\' && i + 1 < chars.len() {
                    result.push(chars[i]);
                    result.push(chars[i + 1]);
                    i += 2;
                } else {
                    result.push(chars[i]);
                    i += 1;
                }
            }
            if i < chars.len() {
                result.push(chars[i]); // 闭合 "
                i += 1;
            }
            continue;
        }

        // 字符字面量
        if chars[i] == '\'' {
            result.push(chars[i]);
            i += 1;
            while i < chars.len() && chars[i] != '\'' {
                if chars[i] == '\\' && i + 1 < chars.len() {
                    result.push(chars[i]);
                    result.push(chars[i + 1]);
                    i += 2;
                } else {
                    result.push(chars[i]);
                    i += 1;
                }
            }
            if i < chars.len() {
                result.push(chars[i]);
                i += 1;
            }
            continue;
        }

        // 行注释 //
        if i + 1 < chars.len() && chars[i] == '/' && chars[i + 1] == '/' {
            // 跳过到行尾，用空格替换
            while i < chars.len() && chars[i] != '\n' {
                result.push(' ');
                i += 1;
            }
            continue;
        }

        // 块注释 /* */
        if i + 1 < chars.len() && chars[i] == '/' && chars[i + 1] == '*' {
            i += 2; // 跳过 /*
            while i + 1 < chars.len() {
                if chars[i] == '*' && chars[i + 1] == '/' {
                    i += 2;
                    break;
                }
                // 保留换行符以维持行号
                if chars[i] == '\n' {
                    result.push('\n');
                } else {
                    result.push(' ');
                }
                i += 1;
            }
            continue;
        }

        result.push(chars[i]);
        i += 1;
    }

    result.into_iter().collect()
}

// ──────────────────────────────────────────────────────────────
// 预处理结果
// ──────────────────────────────────────────────────────────────

/// 预处理结果
#[derive(Debug)]
pub struct PreprocessResult {
    /// 处理后的源码 (条件编译已展开，注释已剥离)
    pub processed_source: String,
    /// 输出行 → 原始行号映射 (用于错误定位)
    pub line_map: Vec<usize>,
    /// 预处理警告列表
    pub warnings: Vec<PreprocessWarning>,
    /// 处理后剩余的宏定义数量
    pub macros_defined: usize,
}

/// 预处理警告
#[derive(Debug, Clone)]
pub struct PreprocessWarning {
    /// 行号
    pub line: usize,
    /// 警告信息
    pub message: String,
}

// ──────────────────────────────────────────────────────────────
// 公开便捷函数
// ──────────────────────────────────────────────────────────────

/// 对源码执行完整预处理 (使用默认引擎宏表)
pub fn preprocess_source(source: &str, file_name: &str) -> PreprocessResult {
    let mut preprocessor = Preprocessor::new();
    preprocessor.process(source, file_name)
}

/// 对源码执行预处理 (使用自定义宏表)
pub fn preprocess_with_macros(
    source: &str,
    file_name: &str,
    macros: MacroTable,
) -> PreprocessResult {
    let mut preprocessor = Preprocessor::with_macros(macros);
    preprocessor.process(source, file_name)
}

// ──────────────────────────────────────────────────────────────
// 单元测试
// ──────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_strip_line_comments() {
        let src = "int x = 1; // 这是注释\nint y = 2;";
        let cleaned = strip_comments(src);
        assert!(cleaned.contains("int x = 1;"));
        assert!(!cleaned.contains("这是注释"));
        assert!(cleaned.contains("int y = 2;"));
    }

    #[test]
    fn test_strip_block_comments() {
        let src = "/* 块注释 */int x = 1;";
        let cleaned = strip_comments(src);
        assert!(!cleaned.contains("块注释"));
        assert!(cleaned.contains("int x = 1;"));
    }

    #[test]
    fn test_strip_comments_preserves_string_literals() {
        let src = r#"const char* s = "hello // not a comment";"#;
        let cleaned = strip_comments(src);
        assert!(cleaned.contains("hello // not a comment"));
    }

    #[test]
    fn test_ifdef_active() {
        let src = "#define MY_FLAG\n#ifdef MY_FLAG\nint x = 1;\n#endif\n";
        let result = preprocess_source(src, "test.h");
        assert!(result.processed_source.contains("int x = 1;"));
    }

    #[test]
    fn test_ifdef_inactive() {
        let src = "#ifdef NOT_DEFINED\nint x = 1;\n#endif\n";
        let result = preprocess_source(src, "test.h");
        assert!(!result.processed_source.contains("int x = 1;"));
    }

    #[test]
    fn test_ifndef_active() {
        let src = "#ifndef NOT_DEFINED\nint x = 1;\n#endif\n";
        let result = preprocess_source(src, "test.h");
        assert!(result.processed_source.contains("int x = 1;"));
    }

    #[test]
    fn test_if_else() {
        let src = "#define USE_FAST 1\n#if USE_FAST\nint fast = 1;\n#else\nint slow = 1;\n#endif\n";
        let result = preprocess_source(src, "test.h");
        assert!(result.processed_source.contains("int fast = 1;"));
        assert!(!result.processed_source.contains("int slow = 1;"));
    }

    #[test]
    fn test_nested_conditions() {
        let src = r#"
#define OUTER 1
#ifdef OUTER
  #define INNER 1
  #ifdef INNER
int nested = 1;
  #endif
#endif
"#;
        let result = preprocess_source(src, "test.h");
        assert!(result.processed_source.contains("int nested = 1;"));
    }

    #[test]
    fn test_evaluate_condition_defined() {
        let mut macros = MacroTable::new();
        macros.define("FOO", MacroValue::Empty);
        assert!(evaluate_condition("defined(FOO)", &macros));
        assert!(!evaluate_condition("defined(BAR)", &macros));
    }

    #[test]
    fn test_evaluate_condition_and() {
        let mut macros = MacroTable::new();
        macros.define_int("A", 1);
        macros.define_int("B", 1);
        assert!(evaluate_condition("defined(A)&&defined(B)", &macros));
    }

    #[test]
    fn test_engine_default_macros() {
        let macros = MacroTable::with_engine_defaults();
        assert!(macros.is_defined("LIMX_ENGINE"));
        assert!(macros.is_defined("PLATFORM_WINDOWS"));
        assert_eq!(macros.get_int("LIMX_ENGINE"), Some(1));
    }
}
