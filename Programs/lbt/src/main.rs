/*******************************************************************************
 * 文件: main.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   LBT (Limx Build Tool) 入口点
 *   - 模块发现：扫描 *.limx.toml 文件
 *   - 依赖解析：构建 DAG，验证层级约束
 *   - CMake 生成：输出模块化 CMakeLists.txt
 *
 ******************************************************************************/

// 允许已实现但尚未集成的模块代码
#![allow(dead_code)]
#![allow(unused_imports)]
#![allow(unused_variables)]

mod checker;
mod cli;
mod compiler;
mod core;
mod generators;
mod integration;

use anyhow::Result;
use clap::Parser;
use std::hash::{Hash, Hasher};
use tracing::info;
use tracing_subscriber::{layer::SubscriberExt, util::SubscriberInitExt, EnvFilter};

use crate::cli::{
    print_banner, print_phase, BuildProgress, BuildReport, Cli, ColoredOutput, Commands,
};
use crate::compiler::{Compiler, Linker};
use crate::core::{
    dependency, discover_plugins, discover_targets, discovery, generate_example_plugin,
    generate_example_target, BuildCache, BuildConfig, BuildPhase, ModuleValidator,
    PerformanceMonitor, TargetType,
};
use crate::generators::{api, cmake, generate_compile_commands, ide, module, vs};

/// 展开字符串中的 `${ENV_VAR}` 为对应环境变量值。
/// 如果环境变量不存在，保留原始 `${...}` 文本并打印警告。
fn expand_env_vars(input: &str) -> String {
    let mut result = String::with_capacity(input.len());
    let mut chars = input.chars().peekable();
    while let Some(ch) = chars.next() {
        if ch == '$' {
            if chars.peek() == Some(&'{') {
                chars.next(); // 消耗 '{'
                let mut var_name = String::new();
                let mut found_close = false;
                for inner in chars.by_ref() {
                    if inner == '}' {
                        found_close = true;
                        break;
                    }
                    var_name.push(inner);
                }
                if found_close && !var_name.is_empty() {
                    match std::env::var(&var_name) {
                        Ok(val) => result.push_str(&val),
                        Err(_) => {
                            eprintln!("  ⚠ 环境变量 ${{{0}}} 未设置", var_name);
                            result.push_str(&format!("${{{}}}", var_name));
                        }
                    }
                } else {
                    result.push('$');
                    result.push('{');
                    result.push_str(&var_name);
                }
            } else {
                result.push(ch);
            }
        } else {
            result.push(ch);
        }
    }
    result
}

fn tool_executable_name(tool: &str) -> String {
    if cfg!(windows) && !tool.ends_with(".exe") {
        format!("{}.exe", tool)
    } else {
        tool.to_string()
    }
}

fn find_tool_executable(tool: &str) -> std::path::PathBuf {
    let exe_name = tool_executable_name(tool);

    if let Ok(current_exe) = std::env::current_exe() {
        if let Some(dir) = current_exe.parent() {
            let candidate = dir.join(&exe_name);
            if candidate.exists() {
                return candidate;
            }
        }
    }

    let cwd = std::env::current_dir().unwrap_or_else(|_| std::path::PathBuf::from("."));
    let candidates = [
        cwd.join("Programs")
            .join("target")
            .join("release")
            .join(&exe_name),
        cwd.join("Programs")
            .join("target")
            .join("debug")
            .join(&exe_name),
        cwd.join("target").join("release").join(&exe_name),
        cwd.join("target").join("debug").join(&exe_name),
    ];

    for candidate in candidates {
        if candidate.exists() {
            return candidate;
        }
    }

    std::path::PathBuf::from(exe_name)
}

fn object_file_for_source(
    source: &std::path::Path,
    intermediate_dir: &std::path::Path,
) -> std::path::PathBuf {
    let file_stem = source
        .file_stem()
        .map(|s| s.to_string_lossy().to_string())
        .unwrap_or_else(|| "unknown".to_string());
    let mut hasher = std::collections::hash_map::DefaultHasher::new();
    source
        .to_string_lossy()
        .replace('\\', "/")
        .hash(&mut hasher);
    let path_hash = hasher.finish();
    intermediate_dir.join(format!("{}_{:016x}.obj", file_stem, path_hash))
}

fn run_lht_reflection_generation(
    source_dir: &std::path::Path,
    output_dir: &std::path::Path,
    verbose: bool,
) -> Result<()> {
    print_phase("生成反射代码 (LHT)");
    std::fs::create_dir_all(output_dir)?;

    let lht_path = find_tool_executable("lht");
    println!("  LHT: {}", lht_path.display());
    println!("  输入: {}", source_dir.display());
    println!("  输出: {}", output_dir.display());

    let mut cmd = std::process::Command::new(&lht_path);
    cmd.arg("generate")
        .arg("--source-dir")
        .arg(source_dir)
        .arg("--output-dir")
        .arg(output_dir)
        .arg("--incremental");

    if verbose {
        println!("  命令: {:?}", cmd);
    }

    let status = cmd
        .status()
        .map_err(|e| anyhow::anyhow!("无法启动 LHT: {} ({})", lht_path.display(), e))?;

    if !status.success() {
        return Err(anyhow::anyhow!("LHT 反射代码生成失败: {}", status));
    }

    Ok(())
}

fn collect_reflection_generated_sources(
    output_dir: &std::path::Path,
    modules: &[crate::core::config::Module],
    verbose: bool,
) -> Result<std::collections::HashMap<String, Vec<std::path::PathBuf>>> {
    let mut stem_to_module: std::collections::HashMap<String, Option<String>> =
        std::collections::HashMap::new();

    for module in modules {
        for header in discovery::collect_module_headers(&module.path) {
            let Some(stem) = header.file_stem().map(|s| s.to_string_lossy().to_string()) else {
                continue;
            };

            match stem_to_module.entry(stem) {
                std::collections::hash_map::Entry::Vacant(entry) => {
                    entry.insert(Some(module.name.clone()));
                }
                std::collections::hash_map::Entry::Occupied(mut entry) => {
                    if entry.get().as_deref() != Some(module.name.as_str()) {
                        entry.insert(None);
                    }
                }
            }
        }
    }

    let mut generated_by_module: std::collections::HashMap<String, Vec<std::path::PathBuf>> =
        std::collections::HashMap::new();

    if !output_dir.is_dir() {
        return Ok(generated_by_module);
    }

    for entry in walkdir::WalkDir::new(output_dir)
        .into_iter()
        .filter_map(|entry| entry.ok())
    {
        let path = entry.path();
        if !path.is_file() {
            continue;
        }

        let Some(file_name) = path.file_name().and_then(|name| name.to_str()) else {
            continue;
        };
        let Some(header_stem) = file_name.strip_suffix(".generated.cpp") else {
            continue;
        };

        match stem_to_module.get(header_stem) {
            Some(Some(module_name)) => {
                generated_by_module
                    .entry(module_name.clone())
                    .or_default()
                    .push(path.to_path_buf());
            }
            Some(None) => {
                return Err(anyhow::anyhow!(
                    "LHT 生成源文件无法归属模块，头文件名重复: {}",
                    file_name
                ));
            }
            None => {
                if verbose {
                    println!("  跳过未归属的 LHT 生成源文件: {}", path.display());
                }
            }
        }
    }

    for sources in generated_by_module.values_mut() {
        sources.sort();
    }

    Ok(generated_by_module)
}

fn compile_project_shaders(
    project_root: &std::path::Path,
    build_config: compiler::BuildConfiguration,
    parallel_jobs: usize,
    verbose: bool,
) -> Result<()> {
    let shader_dir = project_root.join("Shaders");
    if !shader_dir.is_dir() {
        return Ok(());
    }

    print_phase("编译着色器 (LSC)");

    let output_dir = project_root.join("Binaries").join("Shaders");
    std::fs::create_dir_all(&output_dir)?;

    let lsc_path = find_tool_executable("lsc");
    println!("  LSC: {}", lsc_path.display());
    println!("  输入: {}", shader_dir.display());
    println!("  输出: {}", output_dir.display());

    let mut cmd = std::process::Command::new(&lsc_path);
    cmd.arg("compile-all")
        .arg("--source-dir")
        .arg(&shader_dir)
        .arg("--output-dir")
        .arg(&output_dir)
        .arg("--jobs")
        .arg(parallel_jobs.to_string());

    if build_config.name() != "Debug" {
        cmd.arg("--optimize");
    }
    if verbose {
        cmd.arg("--verbose");
    }

    let status = cmd.status().map_err(|e| {
        anyhow::anyhow!(
            "无法启动 LSC: {} ({})。请先构建工具链或将 lsc 加入 PATH。",
            lsc_path.display(),
            e
        )
    })?;

    if !status.success() {
        return Err(anyhow::anyhow!(
            "LSC 着色器编译失败: {}",
            status
                .code()
                .map_or_else(|| "terminated".to_string(), |c| c.to_string())
        ));
    }

    Ok(())
}

fn main() -> Result<()> {
    // 初始化日志 - 同时输出到控制台和文件
    let log_dir = std::path::PathBuf::from("Logs");
    std::fs::create_dir_all(&log_dir).ok();

    let file_appender = tracing_appender::rolling::daily(&log_dir, "lbt.log");
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
        Commands::Generate {
            source_dir,
            output_dir,
            platform,
            config,
        } => {
            print_banner();
            print_phase("生成项目配置");

            let progress = BuildProgress::new();
            let mut monitor = PerformanceMonitor::new();
            let errors = 0;
            let warnings = 0;

            // 1. 加载构建配置
            let build_config = BuildConfig::load_from_dir(&source_dir).unwrap_or_default();
            info!("项目: {}", build_config.project_name);

            // 2. 发现模块
            monitor.start_phase(BuildPhase::Discovery);
            let spinner = progress.discovery_spinner("扫描模块...");
            let modules = discovery::discover_modules(&source_dir)?;
            spinner.finish_with_message(format!("发现 {} 个模块", modules.len()));
            monitor.end_current_phase();

            // 3. 解析依赖
            monitor.start_phase(BuildPhase::DependencyResolution);
            let spinner = progress.discovery_spinner("解析依赖...");
            let graph = dependency::resolve_dependencies(&modules)?;
            spinner.finish_with_message("依赖解析完成");
            monitor.end_current_phase();

            // 4. 生成 CMake
            monitor.start_phase(BuildPhase::CmakeGeneration);
            let pb = progress.build_progress(modules.len() as u64, "生成 CMake");
            cmake::generate_cmake(&graph, &output_dir, &source_dir, &platform, &config)?;
            pb.inc(modules.len() as u64 / 2);

            // 5. 生成 API 头文件
            api::generate_all_api_headers(&modules, &output_dir)?;
            pb.finish_with_message("生成完成");
            monitor.end_current_phase();

            // 打印报告
            let report = BuildReport {
                modules_built: modules.len(),
                modules_skipped: 0,
                errors,
                warnings,
                duration: progress.elapsed(),
            };
            report.print();
            monitor.print_report();

            println!(
                "\n{}",
                ColoredOutput::success(&format!("输出目录: {}", output_dir.display()))
            );
        }

        Commands::NewModule {
            name,
            layer,
            source_dir,
        } => {
            print_banner();
            print_phase("创建新模块");
            println!("  模块名: {}", ColoredOutput::module(&name));
            println!("  层级: {}", ColoredOutput::number(layer));

            discovery::create_module(&source_dir, &name, layer)?;
            println!("\n{}", ColoredOutput::success("模块创建完成"));
        }

        Commands::List { source_dir } => {
            print_banner();
            print_phase("模块列表");

            let progress = BuildProgress::new();
            let spinner = progress.discovery_spinner("扫描模块...");
            let modules = discovery::discover_modules(&source_dir)?;
            spinner.finish_and_clear();

            println!(
                "\n{:<20} {:<8} {:<10} {:<30}",
                ColoredOutput::title("名称"),
                ColoredOutput::title("层级"),
                ColoredOutput::title("类型"),
                ColoredOutput::title("路径")
            );
            println!("{}", "-".repeat(70));
            for module in &modules {
                println!(
                    "{:<20} {:<8} {:<10} {}",
                    ColoredOutput::module(&module.name),
                    ColoredOutput::number(module.layer),
                    format!("{:?}", module.module_type),
                    ColoredOutput::path(&module.path.display().to_string())
                );
            }
            println!("\n共 {} 个模块", ColoredOutput::number(modules.len()));
        }

        Commands::Check {
            source_dir,
            analyze,
        } => {
            print_banner();

            // ── 阶段 1: 模块配置验证 ──
            print_phase("检查模块配置");

            let progress = BuildProgress::new();
            let spinner = progress.discovery_spinner("检查中...");

            let modules = discovery::discover_modules(&source_dir)?;
            let graph = dependency::resolve_dependencies(&modules)?;
            dependency::validate_layer_constraints(&graph)?;

            let mut validator = ModuleValidator::new();
            let config_result = validator.validate_all(&modules);

            spinner.finish_and_clear();

            if config_result.is_valid() {
                println!("  ✓ 模块配置正确 ({} 个模块)", modules.len());
                println!("  ✓ 无循环依赖");
                println!("  ✓ 层级约束满足");
            } else {
                config_result.print_report();
            }

            // ── 阶段 2: Limx 源码 lint ──
            print_phase("检查源码规范");

            let checker = crate::checker::LimxChecker::new();
            let report = checker.check_directory(&source_dir)?;

            // 输出诊断
            for diag in &report.diagnostics {
                let severity_color = match diag.severity {
                    crate::checker::CheckSeverity::Error => "\x1b[91m", // 红
                    crate::checker::CheckSeverity::Warning => "\x1b[93m", // 黄
                    crate::checker::CheckSeverity::Note => "\x1b[96m",  // 青
                };
                println!(
                    "  {}{}:\x1b[0m {}:{} [{}] {}",
                    severity_color,
                    diag.severity,
                    diag.file.display(),
                    diag.line,
                    diag.rule_id,
                    diag.message,
                );
                if !diag.snippet.is_empty() {
                    println!("    → {}", diag.snippet);
                }
            }

            // 汇总
            println!();
            println!(
                "  检查完成: {} 个文件, {} 行",
                report.files_checked, report.lines_checked,
            );

            let err_count = report.error_count();
            let warn_count = report.warning_count();
            let note_count = report.note_count();

            if err_count > 0 {
                println!(
                    "  \x1b[91m✗ {} 个错误\x1b[0m, {} 个警告, {} 个提示",
                    err_count, warn_count, note_count,
                );
            } else if warn_count > 0 {
                println!(
                    "  \x1b[93m⚠ {} 个警告\x1b[0m, {} 个提示",
                    warn_count, note_count,
                );
            } else {
                println!("  {}", ColoredOutput::success("✓ 源码规范检查通过"),);
            }

            // ── 阶段 3: MSVC /analyze (可选) ──
            if analyze {
                print_phase("MSVC 静态分析 (/analyze)");
                println!("  [TODO] MSVC /analyze + /Zs 深度分析模式");
                println!("  此功能将在后续版本中实现");
            }

            // Error 级别的发现必须让进程以非零退出。
            //
            // 在此之前这里只打印不返回, 于是 `lbt check` 无论发现多少
            // 错误都退出 0 —— verify.ps1 的"源码规则"那一步因此从未真正
            // 拦住过任何东西, 整套规则在 CI 里只是装饰。
            //
            // 警告与提示仍然不致命: 它们是建议, 不是约定。
            // 源码规则的 Error 与模块配置的 Error 都要让进程非零退出。
            //
            // 原先只看 err_count (源码规则那一半), 而 config_result 只被用来
            // 选打印分支 —— 于是"重复的模块名"之类的配置错误打印出 ❌ 却
            // 退出 0。verify.ps1 紧接着跑的 validate --strict 恰好也能抓到
            // 这一类, 所以整条流水线没漏; 但单独跑 lbt check 的人会被骗,
            // 而这个子命令的名字正是在邀请人单独跑它。
            if err_count > 0 || !config_result.is_valid()
            {
                std::process::exit(1);
            }
        }

        Commands::GenerateProject {
            source_dir,
            project_dir,
            ide,
        } => {
            info!("LBT: 生成 IDE 项目文件");
            info!("  源目录: {}", source_dir.display());
            info!("  项目目录: {}", project_dir.display());
            info!("  目标 IDE: {}", ide);

            let modules = discovery::discover_modules(&source_dir)?;
            let graph = dependency::resolve_dependencies(&modules)?;

            let ide_type = match ide.to_lowercase().as_str() {
                "vs" | "visualstudio" => ide::IdeType::VisualStudio,
                "vscode" | "code" => ide::IdeType::VSCode,
                "rider" => ide::IdeType::Rider,
                "clion" => ide::IdeType::CLion,
                _ => ide::IdeType::All,
            };

            ide::generate_project_files(&graph, &modules, &project_dir, ide_type)?;
            info!("项目文件生成完成");

            println!("\n✓ 项目文件已生成:");
            println!("  - CMakePresets.json");
            println!("  - .vscode/ (settings, tasks, launch)");
            println!("  - Build.bat / Build.ps1");
            println!("\n下一步:");
            println!("  1. 用 VS/VSCode/Rider 打开项目");
            println!("  2. 运行 ./Build.ps1 或选择构建配置");
        }

        Commands::Build {
            source_dir,
            config,
            compiler: compiler_type,
            jobs,
            pch,
            unity,
            generate_only,
            rebuild,
            skip_shaders,
            verbose,
        } => {
            print_banner();
            print_phase("构建项目");

            let build_config = match config.as_str() {
                "debug" => compiler::BuildConfiguration::Debug,
                "release" => compiler::BuildConfiguration::Release,
                "shipping" => compiler::BuildConfiguration::Shipping,
                _ => compiler::BuildConfiguration::Development,
            };

            let parallel_jobs = if jobs == 0 { num_cpus::get() } else { jobs };

            println!("  配置: {}", ColoredOutput::module(build_config.name()));
            println!("  编译器: {}", ColoredOutput::module(&compiler_type));
            println!("  并行任务: {}", ColoredOutput::number(parallel_jobs));
            if pch {
                println!("  PCH: {}", ColoredOutput::success("启用"));
            }
            if unity {
                println!("  Unity Build: {}", ColoredOutput::success("启用"));
            }
            if skip_shaders {
                println!("  着色器: {}", ColoredOutput::warning("跳过"));
            }

            // 1. 发现模块
            let progress = BuildProgress::new();
            let spinner = progress.discovery_spinner("扫描模块...");
            let modules = discovery::discover_modules(&source_dir)?;
            spinner.finish_and_clear();
            println!("  发现 {} 个模块", ColoredOutput::number(modules.len()));

            if modules.is_empty() {
                println!("\n{}", ColoredOutput::warning("没有找到模块"));
                return Ok(());
            }

            // 2. 解析依赖
            let graph = dependency::resolve_dependencies(&modules)?;
            let project_root = source_dir
                .parent()
                .unwrap_or(std::path::Path::new("."))
                .to_path_buf();
            let reflection_output = project_root
                .join("Intermediate")
                .join("Generated")
                .join("Reflection");

            if generate_only {
                let output_dir = project_root.join("Intermediate").join("Generated");
                std::fs::create_dir_all(&output_dir)?;
                api::generate_all_api_headers(&modules, &output_dir)?;
                run_lht_reflection_generation(&source_dir, &reflection_output, verbose)?;
                println!("\n{}", ColoredOutput::success("生成完成 (仅生成模式)"));
                return Ok(());
            }

            run_lht_reflection_generation(&source_dir, &reflection_output, verbose)?;
            let generated_reflection_sources =
                collect_reflection_generated_sources(&reflection_output, &modules, verbose)?;

            // 3. 加载动作缓存
            let cache_dir = std::path::PathBuf::from("Intermediate/ActionCache");
            std::fs::create_dir_all(&cache_dir)?;
            let action_cache = compiler::action::ActionCache::load(&cache_dir)
                .unwrap_or_else(|_| compiler::action::ActionCache::new(cache_dir.clone()));

            // 3.5 加载内容寻址编译缓存
            let compile_cache_config = compiler::compile_cache::CacheConfig {
                cache_dir: std::path::PathBuf::from("Intermediate/CompileCache"),
                ..Default::default()
            };
            let compile_cache =
                compiler::compile_cache::CompileCache::load_or_create(compile_cache_config);
            println!("  编译缓存: {} 个已有条目", compile_cache.entry_count());

            // 4. 检测编译器并创建调度器
            print_phase("检测工具链");

            let (comp, link): (
                std::sync::Arc<dyn compiler::Compiler>,
                std::sync::Arc<dyn compiler::Linker>,
            ) = match compiler_type.to_lowercase().as_str() {
                "clang" => {
                    let c = compiler::clang::ClangCompiler::detect()
                        .map_err(|e| anyhow::anyhow!("无法检测 Clang: {}", e))?;
                    let l = compiler::clang::LldLinker::detect()
                        .map_err(|e| anyhow::anyhow!("无法检测 LLD 链接器: {}", e))?;
                    println!("  编译器: Clang {}", c.version());
                    (std::sync::Arc::new(c), std::sync::Arc::new(l))
                }
                "gcc" => {
                    let c = compiler::gcc::GccCompiler::detect()
                        .map_err(|e| anyhow::anyhow!("无法检测 GCC: {}", e))?;
                    let l = compiler::gcc::GnuLinker::detect()
                        .map_err(|e| anyhow::anyhow!("无法检测 GNU 链接器: {}", e))?;
                    println!("  编译器: GCC {}", c.version());
                    (std::sync::Arc::new(c), std::sync::Arc::new(l))
                }
                _ => {
                    let c = compiler::msvc::MsvcCompiler::detect()
                        .map_err(|e| anyhow::anyhow!("无法检测 MSVC: {}", e))?;
                    let l = compiler::msvc::MsvcLinker::detect()
                        .map_err(|e| anyhow::anyhow!("无法检测 MSVC 链接器: {}", e))?;
                    println!("  编译器: MSVC {}", c.version());
                    (std::sync::Arc::new(c), std::sync::Arc::new(l))
                }
            };

            // 5. 配置编译器和链接器
            let compiler_config = compiler::CompilerConfig {
                configuration: build_config,
                architecture: compiler::Architecture::X64,
                platform: compiler::Platform::Windows,
                enable_rtti: false,
                enable_exceptions: false,
                multithreaded_runtime: true,
                dynamic_runtime: true,
                ..Default::default()
            };

            let linker_config = compiler::LinkerConfig {
                configuration: build_config,
                architecture: compiler::Architecture::X64,
                platform: compiler::Platform::Windows,
                debug_info: build_config.has_debug_info(),
                incremental: !rebuild,
                ..Default::default()
            };

            // 6. 创建调度器
            let mut scheduler = compiler::scheduler::BuildScheduler::new(
                comp.clone(),
                link.clone(),
                compiler_config.clone(),
                linker_config.clone(),
            );
            scheduler.parallel_jobs(parallel_jobs);

            // 7. 构建上下文
            let ctx = compiler::BuildContext::new(project_root.clone());

            if !skip_shaders {
                compile_project_shaders(&project_root, build_config, parallel_jobs, verbose)?;
            }

            // 8. Unity Build 处理
            let mut unity_sources: std::collections::HashMap<String, Vec<std::path::PathBuf>> =
                std::collections::HashMap::new();

            if unity {
                print_phase("Unity Build 准备");
                let mut unity_config = compiler::unity::UnityBuildConfig::default();
                unity_config.output_dir = std::path::PathBuf::from("Intermediate/Unity");
                std::fs::create_dir_all(&unity_config.output_dir)?;

                let mut unity_gen = compiler::unity::UnityBuildGenerator::new(unity_config);

                for module in &modules {
                    let sources = discovery::collect_module_sources(&module.path);
                    if sources.len() >= 5 {
                        if let Ok(unity_files) = unity_gen.generate(&sources) {
                            if !unity_files.is_empty() {
                                let unity_paths: Vec<_> =
                                    unity_files.iter().map(|f| f.path.clone()).collect();
                                println!(
                                    "  [{}] {} 源文件 → {} Unity 文件",
                                    ColoredOutput::module(&module.name),
                                    sources.len(),
                                    unity_paths.len()
                                );
                                unity_sources.insert(module.name.clone(), unity_paths);
                            }
                        }
                    }
                }
            }

            // 9. PCH 处理
            let mut pch_files: std::collections::HashMap<String, std::path::PathBuf> =
                std::collections::HashMap::new();

            if pch {
                print_phase("PCH 准备");
                for module in &modules {
                    let pch_header = module.path.join("Private").join("PCH.h");
                    if pch_header.exists() {
                        let pch_output = ctx.module_intermediate_dir(&module.name).join("PCH.pch");

                        // 检查 PCH 是否需要重建
                        let need_rebuild = rebuild || !pch_output.exists() || {
                            let pch_time = std::fs::metadata(&pch_output)
                                .ok()
                                .and_then(|m| m.modified().ok());
                            let header_time = std::fs::metadata(&pch_header)
                                .ok()
                                .and_then(|m| m.modified().ok());
                            match (pch_time, header_time) {
                                (Some(pt), Some(ht)) => ht > pt,
                                _ => true,
                            }
                        };

                        if need_rebuild {
                            println!(
                                "  [{}] 创建 PCH: {}",
                                ColoredOutput::module(&module.name),
                                pch_header.display()
                            );

                            // 添加 PCH 创建任务
                            let _pch_task =
                                scheduler.add_create_pch(pch_header.clone(), pch_output.clone());
                        }

                        pch_files.insert(module.name.clone(), pch_output);
                    }
                }
            }

            // 10. 收集编译任务
            print_phase("准备编译任务");
            let mut module_compile_tasks: std::collections::HashMap<String, Vec<usize>> =
                std::collections::HashMap::new();
            // 记录每个模块预期的 .obj 文件路径（含缓存恢复的）
            let mut module_expected_objects: std::collections::HashMap<
                String,
                Vec<std::path::PathBuf>,
            > = std::collections::HashMap::new();
            let mut total_sources = 0;
            let mut skipped_cached = 0;

            // 头文件依赖解析器 — 跨模块共享以复用已扫描文件的内容哈希。
            // 缓存键必须包含头依赖哈希, 否则修改头文件后缓存仍会命中,
            // 复用的 .obj 基于旧的类布局, 与新编译的 TU 混合链接会造成
            // ODR 违规与堆损坏 (实测表现为运行期 0xC0000374)。
            let mut include_resolver = compiler::deps::IncludeResolver::new();

            // 源文件 -> 缓存键。构建后回填缓存时必须使用与查询时完全相同的键,
            // 因此这里记录下来而不是二次计算, 避免两处逻辑漂移导致缓存永不命中。
            let mut source_cache_keys: std::collections::HashMap<
                std::path::PathBuf,
                compiler::compile_cache::CacheKey,
            > = std::collections::HashMap::new();

            for module in &modules {
                let module_name = &module.name;
                let module_path = &module.path;

                // External 模块无源码编译 — 仅向依赖方提供头文件路径和链接库
                if module.module_type == crate::core::config::ModuleType::External {
                    continue;
                }

                // 使用 Unity 源文件或原始源文件
                let mut sources = if let Some(unity_files) = unity_sources.get(module_name) {
                    unity_files.clone()
                } else {
                    discovery::collect_module_sources(module_path)
                };
                if let Some(generated_sources) = generated_reflection_sources.get(module_name) {
                    sources.extend(generated_sources.iter().cloned());
                }

                if sources.is_empty() {
                    continue;
                }

                let intermediate_dir = ctx.module_intermediate_dir(module_name);
                std::fs::create_dir_all(&intermediate_dir)?;

                let mut task_ids = Vec::new();
                let mut expected_objects = Vec::new();

                // ------------------------------------------------------------
                // 模块级头文件搜索路径 — 同一模块内所有源文件共用, 循环外算一次。
                // 既作为编译命令的 /I 参数, 也作为缓存键解析头依赖的搜索基准,
                // 两者共用同一份数据可确保依赖解析与实际编译看到相同的头文件。
                // ------------------------------------------------------------
                let mut module_include_dirs: Vec<std::path::PathBuf> = Vec::new();
                module_include_dirs.push(module_path.join("Public"));
                module_include_dirs.push(module_path.join("Private"));
                if !module_include_dirs.contains(&reflection_output) {
                    module_include_dirs.push(reflection_output.clone());
                }

                // External 依赖向下传播的预处理器定义
                let mut module_external_defines: Vec<String> = Vec::new();

                for dep_module in &modules {
                    if dep_module.name != *module_name {
                        if dep_module.module_type == crate::core::config::ModuleType::External {
                            // External 模块: 传播其 include_paths 和 defines 给依赖方
                            for inc_path in &dep_module.config.compile.include_paths {
                                let expanded = expand_env_vars(inc_path);
                                let path = std::path::PathBuf::from(expanded);
                                if !module_include_dirs.contains(&path) {
                                    module_include_dirs.push(path);
                                }
                            }
                            for ext_def in &dep_module.config.compile.defines {
                                if !module_external_defines.contains(ext_def) {
                                    module_external_defines.push(ext_def.clone());
                                }
                            }
                        } else {
                            module_include_dirs.push(dep_module.path.join("Public"));
                        }
                    }
                }

                // 模块 [compile] 配置中的额外包含路径 (支持 ${ENV} 展开)
                for extra_inc in &module.config.compile.include_paths {
                    let expanded = expand_env_vars(extra_inc);
                    let inc_path = std::path::PathBuf::from(&expanded);
                    if !module_include_dirs.contains(&inc_path) {
                        module_include_dirs.push(inc_path);
                    }
                }

                for source in &sources {
                    let object_file = object_file_for_source(source, &intermediate_dir);

                    // 内容寻址缓存键 — 即使 --rebuild 也要计算,
                    // 构建结束后需用同一个键把新产物写回缓存
                    if let Ok(source_hash) = compiler::compile_cache::hash_file(source) {
                        let cache_key_input = compiler::compile_cache::CacheKeyInput {
                            source_hash,
                            compiler_fingerprint: format!("{}_{}", comp.name(), comp.version()),
                            // 递归包含的全部第一方头文件内容哈希
                            dependencies_hash: include_resolver
                                .dependencies_hash(source, &module_include_dirs),
                            target_platform: format!(
                                "{}_{}",
                                compiler_config.platform.name(),
                                compiler_config.architecture.name()
                            ),
                            build_configuration: build_config.name().to_string(),
                        };
                        let cache_key =
                            compiler::compile_cache::compute_cache_key(&cache_key_input);

                        source_cache_keys.insert(source.clone(), cache_key.clone());

                        // 缓存命中检查 (非强制重编译时)
                        if !rebuild && compile_cache.lookup(&cache_key).is_some() {
                            // 缓存命中 — 从缓存恢复产物文件
                            match compile_cache.restore_artifact(&cache_key, &object_file) {
                                Ok(true) => {
                                    skipped_cached += 1;
                                    expected_objects.push(object_file);
                                    continue;
                                }
                                _ => {} // 恢复失败则重新编译
                            }
                        }
                    }

                    let mut unit = compiler::CompileUnit::new(
                        source.clone(),
                        object_file.clone(),
                        module_name,
                    );

                    // 包含目录 — 复用循环外预计算的模块级搜索路径,
                    // 与缓存键解析头依赖时所用的路径集完全一致
                    unit.include_dirs = module_include_dirs.clone();

                    // External 依赖传播下来的预处理器定义
                    for ext_def in &module_external_defines {
                        if !unit.defines.contains(ext_def) {
                            unit.defines.push(ext_def.clone());
                        }
                    }

                    // 添加模块 [compile] 配置中的额外预处理器定义
                    for extra_def in &module.config.compile.defines {
                        if !unit.defines.contains(extra_def) {
                            unit.defines.push(extra_def.clone());
                        }
                    }

                    // 添加模块 [compile] 配置中的额外编译标志
                    for extra_flag in &module.config.compile.flags {
                        unit.extra_flags.push(extra_flag.clone());
                    }

                    // 添加 API 可见性定义
                    if let Some(api_macro) = &module.config.module.api_macro {
                        if !api_macro.is_empty() {
                            let api_prefix = api_macro.replace("_API", "");
                            if module.module_type == crate::core::config::ModuleType::Static {
                                // 静态库: 定义 _STATIC 使 API 宏展开为空
                                unit.defines.push(format!("{}_STATIC", api_prefix));
                            } else {
                                // 动态库: 定义 _EXPORTS 使 API 宏展开为 dllexport
                                unit.defines.push(format!("{}_EXPORTS", api_prefix));
                            }
                        }
                    }

                    // 为所有依赖的静态库模块定义 _STATIC 宏，
                    // 确保消费者包含其头文件时 API 宏展开为空而非 dllimport
                    for dep_module in &modules {
                        if dep_module.name != *module_name
                            && dep_module.module_type == crate::core::config::ModuleType::Static
                        {
                            if let Some(dep_api) = &dep_module.config.module.api_macro {
                                if !dep_api.is_empty() {
                                    let dep_prefix = dep_api.replace("_API", "");
                                    let static_def = format!("{}_STATIC", dep_prefix);
                                    if !unit.defines.contains(&static_def) {
                                        unit.defines.push(static_def);
                                    }
                                }
                            }
                        }
                    }

                    // PCH 支持
                    if let Some(pch_file) = pch_files.get(module_name) {
                        unit.use_pch = Some(pch_file.clone());
                    }

                    expected_objects.push(object_file);
                    let task_id = scheduler.add_compile(unit);
                    task_ids.push(task_id);
                    total_sources += 1;
                }

                module_compile_tasks.insert(module_name.clone(), task_ids);
                module_expected_objects.insert(module_name.clone(), expected_objects);
            }

            println!(
                "  总任务: {} 个编译, {} 个缓存跳过",
                ColoredOutput::number(total_sources),
                ColoredOutput::number(skipped_cached)
            );

            // 9. 添加链接任务
            let output_dir = ctx.output_dir.join(build_config.name()).join("Win64");
            std::fs::create_dir_all(&output_dir)?;

            // 构建模块名→模块配置的映射，用于依赖解析
            let module_config_map: std::collections::HashMap<String, &crate::core::config::Module> =
                modules.iter().map(|m| (m.name.clone(), m)).collect();

            // 递归收集模块的所有依赖 (公开+私有，含传递依赖)
            fn collect_all_deps(
                module_name: &str,
                module_map: &std::collections::HashMap<String, &crate::core::config::Module>,
                visited: &mut std::collections::HashSet<String>,
            ) {
                let deps = if let Some(m) = module_map.get(module_name) {
                    let mut all = m.config.dependencies.public.clone();
                    all.extend(m.config.dependencies.private.clone());
                    all
                } else {
                    return;
                };
                for dep in deps {
                    if visited.insert(dep.clone()) {
                        collect_all_deps(&dep, module_map, visited);
                    }
                }
            }

            // 记录每个模块的链接任务 ID，用于建立跨模块链接依赖
            let mut module_link_tasks: std::collections::HashMap<String, usize> =
                std::collections::HashMap::new();

            for module in &modules {
                let module_name = &module.name;

                // External 模块无链接目标 — 其 library_paths/libraries 通过依赖收集传播
                if module.module_type == crate::core::config::ModuleType::External {
                    continue;
                }

                let intermediate_dir = ctx.module_intermediate_dir(module_name);

                let (target_type, extension) = match module.module_type {
                    crate::core::config::ModuleType::Shared => {
                        (compiler::TargetType::DynamicLibrary, "dll")
                    }
                    crate::core::config::ModuleType::Static => {
                        (compiler::TargetType::StaticLibrary, "lib")
                    }
                    crate::core::config::ModuleType::Executable => {
                        (compiler::TargetType::Executable, "exe")
                    }
                    _ => (compiler::TargetType::DynamicLibrary, "dll"),
                };

                let output_file = output_dir.join(format!("{}.{}", module_name, extension));

                // 使用预期的 .obj 路径（编译任务产出 + 缓存恢复），而非扫描磁盘
                // 磁盘扫描在首次构建时会得到空列表，因为编译尚未执行
                let module_objects = module_expected_objects
                    .get(module_name)
                    .cloned()
                    .unwrap_or_default();

                if module_objects.is_empty() {
                    continue;
                }

                // 静态库使用 lib.exe (add_static_lib)，动态库/可执行文件使用 link.exe (add_link)
                let link_task_id = if target_type == compiler::TargetType::StaticLibrary {
                    scheduler.add_static_lib(module_objects, output_file.clone())
                } else {
                    let mut link_unit = compiler::LinkUnit::new(output_file.clone(), target_type);
                    link_unit.object_files = module_objects;
                    link_unit.dynamic_libs =
                        vec!["kernel32.lib".to_string(), "user32.lib".to_string()];

                    // 收集所有依赖模块 (递归传递)
                    let mut all_deps = std::collections::HashSet::new();
                    collect_all_deps(module_name, &module_config_map, &mut all_deps);

                    // 将依赖模块的 .lib 文件添加到 static_libs
                    for dep_name in &all_deps {
                        if let Some(dep_module) = module_config_map.get(dep_name.as_str()) {
                            if dep_module.module_type == crate::core::config::ModuleType::Static {
                                let dep_lib = output_dir.join(format!("{}.lib", dep_name));
                                link_unit.static_libs.push(dep_lib);
                            }
                            // 收集依赖模块的库搜索路径和链接库
                            for lib_path in &dep_module.config.compile.library_paths {
                                let expanded = expand_env_vars(lib_path);
                                let path = std::path::PathBuf::from(expanded);
                                if !link_unit.lib_dirs.contains(&path) {
                                    link_unit.lib_dirs.push(path);
                                }
                            }
                            for lib in &dep_module.config.compile.libraries {
                                let lib_file = if lib.ends_with(".lib") {
                                    lib.clone()
                                } else {
                                    format!("{}.lib", lib)
                                };
                                if !link_unit.dynamic_libs.contains(&lib_file) {
                                    link_unit.dynamic_libs.push(lib_file);
                                }
                            }
                        }
                    }

                    // 添加当前模块 [compile] 配置中的库搜索路径和链接库
                    for lib_path in &module.config.compile.library_paths {
                        let expanded = expand_env_vars(lib_path);
                        let path = std::path::PathBuf::from(expanded);
                        if !link_unit.lib_dirs.contains(&path) {
                            link_unit.lib_dirs.push(path);
                        }
                    }
                    for lib in &module.config.compile.libraries {
                        let lib_file = if lib.ends_with(".lib") {
                            lib.clone()
                        } else {
                            format!("{}.lib", lib)
                        };
                        if !link_unit.dynamic_libs.contains(&lib_file) {
                            link_unit.dynamic_libs.push(lib_file);
                        }
                    }

                    link_unit.debug_info = build_config.has_debug_info();
                    link_unit.incremental = !rebuild;
                    scheduler.add_link(link_unit)
                };

                module_link_tasks.insert(module_name.clone(), link_task_id);

                // 链接任务依赖本模块编译任务
                if let Some(compile_tasks) = module_compile_tasks.get(module_name) {
                    for &compile_task in compile_tasks {
                        scheduler.add_dependency(link_task_id, compile_task).ok();
                    }
                }
            }

            // 第二遍：所有链接任务已创建，现在添加跨模块链接依赖
            // 必须在单独的循环中执行，否则如果依赖模块排在当前模块之后，
            // module_link_tasks 中尚无其 task ID，依赖就会丢失 (LNK1104 竞态根因)
            for module in &modules {
                let module_name = &module.name;
                if module.module_type == crate::core::config::ModuleType::External {
                    continue;
                }
                let target_type = match module.module_type {
                    crate::core::config::ModuleType::Shared => compiler::TargetType::DynamicLibrary,
                    crate::core::config::ModuleType::Static => compiler::TargetType::StaticLibrary,
                    crate::core::config::ModuleType::Executable => compiler::TargetType::Executable,
                    _ => compiler::TargetType::DynamicLibrary,
                };
                if target_type == compiler::TargetType::StaticLibrary {
                    continue;
                }
                let link_task_id = match module_link_tasks.get(module_name.as_str()) {
                    Some(&id) => id,
                    None => continue,
                };
                let mut all_deps = std::collections::HashSet::new();
                collect_all_deps(module_name, &module_config_map, &mut all_deps);
                for dep_name in &all_deps {
                    if let Some(&dep_link_task) = module_link_tasks.get(dep_name.as_str()) {
                        scheduler.add_dependency(link_task_id, dep_link_task).ok();
                    }
                    if let Some(dep_compile_tasks) = module_compile_tasks.get(dep_name.as_str()) {
                        for &compile_task in dep_compile_tasks {
                            scheduler.add_dependency(link_task_id, compile_task).ok();
                        }
                    }
                }
            }

            // 10. 执行构建
            print_phase("执行构建");
            let pb = progress.build_progress(scheduler.task_count() as u64, "构建");

            // 连接进度回调 — 实时更新进度条位置和当前任务
            {
                let pb_progress = pb.clone();
                scheduler.on_progress(Box::new(move |prog| {
                    let done = prog.completed_tasks + prog.failed_tasks + prog.skipped_tasks;
                    pb_progress.set_position(done as u64);

                    // 显示当前正在执行的任务
                    if let Some(active) = prog.active_tasks.first() {
                        pb_progress.set_message(active.clone());
                    }
                }));
            }

            // 连接任务完成回调 — 实时输出每个文件的编译/链接状态
            {
                let pb_task = pb.clone();
                let verbose_output = verbose;
                scheduler.on_task_complete(Box::new(move |_task_id, result| {
                    use crate::compiler::scheduler::TaskResult;
                    match result {
                        TaskResult::CompileSuccess(r) => {
                            if verbose_output {
                                let file_name = r
                                    .output_file
                                    .as_ref()
                                    .and_then(|p| p.file_name())
                                    .map(|n| n.to_string_lossy().to_string())
                                    .unwrap_or_else(|| "unknown".to_string());
                                pb_task.println(format!(
                                    "  {} {} ({}ms)",
                                    ColoredOutput::success(""),
                                    file_name,
                                    r.duration_ms
                                ));
                            }
                        }
                        TaskResult::CompileFailure(r) => {
                            let file_name = r
                                .output_file
                                .as_ref()
                                .and_then(|p| p.file_name())
                                .map(|n| n.to_string_lossy().to_string())
                                .unwrap_or_else(|| "unknown".to_string());
                            pb_task.println(format!(
                                "  {} {}",
                                ColoredOutput::error("编译失败"),
                                file_name
                            ));
                            for diag in &r.diagnostics {
                                pb_task.println(format!("    {}", diag));
                            }
                        }
                        TaskResult::LinkSuccess(r) => {
                            let file_name = r
                                .output_file
                                .as_ref()
                                .and_then(|p| p.file_name())
                                .map(|n| n.to_string_lossy().to_string())
                                .unwrap_or_else(|| "unknown".to_string());
                            pb_task.println(format!(
                                "  {} {} ({}ms)",
                                ColoredOutput::success("链接"),
                                file_name,
                                r.duration_ms
                            ));
                        }
                        TaskResult::LinkFailure(r) => {
                            let file_name = r
                                .output_file
                                .as_ref()
                                .and_then(|p| p.file_name())
                                .map(|n| n.to_string_lossy().to_string())
                                .unwrap_or_else(|| "unknown".to_string());
                            pb_task.println(format!(
                                "  {} {}",
                                ColoredOutput::error("链接失败"),
                                file_name
                            ));
                            for diag in &r.diagnostics {
                                pb_task.println(format!("    {}", diag));
                            }
                        }
                        TaskResult::Skipped => {}
                        TaskResult::CommandResult {
                            success, output, ..
                        } => {
                            if !success {
                                pb_task.println(format!(
                                    "  {} {}",
                                    ColoredOutput::error("命令失败"),
                                    output
                                ));
                            }
                        }
                    }
                }));
            }

            let build_result = scheduler.execute()?;

            pb.finish_with_message(if build_result.success {
                "完成"
            } else {
                "失败"
            });

            // 11. 更新缓存
            action_cache.save().ok();

            // 11.5 将新编译的产物存入内容寻址缓存
            if build_result.success {
                for module in &modules {
                    if module.module_type == crate::core::config::ModuleType::External {
                        continue;
                    }
                    let intermediate_dir = ctx.module_intermediate_dir(&module.name);
                    let mut sources = if let Some(unity_files) = unity_sources.get(&module.name) {
                        unity_files.clone()
                    } else {
                        discovery::collect_module_sources(&module.path)
                    };
                    if let Some(generated_sources) =
                        generated_reflection_sources.get(module.name.as_str())
                    {
                        sources.extend(generated_sources.iter().cloned());
                    }

                    for source in &sources {
                        let object_file = object_file_for_source(source, &intermediate_dir);

                        if object_file.exists() {
                            // 复用编译前记录的键 — 必须与查询时的键完全一致,
                            // 若在此重新计算, 任何输入差异都会让缓存永远无法命中
                            if let Some(cache_key) = source_cache_keys.get(source).cloned() {
                                if !compile_cache.contains(&cache_key) {
                                    let artifact_size = std::fs::metadata(&object_file)
                                        .map(|m| m.len())
                                        .unwrap_or(0);
                                    let artifact_name = object_file
                                        .file_name()
                                        .map(|n| n.to_string_lossy().to_string())
                                        .unwrap_or_else(|| "unknown.obj".to_string());

                                    let entry = compiler::compile_cache::CacheEntry {
                                        key: cache_key,
                                        source_file: source.to_string_lossy().to_string(),
                                        artifact_name,
                                        artifact_size,
                                        created_at: std::time::SystemTime::now()
                                            .duration_since(std::time::UNIX_EPOCH)
                                            .map(|d| d.as_secs())
                                            .unwrap_or(0),
                                        last_accessed: std::time::SystemTime::now()
                                            .duration_since(std::time::UNIX_EPOCH)
                                            .map(|d| d.as_secs())
                                            .unwrap_or(0),
                                        access_count: 0,
                                        compile_time_ms: 0,
                                        compiler_version: comp.version().to_string(),
                                        build_config: build_config.name().to_string(),
                                    };
                                    compile_cache.store(entry, &object_file).ok();
                                }
                            }
                        }
                    }
                }
                compile_cache.save_index().ok();
            }

            // 12. 报告
            println!("\n{}", "═".repeat(60));
            if build_result.success {
                println!("{}", ColoredOutput::success("构建成功!"));
            } else {
                println!(
                    "{}",
                    ColoredOutput::error(&format!(
                        "构建失败: {} 个错误",
                        build_result.failed_tasks
                    ))
                );
            }
            println!("  总任务: {}", build_result.total_tasks);
            println!("  完成: {}", build_result.completed_tasks);
            println!("  失败: {}", build_result.failed_tasks);
            println!("  跳过: {}", build_result.skipped_tasks);
            println!("  缓存命中: {} (SHA-256 内容寻址)", skipped_cached);
            let cache_stats = compile_cache.get_stats();
            println!(
                "  缓存条目: {} ({:.1} MB)",
                cache_stats.current_entries,
                cache_stats.current_size_bytes as f64 / 1024.0 / 1024.0
            );
            if cache_stats.total_lookups > 0 {
                println!(
                    "  缓存命中率: {:.1}% (节省 {})",
                    cache_stats.hit_rate_percent(),
                    compile_cache.saved_time_display()
                );
            }
            println!("  耗时: {:.2}s", build_result.total_duration.as_secs_f64());
            println!("  输出: Binaries/{}/Win64/", build_config.name());

            // 输出诊断信息
            if verbose || !build_result.success {
                let diags = build_result.diagnostics.diagnostics();
                if !diags.is_empty() {
                    println!("\n诊断信息:");
                    for diag in diags.iter().take(20) {
                        println!("  {}", diag);
                    }
                    if diags.len() > 20 {
                        println!("  ... 还有 {} 条", diags.len() - 20);
                    }
                }
            }

            // 构建失败必须以非零退出。
            //
            // scheduler.execute() 把失败包在 Ok(BuildResult{success:false})
            // 里返回 —— 失败是一个 Ok 值, 所以上面的 ? 永远不会触发, 而这个
            // 分支原先直接落到函数末尾的 Ok(())。
            //
            // 后果远不止"这一步没报错": verify.ps1 与 ci.yml 都按退出码判定,
            // 构建失败之后那九步单元测试会跑**上一次成功构建留下的旧可执行
            // 文件**并全部通过。一棵编译不过的树可以拿到满屏绿色。
            if !build_result.success {
                std::process::exit(1);
            }
        }

        Commands::GenerateSolution {
            source_dir,
            output_dir,
            name,
        } => {
            info!("LBT: 生成 Visual Studio 解决方案");
            info!("  源目录: {}", source_dir.display());
            info!("  输出目录: {}", output_dir.display());
            info!("  解决方案名称: {}", name);

            // 1. 加载增量编译缓存
            let cache_dir = std::path::PathBuf::from("Intermediate");
            let mut cache = BuildCache::load(&cache_dir).unwrap_or_else(|_| BuildCache::new());

            // 2. 发现模块
            let modules = discovery::discover_modules(&source_dir)?;
            info!("发现 {} 个模块", modules.len());

            // 3. 检测增量变化
            let changes = cache.detect_changes(&modules);
            cache.print_stats(&changes);

            // 4. 计算项目根目录
            let project_root = source_dir.parent().unwrap_or(std::path::Path::new("."));

            // 5. 根据增量结果决定是否重新生成
            let generated_dir = std::path::PathBuf::from("Intermediate/Generated");
            std::fs::create_dir_all(&generated_dir)?;

            // 检查 .sln 是否存在，不存在则强制重新生成
            let sln_path = project_root.join(format!("{}.sln", name));
            let sln_exists = sln_path.exists();

            if !sln_exists
                || changes.need_regenerate_solution
                || changes.dirty_modules.len() == modules.len()
            {
                // 完全重新生成
                info!("完全重新生成解决方案...");
                vs::generate_vs_solution(&modules, &output_dir, project_root, &name)?;
                module::generate_all_modules(&modules, &generated_dir)?;
            } else if !changes.dirty_modules.is_empty() {
                // 增量生成 - 只更新修改的模块
                info!("增量更新 {} 个模块...", changes.dirty_modules.len());

                // 重新生成解决方案（因为 vcxproj 可能有变化）
                vs::generate_vs_solution(&modules, &output_dir, project_root, &name)?;

                // 只重新生成修改的模块头文件
                let dirty_modules: Vec<_> = modules
                    .iter()
                    .filter(|m| changes.dirty_modules.contains(&m.name))
                    .collect();
                for m in &dirty_modules {
                    module::generate_module_header(m, &generated_dir)?;
                }
            } else {
                // 即使模块未修改，也要检查 vcxproj 是否存在
                let all_projects_exist = modules.iter().all(|m| {
                    output_dir
                        .join(&m.name)
                        .join(format!("{}.vcxproj", m.name))
                        .exists()
                });

                if !all_projects_exist {
                    info!("项目文件缺失，重新生成解决方案...");
                    vs::generate_vs_solution(&modules, &output_dir, project_root, &name)?;
                    module::generate_all_modules(&modules, &generated_dir)?;
                } else {
                    info!("所有模块未修改，跳过生成");
                }
            }

            // 6. 更新缓存
            for m in &modules {
                let config_path = m.path.join(format!(
                    "{}.limx.toml",
                    m.name.strip_prefix("Limx").unwrap_or(&m.name)
                ));
                let source_files: Vec<_> = discovery::collect_module_sources(&m.path);
                let header_files: Vec<_> = discovery::collect_module_headers(&m.path);
                cache.update_module_with_hash(&m.name, &config_path, &source_files, &header_files);
            }

            // 7. 保存缓存
            cache.save(&cache_dir)?;
            info!("增量编译缓存已更新");

            // 8. 生成 API 头文件 (总是生成，因为它们依赖模块配置)
            let api_output = std::path::PathBuf::from("Intermediate/Build");
            std::fs::create_dir_all(&api_output)?;
            api::generate_all_api_headers(&modules, &api_output)?;

            // 9. 调用 LHT 生成反射代码
            let reflection_output = project_root
                .join("Intermediate")
                .join("Generated")
                .join("Reflection");
            run_lht_reflection_generation(&source_dir, &reflection_output, false)?;
            info!("反射代码生成完成");

            println!("\n✓ Visual Studio 解决方案已生成:");
            println!("  - {}.sln (项目根目录)", name);
            println!(
                "  - {}/Limx*.vcxproj (每个模块独立项目)",
                output_dir.display()
            );
        }

        Commands::GenerateReflection {
            source_dir,
            output_dir,
            module,
        } => {
            info!("LBT: 生成反射代码");
            info!("  源目录: {}", source_dir.display());
            info!("  输出目录: {}", output_dir.display());

            let lht_path = find_tool_executable("lht");

            // 构建 LHT 命令
            let mut cmd = std::process::Command::new(&lht_path);
            cmd.arg("generate")
                .arg("--source-dir")
                .arg(&source_dir)
                .arg("--output-dir")
                .arg(&output_dir);

            if let Some(m) = &module {
                cmd.arg("--module").arg(m);
            }

            info!("执行 LHT: {:?}", cmd);
            let status = cmd.status()?;

            if !status.success() {
                return Err(anyhow::anyhow!("LHT 执行失败"));
            }

            println!("\n✓ 反射代码已生成:");
            println!("  - {}", output_dir.display());
        }

        Commands::Graph {
            source_dir,
            format,
            output,
        } => {
            let modules = discovery::discover_modules(&source_dir)?;
            let graph = dependency::resolve_dependencies(&modules)?;

            let content = match format.as_str() {
                "dot" => graph.to_dot(),
                "mermaid" => graph.to_mermaid(),
                "stats" => {
                    let stats = graph.get_stats();
                    format!(
                        "模块总数: {}\n依赖边总数: {}\n最大深度: {}\n平均依赖数: {:.2}",
                        stats.total_modules,
                        stats.total_edges,
                        stats.max_depth,
                        stats.avg_dependencies
                    )
                }
                _ => {
                    graph.print_tree();
                    String::new()
                }
            };

            if let Some(output_path) = output {
                std::fs::write(&output_path, &content)?;
                println!("✓ 依赖图已保存到: {}", output_path.display());
            } else if !content.is_empty() {
                println!("{}", content);
            }
        }

        Commands::Clean { all } => {
            info!("LBT: 清理构建缓存");

            let dirs_to_clean = if all {
                vec![
                    "Intermediate/Build",
                    "Intermediate/Generated",
                    "Intermediate/ProjectFiles",
                    "Binaries",
                ]
            } else {
                vec!["Intermediate/Build"]
            };

            for dir in dirs_to_clean {
                let path = std::path::Path::new(dir);
                if path.exists() {
                    std::fs::remove_dir_all(path)?;
                    println!("✓ 已删除: {}", dir);
                }
            }

            // 清理缓存文件
            let cache_file = std::path::Path::new("Intermediate/lbt_cache.json");
            if cache_file.exists() {
                std::fs::remove_file(cache_file)?;
                println!("✓ 已删除: 构建缓存");
            }

            println!("\n✓ 清理完成");
        }

        Commands::Stats { source_dir } => {
            let modules = discovery::discover_modules(&source_dir)?;
            let graph = dependency::resolve_dependencies(&modules)?;
            let stats = graph.get_stats();

            println!("\n╔══════════════════════════════════════════════════════════════╗");
            println!("║                    LBT 构建统计                               ║");
            println!("╠══════════════════════════════════════════════════════════════╣");
            println!(
                "║  模块总数:       {:>6}                                      ║",
                stats.total_modules
            );
            println!(
                "║  依赖边总数:     {:>6}                                      ║",
                stats.total_edges
            );
            println!(
                "║  最大依赖深度:   {:>6}                                      ║",
                stats.max_depth
            );
            println!(
                "║  平均依赖数:     {:>6.2}                                      ║",
                stats.avg_dependencies
            );
            println!("╠══════════════════════════════════════════════════════════════╣");
            println!("║  层级分布:                                                   ║");
            for (layer, count) in &stats.modules_by_layer {
                println!(
                    "║    Layer {}: {:>3} 模块                                       ║",
                    layer, count
                );
            }
            println!("╠══════════════════════════════════════════════════════════════╣");
            println!("║  并行构建组:                                                 ║");
            let groups = graph.get_parallel_build_groups();
            for (i, group) in groups.iter().enumerate() {
                println!(
                    "║    组 {}: {} 模块可并行                                       ║",
                    i + 1,
                    group.len()
                );
            }
            println!("╚══════════════════════════════════════════════════════════════╝");

            // 详细模块列表
            println!("\n模块详情:");
            println!("{:<20} {:<8} {:<8} {:<30}", "名称", "层级", "深度", "依赖");
            println!("{}", "-".repeat(70));
            for module_name in &graph.build_order {
                let Some(module) = graph.modules.get(module_name) else {
                    continue;
                };
                let depth = graph.depths.get(module_name).unwrap_or(&0);
                let deps = graph.edges.get(module_name).cloned().unwrap_or_default();
                let deps_str = if deps.is_empty() {
                    "-".to_string()
                } else {
                    deps.join(", ")
                };
                println!(
                    "{:<20} {:<8} {:<8} {:<30}",
                    module_name, module.layer, depth, deps_str
                );
            }
        }

        Commands::Validate { source_dir, strict } => {
            info!("LBT: 验证模块配置");

            let modules = discovery::discover_modules(&source_dir)?;
            let mut validator = ModuleValidator::new();
            let result = validator.validate_all(&modules);

            result.print_report();

            if !result.is_valid() {
                return Err(anyhow::anyhow!("配置验证失败"));
            }

            // --strict 只对非建议类警告失败 —— 见 ValidationWarning::is_advisory
            let blocking = result
                .warnings
                .iter()
                .filter(|w| !w.is_advisory())
                .count();

            if strict && blocking > 0 {
                return Err(anyhow::anyhow!("严格模式：存在 {} 条警告", blocking));
            }
        }

        Commands::DistCoordinator { port, max_workers } => {
            print_banner();
            print_phase("启动分布式编译协调器");

            println!("  监听端口: {}", ColoredOutput::number(port));
            println!("  最大工作节点: {}", ColoredOutput::number(max_workers));

            let config = compiler::distributed::DistributedConfig {
                enabled: true,
                coordinator_addr: None,
                listen_port: port,
                max_concurrent_jobs: max_workers,
                ..Default::default()
            };

            let mut coordinator = compiler::distributed::Coordinator::new(config);
            println!(
                "\n{}",
                ColoredOutput::success("协调器已启动，等待工作节点连接...")
            );
            println!("  地址: 0.0.0.0:{}", port);

            // 阻塞运行
            coordinator.run()?;
        }

        Commands::DistWorker { coordinator, jobs } => {
            print_banner();
            print_phase("启动分布式编译工作节点");

            let parallel_jobs = if jobs == 0 { num_cpus::get() } else { jobs };

            println!("  协调器: {}", ColoredOutput::module(&coordinator));
            println!("  并发任务: {}", ColoredOutput::number(parallel_jobs));

            let addr: std::net::SocketAddr = coordinator
                .parse()
                .map_err(|e| anyhow::anyhow!("无效的协调器地址: {}", e))?;

            let config = compiler::distributed::DistributedConfig {
                enabled: true,
                coordinator_addr: Some(addr),
                max_concurrent_jobs: parallel_jobs,
                ..Default::default()
            };

            let mut worker = compiler::distributed::Worker::new(config);
            println!(
                "\n{}",
                ColoredOutput::success("工作节点已启动，正在连接协调器...")
            );

            worker.run()?;
        }

        Commands::AnalyzeDeps {
            source_dir,
            format,
            output,
        } => {
            print_banner();
            print_phase("分析头文件依赖");

            let modules = discovery::discover_modules(&source_dir)?;
            println!("  发现 {} 个模块", ColoredOutput::number(modules.len()));

            // 创建依赖分析器
            let analyzer = compiler::deps::DependencyAnalyzer::new();

            let mut all_deps: std::collections::HashMap<String, Vec<String>> =
                std::collections::HashMap::new();

            for module in &modules {
                let sources = discovery::collect_module_sources(&module.path);
                let headers = discovery::collect_module_headers(&module.path);

                println!(
                    "  [{}] {} 源文件, {} 头文件",
                    ColoredOutput::module(&module.name),
                    sources.len(),
                    headers.len()
                );

                let mut module_deps = Vec::new();
                for source in &sources {
                    if let Ok(includes) = analyzer.parse_includes(source) {
                        for (inc, _) in includes {
                            if !module_deps.contains(&inc) {
                                module_deps.push(inc);
                            }
                        }
                    }
                }
                all_deps.insert(module.name.clone(), module_deps);
            }

            // 输出结果
            let content = match format.as_str() {
                "json" => serde_json::to_string_pretty(&all_deps).unwrap_or_default(),
                "dot" => {
                    let mut dot = String::from("digraph Dependencies {\n");
                    for (module, deps) in &all_deps {
                        for dep in deps {
                            dot.push_str(&format!("    \"{}\" -> \"{}\";\n", module, dep));
                        }
                    }
                    dot.push_str("}\n");
                    dot
                }
                _ => {
                    let mut tree = String::from("\n头文件依赖树:\n");
                    for (module, deps) in &all_deps {
                        tree.push_str(&format!("[{}]\n", module));
                        for dep in deps.iter().take(10) {
                            tree.push_str(&format!("  └─ {}\n", dep));
                        }
                        if deps.len() > 10 {
                            tree.push_str(&format!("  ... 还有 {} 个\n", deps.len() - 10));
                        }
                    }
                    tree
                }
            };

            if let Some(output_path) = output {
                std::fs::write(&output_path, &content)?;
                println!(
                    "\n{}",
                    ColoredOutput::success(&format!("依赖分析已保存到: {}", output_path.display()))
                );
            } else {
                println!("{}", content);
            }
        }

        // ──────────────────────────────────────────────────────────────
        // 新命令: Target 系统
        // ──────────────────────────────────────────────────────────────
        Commands::NewTarget {
            name,
            target_type,
            output_dir,
        } => {
            print_banner();
            print_phase("创建新构建目标");

            let parsed_type = match target_type.to_lowercase().as_str() {
                "game" => TargetType::Game,
                "editor" => TargetType::Editor,
                "server" => TargetType::Server,
                "client" => TargetType::Client,
                "program" => TargetType::Program,
                "plugin" => TargetType::Plugin,
                other => {
                    return Err(anyhow::anyhow!(
                        "未知的 Target 类型 '{}' (可选: game/editor/server/client/program/plugin)",
                        other
                    ));
                }
            };

            std::fs::create_dir_all(&output_dir)?;
            let file_name = format!("{}.limx.target.toml", name);
            let file_path = output_dir.join(&file_name);

            if file_path.exists() {
                return Err(anyhow::anyhow!("文件已存在: {}", file_path.display()));
            }

            let content = generate_example_target(&name, parsed_type);
            std::fs::write(&file_path, &content)?;

            println!("  名称: {}", ColoredOutput::module(&name));
            println!("  类型: {}", ColoredOutput::module(parsed_type.name()));
            println!(
                "  文件: {}",
                ColoredOutput::path(&file_path.display().to_string())
            );
            println!("\n{}", ColoredOutput::success("Target 已创建"));
            println!("\n下一步:");
            println!("  1. 编辑 {} 配置允许的模块列表", file_name);
            println!("  2. 运行 lbt list-targets 查看所有 Target");
            println!("  3. 运行 lbt build --target {} 构建此 Target", name);
        }

        Commands::NewPlugin { name, output_dir } => {
            print_banner();
            print_phase("创建新插件");

            let plugin_dir = output_dir.join(&name);
            std::fs::create_dir_all(&plugin_dir)?;

            let file_name = format!("{}.limx.plugin.toml", name);
            let file_path = plugin_dir.join(&file_name);

            if file_path.exists() {
                return Err(anyhow::anyhow!("插件已存在: {}", file_path.display()));
            }

            let content = generate_example_plugin(&name);
            std::fs::write(&file_path, &content)?;

            // 创建插件标准目录结构
            let runtime_module_dir = plugin_dir.join(format!("{}Runtime", name));
            let editor_module_dir = plugin_dir.join(format!("{}Editor", name));
            for dir in &[
                runtime_module_dir.join("Public"),
                runtime_module_dir.join("Private"),
                editor_module_dir.join("Public"),
                editor_module_dir.join("Private"),
            ] {
                std::fs::create_dir_all(dir)?;
            }

            println!("  名称: {}", ColoredOutput::module(&name));
            println!(
                "  目录: {}",
                ColoredOutput::path(&plugin_dir.display().to_string())
            );
            println!(
                "  配置: {}",
                ColoredOutput::path(&file_path.display().to_string())
            );
            println!("\n{}", ColoredOutput::success("Plugin 已创建"));
            println!("\n目录结构:");
            println!("  {}/", name);
            println!("  ├── {}.limx.plugin.toml", name);
            println!("  ├── {}Runtime/", name);
            println!("  │   ├── Public/");
            println!("  │   └── Private/");
            println!("  └── {}Editor/", name);
            println!("      ├── Public/");
            println!("      └── Private/");
        }

        Commands::ListTargets { project_dir } => {
            print_banner();
            print_phase("扫描构建目标");

            let registry = discover_targets(&project_dir)?;

            if registry.targets.is_empty() {
                println!(
                    "\n{}",
                    ColoredOutput::warning("未找到任何 Target 文件 (.limx.target.toml)")
                );
                println!("使用 'lbt new-target --name MyGame' 创建第一个 Target");
            } else {
                registry.print_summary();
                println!(
                    "\n共 {} 个构建目标",
                    ColoredOutput::number(registry.targets.len())
                );
            }
        }

        Commands::ListPlugins { project_dir } => {
            print_banner();
            print_phase("扫描插件");

            let registry = discover_plugins(&project_dir)?;

            if registry.plugins.is_empty() {
                println!(
                    "\n{}",
                    ColoredOutput::warning("未找到任何插件文件 (.limx.plugin.toml)")
                );
                println!("使用 'lbt new-plugin --name MyPlugin' 创建第一个插件");
            } else {
                registry.print_summary();

                println!("\n插件模块汇总:");
                let mut sorted: Vec<_> = registry.plugins.values().collect();
                sorted.sort_by(|a, b| a.name.cmp(&b.name));
                for plugin in &sorted {
                    for m in &plugin.config.modules {
                        println!(
                            "  [{:20}] {:20} — {}",
                            plugin.name,
                            m.name,
                            m.loading_phase.name()
                        );
                    }
                }
                println!(
                    "\n共 {} 个插件",
                    ColoredOutput::number(registry.plugins.len())
                );
            }
        }

        // ──────────────────────────────────────────────────────────────
        // 编译缓存统计
        // ──────────────────────────────────────────────────────────────
        Commands::CacheStats {
            cache_dir,
            format,
            clear,
        } => {
            print_banner();
            print_phase("编译缓存统计");

            let config = compiler::compile_cache::CacheConfig {
                cache_dir: cache_dir.clone(),
                ..Default::default()
            };
            let cache = compiler::compile_cache::CompileCache::load_or_create(config);

            if clear {
                println!("  清空缓存...");
                cache.clear().ok();
                println!("{}", ColoredOutput::success("缓存已清空"));
                return Ok(());
            }

            let report = cache.generate_report();

            if format == "json" {
                let json = serde_json::to_string_pretty(&report).unwrap_or_default();
                println!("{}", json);
            } else {
                println!("\n╔══════════════════════════════════════════════════════════════╗");
                println!("║                    编译缓存统计                              ║");
                println!("╠══════════════════════════════════════════════════════════════╣");
                println!("║  缓存目录: {:48} ║", cache_dir.display());
                println!("║  条目数: {:50} ║", report.stats.current_entries);
                println!(
                    "║  缓存大小: {:39} MB  ║",
                    format!(
                        "{:.2}",
                        report.stats.current_size_bytes as f64 / 1024.0 / 1024.0
                    )
                );
                println!(
                    "║  空间利用率: {:37} %  ║",
                    format!("{:.1}", report.space_utilization_percent)
                );
                println!("╠══════════════════════════════════════════════════════════════╣");
                println!("║  总查询: {:50} ║", report.stats.total_lookups);
                println!("║  命中: {:52} ║", report.stats.hits);
                println!("║  未命中: {:50} ║", report.stats.misses);
                println!(
                    "║  命中率: {:40} %        ║",
                    format!("{:.1}", report.stats.hit_rate_percent())
                );
                println!("║  节省编译时间: {:45} ║", cache.saved_time_display());
                println!("║  存储次数: {:48} ║", report.stats.stores);
                println!("║  淘汰次数: {:48} ║", report.stats.evictions);
                println!("╠══════════════════════════════════════════════════════════════╣");

                if !report.per_config_stats.is_empty() {
                    println!("║  按构建配置:                                                 ║");
                    for (config_name, stats) in &report.per_config_stats {
                        println!(
                            "║    {}: {} 条目, {:.1} MB                     ║",
                            config_name,
                            stats.entry_count,
                            stats.total_size as f64 / 1024.0 / 1024.0
                        );
                    }
                }

                if !report.hottest_entries.is_empty() {
                    println!("╠══════════════════════════════════════════════════════════════╣");
                    println!("║  最热门条目 (按访问次数):                                      ║");
                    for entry in report.hottest_entries.iter().take(5) {
                        let name = std::path::Path::new(&entry.source_file)
                            .file_name()
                            .map(|n| n.to_string_lossy().to_string())
                            .unwrap_or_else(|| entry.source_file.clone());
                        println!(
                            "║    {} — {} 次访问, {}ms 编译时间",
                            name, entry.access_count, entry.compile_time_ms
                        );
                    }
                }

                println!("╚══════════════════════════════════════════════════════════════╝");
            }
        }

        // ──────────────────────────────────────────────────────────────
        // compile_commands.json 生成
        // ──────────────────────────────────────────────────────────────
        Commands::CompileCommands {
            source_dir,
            project_dir,
            compiler,
        } => {
            print_banner();
            print_phase("生成 compile_commands.json");

            let progress = BuildProgress::new();
            let spinner = progress.discovery_spinner("扫描模块...");
            let modules = discovery::discover_modules(&source_dir)?;
            spinner.finish_with_message(format!("发现 {} 个模块", modules.len()));

            if modules.is_empty() {
                println!("\n{}", ColoredOutput::warning("没有找到任何模块"));
                return Ok(());
            }

            let graph = dependency::resolve_dependencies(&modules)?;

            let output_path = project_dir.join("compile_commands.json");
            let pb = progress.build_progress(modules.len() as u64, "生成条目");

            let entry_count = generate_compile_commands(
                &graph,
                &modules,
                &project_dir,
                &output_path,
                Some(&compiler),
            )?;

            pb.finish_with_message("完成");

            println!(
                "\n{}",
                ColoredOutput::success("compile_commands.json 已生成")
            );
            println!(
                "  路径:   {}",
                ColoredOutput::path(&output_path.display().to_string())
            );
            println!("  条目数: {}", ColoredOutput::number(entry_count));
            println!("  模块数: {}", ColoredOutput::number(modules.len()));
            println!("\n使用方式:");
            println!("  VS Code : 安装 clangd 扩展后自动识别");
            println!("  CLion   : File → Reload CMake Project (或直接识别)");
            println!("  Rider   : 自动识别 compile_commands.json");
            println!("  命令行  : clang-tidy -p . Source/**/*.cpp");
        }

        // ──────────────────────────────────────────────────────────────
        // 未使用头文件分析
        // ──────────────────────────────────────────────────────────────
        Commands::AnalyzeIncludes {
            source_dir,
            format,
            output,
        } => {
            print_banner();
            print_phase("分析未使用/冗余 #include");

            let progress = BuildProgress::new();
            let spinner = progress.discovery_spinner("扫描源文件...");

            // 收集所有 .h 和 .cpp 文件内容
            let mut file_contents: std::collections::HashMap<String, String> =
                std::collections::HashMap::new();
            for entry in walkdir::WalkDir::new(&source_dir)
                .into_iter()
                .filter_map(|e| e.ok())
                .filter(|e| {
                    let ext = e.path().extension().and_then(|x| x.to_str()).unwrap_or("");
                    matches!(ext, "h" | "hpp" | "cpp" | "cc" | "cxx")
                })
            {
                if let Ok(content) = std::fs::read_to_string(entry.path()) {
                    let rel = entry
                        .path()
                        .strip_prefix(&source_dir)
                        .unwrap_or(entry.path())
                        .to_string_lossy()
                        .to_string();
                    file_contents.insert(rel, content);
                }
            }

            spinner.finish_with_message(format!("扫描到 {} 个文件", file_contents.len()));

            if file_contents.is_empty() {
                println!("\n{}", ColoredOutput::warning("未找到源文件"));
                return Ok(());
            }

            // 注册头文件符号
            let mut analyzer = compiler::include_analyzer::IncludeAnalyzer::new();
            for (path, content) in &file_contents {
                if path.ends_with(".h") || path.ends_with(".hpp") {
                    analyzer.register_header(path, content);
                }
            }

            // 执行分析
            let report = analyzer.analyze_all(&file_contents);

            let output_content = match format.as_str() {
                "markdown" | "md" => report.to_markdown(),
                "json" => serde_json::to_string_pretty(&report).unwrap_or_default(),
                _ => {
                    // 文本格式
                    let mut text = String::new();
                    text.push_str(&format!("分析文件数: {}\n", report.files_analyzed));
                    text.push_str(&format!("总 #include: {}\n", report.total_includes));
                    text.push_str(&format!("冗余 #include: {}\n", report.total_redundant));
                    if report.total_includes > 0 {
                        text.push_str(&format!(
                            "冗余率: {:.1}%\n",
                            report.total_redundant as f64 / report.total_includes as f64 * 100.0
                        ));
                    }
                    if !report.worst_files.is_empty() {
                        text.push_str("\n冗余最多的文件:\n");
                        for (file_path, redundant_count) in &report.worst_files {
                            text.push_str(&format!(
                                "  {} — {} 个冗余包含\n",
                                file_path, redundant_count
                            ));
                        }
                    }
                    text
                }
            };

            if let Some(out_path) = output {
                std::fs::write(&out_path, &output_content)?;
                println!(
                    "{}",
                    ColoredOutput::success(&format!("报告已保存: {}", out_path.display()))
                );
            } else {
                println!("{}", output_content);
            }

            println!(
                "\n分析完成: {} 文件, {} 冗余 / {} 总包含",
                report.files_analyzed, report.total_redundant, report.total_includes
            );
        }

        // ──────────────────────────────────────────────────────────────
        // PCH 候选分析
        // ──────────────────────────────────────────────────────────────
        Commands::AnalyzePch {
            source_dir,
            max_headers,
            min_frequency,
        } => {
            print_banner();
            print_phase("智能 PCH 候选分析");

            let config = compiler::pch_analyzer::AnalyzerConfig {
                max_pch_headers: max_headers,
                min_inclusion_frequency: min_frequency,
                ..Default::default()
            };
            let mut analyzer = compiler::pch_analyzer::PchAnalyzer::new(config);
            let recommendation = analyzer.analyze_directory(&source_dir);

            println!("\n╔══════════════════════════════════════════════════════════════╗");
            println!("║                    PCH 候选分析结果                           ║");
            println!("╠══════════════════════════════════════════════════════════════╣");
            println!(
                "║  分析文件: {:>6}                                            ║",
                recommendation.source_file_count
            );
            println!(
                "║  唯一头文件: {:>4}                                            ║",
                recommendation.unique_header_count
            );
            println!(
                "║  推荐 PCH 头文件: {:>3}                                       ║",
                recommendation.candidates.len()
            );
            if recommendation.estimated_speedup_percent > 0.0 {
                println!(
                    "║  预计加速: {:>5.1}%                                          ║",
                    recommendation.estimated_speedup_percent
                );
            }
            println!("╠══════════════════════════════════════════════════════════════╣");

            if recommendation.candidates.is_empty() {
                println!("║  未找到适合放入 PCH 的头文件                                  ║");
            } else {
                println!("║  推荐头文件 (按优先级排序):                                    ║");
                for (i, candidate) in recommendation.candidates.iter().enumerate() {
                    println!(
                        "║  {:>2}. {:40} 评分:{:>6.1} ║",
                        i + 1,
                        candidate.header_path,
                        candidate.score
                    );
                }
            }
            println!("╚══════════════════════════════════════════════════════════════╝");
        }

        // ──────────────────────────────────────────────────────────────
        // 模块健康度分析
        // ──────────────────────────────────────────────────────────────
        Commands::HealthCheck {
            source_dir,
            module: module_filter,
            format,
        } => {
            print_banner();
            print_phase("模块健康度分析");

            let progress = BuildProgress::new();
            let spinner = progress.discovery_spinner("扫描模块...");
            let modules = discovery::discover_modules(&source_dir)?;
            spinner.finish_with_message(format!("发现 {} 个模块", modules.len()));

            if modules.is_empty() {
                println!("\n{}", ColoredOutput::warning("未找到模块"));
                return Ok(());
            }

            let analyzer = core::module_health::ModuleHealthAnalyzer::with_defaults();

            let target_modules: Vec<_> = if let Some(ref name) = module_filter {
                modules.iter().filter(|m| m.name == *name).collect()
            } else {
                modules.iter().collect()
            };

            if target_modules.is_empty() {
                println!(
                    "\n{}",
                    ColoredOutput::warning(&format!(
                        "未找到模块 '{}'",
                        module_filter.unwrap_or_default()
                    ))
                );
                return Ok(());
            }

            for m in &target_modules {
                // 收集模块源文件内容
                let mut source_files: std::collections::HashMap<String, String> =
                    std::collections::HashMap::new();
                for entry in walkdir::WalkDir::new(&m.path)
                    .into_iter()
                    .filter_map(|e| e.ok())
                    .filter(|e| {
                        let ext = e.path().extension().and_then(|x| x.to_str()).unwrap_or("");
                        matches!(ext, "h" | "hpp" | "cpp" | "cc" | "cxx")
                    })
                {
                    if let Ok(content) = std::fs::read_to_string(entry.path()) {
                        let rel = entry
                            .path()
                            .strip_prefix(&m.path)
                            .unwrap_or(entry.path())
                            .to_string_lossy()
                            .to_string();
                        source_files.insert(rel, content);
                    }
                }

                let report = analyzer.analyze_module(
                    &m.name,
                    &source_files,
                    core::module_health::DependencyMetrics::default(),
                );

                match format.as_str() {
                    "markdown" | "md" => println!("{}", report.to_markdown()),
                    "json" => println!(
                        "{}",
                        serde_json::to_string_pretty(&report).unwrap_or_default()
                    ),
                    _ => {
                        println!(
                            "\n模块: {} — {} {} (评分: {:.0}/100)",
                            m.name,
                            report.grade.emoji(),
                            report.grade.display_name(),
                            report.health_score
                        );
                        println!(
                            "  代码行: {}  头文件: {}  API 表面积: {}",
                            report.code_metrics.code_lines,
                            report.code_metrics.header_file_count,
                            report.api_surface.total_surface()
                        );
                        if !report.bloat_warnings.is_empty() {
                            println!("  膨胀警告: {} 条", report.bloat_warnings.len());
                            for w in report.bloat_warnings.iter().take(5) {
                                println!("    [{:?}] {}", w.severity, w.message);
                            }
                        }
                    }
                }
            }
        }

        // ──────────────────────────────────────────────────────────────
        // 增强依赖图导出
        // ──────────────────────────────────────────────────────────────
        Commands::GraphExport {
            source_dir,
            format,
            output,
        } => {
            print_banner();
            print_phase("增强依赖图导出");

            let progress = BuildProgress::new();
            let spinner = progress.discovery_spinner("扫描模块...");
            let modules = discovery::discover_modules(&source_dir)?;
            spinner.finish_with_message(format!("发现 {} 个模块", modules.len()));

            if modules.is_empty() {
                println!("\n{}", ColoredOutput::warning("未找到模块"));
                return Ok(());
            }

            let graph = dependency::resolve_dependencies(&modules)?;
            let exporter = generators::graph_export::GraphExporter::with_defaults(&graph);

            let content = match format.as_str() {
                "dot" => exporter.export_dot_heatmap(),
                "mermaid" => exporter.export_mermaid_enhanced(),
                "coupling-csv" => {
                    let out_path = output
                        .clone()
                        .unwrap_or_else(|| "coupling_matrix.csv".into());
                    exporter.export_coupling_csv(&out_path)?;
                    println!(
                        "{}",
                        ColoredOutput::success(&format!(
                            "耦合度矩阵已导出: {}",
                            out_path.display()
                        ))
                    );
                    return Ok(());
                }
                _ => exporter.export_html_interactive(),
            };

            if let Some(out_path) = output {
                std::fs::write(&out_path, &content)?;
                println!(
                    "{}",
                    ColoredOutput::success(&format!(
                        "依赖图已导出: {} (格式: {})",
                        out_path.display(),
                        format
                    ))
                );
            } else {
                println!("{}", content);
            }

            // 显示耦合度摘要
            let metrics = exporter.metrics();
            if !metrics.is_empty() {
                println!("\n模块耦合度指标:");
                for m in metrics.iter().take(10) {
                    println!(
                        "  {:20} 扇入:{:>2}  扇出:{:>2}  不稳定度:{:.2}",
                        m.module_name, m.fan_in, m.fan_out, m.instability
                    );
                }
            }
        }

        // ──────────────────────────────────────────────────────────────
        // 构建性能剖析报告
        // ──────────────────────────────────────────────────────────────
        Commands::BuildProfile {
            profile_dir,
            format,
            output,
        } => {
            print_banner();
            print_phase("构建性能剖析");

            let report_path = profile_dir.join("last_build_report.json");
            if !report_path.exists() {
                println!("\n{}", ColoredOutput::warning("未找到构建剖析数据"));
                println!("请先运行 'lbt build' 构建项目后再查看剖析报告");
                println!("(剖析数据位于 {})", profile_dir.display());
                return Ok(());
            }

            let json_data = std::fs::read_to_string(&report_path)?;
            let report: core::build_profiler::ProfileReport = serde_json::from_str(&json_data)
                .map_err(|e| anyhow::anyhow!("解析剖析数据失败: {}", e))?;

            match format.as_str() {
                "json" => {
                    let json_out = serde_json::to_string_pretty(&report).unwrap_or_default();
                    if let Some(out_path) = output {
                        std::fs::write(&out_path, &json_out)?;
                        println!(
                            "{}",
                            ColoredOutput::success(&format!(
                                "JSON 报告已导出: {}",
                                out_path.display()
                            ))
                        );
                    } else {
                        println!("{}", json_out);
                    }
                }
                "html" => {
                    let profiler =
                        core::build_profiler::BuildProfiler::new(&report.build_configuration, 0);
                    let out_path = output.unwrap_or_else(|| "build_profile.html".into());
                    profiler.export_html(&report, &out_path)?;
                    println!(
                        "{}",
                        ColoredOutput::success(&format!("HTML 报告已导出: {}", out_path.display()))
                    );
                }
                _ => {
                    // 文本格式
                    println!("\n╔══════════════════════════════════════════════════════════════╗");
                    println!("║                    构建性能剖析报告                           ║");
                    println!("╠══════════════════════════════════════════════════════════════╣");
                    println!("║  构建配置: {:48} ║", report.build_configuration);
                    println!(
                        "║  总耗时: {:>8.2}s                                        ║",
                        report.total_build_duration.as_secs_f64()
                    );
                    println!(
                        "║  编译文件: {:>6}                                          ║",
                        report.file_entries.len()
                    );
                    println!(
                        "║  链接目标: {:>6}                                          ║",
                        report.link_entries.len()
                    );

                    if !report.bottlenecks.is_empty() {
                        println!(
                            "╠══════════════════════════════════════════════════════════════╣"
                        );
                        println!(
                            "║  瓶颈 ({} 个):                                              ║",
                            report.bottlenecks.len()
                        );
                        for b in report.bottlenecks.iter().take(10) {
                            println!("║  {} {}", b.severity.icon(), b.description);
                        }
                    }

                    if !report.critical_path.is_empty() {
                        println!(
                            "╠══════════════════════════════════════════════════════════════╣"
                        );
                        println!(
                            "║  关键路径:                                                    ║"
                        );
                        for node in report.critical_path.iter().take(10) {
                            println!(
                                "║    {:40} {:>8.1}ms ║",
                                node.name,
                                node.duration.as_secs_f64() * 1000.0
                            );
                        }
                    }
                    println!("╚══════════════════════════════════════════════════════════════╝");
                }
            }
        }
    }

    Ok(())
}
