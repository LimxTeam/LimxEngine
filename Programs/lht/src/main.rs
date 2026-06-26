/*******************************************************************************
 * 文件: main.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   LHT (Limx Header Tool) 入口点
 *   - 头文件扫描：查找反射宏
 *   - 类型解析：提取类型元数据
 *   - 代码生成：生成 .generated.h/cpp 文件
 *
 ******************************************************************************/

// 允许已实现但尚未集成的模块代码
#![allow(dead_code)]
#![allow(unused_imports)]
#![allow(unused_variables)]

mod cli;
mod codegen;
mod core;
mod hotreload;
mod parser;

use anyhow::Result;
use clap::Parser;
use console::Style;
use indicatif::{MultiProgress, ProgressBar, ProgressStyle};
use std::time::Instant;
use tracing::info;
use tracing_subscriber::{layer::SubscriberExt, util::SubscriberInitExt, EnvFilter};

use crate::cli::{Cli, Commands};
use crate::codegen::reflection;
use crate::codegen::{generate_all_runtime_headers, RuntimeHeaderGenerator};
use crate::parser::{parser as reflection_parser, scanner};

/// 打印 LHT Banner
fn print_banner() {
    let cyan = Style::new().cyan();
    println!(
        "{}",
        cyan.apply_to(
            r#"
    ╦  ╦ ╦╔╦╗
    ║  ╠═╣ ║   Limx Header Tool
    ╩═╝╩ ╩ ╩   v0.1.0
"#
        )
    );
}

/// 打印阶段标题
fn print_phase(phase: &str) {
    let style = Style::new().blue().bold();
    println!("\n{} {}", style.apply_to("▶"), style.apply_to(phase));
}

/// 成功消息
fn success(msg: &str) {
    let style = Style::new().green().bold();
    println!("{} {}", style.apply_to("✓"), msg);
}

/// 错误消息
#[allow(dead_code)]
fn error(msg: &str) {
    let style = Style::new().red().bold();
    eprintln!("{} {}", style.apply_to("✗"), msg);
}

fn main() -> Result<()> {
    // 初始化日志 - 同时输出到控制台和文件
    let log_dir = std::path::PathBuf::from("Logs");
    std::fs::create_dir_all(&log_dir).ok();

    let file_appender = tracing_appender::rolling::daily(&log_dir, "lht.log");
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
            module,
            serialization,
            rpc,
            gc,
            editor_meta,
            script_binding,
            migration,
            type_binding,
            incremental,
            preprocess,
            all,
        } => {
            print_banner();
            print_phase("生成反射代码");
            let start = Instant::now();

            // 确定生成选项
            let gen_serialization = serialization || all;
            let gen_rpc = rpc || all;
            let gen_gc = gc || all;
            let gen_editor_meta = editor_meta || all;
            let gen_script_binding = script_binding || all;
            let gen_migration = migration || all;
            let gen_type_binding = type_binding || all;

            // 增量代码生成引擎 (可选)
            let cache_path = output_dir.join(".lht_incremental_cache.json");
            let mut incremental_engine = if incremental {
                Some(codegen::incremental::IncrementalCodegen::new(cache_path))
            } else {
                None
            };

            // 1. 扫描头文件
            let multi = MultiProgress::new();
            let spinner = multi.add(ProgressBar::new_spinner());
            spinner.set_style(
                ProgressStyle::default_spinner()
                    .template("{spinner:.cyan} {msg}")
                    .unwrap_or_else(|_| ProgressStyle::default_spinner())
                    .tick_chars("⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏"),
            );
            spinner.set_message("扫描头文件...");
            spinner.enable_steady_tick(std::time::Duration::from_millis(80));

            let headers = scanner::scan_headers(&source_dir, module.as_deref())?;
            spinner.finish_with_message(format!("扫描到 {} 个头文件", headers.len()));

            // 2. 解析和生成
            let pb = multi.add(ProgressBar::new(headers.len() as u64));
            pb.set_style(
                ProgressStyle::default_bar()
                    .template("{spinner:.green} [{bar:40.cyan/blue}] {pos}/{len} {msg}")
                    .unwrap_or_else(|_| ProgressStyle::default_bar())
                    .progress_chars("█▓▒░"),
            );

            let mut total_types = 0;
            let mut processed = 0;
            let mut serialization_count = 0;
            let mut rpc_count = 0;
            let mut gc_count = 0;
            let mut editor_meta_count = 0;
            let mut script_binding_count = 0;
            let mut migration_count = 0;
            let mut type_binding_count = 0;
            let mut incremental_skipped = 0;
            let mut all_signatures: Vec<codegen::incremental::TypeSignature> = Vec::new();

            for header in &headers {
                // 预处理器: 展开条件编译块后再解析
                let types = if preprocess {
                    let source = std::fs::read_to_string(header)?;
                    let file_name = header
                        .file_name()
                        .map(|n| n.to_string_lossy().to_string())
                        .unwrap_or_else(|| "unknown.h".to_string());
                    let preprocess_result =
                        parser::preprocessor::preprocess_source(&source, &file_name);
                    // 将预处理后的源码写入临时文件并解析
                    let temp_dir = output_dir.join(".lht_preprocess");
                    std::fs::create_dir_all(&temp_dir)?;
                    let temp_path = temp_dir.join(&file_name);
                    std::fs::write(&temp_path, &preprocess_result.processed_source)?;
                    reflection_parser::parse_header(&temp_path)?
                } else {
                    reflection_parser::parse_header(header)?
                };
                if !types.is_empty() {
                    total_types += types.len();

                    // 增量检测: 收集类型签名，跳过未变更的类型
                    let _skip_set: std::collections::HashSet<String> =
                        std::collections::HashSet::new();
                    if incremental_engine.is_some() {
                        for t in &types {
                            let sig = codegen::incremental::TypeSignature {
                                qualified_name: t.name().to_string(),
                                kind: match t {
                                    crate::parser::parser::ReflectedType::Class(_) => {
                                        codegen::incremental::TypeKind::Class
                                    }
                                    crate::parser::parser::ReflectedType::Struct(_) => {
                                        codegen::incremental::TypeKind::Struct
                                    }
                                    crate::parser::parser::ReflectedType::Enum(_) => {
                                        codegen::incremental::TypeKind::Enum
                                    }
                                    crate::parser::parser::ReflectedType::Delegate(_) => {
                                        codegen::incremental::TypeKind::Enum
                                    }
                                },
                                hash: codegen::incremental::compute_type_hash(&format!("{:?}", t)),
                                source_file: header.to_string_lossy().to_string(),
                                module_name: String::new(),
                                dependencies: Vec::new(),
                                dependents: Vec::new(),
                            };
                            all_signatures.push(sig);
                        }

                        if let Some(ref engine) = incremental_engine {
                            let file_sigs: Vec<_> = all_signatures
                                .iter()
                                .filter(|s| s.source_file == header.to_string_lossy())
                                .cloned()
                                .collect();
                            let detection = engine.detect_changes(&file_sigs);
                            if !detection.has_changes() {
                                incremental_skipped += types.len();
                                pb.inc(1);
                                pb.set_message(format!(
                                    "{} 类型 (跳过 {})",
                                    total_types, incremental_skipped
                                ));
                                continue;
                            }
                        }
                    }

                    reflection::generate_reflection_code(header, &types, &output_dir)?;
                    processed += 1;

                    // 生成高级功能代码
                    for t in &types {
                        if let crate::parser::parser::ReflectedType::Class(class) = t {
                            // 转换为 AST ClassDecl 格式
                            let class_decl = codegen::adapter::class_info_to_decl(class);

                            if gen_serialization {
                                let serialization_gen =
                                    codegen::serialization::SerializationGenerator::new(
                                        codegen::serialization::SerializationOptions::default(),
                                    );
                                let ser_code =
                                    serialization_gen.generate_class_serialization(&class_decl);
                                if !ser_code.is_empty() {
                                    let ser_path = output_dir
                                        .join(format!("{}.serialization.generated.h", class.name));
                                    std::fs::write(&ser_path, &ser_code)?;
                                    serialization_count += 1;
                                }
                            }

                            if gen_rpc {
                                let mut rpc_gen = codegen::rpc::RpcCodeGenerator::new(
                                    codegen::rpc::RpcConfig::default(),
                                );
                                let rpc_code = rpc_gen.generate_class_rpc(&class_decl);
                                if !rpc_code.is_empty() {
                                    let rpc_path =
                                        output_dir.join(format!("{}.rpc.generated.h", class.name));
                                    std::fs::write(&rpc_path, &rpc_code)?;
                                    rpc_count += 1;
                                }
                            }

                            if gen_gc {
                                let gc_gen = codegen::gc::GcCodeGenerator::new(
                                    codegen::gc::GcConfig::default(),
                                );
                                let gc_code = gc_gen.generate_class_gc(&class_decl);
                                if !gc_code.is_empty() {
                                    let gc_path =
                                        output_dir.join(format!("{}.gc.generated.h", class.name));
                                    std::fs::write(&gc_path, &gc_code)?;
                                    gc_count += 1;
                                }
                            }

                            // 编辑器元数据生成
                            if gen_editor_meta {
                                let editor_inputs =
                                    codegen::adapter::class_info_to_editor_inputs(class);
                                if !editor_inputs.is_empty() {
                                    let editor_gen =
                                        codegen::editor_meta::EditorMetaGenerator::new();
                                    let class_meta =
                                        editor_gen.generate_class_meta(&class.name, &editor_inputs);
                                    let header_code = editor_gen.generate_header(&class_meta);
                                    let reg_code = editor_gen.generate_registration(&class_meta);
                                    if !header_code.is_empty() {
                                        let editor_h = output_dir
                                            .join(format!("{}.editor.generated.h", class.name));
                                        std::fs::write(&editor_h, &header_code)?;
                                        let editor_cpp = output_dir
                                            .join(format!("{}.editor.generated.cpp", class.name));
                                        std::fs::write(&editor_cpp, &reg_code)?;
                                        editor_meta_count += 1;
                                    }
                                }
                            }

                            // 脚本绑定生成
                            if gen_script_binding {
                                let binding = codegen::adapter::class_info_to_script_binding(class);
                                if !binding.functions.is_empty() || !binding.properties.is_empty() {
                                    let script_gen =
                                        codegen::script_binding::ScriptBindingGenerator::new();
                                    let generated = script_gen.generate_class_binding(&binding);
                                    if !generated.header_content.is_empty() {
                                        let script_h = output_dir
                                            .join(format!("{}.script.generated.h", class.name));
                                        std::fs::write(&script_h, &generated.header_content)?;
                                        let script_cpp = output_dir
                                            .join(format!("{}.script.generated.cpp", class.name));
                                        std::fs::write(&script_cpp, &generated.impl_content)?;
                                        let script_reg = output_dir.join(format!(
                                            "{}.script_reg.generated.cpp",
                                            class.name
                                        ));
                                        std::fs::write(
                                            &script_reg,
                                            &generated.registration_content,
                                        )?;
                                        script_binding_count += 1;
                                    }
                                }
                            }

                            // 迁移代码生成 (生成当前版本快照，后续与旧版本比对时自动生成迁移)
                            if gen_migration {
                                let type_version =
                                    codegen::adapter::class_info_to_type_version(class, 1);
                                let snapshot_json =
                                    serde_json::to_string_pretty(&type_version).unwrap_or_default();
                                if !snapshot_json.is_empty() {
                                    let snapshot_path =
                                        output_dir.join(format!("{}.version.json", class.name));

                                    // 如果已有旧版本快照，生成迁移代码
                                    if snapshot_path.exists() {
                                        let old_json = std::fs::read_to_string(&snapshot_path)
                                            .unwrap_or_default();
                                        if let Ok(old_version) =
                                            serde_json::from_str::<codegen::migration::TypeVersion>(
                                                &old_json,
                                            )
                                        {
                                            let migration_gen =
                                                codegen::migration::MigrationGenerator::new();
                                            let changes = migration_gen
                                                .diff_versions(&old_version, &type_version);
                                            if !changes.is_empty() {
                                                let step = codegen::migration::MigrationStep {
                                                    class_name: class.name.clone(),
                                                    from_version: old_version.version,
                                                    to_version: type_version.version,
                                                    changes: changes.clone(),
                                                };
                                                let generated =
                                                    migration_gen.generate_migration(&step);
                                                let mig_h = output_dir.join(format!(
                                                    "{}.migrate_v{}_v{}.generated.h",
                                                    class.name,
                                                    old_version.version,
                                                    type_version.version
                                                ));
                                                std::fs::write(&mig_h, &generated.header_content)?;
                                                let mig_cpp = output_dir.join(format!(
                                                    "{}.migrate_v{}_v{}.generated.cpp",
                                                    class.name,
                                                    old_version.version,
                                                    type_version.version
                                                ));
                                                std::fs::write(&mig_cpp, &generated.impl_content)?;
                                                migration_count += 1;
                                            }
                                        }
                                    }

                                    // 保存当前版本快照
                                    std::fs::write(&snapshot_path, &snapshot_json)?;
                                }
                            }

                            // 类型安全属性绑定生成
                            if gen_type_binding {
                                let binding_class =
                                    codegen::adapter::class_info_to_binding_class(class);
                                let binding_gen =
                                    codegen::type_binding::TypeBindingGenerator::with_defaults();
                                let generated = binding_gen.generate_binding(&binding_class);
                                if !generated.header_content.is_empty() {
                                    let bind_h = output_dir
                                        .join(format!("{}.binding.generated.h", class.name));
                                    std::fs::write(&bind_h, &generated.header_content)?;
                                    let bind_cpp = output_dir
                                        .join(format!("{}.binding.generated.cpp", class.name));
                                    std::fs::write(&bind_cpp, &generated.impl_content)?;
                                    type_binding_count += 1;
                                }
                            }
                        }
                    }
                }
                pb.inc(1);
                pb.set_message(format!("{} 类型", total_types));
            }

            // 增量缓存更新与保存
            if let Some(ref mut engine) = incremental_engine {
                engine.update_cache(&all_signatures);
                engine.save_cache().ok();
            }

            pb.finish_with_message(format!("处理完成: {} 个类型", total_types));

            // 打印报告
            let duration = start.elapsed();
            println!("\n╔══════════════════════════════════════════════════════════════╗");
            println!("║                    生成报告                                  ║");
            println!("╠══════════════════════════════════════════════════════════════╣");
            println!(
                "║  扫描文件:       {:>6}                                      ║",
                headers.len()
            );
            println!(
                "║  反射类型:       {:>6}                                      ║",
                total_types
            );
            println!(
                "║  反射文件:       {:>6}                                      ║",
                processed * 2
            );
            if gen_serialization {
                println!(
                    "║  序列化文件:     {:>6}                                      ║",
                    serialization_count
                );
            }
            if gen_rpc {
                println!(
                    "║  RPC 文件:       {:>6}                                      ║",
                    rpc_count
                );
            }
            if gen_gc {
                println!(
                    "║  GC 文件:        {:>6}                                      ║",
                    gc_count
                );
            }
            if gen_editor_meta {
                println!(
                    "║  编辑器元数据:   {:>6}                                      ║",
                    editor_meta_count
                );
            }
            if gen_script_binding {
                println!(
                    "║  脚本绑定:       {:>6}                                      ║",
                    script_binding_count
                );
            }
            if gen_migration {
                println!(
                    "║  迁移代码:       {:>6}                                      ║",
                    migration_count
                );
            }
            if gen_type_binding {
                println!(
                    "║  类型绑定:       {:>6}                                      ║",
                    type_binding_count
                );
            }
            if incremental_engine.is_some() {
                println!(
                    "║  增量跳过:       {:>6}                                      ║",
                    incremental_skipped
                );
            }
            if preprocess {
                println!("║  预处理:         已启用                                      ║");
            }
            println!(
                "║  耗时:           {:>6}ms                                    ║",
                duration.as_millis()
            );
            println!("╚══════════════════════════════════════════════════════════════╝");

            success(&format!("输出目录: {}", output_dir.display()));
        }

        Commands::Check {
            source_dir,
            semantic,
            type_consistency,
            strict,
        } => {
            print_banner();
            print_phase("检查反射宏");

            let spinner = ProgressBar::new_spinner();
            spinner.set_style(
                ProgressStyle::default_spinner()
                    .template("{spinner:.cyan} {msg}")
                    .unwrap_or_else(|_| ProgressStyle::default_spinner()),
            );
            spinner.set_message("扫描头文件...");
            spinner.enable_steady_tick(std::time::Duration::from_millis(80));

            let headers = scanner::scan_headers(&source_dir, None)?;
            spinner.finish_with_message(format!("扫描到 {} 个头文件", headers.len()));

            let mut total_types = 0;
            let mut errors = 0;
            let mut all_types: Vec<crate::parser::parser::ReflectedType> = Vec::new();

            for header in &headers {
                match reflection_parser::parse_header(header) {
                    Ok(types) => {
                        total_types += types.len();
                        for t in &types {
                            println!("✓ {} - {}", header.display(), t.name());
                        }
                        all_types.extend(types);
                    }
                    Err(e) => {
                        eprintln!("✗ {} - {}", header.display(), e);
                        errors += 1;
                    }
                }
            }

            println!("\n扫描完成: {} 个类型, {} 个错误", total_types, errors);

            // 语义分析 (可选)
            if semantic {
                print_phase("语义分析");
                let semantic_report = if strict {
                    parser::semantic::analyze_reflected_types(&all_types, true)
                } else {
                    parser::semantic::analyze_reflected_types(&all_types, false)
                };
                semantic_report.print_report();

                if strict && semantic_report.has_errors() {
                    errors += semantic_report.error_count();
                }
                if semantic_report.warning_count() > 0 {
                    println!("  语义警告: {}", semantic_report.warning_count());
                }
                if strict && semantic_report.warning_count() > 0 {
                    errors += semantic_report.warning_count();
                    println!("  (严格模式: 警告已提升为错误)");
                }
            }

            // 跨模块类型一致性校验 (可选)
            if type_consistency {
                print_phase("跨模块类型一致性校验");
                let mut checker = parser::type_consistency::TypeConsistencyChecker::new();

                // 注册所有类型到类型注册表
                for t in &all_types {
                    match t {
                        crate::parser::parser::ReflectedType::Class(c) => {
                            checker.registry_mut().register_type(
                                parser::type_consistency::TypeDefinition {
                                    qualified_name: c.name.clone(),
                                    short_name: c.name.clone(),
                                    category: parser::type_consistency::TypeCategory::Class,
                                    module_name: String::new(),
                                    header_file: String::new(),
                                    namespace: None,
                                    base_classes: c
                                        .base_classes
                                        .iter()
                                        .map(|b| b.name.clone())
                                        .collect(),
                                    referenced_types: c
                                        .properties
                                        .iter()
                                        .map(|p| p.type_name.clone())
                                        .collect(),
                                    is_template: false,
                                },
                            );
                            // 注册基类引用
                            for base in &c.base_classes {
                                checker.registry_mut().register_reference(
                                    parser::type_consistency::TypeReference {
                                        type_name: base.name.clone(),
                                        source_file: String::new(),
                                        module_name: String::new(),
                                        line_number: None,
                                        context:
                                            parser::type_consistency::ReferenceContext::BaseClass,
                                        can_use_forward_decl: false,
                                    },
                                );
                            }
                            // 注册属性类型引用
                            for prop in &c.properties {
                                let can_fwd = prop.is_pointer || prop.is_reference;
                                checker.registry_mut().register_reference(parser::type_consistency::TypeReference {
                                    type_name: prop.type_name.clone(),
                                    source_file: String::new(),
                                    module_name: String::new(),
                                    line_number: None,
                                    context: if can_fwd {
                                        parser::type_consistency::ReferenceContext::PropertyPointer
                                    } else {
                                        parser::type_consistency::ReferenceContext::PropertyValue
                                    },
                                    can_use_forward_decl: can_fwd,
                                });
                            }
                        }
                        crate::parser::parser::ReflectedType::Struct(s) => {
                            checker.registry_mut().register_type(
                                parser::type_consistency::TypeDefinition {
                                    qualified_name: s.name.clone(),
                                    short_name: s.name.clone(),
                                    category: parser::type_consistency::TypeCategory::Struct,
                                    module_name: String::new(),
                                    header_file: String::new(),
                                    namespace: None,
                                    base_classes: Vec::new(),
                                    referenced_types: s
                                        .properties
                                        .iter()
                                        .map(|p| p.type_name.clone())
                                        .collect(),
                                    is_template: false,
                                },
                            );
                        }
                        crate::parser::parser::ReflectedType::Enum(e) => {
                            checker.registry_mut().register_type(
                                parser::type_consistency::TypeDefinition {
                                    qualified_name: e.name.clone(),
                                    short_name: e.name.clone(),
                                    category: parser::type_consistency::TypeCategory::Enum,
                                    module_name: String::new(),
                                    header_file: String::new(),
                                    namespace: None,
                                    base_classes: Vec::new(),
                                    referenced_types: Vec::new(),
                                    is_template: false,
                                },
                            );
                        }
                        _ => {}
                    }
                }

                let consistency_report = checker.check_all();
                println!("  类型定义: {}", checker.registry().type_count());
                println!("  类型引用: {}", checker.registry().reference_count());

                if consistency_report.is_ok() {
                    println!("  ✓ 类型一致性校验通过");
                } else {
                    println!("  错误: {}", consistency_report.error_count);
                    println!("  警告: {}", consistency_report.warning_count);
                    for diag in &consistency_report.diagnostics {
                        println!("    [{:?}] {}", diag.severity, diag.message);
                    }
                    if strict {
                        errors += consistency_report.error_count + consistency_report.warning_count;
                    } else {
                        errors += consistency_report.error_count;
                    }
                }
            }

            if errors > 0 {
                std::process::exit(1);
            }
        }

        Commands::Watch {
            source_dir,
            output_dir,
        } => {
            info!("LHT: 启动热重载监控");
            info!("  源目录: {}", source_dir.display());
            info!("  输出目录: {}", output_dir.display());

            println!("热重载监控已启动 (按 Ctrl+C 停止)");
            println!("监控目录: {}", source_dir.display());

            // 首次生成
            let headers = scanner::scan_headers(&source_dir, None)?;
            for header in &headers {
                let types = reflection_parser::parse_header(header)?;
                if !types.is_empty() {
                    reflection::generate_reflection_code(header, &types, &output_dir)?;
                }
            }

            // 启动文件监控
            use crate::hotreload::watcher::FileWatcher;
            let mut watcher = FileWatcher::new(
                vec![source_dir.clone()],
                vec!["h".to_string(), "hpp".to_string()],
                500,
            )?;

            println!("等待文件变更...");
            loop {
                std::thread::sleep(std::time::Duration::from_millis(500));

                if let Some(changed_files) = watcher.poll_changes() {
                    for path in changed_files {
                        println!("检测到变更: {}", path.display());
                        match reflection_parser::parse_header(&path) {
                            Ok(types) => {
                                if !types.is_empty() {
                                    reflection::generate_reflection_code(
                                        &path,
                                        &types,
                                        &output_dir,
                                    )?;
                                    println!("✓ 已重新生成: {}", path.display());
                                }
                            }
                            Err(e) => {
                                eprintln!("✗ 解析错误: {}", e);
                            }
                        }
                    }
                }
            }
        }

        Commands::Docs {
            source_dir,
            output_dir,
        } => {
            info!("LHT: 生成 API 文档");

            let headers = scanner::scan_headers(&source_dir, None)?;
            let mut all_types = Vec::new();

            for header in &headers {
                let types = reflection_parser::parse_header(header)?;
                all_types.extend(types);
            }

            use crate::codegen::docs::{generate_api_docs, DocsConfig};
            generate_api_docs(&all_types, &output_dir, &DocsConfig::default())?;

            println!("✓ API 文档已生成: {}", output_dir.display());
            println!("  - {} 个类型", all_types.len());
        }

        Commands::Stats { source_dir } => {
            info!("LHT: 反射统计");

            let headers = scanner::scan_headers(&source_dir, None)?;

            let mut class_count = 0;
            let mut struct_count = 0;
            let mut enum_count = 0;
            let mut delegate_count = 0;
            let mut property_count = 0;
            let mut function_count = 0;

            for header in &headers {
                let types = reflection_parser::parse_header(header)?;
                for t in &types {
                    match t {
                        crate::parser::parser::ReflectedType::Class(c) => {
                            class_count += 1;
                            property_count += c.properties.len();
                            function_count += c.functions.len();
                        }
                        crate::parser::parser::ReflectedType::Struct(s) => {
                            struct_count += 1;
                            property_count += s.properties.len();
                        }
                        crate::parser::parser::ReflectedType::Enum(_) => {
                            enum_count += 1;
                        }
                        crate::parser::parser::ReflectedType::Delegate(_) => {
                            delegate_count += 1;
                        }
                    }
                }
            }

            println!("\n╔══════════════════════════════════════════════════════════════╗");
            println!("║                    LHT 反射统计                               ║");
            println!("╠══════════════════════════════════════════════════════════════╣");
            println!(
                "║  扫描头文件:     {:>6}                                      ║",
                headers.len()
            );
            println!("╠══════════════════════════════════════════════════════════════╣");
            println!(
                "║  类 (LCLASS):    {:>6}                                      ║",
                class_count
            );
            println!(
                "║  结构体 (LSTRUCT):{:>5}                                      ║",
                struct_count
            );
            println!(
                "║  枚举 (LENUM):   {:>6}                                      ║",
                enum_count
            );
            println!(
                "║  委托 (LDELEGATE):{:>5}                                      ║",
                delegate_count
            );
            println!("╠══════════════════════════════════════════════════════════════╣");
            println!(
                "║  属性 (LPROPERTY):{:>5}                                      ║",
                property_count
            );
            println!(
                "║  函数 (LFUNCTION):{:>5}                                      ║",
                function_count
            );
            println!("╚══════════════════════════════════════════════════════════════╝");
        }

        Commands::GenerateRuntime {
            output_dir,
            namespace,
            gc,
            replication,
            editor,
        } => {
            info!("LHT: 生成 C++ 反射运行时头文件");

            let cyan = console::Style::new().cyan();
            let green = console::Style::new().green();

            println!("{}", cyan.apply_to("\n生成 C++ 反射运行时基础设施..."));
            println!("  命名空间: {}", namespace);
            println!("  输出目录: {}", output_dir.display());
            println!("  GC 支持: {}", if gc { "✓" } else { "✗" });
            println!("  网络复制: {}", if replication { "✓" } else { "✗" });
            println!("  编辑器绑定: {}", if editor { "✓" } else { "✗" });

            let generator = RuntimeHeaderGenerator {
                namespace,
                enable_gc: gc,
                enable_replication: replication,
                enable_editor_bindings: editor,
            };

            std::fs::create_dir_all(&output_dir)?;
            let runtime_dir = output_dir.join("Runtime");
            let written_files = generator.generate_all(&runtime_dir)?;

            println!("\n{}", green.apply_to("✓ 运行时头文件已生成:"));
            for file in &written_files {
                println!("  + {}", file.display());
            }

            println!("\n使用方式:");
            println!("  在 C++ 头文件中:");
            println!("    #include \"Runtime/ReflectionMacros.h\"  // 反射宏");
            println!("    #include \"Runtime/ObjectBase.h\"        // 基类");
            println!("  在模块 .limx.toml 中添加此运行时目录到 include_paths:");
            println!("    include_paths = [\"{}\"]", output_dir.display());
        }
    }

    Ok(())
}
