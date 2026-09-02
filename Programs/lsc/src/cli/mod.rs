/*******************************************************************************
 * 文件: cli/mod.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   LSC 命令行接口定义 (生产级增强版)
 *   - 完整的命令行参数支持
 *   - 缓存管理命令
 *   - 变体编译命令
 *   - Vulkan 专属配置
 *
 * 支持的命令:
 *   - compile: 编译单个着色器
 *   - compile-all: 批量编译
 *   - compile-variants: 变体编译
 *   - watch: 文件监视
 *   - reflect: 反射信息提取
 *   - validate: 着色器验证
 *   - disassemble: SPIR-V反汇编
 *   - cache: 缓存管理
 *   - stats: 统计信息
 *   - clean: 清理输出
 *
 ******************************************************************************/

use crate::core::ShaderStage;
use clap::{Args, Parser, Subcommand};
use std::path::PathBuf;

#[derive(Parser)]
#[command(name = "lsc")]
#[command(author = "LimxTeam")]
#[command(version = "0.1.0")]
#[command(about = "Limx Shader Compiler - Vulkan SPIR-V 着色器编译工具")]
#[command(long_about = r#"
LSC (Limx Shader Compiler) 是专为 Vulkan 设计的高性能着色器编译工具。

特性:
  - 支持 GLSL 和 HLSL 着色器
  - 输出 SPIR-V 二进制
  - 完整的反射信息提取
  - 着色器变体系统
  - 增量编译和缓存
  - 并行批量编译

示例:
  lsc compile -s shader.vert -o shader.spv
  lsc compile-all -O
  lsc compile-all -s Shaders/ -o Binaries/Shaders/ -O
  lsc compile-variants -s shader.frag -o variants/
  lsc reflect -s shader.spv
  lsc cache stats
"#)]
pub struct Cli {
    /// 全局详细输出
    #[arg(short, long, global = true)]
    pub verbose: bool,

    /// 禁用颜色输出
    #[arg(long, global = true)]
    pub no_color: bool,

    #[command(subcommand)]
    pub command: Commands,
}

/// 通用编译选项
#[derive(Args, Clone)]
pub struct CompileArgs {
    /// 预处理器宏定义 (格式: NAME 或 NAME=VALUE)
    #[arg(short = 'D', long = "define")]
    pub defines: Vec<String>,

    /// 头文件包含目录
    #[arg(short = 'I', long = "include")]
    pub include_dirs: Vec<PathBuf>,

    /// 启用优化
    #[arg(short = 'O', long)]
    pub optimize: bool,

    /// 生成调试信息
    #[arg(short = 'g', long)]
    pub debug_info: bool,

    /// 目标 Vulkan 版本 (1.0, 1.1, 1.2, 1.3, 1.4)
    #[arg(long, default_value = "1.4")]
    pub vulkan_version: String,

    /// 生成反射信息
    #[arg(long, default_value = "true")]
    pub reflection: bool,

    /// 启用缓存
    #[arg(long, default_value = "true")]
    pub cache: bool,

    /// 缓存目录
    #[arg(long)]
    pub cache_dir: Option<PathBuf>,
}

#[derive(Subcommand)]
pub enum Commands {
    /// 编译单个着色器
    #[command(visible_alias = "c")]
    Compile {
        /// 源文件路径
        #[arg(short, long)]
        source: PathBuf,

        /// 输出文件路径
        #[arg(short, long)]
        output: PathBuf,

        /// 着色器阶段 (auto, vertex, fragment, compute, geometry, tesscontrol, tesseval, raygen, raymiss, rayhit, callable, task, mesh)
        #[arg(long, default_value = "auto")]
        stage: ShaderStage,

        /// 入口点函数名
        #[arg(long, default_value = "main")]
        entry_point: String,

        #[command(flatten)]
        compile_args: CompileArgs,
    },

    /// 批量编译目录下所有着色器
    #[command(visible_alias = "ca")]
    CompileAll {
        /// 源目录 (默认: Shaders)
        #[arg(short, long, default_value = "Shaders")]
        source_dir: PathBuf,

        /// 输出目录 (默认: Binaries/Shaders)
        #[arg(short, long, default_value = "Binaries/Shaders")]
        output_dir: PathBuf,

        /// 并行编译线程数 (0 = 自动)
        #[arg(short = 'j', long, default_value = "0")]
        jobs: usize,

        /// 递归搜索子目录
        #[arg(short = 'r', long, default_value = "true")]
        recursive: bool,

        /// 文件扩展名过滤 (逗号分隔)
        #[arg(long, default_value = "vert,frag,comp,geom,tesc,tese,task,mesh,glsl,hlsl")]
        extensions: String,

        #[command(flatten)]
        compile_args: CompileArgs,
    },

    /// 编译着色器变体
    #[command(visible_alias = "cv")]
    CompileVariants {
        /// 源文件路径
        #[arg(short, long)]
        source: PathBuf,

        /// 输出目录
        #[arg(short, long)]
        output_dir: PathBuf,

        /// 变体配置文件 (JSON)
        #[arg(long)]
        config: Option<PathBuf>,

        /// 并行编译
        #[arg(short = 'j', long, default_value = "true")]
        parallel: bool,

        /// 输出变体包 (.variants.json)
        #[arg(long, default_value = "true")]
        bundle: bool,

        #[command(flatten)]
        compile_args: CompileArgs,
    },

    /// 监视着色器变化并自动重编译
    #[command(visible_alias = "w")]
    Watch {
        /// 监视目录
        #[arg(short, long)]
        source_dir: PathBuf,

        /// 输出目录
        #[arg(short, long)]
        output_dir: PathBuf,

        /// 防抖延迟 (毫秒)
        #[arg(long, default_value = "500")]
        debounce: u64,

        #[command(flatten)]
        compile_args: CompileArgs,
    },

    /// 提取 SPIR-V 反射信息
    #[command(visible_alias = "r")]
    Reflect {
        /// 源文件 (SPIR-V 或着色器源码)
        #[arg(short, long)]
        source: PathBuf,

        /// 输出 JSON 文件路径
        #[arg(short, long)]
        output: Option<PathBuf>,

        /// 输出格式 (json, yaml, rust)
        #[arg(long, default_value = "json")]
        format: String,

        /// 包含详细类型信息
        #[arg(long)]
        detailed: bool,
    },

    /// 验证着色器/SPIR-V
    #[command(visible_alias = "v")]
    Validate {
        /// 源文件
        #[arg(short, long)]
        source: PathBuf,

        /// 严格模式
        #[arg(long)]
        strict: bool,

        /// 目标 Vulkan 版本
        #[arg(long, default_value = "1.4")]
        vulkan_version: String,
    },

    /// 反汇编 SPIR-V
    #[command(visible_alias = "d")]
    Disassemble {
        /// SPIR-V 文件
        #[arg(short, long)]
        source: PathBuf,

        /// 输出文件 (默认输出到控制台)
        #[arg(short, long)]
        output: Option<PathBuf>,

        /// 显示原始 ID (不使用友好名称)
        #[arg(long)]
        raw_id: bool,
    },

    /// 缓存管理
    Cache {
        #[command(subcommand)]
        action: CacheCommands,
    },

    /// 统计着色器信息
    Stats {
        /// 源目录
        #[arg(short, long)]
        source_dir: PathBuf,

        /// 输出格式 (text, json)
        #[arg(long, default_value = "text")]
        format: String,
    },

    /// 清理编译输出
    Clean {
        /// 输出目录
        #[arg(short, long)]
        output_dir: PathBuf,

        /// 同时清理缓存
        #[arg(long)]
        cache: bool,

        /// 干运行 (只显示要删除的文件)
        #[arg(long)]
        dry_run: bool,
    },

    /// 生成着色器绑定代码 (C++/Rust)
    GenerateBindings {
        /// 源文件或目录
        #[arg(short, long)]
        source: PathBuf,

        /// 输出文件
        #[arg(short, long)]
        output: PathBuf,

        /// 目标语言 (cpp, rust)
        #[arg(long, default_value = "cpp")]
        language: String,

        /// 命名空间/模块名
        #[arg(long)]
        namespace: Option<String>,
    },

    /// 分析 SPIR-V 二进制 (指令统计、优化建议)
    #[command(visible_alias = "a")]
    AnalyzeSpirv {
        /// SPIR-V 文件路径
        #[arg(short, long)]
        source: PathBuf,

        /// 输出报告文件 (可选，默认输出到控制台)
        #[arg(short, long)]
        output: Option<PathBuf>,

        /// 输出格式 (text, json)
        #[arg(long, default_value = "text")]
        format: String,
    },

    /// 向 SPIR-V 注入调试信息 (变量名、源码映射、RenderDoc 标记)
    InjectDebug {
        /// SPIR-V 输入文件
        #[arg(short, long)]
        source: PathBuf,

        /// 输出文件 (带调试信息的 SPIR-V)
        #[arg(short, long)]
        output: PathBuf,

        /// 注入 RenderDoc 兼容标记
        #[arg(long)]
        renderdoc: bool,

        /// 注入源码映射
        #[arg(long)]
        source_map: bool,
    },

    /// 分析变体组合并裁剪冗余变体
    PruneVariants {
        /// 变体配置文件 (JSON)
        #[arg(short, long)]
        config: PathBuf,

        /// 使用频率数据文件 (可选，用于基于使用频率裁剪)
        #[arg(long)]
        usage_data: Option<PathBuf>,

        /// 输出裁剪报告
        #[arg(short, long)]
        output: Option<PathBuf>,

        /// 输出格式 (text, json)
        #[arg(long, default_value = "text")]
        format: String,
    },

    /// 编译 .limx.shaders 清单文件
    #[command(visible_alias = "cm")]
    CompileManifest {
        /// 清单文件路径 (.limx.shaders)
        #[arg(short, long)]
        manifest: PathBuf,

        /// 强制重新编译所有着色器 (忽略缓存)
        #[arg(long)]
        force: bool,

        /// 仅编译指定标签的着色器
        #[arg(long)]
        tag: Option<String>,
    },

    /// 在目录中扫描并批量编译所有清单
    CompileAllManifests {
        /// 源目录
        #[arg(short, long, default_value = "Source")]
        source_dir: PathBuf,

        /// 强制重新编译
        #[arg(long)]
        force: bool,
    },

    /// 创建新的 .limx.shaders 清单文件
    NewManifest {
        /// 模块名称
        #[arg(short, long)]
        module: String,

        /// 输出目录
        #[arg(short, long, default_value = ".")]
        output_dir: PathBuf,
    },

    /// 显示 PSO 预热缓存统计
    PsoStats {
        /// PSO 缓存文件路径 (.limx.psocache)
        #[arg(
            short,
            long,
            default_value = "Intermediate/PsoCache/engine.limx.psocache"
        )]
        cache: PathBuf,
    },

    /// 合并多个 PSO 缓存文件
    PsoMerge {
        /// 输入缓存文件列表
        #[arg(short, long)]
        inputs: Vec<PathBuf>,

        /// 输出合并后的缓存文件
        #[arg(short, long)]
        output: PathBuf,

        /// 清除过期条目 (SPIR-V 文件已删除)
        #[arg(long)]
        purge_stale: bool,
    },

    /// 构建着色器 #include 依赖图并分析影响范围
    IncludeGraph {
        /// 着色器源目录
        #[arg(short, long, default_value = "Shaders")]
        source_dir: PathBuf,

        /// 输出格式 (dot, text, json)
        #[arg(short, long, default_value = "text")]
        format: String,

        /// 输出文件 (可选)
        #[arg(short, long)]
        output: Option<PathBuf>,

        /// 检测循环包含
        #[arg(long)]
        detect_cycles: bool,
    },

    /// 着色器性能静态分析
    PerfAnalyze {
        /// 着色器源文件或目录
        #[arg(short, long)]
        source: PathBuf,

        /// 输出格式 (text, json)
        #[arg(short, long, default_value = "text")]
        format: String,

        /// 输出文件 (可选)
        #[arg(short, long)]
        output: Option<PathBuf>,
    },

    /// 跨阶段接口验证 (验证顶点→片段等阶段间 IO 匹配)
    ValidatePipeline {
        /// 着色器源文件列表 (按管线阶段顺序)
        #[arg(short, long)]
        shaders: Vec<PathBuf>,

        /// Push Constants 大小上限 (字节)
        #[arg(long, default_value = "128")]
        push_constant_limit: usize,
    },
}

/// 缓存子命令
#[derive(Subcommand)]
pub enum CacheCommands {
    /// 显示缓存统计
    Stats {
        /// 缓存目录
        #[arg(short, long)]
        cache_dir: Option<PathBuf>,
    },

    /// 清除缓存
    Clear {
        /// 缓存目录
        #[arg(short, long)]
        cache_dir: Option<PathBuf>,

        /// 只清除过期缓存
        #[arg(long)]
        expired_only: bool,

        /// 过期时间 (天)
        #[arg(long, default_value = "7")]
        max_age_days: u32,
    },

    /// 预热缓存
    Warmup {
        /// 缓存目录
        #[arg(short, long)]
        cache_dir: Option<PathBuf>,

        /// 预热条目数
        #[arg(long, default_value = "50")]
        count: usize,
    },

    /// 验证缓存完整性
    Verify {
        /// 缓存目录
        #[arg(short, long)]
        cache_dir: Option<PathBuf>,

        /// 修复无效条目
        #[arg(long)]
        fix: bool,
    },
}

//=============================================================================
// 辅助函数
//=============================================================================

impl CompileArgs {
    /// 解析宏定义
    pub fn parse_defines(&self) -> Vec<(String, Option<String>)> {
        self.defines
            .iter()
            .map(|d| {
                let parts: Vec<&str> = d.splitn(2, '=').collect();
                let name = parts[0].to_string();
                let value = parts.get(1).map(|s| s.to_string());
                (name, value)
            })
            .collect()
    }

    /// 获取缓存目录
    pub fn get_cache_dir(&self) -> PathBuf {
        self.cache_dir.clone().unwrap_or_else(|| {
            dirs::cache_dir()
                .unwrap_or_else(|| PathBuf::from("."))
                .join("lsc")
        })
    }

    /// 解析 Vulkan 版本
    pub fn parse_vulkan_version(&self) -> crate::core::TargetEnvironment {
        match self.vulkan_version.as_str() {
            "1.0" => crate::core::TargetEnvironment::Vulkan1_0,
            "1.1" => crate::core::TargetEnvironment::Vulkan1_1,
            "1.2" => crate::core::TargetEnvironment::Vulkan1_2,
            "1.3" => crate::core::TargetEnvironment::Vulkan1_3,
            _ => crate::core::TargetEnvironment::Vulkan1_4,
        }
    }
}

/// 默认缓存目录
pub fn default_cache_dir() -> PathBuf {
    dirs::cache_dir()
        .unwrap_or_else(|| PathBuf::from("."))
        .join("lsc")
}
