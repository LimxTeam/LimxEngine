/*******************************************************************************
 * 文件: main.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   LSC (Limx Shader Compiler) 入口点
 *   - Vulkan SPIR-V 着色器编译
 *   - GLSL/HLSL 源码编译
 *   - 着色器反射信息提取
 *   - 增量编译支持
 *   - 着色器变体生成
 *
 * 设计哲学:
 *   1. 仅支持 Vulkan - 专注单一图形 API
 *   2. 高性能并行编译
 *   3. 完整的错误诊断
 *   4. 与 LBT 构建系统集成
 *
 * 技术特性:
 *   - 基于 shaderc (glslang) 的 SPIR-V 编译
 *   - 支持 #include 指令
 *   - 支持着色器宏定义
 *   - 支持着色器变体 (Permutations)
 *   - 着色器缓存与增量编译
 *   - 反射信息输出 (绑定、Push Constants 等)
 *
 ******************************************************************************/

// 允许已实现但尚未集成的模块代码
#![allow(dead_code)]
#![allow(unused_imports)]
#![allow(unused_variables)]
#![allow(unused_assignments)]

mod cache;
mod cli;
mod compiler;
mod core;
mod manifest;
mod pso;
mod reflection;
mod variant;

use anyhow::Result;
use clap::Parser;
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::time::Instant;
use tracing::{error, info, warn};
use tracing_subscriber::{layer::SubscriberExt, util::SubscriberInitExt, EnvFilter};

use crate::cache::{CachedShaderCompiler, LruCacheConfig, LruShaderCache, ShaderCache};
use crate::cli::{default_cache_dir, CacheCommands, Cli, Commands, CompileArgs};
use crate::compiler::ShaderCompiler;
use crate::core::{CompileOptions, ShaderSource, ShaderStage};
use crate::manifest::{
    compile_manifest, discover_manifests, generate_example_manifest, load_manifest,
    ManifestCompiler,
};
use crate::pso::{pso_cache_file_extension, PsoCache};
use crate::variant::{VariantBundle, VariantCompiler, VariantParser};

/// 打印 LSC Banner
fn print_banner() {
    println!(
        r#"
    ╦  ╔═╗╔═╗
    ║  ╚═╗║     Limx Shader Compiler
    ╩═╝╚═╝╚═╝   v0.1.0 - Vulkan SPIR-V
"#
    );
}

fn main() -> Result<()> {
    // 初始化日志
    let log_dir = PathBuf::from("Logs");
    std::fs::create_dir_all(&log_dir).ok();

    let file_appender = tracing_appender::rolling::daily(&log_dir, "lsc.log");
    let (non_blocking, _guard) = tracing_appender::non_blocking(file_appender);

    let env_filter = EnvFilter::try_from_default_env().unwrap_or_else(|_| EnvFilter::new("info"));

    tracing_subscriber::registry()
        .with(env_filter)
        .with(tracing_subscriber::fmt::layer().with_writer(std::io::stdout))
        .with(
            tracing_subscriber::fmt::layer()
                .with_writer(non_blocking)
                .with_ansi(false),
        )
        .init();

    let cli = Cli::parse();

    match cli.command {
        Commands::Compile {
            source,
            output,
            stage,
            entry_point,
            compile_args,
        } => {
            if cli.verbose {
                print_banner();
            }

            // 自动检测着色器阶段 (如果是默认的 Vertex，尝试从扩展名推断)
            let actual_stage = if stage == ShaderStage::Vertex {
                // 检查是否应该从扩展名推断
                let ext = source.extension().and_then(|e| e.to_str()).unwrap_or("");
                let inferred = ShaderStage::from_extension(ext);
                // 如果扩展名推断出非 Vertex，使用推断结果
                if inferred != ShaderStage::Vertex || ext == "vert" || ext == "vs" || ext == "vsh" {
                    inferred
                } else {
                    stage
                }
            } else {
                stage
            };

            info!("LSC: 编译着色器");
            info!("  源文件: {}", source.display());
            info!("  输出: {}", output.display());
            info!("  阶段: {:?}", actual_stage);

            let start = Instant::now();

            // 创建编译器
            let compiler = ShaderCompiler::new()?;

            // 构建编译选项
            let mut options = build_compile_options(&compile_args);
            options.entry_point = entry_point;

            // 读取源码
            let source_code = std::fs::read_to_string(&source)?;
            let shader_source = ShaderSource {
                code: source_code,
                file_path: Some(source.clone()),
                stage: actual_stage,
            };

            // 编译
            let result = compiler.compile(&shader_source, &options)?;

            // 写入输出
            std::fs::create_dir_all(output.parent().unwrap_or(&PathBuf::from(".")))?;
            std::fs::write(&output, &result.spirv_binary)?;

            // 输出反射信息
            if compile_args.reflection {
                if let Some(reflection) = &result.reflection {
                    let reflection_path = output.with_extension("json");
                    let json = serde_json::to_string_pretty(reflection)?;
                    std::fs::write(&reflection_path, json)?;
                    info!("  反射信息: {}", reflection_path.display());
                }
            }

            let duration = start.elapsed();
            println!("\n✓ 编译成功");
            println!("  输出: {}", output.display());
            println!("  大小: {} bytes", result.spirv_binary.len());
            println!("  耗时: {:.2}ms", duration.as_secs_f64() * 1000.0);

            if !result.warnings.is_empty() {
                println!("\n警告:");
                for warning in &result.warnings {
                    println!("  {}", warning);
                }
            }
        }

        Commands::CompileAll {
            source_dir,
            output_dir,
            jobs,
            recursive,
            extensions,
            compile_args,
        } => {
            if cli.verbose {
                print_banner();
            }
            info!("LSC: 批量编译着色器");
            info!("  源目录: {}", source_dir.display());
            info!("  输出目录: {}", output_dir.display());

            let start = Instant::now();

            // 解析扩展名
            let ext_list: Vec<&str> = extensions.split(',').collect();

            // 发现所有着色器文件
            let shaders = discover_shaders_with_extensions(&source_dir, &ext_list, recursive)?;
            info!("发现 {} 个着色器文件", shaders.len());

            if shaders.is_empty() {
                println!("未找到着色器文件");
                return Ok(());
            }

            // 创建编译器
            let compiler = ShaderCompiler::new()?;

            // 构建编译选项
            let options = build_compile_options(&compile_args);

            // 创建进度条
            let pb = indicatif::ProgressBar::new(shaders.len() as u64);
            pb.set_style(
                indicatif::ProgressStyle::default_bar()
                    .template("{spinner:.green} [{bar:40.cyan/blue}] {pos}/{len} ({percent}%) [{elapsed_precise}] ETA {eta} | {msg}")
                    .unwrap_or_else(|_| indicatif::ProgressStyle::default_bar())
                    .progress_chars("█▓▒░"),
            );
            pb.set_message("编译着色器");
            pb.enable_steady_tick(std::time::Duration::from_millis(100));

            // 进度回调 — 每完成一个着色器立即更新进度条并输出状态
            let pb_callback = pb.clone();
            let verbose_output = cli.verbose;
            let on_shader_complete = move |result: &crate::core::BatchCompileResult| {
                pb_callback.inc(1);
                let file_name = result
                    .source_path
                    .file_name()
                    .map(|n| n.to_string_lossy().to_string())
                    .unwrap_or_else(|| "unknown".to_string());

                if result.success {
                    if verbose_output {
                        pb_callback.println(format!(
                            "  ✓ {} ({} bytes, {}ms)",
                            file_name, result.output_size, result.duration_ms
                        ));
                    }
                } else {
                    pb_callback.println(format!("  ✗ 编译失败: {}", file_name));
                    for err in &result.errors {
                        pb_callback.println(format!("    {}", err));
                    }
                }

                // 更新消息为当前文件名
                pb_callback.set_message(file_name);
            };

            // 编译所有着色器（带进度回调，保留目录结构）
            let parallel = jobs != 1;
            let results = if parallel {
                if jobs > 0 {
                    let pool = rayon::ThreadPoolBuilder::new().num_threads(jobs).build()?;
                    pool.install(|| {
                        compiler.compile_batch_parallel_with_progress(
                            &shaders,
                            &options,
                            &output_dir,
                            &source_dir,
                            on_shader_complete,
                        )
                    })?
                } else {
                    compiler.compile_batch_parallel_with_progress(
                        &shaders,
                        &options,
                        &output_dir,
                        &source_dir,
                        on_shader_complete,
                    )?
                }
            } else {
                compiler.compile_batch_with_progress(
                    &shaders,
                    &options,
                    &output_dir,
                    &source_dir,
                    on_shader_complete,
                )?
            };

            // 完成进度条
            let success_count = results.iter().filter(|r| r.success).count();
            let fail_count = results.len() - success_count;
            pb.finish_with_message(if fail_count == 0 {
                "完成"
            } else {
                "有失败"
            });

            let total_size: usize = results
                .iter()
                .filter(|r| r.success)
                .map(|r| r.output_size)
                .sum();

            let duration = start.elapsed();

            println!("\n╔══════════════════════════════════════════════════════════════╗");
            println!("║                    编译完成                                  ║");
            println!("╠══════════════════════════════════════════════════════════════╣");
            println!(
                "║  成功: {:>6} 个                                             ║",
                success_count
            );
            if fail_count > 0 {
                println!(
                    "║  失败: {:>6} 个                                             ║",
                    fail_count
                );
            }
            println!(
                "║  总大小: {:>6} KB                                           ║",
                total_size / 1024
            );
            println!(
                "║  耗时: {:>6.2}s                                               ║",
                duration.as_secs_f64()
            );
            println!("╚══════════════════════════════════════════════════════════════╝");

            if fail_count > 0 {
                return Err(anyhow::anyhow!("{} 个着色器编译失败", fail_count));
            }
        }

        Commands::CompileVariants {
            source,
            output_dir,
            config,
            parallel,
            bundle,
            compile_args,
        } => {
            if cli.verbose {
                print_banner();
            }
            info!("LSC: 编译着色器变体");
            info!("  源文件: {}", source.display());
            info!("  输出目录: {}", output_dir.display());

            let start = Instant::now();

            // 读取源码
            let source_code = std::fs::read_to_string(&source)?;
            let stage = ShaderStage::from_extension(
                source.extension().and_then(|e| e.to_str()).unwrap_or(""),
            );

            // 解析变体配置
            let variant_config = if let Some(config_path) = config {
                let config_content = std::fs::read_to_string(&config_path)?;
                serde_json::from_str(&config_content)?
            } else {
                VariantParser::parse(&source_code)
            };

            let shader_source = ShaderSource {
                code: source_code,
                file_path: Some(source.clone()),
                stage,
            };

            let options = build_compile_options(&compile_args);

            // 编译变体
            let variant_compiler = VariantCompiler::new()?;
            let variant_bundle = if parallel {
                variant_compiler.compile_variants_parallel(
                    &shader_source,
                    &variant_config,
                    &options,
                )?
            } else {
                variant_compiler.compile_variants(&shader_source, &variant_config, &options)?
            };

            // 创建输出目录
            std::fs::create_dir_all(&output_dir)?;

            // 保存各变体
            for (idx, variant) in variant_bundle.variants.iter().enumerate() {
                let output_name = format!(
                    "{}_{}.spv",
                    source.file_stem().unwrap_or_default().to_string_lossy(),
                    idx
                );
                let output_path = output_dir.join(&output_name);
                std::fs::write(&output_path, &variant.spirv)?;
            }

            // 保存变体包
            if bundle {
                let bundle_path = output_dir.join(format!(
                    "{}.variants.json",
                    source.file_stem().unwrap_or_default().to_string_lossy()
                ));
                variant_bundle.save(&bundle_path)?;
                info!("  变体包: {}", bundle_path.display());
            }

            let duration = start.elapsed();
            println!("\n✓ 变体编译完成");
            println!("  变体数: {}", variant_bundle.variant_count());
            println!("  总大小: {} KB", variant_bundle.total_size() / 1024);
            println!("  耗时: {:.2}s", duration.as_secs_f64());
        }

        Commands::Watch {
            source_dir,
            output_dir,
            debounce,
            compile_args,
        } => {
            if cli.verbose {
                print_banner();
            }
            info!("LSC: 监视着色器变化");
            info!("  监视目录: {}", source_dir.display());

            let options = build_compile_options(&compile_args);
            watch_shaders(&source_dir, &output_dir, &options, debounce)?;
        }

        Commands::Reflect {
            source,
            output,
            format,
            detailed,
        } => {
            info!("LSC: 提取反射信息");
            info!("  源文件: {}", source.display());

            let compiler = ShaderCompiler::new()?;

            // 判断是SPIR-V还是源码
            let is_spirv = source
                .extension()
                .map(|e| e == "spv" || e == "spirv")
                .unwrap_or(false);

            let reflection = if is_spirv {
                let spirv = std::fs::read(&source)?;
                compiler.reflect_spirv(&spirv)?
            } else {
                let source_code = std::fs::read_to_string(&source)?;
                let stage = ShaderStage::from_extension(
                    source.extension().and_then(|e| e.to_str()).unwrap_or(""),
                );
                let shader = ShaderSource {
                    code: source_code,
                    file_path: Some(source.clone()),
                    stage,
                };
                let options = CompileOptions::new();
                let result = compiler.compile(&shader, &options)?;
                result.reflection.unwrap_or_default()
            };

            // 输出
            let output_str = if detailed {
                serde_json::to_string_pretty(&serde_json::json!({
                    "source": source,
                    "format": format,
                    "reflection": reflection,
                }))?
            } else {
                match format.as_str() {
                    "json" => serde_json::to_string_pretty(&reflection)?,
                    _ => serde_json::to_string_pretty(&reflection)?,
                }
            };

            if let Some(output_path) = output {
                std::fs::write(&output_path, &output_str)?;
                println!("✓ 反射信息已保存: {}", output_path.display());
            } else {
                println!("{}", output_str);
            }
        }

        Commands::Validate {
            source,
            strict,
            vulkan_version,
        } => {
            info!("LSC: 验证着色器");

            let compiler = ShaderCompiler::new()?;

            // 目录: 逐个验证其中的着色器。
            //
            // 此前只接受单个文件, 而 compile-all 的同名参数收的是目录 ——
            // 同一个工具里两种语义。传目录时 read_to_string 会以
            // "Access is denied" (os error 5) 失败, CI 的着色器作业因此
            // 一直是红的, 而错误信息完全指不到真正的原因。
            if source.is_dir() {
                let ext_list: Vec<&str> = vec![
                    "vert", "frag", "comp", "geom", "tesc", "tese",
                ];

                let shaders =
                    discover_shaders_with_extensions(&source, &ext_list, true)?;

                if shaders.is_empty() {
                    return Err(anyhow::anyhow!(
                        "目录 {} 下没有找到着色器",
                        source.display()
                    ));
                }

                let mut failed = 0usize;
                let mut warned = 0usize;

                for path in &shaders {
                    let source_code = std::fs::read_to_string(path)?;
                    let stage = ShaderStage::from_extension(
                        path.extension().and_then(|e| e.to_str()).unwrap_or(""),
                    );
                    let shader = ShaderSource {
                        code: source_code,
                        file_path: Some(path.clone()),
                        stage,
                    };
                    let mut options = CompileOptions::new();
                    options.target_environment = vulkan_version
                        .parse()
                        .map_err(|e: String| anyhow::anyhow!(e))?;

                    match compiler.compile(&shader, &options) {
                        Ok(result) => {
                            if result.warnings.is_empty() {
                                println!("  ✓ {}", path.display());
                            } else {
                                warned += result.warnings.len();
                                println!(
                                    "  ⚠ {} — {} 条警告",
                                    path.display(),
                                    result.warnings.len()
                                );
                                for w in &result.warnings {
                                    println!("      {}", w);
                                }
                            }
                        }
                        Err(e) => {
                            failed += 1;
                            println!("  ✗ {}", path.display());
                            println!("      {}", e);
                        }
                    }
                }

                println!();
                println!(
                    "  验证完成: {} 个着色器, {} 个失败, {} 条警告",
                    shaders.len(),
                    failed,
                    warned
                );

                // 编译失败一律致命; 警告只在 --strict 下致命。
                if failed > 0 {
                    return Err(anyhow::anyhow!("{} 个着色器验证失败", failed));
                }

                if strict && warned > 0 {
                    return Err(anyhow::anyhow!(
                        "严格模式下验证失败: {} 条警告",
                        warned
                    ));
                }

                return Ok(());
            }

            let is_spirv = source
                .extension()
                .map(|e| e == "spv" || e == "spirv")
                .unwrap_or(false);

            if is_spirv {
                let spirv = std::fs::read(&source)?;
                let result = compiler.validate_spirv(&spirv)?;
                result.print_summary();
                if !result.valid {
                    return Err(anyhow::anyhow!("SPIR-V 验证失败"));
                }
            } else {
                let source_code = std::fs::read_to_string(&source)?;
                let stage = ShaderStage::from_extension(
                    source.extension().and_then(|e| e.to_str()).unwrap_or(""),
                );
                let shader = ShaderSource {
                    code: source_code,
                    file_path: Some(source.clone()),
                    stage,
                };
                let mut options = CompileOptions::new();
                options.target_environment = vulkan_version
                    .parse()
                    .map_err(|e: String| anyhow::anyhow!(e))?;

                match compiler.compile(&shader, &options) {
                    Ok(result) => {
                        println!("✓ 着色器验证通过");
                        if !result.warnings.is_empty() {
                            println!("\n警告:");
                            for w in &result.warnings {
                                println!("  {}", w);
                            }
                            if strict {
                                return Err(anyhow::anyhow!(
                                    "严格模式下验证失败: {} 条警告",
                                    result.warnings.len()
                                ));
                            }
                        }
                    }
                    Err(e) => {
                        println!("✗ 着色器验证失败:");
                        println!("  {}", e);
                        return Err(e);
                    }
                }
            }
        }

        Commands::Disassemble {
            source,
            output,
            raw_id,
        } => {
            info!("LSC: 反汇编 SPIR-V");
            if raw_id {
                info!("  输出模式: 原始 ID");
            } else {
                info!("  输出模式: 原始 ID (当前内置反汇编器不执行友好名称重写)");
            }

            let compiler = ShaderCompiler::new()?;
            let spirv = std::fs::read(&source)?;
            let disasm = compiler.disassemble_spirv(&spirv)?;

            if let Some(output_path) = output {
                std::fs::write(&output_path, &disasm)?;
                println!("✓ 反汇编输出已保存: {}", output_path.display());
            } else {
                println!("{}", disasm);
            }
        }

        Commands::Cache { action } => match action {
            CacheCommands::Stats { cache_dir } => {
                let dir = cache_dir.unwrap_or_else(default_cache_dir);
                let cache = LruShaderCache::new(dir, LruCacheConfig::default())?;
                cache.extended_stats().print();
            }
            CacheCommands::Clear {
                cache_dir,
                expired_only,
                max_age_days,
            } => {
                let dir = cache_dir.unwrap_or_else(default_cache_dir);
                let mut cache = ShaderCache::load(&dir)?;

                if expired_only {
                    let max_age_secs = max_age_days as u64 * 24 * 60 * 60;
                    cache.clean_expired(max_age_secs);
                    println!("✓ 已清除过期缓存");
                } else {
                    cache.clear();
                    println!("✓ 已清除所有缓存");
                }
                cache.save()?;
            }
            CacheCommands::Warmup { cache_dir, count } => {
                let dir = cache_dir.unwrap_or_else(default_cache_dir);
                let cache = LruShaderCache::new(dir, LruCacheConfig::default())?;
                cache.warm_up(count);
                println!("✓ 已预热 {} 个缓存条目", count);
            }
            CacheCommands::Verify { cache_dir, fix } => {
                let dir = cache_dir.unwrap_or_else(default_cache_dir);
                let mut cache = ShaderCache::load(&dir)?;

                if fix {
                    cache.clean_invalid();
                    cache.save()?;
                    println!("✓ 已修复无效缓存条目");
                } else {
                    let stats = cache.stats();
                    println!("缓存条目: {}", stats.total_entries);
                }
            }
        },

        Commands::Stats { source_dir, format } => {
            info!("LSC: 着色器统计");

            let shaders = discover_shaders(&source_dir)?;

            let mut by_stage: HashMap<ShaderStage, usize> = HashMap::new();
            let mut total_lines = 0usize;
            let mut total_size = 0u64;

            for shader in &shaders {
                let stage = ShaderStage::from_extension(
                    shader.extension().and_then(|e| e.to_str()).unwrap_or(""),
                );
                *by_stage.entry(stage).or_default() += 1;

                if let Ok(content) = std::fs::read_to_string(shader) {
                    total_lines += content.lines().count();
                }
                if let Ok(meta) = std::fs::metadata(shader) {
                    total_size += meta.len();
                }
            }

            if format == "json" {
                let stats = serde_json::json!({
                    "total_files": shaders.len(),
                    "total_lines": total_lines,
                    "total_size_bytes": total_size,
                    "by_stage": by_stage.iter().map(|(k, v)| (format!("{:?}", k), v)).collect::<HashMap<_, _>>()
                });
                println!("{}", serde_json::to_string_pretty(&stats)?);
            } else {
                println!("\n╔══════════════════════════════════════════════════════════════╗");
                println!("║                    着色器统计                                 ║");
                println!("╠══════════════════════════════════════════════════════════════╣");
                println!(
                    "║  总文件数:     {:>6}                                        ║",
                    shaders.len()
                );
                println!(
                    "║  总行数:       {:>6}                                        ║",
                    total_lines
                );
                println!(
                    "║  总大小:       {:>6} KB                                     ║",
                    total_size / 1024
                );
                println!("╠══════════════════════════════════════════════════════════════╣");
                for (stage, count) in &by_stage {
                    println!(
                        "║  {:12}: {:>6} 个                                       ║",
                        format!("{:?}", stage),
                        count
                    );
                }
                println!("╚══════════════════════════════════════════════════════════════╝");
            }
        }

        Commands::Clean {
            output_dir,
            cache,
            dry_run,
        } => {
            info!("LSC: 清理编译输出");

            if output_dir.exists() {
                let count = if dry_run {
                    count_output_files(&output_dir)?
                } else {
                    clean_output_dir(&output_dir)?
                };

                if dry_run {
                    println!("将清理 {} 个文件 (dry-run)", count);
                } else {
                    println!("✓ 已清理 {} 个文件", count);
                }
            } else {
                println!("输出目录不存在");
            }

            if cache {
                let cache_dir = default_cache_dir();
                if cache_dir.exists() {
                    if dry_run {
                        println!("将清理缓存目录: {} (dry-run)", cache_dir.display());
                    } else {
                        std::fs::remove_dir_all(&cache_dir)?;
                        println!("✓ 已清理缓存目录");
                    }
                }
            }
        }

        Commands::GenerateBindings {
            source,
            output,
            language,
            namespace,
        } => {
            info!("LSC: 生成着色器绑定代码");

            let compiler = ShaderCompiler::new()?;
            let mut reflections = Vec::new();

            // 收集反射信息
            if source.is_dir() {
                let shaders = discover_shaders(&source)?;
                for shader in &shaders {
                    let spirv_path = shader.with_extension("spv");
                    if spirv_path.exists() {
                        let spirv = std::fs::read(&spirv_path)?;
                        if let Ok(refl) = compiler.reflect_spirv(&spirv) {
                            reflections.push((shader.clone(), refl));
                        }
                    }
                }
            } else {
                let spirv = std::fs::read(&source)?;
                let refl = compiler.reflect_spirv(&spirv)?;
                reflections.push((source.clone(), refl));
            }

            // 生成绑定代码
            let ns = namespace.unwrap_or_else(|| "Shaders".to_string());
            let code = match language.as_str() {
                "cpp" => generate_cpp_bindings(&reflections, &ns),
                "rust" => generate_rust_bindings(&reflections, &ns),
                _ => return Err(anyhow::anyhow!("不支持的语言: {}", language)),
            };

            std::fs::write(&output, &code)?;
            println!("✓ 绑定代码已生成: {}", output.display());
        }

        // ──────────────────────────────────────────────────────────────
        // 新命令: 清单系统
        // ──────────────────────────────────────────────────────────────
        Commands::CompileManifest {
            manifest,
            force,
            tag,
        } => {
            if cli.verbose {
                print_banner();
            }
            info!("LSC: 编译着色器清单");

            let start = Instant::now();
            let (mut manifest_data, root) = load_manifest(&manifest)?;

            if let Some(tag_filter) = tag.as_deref() {
                manifest_data.shaders.retain(|shader| {
                    !shader.disabled && shader.tags.iter().any(|tag| tag == tag_filter)
                });
                println!("标签过滤: {}", tag_filter);
            }

            let active_count = manifest_data.active_shaders().len();
            println!(
                "清单: {} ({})",
                manifest_data.manifest.name,
                manifest.display()
            );
            println!(
                "着色器: {} 个 (活跃), {} 个变体",
                active_count,
                manifest_data.total_variant_count()
            );

            if active_count == 0 {
                println!("没有匹配的活跃着色器");
                return Ok(());
            }

            // 创建进度 spinner
            let pb = indicatif::ProgressBar::new(active_count as u64);
            pb.set_style(
                indicatif::ProgressStyle::default_bar()
                    .template("{spinner:.green} [{bar:40.cyan/blue}] {pos}/{len} ({percent}%) [{elapsed_precise}] ETA {eta} | {msg}")
                    .unwrap_or_else(|_| indicatif::ProgressStyle::default_bar())
                    .progress_chars("█▓▒░"),
            );
            pb.set_message("编译清单");
            pb.enable_steady_tick(std::time::Duration::from_millis(100));

            let compiler_instance = ManifestCompiler::new()
                .with_force_rebuild(force)
                .with_parallel_jobs(0);

            let result = compiler_instance.compile(&manifest_data, &root)?;

            // 完成进度条
            pb.finish_with_message(if result.all_success() {
                "完成"
            } else {
                "有失败"
            });

            result.print_report();

            if !result.all_success() {
                return Err(anyhow::anyhow!(
                    "清单编译失败: {} 个着色器失败",
                    result.failure_count()
                ));
            }
        }

        Commands::CompileAllManifests { source_dir, force } => {
            if cli.verbose {
                print_banner();
            }
            info!("LSC: 扫描并编译所有清单");

            let manifest_paths = discover_manifests(&source_dir)?;
            if manifest_paths.is_empty() {
                println!(
                    "未在 {} 中找到任何 .limx.shaders 文件",
                    source_dir.display()
                );
                return Ok(());
            }

            println!("发现 {} 个清单文件", manifest_paths.len());

            // 创建整体进度条
            let pb = indicatif::ProgressBar::new(manifest_paths.len() as u64);
            pb.set_style(
                indicatif::ProgressStyle::default_bar()
                    .template("{spinner:.green} [{bar:40.cyan/blue}] {pos}/{len} ({percent}%) [{elapsed_precise}] | {msg}")
                    .unwrap_or_else(|_| indicatif::ProgressStyle::default_bar())
                    .progress_chars("█▓▒░"),
            );
            pb.set_message("编译清单");
            pb.enable_steady_tick(std::time::Duration::from_millis(100));

            let mut total_success = 0usize;
            let mut total_fail = 0usize;

            for manifest_path in &manifest_paths {
                let manifest_name = manifest_path
                    .file_name()
                    .map(|n| n.to_string_lossy().to_string())
                    .unwrap_or_else(|| "unknown".to_string());
                pb.set_message(manifest_name.clone());

                match compile_manifest(manifest_path, force) {
                    Ok(result) => {
                        total_success += result.success_count();
                        total_fail += result.failure_count();
                        if result.all_success() {
                            pb.println(format!(
                                "  ✓ {} ({} 个着色器)",
                                manifest_name,
                                result.success_count()
                            ));
                        } else {
                            pb.println(format!(
                                "  ✗ {} ({} 成功, {} 失败)",
                                manifest_name,
                                result.success_count(),
                                result.failure_count()
                            ));
                        }
                    }
                    Err(e) => {
                        pb.println(format!("  ✗ {} 编译出错: {}", manifest_name, e));
                        total_fail += 1;
                    }
                }
                pb.inc(1);
            }

            pb.finish_with_message(if total_fail == 0 {
                "完成"
            } else {
                "有失败"
            });

            println!("\n总计: {} 成功, {} 失败", total_success, total_fail);
            if total_fail > 0 {
                return Err(anyhow::anyhow!("部分着色器编译失败"));
            }
        }

        Commands::NewManifest { module, output_dir } => {
            if cli.verbose {
                print_banner();
            }

            std::fs::create_dir_all(&output_dir)?;
            let file_name = format!("{}.limx.shaders", module);
            let file_path = output_dir.join(&file_name);

            if file_path.exists() {
                return Err(anyhow::anyhow!("文件已存在: {}", file_path.display()));
            }

            let content = generate_example_manifest(&module);
            std::fs::write(&file_path, &content)?;

            println!("✓ 清单已创建: {}", file_path.display());
            println!("\n下一步:");
            println!("  1. 在 Shaders/ 目录下创建 .vert/.frag/.comp 等着色器文件");
            println!("  2. 编辑 {} 更新着色器列表", file_name);
            println!(
                "  3. 运行 lsc compile-manifest --manifest {} 编译",
                file_path.display()
            );
        }

        // ──────────────────────────────────────────────────────────────
        // 新命令: PSO 缓存管理
        // ──────────────────────────────────────────────────────────────
        Commands::PsoStats { cache } => {
            if cli.verbose {
                print_banner();
            }

            if !cache.exists() {
                println!("PSO 缓存文件不存在: {}", cache.display());
                println!("(在引擎运行后会自动创建)");
                return Ok(());
            }

            let pso_cache =
                PsoCache::load(&cache).map_err(|e| anyhow::anyhow!("加载 PSO 缓存失败: {}", e))?;

            println!("PSO 缓存: {}", cache.display());
            println!("平台:     {}", pso_cache.platform);
            println!("目标环境: {}", pso_cache.target_env);

            let stats = pso_cache.compute_stats();
            stats.print_report();

            println!("\n最常用的 PSO (Top 10):");
            for entry in pso_cache.most_used(10) {
                println!(
                    "  [{:>4}次] [{}] {}",
                    entry.use_count,
                    entry.descriptor.type_name(),
                    entry.tags.join(", ")
                );
            }
        }

        // ──────────────────────────────────────────────────────────────
        // SPIR-V 分析 / 调试注入 / 变体裁剪
        // ──────────────────────────────────────────────────────────────
        Commands::AnalyzeSpirv {
            source,
            output,
            format,
        } => {
            if cli.verbose {
                print_banner();
            }

            let spirv_data = std::fs::read(&source)
                .map_err(|e| anyhow::anyhow!("无法读取 SPIR-V 文件 {}: {}", source.display(), e))?;

            let shader_name = source
                .file_stem()
                .and_then(|s| s.to_str())
                .unwrap_or("unknown")
                .to_string();

            let analyzer = compiler::spirv_optimizer::SpirvOptimizer::new();
            let report = analyzer
                .analyze(&shader_name, &spirv_data)
                .map_err(|e| anyhow::anyhow!("SPIR-V 分析失败: {}", e))?;

            let content = match format.as_str() {
                "json" => serde_json::to_string_pretty(&report).unwrap_or_default(),
                _ => report.to_markdown(),
            };

            if let Some(output_path) = output {
                std::fs::write(&output_path, &content)?;
                println!("✓ 分析报告已保存: {}", output_path.display());
            } else {
                println!("{}", content);
            }

            println!(
                "\n概要: {} 字节, {} 条指令, 优化潜力 {:.0}/100",
                report.spirv_size_bytes,
                report.stats.total_instructions,
                report.optimization_potential
            );
            if !report.hints.is_empty() {
                println!(
                    "  {} 条优化建议 (预估大小减少 {:.1}%)",
                    report.hints.len(),
                    report.estimated_size_reduction_percent
                );
            }
        }

        Commands::InjectDebug {
            source,
            output,
            renderdoc,
            source_map,
        } => {
            if cli.verbose {
                print_banner();
            }

            let spirv_data = std::fs::read(&source)
                .map_err(|e| anyhow::anyhow!("无法读取 SPIR-V 文件 {}: {}", source.display(), e))?;

            let mut injector = compiler::debug_info::DebugInfoInjector::new();

            if renderdoc {
                let shader_name = source
                    .file_stem()
                    .and_then(|s| s.to_str())
                    .unwrap_or("shader");
                injector.set_renderdoc_prefix(shader_name);
            }

            if source_map {
                injector.add_source_mapping(compiler::debug_info::SourceMapping {
                    file_path: source.to_string_lossy().to_string(),
                    line: 1,
                    column: 1,
                    instruction_offset: None,
                });
            }

            // 先分析现有调试信息
            let existing_stats = injector
                .analyze_existing(&spirv_data)
                .map_err(|e| anyhow::anyhow!("分析调试信息失败: {}", e))?;
            println!("现有调试信息:");
            println!(
                "  OpName: {}, OpLine: {}, 命名覆盖率: {:.1}%",
                existing_stats.name_count,
                existing_stats.line_count,
                existing_stats.naming_coverage_percent()
            );

            // 执行注入
            let (enhanced_spirv, result) = injector
                .inject(&spirv_data)
                .map_err(|e| anyhow::anyhow!("注入调试信息失败: {}", e))?;

            std::fs::write(&output, &enhanced_spirv)?;

            println!("\n✓ 调试信息已注入: {}", output.display());
            println!("  原大小: {} 字节", result.original_size);
            println!(
                "  新大小: {} 字节 (+{})",
                result.new_size, result.size_increase
            );
            println!(
                "  注入 OpName: {}, OpLine: {}, OpString: {}",
                result.names_injected, result.lines_injected, result.strings_injected
            );
        }

        Commands::PruneVariants {
            config,
            usage_data,
            output,
            format,
        } => {
            if cli.verbose {
                print_banner();
            }

            let config_content = std::fs::read_to_string(&config)
                .map_err(|e| anyhow::anyhow!("无法读取变体配置 {}: {}", config.display(), e))?;

            // 从 JSON 配置解析维度和规则
            let variant_config: serde_json::Value = serde_json::from_str(&config_content)
                .map_err(|e| anyhow::anyhow!("变体配置 JSON 解析失败: {}", e))?;

            let mut pruner = compiler::variant_pruner::VariantPruner::new();

            // 解析维度
            if let Some(dims) = variant_config.get("dimensions").and_then(|v| v.as_array()) {
                for dim_val in dims {
                    let name = dim_val
                        .get("name")
                        .and_then(|v| v.as_str())
                        .unwrap_or("unknown");
                    let desc = dim_val
                        .get("description")
                        .and_then(|v| v.as_str())
                        .unwrap_or("");
                    let is_bool = dim_val
                        .get("boolean")
                        .and_then(|v| v.as_bool())
                        .unwrap_or(false);

                    if is_bool {
                        pruner.add_dimension(compiler::variant_pruner::VariantDimension::boolean(
                            name, desc,
                        ));
                    } else if let Some(values) = dim_val.get("values").and_then(|v| v.as_array()) {
                        let vals: Vec<&str> = values.iter().filter_map(|v| v.as_str()).collect();
                        pruner.add_dimension(compiler::variant_pruner::VariantDimension::multi(
                            name, &vals, desc,
                        ));
                    }
                }
            }

            // 解析互斥规则
            if let Some(rules) = variant_config
                .get("exclusion_rules")
                .and_then(|v| v.as_array())
            {
                for rule_val in rules {
                    let name = rule_val
                        .get("name")
                        .and_then(|v| v.as_str())
                        .unwrap_or("unnamed");
                    let reason = rule_val
                        .get("reason")
                        .and_then(|v| v.as_str())
                        .unwrap_or("");
                    let conditions: Vec<(String, String)> = rule_val
                        .get("conditions")
                        .and_then(|v| v.as_object())
                        .map(|obj| {
                            obj.iter()
                                .map(|(k, v)| (k.clone(), v.as_str().unwrap_or("").to_string()))
                                .collect()
                        })
                        .unwrap_or_default();

                    pruner.add_exclusion_rule(compiler::variant_pruner::MutualExclusionRule {
                        name: name.to_string(),
                        conditions,
                        reason: reason.to_string(),
                    });
                }
            }

            // 加载使用频率数据 (可选)
            let usage_record = if let Some(usage_path) = usage_data {
                let usage_json = std::fs::read_to_string(&usage_path).map_err(|e| {
                    anyhow::anyhow!("无法读取使用频率数据 {}: {}", usage_path.display(), e)
                })?;
                let record: compiler::variant_pruner::UsageRecord =
                    serde_json::from_str(&usage_json)
                        .map_err(|e| anyhow::anyhow!("使用频率数据解析失败: {}", e))?;
                Some(record)
            } else {
                None
            };

            let shader_name = config
                .file_stem()
                .and_then(|s| s.to_str())
                .unwrap_or("shader");

            let report = pruner.analyze(shader_name, usage_record.as_ref());

            let content = match format.as_str() {
                "json" => serde_json::to_string_pretty(&report).unwrap_or_default(),
                _ => report.to_markdown(),
            };

            if let Some(output_path) = output {
                std::fs::write(&output_path, &content)?;
                println!("✓ 裁剪报告已保存: {}", output_path.display());
            } else {
                println!("{}", content);
            }

            println!(
                "\n概要: {} 个组合中裁剪 {} 个 ({:.1}%)",
                report.total_combinations, report.pruned_combinations, report.prune_rate_percent
            );
            println!(
                "  保留: {}, 规则裁剪: {}, 频率裁剪: {}, 从未使用: {}",
                report.kept_combinations,
                report.pruned_by_rule,
                report.pruned_by_frequency,
                report.never_used
            );
            if report.estimated_time_saved_seconds > 0.0 {
                println!(
                    "  预估节省: {:.1}s 编译时间, {:.1}MB 磁盘空间",
                    report.estimated_time_saved_seconds, report.estimated_space_saved_mb
                );
            }
        }

        Commands::PsoMerge {
            inputs,
            output,
            purge_stale,
        } => {
            if cli.verbose {
                print_banner();
            }

            if inputs.is_empty() {
                return Err(anyhow::anyhow!("必须指定至少一个输入缓存文件"));
            }

            let mut merged =
                PsoCache::load(&inputs[0]).unwrap_or_else(|_| PsoCache::new("win64", "vulkan1.3"));

            for input in inputs.iter().skip(1) {
                match PsoCache::load(input) {
                    Ok(other) => {
                        let before = merged.entries.len();
                        merged.merge(&other);
                        let added = merged.entries.len() - before;
                        println!("合并 {} → +{} 条目", input.display(), added);
                    }
                    Err(e) => {
                        eprintln!("警告: 无法加载 {} — {}", input.display(), e);
                    }
                }
            }

            if purge_stale {
                let purged = merged.purge_stale();
                println!("清除过期条目: {} 个", purged);
            }

            merged.save(&output)?;

            let stats = merged.compute_stats();
            println!("\n✓ PSO 缓存已合并: {}", output.display());
            println!("  总条目: {}", stats.total_entries);
            println!("  有效条目: {}", stats.valid_entries);
        }

        // ──────────────────────────────────────────────────────────────
        // 着色器 #include 依赖图
        // ──────────────────────────────────────────────────────────────
        Commands::IncludeGraph {
            source_dir,
            format,
            output,
            detect_cycles,
        } => {
            if cli.verbose {
                print_banner();
            }
            println!("构建着色器 #include 依赖图...");

            let shaders = discover_shaders(&source_dir)?;
            let mut graph = compiler::include_graph::ShaderIncludeGraph::new();

            // 扫描所有着色器文件，建立依赖图
            for shader_path in &shaders {
                let source = std::fs::read_to_string(shader_path)?;
                let rel_path = shader_path
                    .strip_prefix(&source_dir)
                    .unwrap_or(shader_path)
                    .to_string_lossy()
                    .to_string();

                let is_entry = shader_path
                    .extension()
                    .and_then(|e| e.to_str())
                    .map(|e| matches!(e, "vert" | "frag" | "comp" | "geom" | "tesc" | "tese"))
                    .unwrap_or(false);

                let stage_str = shader_path
                    .extension()
                    .and_then(|e| e.to_str())
                    .map(|e| e.to_string());

                graph.add_file(compiler::include_graph::ShaderFileInfo {
                    path: rel_path.clone(),
                    content_hash: String::new(),
                    is_entry_shader: is_entry,
                    stage: if is_entry { stage_str } else { None },
                });

                let includes =
                    compiler::include_graph::ShaderIncludeGraph::scan_includes_from_source(&source);
                for inc in &includes {
                    graph.add_include(&rel_path, inc);
                }
            }

            let (file_count, edge_count, entry_count) = graph.stats();

            // 循环检测
            if detect_cycles {
                let cycles = graph.detect_cycles();
                if cycles.is_empty() {
                    println!("✓ 未检测到循环包含");
                } else {
                    println!("⚠ 检测到 {} 个循环包含:", cycles.len());
                    for (i, cycle) in cycles.iter().enumerate() {
                        println!("  {}. {}", i + 1, cycle.join(" → "));
                    }
                }
            }

            // 输出格式化
            let output_content = match format.as_str() {
                "dot" => graph.to_dot(),
                "json" => serde_json::to_string_pretty(&graph.stats()).unwrap_or_else(|_| {
                    format!(
                        "{{\"files\":{},\"edges\":{},\"entries\":{}}}",
                        file_count, edge_count, entry_count
                    )
                }),
                _ => {
                    let mut text = String::new();
                    text.push_str(&format!("着色器文件: {}\n", file_count));
                    text.push_str(&format!("包含关系: {}\n", edge_count));
                    text.push_str(&format!("入口着色器: {}\n", entry_count));
                    text
                }
            };

            if let Some(out_path) = output {
                std::fs::write(&out_path, &output_content)?;
                println!("✓ 依赖图已保存: {}", out_path.display());
            } else {
                println!("{}", output_content);
            }
        }

        // ──────────────────────────────────────────────────────────────
        // 着色器性能静态分析
        // ──────────────────────────────────────────────────────────────
        Commands::PerfAnalyze {
            source,
            format,
            output,
        } => {
            if cli.verbose {
                print_banner();
            }
            println!("着色器性能静态分析...");

            let analyzer = compiler::perf_analyzer::ShaderPerfAnalyzer::new();
            let mut reports: Vec<compiler::perf_analyzer::PerfReport> = Vec::new();

            if source.is_dir() {
                let shaders = discover_shaders(&source)?;
                for shader_path in &shaders {
                    let shader_source = std::fs::read_to_string(shader_path)?;
                    let name = shader_path
                        .file_name()
                        .map(|n| n.to_string_lossy().to_string())
                        .unwrap_or_else(|| "unknown".to_string());
                    let report = analyzer.analyze_source(&name, &shader_source);
                    reports.push(report);
                }
            } else {
                let shader_source = std::fs::read_to_string(&source)?;
                let name = source
                    .file_name()
                    .map(|n| n.to_string_lossy().to_string())
                    .unwrap_or_else(|| "unknown".to_string());
                let report = analyzer.analyze_source(&name, &shader_source);
                reports.push(report);
            }

            let output_content = match format.as_str() {
                "json" => serde_json::to_string_pretty(&reports).unwrap_or_default(),
                _ => {
                    let mut text = String::new();
                    for report in &reports {
                        text.push_str(&format!(
                            "\n╔══════════════════════════════════════════════════════════════╗\n"
                        ));
                        text.push_str(&format!("║  着色器: {:50} ║\n", report.shader_name));
                        text.push_str(&format!(
                            "╠══════════════════════════════════════════════════════════════╣\n"
                        ));
                        text.push_str(&format!(
                            "║  源码行数: {:>6}                                          ║\n",
                            report.source_lines
                        ));
                        text.push_str(&format!(
                            "║  ALU 指令: {:>6}  纹理采样: {:>6}                         ║\n",
                            report.instructions.alu_count, report.instructions.texture_count
                        ));
                        text.push_str(&format!(
                            "║  ALU:TEX: {:>8.1}                                        ║\n",
                            report.instructions.alu_tex_ratio()
                        ));
                        if !report.warnings.is_empty() {
                            text.push_str(&format!(
                                "║  性能警告: {} 条                                            ║\n",
                                report.warnings.len()
                            ));
                            for w in &report.warnings {
                                text.push_str(&format!(
                                    "║    [{:?}] {}                                 \n",
                                    w.severity, w.message
                                ));
                            }
                        }
                        text.push_str(&format!(
                            "╚══════════════════════════════════════════════════════════════╝\n"
                        ));
                    }
                    text
                }
            };

            if let Some(out_path) = output {
                std::fs::write(&out_path, &output_content)?;
                println!("✓ 性能报告已保存: {}", out_path.display());
            } else {
                print!("{}", output_content);
            }

            println!("\n分析完成: {} 个着色器", reports.len());
        }

        // ──────────────────────────────────────────────────────────────
        // 跨阶段接口验证
        // ──────────────────────────────────────────────────────────────
        Commands::ValidatePipeline {
            shaders,
            push_constant_limit,
        } => {
            if cli.verbose {
                print_banner();
            }
            println!("跨阶段接口验证...");

            let mut validator = compiler::stage_interface::StageInterfaceValidator::new();
            validator.set_push_constant_limit(push_constant_limit);

            for shader_path in &shaders {
                let source = std::fs::read_to_string(shader_path)?;
                let ext = shader_path
                    .extension()
                    .and_then(|e| e.to_str())
                    .unwrap_or("");

                let stage = match ext {
                    "vert" | "vs" => compiler::stage_interface::ShaderStageType::Vertex,
                    "frag" | "ps" => compiler::stage_interface::ShaderStageType::Fragment,
                    "comp" | "cs" => compiler::stage_interface::ShaderStageType::Compute,
                    "geom" | "gs" => compiler::stage_interface::ShaderStageType::Geometry,
                    "tesc" => compiler::stage_interface::ShaderStageType::TessControl,
                    "tese" => compiler::stage_interface::ShaderStageType::TessEval,
                    "task" => compiler::stage_interface::ShaderStageType::Task,
                    "mesh" => compiler::stage_interface::ShaderStageType::Mesh,
                    _ => {
                        eprintln!("跳过未知阶段: {}", shader_path.display());
                        continue;
                    }
                };

                let interface = compiler::stage_interface::parse_stage_interface(&source, stage);
                println!(
                    "  阶段: {} ({}) — 输入: {} 输出: {}",
                    stage.display_name(),
                    shader_path.display(),
                    interface.inputs.len(),
                    interface.outputs.len()
                );
                validator.add_stage(stage, interface);
            }

            let report = validator.validate_pipeline();

            if report.is_ok() {
                println!("\n✓ 管线接口验证通过");
            } else {
                println!("\n管线接口验证结果:");
                println!("  错误: {}", report.error_count);
                println!("  警告: {}", report.warning_count);
                for diag in &report.diagnostics {
                    println!("  [{:?}] {}", diag.severity, diag.message);
                }
                if report.error_count > 0 {
                    std::process::exit(1);
                }
            }
        }
    }

    Ok(())
}

/// 从 CompileArgs 构建编译选项
fn build_compile_options(args: &CompileArgs) -> CompileOptions {
    let mut options = CompileOptions::new();

    options.optimization_level = if args.optimize {
        compiler::OptimizationLevel::Performance
    } else {
        compiler::OptimizationLevel::None
    };
    options.generate_debug_info = args.debug_info;
    options.target_environment = args.parse_vulkan_version();
    options.generate_reflection = args.reflection;

    for (name, value) in args.parse_defines() {
        options.defines.push((name, value));
    }

    for dir in &args.include_dirs {
        options.include_dirs.push(dir.clone());
    }

    options
}

/// 发现着色器文件
fn discover_shaders(dir: &PathBuf) -> Result<Vec<PathBuf>> {
    let mut shaders = Vec::new();

    let extensions = [
        "vert", "frag", "comp", "geom", "tesc", "tese", "glsl", "hlsl", "vs", "ps", "cs", "gs",
    ];

    for entry in walkdir::WalkDir::new(dir)
        .into_iter()
        .filter_map(|e| e.ok())
    {
        let path = entry.path();
        if path.is_file() {
            if let Some(ext) = path.extension().and_then(|e| e.to_str()) {
                if extensions.contains(&ext) {
                    shaders.push(path.to_path_buf());
                }
            }
        }
    }

    Ok(shaders)
}

/// 发现指定扩展名的着色器文件
fn discover_shaders_with_extensions(
    dir: &PathBuf,
    extensions: &[&str],
    recursive: bool,
) -> Result<Vec<PathBuf>> {
    let mut shaders = Vec::new();

    let walker = if recursive {
        walkdir::WalkDir::new(dir)
    } else {
        walkdir::WalkDir::new(dir).max_depth(1)
    };

    for entry in walker.into_iter().filter_map(|e| e.ok()) {
        let path = entry.path();
        if path.is_file() {
            if let Some(ext) = path.extension().and_then(|e| e.to_str()) {
                if extensions.contains(&ext) {
                    shaders.push(path.to_path_buf());
                }
            }
        }
    }

    Ok(shaders)
}

/// 监视着色器变化
fn watch_shaders(
    source_dir: &PathBuf,
    output_dir: &PathBuf,
    options: &CompileOptions,
    debounce_ms: u64,
) -> Result<()> {
    use notify::{recommended_watcher, RecursiveMode, Watcher};
    use std::sync::mpsc::channel;

    let (tx, rx) = channel();

    let mut watcher = recommended_watcher(move |res| {
        if let Ok(event) = res {
            let _ = tx.send(event);
        }
    })?;

    watcher.watch(source_dir.as_path(), RecursiveMode::Recursive)?;

    println!("正在监视: {}", source_dir.display());
    println!("按 Ctrl+C 停止\n");

    let compiler = ShaderCompiler::new()?;

    loop {
        match rx.recv() {
            Ok(event) => {
                if let notify::EventKind::Modify(_) = event.kind {
                    if debounce_ms > 0 {
                        std::thread::sleep(std::time::Duration::from_millis(debounce_ms));
                    }

                    let mut changed_paths = event.paths;
                    while let Ok(extra_event) = rx.try_recv() {
                        if let notify::EventKind::Modify(_) = extra_event.kind {
                            changed_paths.extend(extra_event.paths);
                        }
                    }

                    changed_paths.sort();
                    changed_paths.dedup();

                    for path in changed_paths {
                        if is_shader_file(&path) {
                            println!("检测到变化: {}", path.display());

                            // 重新编译
                            if let Err(e) = recompile_shader(&compiler, &path, output_dir, options)
                            {
                                error!("编译失败: {}", e);
                            }
                        }
                    }
                }
            }
            Err(e) => {
                error!("监视错误: {}", e);
                break;
            }
        }
    }

    Ok(())
}

fn is_shader_file(path: &PathBuf) -> bool {
    let extensions = [
        "vert", "frag", "comp", "geom", "tesc", "tese", "glsl", "hlsl",
    ];
    path.extension()
        .and_then(|e| e.to_str())
        .map(|e| extensions.contains(&e))
        .unwrap_or(false)
}

fn recompile_shader(
    compiler: &ShaderCompiler,
    source: &PathBuf,
    output_dir: &PathBuf,
    options: &CompileOptions,
) -> Result<()> {
    let source_code = std::fs::read_to_string(source)?;
    let stage =
        ShaderStage::from_extension(source.extension().and_then(|e| e.to_str()).unwrap_or(""));

    let shader = ShaderSource {
        code: source_code,
        file_path: Some(source.clone()),
        stage,
    };

    let result = compiler.compile(&shader, options)?;

    // 生成输出路径
    let file_name = source.file_stem().unwrap_or_default();
    let output_path = output_dir.join(format!("{}.spv", file_name.to_string_lossy()));

    std::fs::create_dir_all(output_dir)?;
    std::fs::write(&output_path, &result.spirv_binary)?;

    println!(
        "✓ 已编译: {} -> {}",
        source.display(),
        output_path.display()
    );

    Ok(())
}

fn clean_output_dir(dir: &PathBuf) -> Result<usize> {
    let mut count = 0;

    for entry in walkdir::WalkDir::new(dir)
        .into_iter()
        .filter_map(|e| e.ok())
    {
        let path = entry.path();
        if path.is_file() {
            if let Some(ext) = path.extension().and_then(|e| e.to_str()) {
                if ext == "spv" || ext == "spirv" || ext == "json" {
                    std::fs::remove_file(path)?;
                    count += 1;
                }
            }
        }
    }

    Ok(count)
}

/// 统计输出文件数量（用于dry-run）
fn count_output_files(dir: &PathBuf) -> Result<usize> {
    let mut count = 0;

    for entry in walkdir::WalkDir::new(dir)
        .into_iter()
        .filter_map(|e| e.ok())
    {
        let path = entry.path();
        if path.is_file() {
            if let Some(ext) = path.extension().and_then(|e| e.to_str()) {
                if ext == "spv" || ext == "spirv" || ext == "json" {
                    count += 1;
                }
            }
        }
    }

    Ok(count)
}

/// 生成C++绑定代码
fn generate_cpp_bindings(
    reflections: &[(PathBuf, crate::reflection::ShaderReflection)],
    namespace: &str,
) -> String {
    let mut code = String::new();

    code.push_str(&format!(
        r#"/*******************************************************************************
 * 文件: ShaderBindings.h
 * 
 * 警告: 此文件由 LSC (Limx Shader Compiler) 自动生成
 *       请勿手动修改
 *
 ******************************************************************************/

#pragma once

#include <cstdint>
#include <array>

namespace {} {{

"#,
        namespace
    ));

    for (path, reflection) in reflections {
        let shader_name = path
            .file_stem()
            .and_then(|s| s.to_str())
            .unwrap_or("Unknown")
            .to_string()
            .replace("-", "_")
            .replace(".", "_");

        code.push_str(&format!("// Shader: {}\n", path.display()));
        code.push_str(&format!("namespace {} {{\n", shader_name));

        // Uniform Buffers
        for ub in &reflection.uniform_buffers {
            code.push_str(&format!(
                "    // Uniform Buffer: {} (set={}, binding={})\n",
                ub.name, ub.set, ub.binding
            ));
            code.push_str(&format!("    struct {} {{\n", ub.name));
            for member in &ub.members {
                code.push_str(&format!(
                    "        {} {};  // offset: {}\n",
                    cpp_type_name(&member.data_type),
                    member.name,
                    member.offset
                ));
            }
            code.push_str("    };\n\n");
        }

        // Push Constants
        if let Some(pc) = &reflection.push_constants {
            code.push_str(&format!(
                "    // Push Constants (offset={}, size={})\n",
                pc.offset, pc.size
            ));
            code.push_str("    struct PushConstants {\n");
            for member in &pc.members {
                code.push_str(&format!(
                    "        {} {};  // offset: {}\n",
                    cpp_type_name(&member.data_type),
                    member.name,
                    member.offset
                ));
            }
            code.push_str("    };\n\n");
        }

        // Binding Constants
        code.push_str("    struct Bindings {\n");
        for ub in &reflection.uniform_buffers {
            code.push_str(&format!(
                "        static constexpr uint32_t {}Set = {};\n",
                ub.name, ub.set
            ));
            code.push_str(&format!(
                "        static constexpr uint32_t {}Binding = {};\n",
                ub.name, ub.binding
            ));
        }
        for tex in &reflection.textures {
            code.push_str(&format!(
                "        static constexpr uint32_t {}Set = {};\n",
                tex.name, tex.set
            ));
            code.push_str(&format!(
                "        static constexpr uint32_t {}Binding = {};\n",
                tex.name, tex.binding
            ));
        }
        for sampler in &reflection.samplers {
            code.push_str(&format!(
                "        static constexpr uint32_t {}Set = {};\n",
                sampler.name, sampler.set
            ));
            code.push_str(&format!(
                "        static constexpr uint32_t {}Binding = {};\n",
                sampler.name, sampler.binding
            ));
        }
        code.push_str("    };\n");

        code.push_str(&format!("}} // namespace {}\n\n", shader_name));
    }

    code.push_str(&format!("}} // namespace {}\n", namespace));
    code
}

/// 生成Rust绑定代码
fn generate_rust_bindings(
    reflections: &[(PathBuf, crate::reflection::ShaderReflection)],
    module_name: &str,
) -> String {
    let mut code = String::new();

    code.push_str(&format!(
        r#"//! 着色器绑定
//! 
//! 警告: 此文件由 LSC (Limx Shader Compiler) 自动生成
//!       请勿手动修改

#![allow(dead_code)]

pub mod {} {{

"#,
        module_name.to_lowercase()
    ));

    for (path, reflection) in reflections {
        let shader_name = path
            .file_stem()
            .and_then(|s| s.to_str())
            .unwrap_or("unknown")
            .to_string()
            .replace("-", "_")
            .replace(".", "_")
            .to_lowercase();

        code.push_str(&format!("    /// Shader: {}\n", path.display()));
        code.push_str(&format!("    pub mod {} {{\n", shader_name));

        // Uniform Buffers
        for ub in &reflection.uniform_buffers {
            code.push_str(&format!(
                "        /// Uniform Buffer (set={}, binding={})\n",
                ub.set, ub.binding
            ));
            code.push_str("        #[repr(C)]\n");
            code.push_str("        #[derive(Clone, Copy, Debug, Default)]\n");
            code.push_str(&format!("        pub struct {} {{\n", ub.name));
            for member in &ub.members {
                code.push_str(&format!(
                    "            pub {}: {},  // offset: {}\n",
                    member.name.to_lowercase(),
                    rust_type_name(&member.data_type),
                    member.offset
                ));
            }
            code.push_str("        }\n\n");
        }

        // Push Constants
        if let Some(pc) = &reflection.push_constants {
            code.push_str(&format!(
                "        /// Push Constants (offset={}, size={})\n",
                pc.offset, pc.size
            ));
            code.push_str("        #[repr(C)]\n");
            code.push_str("        #[derive(Clone, Copy, Debug, Default)]\n");
            code.push_str("        pub struct PushConstants {\n");
            for member in &pc.members {
                code.push_str(&format!(
                    "            pub {}: {},  // offset: {}\n",
                    member.name.to_lowercase(),
                    rust_type_name(&member.data_type),
                    member.offset
                ));
            }
            code.push_str("        }\n\n");
        }

        // Binding Constants
        code.push_str("        pub mod bindings {\n");
        for ub in &reflection.uniform_buffers {
            code.push_str(&format!(
                "            pub const {}_SET: u32 = {};\n",
                ub.name.to_uppercase(),
                ub.set
            ));
            code.push_str(&format!(
                "            pub const {}_BINDING: u32 = {};\n",
                ub.name.to_uppercase(),
                ub.binding
            ));
        }
        for tex in &reflection.textures {
            code.push_str(&format!(
                "            pub const {}_SET: u32 = {};\n",
                tex.name.to_uppercase(),
                tex.set
            ));
            code.push_str(&format!(
                "            pub const {}_BINDING: u32 = {};\n",
                tex.name.to_uppercase(),
                tex.binding
            ));
        }
        for sampler in &reflection.samplers {
            code.push_str(&format!(
                "            pub const {}_SET: u32 = {};\n",
                sampler.name.to_uppercase(),
                sampler.set
            ));
            code.push_str(&format!(
                "            pub const {}_BINDING: u32 = {};\n",
                sampler.name.to_uppercase(),
                sampler.binding
            ));
        }
        code.push_str("        }\n");

        code.push_str(&format!("    }} // mod {}\n\n", shader_name));
    }

    code.push_str(&format!("}} // mod {}\n", module_name.to_lowercase()));
    code
}

/// 将着色器数据类型转换为C++类型名
fn cpp_type_name(data_type: &crate::reflection::DataType) -> &'static str {
    use crate::reflection::BaseType;
    match (&data_type.base_type, data_type.columns, data_type.vec_size) {
        (BaseType::Float, 1, 1) => "float",
        (BaseType::Float, 1, 2) => "glm::vec2",
        (BaseType::Float, 1, 3) => "glm::vec3",
        (BaseType::Float, 1, 4) => "glm::vec4",
        (BaseType::Float, 4, 4) => "glm::mat4",
        (BaseType::Float, 3, 3) => "glm::mat3",
        (BaseType::Int, 1, 1) => "int32_t",
        (BaseType::Int, 1, 2) => "glm::ivec2",
        (BaseType::Int, 1, 3) => "glm::ivec3",
        (BaseType::Int, 1, 4) => "glm::ivec4",
        (BaseType::UInt, 1, 1) => "uint32_t",
        (BaseType::UInt, 1, 2) => "glm::uvec2",
        (BaseType::UInt, 1, 3) => "glm::uvec3",
        (BaseType::UInt, 1, 4) => "glm::uvec4",
        (BaseType::Bool, 1, 1) => "bool",
        _ => "/* unknown */",
    }
}

/// 将着色器数据类型转换为Rust类型名
fn rust_type_name(data_type: &crate::reflection::DataType) -> &'static str {
    use crate::reflection::BaseType;
    match (&data_type.base_type, data_type.columns, data_type.vec_size) {
        (BaseType::Float, 1, 1) => "f32",
        (BaseType::Float, 1, 2) => "[f32; 2]",
        (BaseType::Float, 1, 3) => "[f32; 3]",
        (BaseType::Float, 1, 4) => "[f32; 4]",
        (BaseType::Float, 4, 4) => "[[f32; 4]; 4]",
        (BaseType::Float, 3, 3) => "[[f32; 3]; 3]",
        (BaseType::Int, 1, 1) => "i32",
        (BaseType::Int, 1, 2) => "[i32; 2]",
        (BaseType::Int, 1, 3) => "[i32; 3]",
        (BaseType::Int, 1, 4) => "[i32; 4]",
        (BaseType::UInt, 1, 1) => "u32",
        (BaseType::UInt, 1, 2) => "[u32; 2]",
        (BaseType::UInt, 1, 3) => "[u32; 3]",
        (BaseType::UInt, 1, 4) => "[u32; 4]",
        (BaseType::Bool, 1, 1) => "bool",
        _ => "/* unknown */",
    }
}
