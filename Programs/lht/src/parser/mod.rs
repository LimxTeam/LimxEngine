/*******************************************************************************
 * 文件: parser/mod.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   LHT 解析器模块 (生产级)
 *   - 头文件扫描器
 *   - 词法分析器
 *   - 抽象语法树
 *   - 语法分析器
 *   - 反射宏解析
 *   - 说明符解析
 *
 ******************************************************************************/

pub mod ast;
pub mod lexer;
pub mod parser;
pub mod preprocessor;
pub mod scanner;
pub mod semantic;
pub mod specifiers;
pub mod syntax;
pub mod type_consistency;

pub use preprocessor::{
    preprocess_source, preprocess_with_macros, strip_comments, MacroTable, Preprocessor,
};
pub use semantic::{
    analyze_reflected_types, DiagnosticSeverity, IncrementalParseCache, SemanticAnalyzer,
    SemanticDiagnostic, SemanticReport,
};
pub use type_consistency::{
    ConsistencyDiagnostic, ConsistencyReport, TypeConsistencyChecker, TypeRegistry,
};
