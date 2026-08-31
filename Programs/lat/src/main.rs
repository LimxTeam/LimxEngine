/*******************************************************************************
 * 文件: main.rs
 * 创建时间: 2026-08-30
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   LAT (Limx Asset Tool) 入口点
 *   - 纹理离线烘焙: 源图 → mip 链 → BC 块压缩 (BC1/3/4/5/7) → DDS (DX10 头)
 *   - 批量递归烘焙
 *   - DDS 检查
 *
 * 设计哲学:
 *   1. 语义由调用方给, 工具不猜文件名
 *   2. 任何失败路径都以非零退出 —— 打印了错误却返回 Ok 等于没有检查
 *   3. 输出用 DDS 标准容器, 出错时肉眼可查
 *
 ******************************************************************************/

mod bake;
mod bc7;
mod cli;
mod dds;
mod format;
mod image_io;
mod mip;

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::sync::Mutex;
use std::time::Instant;

use anyhow::{bail, Context, Result};
use clap::Parser;
use rayon::prelude::*;
use tracing::info;
use tracing_subscriber::{layer::SubscriberExt, util::SubscriberInitExt, EnvFilter};

use crate::bake::{BakeOptions, BakeReport};
use crate::cli::{BakeArgs, Cli, Commands};
use crate::format::dxgi_format_name;

/// 打印 LAT Banner
fn print_banner() {
    println!(
        r#"
    ╦  ╔═╗╔╦╗
    ║  ╠═╣ ║      Limx Asset Tool
    ╩═╝╩ ╩ ╩      v0.1.0 - BC1/BC3/BC4/BC5/BC7 纹理烘焙
"#
    );
}

/// 入口。
///
/// 这一层唯一的职责就是把 `run()` 的 Err 变成非零退出码。
/// 上一轮刚修过五处"打印了错误却返回 Ok"的缺陷 —— 所有失败必须经过这里,
/// 任何子命令内部都不允许自己吞掉错误后 `return Ok(())`。
fn main() -> ExitCode {
    let _log_guard = init_logging();

    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            // {:#} 展开整条 anyhow 上下文链, 错误才指得到真正的原因。
            eprintln!("\n✗ 错误: {:#}", e);
            ExitCode::FAILURE
        }
    }
}

fn init_logging() -> tracing_appender::non_blocking::WorkerGuard {
    let log_dir = PathBuf::from("Logs");
    std::fs::create_dir_all(&log_dir).ok();

    let file_appender = tracing_appender::rolling::daily(&log_dir, "lat.log");
    let (non_blocking, guard) = tracing_appender::non_blocking(file_appender);

    let env_filter = EnvFilter::try_from_default_env().unwrap_or_else(|_| EnvFilter::new("info"));

    tracing_subscriber::registry()
        .with(env_filter)
        .with(
            tracing_subscriber::fmt::layer()
                .with_writer(std::io::stderr)
                .without_time()
                .with_target(false),
        )
        .with(
            tracing_subscriber::fmt::layer()
                .with_writer(non_blocking)
                .with_ansi(false),
        )
        .init();

    guard
}

fn run() -> Result<()> {
    let cli = Cli::parse();

    match cli.command {
        Commands::Bake {
            input,
            output,
            bake_args,
        } => {
            if cli.verbose {
                print_banner();
            }
            cmd_bake(&input, &output, &bake_args, cli.verbose)
        }

        Commands::BakeAll {
            source_dir,
            output_dir,
            jobs,
            no_recursive,
            extensions,
            bake_args,
        } => {
            if cli.verbose {
                print_banner();
            }
            cmd_bake_all(
                &source_dir,
                &output_dir,
                jobs,
                !no_recursive,
                &extensions,
                &bake_args,
                cli.verbose,
            )
        }

        Commands::Inspect { input } => cmd_inspect(&input),
    }
}

// ── bake ────────────────────────────────────────────────────────────────

fn options_from(args: &BakeArgs) -> BakeOptions {
    BakeOptions {
        format: args.format,
        color_space: args.color_space,
        mipmaps: !args.no_mipmaps,
        verify: args.verify,
        min_psnr: args.min_psnr,
    }
}

/// 烘焙一张图并写盘。返回 (输入字节数, 报告)。
fn bake_one(input: &Path, output: &Path, args: &BakeArgs) -> Result<(u64, BakeReport)> {
    if !image_io::is_supported_source(input) {
        bail!(
            "{} 的扩展名不在本工具能解码的格式里 ({})",
            input.display(),
            image_io::SUPPORTED_EXTENSIONS.join(", ")
        );
    }

    if output.exists() && !args.overwrite {
        bail!(
            "输出文件已存在: {} —— 加 --overwrite 覆盖, 或换一个输出路径",
            output.display()
        );
    }

    let input_bytes = std::fs::metadata(input)
        .with_context(|| format!("无法读取 {} 的文件信息", input.display()))?
        .len();

    let loaded = image_io::load(input)?;
    tracing::debug!(
        "{}: {}×{} {} 通道 (容器 {}), alpha {}",
        input.display(),
        loaded.image.width,
        loaded.image.height,
        loaded.traits.channels,
        loaded.detected_format,
        if loaded.traits.alpha_significant {
            "有效"
        } else {
            "无效/不存在"
        }
    );

    let (bytes, report) = bake::bake(&loaded.image, loaded.traits, options_from(args))
        .with_context(|| format!("烘焙 {} 失败", input.display()))?;

    if let Some(parent) = output.parent() {
        std::fs::create_dir_all(parent)
            .with_context(|| format!("无法创建输出目录 {}", parent.display()))?;
    }
    std::fs::write(output, &bytes)
        .with_context(|| format!("写入 {} 失败", output.display()))?;

    Ok((input_bytes, report))
}

fn cmd_bake(input: &Path, output: &Path, args: &BakeArgs, verbose: bool) -> Result<()> {
    info!("LAT: 烘焙纹理");
    info!("  源文件: {}", input.display());
    info!("  输出:   {}", output.display());

    let start = Instant::now();
    let (input_bytes, report) = bake_one(input, output, args)?;
    let elapsed = start.elapsed();

    println!("\n✓ 烘焙成功");
    println!("  输出:   {}", output.display());
    println!(
        "  格式:   {} ({:?})",
        report.format.name(),
        args.color_space
    );
    println!("  尺寸:   {}×{}", report.width, report.height);
    println!("  mip:    {} 层", report.mip_count);
    println!(
        "  大小:   {} → {} ({:.2}×)",
        human_bytes(input_bytes as usize),
        human_bytes(report.output_bytes),
        report.output_bytes as f64 / input_bytes.max(1) as f64
    );
    if let Some(q) = report.psnr {
        println!("  PSNR:   {:.2} dB (第 0 层往返)", q);
    }
    println!("  耗时:   {:.2} ms", elapsed.as_secs_f64() * 1000.0);

    if verbose {
        // 未压缩的 RGBA8 才是运行时显存的真正对照物 —— 源文件是 JPEG,
        // 它的大小和显存占用毫无关系。对照要连 mip 链一起算, 否则
        // 拿"只有第 0 层的 RGBA8"去比"含 mip 的 BC"会低估约 4/3 倍。
        let rgba_bytes = mip::rgba8_chain_bytes(report.width, report.height, report.mip_count);
        println!(
            "  显存对照: RGBA8 {} → {} {} (1/{:.1})",
            human_bytes(rgba_bytes),
            report.format.name(),
            human_bytes(report.output_bytes),
            rgba_bytes as f64 / report.output_bytes as f64
        );
    }

    Ok(())
}

// ── bake-all ────────────────────────────────────────────────────────────

struct BatchOutcome {
    input: PathBuf,
    result: Result<(u64, BakeReport)>,
}

#[allow(clippy::too_many_arguments)]
fn cmd_bake_all(
    source_dir: &Path,
    output_dir: &Path,
    jobs: usize,
    recursive: bool,
    extensions: &str,
    args: &BakeArgs,
    verbose: bool,
) -> Result<()> {
    if !source_dir.is_dir() {
        bail!(
            "源目录不存在或不是目录: {} —— bake-all 的 -s 收的是目录, 单张图请用 bake",
            source_dir.display()
        );
    }

    let ext_list: Vec<String> = extensions
        .split(',')
        .map(|e| e.trim().trim_start_matches('.').to_ascii_lowercase())
        .filter(|e| !e.is_empty())
        .collect();
    if ext_list.is_empty() {
        bail!("--extensions 解析后为空: {:?}", extensions);
    }
    for ext in &ext_list {
        if !image_io::SUPPORTED_EXTENSIONS.contains(&ext.as_str()) {
            bail!(
                "扩展名 '{}' 不在本工具能解码的格式里 ({}) —— \
                 收进来只会在解码时才失败, 不如现在就说清楚",
                ext,
                image_io::SUPPORTED_EXTENSIONS.join(", ")
            );
        }
    }

    info!("LAT: 批量烘焙纹理");
    info!("  源目录:   {}", source_dir.display());
    info!("  输出目录: {}", output_dir.display());

    let start = Instant::now();

    let walker = if recursive {
        walkdir::WalkDir::new(source_dir)
    } else {
        walkdir::WalkDir::new(source_dir).max_depth(1)
    };

    let mut sources: Vec<PathBuf> = Vec::new();
    for entry in walker {
        let entry = entry.with_context(|| format!("遍历 {} 时出错", source_dir.display()))?;
        let path = entry.path();
        if !path.is_file() {
            continue;
        }
        let matches = path
            .extension()
            .and_then(|e| e.to_str())
            .map(|e| ext_list.contains(&e.to_ascii_lowercase()))
            .unwrap_or(false);
        if matches {
            sources.push(path.to_path_buf());
        }
    }
    sources.sort();

    if sources.is_empty() {
        bail!(
            "{} 下没有找到任何 {} 文件 —— 检查路径或 --extensions",
            source_dir.display(),
            ext_list.join("/")
        );
    }

    // 输出路径撞车检查: a.png 与 a.jpg 会映射到同一个 a.dds, 并行写入时
    // 后写的那份赢, 结果是随机的。这种 bug 只在特定目录下出现, 而且
    // 不会有任何报错, 必须提前拦住。
    let mut planned: HashMap<PathBuf, PathBuf> = HashMap::new();
    let mut plan: Vec<(PathBuf, PathBuf)> = Vec::with_capacity(sources.len());
    for src in &sources {
        let rel = src
            .strip_prefix(source_dir)
            .with_context(|| format!("{} 不在源目录之下", src.display()))?;
        let dst = output_dir.join(rel).with_extension("dds");
        if let Some(prev) = planned.get(&dst) {
            bail!(
                "输出路径冲突: {} 与 {} 都会写到 {}\n  \
                 同名不同扩展名的源图不能烘到同一个目录, 请先重命名其中一个",
                prev.display(),
                src.display(),
                dst.display()
            );
        }
        planned.insert(dst.clone(), src.clone());
        plan.push((src.clone(), dst));
    }

    println!("发现 {} 张源图", plan.len());

    let pb = indicatif::ProgressBar::new(plan.len() as u64);
    pb.set_style(
        indicatif::ProgressStyle::default_bar()
            .template("{spinner:.green} [{bar:40.cyan/blue}] {pos}/{len} ({percent}%) [{elapsed_precise}] ETA {eta} | {msg}")
            .unwrap_or_else(|_| indicatif::ProgressStyle::default_bar())
            .progress_chars("█▓▒░"),
    );
    pb.set_message("烘焙纹理");
    pb.enable_steady_tick(std::time::Duration::from_millis(100));

    let outcomes: Mutex<Vec<BatchOutcome>> = Mutex::new(Vec::with_capacity(plan.len()));

    let do_work = || {
        plan.par_iter().for_each(|(src, dst)| {
            let result = bake_one(src, dst, args);

            pb.inc(1);
            let name = src
                .file_name()
                .map(|n| n.to_string_lossy().to_string())
                .unwrap_or_else(|| src.display().to_string());
            match &result {
                Ok((_, report)) => {
                    if verbose {
                        pb.println(format!(
                            "  ✓ {} → {} {}×{} {} 层 {}",
                            name,
                            report.format.name(),
                            report.width,
                            report.height,
                            report.mip_count,
                            human_bytes(report.output_bytes)
                        ));
                    }
                }
                Err(e) => {
                    pb.println(format!("  ✗ {} — {:#}", name, e));
                }
            }
            pb.set_message(name);

            outcomes.lock().expect("结果队列被毒化").push(BatchOutcome {
                input: src.clone(),
                result,
            });
        });
    };

    // jobs = 0 用 rayon 的默认全局池 (硬件线程数)。
    if jobs > 0 {
        let pool = rayon::ThreadPoolBuilder::new()
            .num_threads(jobs)
            .build()
            .context("创建线程池失败")?;
        pool.install(do_work);
    } else {
        do_work();
    }

    let mut outcomes = outcomes.into_inner().expect("结果队列被毒化");
    outcomes.sort_by(|a, b| a.input.cmp(&b.input));
    pb.finish_and_clear();

    let elapsed = start.elapsed();

    let mut ok_count = 0usize;
    let mut total_input = 0u64;
    let mut total_output = 0usize;
    let mut total_rgba8 = 0usize;
    let mut by_format: HashMap<&'static str, usize> = HashMap::new();
    let mut failures: Vec<(PathBuf, String)> = Vec::new();
    let mut psnr_values: Vec<(f64, PathBuf)> = Vec::new();

    for outcome in &outcomes {
        match &outcome.result {
            Ok((input_bytes, report)) => {
                ok_count += 1;
                total_input += input_bytes;
                total_output += report.output_bytes;
                total_rgba8 +=
                    mip::rgba8_chain_bytes(report.width, report.height, report.mip_count);
                *by_format.entry(report.format.name()).or_default() += 1;
                if let Some(q) = report.psnr {
                    psnr_values.push((q, outcome.input.clone()));
                }
            }
            Err(e) => failures.push((outcome.input.clone(), format!("{:#}", e))),
        }
    }

    println!("\n╔══════════════════════════════════════════════════════════════╗");
    println!("║                      烘焙完成                                ║");
    println!("╠══════════════════════════════════════════════════════════════╣");
    println!("║  成功:       {:>10} 张                                   ║", ok_count);
    if !failures.is_empty() {
        println!("║  失败:       {:>10} 张                                   ║", failures.len());
    }
    println!("║  输入总计:   {:>10}                                      ║", human_bytes(total_input as usize));
    println!("║  输出总计:   {:>10}                                      ║", human_bytes(total_output));
    println!(
        "║  输出/输入:  {:>10.2}×                                     ║",
        total_output as f64 / total_input.max(1) as f64
    );
    println!("╠══════════════════════════════════════════════════════════════╣");
    // 这一行才是烘焙真正要看的数字: 运行时显存从 RGBA8 降到了多少。
    // 两侧都含完整 mip 链, 口径一致。
    println!("║  RGBA8 显存: {:>10}                                      ║", human_bytes(total_rgba8));
    println!("║  BC    显存: {:>10}                                      ║", human_bytes(total_output));
    println!(
        "║  显存压缩比: {:>10.2}×                                     ║",
        total_rgba8 as f64 / total_output.max(1) as f64
    );
    println!("╠══════════════════════════════════════════════════════════════╣");
    let mut formats: Vec<_> = by_format.iter().collect();
    formats.sort();
    for (name, count) in formats {
        println!("║  {:<10}: {:>10} 张                                   ║", name, count);
    }
    println!("╠══════════════════════════════════════════════════════════════╣");
    println!(
        "║  耗时:       {:>10.2} s                                    ║",
        elapsed.as_secs_f64()
    );
    println!("╚══════════════════════════════════════════════════════════════╝");

    if !psnr_values.is_empty() {
        psnr_values.sort_by(|a, b| a.0.total_cmp(&b.0));
        let mean: f64 = psnr_values.iter().map(|(q, _)| q).sum::<f64>() / psnr_values.len() as f64;
        println!(
            "\n往返 PSNR (第 0 层): 平均 {:.2} dB, 中位 {:.2} dB",
            mean,
            psnr_values[psnr_values.len() / 2].0
        );
        // 最差的几张才是需要人看一眼的 —— 平均值会把它们藏起来。
        println!("  最差 5 张:");
        for (q, path) in psnr_values.iter().take(5) {
            println!(
                "    {:>6.2} dB  {}",
                q,
                path.file_name()
                    .map(|n| n.to_string_lossy().to_string())
                    .unwrap_or_else(|| path.display().to_string())
            );
        }
    }

    if !failures.is_empty() {
        println!("\n失败清单:");
        for (path, err) in &failures {
            println!("  ✗ {}\n      {}", path.display(), err);
        }
        // 非零退出。批量任务里"大部分成功"不等于成功 —— CI 必须变红。
        bail!("{} 张贴图烘焙失败 (共 {} 张)", failures.len(), outcomes.len());
    }

    Ok(())
}

// ── inspect ─────────────────────────────────────────────────────────────

fn cmd_inspect(input: &Path) -> Result<()> {
    let bytes = std::fs::read(input)
        .with_context(|| format!("无法读取 {}", input.display()))?;

    let info = dds::parse(&bytes)
        .with_context(|| format!("解析 {} 失败", input.display()))?;

    let format_name = dxgi_format_name(info.dxgi_format).unwrap_or("未知");
    let payload: usize = info.level_sizes.iter().sum();

    println!("文件:      {}", input.display());
    println!("大小:      {} ({} 字节)", human_bytes(bytes.len()), bytes.len());
    println!(
        "格式:      {} (dxgiFormat = {})",
        format_name, info.dxgi_format
    );
    println!("尺寸:      {}×{}", info.width, info.height);
    println!("mip 层数:  {}", info.mip_count);
    println!(
        "头部:      {} 字节 (magic 4 + DDS_HEADER 124 + DX10 20)",
        info.payload_offset
    );
    println!("载荷:      {} ({} 字节)", human_bytes(payload), payload);
    println!(
        "标志:      dwFlags = 0x{:08X}  dwCaps = 0x{:08X}  miscFlags2 = {}",
        info.flags, info.caps, info.misc_flags2
    );
    println!(
        "dwPitchOrLinearSize = {} (第 0 层字节数)",
        info.linear_size
    );

    println!("\n  层  尺寸           字节数        偏移");
    println!("  ──  ─────────────  ────────────  ────────────");
    let mut offset = info.payload_offset;
    for (i, ((w, h), size)) in info
        .level_dims
        .iter()
        .zip(info.level_sizes.iter())
        .enumerate()
    {
        println!(
            "  {:>2}  {:>5}×{:<7}  {:>12}  {:>12}",
            i,
            w,
            h,
            size,
            offset
        );
        offset += size;
    }

    // 与未压缩 RGBA8 的对照, 这是这张贴图省下的显存。
    let rgba8: usize = info
        .level_dims
        .iter()
        .map(|(w, h)| *w as usize * *h as usize * 4)
        .sum();
    println!(
        "\n  等价 RGBA8: {} → 压缩比 {:.2}×",
        human_bytes(rgba8),
        rgba8 as f64 / payload.max(1) as f64
    );

    Ok(())
}

// ── 小工具 ──────────────────────────────────────────────────────────────

fn human_bytes(bytes: usize) -> String {
    const KIB: f64 = 1024.0;
    const MIB: f64 = 1024.0 * 1024.0;
    const GIB: f64 = 1024.0 * 1024.0 * 1024.0;
    let b = bytes as f64;
    if b >= GIB {
        format!("{:.2} GiB", b / GIB)
    } else if b >= MIB {
        format!("{:.2} MiB", b / MIB)
    } else if b >= KIB {
        format!("{:.2} KiB", b / KIB)
    } else {
        format!("{} B", bytes)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn human_bytes_boundaries() {
        assert_eq!(human_bytes(0), "0 B");
        assert_eq!(human_bytes(1023), "1023 B");
        assert_eq!(human_bytes(1024), "1.00 KiB");
        assert_eq!(human_bytes(1024 * 1024), "1.00 MiB");
        assert_eq!(human_bytes(1024 * 1024 * 1024), "1.00 GiB");
    }
}
