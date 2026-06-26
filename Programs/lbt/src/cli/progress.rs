/*******************************************************************************
 * 文件: cli/progress.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   CLI 进度显示和颜色输出
 *   - 进度条
 *   - 旋转指示器
 *   - 彩色输出
 *   - 构建状态报告
 *
 ******************************************************************************/

use console::{Style, Term};
use indicatif::{MultiProgress, ProgressBar, ProgressStyle};
use std::time::{Duration, Instant};

/// 构建进度管理器
pub struct BuildProgress {
    multi: MultiProgress,
    term: Term,
    start_time: Instant,
}

impl BuildProgress {
    pub fn new() -> Self {
        Self {
            multi: MultiProgress::new(),
            term: Term::stderr(),
            start_time: Instant::now(),
        }
    }

    /// 创建模块发现进度条
    pub fn discovery_spinner(&self, message: &str) -> ProgressBar {
        let pb = self.multi.add(ProgressBar::new_spinner());
        pb.set_style(
            ProgressStyle::default_spinner()
                .template("{spinner:.cyan} {msg}")
                .unwrap_or_else(|_| ProgressStyle::default_spinner())
                .tick_chars("⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏"),
        );
        pb.set_message(message.to_string());
        pb.enable_steady_tick(Duration::from_millis(80));
        pb
    }

    /// 创建构建进度条
    pub fn build_progress(&self, total: u64, message: &str) -> ProgressBar {
        let pb = self.multi.add(ProgressBar::new(total));
        pb.set_style(
            ProgressStyle::default_bar()
                .template("{spinner:.green} [{bar:40.cyan/blue}] {pos}/{len} ({percent}%) [{elapsed_precise}] ETA {eta} | {msg}")
                .unwrap_or_else(|_| ProgressStyle::default_bar())
                .progress_chars("█▓▒░"),
        );
        pb.set_message(message.to_string());
        pb.enable_steady_tick(Duration::from_millis(100));
        pb
    }

    /// 创建模块构建进度条
    pub fn module_progress(&self, module_name: &str) -> ProgressBar {
        let pb = self.multi.add(ProgressBar::new_spinner());
        pb.set_style(
            ProgressStyle::default_spinner()
                .template("  {spinner:.yellow} {msg}")
                .unwrap_or_else(|_| ProgressStyle::default_spinner())
                .tick_chars("⣾⣽⣻⢿⡿⣟⣯⣷"),
        );
        pb.set_message(format!("编译 {}", module_name));
        pb.enable_steady_tick(Duration::from_millis(100));
        pb
    }

    /// 获取总耗时
    pub fn elapsed(&self) -> Duration {
        self.start_time.elapsed()
    }
}

impl Default for BuildProgress {
    fn default() -> Self {
        Self::new()
    }
}

/// 彩色输出样式
pub struct ColoredOutput;

impl ColoredOutput {
    /// 成功消息 (绿色)
    pub fn success(msg: &str) -> String {
        let style = Style::new().green().bold();
        format!("{} {}", style.apply_to("✓"), msg)
    }

    /// 错误消息 (红色)
    pub fn error(msg: &str) -> String {
        let style = Style::new().red().bold();
        format!("{} {}", style.apply_to("✗"), msg)
    }

    /// 警告消息 (黄色)
    pub fn warning(msg: &str) -> String {
        let style = Style::new().yellow().bold();
        format!("{} {}", style.apply_to("⚠"), msg)
    }

    /// 信息消息 (蓝色)
    pub fn info(msg: &str) -> String {
        let style = Style::new().blue();
        format!("{} {}", style.apply_to("ℹ"), msg)
    }

    /// 标题 (粗体白色)
    pub fn title(msg: &str) -> String {
        let style = Style::new().white().bold();
        style.apply_to(msg).to_string()
    }

    /// 模块名 (青色)
    pub fn module(name: &str) -> String {
        let style = Style::new().cyan();
        style.apply_to(name).to_string()
    }

    /// 路径 (暗灰色)
    pub fn path(path: &str) -> String {
        let style = Style::new().dim();
        style.apply_to(path).to_string()
    }

    /// 数字 (亮白色)
    pub fn number<T: std::fmt::Display>(n: T) -> String {
        let style = Style::new().white().bold();
        style.apply_to(n).to_string()
    }

    /// 时间 (品红色)
    pub fn duration(d: Duration) -> String {
        let style = Style::new().magenta();
        let ms = d.as_millis();
        if ms < 1000 {
            style.apply_to(format!("{}ms", ms)).to_string()
        } else {
            style
                .apply_to(format!("{:.2}s", d.as_secs_f64()))
                .to_string()
        }
    }
}

/// 构建报告
pub struct BuildReport {
    pub modules_built: usize,
    pub modules_skipped: usize,
    pub errors: usize,
    pub warnings: usize,
    pub duration: Duration,
}

impl BuildReport {
    pub fn print(&self) {
        println!();
        println!(
            "{}",
            ColoredOutput::title("═══════════════════════════════════════════════════════════════")
        );
        println!(
            "{}",
            ColoredOutput::title("                        构建报告                               ")
        );
        println!(
            "{}",
            ColoredOutput::title("═══════════════════════════════════════════════════════════════")
        );
        println!();

        if self.errors == 0 {
            println!("  {} 构建成功!", ColoredOutput::success(""));
        } else {
            println!("  {} 构建失败!", ColoredOutput::error(""));
        }

        println!();
        println!(
            "  模块构建:    {}",
            ColoredOutput::number(self.modules_built)
        );
        println!(
            "  模块跳过:    {}",
            ColoredOutput::number(self.modules_skipped)
        );

        if self.errors > 0 {
            println!(
                "  错误:        {}",
                Style::new().red().bold().apply_to(self.errors)
            );
        } else {
            println!("  错误:        {}", ColoredOutput::number(0));
        }

        if self.warnings > 0 {
            println!(
                "  警告:        {}",
                Style::new().yellow().apply_to(self.warnings)
            );
        } else {
            println!("  警告:        {}", ColoredOutput::number(0));
        }

        println!();
        println!("  总耗时:      {}", ColoredOutput::duration(self.duration));
        println!();
        println!(
            "{}",
            ColoredOutput::title("═══════════════════════════════════════════════════════════════")
        );
    }
}

/// 打印 LBT Banner
pub fn print_banner() {
    let banner = r#"
    ╦  ╔╗ ╔╦╗
    ║  ╠╩╗ ║   Limx Build Tool
    ╩═╝╚═╝ ╩   v0.1.0
"#;
    println!("{}", Style::new().cyan().apply_to(banner));
}

/// 打印阶段标题
pub fn print_phase(phase: &str) {
    let style = Style::new().blue().bold();
    println!("\n{} {}", style.apply_to("▶"), style.apply_to(phase));
}

/// 打印分隔线
pub fn print_separator() {
    println!("{}", Style::new().dim().apply_to("─".repeat(65)));
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_colored_output_success() {
        let msg = ColoredOutput::success("Test passed");
        assert!(msg.contains("Test passed"));
    }

    #[test]
    fn test_colored_output_error() {
        let msg = ColoredOutput::error("Test failed");
        assert!(msg.contains("Test failed"));
    }

    #[test]
    fn test_colored_output_duration() {
        let d = Duration::from_millis(500);
        let msg = ColoredOutput::duration(d);
        assert!(msg.contains("500"));
    }

    #[test]
    fn test_build_progress_new() {
        let progress = BuildProgress::new();
        assert!(progress.elapsed() < Duration::from_secs(1));
    }
}
