/*******************************************************************************
 * 文件: generators/mod.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   LBT 代码生成器模块
 *   - CMake 项目生成
 *   - Visual Studio 解决方案生成
 *   - IDE 项目文件生成
 *   - 模块头文件生成
 *   - API 头文件生成
 *
 ******************************************************************************/

pub mod api;
pub mod cmake;
pub mod compile_commands;
pub mod graph_export;
pub mod ide;
pub mod module;
pub mod vs;

pub use compile_commands::{
    generate_compile_commands, CompileCommandsConfig, CompileCommandsGenerator,
};
pub use graph_export::{
    CouplingMatrix, ExportOptions, GraphColorScheme, GraphExporter, ModuleMetrics,
};
