/*******************************************************************************
 * 文件: cli.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   命令行接口定义
 *
 ******************************************************************************/

pub mod progress;

use clap::{Parser, Subcommand};
use std::path::PathBuf;

pub use progress::{print_banner, print_phase, BuildProgress, BuildReport, ColoredOutput};

#[derive(Parser)]
#[command(name = "lbt")]
#[command(author = "LimxTeam")]
#[command(version = "0.1.0")]
#[command(about = "Limx Build Tool - 模块化构建系统")]
#[command(long_about = r#"
LBT (Limx Build Tool) - 模块化 C++ 构建系统

功能:
  • 模块发现与依赖解析
  • CMake/VS 解决方案生成
  • PCH/Unity Build 支持
  • 增量编译与缓存
  • 依赖图可视化

示例:
  lbt generate -s Source          生成 CMake 配置
  lbt generate-solution -s Source 生成 VS 解决方案
  lbt graph -s Source -f mermaid  输出依赖图
  lbt stats -s Source             显示构建统计
  lbt validate -s Source          验证模块配置
"#)]
#[command(after_help = "更多信息请访问: https://github.com/LimxTeam/Limx")]
pub struct Cli {
    #[command(subcommand)]
    pub command: Commands,
}

#[derive(Subcommand)]
pub enum Commands {
    /// 生成 CMake 项目配置
    Generate {
        /// 源代码目录 (包含 *.limx.toml 的目录)
        #[arg(short, long, default_value = "Source")]
        source_dir: PathBuf,

        /// 输出目录 (生成的 CMake 文件位置)
        #[arg(short, long, default_value = "Intermediate/Build")]
        output_dir: PathBuf,

        /// 目标平台
        #[arg(short, long, default_value = "windows")]
        platform: String,

        /// 构建配置
        #[arg(short, long, default_value = "development")]
        config: String,
    },

    /// 创建新模块
    NewModule {
        /// 模块名称
        #[arg(short, long)]
        name: String,

        /// 架构层级 (0-5)
        #[arg(short, long)]
        layer: u8,

        /// 源代码目录
        #[arg(short, long, default_value = "Source")]
        source_dir: PathBuf,
    },

    /// 列出所有已发现的模块
    List {
        /// 源代码目录
        #[arg(short, long, default_value = "Source")]
        source_dir: PathBuf,
    },

    /// 检查模块配置、依赖和源码规范 (类似 cargo check)
    Check {
        /// 源代码目录
        #[arg(short, long, default_value = "Source")]
        source_dir: PathBuf,

        /// 启用 MSVC /analyze 深度静态分析 (较慢)
        #[arg(long, default_value_t = false)]
        analyze: bool,
    },

    /// 生成 IDE 项目文件 (VS/VSCode/Rider)
    GenerateProject {
        /// 源代码目录
        #[arg(short, long, default_value = "Source")]
        source_dir: PathBuf,

        /// 项目根目录
        #[arg(short, long, default_value = ".")]
        project_dir: PathBuf,

        /// 目标 IDE (vs, vscode, rider, clion, all)
        #[arg(short, long, default_value = "all")]
        ide: String,
    },

    /// 构建项目 (完整流程)
    Build {
        /// 源代码目录
        #[arg(short, long, default_value = "Source")]
        source_dir: PathBuf,

        /// 构建配置 (debug, development, release)
        #[arg(short, long, default_value = "development")]
        config: String,

        /// 编译器 (msvc, clang, gcc)
        #[arg(long, default_value = "msvc")]
        compiler: String,

        /// 并行任务数 (0 = 自动检测)
        #[arg(short, long, default_value = "0")]
        jobs: usize,

        /// 启用 PCH 预编译头
        #[arg(long)]
        pch: bool,

        /// 启用 Unity Build
        #[arg(long)]
        unity: bool,

        /// 仅生成，不构建
        #[arg(long)]
        generate_only: bool,

        /// 强制重新编译所有
        #[arg(long)]
        rebuild: bool,

        /// 跳过着色器编译阶段
        #[arg(long)]
        skip_shaders: bool,

        /// 详细输出
        #[arg(short, long)]
        verbose: bool,
    },

    /// 生成 Visual Studio 解决方案 (无需 CMake)
    GenerateSolution {
        /// 源代码目录
        #[arg(short, long, default_value = "Source")]
        source_dir: PathBuf,

        /// 输出目录 (存放 .sln 和 .vcxproj)
        #[arg(short, long, default_value = "Intermediate/ProjectFiles")]
        output_dir: PathBuf,

        /// 解决方案名称
        #[arg(short, long, default_value = "LimxEngine")]
        name: String,
    },

    /// 生成反射代码 (调用 LHT)
    GenerateReflection {
        /// 源代码目录
        #[arg(short, long, default_value = "Source")]
        source_dir: PathBuf,

        /// 输出目录 (生成的 .generated.h/cpp)
        #[arg(short, long, default_value = "Intermediate/Generated")]
        output_dir: PathBuf,

        /// 指定模块 (可选，不指定则处理所有模块)
        #[arg(short, long)]
        module: Option<String>,
    },

    /// 显示依赖图
    Graph {
        /// 源代码目录
        #[arg(short, long, default_value = "Source")]
        source_dir: PathBuf,

        /// 输出格式 (dot, mermaid, tree, stats)
        #[arg(short, long, default_value = "tree")]
        format: String,

        /// 输出文件 (可选，不指定则输出到控制台)
        #[arg(short, long)]
        output: Option<PathBuf>,
    },

    /// 清理构建缓存和中间文件
    Clean {
        /// 是否清理所有 (包括生成的项目文件)
        #[arg(long)]
        all: bool,
    },

    /// 显示构建统计信息
    Stats {
        /// 源代码目录
        #[arg(short, long, default_value = "Source")]
        source_dir: PathBuf,
    },

    /// 验证模块配置
    Validate {
        /// 源代码目录
        #[arg(short, long, default_value = "Source")]
        source_dir: PathBuf,

        /// 严格模式 (警告视为错误)
        #[arg(long)]
        strict: bool,
    },

    /// 启动分布式编译协调器
    DistCoordinator {
        /// 监听端口
        #[arg(short, long, default_value = "19283")]
        port: u16,

        /// 最大工作节点数
        #[arg(long, default_value = "100")]
        max_workers: usize,
    },

    /// 启动分布式编译工作节点
    DistWorker {
        /// 协调器地址
        #[arg(short, long)]
        coordinator: String,

        /// 最大并发任务数 (0 = 自动)
        #[arg(short, long, default_value = "0")]
        jobs: usize,
    },

    /// 分析头文件依赖
    AnalyzeDeps {
        /// 源代码目录
        #[arg(short, long, default_value = "Source")]
        source_dir: PathBuf,

        /// 输出格式 (json, dot, tree)
        #[arg(short, long, default_value = "tree")]
        format: String,

        /// 输出文件 (可选)
        #[arg(short, long)]
        output: Option<PathBuf>,
    },

    /// 创建新的构建目标 (.limx.target.toml)
    NewTarget {
        /// Target 名称 (如 LimxEngineGame)
        #[arg(short, long)]
        name: String,

        /// Target 类型 (game, editor, server, client, program, plugin)
        #[arg(short, long, default_value = "game")]
        target_type: String,

        /// 创建目录
        #[arg(short, long, default_value = ".")]
        output_dir: PathBuf,
    },

    /// 创建新的插件 (.limx.plugin.toml)
    NewPlugin {
        /// 插件名称
        #[arg(short, long)]
        name: String,

        /// 创建目录 (通常是 Plugins/PluginName/)
        #[arg(short, long, default_value = "Plugins")]
        output_dir: PathBuf,
    },

    /// 列出所有 Target
    ListTargets {
        /// 项目根目录
        #[arg(short, long, default_value = ".")]
        project_dir: PathBuf,
    },

    /// 列出所有插件
    ListPlugins {
        /// 项目根目录
        #[arg(short, long, default_value = ".")]
        project_dir: PathBuf,
    },

    /// 显示编译缓存统计
    CacheStats {
        /// 缓存目录
        #[arg(short, long, default_value = "Intermediate/CompileCache")]
        cache_dir: PathBuf,

        /// 输出格式 (text, json)
        #[arg(long, default_value = "text")]
        format: String,

        /// 清空缓存
        #[arg(long)]
        clear: bool,
    },

    /// 生成 compile_commands.json (供 clangd/clang-tidy 使用)
    CompileCommands {
        /// 源代码目录
        #[arg(short, long, default_value = "Source")]
        source_dir: PathBuf,

        /// 项目根目录 (compile_commands.json 输出位置)
        #[arg(short, long, default_value = ".")]
        project_dir: PathBuf,

        /// 编译器路径 (clangd 使用)
        #[arg(long, default_value = "clang++")]
        compiler: String,
    },

    /// 分析未使用/冗余 #include (IncludeAnalyzer)
    AnalyzeIncludes {
        /// 源代码目录
        #[arg(short, long, default_value = "Source")]
        source_dir: PathBuf,

        /// 输出格式 (text, markdown, json)
        #[arg(short, long, default_value = "text")]
        format: String,

        /// 输出文件 (可选)
        #[arg(short, long)]
        output: Option<PathBuf>,
    },

    /// 智能 PCH 候选分析 (推荐放入预编译头的头文件)
    AnalyzePch {
        /// 源代码目录
        #[arg(short, long, default_value = "Source")]
        source_dir: PathBuf,

        /// PCH 最大头文件数
        #[arg(long, default_value = "30")]
        max_headers: usize,

        /// 最小包含频率阈值 (低于此频率的头文件不纳入)
        #[arg(long, default_value = "2")]
        min_frequency: usize,
    },

    /// 模块健康度分析 (代码度量/API 表面积/膨胀检测)
    HealthCheck {
        /// 源代码目录
        #[arg(short, long, default_value = "Source")]
        source_dir: PathBuf,

        /// 指定模块名 (可选，不指定则分析所有)
        #[arg(short, long)]
        module: Option<String>,

        /// 输出格式 (text, markdown, json)
        #[arg(short, long, default_value = "text")]
        format: String,
    },

    /// 导出增强依赖图 (DOT 热力图/Mermaid/HTML 交互式/耦合度矩阵)
    GraphExport {
        /// 源代码目录
        #[arg(short, long, default_value = "Source")]
        source_dir: PathBuf,

        /// 导出格式 (dot, mermaid, html, coupling-csv)
        #[arg(short, long, default_value = "html")]
        format: String,

        /// 输出文件
        #[arg(short, long)]
        output: Option<PathBuf>,
    },

    /// 导出构建剖析报告 (从上次 Build 的数据)
    BuildProfile {
        /// 剖析数据目录
        #[arg(short, long, default_value = "Intermediate/Profile")]
        profile_dir: PathBuf,

        /// 输出格式 (text, json, html)
        #[arg(short, long, default_value = "text")]
        format: String,

        /// 输出文件 (可选)
        #[arg(short, long)]
        output: Option<PathBuf>,
    },
}
