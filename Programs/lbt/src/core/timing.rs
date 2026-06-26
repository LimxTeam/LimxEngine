/*******************************************************************************
 * 文件: core/timing.rs
 * 创建时间: 2025-12-21
 * 创建者: LimxTeam
 *
 * 功能描述:
 *   性能监控与计时工具
 *   - 阶段计时
 *   - 性能报告
 *   - 瓶颈分析
 *
 ******************************************************************************/

use parking_lot::Mutex;
use std::collections::HashMap;
use std::sync::Arc;
use std::time::{Duration, Instant};

/// 构建阶段
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum BuildPhase {
    /// 模块发现
    Discovery,
    /// 依赖解析
    DependencyResolution,
    /// 配置解析
    ConfigParsing,
    /// CMake 生成
    CmakeGeneration,
    /// 项目文件生成
    ProjectGeneration,
    /// 反射代码生成
    ReflectionGeneration,
    /// 编译
    Compilation,
    /// 链接
    Linking,
    /// 缓存操作
    CacheOperation,
    /// 其他
    Other,
}

impl BuildPhase {
    pub fn name(&self) -> &'static str {
        match self {
            Self::Discovery => "模块发现",
            Self::DependencyResolution => "依赖解析",
            Self::ConfigParsing => "配置解析",
            Self::CmakeGeneration => "CMake 生成",
            Self::ProjectGeneration => "项目文件生成",
            Self::ReflectionGeneration => "反射代码生成",
            Self::Compilation => "编译",
            Self::Linking => "链接",
            Self::CacheOperation => "缓存操作",
            Self::Other => "其他",
        }
    }
}

/// 计时条目
#[derive(Debug, Clone)]
pub struct TimingEntry {
    pub phase: BuildPhase,
    pub duration: Duration,
    pub details: Option<String>,
}

/// 性能监控器
#[derive(Debug, Clone)]
pub struct PerformanceMonitor {
    /// 计时条目
    entries: Arc<Mutex<Vec<TimingEntry>>>,
    /// 当前计时开始时间
    current_start: Option<Instant>,
    /// 当前阶段
    current_phase: Option<BuildPhase>,
    /// 总开始时间
    total_start: Instant,
}

impl PerformanceMonitor {
    /// 创建新的性能监控器
    pub fn new() -> Self {
        Self {
            entries: Arc::new(Mutex::new(Vec::new())),
            current_start: None,
            current_phase: None,
            total_start: Instant::now(),
        }
    }

    /// 开始计时阶段
    pub fn start_phase(&mut self, phase: BuildPhase) {
        // 结束当前阶段
        self.end_current_phase();

        self.current_start = Some(Instant::now());
        self.current_phase = Some(phase);
    }

    /// 结束当前阶段
    pub fn end_current_phase(&mut self) {
        if let (Some(start), Some(phase)) = (self.current_start.take(), self.current_phase.take()) {
            let duration = start.elapsed();
            self.entries.lock().push(TimingEntry {
                phase,
                duration,
                details: None,
            });
        }
    }

    /// 记录带详情的阶段
    pub fn record_phase(&mut self, phase: BuildPhase, duration: Duration, details: Option<String>) {
        self.entries.lock().push(TimingEntry {
            phase,
            duration,
            details,
        });
    }

    /// 计时一个闭包
    pub fn time<F, T>(&mut self, phase: BuildPhase, f: F) -> T
    where
        F: FnOnce() -> T,
    {
        let start = Instant::now();
        let result = f();
        let duration = start.elapsed();

        self.entries.lock().push(TimingEntry {
            phase,
            duration,
            details: None,
        });

        result
    }

    /// 获取总耗时
    pub fn total_duration(&self) -> Duration {
        self.total_start.elapsed()
    }

    /// 获取阶段汇总
    pub fn get_phase_summary(&self) -> HashMap<BuildPhase, Duration> {
        let entries = self.entries.lock();
        let mut summary = HashMap::new();

        for entry in entries.iter() {
            *summary.entry(entry.phase).or_insert(Duration::ZERO) += entry.duration;
        }

        summary
    }

    /// 打印性能报告
    pub fn print_report(&self) {
        self.end_current_phase_immut();

        let total = self.total_duration();
        let summary = self.get_phase_summary();

        println!("\n╔══════════════════════════════════════════════════════════════╗");
        println!("║                    构建性能报告                               ║");
        println!("╠══════════════════════════════════════════════════════════════╣");

        // 按耗时排序
        let mut phases: Vec<_> = summary.into_iter().collect();
        phases.sort_by(|a, b| b.1.cmp(&a.1));

        for (phase, duration) in &phases {
            let ms = duration.as_millis();
            let percent = duration.as_secs_f64() / total.as_secs_f64() * 100.0;
            let bar_len = (percent / 5.0) as usize;
            let bar = "█".repeat(bar_len);

            println!(
                "║  {:<16} {:>6} ms ({:>5.1}%) {}",
                phase.name(),
                ms,
                percent,
                bar
            );
        }

        println!("╠══════════════════════════════════════════════════════════════╣");
        println!(
            "║  总耗时:          {:>6} ms                                   ║",
            total.as_millis()
        );
        println!("╚══════════════════════════════════════════════════════════════╝");
    }

    /// 内部方法：不可变地结束当前阶段
    fn end_current_phase_immut(&self) {
        // 这个方法什么都不做，只是为了在打印报告时保持 self 不可变
    }
}

impl Default for PerformanceMonitor {
    fn default() -> Self {
        Self::new()
    }
}

/// RAII 计时器
pub struct ScopedTimer<'a> {
    monitor: &'a mut PerformanceMonitor,
    phase: BuildPhase,
    start: Instant,
}

impl<'a> ScopedTimer<'a> {
    pub fn new(monitor: &'a mut PerformanceMonitor, phase: BuildPhase) -> Self {
        Self {
            monitor,
            phase,
            start: Instant::now(),
        }
    }
}

impl<'a> Drop for ScopedTimer<'a> {
    fn drop(&mut self) {
        let duration = self.start.elapsed();
        self.monitor.record_phase(self.phase, duration, None);
    }
}

/// 简单计时宏
#[macro_export]
macro_rules! timed {
    ($monitor:expr, $phase:expr, $block:block) => {{
        let start = std::time::Instant::now();
        let result = $block;
        let duration = start.elapsed();
        $monitor.record_phase($phase, duration, None);
        result
    }};
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_performance_monitor() {
        let mut monitor = PerformanceMonitor::new();

        monitor.start_phase(BuildPhase::Discovery);
        std::thread::sleep(Duration::from_millis(10));
        monitor.end_current_phase();

        let summary = monitor.get_phase_summary();
        assert!(summary.contains_key(&BuildPhase::Discovery));
    }

    #[test]
    fn test_time_closure() {
        let mut monitor = PerformanceMonitor::new();

        let result = monitor.time(BuildPhase::ConfigParsing, || {
            std::thread::sleep(Duration::from_millis(5));
            42
        });

        assert_eq!(result, 42);
        assert!(!monitor.entries.lock().is_empty());
    }
}
