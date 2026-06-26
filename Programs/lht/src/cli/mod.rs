/*******************************************************************************
 * 文件: cli.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   LHT 命令行接口定义
 *
 ******************************************************************************/

use clap::{Parser, Subcommand};
use std::path::PathBuf;

#[derive(Parser)]
#[command(name = "lht")]
#[command(author = "LimxTeam")]
#[command(version = "0.1.0")]
#[command(about = "Limx Header Tool - 反射代码生成器")]
#[command(long_about = r#"
LHT (Limx Header Tool) - C++ 反射代码生成器和热重载系统

功能:
  • 反射宏解析 (LCLASS, LSTRUCT, LENUM, LPROPERTY, LFUNCTION)
  • 自动生成 .generated.h/cpp 文件
  • 热重载监控与代码重新生成
  • API 文档自动生成
  • 反射统计分析

示例:
  lht generate -s Source -o Generated  生成反射代码
  lht check -s Source                  检查反射宏语法
  lht watch -s Source -o Generated     启动热重载监控
  lht docs -s Source -o Docs/API       生成 API 文档
  lht stats -s Source                  显示反射统计
"#)]
#[command(after_help = "更多信息请访问: https://github.com/LimxTeam/Limx")]
pub struct Cli {
    #[command(subcommand)]
    pub command: Commands,
}

#[derive(Subcommand)]
pub enum Commands {
    /// 生成反射代码
    Generate {
        /// 源代码目录
        #[arg(short, long, default_value = "Source")]
        source_dir: PathBuf,

        /// 输出目录 (生成的代码位置)
        #[arg(short, long, default_value = "Generated")]
        output_dir: PathBuf,

        /// 指定模块名称 (可选，不指定则处理所有模块)
        #[arg(short, long)]
        module: Option<String>,

        /// 生成序列化代码
        #[arg(long)]
        serialization: bool,

        /// 生成 RPC 代码
        #[arg(long)]
        rpc: bool,

        /// 生成 GC 支持代码
        #[arg(long)]
        gc: bool,

        /// 生成编辑器元数据代码 (属性面板控件/范围/条件可见性)
        #[arg(long)]
        editor_meta: bool,

        /// 生成脚本绑定代码 (Thunk 包装/参数编解码/注册)
        #[arg(long)]
        script_binding: bool,

        /// 生成属性迁移代码 (版本差分/链式迁移/注册表)
        #[arg(long)]
        migration: bool,

        /// 生成类型安全属性绑定代码 (constexpr 元数据表/CRTP 适配器/访问器)
        #[arg(long)]
        type_binding: bool,

        /// 启用增量代码生成 (仅重新生成变更的类型)
        #[arg(long)]
        incremental: bool,

        /// 启用预处理器 (展开条件编译后再解析)
        #[arg(long)]
        preprocess: bool,

        /// 生成所有高级功能代码
        #[arg(long)]
        all: bool,
    },

    /// 检查反射宏语法
    Check {
        /// 源代码目录
        #[arg(short, long, default_value = "Source")]
        source_dir: PathBuf,

        /// 启用语义分析 (继承链/说明符冲突/命名规范等深度检查)
        #[arg(long)]
        semantic: bool,

        /// 启用跨模块类型一致性校验 (未解析引用/名称冲突/循环依赖)
        #[arg(long)]
        type_consistency: bool,

        /// 严格模式 (警告视为错误)
        #[arg(long)]
        strict: bool,
    },

    /// 监控文件变更并自动生成 (热重载模式)
    Watch {
        /// 源代码目录
        #[arg(short, long, default_value = "Source")]
        source_dir: PathBuf,

        /// 输出目录
        #[arg(short, long, default_value = "Generated")]
        output_dir: PathBuf,
    },

    /// 生成 API 文档
    Docs {
        /// 源代码目录
        #[arg(short, long, default_value = "Source")]
        source_dir: PathBuf,

        /// 文档输出目录
        #[arg(short, long, default_value = "Docs/API")]
        output_dir: PathBuf,
    },

    /// 显示反射统计信息
    Stats {
        /// 源代码目录
        #[arg(short, long, default_value = "Source")]
        source_dir: PathBuf,
    },

    /// 生成 C++ 反射运行时头文件 (TypeInfo.h/PropertyInfo.h/FunctionInfo.h 等)
    GenerateRuntime {
        /// 输出目录 (生成的运行时头文件位置)
        #[arg(short, long, default_value = "Source/Runtime/Reflection")]
        output_dir: PathBuf,

        /// 命名空间
        #[arg(long, default_value = "Limx")]
        namespace: String,

        /// 是否包含 GC 支持
        #[arg(long, default_value = "true")]
        gc: bool,

        /// 是否包含网络复制支持
        #[arg(long, default_value = "true")]
        replication: bool,

        /// 是否包含编辑器绑定支持
        #[arg(long, default_value = "true")]
        editor: bool,
    },
}
